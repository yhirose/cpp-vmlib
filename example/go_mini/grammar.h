// A small enough slice of Go syntax that a real Go toolchain (`go run`)
// accepts every sample verbatim -- the oracle this front end's tests check
// against is the real language, not a second implementation of it. What it
// covers: top-level funcs with typed params/return, `var` with an explicit
// fixed-width or float type, assignment, `return`, `fmt.Println`, arithmetic/
// comparison/type-conversion expressions, `type ... struct` declarations with
// field access/assignment (`p.X`, `p.X = v`), `switch` over an int subject,
// `if`/`else` and the condition-only `for`, and goroutines with unbuffered
// channels: `go f(args)`, `chan T` as a var/param type, `make(chan T)`,
// `ch <- v` and `<-ch` (as an expression or a statement). No methods, no
// multiple return values, no `select`, no buffered channels -- this front
// end exists to exercise vmlib's Fixed-width integers, `float`, Static
// calls, Struct fields, Switch and (with goroutines) Coroutines + scheduler
// recipes (see the top-level README), not to be a Go implementation.
//
// `if x {` with a bare identifier as the condition is a syntax error here:
// `structlit` (tried ahead of `primary`) eats `x {` as a struct literal
// first, the same pitfall the `switchstmt` note below describes. Real Go
// has a rule against a composite literal in that position; this grammar
// does not, so a sample compares or parenthesizes instead.
//
// Every rule that can end up with exactly one child needs `no_ast_opt`,
// or optimize_ast folds it away and its identity (or its list shape) is
// lost -- see example/pl0/grammar.h's own comment for the canonical case
// this bites (`CALL x` folding to a bare `x`). The rules here that have
// that shape, beyond the ones slice 1 already needed:
//   * `defaultcase` has exactly one child (its `stmts`), so without
//     `no_ast_opt` it would fold into a plain "stmts" node, and a
//     switchstmt's own children could no longer tell its default arm apart
//     from a `case`'s body by tag.
//   * `intlist` folds to its one `caseval` when a case has exactly one
//     value (`case 1:`) unless marked -- the input `case 1, 2:` would need
//     it least reliably, since only the multi-value form happens to keep
//     the list shape on its own.
//   * `fieldinits` is a list that can be empty or hold exactly one
//     `fieldinit`; either way the binder wants a stable "fieldinits" node
//     to walk, the same reason `args`/`params` are marked in slice 1.
//
// `caseval`, not `number`, is what a case value parses as: `number` has no
// sign (a source-level negative goes through `neg` wrapping a `unary`, one
// layer `intlist` does not reach), but Go's own `case -1:` needs one --
// parse_int's `strtoll` already accepts the leading '-' `caseval` captures,
// so only the grammar was missing it.
//   * `switchstmt` itself is marked defensively rather than for a shape
//     that occurs: `switch x {}` (no cases, no default) would be the
//     one-child case, but `structlit` (tried ahead of `primary` in
//     `unary`) eats `x {}` as a struct literal before `switchstmt` ever
//     sees its own brace, so that form is a syntax error here and never
//     reaches optimize_ast -- real Go accepts it (a composite literal is
//     not allowed as a control-clause expression there, a rule this
//     grammar does not have), but no sample needs it, so the mark stays
//     for whichever future case does turn out to have one child rather
//     than being load-bearing today.

#pragma once

namespace go_mini {

inline constexpr const char* kGrammar = R"(
  program    <- _ 'package' __ 'main' _ 'import' __ '"fmt"' _ topdecl* _   { no_ast_opt }
  topdecl    <- structdecl / func

  structdecl <- 'type' __ ident _ 'struct' _ '{' _ fields '}' _
  fields     <- field*                                       { no_ast_opt }
  field      <- ident type

  func       <- 'func' __ ident '(' _ params ')' _ type? '{' _ stmts '}' _
  params     <- (param (',' _ param)*)?                     { no_ast_opt }
  param      <- ident type
  type       <- < ('chan' __)? ('int32' / 'int64' / 'uint32' / 'float32'
                  / 'float64' / 'bool' / [a-zA-Z_][a-zA-Z0-9_]*) > _

