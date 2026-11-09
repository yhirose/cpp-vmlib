#!/bin/sh
# A mutation check for test_trap_kinds: every place the executor names a
# TrapKind is changed, one at a time, to another kind, and the test must fail
# -- so no site's kind is unpinned. Not part of ctest (it compiles the test
# once per site, about a minute at -O0); run it after touching a failing
# site:
#
#   test/mutate_trap_kinds.sh [c++ compiler]
set -eu
cd "$(dirname "$0")/.."
cxx=${1:-${CXX:-c++}}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/inc"
sites=$(grep -nE '(fail\(f,|raise_trap\()[[:space:]]*(TrapKind::[A-Za-z]+|kind_of\(e\))' vmlib.h | cut -d: -f1)
bad=0
n=0
# The sites no program reaches, each named by its line's text. A name that
# matches no line fails the script, so an exemption cannot outlive its site.
#   an array of 2^31 spread arguments:
exempt='fail(f, TrapKind::Arity, {.verb = verb_of(Tag::CallSpread)});'
grep -qF -- "$exempt" vmlib.h || { echo "stale exemption: $exempt"; exit 1; }
for line in $sites; do
  if [ "$(sed -n "${line}p" vmlib.h | sed 's/^ *//')" = "$exempt" ]; then
    continue
  fi
  n=$((n + 1))
  awk -v L="$line" 'NR == L {
         if ($0 ~ /TrapKind::Internal/) sub(/TrapKind::Internal/, "TrapKind::Host");
         else if ($0 ~ /TrapKind::[A-Za-z]+/) sub(/TrapKind::[A-Za-z]+/, "TrapKind::Internal");
         else sub(/kind_of\(e\)/, "TrapKind::Internal");
       } { print }' vmlib.h > "$tmp/inc/vmlib.h"
  if ! "$cxx" -std=c++20 -O0 -I"$tmp/inc" test/test_trap_kinds.cc -o "$tmp/t" 2> "$tmp/err"; then
    echo "line $line: mutant does not compile"; sed -n 1,5p "$tmp/err"; bad=1; continue
  fi
  if "$tmp/t" > /dev/null 2>&1; then
    echo "SURVIVED: vmlib.h:$line: $(sed -n "${line}p" vmlib.h | sed 's/^ *//')"
    bad=1
  fi
done
echo "mutate_trap_kinds: $n sites, $([ $bad = 0 ] && echo 'all killed' || echo 'SURVIVORS')"
exit $bad
