// The pass that makes Python parseable by a PEG at all.
//
// Python's block structure is its indentation, and a PEG has no state to
// count columns with -- every other front end here is delimited by braces,
// by `end` or by parentheses, and needs nothing of the sort. So the source
// is normalized first: each logical line is preceded by an INDENT (0x01),
// or by as many DEDENTs (0x02) as it closes, and terminated by a NEWLINE
// marker (0x03). Those three bytes cannot occur in Python source, which is
// what makes the rewrite unambiguous.
//
// Two details are load-bearing:
//
// **Positions survive.** Every physical line is kept where it was -- a
// continuation's included -- with its own indentation, so a token's line
// and column are the programmer's. A line's INDENT takes the place of its
// indentation's last byte, so an unexpected indent is reported on that
// line; its DEDENTs go after the previous line's NEWLINE marker, since a
// dedent to column 0 has no byte to spare, so a block missing its body is
// reported at the end of its header. The grammar can afford all this
// because a statement ends at a NEWLINE marker, never at a '\n', so a
// '\n' is only whitespace to it.
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
  // Just past the last NEWLINE marker: where the next line's DEDENTs go.
  size_t eol = 0;
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
    size_t k = j;
    while (k < n) {
      const char c = src[k];
      if (c == '\n') {
        ++k;
        if (depth <= 0) break;
        // A continuation: the next physical line follows as it stands,
        // newline and indentation included -- whitespace to the grammar.
        text += c;
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
      // In place of the indentation's last byte -- an indented line has
      // one -- so the INDENT is on the line it opens and moves no column.
      stack.push_back(col);
      out.append(src, i, j - i - 1);
      out += '\x01';
    } else {
      // After the previous NEWLINE marker: a dedent to column 0 has no
      // byte to stand in for.
      std::string dedents;
      while (col < stack.back()) {
        stack.pop_back();
        dedents += '\x02';
      }
      out.insert(eol, dedents);
      out.append(src, i, j - i);
    }
    out += text;
    out += '\x03';
    eol = out.size();
    out += '\n';
    i = k;
  }

  while (stack.size() > 1) {
    stack.pop_back();
    out += '\x02';
  }
  return out;
}

}  // namespace mini_python
