// SPIKE (Phase 0b) -- a tagged runtime value with reference counting.
//
// What this exists to answer, before any of it is built for real:
//
//   1. Where does retain/release go? culebra's answer is "the bytecode
//      compiler emits Retain/Release/Take instructions", because culebra has
//      an LLVM lane whose emitted machine code has to do it explicitly, and
//      because it wants to elide redundant pairs. cpp-vmlib has no such lane.
//      So the cheaper answer is tried here first: make Value itself an RAII
//      handle, and let ordinary C++ assignment and destruction place every
//      retain and release. If that holds up, the compiler stays untouched and
//      a whole class of "missing release" bugs cannot be written.
//
//   2. Does a host throw still leave nothing behind? Today's executor is
//      exception-safe for free because a Frame holds only plain vectors. Once
//      registers hold owned references that stops being free -- unless the
//      ownership lives in the type, in which case the same unwind that
//      destroys the frame stack releases every value in it.
//
// Layout mirrors culebra's JitValue: a tag word and a payload word, with the
// refcount at offset zero of every heap object.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coreir {

// Cell and Func are not types a source language needs to name. A Cell is the
// box a captured variable lives in, so a closure that outlives the frame
// declaring the variable still shares one copy of it; Func is such a closure.
// Both are values because both are refcounted heap objects, and making them
// ordinary tags means one lifetime rule covers everything.
enum class ValueTag : uint8_t { Nil, Int, Str, Cell, Func };

// Live heap objects, so a test can assert a program left none behind. Not a
// design feature -- a spike needs a leak to be loud, and a counter is the
// cheapest loud thing there is.
extern int64_t g_live_heap_objects;

struct HeapObj {
  int64_t rc;  // offset zero, as in culebra's refcounted structs
  explicit HeapObj() : rc(1) { ++g_live_heap_objects; }
  ~HeapObj() { --g_live_heap_objects; }
  HeapObj(const HeapObj&) = delete;
  HeapObj& operator=(const HeapObj&) = delete;
};

struct StrObj : HeapObj {
  std::string s;
  explicit StrObj(std::string v) : s(std::move(v)) {}
};

class Value;

// The box a captured variable lives in. Defined out of line because it holds
// a Value, which is not complete yet.
struct CellObj;
// A closure: which function, plus the cells it closed over. The cells are
// shared with whatever frame or other closure also holds them -- that sharing
// is the entire reason cells exist.
struct ClosureObj;

class Value {
 public:
  Value() : tag_(ValueTag::Nil), data_(0) {}

  static Value make_int(int64_t v) {
    Value r;
    r.tag_ = ValueTag::Int;
    r.data_ = v;
    return r;
  }

  // Takes ownership of the +1 a fresh StrObj is born with.
  static Value make_str(std::string s) {
    Value r;
    r.tag_ = ValueTag::Str;
    r.data_ = reinterpret_cast<int64_t>(new StrObj(std::move(s)));
    return r;
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
  static Value make_closure(int32_t func, std::vector<Value> cells);

  ValueTag tag() const { return tag_; }
  bool is_heap() const {
    return tag_ == ValueTag::Str || tag_ == ValueTag::Cell ||
           tag_ == ValueTag::Func;
  }
  bool is_int() const { return tag_ == ValueTag::Int; }
  bool is_str() const { return tag_ == ValueTag::Str; }
  bool is_nil() const { return tag_ == ValueTag::Nil; }
  bool is_cell() const { return tag_ == ValueTag::Cell; }
  bool is_func() const { return tag_ == ValueTag::Func; }

  int64_t as_int() const { return data_; }
  const std::string& as_str() const { return str_obj()->s; }
  CellObj* as_cell() const { return reinterpret_cast<CellObj*>(data_); }
  ClosureObj* as_closure() const {
    return reinterpret_cast<ClosureObj*>(data_);
  }

  // Truthiness, matching semantics.h's rule for the int-only IR: nil and 0
  // are false, everything else is true. A string's emptiness is deliberately
  // not special-cased -- that is a language's decision, not the VM's.
  bool truthy() const {
    switch (tag_) {
      case ValueTag::Nil: return false;
      case ValueTag::Int: return data_ != 0;
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

struct CellObj : HeapObj {
  Value v;
};

struct ClosureObj : HeapObj {
  int32_t func = 0;
  std::vector<Value> cells;  // every element is a Cell
};

inline Value Value::make_cell() {
  Value r;
  r.tag_ = ValueTag::Cell;
  r.data_ = reinterpret_cast<int64_t>(new CellObj());
  return r;
}

inline Value Value::make_closure(int32_t func, std::vector<Value> cells) {
  auto* c = new ClosureObj();
  c->func = func;
  c->cells = std::move(cells);
  Value r;
  r.tag_ = ValueTag::Func;
  r.data_ = reinterpret_cast<int64_t>(c);
  return r;
}

// SPIKE: reference counting alone cannot free a cycle, and closures make
// cycles reachable in one line of source -- a recursive closure stored in the
// cell it captured is cell -> closure -> cell. Nothing here pretends
// otherwise: this releases what it can, and a cycle stays counted in
// g_live_heap_objects. That is the measurement the tracing backstop phase
// exists to act on, so the spike leaves it visible rather than papering over
// it (see tests/spike_closures.cc, which asserts the leak rather than
// asserting its absence).
inline void Value::release() {
  if (!is_heap()) return;
  HeapObj* h = reinterpret_cast<HeapObj*>(data_);
  const ValueTag t = tag_;
  // Clear before deleting: a cell holding the last reference to a closure
  // holding this same cell would otherwise re-enter release() on a half-dead
  // object.
  tag_ = ValueTag::Nil;
  data_ = 0;
  if (--h->rc != 0) return;
  switch (t) {
    case ValueTag::Str:  delete static_cast<StrObj*>(h); break;
    case ValueTag::Cell: delete static_cast<CellObj*>(h); break;
    case ValueTag::Func: delete static_cast<ClosureObj*>(h); break;
    default: break;
  }
}

}  // namespace coreir
