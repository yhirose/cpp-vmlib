# mini-lua

The fifth front end, and the one that proves **Tail calls** -- the last
recipe in the top-level README that no front end reached.

Lua is one of the few languages whose *specification* requires proper tail
calls, which is what makes `lua` an oracle for the property rather than an
implementation that happens to agree. And it makes the sample honest in a
way a feature demo usually is not: every recursion in `samples/tailcalls.lua`
runs past `RunOptions::max_call_depth`, so the sample does not merely
exercise `Func::tail_calls`, it **fails without it**:

```
$ mini-lua samples/tailcalls.lua
20000100000
...
$ # the same binary built with fn.tail_calls = false:
samples/tailcalls.lua:1:34: recursion limit exceeded
```

It also proves **Coroutines** in the shape they were named for. `vmlib.h`'s
comment on `CoroCreate` opens with *"What Lua's coroutines, Ruby's Fibers
and a goroutine are made of"*; mini-go drives coroutines from a scheduler
and mini-js from `async`/`await`, so both use them from underneath. Lua
hands `coroutine.create` / `resume` / `yield` / `status` / `wrap` to the
program, which is the other half.

Every sample runs unmodified under `lua` (5.4), and each `golden/` file was
captured that way.

## Running

```
build/examples/mini-lua/mini-lua [--dump-ir] [--dump-bc] PROGRAM.lua
```

## A calling convention written over a calling convention

This is the part that was most instructive to write, and it is not a
recipe in the README because it is not the library's problem: **a function
in this IR returns a value, and a Lua function returns any number of
them.**

The convention is that every function returns an *array* of its results,
and every call site adjusts:

| Context | What the site does |
|---|---|
| a value (`local x = f()`) | `$one` -- the first element, or nil |
| a multiple assignment (`local a, b = f()`) | `$nth` per name |
| the last item of an expression list | the array, appended to the ones before it |
| a table constructor's last field (`{f()}`) | `$spread`, since the count is not known until it runs |
| `(f())` | truncated to one value, which is what Lua's parentheses mean |
| `return f(x)` | the array, handed straight back -- **which is what keeps a tail call a tail call** |

That last row is the one that matters. `return f(x)` compiles to a `Return`
whose operand is a `CallValue` in tail position, so `Func::tail_calls`
turns it into a frame replacement -- and because the callee's answer is
already the shape this function's caller expects, there is nothing to
adjust on the way out. A convention that had to unwrap and rewrap would
have put work after the call and destroyed the tail position.

The price is an array per return, and it is worth being explicit about it:
the IR's calling convention is fixed-arity by design, and putting a
variadic one on top costs an allocation. The hot loop of a tail-recursive
function pays nothing *extra*, because a returned array is what it hands
back either way.

## What Lua does not share with the VM's defaults

Three, and each is a func this binder writes in IR -- the same shape
mini-js needed for JavaScript and mini-culebra needed for almost nothing:

**Truthiness.** Only `nil` and `false` are false; `0` and `""` are *true*.
`Value::truthy()` calls 0 false, and its comment names this exact
disagreement as the reason it refuses to decide: *"JavaScript calls both
falsy, Lua calls neither."* So `$truthy`, and every condition goes through
it.

**`%` and `//` floor.** `-7 % 3` is 2 in Lua and -1 in C, and `BinOp::Mod`
is C's; `-7 // 2` is -4 in Lua and -3 from `BinOp::Div`. `$mod` and `$idiv`
apply the correction, which is needed exactly when the operands' signs
disagree and the division was not exact. (`/` is a third: it is *always* a
float in Lua, even on two integers.)

**`==` across two types is `false`**, where `eval_binop` refuses the
comparison -- *"a question a language answers, not the VM."*

