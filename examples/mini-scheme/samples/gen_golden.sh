#!/usr/bin/env bash
# Regenerate the golden files from `guile` -- an independent
# implementation this repository does not contain.
#
# `--no-auto-compile` matters: without it Guile writes progress notes to
# stderr the first time it sees a file, and a golden captured with them is
# a golden of the cache's state rather than of the program.
#
# Run by hand, not by the build, for the reason test/gen_golden.sh states.
#
#   samples/gen_golden.sh [guile]
set -euo pipefail

GUILE=${1:-guile}
cd "$(dirname "$0")"
mkdir -p golden

for f in *.scm; do
  s=${f%.scm}
  "$GUILE" --no-auto-compile -s "$f" >"golden/$s.txt"
  echo "golden/$s.txt"
done
