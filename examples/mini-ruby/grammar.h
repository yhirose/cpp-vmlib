// A slice of Ruby, chosen for the two intrinsics no other front end here
// uses -- `FnArity` and `ArgCount` -- and for `ensure`.
//
// `vmlib.h` introduces FnArity as "The one fact a front end needs to check
// 'this callback takes two arguments' before calling it", and ArgCount as
// "how many arguments the running function was called with... Only
// interesting under Func::lenient_arity". Ruby is the language where both
// are ordinary program-visible behaviour: `Proc#arity` *is* FnArity, and
// the difference between a proc (extra arguments dropped, missing ones
// nil) and a lambda (a mismatch raises ArgumentError) is exactly
// lenient_arity plus an ArgCount check.
//
// What it covers: `def` with an optional `&block` parameter and default
// parameters, blocks in both `{ |x| }` and `do |x| ... end` forms, `yield`,
// `block_given?`, `lambda`/`Proc.new` and `.call`/`.arity`,
// `begin`/`rescue`/`ensure`, `raise`, `if`/`elsif`/`else`/`unless`/`while`/
// `until`/`case`/`when`, statement modifiers (`x if c`), the ternary
// operator, `class` with inheritance/`super`/`attr_accessor`, symbols,
// arrays, hashes, ranges, strings with `#{}` interpolation, and the
// iterator methods the samples need. What it does not: modules, splats,
// keyword arguments, and `method_missing` -- see README.md.
//
// Three things about this grammar are worth knowing.
//
// **A method name may end in `?` or `!`.** That is Ruby, and it is why
// there is no ternary operator here: `x ? a : b` and a predicate named `x?`
// cannot both be parsed without the whitespace rule Ruby itself resorts
// to. `if`/`else` is the replacement, and it is one line either way.
//
// **A `{` after a call is a block; a `{` starting an expression is a
// hash.** Ruby draws the same line and has the same ambiguity. Here the
// block form only follows a call -- either `f(args) { }` or the bare
// `f { }` -- so `h = {"a" => 1}` still reads as a hash: `f { }` is tried
// first, its body fails to parse `"a" => 1`, and PEG backtracks to the
// plain identifier.
//
// **A primary ends at the newline.** Every token that can finish an
// expression -- a literal, an identifier, a closing `)`, `]`, `}` or
// `end` -- skips horizontal space only (`hs`), and newlines are consumed
// at the statement level and inside brackets instead. Without that,
// `x = "outer"` followed by `[1].each { }` on the next line reads as
// `"outer"[1]`, and `puts i` followed by `(1..3).each` reads as
// `i(1..3)`. Both were real bugs here, and both surfaced as a type error
// pages away from the cause -- which is a good argument for why Ruby's
// own grammar treats a newline as a terminator rather than as space.
//
// **A newline ends a paren-less command's argument list.** `puts` on its
// own line followed by `puts x` on the next is two statements, not one
// call of the other -- so `puts`, `raise`, `yield` and `return` are the
// four places this grammar stops treating a newline as whitespace, and
// skip only horizontal space (`hs`) after the keyword. Everywhere else a
// newline is whitespace, which is a simplification the oracle keeps
// honest: a disagreement about where a statement ends becomes a failing
// diff.
//
// **`ident` has to refuse the keywords**, because Ruby's blocks end with
// the word `end` -- the trap examples/mini-lua's grammar found first, and
// the reason both of them carry a `keyword` rule.

#pragma once

namespace mini_ruby {

inline constexpr const char* kGrammar = R"GRAMMAR(
  program    <- _ (stmt _)* !.                                { no_ast_opt }
  body       <- _ (stmt _)*                                   { no_ast_opt }

  stmt       <- defstmt / classdef / ifstmt / unlessstmt / whilestmt
              / untilstmt / casestmt / beginstmt / attrstmt / modstmt
  # A statement modifier -- `expr if cond` -- is tried after the plain
  # form, so `x = 1 if cond` still parses as an assignment with a trailing
  # clause rather than the clause swallowing part of the expression.
  modstmt    <- (assign / exprstmt) modpart?
  modpart    <- ifmod / unlessmod / whilemod / untilmod
  ifmod      <- 'if' __ expr                                 { no_ast_opt }
  unlessmod  <- 'unless' __ expr                              { no_ast_opt }
  whilemod   <- 'while' __ expr                               { no_ast_opt }
  untilmod   <- 'until' __ expr                               { no_ast_opt }

