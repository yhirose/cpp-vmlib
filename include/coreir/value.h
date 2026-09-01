// A tagged runtime value, reference counted.
//
// Layout mirrors culebra's JitValue: a tag word and a payload word, with the
// refcount at offset zero of every heap object. Where it deliberately differs
// is who places the retain and the release. culebra's bytecode compiler emits
// Retain/Release/Take instructions, because its LLVM lane emits machine code
// that has to do the counting explicitly and because it wants to elide
// redundant pairs. cpp-vmlib has no such lane, so Value is an RAII handle
// instead and ordinary C++ assignment and destruction place every one of
// them. vm/compiler.cc contains no reference counting at all, and a missing
// release is not a thing that can be written.
//
// That is also what makes a host throw safe. The executor used to be
// exception-safe for free because a Frame held only plain vectors; owned
// references would have ended that, except that the unwind which destroys the
// frame stack now releases every value in every live frame, with no unwind
// table for anyone to emit and get wrong.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace coreir {

// Uninit is what a local slot holds before anything is stored in it. It is a
// tag rather than a flag beside the value so that a slot is one word pair
// like everything else, and so a tracing collector scanning slots does not
// need to know about a parallel array of flags. No operation accepts one; a
// read reports "uninitialized variable" and stops.
//
// Cell and Func are not types a source language needs to name. A Cell is the
// box a captured variable lives in, so a closure that outlives the frame
// declaring the variable still shares one copy of it; Func is such a closure.
// Both are values because both are refcounted heap objects, and making them
// ordinary tags means one lifetime rule covers everything.
enum class ValueTag : uint8_t {
  Uninit, Nil, Bool, Int, Double, Str, Array, Object, Cell, Func, Generator
};

struct HeapObj;

// Everything a running program's heap consists of, owned by an object rather
// than by file-scope state.
//
// Two things want that. A tracing collector has to enumerate live objects,
// which a bare counter cannot do, so objects go on an intrusive list. And a
// design where two programs run at once -- separate heaps, values crossing
// only by copying -- needs the heap to be a thing there can be two of.
// Neither is built yet; both would be a rewrite rather than an addition if
// this state stayed global, which is the reason to pay for it now.
//
// One Runtime is current per thread; Value's allocators reach it through
// Runtime::current(), the way culebra's runtime reaches its own.
class Runtime {
 public:
  Runtime();
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  int64_t live_objects() const { return live_; }
  HeapObj* first_object() const { return head_; }

  // The tracing backstop: frees the reference cycles counting cannot. An
  // object is a root exactly when its refcount exceeds the number of
  // references other heap objects hold on it -- the remainder are handles
  // on the C++ side (frame registers and locals, a defer stack, an executor
  // temporary halfway through an instruction), which no registration scheme
  // could enumerate. Everything unreachable from those roots is condemned,
  // handed to the finalize hook, then pinned, stripped of its references,
  // and freed -- ~Runtime's own two-step, run early. Runs automatically
  // from the allocators once the heap outgrows a doubling threshold, on
  // every allocation under COREIR_GC_STRESS=1, and on demand here.
  void collect();

  // Called by the Value allocators after each fully-constructed object --
  // never during construction, when a half-built object cannot answer for
  // its children.
  void maybe_collect() {
    if ((stress_ || live_ >= next_gc_) && !in_collect_) collect();
  }

  // One call per condemned object, before any of them is freed. What it
  // does with an object must not allocate on this runtime.
  void set_finalize_fn(void* ctx, void (*fn)(void* ctx, HeapObj* o)) {
    finalize_ctx_ = ctx;
    finalize_ = fn;
  }

  // Deterministic destructors: an Object whose "\x01drop" key holds a
  // closure gets it called -- with the object as its one argument -- at the
  // moment its refcount reaches zero, before the object is freed. The hook
  // is how: the executor installs one for the lifetime of a run (it is what
  // can call a closure), and release() hands it each zero-count Object that
  // carries the key. The hook runs over a pinned object; if it stores the
  // object somewhere (a resurrection), the free is skipped.
  void set_drop_fn(void* ctx, void (*fn)(void* ctx, HeapObj* o)) {
    drop_ctx_ = ctx;
    drop_ = fn;
  }
  void* drop_ctx() const { return drop_ctx_; }
  void (*drop_fn() const)(void*, HeapObj*) { return drop_; }
  bool in_collect() const { return in_collect_; }

  static Runtime* current() { return current_; }

