# mini-js

The third front end for cpp-vmlib's Core-IR, and the dynamic counterpart
to mini-go. The top-level README's **Scope** section calls a JavaScript
subset "the easy case" for this library; this front end is that claim
turned into running code, checked against `node`. Where PL/0 validated the
IR at all and mini-go proves the *managed, statically-typed* recipes
against `go run`, this one proves the recipes a dynamic language needs:
**Closures**, **Exceptions**, **Generators**, **Maps**, **Strings and
slices**, and **Coroutines** + **Scheduler** (as `async`/`await` over a
Promise it writes itself).

A source file here is real JavaScript -- every sample in `samples/` runs
unmodified under `node`, and each sample's `golden/` output was captured
that way, the same relationship PL/0's samples have to culebra's own PL/0
interpreter and mini-go's have to `go run`.

## Running

```
build/examples/mini-js/mini-js [--dump-ir] [--dump-bc] FILE.js [FILE.js ...]
```

More than one file is concatenated, in order, into one program. That is
not something `node` does, and it exists for exactly one reason: the
samples share `samples/prelude.js` without this subset needing a module
system it has no other use for. The oracle side of the same comparison is
`cat prelude.js sample.js | node`; see `samples/gen_golden.sh`.

## What the VM does *not* decide

This is the part worth reading if you are writing your own dynamic front
end. `vmlib.h` refuses, on purpose, to answer three questions that look
like they belong to a runtime and do not:

**What is true.** `Value::truthy()` calls the empty string and `NaN` true,
and its own comment says why: *"JavaScript calls both falsy, Lua calls
neither, and that disagreement is what makes it a language's decision
rather than the VM's."* So `$truthy` is a func this binder writes in IR,
and every condition, `!`, `&&` and `||` goes through it. (Not quite every
one: a comparison already produced a bool and ToBoolean is the identity on
those, so `i < n` skips the call -- see `Binder::truthy`.)

**What is equal.** `BinOp::Eq` refuses two values of different types --
*"a question a language answers, not the VM"* -- which is a trap where
JavaScript wants `false`. `$seq` is `===`: different types are unequal,
`nil` equals `nil`, scalars compare by value, and everything else is
`IntrinsicId::Same`, reference identity.

**How a number prints.** `ToStr` is shortest-round-trip, which agrees with
ES `Number::toString` on `0.1 + 0.2` and on `3.5` but has no reason to
prefer `"3"` over `"3E0"` for a whole double. `$numstr` routes integral
values within ±2^53 through the integer path and leaves the rest to
`ToStr`.

Each of those is a `$`-named func built once per module, and the `$` keeps
them out of the source language's reach -- the same convention mini-go's
channel runtime uses. They are reached through a cell the calling
function's preamble fills once per activation (the top-level README's
**Static calls** recipe), not through a fresh `MakeClosure` at each of the
hundreds of call sites a program's conditions add up to.

## A worked example: `let` in a loop is a new binding every time

`for (let i = 0; i < 3; i++) fs.push(() => i)` must give three closures
that answer 0, 1 and 2. That is ES2023 14.7.4.4 -- each iteration gets its
own binding, seeded from the previous one -- and it is the single most
common thing a naive lowering gets wrong (`var`, which this subset
deliberately does not have, is the version that answers 3, 3, 3).

`Tag::CellFresh` is exactly this and nothing else: *"Replaces one of the
frame's cells with a fresh, nil-holding box... closures built in earlier
iterations keep the old cell."* `--dump-ir` on that loop:

```
while  @2:1
  literal 1  @2:1                     ; while (true)
  block  @2:1
    if  @2:1
      varref local[1]  @2:1           ; $first?
      assign local[1]  ...            ;   first time: just clear the flag
      block  @2:1                     ;   every later time:
        assign local[2]                ;     carry = i
          varref cell[0]
        cellfresh cell[0]              ;     a new binding
        assign cell[0]                 ;     i = carry
          varref local[2]
        assign cell[0]  @2:24          ;     i++  (on the new binding)
          ...
    if  @2:1
      eq                               ; if (!(i < 3)) break
        lt  @2:19
          varref cell[0]  @2:17
        literal 0
      break  @2:1
    block  @2:29
      callvalue  @3:5                  ; fs.push(...)
        ...
        makeclosure <anon> #34 cmap=3  ; captures cell[0] -- *this* box
```

