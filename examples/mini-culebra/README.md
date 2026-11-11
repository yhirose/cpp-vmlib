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

And one recipe from the top-level README that is this front end's whole
standard library: **Host functions**. culebra's standard library is not
part of its language, and it is not part of this IR either -- `println`,
`type_of`, `.size()`, `.push()`, `.map()` are `Tag::NativeRef`, declared by
the module and supplied by the run. Other front ends here supply natives
too; this one has no library besides them, and `map` calls back into the
program.

Every sample runs unmodified under `culebra`, and each `golden/` file was
captured that way.

## Running

```
build/examples/mini-culebra/mini-culebra [--dump-ir] [--dump-bc] [--check-effects] [--profile] PROGRAM.cul
```

## What this front end did *not* have to write

The interesting comparison is with [mini-js](../mini-js/), which writes
`$truthy`, `$seq` and `$add` in IR because JavaScript disagrees
with the VM about what is true and what is comparable.
culebra mostly does not:

* **`if` takes a Bool, a Long or a Float and nothing else** -- which is
  `Value::truthy()`'s own rule for those three -- so a condition here is a
  bare `Tag::If` with no call in front of it. (`if ''` is a *type error* in
  culebra; mini-culebra does not raise it, which is the one place it is
  laxer than the language.)
* **`/` on two Longs is integer division and `%` is C's**, a Float's
  `%` fmod, which is `BinOp::Div` and `BinOp::Mod`: `binop_fault`'s "two
  ints stay integers" is culebra's rule already. They are exact (vmlib.h's
  `kExact`), with `$div` and `$mod` behind them for the one thing culebra
  does not share: a zero divisor, a Float's as well as a Long's, raises
  `ZeroDivisionError`.
* **A Long is `Value`'s Int and a Float is its Double.** There is no single
  number type to normalize into and back out of.

Two things did need writing, and they are exactly the two the header
predicts:

**`==`.** `binop_fault` refuses two values of different types -- *"a
question a language answers, not the VM"* -- where culebra answers `false`;
and culebra compares arrays and objects *structurally*, which no intrinsic
does. `$eq` is that, recursively -- called only past the values where
`Eq` is culebra's own (null, bools, numbers, strings), as the fallback of
an `Eq` (vmlib.h's `Tag::Unary`).

**Float display.** `to_display` is shortest round-trip, and its comment
names this case: *"4.0 is '4' -- whether a whole double should show a
decimal point is a language's decision."* The language's decision is in
`examples/common/double_format.h`'s `kCulebra`: the shortest digits, fixed
from 1e-4 up to 1e6 (a whole number there gets its `.0`: `100000.0`) and an
exponent outside it (`1e+06`, `1e-05`, never a `.0`). `$disp` calls the host
function `dstr` that writes it, and `describe` words an uncaught `throw 0.0001`
the same way, so print and the report agree.

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

`Runtime::run_drop`'s contract is that an Object given a callable
destructor -- `DropSet`, which keeps it beside the object's keys rather
than among them -- gets it called with the object as its one argument.
Two things follow, and both fall out of one convention rather than being
arranged:

**Giving the destructor is the registration.** `ObjectObj::set_drop`
calls `owned_register`, so `{drop: fn () {...}}` is a resource from the
moment the literal is built -- the binder gives the object its `drop`
property's value as its destructor instead of storing a property named
`"drop"`. No key reaches it: a program that writes a key, with whatever
bytes, writes a property, and makes nothing a resource.

**Every function's parameter 0 is `self`.** That is this binder's one
structural convention, and it is not an invention: culebra's own
`{iter: fn () { self }}` needs it. It means a method reaches its receiver,
an object literal's own functions do too -- and the drop protocol needs no
special case at all, because "called with the object as its one argument"
lands exactly in `self`. A class's `drop()` method is given to each
instance as its destructor verbatim.

## Classes, without a class in the IR

There is no class tag. A class value is an object whose kind record --
in its meta, the slot no key reaches (`MetaGet`/`MetaSet`) -- holds its
name, its declared `new` and its methods; an instance is an object of its
fields whose meta is its class; `C.new(...)` is a constructor this binder
synthesizes:

```
C = {}, kind record ["class", "C", nil, <new>, { m: <m>, drop: <drop> }]
C["new"] = MakeClosure(<synthesized ctor>, over the cell holding C)
C["name"] = "C"

ctor(self, args...):
  inst = {}, meta C
  dropset(inst, record(C).methods["drop"])   ; when the class declares one
  record(C).new(inst, args...)
  return inst
```

That is [examples/common/runtime_parts.h](../common/runtime_parts.h)'s
layout, which mini-ruby, mini-python and mini-csharp share, with a base
link this front end has no use for -- and with the methods in the record
rather than on the class value, because a class value is a program
object: its fields are `new` and its `name`, it prints as `{new:
[function], name: 'Point'}`, and a program can index and write it like
any object. So a method named `new` or `name` meets neither field, one
named `drop` makes no *class* a resource, and nothing a program writes on
the class -- under whatever key -- reaches a method, the constructor or
the class's name.

The name is there twice. The program's `name` field is a copy; the
record's is the class's own, where culebra keeps it, in data a program
cannot write. That one is what an instance prints and what `type_of` (a
host function, below) answers for it, so a write to `Point.name` -- an
error in culebra, unchecked here -- cannot change every `Point`'s type. A
class is the object carrying that record, which `type_of` answers
`Class`; any other object -- a literal with a `new` and a `name` of its
own included -- is an `Object`. And `==`, which compares objects field by
field, asks first that the two have one meta: an instance equals no
literal, and no other class's instance, whatever their fields.

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

An assignment is the same kind of expression, worth what it stored, and
runs in culebra's order: the value first, then the target (receiver, then
key), then for `+=` and the rest the present value read through it, then
the store. A binding or assignment whose value nothing reads -- any
statement but a block's last, and a loop body's last -- is the store
alone, so a loop pays nothing for the value.

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