  // Makes `rt` current for its own lifetime and restores whatever was current
  // before. Scoped rather than set-and-forget, so an exception leaving a run
  // cannot strand a dangling current pointer.
  class Scope {
   public:
    explicit Scope(Runtime& rt) : prev_(current_) { current_ = &rt; }
    ~Scope() { current_ = prev_; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    Runtime* prev_;
  };

 private:
  friend struct HeapObj;
  void link(HeapObj* o);
  void unlink(HeapObj* o);

  HeapObj* head_ = nullptr;
  int64_t live_ = 0;
  int64_t next_gc_ = 4096;
  bool stress_ = false;
  bool in_collect_ = false;
  void* finalize_ctx_ = nullptr;
  void (*finalize_)(void*, HeapObj*) = nullptr;
  void* drop_ctx_ = nullptr;
  void (*drop_)(void*, HeapObj*) = nullptr;
  static thread_local Runtime* current_;
};

struct HeapObj {
  int64_t rc;  // offset zero, as in culebra's refcounted structs
  // Collector scratch: how many references other heap objects hold on this
  // one (recomputed per collect), and the reachability mark.
  int64_t gc_refs = 0;
  bool gc_marked = false;
  // Which concrete object this is. A tag rather than a vtable: ir.h's own
  // rule is that nothing here has virtual functions or RTTI, because that is
  // what survives --gc-sections as residue in a binary that never uses it.
  ValueTag kind;
  // Intrusive links: enumerating the heap costs no side table, and allocating
  // costs no container growth.
  HeapObj* prev = nullptr;
  HeapObj* next = nullptr;
  Runtime* owner = nullptr;

  explicit HeapObj(ValueTag k);
  ~HeapObj();
  HeapObj(const HeapObj&) = delete;
  HeapObj& operator=(const HeapObj&) = delete;
};

// Frees a heap object through its kind tag. Out of line because it needs
// every concrete type complete.
void destroy_heap_object(HeapObj* o);

// Drops everything an object refers to, without freeing the object itself.
// Teardown needs this separated from destruction: see ~Runtime.
void clear_heap_object_refs(HeapObj* o);

struct StrObj : HeapObj {
  std::string s;
  explicit StrObj(std::string v) : HeapObj(ValueTag::Str), s(std::move(v)) {}
};

class Value;

struct ArrayObj;
struct ObjectObj;
// The box a captured variable lives in. Defined out of line because it holds
// a Value, which is not complete yet.
struct CellObj;
// A closure: which function, plus the cells it closed over. The cells are
// shared with whatever frame or other closure also holds them -- that sharing
// is the entire reason cells exist.
struct ClosureObj;
// A suspended generator activation, owning its frame's storage while no
// executor frame exists for it. Defined out of line, like CellObj.
struct GeneratorObj;

class Value {
 public:
  Value() : tag_(ValueTag::Nil), data_(0) {}

  static Value uninit() {
    Value r;
    r.tag_ = ValueTag::Uninit;
    return r;
  }

  static Value make_bool(bool v) {
    Value r;
    r.tag_ = ValueTag::Bool;
    r.data_ = v ? 1 : 0;
    return r;
  }

  static Value make_int(int64_t v) {
    Value r;
    r.tag_ = ValueTag::Int;
    r.data_ = v;
    return r;
  }

  // The bit pattern, as culebra's JitValue stores a Float: a payload word is
  // a payload word, and bit_cast keeps it from being a strict-aliasing
  // question.
  static Value make_double(double v) {
    Value r;
    r.tag_ = ValueTag::Double;
    std::memcpy(&r.data_, &v, sizeof(double));
    return r;
  }

  // Takes ownership of the +1 a fresh StrObj is born with.
  static Value make_str(std::string s) {
    Value r;
    r.tag_ = ValueTag::Str;
    r.data_ = reinterpret_cast<int64_t>(new StrObj(std::move(s)));
    gc_safepoint();
    return r;
  }

  // The allocators' collection point. The fresh object is complete and held
  // by a C++ handle (rc above its zero internal references), so a stress
  // collect right here cannot take it.
  static void gc_safepoint() {
    if (Runtime* rt = Runtime::current()) rt->maybe_collect();
  }

  Value(const Value& o) : tag_(o.tag_), data_(o.data_) { retain(); }
  Value(Value&& o) noexcept : tag_(o.tag_), data_(o.data_) {
    o.tag_ = ValueTag::Nil;
    o.data_ = 0;
  }
  Value& operator=(const Value& o) {
    if (this != &o) {
      Value tmp(o);  // retain before release: self-referential stores are safe
      swap(tmp);
    }
    return *this;
  }
  Value& operator=(Value&& o) noexcept {
    if (this != &o) {
      Value tmp(std::move(o));
      swap(tmp);
    }
    return *this;
  }
  ~Value() { release(); }

