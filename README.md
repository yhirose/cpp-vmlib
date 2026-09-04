# cpp-vmlib

A reusable back end for small languages: a closed intermediate
representation, a register-bytecode compiler, and the executor that runs it
-- in one C++20 header.

```
   your parser  →  coreir::Module  →  vm::compile  →  vm::run
                   (the IR)           (bytecode)      (executor)
   ─────────────   ────────────────────────────────────────────
   you write this  vmlib.h gives you this
```

You write a *binder*: the pass that turns your parse tree into the IR's
fixed node shapes. Everything below that -- register allocation, closures,
reference counting plus a cycle collector, exceptions, defers, generators,
stackful coroutines and the scheduler that turns them into green threads,
tail calls, value-keyed maps, host functions -- already exists and is shared
by every front end. Nothing under `coreir` knows what parser, or what
language, produced the nodes it is given.

## Quickstart

One header, no dependencies. Copy `vmlib.h` into your project and build the
IR for `sum = 1+2+...+5; print sum`:

```cpp
#define VMLIB_DEFAULT_RUNTIME   // take the stdio host, in one TU
#include "vmlib.h"

int main() {
  using namespace coreir;

  Module m;
  Builder b(m);
  SrcPos p{1, 1};  // every node carries a source position, for diagnostics

  // Two locals: slot 0 = i, slot 1 = sum. Slot numbering is yours to assign.
  NodeId body = b.block({
      b.assign(VarKind::Local, 0, b.literal(1, p), p),
      b.assign(VarKind::Local, 1, b.literal(0, p), p),
      b.make_while(
          b.binary(BinOp::Le, b.varref(VarKind::Local, 0, p), b.literal(5, p), p),
          b.block({
              b.assign(VarKind::Local, 1,
                       b.binary(BinOp::Add, b.varref(VarKind::Local, 1, p),
                                b.varref(VarKind::Local, 0, p), p), p),
              b.assign(VarKind::Local, 0,
                       b.binary(BinOp::Add, b.varref(VarKind::Local, 0, p),
                                b.literal(1, p), p), p),
          }, p),
          p),
      b.intrinsic(IntrinsicId::Print, {b.varref(VarKind::Local, 1, p)}, p),
  }, p);

  Func f;                          // funcs[0] is the entry point
  f.name = "main";
  f.num_locals = 2;
  f.local_names = {"i", "sum"};    // diagnostics only
  f.body = b.scope(0, 2, body, p); // a Scope owns slots [0, 2) and releases them

  m.funcs.push_back(f);

  if (auto err = verify(m)) {      // catches malformed IR before compiling
    std::fprintf(stderr, "bad IR: %s\n", err->c_str());
    return 1;
  }
  vm::run(vm::compile(m));         // prints 15
}
```

```
g++ -std=c++20 -I path/to/cpp-vmlib quick.cc -o quick
```

For a real front end -- a grammar, a binder that tracks scopes, and a CLI --
read [example/pl0/](example/pl0/).

## The IR in one page

A `Module` is flat arrays: `nodes`, `consts`, `funcs`, and the `child_ids`
that back every node's children. `Builder` is the only thing you need to
write one; `verify()` checks it.

`Tag` is a closed enum -- two dozen node shapes, and a front end cannot add
to it:

| Group | Tags |
|---|---|
| Values | `Literal`, `VarRef`, `Assign` |
| Operators | `Unary`, `Binary`, `Intrinsic` |
| Control flow | `If`, `Switch`, `While`, `Block`, `Break`, `Continue`, `Return` |
| Functions | `MakeClosure`, `CallValue`, `Yield`, `NativeRef` |
| Containers | `ArrayLit`, `ObjectLit`, `Index`, `SetIndex`, `FieldGet`, `FieldSet` |
| Lifetimes | `Scope`, `Defer`, `CellFresh` |
| Exceptions | `Throw`, `TryCatch` |

A few things worth knowing before you write a binder:

- **Every node produces a value.** `Block` yields its last child, `If` the
  branch taken, `CallValue` the callee's return value. Statements yield `Void`.
