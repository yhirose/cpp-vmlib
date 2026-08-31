# cpp-codegen

A closed intermediate representation and three backends that consume it.

```
        Core-IR
        ├──────────────→ interp        lane 1
        │
        └→ bytecode ──┬→ exec          lane 2
         (register)   └→ llvm (LLJIT)  lane 3
```

A front end lowers its own grammar into a fixed set of node shapes. Nothing
below `coreir` knows what parser -- or what language -- produced them, so a
backend written once serves every front end that can reach the IR. See
[Front ends](#front-ends) for what exists today.

## Why the shape is this shape

**The IR is closed.** `Tag` is a fixed enum a front end cannot add to. A front
end's own rule names, child positions and vocabulary stay in its binder; what
reaches the backends is a fixed set of shapes with a fixed arity table that
`verify()` enforces. That is the difference between "an IR" and "whatever tree
the parser happened to produce".

**The LLVM lane consumes the bytecode, not the tree.** This is the
load-bearing decision. Two backends that each interpret a language's control
flow for themselves can disagree about it; two backends reading one
instruction stream with one set of jump targets cannot. Lowering to LLVM is
label resolution, not a second reading of the language.

**One runtime, three lanes.** A front end's I/O and error reporting go through
a small set of C functions in the host process, which the JIT resolves out of
that same process. Output formatting, error wording, error position and exit
code therefore have exactly one implementation per front end. Divergence in
those is not caught by a test; it is unavailable.

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

If CMake can't find LLVM on its own, point it at the config directory
directly: `-DLLVM_DIR=/path/to/lib/cmake/llvm` (find it with
`llvm-config --cmakedir` when that binary is on `PATH`, or by locating
`LLVMConfig.cmake` under your LLVM install otherwise).

`-DPL0_ENABLE_LLVM=OFF` drops the third lane for the PL/0 front end. The
default is ON deliberately: a lane that is off by default is a lane that rots.

## Front ends

| Front end | Parser | Notes |
|---|---|---|
| [PL/0](examples/pl0/) | PEG, via cpp-peglib | Wirth's teaching language. See [examples/pl0/README.md](examples/pl0/README.md). |

Adding one means writing a binder -- your parser's tree to `coreir::Module` --
under `examples/<name>/`, plus that front end's own runtime and CLI if it
needs them. Nothing in `coreir`, `interp`, `vm` or `llvmgen` changes, or even
has to know the front end exists.

## Testing

Each front end owns its own verification, but the shape is the same
everywhere: run every sample through all three lanes and require them to
agree, then check that agreement against an oracle this repository does not
contain. Three lanes agreeing with each other is not the same as three lanes
agreeing with the language -- see a front end's own README for what its oracle
is and what it catches that cross-lane comparison alone would miss.

## Scope

The IR and its three lanes are meant to grow only by adding tags, never by
special-casing an existing one -- see each header's own design notes
(`include/coreir/ir.h`, `include/coreir/semantics.h`) for what is deliberately
fixed. What a given front end exercises is necessarily narrower than that;
see its own README for what it does and does not rehearse.
