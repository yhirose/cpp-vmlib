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
through four C functions declared in `include/coreir/rt.h` and implemented by
whatever host links the executor: the standalone CLI's stdio implementation
(`runtime/coreir_rt_default.cc`), or a different host entirely -- a script
that embeds this library as a VM-building toolkit, say, reporting errors
through its own exception mechanism instead of exiting a process. Output
formatting, error wording, error position and exit behavior have exactly one
implementation per host. Divergence in those is not caught by a test; it is
unavailable.

**Variables are captures, not static links.** A variable reference names
either a slot in the current frame or a slot borrowed from an enclosing one,
and the forwarding table for a call's captures belongs to the call site, not
the callee. A static link would assume the defining activation is still on the
stack, which closures break, and a per-function capture list would break on
self-recursion for the same reason. A design built around call-site forwarding
survives both without `VarRef` -- which appears at every variable access --
ever having to change what it means.

## Building

```
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

No external dependencies beyond a C++20 compiler. `-DCPP_VMLIB_BUILD_EXAMPLES=OFF`
skips the example front end (and its own dependency on a vendored PEG parser)
for a build that only wants the `coreir`/`vm` libraries.

## Front ends

| Front end | Parser | Notes |
|---|---|---|
| [PL/0](examples/pl0/) | PEG, via cpp-peglib | Wirth's teaching language. See [examples/pl0/README.md](examples/pl0/README.md). |

Adding one means writing a binder -- your parser's tree to `coreir::Module`
-- under `examples/<name>/`, plus that front end's own implementation of the
`coreir_rt.h` contract and CLI if it needs them. Nothing in `coreir` or `vm`
changes, or even has to know the front end exists.

## Testing

Each front end owns its own verification, but the shape is the same
everywhere: run every sample and check the output against an oracle this
repository does not contain. Passing your own test suite is not the same as
matching the language -- see a front end's own README for what its oracle is.

## Scope

The IR and the compiler/executor pair are meant to grow only by adding tags,
never by special-casing an existing one -- see each header's own design notes
(`include/coreir/ir.h`, `include/coreir/semantics.h`) for what is deliberately
fixed. What a given front end exercises is necessarily narrower than that;
see its own README for what it does and does not rehearse.

Two things a host embedding this library gets for free, because the executor
calls for them at the right points rather than leaving a host to add them
after the fact: `coreir_rt_poll()` runs on every loop back-edge and call, so a
host that wants to interrupt a running program has a place to do it; a
recursion-depth counter turns a runaway procedure call into a reported
failure instead of a silent stack overflow.
