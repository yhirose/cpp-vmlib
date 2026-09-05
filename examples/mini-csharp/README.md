# mini-csharp

The ninth front end, and the only *statically-typed* one -- which is the
whole of what it is here to prove. The top-level README's Scope section
says:

> "A managed, statically-typed language (a C#, Java or Go subset) is in
> scope too -- a binder that has already type-checked can erase types on
> the way into the IR, and the runtime's refcounted objects, cycle
> collector, `Scope`/`Defer` pair, generators, coroutines and scheduler
> cover classes, `using`/`try`-`finally`, `yield return`, `async` and
> goroutines with channels without changes."

examples/mini-go proves the second half of that list. This one is the
first half: **classes with inheritance and virtual dispatch**, which no
other front end here has, and **types that are parsed and then erased**,
which is the claim itself. Nothing in `coreir` or `vm` was added for it.

## Running

```
build/examples/mini-csharp/mini-csharp [--dump-ir] [--dump-bc] PROGRAM.cs
```

## A class is an object, and dispatch is a walk

There is no class machinery in this IR, so a class here is an ordinary
object built once at file scope:

```
{ "\x02name": "Circle",
  "\x02base": <the Shape table, or absent>,
  "\x02ctor": <closure>,
  "\x02Area":  <closure>,          // one entry per method
  "\x02Describe": <closure>,
  <static fields, by name> }
```

An instance is an object with one hidden key, `"\x01c"`, pointing at its
class's table; everything else in it is a field. `new Circle(2)` allocates
that object and calls the table's `"\x02ctor"` with it as argument 0.

Method lookup is then five lines of IR (`$mfind`), and it is worth reading
because *this is all virtual dispatch is*:

```
t = recv["\x01c"]
while typeof(t) == "object":
  if t has "\x02" + name: return t["\x02" + name]
  if not (t has "\x02base"): break
  t = t["\x02base"]
```

Because the walk starts at the *receiver's* table and not at the class
that contains the call site, a base-class method calling `Describe()`
reaches the derived class's override. `samples/shapes.cs` is two levels of
that, plus `: base(...)` chaining and a default constructor that chains
because the binder synthesized one. Nothing in the IR knows any of it
happened.

The one convention that carries it is the same one examples/mini-culebra
uses: **every method's parameter 0 is `this`**, and an implicit-`this`
call is written as an explicit one before it is emitted. That is why
`Speak()` inside a method and `x.Speak()` outside it compile to the same
three nodes.

## Types are parsed, and then thrown away

`type` exists in the grammar so that the samples are real C# -- they
compile under `dotnet` unmodified, which is what makes `dotnet` an oracle
-- and for no other reason. The binder keeps exactly two facts out of a
declaration, and both are about *what calling it builds*, not about what
it returns:

* `static` decides where the method is bound (on the class table, reached
  without a receiver) rather than what it means.
* `async`, and the presence of a `yield` in the body, decide whether the
  method compiles to one func or three.

Everything else -- `int` vs `long` vs `double`, `List<Shape>` vs
`IEnumerable<int>`, the base list, the return type -- is discarded. A
downcast `(Shape)x` is erased along with it, because nothing downstream
reads a type and there is nothing for the cast to do.

The two exceptions are not annotations but operations: `(int)` is `ToInt`
and `(double)` is `ToDouble`, because C# performs those, and erasing them
would answer `3.9` where C# answers `3`.

## What C# does not ask the VM to decide

This is the interesting half, and it is short. Every dynamic front end
here writes helper funcs for things `vmlib.h` explicitly refuses to pick:
`truthy`'s comment names JavaScript and Lua disagreeing as its reason, and
`to_display`'s leaves whole-float formatting to the front end. C# needs
neither:

| The VM's rule | C# | So this front end |
|---|---|---|
| `Value::truthy()` calls `0` and `""` false, NaN true | only `bool` is a condition | passes the condition through |
| `to_display` prints `4.0` as `4` | `Console.WriteLine(4.0)` prints `4` | prints what `ToStr` gives |
| `BinOp::Div` and `Mod` truncate toward zero, C-style | `/` and `%` on two `int`s do the same, including `-7 / 2 == -3` | emits the `BinOp` |
| two ints stay int, a double widens the pair | the same | emits the `BinOp` |

`samples/basics.cs` is that table, run against `dotnet`.

Two helpers survive, and both are there for a reason the table explains
rather than contradicts:

* `$add`, because `"count: " + 3` concatenates -- the *operands* need
  converting, not the arithmetic.
