// How each front end's language spells a double -- 1e-05 or 1e-5, 1.0e+16
// or 1E+16, 100000 or 100000.0, inf or Infinity or +inf.0 -- held once, as
// a table of styles, and one formatter every style goes through.
//
// vmlib.h's ToStr (to_display) prints the shortest round-trip digits and
// picks fixed or exponent by whichever is shorter, which is no language's
// rule, and says that how a double prints is the language's decision. So a
// front end does not ask ToStr for a double: it calls the host function
// dstr_native with its style as the ctx (`{"dstr", 1, dstr_native,
// &kPython}` in its natives), and its `describe` shows a raised double with
// display_with, so an uncaught `raise 1e16` words the number as print does.
//
// The digits are always the shortest that round-trip (std::to_chars in its
// scientific form, one call); what a style decides is the layout:
//
//   * x is the decimal exponent of the first digit (1e5 has x = 5). The
//     exponent form is used when x < lo or x >= hi -- and for Guile, whose
//     cutoff also depends on the digit count k, when x + 1 > max(hi, k +
//     pad_zeros): at most `hi` integer digits, or at most `pad_zeros`
//     zeros padded onto the digits.
//   * the exponent is written exp_char, a sign (always with exp_plus,
//     only a minus without), and at least exp_min_digits digits.
//   * sci_point: a one-digit mantissa still gets ".0" (Ruby's 1.0e+16).
//   * whole_point: a whole number in fixed form gets ".0" (Python's 100000.0).
//
// A float32 (Go's) shortens to the digits of the float and uses hi_f32.
//
// Each style was held to the real language over 500 doubles (a decade
// grid and random values); the rows of test_double_format.cc are the
// pinned ones. What is not pinned is what differs between versions or
// machines: Lua's nan sign, C#'s culture-dependent infinity symbol.

#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "vmlib.h"

namespace double_format {

enum class Cutoff : unsigned char {
  Exponent,  // x < lo || x >= hi
  Digits,    // x < lo || x + 1 > max(hi, k + pad_zeros)   (Guile)
};

struct DoubleStyle {
  int lo;
  int hi;
  int hi_f32;
  Cutoff cutoff;
  int pad_zeros;
  char exp_char;
  bool exp_plus;
  int exp_min_digits;
  bool sci_point;
  bool whole_point;
  const char* nan;
  const char* inf;
  const char* neg_inf;
  const char* neg_zero;
};

//                                  lo  hi hi32 cutoff           pad ec   plus min spt   wpt
inline constexpr DoubleStyle kPython{-4, 16, 16, Cutoff::Exponent, 0, 'e', true, 2, false, true,
                                     "nan", "inf", "-inf", "-0.0"};
inline constexpr DoubleStyle kRuby{-4, 15, 15, Cutoff::Exponent, 0, 'e', true, 2, true, true,
                                   "NaN", "Infinity", "-Infinity", "-0.0"};
// String(x), template literals and `+`: -0 is "0". console.log of a bare
// number is kJsInspect: the same, but -0 is "-0".
inline constexpr DoubleStyle kJs{-6, 21, 21, Cutoff::Exponent, 0, 'e', true, 1, false, false,
                                 "NaN", "Infinity", "-Infinity", "0"};
inline constexpr DoubleStyle kJsInspect{-6, 21, 21, Cutoff::Exponent, 0, 'e', true, 1, false, false,
                                        "NaN", "Infinity", "-Infinity", "-0"};
inline constexpr DoubleStyle kCSharp{-4, 17, 9, Cutoff::Exponent, 0, 'E', true, 2, false, false,
                                     "NaN", "Infinity", "-Infinity", "-0"};
inline constexpr DoubleStyle kGo{-4, 6, 6, Cutoff::Exponent, 0, 'e', true, 2, false, false,
                                 "NaN", "+Inf", "-Inf", "-0"};
inline constexpr DoubleStyle kCulebra{-4, 6, 6, Cutoff::Exponent, 0, 'e', true, 2, false, true,
                                      "nan", "inf", "-inf", "-0.0"};
inline constexpr DoubleStyle kGuile{-3, 7, 7, Cutoff::Digits, 3, 'e', false, 1, true, true,
                                    "+nan.0", "+inf.0", "-inf.0", "-0.0"};

namespace detail {

template <class F>
std::string layout(F v, const DoubleStyle& s) {
  if (std::isnan(v)) return s.nan;  // its sign bit is not part of its value
  if (std::isinf(v)) return v < 0 ? s.neg_inf : s.inf;
  if (v == 0) return std::signbit(v) ? s.neg_zero : (s.whole_point ? "0.0" : "0");

  char buf[64];
  const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);
  std::string_view t(buf, static_cast<size_t>(r.ptr - buf));
  const bool neg = t.front() == '-';
  if (neg) t.remove_prefix(1);
  const size_t e = t.find('e');
  const int x = std::atoi(std::string(t.substr(e + 1)).c_str());
  std::string digits;
  for (size_t i = 0; i < e; ++i) {
    if (t[i] != '.') digits += t[i];
  }
  const int k = static_cast<int>(digits.size());

  int hi = std::is_same_v<F, float> ? s.hi_f32 : s.hi;
  if (s.cutoff == Cutoff::Digits) hi = std::max(hi, k + s.pad_zeros);
  std::string out = neg ? "-" : "";
  if (x < s.lo || (s.cutoff == Cutoff::Digits ? x + 1 > hi : x >= hi)) {
    out += digits[0];
    if (k > 1) {
      out += '.';
      out.append(digits, 1, std::string::npos);
    } else if (s.sci_point) {
      out += ".0";
    }
    out += s.exp_char;
    if (x < 0) {
      out += '-';
    } else if (s.exp_plus) {
      out += '+';
    }
    std::string ed = std::to_string(std::abs(x));
    if (static_cast<int>(ed.size()) < s.exp_min_digits) {
      ed.insert(0, static_cast<size_t>(s.exp_min_digits) - ed.size(), '0');
    }
    return out + ed;
  }
  const int point = x + 1;  // digits before the decimal point
  if (point <= 0) {
    out += "0.";
    out.append(static_cast<size_t>(-point), '0');
    out += digits;
  } else if (point >= k) {
    out += digits;
    out.append(static_cast<size_t>(point - k), '0');
    if (s.whole_point) out += ".0";
  } else {
    out.append(digits, 0, static_cast<size_t>(point));
    out += '.';
    out.append(digits, static_cast<size_t>(point), std::string::npos);
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

// The NativeDef of one of those, with the style as its ctx -- `native_def(
// "dstr", dstr_native, kPython)`. A style is a constant object that outlives
// every run, and NativeDef::ctx is the untyped pointer a host function reads.
inline vm::NativeDef native_def(std::string name, coreir::NativeFn fn,
                                const DoubleStyle& style) {
  return {std::move(name), 1, fn, const_cast<DoubleStyle*>(&style)};
}

}  // namespace double_format
