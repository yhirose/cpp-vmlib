// A slice of Python 3, chosen for the last recipe in the top-level README
// that no front end had reached: **Arbitrary-precision integers**. Python's
// `int` is unbounded, so `python3` is an oracle for a bignum that this
// library deliberately does not have a Value tag for -- "a decision rather
// than a gap", says that section, and the front end carries them itself.
// `with` is the second reason: a context manager is a Scope plus a Defer,
// which is what those two tags are for.
//
// What it covers: `def` with positional, default, `*args` and `**kwargs`
// parameters; `class` with inheritance, `super()`, `__init__`/`__str__`/
// `__repr__`/`__eq__`; `lambda`, closures, `global`/`nonlocal`; lists,
// dicts, tuples and strings with their methods; comprehensions (list,
// dict and generator); f-strings; indexing with negative indices and
// slicing; `if`/`elif`/`else`, `while`, `for ... in` (with unpacking),
// `break`/`continue`/`pass`; assignment (including `a, b = b, a`) and the
// augmented forms; `try`/`except`/`finally` (multiple clauses, a bare
// `except:`, user exception classes); `raise`, `with`, `yield`/
// `yield from`, `del`, `assert`, `is`/`is not`, decorators, and the
// builtins the samples need. What it does not: sets, imports, the numeric
// tower beyond `int`/`float`/the bignum, multiple inheritance, properties,
// `async`, and `@staticmethod`/`@classmethod` -- see README.md.
//
// **This is the one grammar here that a PEG cannot express on its own.**
// Python's block structure is its indentation, and a PEG has no state to
// count columns with. So the source is normalized before the parser sees
// it (Binder::layout, in binder.cc): each logical line is stripped of its
// leading whitespace and preceded by an INDENT (0x01) or as many DEDENTs
// (0x02) as it closes, and terminated by a NEWLINE marker (0x03). Those
// three bytes cannot occur in Python source, so the transformation is
// unambiguous, and the newline itself is kept after the marker so that
// peglib's line numbers -- and therefore every diagnostic -- still point at
// the right line. Bracket depth and string quoting are tracked while
// scanning, because a line inside `[` or `(` is a continuation and not a
// logical line of its own.
//
// **A suite may be on the same line, and `;` joins simple statements.**
// `if x: return 1` and `a = 1; b = 2` are both Python and neither produces
// an INDENT, so `block` has a second alternative and `simpleline` holds the
// semicolon-separated list.
//
// Two smaller things worth knowing:
//
// **`a, b = b, a + b` is one statement.** The targets and the values are
// each a comma list, and the whole right-hand side is evaluated before any
// of it is stored -- which is the only reason the swap in a Fibonacci loop
// works. `targets` and `exprs` are the two lists.
//
// **`elif` is a chain on one node, not nesting.** Like Lua's, and read the
// same way in the binder.
//
// **`ident` must refuse the keywords.** `not`, `in`, `and`, `or`, `None`,
// `True` and `False` are all identifier-shaped, and `x in y` would parse
// as two expression statements without the guard -- examples/mini-lua's
// grammar found the same trap with `end`.

#pragma once

namespace mini_python {

inline constexpr const char* kGrammar = R"GRAMMAR(
  program    <- [\n]* NL* stmt* !.                           { no_ast_opt }
  block      <- ':' _ NL IND stmt+ DED / ':' _ simpleline    { no_ast_opt }

  stmt       <- funcdef / classdef / ifstmt / whilestmt / forstmt
              / trystmt / withstmt / simpleline
  simpleline <- simple (';' _ simple)* ';'? _ NL             { no_ast_opt }
  simple     <- returnstmt / raisestmt / passstmt / breakstmt / contstmt
              / yieldstmt / globalstmt / nonlocalstmt / delstmt
              / assertstmt / assign / exprstmt

  funcdef    <- decorators? 'def' __ ident '(' _ params ')' _ block
  decorators <- decorator+                                   { no_ast_opt }
  decorator  <- '@' _ postfix NL                             { no_ast_opt }
  params     <- (param (',' _ param)*)? ','? _               { no_ast_opt }
  param      <- kwrest / rest / defparam / ident
  kwrest     <- '**' _ ident                                 { no_ast_opt }
  rest       <- '*' ![*] _ ident                             { no_ast_opt }
  defparam   <- ident '=' ![=] _ expr
  classdef   <- 'class' __ ident classargs? _ block
  classargs  <- '(' _ (ident (',' _ ident)*)? ','? _ ')' _    { no_ast_opt }