`println` is handed a string the program already built. A Float's text
is the one display rule that is a native (`dstr`), since producing digits
from a number is a service of the host.

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
kind comes back. The other two kinds, and the step that answers
`{value, done}` for all three, are
[examples/common/runtime_parts.h](../common/runtime_parts.h)'s, which
mini-js, mini-python and mini-csharp drive their loops with too; this
kind is the arm culebra's `$iternext` tests ahead of that step.

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
  statements; this one does not, so it accepts programs culebra rejects,
  and reads a line that begins with `(` or `[` as a call or an index of
  the line before -- write `;` between them, which does end a statement
  here. The oracle is what makes that safe: a disagreement about where a
  statement ends becomes a failing diff.
* **Immutability is not enforced.** culebra rejects `p.x = 1` on a property
  the literal did not declare `mut`, and rejects assigning to a `let`
  binding (which this front end *does* check), and a class value's `new`
  and `name` are not writable (here a write to `name` reaches only the
  copy a program sees; see above). The samples stay on the right side of
  the rule -- mutation goes through arrays and class instances -- because
  a printed object shows `mut` per property and this front end does not
  track that per property. It infers it: a class instance's fields are
  all mutable, a literal's are not, and neither are a class value's (where
  culebra prints a field added to one as `mut`).
* **Runtime traps are worded as culebra's errors, not all of them.** The
  executor's own traps reach a `catch` converted to `{kind, message}`
  (`convert_trap`, in culebra's words: `TypeError: type error: ...`,
  `IndexError`, `ZeroDivisionError`, ...), and the errors this binder raises
  itself (`$err`) have the same shape. `e.line` and `e.col` are absent.