A fourth, which had to be a host function rather than an IR one:
`tostring` of a float. Lua formats with `"%.14g"` and then makes the result
look like a float again, so `10/2` is `"5.0"` and `0.1+0.2` is `"0.3"` --
neither of which is `to_display`'s shortest round trip (`"5"`,
`"0.30000000000000004"`), and neither of which the IR could compute.

## A table is a Map

A Lua table is one container with an array part and a hash part, keyed by
any value. That is `IntrinsicId::MapNew` exactly -- the header's own
distinction between `Object` (string keys, what a struct is indexed by) and
`Map` (value keys) is Lua's distinction between a table's two halves, made
by the same container.

Three rules the binder writes on top of it:

* **`t[1.0]` and `t[1]` are one slot.** `$key` normalizes an integral float
  to the integer it equals before it is used, so a table keyed by
  arithmetic results behaves.
* **Assigning nil removes the key**, which is what keeps `#` and `pairs`
  agreeing about what the table holds.
* **`#` is a border**, found by walking from 1 -- not `Len`, which answers
  how many keys a Map has and would count the hash part too.

The metatable hangs on the same map under a key whose first byte no Lua
source can produce, the trick `vmlib.h`'s own `kDropKey` uses. `pairs` has
to step over it, which is the one place that choice shows.

## The rest of the metamethods, and the walk they all share

`__index` predates everything below and stays written the way it always
was (`rt_get`, inline); `__add`/`__sub`/`__mul`/`__eq`/`__tostring`/
`__call`/`__newindex` all go through two small funcs, `$mt` (a table's
metatable, or nil) and `$mm` (one metamethod off it, or nil):

```
$mm(v, name):
  t = $mt(v)
  if t == nil: return nil
  if not (t has name): return nil
  return t[name]
```

Everywhere a metamethod applies, the shape is the same: try the ordinary
operation, and only when it does not apply, ask `$mm` and call what it
finds. `+`/`-`/`*` on two numbers stay a plain `BinOp` -- the fallback
only runs when at least one operand is not a number, checking the left
operand's metatable first and then the right's, which is Lua's own order.
`==` on two tables tries `__eq` before falling back to `Same`. `$str`
(what `tostring` and string coercion both go through) tries `__tostring`
before the host's own formatter. A call whose target turns out to be a
table checks `__call` at the call site itself, in `emit_suffixed`, since
building the argument list is already there and does not need to move to
a shared helper -- `Adder(1, 2)` on a table with `__call` and a plain
`f(1, 2)` compile to the same `CallValue` either way.

`__newindex` is the one direction: it fires only when the *raw* table
does not already have the key, matching Lua's rule that assigning over an
existing field never consults it, and it dispatches through a function or
recurses into a table the same way `__index` does. Writing a `__newindex`
handler without an escape hatch would be a program bug in real Lua too,
which is what `rawset`/`rawget` are for -- `$set`/`$get` with the
metamethod detour skipped, so a logging `__newindex` can still store the
value.

`samples/operators.lua` is `Vec.__add`/`__sub`/`__mul`, `__eq`, a
`__tostring` that `print` itself has to go through (the host's own
formatter cannot call a Lua closure, so `print` now stringifies every
argument with `$str` before the native ever sees them), and two
`__call`-based callables, one of them stateful.

## The real generic-`for` protocol

`ipairs(t)` and `pairs(t)` were the whole of `for ... in` before: two
library names, matched syntactically and compiled to their own loops.
They still are, for speed, but `for ... in` now also accepts what Lua
actually specifies -- three values (an iterator function, a state, and an
initial control value, `nil`-padded past three) and a loop that calls the
function with `(state, control)`, stops when the first result is `nil`,
and otherwise binds every result to the namelist and keeps the first as
the next control value. `ipairs`/`pairs` are simply two functions that
would satisfy this protocol themselves; special-casing them is an
optimization this front end takes, not a difference in what `for ... in`
means. `samples/operators.lua` has both a stateless iterator (`range`,
closing over nothing) and a stateful one (`enumerate`, a closure that
hands back an index and a value together, the same two-value shape
`pairs` uses) -- checked against `lua` either way.

