// A slice of JavaScript narrow enough to write a binder for, and real
// enough that `node` runs every sample verbatim -- the oracle this front
// end's tests check against is the language, not a second implementation
// of it. What it covers: `let`/`const` with block scope, function
// declarations and expressions, arrow functions, closures, objects and
// arrays, member and index access, the arithmetic/comparison/logical
// operator set, `if`/`while`/`for`/`for...of` with `break`/`continue`,
// `throw` and `try`/`catch`/`finally`, `function*` with `yield`, `async`
// functions with `await`, `class` with `this` and `new` (no `extends`),
// and `new` for the three built-in constructors this subset has (`Error`,
// `Map`, `Set`, plus `Promise`'s executor form). What it does not:
// `extends`/`super`, `var`, destructuring, spread, template literals,
// regular expressions, labels, `switch`, `for...in`, getters/setters,
// `yield*`, and JavaScript's implicit type coercions -- see README.md for
// why each is out, and the top-level README for the recipes the ones
// that are in exist to prove. `var` and `yield*` parse and are then
// refused by name, so that using one gets a diagnostic saying so rather
// than a syntax error pointing at the wrong thing.
//
// Three PEG shapes here are worth knowing before editing this.
//
// **`fnasync` and `fnstar` always match.** `async` and `*` are optional
// modifiers in the middle of a rule (`async function* f`), and an
// optional *node* -- `('async' __)?` -- would shift every later child's
// position by one when it is absent, so the binder could no longer read
// `funcdecl`'s children positionally. Both are instead written as a
// capture of an optional (`< 'async'? >`), which always produces exactly
// one child whose token is either the keyword or the empty string. The
// binder asks the token, not the child count.
//
// **`arrow` is tried before everything it could be mistaken for.** `(a,
// b) => a + b` and `(a, b)` share their first four tokens, and `x => x`
// and `x` share their first one, so `arrow` has to come first in
// `primary` and let PEG's own backtracking undo the parse when the `=>`
// does not arrive. The same reason `forof` precedes `forstmt` (both
// start `for ( let i`) and `block` precedes `exprstmt` (a statement
// starting `{` is a block, JavaScript's own rule).
//
// **The `![=]` guards.** `'-'` must not eat the `-` of `-=`, `'/'` must
// not eat `/=` or the `//` of a comment, `'='` must not eat `==`, and
// `'&&'`/`'||'` must be tried before any single-character operator that
// prefixes them. PEG has no maximal-munch tokenizer to do this for it, so
// each operator states what may not follow it.
//
// Every rule that can end up with exactly one child carries `no_ast_opt`
// -- the binder calls optimize_ast, and without the mark a wrapper's
// identity (a `return x` folding into a bare `x`) or a list's shape (a
// one-argument call's `args` node vanishing, taking that argument's
// position with it) is lost. See examples/pl0/grammar.h for the canonical
// case this bites.
//
// (Comments cannot live inside the grammar string below: peglib's own
// grammar DSL comments with `#`, and `//` there would parse as grammar
// text rather than as a comment.)

#pragma once

namespace mini_js {

inline constexpr const char* kGrammar = R"GRAMMAR(
  program    <- _ stmt* !.                                  { no_ast_opt }

  stmt       <- funcdecl / classdecl / vardecl / ifstmt / forof / forstmt
              / whilestmt / retstmt / throwstmt / trystmt / breakstmt
              / contstmt / block / emptystmt / exprstmt

  funcdecl   <- fnasync 'function' _ fnstar ident '(' _ params ')' _ block

  # A class is a plain function under the hood -- `new ClassName(...)` and
  # `ClassName(...)` compile to the same call -- so the grammar only has
  # to name the shape: a constructor is a member spelled `constructor`,
  # and every other member is a method. No `extends`: see README.md.
  classdecl  <- 'class' ![a-zA-Z0-9_$] _ ident '{' _ classmember* '}' _
                                                             { no_ast_opt }
  classmember <- ident '(' _ params ')' _ block
  fnasync    <- < ('async' ![a-zA-Z0-9_])? > _
  fnstar     <- < '*'? > _
  params     <- (ident (',' _ ident)*)? ','? _              { no_ast_opt }

  vardecl    <- varkw ident ('=' ![=] _ assign)? eos        { no_ast_opt }
  varkw      <- < 'let' / 'const' / 'var' > ![a-zA-Z0-9_] _

