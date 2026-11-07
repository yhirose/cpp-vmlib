#!/usr/bin/env bash
# A real language, run the way a front end's binary is: on the program
# FILE... from this directory, with this stdin, printing what the program
# prints and exiting as it exits. A front end's goldens test runs its
# samples through this in its place (front_end_tests, run_samples.cmake),
# which is what checks the goldens against the language they are the
# output of.
#
#   test/oracle.sh LANG FILE...
#   test/oracle.sh LANG --version
#
# LANG is a key of test/languages.sh, which says how the language runs and
# at which version. Any other version is refused (lang_pinned); --version
# names the one that would run, or refuses the same way. run_samples.cmake
# asks that once per test, and says it did in VMLIB_ORACLE_CHECKED, which
# spares each case the same question; run by hand, a case asks it itself.
set -euo pipefail
. "$(dirname "$0")/languages.sh"

lang=${1:?usage: test/oracle.sh LANG FILE... | LANG --version}
shift
if [ "${1:-}" = --version ]; then
  lang_pinned "$lang"
  exit
fi
if [ "${VMLIB_ORACLE_CHECKED:-}" != "$lang" ]; then
  lang_pinned "$lang" >/dev/null
fi

trap 'rm -rf "${LANG_SCRATCH:-}"' EXIT
lang_command "$lang" "" "$@"
"${CMD[@]}"