  ifstmt     <- 'if' __ expr block elifpart* elsepart?
  elifpart   <- 'elif' __ expr block
  elsepart   <- 'else' _ block                               { no_ast_opt }
  whilestmt  <- 'while' __ expr block
  forstmt    <- 'for' __ fortargets 'in' __ expr block
  fortargets <- ident (',' _ ident)*                         { no_ast_opt }
  trystmt    <- 'try' _ block exceptpart* finallypart?       { no_ast_opt }
  exceptpart <- 'except' __ ident ('as' __ ident)? block
              / 'except' _ block                             { no_ast_opt }
  finallypart <- 'finally' _ block                           { no_ast_opt }
  withstmt   <- 'with' __ expr ('as' __ ident)? block

  returnstmt <- 'return' ![a-zA-Z0-9_] _ exprs?              { no_ast_opt }
  raisestmt  <- 'raise' __ expr                              { no_ast_opt }
  yieldstmt  <- yieldfrom / yieldone
  yieldfrom  <- 'yield' __ 'from' __ expr                    { no_ast_opt }
  yieldone   <- 'yield' __ exprs                             { no_ast_opt }
  globalstmt <- 'global' __ ident (',' _ ident)*             { no_ast_opt }
  nonlocalstmt <- 'nonlocal' __ ident (',' _ ident)*         { no_ast_opt }
  delstmt    <- 'del' __ postfix (',' _ postfix)*            { no_ast_opt }
  assertstmt <- 'assert' __ expr (',' _ expr)?               { no_ast_opt }
  passstmt   <- 'pass' ![a-zA-Z0-9_] _                       { no_ast_opt }
  breakstmt  <- 'break' ![a-zA-Z0-9_] _                      { no_ast_opt }
  contstmt   <- 'continue' ![a-zA-Z0-9_] _                   { no_ast_opt }
  assign     <- targets assignop exprs
  targets    <- postfix (',' _ postfix)*                     { no_ast_opt }
  exprs      <- expr (',' _ expr)*                           { no_ast_opt }
  assignop   <- < '+=' / '-=' / '*=' / '//=' / '/=' / '%=' / '=' ![=] > _
  exprstmt   <- expr                                         { no_ast_opt }

  expr       <- ternary
  ternary    <- orexp ('if' __ orexp 'else' __ ternary)?
  orexp      <- andexp ('or' __ andexp)*
  andexp     <- notexp ('and' __ notexp)*
  notexp     <- notop / cmpexp
  notop      <- 'not' __ notexp                              { no_ast_opt }
  cmpexp     <- addexp (cmpop addexp)*
  cmpop      <- < '==' / '!=' / '<=' / '>=' / '<' / '>'
                / 'not' __ 'in' / 'in' ![a-zA-Z0-9_]
                / 'is' __ 'not' / 'is' ![a-zA-Z0-9_] > _
  addexp     <- mulexp (addop mulexp)*
  addop      <- < '+' ![=] / '-' ![=] > _
  mulexp     <- unary (mulop unary)*
  mulop      <- < '//' ![=] / '*' ![*=] / '/' ![=] / '%' ![=] > _
  unary      <- negexp / powexp
  negexp     <- '-' ![=] _ unary                             { no_ast_opt }
  powexp     <- postfix powpart?
  powpart    <- '**' _ unary                                 { no_ast_opt }

  postfix    <- primary suffix*
  suffix     <- callsfx / dotsfx / indexsfx
  callsfx    <- '(' _ args ')' _                             { no_ast_opt }
  args       <- (arg (',' _ arg)*)? ','? _                   { no_ast_opt }
  arg        <- kwsplat / splat / kwarg / bargen / expr
  kwsplat    <- '**' _ expr                                  { no_ast_opt }
  splat      <- '*' ![*] _ expr                              { no_ast_opt }
  kwarg      <- ident '=' ![=] _ expr
  dotsfx     <- '.' _ ident                                  { no_ast_opt }
  indexsfx   <- '[' _ subscript ']' _                        { no_ast_opt }
  subscript  <- sliceboth / slicelo / slicehi / sliceall / expr
  sliceboth  <- expr ':' _ expr
  slicelo    <- expr ':' _                                   { no_ast_opt }
  slicehi    <- ':' _ expr                                   { no_ast_opt }
  sliceall   <- ':' _                                        { no_ast_opt }

