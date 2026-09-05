# mini-culebra

The fourth front end, and the one whose oracle is the language this library
was written to rehearse a back end for. Three mechanisms in `vmlib.h` are
the shape they are *because* of culebra, and say so in their own comments;
no other front end here touches any of them. This is where they are shown
working, against `culebra` itself.

| What | `vmlib.h` says | Here |
|---|---|---|
| The owned stack | "Deterministic drop for cycles: the owned stack (culebra's design)." | `class R { drop() {...} }` and `{drop: fn () {...}}`, and `samples/drops.cul` |
| A Scope's explicit release order | "A front end that needs reverse declaration order across the two hands the scope its release list as an optional second child." | Every block, `Binder::release_list` |
| `entry_frame_drops = false` | "That is culebra's rule for top-level bindings (only top-level defers run at exit)." | `main.cc`, plus a `[0, 0)` Scope on `funcs[0]` |

And one recipe from the top-level README that no other front end here
reaches at all: **Host functions**. culebra's standard library is not part
of its language, and it is not part of this IR either -- `println`,
`type_of`, `.size()`, `.push()`, `.map()` are `Tag::NativeRef`, declared by
the module and supplied by the run.

Every sample runs unmodified under `culebra`, and each `golden/` file was
captured that way.

## Running

```
build/examples/mini-culebra/mini-culebra [--dump-ir] [--dump-bc] PROGRAM.cul
```

## What this front end did *not* have to write

The interesting comparison is with [mini-js](../mini-js/), which writes
`$truthy`, `$seq`, `$add` and `$numstr` in IR because JavaScript disagrees
with the VM about what is true, what is comparable and how a number prints.
culebra mostly does not:

* **`if` takes a Bool, a Long or a Float and nothing else** -- which is
  `Value::truthy()`'s own rule for those three -- so a condition here is a
  bare `Tag::If` with no call in front of it. (`if ''` is a *type error* in
  culebra; mini-culebra does not raise it, which is the one place it is
  laxer than the language.)
* **`/` on two Longs is integer division and `%` is C's**, which is
  `BinOp::Div` and `BinOp::Mod` unchanged: `eval_binop`'s "two ints stay
  integers" is culebra's rule already.
* **A Long is `Value`'s Int and a Float is its Double.** There is no single
  number type to normalize into and back out of.

Two things did need writing, and they are exactly the two the header
predicts:

**`==`.** `eval_binop` refuses two values of different types -- *"a
question a language answers, not the VM"* -- where culebra answers `false`;
and culebra compares arrays and objects *structurally*, which no intrinsic
does. `$eq` is that, recursively.

**Float display.** `to_display` is shortest round-trip, and its comment
names this case: *"4.0 is '4' -- whether a whole double should show a
decimal point is a language's decision, and a front end that cares builds
the string."* `$fstr` is that front end caring: an integral value within
±2^53 gets its `.0`, `-0.0` is special-cased because its integer form has
lost the sign, and everything else -- `1e+21`, `2.5e-07`,
`0.30000000000000004` -- is `to_chars`' spelling, which is culebra's too.

## A worked example: the release order

culebra releases a block's bindings in reverse declaration order. A
captured binding lives in a `Cell`, and cells are *not* in a Scope's local
range at all -- so getting the order right across the two is what the
optional release list exists for. `--dump-ir` on

```
fn captured() {
  let z = R.new('z')
  let a = R.new('a')
  let peek = || a.id
  nil
}
```

shows it:

```
scope local[1..3) +release  @2:15
  block  @2:15
    cellfresh cell[0]                  ; `a` is captured -> a cell
    ... z into local[1], a into cell[0], peek into local[2] ...
  block  @2:15                         ; the release order, spelled out
    varref local[2]                    ;   peek
    varref cell[0]                     ;   a      <- a cell, not in the range
    varref local[1]                    ;   z
```

Without the list the scope would release `local[1..3)` last-slot-first and
leave `cell[0]` to the frame -- so `a`'s destructor would run when the
function returned rather than in its place in the order. That is one line
of a golden file, and it is the reason this feature exists.

## A worked example: what makes an object a resource

`Runtime::run_drop`'s contract is that an Object whose `kDropKey` holds a
callable gets it called with the object as its one argument. Two things
follow, and both fall out of one convention rather than being arranged:

**Binding the key is the registration.** `ObjectObj::set` calls
`owned_register` when the key is `kDropKey`, so `{drop: fn () {...}}` is a
resource from the moment the literal is built -- the binder just emits that
property under the real key (`"\x01" "drop"`, unspellable from a language
whose identifiers are printable) instead of under `"drop"`.

**Every function's parameter 0 is `self`.** That is this binder's one
structural convention, and it is not an invention: culebra's own
`{iter: fn () { self }}` needs it. It means a method reaches its receiver,
an object literal's own functions do too -- and the drop protocol needs no
special case at all, because "called with the object as its one argument"
lands exactly in `self`. A class's `drop()` method is hung on the instance
as the drop key verbatim.

## Classes, without a class in the IR

There is no class tag. A class value is an object holding its methods; an
instance is an object pointing back at it; `C.new(...)` is a constructor
this binder synthesizes:

```
C = { "\x02init": <new>, "\x02m": <m>, "\x02drop": <drop>, "\x02name": "C" }
C["new"] = MakeClosure(<synthesized ctor>, over the cell holding C)

ctor(self, args...):
  inst = { "\x01class": C }
  inst["\x01drop"] = C["\x02drop"]        ; when the class declares one
  C["\x02init"](inst, args...)
  return inst
```

