# mini-scheme

The seventh front end, and the one that shows a **boundary** rather than a
recipe.

The top-level README's Scope section lists what this library cannot reach,
and one entry names Scheme:

> *Multi-shot continuations*: a coroutine is one-shot -- its parked frames
> move, they are not copied -- and Scheme's full `call/cc` would need a rule
> for what a copied cell means that nothing here has.

Scheme is the one language that lets that sentence be *shown* instead of
asserted, because `call/cc` splits cleanly in two and only one half is out
of reach.

It also has the smallest *grammar* here by a wide margin, which turns out
to be the more interesting measurement:

| Front end | Grammar rules | Binder |
|---|---:|---:|
| mini-scheme | 11 | 1406 |
| PL/0 | 27 | 542 |
| mini-go | 49 | 1307 |
| mini-culebra | 63 | 1821 |
| mini-lua | 68 | 1990 |
| mini-js | 72 | 2276 |
| mini-python | 73 | 2426 |

Eleven rules against seventy, and a binder in the same range as everyone
else's. That gap is the point: **the syntax is not the front end**. What
survives when the syntax goes away is scope resolution, closure conversion
and a library -- which is what every other binder here is mostly made of
too, under the parsing.

## Running

```
build/examples/mini-scheme/mini-scheme [--dump-ir] [--dump-bc] PROGRAM.scm
```

## The half that works

An **escape** continuation -- one invoked while the `call/cc` that made it
is still on the stack -- is an unwind to a known point. That is
`Tag::TryCatch` and `Tag::Throw`:

```
Scope {
  tag = a fresh object, whose *identity* names this activation
  TryCatch(exc,
    body:    f(<escape closure capturing tag>)
    handler: exc carries our tag ? its value : rethrow)
}
```

Invoking the escape throws a value tagged with that identity; the throw
unwinds to the `call/cc` and the handler answers with it. Nested `call/cc`s
each name their own activation, which is why the inner one leaves only the
inner. `samples/continuations.scm` uses it for the four things programs
actually use `call/cc` for: an early return with a value, escaping out of a
fold, stopping a `for-each` that has no `break`, and unwinding a hundred
frames at once.

## The half that does not, and what it says when you ask

Storing a continuation and invoking it *after* its `call/cc` has already
returned is the multi-shot case. It cannot work here, and the front end
says so rather than doing something almost right:

```
$ cat reentry.scm
(define saved #f)
(display (call/cc (lambda (k) (set! saved k) 1)))
(newline)
(saved 2)

$ guile -s reentry.scm
1
1
...

$ mini-scheme reentry.scm
1
reentry.scm:1:1: uncaught: call/cc: a continuation was invoked after the
call/cc that made it had already returned. This front end supports escape
continuations only -- see examples/mini-scheme/README.md
```

The mechanism is worth knowing because it is the shape of every honest
limit: the escape token is still thrown, and the `TryCatch` that would have
caught it is gone, so it reaches the entry frame -- where the binder wraps
the whole program in one more handler whose only job is to recognize a
stray token and name the reason. A limit that reports itself is worth more
than one that has to be read about.

## Tail calls, where they are not an optimization

examples/mini-lua proves `Func::tail_calls` against a specification that
requires it. Scheme is where the property is *load-bearing*: iteration is
tail recursion and nothing else, so the flag is the difference between a
loop working and not.

```
$ mini-scheme samples/tailcalls.scm
500000500000
...
$ # the same binary built with fn.tail_calls = false:
samples/tailcalls.scm:1:25: recursion limit exceeded
```

`samples/tailcalls.scm` covers the shapes the IR has to see *through* to
find a call still in tail position: the arms of an `if`, every clause of a
`cond`, a `begin`, a `let`, mutual recursion between two procedures, a call
through a value rather than a name, and continuation-passing style, which
is all tail calls by construction. Named `let` -- Scheme's loop -- is in
there too, and it is worth saying plainly what it is: a procedure bound to
a name that tail-calls itself. Nothing about it is a loop construct.

## What is left when the syntax goes away

Eleven grammar rules, and four of them are atoms. There is no expression
grammar, no precedence table, no statement forms and no layout pass,
because an s-expression has none of those -- and no `no_ast_opt` anywhere,
because there is no rule that could fold wrongly.

The binder is still fourteen hundred lines. That is the measurement worth
having: what a front end costs is not its syntax. Scope resolution alone is
four different answers to when a name becomes visible -- `let` binds after
its initializers, `let*` between them, `letrec` before them, and an
internal `define` before the whole body -- and none of that gets easier for
being written in parentheses.

Two details of that library are worth noting:

