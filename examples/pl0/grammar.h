// PL/0's grammar, carried over verbatim from culebra's examples/pl0/pl0.cul.
//
// The `no_ast_opt` annotations are load-bearing and only make sense together
// with optimize_ast, which the binder calls. cpp-peglib's own pl0.cc does not
// call it, so its grammar carries no annotations and its consumers descend
// through single-child nodes by hand -- the two reference implementations see
// genuinely different tree shapes, and mixing their idioms does not work.
//
//   * `{ no_ast_opt }` keeps a node whose own name carries the meaning. CALL x
//     would otherwise fold to a bare ident, indistinguishable from reading x.
//   * `sign?` rather than a `[-+]?` token that always matches: an unsigned
//     expression then has one child and folds away to the term itself.
//   * a rule whose parent reads children by position must not fold. `block` is
//     `const var procedure statement`, and `VAR w;` -- one variable, so one
//     child -- would fold the var node away and shift everything after it.

#pragma once

namespace pl0 {

inline constexpr const char* kGrammar = R"(
  program    <- _ block '.' _

  block      <- const var procedure statement
  const      <- ('CONST' __ ident '=' _ number (',' _ ident '=' _ number)* ';' _)?
  var        <- ('VAR' __ ident (',' _ ident)* ';' _)?      { no_ast_opt }
  procedure  <- ('PROCEDURE' __ ident ';' _ block ';' _)*

  statement  <- (assignment / call / statements / if / while / out / in)?
  assignment <- ident ':=' _ expression
  call       <- 'CALL' __ ident                             { no_ast_opt }
  statements <- 'BEGIN' __ statement (';' _ statement )* 'END' __
  if         <- 'IF' __ condition 'THEN' __ statement
  while      <- 'WHILE' __ condition 'DO' __ statement
  out        <- ('out' __ / 'write' __ / '!' _) expression  { no_ast_opt }
  in         <- ('in' __ / 'read' __ / '?' _) ident         { no_ast_opt }

  condition  <- odd / compare
  odd        <- 'ODD' __ expression                         { no_ast_opt }
  compare    <- expression compare_op expression
  compare_op <- < '=' / '#' / '<=' / '<' / '>=' / '>' > _

  expression <- sign? term (term_op term)*
  sign       <- < [-+] > _
  term_op    <- < [-+] > _

  term       <- factor (factor_op factor)*
  factor_op  <- < [*/] > _

  factor     <- ident / number / '(' _ expression ')' _

  ident      <- < [a-z] [a-z0-9]* > _
  number     <- < [0-9]+ > _

  ~_         <- [ \t\r\n]*
  ~__        <- ![a-z0-9_] _
)";

}  // namespace pl0
