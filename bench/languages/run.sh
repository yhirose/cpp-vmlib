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
# uneven across these seven real languages -- a bare "print a constant"
# costs lua5.4 about a millisecond and python3 about fifty, on this
# machine. Every other workload's real_ms includes that same tax, so
# reading a workload's real_ms *next to* startup's is what tells apart
# "this language is slow here" from "this language is slow to start" --
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
# loop and array fold their running total through `% 1000000007` on every
# step, not to make the arithmetic harder but to keep the *answer* small:
# without it, N large enough for the four real interpreters' work to
# outweigh their own startup cost overflows a JS `number`'s 53-bit exact
# range (JavaScript has no true integer), so mini-js and real `node` would
# legitimately -- not buggily -- disagree with the other six on the exact
# total once it stops fitting. Every element stored along the way is still
# the real, un-reduced square; only the accumulator is bounded.
#
# A real interpreter not found on PATH is skipped (reported as "-") rather
# than failing the comparison: not every machine -- this repository's CI
# included -- has all seven installed.
#
# C# is the one language here with a real compile step, and `dotnet build`
# on a scratch project costs on the order of a second on its own -- more
# than every other language's own steady-state number on every workload
# below, combined. That is not "how slow C# is", it is the price of
# restoring and compiling a project that was never installed, so it is
# paid once per workload (outside the timed loop) rather than once per
# rep, the same way this script never re-times a mini-* binary's own
# build (there is none to pay -- see bench/bench.cc's own comment on the
# same principle for the executor's Core-IR compile step).
#
# Usage: bench/languages/run.sh [--reps N]
# BUILD_DIR overrides where the mini-* binaries are looked for (default:
# the repository's own build/ directory).
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT/build"}
REPS=3
if [ "${1:-}" = "--reps" ]; then REPS=$2; fi

WORKLOADS=(startup fib loop array strings)
declare -A EXPECT=(
  [startup]=1
  [fib]=317811
  [loop]=968194995
  [array]=375082463
  [strings]=40000
)

now_ms() { date +%s%3N; }

# Runs the shell function named "$1" with the workload name "$2" once to
# check its output against that workload's known answer (a warning, not a
# failure, on mismatch -- a wrong-but-fast number is worse than a slow
# one), then REPS more times to find the best wall time in milliseconds.
# Prints "-" if the command does not exist or fails outright (including
# "not installed").
best_ms() {
  local fn=$1 workload=$2 out best="" t0 t1 dt
  # stderr is discarded here, not merged: a diagnostic on stderr (guile's
  # "source newer than compiled" note is the one that actually shows up)
  # is not part of the answer, and merging it in would fail the check for
  # a program that is behaving correctly.
  if ! out=$("$fn" "$workload" 2>/dev/null); then
    echo "-"
    return
  fi
  local want=${EXPECT[$workload]}
  if [ "$out" != "$want" ]; then
    echo "warning: $fn($workload) printed '$out', expected $want" >&2
  fi
  for _ in $(seq 1 "$REPS"); do
    t0=$(now_ms)
    "$fn" "$workload" >/dev/null 2>&1 || true
    t1=$(now_ms)
    dt=$((t1 - t0))
    if [ -z "$best" ] || [ "$dt" -lt "$best" ]; then best=$dt; fi
  done
  echo "$best"
}

mini_python() { "$BUILD_DIR/examples/mini-python/mini-python" "$1.py"; }
real_python() { command -v python3 >/dev/null && python3 "$1.py"; }

mini_ruby() { "$BUILD_DIR/examples/mini-ruby/mini-ruby" "$1.rb"; }
real_ruby() { command -v ruby >/dev/null && ruby "$1.rb"; }

mini_lua() { "$BUILD_DIR/examples/mini-lua/mini-lua" "$1.lua"; }
real_lua() {
  local l
  l=$(command -v lua5.4 || command -v lua || true)
  [ -n "$l" ] && "$l" "$1.lua"
}

mini_scheme() { "$BUILD_DIR/examples/mini-scheme/mini-scheme" "$1.scm"; }
real_scheme() { command -v guile >/dev/null && guile --no-auto-compile -s "$1.scm"; }

mini_culebra() { "$BUILD_DIR/examples/mini-culebra/mini-culebra" "$1.cul"; }
real_culebra() { command -v culebra >/dev/null && culebra "$1.cul"; }

mini_js() { "$BUILD_DIR/examples/mini-js/mini-js" "$1.js"; }
real_js() { command -v node >/dev/null && node "$1.js"; }

mini_csharp() { "$BUILD_DIR/examples/mini-csharp/mini-csharp" "$1.cs"; }

CS_TMPDIRS=()
cleanup_cs() {
  for d in "${CS_TMPDIRS[@]:-}"; do rm -rf "$d"; done
}
trap cleanup_cs EXIT
declare -A CSHARP_DLL

# Builds "$1.cs" once into a scratch project's Release output and remembers
# the compiled dll's path in CSHARP_DLL[$1]. Failure (dotnet missing, or
# the build itself failing) just leaves that workload's entry unset, which
# real_csharp below treats as "not installed".
build_csharp() {
  local workload=$1
  command -v dotnet >/dev/null || return 0
  local dir
  dir=$(mktemp -d)
  CS_TMPDIRS+=("$dir")
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
  row "$w" python python3
  row "$w" ruby ruby
  row "$w" lua lua
  row "$w" scheme guile
  row "$w" culebra culebra
  row "$w" js node
  row "$w" csharp dotnet
done
