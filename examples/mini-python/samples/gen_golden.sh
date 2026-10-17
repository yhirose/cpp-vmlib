#!/usr/bin/env bash
# Regenerate the golden files from `python3` -- an independent
# implementation this repository does not contain, and (for bignums.py) one
# whose integers are unbounded by definition rather than by luck.
#
# Run by hand, not by the build, for the reason test/gen_golden.sh states.
#
# From Python 3.14 only. CPython rewords its messages between releases --
# 3.14 made every division by zero "division by zero" and gave "too many
# values to unpack" its count -- and mini-python's wording is 3.14's, so a
# golden captured from another version is a diff in wording, not a bug.
#
#   samples/gen_golden.sh [python3]
set -euo pipefail

PY=${1:-python3}
want=3.14
have=$("$PY" -c 'import sys; print("%d.%d" % sys.version_info[:2])')
if [ "$have" != "$want" ]; then
  echo "gen_golden.sh: the goldens are Python $want's output, and $PY is" \
       "$have; pass a $want interpreter (e.g. python$want)" >&2
  exit 1
fi
cd "$(dirname "$0")"
mkdir -p golden

for f in *.py; do
  s=${f%.py}
  "$PY" "$f" >"golden/$s.txt"
  echo "golden/$s.txt"
done