Two prefixes, not one, and the difference matters: `"\x01" "drop"` is the
runtime's own key, so a method table that stored its methods under the
first prefix would make the *class itself* a resource. `0x02` also keeps
methods out of `keys()` and out of a printed object, which is what culebra
shows -- a class value prints as `{new: [function]}`.

The class binding is forced into a cell because its own constructor
captures it. That is the chicken-and-egg of building the table and the
constructor in one expression, solved by building the table first and
hanging the constructor on it afterwards.

## `let x = v` evaluates to `v`

Worth its own note, because it is not cosmetic. A culebra function answers
its body's last value, and a `let` statement's value is the value bound --
so a body ending in a binding hands that binding's value *back*, and the
resource it holds therefore dies at the call site rather than at the
scope's exit:

```
fn plain() {
  let a = R.new('a')
  let b = R.new('b')
  let c = R.new('c')     # escapes as the return value
}
                         # drop b, drop a, then drop c at the call site
```

This binder emitted the `Assign` alone at first, which is a statement -- so
the function returned nil and `c` dropped one line early. `samples/drops.cul`
has both shapes, one ending in a binding and one ending in `nil`.

## The standard library is host functions

`println`, `print`, `type_of`, `.size()`, `.push()`, `.pop()`, `.keys()`
and `.map()` are `NativeDef`s the run supplies (`stdlib()`, at the bottom
of `binder.cc`). The linkage happens before the first instruction -- a name
this file forgot would fail the whole run rather than the call site.

`map` is the one that shows the other half of the contract:
`NativeCall::call` runs program code from inside C++, and a throw the
callback lets out travels through the native's frame to the caller's
handler. It walks the array by index rather than by iterator, because the
callback may push onto the very array being walked.

Note what is deliberately *not* a native. Display conversion -- `"4.0"` for
a whole Float -- is in the binder's own IR, because it is a rule of the
language rather than a service of the host; `println` is handed a string
the program already built. That is the same split mini-go draws between
vmlib's scheduler primitives and Go's channel rules.

## The custom iterator protocol, and the one piece of it left out

`for x in v` reached an array and a generator; it now also reaches any
object whose class declares `iter()` -- culebra's own extension point,
not a third hard-coded type. `$iter` asks the receiver's class table for
an `iter` method the same way `$methodof` already looks any method up,
calls it (usually answering `self`, but a class can hand back a different
object entirely -- `samples/iterators.cul`'s `CountdownView` does), and
wraps the result in a cursor kind of its own; `$iternext` drives that kind
with `has_next()`/`next()`, found and called the same way every other
method call in this front end is. Nothing about the loop itself changed --
`emit_for` still calls `$iter` once and `$iternext` per step, whichever
kind comes back.

culebra's full protocol has a fourth part, `dispose()`, called however the
loop is left. This front end does not reach it: wiring it through `Defer`
needs the disposer and the target living somewhere `Defer`'s callable can
capture from, which means a cell -- and giving the array and generator
cursors one, even nil-valued, moved *when* their own elements are
released relative to the loop body's other bindings, which is
`samples/drops.cul`, the sample this front end exists to get right. A
correct `dispose()` is possible; it did not fit inside changing the one
thing this front end is a showcase for, so it stays out rather than going
in half-verified.

## What is absent, and what quietly differs

Absent: traits, enums, multimethods, decorators, algebraic effects, pattern
matching and `match`/`cond`, destructuring, the module system, statement
modifiers (`x if c`), loop labels, format specs (`{x:>5}`), sets, regexes,
the null-safe operators (`?.`), the `_` sink, and `dispose()` from the
iterator protocol above. Bitwise operators are absent because mini-go
already proves the Fixed-width integers recipe.

Quietly different:

* **A newline is whitespace here, not a statement separator.** culebra's
  grammar threads `_sp_`/`_nl_` through every rule so that `a\n-b` is two
  statements; this one does not, so it accepts programs culebra rejects.
  The oracle is what makes that safe: a disagreement about where a
  statement ends becomes a failing diff.
* **Immutability is not enforced.** culebra rejects `p.x = 1` on a property
  the literal did not declare `mut`, and rejects assigning to a `let`
  binding (which this front end *does* check). The samples stay on the
  right side of the rule -- mutation goes through arrays and class
  instances -- because a printed object shows `mut` per property and this
  front end does not track that per property. It infers it: a class
  instance's fields are all mutable, a literal's are not.
* **Runtime traps are not culebra's errors.** A divide by zero or an
  out-of-range slice raises the executor's own trap value, which a `catch`
  lands but which has no `.kind`. The two container errors this binder
  raises itself -- `IndexError` and `KeyError` -- do carry `kind` and
  `message`, because they are the ones the samples ask about.
* **Float division by zero yields an infinity** where culebra raises
  `ZeroDivisionError`. Integer division by zero traps in both.
* **A generator prints as `<generator>`.** culebra prints its internal
  state object, which nothing could reproduce.
* **A builtin method name wins over a property.** `size`, `push`, `pop`,
  `keys` and `map` are resolved at bind time, so an object of your own must
  not put a function under one of those names.

## Testing

`ctest -R mini-culebra-samples` runs the eight samples and requires each
one's output to match a golden file captured from `culebra` -- see PL/0's
own README for why an external, independent oracle is what "passing" means
here.

`samples/gen_golden.sh` regenerates the goldens, by hand and never from the
build, for the reason `test/gen_golden.sh` states.

All eight samples are also clean under the two lanes
`.github/workflows/ci.yml` runs over the rest of the suite --
ASan/UBSan/LeakSanitizer, and `COREIR_GC_STRESS=1` (a full collection at
every single allocation), which is the one that matters most here: this is
the front end whose samples deliberately build reference cycles.
