// A slice of culebra -- the language this library was written to rehearse
// a back end for, and the one front end here whose oracle is the language's
// own implementation rather than a third party's. Every sample runs
// unmodified under `culebra`, and the golden files were captured that way.
//
// What it covers: `let`/`mut` bindings, `fn` declarations and anonymous
// `fn`/`|x|` lambdas, closures, `class` with `new`, methods and `drop`,
// objects and arrays, `if`/`else` and `try`/`catch` as *expressions*,
// `while`, `for ... in`, `break`/`continue`, `return`, `throw`, `defer`,
// `yield` and `yield from`, and interpolated `"{...}"` strings. What it
// does not: traits, enums, multimethods, decorators, effects, pattern
// matching, destructuring, the module system, `match`/`cond`, statement
// modifiers (`x if c`), loop labels, format specs, sets, regexes, the
// null-safe operators, and `dispose()` from the custom iterator protocol
// (`iter`/`has_next`/`next` are reached; `dispose` is not, and README.md
// says why) -- see README.md.
//
// Four things about this grammar are worth knowing before editing it.
//
// **`__` only means anything right after a bare keyword literal.** It is
// `![a-zA-Z0-9_] _`, and `ident` already ends in a `_` of its own -- so by
// the time a `__` written after one runs, the separator is gone and the
// lookahead is testing the *next* token's first character, which always
// fails. `forstmt` had exactly this bug on its first pass (`'for' __ ident
// __ 'in'`, where the `__` before `'in'` could never match), and the whole
// loop fell through to three expression statements named `for`, `x` and
// `in` rather than failing outright. examples/mini-go/grammar.h documents
// the same trap; it is the one this family of grammars keeps re-finding.
//
// **A newline is whitespace here, and a statement separator in culebra.**
// culebra's own grammar (misc/culebra.peg) threads `_sp_` and `_nl_`
// through every rule so that `a\n-b` is two statements rather than a
// subtraction. This one does not, which means it accepts programs culebra
// rejects and could in principle read one of them differently. That is a
// safe simplification only because of how this front end is tested: every
// sample must also run under `culebra` and print the same bytes, so a
// disagreement about where a statement ends shows up as a failing diff
// rather than as a quiet difference.
//
// **`throw`, `return`, `break` and `continue` are expressions too.**
// culebra is expression-oriented: its PRIMARY lists all four, which is what
// makes `|| throw 'boom'` a lambda body rather than a syntax error. They
// appear twice here for that reason -- once in `stmt`, where they are tried
// before `exprstmt`, and once in `primary`.
//
// **`'...'` is a plain string and `"..."` is an interpolated one.** That is
// culebra's own split, and it is why `istring` has a structure (alternating
// text and `{expr}` holes) where `string` is one token.
//
// **A `{` at the start of a statement is a block, not an object literal.**
// culebra's STATEMENT_BASE tries LEXICAL_SCOPE before EXPRESSION, so this
// one puts `block` ahead of `exprstmt` for the same reason -- and an object
// literal in statement position has to be parenthesized, in both.
//
// Every rule that can end up with exactly one child carries `no_ast_opt`;
// see examples/pl0/grammar.h for the canonical case that bites.

#pragma once

namespace mini_culebra {

inline constexpr const char* kGrammar = R"GRAMMAR(
  program    <- _ stmt* !.                                  { no_ast_opt }

  stmt       <- fndecl / classdecl / vardecl / deferstmt / returnstmt
              / throwstmt / yieldfrom / yieldstmt / breakstmt / contstmt
              / whilestmt / forstmt / block / exprstmt

  fndecl     <- 'fn' __ ident params block
  params     <- '(' _ (ident (',' _ ident)*)? ','? _ ')' _   { no_ast_opt }
  classdecl  <- 'class' __ ident '{' _ method* '}' _         { no_ast_opt }
  method     <- ident params block

  vardecl    <- varkw ident '=' ![=] _ expr
  varkw      <- < 'let' / 'mut' > ![a-zA-Z0-9_] _