  void swap(Value& o) noexcept {
    std::swap(tag_, o.tag_);
    std::swap(data_, o.data_);
  }

  static Value make_cell();                     // a fresh box holding nil
  static Value make_generator();                // empty; the executor fills it
  static Value make_closure(int32_t func, std::vector<Value> cells);
  static Value make_array(std::vector<Value> items);
  static Value make_object();

  ValueTag tag() const { return tag_; }
  // The raw payload, for the collector's child walk only.
  int64_t raw_data() const { return data_; }

  // A retained reference to an already-live heap object -- what the drop
  // hook hands the destructor as its argument.
  static Value make_ref(HeapObj* h);

  bool is_heap() const {
    return tag_ == ValueTag::Str || tag_ == ValueTag::Array ||
           tag_ == ValueTag::Object || tag_ == ValueTag::Cell ||
           tag_ == ValueTag::Func || tag_ == ValueTag::Generator;
  }
  bool is_uninit() const { return tag_ == ValueTag::Uninit; }
  bool is_bool() const { return tag_ == ValueTag::Bool; }
  bool is_int() const { return tag_ == ValueTag::Int; }
  bool is_double() const { return tag_ == ValueTag::Double; }
  bool is_number() const { return is_int() || is_double(); }
  bool is_str() const { return tag_ == ValueTag::Str; }
  bool is_array() const { return tag_ == ValueTag::Array; }
  bool is_object() const { return tag_ == ValueTag::Object; }
  bool is_nil() const { return tag_ == ValueTag::Nil; }
  bool is_cell() const { return tag_ == ValueTag::Cell; }
  bool is_func() const { return tag_ == ValueTag::Func; }
  bool is_generator() const { return tag_ == ValueTag::Generator; }

  bool as_bool() const { return data_ != 0; }
  int64_t as_int() const { return data_; }
  double as_double() const {
    double d;
    std::memcpy(&d, &data_, sizeof(double));
    return d;
  }
  // Either numeric tag widened, for the mixed-arithmetic path.
  double as_number() const {
    return is_int() ? static_cast<double>(data_) : as_double();
  }
  const std::string& as_str() const { return str_obj()->s; }
  ArrayObj* as_array() const { return reinterpret_cast<ArrayObj*>(data_); }
  ObjectObj* as_object() const {
    return reinterpret_cast<ObjectObj*>(data_);
  }
  CellObj* as_cell() const { return reinterpret_cast<CellObj*>(data_); }
  ClosureObj* as_closure() const {
    return reinterpret_cast<ClosureObj*>(data_);
  }
  GeneratorObj* as_generator() const {
    return reinterpret_cast<GeneratorObj*>(data_);
  }

  // nil, false and zero are false; everything else is true. A string's
  // emptiness and NaN are deliberately not special-cased -- JavaScript calls
  // both falsy, Lua calls neither, and that disagreement is what makes it a
  // language's decision rather than the VM's. A front end wanting other rules
  // lowers its own test instead of asking this one to grow options.
  bool truthy() const {
    switch (tag_) {
      case ValueTag::Uninit: return false;
      case ValueTag::Nil:    return false;
      case ValueTag::Bool:   return data_ != 0;
      case ValueTag::Int:    return data_ != 0;
      case ValueTag::Double: return as_double() != 0.0;
      default: return true;
    }
  }

 private:
  StrObj* str_obj() const { return reinterpret_cast<StrObj*>(data_); }

  void retain() {
    if (is_heap()) ++reinterpret_cast<HeapObj*>(data_)->rc;
  }
  void release();  // out of line: needs CellObj and ClosureObj complete

  ValueTag tag_;
  int64_t data_;
};

struct ArrayObj : HeapObj {
  ArrayObj() : HeapObj(ValueTag::Array) {}
  std::vector<Value> items;
};

// Keys in insertion order, looked up linearly. A hash map would be faster and
// would lose the order, which JavaScript's own object semantics require and
// which makes a printed object reproducible; at the sizes a front end built
// on this deals in, the scan is not what will be slow.
struct ObjectObj : HeapObj {
  ObjectObj() : HeapObj(ValueTag::Object) {}
  std::vector<std::pair<std::string, Value>> props;