  defstmt    <- 'def' __ defname paramlist body 'end' __
  # A handful of operator methods, recognized only by name -- see
  # README.md for why overloading stops at these.
  defname    <- opname / ident
  opname     <- < '==' / '<=>' > hs
  paramlist  <- ('(' _ (param (',' _ param)*)? ','? _ ')' _)? { no_ast_opt }
  param      <- blockparam / defparam / ident
  blockparam <- '&' _ ident                                  { no_ast_opt }
  defparam   <- ident '=' ![=] _ expr

  # A class is a table of methods and (with `attr_accessor`) of generated
  # ones, reached the same way examples/mini-python and examples/mini-csharp
  # reach theirs -- see README.md.
  classdef   <- 'class' __ ident superclass? _ body 'end' __
  superclass <- '<' _ ident                                  { no_ast_opt }
  attrstmt   <- attrkind __ symbol (',' _ symbol)*
  attrkind   <- < 'attr_accessor' / 'attr_reader' / 'attr_writer' >
                ![a-zA-Z0-9_?!] hs
  supercall  <- 'super' ![a-zA-Z0-9_?!] hs ('(' _ args ')' hs)? { no_ast_opt }

  casestmt   <- 'case' __ expr thensep _ whenpart+ elsepart? 'end' __
  whenpart   <- 'when' __ whenvals thensep body
  whenvals   <- expr (',' _ expr)*                            { no_ast_opt }

  ifstmt     <- 'if' __ expr thensep body elsifpart* elsepart? 'end' __
  elsifpart  <- 'elsif' __ expr thensep body
  elsepart   <- 'else' __ body                               { no_ast_opt }
  unlessstmt <- 'unless' __ expr thensep body elsepart? 'end' __
  whilestmt  <- 'while' __ expr dosep body 'end' __
  untilstmt  <- 'until' __ expr dosep body 'end' __
  ~thensep   <- ('then' ![a-zA-Z0-9_] _)?
  ~dosep     <- ('do' ![a-zA-Z0-9_] _)?

  beginstmt  <- 'begin' __ body rescuepart? ensurepart? 'end' __
                                                             { no_ast_opt }
  rescuepart <- 'rescue' ![a-zA-Z0-9_] _ rescuevar? body     { no_ast_opt }
  rescuevar  <- '=>' _ ident                                 { no_ast_opt }
  ensurepart <- 'ensure' __ body                             { no_ast_opt }

  assign     <- postfix assignop expr
  assignop   <- < '+=' / '-=' / '*=' / '/=' / '%=' / '=' ![=~] > _
  exprstmt   <- expr                                         { no_ast_opt }

  expr       <- ternary
  ternary    <- rangeexp ('?' _ expr ':' _ expr)?
  rangeexp   <- orexp ('..' _ orexp)?
  orexp      <- andexp ('||' _ andexp)*
  andexp     <- noteexp ('&&' _ noteexp)*
  noteexp    <- notop / cmpexp
  notop      <- '!' ![=] _ noteexp                           { no_ast_opt }
  cmpexp     <- addexp (cmpop addexp)*
  cmpop      <- < '==' / '!=' / '<=' / '>=' / '<' / '>' > _
  addexp     <- mulexp (addop mulexp)*
  addop      <- < '+' ![=] / '-' ![=] > _
  mulexp     <- unary (mulop unary)*
  mulop      <- < '**' / '*' ![*=] / '/' ![=] / '%' ![=] > _
  unary      <- negexp / postfix
  negexp     <- '-' ![=] _ unary                             { no_ast_opt }

