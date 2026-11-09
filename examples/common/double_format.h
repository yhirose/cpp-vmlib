// How each front end's language spells a double -- 1e-05 or 1e-5, 1.0e+16
// or 1E+16, 100000 or 100000.0, inf or Infinity or +inf.0 -- held once, as
// a table of styles, and one formatter every style goes through.
//
// vmlib.h's ToStr (to_display) prints the shortest round-trip digits and
// picks fixed or exponent by whichever is shorter, which is no language's
// rule, and says that how a double prints is the language's decision. So a
// front end does not ask ToStr for a double: it registers the host function
// dstr with its style (`double_format::dstr_def(kPython)` among its natives)
// and calls it, and its `describe` shows a raised double with display_with,
// so an uncaught `raise 1e16` words the number as print does.
//
// A float32 (Go's) is a call-site native, f32str, and not a flag on the
// value: a Value holds a double, and the front end that has float32s knows
// the static type where it prints, so nothing in the VM has to carry it.
//
// The digits are the shortest that round-trip (std::to_chars in its
// scientific form, one call), or, for a style with max_digits, that many
// significant digits with the trailing zeros dropped (Lua's "%.14g").
// What a style decides is the layout:
//
//   * x is the decimal exponent of the first digit (1e5 has x = 5). The
//     exponent form is used when x < lo or x >= hi. With pad_zeros (Guile),
//     hi is raised to the digit count k plus pad_zeros: at most `hi`
//     integer digits, or at most pad_zeros zeros padded onto the digits.
//   * the exponent is written exp_char, a sign (always with exp_plus, only
//     a minus without), and at least exp_min_digits digits.
//   * sci_point: a one-digit mantissa still gets ".0" (Ruby's 1.0e+16).
//   * whole_point: a whole number in fixed form gets ".0" (Python's 100000.0).
//   * -0.0 is "-" and the zero's own text, unless neg_zero_sign is false
//     (JavaScript's String(-0) is "0"; console.log(-0) is kJsInspect's).
//   * -inf is "-" and inf without its own leading "+" ("+Inf" -> "-Inf").
//
// A float32 uses hi_f32 for hi when it is not 0.
//
// Each style was held to the real language over 500 doubles (a decade
// grid and random values); the rows of test_double_format.cc are the
// pinned ones. What is not pinned is what differs between versions or
// machines: a nan's sign (Lua prints "-nan" on one machine and "nan" on
// another; every style here says its nan without one), C#'s
// culture-dependent infinity symbol.

#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

#include "vmlib.h"

