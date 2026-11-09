// escapes.h's primitives: the digit readers, the UTF-8 writer (Lua's, with
// its 5- and 6-byte forms), the one-letter table and the error record. The
// byte rows for append_utf8 are what lua5.4 writes for "\u{...}"; the
// per-language decoders built from these are covered by each front end's
// literals sample, which the real language prints the golden of.
//
// No VM: pure functions, so this runs in milliseconds.

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "escapes.h"

using namespace escapes;

namespace {

int g_failed = 0;

void expect(bool ok, const char* what) {
  if (ok) return;
  std::fprintf(stderr, "FAIL: %s\n", what);
  ++g_failed;
}

std::string hex_of(const std::string& s) {
  std::string r;
  char buf[4];
  for (unsigned char c : s) {
    std::snprintf(buf, sizeof buf, "%02x", c);
    r += buf;
  }
  return r;
}

void utf8(uint32_t cp, const char* want) {
  std::string out;
  append_utf8(out, cp);
  if (hex_of(out) == want) return;
  std::fprintf(stderr, "FAIL: append_utf8(%x): want %s, got %s\n", cp, want,
               hex_of(out).c_str());
  ++g_failed;
}

}  // namespace

int main() {
  utf8(0x41, "41");
  utf8(0x7F, "7f");
  utf8(0x80, "c280");
  utf8(0x7FF, "dfbf");
  utf8(0x800, "e0a080");
  utf8(0x20AC, "e282ac");
  utf8(0xD800, "eda080");  // a lone surrogate, as Lua and Python write it
  utf8(0xFFFF, "efbfbf");
  utf8(0x10000, "f0908080");
  utf8(0x10FFFF, "f48fbfbf");
  utf8(0x200000, "f888808080");
  utf8(0x3FFFFFF, "fbbfbfbfbf");
  utf8(0x4000000, "fc8480808080");
  utf8(0x7FFFFFFF, "fdbfbfbfbfbf");

  uint32_t v = 0;
  expect(digits("123x", 0, 10, 3, v) == 3 && v == 123, "decimal, three");
  expect(digits("1234", 0, 10, 3, v) == 3 && v == 123, "decimal stops at max");
  expect(digits("12a", 0, 10, 3, v) == 2 && v == 12, "decimal stops at a letter");
  expect(digits("x", 0, 10, 3, v) == 0 && v == 0, "no digit");
  expect(digits("ab", 0, 10, 3, v) == 0, "decimal does not read hex");
  expect(digits("777", 0, 8, 3, v) == 3 && v == 511, "octal");
  expect(digits("78", 0, 8, 3, v) == 1 && v == 7, "octal stops at 8");
  expect(digits("..ff", 2, 16, 8, v) == 2 && v == 255, "hex from an offset");
  expect(digits("fFaA", 0, 16, 8, v) == 4 && v == 0xFFAA, "hex, both cases");
  expect(digits("ffffffffffff", 0, 16, 64, v) == 12 && v == 0xFFFFFFFFu,
         "a long run saturates, it does not wrap");
  expect(digits("100000000", 0, 16, 64, v) == 9 && v > 0x7FFFFFFFu,
         "2^32 is past any code point");
  expect(digits("7fffffff", 0, 16, 64, v) == 8 && v == 0x7FFFFFFFu,
         "the largest Lua code point is exact");

  expect(hex_digits("4", 0, 2, 2, v) == 0, "hex_digits: too few is none");
  expect(hex_digits("41", 0, 2, 2, v) == 2 && v == 0x41, "hex_digits: two");
  expect(hex_digits("4g", 0, 2, 2, v) == 0, "hex_digits: a bad digit");
  expect(hex_digits("414", 0, 1, 2, v) == 2 && v == 0x41, "hex_digits: max");


  // UTF-16 units: a pair is one code point, a lone half is U+FFFD (what
  // node and .NET print for it).
  const auto utf16 = [&](uint32_t unit, std::string_view rest, const char* want,
                         size_t used, const char* what) {
    std::string out;
    size_t i = 0;
    append_utf16_unit(out, unit, rest, i);
    expect(hex_of(out) == want && i == used, what);
  };
  utf16(0x41, "", "41", 0, "utf16: a plain unit");
  utf16(0xD83D, "\\uDE00", "f09f9880", 6, "utf16: a pair");
  utf16(0xD83D, "\\uDE00x", "f09f9880", 6, "utf16: a pair, then more");
  utf16(0xD83D, "", "efbfbd", 0, "utf16: a lone high half");
  utf16(0xD83D, "\\u0041", "efbfbd", 0, "utf16: a high half, then a letter");
  utf16(0xDE00, "\\uDE00", "efbfbd", 0, "utf16: a lone low half");
  utf16(0xD83D, "\\uDE0", "efbfbd", 0, "utf16: a short second half");

  static constexpr Simple kT[] = {{'n', '\n'}, {'\n', kDrop}, {'q', 'Q'}};
  expect(simple_escape(kT, 'n') == '\n', "simple: n");
  expect(simple_escape(kT, '\n') == kDrop, "simple: a dropped letter");
  expect(simple_escape(kT, 'z') == kNotSimple, "simple: not in the table");

  Unescaper u("ab");
  expect(!u.done() && !u.failed(), "unescaper: fresh");
  u.fail_at(1, "first");
  u.fail_at(0, "second");
  expect(u.failed() && u.error == "first" && u.error_at == 1 && u.done(),
         "unescaper: the first error stays");

  if (g_failed != 0) {
    std::fprintf(stderr, "%d failures\n", g_failed);
    return 1;
  }
  std::puts("escapes OK");
  return 0;
}
