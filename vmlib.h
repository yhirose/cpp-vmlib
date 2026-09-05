//
//  vmlib.h
//
//  Copyright (c) 2026 Yuji Hirose. All rights reserved.
//  MIT License
//

#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// ===== coreir/value.h =====

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
//
// Map is the associative container whose keys are values rather than
// strings -- what Object deliberately is not. An Object's string keys are
// what let a struct be indexed by slot (FieldGet/FieldSet) and what the
// drop key hangs on; a language's dict/map with int, double or object keys
// needs a second container rather than a widened first one, so nothing
// about Object changes when Map arrives. Coroutine is a suspended
// activation like Generator, but of a whole frame *stack* rather than one
// frame, and entered dynamically (CoroYield anywhere below CoroResume)
// rather than lexically (Yield inside an is_generator body). Native is a
// host function registered for the run (vm::NativeDef), callable like a
// closure.
enum class ValueTag : uint8_t {
  Uninit, Nil, Bool, Int, Double, Str, Array, Object, Cell, Func, Generator,
  Map, Coroutine, Native
};

// The immediates come first and the heap tags last, so Value::is_heap is a
// single comparison against Str. Order is load-bearing, not cosmetic.
static_assert(ValueTag::Str > ValueTag::Double);
static_assert(ValueTag::Generator > ValueTag::Str);
static_assert(ValueTag::Native > ValueTag::Str);

struct HeapObj;
struct ObjectObj;

// The key under which an Object carries its destructor (Runtime::set_drop_fn
// has the contract). Unspellable from a language whose identifiers are
// printable, which is the point.
inline constexpr char kDropKey[] = "\x01" "drop";

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
  // pinned, given its destructor (the drop hook, for each condemned Object
  // carrying the drop key -- newest object first, the heap list's order),
  // handed to the finalize hook, then stripped of its references and freed
  // -- ~Runtime's own two-step, run early. Runs automatically from the
  // allocators once the heap outgrows a doubling threshold, on every
  // allocation under COREIR_GC_STRESS=1, and on demand here. Answers how
  // many objects it freed (0 when re-entered: a destructor asking for a
  // collection while one is running gets nothing).
  //
  // A destructor runs over whole objects -- the cycle it belongs to is
  // still intact -- and may store its object, or any other condemned one,
  // somewhere reachable. That resurrects it: reachability is recomputed
  // after the destructors, only what is still unreachable is freed, and a
  // resurrected object lives on intact -- already dropped, so it is not
  // dropped again when it next dies (run_drop's rule).
  int64_t collect();

  // Bytes the live objects hold: each one's own struct plus the storage its
  // containers have reserved. Computed by a walk over the heap -- O(live)
  // -- so that the allocation path carries no accounting for it.
  int64_t heap_bytes() const;

  // Called by the Value allocators after each fully-constructed object --
  // never during construction, when a half-built object cannot answer for
  // its children.
  void maybe_collect() {
    if ((stress_ || live_ >= next_gc_) && !in_collect_) collect();
  }

  // One call per object about to be freed, after the destructors have run
  // and before any of them is freed. What it does with an object must not
  // allocate on this runtime.
  void set_finalize_fn(void* ctx, void (*fn)(void* ctx, HeapObj* o)) {
    finalize_ctx_ = ctx;
    finalize_ = fn;
  }

  // Deterministic destructors: an Object whose kDropKey holds a callable
  // gets it called -- with the object as its one argument -- at the moment
  // its refcount reaches zero, before the object is freed; at the exit of
  // the scope that owns the cycle it is in (owned_scope_exit); or, last,
  // in the collection that condemns it. The hook is how: the executor
  // installs one for the lifetime of a run (it is what can call a
  // closure), and every one of those paths reaches it through run_drop.
  // The hook runs over a pinned object; if it stores the object somewhere
  // (a resurrection), the free is skipped.
  void set_drop_fn(void* ctx, void (*fn)(void* ctx, HeapObj* o)) {
    drop_ctx_ = ctx;
    drop_ = fn;
  }
  void* drop_ctx() const { return drop_ctx_; }
  void (*drop_fn() const)(void*, HeapObj*) { return drop_; }

  // The drop chokepoint: hands `o` to the hook if it is an Object carrying
  // kDropKey that has not been dropped yet, marking it dropped first -- so
  // a destructor runs at most once per object, whatever its own body
  // releases and whichever of the three paths gets there first. Answers
  // whether it ran.
  bool run_drop(HeapObj* o);

  // Deterministic drop for cycles: the owned stack (culebra's design).
  // Every Object goes on it the moment its drop key is bound, and a Scope
  // resolves, at its exit, the entries registered since it was entered --
  // dynamically, so what a callee bound counts. An entry every reference
  // to which comes from objects reachable only from itself -- a member of
  // a cycle, through plain objects, closures or cells -- gets its
  // destructor, newest first, and is then freed; one still reachable from
  // outside stays for an outer scope. An object merely hanging off a cycle
  // it is not a member of is not the scope's to resolve: the collector's
  // backstop takes it. A destructor that stores a condemned object
  // somewhere reachable spares it, intact and already dropped. The
  // analysis is a trial deletion over the candidates' own subgraph, so a
  // scope that bound nothing costs one comparison; past kOwnedNodeBudget
  // nodes every candidate survives to the backstop.
  uint64_t owned_mark() const { return owned_next_id_; }
  void owned_scope_exit(uint64_t mark);
  // ObjectObj::set / remove's chokepoints for the drop key.
  void owned_register(ObjectObj* o);
  void owned_unregister(ObjectObj* o);
  bool in_collect() const { return in_collect_; }
  // The sweep proper: condemned objects being stripped and freed. The one
  // window in which a refcount reaching zero must not run a destructor --
  // every object that could is pinned, so it cannot happen anyway.
  bool sweeping() const { return sweeping_; }

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
  friend void heap_release_to_zero(HeapObj* h);
  void link(HeapObj* o);
  void unlink(HeapObj* o);
  void mark();

  HeapObj* head_ = nullptr;
  int64_t live_ = 0;
  int64_t next_gc_ = 4096;
  bool stress_ = false;
  bool in_collect_ = false;
  bool sweeping_ = false;
  struct OwnedEntry {
    ObjectObj* obj;  // null: a tombstone (the object died by another path)
    uint64_t id;     // monotonic: a scope's mark is the next id at entry
  };
  std::vector<OwnedEntry> owned_;
  uint64_t owned_next_id_ = 0;
  void* finalize_ctx_ = nullptr;
  void (*finalize_)(void*, HeapObj*) = nullptr;
  void* drop_ctx_ = nullptr;
  void (*drop_)(void*, HeapObj*) = nullptr;
  inline static thread_local Runtime* current_ = nullptr;
};

struct HeapObj {
  int64_t rc;  // offset zero, as in culebra's refcounted structs
  // Collector scratch: how many references other heap objects hold on this
  // one (recomputed per collect), the reachability mark, and whether the
  // running collection has condemned -- and so pinned -- it.
  int64_t gc_refs = 0;
  bool gc_marked = false;
  bool gc_condemned = false;
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
// A value-keyed associative container (ValueTag::Map), a suspended frame
// stack (ValueTag::Coroutine) and a registered host function
// (ValueTag::Native). All three hold Values, so all three are defined
// after Value, like CellObj.
struct MapObj;
struct CoroObj;
struct NativeObj;
// A host function's signature (vm/exec.h defines NativeCall): answers true
// with NativeCall::result set, or false with NativeCall::error set.
struct NativeCall;
using NativeFn = bool (*)(NativeCall& call);

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
    Value r = adopt(new StrObj(std::move(s)));
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
  static Value make_map();                      // empty
  static Value make_coroutine();                // Start state; the executor fills it
  // A host function, as a value. `arity` is what FnArity answers and what
  // a call is checked against (-1: any count). `name` is diagnostics only.
  static Value make_native(std::string name, int32_t arity, void* ctx,
                           NativeFn fn);

  ValueTag tag() const { return tag_; }
  // The raw payload, for the collector's child walk only.
  int64_t raw_data() const { return data_; }

  // A retained reference to an already-live heap object -- what the drop
  // hook hands the destructor as its argument.
  static Value make_ref(HeapObj* h);

  // Every refcounted tag sorts after every immediate one, so the predicate
  // retain and release consult on every Value copy is one comparison rather
  // than a six-arm chain. The static_assert is what keeps that true: a new
  // heap tag belongs at the end, a new immediate one before Str.
  bool is_heap() const { return tag_ >= ValueTag::Str; }
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
  bool is_map() const { return tag_ == ValueTag::Map; }
  bool is_coroutine() const { return tag_ == ValueTag::Coroutine; }
  bool is_native() const { return tag_ == ValueTag::Native; }
  // Anything CallValue accepts: a closure or a host function.
  bool is_callable() const { return is_func() || is_native(); }

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
  MapObj* as_map() const { return reinterpret_cast<MapObj*>(data_); }
  CoroObj* as_coroutine() const { return reinterpret_cast<CoroObj*>(data_); }
  NativeObj* as_native() const { return reinterpret_cast<NativeObj*>(data_); }

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
  // The one place a heap pointer becomes a Value. Every object already
  // carries its own tag, so no allocator has to restate which tag it built,
  // and whatever the allocation path grows next -- accounting, an owner
  // assertion -- has a single site to grow in rather than six.
  static Value adopt(HeapObj* h) {
    Value r;
    r.tag_ = h->kind;
    r.data_ = reinterpret_cast<int64_t>(h);
    return r;
  }

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
  // The owned stack's back-pointer (-1: not on it) and the at-most-once
  // flag Runtime::run_drop sets.
  int64_t owned_idx = -1;
  bool dropped = false;

  Value* find(const std::string& k) {
    for (auto& kv : props) {
      if (kv.first == k) return &kv.second;
    }
    return nullptr;
  }
  // Binding the drop key is what puts an object on the owned stack;
  // removing it takes it off. Both go through here, so the two cannot
  // drift apart.
  // By value: a caller with a value to spare (a freshly built result object,
  // say) hands it over instead of paying a retain/release pair, and one with
  // a live one pays exactly the copy it paid before.
  void set(const std::string& k, Value v) {
    if (Value* slot = find(k)) {
      *slot = std::move(v);
    } else {
      props.emplace_back(k, std::move(v));
    }
    if (owner && k == kDropKey) owner->owned_register(this);
  }