  block      <- '{' _ stmt* '}' _                           { no_ast_opt }
  emptystmt  <- ';' _                                       { no_ast_opt }
  ifstmt     <- 'if' _ '(' _ expr ')' _ stmt ('else' ![a-zA-Z0-9_] _ stmt)?
  whilestmt  <- 'while' _ '(' _ expr ')' _ stmt
  forstmt    <- 'for' _ '(' _ forinit ';' _ forcond ';' _ forupd ')' _ stmt
  forinit    <- (vardeclbare / expr)?                       { no_ast_opt }
  forcond    <- expr?                                       { no_ast_opt }
  forupd     <- expr?                                       { no_ast_opt }
  vardeclbare <- varkw ident '=' ![=] _ assign              { no_ast_opt }
  forof      <- 'for' _ '(' _ varkw ident 'of' ![a-zA-Z0-9_] _ expr ')' _ stmt
  retstmt    <- 'return' ![a-zA-Z0-9_] _ expr? eos          { no_ast_opt }
  throwstmt  <- 'throw' ![a-zA-Z0-9_] _ expr eos            { no_ast_opt }
  breakstmt  <- 'break' ![a-zA-Z0-9_] _ eos                 { no_ast_opt }
  contstmt   <- 'continue' ![a-zA-Z0-9_] _ eos              { no_ast_opt }
  trystmt    <- 'try' _ block catchcl? finallycl?           { no_ast_opt }
  catchcl    <- 'catch' _ '(' _ ident ')' _ block           { no_ast_opt }
  finallycl  <- 'finally' _ block                           { no_ast_opt }
  exprstmt   <- expr eos                                    { no_ast_opt }
  ~eos       <- (';' _)?

  expr       <- assign
  assign     <- postfix assignop assign / cond
  assignop   <- < '+=' / '-=' / '*=' / '/=' / '%=' / '=' ![=] > _
  cond       <- logor ('?' _ assign ':' _ assign)?
  logor      <- logand ('||' _ logand)*
  logand     <- equality ('&&' _ equality)*
  equality   <- relational (eqop relational)*
  eqop       <- < '===' / '!==' / '==' / '!=' > _
  relational <- additive (relop additive)*
  relop      <- < '<=' / '>=' / '<' / '>' > _
  additive   <- multiplicative (addop multiplicative)*
  addop      <- < '+' ![+=] / '-' ![-=] > _
  multiplicative <- unary (mulop unary)*
  mulop      <- < '*' ![=] / '/' ![=] / '%' ![=] > _

  unary      <- notexpr / negexpr / typeofexpr / awaitexpr / yieldexpr
              / newexpr / postfix
  notexpr    <- '!' ![=] _ unary                            { no_ast_opt }
  negexpr    <- '-' ![-=] _ unary                           { no_ast_opt }
  typeofexpr <- 'typeof' ![a-zA-Z0-9_] _ unary              { no_ast_opt }
  awaitexpr  <- 'await' ![a-zA-Z0-9_] _ unary               { no_ast_opt }
  newexpr    <- 'new' ![a-zA-Z0-9_] _ postfix               { no_ast_opt }
  yieldexpr  <- 'yield' ![a-zA-Z0-9_] yieldstar assign?     { no_ast_opt }
  yieldstar  <- < '*'? > _

  postfix    <- primary suffix*
  suffix     <- callsfx / membersfx / indexsfx / incsfx
  callsfx    <- '(' _ args ')' _                            { no_ast_opt }
  args       <- (assign (',' _ assign)*)? ','? _            { no_ast_opt }
  membersfx  <- '.' _ ident                                 { no_ast_opt }
  indexsfx   <- '[' _ expr ']' _                            { no_ast_opt }
  incsfx     <- < '++' / '--' > _

  primary    <- arrow / number / string / arraylit / objectlit / funcexpr
              / thisexpr / literal / ident / paren
  thisexpr   <- 'this' ![a-zA-Z0-9_$] _                     { no_ast_opt }
  paren      <- '(' _ expr ')' _                            { no_ast_opt }
  arrow      <- arrowparams '=>' _ arrowbody
  arrowparams <- '(' _ params ')' _ / arrowone
  arrowone   <- ident
  arrowbody  <- block / assign
  funcexpr   <- fnasync 'function' _ fnstar ident? '(' _ params ')' _ block
  arraylit   <- '[' _ (assign (',' _ assign)*)? ','? _ ']' _  { no_ast_opt }
  objectlit  <- '{' _ (propdef (',' _ propdef)*)? ','? _ '}' _ { no_ast_opt }
  propdef    <- propkey ':' _ assign
  propkey    <- ident / string
  literal    <- < ('true' / 'false' / 'null' / 'undefined')
                 ![a-zA-Z0-9_] > _

  number     <- < [0-9]+ ('.' [0-9]+)? ([eE] [-+]? [0-9]+)? > _
  string     <- < ['] (!['\\] . / '\\' .)* ['] > _
              / < ["] (!["\\] . / '\\' .)* ["] > _
  ident      <- < [a-zA-Z_$] [a-zA-Z0-9_$]* > _

  ~_         <- ([ \t\r\n] / '//' [^\n]* / '/*' (!'*/' .)* '*/')*
)GRAMMAR";

}  // namespace mini_js
