// literals.h's parse_int_literal: where an integer literal stops fitting an
// int64, and how the sign moves that edge. The languages agree on the edge
// (Lua 5.4 turns 9223372036854775808 into a float, Ruby and Scheme make a
// bignum of it, C#, Go and culebra refuse it; checked with each). They
// differ in what they do past it, which is the caller's business and is
// covered by each front end's literals sample and errors/ cases.
//
// No VM: a pure function of the text, so this runs in milliseconds.

#include <cstdint>
#include <cstdio>
#include <limits>

#include "literals.h"

using literals::has_leading_zero;
using literals::parse_int_literal;

namespace {

int g_failed = 0;

void check(const char* text, bool fits, int64_t value) {
  const auto r = parse_int_literal(text);
  if (r.fits == fits && (!fits || r.value == value)) return;
  std::fprintf(stderr, "FAIL: \"%s\": want %s %lld, got %s %lld\n", text,
               fits ? "fits" : "no fit", static_cast<long long>(value),
               r.fits ? "fits" : "no fit", static_cast<long long>(r.value));
  ++g_failed;
}

void zero(const char* text, bool want) {
  if (has_leading_zero(text) == want) return;
  std::fprintf(stderr, "FAIL: has_leading_zero(\"%s\"): want %d\n", text,
               want);
  ++g_failed;
}

constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
constexpr int64_t kMax = std::numeric_limits<int64_t>::max();

}  // namespace

int main() {
  check("0", true, 0);
  check("42", true, 42);
  check("9223372036854775807", true, kMax);
  // One past: strtoll clamped this to kMax; no longer.
  check("9223372036854775808", false, 0);
  check("18446744073709551615", false, 0);  // C#'s ulong max
  check("18446744073709551616", false, 0);  // overflows uint64
  check("99999999999999999999999", false, 0);
  // The sign is part of the range: -2^63 fits, -2^63-1 does not.
  check("-9223372036854775808", true, kMin);
  check("-9223372036854775809", false, 0);
  check("-0", true, 0);
  check("-5", true, -5);
  // Not digits.
  check("", false, 0);
  check("-", false, 0);
  check("12a", false, 0);
  check("--1", false, 0);
  check("+1", false, 0);
  check("1.5", false, 0);

  // A leading zero before more digits is octal in Ruby, Go and sloppy JS;
  // "00" is 0 in all of them, and so is "0".
  zero("0", false);
  zero("00", false);
  zero("000", false);
  zero("010", true);
  zero("-07", true);
  zero("08", true);
  zero("00.5", true);
  zero("0.5", false);
  zero("10", false);
  zero("0e1", false);
  zero("", false);

  if (g_failed != 0) {
    std::fprintf(stderr, "%d failures\n", g_failed);
    return 1;
  }
  std::puts("literals OK");
  return 0;
}
