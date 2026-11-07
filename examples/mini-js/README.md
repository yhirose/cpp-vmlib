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
build/examples/mini-js/mini-js [--dump-ir] [--dump-bc] [--check-sharing] FILE.js [FILE.js ...]
```

More than one file is concatenated, in order, into one program. That is
not something `node` does, and it exists for exactly one reason: the
samples share `samples/prelude.js` without this subset needing a module
system it has no other use for. The oracle side of the same comparison is
`cat prelude.js sample.js | node`; see `test/languages.sh`.

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
values within ±2^53 through the integer path, spells `NaN` and `±Infinity`
itself, and leaves the rest to `ToStr`.

Each of those is a `$`-named func built once per module, and the `$` keeps
them out of the source language's reach -- the same convention mini-go's
channel runtime uses. Each call site is a plain `MakeClosure` +
`CallValue`; the helpers are `Func::singleton` (the top-level README's
**Static calls** recipe), so the executor builds each one's closure once
per run, however many of the hundreds of call sites a program's conditions
add up to name it.

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
  literal true  @2:1                  ; while (true)
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
        literal false
      break  @2:1
    block  @2:29
      callvalue  @3:5                  ; fs.push(...)
        ...
        makeclosure <arrow> #34 cmap=3 ; captures cell[0] -- *this* box
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

**A Promise is an object.** Its state -- pending, fulfilled or rejected
-- the settled value, and the reactions waiting on it, under keys no
property name can be, so a promise has none of them as properties and an
object that merely looks like one is awaited as itself. `$psettle` sets
the state once and `Enqueue`s every waiting reaction. These, `$await` and
the rest of the settling are
[examples/common/runtime_parts.h](../common/runtime_parts.h)'s, shared
with mini-csharp's Task. `$then` and `$react` are JavaScript's
combinators, including the three rules that make a chain behave: a link
with no handler passes the state through, a handler that throws rejects
the next promise, and resolving with a promise *adopts* it rather than
nesting it.

**`await` parks the coroutine directly.** `Enqueue` accepts a closure *or a
coroutine*, so `$await` registers `CoroCurrent()` itself as the promise's
reaction -- settling the promise wakes the awaiting function with no
callback hop in between:

```
$await(v):
  p = $topromise(v)
  $pon(p, CoroCurrent())      ; wake me when it settles
  CoroYield(nil)
  if (p is rejected) throw its reason
  return its value
```

Because `CoroYield` parks *every* frame from the coroutine's bottom up to
itself, this works from inside a call the async body makes, not only at the
body's own top level -- which is what an `await` in a helper function needs
and what a generator's single-frame `Yield` could not do.

**An async function is three funcs**, written by
[examples/common/runtime_parts.h](../common/runtime_parts.h)'s
`write_async`, which mini-csharp's async methods are lowered by too.
`--dump-ir` on
`async function twice(v) { return (await v) * 2; }`:

```
func #40 twice  locals=1 captures=0
  cellfresh cell[0]; assign cell[0] <- $pnew()          ; the promise
  cellfresh cell[1]; assign cell[1] <- makeclosure twice$body #41
  cellfresh cell[2]; assign cell[2] <- varref local[0]  ; the argument
  cororesume
    corocreate
      makeclosure twice$run #42 cmap=5
    literal nil
  return varref cell[0]                                  ; the promise
func #41 twice$body   ; the body you wrote, with twice's own captures
func #42 twice$run    ; calls the body, settles the promise with the
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

A class is a plain object whose table of methods holds one closure per
member -- `constructor` under that name too -- built at the class
declaration's own lexical position. That last part is load-bearing, not
a style choice: `emit_classdecl` is what a method's free variables were
resolved *against* (`resolve_fn`'s `is_method` flag just prepends a
synthetic `this` to the same machinery every other function here uses),
so its closures have to be minted from that exact spot, the way
examples/mini-python's constructor and examples/mini-ruby's method table
are built at their own `class`/`def` rather than wherever `new` happens
to run. The table is apart from the class's own properties, so
`ClassName.m = f`, a static in JavaScript, leaves what an instance's `m`
calls alone, while `instance.constructor === ClassName` still reads true
the way real JavaScript's does. The table itself sits under
`"\x01methods"`, a key no property name a program writes can be -- the
[shared convention](../common/runtime_parts.h) for the implementation's
own keys, which the collection a Map or a Set wraps follows too -- so
even a static named `$methods` is only a static.