  block      <- '{' _ stmt* '}' _                            { no_ast_opt }
  deferstmt  <- 'defer' _ block                              { no_ast_opt }
  returnstmt <- 'return' ![a-zA-Z0-9_] _ expr?               { no_ast_opt }
  throwstmt  <- 'throw' ![a-zA-Z0-9_] _ expr                 { no_ast_opt }
  yieldfrom  <- 'yield' __ 'from' __ expr                    { no_ast_opt }
  yieldstmt  <- 'yield' __ expr                              { no_ast_opt }
  breakstmt  <- 'break' ![a-zA-Z0-9_] _                      { no_ast_opt }
  contstmt   <- 'continue' ![a-zA-Z0-9_] _                   { no_ast_opt }
  whilestmt  <- 'while' __ expr block
  forstmt    <- 'for' __ ident 'in' __ expr block
  exprstmt   <- expr                                         { no_ast_opt }

  expr       <- assign / logor
  assign     <- postfix assignop expr
  assignop   <- < '+=' / '-=' / '*=' / '/=' / '%=' / '=' ![=] > _
  logor      <- logand ('||' _ logand)*
  logand     <- equality ('&&' _ equality)*
  equality   <- relational (eqop relational)*
  eqop       <- < '==' / '!=' > _
  relational <- additive (relop additive)*
  relop      <- < '<=' / '>=' / '<' / '>' > _
  additive   <- multiplicative (addop multiplicative)*
  addop      <- < '+' ![=] / '-' ![=] > _
  multiplicative <- unary (mulop unary)*
  mulop      <- < '*' ![=] / '/' ![=] / '%' ![=] > _

  unary      <- notexpr / negexpr / postfix
  notexpr    <- '!' ![=] _ unary                             { no_ast_opt }
  negexpr    <- '-' ![=] _ unary                             { no_ast_opt }

  postfix    <- primary suffix*
  suffix     <- callsfx / membersfx / indexsfx
  callsfx    <- '(' _ args ')' _                             { no_ast_opt }
  args       <- (expr (',' _ expr)*)? ','? _                 { no_ast_opt }
  membersfx  <- '.' _ ident                                  { no_ast_opt }
  indexsfx   <- '[' _ expr ']' _                             { no_ast_opt }

  primary    <- ifexpr / tryexpr / lambda / fnexpr / returnstmt / throwstmt
              / breakstmt / contstmt / float / number
              / istring / string / arraylit / objectlit / literal / ident
              / paren
  paren      <- '(' _ expr ')' _                             { no_ast_opt }
  ifexpr     <- 'if' __ expr block ('else' ![a-zA-Z0-9_] _ (ifexpr / block))?
  tryexpr    <- 'try' _ block _ 'catch' __ ident block
  fnexpr     <- 'fn' _ params block
  lambda     <- lparams (block / expr)
  lparams    <- '|' _ (ident (',' _ ident)*)? ','? _ '|' _   { no_ast_opt }

  arraylit   <- '[' _ (expr (',' _ expr)*)? ','? _ ']' _     { no_ast_opt }
  objectlit  <- '{' _ (propdef (',' _ propdef)*)? ','? _ '}' _ { no_ast_opt }
  propdef    <- propkey ':' _ expr
  propkey    <- ident / string
  literal    <- < ('true' / 'false' / 'nil') ![a-zA-Z0-9_] > _

  float      <- < [0-9]+ '.' [0-9]+ ([eE] [-+]? [0-9]+)?
                / [0-9]+ [eE] [-+]? [0-9]+ > _
  number     <- < [0-9]+ > _
  string     <- ['] < (!['\\] . / '\\' .)* > ['] _
  istring    <- ["] ipart* ["] _                             { no_ast_opt }
  ipart      <- ihole / itext
  ihole      <- '{' _ expr '}'                               { no_ast_opt }
  itext      <- < (!["{\\] . / '\\' .)+ >
  ident      <- < [a-zA-Z_] [a-zA-Z0-9_]* > _

  ~_         <- ([ \t\r\n;] / '#' [^\n]*)*
  ~__        <- ![a-zA-Z0-9_] _
)GRAMMAR";

}  // namespace mini_culebra
