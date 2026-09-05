// A slice of C#, and the front end that turns one paragraph of the
// top-level README into running code. Its Scope section says:
//
//   "A managed, statically-typed language (a C#, Java or Go subset) is in
//    scope too -- a binder that has already type-checked can erase types
//    on the way into the IR, and the runtime's refcounted objects, cycle
//    collector, Scope/Defer pair, generators, coroutines and scheduler
//    cover classes, `using`/`try`-`finally`, `yield return`, `async` and
//    goroutines with channels without changes."
//
// examples/mini-go proves the second half of that list. This one is the
// first half: **classes with inheritance and virtual dispatch**, which no
// other front end here has, and **types that are parsed and then erased**,
// which is the claim itself.
//
// What it covers: `class` with fields, constructors (including `: base(...)`),
// instance and `static` methods, `virtual`/`override`, instance properties
// (both `{ get; set; }` and a `get`/`set` with a body), `using` over
// `IDisposable`, `try`/`catch`/`finally`, `throw`, `IEnumerable<T>` methods
// with `yield return`, `foreach`, `async Task<T>` with `await` and
// `.Wait()`/`.Result`, `List<T>`, and the operator set. What it does not:
// interfaces, generics of its own, `out`/`ref`, `switch`, LINQ, delegates,
// `struct` value semantics (examples/mini-go proves that recipe), a
// `static` property, operator overloading, and `int` overflow's wrap
// (mini-go proves that one too) -- see README.md.
//
// Three things about this grammar are worth knowing.
//
// **`new` is a primary, not a unary.** `new Plain().Describe()` chains a
// suffix onto the object it just made, so `newexp` has to sit where a
// suffix chain can follow it. Its two shapes are spelled out rather than
// made optional, because `new List<int> { 1, 2 }` has no argument list at
// all and `new List<int>` alone is not an expression.
//
// **A declaration is two identifiers in a row.** `T x = e;` and `x = e;`
// differ only in that, so `localdecl` is tried first and PEG backtracks
// when the second identifier does not arrive. The same trick separates a
// constructor (`Name(` ) from a method (`Type Name(` ).
//
// **A type is parsed and then thrown away.** `type` exists so that the
// source is real C#, not so that anything downstream reads it -- the
// binder keeps only the two facts it cannot do without: whether a method
// is `static` (which decides where it is bound) and whether it is `async`
// or an iterator (which decides what calling it builds).
//
// **`ident` must refuse the keywords**, the trap examples/mini-lua's
// grammar found first: `new` and `return` are identifier-shaped, and a
// `type` that swallowed one would take the statement's first token with
// it.

#pragma once

namespace mini_csharp {

inline constexpr const char* kGrammar = R"GRAMMAR(
  program    <- _ usingdir* classdecl+ !.                    { no_ast_opt }
  usingdir   <- 'using' __ qname ';' _                       { no_ast_opt }
  qname      <- ident ('.' _ ident)*                         { no_ast_opt }

  classdecl  <- 'class' __ ident basespec? '{' _ member* '}' _
  basespec   <- ':' _ ident (',' _ ident)*                   { no_ast_opt }
  member     <- ctordecl / propdecl / methoddecl / fielddecl
  ctordecl   <- modifier* ident '(' _ params ')' _ baseinit? block
  baseinit   <- ':' _ 'base' _ '(' _ args ')' _              { no_ast_opt }
  methoddecl <- modifier* type ident '(' _ params ')' _ block
  fielddecl  <- modifier* type ident fieldinit? ';' _
  fieldinit  <- '=' ![=] _ expr                              { no_ast_opt }

  # An auto property (`{ get; set; }`, backed by a hidden field the
  # binder invents) and a full one (a `get`/`set` with a block, where
  # `value` is the setter's one implicit parameter) share this shape;
  # mixing the two on one property is a bind-time error, not a grammar
  # one, since both parse the same way up to the accessor bodies.
  propdecl   <- modifier* type ident '{' _ propaccessor+ '}' _
                (fieldinit ';' _)?
  propaccessor <- getbody / setbody / getauto / setauto
  getbody    <- 'get' ![a-zA-Z0-9_] _ block                  { no_ast_opt }
  setbody    <- 'set' ![a-zA-Z0-9_] _ block                  { no_ast_opt }
  getauto    <- 'get' ![a-zA-Z0-9_] _ ';' _                  { no_ast_opt }
  setauto    <- 'set' ![a-zA-Z0-9_] _ ';' _                  { no_ast_opt }
  modifier   <- < ('public' / 'private' / 'protected' / 'static'
                 / 'virtual' / 'override' / 'async' / 'readonly') >
                ![a-zA-Z0-9_] _
  params     <- (param (',' _ param)*)? ','? _               { no_ast_opt }
  param      <- type ident
  type       <- < ident ('<' _ type (',' _ type)* '>')? ('[' _ ']')? > _