* **A call's arity is culebra's, for the calls its tests pin.** A
  `fn name(...)` declaration is `lenient_arity = false`, so the executor's
  own trap covers a missing and a surplus argument alike at no cost, and
  `convert_trap` words it `DispatchError: no matching method for \`name\``
  (the trap fact that says "a function, not a host function" is its `Right`).
  A lambda, a `fn` expression, a method and a constructor take a surplus
  silently, as culebra's do, and begin with a check that is one compare when
  nothing is missing: `ArityError: missing required argument 'b'`, the name
  from the declaration (`Binder::missing_arg_check`; the constructor's
  is before the instance is built, and takes the names of the class's own
  `new`). A built-in method's trap is `ArityError: 'push' takes 1 argument
  but 0 given`, the receiver not counted. What is not done: `__ARGS__`
  (where the surplus lands in culebra), a check in a generator, the callback
  arity checks (`[1].map(|a, b| a)` is a `TypeError` in culebra), and the
  wording of `println`'s, `print`'s and `type_of`'s own counts, which no
  test pins.
* **`C(args)` is `C.new(args)` only when `C` is a class declaration named
  in the call.** That is rewritten at bind time, so it costs nothing. A class
  reached any other way (`makers[0](1, 2)`, a parameter called as `f()`) is
  an object this front end cannot tell from one with a `new` field, and
  calling it is a `TypeError`: culebra constructs there too. `map(Point)`
  stays a `TypeError` and an object with a `new` field is not callable, as in
  culebra. A class with no `new` of its own cannot be constructed here at
  all (culebra's `Empty()` answers an instance).
* **A read counts a negative index from the end** (`[1, 2, 3][-1]` is `3`,
  `[-4]` is an `IndexError`), as culebra's does; a write does not, and nor
  does the read inside `xs[-1] += 1` (`$idxraw`), which culebra rejects
  like the store. The message of the `IndexError` a negative read raises is
  not pinned by culebra's tests; here it is `index out of range`. A String
  is not indexable in culebra (a `TypeError`), nor here.
* **An uncaught value is reported in this runtime's own format**
  (`file:L:C: uncaught: ...`, not culebra's `at L:C`), with culebra's words
  for the value: an error is `Kind: message`, an array or an object is
  shown as `println` shows it (`describe`'s `inspect`, which is `$insp`
  again, in C++ because the report has only the value; a Float nested in a
  container prints in the VM's shortest digits there, not culebra's), a
  string as itself. A program's own `{kind, message}` object is reported as
  an error, as it would be caught as one.
* **Not reproduced, though culebra's tests pin them.** A method that does not
  exist: culebra runs the arguments, then raises `TypeError: type error:
  expected Function, got Nil ('nope' is not a method of Array)`; here
  `$methodof` raises a `NameError` before them. A method call on `nil`:
  `TypeError: type error: expected Object, Array, or Tensor, got Nil`.
  `__ARGS__`, a surplus's home. The arity of a callback a built-in
  calls (`[1, 2].map(|a, b| a)`), and `map`'s wording for a non-function
  (`type error: parameter 'f' expects Function`). `e.line` and `e.col`.
  The `**` operator. Indexing a String. The standard library is the five
  methods and three functions above: there is no `range`, `filter`,
  `reduce` or `join`.
* **A generator prints as `<generator>`.** culebra prints its internal
  state object, which nothing could reproduce.
* **A builtin method name wins over a property.** `size`, `push`, `pop`,
  `keys` and `map` are resolved at bind time, so an object of your own must
  not put a function under one of those names.

## Testing

`ctest -R mini-culebra-samples` runs the samples and requires each
one's output to match a golden file captured from `culebra` -- see PL/0's
own README for why an external, independent oracle is what "passing" means
here.

The goldens are the output of the culebra release `test/languages.sh`
pins, and this front end is a slice of that release: culebra's output
still moves between releases, so a new pin is a new comparison, and
whatever it shows is this front end's to follow. (`iterators.cul` names
its class `Span`: culebra takes a class named `Range` for its own
built-in range.)

All the samples are also clean under the two lanes
`.github/workflows/ci.yml` runs over the rest of the suite --
ASan/UBSan/LeakSanitizer, and `COREIR_GC_STRESS=1` (a full collection at
every single allocation), which is the one that matters most here: this is
the front end whose samples deliberately build reference cycles.