Two things in that shape are load-bearing:

**The update runs at the top, behind a flag.** `Tag::Continue` re-tests the
loop's condition, so an update written at the bottom of the body would be
skipped by every `continue`. Hoisting it to the top and guarding the first
pass runs it on every path back into the loop and on none into the first
iteration.

**The carry/`cellfresh`/copy trio comes before the update, not after.**
JavaScript copies the old binding's value into the new binding and *then*
runs `i++` on the new one. A closure made during iteration 2 captured the
box that existed then, and nothing later can reach into it.

The same rule has a second, quieter consequence, and it is the bug this
front end actually shipped on its first pass: `CellFresh` cannot be emitted
at the *declaration statement*. A hoisted function declared at the top of a
block captures the cell of a `const` declared below it, and a `CellFresh`
running later at that `const` swaps the box out from under a capture the
closure is already holding -- so `log.push(...)` inside it pushed into
`nil`. ES2023 14.2.3 says the bindings are created when the block is
*entered* and only initialized where they stand, so that is where
`Binder::emit_block` emits them. Doing it there keeps the per-iteration
binding for free, because a loop body *is* a block and entering it again
is precisely when JavaScript wants new bindings.

## `finally` is a Defer, not a copy

`try { ... } finally { ... }` could be lowered by duplicating the finally
block down every exit path. It is not, because there are more paths than
they look like -- falling through, `return`, `break`, `continue`, and a
throw nothing here catches -- and `Tag::Defer` already runs on all of them:
*"however it exits... LIFO within the scope."*

```
Scope {
  Defer(MakeClosure(<finally>, cmap))
  TryCatch($exc, body, handler)
}
```

The consequence is that a `finally` block is resolved as a *function of its
own* -- `Defer` takes a callable, and a callable reaches the enclosing
frame only through cells. That falls out of the same analysis everything
else uses: anything the finally block reads becomes a capture, and so
becomes a cell in the enclosing function. `samples/errors.js` covers the
four exits that make this worth doing: through a `return`, through a
`break` out of a loop, through a rethrow, and through a throw with no
`catch` clause at all (where the handler this binder synthesizes just
re-raises, so the Defer still runs on the way past).

## Generators need no protocol translation

`IntrinsicId::GenResume` answers a fresh `{value, done}` object, and the
header says why that shape: *"the JS result shape, chosen because it
carries 'finished' and 'what came out' in one allocation."* So
`g.next()` **is** `GenResume(g, sent)`, with nothing in between --
`g.return(v)` is `GenReturn`, `g.throw(e)` is `GenThrow`, and a
`for...of` over a generator is `GenResume` in a loop. `samples/generators.js`
checks the parts that are easy to get wrong and that this front end got for
free: a generator body does not start until the first resume, `yield` is an
expression that evaluates to what the *next* resume sent, `.return()` runs
the body's pending `finally` before reporting done, and `.throw()` is
catchable by the body's own `try`.

## async/await: a coroutine, a job queue, and a Promise written in IR

The top-level README's **Coroutines** and **Scheduler** sections, in
JavaScript's vocabulary. Nothing about promises is in `vmlib.h`, and
nothing needed to be. The library supplies a coroutine that can park a
whole stack of frames (`CoroCreate`/`CoroYield`/`CoroCurrent`) and one FIFO
of jobs (`Enqueue`); *which* queue discipline settles what, in what order,
is JavaScript's rule and not every language's, so it lives here -- the same
division mini-go draws for a Go channel.

**A Promise is an object.** `{$promise, s: 0|1|2, v, cbs}` -- pending,
fulfilled or rejected, the settled value, and the reactions waiting on it.
`$psettle` sets the state once and `Enqueue`s every waiting reaction;
`$then` and `$react` are the combinators, including the three rules that
make a chain behave: a link with no handler passes the state through, a
handler that throws rejects the next promise, and resolving with a promise
*adopts* it rather than nesting it.