`new ClassName(args)` copies every member onto a fresh instance and calls
whichever one is named `constructor`. What it copies is not quite the raw
closure, though: a method's own signature is `(this, ...declared)`, since
that is what a call *through the class table* would need, but
`instance.method(args)` -- the call this front end already knows how to
make, `emit_method`'s generic fallback -- calls with just `(...declared)`.
So each instance gets a small per-member wrapper instead, closing over
the class table and itself, that supplies `this` and forwards the rest.
It is built once per `new` expression (a `Func` and capture map shared
across however many times that expression runs, fresh cells and a fresh
closure per instance), the
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
(declared or as an object literal's property) gets no `this` of its own:
real JavaScript binds a plain function's `this` *dynamically*, by how it is
called, which this front end does not model outside of a class's own
methods. Outside any method, `this` there is a bind-time error; inside
one, it resolves outward to the method's `this` the way an arrow's would
(see "quietly different" below).

## Rest parameters and spread

A rest parameter is `Func::rest_param` -- the top-level README's
**Variadic calls** -- and already the right type, since JavaScript's is an
Array too. A call with `...` among its arguments is a `CallSpread`: the
arguments before the first `...` as they are, and everything from there
on as one array. A call that ends in a single `...xs` passes `xs` itself,
because the `CallSpread` copies it when it runs and nothing after it can
run first; anywhere else each `...x` is copied where it stands, as
JavaScript reads it, so `f(...a, a.push(1), ...a)` sees `a` before the
push and after it. The copy is `$arrayof(...x)` -- a `CallSpread` into a
helper whose rest param is its whole answer -- so checking that `x` is an
array and copying it are the executor's own work, with its own words for
a value that is not one. Array literals build `[a, ...xs]` from the same
pieces.

Two of this front end's functions forward their arguments to another:
an async function's launcher hands them to its body, and `new` gives each
instance a wrapper per method that adds `this`. Both take a rest param
where the function they forward to has one, and spread it back out, so
the callee's own rest param collects it afresh.

`console.log` and `push` take any number of arguments, and a spread among
them; `a.push(1, 2)` used to push only the first. Every other builtin
matched by name reads its arguments one value each, and a spread in one is
a bind-time error rather than a silently dropped argument.

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
* **`i++`/`i--` as expressions.** Statements and `for` updates only;
  `let j = i++` is a bind-time error.
* **Destructuring, template literals, `switch`, labels, `for...in`,
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
  much as the operator's other meaning. Comparing across types traps the
  same way: `1 < 2 < 3` compares `true` with `3`, which JavaScript turns
  into `1 < 3`.
* **A plain function inside a method sees the method's `this`.** It
  resolves outward like an arrow's, where JavaScript would rebind it by
  how the function is called -- `function f() { return this.x }` called
  bare inside a method answers the instance's `x` here.
* **Only an array can be spread.** JavaScript spreads any iterable -- a
  string, a Map, a Set, a generator; here anything but an array stops the
  run with the executor's "cannot spread" error.
* **An unhandled rejection is silent.** `node` prints the error and exits
  1; here the promise simply stays rejected. (An `await` on a promise
  nothing ever settles, on the other hand, matches: both just exit.)

## Testing

`ctest -R mini-js-samples` runs the nine samples and requires each one's
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
about what the program computes. (The prelude is also its own first test:
mutual recursion between two hoisted declarations, closures, `for` over
arrays, and `typeof`.)

Beyond the sample check, all nine samples are clean under the two lanes
`.github/workflows/ci.yml` runs over the rest of the suite --
ASan/UBSan/LeakSanitizer, and `COREIR_GC_STRESS=1` (a full collection at
every single allocation).

`ctest -R mini-js-uncaught` runs `uncaught/`, programs that each end in
an exception nothing catches, and requires the report -- message,
position and exit code -- to match `uncaught/expected/`, through the
`test/run_errors.cmake` PL/0's error cases use. The words after the
position are `RunOptions::describe`'s, and they are `node`'s own:
the line under its source excerpt, which each case's comment quotes.