  Value* find(const std::string& k) {
    for (auto& kv : props) {
      if (kv.first == k) return &kv.second;
    }
    return nullptr;
  }
  void set(const std::string& k, const Value& v) {
    if (Value* slot = find(k)) {
      *slot = v;
      return;
    }
    props.emplace_back(k, v);
  }

  // Erases the key if present; absent is a no-op. Order of the survivors
  // is preserved (props is the iteration order).
  void remove(const std::string& k) {
    for (auto it = props.begin(); it != props.end(); ++it) {
      if (it->first == k) {
        props.erase(it);
        return;
      }
    }
  }
};

struct CellObj : HeapObj {
  CellObj() : HeapObj(ValueTag::Cell) {}
  Value v;
};

struct ClosureObj : HeapObj {
  ClosureObj() : HeapObj(ValueTag::Func) {}
  int32_t func = 0;
  std::vector<Value> cells;  // every element is a Cell
};

// The storage a generator activation keeps between resumes: everything a
// vm Frame owns, minus the chunk pointer (`func` re-finds it) and the
// return register (each resume brings its own). The executor moves these
// vectors out to run and back in to suspend, so a suspend costs six moves,
// not a copy of the frame.
struct GenFrame {
  int32_t func = 0;         // chunk index
  int64_t pc = 0;           // where the next resume re-enters
  int32_t yield_reg = -1;   // register the resumed-with value lands in
  int32_t argc = 0;         // what the activation's call supplied
  std::vector<Value> locals;
  std::vector<Value> regs;
  std::vector<Value> cells;
  std::vector<Value> captures;
  std::vector<Value> defers;
  std::vector<std::pair<size_t, int32_t>> defer_marks;
};

// Calling a generator function makes one of these instead of running the
// body. Start holds the packaged arguments; Suspended holds the live frame;
// Running means the frame is on the executor's stack (its storage here is
// empty); Done frees the storage and answers every further resume with
// {value: nil, done: true}.
struct GeneratorObj : HeapObj {
  GeneratorObj() : HeapObj(ValueTag::Generator) {}
  enum class State : uint8_t { Start, Suspended, Running, Done };
  State state = State::Start;
  GenFrame frame;
};

inline Value Value::make_array(std::vector<Value> items) {
  auto* a = new ArrayObj();
  a->items = std::move(items);
  Value r;
  r.tag_ = ValueTag::Array;
  r.data_ = reinterpret_cast<int64_t>(a);
  gc_safepoint();
  return r;
}

inline Value Value::make_object() {
  Value r;
  r.tag_ = ValueTag::Object;
  r.data_ = reinterpret_cast<int64_t>(new ObjectObj());
  gc_safepoint();
  return r;
}

inline Value Value::make_cell() {
  Value r;
  r.tag_ = ValueTag::Cell;
  r.data_ = reinterpret_cast<int64_t>(new CellObj());
  gc_safepoint();
  return r;
}

inline Value Value::make_generator() {
  Value r;
  r.tag_ = ValueTag::Generator;
  r.data_ = reinterpret_cast<int64_t>(new GeneratorObj());
  gc_safepoint();
  return r;
}

inline Value Value::make_closure(int32_t func, std::vector<Value> cells) {
  auto* c = new ClosureObj();
  c->func = func;
  c->cells = std::move(cells);
  Value r;
  r.tag_ = ValueTag::Func;
  r.data_ = reinterpret_cast<int64_t>(c);
  gc_safepoint();
  return r;
}

// Reference counting alone cannot free a cycle, and closures make cycles
// reachable in one line of source -- a recursive closure stored in the cell
// it captured is cell -> closure -> cell. Nothing here pretends otherwise:
// this releases what it can, and a cycle stays counted in
// g_live_heap_objects. That count is what the tracing backstop will drive to
// zero; tests/closures.cc asserts the leak rather than its absence, so that
// collecting it later shows up as a change rather than as silence.
// Out of line (ir.cc): the zero-count path consults the runtime's drop
// hook for Objects before freeing.
void heap_release_to_zero(HeapObj* h);

inline Value Value::make_ref(HeapObj* h) {
  Value r;
  r.tag_ = h->kind;
  r.data_ = reinterpret_cast<int64_t>(h);
  ++h->rc;
  return r;
}

inline void Value::release() {
  if (!is_heap()) return;
  HeapObj* h = reinterpret_cast<HeapObj*>(data_);
  // Clear before deleting: a cell holding the last reference to a closure
  // holding this same cell would otherwise re-enter release() on a half-dead
  // object.
  tag_ = ValueTag::Nil;
  data_ = 0;
  if (--h->rc == 0) heap_release_to_zero(h);
}

}  // namespace coreir
