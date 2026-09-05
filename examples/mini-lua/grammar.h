// A slice of Lua 5.4, chosen for the two recipes no other front end here
// reaches: **Tail calls**, which Lua is one of the few languages to
// *guarantee* (so `lua` itself is a real oracle for them), and
// **Coroutines** in their user-facing form -- `coroutine.create` /
// `resume` / `yield` / `status` / `wrap`, which vmlib.h's own comment
// names as what CoroCreate and friends are made of. Multiple return values
// come along as a third: Lua's calling convention is not the IR's, so the
// binder writes its own over it.
//
// What it covers: `local`, functions (declared, anonymous, and `function
// t.k()` / `function t:m()`), closures, tables with both array and hash
// parts, metatables through `setmetatable` and `__index`/`__newindex`/
// `__add`/`__sub`/`__mul`/`__eq`/`__tostring`/`__call`, `if`/`elseif`/
// `else`, `while`, `repeat`/`until`, both `for` forms (including the real
// generic-`for` protocol, not just `ipairs`/`pairs`), `break`, multiple
// assignment and multiple return, the operator set including `//`, `%`,
// `^` and `..`, and the standard-library slice the samples need. What it
// does not: `goto`, varargs (`...`), `string` patterns, `os`/`io` beyond
// `io.write`, integer division by zero's own error, weak tables, `__gc`,
// and the remaining metamethods -- see README.md.
//
// Four things about this grammar are worth knowing before editing it.
//
// **`--` is a comment, and `-` is not.** `_` eats `--` to end of line, so
// `a --b` is `a` followed by a comment exactly as in Lua. `negexp` guards
// with `![-]` so that a unary minus never starts one.
//
// **`then`, `do`, `end` and `until` are bare keyword literals, so they
// take `__` after them; a rule that has just finished an `ident` or an
// `expr` must not.** `__` is `![a-zA-Z0-9_] _`, and those rules already
// consumed the separator -- see examples/mini-go/grammar.h for the trap,
// which examples/mini-culebra/grammar.h then fell into anyway.
//
// **A call is a statement and an expression.** Lua allows `f()` on its own
// line, so `stmt` lists `assign` before `callstmt` (both start with a
// suffixed expression) and lets PEG backtrack when the `=` does not
// arrive.
//
// **`ident` has to refuse the keywords.** Lua's blocks end with the word
// `end` rather than a brace, and without the guard `ident` matches it --
// so `end` parses as an expression statement, the block swallows its own
// terminator, and the error surfaces pages later as "expecting 'end'".
// This grammar had exactly that bug on its first pass. A brace-terminated
// language (the other three front ends here) never notices, which is why
// the trap is new at the fourth.
//
// **`^` is right-associative and binds tighter than unary minus**, so
// `powexp` recurses into `unaryexp` on its right rather than into itself:
// `-2^2` is -(2^2) in Lua, and this grammar says so by putting `powexp`
// under `unaryexp` rather than beside it.
//
// Every rule that can end up with exactly one child carries `no_ast_opt`;
// see examples/pl0/grammar.h for the canonical case that bites.

#pragma once

namespace mini_lua {

inline constexpr const char* kGrammar = R"GRAMMAR(
  program    <- _ block !.                                  { no_ast_opt }
  block      <- stmt*                                       { no_ast_opt }

  stmt       <- emptystmt / localfn / localdecl / fnstat / ifstat / whilestat
              / repeatstat / fornum / forin / dostat / breakstat / retstat
              / assign / callstmt

  emptystmt  <- ';' _                                       { no_ast_opt }
  localfn    <- 'local' __ 'function' __ ident funcbody
  localdecl  <- 'local' __ namelist ('=' ![=] _ exprlist)?   { no_ast_opt }
  namelist   <- ident (',' _ ident)*                         { no_ast_opt }
  exprlist   <- expr (',' _ expr)*                           { no_ast_opt }

  fnstat     <- 'function' __ funcname funcbody
  funcname   <- ident (dotname / colonname)*                 { no_ast_opt }
  dotname    <- '.' _ ident                                  { no_ast_opt }
  colonname  <- ':' _ ident                                  { no_ast_opt }
  funcbody   <- '(' _ params ')' _ block 'end' __
  params     <- (ident (',' _ ident)*)? ','? _               { no_ast_opt }

