#!/usr/bin/env bash
# Five small programs -- startup (prints a constant, nothing else),
# fib(28) (recursion/calls), loop (a tight arithmetic loop, no calls),
# array (building and then indexing a container), strings (naive repeated
# concatenation) -- each run through every front end's own binary and,
# where the real language is installed, through it too. So the numbers
# say what a front end costs beyond the language it is imitating on a few
# different shapes of work, not just an absolute time nobody has a
# baseline for on one shape.
#
# `startup` is there because process startup is not free, and it is wildly
# uneven across these eight real languages -- a bare "print a constant"
# costs a compiled Go binary and lua5.4 about a millisecond and python3
# about fifty, on this machine. Every other workload's real_ms includes
# that same tax, so reading a workload's real_ms *next to* startup's is
# what tells apart "this language is slow here" from "this language is slow
# to start" --
# subtracting startup's number from the others is the honest way to
# compare steady-state speed alone. The four compute workloads are sized
# so their own work clearly outweighs that tax rather than being buried
# under it (fib and loop still spend a meaningful fraction of a fast
# interpreter's time on startup -- there is no N for naive recursion that
# fixes this without making the slowest front end here take a full minute
# to compile-and-run, so `startup`'s own row is what closes that gap).
#
# mini-scheme has no vectors and immutable pairs, so its array.scm builds
# and sums a list by tail recursion instead of by indexing -- the same
# total, reached the way this language actually reaches it, not a
# structurally different benchmark.
#
# mini-go and mini-csharp are the two front ends here with a static type
# system, so theirs are the only programs that spell out a type: `var total
# int64 = 0`, `[]int64`, `string` in Go, `long total = 0` in C#. They are
# otherwise the same programs -- the same loop, the same append-and-index,
# the same naive concatenation -- and every one of them also runs
# unmodified under `go run` or `dotnet`, the same relationship each front
# end's own samples have to it.
#
# loop and array fold their running total through `% 1000000007` on every
# step, not to make the arithmetic harder but to keep the *answer* small:
# without it, N large enough for the four real interpreters' work to
# outweigh their own startup cost overflows a JS `number`'s 53-bit exact
# range (JavaScript has no true integer), so mini-js and real `node` would
# legitimately -- not buggily -- disagree with the other seven on the exact
# total once it stops fitting. Every element stored along the way is still
# the real, un-reduced square; only the accumulator is bounded.
#
# How each real language runs, and at which version, is
# test/languages.sh's to say -- the same commands test/goldens.sh checks
# every front end's goldens with, so the program timed here is run the
# way the samples' oracle is. A language not found on PATH is skipped
# (reported as "-") rather than failing the comparison: not every machine
# has all eight installed (CI's bench job does, through
# .github/actions/languages).
#
# C# and Go are the two languages here with a real compile step, and
# `dotnet build` on a scratch project costs on the order of a second on its
# own -- more than every other language's own steady-state number on every
# workload below, combined. That is not "how slow C# is", it is the price
# of restoring and compiling a project that was never installed, so it is
# paid once per workload (outside the timed loop) rather than once per rep,
# the same way this script never re-times a mini-* binary's own build
# (there is none to pay -- see bench/bench.cc's own comment on the same
# principle for the executor's Core-IR compile step). Go gets the same
# treatment for the same reason: `go run` compiles and links on every
# invocation, so timing it would report a compiler, not a runtime -- each
# workload is built once with `go build` and the binary is what is timed.
# What is left in Go's `real_ms` really is a native executable's own time,
# which is why its `startup` row is the floor everything else here is
# measured against.
#
# Usage: bench/languages/run.sh [--reps N]
# BUILD_DIR overrides where the mini-* binaries are looked for (default:
# the repository's own build/ directory).
set -euo pipefail
# bash 5 for EPOCHREALTIME, best_ms's clock (macOS's own /bin/bash is 3.2).
if [ "${BASH_VERSINFO[0]}" -lt 5 ]; then
  echo "$0: needs bash 5 or later, found $BASH_VERSION" >&2
  exit 1
fi
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
. "$ROOT/test/languages.sh"
BUILD_DIR=${BUILD_DIR:-"$ROOT/build"}
REPS=3
if [ "${1:-}" = "--reps" ]; then REPS=$2; fi

# One scratch directory for the whole run -- the timed runs' output and
# the C# and Go builds below -- removed however the run ends.
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT

WORKLOADS=(startup fib loop array strings)
LANGS=(python ruby lua scheme culebra js csharp go)
declare -A EXPECT=(
  [startup]=1
  [fib]=317811
  [loop]=968194995
  [array]=375082463
  [strings]=40000
)

# Runs the command "$2"... REPS times and prints the best wall time in
# milliseconds, or "-" if a run fails outright (including "not
# installed"). The last run's output is then checked against workload
# "$1"'s known answer (a warning, not a failure, on mismatch -- a
# wrong-but-fast number is worse than a slow one). The check reads a timed
# run's output rather than running the program once more just for it: the
# runs are nearly all of this script's time.
#
# The clock is EPOCHREALTIME with its decimal point (a comma in some
# locales) taken out: bash always prints six fractional digits, so that
# leaves microseconds. Reading a variable needs no GNU date for a
# sub-second format, and starts no process -- `$(date ...)` would leave
# the exit of the date reading t0 and the startup of the one reading t1
# inside the timed span. For the same reason, what is timed is the
# program's own command line and nothing around it.
best_ms() {
  local workload=$1 out best="" t0 t1 dt
  shift
  for _ in $(seq 1 "$REPS"); do
    t0=${EPOCHREALTIME/[^0-9]/}
    # stdout goes to a file, not through $(...), so every rep times the
    # same command. stderr is discarded, not merged: a diagnostic on
    # stderr (guile's "source newer than compiled" note is the one that
    # actually shows up) is not part of the answer, and merging it in
    # would fail the check for a program that is behaving correctly.
    if ! "$@" >"$SCRATCH/out" 2>/dev/null; then
      echo "-"
      return
    fi
    t1=${EPOCHREALTIME/[^0-9]/}
    dt=$((t1 - t0))
    if [ -z "$best" ] || [ "$dt" -lt "$best" ]; then best=$dt; fi
  done
  out=$(<"$SCRATCH/out")
  local want=${EXPECT[$workload]}
  if [ "$out" != "$want" ]; then
    echo "warning: '$*' printed '$out', expected $want" >&2
  fi
  echo $(((best + 500) / 1000))
}

# One workload through one language: its front end's own binary, then the
# real language -- compiled first, outside the timed runs, where it has a
# compile step, and "-" where that compile fails or it is not installed.
row() {
  local workload=$1 lang=$2 file=$1.${LANG_EXT[$2]} mini_ms real_ms=-
  mini_ms=$(best_ms "$workload" "$BUILD_DIR/examples/mini-$lang/mini-$lang" "$file")
  if lang_command "$lang" "$SCRATCH/$lang-$workload" "$file" 2>/dev/null; then
    real_ms=$(best_ms "$workload" "${CMD[@]}")
  fi
  printf '%s\t%s\t%s\t%s\t%s\n' "$workload" "$lang" "$mini_ms" \
    "$(lang_name "$lang")" "$real_ms"
}

printf 'task\tlang\tmini\treal\treal_ms\n'
for w in "${WORKLOADS[@]}"; do
  for lang in "${LANGS[@]}"; do
    row "$w" "$lang"
  done
done
