#!/usr/bin/env bash
# Regenerate the golden files from culebra's own PL/0 interpreter.
#
# Run by hand, not by the build: the point of a golden file is that it was
# produced by an implementation this repository does not contain, so the tests
# must not be able to refresh it themselves.
#
#   test/gen_golden.sh /path/to/culebra /path/to/culebra/examples/pl0/pl0.cul
set -euo pipefail

CULEBRA=${1:?usage: gen_golden.sh CULEBRA PL0_CUL}
PL0_CUL=${2:?usage: gen_golden.sh CULEBRA PL0_CUL}

cd "$(dirname "$0")/../examples/pl0/samples"
mkdir -p golden

for f in *.pas; do
  s=${f%.pas}
  if [ -f "$s.stdin" ]; then
    "$CULEBRA" "$PL0_CUL" "$f" <"$s.stdin" >"golden/$s.txt"
  else
    "$CULEBRA" "$PL0_CUL" "$f" >"golden/$s.txt"
  fi
  echo "golden/$s.txt"
done