* `$eq`, because `eval_binop` refuses a cross-type comparison ("a
  question a language answers, not the VM") and `x == null` is exactly
  that question. C#'s answer is: numbers compare numerically, different
  types are unequal, `null == null`, and two references are equal when
  they are the same object -- which is `Same`.

## `using`, `yield return`, `async`: three tags, verbatim

Each of these is one recipe from the top-level README, reached from C#'s
direction:

**`using (var r = ...)`** is a `Scope` with a `Defer` whose callable is a
synthesized thunk that calls `Dispose()` on a cell holding the resource.
Because `Defer` runs on *every* exit, the same three nodes cover falling
off the end, `return`, `break` and a `throw` passing through -- which is
what `samples/resources.cs` checks, one path at a time, against `dotnet`.
`try`/`finally` is the same shape with the body's `finally` block as the
thunk.

**`yield return`** is `Func::is_generator` plus `Tag::Yield`, and `yield
break` is a `Return`. `foreach` drives it through `GenResume`, whose
`{value, done}` result *is* `IEnumerator`'s `MoveNext`/`Current` pair
turned inside out. `samples/iterators.cs` checks that the laziness is
real: a `Trace` list records that the producer runs one element at a time,
interleaved with the consumer, and stops early when the loop `break`s.

**`async`/`await`** is three funcs -- the method the source declared,
which builds a Task, spawns a coroutine and returns; the body; and the
entry that settles the Task -- which is *the same lowering
examples/mini-js writes for a Promise*. Worth noticing: the
statically-typed language and the dynamic one wanted exactly the same
thing from `CoroCreate`, `CoroYield` and `Enqueue`, and neither needed the
other's.

`Main` itself is wrapped in a coroutine and enqueued, because `.Wait()`
and `.Result` have to be able to suspend the caller, and only a coroutine
can. That is the one place this front end departs from C#'s own model,
where `.Wait()` blocks a thread. The observable behaviour is the same for
these samples, and `samples/tasks.cs` pins the ordering that matters --
including the rule that an `async` body runs synchronously up to its
first `await`, which is a one-line diff away from being wrong and was, in
examples/mini-js, until `node` said so.

## A property is two methods with the names C# itself gives them

`public int Value { get; set; }` and `public double Celsius { get { ... }
set { ... } }` both compile to a `get_Value`/`set_Value` (or
`get_Celsius`/`set_Celsius`) pair stored on the class table exactly where
an ordinary method is -- which is not an analogy, it is what `csc` itself
names them. `obj.Value` and `obj.Value = v` become a `$mfind` for
`get_Value`/`set_Value`, the same walk `emit_method` uses for
`obj.Speak()`, so a `virtual` property's `override` costs nothing beyond
declaring the derived accessor: `samples/properties.cs`'s `Shape.Label`
and `Square.Label` are that, with no code of their own for the dispatch.
`Value` read or written bare inside the declaring class's own method is
the same deal implicit `this` already is for a field -- pass A marks the
identifier, pass B routes it through the same two helpers.

An **auto property** (`{ get; set; }`, both required -- one alone is a
bind-time error, matching `get`/`set` bodies being required not to mix
with `get;`/`set;` on the same property) gets no user-written accessor at
all: its `get_Name`/`set_Name` are built by hand at class-table time, over
a hidden field keyed the way a method already is (`\x02` for a method,
`\x03` for this), the same trick emit_default_ctor uses for a missing
constructor. Its `= 0` initializer follows the exact rule a plain field's
already does: it runs in the *constructor*, so a class with no
constructor of its own gets `nil`, not the literal -- `Counter` in
`samples/properties.cs` declares an empty one for exactly this reason.

## What is absent, and what quietly differs

Absent: interfaces, generics of its own (`List<T>` and `Task<T>` are
recognized names, not type parameters), `out`/`ref`, `switch`, LINQ,
delegates and lambdas, `struct` value semantics (examples/mini-go proves
that recipe), a `static` property, operator overloading, `partial`,
attributes, namespaces, and the `checked` arithmetic of `int` overflow --
mini-go proves the fixed-width recipe, and an `int` here is the VM's
int64.

Quietly different:

* **`catch` has no type filter that matters**: the type is parsed and
  erased like every other, so every `catch` catches everything.
* **`Exception` is a plain object** with a message; there is no hierarchy
  and no stack trace.
* **`Dispose` is found by name**, not through `IDisposable`, because
  there are no interfaces.
* **A `List<T>` is an array**, so `.Count` is `Len` and `.Add` is
  `ArrayPush`; `Insert`, `Remove`, `Sort` and the rest are not here.
* **`string.Join` is the only `string` static**, and `.Length`, `.Count`,
  `.Message` and `.Result` are four built-in properties a user-declared
  one cannot shadow -- everything else after a `.` that names neither a
  method nor a declared property is a field.

## Where it fought back

One bug, five times: **a name the emitter reads has to have been
registered by resolution first**, and C# has more shapes that mention a
name without looking like a variable reference than any other language
here.

| The shape | What it reaches | How it failed |
|---|---|---|
| `Speak()` inside a method | `this`, then the class chain | read as an unknown global |
| `new Circle(2)` | the `Circle` table | `capture_index.at` threw |
| `: base(n)` | the base class's table | the same |
| `Klass.Static(x)` | the static's own binding | the same |
| `Log.Add(s)`, `Log` a static field | the field on the class table | routed to the library path, as if `Log` were `Console` |

Each is three lines in pass A and one condition in pass B, and none of
them is visible in the source as anything but an identifier. The lesson is
the one examples/mini-python's dangling-reference bug taught in a
different key: a two-pass binder is only correct if pass A sees *exactly*
what pass B will emit.

A sixth is grammatical and worth recording on its own: `new` was a
`unary`, so `new Plain().Describe()` had nowhere to hang the suffix. It is
a `primary`. Its two shapes are spelled out rather than made optional,
because `new List<int> { 1, 2 }` has no argument list at all.

## Testing

`ctest -R mini-csharp-samples` runs the six samples and requires each
one's output to match a golden file captured from `dotnet` -- see PL/0's
own README for why an external, independent oracle is what "passing"
means here.

`samples/gen_golden.sh` regenerates the goldens, by hand and never from
the build. It compiles each sample in a scratch project of its own
(`samples/run_one.sh`), because the SDK builds every `.cs` in a directory
and two samples in one would both declare `Main`.

All six samples are clean under ASan/UBSan/LeakSanitizer and under
`COREIR_GC_STRESS=1`.
