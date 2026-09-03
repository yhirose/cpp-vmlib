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
reference counting plus a cycle collector, exceptions, defers, generators --
already exists and is shared by every front end. Nothing under `coreir` knows
what parser, or what language, produced the nodes it is given.

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
| Control flow | `If`, `While`, `Block`, `Break`, `Continue`, `Return` |
| Functions | `MakeClosure`, `CallValue`, `Yield` |
| Containers | `ArrayLit`, `ObjectLit`, `Index`, `SetIndex` |
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
  `CallValue` calls whatever a value turns out to be. There is no "call
  function #n" shortcut -- see [Design notes](#design-notes).
- **Intrinsics** are what a language cannot write in itself: `Print`,
  `ReadInt`, `Len`, `ToStr`, `TypeOf`, numeric conversions, the array and
  object primitives (`ArrayPush`, `ObjectKeys`, ...), `Same`, `Collect`.
  Everything else -- map, filter, slices -- is a loop your front end writes
  in its own language.

The authoritative reference is the commentary in `vmlib.h` itself, in the
`coreir/ir.h` and `coreir/semantics.h` sections.

## The host contract

`vm/exec.cc` links against exactly six C functions and nothing else:

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

## Front ends

| Front end | Parser | Notes |
|---|---|---|
| [PL/0](example/pl0/) | PEG, via cpp-peglib | Wirth's teaching language. See [example/pl0/README.md](example/pl0/README.md). |

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
example does.

**Calls do not recurse through the host's C++ stack.** The executor keeps its
own stack of heap-allocated frames, so a deep call chain costs heap rather
than machine stack and cannot overflow the thread it runs on, and a frame's
address stays put for as long as it is live.

## Scope

The IR and the compiler/executor pair are meant to grow only by adding tags,
never by special-casing an existing one -- see the design notes in `vmlib.h`
(the `coreir/ir.h` and `coreir/semantics.h` sections) for what is
deliberately fixed. What a given front end exercises is necessarily narrower
than that; see its own README for what it does and does not rehearse.

## Testing

Each front end owns its own verification, but the shape is the same
everywhere: run every sample and check the output against an oracle this
repository does not contain. Passing your own test suite is not the same as
matching the language -- see a front end's own README for what its oracle is.
