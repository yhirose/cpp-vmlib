// Integer literals, read once for every front end.
//
// The front ends used to call `strtoll`, which clamps a literal that does
// not fit to INT64_MAX without a word, so `9223372036854775808` quietly
// meant 9223372036854775807. What a too-big literal *is* differs by
// language -- Lua makes it a float, Ruby and Scheme a bignum, C#, Go and
// culebra refuse it -- so that stays with each binder; what is shared is
// the question "does this text fit an int64", answered exactly.
//
// The sign takes part in the range check: -9223372036854775808 is a valid
// literal in Ruby, Scheme, C# and Go (a language where the minus belongs to
// the literal, or a constant expression the compiler folds), though
// 9223372036854775808 is not. A language that negates a literal that was
// already read (Lua, culebra) never sees a '-' here, and negates the value.

#pragma once

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "vmlib.h"

namespace literals {

struct IntLiteral {
  bool fits = false;
  int64_t value = 0;  // meaningful only when `fits`
};

// The decimal digits of `text`, optionally led by '-' (Scheme's and Go's
// signed tokens). Anything that is not wholly digits, or overflows the
// range, does not fit.
inline IntLiteral parse_int_literal(std::string_view text) {
  const bool negative = !text.empty() && text.front() == '-';
  if (negative) text.remove_prefix(1);
  uint64_t mag = 0;
  const char* end = text.data() + text.size();
  const auto r = std::from_chars(text.data(), end, mag);
  if (text.empty() || r.ec != std::errc() || r.ptr != end) return {};
  constexpr uint64_t kMax =
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  if (mag > kMax + (negative ? 1 : 0)) return {};
  // Two's complement negation, so that 2^63 becomes INT64_MIN.
  const uint64_t bits = negative ? ~mag + 1 : mag;
  return {true, static_cast<int64_t>(bits)};
}

// A float literal's value. strtod, and not a locale-free parse: the binaries
// never set a locale, so its decimal point is '.', and this is the one place
// that says so.
inline double parse_float_literal(std::string_view text) {
  return std::strtod(std::string(text).c_str(), nullptr);
}

// The refusal of an integer literal past int64 in a language whose integers
// stop there (Ruby, Scheme, C#): this front end has no bignum.
inline std::string no_bignum_message(std::string_view text) {
  return "integer literal " + std::string(text) +
         " is not supported here (no Bignum)";
}

// A refusal at a literal node, worded by the caller (a message is the
// language's, or the mini's own; the position is the literal's).
template <class Ast>
[[noreturn]] void refuse(const Ast& node, const std::string& msg) {
  coreir_rt::fail(msg, static_cast<uint32_t>(node.line),
                  static_cast<uint32_t>(node.column));
}

// The value of an integer literal token, or a refusal with `msg` when it
// does not fit an int64.
template <class Ast>
int64_t check_int(const Ast& node, std::string_view text,
                  const std::string& msg) {
  const IntLiteral n = parse_int_literal(text);
  if (!n.fits) refuse(node, msg);
  return n.value;
}

// A digit run led by a zero and then more digits (010, -07, 08, 00.5): octal
// in Ruby, Go and sloppy JavaScript, a SyntaxError in Python, and read as
// decimal in Lua, C#, Scheme and culebra. Only the all-zero run ("00") is
// not one: every language reads it as 0.
inline bool has_leading_zero(std::string_view text) {
  if (!text.empty() && text.front() == '-') text.remove_prefix(1);
  return text.size() > 1 && text[0] == '0' && text[1] >= '0' &&
         text[1] <= '9' &&
         text.find_first_not_of('0') != std::string_view::npos;
}

// A refusal of a leading-zero literal, with `msg`.
template <class Ast>
void check_no_leading_zero(const Ast& node, std::string_view text,
                           const std::string& msg) {
  if (has_leading_zero(text)) refuse(node, msg);
}

// A unary minus over a bare integer literal, folded into the literal: C# and
// Go read -9223372036854775808 that way (the range is checked after the
// minus). Answers nullopt when `operand` is not a bare integer literal, so
// there is nothing to fold; otherwise the folded value, or a refusal with
// `msg` when it does not fit.
template <class Ast>
std::optional<int64_t> fold_neg_const(const Ast& operand,
                                      const std::string& msg) {
  if (operand.name != "number") return std::nullopt;
  return check_int(operand, "-" + std::string(operand.token), msg);
}

}  // namespace literals
