# mini-python

The sixth front end, and the one that proves **Arbitrary-precision
integers** -- the last recipe in the top-level README that nothing else
here reached. It has since grown into the one with the most of Python's
own surface: closures, classes with inheritance, tuples, comprehensions,
`global`/`nonlocal`, decorators, and the calling convention every one of
`*args`, `**kwargs`, keyword arguments and defaults needed underneath it.
See "A calling convention that is not the IR's" below for the one design
decision the rest of this front end sits on.

That section is worth quoting, because this front end is its consequence:

> `Value` holds an `int64` or a `double` and nothing wider, and this is a
> decision rather than a gap: a third numeric tag would sit in
> `eval_binop`'s hot path and tax every language's integer arithmetic for
> the few that need bignums. A front end that does (Python's `int`, Ruby's
> `Integer`, Scheme's numbers) carries them itself, as a little-endian
> `Array` of `Int` limbs in base 10^9.

Python's `int` is unbounded by definition, so `python3` is an oracle for
exactly that. `samples/bignums.py` prints `2 ** 200`, `fact(50)` and
`fib(300)` and compares them digit for digit.

It also proves **`with`** as a Scope plus a Defer, and it is the one front
end here whose *lexical* structure a PEG cannot express.

## Running

```
build/examples/mini-python/mini-python [--dump-ir] [--dump-bc] PROGRAM.py
```

## The bignum, and the two rules around it

`test/test_bigint_recipe.cc` already writes addition, schoolbook
multiplication and decimal rendering in IR and checks them against
`unsigned __int128`. What a *front end* has to add is everything around
them, and that is the interesting part of `binder.cc`:

**The representation.** A little-endian `Array` of `Int` limbs in base
10^9, wrapped in an object under a key no Python source can spell, with the
sign beside it. Base 10^9 because a limb product plus two carries stays
under 2^63, and because rendering is nine decimal digits per limb with no
division at all -- `$bstr` is a loop over `ToStr` and a zero-pad.

**The promotion rule.** `$add` and `$mul` do the arithmetic with `BinOp`
when both operands are machine integers small enough that the operation
*cannot* leave int64 -- under 2^62 for a sum, under 2^31 for a product --
and go to the limbs otherwise. That is what keeps a loop counter a loop
counter.

**The demotion rule.** `$mkbig` strips leading zero limbs and, at two limbs
or fewer, hands back an ordinary `Int` -- 10^18 fits an int64 with room to
spare. Without it every result of a big operation would stay big forever,
and `big - (big - 7)` would be a heap object holding 7. `samples/bignums.py`
asks `type(...).__name__` about exactly that.

A literal too wide for an int64 is built at bind time, by long-dividing its
decimal digits by 10^9 -- the same base the runtime works in, so the
constant arrives in the shape `$bigadd` expects.

Ordering and equality reach across the two representations, because a
program cannot see which one it has: `$cmp` compares two `Int`s directly,
two floats as floats, and anything wider by sign and then by limbs.

## A calling convention that is not the IR's

A `CallValue` fixes an argument count at the call site and a `Func` fixes
`num_params`, which is everything Python's own convention is not: `f(*xs)`
decides a count at run time, `f(b=3)` passes by name, and `def f(*rest)`
collects what was left over -- while `Func::lenient_arity` *drops* a
surplus rather than handing it anywhere. Specializing the call site does
not get around it either, because `obj.method(x)` calls whatever
`$getattr` returned, and the binder never sees which function that is.

So every Python function takes exactly two IR arguments, always -- the
positional array and the keyword object, nil when the call had no keywords
-- and a prologue turns them back into what the source declared: a
positional argument by position, then by name, then its default; `*rest`
takes what those did not; `**kwargs` takes what no parameter's name
claimed; and a call with too many positional arguments or an unclaimed
keyword raises the exact `TypeError` CPython does, by name and by count,
because the executor's own arity check knows neither. It is the same trade
examples/mini-ruby makes for a block (parameter 0 is always the block) and
examples/mini-lua makes for multiple return values (always an array): a
convention the IR does not have, written over the one it does.

Three things follow from it once and are then just true:

* **A class is called the same way its methods are.** The constructor
  takes the instance in front of whatever the call passed, unpacked by
  `$acons`, and hands it straight to `__init__` without knowing what
  `__init__` declared -- so a subclass's `__init__` can take a different
  signature from its base's, and `super().__init__(...)` reaches it.
* **A default is evaluated where the `def` stands, once.** It is not
  computed by the prologue above; it is computed into a cell of the
  *enclosing* function, the moment the `def` (or `class`, for a method) is
  reached, and the prologue reads that cell only when neither a positional
  nor a keyword argument answered. That is what makes `def f(x, into=[])`
  share one list across every call with no `into`, and what makes
  `def make(k=i)` close over `i`'s value at loop-iteration time rather
  than the loop's final value.
* **A builtin handed around as a value** -- `sorted(xs, key=len)` -- is not
  a call site the emitter can specialize inline, so `len` there becomes a
  small function of its own, built once and shared, that takes the
  convention like anything else.

`samples/functions.py` is the whole of this, checked line for line against
`python3`, including the exact wording of `TypeError`'s two messages.

## Inheritance, and the walk it is

There is no class in this IR and nothing was added for one: a class is an
object holding its methods, its base's table, and the constructor closure
that is its identity (`isinstance` and `except ThisClass` both compare
against it); an instance is an object with one hidden key pointing back at
its class. Every question a class system answers is then a walk up that
chain:

```
$clsfind(t, key):        # method lookup, used by getattr, super(), == and str()
  while typeof(t) == "object":
    if t has key: return t[key]
    if not (t has base): break
    t = t[base]
  return nil
```

`isinstance`, `isinstance(v, str)`-style checks against a builtin type
name, and `except SomeClass` are the same walk asking a different
question -- identity, a name match, or an exception's class -- of the same
chain. `super().m(...)` reaches one step further than `self`'s own class:
it starts the walk at the *base* of the class the method was declared in,
which is a capture of that class's table rather than anything computed
from the instance, so a three-level override chain resolves correctly no
matter which subclass called it.

A class that derives from a builtin exception (`class AppError(Exception)`)
gets no table to inherit from -- `Exception` is not a class here -- so it
is marked with the name it is rooted at instead, and `$isname` walks the
same chain checking that mark. `__str__`, `__repr__` and `__eq__` are
found by `$clsfind` exactly like any other method and called through the
same convention as everything else; `$str`, `$repr` and `$eq` each try one
before falling back to their builtin behavior.

`samples/inherit.py` is a three-level shape hierarchy, `super()` at every
level including a synthesized default constructor, `isinstance` walking
past two bases, `__str__`/`__repr__`/`__eq__` on plain objects, and a
user exception hierarchy that `except AppError` catches by name while
`except NotFound` does not.

## Tuples are not lists, quietly

The IR has exactly one sequence type, and Python has two that print and
compare differently -- `(1, 2) == [1, 2]` is `False`, `str((1,))` keeps its
trailing comma, `type(t).__name__` is `"tuple"`. So a tuple is its array
wrapped in an object under a key no Python source can write (the same
trick every other hidden field in this front end uses), and every sequence
func -- `$len`, `$idx`, `$iter`, `$slice`, `$eq`, `$cmp`, `$str` -- unwraps
before it looks. `a, b = f()`, `for k, v in d.items()`, and a return of
`a, b` are all one function, `$unpack`, that raises Python's own "too many"
/"not enough values to unpack" wording; `enumerate`, `zip`, `.items()` and
sorting a list of pairs all build and compare tuples through the same
funcs. `samples/tuples.py` covers all of it, including the two `ValueError`
messages and that a bare `(expr)` is not a tuple but `(expr,)` is.

## Comprehensions are a function, called immediately

Exactly as in CPython: a list, dict or generator comprehension compiles to
a function of its own, whose only unusual property is that it is called
the instant it is built. That is why its target does not leak into the
enclosing scope (Pass A gives it its own scope, the same as any `def`) and
why the generator form is lazy without anything else changing -- it is
`Func::is_generator` like any generator, its clauses are nested `while`
loops built innermost-out, and calling it just starts the coroutine rather
than running it. `samples/comprehensions.py` traces exactly when a
generator expression's body runs, including inside `sum(...)` and a `for`
loop that `break`s early.

## What Python does not share with the VM's defaults

* **Truthiness.** `0`, `""`, `[]`, `{}` and `None` are false.
  `Value::truthy()` agrees about the first and the last and calls the
  middle three true -- which its own comment gives as the reason it refuses
  to decide.
* **`//` and `%` floor**, where `BinOp::Div` truncates and `BinOp::Mod` is
  C's; `/` is *always* a float, even on two ints.
* **`==` across two types is `False`**, where `eval_binop` refuses the
  comparison.
* **Comparisons chain.** `a < b <= c` means `a < b and b <= c` with `b`
  evaluated once, which no left fold can express -- the fold would compare
  a bool with `c`. Each operand lands in a slot and each link is guarded by
  the one before it.

`Tag::Defer` runs its callable "however it exits -- falling through,
`Break`, `Continue`, `Return`, or an unwinding throw". That is a context
manager's contract word for word, so:

```
Scope {
  cellfresh c ; c = <the context manager>
  x = c.__enter__(c)
  Defer(<thunk capturing c, calling c.__exit__(c, None, None, None)>)
  <body>
}
```

The manager goes into a *cell* because a `Defer` takes a callable and a
callable reaches the enclosing frame only through cells. `samples/withstmt.py`
covers the four exits that make this worth doing rather than duplicating
the block: falling through, a `return`, a `break` out of a loop, and an
exception nothing catches.

## Python's layout is not a PEG's

Every other front end here is delimited by braces or by `end`. Python's
block structure is its indentation, and a PEG has no state to count columns
with -- so `layout.h` rewrites the source before the parser sees it: each
logical line is stripped of its leading whitespace and preceded by an
INDENT (0x01) or as many DEDENTs (0x02) as it closes, and terminated by a
NEWLINE marker (0x03). Those three bytes cannot occur in Python source,
which is what makes the rewrite unambiguous.

Two details are load-bearing. **Line numbers survive**: one `\n` is emitted
per *physical* line consumed, so a diagnostic still points where the
programmer looked even across a continuation. And **brackets and quotes are
tracked while scanning**, because a newline inside `(`, `[` or `{`
continues the logical line, and a `#` or a bracket inside a string is text.

## What Python does not share with the VM's defaults

* **Truthiness.** `0`, `""`, `[]`, `{}` and `None` are false.
  `Value::truthy()` agrees about the first and the last and calls the
  middle three true -- which its own comment gives as the reason it refuses
  to decide.
* **`//` and `%` floor**, where `BinOp::Div` truncates and `BinOp::Mod` is
  C's; `/` is *always* a float, even on two ints.
* **`==` across two types is `False`**, where `eval_binop` refuses the
  comparison.
* **Comparisons chain.** `a < b <= c` means `a < b and b <= c` with `b`
  evaluated once, which no left fold can express -- the fold would compare
  a bool with `c`. Each operand lands in a slot and each link is guarded by
  the one before it.
* **Bindings are function-scoped and closures are late.** A `lambda` made
  in a loop shares the loop's variable and sees its final value, unless it
  gives that variable to a *default* -- `def make(k=i): return k` -- which
  evaluates where the `def` stands rather than where it is called and so
  is the one place this front end's closures are early, exactly as
  Python's are. `CellFresh` otherwise runs once, at the function's entry,
  where examples/mini-js runs it per iteration for `let` -- the two
  languages want opposite things from the same tag, which is a good
  argument for it being a tag rather than a policy.

## A builtin method name loses to a user's own

`get` is a dict method and also the most ordinary method name there is, and
a Python program that defines one is not unusual -- `x.get()` on a class
instance is the first thing `samples/classes.py` would have broken on. The
other front ends here resolve such a name at bind time and say so in their
READMEs; this one asks what the receiver is:

```
$self = <receiver>
if typeof($self) == "map" then <the dict method> else <the user's method>
```

Both arms are static, so only the branch is new. It is the honest answer,
and the reason it is affordable here and not elsewhere is that the set is
small: `append`, `keys`, `get`, `upper`, `lower`, `join`.

## `yield from`, `global`/`nonlocal`, `del`, `assert`, `is`, decorators

Each of these is a small, exact rewrite once the calling convention and
inheritance above exist, so they get one paragraph between them rather
than one each.

`yield from it` is the `while`/`$iternext` loop it stands for, with what
the sub-generator *returns* dropped on the floor -- see "what is absent"
below for why. `global x` makes Pass A's name-collection put `x` in the
*module's* scope no matter how many functions stand between; `nonlocal x`
does the same for the nearest enclosing function; both change what a
plain assignment to `x` in the rest of the body means, which is why
`bind_names` gathers every `global`/`nonlocal` in a body before it decides
what any assignment declares. `del c[k]` is `$delitem`, one runtime check
away from `ObjectRemove`/`ArrayPop`; deleting a bare name is refused,
because unbinding one is not a thing this IR's locals can do. `assert`
lowers to the `If`/`Throw` any other raise does, with `AssertionError` as
the class. `is`/`is not` is `Same`, unmediated -- the one operator in this
front end that needed no helper at all. A decorator is resolved and
evaluated exactly where a default is (the enclosing scope, at `def`/`class`
time) and applied bottom-up, so `@a @b def f` is `f = a(b(f))` with no
machinery beyond `CallValue` through the same two-argument convention.
`samples/scoping.py` is all six, plus a `wrapper`/`deep` pair that shows
`global` skipping a function that shadows the name.

## A builtin method name loses to a user's own

Every string, list and dict method a sample reaches for -- `split`,
`strip`, `replace`, `find`, `count`, `startswith`, `endswith`, `join`;
`append`, `extend`, `pop`, `insert`, `remove`, `index`, `count`, `reverse`,
`sort`; `keys`, `items`, `values`, `get`, `update`, `pop` -- is written in
this front end's own IR, because every one of them is a scan and none
needs the host. `upper` and `lower` are the two exceptions, because case
folding is a Unicode table and not a scan, so they alone go to a host
function. Their trade-off is a name collision `str`/`list`/`dict` do not
have to worry about: `get` is a dict method and also the most ordinary
method name a class can give itself, and a program that writes one is not
unusual -- `x.get()` on a `Bag` instance is the first thing
`samples/strings.py` checks. The other front ends here resolve such a name
at bind time and say so in their READMEs; this one asks what the receiver
is at the call, for every name a builtin might also answer to:

```
$self = <receiver>
if typeof($self) == "map" then <the dict method> else <the user's method>
```

(`pop` and `count` try two branches, one per type each could mean.) Every
arm is static, so only the branch is new, and the set stays affordable
because a program cannot make a plain `array`, `string` or `map` answer to
a method of its own -- only a class instance can, and only the fallback
branch ever reaches one.

## f-strings, and the one thing they hand to the host

An f-string is parsed structurally rather than re-parsed later: `fexpr` and
the literal text between interpolations are alternatives of the same rule,
so `f"{n:>5}"` is one node with the expression, an optional `!r`/`!s` and
an optional format spec sitting inside it. `$fmt` applies the spec -- a
fill, an alignment, a width, and a `.Nf` precision -- as one function per
value, called even with an empty spec so a number and a string reach
`$str` the same way. Fixed-precision decimal formatting (`.2f`) is the one
piece of this that is not a scan: rendering a `double` to exactly N
decimal digits is the C library's job (`nat_ffmt`, `%.*f`), the same job
`ToStr`'s shortest-round-trip formatting is for the ordinary case, so it
is the second host function this front end needs. `samples/strings.py`
checks fill, alignment (default alignment differs by type, exactly as
Python's does), width, precision, and that an unsupported spec is a
`ValueError` naming the spec rather than a silent near miss.

## What is absent, and what quietly differs

Absent: sets, imports, `*args`/`**kwargs` mixed with a trailing keyword
default the caller can still shadow (a shape the grammar allows but the
runtime does not special-case beyond what is documented above), the
numeric tower beyond `int`/`float`/the bignum, multiple inheritance,
properties, `@staticmethod`/`@classmethod` recognized specially (a
decorated method is refused outright -- see below), string formatting
beyond `{}`'s fill/align/width/`.Nf`, and generator `send`/`throw`/`close`
or a `return` value from a generator (dropped by `yield from`, refused by
`return expr` inside a generator body being simply not distinguished from
`StopIteration`'s payload).

A decorated *method* is refused rather than silently mis-bound, because
`class_body` walks a fixed shape (`def` or `pass`) and a decorator there
would need to know whether it is a plain function, and this subset does
not have `@staticmethod`/`@classmethod` to tell it. A decorated top-level
`def` is the one shape this front end supports, and it is the shape every
`samples/scoping.py` decorator uses.

Quietly different:

* **`global` reaches past every enclosing function to the module**, even
  past one that shadows the name -- and an assignment under `global` is
  what *creates* the module binding if nothing declared one yet, which
  Pass A has to do before it walks the rest of the body: `bind_names`
  gathers every `global`/`nonlocal` first, precisely because they change
  what everything after them means.
* **`True` and `1` are different dict keys.** Python hashes them the same;
  a value-keyed `Map` does not.
* **A context manager cannot suppress an exception.** `__exit__` is called
  with three `None`s and its result is ignored, so `with` here never
  swallows what it did not raise.
* **`except` matches one name, with `Exception` as the catch-all.** There
  is no exception hierarchy, since there is no inheritance.
* **Runtime failures raise this front end's own exceptions**, not the
  executor's traps, for the cases a sample asks about -- `IndexError`,
  `KeyError`, `ZeroDivisionError`, `TypeError`. Anything else the executor
  traps on is still catchable but carries no `type(e).__name__`.
* **A method is not a value.** `p.m()` works; `f = p.m` does not (a
  builtin handed around as a value is the one exception -- see above,
  where it is a function this front end writes for exactly that purpose).
* **`sort`/`sorted` are a stable insertion sort**, not Timsort, though the
  stability a program can observe is the same.

## Testing

`ctest -R mini-python-samples` runs the fourteen samples and requires each
one's output to match a golden file captured from `python3` -- see PL/0's
own README for why an external, independent oracle is what "passing" means
here.

`samples/gen_golden.sh` regenerates the goldens, by hand and never from the
build.

All fourteen samples are clean under ASan/UBSan/LeakSanitizer and under
`COREIR_GC_STRESS=1` (a full collection at every single allocation) --
which matters here, because the bignum path allocates an array per limb
operation and is by far the most allocation-heavy code any front end in
this repository emits.
