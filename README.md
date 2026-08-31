# cpp-codegen

A closed intermediate representation and three backends that consume it.

```
        Core-IR
        ├──────────────→ interp        lane 1
        │
        └→ bytecode ──┬→ exec          lane 2
         (register)   └→ llvm (LLJIT)  lane 3
```

A front end lowers its own grammar into ten node shapes. Nothing below
`coreir` knows what parser produced them, so a backend written once serves
every front end that can reach the IR. The first front end is PL/0, under
`examples/pl0`.

## Why the shape is this shape

**The IR is closed.** `Tag` has ten members and a front end cannot add to it.
Its own rule names, child positions and vocabulary stay in its binder; what
reaches the backends is a fixed set of shapes with a fixed arity table that
`verify()` enforces. That is the difference between "an IR" and "whatever tree
the parser happened to produce".

**The LLVM lane consumes the bytecode, not the tree.** This is the load-bearing
decision. Two backends that independently interpret `if` and `while` can
disagree about them; two backends reading one instruction stream with one set
of jump targets cannot. Lowering to LLVM here is label resolution rather than a
second reading of the language.

**One runtime, three lanes.** `pl0_rt_out`, `pl0_rt_in` and `pl0_rt_fail` are C
functions in the host process, and the JIT resolves them out of that same
process. Output formatting, error wording, error position and exit code
therefore have exactly one implementation. Divergence in those is not caught by
a test here; it is unavailable.

**Variables are captures, not static links.** `VarRef` names either a slot in
this frame or a slot borrowed from an enclosing one. A static link would be the
textbook fit for PL/0 and simpler today, but it assumes the defining activation
is still on the stack, which closures break — and `VarRef` appears at every
variable access, so changing its meaning later is a breaking change rather than
a new tag.

The capture forwarding table belongs to the **call site**, not the function. A
per-function list expressed in the defining frame cannot work: `fib` captures
the root frame's variables, but `fib`'s own recursive call runs with `fib`'s
frame, and finding "the defining frame" at run time is exactly the static link
this design rejects. `--dump-ir examples/pl0/samples/fib.pas` shows the two
call sites resolving differently — `local[1] local[2]` from the root, and
`capture[0] capture[1]` forwarding through the recursion.

## Building

```
cmake -S . -B build -DLLVM_DIR=$(llvm-config --cmakedir)
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`-DPL0_ENABLE_LLVM=OFF` drops the third lane. The default is ON deliberately: a
lane that is off by default is a lane that rots.

## Running

```
build/examples/pl0/pl0 [--engine=interp|vm|llvm] [--dump-ir] [--dump-bc] [--emit-ir] PROGRAM.pas
```

## Testing

Two tiers, because one is not enough.

`ctest -R samples` runs every sample under all three lanes and requires stdout,
stderr and exit code to match — and then requires that agreed-upon output to
match a golden file taken from culebra's own PL/0 interpreter. Three lanes
agreeing does not make them right: cpp-peglib's `pl0.cc` has two lanes that
agree with each other that `ODD e` means `e != 0`, and both are wrong. Its only
`ODD` sample cannot tell, because the value it affects is overwritten before
anything prints it. `samples/odd.pas` exists to tell.

`ctest -R errors` covers the diagnostics. These are not golden-compared: the
binder reports undefined names, constant assignment and duplicate declarations
at bind time where culebra's interpreter reports them at run time, and this
runtime writes to stderr where that one writes to stdout. Both departures are
deliberate. What is checked is that the three lanes are byte-identical in
message, position and exit code, and that the message matches
`tests/errors/expected/`.

## What the reference implementations get wrong

Two PL/0 implementations informed this one, and neither could be copied whole.

| | `pl0.cul` (culebra) | `pl0.cc` (cpp-peglib) | here |
|---|---|---|---|
| `ODD e` | `e mod 2 != 0` | `e != 0` in both lanes | `e mod 2 != 0` |
| divide by zero | `divide by zero`, stdout, at the right operand | `divide by 0 error` on stderr in one lane, `divide by 0` on stdout with no position in the other | one message, one stream, at the right operand |
| uninitialized read | reported | reported by the interpreter, **not checked at all by the JIT** (`undef` load) | reported by all three |
| `?x` | reads a line | **no `in` case in the JIT** — it crashes | reads a line, all three |
| forward call to a later sibling | works | fails | works |

The last one is a traversal-order side effect in `pl0.cc` rather than a
decision about the language: it registers each procedure and descends into it
before looking at the next. The binder here collects every sibling's name
first.

## Scope

PL/0 only, and deliberately: integers, nested procedures without parameters,
seven statements. No heap, so nothing here rehearses the memory-management side
of multi-backend symmetry. What it does rehearse is diagnostics — position
propagation into compiled code, message identity, and the ordering of checks.
