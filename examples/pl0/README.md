# PL/0

The first front end for cpp-codegen's Core-IR. PL/0 is Wirth's teaching
language: integers only, nested procedures without parameters, seven
statements -- small enough that the whole chain (grammar, binder, three
backends) fits together in one place, which is why it was chosen to validate
the IR before anything larger is pointed at it.

The grammar in `grammar.h` is carried over verbatim from culebra's
`examples/pl0/pl0.cul`, including its `no_ast_opt` annotations -- the binder
calls `parser.optimize_ast()`, and those annotations only make sense together
with that call. `binder.cc` is the only file in this repository that includes
`peglib.h`; nothing under `include/coreir/`, `interp/`, `vm/` or `llvmgen/`
knows PL/0 or peglib exist.

## Running

```
build/examples/pl0/pl0 [--engine=interp|vm|llvm] [--dump-ir] [--dump-bc] [--emit-ir] PROGRAM.pas
```

## A worked example: captures across a call

`--dump-ir examples/pl0/samples/fib.pas` shows the same function's two call
sites resolving captures differently:

```
func #1 fib  locals=3 captures=2 [x r]
  call fib #1 cmap=1  @11:5    ; the recursive call, inside fib itself
  call fib #1 cmap=2  @15:5

cmap 0: local[1] local[2]      ; main -> fib
cmap 1: capture[0] capture[1]  ; fib -> fib (self-recursion)
```

The call from `main` forwards its own locals; the recursive call inside `fib`
forwards its own captures through unchanged. A per-function capture list
(expressed in the frame where `fib` is defined) cannot express this -- the
recursive call runs with `fib`'s own frame, not the frame that declared it, so
"the defining frame" isn't something the callee can name at that call site.
The forwarding table has to live on the call, not the function.

## Testing

Two tiers, because one is not enough.

`ctest -R samples` runs `samples/*.pas` under all three lanes and requires
stdout, stderr and exit code to match -- and then requires that agreed-upon
output to match a golden file (`samples/golden/`) taken from culebra's own
PL/0 interpreter (`examples/pl0/pl0.cul` in the culebra repository). Three
lanes agreeing does not make them right: see "What the reference
implementations get wrong" below for a case where two independent lanes agree
with each other and are both wrong.

`ctest -R errors` covers the diagnostics (`tests/errors/`). These are not
golden-compared: the binder reports undefined names, constant assignment and
duplicate declarations at bind time where culebra's interpreter reports them
at run time, and this runtime writes to stderr where that one writes to
stdout. Both departures are deliberate. What is checked is that the three
lanes are byte-identical in message, position and exit code, against
`tests/errors/expected/`.

`samples/nested.pas`, `unary.pas`, `odd.pas` and `read.pas` exist because the
three samples inherited from culebra's own example never nest a procedure
inside another, never use a leading sign, and never read input -- so the parts
of the design that exist specifically for those shapes (capture chains two
frames deep, sign folding, `?x`) would otherwise ship untested.

## What the reference implementations get wrong

Two existing PL/0 implementations informed this one, and neither could be
copied whole: culebra's own `examples/pl0/pl0.cul`, and cpp-peglib's
`pl0/pl0.cc` (a from-scratch interpreter plus LLVM JIT written in C++, by the
same author).

| | `pl0.cul` (culebra) | `pl0.cc` (cpp-peglib) | here |
|---|---|---|---|
| `ODD e` | `e mod 2 != 0` | `e != 0` in both lanes | `e mod 2 != 0` |
| divide by zero | `divide by zero`, stdout, at the right operand | `divide by 0 error` on stderr in one lane, `divide by 0` on stdout with no position in the other | one message, one stream, at the right operand |
| uninitialized read | reported | reported by the interpreter, **not checked at all by the JIT** (`undef` load) | reported by all three |
| `?x` | reads a line | **no `in` case in the JIT** -- it crashes | reads a line, all three |
| forward call to a later sibling | works | fails | works |

`pl0.cc`'s own `ODD` sample (`gcd.pas`) cannot detect its bug: the value it
affects is overwritten before anything prints it. `samples/odd.pas` exists to
detect it. The forward-reference failure is a traversal-order side effect in
`pl0.cc` rather than a decision about the language -- it registers each
procedure and descends into it before looking at the next sibling; the binder
here collects every sibling's name first.

## Scope

Integers, nested procedures without parameters, no heap -- so nothing here
rehearses the memory-management side of multi-backend symmetry. What it does
rehearse is diagnostics: position propagation into compiled code, message
identity across lanes, and the ordering of checks.