  ifstat     <- 'if' __ expr 'then' __ block elseifpart* elsepart? 'end' __
  elseifpart <- 'elseif' __ expr 'then' __ block
  elsepart   <- 'else' __ block                              { no_ast_opt }
  whilestat  <- 'while' __ expr 'do' __ block 'end' __
  repeatstat <- 'repeat' __ block 'until' __ expr
  fornum     <- 'for' __ ident '=' ![=] _ expr ',' _ expr forstep? 'do' __
                block 'end' __
  forstep    <- ',' _ expr                                   { no_ast_opt }
  forin      <- 'for' __ namelist 'in' __ exprlist 'do' __ block 'end' __
  dostat     <- 'do' __ block 'end' __                       { no_ast_opt }
  breakstat  <- 'break' __                                   { no_ast_opt }
  retstat    <- 'return' __ exprlist? ';'? _                 { no_ast_opt }
  assign     <- varlist '=' ![=] _ exprlist
  varlist    <- suffixedexp (',' _ suffixedexp)*             { no_ast_opt }
  callstmt   <- suffixedexp                                  { no_ast_opt }

  expr       <- orexp
  orexp      <- andexp ('or' __ andexp)*
  andexp     <- cmpexp ('and' __ cmpexp)*
  cmpexp     <- concatexp (cmpop concatexp)*
  cmpop      <- < '==' / '~=' / '<=' / '>=' / '<' / '>' > _
  concatexp  <- addexp ('..' ![.] _ addexp)*
  addexp     <- mulexp (addop mulexp)*
  addop      <- < '+' / '-' ![-] > _
  mulexp     <- unaryexp (mulop unaryexp)*
  mulop      <- < '//' / '*' / '/' / '%' > _

  unaryexp   <- notexp / negexp / lenexp / powexp
  notexp     <- 'not' __ unaryexp                            { no_ast_opt }
  negexp     <- '-' ![-] _ unaryexp                          { no_ast_opt }
  lenexp     <- '#' _ unaryexp                               { no_ast_opt }
  powexp     <- suffixedexp powpart?
  powpart    <- '^' _ unaryexp                               { no_ast_opt }

  suffixedexp <- primaryexp suffix*
  suffix     <- callsfx / methodsfx / dotsfx / indexsfx
  callsfx    <- '(' _ args ')' _ / strargsfx                 { no_ast_opt }
  strargsfx  <- string                                       { no_ast_opt }
  args       <- (expr (',' _ expr)*)? ','? _                 { no_ast_opt }
  methodsfx  <- ':' _ ident '(' _ args ')' _                 { no_ast_opt }
  dotsfx     <- '.' _ ident                                  { no_ast_opt }
  indexsfx   <- '[' _ expr ']' _                             { no_ast_opt }

  primaryexp <- funcexpr / tablector / float / number / string / literal
              / ident / paren
  paren      <- '(' _ expr ')' _                             { no_ast_opt }
  funcexpr   <- 'function' __ funcbody                       { no_ast_opt }

  tablector  <- '{' _ (field (fieldsep field)*)? fieldsep? _ '}' _
                                                             { no_ast_opt }
  field      <- keyedfield / namedfield / expr
  keyedfield <- '[' _ expr ']' _ '=' ![=] _ expr
  namedfield <- ident '=' ![=] _ expr
  ~fieldsep  <- (',' / ';') _

  literal    <- < ('true' / 'false' / 'nil') ![a-zA-Z0-9_] > _
  float      <- < [0-9]+ '.' [0-9]* ([eE] [-+]? [0-9]+)?
                / [0-9]+ [eE] [-+]? [0-9]+ > _
  number     <- < [0-9]+ > _
  string     <- ['] < (!['\\] . / '\\' .)* > ['] _
              / ["] < (!["\\] . / '\\' .)* > ["] _
  ident      <- !keyword < [a-zA-Z_] [a-zA-Z0-9_]* > _
  ~keyword   <- ('and' / 'break' / 'do' / 'elseif' / 'else' / 'end' / 'false'
               / 'for' / 'function' / 'goto' / 'if' / 'in' / 'local' / 'nil'
               / 'not' / 'or' / 'repeat' / 'return' / 'then' / 'true'
               / 'until' / 'while') ![a-zA-Z0-9_]

  ~_         <- ([ \t\r\n] / '--' [^\n]*)*
  ~__        <- ![a-zA-Z0-9_] _
)GRAMMAR";

}  // namespace mini_lua