  block      <- '{' _ stmt* '}' _                            { no_ast_opt }
  stmt       <- localdecl / ifstmt / whilestmt / forstmt / foreachstmt
              / usingstmt / trystmt / returnstmt / throwstmt / yieldstmt
              / breakstmt / contstmt / block / emptystmt / exprstmt
  localdecl  <- type ident fieldinit? ';' _
  emptystmt  <- ';' _                                        { no_ast_opt }
  ifstmt     <- 'if' _ '(' _ expr ')' _ stmt ('else' ![a-zA-Z0-9_] _ stmt)?
  whilestmt  <- 'while' _ '(' _ expr ')' _ stmt
  forstmt    <- 'for' _ '(' _ forinit ';' _ forcond ';' _ forupd ')' _ stmt
  forinit    <- (localdeclbare / expr)?                      { no_ast_opt }
  localdeclbare <- type ident '=' ![=] _ expr                { no_ast_opt }
  forcond    <- expr?                                        { no_ast_opt }
  forupd     <- expr?                                        { no_ast_opt }
  foreachstmt <- 'foreach' _ '(' _ type ident 'in' ![a-zA-Z0-9_] _ expr ')' _
                 stmt
  usingstmt  <- 'using' _ '(' _ type ident '=' ![=] _ expr ')' _ stmt
  trystmt    <- 'try' _ block catchcl? finallycl?            { no_ast_opt }
  catchcl    <- 'catch' _ '(' _ type ident? ')' _ block      { no_ast_opt }
  finallycl  <- 'finally' _ block                            { no_ast_opt }
  returnstmt <- 'return' ![a-zA-Z0-9_] _ expr? ';' _         { no_ast_opt }
  throwstmt  <- 'throw' ![a-zA-Z0-9_] _ expr ';' _           { no_ast_opt }
  yieldstmt  <- 'yield' __ 'return' ![a-zA-Z0-9_] _ expr ';' _
              / 'yield' __ 'break' ![a-zA-Z0-9_] _ ';' _     { no_ast_opt }
  breakstmt  <- 'break' ![a-zA-Z0-9_] _ ';' _                { no_ast_opt }
  contstmt   <- 'continue' ![a-zA-Z0-9_] _ ';' _             { no_ast_opt }
  exprstmt   <- expr ';' _                                   { no_ast_opt }

  expr       <- assign / cond
  assign     <- postfix assignop expr
  assignop   <- < '+=' / '-=' / '*=' / '/=' / '%=' / '=' ![=] > _
  cond       <- logor ('?' _ expr ':' _ expr)?
  logor      <- logand ('||' _ logand)*
  logand     <- equality ('&&' _ equality)*
  equality   <- relational (eqop relational)*
  eqop       <- < '==' / '!=' > _
  relational <- additive (relop additive)*
  relop      <- < '<=' / '>=' / '<' ![<] / '>' ![>] > _
  additive   <- multiplicative (addop multiplicative)*
  addop      <- < '+' ![+=] / '-' ![-=] > _
  multiplicative <- unary (mulop unary)*
  mulop      <- < '*' ![=] / '/' ![=] / '%' ![=] > _

  unary      <- notexp / negexp / awaitexp / castexp / postfix
  notexp     <- '!' ![=] _ unary                             { no_ast_opt }
  negexp     <- '-' ![-=] _ unary                            { no_ast_opt }
  awaitexp   <- 'await' ![a-zA-Z0-9_] _ unary                { no_ast_opt }
  castexp    <- '(' _ type ')' _ unary                       { no_ast_opt }
  newexp     <- 'new' ![a-zA-Z0-9_] _ type '(' _ args ')' _ initlist?
              / 'new' ![a-zA-Z0-9_] _ type initlist          { no_ast_opt }
  initlist   <- '{' _ (expr (',' _ expr)*)? ','? _ '}' _     { no_ast_opt }

  postfix    <- primary suffix*
  suffix     <- callsfx / membersfx / indexsfx / incsfx
  callsfx    <- '(' _ args ')' _                             { no_ast_opt }
  args       <- (expr (',' _ expr)*)? ','? _                 { no_ast_opt }
  membersfx  <- '.' _ ident                                  { no_ast_opt }
  indexsfx   <- '[' _ expr ']' _                             { no_ast_opt }
  incsfx     <- < '++' / '--' > _

  primary    <- newexp / float / number / string / literal / ident
              / paren
  paren      <- '(' _ expr ')' _                             { no_ast_opt }
  literal    <- < ('true' / 'false' / 'null') ![a-zA-Z0-9_] > _
  float      <- < [0-9]+ '.' [0-9]+ ([eE] [-+]? [0-9]+)? > _
  number     <- < [0-9]+ > _
  string     <- ["] < (!["\\] . / '\\' .)* > ["] _
  ident      <- !keyword < [a-zA-Z_] [a-zA-Z0-9_]* > _
  ~keyword   <- ('break' / 'catch' / 'class' / 'continue' / 'else' / 'false'
               / 'finally' / 'for' / 'foreach' / 'if' / 'in' / 'new'
               / 'null' / 'return' / 'throw' / 'true' / 'try' / 'using'
               / 'while' / 'yield') ![a-zA-Z0-9_]

  ~_         <- ([ \t\r\n] / '//' [^\n]* / '/*' (!'*/' .)* '*/')*
  ~__        <- ![a-zA-Z0-9_] _
)GRAMMAR";

}  // namespace mini_csharp