  // Erases the key if present; absent is a no-op. Order of the survivors
  // is preserved (props is the iteration order).
  void remove(const std::string& k) {
    for (auto it = props.begin(); it != props.end(); ++it) {
      if (it->first == k) {
        props.erase(it);
        if (owner && k == kDropKey) owner->owned_unregister(this);
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
  // Where this frame's own result goes in the frame below it -- only a
  // coroutine's parked stack has a "below" to keep (a generator's single
  // frame gets its ret_reg from each resume); its bottom frame's is
  // re-patched on every resume the same way.
  int32_t ret_reg = -1;
  std::vector<Value> locals;
  std::vector<Value> regs;
  std::vector<Value> cells;
  std::vector<Value> captures;
  std::vector<Value> defers;
  std::vector<std::pair<size_t, int32_t>> defer_marks;
  std::vector<std::pair<uint64_t, int32_t>> owned_marks;
  // A generator activation caught inside a coroutine's parked stack keeps
  // its GeneratorObj here (Running state) until the stack resumes.
  Value gen_self;
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

// A coroutine: the same four states as a generator, over a whole frame
// stack. CoroCreate(f) makes one in Start holding `f`; the first
// CoroResume calls it, and a CoroYield anywhere in the frames that call
// builds -- however deep -- parks every frame from the one CoroResume
// entered (the bottom) to the yielding one (the top) here, bottom first.
// What a Yield does lexically for one frame, this does dynamically for a
// stack, and the storage is the same GenFrame per frame.
struct CoroObj : HeapObj {
  CoroObj() : HeapObj(ValueTag::Coroutine) {}
  enum class State : uint8_t { Start, Suspended, Running, Done };
  State state = State::Start;
  Value fn;                       // Start only: what the first resume calls
  std::vector<GenFrame> frames;   // Suspended only: bottom frame first
  // Set once the scheduler has taken it (Enqueue of a coroutine), and
  // from then on the executor holds its own reference to it until it
  // finishes (Exec::scheduled) -- so vm::run's end-of-run deadlock check
  // sees every scheduled coroutine still Suspended, including one nothing
  // else refers to any more.
  bool scheduled = false;
};

// A host function registered for one run (vm::RunOptions::natives) and
// reached by a program through Tag::NativeRef.
struct NativeObj : HeapObj {
  NativeObj() : HeapObj(ValueTag::Native) {}
  std::string name;      // diagnostics only
  int32_t arity = -1;    // -1: any argument count
  void* ctx = nullptr;
  NativeFn fn = nullptr;
};

// Map keys, stated once -- as the index's key type, since that is what
// every lookup actually goes through. Two values are the same key when
// they have the same tag and, for a string, the same bytes -- or, for
// everything else, the same payload word: an int by value, a double by
// bit pattern (so -0.0 and 0.0 are two keys and NaN is one; a language
// with SameValueZero normalizes before the value gets here), a heap
// object by identity. This is a third rule beside Eq (which refuses to
// compare across types at all, and refuses objects) and Same (which
// compares a string by pointer, so two equal strings from two allocations
// are not Same): a key is neither a comparison a language defines nor a
// question of which allocation this is. It lives here rather than in
// semantics.h because MapObj's own index is built on it.
//
// A MapKeyRef is a Value's tag and payload without the retain -- the entry
// vector already owns the key, and a second owning copy in the index would
// make every key look externally held to the collector (two counted
// references, one visit_children edge). A string key is compared through
// its StrObj, which the owning entry keeps alive for as long as the index
// names it.
struct MapKeyRef {
  ValueTag tag;
  int64_t bits;
  static MapKeyRef of(const Value& v) { return {v.tag(), v.raw_data()}; }
  const std::string& str() const {
    return reinterpret_cast<const StrObj*>(bits)->s;
  }
  bool operator==(const MapKeyRef& o) const {
    if (tag != o.tag) return false;
    if (tag == ValueTag::Str) return str() == o.str();
    return bits == o.bits;
  }
};
struct MapKeyHash {
  size_t operator()(const MapKeyRef& k) const {
    if (k.tag == ValueTag::Str) return std::hash<std::string_view>{}(k.str());
    const uint64_t x = static_cast<uint64_t>(k.bits) ^
                       (static_cast<uint64_t>(k.tag) << 56);
    return std::hash<uint64_t>{}(x);
  }
};

// Insertion-ordered like Object (so ObjectKeys over a map is reproducible),
// but found by hash rather than by scan: `entries` holds the order, `index`
// finds a key in O(1). A removed entry leaves a tombstone -- an Uninit key,
// which no real key can be, since a read of one traps before it reaches a
// container -- rather than shifting the entries behind it, and the vector
// is compacted once the tombstones outnumber what is live.
struct MapObj : HeapObj {
  MapObj() : HeapObj(ValueTag::Map) {}
  std::vector<std::pair<Value, Value>> entries;
  std::unordered_map<MapKeyRef, size_t, MapKeyHash> index;
  size_t live = 0;

  Value* find(const Value& k) {
    auto it = index.find(MapKeyRef::of(k));
    return it == index.end() ? nullptr : &entries[it->second].second;
  }
  void set(const Value& k, Value v) {
    auto it = index.find(MapKeyRef::of(k));
    if (it != index.end()) {
      entries[it->second].second = std::move(v);
      return;
    }
    entries.emplace_back(k, std::move(v));
    index.emplace(MapKeyRef::of(entries.back().first), entries.size() - 1);
    ++live;
  }
  void remove(const Value& k) {
    auto it = index.find(MapKeyRef::of(k));
    if (it == index.end()) return;
    auto& e = entries[it->second];
    index.erase(it);  // before the key it refers through is released
    e.first = Value::uninit();
    e.second = Value();
    --live;
    if (entries.size() > 8 && live * 2 < entries.size()) compact();
  }
  void compact() {
    std::vector<std::pair<Value, Value>> kept;
    kept.reserve(live);
    for (auto& e : entries) {
      if (!e.first.is_uninit()) kept.push_back(std::move(e));
    }
    entries.swap(kept);
    index.clear();
    for (size_t i = 0; i < entries.size(); ++i) {
      index.emplace(MapKeyRef::of(entries[i].first), i);
    }
  }
  void clear() {
    index.clear();  // first: it refers through keys the entries own
    entries.clear();
    live = 0;
  }
};

inline Value Value::make_array(std::vector<Value> items) {
  auto* a = new ArrayObj();
  a->items = std::move(items);
  Value r = adopt(a);
  gc_safepoint();
  return r;
}

inline Value Value::make_object() {
  Value r = adopt(new ObjectObj());
  gc_safepoint();
  return r;
}

inline Value Value::make_map() {
  Value r = adopt(new MapObj());
  gc_safepoint();
  return r;
}

inline Value Value::make_coroutine() {
  Value r = adopt(new CoroObj());
  gc_safepoint();
  return r;
}

inline Value Value::make_native(std::string name, int32_t arity, void* ctx,
                                NativeFn fn) {
  auto* n = new NativeObj();
  n->name = std::move(name);
  n->arity = arity;
  n->ctx = ctx;
  n->fn = fn;
  Value r = adopt(n);
  gc_safepoint();
  return r;
}

inline Value Value::make_cell() {
  Value r = adopt(new CellObj());
  gc_safepoint();
  return r;
}

inline Value Value::make_generator() {
  Value r = adopt(new GeneratorObj());
  gc_safepoint();
  return r;
}

inline Value Value::make_closure(int32_t func, std::vector<Value> cells) {
  auto* c = new ClosureObj();
  c->func = func;
  c->cells = std::move(cells);
  Value r = adopt(c);
  gc_safepoint();
  return r;
}

// Reference counting alone cannot free a cycle, and closures make cycles
// reachable in one line of source -- a recursive closure stored in the cell
// it captured is cell -> closure -> cell. Nothing here pretends otherwise:
// this releases what it can, and a cycle stays counted in Runtime::live_.
// Runtime::collect is the tracing backstop that drives that count to zero --
// test/test_gc.cc asserts it does -- but only when it runs; until then the
// cycle is live, which is what test/test_closures.cc measures.
// Out of line (ir.cc): the zero-count path consults the runtime's drop
// hook for Objects before freeing.
void heap_release_to_zero(HeapObj* h);

inline Value Value::make_ref(HeapObj* h) {
  ++h->rc;
  return adopt(h);
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

// ===== coreir/rt.h =====

// The contract every host implements: six C functions, nothing else.
//
// vm/exec.cc calls these and links against nothing beyond this declaration --
// it does not know or care whether the definitions come from the stdio
// implementation at the end of this header (VMLIB_DEFAULT_RUNTIME) or from
// a host that embeds this library elsewhere and wants its own output and
// error reporting (a script-hosted VM inside an interpreter, say). Whichever
// implementation is linked in is the only one in the binary; there is no
// indirection to pay for and no way for two hosts' behavior to blend.

extern "C" {

// Print one integer, followed by a newline.
void coreir_rt_out(int64_t v);

// Print one string, followed by a newline. Everything that is not an integer
// arrives here, formatted by the executor. A separate symbol rather than a
// widened coreir_rt_out, deliberately: a host defines these itself, so
// changing an existing signature breaks that host's build while adding one
// does not.
void coreir_rt_out_str(const char* bytes, int64_t len);

// The same, without the newline -- a language's `print` next to its
// `println`. Added as a sixth symbol under the same rule as the fifth.
void coreir_rt_out_raw(const char* bytes, int64_t len);

// Read one line and convert it to an integer. A line that is not an integer,
// or end of input, fails at the position given.
int64_t coreir_rt_in(int64_t line, int64_t col);

// Report and terminate the running program -- and only that. A failure a
// running program can recover from never arrives here: a trap (divide by
// zero, a wrong-typed operand) or a Throw unwinds the executor's own frame
// stack first, and one a TryCatch guards resumes at its handler. What does
// arrive is fatal by construction: an unguarded failure whose frames the
// unwinder has already popped and released, reported with a trap's original
// diagnostic or as "uncaught: <value>" for a Throw no handler took.
//
// What "terminate" means is the host's choice -- exit the process, or throw
// a host-level exception -- as long as it does not return. A host must still
// not implement this with longjmp: the throw's path out of vm::run runs
// destructors (the executor and its containers among them), and jumping past
// them is undefined behavior.
[[noreturn]] void coreir_rt_fail(const char* msg, int64_t line, int64_t col);

// Called on every loop back-edge and call, so a host that wants to interrupt
// a running program (Ctrl+C, a cooperative cancellation flag) has a place to
// do it. The default implementation is a no-op.
void coreir_rt_poll(void);

}  // extern "C"

namespace coreir_rt {

// A std::string convenience over coreir_rt_fail -- not a second formatter,
// just an argument-type adapter, so callers do not each write
// `msg.c_str()` themselves.
[[noreturn]] inline void fail(const std::string& msg, uint32_t line,
                              uint32_t col) {
  coreir_rt_fail(msg.c_str(), line, col);
}

}  // namespace coreir_rt

// ===== coreir/ir.h =====

// coreir -- a closed intermediate representation.
//
// A front end lowers its own grammar into these fifteen node shapes; nothing in
// this header, or in any backend that consumes it, knows what parser produced
// them. That is the whole point: the tag set is closed, so a backend written
// once serves every front end that can reach it.
//
// Node is a fixed-size POD with no virtual functions, no RTTI, no
// std::function and no static initializer. Those are what survive
// --gc-sections as vtable/typeinfo/comdat residue in a binary that never uses
// them, so keeping them out is a size property, not a style preference.

namespace coreir {

// ---------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------

// Convention: every node produces a value. Statements produce Void. Block is
// the value of its last child, If the value of the branch taken, Call the
// callee's return value. PL/0 never observes this -- everything is Void -- but
// fixing it now keeps an expression-oriented front end from having to redefine
// Block/If/Call later, which would be a breaking change rather than a new tag.
enum class Tag : uint8_t {
  Literal,    // a = const pool index
  VarRef,     // op = VarKind, a = index
  Unary,      // op = UnOp,    children: operand
  Binary,     // op = BinOp,   children: lhs, rhs
  Assign,     // op = VarKind, a = index, children: value
  If,         // children: cond, then [, else]
  // A branch on one of several literal keys, all the same coreir::ConstKind
  // (Int or Str) and pairwise distinct -- verify()'s job, so lowering never
  // meets an ambiguous key set. Each key is a Literal child rather than a
  // side table, the shape ObjectLit's key/value pairs already are; an even
  // child count means the last child is a default arm, odd means there is
  // none. Yields the taken arm's value, like If -- and like If, a subject
  // that matches no key and has no default yields nil rather than trapping.
  // A subject whose runtime type does not match the keys' ConstKind does
  // trap (the same line Eq draws against cross-type comparison): a C#
  // `switch (string)` seeing null is a front end's job to catch before the
  // switch, by testing for nil first and branching to its own default.
  // Break does not stop at a Switch, the same way it does not stop at an
  // If -- a Break inside an arm still targets the enclosing While. Arms
  // that fall through (Java) or a switch-scoped break (C#/Java) are front
  // end lowerings, not new tags.
  Switch,     // children: subject, key, body, key, body, ... [, default]
  While,      // children: cond, body
  Block,      // children: stmts...  (zero children = the empty statement)
  Intrinsic,  // op = IntrinsicId, children: args...
  // Calling, in two halves. MakeClosure yields a callable value owning the
  // cells its capture map names, resolved in the frame that builds it;
  // CallValue calls whatever a value turns out to be.
  //
  // There is deliberately no third tag for "call this function by index".
  // One existed, forwarding the caller's slots directly, and it was both
  // faster and unable to express a function that outlives the frame it was
  // written in. Keeping it would have meant two ownership rules in one
  // Frame -- a borrowed slot pointer and an owned cell -- with the meaning of
  // VarKind::Capture depending on which kind of call got you there. A front
  // end that wants the old shape builds a closure and calls it immediately;
  // examples/pl0 does exactly that.
  MakeClosure,  // a = func index, b = capture map index
  CallValue,    // children: callee, args...
  // Containers. Index and SetIndex dispatch on what the receiver turns out
  // to be, rather than there being a separate pair per container type: a[0]
  // and o["k"] differ only in what a and o are, and a front end that wants
  // them to be different syntax can still lower both to these.
  ArrayLit,   // children: items...
  ObjectLit,  // children: key, value, key, value, ...
  Index,      // children: receiver, key
  SetIndex,   // children: receiver, key, value
  // A struct field, at a slot the front end assigns -- the same contract
  // VarKind::Local's slot indices already are. Where Index looks a key up
  // by comparing it against every prop in turn, FieldGet/FieldSet read
  // props[slot] directly: O(1) rather than O(props.size()), for a receiver
  // whose shape (which field lives at which slot) is known at compile time
  // rather than discovered at runtime, which a statically-typed struct's
  // fields are and a dynamic object's keys are not. The receiver is still
  // an ObjectObj -- same drop key, same owned-stack machinery, same
  // ObjectKeys -- so a struct is exactly an Object a front end has
  // promised to index this way instead of by key. That promise has two
  // obligations: every field is present (built via ObjectLit, which fills
  // props in key order and never leaves a gap) before any FieldGet/FieldSet
  // reaches it, and a struct is never handed to ObjectRemove -- removing a
  // key would shift every later field's slot out from under the front
  // end's own numbering.
  // a = props index, b = field name (str const, diagnostics only -- read on
  // the trap path, never to execute anything).
  FieldGet,   // children: receiver
  FieldSet,   // children: receiver, value
  // A lexical region and the local slots it owns: a = first local,
  // b = one past last. Slot indices are the front end's to assign, so the
  // scope structure they follow is too -- this is how it says it. The
  // compiler releases the range when the region exits, however it exits
  // (falling off the end, or the unwinding a later phase adds), which is
  // what makes a value's lifetime end with its scope rather than with the
  // whole frame. The release is last-declared-first, and after it the
  // scope resolves the drop-bearing objects bound under it that a cycle
  // kept alive (Runtime::owned_scope_exit). Yields its child's value,
  // like Block.
  //
  // A captured local lives in a Cell, not in its slot, and cells are not
  // in the range -- so a front end that needs reverse declaration order
  // across the two hands the scope its release list as an optional second
  // child: a Block of VarRef nodes (Local or Cell, no duplicates, the
  // locals within the range), in the order they are to be released. The
  // scope then releases exactly that list, on every exit; a released cell
  // is replaced by a fresh one, as CellFresh would. Without it the range
  // is released last-slot-first and the cells stay the frame's.
  Scope,      // a = first local, b = one past last;
              // children: body [, release order: Block of VarRef]
  // Non-local exits. Statements, like PL/0's: nothing reads their value.
  // Each one leaves every Scope between it and its target the way the
  // scope's own exit would -- locals released -- which is what earns them a
  // place in the IR rather than being a front-end Jump. Break and Continue
  // name their loop by how many enclosing While bodies to skip: a = 0 is
  // the innermost (the plain break), a = 1 the one around it -- Java's and
  // Go's labeled break/continue, with the label resolved to a depth by the
  // front end. verify() requires the depth to name a loop that is open.
  Return,     // children: value (optional; none returns nil)
  Break,      // a = loops to skip (0: innermost); no children
  Continue,   // a = loops to skip (0: innermost); no children
  // Exceptions. Throw raises any value; TryCatch guards its first child,
  // lands what a throw (or a trap the executor raises itself -- divide by
  // zero, a wrong-typed operand) carried in the local slot `a`, and resumes
  // in its second. Yields the value of whichever child finished, like If.
  // The handler is not guarded by its own try; a throw there unwinds to the
  // next enclosing one.
  Throw,      // children: value
  TryCatch,   // a = caught local slot; children: body, handler
  // Registers a 0-arity callable to run when the enclosing Scope exits --
  // however it exits: falling through, Break, Continue, Return, or an
  // unwinding throw. LIFO within the scope. verify() requires an enclosing
  // Scope: a front end that wants function-level defers wraps the function
  // body in one.
  Defer,      // children: value (a callable)
  // Replaces one of the frame's cells with a fresh, nil-holding box. What a
  // "loop iteration's own binding" means is exactly this: closures built in
  // earlier iterations keep the old cell (their captures own it), while the
  // scope entering now declares into the new one. A front end emits it at
  // the top of any region whose per-entry bindings are captured.
  CellFresh,  // a = cell index
  // Suspends the enclosing generator function, handing its child's value to
  // whoever resumed it, and yields -- when a later GenResume re-enters --
  // the value that resume was called with. verify() requires the enclosing
  // Func to carry is_generator; calling such a function builds a suspended
  // activation instead of running the body, and the GenResume / GenReturn
  // intrinsics drive it from there.
  Yield,      // children: value
  // A host function, as a value: the one Module::natives[a] names. What
  // the name resolves to is the run's business (vm::RunOptions::natives),
  // not the module's -- the same module runs against any host that
  // supplies the names it declares, and a name the host does not supply
  // fails the run before its first instruction rather than at the call.
  // Yields a callable; a front end that calls one host function from many
  // sites hoists this into a Cell exactly as the Static calls recipe hoists
  // a MakeClosure.
  NativeRef,  // a = index into Module::natives
};

// The last enumerator, for the scans (from_name, test_names) that walk the
// enum's range.
inline constexpr Tag kLastTag = Tag::NativeRef;

// WrapI8..WrapU32 truncate an int to a narrower width and sign- or
// zero-extend it back to int64 -- non-int traps, like Neg and BitNot. A
// front end for a fixed-width language (a C#/Java/Go int, say) keeps every
// slot holding one of these normalized values: a signed width sign-extended,
// an unsigned width zero-extended (so it reads as a non-negative int64), and
// a u64 as int64's own bit pattern verbatim. That single convention is what
// lets Lt/Le/Div/Mod/Shr stay correct unchanged for every width up to 32
// bits -- only the arithmetic that can leave the width (Neg, Add, Sub, Mul,
// Shl, and Div/BitNot at their boundaries -- see the README's recipe table)
// needs a Wrap after it, and u64 needs its own comparison, division and
// shift (BinOp's U-prefixed group) because its bit pattern does not fit
// int64's own ordering.
enum class UnOp : uint8_t {
  Neg, BitNot,
  WrapI8, WrapI16, WrapI32, WrapU8, WrapU16, WrapU32,
};

enum class BinOp : uint8_t {
  Add, Sub, Mul, Div, Mod,
  Eq, Ne, Lt, Le, Gt, Ge,
  // Int-only. Shift counts are masked to the low six bits (1 << 64 == 1),
  // matching the hardware and Java rather than trapping; Shr is arithmetic.
  // A narrower width's own shift-count rule (Java's int shift masks to 5
  // bits, not 6) is the front end's to apply before this -- BitAnd the count
  // against the width's own mask -- rather than this growing a width
  // parameter.
  BitAnd, BitOr, BitXor, Shl, Shr,
  // u64 as unsigned: int64's own Lt/Le/Gt/Ge/Div/Mod/Shr read a u64's bit
  // pattern as negative once its top bit is set, so a front end normalizing
  // u64 into that bit pattern (WrapU8..WrapU32's own convention, one width
  // further) needs these instead. Eq/Ne need no U form -- a bit pattern
  // comparison does not care about sign -- and neither does Add/Sub/Mul,
  // whose wrapping result is the same bits either way.
  UDiv, UMod, UShr, ULt, ULe, UGt, UGe,
};

// The last enumerator of each, for the scans (from_name, test_names) that
// walk the enum's range -- kLastTag's own reason, one enum further.
inline constexpr UnOp kLastUnOp = UnOp::WrapU32;
inline constexpr BinOp kLastBinOp = BinOp::UGe;

// ToStr formats any value the way to_display (coreir/semantics.h) does --
// shortest-round-trip doubles included. A language whose display rules
// differ (culebra prints whole doubles as "4.0") post-processes the result
// rather than this growing a mode; what it cannot do in-language is produce
// digits from a number at all, which is why this is an intrinsic.
// TypeOf yields the value's tag as a string -- type_name's vocabulary
// ("int", "double", "string", ...). A dynamic front end needs it for any
// dispatch its own semantics do on a value's kind; its own type names are
// its own mapping to write over this one.
// The numeric conversions and the two float operations a front end cannot
// write in-language: ToInt truncates a double toward zero (and traps on
// NaN, an infinity, or a value outside int64's range); ToDouble widens an
// int; FMod is IEEE fmod over doubles (int operands widen; a zero divisor
// traps, like integer Mod); Pow is std::pow over doubles. Integer
// exponentiation is deliberately absent -- a loop over wrapping Mul writes
// it in-language, in whatever overflow discipline the language wants.
enum class IntrinsicId : uint8_t {
  Print, ReadInt, Len, ToStr, TypeOf, ToInt, ToDouble, FMod, Pow,
  // Print without the trailing newline, through coreir_rt_out_raw. The value
  // is formatted the way ToStr formats it.
  PrintRaw,
  // The container primitives a front end cannot write over Index/SetIndex
  // alone: growing and shrinking an array, and asking an object what it
  // holds. Everything else -- insert, slices, maps, filters -- is a rebuild
  // loop a front end writes in its own language.
  ArrayPush,    // (array, value) -> nil, appends
  ArrayPop,     // (array) -> last value, removed; an empty array fails
  ObjectHas,    // (object, key) -> bool (Index reads a missing key as nil,
                //  which cannot tell absent from nil-valued)
  ObjectKeys,   // (object) -> array of keys, insertion order
  ObjectRemove, // (object, key) -> nil; removing an absent key is a no-op
  // How many arguments the running function was called with -- the count
  // the caller supplied, not num_params. Only interesting under
  // Func::lenient_arity, where the two can differ; 0 at the entry point.
  ArgCount,     // () -> int
  // Reference identity: whether two values are the same heap object (or,
  // for scalars, the same tag and payload). Eq deliberately refuses two
  // objects -- what "equal" means for them is the language's call -- but
  // every language needs this one primitive underneath its answer.
  Same,         // (a, b) -> bool
  // A function value's declared parameter count -- Func::num_params, which
  // is every parameter (there are no hidden ones: captures travel as cells,
  // and a generator's parameters are the same locals). The one fact a front
  // end needs to check "this callback takes two arguments" before calling
  // it. A host function answers the arity it was registered with, or -1
  // when it was registered as taking any count; anything else traps.
  FnArity,      // (f) -> int, or -1 for a variadic host function
  // The tracing collector, on demand: a full collection right now, on top
  // of the ones the allocators run on their own (Runtime::collect). Answers
  // how many objects it freed. A condemned Object carrying the drop key
  // gets its destructor -- the same closure call a refcount death makes,
  // over a still-whole cycle, newest object first -- and one that stores
  // itself somewhere reachable is spared (Runtime::collect has the rule).
  // Called from inside a destructor it answers 0: one collection at a
  // time.
  Collect,      // () -> int (objects freed)
  // The heap's current size, as a fresh {live_objects, heap_bytes} object:
  // live_objects is Runtime::live_objects(), heap_bytes is
  // Runtime::heap_bytes(). Both are read before the result object is
  // allocated, so it does not count itself. The keys are the contract; a
  // host may add to them but not rename them.
  HeapStats,    // () -> {live_objects: int, heap_bytes: int}
  // Generators. Both answer with a fresh {value, done} object -- the JS
  // result shape, chosen because it carries "finished" and "what came out"
  // in one allocation a front end can destructure however its own protocol
  // likes. Resume re-enters at the Yield (whose value becomes the sent
  // argument; a first resume's is ignored) and runs to the next Yield
  // ({value, done: false}) or to the body's return ({value, done: true}).
  // A finished generator answers {value: nil, done: true}; resuming one
  // that is already running traps. Return closes early: it runs the
  // suspended frame's pending defers -- innermost first, as the yield
  // point's own Return would -- then discards the frame and answers
  // {value: <arg>, done: true}. A dropped generator runs nothing.
  GenResume,    // (generator, sent) -> {value, done}
  GenReturn,    // (generator, value) -> {value, done: true}
  // Throw delivers a value into a suspended generator at its Yield, as if
  // the yield expression had thrown it: the body's own handlers get the
  // first look, its defers run as the throw crosses them, and one the body
  // does not catch reaches this call with the generator done. A body that
  // catches it and yields again answers {value, done: false} like any
  // resume. A generator that never started or already finished has no
  // frame for the value to land in, so it is thrown here directly (and a
  // Start-state one is done, its arguments dropped).
  GenThrow,     // (generator, value) -> {value, done}
  // The job queue: a FIFO of 0-argument closures the run drains once the
  // entry frame has returned, each driven to completion before the next,
  // so a job's own enqueues run after every job already waiting. The
  // primitive a language's microtasks are made of -- a Promise reaction,
  // a queued callback -- with the language's own queue discipline (which
  // settles what, in what order) written on top of it. A job's uncaught
  // throw ends the run the way the entry frame's would; the queue is
  // never drained inside a job (it is not re-entrant), only between them.
  //
  // A coroutine may be enqueued too, which makes the queue a scheduler:
  // taken from the queue it is resumed (a first time as f(nil), later at
  // its CoroYield with nil) and driven until it yields or finishes; a
  // yield does not put it back -- whoever it is waiting for enqueues it
  // again (a channel's receiver waking its sender, say), and a coroutine
  // that wants merely to let others run enqueues itself first. That is
  // every green-thread primitive: spawn is Enqueue(CoroCreate(f)), block
  // is "record CoroCurrent() somewhere, then CoroYield", wake is Enqueue.
  // A coroutine once enqueued is the scheduler's until it finishes: if
  // the queue runs dry while any such coroutine is still suspended,
  // nothing can ever wake it, and the run fails with a deadlock diagnostic
  // through coreir_rt_fail -- Go's "all goroutines are asleep".
  Enqueue,      // (closure | coroutine) -> nil
  // Rounds a double to what it would be after passing through a 32-bit
  // float -- the one float-specific fact a front end for a language with
  // both `float` and `double` cannot write in-language, the same way ToStr
  // cannot produce digits from a number at all. An int widens first, like
  // ToDouble. A front end for such a language re-applies this after every
  // arithmetic op on a float value, the same discipline WrapI32 et al. use
  // for fixed-width ints (see the README's Fixed-width integers section) --
  // this is that discipline's one addition for a language with `float`.
  ToFloat32,    // (n) -> double, rounded to float precision
  // The string and array primitives a front end cannot write over Index
  // alone at a reasonable cost. A slice is the half-open range [i, j),
  // bounds-checked the way Index is (semantics.h's slice_error) and always
  // a fresh value; a front end that wants Python's clamping or negative
  // indices normalizes first. StrByte reads one byte as its 0..255 value
  // (Index on a string yields a one-byte string, which nothing can turn
  // into a number), and StrFromByte is its inverse -- together they are
  // what a language's ord/chr, or any UTF-8 decoding it writes itself,
  // bottoms out in.
  StrSlice,     // (str, i, j) -> str
  ArraySlice,   // (array, i, j) -> array
  StrByte,      // (str, i) -> int, 0..255; out of range traps like Index
  StrFromByte,  // (int 0..255) -> str of one byte; anything else traps
  // A fresh, empty Map (value.h's MapObj). Filled through SetIndex, read
  // through Index, and asked about through ObjectHas / ObjectKeys /
  // ObjectRemove, each of which accepts a map receiver as well as an
  // object one -- Index already dispatches on the receiver's kind, and the
  // three questions are the same questions.
  MapNew,       // () -> map
  // Coroutines: Yield's dynamic counterpart, over a whole frame stack. A
  // generator suspends the one frame whose body lexically contains the
  // Yield; a coroutine suspends every frame from the one CoroResume
  // entered down to wherever CoroYield is reached -- three calls deep, in
  // a callback, in a function that has no idea it is running inside one.
  // What Lua's coroutines, Ruby's Fibers and a goroutine are made of, and
  // what an `await` that is not itself in a generator body needs.
  //
  // CoroCreate(f) answers a coroutine in its Start state holding `f`. The
  // first CoroResume(co, v) calls f(v) -- one argument, so f takes one
  // parameter (or is lenient) -- and runs until a CoroYield or f's return;
  // each later CoroResume re-enters at the CoroYield, which yields the
  // sent value. Both answer the {value, done} object GenResume does:
  // {yielded, false} at a CoroYield, {returned, true} at f's return, and
  // {nil, true} for a coroutine already done. A throw f's frames do not
  // catch finishes the coroutine and continues at the CoroResume, into
  // the resumer's own handlers. CoroYield finds the innermost running
  // coroutine's frames above it and parks them; with none, it traps --
  // and with the coroutine's bottom frame below a host boundary (a native
  // that called back in, a destructor, a defer, a job being driven) it
  // traps too, since C++ frames cannot be parked: Lua's "attempt to yield
  // across a C-call boundary", stated once in Exec's CoroYield.
  //
  // CoroClose(co) finishes a suspended coroutine early, running its parked
  // frames' pending defers innermost frame first (as GenReturn does for a
  // generator's one frame); on one in Start it just drops `f`; on a done
  // one it is a no-op; on a running one it traps. A coroutine that is
  // dropped rather than closed runs nothing, the generator's rule.
  // CoroStatus answers "start" / "suspended" / "running" / "done", and
  // CoroCurrent the innermost running coroutine, or nil outside any.
  CoroCreate,   // (f) -> coroutine
  CoroResume,   // (coroutine, sent) -> {value, done}
  CoroYield,    // (value) -> sent
  CoroClose,    // (coroutine) -> nil
  CoroStatus,   // (coroutine) -> str
  CoroCurrent,  // () -> coroutine | nil
};

// The last enumerator, for the scans (from_name, test_names) that walk the
// enum's range: one place to move when an intrinsic is added, rather than
// every scan naming the last one itself.
inline constexpr IntrinsicId kLastIntrinsic = IntrinsicId::CoroCurrent;

// A variable is either a slot in this frame or a slot borrowed from an
// enclosing one. There is deliberately no "level" -- static links assume the
// defining activation is still on the stack, which closures break. A capture
// list survives first-class functions, so VarRef's meaning does not have to
// change when they arrive.
// Cell joins the two. A local a closure captures cannot stay a slot in
// the frame -- the closure may outlive the frame -- so a front end promotes it
// to a Cell, a heap box the frame and every closure over it share. Which
// locals need promoting is the front end's analysis to do (culebra's
// FnAnalysis calls them captured_locals); the IR only needs the distinction to
// exist. A CaptureSrc therefore names a Cell or a Capture, never a Local.
enum class VarKind : uint8_t { Local, Capture, Cell };

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

inline constexpr uint32_t kNoNodeId = 0xFFFFFFFFu;

struct NodeId {
  uint32_t v = kNoNodeId;
  constexpr bool valid() const { return v != kNoNodeId; }
};

inline constexpr bool operator==(NodeId a, NodeId b) { return a.v == b.v; }
inline constexpr bool operator!=(NodeId a, NodeId b) { return a.v != b.v; }

struct SrcPos {
  uint32_t line = 0;
  uint32_t col = 0;
};

// `bits` is the payload for Int, Bool and Double (the last as its bit
// pattern); for Str it indexes Module::str_consts rather than holding the
// bytes, so Const stays a two-word POD and the pool stays in one place.
enum class ConstKind : uint8_t { Nil, Int, Bool, Double, Str };

struct Const {
  ConstKind kind = ConstKind::Int;
  int64_t bits = 0;
};

// Where a closure's capture comes from, expressed in the frame that builds
// it. A per-function list would not work: fib's captures live in the root
// frame, but fib's own recursive call runs with fib's frame, and finding "the
// defining frame" at run time is exactly the static link this design rejects.
// So the forwarding table belongs to the site that makes the closure.
struct CaptureSrc {
  VarKind from = VarKind::Local;
  int32_t index = 0;
};

struct Func {
  std::string name;
  int32_t num_locals = 0;
  int32_t num_captures = 0;
  NodeId body;
  // Diagnostics only -- never read to execute anything. Kept the way culebra
  // keeps Chunk::positions: information the run does not need, held
  // structurally so error messages do not have to thread it by hand.
  std::vector<std::string> local_names;
  std::vector<std::string> capture_names;
  // Cells are storage the frame shares with closures built inside it;
  // params are the first `num_params` locals, so an argument that needs
  // capturing is copied into a cell by the front end and the calling
  // convention itself stays about locals only. Both sit after the existing
  // members so that every brace initializer already written still means what
  // it did.
  int32_t num_cells = 0;
  int32_t num_params = 0;
  // Calling this function packages an activation instead of running it; its
  // body may Yield. Appended after the two above, for brace-init
  // compatibility.
  bool is_generator = false;
  // Calls tolerate any argument count: extras are dropped, missing params
  // start as nil, and ArgCount tells the body how many were actually
  // passed -- JavaScript's convention, so a front end can raise its own
  // "missing argument" diagnostic (with the parameter's name, which the
  // executor never knew) or fill a default. Off, a mismatch traps.
  bool lenient_arity = false;
  // A CallValue in tail position (the operand of a Return, or the value the
  // body ends in, through any Block / If / Switch / Scope on the way)
  // replaces this activation instead of stacking on it, so a loop written
  // as a call chain runs in one frame. The frame's exit happens *before*
  // the callee runs: locals released last-slot-first, each open Scope's
  // owned cycles resolved -- Rust's rule for `become`, and the one
  // observable difference from a plain call, which is why this is the
  // front end's to switch on rather than the default. Not applied inside
  // a TryCatch body (the handler is a pc range of this chunk, and would be
  // lost with the frame) or a Scope that declares defers (which the exit
  // ordering above would run before the callee); and only for a callee
  // that is a plain closure -- a native or a generator function is called
  // the ordinary way and the frame returns its result. Off, a tail call
  // is a call.
  bool tail_calls = false;
};

struct Node {
  Tag tag = Tag::Block;
  uint8_t op = 0;        // UnOp / BinOp / IntrinsicId / VarKind, per tag
  uint32_t pos = 0;      // index into Module::positions
  int32_t a = 0;         // const index / var index / func index
  int32_t b = 0;         // capture map index
  uint32_t first_child = 0;
  uint32_t num_children = 0;
};

struct Module {
  std::vector<Node> nodes;
  std::vector<NodeId> child_ids;  // flat backing for every node's children
  std::vector<SrcPos> positions;
  std::vector<Const> consts;
  std::vector<std::string> str_consts;  // bytes for ConstKind::Str
  std::vector<Func> funcs;                      // funcs[0] is the entry point
  std::vector<std::vector<CaptureSrc>> capture_maps;
  // The host functions this module calls, by name; Tag::NativeRef indexes
  // this. Resolved against the run's RunOptions::natives by vm::run.
  std::vector<std::string> natives;

  const Node& at(NodeId id) const { return nodes[id.v]; }
  NodeId child(NodeId id, uint32_t i) const {
    return child_ids[nodes[id.v].first_child + i];
  }
  uint32_t num_children(NodeId id) const { return nodes[id.v].num_children; }
  SrcPos pos_of(NodeId id) const { return positions[nodes[id.v].pos]; }
  int64_t int_const(NodeId id) const { return consts[nodes[id.v].a].bits; }
  // The other four literal kinds int_const doesn't cover: what a Literal
  // node actually holds is (kind, bits), and only the caller who already
  // knows the kind should decode bits -- const_kind is how it finds out.
  ConstKind const_kind(NodeId id) const { return consts[nodes[id.v].a].kind; }
  bool bool_const(NodeId id) const { return consts[nodes[id.v].a].bits != 0; }
  double double_const(NodeId id) const {
    double d;
    const int64_t bits = consts[nodes[id.v].a].bits;
    std::memcpy(&d, &bits, sizeof(double));
    return d;
  }
  const std::string& str_const(NodeId id) const {
    return str_consts[static_cast<size_t>(consts[nodes[id.v].a].bits)];
  }
  // The same decode as str_const, from a raw const-pool index rather than a
  // Literal NodeId -- what FieldGet/FieldSet's diagnostic-only name const
  // is (it names a field, not an operand position a node's own const index
  // would point through).
  const std::string& str_const_at(int32_t const_index) const {
    return str_consts[static_cast<size_t>(
        consts[static_cast<size_t>(const_index)].bits)];
  }
};

// ---------------------------------------------------------------------------
// Arity
//
// -1 means variadic, or -- for If and Intrinsic -- "constrained, but not by a
// single number." The fixed-arity tags have their shape stated exactly
// once, here. If's 2-or-3 and Intrinsic's per-id count are still centralized,
// just in Verifier::check_node and intrinsic_arity() respectively rather than
// in this table. Call's shape (capture map length against the callee) has no
// single-number arity at all and lives entirely in check_node.
// ---------------------------------------------------------------------------

inline constexpr int arity_of(Tag t) {
  switch (t) {
    case Tag::Literal:   return 0;
    case Tag::VarRef:    return 0;
    case Tag::Unary:     return 1;
    case Tag::Binary:    return 2;
    case Tag::Assign:    return 1;
    case Tag::If:        return -1;  // 2 or 3
    case Tag::Switch:    return -1;  // subject, (key, body)*, optional default
    case Tag::While:     return 2;
    case Tag::Block:     return -1;
    case Tag::Intrinsic: return -1;  // per IntrinsicId
    case Tag::MakeClosure: return 0;
    case Tag::CallValue:   return -1;  // callee, then args
    case Tag::ArrayLit:    return -1;
    case Tag::ObjectLit:   return -1;  // an even number: key, value, ...
    case Tag::Index:       return 2;
    case Tag::SetIndex:    return 3;
    case Tag::FieldGet:    return 1;
    case Tag::FieldSet:    return 2;
    case Tag::Scope:       return -1;  // body, or body + release list
    case Tag::Return:      return -1;  // 0 or 1
    case Tag::Break:       return 0;
    case Tag::Continue:    return 0;
    case Tag::Throw:       return 1;
    case Tag::TryCatch:    return 2;
    case Tag::Defer:       return 1;
    case Tag::CellFresh:   return 0;
    case Tag::Yield:       return 1;
    case Tag::NativeRef:   return 0;
  }
  return -1;
}

// How many slots of a given kind a function has. One place, because the
// verifier bounds-checks three different things against it (a VarRef, an
// Assign and a capture map entry) and a fourth kind would otherwise mean
// finding all three.
inline int32_t slot_limit(const Func& f, VarKind k) {
  switch (k) {
    case VarKind::Local:   return f.num_locals;
    case VarKind::Capture: return f.num_captures;
    case VarKind::Cell:    return f.num_cells;
  }
  return 0;
}

inline constexpr uint32_t intrinsic_arity(IntrinsicId id) {
  switch (id) {
    case IntrinsicId::Print:   return 1;
    case IntrinsicId::ReadInt: return 0;
    case IntrinsicId::Len:     return 1;
    case IntrinsicId::ToStr:   return 1;
    case IntrinsicId::TypeOf:  return 1;
    case IntrinsicId::ToInt:   return 1;
    case IntrinsicId::ToDouble: return 1;
    case IntrinsicId::FMod:    return 2;
    case IntrinsicId::Pow:     return 2;
    case IntrinsicId::PrintRaw: return 1;
    case IntrinsicId::ArrayPush: return 2;
    case IntrinsicId::ArrayPop: return 1;
    case IntrinsicId::ObjectHas: return 2;
    case IntrinsicId::ObjectKeys: return 1;
    case IntrinsicId::ObjectRemove: return 2;
    case IntrinsicId::ArgCount: return 0;
    case IntrinsicId::Same: return 2;
    case IntrinsicId::FnArity: return 1;
    case IntrinsicId::Collect: return 0;
    case IntrinsicId::HeapStats: return 0;
    case IntrinsicId::GenResume: return 2;
    case IntrinsicId::GenReturn: return 2;
    case IntrinsicId::GenThrow: return 2;
    case IntrinsicId::Enqueue: return 1;
    case IntrinsicId::ToFloat32: return 1;
    case IntrinsicId::StrSlice: return 3;
    case IntrinsicId::ArraySlice: return 3;
    case IntrinsicId::StrByte: return 2;
    case IntrinsicId::StrFromByte: return 1;
    case IntrinsicId::MapNew: return 0;
    case IntrinsicId::CoroCreate: return 1;
    case IntrinsicId::CoroResume: return 2;
    case IntrinsicId::CoroYield: return 1;
    case IntrinsicId::CoroClose: return 1;
    case IntrinsicId::CoroStatus: return 1;
    case IntrinsicId::CoroCurrent: return 0;
  }
  return 0;
}

// Whether a tag yields a value usable as an operand. Under the "every node
// produces a value" convention every tag does in principle, but PL/0's
// statements are Void, and verify() uses this to catch a front end that wires
// a statement into an operand position.
inline constexpr bool yields_value(Tag t) {
  switch (t) {
    case Tag::Literal:
    case Tag::VarRef:
    case Tag::Unary:
    case Tag::Binary:
    case Tag::Intrinsic:
    // Block and If were always documented as producing one -- "Block is the
    // value of its last child, If the value of the branch taken" at the top
    // of this header -- and were only false here because PL/0 has no way to
    // observe it. A function whose body is a block that ends in an expression
    // does observe it.
    case Tag::Block:
    case Tag::If:
    case Tag::Switch:
    case Tag::MakeClosure:
    case Tag::CallValue:
    case Tag::ArrayLit:
    case Tag::ObjectLit:
    case Tag::Index:
    case Tag::FieldGet:
    case Tag::Scope:
    case Tag::TryCatch:
    case Tag::Yield:
    case Tag::NativeRef:
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Views -- the only way to read a node's children
//
// Each view is paired with a Builder::make_* below. Nothing outside this
// header should index child_ids directly, so a tag's layout lives in exactly
// one place even though both the binder that builds a Module and the
// compiler that reads it need to agree on that layout.
//
// Views hold NodeId, never const Node* -- the arena grows and pointers into it
// do not survive that.
// ---------------------------------------------------------------------------

struct UnaryView  { UnOp op; NodeId operand; };
struct BinaryView { BinOp op; NodeId lhs, rhs; };
struct AssignView { VarKind kind; int32_t index; NodeId value; };
struct VarRefView { VarKind kind; int32_t index; };
struct IfView     { NodeId cond, then_, els; };  // els may be invalid
// Switch's shape is decided by parity, not position: subject, (key, body)
// pairs, then a default if the count came out even. Stated once here for
// the two verify() passes and FnCompiler that would each otherwise
// rederive it -- the discipline the rest of this section is for.
// default_body is invalid when there is none, the same optional-child
// shape IfView's `els` uses; the arms themselves stay in the arena, read
// by index through switch_key/switch_body the way a Block's statements are
// read through Module::child.
struct SwitchView { NodeId subject, default_body; uint32_t arm_count; };
struct WhileView  { NodeId cond, body; };
struct ClosureView { int32_t func; int32_t capture_map; };
struct IntrinsicView { IntrinsicId id; };
// release_order is invalid when the Scope has no explicit release list (the
// range releases last-slot-first instead) -- the same optional-child shape
// IfView's `els` uses.
struct ScopeView { int32_t first_local, end_local; NodeId body, release_order; };
struct TryView { int32_t caught_local; NodeId body, handler; };
struct CellFreshView { int32_t cell; };
struct FieldView { int32_t slot, name_const; NodeId receiver; };
struct FieldSetView { int32_t slot, name_const; NodeId receiver, value; };
// Break and Continue carry the same one field, so they share a view: how
// many enclosing loops to skip, 0 being the innermost.
struct LoopJumpView { int32_t depth; };
struct NativeRefView { int32_t index; };

inline UnaryView view_unary(const Module& m, NodeId n) {
  return {static_cast<UnOp>(m.at(n).op), m.child(n, 0)};
}
inline BinaryView view_binary(const Module& m, NodeId n) {
  return {static_cast<BinOp>(m.at(n).op), m.child(n, 0), m.child(n, 1)};
}
inline AssignView view_assign(const Module& m, NodeId n) {
  return {static_cast<VarKind>(m.at(n).op), m.at(n).a, m.child(n, 0)};
}
inline VarRefView view_varref(const Module& m, NodeId n) {
  return {static_cast<VarKind>(m.at(n).op), m.at(n).a};
}
inline IfView view_if(const Module& m, NodeId n) {
  IfView v{m.child(n, 0), m.child(n, 1), NodeId{}};
  if (m.num_children(n) > 2) v.els = m.child(n, 2);
  return v;
}
// Callers only reach this once check_node has already rejected a Switch
// with no subject, so num_children(n) >= 1 always holds here.
inline SwitchView view_switch(const Module& m, NodeId n) {
  const uint32_t rest = m.num_children(n) - 1;
  // rest / 2 either way: an odd `rest` is the one with a default, and the
  // odd child it leaves over is exactly what the truncation drops.
  SwitchView v{m.child(n, 0), NodeId{}, rest / 2};
  if (rest % 2 != 0) v.default_body = m.child(n, m.num_children(n) - 1);
  return v;
}
inline NodeId switch_key(const Module& m, NodeId n, uint32_t i) {
  return m.child(n, 1 + 2 * i);
}
inline NodeId switch_body(const Module& m, NodeId n, uint32_t i) {
  return m.child(n, 2 + 2 * i);
}
inline WhileView view_while(const Module& m, NodeId n) {
  return {m.child(n, 0), m.child(n, 1)};
}
inline ClosureView view_make_closure(const Module& m, NodeId n) {
  return {m.at(n).a, m.at(n).b};
}
inline IntrinsicView view_intrinsic(const Module& m, NodeId n) {
  return {static_cast<IntrinsicId>(m.at(n).op)};
}
inline ScopeView view_scope(const Module& m, NodeId n) {
  ScopeView v{m.at(n).a, m.at(n).b, m.child(n, 0), NodeId{}};
  if (m.num_children(n) > 1) v.release_order = m.child(n, 1);
  return v;
}
inline TryView view_try(const Module& m, NodeId n) {
  return {m.at(n).a, m.child(n, 0), m.child(n, 1)};
}
inline CellFreshView view_cellfresh(const Module& m, NodeId n) {
  return {m.at(n).a};
}
inline FieldView view_field_get(const Module& m, NodeId n) {
  return {m.at(n).a, m.at(n).b, m.child(n, 0)};
}
inline FieldSetView view_field_set(const Module& m, NodeId n) {
  return {m.at(n).a, m.at(n).b, m.child(n, 0), m.child(n, 1)};
}
inline LoopJumpView view_break(const Module& m, NodeId n) {
  return {m.at(n).a};
}
inline LoopJumpView view_continue(const Module& m, NodeId n) {
  return {m.at(n).a};
}
inline NativeRefView view_native_ref(const Module& m, NodeId n) {
  return {m.at(n).a};
}

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

class Builder {
public:
  explicit Builder(Module& m) : m_(m) {}

  // Linear scan: called from every node emit() builds, so this is O(n) over
  // already-seen positions per node -- fine for PL/0's few hundred nodes, but
  // an O(module size squared) cost if this IR is ever pointed at something
  // large. A hash map keyed on (line, col) is the fix, if that day comes.
  uint32_t intern_pos(SrcPos p) {
    for (uint32_t i = 0; i < m_.positions.size(); ++i) {
      if (m_.positions[i].line == p.line && m_.positions[i].col == p.col) {
        return i;
      }
    }
    m_.positions.push_back(p);
    return static_cast<uint32_t>(m_.positions.size() - 1);
  }

  // Same tradeoff as intern_pos, once per numeric literal rather than per
  // node. Kept as its own name because callers spell integers this way; the
  // policy itself is intern_scalar's.
  int32_t intern_int(int64_t v) { return intern_scalar(ConstKind::Int, v); }

  // Same interning shape as intern_int, over the string pool.
  int32_t intern_str(const std::string& s) {
    for (uint32_t i = 0; i < m_.consts.size(); ++i) {
      if (m_.consts[i].kind == ConstKind::Str &&
          m_.str_consts[static_cast<size_t>(m_.consts[i].bits)] == s) {
        return static_cast<int32_t>(i);
      }
    }
    m_.str_consts.push_back(s);
    m_.consts.push_back(
        {ConstKind::Str, static_cast<int64_t>(m_.str_consts.size() - 1)});
    return static_cast<int32_t>(m_.consts.size() - 1);
  }

  // Interning by (kind, bits) rather than a per-kind scan: Bool and Double
  // want the same dedup Int gets, and one comparison covers all three because
  // a Double is stored as its bit pattern.
  int32_t intern_scalar(ConstKind kind, int64_t bits) {
    for (uint32_t i = 0; i < m_.consts.size(); ++i) {
      if (m_.consts[i].kind == kind && m_.consts[i].bits == bits) {
        return static_cast<int32_t>(i);
      }
    }
    m_.consts.push_back({kind, bits});
    return static_cast<int32_t>(m_.consts.size() - 1);
  }

  NodeId literal(int64_t v, SrcPos p) {
    return emit(Tag::Literal, 0, p, intern_int(v), 0, {});
  }
  NodeId bool_literal(bool v, SrcPos p) {
    return emit(Tag::Literal, 0, p, intern_scalar(ConstKind::Bool, v ? 1 : 0),
                0, {});
  }
  NodeId double_literal(double v, SrcPos p) {
    int64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(double));
    return emit(Tag::Literal, 0, p, intern_scalar(ConstKind::Double, bits), 0,
                {});
  }
  NodeId nil_literal(SrcPos p) {
    return emit(Tag::Literal, 0, p, intern_scalar(ConstKind::Nil, 0), 0, {});
  }
  NodeId str_literal(const std::string& s, SrcPos p) {
    return emit(Tag::Literal, 0, p, intern_str(s), 0, {});
  }
  NodeId varref(VarKind k, int32_t index, SrcPos p) {
    return emit(Tag::VarRef, static_cast<uint8_t>(k), p, index, 0, {});
  }
  NodeId unary(UnOp op, NodeId operand, SrcPos p) {
    return emit(Tag::Unary, static_cast<uint8_t>(op), p, 0, 0, {operand});
  }
  NodeId binary(BinOp op, NodeId lhs, NodeId rhs, SrcPos p) {
    return emit(Tag::Binary, static_cast<uint8_t>(op), p, 0, 0, {lhs, rhs});
  }
  NodeId assign(VarKind k, int32_t index, NodeId value, SrcPos p) {
    return emit(Tag::Assign, static_cast<uint8_t>(k), p, index, 0, {value});
  }
  NodeId make_if(NodeId cond, NodeId then_, NodeId els, SrcPos p) {
    if (els.valid()) return emit(Tag::If, 0, p, 0, 0, {cond, then_, els});
    return emit(Tag::If, 0, p, 0, 0, {cond, then_});
  }
  NodeId make_while(NodeId cond, NodeId body, SrcPos p) {
    return emit(Tag::While, 0, p, 0, 0, {cond, body});
  }
  // `arms` keys must be Literal nodes (see b.literal / b.str_literal), all
  // the same ConstKind and pairwise distinct -- verify() enforces both.
  // `default_body` may be an invalid NodeId for a switch with no default.
  NodeId make_switch(NodeId subject,
                     const std::vector<std::pair<NodeId, NodeId>>& arms,
                     NodeId default_body, SrcPos p) {
    std::vector<NodeId> children{subject};
    children.reserve(1 + arms.size() * 2 + (default_body.valid() ? 1 : 0));
    for (const auto& kv : arms) {
      children.push_back(kv.first);
      children.push_back(kv.second);
    }
    if (default_body.valid()) children.push_back(default_body);
    return emit(Tag::Switch, 0, p, 0, 0, children);
  }
  NodeId block(const std::vector<NodeId>& stmts, SrcPos p) {
    return emit(Tag::Block, 0, p, 0, 0, stmts);
  }
  NodeId scope(int32_t first_local, int32_t end_local, NodeId body, SrcPos p) {
    return emit(Tag::Scope, 0, p, first_local, end_local, {body});
  }
  // The same with the release order spelled out: `release` is VarRef nodes
  // (Local or Cell), released in that order at every exit.
  NodeId scope(int32_t first_local, int32_t end_local, NodeId body,
               const std::vector<NodeId>& release, SrcPos p) {
    return emit(Tag::Scope, 0, p, first_local, end_local,
                {body, block(release, p)});
  }
  NodeId make_return(NodeId value, SrcPos p) {
    if (value.valid()) return emit(Tag::Return, 0, p, 0, 0, {value});
    return emit(Tag::Return, 0, p, 0, 0, {});
  }
  // `depth`: how many enclosing loops to skip -- 0 (the default) leaves
  // the innermost, 1 the one around it, and so on.
  NodeId make_break(SrcPos p, int32_t depth = 0) {
    return emit(Tag::Break, 0, p, depth, 0, {});
  }
  NodeId make_throw(NodeId value, SrcPos p) {
    return emit(Tag::Throw, 0, p, 0, 0, {value});
  }
  NodeId make_defer(NodeId value, SrcPos p) {
    return emit(Tag::Defer, 0, p, 0, 0, {value});
  }
  NodeId cell_fresh(int32_t cell, SrcPos p) {
    return emit(Tag::CellFresh, 0, p, cell, 0, {});
  }
  NodeId make_yield(NodeId value, SrcPos p) {
    return emit(Tag::Yield, 0, p, 0, 0, {value});
  }
  NodeId make_try(int32_t caught_local, NodeId body, NodeId handler,
                  SrcPos p) {
    return emit(Tag::TryCatch, 0, p, caught_local, 0, {body, handler});
  }
  NodeId make_continue(SrcPos p, int32_t depth = 0) {
    return emit(Tag::Continue, 0, p, depth, 0, {});
  }
  NodeId intrinsic(IntrinsicId id, const std::vector<NodeId>& args, SrcPos p) {
    return emit(Tag::Intrinsic, static_cast<uint8_t>(id), p, 0, 0, args);
  }
  NodeId make_closure(int32_t func, int32_t capture_map, SrcPos p) {
    return emit(Tag::MakeClosure, 0, p, func, capture_map, {});
  }
  NodeId array_lit(const std::vector<NodeId>& items, SrcPos p) {
    return emit(Tag::ArrayLit, 0, p, 0, 0, items);
  }
  // Pairs rather than a side table of names, so a computed key costs nothing
  // extra and the verifier has one shape to check.
  NodeId object_lit(const std::vector<std::pair<NodeId, NodeId>>& kvs,
                    SrcPos p) {
    std::vector<NodeId> children;
    children.reserve(kvs.size() * 2);
    for (const auto& kv : kvs) {
      children.push_back(kv.first);
      children.push_back(kv.second);
    }
    return emit(Tag::ObjectLit, 0, p, 0, 0, children);
  }
  NodeId index(NodeId recv, NodeId key, SrcPos p) {
    return emit(Tag::Index, 0, p, 0, 0, {recv, key});
  }
  NodeId set_index(NodeId recv, NodeId key, NodeId value, SrcPos p) {
    return emit(Tag::SetIndex, 0, p, 0, 0, {recv, key, value});
  }
  // `slot` is the front end's own field numbering (props index); `name` is
  // carried along only for a trap message ("field 'next' of ..."), never
  // read to execute anything -- the same diagnostics-only role
  // Func::local_names plays for VarKind::Local.
  NodeId field_get(NodeId recv, int32_t slot, const std::string& name,
                   SrcPos p) {
    return emit(Tag::FieldGet, 0, p, slot, intern_str(name), {recv});
  }
  NodeId field_set(NodeId recv, int32_t slot, const std::string& name,
                   NodeId value, SrcPos p) {
    return emit(Tag::FieldSet, 0, p, slot, intern_str(name), {recv, value});
  }
  NodeId call_value(NodeId callee, const std::vector<NodeId>& args, SrcPos p) {
    std::vector<NodeId> children{callee};
    children.insert(children.end(), args.begin(), args.end());
    return emit(Tag::CallValue, 0, p, 0, 0, children);
  }
  // The host function Module::natives[index] names, as a callable value.
  NodeId native_ref(int32_t index, SrcPos p) {
    return emit(Tag::NativeRef, 0, p, index, 0, {});
  }
  // Declares a host function by name (or finds the declaration already
  // made) and answers its Module::natives index -- what native_ref takes.
  int32_t declare_native(const std::string& name) {
    for (size_t i = 0; i < m_.natives.size(); ++i) {
      if (m_.natives[i] == name) return static_cast<int32_t>(i);
    }
    m_.natives.push_back(name);
    return static_cast<int32_t>(m_.natives.size() - 1);
  }

private:
  NodeId emit(Tag tag, uint8_t op, SrcPos p, int32_t a, int32_t b,
              const std::vector<NodeId>& children) {
    Node n;
    n.tag = tag;
    n.op = op;
    n.pos = intern_pos(p);
    n.a = a;
    n.b = b;
    n.first_child = static_cast<uint32_t>(m_.child_ids.size());
    n.num_children = static_cast<uint32_t>(children.size());
    for (NodeId c : children) m_.child_ids.push_back(c);
    m_.nodes.push_back(n);
    return NodeId{static_cast<uint32_t>(m_.nodes.size() - 1)};
  }

  Module& m_;
};

// ---------------------------------------------------------------------------
// verify -- what makes "closed" mean something
// ---------------------------------------------------------------------------

std::optional<std::string> verify(const Module& m);

// A readable dump of the IR, for --dump-ir.
std::string to_string(const Module& m);

// Public so vm's own instruction-name table can share it instead of
// restating the same eleven strings, and so a front end resolving its own
// vocabulary (codegen.h's builders, for one) has one table to call rather
// than a copy of these strings of its own.
const char* name_of(Tag t);
const char* name_of(UnOp op);
const char* name_of(BinOp op);
const char* name_of(VarKind k);
const char* name_of(IntrinsicId id);
const char* name_of(ConstKind k);

// The inverse: a string a front end holds (a builder call's `op:`/`kind:`
// argument, say), resolved back to the enumerator name_of prints for it, or
// nullopt for anything else. Defined per enum in ir.cc, each by scanning
// name_of over that enum's own range, so the two directions can never name
// the same value differently.
template <class E> std::optional<E> from_name(std::string_view s);

}  // namespace coreir

// ===== coreir/semantics.h =====

// The value model, stated once.
//
// A value is a tagged Value (see coreir/value.h), not an i64, so every
// operation here dispatches on the operand tags and may fail because of them
// -- the shape a dynamically typed front end needs, and the shape PL/0 never
// exercised because it only ever had one type.
//
// Comparisons yield Bool; If and While test truthiness. Integer arithmetic
// wraps, and mixed integer/double arithmetic widens. PL/0's grammar happens to keep comparisons out of arithmetic
// positions, but Core-IR claims to be grammar-independent, so that accident
// is not something a backend may rely on.
//
// vm::compile and the bytecode executor both include this header directly,
// so wrapping and trap conditions have exactly one implementation between
// them. A backend that cannot call C++ (one that emits another language's
// text, say) has to restate the equivalent instead -- the one place such a
// backend's author should expect to read this file twice.

namespace coreir {

inline const char* type_name(ValueTag t) {
  switch (t) {
    case ValueTag::Uninit: return "uninitialized";
    case ValueTag::Nil:    return "nil";
    case ValueTag::Bool:   return "bool";
    case ValueTag::Int:    return "int";
    case ValueTag::Double: return "double";
    case ValueTag::Str:    return "string";
    case ValueTag::Array:  return "array";
    case ValueTag::Object: return "object";
    case ValueTag::Cell:   return "cell";
    case ValueTag::Func:   return "function";
    case ValueTag::Generator: return "generator";
    case ValueTag::Map:    return "map";
    case ValueTag::Coroutine: return "coroutine";
    // A host function answers "function" like a closure: what a program
    // can do with the two is the same (call it, ask its arity), and a
    // language's own type test should not tell them apart.
    case ValueTag::Native: return "function";
  }
  return "?";
}

// Wrapping, always: signed overflow is UB in C++, so the arithmetic goes
// through uint64_t here rather than leaving it to the optimizer's discretion.
inline int64_t wrap_add(int64_t a, int64_t b) {
  return static_cast<int64_t>(static_cast<uint64_t>(a) +
                              static_cast<uint64_t>(b));
}
inline int64_t wrap_sub(int64_t a, int64_t b) {
  return static_cast<int64_t>(static_cast<uint64_t>(a) -
                              static_cast<uint64_t>(b));
}
inline int64_t wrap_mul(int64_t a, int64_t b) {
  return static_cast<int64_t>(static_cast<uint64_t>(a) *
                              static_cast<uint64_t>(b));
}
inline int64_t wrap_neg(int64_t a) {
  return static_cast<int64_t>(0u - static_cast<uint64_t>(a));
}

// Operators that never widen to double: the classic bitops plus the
// unsigned-only group (a bit pattern's "unsigned" reading has nothing to
// widen to). No longer is_bitop -- the U-prefixed group is not a bitop, and
// int-only is the property the two groups actually share.
inline bool is_int_only(BinOp op) {
  switch (op) {
    case BinOp::BitAnd: case BinOp::BitOr: case BinOp::BitXor:
    case BinOp::Shl: case BinOp::Shr:
    case BinOp::UDiv: case BinOp::UMod: case BinOp::UShr:
    case BinOp::ULt: case BinOp::ULe: case BinOp::UGt: case BinOp::UGe:
      return true;
    default:
      return false;
  }
}

inline bool is_comparison(BinOp op) {
  switch (op) {
    case BinOp::Eq: case BinOp::Ne: case BinOp::Lt:
    case BinOp::Le: case BinOp::Gt: case BinOp::Ge:
      return true;
    default:
      return false;
  }
}

// Comparisons produce Bool, not 0/1. PL/0 cannot tell the difference -- its
// grammar only ever puts a comparison in a condition -- but a language with
// a boolean type would have to undo an integer here.
template <typename T>
inline Value compare(BinOp op, const T& a, const T& b) {
  switch (op) {
    case BinOp::Eq: return Value::make_bool(a == b);
    case BinOp::Ne: return Value::make_bool(a != b);
    case BinOp::Lt: return Value::make_bool(a < b);
    case BinOp::Le: return Value::make_bool(a <= b);
    case BinOp::Gt: return Value::make_bool(a > b);
    case BinOp::Ge: return Value::make_bool(a >= b);
    default: return Value();
  }
}

// The check and the arithmetic in one walk of the operand ladder: on
// success `out` takes the result and the answer is empty, and on failure the
// diagnostic comes back and `out` is untouched. Two failure kinds, where the
// i64 IR had one: the arithmetic traps it always had, and type errors, which
// only exist once values can disagree about what they are.
//
// One walk rather than two because this is the instruction a loop spends its
// time in, and asking what the operands are and then asking again in order to
// act on the answer was a type ladder per instruction more than the work
// needs. binop_error and apply_binop below are this same function asked for
// one half at a time, for a front end folding constants at compile time.
//
// Returning std::string rather than a const char* costs an allocation on the
// failing path only, and buys operand types in the message -- most of what
// makes a dynamic language's type error useful.
inline std::string eval_binop(BinOp op, const Value& l, const Value& r,
                              Value& out) {
  // Two ints stay integers; anything else numeric widens to double. Mod on
  // doubles is deliberately absent -- fmod versus truncation is a language's
  // decision, and no front end here needs it yet.
  if (l.is_int() && r.is_int()) {
    const int64_t a = l.as_int();
    const int64_t b = r.as_int();
    switch (op) {
      case BinOp::Add: out = Value::make_int(wrap_add(a, b)); return {};
      case BinOp::Sub: out = Value::make_int(wrap_sub(a, b)); return {};
      case BinOp::Mul: out = Value::make_int(wrap_mul(a, b)); return {};
      case BinOp::Div: case BinOp::Mod:
      case BinOp::UDiv: case BinOp::UMod: {
        // Every division form traps on a zero divisor -- one statement of
        // that, since UDiv/UMod differ from Div/Mod only in the overflow
        // case they do not have (no negative dividend for -1 to misfire on).
        if (b == 0) return "divide by zero";
        if ((op == BinOp::Div || op == BinOp::Mod) &&
            a == std::numeric_limits<int64_t>::min() && b == -1) {
          return "division overflow";
        }
        const uint64_t ua = static_cast<uint64_t>(a);
        const uint64_t ub = static_cast<uint64_t>(b);
        // Unsigned: reinterpret both operands' bits as uint64_t, do the
        // operation there, and cast the bit pattern back. Same convention as
        // wrap_add et al -- the arithmetic goes through uint64_t rather than
        // trusting int64_t's signed behavior to match.
        switch (op) {
          case BinOp::Div:  out = Value::make_int(a / b); break;
          case BinOp::Mod:  out = Value::make_int(a % b); break;
          case BinOp::UDiv: out = Value::make_int(static_cast<int64_t>(ua / ub)); break;
          default:          out = Value::make_int(static_cast<int64_t>(ua % ub)); break;
        }
        return {};
      }
      case BinOp::BitAnd: out = Value::make_int(a & b); return {};
      case BinOp::BitOr:  out = Value::make_int(a | b); return {};
      case BinOp::BitXor: out = Value::make_int(a ^ b); return {};
      // Through uint64_t like wrap_add: a left shift that overflows is
      // wrapping, not UB. Shr stays signed -- C++20 defines it arithmetic.
      case BinOp::Shl:
        out = Value::make_int(
            static_cast<int64_t>(static_cast<uint64_t>(a) << (b & 63)));
        return {};
      case BinOp::Shr: out = Value::make_int(a >> (b & 63)); return {};
      case BinOp::UShr:
        out = Value::make_int(
            static_cast<int64_t>(static_cast<uint64_t>(a) >> (b & 63)));
        return {};
      case BinOp::ULt:
        out = Value::make_bool(static_cast<uint64_t>(a) <
                               static_cast<uint64_t>(b));
        return {};
      case BinOp::ULe:
        out = Value::make_bool(static_cast<uint64_t>(a) <=
                               static_cast<uint64_t>(b));
        return {};
      case BinOp::UGt:
        out = Value::make_bool(static_cast<uint64_t>(a) >
                               static_cast<uint64_t>(b));
        return {};
      case BinOp::UGe:
        out = Value::make_bool(static_cast<uint64_t>(a) >=
                               static_cast<uint64_t>(b));
        return {};
      default: out = compare(op, a, b); return {};  // Eq .. Ge
    }
  }
  if (l.is_number() && r.is_number() && !is_int_only(op)) {
    if (op == BinOp::Mod) return "cannot mod double";
    const double a = l.as_number();
    const double b = r.as_number();
    switch (op) {
      case BinOp::Add: out = Value::make_double(a + b); return {};
      case BinOp::Sub: out = Value::make_double(a - b); return {};
      case BinOp::Mul: out = Value::make_double(a * b); return {};
      // inf/nan, not a trap
      case BinOp::Div: out = Value::make_double(a / b); return {};
      default: out = compare(op, a, b); return {};
    }
  }
  if (l.is_str() && r.is_str()) {
    // Strings concatenate and compare; they do not subtract or divide.
    if (op == BinOp::Add) {
      out = Value::make_str(l.as_str() + r.as_str());
      return {};
    }
    if (is_comparison(op)) {
      out = compare(op, l.as_str(), r.as_str());
      return {};
    }
  }
  if (l.is_bool() && r.is_bool()) {
    if (op == BinOp::Eq || op == BinOp::Ne) {
      out = compare(op, l.as_bool(), r.as_bool());
      return {};
    }
  }
  if (l.is_nil() && r.is_nil()) {
    if (op == BinOp::Eq || op == BinOp::Ne) {
      out = Value::make_bool(op == BinOp::Eq);
      return {};
    }
  }
  // Comparing across types is a question ("is 1 == '1'?", "is nil == false?")
  // a language answers, not the VM. Refusing it keeps the VM from baking in an
  // answer a front end would then have to work around.
  return std::string("cannot ") + name_of(op) + " " + type_name(l.tag()) +
         " and " + type_name(r.tag());
}

// Whether the operation is defined for these operands: empty when it is, the
// diagnostic when it is not. The result eval_binop computed on the way to
// the answer is dropped, which is what a caller asking only this wants.
inline std::string binop_error(BinOp op, const Value& l, const Value& r) {
  Value unused;
  return eval_binop(op, l, r, unused);
}

// The other half. Only valid when binop_error returned empty.
inline Value apply_binop(BinOp op, const Value& l, const Value& r) {
  Value out;
  eval_binop(op, l, r, out);
  return out;
}

inline bool is_wrap(UnOp op) {
  switch (op) {
    case UnOp::WrapI8: case UnOp::WrapI16: case UnOp::WrapI32:
    case UnOp::WrapU8: case UnOp::WrapU16: case UnOp::WrapU32:
      return true;
    default:
      return false;
  }
}

inline std::string unop_error(UnOp op, const Value& v) {
  if (op == UnOp::Neg && !v.is_number()) {
    return std::string("cannot negate ") + type_name(v.tag());
  }
  if (op == UnOp::BitNot && !v.is_int()) {
    return std::string("cannot bitwise-not ") + type_name(v.tag());
  }
  if (is_wrap(op) && !v.is_int()) {
    return std::string("cannot truncate ") + type_name(v.tag());
  }
  return {};
}

// Truncate-and-widen, stated once: the six Wrap ops differ only in the type
// whose width they land in, and C++'s own conversion from int64 down to a
// narrower type and back is exactly the promise WrapI8..WrapU32 make to a
// front end -- the same reason wrap_add et al. go through uint64_t rather
// than restating the rule per operator.
template <typename T>
inline Value wrap_to(int64_t x) {
  return Value::make_int(static_cast<int64_t>(static_cast<T>(x)));
}

inline Value apply_unop(UnOp op, const Value& v) {
  // One exhaustive switch rather than an is_wrap guard around a second one:
  // a width added to UnOp without a case here fails the build (-Wswitch),
  // not silently falls into whichever arm happened to be last.
  switch (op) {
    case UnOp::BitNot:  return Value::make_int(~v.as_int());
    case UnOp::WrapI8:  return wrap_to<int8_t>(v.as_int());
    case UnOp::WrapI16: return wrap_to<int16_t>(v.as_int());
    case UnOp::WrapI32: return wrap_to<int32_t>(v.as_int());
    case UnOp::WrapU8:  return wrap_to<uint8_t>(v.as_int());
    case UnOp::WrapU16: return wrap_to<uint16_t>(v.as_int());
    case UnOp::WrapU32: return wrap_to<uint32_t>(v.as_int());
    case UnOp::Neg:     break;
  }
  return v.is_int() ? Value::make_int(wrap_neg(v.as_int()))
                    : Value::make_double(-v.as_double());
}

// Indexing, shared by the executor's Index and SetIndex so that "what can be
// indexed by what" is answered once. An out-of-range or wrong-typed key
// fails rather than yielding nil: a language that wants nil can ask the
// length first, while one that wants the error cannot recover it from a nil.
inline std::string index_error(const Value& recv, const Value& key) {
  if (recv.is_str()) {
    if (!key.is_int()) {
      return std::string("string index must be an int, not ") +
             type_name(key.tag());
    }
    const int64_t i = key.as_int();
    const auto n = static_cast<int64_t>(recv.as_str().size());
    if (i < 0 || i >= n) {
      return "string index " + std::to_string(i) +
             " out of range for length " + std::to_string(n);
    }
    return {};
  }
  if (recv.is_array()) {
    if (!key.is_int()) {
      return std::string("array index must be an int, not ") +
             type_name(key.tag());
    }
    const int64_t i = key.as_int();
    const auto n = static_cast<int64_t>(recv.as_array()->items.size());
    if (i < 0 || i >= n) {
      return "array index " + std::to_string(i) + " out of range for length " +
             std::to_string(n);
    }
    return {};
  }
  if (recv.is_object()) {
    if (!key.is_str()) {
      return std::string("object key must be a string, not ") +
             type_name(key.tag());
    }
    return {};
  }
  // A map takes any value as a key (MapKeyRef's rule, value.h) -- what it is
  // for. Uninit is the one exception, and an unassigned local traps at its
  // read before it could get here, so nothing needs saying about it.
  if (recv.is_map()) return {};
  return std::string("cannot index ") + type_name(recv.tag());
}

// A missing property reads as nil rather than failing, unlike a missing array
// element. The asymmetry is deliberate and matches what the two are for: an
// array index out of range is almost always a bug, while asking an object
// whether it has a key is how you find out.
inline Value index_get(const Value& recv, const Value& key) {
  if (recv.is_str()) {
    // One byte, as a string. Bytes, not code points: what a code point is
    // belongs to the language's string model, and a front end with one
    // builds it over this.
    return Value::make_str(
        std::string(1, recv.as_str()[static_cast<size_t>(key.as_int())]));
  }
  if (recv.is_array()) {
    return recv.as_array()->items[static_cast<size_t>(key.as_int())];
  }
  // A missing map key reads as nil, like a missing property: the same
  // asymmetry with arrays, for the same reason.
  if (recv.is_map()) {
    const Value* v = recv.as_map()->find(key);
    return v ? *v : Value();
  }
  const Value* v = recv.as_object()->find(key.as_str());
  return v ? *v : Value();
}

inline void index_set(const Value& recv, const Value& key, const Value& v) {
  if (recv.is_array()) {
    recv.as_array()->items[static_cast<size_t>(key.as_int())] = v;
    return;
  }
  if (recv.is_map()) {
    recv.as_map()->set(key, v);
    return;
  }
  recv.as_object()->set(key.as_str(), v);
}

inline std::string len_error(const Value& v) {
  if (v.is_array() || v.is_str() || v.is_object() || v.is_map()) return {};
  return std::string("cannot take the length of ") + type_name(v.tag());
}

inline Value length_of(const Value& v) {
  if (v.is_str()) return Value::make_int(static_cast<int64_t>(v.as_str().size()));
  if (v.is_array()) {
    return Value::make_int(static_cast<int64_t>(v.as_array()->items.size()));
  }
  if (v.is_map()) return Value::make_int(static_cast<int64_t>(v.as_map()->live));
  return Value::make_int(static_cast<int64_t>(v.as_object()->props.size()));
}

// The half-open slice [i, j) of a string or an array, shared by StrSlice
// and ArraySlice so that "what bounds are legal" is answered once: both
// ends must be ints with 0 <= i <= j <= len, and anything else fails rather
// than clamping -- a language that clamps (Python) or counts from the end
// (negative indices) normalizes in its own lowering first, the same way it
// does for Index. A slice is a fresh value either way; nothing aliases.
inline std::string slice_error(const Value& recv, const Value& i,
                               const Value& j, int64_t len) {
  if (!i.is_int() || !j.is_int()) {
    return std::string("slice bounds must be ints, not ") +
           type_name(i.tag()) + " and " + type_name(j.tag());
  }
  const int64_t a = i.as_int();
  const int64_t b = j.as_int();
  if (a < 0 || b < a || b > len) {
    return "slice [" + std::to_string(a) + ", " + std::to_string(b) +
           ") out of range for " + type_name(recv.tag()) + " of length " +
           std::to_string(len);
  }
  return {};
}

// How a non-string scalar prints, when a front end has not formatted it
// itself. Shortest round-trip for a double, so 3.5 is "3.5" and not
// "3.500000", and 4.0 is "4" -- whether a whole double should show a decimal
// point is a language's decision, and a front end that cares builds the
// string rather than asking this to grow a mode.
inline std::string to_display(const Value& v) {
  switch (v.tag()) {
    case ValueTag::Nil:  return "nil";
    case ValueTag::Bool: return v.as_bool() ? "true" : "false";
    case ValueTag::Int:  return std::to_string(v.as_int());
    case ValueTag::Double: {
      char buf[32];
      auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v.as_double());
      return ec == std::errc() ? std::string(buf, end) : std::string("?");
    }
    case ValueTag::Str:  return v.as_str();
    default:             return std::string("<") + type_name(v.tag()) + ">";
  }
}

// The one formatter for "read before assigned". A future backend that cannot
// call this directly (one emitting another language's text, say) should call
// this at IR-build time to bake the finished string into what it emits,
// rather than restating the wording itself.
inline std::string format_uninit_var(const std::string& name) {
  return "uninitialized variable '" + name + "'";
}

}  // namespace coreir

// ===== vm/bytecode.h =====

// A register machine, matching culebra's choice.
//
// A stack machine was the other option; a register machine was chosen because
// it is what culebra's own bytecode is, and this library exists in part to
// rehearse a design culebra could grow into. Lowering a stack machine to SSA
// (for a future backend that wants one) means symbolically executing the
// operand stack across block boundaries, which is knowledge worth not
// acquiring if a register machine sidesteps it for free.

namespace vm {

enum class Op : uint8_t {
  LoadConst,    // a = dst, b = const index
  Neg,          // a = dst, b = src
  BitNot,       // a = dst, b = src
  // coreir::UnOp's WrapI8..WrapU32, at a fixed offset from Neg/BitNot the
  // same way Add..Ge sits at one from coreir::BinOp's -- see kUnOpOffset.
  WrapI8, WrapI16, WrapI32, WrapU8, WrapU16, WrapU32,  // a = dst, b = src
  Add, Sub, Mul, Div, Mod,   // a = dst, b = lhs, c = rhs
  Eq, Ne, Lt, Le, Gt, Ge,    // a = dst, b = lhs, c = rhs   (writes 0 or 1)
  BitAnd, BitOr, BitXor, Shl, Shr,  // a = dst, b = lhs, c = rhs
  // coreir::BinOp's u64 group, same offset scheme as Add..Ge -- see
  // kBinOpOffset.
  UDiv, UMod, UShr, ULt, ULe, UGt, UGe,  // a = dst, b = lhs, c = rhs
  // Which storage class a variable lives in is a compile-time fact -- the
  // coreir::VarKind on the VarRef the compiler is lowering -- so it is an
  // opcode here rather than an operand the executor switches on a second
  // time. Reading a local is the most frequent instruction a program has;
  // one dispatch is what it should cost.
  // Both groups sit at a fixed offset from coreir::VarKind's own Local,
  // Capture, Cell -- the same trick Add..UGe plays on coreir::BinOp, and for
  // the same reason: the correspondence is a subtraction the static_asserts
  // below pin, not a switch that could drift.
  LoadLocal, LoadCapture, LoadCell,     // a = dst, b = index
  StoreLocal, StoreCapture, StoreCell,  // a = index, b = src
  Jump,         // a = target
  JumpIfFalse,  // a = cond reg, b = target
  // Looks the subject up in Chunk::switch_tables[b] and jumps to the
  // matching arm's pc; on no match it jumps to the table's default_pc if it
  // has one, and otherwise falls through to the next instruction (a LoadNil
  // FnCompiler emits there, the same "no arm taken" nil an If without an
  // else yields). Traps when the subject's ValueTag does not match the
  // table's key kind -- except a table with no keys at all (a Switch whose
  // every arm is its default), which has no key kind to hold the subject
  // to and so never traps on it.
  Switch,       // a = subject reg, b = table index
  Out,          // a = src
  OutRaw,       // a = src   (no trailing newline)
  In,           // a = dst
  Ret,          // a = result reg, b = 1 if there is one
  Throw,        // a = value reg; unwinds to the nearest handler
  // Defers. Push registers a callable to run at the owning scope's exit;
  // Mark records the defer-stack height (and its own pc -- the unwinder
  // matches marks to regions by it); RunTo pops the innermost mark and runs
  // the defers above it, LIFO, each as a normal 0-arity call.
  DeferPush,    // a = value reg
  DeferMark,    // no operands
  DeferRunTo,   // no operands
  // A Scope's entry: records the owned stack's mark (and its own pc, the
  // region's identity for the unwinder). The scope's ClearLocals -- at its
  // own exit, or emitted for it by a Break / Return -- pops the mark and
  // resolves the entries above it (Runtime::owned_scope_exit).
  OwnedMark,    // no operands
  // First-class functions.
  MakeClosure,  // a = dst, b = func index, c = capture map index
  CallValue,    // a = dst, b = callee reg, c = first arg reg, d = arg count
  // CallValue in tail position of a Func::tail_calls function: the same
  // operands, and the same call when the callee is not a plain closure or
  // this is the entry frame; otherwise the frame exits (locals, then each
  // open scope's owned resolution, innermost first) and the callee's
  // activation takes its place -- same ret_reg, same slot in the stack --
  // rather than being pushed on top of it. The instructions after it (the
  // exits and Ret the compiler emits regardless) run only on the call path.
  TailCall,     // a = dst, b = callee reg, c = first arg reg, d = arg count
  CellNew,      // a = cell index   (fresh box, holding nil)
  LoadNil,      // a = dst
  Move,         // a = dst, b = src
  // Drops registers [a, b) -- the temporaries a statement finished with.
  // Without it a dead temporary keeps whatever it holds alive until its
  // register is reused or the frame returns, which for a value with a
  // release worth timing is too late.
  ClearRegs,    // a = first, b = one past last
  // The same, for a scope's local slots as it exits. Back to Uninit, not
  // nil: the slots are dead until something declares into them again, and a
  // read before that is the same mistake as a read before first assignment.
  ClearLocals,  // a = first, b = one past last
  // The same for a scope that spelled its release order out
  // (Chunk::release_lists[a]): each slot in list order, a local back to
  // Uninit, a cell replaced by a fresh one.
  ReleaseSlots, // a = release list index
  NewArray,     // a = dst, b = first item reg, c = item count
  Index,        // a = dst, b = receiver reg, c = key reg
  SetIndex,     // a = receiver reg, b = key reg, c = value reg
  FieldGet,     // a = dst, b = receiver reg, c = slot, d = name const
  FieldSet,     // a = receiver reg, b = slot, c = value reg, d = name const
  Len,          // a = dst, b = src
  ToStr,        // a = dst, b = src   (to_display's formatting)
  ArrayPush,    // a = array reg, b = value reg
  ArrayPop,     // a = dst, b = array reg
  ObjectHas,    // a = dst, b = object reg, c = key reg
  ObjectKeys,   // a = dst, b = object reg
  ObjectRemove, // a = object reg, b = key reg
  ArgCount,     // a = dst   (the frame's supplied argument count)
  Same,         // a = dst, b = lhs, c = rhs   (reference identity)
  FnArity,      // a = dst, b = src   (num_params, or a native's arity)
  Collect,      // a = dst   (runs Runtime::collect; the count it freed)
  HeapStats,    // a = dst   (fresh {live_objects, heap_bytes} object)
  TypeOf,       // a = dst, b = src   (type_name's vocabulary, as a string)
  ToInt,        // a = dst, b = src   (truncate toward zero; traps off-range)
  ToDouble,     // a = dst, b = src
  FMod,         // a = dst, b = lhs, c = rhs   (IEEE fmod; 0 divisor traps)
  Pow,          // a = dst, b = lhs, c = rhs   (std::pow over doubles)
  NewObject,    // a = dst   (empty; ObjectLit fills it with SetIndex)
  // Generators. Yield suspends the frame back into its GeneratorObj and
  // delivers {value, done: false} to whoever resumed it; the sent value of
  // the next resume lands in `a`. GenResume, GenReturn and GenThrow are
  // ir.h's intrinsics of the same names.
  Yield,        // a = dst (sent value on re-entry), b = value reg
  GenResume,    // a = dst, b = generator reg, c = sent reg
  GenReturn,    // a = dst, b = generator reg, c = value reg
  GenThrow,     // a = dst, b = generator reg, c = value reg
  // The job queue (ir.h's Enqueue): the closure -- or the coroutine, which
  // is what makes the queue a scheduler -- joins the run's FIFO.
  Enqueue,      // a = closure or coroutine reg
  ToFloat32,    // a = dst, b = src   (round to float precision, as a double)
  // Slices spend the fourth operand on their second bound, the way
  // CallValue spends it on its argument count.
  StrSlice,     // a = dst, b = str reg, c = from reg, d = to reg
  ArraySlice,   // a = dst, b = array reg, c = from reg, d = to reg
  StrByte,      // a = dst, b = str reg, c = index reg
  StrFromByte,  // a = dst, b = src
  NewMap,       // a = dst   (empty; SetIndex fills it)
  NativeRef,    // a = dst, b = index into Program::natives
  // Coroutines (ir.h's intrinsics of the same names). CoroYield parks
  // every frame from the innermost coroutine's bottom frame up to this
  // one into its CoroObj and delivers {value, done: false} to the
  // resumer; the next CoroResume's sent value lands in `a`.
  CoroCreate,   // a = dst, b = fn reg
  CoroResume,   // a = dst, b = coroutine reg, c = sent reg
  CoroYield,    // a = dst (sent value on re-entry), b = value reg
  CoroClose,    // a = coroutine reg
  CoroStatus,   // a = dst, b = coroutine reg
  CoroCurrent,  // a = dst
};

// vm::Op's Add..UGe deliberately sit at a fixed offset from coreir::BinOp's
// own Add..UGe, so the compiler and the executor can share one arithmetic
// conversion instead of each hand-writing a switch that could silently drift
// out of step with the other.
inline constexpr int32_t kBinOpOffset =
    static_cast<int32_t>(Op::Add) - static_cast<int32_t>(coreir::BinOp::Add);

inline constexpr Op op_of(coreir::BinOp op) {
  return static_cast<Op>(static_cast<int32_t>(op) + kBinOpOffset);
}
inline constexpr coreir::BinOp binop_of(Op op) {
  return static_cast<coreir::BinOp>(static_cast<int32_t>(op) - kBinOpOffset);
}
static_assert(op_of(coreir::BinOp::Add) == Op::Add);
static_assert(op_of(coreir::BinOp::Ge) == Op::Ge);
static_assert(op_of(coreir::BinOp::Shr) == Op::Shr);
static_assert(op_of(coreir::BinOp::UGe) == Op::UGe);

// The same trick for coreir::UnOp's Neg..WrapU32 against vm::Op's own
// Neg..WrapU32 -- the pair Exec used to tell apart with a two-way ternary
// before WrapI8..WrapU32 existed to make that stop being true.
inline constexpr int32_t kUnOpOffset =
    static_cast<int32_t>(Op::Neg) - static_cast<int32_t>(coreir::UnOp::Neg);

inline constexpr Op op_of(coreir::UnOp op) {
  return static_cast<Op>(static_cast<int32_t>(op) + kUnOpOffset);
}
inline constexpr coreir::UnOp unop_of(Op op) {
  return static_cast<coreir::UnOp>(static_cast<int32_t>(op) - kUnOpOffset);
}
static_assert(op_of(coreir::UnOp::Neg) == Op::Neg);
static_assert(op_of(coreir::UnOp::BitNot) == Op::BitNot);
static_assert(op_of(coreir::UnOp::WrapU32) == Op::WrapU32);

// A variable's storage class, as the opcode that reads it and the one that
// writes it: the choice is made once, at compile time, which is the whole
// point of there being six opcodes rather than two operands the executor
// would switch on again.
inline constexpr int32_t kVarLoadOffset =
    static_cast<int32_t>(Op::LoadLocal) -
    static_cast<int32_t>(coreir::VarKind::Local);
inline constexpr int32_t kVarStoreOffset =
    static_cast<int32_t>(Op::StoreLocal) -
    static_cast<int32_t>(coreir::VarKind::Local);

inline constexpr Op load_op_of(coreir::VarKind k) {
  return static_cast<Op>(static_cast<int32_t>(k) + kVarLoadOffset);
}
inline constexpr Op store_op_of(coreir::VarKind k) {
  return static_cast<Op>(static_cast<int32_t>(k) + kVarStoreOffset);
}
static_assert(load_op_of(coreir::VarKind::Local) == Op::LoadLocal);
static_assert(load_op_of(coreir::VarKind::Capture) == Op::LoadCapture);
static_assert(load_op_of(coreir::VarKind::Cell) == Op::LoadCell);
static_assert(store_op_of(coreir::VarKind::Local) == Op::StoreLocal);
static_assert(store_op_of(coreir::VarKind::Capture) == Op::StoreCapture);
static_assert(store_op_of(coreir::VarKind::Cell) == Op::StoreCell);

// The intrinsics have no such offset -- their opcodes were not laid out to
// have one -- so the correspondence is a table. It is still one table: with
// this and coreir::intrinsic_arity, lowering an intrinsic needs no knowledge
// of which intrinsic it is, and a new one is a case here rather than a new
// arm in the compiler. The switch is exhaustive on purpose: -Wswitch reports
// an id added to coreir without an opcode.
inline constexpr Op op_of(coreir::IntrinsicId id) {
  using I = coreir::IntrinsicId;
  switch (id) {
    case I::Print:        return Op::Out;
    case I::PrintRaw:     return Op::OutRaw;
    case I::ReadInt:      return Op::In;
    case I::Len:          return Op::Len;
    case I::ToStr:        return Op::ToStr;
    case I::TypeOf:       return Op::TypeOf;
    case I::ToInt:        return Op::ToInt;
    case I::ToDouble:     return Op::ToDouble;
    case I::FMod:         return Op::FMod;
    case I::Pow:          return Op::Pow;
    case I::ArrayPush:    return Op::ArrayPush;
    case I::ArrayPop:     return Op::ArrayPop;
    case I::ObjectHas:    return Op::ObjectHas;
    case I::ObjectKeys:   return Op::ObjectKeys;
    case I::ObjectRemove: return Op::ObjectRemove;
    case I::ArgCount:     return Op::ArgCount;
    case I::Same:         return Op::Same;
    case I::FnArity:      return Op::FnArity;
    case I::Collect:      return Op::Collect;
    case I::HeapStats:    return Op::HeapStats;
    case I::GenResume:    return Op::GenResume;
    case I::GenReturn:    return Op::GenReturn;
    case I::GenThrow:     return Op::GenThrow;
    case I::Enqueue:      return Op::Enqueue;
    case I::ToFloat32:    return Op::ToFloat32;
    case I::StrSlice:     return Op::StrSlice;
    case I::ArraySlice:   return Op::ArraySlice;
    case I::StrByte:      return Op::StrByte;
    case I::StrFromByte:  return Op::StrFromByte;
    case I::MapNew:       return Op::NewMap;
    case I::CoroCreate:   return Op::CoroCreate;
    case I::CoroResume:   return Op::CoroResume;
    case I::CoroYield:    return Op::CoroYield;
    case I::CoroClose:    return Op::CoroClose;
    case I::CoroStatus:   return Op::CoroStatus;
    case I::CoroCurrent:  return Op::CoroCurrent;
  }
  return Op::LoadNil;
}

// Whether the opcode writes a destination register. The statement-shaped
// intrinsics take their operands in a and b and produce nothing; in value
// position the compiler follows them with a LoadNil.
inline constexpr bool intrinsic_has_dst(coreir::IntrinsicId id) {
  using I = coreir::IntrinsicId;
  switch (id) {
    case I::Print:
    case I::PrintRaw:
    case I::ArrayPush:
    case I::ObjectRemove:
    case I::Enqueue:
    case I::CoroClose:
      return false;
    default:
      return true;
  }
}

struct Insn {
  Op op;
  int32_t a = 0;
  int32_t b = 0;
  int32_t c = 0;
  int32_t d = 0;  // CallValue's arg count, FieldGet/FieldSet's name const,
                  // a slice's second bound; zero for everything else
};

// One record per lexical region the compiler closed. Children close before
// their parent, so scanning the vector in order visits the regions holding a
// given pc innermost first -- the order an unwinding walk wants -- with no
// parent links to maintain.
//
// Exec::unwind is the reader this shape is for: it walks the vector in
// order, and every field here answers something it has to ask.
struct Cleanup {
  int32_t start_pc = 0;      // half-open instruction range of the region,
  int32_t end_pc = 0;        // including its own exit-time ClearLocals
  int32_t first_local = 0;   // the local slots the region owns
  int32_t end_local = 0;
  int32_t regs_base = 0;     // registers >= this are the region's temps
  int32_t handler_pc = -1;   // >= 0: a try region; where its handler starts
  int32_t caught_local = -1; // the local slot a caught value lands in
  // >= 0: the region declares defers, and this is its DeferMark's pc. The
  // unwinder runs the region's pending defers only while the mark is still
  // outstanding (top of the frame's mark stack carries the same pc), which
  // is what keeps a throw out of the region's own exit-time defer run from
  // running them twice.
  int32_t defer_mark_pc = -1;
  // >= 0 for a Scope region: its OwnedMark's pc, matched against the top of
  // the frame's mark stack the same way, so a region whose exit already
  // resolved (a Break's ClearLocals, before a later exit-time throw) is not
  // resolved twice.
  int32_t owned_mark_pc = -1;
  // >= 0: the region releases Chunk::release_lists[release_list] instead
  // of its local range.
  int32_t release_list = -1;
};

// One entry of a scope's release order: a local slot or a cell.
struct SlotRef {
  coreir::VarKind kind;
  int32_t index;
};

// One Switch's key table (Chunk::switch_tables[Op::Switch.b]), built once at
// compile time from the coreir::Switch node's Literal keys. Dense int keys
// (a run short enough that a base-offset array does not waste much space)
// get `pcs`, so a match costs one subtraction, one bounds check and one
// array read; anything else -- sparse int keys, or Str keys, which have no
// useful notion of "dense" -- gets a sorted table Exec binary-searches.
// Exactly one of the two forms is populated, per `dense` -- except a Switch
// with no keys at all (every arm its own default: `switch (x) { default:
// ... }`), whose table populates neither and whose key_kind is therefore
// not a promise about anything; has_keys() is how Exec tells the two cases
// apart, so a keyless table's default Int key_kind never gets read as one.
struct SwitchTable {
  coreir::ConstKind key_kind = coreir::ConstKind::Int;  // Int or Str
  bool dense = false;
  int64_t base = 0;              // dense form: pcs[subject - base]
  std::vector<int32_t> pcs;      // dense form; -1 = no arm at that offset
  std::vector<int64_t> int_keys; // sparse Int form, sorted ascending
  std::vector<std::string> str_keys;  // Str form, sorted ascending
  std::vector<int32_t> arm_pcs;  // sparse form, parallel to int_keys/str_keys
  // -1: no default arm -- Op::Switch falls through to the FnCompiler-emitted
  // LoadNil rather than jumping here.
  int32_t default_pc = -1;

  bool has_keys() const {
    return dense || !int_keys.empty() || !str_keys.empty();
  }
};

// The sparse form's lookup: the arm pc for `key`, or -1 if no arm has it.
// One template rather than a copy for int_keys and a copy for str_keys --
// they differ only in what a key is, and two hand-written binary searches
// are two chances for the found/not-found edge to drift out of step with
// each other.
template <class T>
inline int32_t sparse_arm_pc(const std::vector<T>& keys,
                             const std::vector<int32_t>& arm_pcs,
                             const T& key) {
  const auto it = std::lower_bound(keys.begin(), keys.end(), key);
  if (it == keys.end() || !(*it == key)) return -1;
  return arm_pcs[static_cast<size_t>(it - keys.begin())];
}

struct Chunk {
  std::string name;
  std::vector<Insn> code;
  // Parallel to code: index into Program::positions. A side table, the way
  // culebra holds PosEntry, so error positions are structural rather than
  // hand-threaded through every instruction.
  std::vector<uint32_t> code_pos;
  int32_t num_locals = 0;
  int32_t num_captures = 0;
  int32_t num_regs = 0;
  int32_t num_cells = 0;
  int32_t num_params = 0;
  bool is_generator = false;
  bool lenient_arity = false;
  bool tail_calls = false;
  std::vector<std::string> local_names;
  std::vector<std::string> capture_names;
  std::vector<Cleanup> cleanups;
  std::vector<std::vector<SlotRef>> release_lists;
  std::vector<SwitchTable> switch_tables;
};

struct Program {
  std::vector<Chunk> chunks;  // chunks[0] is the entry point
  std::vector<coreir::Const> consts;
  std::vector<std::string> str_consts;  // bytes for ConstKind::Str
  std::vector<coreir::SrcPos> positions;
  std::vector<std::vector<coreir::CaptureSrc>> capture_maps;
  std::vector<std::string> natives;  // Module::natives, for Op::NativeRef

  // The same decode coreir::Module::str_const_at does, from this program's
  // own pools -- what FieldGet/FieldSet's diagnostic-only name const needs
  // on the trap path, and the only place a bytecode operand names a string.
  const std::string& str_const_at(int32_t const_index) const {
    return str_consts[static_cast<size_t>(
        consts[static_cast<size_t>(const_index)].bits)];
  }
};

std::string to_string(const Program& p);

}  // namespace vm

// ===== vm/compiler.h =====

namespace vm {

Program compile(const coreir::Module& m);

}  // namespace vm

// ===== vm/exec.h =====

namespace coreir {

// What a host function is handed when a program calls it (value.h's
// NativeFn). The function reads its arguments here, writes its answer to
// `result` and returns true -- or writes what the call should throw to
// `error` and returns false, which raises it at the call site exactly as a
// Throw there would, catchable by the program's own TryCatch. That is the
// one way a native fails a program: a C++ exception thrown out of the
// function is not the program's to catch (it passes through the executor
// untouched, the rule coreir_rt_* hooks already have) and ends the run.
struct NativeCall {
  const Value* args = nullptr;
  int32_t argc = 0;
  void* ctx = nullptr;   // NativeDef::ctx, verbatim
  SrcPos pos{};          // the call site
  Value result;
  Value error;

  // The i-th argument, or nil past the end -- so a variadic native need
  // not bounds-check each read.
  const Value& arg(int32_t i) const {
    static const Value kNil;
    return i < argc ? args[i] : kNil;
  }

  // Calls back into the program: runs `callee` (a closure, or another
  // native) with `argv` to completion and answers its return value. A
  // throw the callee does not catch propagates out of this call as the
  // executor's own C++ exception, through the native's frame, to the
  // nearest handler in the *calling* program -- so a native that calls
  // back keeps what it owns in RAII handles (a Value is one) rather than
  // in raw pointers it would have to free on that path.
  Value call(const Value& callee, const Value* argv, int32_t argc);

  // A fresh {message, line, col} object at this call's position: the
  // value the executor's own traps carry, for a native that wants to fail
  // the way a divide by zero does (`call.error = call.trap("...")`).
  Value trap(const std::string& msg) const;

  void* exec = nullptr;  // vm::detail::Exec*, set by the executor
};

}  // namespace coreir

namespace vm {

// One host function, as the run registers it: the name a module's
// Tag::NativeRef resolves by, the argument count a call is checked against
// (-1: any), the function and the context pointer it is handed.
struct NativeDef {
  std::string name;
  int32_t arity = -1;
  coreir::NativeFn fn = nullptr;
  void* ctx = nullptr;
};

struct RunOptions {
  // Calls do not recurse through this thread's C++ stack -- the executor
  // keeps its own stack of heap-allocated frames -- so the bound is on how
  // many of those may be live at once, not on how much machine stack they
  // would need. A runaway program still fails cleanly, at its call site,
  // rather than growing the heap until the allocator gives out.
  int max_call_depth = 10000;
  // Whether the entry frame's own values run their drop hooks when the
  // program ends -- at its Ret, or as an uncaught throw unwinds past it.
  // Off, they are released with the hook disarmed: the memory goes, the
  // destructors do not run. That is culebra's rule for top-level bindings
  // (only top-level defers run at exit), and a front end wanting both under
  // this option gives the entry frame a Scope over an empty local range,
  // [0, 0): its defers still run at its exit, while the bindings stay the
  // frame's to release, destructor-free.
  bool entry_frame_drops = true;
  // The host functions this run supplies. Every name the program declares
  // (Program::natives) must be here, or the run fails before its first
  // instruction; names the program does not declare are simply unused.
  std::vector<NativeDef> natives;
};

// Run the bytecode, on a heap the caller owns. Use this to inspect what a
// program left behind: a Runtime frees its remaining objects when it is
// destroyed -- which is how a reference cycle finally goes away -- so a
// count taken after vm::run and before ~Runtime is the one that shows a
// leak.
void run(const Program& p, coreir::Runtime& rt, const RunOptions& opts);

// The same with default options but for the depth bound, and on a heap of
// the run's own.
void run(const Program& p, coreir::Runtime& rt, int max_call_depth = 10000);
void run(const Program& p, int max_call_depth = 10000);

}  // namespace vm

// ===== coreir/ir.cc =====

namespace coreir {

// coreir/value.h's out-of-line members.

inline void Runtime::link(HeapObj* o) {
  o->prev = nullptr;
  o->next = head_;
  if (head_) head_->prev = o;
  head_ = o;
  ++live_;
}

inline void Runtime::unlink(HeapObj* o) {
  if (o->prev) o->prev->next = o->next;
  else head_ = o->next;
  if (o->next) o->next->prev = o->prev;
  o->prev = o->next = nullptr;
  --live_;
  // Every path that frees an object comes through here, which makes it the
  // one place an owned-stack entry is tombstoned.
  if (o->kind == ValueTag::Object) owned_unregister(static_cast<ObjectObj*>(o));
}

// A Runtime outliving its objects is the normal case -- everything a program
// allocated and released is already gone. What can remain is a reference
// cycle, which counting cannot collect; those are freed here, so the heap
// really does end empty and a leak checker does not report as a bug the one
// thing this design openly cannot do on its own.
//
// Freeing a cycle cannot be a single pass. Destroying one member releases its
// references, which frees the next member, whose destructor releases the
// reference back to the first -- which is mid-destruction. So: pin everything
// first, drop every outgoing reference while nothing can be freed, and only
// then free the shells, which by that point refer to nothing. culebra's
// collector separates the same two steps for the same reason (memory.md's
// finalize-then-sweep), and this is where a tracing collector would hook in.
inline Runtime::Runtime() {
  const char* s = std::getenv("COREIR_GC_STRESS");
  stress_ = s && *s && *s != '0';
}

namespace detail {

// One place that knows each kind's outgoing references, shared by the edge
// count and the mark. New heap kinds add their arm here in the same commit
// that adds the type.
template <typename Visit>
void visit_gen_frame(const GenFrame& gf, Visit& visit) {
  for (const Value& v : gf.locals) visit(v);
  for (const Value& v : gf.regs) visit(v);
  for (const Value& v : gf.cells) visit(v);
  for (const Value& v : gf.captures) visit(v);
  for (const Value& v : gf.defers) visit(v);
  visit(gf.gen_self);
}

template <typename Fn>
void visit_children(HeapObj* o, Fn fn) {
  auto visit = [&](const Value& v) {
    if (v.is_heap()) fn(reinterpret_cast<HeapObj*>(v.raw_data()));
  };
  switch (o->kind) {
    case ValueTag::Array:
      for (const Value& v : static_cast<ArrayObj*>(o)->items) visit(v);
      break;
    case ValueTag::Object:
      for (const auto& kv : static_cast<ObjectObj*>(o)->props) {
        visit(kv.second);
      }
      break;
    case ValueTag::Cell:
      visit(static_cast<CellObj*>(o)->v);
      break;
    case ValueTag::Func:
      for (const Value& v : static_cast<ClosureObj*>(o)->cells) visit(v);
      break;
    case ValueTag::Generator:
      visit_gen_frame(static_cast<GeneratorObj*>(o)->frame, visit);
      break;
    case ValueTag::Map:
      // Keys and values both; the index refers through the keys the
      // entries own and holds no reference of its own (MapKeyRef).
      for (const auto& kv : static_cast<MapObj*>(o)->entries) {
        visit(kv.first);
        visit(kv.second);
      }
      break;
    case ValueTag::Coroutine: {
      auto* co = static_cast<CoroObj*>(o);
      visit(co->fn);
      for (const GenFrame& gf : co->frames) visit_gen_frame(gf, visit);
      break;
    }
    default:
      break;  // a string, or a native, refers to nothing
  }
}

// The owned stack's trial deletion, over the subgraph the candidates reach:
// every reference occurrence found inside it is explained, a node with
// references beyond that is held from outside (a frame, an outer scope's
// binding), and so is everything it reaches. Answers, per candidate,
// whether nothing outside holds it. Each candidate carries one pin,
// credited as the seed's own occurrence. Nothing runs during the walk, so
// the counts it reads hold. A worklist, not recursion, like mark().
inline constexpr size_t kOwnedNodeBudget = 4096;

inline std::vector<char> owned_unreachable(const std::vector<ObjectObj*>& cand) {
  struct Node {
    HeapObj* obj;
    int64_t explained;
    std::vector<size_t> out;
  };
  constexpr size_t npos = static_cast<size_t>(-1);
  std::vector<Node> nodes;
  std::unordered_map<HeapObj*, size_t> index;
  std::vector<size_t> work;
  bool overflow = false;
  auto occurrence = [&](HeapObj* h, size_t from) {
    auto [it, fresh] = index.try_emplace(h, nodes.size());
    if (fresh) {
      nodes.push_back({h, 0, {}});
      work.push_back(it->second);
      if (nodes.size() > kOwnedNodeBudget) overflow = true;
    }
    ++nodes[it->second].explained;
    if (from != npos) nodes[from].out.push_back(it->second);
  };
  for (ObjectObj* c : cand) occurrence(c, npos);
  while (!work.empty() && !overflow) {
    const size_t id = work.back();
    work.pop_back();
    visit_children(nodes[id].obj, [&](HeapObj* c) { occurrence(c, id); });
  }

  std::vector<char> held(nodes.size(), 0);
  std::vector<size_t> q;
  for (size_t i = 0; i < nodes.size(); ++i) {
    if (overflow || nodes[i].obj->rc > nodes[i].explained) {
      held[i] = 1;
      q.push_back(i);
    }
  }
  while (!q.empty()) {
    const size_t i = q.back();
    q.pop_back();
    for (size_t t : nodes[i].out) {
      if (!held[t]) {
        held[t] = 1;
        q.push_back(t);
      }
    }
  }
  std::vector<char> gone(cand.size());
  for (size_t i = 0; i < cand.size(); ++i) gone[i] = !held[index[cand[i]]];
  return gone;
}

}  // namespace detail

inline bool Runtime::run_drop(HeapObj* h) {
  if (h->kind != ValueTag::Object || !drop_) return false;
  auto* o = static_cast<ObjectObj*>(h);
  if (o->dropped || !o->find(kDropKey)) return false;
  o->dropped = true;  // before the call: re-entrant, and at most once
  drop_(drop_ctx_, h);
  return true;
}

inline void Runtime::owned_register(ObjectObj* o) {
  if (o->owned_idx >= 0) return;
  o->owned_idx = static_cast<int64_t>(owned_.size());
  owned_.push_back({o, owned_next_id_++});
  // Tombstones pile up under a scope that never exits (the entry frame's);
  // prune now and then, keeping every back-pointer right.
  if ((owned_next_id_ & 1023) == 0) {
    std::erase_if(owned_, [](const OwnedEntry& e) { return !e.obj; });
    for (size_t i = 0; i < owned_.size(); ++i) {
      owned_[i].obj->owned_idx = static_cast<int64_t>(i);
    }
  }
}

inline void Runtime::owned_unregister(ObjectObj* o) {
  if (o->owned_idx < 0) return;
  owned_[static_cast<size_t>(o->owned_idx)].obj = nullptr;
  o->owned_idx = -1;
}

inline void Runtime::owned_scope_exit(uint64_t mark) {
  // The common case: nothing was bound under this scope.
  if (owned_.empty() || owned_.back().id < mark) return;

  // The candidates, newest first: the entries above the mark, less the
  // tombstones and the already dropped. Pinned, since a destructor below
  // may drop the last edge between two of them, and neither may go before
  // the sweep at the end decides.
  std::vector<OwnedEntry> cand;
  while (!owned_.empty() && owned_.back().id >= mark) {
    const OwnedEntry e = owned_.back();
    owned_.pop_back();
    if (!e.obj) continue;
    e.obj->owned_idx = -1;
    if (e.obj->dropped) continue;
    ++e.obj->rc;
    cand.push_back(e);
  }
  if (cand.empty()) return;

  std::vector<ObjectObj*> objs;
  objs.reserve(cand.size());
  for (const OwnedEntry& e : cand) objs.push_back(e.obj);
  const std::vector<char> gone = detail::owned_unreachable(objs);

  // Survivors go back first, oldest first and with their ids, so the
  // outer scope whose mark they are above resolves them -- and before any
  // destructor runs, since one may bind new entries, which must sit above.
  for (size_t i = cand.size(); i-- > 0;) {
    if (gone[i]) continue;
    cand[i].obj->owned_idx = static_cast<int64_t>(owned_.size());
    owned_.push_back(cand[i]);
  }

  // Destructors, newest first, each over a still-whole cycle.
  std::vector<ObjectObj*> dropped;
  for (size_t i = 0; i < cand.size(); ++i) {
    if (!gone[i]) continue;
    run_drop(cand[i].obj);
    dropped.push_back(cand[i].obj);
  }

  // A destructor may have stored one of them somewhere reachable. Decide
  // again, and strip only what is still nobody's: with its references gone
  // the cycle is broken, and the pins coming off free it -- the plain
  // members with it. What was resurrected stays intact, dropped already.
  if (!dropped.empty()) {
    const std::vector<char> still = detail::owned_unreachable(dropped);
    for (size_t i = 0; i < dropped.size(); ++i) {
      if (still[i]) clear_heap_object_refs(dropped[i]);
    }
  }
  for (const OwnedEntry& e : cand) {
    if (--e.obj->rc == 0) heap_release_to_zero(e.obj);
  }
}

// Marks everything reachable from an externally-held object. An object is
// held externally when its refcount exceeds the references other heap
// objects hold on it -- less the collection's own pin, on an object it has
// condemned. A worklist, not recursion: a deep structure must not become a
// C++ stack problem.
inline void Runtime::mark() {
  for (HeapObj* o = head_; o; o = o->next) {
    o->gc_refs = 0;
    o->gc_marked = false;
  }
  for (HeapObj* o = head_; o; o = o->next) {
    detail::visit_children(o, [](HeapObj* c) { ++c->gc_refs; });
  }
  std::vector<HeapObj*> work;
  for (HeapObj* o = head_; o; o = o->next) {
    if (o->rc - (o->gc_condemned ? 1 : 0) > o->gc_refs) {
      o->gc_marked = true;
      work.push_back(o);
    }
  }
  while (!work.empty()) {
    HeapObj* o = work.back();
    work.pop_back();
    detail::visit_children(o, [&](HeapObj* c) {
      if (!c->gc_marked) {
        c->gc_marked = true;
        work.push_back(c);
      }
    });
  }
}

inline int64_t Runtime::collect() {
  if (in_collect_) return 0;
  in_collect_ = true;

  // Condemn what nothing reaches, and pin it: a destructor about to run
  // may drop the last reference between two condemned objects, and that
  // must not free one of them out from under the sweep.
  mark();
  std::vector<HeapObj*> dead;
  for (HeapObj* o = head_; o; o = o->next) {
    if (!o->gc_marked) {
      o->gc_condemned = true;
      ++o->rc;
      dead.push_back(o);
    }
  }

  // Destructors, while every condemned object is still whole: newest first
  // (the heap list's order), the way a scope releases its locals in reverse.
  // One may store a condemned object somewhere reachable -- so if any ran,
  // reachability is recomputed and what it resurrected is unpinned and
  // spared, dropped already. Allocation here is ordinary: a nested collect
  // is refused by in_collect_, and an object dying by refcount gets its
  // own destructor.
  bool ran = false;
  for (HeapObj* o : dead) {
    if (run_drop(o)) ran = true;
  }
  if (ran) {
    mark();
    std::vector<HeapObj*> still;
    for (HeapObj* o : dead) {
      if (o->gc_marked) {
        o->gc_condemned = false;
        --o->rc;  // the pin; a reachable object holds at least one more
      } else {
        still.push_back(o);
      }
    }
    dead.swap(still);
  }

  // The host's last look (while every member is still whole), then
  // ~Runtime's strip / free on just this set.
  if (finalize_) {
    for (HeapObj* o : dead) finalize_(finalize_ctx_, o);
  }
  sweeping_ = true;
  for (HeapObj* o : dead) clear_heap_object_refs(o);
  for (HeapObj* o : dead) {
    unlink(o);
    o->owner = nullptr;
    destroy_heap_object(o);
  }
  sweeping_ = false;

  next_gc_ = live_ < 2048 ? 4096 : live_ * 2;
  in_collect_ = false;
  return static_cast<int64_t>(dead.size());
}

// A walk rather than a counter kept on the allocation path: nothing here
// makes an allocation or a container growth pay for a number only a
// diagnostic asks for. Per object, its own struct plus what its containers
// have reserved -- capacity, not size, since that is what is held.
inline int64_t Runtime::heap_bytes() const {
  int64_t bytes = 0;
  auto vec = [](const auto& v) {
    return static_cast<int64_t>(v.capacity() * sizeof(v[0]));
  };
  auto gen_frame_bytes = [&vec](const GenFrame& gf) {
    return vec(gf.locals) + vec(gf.regs) + vec(gf.cells) + vec(gf.captures) +
           vec(gf.defers) + vec(gf.defer_marks) + vec(gf.owned_marks);
  };
  for (HeapObj* o = head_; o; o = o->next) {
    switch (o->kind) {
      case ValueTag::Str:
        bytes += sizeof(StrObj) + vec(static_cast<StrObj*>(o)->s);
        break;
      case ValueTag::Array:
        bytes += sizeof(ArrayObj) + vec(static_cast<ArrayObj*>(o)->items);
        break;
      case ValueTag::Object: {
        auto* obj = static_cast<ObjectObj*>(o);
        bytes += sizeof(ObjectObj) + vec(obj->props);
        for (const auto& kv : obj->props) bytes += vec(kv.first);
        break;
      }
      case ValueTag::Cell:
        bytes += sizeof(CellObj);
        break;
      case ValueTag::Func:
        bytes += sizeof(ClosureObj) + vec(static_cast<ClosureObj*>(o)->cells);
        break;
      case ValueTag::Generator:
        bytes += sizeof(GeneratorObj) +
                 gen_frame_bytes(static_cast<GeneratorObj*>(o)->frame);
        break;
      case ValueTag::Map: {
        auto* mp = static_cast<MapObj*>(o);
        bytes += sizeof(MapObj) + vec(mp->entries) +
                 static_cast<int64_t>(mp->index.bucket_count() *
                                      sizeof(void*));
        break;
      }
      case ValueTag::Coroutine: {
        auto* co = static_cast<CoroObj*>(o);
        bytes += sizeof(CoroObj) + vec(co->frames);
        for (const GenFrame& gf : co->frames) bytes += gen_frame_bytes(gf);
        break;
      }
      case ValueTag::Native:
        bytes += sizeof(NativeObj) + vec(static_cast<NativeObj*>(o)->name);
        break;
      default:
        break;
    }
  }
  return bytes;
}

// The refcount just hit zero. An Object carrying the drop contract's key
// runs its destructor first (unless it already has), pinned so the closure
// can read the fields; a destructor that stores the object somewhere
// resurrects it and the free is skipped. Everything else -- and every
// non-Object -- frees directly.
inline void heap_release_to_zero(HeapObj* h) {
  if (h->kind == ValueTag::Object && h->owner && !h->owner->sweeping()) {
    ++h->rc;  // pin across the destructor
    h->owner->run_drop(h);
    if (--h->rc != 0) return;  // resurrected
  }
  // Off the heap list before its children go: releasing them can run a
  // destructor, which can allocate, which can collect -- and a collection
  // must not find a zero-count object mid-destruction and free it a second
  // time. The same order collect() and ~Runtime use.
  if (h->owner) {
    h->owner->unlink(h);
    h->owner = nullptr;
  }
  destroy_heap_object(h);
}

inline Runtime::~Runtime() {
  for (HeapObj* o = head_; o; o = o->next) ++o->rc;
  for (HeapObj* o = head_; o; o = o->next) clear_heap_object_refs(o);
  while (head_) {
    HeapObj* o = head_;
    unlink(o);
    o->owner = nullptr;  // already unlinked; do not let ~HeapObj do it again
    destroy_heap_object(o);
  }
}

inline HeapObj::HeapObj(ValueTag k) : rc(1), kind(k), owner(Runtime::current()) {
  if (owner) owner->link(this);
}

inline HeapObj::~HeapObj() {
  if (owner) owner->unlink(this);
}

inline void clear_heap_object_refs(HeapObj* o) {
  switch (o->kind) {
    case ValueTag::Array:
      static_cast<ArrayObj*>(o)->items.clear();
      break;
    case ValueTag::Object:
      static_cast<ObjectObj*>(o)->props.clear();
      break;
    case ValueTag::Cell:
      static_cast<CellObj*>(o)->v = Value();
      break;
    case ValueTag::Func:
      static_cast<ClosureObj*>(o)->cells.clear();
      break;
    case ValueTag::Generator:
      static_cast<GeneratorObj*>(o)->frame = GenFrame{};
      break;
    case ValueTag::Map:
      static_cast<MapObj*>(o)->clear();
      break;
    case ValueTag::Coroutine: {
      auto* co = static_cast<CoroObj*>(o);
      co->fn = Value();
      co->frames.clear();
      break;
    }
    default:
      break;  // a string, or a native, refers to nothing
  }
}

inline void destroy_heap_object(HeapObj* o) {
  switch (o->kind) {
    case ValueTag::Str:   delete static_cast<StrObj*>(o); break;
    case ValueTag::Array: delete static_cast<ArrayObj*>(o); break;
    case ValueTag::Object: delete static_cast<ObjectObj*>(o); break;
    case ValueTag::Cell:  delete static_cast<CellObj*>(o); break;
    case ValueTag::Func:  delete static_cast<ClosureObj*>(o); break;
    case ValueTag::Generator: delete static_cast<GeneratorObj*>(o); break;
    case ValueTag::Map:   delete static_cast<MapObj*>(o); break;
    case ValueTag::Coroutine: delete static_cast<CoroObj*>(o); break;
    case ValueTag::Native: delete static_cast<NativeObj*>(o); break;
    default: break;  // no other tag names a heap object
  }
}

// Extracted from the Dumper's node() switch below, so dump_ir()'s vocabulary
// and this one are the same table read two ways: Unary/Binary/Intrinsic are
// the exceptions, since the Dumper prints their *operator* name there
// instead of the tag's.
inline const char* name_of(Tag t) {
  switch (t) {
    case Tag::Literal:     return "literal";
    case Tag::VarRef:      return "varref";
    case Tag::Unary:       return "unary";
    case Tag::Binary:      return "binary";
    case Tag::Assign:      return "assign";
    case Tag::If:          return "if";
    case Tag::Switch:      return "switch";
    case Tag::While:       return "while";
    case Tag::Block:       return "block";
    case Tag::Intrinsic:   return "intrinsic";
    case Tag::MakeClosure: return "makeclosure";
    case Tag::CallValue:   return "callvalue";
    case Tag::ArrayLit:    return "arraylit";
    case Tag::ObjectLit:   return "objectlit";
    case Tag::Index:       return "index";
    case Tag::SetIndex:    return "setindex";
    case Tag::FieldGet:    return "fieldget";
    case Tag::FieldSet:    return "fieldset";
    case Tag::Scope:       return "scope";
    case Tag::Return:      return "return";
    case Tag::Break:       return "break";
    case Tag::Continue:    return "continue";
    case Tag::Throw:       return "throw";
    case Tag::TryCatch:    return "try";
    case Tag::Defer:       return "defer";
    case Tag::CellFresh:   return "cellfresh";
    case Tag::Yield:       return "yield";
    case Tag::NativeRef:   return "nativeref";
  }
  return "?";
}

inline const char* name_of(UnOp op) {
  switch (op) {
    case UnOp::Neg:    return "neg";
    case UnOp::BitNot: return "bitnot";
    case UnOp::WrapI8:  return "wrapi8";
    case UnOp::WrapI16: return "wrapi16";
    case UnOp::WrapI32: return "wrapi32";
    case UnOp::WrapU8:  return "wrapu8";
    case UnOp::WrapU16: return "wrapu16";
    case UnOp::WrapU32: return "wrapu32";
  }
  return "?";
}

// Declared in ir.h and public: vm/bytecode.cc's own instruction-name table
// shares this rather than repeating the eleven arithmetic/compare names in a
// second switch of its own.
inline const char* name_of(BinOp op) {
  switch (op) {
    case BinOp::Add: return "add";
    case BinOp::Sub: return "sub";
    case BinOp::Mul: return "mul";
    case BinOp::Div: return "div";
    case BinOp::Mod: return "mod";
    case BinOp::Eq:  return "eq";
    case BinOp::Ne:  return "ne";
    case BinOp::Lt:  return "lt";
    case BinOp::Le:  return "le";
    case BinOp::Gt:  return "gt";
    case BinOp::Ge:  return "ge";
    case BinOp::BitAnd: return "bitand";
    case BinOp::BitOr:  return "bitor";
    case BinOp::BitXor: return "bitxor";
    case BinOp::Shl:    return "shl";
    case BinOp::Shr:    return "shr";
    case BinOp::UDiv: return "udiv";
    case BinOp::UMod: return "umod";
    case BinOp::UShr: return "ushr";
    case BinOp::ULt:  return "ult";
    case BinOp::ULe:  return "ule";
    case BinOp::UGt:  return "ugt";
    case BinOp::UGe:  return "uge";
  }
  return "?";
}

// Public for the same reason name_of(BinOp) is: the bytecode dumper names
// var kinds too, and one vocabulary beats two that can drift.
inline const char* name_of(VarKind k) {
  switch (k) {
    case VarKind::Local:   return "local";
    case VarKind::Capture: return "capture";
    case VarKind::Cell:    return "cell";
  }
  return "?";
}

// Declared in ir.h and public, same as BinOp/VarKind above: a front end
// resolving its own intrinsic name (codegen.h's `intrinsic(name:)`, say)
// shares this rather than typing the 24 names a second time.
inline const char* name_of(IntrinsicId id) {
  switch (id) {
    case IntrinsicId::Print:   return "print";
    case IntrinsicId::ReadInt: return "readint";
    case IntrinsicId::Len:     return "len";
    case IntrinsicId::ToStr:   return "tostr";
    case IntrinsicId::TypeOf:  return "typeof";
    case IntrinsicId::ToInt:   return "toint";
    case IntrinsicId::ToDouble: return "todouble";
    case IntrinsicId::FMod:    return "fmod";
    case IntrinsicId::Pow:     return "pow";
    case IntrinsicId::PrintRaw: return "printraw";
    case IntrinsicId::ArrayPush: return "arraypush";
    case IntrinsicId::ArrayPop: return "arraypop";
    case IntrinsicId::ObjectHas: return "objecthas";
    case IntrinsicId::ObjectKeys: return "objectkeys";
    case IntrinsicId::ObjectRemove: return "objectremove";
    case IntrinsicId::ArgCount: return "argcount";
    case IntrinsicId::Same: return "same";
    case IntrinsicId::FnArity: return "fnarity";
    case IntrinsicId::Collect: return "collect";
    case IntrinsicId::HeapStats: return "heapstats";
    case IntrinsicId::GenResume: return "genresume";
    case IntrinsicId::GenReturn: return "genreturn";
    case IntrinsicId::GenThrow: return "genthrow";
    case IntrinsicId::Enqueue: return "enqueue";
    case IntrinsicId::ToFloat32: return "tofloat32";
    case IntrinsicId::StrSlice: return "strslice";
    case IntrinsicId::ArraySlice: return "arrayslice";
    case IntrinsicId::StrByte: return "strbyte";
    case IntrinsicId::StrFromByte: return "strfrombyte";
    case IntrinsicId::MapNew: return "mapnew";
    case IntrinsicId::CoroCreate: return "corocreate";
    case IntrinsicId::CoroResume: return "cororesume";
    case IntrinsicId::CoroYield: return "coroyield";
    case IntrinsicId::CoroClose: return "coroclose";
    case IntrinsicId::CoroStatus: return "corostatus";
    case IntrinsicId::CoroCurrent: return "corocurrent";
  }
  return "?";
}

inline const char* name_of(ConstKind k) {
  switch (k) {
    case ConstKind::Nil:    return "nil";
    case ConstKind::Int:    return "int";
    case ConstKind::Bool:   return "bool";
    case ConstKind::Double: return "double";
    case ConstKind::Str:    return "str";
  }
  return "?";
}

// The inverse of each name_of above: name_of, run over every value the enum
// holds, until one matches -- so the two directions can never name the same
// value differently by construction. Each enum's values are contiguous from
// 0 (a plain enum class with no explicit initializers), so the scan's upper
// bound is just its last enumerator.
template <>
inline std::optional<Tag> from_name<Tag>(std::string_view s) {
  for (uint8_t i = 0; i <= static_cast<uint8_t>(kLastTag); ++i) {
    const auto t = static_cast<Tag>(i);
    if (name_of(t) == s) return t;
  }
  return std::nullopt;
}

template <>
inline std::optional<UnOp> from_name<UnOp>(std::string_view s) {
  for (uint8_t i = 0; i <= static_cast<uint8_t>(kLastUnOp); ++i) {
    const auto op = static_cast<UnOp>(i);
    if (name_of(op) == s) return op;
  }
  return std::nullopt;
}

template <>
inline std::optional<BinOp> from_name<BinOp>(std::string_view s) {
  for (uint8_t i = 0; i <= static_cast<uint8_t>(kLastBinOp); ++i) {
    const auto op = static_cast<BinOp>(i);
    if (name_of(op) == s) return op;
  }
  return std::nullopt;
}

template <>
inline std::optional<VarKind> from_name<VarKind>(std::string_view s) {
  for (uint8_t i = 0; i <= static_cast<uint8_t>(VarKind::Cell); ++i) {
    const auto k = static_cast<VarKind>(i);
    if (name_of(k) == s) return k;
  }
  return std::nullopt;
}

template <>
inline std::optional<IntrinsicId> from_name<IntrinsicId>(std::string_view s) {
  for (uint8_t i = 0; i <= static_cast<uint8_t>(kLastIntrinsic); ++i) {
    const auto id = static_cast<IntrinsicId>(i);
    if (name_of(id) == s) return id;
  }
  return std::nullopt;
}

template <>
inline std::optional<ConstKind> from_name<ConstKind>(std::string_view s) {
  for (uint8_t i = 0; i <= static_cast<uint8_t>(ConstKind::Str); ++i) {
    const auto k = static_cast<ConstKind>(i);
    if (name_of(k) == s) return k;
  }
  return std::nullopt;
}

namespace detail {

struct Verifier {
  const Module& m;
  std::string err;

  bool fail(const std::string& msg) {
    if (err.empty()) err = msg;
    return false;
  }

  bool check_node(NodeId id, const Func& f, int loop_depth, int scope_depth) {
    if (!id.valid() || id.v >= m.nodes.size()) return fail("dangling NodeId");
    const Node& n = m.at(id);

    if (n.pos >= m.positions.size()) return fail("node pos out of range");
    if (n.first_child + n.num_children > m.child_ids.size()) {
      return fail("node children out of range");
    }

    const int want = arity_of(n.tag);
    if (want >= 0 && n.num_children != static_cast<uint32_t>(want)) {
      return fail("wrong arity for tag");
    }

    switch (n.tag) {
      case Tag::Literal:
        if (n.a < 0 || static_cast<size_t>(n.a) >= m.consts.size()) {
          return fail("literal const index out of range");
        }
        break;
      case Tag::VarRef:
      case Tag::Assign: {
        if (n.a < 0 || n.a >= slot_limit(f, static_cast<VarKind>(n.op))) {
          return fail("var index out of range");
        }
        break;
      }
      case Tag::If:
        if (n.num_children != 2 && n.num_children != 3) {
          return fail("If takes 2 or 3 children");
        }
        break;
      // The subject aside, children come in (key, body) pairs plus an
      // optional trailing default -- an even total means the last one is
      // that default, the same parity test ObjectLit would use if it had
      // an odd-shaped variant. Each key must be a Literal (verify() cannot
      // evaluate an arbitrary expression at compile time), all of one
      // ConstKind, and pairwise distinct -- ambiguity FnCompiler's table
      // build has no way to resolve.
      case Tag::Switch: {
        if (n.num_children < 1) return fail("Switch needs a subject");
        const auto sw = view_switch(m, id);
        std::optional<ConstKind> kind;
        std::vector<int64_t> seen_int;
        std::vector<std::string> seen_str;
        // Linear, like intern_pos: a switch has a handful of arms, and a
        // set would cost more to build than the scan it saves. Written once
        // over whichever pool the key's kind picked, so the two kinds
        // cannot drift apart on what counts as a duplicate.
        auto first_time = [](auto& seen, const auto& v) {
          if (std::find(seen.begin(), seen.end(), v) != seen.end()) {
            return false;
          }
          seen.push_back(v);
          return true;
        };
        for (uint32_t i = 0; i < sw.arm_count; ++i) {
          const NodeId key = switch_key(m, id, i);
          if (!key.valid() || key.v >= m.nodes.size() ||
              m.at(key).tag != Tag::Literal) {
            return fail("switch key must be a literal");
          }
          const ConstKind k = m.const_kind(key);
          if (k != ConstKind::Int && k != ConstKind::Str) {
            return fail("switch key must be an int or a string");
          }
          if (!kind.has_value()) {
            kind = k;
          } else if (*kind != k) {
            return fail("switch keys must share one const kind");
          }
          const bool fresh = k == ConstKind::Int
                                 ? first_time(seen_int, m.int_const(key))
                                 : first_time(seen_str, m.str_const(key));
          if (!fresh) return fail("duplicate switch key");
        }
        break;
      }
      // A closure's captures are cells, which outlive the frame that
      // built it. A plain Local cannot be one -- it dies with the frame -- so
      // naming one here is the mistake this rejects, and the front end's cue
      // that the variable needed promoting to a Cell first.
      case Tag::MakeClosure: {
        if (n.a < 0 || static_cast<size_t>(n.a) >= m.funcs.size()) {
          return fail("closure func index out of range");
        }
        if (n.b < 0 || static_cast<size_t>(n.b) >= m.capture_maps.size()) {
          return fail("closure capture map index out of range");
        }
        const auto& cmap = m.capture_maps[n.b];
        if (cmap.size() != static_cast<size_t>(m.funcs[n.a].num_captures)) {
          return fail("capture map length does not match closure");
        }
        for (const CaptureSrc& src : cmap) {
          if (src.from == VarKind::Local) {
            return fail("a closure cannot capture a local; promote it to a cell");
          }
          if (src.index < 0 || src.index >= slot_limit(f, src.from)) {
            return fail("capture map entry out of range in caller frame");
          }
        }
        break;
      }
      case Tag::CallValue:
        if (n.num_children < 1) return fail("CallValue needs a callee");
        break;
      case Tag::Scope: {
        if (n.a < 0 || n.a > n.b || n.b > f.num_locals) {
          return fail("scope local range out of range");
        }
        if (n.num_children != 1 && n.num_children != 2) {
          return fail("Scope takes a body and an optional release list");
        }
        if (n.num_children == 2) {
          const NodeId list = m.child(id, 1);
          if (!list.valid() || list.v >= m.nodes.size() ||
              m.at(list).tag != Tag::Block) {
            return fail("scope release list must be a Block of VarRef");
          }
          std::vector<std::pair<VarKind, int32_t>> seen;
          for (uint32_t i = 0; i < m.num_children(list); ++i) {
            const NodeId e = m.child(list, i);
            if (!e.valid() || e.v >= m.nodes.size() ||
                m.at(e).tag != Tag::VarRef) {
              return fail("scope release list must be a Block of VarRef");
            }
            const auto v = view_varref(m, e);
            if (v.kind == VarKind::Capture) {
              return fail("scope release list cannot name a capture");
            }
            if (v.index < 0 || v.index >= slot_limit(f, v.kind)) {
              return fail("scope release list entry out of range");
            }
            if (v.kind == VarKind::Local && (v.index < n.a || v.index >= n.b)) {
              return fail("scope release list names a local outside its range");
            }
            for (const auto& s : seen) {
              if (s.first == v.kind && s.second == v.index) {
                return fail("scope release list repeats an entry");
              }
            }
            seen.emplace_back(v.kind, v.index);
          }
        }
        break;
      }
      case Tag::Return:
        if (n.num_children > 1) return fail("Return takes 0 or 1 children");
        break;
      case Tag::Break:
        if (loop_depth == 0) return fail("Break outside a loop body");
        if (view_break(m, id).depth < 0 ||
            view_break(m, id).depth >= loop_depth) {
          return fail("Break names a loop that is not open");
        }
        break;
      case Tag::Continue:
        if (loop_depth == 0) return fail("Continue outside a loop body");
        if (view_continue(m, id).depth < 0 ||
            view_continue(m, id).depth >= loop_depth) {
          return fail("Continue names a loop that is not open");
        }
        break;
      case Tag::TryCatch:
        if (n.a < 0 || n.a >= f.num_locals) {
          return fail("caught local slot out of range");
        }
        break;
      case Tag::Defer:
        if (scope_depth == 0) {
          return fail("Defer outside a Scope; wrap the region in one");
        }
        break;
      case Tag::CellFresh:
        if (n.a < 0 || n.a >= f.num_cells) {
          return fail("cell index out of range");
        }
        break;
      case Tag::Yield:
        if (!f.is_generator) {
          return fail("Yield outside a generator function");
        }
        break;
      case Tag::NativeRef: {
        const int32_t idx = view_native_ref(m, id).index;
        if (idx < 0 || static_cast<size_t>(idx) >= m.natives.size()) {
          return fail("native index out of range");
        }
        break;
      }
      case Tag::ObjectLit:
        if (n.num_children % 2 != 0) {
          return fail("ObjectLit takes key/value pairs");
        }
        break;
      // The slot itself is the front end's promise, the same way a Local's
      // slot index is -- not something verify() can bound-check against a
      // receiver whose actual prop count is a runtime fact. What it can
      // check is the diagnostic name: a name const that is not there, or
      // not a string, is a front end bug regardless of what receiver shows
      // up at runtime.
      case Tag::FieldGet:
      case Tag::FieldSet:
        if (n.a < 0) return fail("field slot must be non-negative");
        if (n.b < 0 || static_cast<size_t>(n.b) >= m.consts.size() ||
            m.consts[static_cast<size_t>(n.b)].kind != ConstKind::Str) {
          return fail("field name must be a string const");
        }
        break;
      case Tag::Intrinsic: {
        const auto id = static_cast<IntrinsicId>(n.op);
        if (n.num_children != intrinsic_arity(id)) {
          return fail("wrong arity for intrinsic");
        }
        break;
      }
      default:
        break;
    }

    // Operand positions must hold value-producing nodes.
    auto operand = [&](uint32_t i) {
      NodeId c = m.child(id, i);
      if (!c.valid() || c.v >= m.nodes.size()) return fail("dangling child");
      if (!yields_value(m.at(c).tag)) return fail("statement in value position");
      return true;
    };

    switch (n.tag) {
      case Tag::Unary:
      case Tag::Binary:
      case Tag::Intrinsic:
      case Tag::ArrayLit:
      case Tag::ObjectLit:
      case Tag::Index:
      case Tag::SetIndex:
      case Tag::FieldGet:
      case Tag::FieldSet:
      case Tag::CallValue:  // callee, then args -- all value positions
        for (uint32_t i = 0; i < n.num_children; ++i) {
          if (!operand(i)) return false;
        }
        break;
      case Tag::Assign:
      case Tag::If:
      case Tag::While:
      case Tag::Throw:
      case Tag::Defer:
      case Tag::Yield:
        if (!operand(0)) return false;
        break;
      // Only the subject and the keys are read as values; each arm body is
      // compiled through compile_value the way If's branches are (branch_
      // into -> compile_value), so a statement body -- a bare Break, an
      // Assign -- is as legal here as it is as an If arm.
      case Tag::Switch: {
        if (!operand(0)) return false;
        const auto sw = view_switch(m, id);
        for (uint32_t i = 0; i < sw.arm_count; ++i) {
          if (!operand(1 + 2 * i)) return false;
        }
        break;
      }
      default:
        break;
    }

    for (uint32_t i = 0; i < n.num_children; ++i) {
      // A While's body is the one edge that opens a loop; its condition is
      // not part of the body, so a Break there names the loop outside it.
      const int child_depth =
          (n.tag == Tag::While && i == 1) ? loop_depth + 1 : loop_depth;
      const int child_scope =
          n.tag == Tag::Scope ? scope_depth + 1 : scope_depth;
      if (!check_node(m.child(id, i), f, child_depth, child_scope)) {
        return false;
      }
    }
    return true;
  }
};

struct Dumper {
  const Module& m;
  std::ostringstream out;

  void indent(int d) { out << std::string(static_cast<size_t>(d) * 2, ' '); }

  void node(NodeId id, int d) {
    const Node& n = m.at(id);
    const SrcPos p = m.pos_of(id);
    indent(d);
    switch (n.tag) {
      case Tag::Literal:
        out << "literal " << m.int_const(id);
        break;
      case Tag::VarRef: {
        auto v = view_varref(m, id);
        out << "varref " << name_of(v.kind) << "[" << v.index << "]";
        break;
      }
      case Tag::Unary:
        out << name_of(static_cast<UnOp>(n.op));
        break;
      case Tag::Binary:
        out << name_of(static_cast<BinOp>(n.op));
        break;
      case Tag::Assign: {
        auto v = view_assign(m, id);
        out << "assign " << name_of(v.kind) << "[" << v.index << "]";
        break;
      }
      case Tag::If:     out << "if"; break;
      case Tag::Switch: out << "switch"; break;
      case Tag::Scope: {
        const auto v = view_scope(m, id);
        out << "scope local[" << v.first_local << ".." << v.end_local << ")";
        if (v.release_order.valid()) out << " +release";
        break;
      }
      case Tag::Return:   out << "return"; break;
      case Tag::Break:
      case Tag::Continue: {
        out << name_of(n.tag);
        const int32_t depth = n.tag == Tag::Break ? view_break(m, id).depth
                                                  : view_continue(m, id).depth;
        if (depth > 0) out << " ^" << depth;  // how many loops it skips
        break;
      }
      case Tag::Throw:    out << "throw"; break;
      case Tag::Defer:    out << "defer"; break;
      case Tag::CellFresh:
        out << "cellfresh cell[" << view_cellfresh(m, id).cell << "]";
        break;
      case Tag::Yield: out << "yield"; break;
      case Tag::TryCatch:
        out << "try caught=local[" << view_try(m, id).caught_local << "]";
        break;
      case Tag::While:  out << "while"; break;
      case Tag::Block:  out << "block"; break;
      case Tag::Intrinsic:
        out << name_of(static_cast<IntrinsicId>(n.op));
        break;
      case Tag::MakeClosure:
        out << "makeclosure " << m.funcs[n.a].name << " #" << n.a
            << " cmap=" << n.b;
        break;
      case Tag::CallValue:
        out << "callvalue";
        break;
      case Tag::ArrayLit:  out << "arraylit"; break;
      case Tag::ObjectLit: out << "objectlit"; break;
      case Tag::Index:    out << "index"; break;
      case Tag::SetIndex: out << "setindex"; break;
      // One case for both: FieldSetView is FieldView plus a value child, so
      // the slot and the name const are the same a/b either way, and the
      // word is the one name_of already has for the tag -- the same way
      // Unary and Binary above defer to name_of for their operator's name
      // rather than spelling it out here.
      case Tag::FieldGet:
      case Tag::FieldSet: {
        const auto v = view_field_get(m, id);
        out << name_of(n.tag) << " field[" << v.slot
            << "]=" << m.str_const_at(v.name_const);
        break;
      }
      case Tag::NativeRef: {
        const int32_t idx = view_native_ref(m, id).index;
        out << "nativeref " << m.natives[static_cast<size_t>(idx)] << " #"
            << idx;
        break;
      }
    }
    out << "  @" << p.line << ":" << p.col << "\n";
    for (uint32_t i = 0; i < n.num_children; ++i) node(m.child(id, i), d + 1);
  }
};

}  // namespace detail

inline std::optional<std::string> verify(const Module& m) {
  if (m.funcs.empty()) return std::string("module has no entry function");
  detail::Verifier v{m, {}};
  for (const Func& f : m.funcs) {
    if (static_cast<size_t>(f.num_locals) != f.local_names.size() ||
        static_cast<size_t>(f.num_captures) != f.capture_names.size()) {
      return std::string("func name table does not match its slot count");
    }
    // The params are the first num_params locals, so a frame that declares
    // more params than locals has the call write past its own locals array
    // (push_closure fills [0, num_params) of a vector sized num_locals).
    // The name-table check above already pins num_locals and num_captures to
    // a real size; num_cells and num_params have no table to be pinned by.
    if (f.num_cells < 0 || f.num_params < 0) {
      return std::string("func slot count is negative");
    }
    if (f.num_params > f.num_locals) {
      return std::string("func declares more params than locals");
    }
    if (!v.check_node(f.body, f, 0, 0)) return v.err;
  }
  if (!m.funcs[0].capture_names.empty()) {
    return std::string("entry function must capture nothing");
  }
  if (m.funcs[0].is_generator) {
    return std::string("entry function cannot be a generator");
  }
  return std::nullopt;
}

inline std::string to_string(const Module& m) {
  detail::Dumper d{m, {}};
  for (size_t i = 0; i < m.funcs.size(); ++i) {
    const Func& f = m.funcs[i];
    d.out << "func #" << i << " " << f.name << "  locals=" << f.num_locals
          << " captures=" << f.num_captures;
    if (!f.capture_names.empty()) {
      d.out << " [";
      for (size_t j = 0; j < f.capture_names.size(); ++j) {
        d.out << (j ? " " : "") << f.capture_names[j];
      }
      d.out << "]";
    }
    d.out << "\n";
    d.node(f.body, 1);
  }
  for (size_t i = 0; i < m.capture_maps.size(); ++i) {
    d.out << "cmap " << i << ":";
    for (const CaptureSrc& s : m.capture_maps[i]) {
      d.out << " " << name_of(s.from) << "[" << s.index << "]";
    }
    d.out << "\n";
  }
  return d.out.str();
}

}  // namespace coreir

// ===== vm/bytecode.cc =====

namespace vm {
namespace detail {

inline const char* name_of(Op op) {
  switch (op) {
    case Op::LoadConst:   return "loadconst";
    case Op::Neg:         return "neg";
    case Op::BitNot:      return "bitnot";
    // Shares coreir's name table via unop_of rather than re-typing the
    // Wrap* names.
    case Op::WrapI8: case Op::WrapI16: case Op::WrapI32:
    case Op::WrapU8: case Op::WrapU16: case Op::WrapU32:
      return coreir::name_of(unop_of(op));
    // The arithmetic/compare range shares coreir's name table via binop_of
    // rather than re-typing the same strings.
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
    case Op::Eq:  case Op::Ne:  case Op::Lt:  case Op::Le:
    case Op::Gt:  case Op::Ge:
    case Op::BitAnd: case Op::BitOr: case Op::BitXor:
    case Op::Shl: case Op::Shr:
    case Op::UDiv: case Op::UMod: case Op::UShr:
    case Op::ULt: case Op::ULe: case Op::UGt: case Op::UGe:
      return coreir::name_of(binop_of(op));
    case Op::LoadLocal:   return "loadlocal";
    case Op::LoadCell:    return "loadcell";
    case Op::LoadCapture: return "loadcapture";
    case Op::StoreLocal:  return "storelocal";
    case Op::StoreCell:   return "storecell";
    case Op::StoreCapture: return "storecapture";
    case Op::Jump:        return "jump";
    case Op::JumpIfFalse: return "jumpiffalse";
    case Op::Switch:      return "switch";
    case Op::Out:         return "out";
    case Op::OutRaw:      return "outraw";
    case Op::In:          return "in";
    case Op::Ret:         return "ret";
    case Op::Throw:       return "throw";
    case Op::DeferPush:   return "deferpush";
    case Op::DeferMark:   return "defermark";
    case Op::DeferRunTo:  return "deferrunto";
    case Op::OwnedMark:   return "ownedmark";
    case Op::MakeClosure: return "makeclosure";
    case Op::CallValue:   return "callvalue";
    case Op::TailCall:    return "tailcall";
    case Op::CellNew:     return "cellnew";
    case Op::LoadNil:     return "loadnil";
    case Op::Move:        return "move";
    case Op::ClearRegs:   return "clearregs";
    case Op::ClearLocals: return "clearlocals";
    case Op::ReleaseSlots: return "releaseslots";
    case Op::NewArray:    return "newarray";
    case Op::Index:       return "index";
    case Op::SetIndex:    return "setindex";
    case Op::FieldGet:    return "fieldget";
    case Op::FieldSet:    return "fieldset";
    case Op::Len:         return "len";
    case Op::ToStr:       return "tostr";
    case Op::ArrayPush:   return "arraypush";
    case Op::ArrayPop:    return "arraypop";
    case Op::ObjectHas:   return "objecthas";
    case Op::ObjectKeys:  return "objectkeys";
    case Op::ObjectRemove: return "objectremove";
    case Op::ArgCount:     return "argcount";
    case Op::Same:         return "same";
    case Op::FnArity:      return "fnarity";
    case Op::Collect:      return "collect";
    case Op::HeapStats:    return "heapstats";
    case Op::TypeOf:      return "typeof";
    case Op::ToInt:       return "toint";
    case Op::ToDouble:    return "todouble";
    case Op::FMod:        return "fmod";
    case Op::Pow:         return "pow";
    case Op::NewObject:   return "newobject";
    case Op::Yield:       return "yield";
    case Op::GenResume:   return "genresume";
    case Op::GenReturn:   return "genreturn";
    case Op::GenThrow:    return "genthrow";
    case Op::Enqueue:     return "enqueue";
    case Op::ToFloat32:   return "tofloat32";
    case Op::StrSlice:    return "strslice";
    case Op::ArraySlice:  return "arrayslice";
    case Op::StrByte:     return "strbyte";
    case Op::StrFromByte: return "strfrombyte";
    case Op::NewMap:      return "newmap";
    case Op::NativeRef:   return "nativeref";
    case Op::CoroCreate:  return "corocreate";
    case Op::CoroResume:  return "cororesume";
    case Op::CoroYield:   return "coroyield";
    case Op::CoroClose:   return "coroclose";
    case Op::CoroStatus:  return "corostatus";
    case Op::CoroCurrent: return "corocurrent";
  }
  return "?";
}

}  // namespace detail

inline std::string to_string(const Program& p) {
  std::ostringstream out;
  for (size_t i = 0; i < p.chunks.size(); ++i) {
    const Chunk& ch = p.chunks[i];
    out << "chunk #" << i << " " << ch.name << "  locals=" << ch.num_locals
        << " captures=" << ch.num_captures << " regs=" << ch.num_regs << "\n";
    for (size_t j = 0; j < ch.code.size(); ++j) {
      const Insn& in = ch.code[j];
      const coreir::SrcPos sp = p.positions[ch.code_pos[j]];
      out << "  " << j << "\t" << detail::name_of(in.op);
      switch (in.op) {
        case Op::LoadConst:
          out << " r" << in.a << ", " << p.consts[in.b].bits;
          break;
        case Op::Neg:
          out << " r" << in.a << ", r" << in.b;
          break;
        // The opcode's own name says which storage class this is, so the
        // operand is just the index into it.
        case Op::LoadLocal: case Op::LoadCell: case Op::LoadCapture:
          out << " r" << in.a << ", [" << in.b << "]";
          break;
        case Op::StoreLocal: case Op::StoreCell: case Op::StoreCapture:
          out << " [" << in.a << "], r" << in.b;
          break;
        case Op::MakeClosure:
          out << " r" << in.a << ", #" << in.b << " cmap=" << in.c;
          break;
        case Op::NativeRef:
          out << " r" << in.a << ", " << p.natives[static_cast<size_t>(in.b)]
              << " #" << in.b;
          break;
        case Op::CallValue:
        case Op::TailCall:
          out << " r" << in.a << ", r" << in.b << ", args r" << in.c << ".."
              << (in.c + in.d);
          break;
        case Op::CellNew:
          out << " cell[" << in.a << "]";
          break;
        case Op::ClearRegs:
          out << " r" << in.a << ".." << in.b;
          break;
        case Op::ClearLocals:
          out << " local[" << in.a << ".." << in.b << ")";
          break;
        case Op::ReleaseSlots:
          out << " list#" << in.a;
          break;
        case Op::NewArray:
          out << " r" << in.a << ", items r" << in.b << ".."
              << (in.b + in.c);
          break;
        case Op::Jump:
          out << " " << in.a;
          break;
        case Op::JumpIfFalse:
          out << " r" << in.a << ", " << in.b;
          break;
        case Op::Switch:
          out << " r" << in.a << ", table#" << in.b;
          break;
        case Op::Out:
        case Op::OutRaw:
        case Op::In:
        case Op::Throw:
        case Op::DeferPush:
          out << " r" << in.a;
          break;
        case Op::DeferMark:
        case Op::DeferRunTo:
        case Op::OwnedMark:
          break;
        case Op::Ret:
          break;
        case Op::FieldGet:
          out << " r" << in.a << ", r" << in.b << ", field[" << in.c << "]";
          break;
        case Op::FieldSet:
          out << " r" << in.a << ", field[" << in.b << "], r" << in.c;
          break;
        case Op::StrSlice:
        case Op::ArraySlice:
          out << " r" << in.a << ", r" << in.b << ", r" << in.c << ", r"
              << in.d;
          break;
        default:
          out << " r" << in.a << ", r" << in.b << ", r" << in.c;
          break;
      }
      out << "\t; " << sp.line << ":" << sp.col << "\n";
    }
    for (const Cleanup& cl : ch.cleanups) {
      out << "  cleanup [" << cl.start_pc << ".." << cl.end_pc << ") local["
          << cl.first_local << ".." << cl.end_local << ") regs>=" << cl.regs_base;
      if (cl.handler_pc >= 0) {
        out << " handler=" << cl.handler_pc << " caught=local["
            << cl.caught_local << "]";
      }
      if (cl.defer_mark_pc >= 0) out << " defer_mark=" << cl.defer_mark_pc;
      if (cl.owned_mark_pc >= 0) out << " owned_mark=" << cl.owned_mark_pc;
      if (cl.release_list >= 0) out << " release=list#" << cl.release_list;
      out << "\n";
    }
    for (size_t j = 0; j < ch.switch_tables.size(); ++j) {
      const SwitchTable& t = ch.switch_tables[j];
      out << "  switch_table#" << j << " "
          << (t.key_kind == coreir::ConstKind::Str ? "str" : "int") << " "
          << (t.dense ? "dense" : "sparse");
      if (t.dense) {
        out << " base=" << t.base << " [";
        for (size_t k = 0; k < t.pcs.size(); ++k) {
          out << (k ? " " : "") << t.pcs[k];
        }
        out << "]";
      } else if (t.key_kind == coreir::ConstKind::Str) {
        for (size_t k = 0; k < t.str_keys.size(); ++k) {
          out << " \"" << t.str_keys[k] << "\"->" << t.arm_pcs[k];
        }
      } else {
        for (size_t k = 0; k < t.int_keys.size(); ++k) {
          out << " " << t.int_keys[k] << "->" << t.arm_pcs[k];
        }
      }
      if (t.default_pc >= 0) out << " default=" << t.default_pc;
      out << "\n";
    }
  }
  return out.str();
}

}  // namespace vm

// ===== vm/compiler.cc =====

namespace vm {
namespace detail {

// Scoped to vm::detail: a using-directive at vm scope would leak into every
// translation unit that reopens the namespace.
using namespace coreir;

struct FnCompiler {
  const Module& m;
  Chunk& ch;

  // The loops and scopes currently open at the point being compiled, so a
  // non-local exit can leave each region it crosses the way the region's own
  // exit would.
  struct OpenLoop {
    int32_t head;                    // where Continue re-enters (the test)
    int32_t stmt_base;               // register floor of the While statement
    size_t scope_depth;              // open_scopes.size() at loop entry
    std::vector<size_t> break_jumps; // patched to the loop's exit
  };
  std::vector<OpenLoop> open_loops;
  struct OpenScope {
    int32_t first_local, end_local;
    bool has_defers;
    int32_t release_list;  // -1: release the range
  };
  std::vector<OpenScope> open_scopes;
  // Where the most recently closed Scope's exit-time DeferRunTo sits, so a
  // TryCatch whose body is that scope can end its guarded region before it
  // (a defer throwing at the body's fall-through exit escapes its own catch,
  // the way culebra's does). -1 when the last scope had no defers.
  int32_t last_scope_defer_run_pc = -1;
  // How many TryCatch bodies enclose the point being compiled. A tail call
  // inside one would drop the handler with the frame (Cleanup regions are
  // pc ranges of this chunk), so none is emitted there.
  int try_depth = 0;

  // Whether a CallValue here may be emitted as a TailCall: the function
  // asked for them (Func::tail_calls), no try body is open, and no open
  // Scope declares defers or a release order -- Func::tail_calls' comment
  // has the reasons for the first two. The third is the same shape: TailCall
  // exits the frame with a blanket release_range over the locals, which is
  // neither the declared order nor a cell refresh, so a scope that named one
  // keeps the ordinary call rather than silently getting the default order.
  bool tail_call_ok() const {
    if (!ch.tail_calls || try_depth > 0) return false;
    for (const OpenScope& sc : open_scopes) {
      if (sc.has_defers || sc.release_list >= 0) return false;
    }
    return true;
  }

  int32_t top = 0;  // next free register
  // Highest register the statement being compiled has reached, so its end can
  // drop exactly the range it used rather than every register the function
  // owns.
  int32_t high_water = 0;

  int32_t alloc() {
    const int32_t r = top++;
    if (top > high_water) high_water = top;
    ch.num_regs = std::max(ch.num_regs, top);
    return r;
  }

  size_t emit(Op op, int32_t a, int32_t b, int32_t c, uint32_t pos,
              int32_t d = 0) {
    ch.code.push_back({op, a, b, c, d});
    ch.code_pos.push_back(pos);
    return ch.code.size() - 1;
  }

  void patch(size_t at, int32_t target) {
    Insn& in = ch.code[at];
    if (in.op == Op::Jump) {
      in.a = target;
    } else {
      in.b = target;
    }
  }

  int32_t here() const { return static_cast<int32_t>(ch.code.size()); }

  // Whether this subtree registers a Defer with the scope being compiled.
  // Nested Scopes own their own defers, so the walk does not descend into
  // them; function literals are separate funcs and never reached.
  bool declares_defers(NodeId id) const {
    const Node& n = m.at(id);
    if (n.tag == Tag::Defer) return true;
    if (n.tag == Tag::Scope) return false;
    for (uint32_t i = 0; i < n.num_children; ++i) {
      if (declares_defers(m.child(id, i))) return true;
    }
    return false;
  }

  // What a jump out of nested regions owes them: the registers above the
  // target statement's floor -- temps of the abandoned regions, dead once
  // the jump is taken -- are dropped, and then each Scope down to (not
  // including) `scope_depth` is left the way its own exit leaves it. The
  // temps go first so that a scope's owned resolution does not count them
  // as holders: a condition's register still names the local it tested.
  // num_regs at this point bounds every register a region entered so far
  // can have touched, so the clear cannot miss one.
  void leave_down_to(size_t scope_depth, int32_t regs_floor, uint32_t pos) {
    if (ch.num_regs > regs_floor) {
      emit(Op::ClearRegs, regs_floor, ch.num_regs, 0, pos);
    }
    for (size_t i = open_scopes.size(); i > scope_depth; --i) {
      const OpenScope& sc = open_scopes[i - 1];
      if (sc.has_defers) emit(Op::DeferRunTo, 0, 0, 0, pos);
      emit_scope_release(sc, pos);
    }
  }

  // A scope's release: the list it spelled out, or its local range.
  void emit_scope_release(const OpenScope& sc, uint32_t pos) {
    if (sc.release_list >= 0) {
      emit(Op::ReleaseSlots, sc.release_list, 0, 0, pos);
    } else {
      emit(Op::ClearLocals, sc.first_local, sc.end_local, 0, pos);
    }
  }

  // Scope's optional release list, as a Chunk::release_lists entry.
  int32_t compile_release_list(const ScopeView& sv) {
    if (!sv.release_order.valid()) return -1;
    const NodeId list = sv.release_order;
    std::vector<SlotRef> slots;
    for (uint32_t i = 0; i < m.num_children(list); ++i) {
      const auto v = view_varref(m, m.child(list, i));
      slots.push_back({v.kind, v.index});
    }
    ch.release_lists.push_back(std::move(slots));
    return static_cast<int32_t>(ch.release_lists.size() - 1);
  }

  // Fills in ch.switch_tables[idx] -- pushed empty when the Switch's own
  // Op::Switch was emitted, so its index survives any switch_tables growth a
  // nested Switch in one of the arms just compiled -- now that every arm's
  // pc is known.
  void finish_switch_table(int32_t idx,
                           const std::vector<std::pair<NodeId, int32_t>>& arms,
                           int32_t default_pc) {
    SwitchTable t;
    t.default_pc = default_pc;
    if (!arms.empty()) t.key_kind = m.const_kind(arms.front().first);
    // Sorting a (key, pc) list and splitting it into the two parallel
    // vectors Exec binary-searches: the same two steps for int keys and for
    // str keys, so which vector the keys land in is the only thing written
    // twice.
    auto fill_sparse = [&t](auto& kv, auto& keys) {
      std::sort(kv.begin(), kv.end());
      for (auto& e : kv) {
        keys.push_back(std::move(e.first));
        t.arm_pcs.push_back(e.second);
      }
    };
    if (t.key_kind == ConstKind::Int) {
      std::vector<std::pair<int64_t, int32_t>> kv;
      kv.reserve(arms.size());
      for (const auto& a : arms) {
        kv.emplace_back(m.int_const(a.first), a.second);
      }
      int64_t lo = 0, hi = 0;
      for (size_t i = 0; i < kv.size(); ++i) {
        if (i == 0 || kv[i].first < lo) lo = kv[i].first;
        if (i == 0 || kv[i].first > hi) hi = kv[i].first;
      }
      // hi - lo is the one subtraction here whose operands are a front
      // end's own key values, so it is done unsigned: a u64-lowering front
      // end stores bit patterns, and a switch spanning {INT64_MIN,
      // INT64_MAX} would take a signed subtraction out of int64's range,
      // which is UB rather than just a wrong answer. The cap is on the
      // span (the width less one) rather than the width itself, so a
      // full-range span cannot wrap back into something small enough to
      // look dense.
      const uint64_t span =
          kv.empty() ? 0
                     : static_cast<uint64_t>(hi) - static_cast<uint64_t>(lo);
      // Dense when the array would not be mostly holes, and small enough
      // that a pathological front end (a switch over {0, 1000000}) cannot
      // make this allocate an unreasonable table.
      if (!kv.empty() && span < 4096 &&
          span + 1 <= static_cast<uint64_t>(kv.size()) * 4) {
        t.dense = true;
        t.base = lo;
        t.pcs.assign(static_cast<size_t>(span) + 1, -1);
        for (const auto& e : kv) {
          const uint64_t off =
              static_cast<uint64_t>(e.first) - static_cast<uint64_t>(lo);
          t.pcs[static_cast<size_t>(off)] = e.second;
        }
      } else {
        fill_sparse(kv, t.int_keys);
      }
    } else {
      std::vector<std::pair<std::string, int32_t>> kv;
      kv.reserve(arms.size());
      for (const auto& a : arms) {
        kv.emplace_back(m.str_const(a.first), a.second);
      }
      fill_sparse(kv, t.str_keys);
    }
    ch.switch_tables[static_cast<size_t>(idx)] = std::move(t);
  }

  // A Scope, in value position (its body's value lands in the register
  // returned) or as a statement (`want_value` false: the body is compiled
  // as one and nothing is returned). The distinction matters at the exit:
  // a value held in a register across the scope's release would count as
  // an outside holder in the owned resolution, keeping a cycle the block
  // merely named last -- `{ ...; a }` as a statement -- from dropping.
  int32_t compile_scope(NodeId id, bool want_value, bool tail = false) {
    const Node& sn = m.at(id);
    const ScopeView sv = view_scope(m, id);
    const bool defers = declares_defers(sv.body);
    const int32_t regs_base = top;
    // Every Scope takes an owned-stack mark on entry; its exit resolves
    // the drop-bearing cycles bound under it. The mark's pc is the
    // region's identity for the unwinder, as the DeferMark's is.
    const int32_t owned_pc = here();
    emit(Op::OwnedMark, 0, 0, 0, sn.pos);
    const int32_t mark_pc = defers ? here() : -1;
    if (defers) emit(Op::DeferMark, 0, 0, 0, sn.pos);
    const int32_t start = here();
    const OpenScope sc{sv.first_local, sv.end_local, defers,
                       compile_release_list(sv)};
    open_scopes.push_back(sc);
    int32_t r = -1;
    if (want_value) {
      r = compile_value(sv.body, tail);
    } else {
      compile_stmt(sv.body);
    }
    open_scopes.pop_back();
    int32_t defer_run_pc = -1;
    if (defers) {
      // The Move puts one instruction between the body's last call and
      // the exit-time defer run: a callee's resume pc then still sits
      // inside every region that must see its throw, while the
      // DeferRunTo's own pc sits outside a fused try's (below). A
      // statement body ends in its own ClearRegs, which does the same.
      if (want_value) {
        const int32_t held = alloc();
        emit(Op::Move, held, r, 0, sn.pos);
        r = held;
      }
      defer_run_pc = here();
      emit(Op::DeferRunTo, 0, 0, 0, sn.pos);
    }
    emit_scope_release(sc, sn.pos);
    ch.cleanups.push_back({start, here(), sv.first_local, sv.end_local,
                           regs_base, -1, -1, mark_pc, owned_pc,
                           sc.release_list});
    last_scope_defer_run_pc = defer_run_pc;
    return r;
  }

  // "Whatever this node is, leave its value in a register." A statement --
  // an Assign, a static Call, a While -- has no value, so it runs and yields
  // nil. Three callers need exactly this (a function body, a block's last
  // child, an If arm), and each of them can be handed a statement by a front
  // end that is not doing anything wrong.
  //
  // `tail` says the value is what the function returns, with nothing left
  // to run in this frame after it -- a Return's operand, or the body's
  // last value -- and follows the value down through Block, If, Switch and
  // Scope to whatever CallValue ends it, which may then be a TailCall. A
  // TryCatch body is never in tail position (its handler must survive the
  // call), and nothing else propagates it.
  int32_t compile_value(NodeId id, bool tail = false) {
    if (yields_value(m.at(id).tag)) return compile_expr(id, tail);
    compile_stmt(id);
    const int32_t r = alloc();
    emit(Op::LoadNil, r, 0, 0, m.at(id).pos);
    return r;
  }

  // Compile one arm of a value-producing If so its result lands in `dst`
  // rather than wherever the arm's own allocation happened to put it.
  void branch_into(int32_t dst, NodeId arm, uint32_t pos, bool tail = false) {
    const int32_t base = top;
    const int32_t r = compile_value(arm, tail);
    if (r != dst) emit(Op::Move, dst, r, 0, pos);
    top = base;
  }

  // Every expression lands in a fresh register; a statement releases whatever
  // it used. PL/0 nests shallowly enough that nothing smarter earns its keep.
  int32_t compile_expr(NodeId id, bool tail = false) {
    const Node& n = m.at(id);
    switch (n.tag) {
      case Tag::Literal: {
        const int32_t r = alloc();
        emit(Op::LoadConst, r, n.a, 0, n.pos);
        return r;
      }
      case Tag::VarRef: {
        auto v = view_varref(m, id);
        const int32_t r = alloc();
        emit(load_op_of(v.kind), r, v.index, 0, n.pos);
        return r;
      }
      case Tag::Unary: {
        auto v = view_unary(m, id);
        const int32_t base = top;
        const int32_t s = compile_expr(v.operand);
        top = base;
        const int32_t r = alloc();
        emit(op_of(v.op), r, s, 0, n.pos);
        return r;
      }
      case Tag::Scope:
        return compile_scope(id, true, tail);
      case Tag::TryCatch: {
        // dst sits below regs_base on purpose: the unwinder drops the
        // region's temps, and the result register must not be one of them.
        auto v = view_try(m, id);
        const int32_t dst = alloc();
        const int32_t regs_base = top;
        const int32_t start = here();
        last_scope_defer_run_pc = -1;
        ++try_depth;
        branch_into(dst, v.body, n.pos);
        --try_depth;
        // A body that is a defer-declaring Scope runs those defers at its
        // fall-through exit; the guarded region ends before that run, so a
        // defer throwing there escapes its own catch (culebra's rule: a try
        // ends its region before the body's fall-through defer run).
        const int32_t body_end =
            (m.at(v.body).tag == Tag::Scope && last_scope_defer_run_pc >= 0)
                ? last_scope_defer_run_pc
                : -1;
        const size_t jend = emit(Op::Jump, 0, 0, 0, n.pos);
        const int32_t handler_pc = here();
        // Otherwise the region ends where the handler starts: the jump over
        // it still belongs to the guarded range (a callee's resume pc can
        // point at it), while the handler must not be guarded by its own try.
        ch.cleanups.push_back({start, body_end >= 0 ? body_end : handler_pc,
                               0, 0, regs_base, handler_pc, v.caught_local});
        branch_into(dst, v.handler, n.pos);
        patch(jend, here());
        return dst;
      }
      case Tag::Binary: {
        auto v = view_binary(m, id);
        const int32_t base = top;
        const int32_t l = compile_expr(v.lhs);
        const int32_t rr = compile_expr(v.rhs);
        top = base;
        const int32_t r = alloc();
        // A failing division reports at its right operand, so that is the
        // position stamped on the instruction that can trap.
        const uint32_t pos = (v.op == BinOp::Div || v.op == BinOp::Mod)
                                 ? m.at(v.rhs).pos
                                 : n.pos;
        emit(op_of(v.op), r, l, rr, pos);
        return r;
      }
      case Tag::Intrinsic: {
        // Every intrinsic lowers the same way, because the three facts that
        // distinguish them are all stated elsewhere exactly once: how many
        // operands (coreir::intrinsic_arity), which opcode (op_of), and
        // whether it writes a result (intrinsic_has_dst). A new intrinsic
        // needs no case here.
        auto v = view_intrinsic(m, id);
        // Print and PrintRaw are the two intrinsics that also have a
        // statement lowering, and it does more than emit the opcode: the
        // statement path ends by clearing the registers the operand used,
        // so what was printed is not held live by a stale register. Going
        // through it keeps that; the value position then yields nil.
        if (v.id == IntrinsicId::Print || v.id == IntrinsicId::PrintRaw) {
          compile_stmt(id);
          const int32_t r = alloc();
          emit(Op::LoadNil, r, 0, 0, n.pos);
          return r;
        }
        // Three, because an Insn has four operand fields and the widest
        // intrinsic spends one of them on its destination (a slice: the
        // receiver and two bounds). An intrinsic wanting more would need a
        // different instruction shape, not a bigger array here.
        constexpr uint32_t kMaxArgs = 3;
        const uint32_t argc = std::min(intrinsic_arity(v.id), kMaxArgs);
        const int32_t base = top;
        int32_t a[kMaxArgs] = {0, 0, 0};
        for (uint32_t i = 0; i < argc; ++i) {
          a[i] = compile_expr(m.child(id, i));
        }
        top = base;
        if (intrinsic_has_dst(v.id)) {
          const int32_t r = alloc();
          emit(op_of(v.id), r, a[0], a[1], n.pos, a[2]);
          return r;
        }
        // Statement-shaped: the operands are a and b, and the value is nil.
        // Print used to emit LoadConst 0 here, which read the first entry of
        // a const pool that a program need not have -- latent until Block
        // became a value-producing tag and put Print in value position for
        // the first time.
        emit(op_of(v.id), a[0], a[1], 0, n.pos);
        const int32_t r = alloc();
        emit(Op::LoadNil, r, 0, 0, n.pos);
        return r;
      }
      // A block's value is its last child's; every child before it is a
      // statement. An empty block is nil.
      case Tag::Block: {
        if (n.num_children == 0) {
          const int32_t r = alloc();
          emit(Op::LoadNil, r, 0, 0, n.pos);
          return r;
        }
        for (uint32_t i = 0; i + 1 < n.num_children; ++i) {
          compile_stmt(m.child(id, i));
        }
        return compile_value(m.child(id, n.num_children - 1), tail);
      }

      // An If's value is the branch taken's, so both branches have to land in
      // the same register -- which is why this cannot just be two
      // compile_expr calls. A missing else yields nil.
      case Tag::If: {
        auto v = view_if(m, id);
        const int32_t base = top;
        const int32_t c = compile_expr(v.cond);
        top = base;
        const int32_t r = alloc();
        const size_t jf = emit(Op::JumpIfFalse, c, 0, 0, n.pos);
        branch_into(r, v.then_, n.pos, tail);
        const size_t jend = emit(Op::Jump, 0, 0, 0, n.pos);
        patch(jf, here());
        if (v.els.valid()) {
          branch_into(r, v.els, n.pos, tail);
        } else {
          emit(Op::LoadNil, r, 0, 0, n.pos);
        }
        patch(jend, here());
        return r;
      }

      // Same shape as If -- every arm lands its value in one register --
      // fanned out over a Chunk::switch_tables entry instead of a single
      // JumpIfFalse. The table's arm pcs are only known once each arm has
      // been emitted, so it is finished (finish_switch_table) after the
      // loop below rather than built up front the way compile_release_list
      // builds a Scope's.
      case Tag::Switch: {
        const auto sw = view_switch(m, id);
        const int32_t base = top;
        const int32_t s = compile_expr(sw.subject);
        top = base;
        const int32_t r = alloc();
        const int32_t table_idx = static_cast<int32_t>(ch.switch_tables.size());
        ch.switch_tables.emplace_back();
        emit(Op::Switch, s, table_idx, 0, n.pos);
        std::vector<size_t> end_jumps;
        if (!sw.default_body.valid()) {
          emit(Op::LoadNil, r, 0, 0, n.pos);
          end_jumps.push_back(emit(Op::Jump, 0, 0, 0, n.pos));
        }
        std::vector<std::pair<NodeId, int32_t>> arm_keys;
        arm_keys.reserve(sw.arm_count);
        for (uint32_t i = 0; i < sw.arm_count; ++i) {
          arm_keys.emplace_back(switch_key(m, id, i), here());
          branch_into(r, switch_body(m, id, i), n.pos, tail);
          end_jumps.push_back(emit(Op::Jump, 0, 0, 0, n.pos));
        }
        int32_t default_pc = -1;
        if (sw.default_body.valid()) {
          default_pc = here();
          branch_into(r, sw.default_body, n.pos, tail);
          end_jumps.push_back(emit(Op::Jump, 0, 0, 0, n.pos));
        }
        for (size_t j : end_jumps) patch(j, here());
        finish_switch_table(table_idx, arm_keys, default_pc);
        return r;
      }

      case Tag::MakeClosure: {
        auto v = view_make_closure(m, id);
        const int32_t r = alloc();
        emit(Op::MakeClosure, r, v.func, v.capture_map, n.pos);
        return r;
      }
      case Tag::NativeRef: {
        const int32_t r = alloc();
        emit(Op::NativeRef, r, view_native_ref(m, id).index, 0, n.pos);
        return r;
      }
      case Tag::Yield: {
        const int32_t base = top;
        const int32_t s = compile_expr(m.child(id, 0));
        top = base;
        const int32_t r = alloc();
        emit(Op::Yield, r, s, 0, n.pos);
        return r;
      }
      case Tag::ArrayLit: {
        // Items go in one contiguous run, the same arrangement CallValue's
        // arguments use, so the instruction needs only a start and a count.
        const int32_t base = top;
        const int32_t items_at = top;
        for (uint32_t i = 0; i < n.num_children; ++i) {
          compile_expr(m.child(id, i));
        }
        top = base;
        const int32_t r = alloc();
        emit(Op::NewArray, r, items_at, static_cast<int32_t>(n.num_children),
             n.pos);
        return r;
      }
      case Tag::ObjectLit: {
        // Built empty and filled with the same SetIndex a later assignment
        // uses, rather than a second construction path that could disagree
        // with it about duplicate keys or key types.
        const int32_t r = alloc();
        emit(Op::NewObject, r, 0, 0, n.pos);
        for (uint32_t i = 0; i < n.num_children; i += 2) {
          const int32_t base = top;
          const int32_t k = compile_expr(m.child(id, i));
          const int32_t v = compile_expr(m.child(id, i + 1));
          emit(Op::SetIndex, r, k, v, n.pos);
          top = base;
        }
        return r;
      }
      case Tag::Index: {
        const int32_t base = top;
        const int32_t recv = compile_expr(m.child(id, 0));
        const int32_t key = compile_expr(m.child(id, 1));
        top = base;
        const int32_t r = alloc();
        emit(Op::Index, r, recv, key, n.pos);
        return r;
      }
      case Tag::FieldGet: {
        auto v = view_field_get(m, id);
        const int32_t base = top;
        const int32_t recv = compile_expr(v.receiver);
        top = base;
        const int32_t r = alloc();
        emit(Op::FieldGet, r, recv, v.slot, n.pos, v.name_const);
        return r;
      }
      case Tag::CallValue: {
        // The callee and the arguments go into one contiguous run of
        // registers, so the instruction needs only where the run starts and
        // how long it is. `top` is already a stack, so "contiguous" costs
        // nothing to arrange -- just do not reset it between operands.
        const int32_t base = top;
        const int32_t callee = compile_expr(m.child(id, 0));
        const int32_t argc = static_cast<int32_t>(n.num_children) - 1;
        const int32_t args_at = top;
        for (int32_t i = 0; i < argc; ++i) {
          compile_expr(m.child(id, static_cast<uint32_t>(i + 1)));
        }
        top = base;
        const int32_t r = alloc();
        // In tail position, and allowed here, the call may replace the
        // frame (Op::TailCall). Everything after it -- the scope exits and
        // the Ret a Return or the body's end emits -- stays emitted: when
        // the replacement does not happen (a native or generator callee,
        // the entry frame) TailCall is a CallValue and that code runs.
        emit(tail && tail_call_ok() ? Op::TailCall : Op::CallValue, r, callee,
             args_at, n.pos, argc);
        return r;
      }
      default:
        // A statement in value position: verify() rejects this shape before
        // compilation ever runs (main.cc always verifies first), so there is
        // no legitimate value to fabricate here.
        std::abort();
    }
  }

  void compile_stmt(NodeId id) {
    const Node& n = m.at(id);
    const int32_t base = top;
    // A statement's temporaries die with the statement. Resetting `top` alone
    // only tells the compiler the registers are reusable; whatever they hold
    // stays held until something overwrites them or the frame returns, which
    // is not when a source-level scope ended.
    const int32_t outer_high_water = high_water;
    high_water = top;
    switch (n.tag) {
      case Tag::Block:
        for (uint32_t i = 0; i < n.num_children; ++i) {
          compile_stmt(m.child(id, i));
        }
        break;

      case Tag::Assign: {
        auto v = view_assign(m, id);
        const int32_t s = compile_expr(v.value);
        emit(store_op_of(v.kind), v.index, s, 0, n.pos);
        break;
      }

      case Tag::If: {
        auto v = view_if(m, id);
        const int32_t c = compile_expr(v.cond);
        top = base;
        const size_t jf = emit(Op::JumpIfFalse, c, 0, 0, n.pos);
        compile_stmt(v.then_);
        if (v.els.valid()) {
          const size_t jend = emit(Op::Jump, 0, 0, 0, n.pos);
          patch(jf, here());
          compile_stmt(v.els);
          patch(jend, here());
        } else {
          patch(jf, here());
        }
        break;
      }

      case Tag::While: {
        auto v = view_while(m, id);
        const int32_t start = here();
        const int32_t c = compile_expr(v.cond);
        top = base;
        const size_t jf = emit(Op::JumpIfFalse, c, 0, 0, n.pos);
        open_loops.push_back({start, base, open_scopes.size(), {}});
        compile_stmt(v.body);
        emit(Op::Jump, start, 0, 0, n.pos);
        patch(jf, here());
        for (size_t j : open_loops.back().break_jumps) patch(j, here());
        open_loops.pop_back();
        break;
      }

      case Tag::Intrinsic: {
        auto v = view_intrinsic(m, id);
        if (v.id == IntrinsicId::Print || v.id == IntrinsicId::PrintRaw) {
          const int32_t s = compile_expr(m.child(id, 0));
          emit(op_of(v.id), s, 0, 0, n.pos);
        } else {
          compile_expr(id);  // value discarded
        }
        break;
      }

      case Tag::SetIndex: {
        const int32_t recv = compile_expr(m.child(id, 0));
        const int32_t key = compile_expr(m.child(id, 1));
        const int32_t val = compile_expr(m.child(id, 2));
        emit(Op::SetIndex, recv, key, val, n.pos);
        break;
      }

      case Tag::FieldSet: {
        auto v = view_field_set(m, id);
        const int32_t recv = compile_expr(v.receiver);
        const int32_t val = compile_expr(v.value);
        emit(Op::FieldSet, recv, v.slot, val, n.pos, v.name_const);
        break;
      }

      case Tag::Return: {
        // Every open scope is left the way its own exit leaves it --
        // innermost first, its defers and then its locals -- after the
        // return value is computed, exactly as Break leaves the scopes it
        // crosses. The Ret could free the frame in one go, but then every
        // defer would precede every drop, and the locals would die in slot
        // order rather than the scoped, last-declared-first order.
        int32_t r;
        if (n.num_children == 1) {
          r = compile_value(m.child(id, 0), /*tail=*/true);
        } else {
          r = alloc();
          emit(Op::LoadNil, r, 0, 0, n.pos);
        }
        leave_down_to(0, r + 1, n.pos);
        emit(Op::Ret, r, 1, 0, n.pos);
        break;
      }

      case Tag::Throw: {
        const int32_t r = compile_expr(m.child(id, 0));
        emit(Op::Throw, r, 0, 0, n.pos);
        break;
      }

      case Tag::Defer: {
        const int32_t r = compile_expr(m.child(id, 0));
        emit(Op::DeferPush, r, 0, 0, n.pos);
        break;
      }

      case Tag::CellFresh:
        emit(Op::CellNew, view_cellfresh(m, id).cell, 0, 0, n.pos);
        break;

      case Tag::Break:
      case Tag::Continue: {
        // verify(): inside a loop body, and the depth names one that is
        // open. leave_down_to already leaves every scope between here and
        // the target loop, however many loops lie in between, so a labeled
        // exit costs nothing the plain one did not.
        const int32_t depth = n.tag == Tag::Break ? view_break(m, id).depth
                                                  : view_continue(m, id).depth;
        OpenLoop& loop =
            open_loops[open_loops.size() - 1 - static_cast<size_t>(depth)];
        leave_down_to(loop.scope_depth, loop.stmt_base, n.pos);
        if (n.tag == Tag::Continue) {
          emit(Op::Jump, loop.head, 0, 0, n.pos);
        } else {
          loop.break_jumps.push_back(emit(Op::Jump, 0, 0, 0, n.pos));
        }
        break;
      }

      case Tag::Scope:
        compile_scope(id, false);
        break;

      default: {
        // An expression used as a statement: evaluate it and drop the result.
        compile_expr(id);
        break;
      }
    }
    if (high_water > base) emit(Op::ClearRegs, base, high_water, 0, n.pos);
    high_water = std::max(outer_high_water, high_water);
    top = base;
  }
};

}  // namespace detail

inline Program compile(const coreir::Module& m) {
  Program p;
  p.consts = m.consts;
  p.str_consts = m.str_consts;
  p.positions = m.positions;
  p.capture_maps = m.capture_maps;
  p.natives = m.natives;
  p.chunks.resize(m.funcs.size());

  for (size_t i = 0; i < m.funcs.size(); ++i) {
    const coreir::Func& fn = m.funcs[i];
    Chunk& ch = p.chunks[i];
    ch.name = fn.name;
    ch.num_locals = fn.num_locals;
    ch.num_captures = fn.num_captures;
    ch.local_names = fn.local_names;
    ch.capture_names = fn.capture_names;

    ch.num_cells = fn.num_cells;
    ch.num_params = fn.num_params;
    ch.is_generator = fn.is_generator;
    ch.lenient_arity = fn.lenient_arity;
    ch.tail_calls = fn.tail_calls;

    detail::FnCompiler fc{m, ch};
    const uint32_t body_pos = m.at(fn.body).pos;

    // Cells are boxes, and a fresh frame needs fresh ones -- sharing
    // them across activations is exactly the bug that makes a recursive
    // closure see the wrong variable.
    for (int32_t c = 0; c < fn.num_cells; ++c) {
      fc.emit(Op::CellNew, c, 0, 0, body_pos);
    }

    // A function returns its body's value, whatever that is. PL/0's bodies
    // are blocks ending in statements, so they return nil and nothing reads
    // it -- one path rather than a "does this body produce a value" fork that
    // Block, now value-producing, no longer answers usefully anyway. The
    // body is in tail position: a call it ends in may replace the frame.
    const int32_t r = fc.compile_value(fn.body, /*tail=*/true);
    fc.emit(Op::Ret, r, 1, 0, body_pos);
  }
  return p;
}

}  // namespace vm

// ===== vm/exec.cc =====

namespace vm {
namespace detail {

// What a Throw (or a trap the executor raises itself) travels as, from the
// raise to the unwinder in run(). Typed, so a host exception thrown out of a
// coreir_rt_* hook passes through untouched -- only the VM's own failures
// unwind to a script handler.
//
// `fatal_msg` is what coreir_rt_fail gets if no handler catches this: a
// trap keeps its original diagnostic (so an unguarded program fails with
// byte-identical output to the pre-exception executor), while a user Throw
// leaves it empty and is formatted as "uncaught: <value>" at that point --
// not eagerly, since a caught throw never needs it.
struct Raise {
  Value value;
  SrcPos pos;
  std::string fatal_msg;
};

struct FramePool;

// One activation record. A frame is a heap object owned by Exec's stack, not
// a C++ stack frame, so its address is stable for as long as it is live, and
// its ownership can move: into a GeneratorObj at a Yield, into a CoroObj
// with every frame above the coroutine's bottom at a CoroYield, and back.
//
// `regs` and `locals` hold owned references, and nothing in this
// file places a retain or a release to make that work. Value is an RAII
// handle, so an ordinary assignment releases what a register held and retains
// what it now holds, and ~Frame releases the lot. That is also what keeps a
// host throw exception-safe for free: the unwind that destroys Exec's frame
// stack releases every value in every live frame, with no unwind table to
// get wrong.
struct Frame {
  const Chunk* chunk = nullptr;
  size_t pc = 0;
  std::vector<Value> locals;
  std::vector<Value> regs;
  // Cells this frame owns, and cells the closure being run brought with it.
  // Both are Cell values -- shared, refcounted, and not tied to any frame's
  // lifetime, which is what lets a closure be called after the frame that
  // built it has returned.
  std::vector<Value> cells;
  std::vector<Value> captures;
  // The frame's pending defers (owned closure values, LIFO) and the marks
  // its open defer-scopes took: {stack height, the DeferMark's pc}. The pc
  // is what lets the unwinder pair a mark with a Cleanup region, and skip
  // regions whose exit-time run already popped theirs.
  std::vector<Value> defers;
  std::vector<std::pair<size_t, int32_t>> defer_marks;
  // The owned-stack marks of the frame's open scopes: {Runtime::owned_mark
  // at entry, the OwnedMark's pc} -- the same pairing with Cleanup regions
  // as defer_marks.
  std::vector<std::pair<uint64_t, int32_t>> owned_marks;
  int32_t ret_reg = -1;  // where in the caller the result goes
  int32_t argc = 0;      // arguments the call supplied (Op::ArgCount)
  // The program's own frame -- vm::run's first -- as opposed to a job's,
  // which sits at the same stack depth after it: RunOptions::
  // entry_frame_drops is about this one only.
  bool entry = false;
  // Non-nil exactly when this frame is a generator activation: the
  // GeneratorObj it suspends back into. Owning, so the generator cannot be
  // freed out from under its own running frame.
  Value gen_self;
  // The free list this frame goes back to when it dies (null: it is the
  // allocator's). Held here rather than in the deleter so that the deleter
  // stays empty and a FramePtr stays one word -- `frames.back()` is read at
  // the top of every instruction, and a two-word handle doubles what that
  // walk touches.
  FramePool* pool = nullptr;
  // Non-nil exactly when this frame is the bottom of a running coroutine
  // -- the one CoroResume entered. CoroYield walks down to the nearest one
  // to know where the parked slice starts; Ret and the unwinder finish the
  // coroutine when this frame goes. Owning, like gen_self.
  Value coro_self;
};

// A blank frame, stated once: everything it holds released in the order
// ~Frame would have released it -- the two self-references first, then the
// vectors from the last declared to the first -- and every scalar back to
// the value a fresh Frame's own initializer gives it. Recycling a frame
// means running this instead of the destructor, so a drop hook a release
// fires sees the order it saw before there was a pool, and a field added to
// Frame is blanked on a recycled frame the way it is on a fresh one.
// `pool` is the exception: it says where this frame goes next, not what it
// holds.
inline void clear_frame(Frame& f) {
  f.coro_self = Value();
  f.gen_self = Value();
  f.owned_marks.clear();
  f.defer_marks.clear();
  f.defers.clear();
  f.captures.clear();
  f.cells.clear();
  f.regs.clear();
  f.locals.clear();
  f.chunk = nullptr;
  f.pc = 0;
  f.ret_reg = -1;
  f.argc = 0;
  f.entry = false;
}

// The frames a call is not paying the allocator for. A frame is a heap
// object with five vectors in it, so a call that pushed one and a return
// that dropped it were several malloc/free pairs each -- on the one path a
// program takes most often. A returned frame comes back here with its
// vectors emptied but their capacity kept, and the next call of any shape
// takes it and resizes into that capacity.
//
// Bounded: a run that goes a thousand frames deep once should not hold a
// thousand frames' worth of registers for the rest of the run.
struct FramePool {
  static constexpr size_t kMax = 64;
  // Plain unique_ptrs, not FramePtrs: a frame already on the free list must
  // be deleted when the list goes, not handed back to the list it is in.
  std::vector<std::unique_ptr<Frame>> free_list;
};

// Every way a frame dies goes through a unique_ptr's deleter, so this is the
// one place recycling has to be written -- rather than at each pop_back, the
// coroutine park's erase, and the unwinder's two loops. Exec::take_frame is
// the only thing that ever builds a Frame, and it sets `pool`, so there is
// no frame here without one.
struct FrameDeleter {
  void operator()(Frame* f) const {
    if (f->pool->free_list.size() >= FramePool::kMax) {
      delete f;
      return;
    }
    // Emptied before it joins the list, never after: a release here can run
    // a drop hook, which re-enters the executor and may take a frame.
    clear_frame(*f);
    f->pool->free_list.push_back(std::unique_ptr<Frame>(f));
  }
};

using FramePtr = std::unique_ptr<Frame, FrameDeleter>;

struct Exec {
  const Program& p;
  size_t max_frames;
  Runtime& rt;
  bool entry_frame_drops;  // RunOptions::entry_frame_drops

  // Declared before `frames`, so it outlives them: a frame returning to the
  // pool during teardown must find the pool still there.
  FramePool pool;

  // unique_ptr rather than a vector<Frame> or a deque<Frame>: a vector moves
  // its elements as it grows, which would invalidate every `captures` pointer
  // aimed at them, and a deque owns its elements outright, which leaves no
  // way to hand one frame's ownership elsewhere -- what a call that can
  // suspend and be resumed later would need. The indirection buys both.
  std::vector<FramePtr> frames;

  // The job queue (IntrinsicId::Enqueue): closures and coroutines waiting
  // to run after the entry frame, FIFO. C++-side handles, so the collector
  // sees them as roots the way it sees a frame's registers.
  std::deque<Value> jobs;

  // Program::natives, resolved against RunOptions::natives into NativeObj
  // values before the first instruction; Op::NativeRef reads one out.
  std::vector<Value> natives;

  // Where a call made from C++ (NativeCall::call) gets its result: a
  // ret_reg of kSyncRet names this slot instead of a register in the frame
  // below. One slot suffices because such calls nest as a stack -- an
  // inner one has been consumed before the outer one delivers.
  static constexpr int32_t kSyncRet = -2;
  Value sync_result;

  // Every coroutine ever handed to Enqueue, held until it finishes: the
  // scheduler's own reference, so a coroutine that parks and is then
  // forgotten by everything else is still there for check_deadlock to
  // count rather than quietly freed -- Go's rule, that a goroutine is the
  // runtime's until it returns. Keyed by object for the O(1) erase at
  // finish_coro.
  std::unordered_map<const CoroObj*, Value> scheduled;

  // The one place a coroutine becomes Done: from its bottom frame's Ret or
  // an unwind past it, from CoroClose, or from a first resume whose
  // function completed on the spot. Drops the scheduler's reference if it
  // held one. `co` stays alive through the call -- every caller holds it
  // in a frame's coro_self or a local Value.
  void finish_coro(CoroObj* co) {
    co->state = CoroObj::State::Done;
    if (co->scheduled) scheduled.erase(co);
  }

  // A blank frame: the pool's, if it has one -- clear_frame blanked it on
  // the way in, and its vectors' capacity is still there for a caller's
  // resize to land in -- and a fresh one otherwise. The only place a Frame
  // is ever built, which is what lets the deleter trust `pool`.
  FramePtr take_frame() {
    if (!pool.free_list.empty()) {
      FramePtr f(pool.free_list.back().release());
      pool.free_list.pop_back();
      return f;
    }
    FramePtr fresh(new Frame());
    fresh->pool = &pool;
    return fresh;
  }

  FramePtr make_frame(const Chunk& ch) {
    FramePtr f = take_frame();
    f->chunk = &ch;
    f->locals.assign(static_cast<size_t>(ch.num_locals), Value::uninit());
    f->regs.resize(static_cast<size_t>(ch.num_regs));
    f->cells.resize(static_cast<size_t>(ch.num_cells));
    return f;
  }

  // A trap: the executor's own failure, catchable like any Throw. The
  // value a handler sees is an object {message, line, col} -- built here,
  // once, rather than each front end inventing its own materialization.
  [[noreturn]] void raise_trap(const std::string& msg, SrcPos pos) {
    Value e = Value::make_object();
    e.as_object()->set("message", Value::make_str(msg));
    e.as_object()->set("line", Value::make_int(pos.line));
    e.as_object()->set("col", Value::make_int(pos.col));
    throw Raise{std::move(e), pos, msg};
  }

  // Shared by both call forms: the depth bound and the interrupt point.
  void check_can_push(SrcPos pos) {
    if (frames.size() > max_frames) {
      raise_trap("recursion limit exceeded", pos);
    }
    coreir_rt_poll();
  }

  // The {value, done} object both generator intrinsics answer with. The
  // property count is known, so the object is sized once rather than grown
  // twice -- this runs on every step of every generator loop.
  Value gen_result(Value v, bool done) {
    Value o = Value::make_object();
    ObjectObj* obj = o.as_object();
    obj->props.reserve(2);
    obj->set("value", std::move(v));
    obj->set("done", Value::make_bool(done));
    return o;
  }

  // What GenResume, GenReturn and GenThrow all establish before they
  // diverge: the operand is a generator, and it is not the activation
  // already running. The verb naming the attempt is the only difference.
  GeneratorObj* gen_operand(Frame& f, const Value& gv, const char* verb) {
    if (!gv.is_generator()) {
      fail(f, std::string("cannot ") + verb + " " + type_name(gv.tag()));
    }
    GeneratorObj* go = gv.as_generator();
    if (go->state == GeneratorObj::State::Running) {
      fail(f, "generator already running");
    }
    return go;
  }

  // Move a frame's storage into a generator's keeping (suspend)...
  //
  // Swapped rather than move-assigned, here and in restore_frame: a move
  // frees whatever the destination held, and between the two of them that
  // is the pool's own capacity on the way in and the generator's on the way
  // out -- two frees per vector per suspend/resume pair, for buffers both
  // sides are about to want again. A swap leaves each side holding the
  // other's, which is exactly the circulation the pool exists for. The
  // frame is dead either way: park_frame is only ever called on one about
  // to be popped.
  void park_frame(GenFrame& gf, Frame& f) {
    gf.locals.swap(f.locals);
    gf.regs.swap(f.regs);
    gf.cells.swap(f.cells);
    gf.captures.swap(f.captures);
    gf.defers.swap(f.defers);
    gf.defer_marks.swap(f.defer_marks);
    gf.owned_marks.swap(f.owned_marks);
  }

  // ...and back into a fresh executor frame, storage and all. The
  // GenFrame's own ret_reg and gen_self come along; a caller that has its
  // own (a generator's resume site) overwrites them.
  FramePtr restore_frame(GenFrame& gf) {
    FramePtr f = take_frame();
    f->chunk = &p.chunks[static_cast<size_t>(gf.func)];
    f->pc = static_cast<size_t>(gf.pc);
    f->locals.swap(gf.locals);
    f->regs.swap(gf.regs);
    f->cells.swap(gf.cells);
    f->captures.swap(gf.captures);
    f->defers.swap(gf.defers);
    f->defer_marks.swap(gf.defer_marks);
    f->owned_marks.swap(gf.owned_marks);
    f->ret_reg = gf.ret_reg;
    f->argc = gf.argc;
    f->gen_self = std::move(gf.gen_self);
    return f;
  }

  // A generator's frame, back onto the executor's stack (resume). The new
  // frame owns the generator for as long as it runs.
  FramePtr unpark_frame(const Value& gv, int32_t ret_reg) {
    GeneratorObj* go = gv.as_generator();
    auto f = restore_frame(go->frame);
    f->ret_reg = ret_reg;
    f->gen_self = gv;
    go->state = GeneratorObj::State::Running;
    return f;
  }

  // Coroutines. The innermost frame carrying coro_self, searching down
  // from the top -- the bottom of the running coroutine a CoroYield here
  // would suspend; frames.size() when no frame does.
  size_t coro_bottom() const {
    for (size_t i = frames.size(); i-- > 0;) {
      if (frames[i]->coro_self.is_coroutine()) return i;
    }
    return frames.size();
  }

  CoroObj* coro_operand(Frame& f, const Value& cv, const char* verb) {
    if (!cv.is_coroutine()) {
      fail(f, std::string("cannot ") + verb + " " + type_name(cv.tag()));
    }
    return cv.as_coroutine();
  }

  // Suspend: frames [bottom, top] move into the coroutine, bottom first,
  // each with its own pc (every frame below the top is already advanced
  // past its call; the top's pc is the caller's to set), its ret_reg into
  // the frame below it, and -- for the top -- the register the next
  // resume's value lands in. Then the frames go: their storage is gone,
  // and what a Frame still holds (the bottom's coro_self) goes with them.
  void park_coro(CoroObj* co, size_t bottom, int32_t yield_reg) {
    co->frames.clear();
    co->frames.reserve(frames.size() - bottom);
    for (size_t i = bottom; i < frames.size(); ++i) {
      Frame& fr = *frames[i];
      GenFrame gf;
      gf.func = static_cast<int32_t>(fr.chunk - p.chunks.data());
      gf.pc = static_cast<int64_t>(fr.pc);
      gf.yield_reg = i + 1 == frames.size() ? yield_reg : -1;
      gf.argc = fr.argc;
      gf.ret_reg = fr.ret_reg;
      park_frame(gf, fr);
      gf.gen_self = std::move(fr.gen_self);
      co->frames.push_back(std::move(gf));
    }
    frames.erase(frames.begin() + static_cast<std::ptrdiff_t>(bottom),
                 frames.end());
    co->state = CoroObj::State::Suspended;
  }

  // Resume: the parked frames back onto the stack in order. The bottom
  // frame's result now goes to this resume's register (`ret_reg`) and it
  // carries the coroutine again; the top frame receives `sent` where its
  // CoroYield's result was to land. A null `sent` (CoroClose) delivers
  // nothing.
  void unpark_coro(const Value& cv, int32_t ret_reg, const Value* sent) {
    CoroObj* co = cv.as_coroutine();
    const size_t n = co->frames.size();
    for (size_t i = 0; i < n; ++i) {
      GenFrame& gf = co->frames[i];
      const int32_t yield_reg = gf.yield_reg;
      auto nf = restore_frame(gf);
      if (i == 0) {
        nf->ret_reg = ret_reg;
        nf->coro_self = cv;
      }
      if (i + 1 == n && sent && yield_reg >= 0) {
        nf->regs[static_cast<size_t>(yield_reg)] = *sent;
      }
      frames.push_back(std::move(nf));
    }
    co->frames.clear();
    co->state = CoroObj::State::Running;
  }

  // A Start-state coroutine's first resume, wherever it comes from -- an
  // Op::CoroResume, or the scheduler taking it off the queue: call f(sent),
  // and make the frame that call entered the coroutine's bottom. Answers
  // whether there is now a frame to drive; a callee that completes on the
  // spot (a native, or a generator function, whose call builds an object
  // rather than running a body) leaves none, and the coroutine is done at
  // once with whatever went to ret_reg. A trap at the call itself (arity)
  // finishes it too, and propagates.
  bool start_coro(const Value& cv, const Value& sent, int32_t ret_reg,
                  SrcPos pos) {
    CoroObj* co = cv.as_coroutine();
    Value fn = std::move(co->fn);
    co->state = CoroObj::State::Running;
    const size_t before = frames.size();
    try {
      push_closure(fn, &sent, 1, ret_reg, pos);
    } catch (Raise&) {
      finish_coro(co);
      throw;
    }
    if (frames.size() == before) {
      finish_coro(co);
      return false;
    }
    frames.back()->coro_self = cv;
    return true;
  }

  // The arity contract, stated once: a mismatch traps unless the chunk is
  // lenient, and the params window then takes what it can -- extras stay
  // with the caller, and a param nothing arrived for is filled with nil
  // rather than left Uninit, so the body can test it without tripping the
  // read-before-init check. Both ways into an activation ask this:
  // push_closure for a fresh frame, Op::TailCall for the one it reuses.
  int32_t check_arity(const Chunk& ch, int32_t argc, SrcPos pos) {
    if (argc != ch.num_params && !ch.lenient_arity) {
      raise_trap(ch.name + " takes " + std::to_string(ch.num_params) +
                     " argument(s), given " + std::to_string(argc),
                 pos);
    }
    return std::min(argc, ch.num_params);
  }

  // Calling a closure into a fresh frame. Op::TailCall is the other way
  // into an activation -- it reuses this frame rather than pushing one --
  // and the two share check_arity above. The callee gets the closure's
  // cells, which are shared rather than pointed at, so nothing here depends
  // on the caller still being alive.
  void push_closure(const Value& callee, const Value* args, int32_t argc,
                    int32_t ret_reg, SrcPos pos) {
    if (callee.is_native()) {
      call_native(callee.as_native(), args, argc, ret_reg, pos);
      return;
    }
    if (!callee.is_func()) {
      raise_trap(std::string("cannot call ") + type_name(callee.tag()), pos);
    }
    const ClosureObj* c = callee.as_closure();
    const Chunk& ch = p.chunks[static_cast<size_t>(c->func)];
    const int32_t taken = check_arity(ch, argc, pos);
    // Calling a generator function runs none of it: the arguments and
    // captures are packaged into a Start-state activation and that is the
    // call's value. No frame, so no depth check.
    if (ch.is_generator) {
      Value g = Value::make_generator();
      GenFrame& gf = g.as_generator()->frame;
      gf.func = c->func;
      gf.argc = argc;
      gf.locals.assign(static_cast<size_t>(ch.num_locals), Value::uninit());
      for (int32_t i = 0; i < taken; ++i) {
        gf.locals[static_cast<size_t>(i)] = args[i];
      }
      for (int32_t i = taken; i < ch.num_params; ++i) {
        gf.locals[static_cast<size_t>(i)] = Value();
      }
      gf.regs.resize(static_cast<size_t>(ch.num_regs));
      gf.cells.resize(static_cast<size_t>(ch.num_cells));
      gf.captures = c->cells;
      deliver(ret_reg, std::move(g));
      return;
    }
    check_can_push(pos);
    FramePtr f = make_frame(ch);
    f->captures = c->cells;  // shared, not copied: each element is a Cell
    for (int32_t i = 0; i < taken; ++i) {
      f->locals[static_cast<size_t>(i)] = args[i];
    }
    for (int32_t i = taken; i < ch.num_params; ++i) {
      f->locals[static_cast<size_t>(i)] = Value();
    }
    f->argc = argc;
    f->ret_reg = ret_reg;
    frames.push_back(std::move(f));
  }

  // Where an instruction came from. Two side tables deep -- a chunk's
  // code_pos indexes the program's shared position pool -- so the walk is
  // written once here rather than at each of the arms that report a trap.
  SrcPos pos_at(const Chunk& ch, size_t pc) const {
    return p.positions[ch.code_pos[pc]];
  }

  [[noreturn]] void fail(const Frame& f, const std::string& msg) {
    raise_trap(msg, pos_at(*f.chunk, f.pc));
  }

  // The two checks a field access owes before it may index props directly,
  // and the two traps they produce. FieldGet and FieldSet differ only in
  // which operand holds the receiver and which the slot, never in what
  // makes an access invalid, so the checking is written once here and the
  // verb ("get"/"set") is all the caller supplies.
  ObjectObj* field_target(const Frame& f, const Value& recv, int32_t slot,
                          int32_t name_const, const char* verb) {
    if (!recv.is_object()) {
      fail(f, std::string("cannot ") + verb + " field '" +
                  p.str_const_at(name_const) + "' of " +
                  type_name(recv.tag()));
    }
    ObjectObj* o = recv.as_object();
    if (slot < 0 || static_cast<size_t>(slot) >= o->props.size()) {
      fail(f, std::string("field '") + p.str_const_at(name_const) +
                  "' out of range");
    }
    return o;
  }

  // Scalars cost nothing to rebuild; a string literal allocates on every
  // load, which the real version will not want. culebra's answer is that
  // constants are immortal and LoadConst does not refcount them at all
  // (vm.md 5.2). Deferred on purpose: an immortal object is a second lifetime
  // rule, and the whole point of the RAII Value is that there is one.
  Value const_value(int32_t index) const {
    const Const& c = p.consts[static_cast<size_t>(index)];
    switch (c.kind) {
      case ConstKind::Nil:
        return Value();
      case ConstKind::Bool:
        return Value::make_bool(c.bits != 0);
      case ConstKind::Int:
        return Value::make_int(c.bits);
      case ConstKind::Double: {
        double d;
        std::memcpy(&d, &c.bits, sizeof(double));
        return Value::make_double(d);
      }
      case ConstKind::Str:
        return Value::make_str(p.str_consts[static_cast<size_t>(c.bits)]);
    }
    return Value();
  }

  // A local read's one check, which the other two storage classes do not
  // have: a local starts out Uninit and a cell starts out nil, so "read
  // before assigned" is observable through the first and not the second --
  // which is right, since a cell is created by the frame rather than by the
  // source-level declaration a diagnostic would name.
  Value& local_ref(Frame& f, int32_t index, const Chunk& ch) {
    Value& v = f.locals[static_cast<size_t>(index)];
    if (v.is_uninit()) {
      fail(f, format_uninit_var(ch.local_names[static_cast<size_t>(index)]));
    }
    return v;
  }

  // The cells a MakeClosure hands to the closure it builds, resolved in the
  // frame doing the building. A Local is rejected by verify() -- it would die
  // with this frame -- so only these two cases exist.
  Value capture_cell(Frame& f, const CaptureSrc& src) {
    return src.from == VarKind::Cell ? f.cells[src.index]
                                     : f.captures[src.index];
  }

  // The entry frame to its end, then the job queue: a closure job is a
  // fresh 0-argument call, a coroutine job a resume (schedule_coroutine),
  // each driven until it finishes -- or, for a coroutine, until it yields
  // -- before the next is taken, so the jobs it enqueues run after every
  // one already waiting. A job whose call itself traps (a parameter it
  // cannot be given) fails the run the way an uncaught throw does; a queue
  // that runs dry with coroutines still parked is check_deadlock's.
  void run() {
    drive();
    while (!jobs.empty()) {
      Value job = std::move(jobs.front());
      jobs.pop_front();
      try {
        if (job.is_coroutine()) {
          if (!schedule_coroutine(job)) continue;
        } else {
          push_closure(job, nullptr, 0, -1, SrcPos{0, 0});
        }
      } catch (Raise& r) {
        report_uncaught(r);
        return;
      }
      drive();
    }
    check_deadlock();
  }

  // A coroutine's turn from the scheduler: its first resume calls f(nil),
  // a later one re-enters its CoroYield with nil, and its result goes
  // nowhere (ret_reg -1). Answers whether there is now something on the
  // stack to drive -- a coroutine already done (woken twice, say), or one
  // whose function completed on the spot, leaves nothing.
  bool schedule_coroutine(const Value& cv) {
    CoroObj* co = cv.as_coroutine();
    using St = CoroObj::State;
    if (co->state == St::Done || co->state == St::Running) return false;
    const Value nil;
    if (co->state == St::Start) return start_coro(cv, nil, -1, SrcPos{0, 0});
    if (co->frames.size() > max_frames) {
      raise_trap("recursion limit exceeded", SrcPos{0, 0});
    }
    unpark_coro(cv, -1, &nil);
    return true;
  }

  // The queue is empty and nothing is running: a scheduled coroutine
  // still suspended now has nothing left that could enqueue it.
  void check_deadlock() {
    int64_t parked = 0;
    for (const auto& [co, v] : scheduled) {
      if (co->state == CoroObj::State::Suspended) ++parked;
    }
    if (parked > 0) {
      coreir_rt::fail("deadlock: " + std::to_string(parked) +
                          " coroutine(s) parked with nothing to wake them",
                      0, 0);
    }
  }

  // Drive the frame stack to empty; an uncaught throw is the run's failure.
  void drive() {
    while (!frames.empty()) {
      try {
        dispatch(0);
        return;
      } catch (Raise& r) {
        if (!unwind(r, 0)) {
          report_uncaught(r);
          return;
        }
        // A handler took the value; dispatch resumes at its pc.
      }
    }
  }

  void report_uncaught(const Raise& r) {
    const std::string msg = r.fatal_msg.empty()
                                ? "uncaught: " + to_display(r.value)
                                : r.fatal_msg;
    coreir_rt::fail(msg, r.pos.line, r.pos.col);
  }

  // Run one frame's pending defers back to `mark`, LIFO, each as a normal
  // 0-arity call driven to completion by a nested, floor-bounded dispatch.
  // The nesting recurses through the host stack once per defer *run* (not
  // per call -- calls inside the defer stay flat), so only pathological
  // defers-spawning-defers chains deepen it.
  //
  // A defer whose own throw is not handled within its frames aborts the
  // run: the remaining defers of the same mark are dropped unrun (their
  // values released), and the throw replaces whatever was unwinding --
  // culebra's rule, minus its quirk of skipping the aborting frame's own
  // remaining handlers.
  void run_defers_now(Frame& f, size_t mark, SrcPos pos) {
    while (f.defers.size() > mark) {
      Value d = std::move(f.defers.back());
      f.defers.pop_back();
      const size_t floor = frames.size();
      try {
        push_closure(d, nullptr, 0, -1, pos);
        run_nested(floor);
      } catch (Raise&) {
        while (f.defers.size() > mark) f.defers.pop_back();
        throw;
      }
    }
  }

  // Every defer mark a frame still holds, innermost first -- what closing a
  // suspended activation owes before its frame goes, as opposed to
  // returning from one, where each scope's own exit already ran its share.
  // A defer that throws propagates to the caller; the marks already run
  // stay run.
  void run_pending_defers(Frame& f, SrcPos pos) {
    while (!f.defer_marks.empty()) {
      const size_t mark = f.defer_marks.back().first;
      f.defer_marks.pop_back();
      run_defers_now(f, mark, pos);
    }
  }

  // A scope's release, the two forms: its local range last-slot-first, or
  // the list it spelled out in list order. A released cell is replaced by
  // a fresh one -- what CellFresh does -- so the slot always holds a cell
  // and a closure that captured the old one keeps it, value and all.
  void release_range(Frame& f, int32_t first, int32_t end) {
    for (int32_t i = end; i-- > first;) f.locals[i] = Value::uninit();
  }
  void release_list(Frame& f, int32_t list) {
    for (const SlotRef& s : f.chunk->release_lists[static_cast<size_t>(list)]) {
      if (s.kind == VarKind::Local) {
        f.locals[s.index] = Value::uninit();
      } else {
        f.cells[s.index] = Value::make_cell();
      }
    }
  }

  // A Scope's exit, after its locals are gone: pop the mark it took and
  // resolve the owned stack above it (Runtime::owned_scope_exit). The entry
  // frame's outermost scope is the program's end, and under
  // entry_frame_drops == false resolves nothing: a cycle discarded at the
  // top level is the collector's, and at exit not even that -- the rule
  // pop_frame applies to the frame's own values.
  void leave_scope_owned(Frame& f) {
    if (f.owned_marks.empty()) return;  // every ClearLocals has its OwnedMark
    const uint64_t mark = f.owned_marks.back().first;
    f.owned_marks.pop_back();
    if (!entry_frame_drops && f.entry && f.owned_marks.empty()) return;
    rt.owned_scope_exit(mark);
  }

  // Drive whatever was just pushed to completion, unwinding within it.
  void run_nested(size_t floor) {
    while (frames.size() > floor) {
      try {
        dispatch(floor);
      } catch (Raise& r) {
        if (!unwind(r, floor)) throw;
      }
    }
  }

  // The drop-contract hook (see Runtime::set_drop_fn): run the object's
  // "\x01" "drop" closure with the object itself as the argument. A throwing
  // destructor is reported on stderr as "drop: <value>" (a trap: its own
  // message) and swallowed -- an object going away must not fail the
  // program that let go of it.
  static void drop_hook(void* ctx, HeapObj* h) {
    auto* self = static_cast<Exec*>(ctx);
    auto* o = static_cast<ObjectObj*>(h);
    Value* dv = o->find(kDropKey);
    if (!dv || !dv->is_callable()) return;
    Value closure = *dv;
    Value arg = Value::make_ref(h);
    const size_t floor = self->frames.size();
    try {
      self->push_closure(closure, &arg, 1, -1, SrcPos{0, 0});
      self->run_nested(floor);
    } catch (Raise& r) {
      const std::string what =
          r.fatal_msg.empty() ? to_display(r.value) : r.fatal_msg;
      std::fprintf(stderr, "drop: %s\n", what.c_str());
      while (self->frames.size() > floor) self->frames.pop_back();
    }
  }

  // Walks frames top-down, and within each the cleanup regions holding its
  // pc innermost-out (the vector's own order -- children were recorded
  // first). Every region crossed is left the way its own exit would leave
  // it: temps above its register floor dropped, its scope locals back to
  // Uninit. A region with a handler ends the walk: the carried value lands
  // in the caught slot and the frame resumes there. Frames without one are
  // popped, their values released by ~Frame.
  bool unwind(Raise& r, size_t floor) {
    while (frames.size() > floor) {
      Frame& f = *frames.back();
      for (const Cleanup& cl : f.chunk->cleanups) {
        if (f.pc < static_cast<size_t>(cl.start_pc) ||
            f.pc >= static_cast<size_t>(cl.end_pc)) {
          continue;
        }
        // The region's pending defers run first (culebra's order: defers,
        // then the scope's own releases), and only while its mark is still
        // outstanding -- a throw out of the region's exit-time DeferRunTo
        // arrives here with the mark already popped, and must not run them
        // twice. A defer throwing *here* replaces the in-flight value and
        // the walk continues with it: the rest of this region still tears
        // down, and the enclosing regions' handlers stay eligible.
        if (cl.defer_mark_pc >= 0 && !f.defer_marks.empty() &&
            f.defer_marks.back().second == cl.defer_mark_pc) {
          const size_t mark = f.defer_marks.back().first;
          f.defer_marks.pop_back();
          try {
            run_defers_now(f, mark, r.pos);
          } catch (Raise& replacement) {
            r = std::move(replacement);
          }
        }
        for (size_t i = static_cast<size_t>(cl.regs_base); i < f.regs.size();
             ++i) {
          f.regs[i] = Value();
        }
        if (cl.release_list >= 0) {
          release_list(f, cl.release_list);
        } else {
          release_range(f, cl.first_local, cl.end_local);
        }
        if (cl.owned_mark_pc >= 0 && !f.owned_marks.empty() &&
            f.owned_marks.back().second == cl.owned_mark_pc) {
          leave_scope_owned(f);
        }
        if (cl.handler_pc >= 0) {
          f.locals[cl.caught_local] = r.value;
          f.pc = static_cast<size_t>(cl.handler_pc);
          return true;
        }
      }
      // A generator frame unwound past is finished for good: the throw
      // reaches whoever resumed it, and every later resume answers done.
      // A coroutine's bottom frame, the same: the frame below it is the
      // resumer's, at its CoroResume, whose own handlers get the throw
      // next.
      if (f.gen_self.is_generator()) {
        f.gen_self.as_generator()->state = GeneratorObj::State::Done;
      }
      if (f.coro_self.is_coroutine()) finish_coro(f.coro_self.as_coroutine());
      pop_frame();
    }
    return false;
  }

  // Releases the top frame: its locals last-declared-first -- the order a
  // scope's own exit uses, so a function's unscoped locals (its parameters,
  // say) go the same way -- and the rest with the Frame. Popping the entry
  // frame is the program's end, and under RunOptions::entry_frame_drops ==
  // false its values go with the drop hook disarmed; the jobs that may run
  // after it get the hook back.
  void pop_frame() {
    Frame& f = *frames.back();
    const bool bare = f.entry && !entry_frame_drops;
    if (bare) rt.set_drop_fn(nullptr, nullptr);
    release_range(f, 0, static_cast<int32_t>(f.locals.size()));
    frames.pop_back();
    if (bare) rt.set_drop_fn(this, &Exec::drop_hook);
  }

  // Hands a popped frame's result to whoever asked for it. Ret and Yield
  // both end this way, and both can pop the last frame there is -- a call
  // that wanted no result (ret_reg -1) and an empty stack are the two cases
  // that mean "nobody is listening".
  void deliver(int32_t ret_reg, Value result) {
    if (ret_reg == kSyncRet) {
      sync_result = std::move(result);
    } else if (ret_reg >= 0 && !frames.empty()) {
      frames.back()->regs[ret_reg] = std::move(result);
    }
  }

  // A call made from C++ rather than from an instruction: push the callee,
  // drive it to completion inside a nested dispatch, answer what it
  // returned. What NativeCall::call is; also usable by a host that wants to
  // call a program's closure directly.
  Value call_sync(const Value& callee, const Value* args, int32_t argc,
                  SrcPos pos) {
    const size_t floor = frames.size();
    push_closure(callee, args, argc, kSyncRet, pos);
    if (frames.size() > floor) run_nested(floor);
    return std::move(sync_result);
  }

  // A native's turn: arity-checked like a closure, run to completion on
  // the spot (no frame -- it is C++), its answer delivered where a Ret's
  // would go. `false` from the function raises `error` at the call site,
  // as a Throw there would; the arguments stay borrowed from the caller's
  // registers for the duration, which the caller's frame outlives.
  void call_native(const NativeObj* n, const Value* args, int32_t argc,
                   int32_t ret_reg, SrcPos pos) {
    if (n->arity >= 0 && argc != n->arity) {
      raise_trap(n->name + " takes " + std::to_string(n->arity) +
                     " argument(s), given " + std::to_string(argc),
                 pos);
    }
    coreir_rt_poll();
    NativeCall call;
    call.args = args;
    call.argc = argc;
    call.ctx = n->ctx;
    call.pos = pos;
    call.exec = this;
    if (!n->fn(call)) throw Raise{std::move(call.error), pos, {}};
    deliver(ret_reg, std::move(call.result));
  }

  void dispatch(size_t floor) {
    while (true) {
      Frame& f = *frames.back();
      const Chunk& ch = *f.chunk;

      // Every chunk the compiler emits ends in Ret; running off the end is
      // the same belt and braces the old loop condition was.
      if (f.pc >= ch.code.size()) {
        frames.pop_back();
        if (frames.size() <= floor) return;
        continue;
      }

      const Insn& in = ch.code[f.pc];
      switch (in.op) {
        case Op::LoadConst:
          f.regs[in.a] = const_value(in.b);
          break;
        case Op::Neg:
        case Op::BitNot:
        case Op::WrapI8: case Op::WrapI16: case Op::WrapI32:
        case Op::WrapU8: case Op::WrapU16: case Op::WrapU32: {
          const UnOp uop = unop_of(in.op);
          const Value& v = f.regs[in.b];
          if (auto err = unop_error(uop, v); !err.empty()) fail(f, err);
          f.regs[in.a] = apply_unop(uop, v);
          break;
        }
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
        case Op::Eq:  case Op::Ne:  case Op::Lt:  case Op::Le:
        case Op::Gt:  case Op::Ge:
        case Op::BitAnd: case Op::BitOr: case Op::BitXor:
        case Op::Shl: case Op::Shr:
        case Op::UDiv: case Op::UMod: case Op::UShr:
        case Op::ULt: case Op::ULe: case Op::UGt: case Op::UGe: {
          // Straight into the destination register: eval_binop computes
          // the whole result before it assigns, so the compiler reusing an
          // operand's register as the destination -- which it does freely --
          // needs no temporary here. A failure leaves the register alone.
          if (auto err = eval_binop(binop_of(in.op), f.regs[in.b], f.regs[in.c],
                                    f.regs[in.a]);
              !err.empty()) {
            fail(f, err);
          }
          break;
        }
        case Op::LoadLocal:
          f.regs[in.a] = local_ref(f, in.b, ch);
          break;
        case Op::LoadCell:
          f.regs[in.a] = f.cells[in.b].as_cell()->v;
          break;
        case Op::LoadCapture:
          f.regs[in.a] = f.captures[in.b].as_cell()->v;
          break;
        case Op::StoreLocal:
          f.locals[in.a] = f.regs[in.b];
          break;
        case Op::StoreCell:
          f.cells[in.a].as_cell()->v = f.regs[in.b];
          break;
        case Op::StoreCapture:
          f.captures[in.a].as_cell()->v = f.regs[in.b];
          break;
        case Op::Jump:
          // A backward (or self) jump is a loop iteration -- the one place a
          // program can spin without ever calling or producing output, so
          // it is also the one place a host needs to interrupt one.
          if (static_cast<size_t>(in.a) <= f.pc) coreir_rt_poll();
          f.pc = static_cast<size_t>(in.a);
          continue;
        case Op::JumpIfFalse:
          if (!f.regs[in.a].truthy()) {
            f.pc = static_cast<size_t>(in.b);
            continue;
          }
          break;
        case Op::Switch: {
          const SwitchTable& t = ch.switch_tables[static_cast<size_t>(in.b)];
          int32_t target = -1;
          if (t.has_keys()) {
            const Value& subj = f.regs[in.a];
            if (t.key_kind == coreir::ConstKind::Int) {
              if (!subj.is_int()) {
                fail(f, std::string("switch subject is ") +
                            type_name(subj.tag()) + ", not int");
              }
              const int64_t key = subj.as_int();
              if (t.dense) {
                // Unsigned, so a base far from the subject cannot overflow
                // the subtraction the way a signed one would, and so the
                // one wrapped comparison below stands in for both halves
                // of an off >= 0 / off < size pair: a subject below base,
                // or past the table's end, wraps to a huge value either
                // way and fails the same bounds check.
                const uint64_t off = static_cast<uint64_t>(key) -
                                     static_cast<uint64_t>(t.base);
                if (off < t.pcs.size()) {
                  target = t.pcs[static_cast<size_t>(off)];
                }
              } else {
                target = sparse_arm_pc(t.int_keys, t.arm_pcs, key);
              }
            } else {
              if (!subj.is_str()) {
                fail(f, std::string("switch subject is ") +
                            type_name(subj.tag()) + ", not str");
              }
              target = sparse_arm_pc(t.str_keys, t.arm_pcs, subj.as_str());
            }
          }
          if (target < 0) target = t.default_pc;
          if (target >= 0) {
            f.pc = static_cast<size_t>(target);
            continue;
          }
          break;
        }
        case Op::OutRaw: {
          const std::string s = to_display(f.regs[in.a]);
          coreir_rt_out_raw(s.data(), static_cast<int64_t>(s.size()));
          break;
        }
        case Op::Out: {
          const Value& v = f.regs[in.a];
          // An Int keeps the dedicated integer entry point: it is the one
          // shape the host contract had before values were tagged, and PL/0
          // still goes through it byte for byte.
          if (v.is_int()) {
            coreir_rt_out(v.as_int());
          } else if (v.is_str()) {
            coreir_rt_out_str(v.as_str().data(),
                              static_cast<int64_t>(v.as_str().size()));
          } else {
            const std::string s = to_display(v);
            coreir_rt_out_str(s.data(), static_cast<int64_t>(s.size()));
          }
          break;
        }
        case Op::In: {
          const SrcPos sp = pos_at(ch, f.pc);
          f.regs[in.a] = Value::make_int(coreir_rt_in(sp.line, sp.col));
          break;
        }
        case Op::ArgCount:
          f.regs[in.a] = Value::make_int(f.argc);
          break;
        case Op::Same: {
          const Value& l = f.regs[in.b];
          const Value& r = f.regs[in.c];
          f.regs[in.a] = Value::make_bool(l.tag() == r.tag() &&
                                          l.raw_data() == r.raw_data());
          break;
        }
        case Op::FnArity: {
          const Value& v = f.regs[in.b];
          if (v.is_native()) {
            // The registered count; -1 for a native that takes any.
            f.regs[in.a] = Value::make_int(v.as_native()->arity);
            break;
          }
          if (!v.is_func()) {
            fail(f, std::string("cannot take the arity of ") +
                        type_name(v.tag()));
          }
          const size_t fi = static_cast<size_t>(v.as_closure()->func);
          f.regs[in.a] = Value::make_int(p.chunks[fi].num_params);
          break;
        }
        case Op::NativeRef:
          f.regs[in.a] = natives[static_cast<size_t>(in.b)];
          break;
        case Op::Collect:
          // Safe mid-instruction for the same reason a stress collect at
          // every allocation is: every register and local is a C++ handle,
          // and a handle is a root.
          f.regs[in.a] = Value::make_int(Runtime::current()->collect());
          break;
        case Op::HeapStats: {
          const Runtime& rt = *Runtime::current();
          const int64_t live = rt.live_objects();
          const int64_t bytes = rt.heap_bytes();
          Value o = Value::make_object();
          o.as_object()->set("live_objects", Value::make_int(live));
          o.as_object()->set("heap_bytes", Value::make_int(bytes));
          f.regs[in.a] = std::move(o);
          break;
        }
        case Op::LoadNil:
          f.regs[in.a] = Value();
          break;
        case Op::Move:
          f.regs[in.a] = f.regs[in.b];
          break;
        case Op::ClearRegs:
          // Only the ones holding something. A statement's dead registers
          // are scratch the next statement writes before it reads, so what
          // this is for is dropping the *references* they still hold --
          // an int or a bool left behind pins nothing and is not a root the
          // collector can trip over. Clearing those too was a Value
          // assignment, refcount branch and all, per register per
          // statement, and statements are 15-25% of everything executed.
          for (int32_t i = in.a; i < in.b; ++i) {
            if (f.regs[i].is_heap()) f.regs[i] = Value();
          }
          break;
        case Op::ClearLocals:
          // Last declared, first released: a value whose release runs a
          // destructor sees the ones declared after it already gone, the
          // order a language with scoped destructors promises. Then what
          // the releases could not free -- the scope's cycles.
          release_range(f, in.a, in.b);
          leave_scope_owned(f);
          break;
        case Op::ReleaseSlots:
          release_list(f, in.a);
          leave_scope_owned(f);
          break;
        case Op::OwnedMark:
          f.owned_marks.push_back({rt.owned_mark(), static_cast<int32_t>(f.pc)});
          break;
        case Op::NewArray: {
          std::vector<Value> items;
          items.reserve(static_cast<size_t>(in.c));
          for (int32_t i = 0; i < in.c; ++i) items.push_back(f.regs[in.b + i]);
          f.regs[in.a] = Value::make_array(std::move(items));
          break;
        }
        case Op::NewObject:
          f.regs[in.a] = Value::make_object();
          break;
        case Op::Index: {
          const Value& recv = f.regs[in.b];
          const Value& key = f.regs[in.c];
          if (auto err = index_error(recv, key); !err.empty()) fail(f, err);
          f.regs[in.a] = index_get(recv, key);
          break;
        }
        case Op::SetIndex: {
          const Value& recv = f.regs[in.a];
          const Value& key = f.regs[in.b];
          // Strings read through Index but do not write: they are immutable
          // values, not containers of cells.
          if (recv.is_str()) fail(f, "cannot assign into a string");
          if (auto err = index_error(recv, key); !err.empty()) fail(f, err);
          index_set(recv, key, f.regs[in.c]);
          break;
        }
        case Op::FieldGet: {
          ObjectObj* o = field_target(f, f.regs[in.b], in.c, in.d, "get");
          // Direct slot access, not ObjectObj::find: the whole point of
          // FieldGet is that the front end already knows the offset, so
          // this pays no key comparison at all, let alone find()'s linear
          // scan over every prop before this one. Safe even when in.a ==
          // in.b (the compiler routinely reuses the receiver's own
          // register as the destination here, the same shape Index's
          // compilation gives it): Value::operator= copies its argument
          // into a temporary and retains it before releasing *this, so
          // reading out of the very object *this holds the only reference
          // to is exactly the self-assignment case that idiom exists for.
          f.regs[in.a] = o->props[static_cast<size_t>(in.c)].second;
          break;
        }
        case Op::FieldSet: {
          ObjectObj* o = field_target(f, f.regs[in.a], in.b, in.d, "set");
          // A plain overwrite of an existing slot, never a new key -- so
          // unlike ObjectObj::set() this never has a drop key to register:
          // a struct's drop key (if it has one) is already registered, from
          // when ObjectLit first built it with that key present. See the
          // FieldGet/FieldSet Tag comment for why front ends must build
          // every field through ObjectLit before any FieldSet reaches it.
          o->props[static_cast<size_t>(in.b)].second = f.regs[in.c];
          break;
        }
        case Op::Len: {
          const Value& v = f.regs[in.b];
          if (auto err = len_error(v); !err.empty()) fail(f, err);
          f.regs[in.a] = length_of(v);
          break;
        }
        case Op::ToStr:
          f.regs[in.a] = Value::make_str(to_display(f.regs[in.b]));
          break;
        case Op::TypeOf:
          f.regs[in.a] = Value::make_str(type_name(f.regs[in.b].tag()));
          break;
        case Op::ArrayPush: {
          const Value& a = f.regs[in.a];
          if (!a.is_array()) {
            fail(f, std::string("cannot push into ") + type_name(a.tag()));
          }
          a.as_array()->items.push_back(f.regs[in.b]);
          break;
        }
        case Op::ArrayPop: {
          const Value& a = f.regs[in.b];
          if (!a.is_array()) {
            fail(f, std::string("cannot pop from ") + type_name(a.tag()));
          }
          auto& items = a.as_array()->items;
          if (items.empty()) fail(f, "pop from an empty array");
          // Move out before shrinking: dst may alias the array register.
          Value out = std::move(items.back());
          items.pop_back();
          f.regs[in.a] = std::move(out);
          break;
        }
        case Op::ObjectHas: {
          const Value& o = f.regs[in.b];
          const Value& k = f.regs[in.c];
          if (o.is_map()) {
            f.regs[in.a] = Value::make_bool(o.as_map()->find(k) != nullptr);
            break;
          }
          if (!o.is_object() || !k.is_str()) {
            fail(f, std::string("cannot ask ") + type_name(o.tag()) +
                        " for a " + type_name(k.tag()) + " key");
          }
          f.regs[in.a] = Value::make_bool(o.as_object()->find(k.as_str()) !=
                                          nullptr);
          break;
        }
        case Op::ObjectKeys: {
          const Value& o = f.regs[in.b];
          std::vector<Value> keys;
          if (o.is_map()) {
            // Insertion order, tombstones skipped: the same reproducible
            // order an object's keys come out in.
            const MapObj* mp = o.as_map();
            keys.reserve(mp->live);
            for (const auto& kv : mp->entries) {
              if (!kv.first.is_uninit()) keys.push_back(kv.first);
            }
          } else {
            if (!o.is_object()) {
              fail(f, std::string("cannot list keys of ") + type_name(o.tag()));
            }
            keys.reserve(o.as_object()->props.size());
            for (const auto& kv : o.as_object()->props) {
              keys.push_back(Value::make_str(kv.first));
            }
          }
          f.regs[in.a] = Value::make_array(std::move(keys));
          break;
        }
        case Op::ObjectRemove: {
          const Value& o = f.regs[in.a];
          const Value& k = f.regs[in.b];
          if (o.is_map()) {
            o.as_map()->remove(k);
            break;
          }
          if (!o.is_object() || !k.is_str()) {
            fail(f, std::string("cannot remove a ") + type_name(k.tag()) +
                        " key from " + type_name(o.tag()));
          }
          o.as_object()->remove(k.as_str());
          break;
        }
        case Op::NewMap:
          f.regs[in.a] = Value::make_map();
          break;
        case Op::StrSlice:
        case Op::ArraySlice: {
          const Value& recv = f.regs[in.b];
          const bool want_str = in.op == Op::StrSlice;
          if (want_str ? !recv.is_str() : !recv.is_array()) {
            fail(f, std::string("cannot slice ") + type_name(recv.tag()) +
                        (want_str ? " as a string" : " as an array"));
          }
          const int64_t len =
              want_str ? static_cast<int64_t>(recv.as_str().size())
                       : static_cast<int64_t>(recv.as_array()->items.size());
          const Value& i = f.regs[in.c];
          const Value& j = f.regs[in.d];
          if (auto err = slice_error(recv, i, j, len); !err.empty()) {
            fail(f, err);
          }
          const auto from = static_cast<size_t>(i.as_int());
          const auto to = static_cast<size_t>(j.as_int());
          // Build the result before storing it: dst may alias the receiver.
          Value out;
          if (want_str) {
            out = Value::make_str(recv.as_str().substr(from, to - from));
          } else {
            const auto& items = recv.as_array()->items;
            out = Value::make_array(std::vector<Value>(
                items.begin() + static_cast<std::ptrdiff_t>(from),
                items.begin() + static_cast<std::ptrdiff_t>(to)));
          }
          f.regs[in.a] = std::move(out);
          break;
        }
        case Op::StrByte: {
          const Value& s = f.regs[in.b];
          const Value& k = f.regs[in.c];
          if (!s.is_str()) {
            fail(f, std::string("cannot read a byte of ") + type_name(s.tag()));
          }
          if (auto err = index_error(s, k); !err.empty()) fail(f, err);
          const auto byte = static_cast<unsigned char>(
              s.as_str()[static_cast<size_t>(k.as_int())]);
          f.regs[in.a] = Value::make_int(byte);
          break;
        }
        case Op::StrFromByte: {
          const Value& v = f.regs[in.b];
          if (!v.is_int() || v.as_int() < 0 || v.as_int() > 255) {
            fail(f, "byte value must be an int in 0..255, not " +
                        (v.is_int() ? std::to_string(v.as_int())
                                    : std::string(type_name(v.tag()))));
          }
          f.regs[in.a] = Value::make_str(
              std::string(1, static_cast<char>(v.as_int())));
          break;
        }
        case Op::ToInt: {
          const Value& v = f.regs[in.b];
          if (v.is_int()) {
            f.regs[in.a] = v;
            break;
          }
          if (!v.is_number()) {
            fail(f, std::string("cannot convert ") + type_name(v.tag()) +
                        " to int");
          }
          const double d = v.as_double();
          // The comparison is in double, where int64's max rounds up to
          // 2^63 -- so >= on the top edge, > on the bottom.
          if (std::isnan(d) || d >= 9223372036854775808.0 ||
              d < -9223372036854775808.0) {
            fail(f, "double value out of int range");
          }
          f.regs[in.a] = Value::make_int(static_cast<int64_t>(d));
          break;
        }
        case Op::ToDouble: {
          const Value& v = f.regs[in.b];
          if (!v.is_number()) {
            fail(f, std::string("cannot convert ") + type_name(v.tag()) +
                        " to double");
          }
          f.regs[in.a] = Value::make_double(v.as_number());
          break;
        }
        case Op::ToFloat32: {
          const Value& v = f.regs[in.b];
          if (!v.is_number()) {
            fail(f, std::string("cannot convert ") + type_name(v.tag()) +
                        " to float");
          }
          // Round-trip through float and back: the one thing a front end
          // for a language with both float and double cannot write itself,
          // the same way ToStr's digits are the one thing it cannot derive
          // from a number in-language. A magnitude beyond float's range is
          // undefined behavior for a bare static_cast<float> ([conv.double]
          // defines the cast only up to the destination type's range), so
          // that range is handled by hand rather than left to the cast --
          // Java and C# both round such a value to +-infinity, not a trap,
          // and NaN survives the round trip as NaN either way. The boundary
          // for that is not float's own max: round-to-nearest sends
          // everything below the midpoint between kFloatMax and 2^128 back
          // down to kFloatMax, the same as an in-range value would round,
          // and only that midpoint (kFloatOverflow) and beyond actually
          // overflows to infinity.
          const double d = v.as_number();
          constexpr double kFloatMax =
              static_cast<double>(std::numeric_limits<float>::max());
          constexpr double kFloatOverflow = 0x1.ffffffp127;  // (2-2^-24)*2^127
          double result;
          if (std::isnan(d)) {
            result = d;
          } else if (d >= kFloatOverflow) {
            result = std::numeric_limits<double>::infinity();
          } else if (d > kFloatMax) {
            result = kFloatMax;
          } else if (d <= -kFloatOverflow) {
            result = -std::numeric_limits<double>::infinity();
          } else if (d < -kFloatMax) {
            result = -kFloatMax;
          } else {
            result = static_cast<double>(static_cast<float>(d));
          }
          f.regs[in.a] = Value::make_double(result);
          break;
        }
        case Op::FMod: {
          const Value& l = f.regs[in.b];
          const Value& r = f.regs[in.c];
          if (!l.is_number() || !r.is_number()) {
            fail(f, std::string("cannot fmod ") + type_name(l.tag()) +
                        " and " + type_name(r.tag()));
          }
          if (r.as_number() == 0.0) fail(f, "divide by zero");
          f.regs[in.a] =
              Value::make_double(std::fmod(l.as_number(), r.as_number()));
          break;
        }
        case Op::Pow: {
          const Value& l = f.regs[in.b];
          const Value& r = f.regs[in.c];
          if (!l.is_number() || !r.is_number()) {
            fail(f, std::string("cannot pow ") + type_name(l.tag()) +
                        " and " + type_name(r.tag()));
          }
          f.regs[in.a] =
              Value::make_double(std::pow(l.as_number(), r.as_number()));
          break;
        }
        case Op::CellNew:
          f.cells[in.a] = Value::make_cell();
          break;
        case Op::MakeClosure: {
          std::vector<Value> cells;
          const auto& cmap = p.capture_maps[static_cast<size_t>(in.c)];
          cells.reserve(cmap.size());
          for (const CaptureSrc& src : cmap) {
            cells.push_back(capture_cell(f, src));
          }
          f.regs[in.a] = Value::make_closure(in.b, std::move(cells));
          break;
        }
        case Op::CallValue: {
          const SrcPos pos = pos_at(ch, f.pc);
          // Copy the callee out first: the result register may be the callee's
          // own, and the frame this pushes outlives the read either way.
          // Advance before pushing -- this is where the caller resumes, and a
          // callee that never returns never reads it.
          const Value callee = f.regs[in.b];
          ++f.pc;
          push_closure(callee, f.regs.data() + in.c, in.d, in.a, pos);
          continue;
        }
        case Op::TailCall: {
          const SrcPos pos = pos_at(ch, f.pc);
          const Value callee = f.regs[in.b];
          // The replacement needs a callee whose activation is a plain frame
          // (a native runs on the spot, a generator call builds an object),
          // and a frame that is not the program's own (the entry frame's
          // end is the program's end, with entry_frame_drops' rules). Any
          // other shape is the call it would otherwise have been.
          const bool replace =
              callee.is_func() && !f.entry &&
              !p.chunks[static_cast<size_t>(callee.as_closure()->func)]
                   .is_generator;
          if (!replace) {
            ++f.pc;
            push_closure(callee, f.regs.data() + in.c, in.d, in.a, pos);
            continue;
          }
          const ClosureObj* c = callee.as_closure();
          const Chunk& nch = p.chunks[static_cast<size_t>(c->func)];
          // This frame's exit, in full, before the callee starts: every
          // temporary except the argument window, the locals
          // last-slot-first, then each open scope's owned resolution
          // innermost-first -- what falling off the end would do, minus
          // per-scope interleaving, since the frame is going as a whole.
          // The compiler has kept defers out of here (tail_call_ok), so
          // there are none to run: neither f.defers nor f.defer_marks can
          // be non-empty at a TailCall.
          //
          // The argument window stays: those registers hold the arguments'
          // one counted reference across the exit, and the callee's locals
          // move out of them below. They cannot be stashed anywhere else
          // -- a release here can run a drop hook, which re-enters dispatch
          // and may tail-call again, so a buffer Exec owned would be
          // clobbered under them -- and copying them out would be an
          // allocation per call, on the one path that exists to make a call
          // loop cheap.
          for (int32_t r = 0; r < in.c; ++r) {
            f.regs[static_cast<size_t>(r)] = Value();
          }
          for (size_t r = static_cast<size_t>(in.c + in.d); r < f.regs.size();
               ++r) {
            f.regs[r] = Value();
          }
          release_range(f, 0, static_cast<int32_t>(f.locals.size()));
          while (!f.owned_marks.empty()) leave_scope_owned(f);
          // The new activation, in place: ret_reg, gen_self, coro_self and
          // the frame's position in the stack are the caller's and stay --
          // a tail call from a coroutine's bottom frame is still that
          // coroutine's bottom frame.
          f.chunk = &nch;
          f.pc = 0;
          f.locals.assign(static_cast<size_t>(nch.num_locals), Value::uninit());
          const int32_t taken = check_arity(nch, in.d, pos);
          for (int32_t i = 0; i < taken; ++i) {
            f.locals[static_cast<size_t>(i)] =
                std::move(f.regs[static_cast<size_t>(in.c + i)]);
          }
          for (int32_t i = taken; i < nch.num_params; ++i) {
            f.locals[static_cast<size_t>(i)] = Value();
          }
          f.regs.assign(static_cast<size_t>(nch.num_regs), Value());
          f.cells.assign(static_cast<size_t>(nch.num_cells), Value());
          f.captures = c->cells;
          f.argc = in.d;
          // A tail call is the loop back-edge of a program written as calls:
          // the same interrupt point Jump gives a While.
          coreir_rt_poll();
          continue;
        }
        case Op::Throw: {
          const SrcPos sp = pos_at(ch, f.pc);
          throw Raise{f.regs[in.a], sp, {}};
        }
        case Op::Yield: {
          // Suspend: the frame's storage moves back into the generator, the
          // frame pops, and the resumer gets {value, done: false} -- the
          // same delivery shape as Ret, one level up.
          if (!f.gen_self.is_generator()) {
            fail(f, "yield outside a generator");  // verify() precludes this
          }
          Value self = std::move(f.gen_self);
          GeneratorObj* go = self.as_generator();
          Value out = f.regs[in.b];
          go->frame.pc = static_cast<int64_t>(f.pc) + 1;
          go->frame.yield_reg = in.a;
          park_frame(go->frame, f);
          go->state = GeneratorObj::State::Suspended;
          const int32_t ret_reg = f.ret_reg;
          frames.pop_back();
          deliver(ret_reg, gen_result(std::move(out), false));
          if (frames.size() <= floor) return;
          continue;
        }
        case Op::GenResume: {
          const SrcPos pos = pos_at(ch, f.pc);
          const Value gv = f.regs[in.b];
          GeneratorObj* go = gen_operand(f, gv, "resume");
          using St = GeneratorObj::State;
          if (go->state == St::Done) {
            f.regs[in.a] = gen_result(Value(), true);
            break;
          }
          const bool started = go->state == St::Suspended;
          const int32_t yield_reg = go->frame.yield_reg;
          check_can_push(pos);
          auto nf = unpark_frame(gv, in.a);
          // The sent value lands where the Yield's own result goes; a first
          // resume has no yield in flight and its argument is ignored.
          if (started && yield_reg >= 0) {
            nf->regs[static_cast<size_t>(yield_reg)] = f.regs[in.c];
          }
          ++f.pc;
          frames.push_back(std::move(nf));
          continue;
        }
        case Op::GenReturn: {
          const SrcPos pos = pos_at(ch, f.pc);
          const Value gv = f.regs[in.b];
          GeneratorObj* go = gen_operand(f, gv, "close");
          using St = GeneratorObj::State;
          Value out = f.regs[in.c];
          if (go->state == St::Suspended) {
            // Close: restore the activation, run its pending defers --
            // innermost first, as the yield point's own Return would --
            // then discard the frame without executing any more of it. A
            // defer that throws propagates to this call; the generator is
            // done either way (the popped frame releases what it held).
            check_can_push(pos);
            frames.push_back(unpark_frame(gv, -1));
            Frame& gf = *frames.back();
            try {
              run_pending_defers(gf, pos);
            } catch (Raise&) {
              go->state = St::Done;
              frames.pop_back();
              throw;
            }
            go->state = St::Done;
            frames.pop_back();
          } else {
            // Start never ran (nothing registered, nothing to run); Done
            // holds nothing either. Both just settle the state and drop
            // whatever arguments a Start-state activation still packaged.
            go->state = St::Done;
            go->frame = GenFrame{};
          }
          f.regs[in.a] = gen_result(std::move(out), true);
          break;
        }
        case Op::GenThrow: {
          const SrcPos pos = pos_at(ch, f.pc);
          const Value gv = f.regs[in.b];
          GeneratorObj* go = gen_operand(f, gv, "throw into");
          using St = GeneratorObj::State;
          Value v = f.regs[in.c];
          if (go->state != St::Suspended) {
            // No frame for it to land in: the generator is done (a Start
            // activation drops what it packaged) and the throw is this
            // instruction's own, at its own pc like a Throw.
            go->state = St::Done;
            go->frame = GenFrame{};
            throw Raise{std::move(v), pos, {}};
          }
          // Re-enter at the Yield itself rather than after it, so the
          // regions enclosing the yield -- its handlers, its defers -- are
          // the ones the throw crosses; the resumer's pc advances as for a
          // call, which is where the throw lands if the body lets it out.
          check_can_push(pos);
          const size_t at = static_cast<size_t>(go->frame.pc) - 1;
          auto nf = unpark_frame(gv, in.a);
          nf->pc = at;
          const SrcPos yp = pos_at(*nf->chunk, at);
          ++f.pc;
          frames.push_back(std::move(nf));
          throw Raise{std::move(v), yp, {}};
        }
        case Op::Enqueue: {
          const Value& v = f.regs[in.a];
          if (v.is_coroutine()) {
            CoroObj* co = v.as_coroutine();
            if (!co->scheduled && co->state != CoroObj::State::Done) {
              co->scheduled = true;
              scheduled.emplace(co, v);
            }
          } else if (!v.is_callable()) {
            fail(f, std::string("enqueue needs a function, not ") +
                        type_name(v.tag()));
          }
          jobs.push_back(v);
          break;
        }
        case Op::DeferPush: {
          const Value& v = f.regs[in.a];
          if (!v.is_callable()) {
            fail(f, std::string("defer needs a function, not ") +
                        type_name(v.tag()));
          }
          f.defers.push_back(v);
          break;
        }
        case Op::DeferMark:
          f.defer_marks.push_back(
              {f.defers.size(), static_cast<int32_t>(f.pc)});
          break;
        case Op::DeferRunTo: {
          const size_t mark = f.defer_marks.back().first;
          f.defer_marks.pop_back();
          const SrcPos sp = pos_at(ch, f.pc);
          // Advance first: the nested run pushes frames, and this is where
          // execution resumes when the last defer returns.
          ++f.pc;
          run_defers_now(f, mark, sp);
          continue;
        }
        case Op::Ret: {
          // Move the result into the caller before the frame goes: after the
          // pop, the register it lives in is gone.
          Value result;
          if (in.b != 0) result = f.regs[in.a];
          // A generator body's return finishes the generator; its resume
          // caller sees {value, done: true} where a plain call would see
          // the bare value. A coroutine's bottom frame returning finishes
          // the coroutine the same way, for its CoroResume.
          if (f.gen_self.is_generator()) {
            f.gen_self.as_generator()->state = GeneratorObj::State::Done;
            result = gen_result(std::move(result), true);
          }
          if (f.coro_self.is_coroutine()) {
            finish_coro(f.coro_self.as_coroutine());
            result = gen_result(std::move(result), true);
          }
          const int32_t ret_reg = f.ret_reg;
          pop_frame();
          deliver(ret_reg, std::move(result));
          if (frames.size() <= floor) return;
          continue;
        }
        case Op::CoroCreate: {
          const Value& fn = f.regs[in.b];
          if (!fn.is_callable()) {
            fail(f, std::string("coroutine needs a function, not ") +
                        type_name(fn.tag()));
          }
          Value co = Value::make_coroutine();
          co.as_coroutine()->fn = fn;
          f.regs[in.a] = std::move(co);
          break;
        }
        case Op::CoroResume: {
          const SrcPos pos = pos_at(ch, f.pc);
          const Value cv = f.regs[in.b];
          CoroObj* co = coro_operand(f, cv, "resume");
          using St = CoroObj::State;
          if (co->state == St::Running) fail(f, "coroutine already running");
          if (co->state == St::Done) {
            f.regs[in.a] = gen_result(Value(), true);
            break;
          }
          if (co->state == St::Start) {
            // The first resume is the call (start_coro), and a trap at it
            // is this instruction's own. A callee that completed on the
            // spot left no frame to suspend, so the coroutine is done at
            // once, with the answer that call delivered into in.a.
            const Value sent = f.regs[in.c];
            ++f.pc;
            if (!start_coro(cv, sent, in.a, pos)) {
              f.regs[in.a] = gen_result(std::move(f.regs[in.a]), true);
            }
            continue;
          }
          // Suspended: the parked frames come back, all of them at once,
          // so the depth bound is checked against the whole slice.
          if (frames.size() + co->frames.size() > max_frames) {
            raise_trap("recursion limit exceeded", pos);
          }
          coreir_rt_poll();
          ++f.pc;
          unpark_coro(cv, in.a, &f.regs[in.c]);
          continue;
        }
        case Op::CoroYield: {
          // The frames to park are the innermost running coroutine's:
          // from its bottom (the frame CoroResume entered) up to here. A
          // bottom below this dispatch's floor belongs to an outer
          // dispatch -- there is C++ between it and here (a native that
          // called back in, a destructor, a defer, the job driver) which
          // no frame stack can hold -- so that yield cannot happen: Lua's
          // "attempt to yield across a C-call boundary".
          const size_t bottom = coro_bottom();
          if (bottom == frames.size()) fail(f, "yield outside a coroutine");
          if (bottom < floor) fail(f, "cannot yield across a host boundary");
          const Value self = frames[bottom]->coro_self;
          Value out = f.regs[in.b];
          const int32_t ret_reg = frames[bottom]->ret_reg;
          ++f.pc;  // where the next resume re-enters
          park_coro(self.as_coroutine(), bottom, in.a);
          // The {value, done} answer belongs to whoever resumed. A
          // scheduler turn (ret_reg -1) resumed nobody's expression, so
          // there is no answer to build.
          if (ret_reg != -1) {
            deliver(ret_reg, gen_result(std::move(out), false));
          }
          if (frames.size() <= floor) return;
          continue;
        }
        case Op::CoroClose: {
          const SrcPos pos = pos_at(ch, f.pc);
          const Value cv = f.regs[in.a];
          CoroObj* co = coro_operand(f, cv, "close");
          using St = CoroObj::State;
          if (co->state == St::Running) {
            fail(f, "cannot close a running coroutine");
          }
          if (co->state == St::Done) break;
          if (co->state == St::Start) {
            co->fn = Value();
            finish_coro(co);
            break;
          }
          // Restore the slice and unwind it by hand, top frame first: each
          // frame's pending defers innermost mark first, then the frame
          // itself (its locals last-slot-first, as any return would). A
          // generator activation caught in the slice is finished with it.
          // A defer that throws propagates to this call; the coroutine is
          // done either way.
          if (frames.size() + co->frames.size() > max_frames) {
            raise_trap("recursion limit exceeded", pos);
          }
          const size_t base = frames.size();
          ++f.pc;
          unpark_coro(cv, -1, nullptr);
          try {
            while (frames.size() > base) {
              Frame& t = *frames.back();
              run_pending_defers(t, pos);
              if (t.gen_self.is_generator()) {
                t.gen_self.as_generator()->state = GeneratorObj::State::Done;
              }
              pop_frame();
            }
          } catch (Raise&) {
            finish_coro(co);
            while (frames.size() > base) frames.pop_back();
            throw;
          }
          finish_coro(co);
          continue;
        }
        case Op::CoroStatus: {
          const Value& cv = f.regs[in.b];
          CoroObj* co = coro_operand(f, cv, "ask the status of");
          const char* s = "?";
          switch (co->state) {
            case CoroObj::State::Start:     s = "start"; break;
            case CoroObj::State::Suspended: s = "suspended"; break;
            case CoroObj::State::Running:   s = "running"; break;
            case CoroObj::State::Done:      s = "done"; break;
          }
          f.regs[in.a] = Value::make_str(s);
          break;
        }
        case Op::CoroCurrent: {
          const size_t bottom = coro_bottom();
          f.regs[in.a] =
              bottom < frames.size() ? frames[bottom]->coro_self : Value();
          break;
        }
      }
      ++f.pc;
    }
  }
};

}  // namespace detail

}  // namespace vm

namespace coreir {

inline Value NativeCall::call(const Value& callee, const Value* argv,
                              int32_t n) {
  return static_cast<vm::detail::Exec*>(exec)->call_sync(callee, argv, n, pos);
}

inline Value NativeCall::trap(const std::string& msg) const {
  Value e = Value::make_object();
  e.as_object()->set("message", Value::make_str(msg));
  e.as_object()->set("line", Value::make_int(pos.line));
  e.as_object()->set("col", Value::make_int(pos.col));
  return e;
}

}  // namespace coreir

namespace vm {

inline void run(const Program& p, coreir::Runtime& rt, const RunOptions& opts) {
  coreir::Runtime::Scope scope(rt);
  const int depth = opts.max_call_depth;
  detail::Exec e{p, depth < 0 ? 0 : static_cast<size_t>(depth), rt,
         opts.entry_frame_drops, {}};
  // Link the program's declared host functions before anything runs: a
  // name the host did not supply is a configuration error of the whole
  // run, not something to discover at the one call site that reaches it.
  e.natives.reserve(p.natives.size());
  for (const std::string& name : p.natives) {
    const NativeDef* def = nullptr;
    for (const NativeDef& d : opts.natives) {
      if (d.name == name) { def = &d; break; }  // the first definition wins
    }
    if (!def || !def->fn) {
      coreir_rt::fail("unresolved native '" + name + "'", 0, 0);
    }
    e.natives.push_back(
        coreir::Value::make_native(def->name, def->arity, def->ctx, def->fn));
  }
  rt.set_drop_fn(&e, &detail::Exec::drop_hook);
  e.frames.push_back(e.make_frame(p.chunks[0]));
  e.frames.back()->entry = true;
  try {
    e.run();
  } catch (...) {
    rt.set_drop_fn(nullptr, nullptr);
    throw;
  }
  rt.set_drop_fn(nullptr, nullptr);
}

inline void run(const Program& p, coreir::Runtime& rt, int max_call_depth) {
  RunOptions opts;
  opts.max_call_depth = max_call_depth;
  run(p, rt, opts);
}

inline void run(const Program& p, int max_call_depth) {
  coreir::Runtime rt;
  run(p, rt, max_call_depth);
}

}  // namespace vm

// ===== runtime/coreir_rt_default.cc =====

// The stdio host: what a program gets by defining VMLIB_DEFAULT_RUNTIME in
// exactly one translation unit before including this header. A host
// embedding the library elsewhere defines the coreir_rt_* symbols itself
// instead and leaves the macro undefined (see coreir/rt.h).

// The stdio host's own knob, not part of the coreir_rt contract.
//
// "<path>:<line>:<col>: <message>" is this implementation's choice of
// wording; a different host formats errors however it formats its own, and
// has no use for a source path threaded in this way. That is why set_path
// lives here rather than in coreir/rt.h.
namespace coreir_rt_default {

void set_path(std::string path);

}  // namespace coreir_rt_default

#ifdef VMLIB_DEFAULT_RUNTIME

#include <cerrno>
#include <cinttypes>
#include <iostream>

namespace coreir_rt_default {

namespace detail {
std::string g_path = "<input>";
}  // namespace detail

void set_path(std::string path) { detail::g_path = std::move(path); }

}  // namespace coreir_rt_default

extern "C" {

void coreir_rt_out(int64_t v) { std::printf("%" PRId64 "\n", v); }

void coreir_rt_out_raw(const char* bytes, int64_t len) {
  std::fwrite(bytes, 1, static_cast<size_t>(len), stdout);
}

void coreir_rt_out_str(const char* bytes, int64_t len) {
  std::printf("%.*s\n", static_cast<int>(len), bytes);
}

int64_t coreir_rt_in(int64_t line, int64_t col) {
  std::string s;
  if (!std::getline(std::cin, s)) coreir_rt_fail("invalid input", line, col);

  // One line, converted whole -- matching pl0.cul's `to_long(IO.input())`
  // rather than scanf's whitespace-delimited token. strtoll skips leading
  // whitespace on its own; only trailing whitespace needs trimming so the
  // full-consumption check below lands on the right end of the string.
  const size_t e = s.find_last_not_of(" \t\r\n");
  if (e == std::string::npos) coreir_rt_fail("invalid input", line, col);
  s.resize(e + 1);

  errno = 0;
  char* end = nullptr;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (errno != 0 || end != s.c_str() + s.size()) {
    coreir_rt_fail("invalid input", line, col);
  }
  return static_cast<int64_t>(v);
}

void coreir_rt_fail(const char* msg, int64_t line, int64_t col) {
  std::fflush(stdout);
  std::fprintf(stderr, "%s:%" PRId64 ":%" PRId64 ": %s\n",
               coreir_rt_default::detail::g_path.c_str(), line, col, msg);
  std::exit(1);
}

void coreir_rt_poll(void) {}

}  // extern "C"

#endif  // VMLIB_DEFAULT_RUNTIME
