#!/usr/bin/env bash
# Regenerate the golden files from `lua` -- an independent implementation
# this repository does not contain, and (for tail calls) one whose language
# *specification* requires the behaviour being checked.
#
# Run by hand, not by the build, for the reason test/gen_golden.sh states.
# The chunk name matters: Lua's error messages name the script as it was
# given on the command line, so both sides are run from this directory with
# a bare filename.
#
#   samples/gen_golden.sh [lua]
set -euo pipefail

LUA=${1:-lua}
cd "$(dirname "$0")"
mkdir -p golden

for f in *.lua; do
  s=${f%.lua}
  "$LUA" "$f" >"golden/$s.txt"
  echo "golden/$s.txt"
done
