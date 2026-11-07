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
# names the one that would run, or refuses the same way.
set -euo pipefail
. "$(dirname "$0")/languages.sh"

lang=${1:?usage: test/oracle.sh LANG FILE... | LANG --version}
shift
if [ "${1:-}" = --version ]; then
  lang_pinned "$lang"
  exit
fi
lang_pinned "$lang" >/dev/null

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
lang_command "$lang" "$scratch" "$@"
"${CMD[@]}"