**Every builtin is an ordinary function**, so a symbol naming one is a
*value* with nothing special done for it -- `(map car xs)` works because
`car` is `MakeClosure` of a func like any other. A direct call goes through
the **Static calls** recipe's cell instead, which is the same optimization
examples/mini-go describes.

**Scheme's variadic forms fold at the call site**, where the arity is
known: `(+ a b c)` becomes two calls to a two-argument `$add`, and
`(< a b c)` becomes a conjunction of two comparisons with the middle
operand shared. The IR's calls are fixed-arity by design and Scheme's are
not, which is the same mismatch examples/mini-lua solves the other way (an
array per call) -- two front ends, two answers, and the difference is that
Lua needs the *count* at run time and Scheme does not.

## What Scheme does not share with the VM's defaults

* **Only `#f` is false.** `'()` and `0` are both *true*, where
  `Value::truthy()` calls both false -- and its comment names precisely
  this disagreement as the reason it will not decide. So `$true`, and every
  condition goes through it.
* **A whole inexact number prints with a point.** `to_display` gives `"1"`
  for `1.0`, and Guile gives `1.0`; its comment says a front end that cares
  builds the string, so `$fstr` does.
* **`modulo` follows the divisor's sign** where `BinOp::Mod` (like C's
  `%`, and like `remainder`) follows the dividend's.

## `case`, and `define-record-type` -- R7RS's answer to "a class"

Both are ordinary additions once `cond`'s machinery exists: `case`
evaluates its key once, into a local, and compares it against each
clause's literal data with `eqv?` -- one nested `If` per clause, the same
shape `emit_cond` already builds, just comparing instead of testing.

`define-record-type` needed nothing from the IR that classes elsewhere in
this repository did not already ask for: a record instance is a plain
object under a key no Scheme symbol can start with, and the constructor,
predicate, and every accessor and mutator the form names are built
directly rather than compiled from a body -- there is no source to
compile, the way examples/mini-python's constructor and
examples/mini-ruby's `attr_accessor` methods have none either. A field
with no mutator listed simply gets none; nothing enforces immutability
beyond that no name exists to call.

Two things about testing it are worth knowing. `guile` does not put
`define-record-type` in its default top-level environment -- it lives in
the R7RS library `(scheme base)`, so `samples/records.scm` opens with
`(import (scheme base) (scheme write))`, which this front end recognizes
and does nothing with (`import` names libraries, not expressions, and
there is only one namespace here to import into). And `(eq? '(1) '(1))`,
which `samples/basics.scm` used to check, turned out to be asking
Guile a question R7RS leaves unspecified -- whether two textually
identical quoted literals share storage -- and current Guile answers
differently depending on its compilation cache, which this front end
(never interning a literal) does not. The sample now asks `eq?` about the
same list bound to a name instead, which every implementation agrees on.

## What is absent, and what quietly differs

Absent: exact rationals (`(/ 1 2)` is `1/2` in Scheme and `0.5` here),
bignums (`examples/mini-python` is where that recipe lives), `define-syntax`
and macros, `do`, vectors, characters, ports, `dynamic-wind`, `values` and
`call-with-values`, variadic lambdas (`(lambda args ...)`) and `apply` --
both would need a call-site convention change on top of the one this
front end already writes for `+`/`<`/`list`, which is more than its
remaining budget covers -- and tail patterns in `case-lambda`.

Quietly different:

* **A symbol is a string.** They print the same under `display`, which is
  what the samples compare, but `(eq? 'a "a")` is `#t` here and `#f` in
  Scheme, and `(symbol? "a")` would be wrong -- so `symbol?` is not in the
  library at all rather than being wrong.
* **A special form always wins over a binding of the same name.** A program
  that rebinds `if` is not in this subset.
* **`(list e ...)` folds at the call site**, so `list` used as a *value*
  is the one-argument version.
* **Numbers are the machine's.** An exact integer that leaves int64 wraps
  rather than growing; Scheme's would be exact.

## Testing

`ctest -R mini-scheme-samples` runs the six samples and requires each
one's output to match a golden file captured from `guile` -- see PL/0's own
README for why an external, independent oracle is what "passing" means
here.

`samples/gen_golden.sh` regenerates the goldens, by hand and never from the
build. It passes `--no-auto-compile`, because without it Guile writes
progress notes to stderr the first time it sees a file and the golden
becomes a record of the cache's state rather than of the program.

All six samples are clean under ASan/UBSan/LeakSanitizer and under
`COREIR_GC_STRESS=1` -- `tailcalls.scm` included, though its million
iterations times a full collection per allocation take real time under
that lane, which is why `ctest` itself does not run it there and
`.github/workflows/ci.yml` leaves out `deep_calls` for the same reason.