**`await` parks the coroutine directly.** `Enqueue` accepts a closure *or a
coroutine*, so `$await` registers `CoroCurrent()` itself as the promise's
reaction -- settling the promise wakes the awaiting function with no
callback hop in between:

```
$await(v):
  p = $topromise(v)
  $pon(p, CoroCurrent())      ; wake me when it settles
  CoroYield(nil)
  if (p.s === rejected) throw p.v
  return p.v
```

Because `CoroYield` parks *every* frame from the coroutine's bottom up to
itself, this works from inside a call the async body makes, not only at the
body's own top level -- which is what an `await` in a helper function needs
and what a generator's single-frame `Yield` could not do.

**An async function is three funcs.** `--dump-ir` on
`async function twice(v) { return (await v) * 2; }`:

```
func #34 twice  locals=1 captures=0
  cellfresh cell[0]; assign cell[0] <- $pnew()          ; the promise
  cellfresh cell[1]; assign cell[1] <- makeclosure twice$body #36
  cellfresh cell[2]; assign cell[2] <- varref local[0]  ; the argument
  cororesume
    corocreate
      makeclosure twice$run #37 cmap=6
    literal 0
  return varref cell[0]                                  ; the promise
func #36 twice$body   ; the body you wrote, with twice's own captures
func #37 twice$run    ; calls the body, settles the promise with the
                      ; value it returned or the value it threw
```

`CoroResume`, not `Enqueue`, and that difference is observable: an async
function's body runs **synchronously up to its first `await`** (ES2023
27.7.5.1), so the call returns to its caller only once the body has
suspended or finished. Spawning it onto the job queue instead --
`Enqueue(CoroCreate(...))`, which is what mini-go's `go` statement does,
because a goroutine's rule *is* "later" -- put the body's first line after
the caller's next line, and `samples/async.js` caught it as a one-line
ordering diff against `node`. From the first `await` onward the scheduler
does own the coroutine: whoever settles the promise it parked on enqueues
it.

The arguments go into fresh cells at the call (JavaScript's own rule --
they are evaluated at the call, not at the first resume), so two calls in
flight at once do not share them.

## `class`, without a prototype in the IR

A class is a plain object holding one closure per member -- `constructor`
under that name too, so `instance.constructor === ClassName` reads true
the way real JavaScript's does -- built at the class declaration's own
lexical position. That last part is load-bearing, not a style choice:
`emit_classdecl` is what a method's free variables were resolved
*against* (`resolve_fn`'s `is_method` flag just prepends a synthetic
`this` to the same machinery every other function here uses), so its
closures have to be minted from that exact spot, the way examples/mini-
python's constructor and examples/mini-ruby's method table are built at
their own `class`/`def` rather than wherever `new` happens to run.

`new ClassName(args)` copies every member onto a fresh instance and calls
whichever one is named `constructor`. What it copies is not quite the raw
closure, though: a method's own signature is `(this, ...declared)`, since
that is what a call *through the class table* would need, but
`instance.method(args)` -- the call this front end already knows how to
make, `emit_method`'s generic fallback -- calls with just `(...declared)`.
So each instance gets a small per-member wrapper instead, closing over
the class table and itself, that supplies `this` and forwards the rest.
It is built once per `new` expression (a `Func` shared across however
many times that expression runs, a fresh capture map per instance), the
same shape `new Promise(...)` above already uses for its own per-
occurrence state.

