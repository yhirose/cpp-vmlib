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
// 9223372036854775808 is not. A caller whose language negates a literal
// that was already read (Lua, culebra) passes negative = false and the
// negation happens on the value.

#pragma once

#include <charconv>
#include <cstdint>
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

// `text` is the digits in `base`, optionally led by '-' (Scheme's and Go's
// signed tokens). `negative` says the caller is folding a unary minus into
// the literal; a '-' in the text means the same. Anything that is not
// wholly digits (or overflows uint64) does not fit.
inline IntLiteral parse_int_literal(std::string_view text, int base = 10,
                                    bool negative = false) {
  if (!text.empty() && text.front() == '-') {
    negative = true;
    text.remove_prefix(1);
  }
  uint64_t mag = 0;
  const char* end = text.data() + text.size();
  const auto r = std::from_chars(text.data(), end, mag, base);
  if (text.empty() || r.ec != std::errc() || r.ptr != end) return {};
  constexpr uint64_t kMax =
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  if (mag > kMax + (negative ? 1 : 0)) return {};
  // Two's complement negation, so that 2^63 becomes INT64_MIN.
  const uint64_t bits = negative ? ~mag + 1 : mag;
  return {true, static_cast<int64_t>(bits)};
}

// A unary minus over a bare integer literal, folded into the literal: C# and
// Go read -9223372036854775808 that way (the constant is checked after the
// minus, not before). Answers nullopt when `operand` is not a bare integer
// literal, so there is nothing to fold; otherwise the literal read as
// negative, which `fits` or not like any other. `Ast` is the front end's
// peglib node.
template <class Ast>
std::optional<IntLiteral> fold_neg_const(const Ast& operand) {
  const std::string_view t = operand.token;
  if (operand.name != "number" || t.empty() ||
      t.find_first_not_of("0123456789") != std::string_view::npos) {
    return std::nullopt;
  }
  return parse_int_literal(t, 10, true);
}

}  // namespace literals