  postfix    <- primary suffix*
  suffix     <- methodsfx / indexsfx
  methodsfx  <- '.' _ methodname callargs? blockarg?         { no_ast_opt }
  # A method name is not subject to the keyword guard: `.class` and
  # `.when` are unambiguous after a dot, the way real Ruby's are.
  methodname <- < [a-zA-Z_] [a-zA-Z0-9_]* [?!]? > hs
  indexsfx   <- '[' _ expr ']' hs                            { no_ast_opt }
  callargs   <- '(' _ args ')' hs                            { no_ast_opt }
  args       <- (arg (',' _ arg)*)? ','? _                    { no_ast_opt }
  arg        <- blockpass / expr
  blockpass  <- '&' _ expr                                    { no_ast_opt }

  blockarg   <- braceblock / doblock
  braceblock <- '{' _ blockparams body '}' hs                { no_ast_opt }
  doblock    <- 'do' __ blockparams body 'end' ![a-zA-Z0-9_?!] hs
                                                             { no_ast_opt }
  blockparams <- ('|' _ (ident (',' _ ident)*)? ','? _ '|' _)?
                                                             { no_ast_opt }

  primary    <- cmdcall / supercall / yieldexpr / returnstmt / nextstmt
              / breakstmt / float / number / istring / string / symbol
              / arraylit / hashlit / literal / selfexpr / ivar / callexpr
              / ident / paren
  selfexpr   <- 'self' ![a-zA-Z0-9_?!] hs                    { no_ast_opt }
  ivar       <- '@' < [a-zA-Z_] [a-zA-Z0-9_]* > hs
  symbol     <- ':' < [a-zA-Z_] [a-zA-Z0-9_]* [?!]? > hs
  paren      <- '(' _ expr ')' hs                            { no_ast_opt }
  callexpr   <- ident callargs blockarg? / ident blockarg
  cmdcall    <- cmdname args1?                               { no_ast_opt }
  cmdname    <- < ('puts' / 'print' / 'raise' / 'p') > ![a-zA-Z0-9_?!] hs
  args1      <- expr (',' _ expr)*                           { no_ast_opt }
  yieldexpr  <- 'yield' ![a-zA-Z0-9_?!] hs args1?            { no_ast_opt }
  returnstmt <- 'return' ![a-zA-Z0-9_?!] hs expr?            { no_ast_opt }
  nextstmt   <- 'next' ![a-zA-Z0-9_?!] _                     { no_ast_opt }
  breakstmt  <- 'break' ![a-zA-Z0-9_?!] _                    { no_ast_opt }

  arraylit   <- '[' _ (expr (',' _ expr)*)? ','? _ ']' hs    { no_ast_opt }
  hashlit    <- '{' _ (hpair (',' _ hpair)*)? ','? _ '}' hs  { no_ast_opt }
  hpair      <- expr '=>' _ expr
  literal    <- < ('true' / 'false' / 'nil') ![a-zA-Z0-9_] > hs

  float      <- < [0-9]+ '.' [0-9]+ ([eE] [-+]? [0-9]+)? > hs
  number     <- < [0-9]+ > hs
  string     <- ['] < (!['\\] . / '\\' .)* > ['] hs
  istring    <- ["] ipart* ["] hs                            { no_ast_opt }
  ipart      <- ihole / itext
  ihole      <- '#{' _ expr '}'                              { no_ast_opt }
  itext      <- < (!["\\] !'#{' . / '\\' .)+ >
  ident      <- !keyword < [a-zA-Z_] [a-zA-Z0-9_]* [?!]? > hs
  ~keyword   <- ('begin' / 'break' / 'case' / 'class' / 'def' / 'do'
               / 'elsif' / 'else' / 'end' / 'ensure' / 'false' / 'if'
               / 'next' / 'nil' / 'rescue' / 'return' / 'self' / 'super'
               / 'then' / 'true' / 'unless' / 'until' / 'when' / 'while'
               / 'yield') ![a-zA-Z0-9_?!]

  ~_         <- ([ \t\r\n;] / '#' !'{' [^\n]*)*
  ~hs        <- ([ \t] / '#' !'{' [^\n]*)*
  ~__        <- ![a-zA-Z0-9_] _
)GRAMMAR";

}  // namespace mini_ruby
