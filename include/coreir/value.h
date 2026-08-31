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

namespace coreir {

enum class ValueTag : uint8_t { Nil, Int, Str };

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

  ValueTag tag() const { return tag_; }
  bool is_heap() const { return tag_ == ValueTag::Str; }
  bool is_int() const { return tag_ == ValueTag::Int; }
  bool is_str() const { return tag_ == ValueTag::Str; }
  bool is_nil() const { return tag_ == ValueTag::Nil; }

  int64_t as_int() const { return data_; }
  const std::string& as_str() const { return str_obj()->s; }

  // Truthiness, matching semantics.h's rule for the int-only IR: nil and 0
  // are false, everything else is true. A string's emptiness is deliberately
  // not special-cased -- that is a language's decision, not the VM's.
  bool truthy() const {
    switch (tag_) {
      case ValueTag::Nil: return false;
      case ValueTag::Int: return data_ != 0;
      case ValueTag::Str: return true;
    }
    return false;
  }

 private:
  StrObj* str_obj() const { return reinterpret_cast<StrObj*>(data_); }

  void retain() {
    if (is_heap()) ++reinterpret_cast<HeapObj*>(data_)->rc;
  }
  void release() {
    if (!is_heap()) return;
    HeapObj* h = reinterpret_cast<HeapObj*>(data_);
    if (--h->rc == 0) delete static_cast<StrObj*>(h);
    tag_ = ValueTag::Nil;
    data_ = 0;
  }

  ValueTag tag_;
  int64_t data_;
};

}  // namespace coreir
