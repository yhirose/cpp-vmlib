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

#pragma once

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>

#include "coreir/ir.h"
#include "coreir/value.h"

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
// Returning std::string rather than a const char* costs an allocation on the
// failing path only, and buys operand types in the message -- most of what
// makes a dynamic language's type error useful.
inline std::string binop_error(BinOp op, const Value& l, const Value& r) {
  // Two ints stay integers; anything else numeric widens to double. Mod on
  // doubles is deliberately absent -- fmod versus truncation is a language's
  // decision, and no front end here needs it yet.
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
  if (l.is_number() && r.is_number()) {
    if (op == BinOp::Mod) return "cannot mod double";
    return {};
  }
  if (l.is_str() && r.is_str()) {
    // Strings concatenate and compare; they do not subtract or divide.
    if (op == BinOp::Add || is_comparison(op)) return {};
  }
  if (l.is_bool() && r.is_bool()) {
    if (op == BinOp::Eq || op == BinOp::Ne) return {};
  }
  if (l.is_nil() && r.is_nil()) {
    if (op == BinOp::Eq || op == BinOp::Ne) return {};
  }
  // Comparing across types is a question ("is 1 == '1'?", "is nil == false?")
  // a language answers, not the VM. Refusing it keeps the VM from baking in an
  // answer a front end would then have to work around.
  return std::string("cannot ") + name_of(op) + " " + type_name(l.tag()) +
         " and " + type_name(r.tag());
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

// Only valid when binop_error returned empty.
inline Value apply_binop(BinOp op, const Value& l, const Value& r) {
  if (l.is_str()) {
    if (op == BinOp::Add) return Value::make_str(l.as_str() + r.as_str());
    return compare(op, l.as_str(), r.as_str());
  }
  if (l.is_bool()) return compare(op, l.as_bool(), r.as_bool());
  if (l.is_nil()) return Value::make_bool(op == BinOp::Eq);

  if (l.is_int() && r.is_int()) {
    const int64_t a = l.as_int();
    const int64_t b = r.as_int();
    switch (op) {
      case BinOp::Add: return Value::make_int(wrap_add(a, b));
      case BinOp::Sub: return Value::make_int(wrap_sub(a, b));
      case BinOp::Mul: return Value::make_int(wrap_mul(a, b));
      case BinOp::Div: return Value::make_int(a / b);
      case BinOp::Mod: return Value::make_int(a % b);
      default: return compare(op, a, b);
    }
  }

  const double a = l.as_number();
  const double b = r.as_number();
  switch (op) {
    case BinOp::Add: return Value::make_double(a + b);
    case BinOp::Sub: return Value::make_double(a - b);
    case BinOp::Mul: return Value::make_double(a * b);
    case BinOp::Div: return Value::make_double(a / b);  // inf/nan, not a trap
    default: return compare(op, a, b);
  }
}

inline std::string unop_error(UnOp op, const Value& v) {
  if (op == UnOp::Neg && !v.is_number()) {
    return std::string("cannot negate ") + type_name(v.tag());
  }
  return {};
}

inline Value apply_unop(UnOp, const Value& v) {
  return v.is_int() ? Value::make_int(wrap_neg(v.as_int()))
                    : Value::make_double(-v.as_double());
}

// Indexing, shared by the executor's Index and SetIndex so that "what can be
// indexed by what" is answered once. An out-of-range or wrong-typed key
// fails rather than yielding nil: a language that wants nil can ask the
// length first, while one that wants the error cannot recover it from a nil.
inline std::string index_error(const Value& recv, const Value& key) {
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
  return std::string("cannot index ") + type_name(recv.tag());
}

// A missing property reads as nil rather than failing, unlike a missing array
// element. The asymmetry is deliberate and matches what the two are for: an
// array index out of range is almost always a bug, while asking an object
// whether it has a key is how you find out.
inline Value index_get(const Value& recv, const Value& key) {
  if (recv.is_array()) {
    return recv.as_array()->items[static_cast<size_t>(key.as_int())];
  }
  const Value* v = recv.as_object()->find(key.as_str());
  return v ? *v : Value();
}

inline void index_set(const Value& recv, const Value& key, const Value& v) {
  if (recv.is_array()) {
    recv.as_array()->items[static_cast<size_t>(key.as_int())] = v;
    return;
  }
  recv.as_object()->set(key.as_str(), v);
}

inline std::string len_error(const Value& v) {
  if (v.is_array() || v.is_str() || v.is_object()) return {};
  return std::string("cannot take the length of ") + type_name(v.tag());
}

inline Value length_of(const Value& v) {
  if (v.is_str()) return Value::make_int(static_cast<int64_t>(v.as_str().size()));
  if (v.is_array()) {
    return Value::make_int(static_cast<int64_t>(v.as_array()->items.size()));
  }
  return Value::make_int(static_cast<int64_t>(v.as_object()->props.size()));
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
