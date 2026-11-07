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
| mini-scheme | 11 | 1684 |
| PL/0 | 27 | 542 |
| mini-go | 53 | 1547 |
| mini-culebra | 63 | 1719 |
| mini-lua | 69 | 2220 |
| mini-js | 79 | 2499 |
| mini-csharp | 81 | 2235 |
| mini-ruby | 100 | 2584 |
| mini-python | 106 | 4614 |

Eleven rules against fifty to a hundred, and a binder in the same range as
everyone else's. That gap is the point: **the syntax is not the front
end**. What survives when the syntax goes away is scope resolution,
closure conversion and a library -- which is what every other binder here
is mostly made of too, under the parsing.

## Running

```
build/examples/mini-scheme/mini-scheme [--dump-ir] [--dump-bc] [--check-sharing] PROGRAM.scm
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
fold, stopping a `for-each` that has no `break`, and escaping from the
bottom of a hundred tail calls.

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
samples/tailcalls.scm:5:25: recursion limit exceeded
```

`samples/tailcalls.scm` covers the shapes the IR has to see *through* to
find a call still in tail position: the arms of an `if`, every clause of a
`cond`, a `begin`, a `let`, mutual recursion between two procedures, a call
through a value rather than a name, and continuation-passing style, which
is all tail calls by construction. Named `let` -- Scheme's loop -- is in
there too, and it is worth saying plainly what it is: a procedure bound to
a name that tail-calls itself. Nothing about it is a loop construct.

## What is left when the syntax goes away

Eleven grammar rules, and five of them are atoms. There is no expression
grammar, no precedence table, no statement forms and no layout pass,
because an s-expression has none of those -- and `no_ast_opt` only on the
three rules whose node has to survive whatever it holds.

The binder is still seventeen hundred lines. That is the measurement worth
having: what a front end costs is not its syntax. Scope resolution alone is
four different answers to when a name becomes visible -- `let` binds after
its initializers, `let*` between them, `letrec` before them, and an
internal `define` before the whole body -- and none of that gets easier for
being written in parentheses.

Two details of that library are worth noting:

**Every builtin is an ordinary function**, so a symbol naming one is a
*value* with nothing special done for it -- `(map car xs)` works because
`car` is `MakeClosure` of a func like any other. A direct call is the same
`MakeClosure`: every library func is a `Func::singleton` (the **Static
calls** recipe, the same one examples/mini-go describes), so both share one
closure.

**Scheme's variadic builtins fold at the call site**, where the arity is
known: `(+ a b c)` becomes two calls to a two-argument `$add`, and
`(< a b c)` becomes a conjunction of two comparisons, with every operand
evaluated once, in order, into a temporary first -- they are arguments,
so each runs even after an earlier comparison has failed. The same
builtin as a *value* -- `(apply + xs)`, `(fold + 0 xs)` -- is a procedure
with a rest param that runs the same fold at run time. Each builtin's
table entry says how it folds (`(+)` is 0, `(- x)` is `(- 0 x)`, `(< a)`
is `#t`), and both read it, so a call written out and a call through a
value cannot disagree. The written-out call stays plain calls and
allocates nothing; the value pays for one array per call, its rest
param.

## Rest parameters and `apply`

A procedure's own rest parameter -- `(lambda args ...)`,
`(define (f a b . rest) ...)` -- is the top-level README's
`Func::rest_param`. The executor collects the extra arguments into a fresh
array, and the procedure's first line makes that a list, since a Scheme
rest parameter is one; a closure that captures it, or a `set!` to it, sees
the list.

`apply` is the other end of the same call: `(apply f a lst)` is a
`CallSpread` of `f` over `a` and then `lst`'s elements, which `$listarr`
lays out as an array (refusing an improper list, as R7RS does). In tail
position that is a tail call, which R7RS requires of `apply`, and
`samples/variadic.scm` loops through it to twice the executor's call depth.
`apply` as a value is a helper with a rest param whose own `CallSpread` is
in tail position, so a tail call through it stays one too.

## What Scheme does not share with the VM's defaults

* **Only `#f` is false.** `'()` and `0` are both *true*, where
  `Value::truthy()` calls both false -- its comment fixes nil, false and
  zero as false and leaves only a string's emptiness and NaN to the
  language. So Scheme overrides it with `$true`, and every condition goes
  through it.
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
`call-with-values`, and `case-lambda`.

Quietly different:

* **A symbol is a string.** They print the same under `display`, which is
  what the samples compare, but `(eq? 'a "a")` is `#t` here and `#f` in
  Scheme, and `(symbol? "a")` would be wrong -- so `symbol?` is not in the
  library at all rather than being wrong. The same is why a record's
  string field displays without quotes: guile writes a record's fields
  with `write`, which would quote a string and not a symbol, and here the
  two cannot be told apart.
* **A special form always wins over a binding of the same name.** A program
  that rebinds `if` is not in this subset.
* **Numbers are the machine's.** An exact integer that leaves int64 wraps
  rather than growing; Scheme's would be exact.

## Testing

`ctest -R mini-scheme-samples` runs the seven samples and requires each
one's output to match a golden file captured from `guile` -- see PL/0's own
README for why an external, independent oracle is what "passing" means
here.

`test/goldens.sh mini-scheme` checks the goldens against `guile`, and
`--update` regenerates them -- by hand, never from the build. Guile runs
with `--no-auto-compile` (`test/languages.sh`), because without it it
writes progress notes the first time it sees a file, a record of the
cache's state rather than of the program.

All seven samples are clean under ASan/UBSan/LeakSanitizer and under
`COREIR_GC_STRESS=1` (`just sanitizers`, `just gc-stress`), `tailcalls.scm`
included -- its million iterations times a full collection per allocation
take seconds, not the quadratic blowup that keeps `deep_calls` out of that
lane.
