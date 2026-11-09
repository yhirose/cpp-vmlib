// The pieces of a string-literal decoder, shared by the front ends.
//
// Every language spells its escapes differently, and not just in the
// letters: Lua has `\z` and decimal `\ddd`, Ruby `\u{41 42}` and `\c`, C#
// takes one to four hex digits for `\x`, JavaScript stops an octal escape
// when the value would pass 0377, Go's `\x` is a byte where Python's is a
// code point. A table cannot say all that without growing into a language
// of its own, so each front end keeps its decoder, a switch of a few lines
// in its own binder, and what is shared here is what every one of them
// repeats: reading digits, encoding a code point, the table of one-letter
// escapes, and an error that remembers where it was found.
//
// An error is recorded, not thrown: the binder turns it into its own
// diagnostic (`fail(node, message)`) at the literal.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace escapes {

// One-letter escapes: `\n` is a newline, `\\` a backslash. A language lists
// its own; `kDrop` is for a letter that stands for nothing (a line
// continuation).
struct Simple {
  char letter;
  int value;  // a byte, or kDrop
};
inline constexpr int kDrop = -1;

template <size_t N>
inline int simple_escape(const Simple (&table)[N], char letter) {
  for (const Simple& s : table) {
    if (s.letter == letter) return s.value;
  }
  return -2;  // not one of this language's
}
inline constexpr int kNotSimple = -2;

inline int digit_value(char c, int radix) {
  int d = -1;
  if (c >= '0' && c <= '9') {
    d = c - '0';
  } else if (c >= 'a' && c <= 'f') {
    d = c - 'a' + 10;
  } else if (c >= 'A' && c <= 'F') {
    d = c - 'A' + 10;
  }
  return d < radix ? d : -1;
}

// Up to `max` digits of `radix` at raw[i...], accumulated into `val`
// (saturating well above any code point). Answers how many were read; the
// caller decides whether fewer than its minimum is an error.
inline size_t digits(std::string_view raw, size_t i, int radix, size_t max,
                     uint32_t& val) {
  size_t n = 0;
  val = 0;
  while (n < max && i + n < raw.size()) {
    const int d = digit_value(raw[i + n], radix);
    if (d < 0) break;
    // Saturates at 0xFFFFFFFF, past any value a decoder accepts, so a long
    // run of digits cannot wrap around into a small one.
    val = val > 0x0FFFFFFFu ? 0xFFFFFFFFu
                            : val * static_cast<uint32_t>(radix) + d;
    ++n;
  }
  return n;
}

inline size_t hex_digits(std::string_view raw, size_t i, size_t min,
                         size_t max, uint32_t& val) {
  const size_t n = digits(raw, i, 16, max, val);
  return n < min ? 0 : n;
}

// A code point as UTF-8, any value up to 0x7FFFFFFF the way Lua writes it
// (the 5- and 6-byte forms UTF-8 itself no longer has).
inline void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x200000) {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x4000000) {
    out.push_back(static_cast<char>(0xF8 | (cp >> 24)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 18) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xFC | (cp >> 30)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 24) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 18) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// A literal being decoded: the raw text between the quotes, a cursor, the
// bytes so far and the first error. `fail_at` records it once; the decoder
// stops when `failed()`.
struct Unescaper {
  std::string_view raw;
  size_t i = 0;
  std::string out;
  std::string error;
  size_t error_at = 0;

  explicit Unescaper(std::string_view r) : raw(r) {}

  bool failed() const { return !error.empty(); }
  bool done() const { return i >= raw.size() || failed(); }
  void fail_at(size_t offset, std::string msg) {
    if (failed()) return;
    error = std::move(msg);
    error_at = offset;
  }
};

}  // namespace escapes
