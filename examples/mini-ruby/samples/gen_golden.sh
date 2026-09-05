#!/usr/bin/env bash
# Regenerate the golden files from `ruby` -- an independent implementation
# this repository does not contain.
#
# Run by hand, not by the build, for the reason test/gen_golden.sh states.
#
#   samples/gen_golden.sh [ruby]
set -euo pipefail

RUBY=${1:-ruby}
cd "$(dirname "$0")"
mkdir -p golden

for f in *.rb; do
  s=${f%.rb}
  "$RUBY" "$f" >"golden/$s.txt"
  echo "golden/$s.txt"
done