  primary    <- lambda / fstring / float / number / string
              / listcomp / listlit / dictcomp / dictlit
              / literal / ident / gencomp / tuplelit / paren

  # A comprehension is a function of its own -- which is why its target
  # does not leak, and why the generator form can be lazy. The iterable and
  # the condition stop at `orexp` so that a trailing `if` is a clause and
  # not a ternary.
  listcomp   <- '[' _ expr compclause+ ']' _                 { no_ast_opt }
  dictcomp   <- '{' _ expr ':' _ expr compclause+ '}' _      { no_ast_opt }
  gencomp    <- '(' _ expr compclause+ ')' _                 { no_ast_opt }
  # `sum(x*x for x in xs)`: a generator expression is its own argument
  # and needs no parentheses of its own. It cannot be an alternative of
  # `gencomp` -- `primary` is reached from `expr`, so a bare form there
  # would left-recurse -- so it is a rule the binder reads the same way.
  bargen     <- expr compclause+                             { no_ast_opt }
  compclause <- compfor / compif
  compfor    <- 'for' __ fortargets 'in' __ orexp            { no_ast_opt }
  compif     <- 'if' __ orexp                                { no_ast_opt }
  paren      <- '(' _ expr ')' _                             { no_ast_opt }
  tuplelit   <- '(' _ ')' _
              / '(' _ expr (',' _ expr)+ ','? _ ')' _
              / '(' _ expr ',' _ ')' _                       { no_ast_opt }
  lambda     <- 'lambda' __ params ':' _ expr
  listlit    <- '[' _ (expr (',' _ expr)*)? ','? _ ']' _     { no_ast_opt }
  dictlit    <- '{' _ (pair (',' _ pair)*)? ','? _ '}' _     { no_ast_opt }
  pair       <- expr ':' _ expr

  # An f-string is parsed structurally rather than re-parsed later: the
  # interpolations are ordinary expressions sitting inside the literal, so
  # the one grammar handles both. `{{` and `}}` are literal braces.
  fstring    <- 'f' ["] fdq* ["] _ / 'f' ['] fsq* ['] _      { no_ast_opt }
  fdq        <- fexpr / ftextd
  fsq        <- fexpr / ftexts
  ftextd     <- < ('{{' / '}}' / '\\' . / !["{}] .)+ >
  ftexts     <- < ('{{' / '}}' / '\\' . / !['{}] .)+ >
  fexpr      <- '{' _ expr fconv? fspec? '}'                  { no_ast_opt }
  fconv      <- '!' < [rs] >                                 { no_ast_opt }
  fspec      <- ':' < [^}]* >                                { no_ast_opt }

  literal    <- < ('True' / 'False' / 'None') ![a-zA-Z0-9_] > _
  float      <- < [0-9]+ '.' [0-9]* ([eE] [-+]? [0-9]+)?
                / [0-9]+ [eE] [-+]? [0-9]+ > _
  number     <- < [0-9]+ ('_' [0-9]+)* > _
  string     <- ['] < (!['\\] . / '\\' .)* > ['] _
              / ["] < (!["\\] . / '\\' .)* > ["] _
  ident      <- !keyword < [a-zA-Z_] [a-zA-Z0-9_]* > _
  ~keyword   <- ('and' / 'as' / 'break' / 'class' / 'continue' / 'def'
               / 'elif' / 'else' / 'except' / 'False' / 'finally' / 'for'
               / 'from' / 'global' / 'if' / 'import' / 'in' / 'is'
               / 'lambda' / 'None' / 'nonlocal' / 'not' / 'or' / 'pass'
               / 'raise' / 'return' / 'True' / 'try' / 'while' / 'with'
               / 'yield' / 'assert' / 'del') ![a-zA-Z0-9_]

  ~_         <- ([ \t] / '#' [^\n\x01\x02\x03]*)*
  ~__        <- ![a-zA-Z0-9_] _
  ~NL        <- '\x03' [\n]* _
  ~IND       <- '\x01' _
  ~DED       <- '\x02' _
)GRAMMAR";

}  // namespace mini_python
