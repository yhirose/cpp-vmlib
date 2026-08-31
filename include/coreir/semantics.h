// The value model, stated once.
//
// Every Core-IR value is an i64. Comparisons yield 0 or 1; If and While test
// `!= 0`. Arithmetic wraps. PL/0's grammar happens to keep comparisons out of
// arithmetic positions, but Core-IR claims to be grammar-independent, so that
// accident is not something a backend may rely on.
//
// interp and the bytecode executor share these helpers outright, so they
// cannot drift. llvmgen has to emit the equivalent instead of calling them --
// the one place where the value model is restated rather than shared, and
// therefore the one place worth reading twice.

#pragma once

#include <cstdint>
#include <limits>

#include "coreir/ir.h"

namespace coreir {

struct Slot {
  int64_t value = 0;
  uint8_t inited = 0;
};

// Wrapping, always: signed overflow is UB in C++ and poison in LLVM, so the
// arithmetic goes through uint64_t here and llvmgen emits add/sub/mul with
// neither nsw nor nuw. Leaving this undecided is how -O2 makes one lane
// disagree with the others.
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

// Div and Mod are the only trapping operations. INT64_MIN / -1 is UB in C++
// and poison in LLVM just as surely as division by zero is, and wrapping
// arithmetic puts INT64_MIN within reach, so both are guarded.
inline const char* binop_trap(BinOp op, int64_t l, int64_t r) {
  if (op != BinOp::Div && op != BinOp::Mod) return nullptr;
  if (r == 0) return "divide by zero";
  if (l == std::numeric_limits<int64_t>::min() && r == -1) {
    return "division overflow";
  }
  return nullptr;
}

// Only valid when binop_trap returned nullptr.
inline int64_t apply_binop(BinOp op, int64_t l, int64_t r) {
  switch (op) {
    case BinOp::Add: return wrap_add(l, r);
    case BinOp::Sub: return wrap_sub(l, r);
    case BinOp::Mul: return wrap_mul(l, r);
    case BinOp::Div: return l / r;
    case BinOp::Mod: return l % r;
    case BinOp::Eq:  return l == r ? 1 : 0;
    case BinOp::Ne:  return l != r ? 1 : 0;
    case BinOp::Lt:  return l < r ? 1 : 0;
    case BinOp::Le:  return l <= r ? 1 : 0;
    case BinOp::Gt:  return l > r ? 1 : 0;
    case BinOp::Ge:  return l >= r ? 1 : 0;
  }
  return 0;
}

inline bool truthy(int64_t v) { return v != 0; }

}  // namespace coreir
