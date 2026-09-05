#!/usr/bin/env bash
# Regenerate the golden files from `python3` -- an independent
# implementation this repository does not contain, and (for bignums.py) one
# whose integers are unbounded by definition rather than by luck.
#
# Run by hand, not by the build, for the reason test/gen_golden.sh states.
#
#   samples/gen_golden.sh [python3]
set -euo pipefail

PY=${1:-python3}
cd "$(dirname "$0")"
mkdir -p golden

for f in *.py; do
  s=${f%.py}
  "$PY" "$f" >"golden/$s.txt"
  echo "golden/$s.txt"
done
