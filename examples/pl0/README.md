# PL/0

The first front end for cpp-vmlib's Core-IR. PL/0 is Wirth's teaching
language: integers only, nested procedures without parameters, seven
statements -- small enough that the whole chain (grammar, binder, compiler,
executor) fits together in one place, which is why it was chosen to validate
the IR before anything larger is pointed at it.

The grammar in `grammar.h` is carried over verbatim from culebra's
`examples/pl0/pl0.cul`, including its `no_ast_opt` annotations -- the binder
calls `parser.optimize_ast()`, and those annotations only make sense together
with that call. `binder.cc` is the only file in this repository that includes
`peglib.h`; nothing in `vmlib.h` knows PL/0 or peglib exist. `main.cc` is
the one translation unit that defines `VMLIB_DEFAULT_RUNTIME`, taking the
stdio host the header carries.

## Running

```
build/examples/pl0/pl0 [--dump-ir] [--dump-bc] PROGRAM.pas
```

## A worked example: captures across a call

A PL/0 procedure call is a closure built over the caller's cells and called
immediately. `--dump-ir examples/pl0/samples/fib.pas` shows the same function
built from two places, resolving its captures differently each time:

```
makeclosure fib #1 cmap=0  @25:5   ; from main
makeclosure fib #1 cmap=1  @11:5   ; the recursive one, inside fib itself

cmap 0: cell[0] cell[1]            ; main -> fib
cmap 1: capture[0] capture[1]      ; fib -> fib (self-recursion)
```

`main` hands over two of its own cells; the recursive site inside `fib` hands
over the cells `fib` itself was given, unchanged. A per-function capture list
(expressed in the frame where `fib` is defined) cannot express this -- the
recursive call runs with `fib`'s own frame, not the frame that declared it, so
"the defining frame" isn't something the callee can name there. The forwarding
table has to live where the closure is built, not on the function.

`x` and `r` are cells rather than locals in `main` because `fib` captures
them; PL/0's other variables stay plain locals. Which ones need promoting is
the binder's own analysis, and the IR refuses a closure over a local -- it
would die with the frame.

## Testing

`ctest -R samples` runs `samples/*.pas` and requires the output to match a
golden file (`samples/golden/`) taken from culebra's own PL/0 interpreter
(`examples/pl0/pl0.cul` in the culebra repository) -- an independent
implementation in a different language. Passing your own tests is not the
same as matching the language: see "What the reference implementations get
wrong" below for a case where two lanes of a different independent
implementation agree with each other and are both wrong, which a
self-consistency check alone would never catch.

`ctest -R errors` covers the diagnostics (`test/errors/`). These are not
golden-compared: the binder reports undefined names, constant assignment and
duplicate declarations at bind time where culebra's interpreter reports them
at run time, and this runtime writes to stderr where that one writes to
stdout. Both departures are deliberate. What is checked is that message,
position and exit code match `test/errors/expected/`.

`samples/nested.pas`, `unary.pas`, `odd.pas` and `read.pas` exist because the
three samples inherited from culebra's own example never nest a procedure
inside another, never use a leading sign, and never read input -- so the parts
of the design that exist specifically for those shapes (capture chains two
frames deep, sign folding, `?x`) would otherwise ship untested.
`test/errors/recursion.pas` exists because none of the samples recurse
without terminating, so the executor's recursion-depth guard would otherwise
ship untested too.

## What the reference implementations get wrong

Two existing PL/0 implementations informed this one, and neither could be
copied whole: culebra's own `examples/pl0/pl0.cul`, and cpp-peglib's
`pl0/pl0.cc` (a from-scratch interpreter plus LLVM JIT written in C++, by the
same author).

| | `pl0.cul` (culebra) | `pl0.cc` (cpp-peglib) | here |
|---|---|---|---|
| `ODD e` | `e mod 2 != 0` | `e != 0` in both its interpreter and its JIT | `e mod 2 != 0` |
| divide by zero | `divide by zero`, stdout, at the right operand | `divide by 0 error` on stderr from the interpreter, `divide by 0` on stdout with no position from the JIT | one message, one stream, at the right operand |
| uninitialized read | reported | reported by the interpreter, **not checked at all by the JIT** (`undef` load) | reported |
| `?x` | reads a line | **no `in` case in the JIT** -- it crashes | reads a line |
| forward call to a later sibling | works | fails | works |
| runaway recursion | — | stack overflow, uncatchable | reported as `recursion limit exceeded` |

`pl0.cc`'s own `ODD` sample (`gcd.pas`) cannot detect its bug: the value it
affects is overwritten before anything prints it. `samples/odd.pas` exists to
detect it. The forward-reference failure is a traversal-order side effect in
`pl0.cc` rather than a decision about the language -- it registers each
procedure and descends into it before looking at the next sibling; the binder
here collects every sibling's name first.

## Scope

Integers, nested procedures without parameters, no heap -- so nothing here
rehearses the memory-management side of embedding this library elsewhere.
What it does rehearse is diagnostics: position propagation into compiled
code, message identity, and the ordering of checks.