`new` needs the class *by name*, at the call site: `ref_of` on the
identifier has to land on a binding `class_by_var` recognizes, because
building each wrapper needs a method's exact declared parameter count,
known only from the class's own `ClassInfo`. `new Alias(...)`, where
`Alias` is a variable a function *returned* a class through rather than
the class's own name, is a compile-time error naming so -- `samples/
classes.js`'s `makeAdder` instantiates where the class is declared for
exactly this reason.

An arrow function written inside a method closes over `this` lexically,
through the ordinary capture machinery (`this` is a scoped variable,
`resolve()` walks to it like any other free name) -- which is
JavaScript's own rule for an arrow's `this`. An ordinary `function`
(declared or as an object literal's property) does not get one: real
JavaScript binds a plain function's `this` *dynamically*, by how it is
called, which this front end does not model outside of a class's own
methods -- `this` used there is a bind-time error.

## What is deliberately absent, and what quietly differs

Absent, because none of it exercises a recipe further:

* **`extends`, and therefore `super`.** A subclass's method table would
  need to fall back to its base's, which is the same walk-up-a-chain
  recipe examples/mini-python's and examples/mini-ruby's inheritance is
  built on -- reasonable to add, just not inside this front end's
  remaining budget once the calling-convention work above was done.
* **`var`.** Its function-scoped, hoisted binding is the *absence* of the
  per-iteration rule `CellFresh` exists to implement, so having it would
  subtract from what this front end demonstrates. Declaring one is a
  bind-time error rather than a silent `let`.
* **Destructuring, spread, template literals, `switch`, labels, `for...in`,
  regular expressions, getters/setters, `yield*`, async arrows and
  top-level `await`.** Syntax, mostly. `switch` is already mini-go's to
  prove, and top-level `await` would need `funcs[0]` itself to be a
  coroutine.
* **`==` and `!=`.** JavaScript's loose equality is the coercion ladder,
  and the ladder is what this subset does not have. Writing one is a
  bind-time error naming `===`.

Quietly different, and therefore worth knowing:

* **`null` and `undefined` are the same value.** Both lower to nil, so
  `null === undefined` answers `true` here and `false` in JavaScript, and
  `typeof null` is `"undefined"` rather than `"object"`. Distinguishing
  them would need a second scalar the IR does not have, or a heap
  singleton every site would have to reach.
* **A builtin method name wins over a property.** `x.push(...)`,
  `.slice`, `.join`, `.pop`, `.next`, `.return`, `.throw`, `.get`, `.set`,
  `.has`, `.delete`, `.add`, `.then` and `.catch` are resolved at bind time
  -- a subset without prototypes has no runtime lookup to do it with -- so
  an object of your own, or a class's own method table, must not put a
  function under one of those names. They are also not readable as
  properties: `typeof p.then` is
  `"undefined"`.
* **Numbers outside the samples' range print differently.** `1e21`,
  `1e-7` and anything else `$numstr` leaves to `ToStr` come out in
  `to_chars`' exponent form (`1e+21`, `1e-07`) rather than ES
  `Number::toString`'s (`1e+21`, `1e-7`). Reproducing that spelling is a
  digit-generation exercise that would prove nothing about the IR.
* **Arithmetic on non-numbers traps instead of producing `NaN`.** `'a' * 2`
  is a run failure here and `NaN` in JavaScript; the coercion ladder again.
  `+` is the exception, because string concatenation is not coercion so
  much as the operator's other meaning.
* **An unhandled rejection is silent.** `node` prints the error and exits
  1; here the promise simply stays rejected. (An `await` on a promise
  nothing ever settles, on the other hand, matches: both just exit.)

## Testing

`ctest -R mini-js-samples` runs the eight samples and requires each one's
output to match a golden file captured from `node` -- see PL/0's own README
for why an external, independent oracle is what "passing" means here, not
just this binary agreeing with itself.

Every sample is run with `samples/prelude.js` ahead of it, and the golden
was captured the same way. The prelude is the formatting the samples print
through, written in the source language on purpose: `console.log` of an
array or an object is Node's `util.inspect`, a renderer with its own rules
about quoting, spacing, depth and colour that have nothing to do with the
language, and reproducing those would be work that proves nothing. Both
sides running the same JavaScript to build their output strings leaves the
comparison testing the only thing worth testing -- whether the two agree
about what the program computes. (The prelude is also the largest program
in this subset, and its own first test: mutual recursion between two
hoisted declarations, closures, `for` over arrays, and `typeof`.)

`samples/gen_golden.sh` regenerates the goldens. It is run by hand, never
by the build, for the reason `test/gen_golden.sh` states: a golden file the
tests could refresh themselves is not evidence of anything.

Beyond the sample check, all eight samples are clean under the two lanes
`.github/workflows/ci.yml` runs over the rest of the suite --
ASan/UBSan/LeakSanitizer, and `COREIR_GC_STRESS=1` (a full collection at
every single allocation).