  stmts      <- stmt*                                       { no_ast_opt }
  stmt       <- vardecl / assign / ret / print / switchstmt / gostmt
              / sendstmt / recvstmt / forstmt / ifstmt

  vardecl    <- 'var' __ ident type '=' _ expr
  assign     <- ident ('.' ident)* '=' _ expr
  ret        <- 'return' __ expr                             { no_ast_opt }
  print      <- 'fmt.Println' _ '(' _ expr ')' _              { no_ast_opt }

  gostmt     <- 'go' __ call                                  { no_ast_opt }
  sendstmt   <- ident '<-' _ expr
  recvstmt   <- '<-' _ ident                                  { no_ast_opt }
  forstmt    <- 'for' __ expr '{' _ stmts '}' _
  ifstmt     <- 'if' __ expr '{' _ stmts '}' _ ('else' _ '{' _ stmts '}' _)?

  switchstmt <- 'switch' __ expr '{' _ case* defaultcase? '}' _   { no_ast_opt }
  case       <- 'case' __ intlist ':' _ stmts
  intlist    <- caseval (',' _ caseval)*                      { no_ast_opt }
  caseval    <- < '-'? [0-9]+ > _
  defaultcase <- 'default' _ ':' _ stmts                      { no_ast_opt }

  expr       <- equality
  equality   <- relational (eqop relational)*
  eqop       <- < '==' / '!=' > _
  relational <- additive (relop additive)*
  relop      <- < '<=' / '>=' / '<' / '>' > _
  additive   <- multiplicative (addop multiplicative)*
  addop      <- < [-+] > _
  multiplicative <- unary (mulop unary)*
  mulop      <- < [*/] > _
  unary      <- neg / recv / makechan / structlit / fieldaccess / call / primary
  neg        <- '-' _ unary                                   { no_ast_opt }
  recv       <- '<-' _ ident                                  { no_ast_opt }
  makechan   <- 'make' _ '(' _ type ')' _                      { no_ast_opt }
  structlit  <- ident '{' _ fieldinits '}' _
  fieldinits <- (fieldinit (',' _ fieldinit)*)?                { no_ast_opt }
  fieldinit  <- ident ':' _ expr
  fieldaccess <- ident ('.' ident)+
  call       <- ident '(' _ args ')' _
  args       <- (expr (',' _ expr)*)?                         { no_ast_opt }
  primary    <- number / ident / '(' _ expr ')' _

  number     <- < [0-9]+ ('.' [0-9]+)? > _
  ident      <- < [a-zA-Z_] [a-zA-Z0-9_]* > _

  ~_         <- ([ \t\r\n] / '//' [^\n]*)*
  ~__        <- ![a-zA-Z0-9_] _
)";

// The word-boundary check `__` only means anything right after a bare
// keyword literal ('func', 'var', 'return', ...): the literal itself
// consumes no trailing space, so `__`'s lookahead still sees whatever comes
// right after the keyword's own last character. `ident`/`type`/`number`
// already end in a plain `_` (their own `<...> _`), so by the time
// anything after one of them runs, the separator is already gone and
// `__`'s lookahead would be looking at the *next* token's first
// character -- always failing. `param <- ident type` and `vardecl <- 'var'
// __ ident type ...` want nothing there at all, not `__`: `ident`'s own
// trailing `_` is already the separator. `structdecl` had exactly this bug
// on its first pass (`'type' __ ident __ 'struct' ...`, where a plain `_`
// -- not nothing -- really was missing, since `'struct'` is a keyword
// literal rather than another `ident`), caught by every sample failing to
// parse its own `type ... struct` line. (A `//`-style comment cannot live
// inside
// the grammar string above for the same reason this one is out here:
// peglib's own grammar DSL comments with `#`, not `//` -- `//` here would
// parse as grammar text, not a comment, and fail to load.)

}  // namespace go_mini
