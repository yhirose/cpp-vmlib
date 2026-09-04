#!/bin/sh
# Two bench binaries, side by side: build one from the tree before a change and
# one from the tree after it, then
#
#   bench/compare.sh path/to/bench_before path/to/bench_after [reps]
#
# The two binaries are run *alternately*, case by case, and each one's best
# time is kept. Alternating is what makes the comparison survive a machine
# whose speed drifts under load: a slow minute hits both sides, where running
# all of one and then all of the other would charge it to whichever went
# second. Best-of-N rather than the mean for the same reason -- noise only
# ever adds time, so the minimum is the closest thing to a clean run.
#
# A row marked CHECK-DIFF is not a comparison: the two builds printed
# different things, so they did different work.
set -eu

if [ $# -lt 2 ]; then
  echo "usage: $0 <bench-before> <bench-after> [reps]" >&2
  exit 2
fi

before=$1
after=$2
reps=${3:-5}

cases=$("$before" --help | sed -n 's/^  //p')

# One core, if the tools are here to ask for one: a run that migrates between
# cores mid-measurement is the other half of the noise.
run() {
  if command -v taskset > /dev/null 2>&1; then
    taskset -c 0 "$@"
  else
    "$@"
  fi
}

printf '%-14s %10s %10s %9s  %s\n' case before after change note
for c in $cases; do
  # Each sample is one process running the case twice and reporting its own
  # best: the second run is warm, where a one-shot process pays first-touch
  # page faults inside the timed region.
  samples=""
  i=0
  while [ "$i" -lt "$reps" ]; do
    samples="$samples$(run "$before" --reps 2 "$c" | tail -1)
$(run "$after" --reps 2 "$c" | tail -1)
"
    i=$((i + 1))
  done
  printf '%s' "$samples" | awk -F'\t' -v c="$c" '
    { ms = $2; check = $4
      side = (NR % 2) ? "b" : "a"
      if (side == "b") { if (bn++ == 0 || ms < b) b = ms; bc = check }
      else             { if (an++ == 0 || ms < a) a = ms; ac = check }
    }
    END {
      pct = (b > 0) ? (a - b) / b * 100 : 0
      printf "%-14s %9.1fms %9.1fms %+8.1f%%  %s\n", c, b, a, pct,
             (bc == ac) ? "" : "CHECK-DIFF"
    }'
done
