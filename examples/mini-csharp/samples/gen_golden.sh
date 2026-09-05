#!/usr/bin/env bash
# Regenerate the golden files from `dotnet` -- an independent
# implementation this repository does not contain.
#
# Run by hand, not by the build, for the reason test/gen_golden.sh states.
# Each sample is compiled in a scratch project of its own, because the SDK
# builds every .cs in a directory and two samples in one would both
# declare Main.
#
#   samples/gen_golden.sh
set -euo pipefail

cd "$(dirname "$0")"
mkdir -p golden

for f in *.cs; do
  s=${f%.cs}
  ./run_one.sh "$f" >"golden/$s.txt"
  echo "golden/$s.txt"
done
