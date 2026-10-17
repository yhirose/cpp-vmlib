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
# A real language's own toolchain not found on PATH is skipped (reported as
# "-") rather than failing the comparison: not every machine -- this
# repository's CI included -- has all eight installed.
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
# invocation, so timing it would report a compiler, not a runtime -- this
# builds each workload once with `go build` and times the binary. What is
# left in Go's `real_ms` really is a native executable's own time, which is
# why its `startup` row is the floor everything else here is measured
# against.
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
BUILD_DIR=${BUILD_DIR:-"$ROOT/build"}
REPS=3
if [ "${1:-}" = "--reps" ]; then REPS=$2; fi

# One scratch directory for the whole run -- the timed runs' output and
# the C# and Go builds below -- removed however the run ends.
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT

WORKLOADS=(startup fib loop array strings)
declare -A EXPECT=(
  [startup]=1
  [fib]=317811
  [loop]=968194995
  [array]=375082463
  [strings]=40000
)

# Runs the shell function named "$1" with the workload name "$2" REPS
# times and prints the best wall time in milliseconds, or "-" if the
# command does not exist or a run fails outright (including "not
# installed"). The last run's output is then checked against that
# workload's known answer (a warning, not a failure, on mismatch -- a
# wrong-but-fast number is worse than a slow one). The check reads a timed
# run's output rather than running the program once more just for it: the
# runs are nearly all of this script's time.
#
# The clock is EPOCHREALTIME with its decimal point (a comma in some
# locales) taken out: bash always prints six fractional digits, so that
# leaves microseconds. Reading a variable needs no GNU date for a
# sub-second format, and starts no process -- `$(date ...)` would leave
# the exit of the date reading t0 and the startup of the one reading t1
# inside the timed span. For the same reason, the functions below that
# best_ms times start no process but the program itself.
best_ms() {
  local fn=$1 workload=$2 out best="" t0 t1 dt
  for _ in $(seq 1 "$REPS"); do
    t0=${EPOCHREALTIME/[^0-9]/}
    # stdout goes to a file, not through $(...), so every rep times the
    # same command. stderr is discarded, not merged: a diagnostic on
    # stderr (guile's "source newer than compiled" note is the one that
    # actually shows up) is not part of the answer, and merging it in
    # would fail the check for a program that is behaving correctly.
    if ! "$fn" "$workload" >"$SCRATCH/out" 2>/dev/null; then
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
    echo "warning: $fn($workload) printed '$out', expected $want" >&2
  fi
  echo $(((best + 500) / 1000))
}

mini_python() { "$BUILD_DIR/examples/mini-python/mini-python" "$1.py"; }
real_python() { command -v python3 >/dev/null && python3 "$1.py"; }

mini_ruby() { "$BUILD_DIR/examples/mini-ruby/mini-ruby" "$1.rb"; }
real_ruby() { command -v ruby >/dev/null && ruby "$1.rb"; }

mini_lua() { "$BUILD_DIR/examples/mini-lua/mini-lua" "$1.lua"; }
# Looked up once, out here: a $(...) inside real_lua would be a subshell
# in every timed run.
LUA=$(command -v lua5.4 || command -v lua || true)
real_lua() { [ -n "$LUA" ] && "$LUA" "$1.lua"; }

mini_scheme() { "$BUILD_DIR/examples/mini-scheme/mini-scheme" "$1.scm"; }
real_scheme() { command -v guile >/dev/null && guile --no-auto-compile -s "$1.scm"; }

mini_culebra() { "$BUILD_DIR/examples/mini-culebra/mini-culebra" "$1.cul"; }
real_culebra() { command -v culebra >/dev/null && culebra "$1.cul"; }

mini_js() { "$BUILD_DIR/examples/mini-js/mini-js" "$1.js"; }
real_js() { command -v node >/dev/null && node "$1.js"; }

mini_csharp() { "$BUILD_DIR/examples/mini-csharp/mini-csharp" "$1.cs"; }

mini_go() { "$BUILD_DIR/examples/mini-go/mini-go" "$1.go"; }

declare -A GO_EXE

# Compiles "$1.go" once and remembers the binary in GO_EXE[$1]. As with
# build_csharp, failure (go missing, or the build failing) just leaves the
# entry unset, which real_go below reports as "not installed".
build_go() {
  local workload=$1
  command -v go >/dev/null || return 0
  local exe=$SCRATCH/go-$workload
  if go build -o "$exe" "$workload.go" >/dev/null 2>&1; then
    GO_EXE[$workload]=$exe
  fi
}

real_go() {
  local exe=${GO_EXE[$1]:-}
  [ -n "$exe" ] && [ -f "$exe" ] || return 1
  "$exe"
}

declare -A CSHARP_DLL

# Builds "$1.cs" once into a scratch project's Release output and remembers
# the compiled dll's path in CSHARP_DLL[$1]. Failure (dotnet missing, or
# the build itself failing) just leaves that workload's entry unset, which
# real_csharp below treats as "not installed".
build_csharp() {
  local workload=$1
  command -v dotnet >/dev/null || return 0
  local dir=$SCRATCH/cs-$workload
  mkdir "$dir"
  cp "$workload.cs" "$dir/Program.cs"
  cat > "$dir/s.csproj" <<'EOP'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <Nullable>disable</Nullable>
    <AssemblyName>s</AssemblyName>
    <InvariantGlobalization>true</InvariantGlobalization>
  </PropertyGroup>
</Project>
EOP
  if dotnet build "$dir/s.csproj" -c Release -o "$dir/out" >/dev/null 2>&1; then
    CSHARP_DLL[$workload]="$dir/out/s.dll"
  fi
}

real_csharp() {
  local dll=${CSHARP_DLL[$1]:-}
  [ -n "$dll" ] && [ -f "$dll" ] || return 1
  dotnet "$dll"
}

row() {
  local workload=$1 lang=$2 real_label=$3
  printf '%s\t%s\t%s\t%s\t%s\n' "$workload" "$lang" \
    "$(best_ms "mini_$lang" "$workload")" "$real_label" \
    "$(best_ms "real_$lang" "$workload")"
}

printf 'task\tlang\tmini\treal\treal_ms\n'
for w in "${WORKLOADS[@]}"; do
  build_csharp "$w"
  build_go "$w"
  row "$w" python python3
  row "$w" ruby ruby
  row "$w" lua lua
  row "$w" scheme guile
  row "$w" culebra culebra
  row "$w" js node
  row "$w" csharp dotnet
  row "$w" go go
done