- **A variable is one of three things** (`VarKind`): a `Local` slot in the
  frame, a `Cell` the frame owns and shares with closures, or a `Capture` the
  running closure brought with it. Slot indices are yours to assign.
- **`Scope` is how you state a lifetime.** It owns a local-slot range and
  releases it on *every* exit -- falling off the end, `Break`, `Return`, or
  an unwinding throw. That is what makes a value die with its scope rather
  than with the frame.
- **Calling is two tags.** `MakeClosure` builds a callable over cells;
  `CallValue` calls whatever a value turns out to be -- a closure, or a host
  function a `NativeRef` named (see [Host functions](#host-functions)).
  There is no "call function #n" shortcut -- see
  [Design notes](#design-notes).
- **`Break` and `Continue` name their loop by depth.** `a = 0` is the
  innermost `While` (the plain form), `a = 1` the one around it: Java's and
  Go's labeled `break`/`continue`, with the label resolved to a depth by
  your binder. `verify()` requires the depth to name a loop that is open.
- **Intrinsics** are what a language cannot write in itself: `Print`,
  `ReadInt`, `Len`, `ToStr`, `TypeOf`, numeric conversions, the array,
  object and map primitives (`ArrayPush`, `ArraySlice`, `ObjectKeys`,
  `MapNew`, ...), the string primitives (`StrSlice`, `StrByte`,
  `StrFromByte`), `Same`, `Collect`, the generator and coroutine protocols
  (`GenResume`, `CoroResume`, `CoroYield`, ...) and the job queue
  (`Enqueue`). Everything else -- map, filter, a channel, a Promise -- is a
  loop or an object your front end writes in its own language.

The authoritative reference is the commentary in `vmlib.h` itself, in the
`coreir/ir.h` and `coreir/semantics.h` sections.

## Fixed-width integers

`Value` only ever holds an `int64` or a `double` -- there is no `int32`, no
`uint` of any width. A dynamically-typed front end never notices; a
statically-typed one whose `int`/`uint`/`long`/`ulong` have to wrap and
compare the way the source language promises needs a convention for landing
those into an `int64` slot, which `UnOp`'s `WrapI8`..`WrapU32` and `BinOp`'s
`UDiv`/`UMod`/`UShr`/`ULt`/`ULe`/`UGt`/`UGe` exist to make possible without a
dedicated opcode per width.

**The convention: every slot holds a normalized value.** A signed width's
value is sign-extended to `int64`; an unsigned width's is zero-extended, so
it reads as a non-negative `int64`; a `u64`'s is its own bit pattern,
verbatim, in the `int64` slot. Once operands are normalized this way,
`Lt`/`Le`/`Gt`/`Ge`/`Div`/`Mod`/`Shr` are already correct for every width up
to 32 bits without a wrapped or unsigned form -- only arithmetic that can
carry a result out of its width needs attention afterward, and `u64` needs
its own comparison, division and shift because its bit pattern does not
sit in `int64`'s own ordering. Lowering a fixed-width language means keeping
every value in this form on the way into and out of every operation --
`WrapI32(x)` after loading a param whose static type is `int`, say -- and it
is a property this repository checks against C++'s own `int32_t`/`uint32_t`/
`uint64_t` arithmetic as the oracle, in `test/test_ints.cc`.

Recipes, in terms of a source language's own operator:

```
i32 a + b / a - b / a * b   ->  WrapI32(Add(a, b))   -- likewise Sub, Mul
i32 a / b                   ->  WrapI32(Div(a, b))   -- INT32_MIN / -1 is the
                                                         one case Div does not
                                                         trap on but i32 must
                                                         wrap (Java's rule)
i32 -a                      ->  WrapI32(Neg(a))      -- -INT32_MIN is the one
                                                         case that leaves i32
u32 a + b / a - b / a * b   ->  WrapU32(Add(a, b))    -- likewise Sub, Mul
u32 -a                      ->  WrapU32(Neg(a))       -- every nonzero case
                                                          leaves u32
u32 ~a                      ->  WrapU32(BitNot(a))
u32 / u64 a / b, a % b      ->  Div(a, b), Mod(a, b) / UDiv(a, b), UMod(a, b)
u32 / u64 a < b (etc.)      ->  Lt(a, b) (etc.) / ULt(a, b) (etc.)
i32 a << b                  ->  WrapI32(Shl(a, BitAnd(b, 31)))
u32 a >> b                  ->  Shr(a, BitAnd(b, 31))
u64 a >> b                  ->  UShr(a, b)   -- the VM's own & 63 already
                                                 matches a 64-bit shift's rule
i32 -> i64                  ->  nothing; i64 -> i32  ->  WrapI32(x)
u64 constant                ->  literal(bits) -- the bit pattern as int64
```

A width narrower than 32 bits (`i8`/`u8`/`i16`/`u16`) follows the same
pattern with the matching `Wrap*`; the VM's shift-count mask (`& 63`) is
always wider than any source width's own mask, so a front end must apply its
narrower mask itself rather than relying on the VM's.

## `float`

`Value` has no third numeric tag for a 32-bit float; a language with both
`float` and `double` (C#, Java) keeps a `float`'s value in a `Double` the
same way it keeps an `i32` in an `Int` -- normalized, this time to what the
value would be after rounding through an actual `float`. The one thing a
front end cannot do in-language is produce that rounding at all (the same
reason `ToStr`'s digits are an intrinsic), so `ToFloat32` exists to do it:
`Intrinsic(ToFloat32, x) -> double`, an int widening first like `ToDouble`.
A magnitude too large to round back to a finite `float` becomes +-infinity
rather than trapping (Java's and C#'s own rule for a narrowing cast) -- one
just past `float`'s maximum rounds back down to it, the way round-to-nearest
rounds any in-range value, and only the true overflow boundary goes to
infinity -- and `NaN` passes through unchanged.

A front end re-applies `ToFloat32` after every operation whose result can
leave float precision -- `Add`/`Sub`/`Mul`/`Div`, `FMod`, `Pow`, and after
widening through `ToDouble` -- the same discipline `WrapI32` et al. use
above, once more per operation. A comparison (`Lt`, `Eq`, ...) needs no
`ToFloat32`: comparing two already-rounded doubles is exact.

## Static calls

A statically-typed language often calls something the binder already knows
by name -- a static method, a top-level function -- rather than a value that
merely turns out to be callable. The IR still has exactly one way to call
something (see [Design notes](#design-notes) for why the faster, index-based
form was removed): `MakeClosure` builds a callable, `CallValue` calls it.
PL/0's own binder builds one and calls it on the spot, which is fine for a
call site reached once; a call inside a loop, or any method called from more
than one place, should not pay `MakeClosure`'s allocation on every visit for
a target that never changes between them.

The recipe costs nothing in the library: build the closure once, at module
initialization, into a `Cell` the binder reserves for it -- the same kind of
`Cell` a captured local would use, just never reassigned after its one
store -- and lower every call site to `VarRef(Cell)` followed by
`CallValue`, instead of a fresh `MakeClosure` at each one. The difference
from PL/0's shape is exactly that one hoist; nothing about `CallValue`
itself changes.

If that recipe is not enough once profiled, the next step lives entirely in
the executor and needs no new IR: cache the `ClosureObj` a capture-map-empty
`MakeClosure` builds, keyed by func index, in a table `Exec` owns for the
run -- not `Program`, since the cached value is a refcounted heap object
bound to one `Runtime` and so cannot be precomputed once and reused across
runs the way a compiled `Chunk` can be. `Op::MakeClosure` checks the table
before allocating a new closure; no new opcode.

## Struct fields

`Index`/`SetIndex` read an `ObjectObj`'s props by comparing a key against
every prop in turn -- fine for a dynamic language's objects, whose set of
keys is a runtime fact, but the wrong cost for a struct whose fields (which
name lives at which offset) a statically-typed binder already knows before
it ever runs. `FieldGet`/`FieldSet` are the same receiver, read and written
by slot instead of by key: `props[slot]` directly, no comparison at all. A
struct is exactly an `ObjectObj` a front end has promised to index this way
-- same drop key, same owned-stack machinery, same `ObjectKeys` -- so
nothing about the value model changes, only how one kind of front end
chooses to reach it.

The promise a front end makes in exchange for the O(1) access: build every
field through one `ObjectLit` (which fills props in key order and never
leaves a gap) before any `FieldGet`/`FieldSet` reaches the object, and never
hand a struct-backed object to `ObjectRemove` -- removing a key would shift
every later field's slot out from under the front end's own numbering. A
type with a destructor keeps its drop key as one of those fields, occupying
a slot like any other (first is the simplest layout to compute by hand).
Slot numbering is the front end's own to assign, the same contract
`VarKind::Local`'s slot indices already are -- `FieldGet`/`FieldSet` carry
the field's name too, but only for a trap message ("field 'next' of ..."),
never to execute anything.

## Switch

C#'s `switch`, Java's `switch`, Go's `switch` with an expression: all three
pick one of several arms by comparing a subject against a set of constants,
and all three want better than the `if`/`else if` chain a front end without
`Switch` is forced into, which pays one comparison per arm even when the
keys are dense integers a jump table could dispatch in one step.

`Switch`'s children are `subject, key, body, key, body, ..., [default]` --
each key a `Literal` child rather than a side table, the same shape
`ObjectLit`'s key/value pairs already are. All keys share one
`ConstKind` (`Int` or `Str`) and are pairwise distinct; `verify()` rejects a
front end that gets either wrong, since neither is a runtime fact the
executor could recover from. `Switch` yields the taken arm's value, like
`If` -- and, like `If` with no `else`, a subject matching no key and with no
default yields `nil` rather than trapping. A subject whose runtime type does
not match the keys' `ConstKind` does trap, the same line `Eq` already draws
against comparing across types: a C# `switch (string)` seeing `null` is the
front end's job to catch before the switch reaches it, by testing for `nil`
first and branching to its own default.

`Break` does not stop at a `Switch`, the same way it does not stop at an
`If` -- a `Break` inside an arm still targets the enclosing `While`. That
leaves three things front ends differ on as lowering, not IR:

```
C#/Java's switch-scoped break   ->  wrap the switch in front-end state (a
                                     flag checked after each arm) or, if the
                                     switch is already loop-shaped, a
                                     synthetic one-iteration While around it
                                     so Break has something of its own to hit
Java's case fallthrough         ->  duplicate the falling-through arms'
                                     bodies rather than trying to jump
                                     between them
Go's `case a, b:` (multiple      ->  one (key, body) child pair per label,
  keys, one body)                    all pointing at the same body NodeId --
                                     safe to share since nothing about a
                                     node's identity is mutated by compiling
                                     it, just emitted again at each position
```

## Maps

`Object` is keyed by strings, and deliberately so: string keys are what let
a struct be indexed by slot (`FieldGet`/`FieldSet`) and what the drop key
hangs on. A language's `dict`/`Map`/`map[int]T`/numeric-keyed table needs
keys that are values, and gets a second container rather than a widened
first one: `ValueTag::Map`, made by `Intrinsic(MapNew)`, filled and read
through the same `Index`/`SetIndex` (which already dispatch on the
receiver), sized by `Len`, and asked about through `ObjectHas`, `ObjectKeys`
and `ObjectRemove`, each of which accepts a map receiver too. Nothing about
`Object` changes.

Two values are the same key when they have the same tag and -- for a string
-- the same bytes, or -- for everything else -- the same payload: an int by
value, a double by bit pattern (so `-0.0` and `0.0` are two keys and `NaN` is
one; a language with SameValueZero normalizes first), a heap object by
identity. That is a third rule beside `Eq` (which refuses to compare across
types) and `Same` (which compares two equal strings from two allocations as
different), stated once as `MapKeyRef` in `vmlib.h`. Lookup is a hash;
iteration is insertion order, like an object's, so a printed map is
reproducible.

## Strings and slices

`Index` on a string yields one byte, as a string. That is enough to express
everything and too slow for most of it: a substring built by concatenating
bytes is quadratic, and a byte's numeric value cannot be recovered from a
one-byte string at all. Four intrinsics fill exactly those gaps and no
more: `StrSlice(s, i, j)` and `ArraySlice(a, i, j)` take the half-open
range `[i, j)` -- bounds-checked the way `Index` is (`0 <= i <= j <= len`,
anything else traps; a language that clamps or counts from the end
normalizes first), always a fresh value -- and `StrByte(s, i)` /
`StrFromByte(n)` are what `ord`/`chr`, and any UTF-8 decoding a front end
writes itself, bottom out in. Strings stay byte sequences; what a code point
is belongs to the language.

## Host functions

The six-function contract below is what the executor itself needs from a
host. What a *program* needs from a host -- a clock, a file, a socket, a
random number -- is not the executor's to know, so it is not an intrinsic:
a module declares the host functions it calls by name (`Module::natives`,
`Builder::declare_native`), reaches one through `Tag::NativeRef` as a
callable value, and the run supplies the definitions
(`RunOptions::natives`, a list of `NativeDef{name, arity, fn, ctx}`).
`vm::run` links the two before the first instruction: a name the host did
not supply fails the whole run, not the one call site that reaches it, and
where a name is supplied twice the first definition wins.

A native's `arity` is checked the way a closure's `num_params` is -- a
mismatch traps at the call site with the same wording -- except for `-1`,
which registers it as taking any count, so the function reads
`NativeCall::argc` to find out what it got. `FnArity` answers that number,
`-1` and all, where for a closure it answers `num_params`.

A native runs on the spot, with no frame, and its answer lands where a
closure's return would; it is callable wherever a closure is -- `CallValue`,
`Enqueue`, `Defer`, the drop key -- and `TypeOf` says "function" for it.
Its signature is `bool (*)(NativeCall&)`: read the arguments, set `result`
and return `true`, or set `error` and return `false`, which raises that
value at the call site exactly as a `Throw` there would, catchable by the
program's own `TryCatch`. That is the one way a native fails a program (a
C++ exception thrown out of one is not the program's to catch: it passes
through the executor untouched, the rule the `coreir_rt_*` hooks already
have, and ends the run). `NativeCall::call` runs a closure -- or another
native -- to completion from inside a native, and a throw the callee lets
out travels through the native to the caller's handler; a native that calls
back therefore keeps what it owns in RAII handles. A front end that calls
one host function from many sites hoists the `NativeRef` into a `Cell`
exactly as [Static calls](#static-calls) hoists a `MakeClosure`.

## Tail calls

With `Func::tail_calls` on, a `CallValue` in tail position -- a `Return`'s
operand, or the value the body ends in, followed down through `Block`,
`If`, `Switch` and `Scope` -- replaces the frame instead of stacking on it,
so a loop written as a call chain runs in one frame however long it goes,
and `max_call_depth` stops mattering for it. The frame exits *before* the
callee runs: temporaries, then locals last-slot-first, then each open
scope's owned resolution. That is Rust's rule for `become`, and the one
observable difference from a plain call -- a local's destructor now prints
before the callee's output rather than after -- which is why a front end
switches it on rather than getting it by default. The compiler leaves a
call alone inside a `TryCatch` body (the handler is a pc range of this
chunk and would go with the frame) and inside a `Scope` that declares
defers; the executor leaves one alone when the callee is a native or a
generator function, or the frame is the entry frame -- each of those is
the call it would otherwise have been, and the frame returns its result.
`test/test_tailcalls.cc` runs a million-deep mutual recursion under the
default 10000-frame limit.

## Coroutines

A generator suspends the one frame whose body lexically contains the
`Yield`. A coroutine suspends every frame from the one `CoroResume` entered
down to wherever `CoroYield` is reached -- three calls deep, in a callback,
in a function that never heard of coroutines. That is what Lua's
coroutines, Ruby's Fibers and a goroutine are made of, and what an `await`
that is not itself inside a generator body needs.

`CoroCreate(f)` answers a coroutine in its Start state. The first
`CoroResume(co, v)` calls `f(v)` -- one argument, so `f` takes one
parameter or is lenient -- and runs until a `CoroYield` or `f`'s return;
each later `CoroResume` re-enters at the `CoroYield`, which yields the sent
value. Both answer the `{value, done}` object `GenResume` does. A throw the
coroutine's frames do not catch finishes it and continues at the
`CoroResume`, into the resumer's own handlers. `CoroClose(co)` finishes a
suspended coroutine early, running its parked frames' pending defers
innermost frame first (what `GenReturn` does for a generator's one frame);
a coroutine that is dropped rather than closed runs nothing, the
generator's rule. `CoroStatus` and `CoroCurrent` are the introspection.

One rule, stated once in the executor's `CoroYield`: a yield whose
coroutine's bottom frame lies below the current dispatch's floor -- there is
C++ between it and here: a native that called back in, a destructor, a
defer, the job driver -- traps with "cannot yield across a host boundary",
Lua's C-call boundary. No frame stack can hold what is on the machine
stack; everything else can be parked. The storage is the same `GenFrame`
per frame a generator uses, and the executor's frames were `unique_ptr`s
from the start so that a frame's ownership could move -- this is the change
that design note was waiting for.

## Scheduler

`Enqueue` accepts a coroutine as well as a closure, which makes the job
queue a scheduler. Taken from the queue it is resumed -- a first time as
`f(nil)`, later at its `CoroYield` with `nil` -- and driven until it yields
or finishes; a yield does not put it back, whoever it is waiting for
enqueues it again. That is every green-thread primitive: spawn is
`Enqueue(CoroCreate(f))`, block is "record `CoroCurrent()` somewhere, then
`CoroYield`", wake is `Enqueue`, and a coroutine that merely wants to let
others run enqueues itself first. A channel, a mutex, a `select` are then
objects a front end writes in its own language over those three; Go's
rendezvous rule for an unbuffered channel is forty lines of IR in
[example/go_mini/](example/go_mini/)'s binder, not a line of `vmlib.h`.

A coroutine once enqueued is the scheduler's until it finishes: the
executor holds its own reference, so one that parks and is then forgotten
by everything else is not quietly freed, and when the queue runs dry with
any of them still suspended the run fails through `coreir_rt_fail` with a
deadlock diagnostic -- Go's "all goroutines are asleep". The entry frame is
not a coroutine (a `CoroYield` there has no frames to park), so a front end
whose main *is* a green thread -- Go's -- spawns it from a bootstrap entry
function and returns; go_mini does exactly that.

## Arbitrary-precision integers

`Value` holds an `int64` or a `double` and nothing wider, and this is a
decision rather than a gap: a third numeric tag would sit in `eval_binop`'s
hot path and tax every language's integer arithmetic for the few that need
bignums. A front end that does (Python's `int`, Ruby's `Integer`, Scheme's
numbers) carries them itself, as a little-endian `Array` of `Int` limbs in
base 10^9: a limb product plus two carries then fits an `int64` with room to
spare, and rendering is nine decimal digits per limb with no division at
all. `test/test_bigint_recipe.cc` writes addition, schoolbook
multiplication and decimal rendering once in IR, the way a front end would
emit them, and checks 312 operand pairs against C++'s own `unsigned
__int128` -- the same oracle discipline `test_ints.cc` uses for the
fixed-width recipe. If a front end living on this recipe ever shows it to be
the bottleneck, that measurement is the moment to revisit the tag; until
then the hot path stays two-tagged.

## The host contract

`vm/exec.cc` links against exactly six C functions and nothing else -- the
program-level host functions above arrive through `RunOptions`, as data, so
they change nothing here:

```cpp
void coreir_rt_out(int64_t v);                       // print an integer
void coreir_rt_out_str(const char* bytes, int64_t len);   // print a string
void coreir_rt_out_raw(const char* bytes, int64_t len);   // ... no newline
int64_t coreir_rt_in(int64_t line, int64_t col);     // read an integer
[[noreturn]] void coreir_rt_fail(const char*, int64_t line, int64_t col);
void coreir_rt_poll(void);                           // on every back-edge/call
```

Define `VMLIB_DEFAULT_RUNTIME` in exactly one translation unit to take the
stdio implementation the header carries, or write your own six -- an embedder
that reports errors through its own exception mechanism instead of exiting
the process, say. Whichever is linked is the only one in the binary: output
formatting, error wording and exit behavior have exactly one implementation
per host, and divergence is not something a test has to catch because it is
unavailable.

`coreir_rt_poll()` is your interrupt point (Ctrl+C, a cancellation flag), and
`RunOptions::max_call_depth` turns a runaway call chain into a reported
failure at its call site.

## Building

With CMake, `add_subdirectory` this repository and link the `vmlib`
interface target. To build the repository itself:

```
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`VMLIB_BUILD_TESTS` (default `ON`) builds the tests, and with them the PL/0
front end their sample and error transcripts run. `VMLIB_BUILD_EXAMPLES`
(default `OFF`) builds the example front ends on their own.
`VMLIB_BUILD_BENCH` (default `ON`) builds the benchmark harness.

## Benchmarks

`bench/bench.cc` builds one Core-IR program per thing the executor spends
its time on -- calls, variable traffic, the arithmetic dispatch, string
constants, field access, allocation -- and times `vm::run` on each. It
prints one TSV row per case (name, best-of-N ms, mean ms, and a checksum of
what the program printed):

```
./build/bench/bench --reps 5          # every case
./build/bench/bench call_fib alloc    # named cases only
```

A change is judged by comparing two builds, not by one number. Build the
harness before the change, keep the binary, build it after, and:

```
bench/compare.sh path/to/bench_before ./build/bench/bench 9
```

The two binaries are run alternately, case by case, and each one's best time
is kept -- alternating because a machine whose speed drifts under load would
otherwise charge the drift to whichever side ran second. The checksum column
is what makes the comparison honest: a row marked `CHECK-DIFF` means the two
builds printed different things, so they did not do the same work.

Two cautions. Anything under about 5% between two *different binaries* is
usually the compiler's code layout rather than the change -- the dispatch
loop is one enormous function, and adding code anywhere in it moves the
numbers of cases that never execute that code. Where it matters, make the
same binary do both things (a runtime flag) and compare it against itself;
that removes layout from the question entirely.

## Front ends

| Front end | Parser | Notes |
|---|---|---|
| [PL/0](example/pl0/) | PEG, via cpp-peglib | Wirth's teaching language. See [example/pl0/README.md](example/pl0/README.md). |
| [go_mini](example/go_mini/) | PEG, via cpp-peglib | A narrow, real slice of Go -- proves Fixed-width integers, `float`, Static calls, Struct fields, Switch, and (with goroutines and unbuffered channels) Coroutines and the Scheduler against `go run`. See [example/go_mini/README.md](example/go_mini/README.md). |

Adding one means writing a binder -- your parser's tree to `coreir::Module`
-- under `example/<name>/`, plus that front end's own implementation of the
`coreir_rt_*` contract and CLI if it needs them. Nothing in `coreir` or `vm`
changes, or even has to know the front end exists.

## Design notes

Why the shape is this shape.

**The IR is closed.** `Tag` is a fixed enum, with a fixed arity table that
`verify()` enforces. A front end's own rule names, child positions and
vocabulary stay in its binder. That is the difference between "an IR" and
"whatever tree the parser happened to produce" -- and it is what lets the
compiler and executor be written once.

**The bytecode is a register machine**, matching culebra's own choice --
this library exists in part to rehearse a design culebra could grow into.

**Variables are captures, not static links.** The forwarding table that fills
a closure's captures belongs to the site building the closure, not to the
function. A static link would assume the defining activation is still on the
stack, which closures break; a per-function capture list would break on
self-recursion for the same reason. Forwarding from the build site survives
both without `VarRef` -- which appears at every variable access -- ever
having to change what it means.

**There is one way to call something.** A second, faster form used to exist,
naming a function by index and forwarding the caller's slots directly, and it
could not express a function that outlives the frame it was written in.
Keeping both would have meant two ownership rules inside one frame -- a
borrowed slot pointer and an owned cell -- with the meaning of a capture
depending on which kind of call got you there. A front end wanting the old
shape builds a closure and calls it on the spot, which is what the PL/0
example does. A host function is called by that same `CallValue` (it is a
value a `NativeRef` produced), and a tail call is that same call in tail
position -- neither added a second way.

**Calls do not recurse through the host's C++ stack.** The executor keeps its
own stack of heap-allocated frames, so a deep call chain costs heap rather
than machine stack and cannot overflow the thread it runs on, and a frame's
address stays put for as long as it is live. The frames are `unique_ptr`s
rather than a `vector<Frame>` so that a frame's ownership can move -- which
is exactly what a generator's `Yield` does with one frame and a coroutine's
`CoroYield` does with a whole slice of them. A returned frame is not freed
but recycled: it goes back to a bounded pool with its registers' capacity
kept, so an ordinary call allocates nothing, and every way a frame can die
reaches the pool through one `unique_ptr` deleter rather than through a
recycling step written at each site. The one thing that cannot be
parked is C++: a native that called back in, a destructor, a defer, the job
driver each re-enter the dispatch loop with a floor, and a yield from under
one of those traps rather than pretending.

**Code runs at a few, named moments.** A destructor runs at a refcount
death, a scope exit or a collection; a defer at its scope's exit; a
coroutine's parked defers at an explicit `CoroClose`. Dropping a suspended
coroutine or generator runs nothing, and a native fails a program only by
returning `false` -- every path on which the executor calls back into
program code is one a reader can name.

## Scope

The IR and the compiler/executor pair are meant to grow only by adding tags,
never by special-casing an existing one -- see the design notes in `vmlib.h`
(the `coreir/ir.h` and `coreir/semantics.h` sections) for what is
deliberately fixed. What a given front end exercises is necessarily narrower
than that; see its own README for what it does and does not rehearse.

**What kind of language this targets.** A dynamically-typed language (PL/0,
culebra, a JavaScript or Lua subset) is the easy case: `Value` already is
the dynamic-typed value, `Map` is its dictionary, `Generator` and
`Coroutine` are its `function*` and its `coroutine.create`, and `async`/
`await` is a coroutine driven from the job queue. A managed,
statically-typed language (a C#, Java or Go subset) is in scope too -- a
binder that has already type-checked can erase types on the way into the
IR, and the runtime's refcounted objects, cycle collector, `Scope`/`Defer`
pair, generators, coroutines and scheduler cover classes, `using`/`try`-
`finally`, `yield return`, `async` and goroutines with channels without
changes. What the binder still has to write itself, with a section above
for each: `int`/`uint`/`long`/`ulong` as **Fixed-width integers**, `float`
as a re-rounded double, a struct's slot layout for **Struct fields**, a
bignum as the **Arbitrary-precision integers** recipe, a channel or a
Promise over the **Scheduler**'s three primitives, and its standard library
as **Host functions** the run supplies. Real work either way, but none of
it is a wall: every one of those has a recipe here that a test checks
against an oracle this repository does not contain.

**What stays out of reach, and why.** *C, C++, Zig, Rust's `unsafe`*:
`Value` is a tagged value, not a byte representation, so `&x`, `*p`, pointer
arithmetic, a `union`'s type punning and a `memcpy` reinterpreting one
type's bytes as another's have nothing to lower to. Faking a
byte-addressable heap as one big int array (pointers as indices, the wasm
approach) sidesteps that, but then nothing else here is being used either,
so a front end wanting that shape is better off without this library
underneath it. *Shared-memory parallelism*: one `Runtime` is current per
thread and its refcounts are not atomic; the scheduler is N:1 on one
thread, and the way to two programs running at once is two runtimes with
values crossing only by copying (the design `Runtime` was made an object
for), not one heap with locks. *Multi-shot continuations*: a coroutine is
one-shot -- its parked frames move, they are not copied -- and Scheme's
full `call/cc` would need a rule for what a copied cell means that nothing
here has. *Speed*: there is no JIT, no inline cache, no inlining; a
language that works here is not thereby fast -- what it does cost is at
least measurable, in [Benchmarks](#benchmarks).

## Testing

Each front end owns its own verification, but the shape is the same
everywhere: run every sample and check the output against an oracle this
repository does not contain. Passing your own test suite is not the same as
matching the language -- see a front end's own README for what its oracle is.
