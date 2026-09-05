// A slice of R7RS Scheme, and the smallest grammar in this repository by a
// wide margin -- an s-expression is four rules, where the other five front
// ends need forty to a hundred and fifty. That is most of the point: it
// shows how little of a front end is the *parser*, and how much of it is
// the binder.
//
// The other point is `call/cc`. The top-level README lists multi-shot
// continuations under "What stays out of reach, and why" -- "a coroutine is
// one-shot -- its parked frames move, they are not copied -- and Scheme's
// full `call/cc` would need a rule for what a copied cell means that
// nothing here has". Scheme is the one language that lets that boundary be
// shown rather than asserted: the *escape* half of `call/cc` is a
// `TryCatch` and a `Throw`, and works; the re-entrant half is what the
// sentence above is about. See README.md.
//
// `case` and `define-record-type` joined `cond`/`let`/etc. as special
// forms without changing a rule here -- both are ordinary lists to the
// grammar, and everything about what they mean is the binder's problem,
// which a homoiconic syntax is what makes that split so clean.
//
// There is no `no_ast_opt` anywhere below, and no rule that could fold
// wrongly: `list` keeps its identity because it is the only rule with
// parentheses, and every atom is a token. A grammar with nothing to get
// wrong is what a homoiconic language buys.

#pragma once

namespace mini_scheme {

inline constexpr const char* kGrammar = R"GRAMMAR(
  program  <- _ form* !.                                   { no_ast_opt }
  form     <- list / quoted / atom
  list     <- '(' _ form* ')' _                            { no_ast_opt }
  quoted   <- ['] _ form                                   { no_ast_opt }
  atom     <- boolean / float / number / string / symbol

  boolean  <- < '#' [tf] > ![a-zA-Z0-9] _
  float    <- < '-'? [0-9]+ '.' [0-9]* ([eE] [-+]? [0-9]+)? > _
  number   <- < '-'? [0-9]+ > ![a-zA-Z0-9.!?*/+<>=-] _
  string   <- ["] < (!["\\] . / '\\' .)* > ["] _
  symbol   <- < [^ \t\r\n()';"]+ > _

  ~_       <- ([ \t\r\n] / ';' [^\n]*)*
)GRAMMAR";

}  // namespace mini_scheme
