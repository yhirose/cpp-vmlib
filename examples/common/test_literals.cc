// literals.h's parse_int_literal: where an integer literal stops fitting an
// int64, and how the sign moves that edge. The languages agree on the edge
// (Lua 5.4 turns 9223372036854775808 into a float, Ruby/Scheme make it a
// bignum, C#/Go/culebra refuse it; checked with each); they differ in what
// they do past it, which is the caller's business and is covered by each
// front end's literals sample and errors/ cases.
//
// No VM: a pure function of the text, so this runs in milliseconds.

#include <cstdint>
#include <cstdio>
#include <limits>

#include "literals.h"

using literals::parse_int_literal;

namespace {

int g_failed = 0;

void check(const char* text, int base, bool negative, bool fits, int64_t value) {
  const auto r = parse_int_literal(text, base, negative);
  if (r.fits == fits && (!fits || r.value == value)) return;
  std::fprintf(stderr, "FAIL: \"%s\" base %d negative %d: want %s %lld, got %s %lld\n",
               text, base, negative, fits ? "fits" : "no fit",
               static_cast<long long>(value), r.fits ? "fits" : "no fit",
               static_cast<long long>(r.value));
  ++g_failed;
}

constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
constexpr int64_t kMax = std::numeric_limits<int64_t>::max();

}  // namespace

int main() {
  check("0", 10, false, true, 0);
  check("42", 10, false, true, 42);
  check("9223372036854775807", 10, false, true, kMax);
  // One past: strtoll clamped this to kMax; no longer.
  check("9223372036854775808", 10, false, false, 0);
  check("18446744073709551615", 10, false, false, 0);  // C#'s ulong max
  check("18446744073709551616", 10, false, false, 0);  // overflows uint64
  check("99999999999999999999999", 10, false, false, 0);
  // The sign is part of the range: -2^63 fits, -2^63-1 does not.
  check("9223372036854775808", 10, true, true, kMin);
  check("9223372036854775809", 10, true, false, 0);
  check("-9223372036854775808", 10, false, true, kMin);  // a signed token
  check("-9223372036854775809", 10, false, false, 0);
  check("-0", 10, false, true, 0);
  check("-5", 10, false, true, -5);
  check("5", 10, true, true, -5);
  // Not digits.
  check("", 10, false, false, 0);
  check("-", 10, false, false, 0);
  check("12a", 10, false, false, 0);
  check("--1", 10, false, false, 0);
  check("+1", 10, false, false, 0);
  // Other bases.
  check("7fffffffffffffff", 16, false, true, kMax);
  check("8000000000000000", 16, false, false, 0);
  check("8000000000000000", 16, true, true, kMin);
  check("777", 8, false, true, 511);
  check("101", 2, false, true, 5);

  if (g_failed != 0) {
    std::fprintf(stderr, "%d failures\n", g_failed);
    return 1;
  }
  std::puts("literals OK");
  return 0;
}
