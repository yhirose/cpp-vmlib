#!/usr/bin/env bash
# Regenerate the golden files from Node -- an independent implementation
# this repository does not contain.
#
# Run by hand, not by the build: the point of a golden file is that
# something else produced it, so the tests must not be able to refresh it
# themselves. Same rule as test/gen_golden.sh, whose oracle is culebra's
# PL/0 interpreter.
#
# Each sample is prepended with prelude.js, because that is how the front
# end runs it too (`mini-js prelude.js sample.js`) -- both sides build
# their output strings with the same JavaScript, so what the comparison
# tests is the language rather than console.log's rendering of a container.
#
#   samples/gen_golden.sh [node]
set -euo pipefail

NODE=${1:-node}
cd "$(dirname "$0")"
mkdir -p golden
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

for f in *.js; do
  [ "$f" = prelude.js ] && continue
  s=${f%.js}
  cat prelude.js "$f" >"$tmp/$s.js"
  "$NODE" "$tmp/$s.js" >"golden/$s.txt"
  echo "golden/$s.txt"
done
