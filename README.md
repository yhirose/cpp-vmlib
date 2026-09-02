# cpp-vmlib

A closed intermediate representation, a register-bytecode compiler, and the
executor that runs it.

```
        Core-IR → bytecode (register) → exec
```

A front end lowers its own grammar into a fixed set of node shapes. Nothing
below `coreir` knows what parser -- or what language -- produced them, so the
compiler and executor, written once, serve every front end that can reach the
IR. See [Front ends](#front-ends) for what exists today.

## Why the shape is this shape

**The IR is closed.** `Tag` is a fixed enum a front end cannot add to. A front
end's own rule names, child positions and vocabulary stay in its binder; what
reaches `vm::compile` is a fixed set of shapes with a fixed arity table that
`verify()` enforces. That is the difference between "an IR" and "whatever tree
the parser happened to produce".

**The bytecode is a register machine**, matching culebra's own choice --
this library exists in part to rehearse a design culebra could grow into.

**One host runtime, one contract.** A front end's I/O and error reporting go
through six C functions declared in the `coreir/rt.h` section of `vmlib.h`
and implemented by whatever host links the executor: the stdio implementation
the header itself carries (see [Using](#using)), or a different host entirely
-- a script that embeds this library as a VM-building toolkit, say, reporting
errors through its own exception mechanism instead of exiting a process. Output
formatting, error wording, error position and exit behavior have exactly one
implementation per host. Divergence in those is not caught by a test; it is
unavailable.

**Variables are captures, not static links.** A variable reference names a
slot in the current frame, a cell the frame owns, or a cell the closure being
run brought with it -- and the forwarding table that fills the last of those
belongs to the site building the closure, not to the function. A static link
would assume the defining activation is still on the stack, which closures
break, and a per-function capture list would break on self-recursion for the
same reason. Forwarding from the build site survives both without `VarRef` --
which appears at every variable access -- ever having to change what it means.

**There is one way to call something.** `MakeClosure` builds a callable value
over cells; `CallValue` calls a value. A second, faster form used to exist,
naming a function by index and forwarding the caller's slots directly, and it
could not express a function that outlives the frame it was written in.
Keeping both would have meant two ownership rules inside one frame -- a
borrowed slot pointer and an owned cell -- with the meaning of a capture
depending on which kind of call got you there. A front end wanting the old
shape builds a closure and calls it on the spot, which is what the PL/0
example does.

## Using

The library is one header. Copy `vmlib.h` into your project, or add this
directory to the include path:

```cpp
#include "vmlib.h"
```

Every translation unit that includes it gets the `coreir` and `vm`
namespaces; a program also needs one definition of the six `coreir_rt_*`
functions. A host supplies its own, or takes the stdio implementation the
header carries by defining `VMLIB_DEFAULT_RUNTIME` in exactly one
translation unit before the include:

```cpp
#define VMLIB_DEFAULT_RUNTIME
#include "vmlib.h"
```

With CMake, `add_subdirectory` this repository and link the `vmlib`
interface target. No external dependencies beyond a C++20 compiler.

## Building

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

## Testing

Each front end owns its own verification, but the shape is the same
everywhere: run every sample and check the output against an oracle this
repository does not contain. Passing your own test suite is not the same as
matching the language -- see a front end's own README for what its oracle is.

## Scope

The IR and the compiler/executor pair are meant to grow only by adding tags,
never by special-casing an existing one -- see the design notes in `vmlib.h`
(the `coreir/ir.h` and `coreir/semantics.h` sections) for what is deliberately
fixed. What a given front end exercises is necessarily narrower than that;
see its own README for what it does and does not rehearse.

Two things a host embedding this library gets for free, because the executor
calls for them at the right points rather than leaving a host to add them
after the fact: `coreir_rt_poll()` runs on every loop back-edge and call, so a
host that wants to interrupt a running program has a place to do it; a bound
on how many frames may be live turns a runaway procedure call into a reported
failure at its call site.

Calls do not recurse through the host's C++ stack. The executor keeps its own
stack of heap-allocated frames, so a deep call chain costs heap rather than
machine stack and cannot overflow the thread it runs on, and a frame's address
stays put for as long as it is live.
