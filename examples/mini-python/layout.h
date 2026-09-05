// The pass that makes Python parseable by a PEG at all.
//
// Python's block structure is its indentation, and a PEG has no state to
// count columns with -- every other front end here is delimited by braces
// or by `end`, and needs nothing of the sort. So the source is normalized
// first: each logical line is stripped of its leading whitespace and
// preceded by an INDENT (0x01), or by as many DEDENTs (0x02) as it closes,
// and terminated by a NEWLINE marker (0x03). Those three bytes cannot
// occur in Python source, which is what makes the rewrite unambiguous.
//
// Two details are load-bearing:
//
// **Line numbers survive.** One '\n' is emitted per *physical* line
// consumed, so a diagnostic's line still points where the programmer
// looked -- which matters most for a continuation, where one logical line
// spans several physical ones.
//
// **Brackets and quotes are tracked while scanning.** A newline inside
// `(`, `[` or `{` continues the logical line rather than ending it, and a
// `#` or a bracket inside a string is text. Getting either wrong turns a
// well-formed program into a syntax error somewhere else entirely.

#pragma once

#include <string>
#include <vector>

namespace mini_python {

inline std::string layout(const std::string& src) {
  std::string out;
  std::vector<int> stack{0};
  const size_t n = src.size();
  size_t i = 0;

  while (i < n) {
    // The indentation of this physical line, tabs counted as 8 the way
    // Python's own tokenizer does.
    size_t j = i;
    int col = 0;
    while (j < n && (src[j] == ' ' || src[j] == '\t')) {
      col += src[j] == '\t' ? 8 : 1;
      ++j;
    }
    if (j >= n) break;
    // A blank or comment-only line produces no logical line, but it did
    // consume a physical one -- so the newline still has to be emitted, or
    // every diagnostic below it names the wrong line.
    if (src[j] == '\n') {  // blank
      out += '\n';
      i = j + 1;
      continue;
    }
    if (src[j] == '#') {  // comment only
      while (j < n && src[j] != '\n') ++j;
      out += '\n';
      i = j < n ? j + 1 : n;
      continue;
    }

    std::string text;
    int depth = 0;
    int newlines = 0;
    size_t k = j;
    while (k < n) {
      const char c = src[k];
      if (c == '\n') {
        ++newlines;
        ++k;
        if (depth <= 0) break;
        // A continuation: fold the next physical line onto this one.
        text += ' ';
        while (k < n && (src[k] == ' ' || src[k] == '\t')) ++k;
        continue;
      }
      if (c == '#') {  // a trailing comment, in or out of brackets
        while (k < n && src[k] != '\n') ++k;
        continue;
      }
      if (c == '\'' || c == '"') {
        const char q = c;
        text += c;
        ++k;
        while (k < n && src[k] != q) {
          if (src[k] == '\\' && k + 1 < n) {
            text += src[k];
            ++k;
          }
          text += src[k];
          ++k;
        }
        if (k < n) {
          text += src[k];
          ++k;
        }
        continue;
      }
      if (c == '(' || c == '[' || c == '{') ++depth;
      if (c == ')' || c == ']' || c == '}') --depth;
      text += c;
      ++k;
    }

    if (col > stack.back()) {
      stack.push_back(col);
      out += '\x01';
    } else {
      while (col < stack.back()) {
        stack.pop_back();
        out += '\x02';
      }
    }
    out += text;
    out += '\x03';
    for (int e = 0; e < (newlines == 0 ? 1 : newlines); ++e) out += '\n';
    i = k;
  }

  while (stack.size() > 1) {
    stack.pop_back();
    out += '\x02';
  }
  return out;
}

}  // namespace mini_python
