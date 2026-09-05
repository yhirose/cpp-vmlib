#!/usr/bin/env bash
# Regenerate the golden files from culebra itself -- the implementation
# this front end is a slice of, and one this repository does not contain.
#
# Run by hand, not by the build: a golden file the tests could refresh
# themselves is not evidence of anything. Same rule as test/gen_golden.sh
# (whose oracle is culebra's PL/0 interpreter) and mini-js's.
#
#   samples/gen_golden.sh [culebra]
set -euo pipefail

CULEBRA=${1:-culebra}
cd "$(dirname "$0")"
mkdir -p golden

for f in *.cul; do
  s=${f%.cul}
  "$CULEBRA" "$f" >"golden/$s.txt"
  echo "golden/$s.txt"
done