## `coroutine` over `Coro*`

The mapping is almost one to one, and the places it is not are all Lua's
rules rather than the intrinsics':

* `CoroStatus` answers `"start"`/`"done"`; Lua says `"suspended"`/`"dead"`.
* `CoroResume` answers `{value, done}`; Lua answers `true` followed by the
  values, or `false` and a message.
* Resuming a finished coroutine **traps** in the IR and **answers false**
  in Lua, so `$resume` checks the status first.
* A throw the coroutine's frames do not catch *"continues at the
  CoroResume, into the resumer's own handlers"* -- and Lua turns it into
  `false, err` instead, so `$resume` wraps the resume in a `TryCatch`.
* `coroutine.wrap` is a closure over a coroutine of its own, which raises
  where `resume` would have answered false.

`samples/coroutines.lua` includes the thing a generator could not do:
yielding from three frames down inside the coroutine's body. `CoroYield`
parks every frame from the resume to itself, which is the whole difference
between `Tag::Yield` and a coroutine.

## `pcall` is a TryCatch

Because that is what it is. Both arms build the array Lua answers with --
`true` plus the call's results, or `false` and whatever was raised -- and
the value of the `TryCatch` is the arm that ran.

`error(msg)` prefixes a *string* message with the chunk name and line, Lua's
default error level, which is why the binder is told the script's path.
Anything else is raised unchanged, so a table can carry structured detail.
A trap the executor raises itself -- `nil + 1` -- is caught by the same
`pcall`, since `TryCatch` lands both.

## What is absent, and what quietly differs

Absent: `goto`, varargs (`...` and `select`) -- both would need a
call-site convention change on top of the results-array one already here,
which is more than this front end's remaining budget covers -- string
patterns and `string.format`, `os`, `io` beyond `io.write`, integer
overflow's wrap to float, weak tables, `__gc`, and the remaining
metamethods (`__lt`/`__le`/`__concat`/`__len`/`__unm`/`__div`/`__mod`/
`__pow`).

Quietly different:

* **`#` on a table with a hole may pick a different border.** Lua says any
  border is valid; this front end walks from 1 and Lua uses its array
  part's size, so the two can disagree about a table you have deleted from
  the middle of. The samples do not ask.
* **`pairs` yields insertion order.** Lua's order is unspecified; the
  samples use `ipairs` or a single key so the two cannot disagree.
* **`tostring` of a table or a function has no address.** Lua prints
  `table: 0x...`, which nothing could reproduce.
* **A call in a *non-final* argument position to a user function is
  truncated to one value**, which is Lua's rule; a call in the final
  position expands only for the library functions that take their
  arguments as one array (`print`, `io.write`). A user function's arity is
  fixed at its call site.
* **A library name is not a value**, except `print`, `type` and `tostring`,
  which are `NativeRef`s so that `type(print)` answers. `table`, `string`,
  `math`, `io` and `coroutine` are namespaces this front end matches
  syntactically, not tables it could hand back.
* **A string has no metatable**, so `("x"):rep(3)` is resolved by name at
  bind time -- an object of your own must not use those method names.

## Testing

`ctest -R mini-lua-samples` runs the eight samples and requires each one's
output to match a golden file captured from `lua` -- see PL/0's own README
for why an external, independent oracle is what "passing" means here.

`samples/gen_golden.sh` regenerates the goldens, by hand and never from the
build. Both sides are run from the samples directory with a bare filename,
because Lua's error messages name the chunk as it was given.

All eight samples are clean under ASan/UBSan/LeakSanitizer and under
`COREIR_GC_STRESS=1` -- `tailcalls.lua` included, though its two hundred
thousand iterations times a full collection per allocation take several
minutes under that lane, which is why `ctest` itself does not run it
there and `.github/workflows/ci.yml` leaves out `deep_calls` for the same
reason.
