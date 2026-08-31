// The value model, stated once.
//
// SPIKE (Phase 0b): a value is no longer an i64. It is a tagged Value (see
// coreir/value.h) that may hold a reference to a heap object, so every
// operation here dispatches on the operand tags and may fail because of them
// -- the shape a dynamically typed front end needs, and the shape PL/0 never
// exercised because it only ever had one type.
//
// Comparisons yield 0 or 1; If and While test truthiness. Integer arithmetic
// wraps. PL/0's grammar happens to keep comparisons out of arithmetic
// positions, but Core-IR claims to be grammar-independent, so that accident
// is not something a backend may rely on.
//
// vm::compile and the bytecode executor both include this header directly,
// so wrapping and trap conditions have exactly one implementation between
// them. A backend that cannot call C++ (one that emits another language's
// text, say) has to restate the equivalent instead -- the one place such a
// backend's author should expect to read this file twice.

#pragma once

#include <cstdint>
#include <limits>
#include <string>

#include "coreir/ir.h"
#include "coreir/value.h"

namespace coreir {

// A variable's storage. `inited` is separate from the tag rather than folded
// into it as an Uninit tag: a spike is not the place to decide whether "read
// before assigned" is a distinct value or a distinct flag, and keeping it a
// flag leaves today's diagnostic wording untouched.
struct Slot {
  Value value;
  uint8_t inited = 0;
};

inline const char* type_name(ValueTag t) {
  switch (t) {
    case ValueTag::Nil: return "nil";
    case ValueTag::Int: return "int";
    case ValueTag::Str: return "string";
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

inline bool is_comparison(BinOp op) {
  switch (op) {
    case BinOp::Eq: case BinOp::Ne: case BinOp::Lt:
    case BinOp::Le: case BinOp::Gt: case BinOp::Ge:
      return true;
    default:
      return false;
  }
}

// Returns an empty string when the operation is defined for these operands,
// or the diagnostic when it is not. Two failure kinds now, where the i64 IR
// had one: the arithmetic traps it always had, and type errors, which only
// exist once values can disagree about what they are.
//
// SPIKE: returning std::string rather than a const char* costs an allocation
// the old code did not pay. It buys operand types in the message, which is
// most of what makes a dynamic language's type error useful; whether the real
// version formats lazily is a decision for the phase that has a Double, a
// Bool and an Array to name as well.
inline std::string binop_error(BinOp op, const Value& l, const Value& r) {
  if (l.is_int() && r.is_int()) {
    if (op == BinOp::Div || op == BinOp::Mod) {
      if (r.as_int() == 0) return "divide by zero";
      if (l.as_int() == std::numeric_limits<int64_t>::min() &&
          r.as_int() == -1) {
        return "division overflow";
      }
    }
    return {};
  }
  if (l.is_str() && r.is_str()) {
    // Strings concatenate and compare; they do not subtract or divide.
    if (op == BinOp::Add || is_comparison(op)) return {};
  }
  // Equality across types is a question ("is 1 == '1'?") a language answers,
  // not the VM. Refusing it keeps the VM from baking in an answer that a
  // front end would then have to work around.
  return std::string("cannot ") + name_of(op) + " " + type_name(l.tag()) +
         " and " + type_name(r.tag());
}

// Only valid when binop_error returned empty.
inline Value apply_binop(BinOp op, const Value& l, const Value& r) {
  if (l.is_str()) {
    const std::string& a = l.as_str();
    const std::string& b = r.as_str();
    switch (op) {
      case BinOp::Add: return Value::make_str(a + b);
      case BinOp::Eq:  return Value::make_int(a == b ? 1 : 0);
      case BinOp::Ne:  return Value::make_int(a != b ? 1 : 0);
      case BinOp::Lt:  return Value::make_int(a < b ? 1 : 0);
      case BinOp::Le:  return Value::make_int(a <= b ? 1 : 0);
      case BinOp::Gt:  return Value::make_int(a > b ? 1 : 0);
      case BinOp::Ge:  return Value::make_int(a >= b ? 1 : 0);
      default: break;
    }
    return Value();
  }
  const int64_t a = l.as_int();
  const int64_t b = r.as_int();
  switch (op) {
    case BinOp::Add: return Value::make_int(wrap_add(a, b));
    case BinOp::Sub: return Value::make_int(wrap_sub(a, b));
    case BinOp::Mul: return Value::make_int(wrap_mul(a, b));
    case BinOp::Div: return Value::make_int(a / b);
    case BinOp::Mod: return Value::make_int(a % b);
    case BinOp::Eq:  return Value::make_int(a == b ? 1 : 0);
    case BinOp::Ne:  return Value::make_int(a != b ? 1 : 0);
    case BinOp::Lt:  return Value::make_int(a < b ? 1 : 0);
    case BinOp::Le:  return Value::make_int(a <= b ? 1 : 0);
    case BinOp::Gt:  return Value::make_int(a > b ? 1 : 0);
    case BinOp::Ge:  return Value::make_int(a >= b ? 1 : 0);
  }
  return Value();
}

inline std::string unop_error(UnOp op, const Value& v) {
  if (op == UnOp::Neg && !v.is_int()) {
    return std::string("cannot negate ") + type_name(v.tag());
  }
  return {};
}

inline Value apply_unop(UnOp, const Value& v) {
  return Value::make_int(wrap_neg(v.as_int()));
}

// The one formatter for "read before assigned". A future backend that cannot
// call this directly (one emitting another language's text, say) should call
// this at IR-build time to bake the finished string into what it emits,
// rather than restating the wording itself.
inline std::string format_uninit_var(const std::string& name) {
  return "uninitialized variable '" + name + "'";
}

}  // namespace coreir