namespace double_format {

struct DoubleStyle {
  int lo;
  int hi;
  int hi_f32;       // 0: the same as hi
  int pad_zeros;    // 0: off
  int max_digits;   // 0: the shortest that round-trips
  char exp_char;
  bool exp_plus;
  int exp_min_digits;
  bool sci_point;
  bool whole_point;
  bool neg_zero_sign;
  const char* nan;
  const char* inf;
};

//                                  lo  hi f32 pad dig ec   plus min spt   wpt   nz
inline constexpr DoubleStyle kPython{-4, 16, 0, 0, 0, 'e', true, 2, false, true, true,
                                     "nan", "inf"};
inline constexpr DoubleStyle kRuby{-4, 15, 0, 0, 0, 'e', true, 2, true, true, true,
                                   "NaN", "Infinity"};
// String(x), template literals and `+`: -0 is "0". console.log of a bare
// number is kJsInspect: the same, but -0 is "-0".
inline constexpr DoubleStyle kJs{-6, 21, 0, 0, 0, 'e', true, 1, false, false, false,
                                 "NaN", "Infinity"};
inline constexpr DoubleStyle kJsInspect{-6, 21, 0, 0, 0, 'e', true, 1, false, false, true,
                                        "NaN", "Infinity"};
inline constexpr DoubleStyle kCSharp{-4, 17, 9, 0, 0, 'E', true, 2, false, false, true,
                                     "NaN", "Infinity"};
inline constexpr DoubleStyle kGo{-4, 6, 0, 0, 0, 'e', true, 2, false, false, true,
                                 "NaN", "+Inf"};
inline constexpr DoubleStyle kCulebra{-4, 6, 0, 0, 0, 'e', true, 2, false, true, true,
                                      "nan", "inf"};
inline constexpr DoubleStyle kGuile{-3, 7, 0, 3, 0, 'e', false, 1, true, true, true,
                                    "+nan.0", "+inf.0"};
// "%.14g", and ".0" on what then looks like an integer.
inline constexpr DoubleStyle kLua{-4, 14, 0, 0, 14, 'e', true, 2, false, true, true,
                                  "nan", "inf"};

namespace detail {

template <class F>
std::string layout(F v, const DoubleStyle& s) {
  if (std::isnan(v)) return s.nan;  // its sign bit is not part of its value
  const bool neg = std::signbit(v);
  std::string out = neg ? "-" : "";
  if (std::isinf(v)) {
    if (!neg) return s.inf;
    const std::string_view inf = s.inf;
    return out + std::string(inf.substr(inf.front() == '+' ? 1 : 0));
  }
  if (v == 0) {
    if (neg && !s.neg_zero_sign) out.clear();
    return out + (s.whole_point ? "0.0" : "0");
  }

  // "d.ddde+XX": the first digit, the rest, the exponent.
  char buf[64];
  const auto r = s.max_digits != 0
                     ? std::to_chars(buf, buf + sizeof buf, v,
                                     std::chars_format::scientific, s.max_digits - 1)
                     : std::to_chars(buf, buf + sizeof buf, v,
                                     std::chars_format::scientific);
  const char* p = buf + (neg ? 1 : 0);
  const char* e = std::find(p, static_cast<const char*>(r.ptr), 'e');
  const char first = *p;
  std::string_view rest(p + 1, static_cast<size_t>(e - p - 1));  // ".ddd" or ""
  if (!rest.empty()) rest.remove_prefix(1);
  if (s.max_digits != 0) {
    while (!rest.empty() && rest.back() == '0') rest.remove_suffix(1);
  }
  int x = 0;
  std::from_chars(e + 1 + (e[1] == '+' ? 1 : 0), static_cast<const char*>(r.ptr), x);
  const int k = 1 + static_cast<int>(rest.size());
  const auto digit = [&](int i) { return i == 0 ? first : rest[static_cast<size_t>(i - 1)]; };

  int hi = std::is_same_v<F, float> && s.hi_f32 != 0 ? s.hi_f32 : s.hi;
  if (s.pad_zeros != 0) hi = std::max(hi, k + s.pad_zeros);
  if (x < s.lo || x >= hi) {
    out += first;
    if (k > 1) {
      out += '.';
      out += rest;
    } else if (s.sci_point) {
      out += ".0";
    }
    out += s.exp_char;
    if (x < 0) {
      out += '-';
    } else if (s.exp_plus) {
      out += '+';
    }
    char eb[8];
    const auto en = std::to_chars(eb, eb + sizeof eb, std::abs(x));
    for (int pad = s.exp_min_digits - static_cast<int>(en.ptr - eb); pad > 0; --pad) {
      out += '0';
    }
    out.append(eb, en.ptr);
    return out;
  }
  const int point = x + 1;  // digits before the decimal point
  if (point <= 0) {
    out += "0.";
    out.append(static_cast<size_t>(-point), '0');
    for (int i = 0; i < k; ++i) out += digit(i);
  } else if (point >= k) {
    for (int i = 0; i < k; ++i) out += digit(i);
    out.append(static_cast<size_t>(point - k), '0');
    if (s.whole_point) out += ".0";
  } else {
    for (int i = 0; i < k; ++i) {
      if (i == point) out += '.';
      out += digit(i);
    }
  }
  return out;
}

}  // namespace detail

inline std::string format_double(double v, const DoubleStyle& s) {
  return detail::layout<double>(v, s);
}

// A double that holds a float32 (Go's float32 is a double rounded through
// ToFloat32): the digits that float needs, not the double's.
inline std::string format_float32(double v, const DoubleStyle& s) {
  return detail::layout<float>(static_cast<float>(v), s);
}

// to_display, with a double spelled by the style: what a front end's
// describe words a raised value with, and what dstr_native answers.
inline std::string display_with(const coreir::Value& v, const DoubleStyle& s) {
  return v.is_double() ? format_double(v.as_double(), s) : coreir::to_display(v);
}

// The host functions. The style is the NativeDef's ctx. dstr takes a
// double; an int arriving here is printed as an int, so a helper may hand
// it any number.
inline bool dstr_native(coreir::NativeCall& c) {
  c.result = coreir::Value::make_str(
      display_with(c.arg(0), *static_cast<const DoubleStyle*>(c.ctx)));
  return true;
}
inline bool f32str_native(coreir::NativeCall& c) {
  const coreir::Value& v = c.arg(0);
  c.result = coreir::Value::make_str(
      v.is_double()
          ? format_float32(v.as_double(), *static_cast<const DoubleStyle*>(c.ctx))
          : coreir::to_display(v));
  return true;
}

// The NativeDef of one of those, with the style as its ctx. A style is a
// constant object that outlives every run, and NativeDef::ctx is the
// untyped pointer a host function reads. `dstr_def(kPython)` is the one
// line a front end registers; a language with a second surface names it
// (JavaScript's "inspectnum").
inline vm::NativeDef native_def(std::string name, coreir::NativeFn fn,
                                const DoubleStyle& style) {
  return {std::move(name), 1, fn, const_cast<DoubleStyle*>(&style)};
}
inline vm::NativeDef dstr_def(const DoubleStyle& style, std::string name = "dstr") {
  return native_def(std::move(name), dstr_native, style);
}
inline vm::NativeDef f32str_def(const DoubleStyle& style) {
  return native_def("f32str", f32str_native, style);
}

}  // namespace double_format
