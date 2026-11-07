#!/usr/bin/env bash
# Every front end's goldens checked against the real language they are the
# output of: the goldens tests (front_end_tests, test/run_samples.cmake),
# in a build directory of their own, build-goldens, configured with
# VMLIB_CHECK_GOLDENS. Nothing is compiled -- those tests run
# test/oracle.sh, not a front end -- and ctest runs the front ends side by
# side, CTEST_PARALLEL_LEVEL at a time (default: one per core).
#
#   test/goldens.sh [--update] [FRONT_END...]
#
# --update rewrites the goldens from the language instead
# (VMLIB_UPDATE_GOLDENS), by hand and never from the build. FRONT_END is a
# directory under examples/ (mini-python, pl0, ...); with none, every one.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)

if [ "${1:-}" = --update ]; then
  export VMLIB_UPDATE_GOLDENS=1
  shift
fi
BUILD=$ROOT/build-goldens
cmake -S "$ROOT" -B "$BUILD" -DVMLIB_BUILD_EXAMPLES=ON \
      -DVMLIB_CHECK_GOLDENS=ON -DVMLIB_BUILD_BENCH=OFF >/dev/null

# A front end's goldens test is its test-name prefix, as the build's
# registry lists it (front-ends.txt), and `goldens`.
tests=
for fe in "$@"; do
  fe=${fe%/}
  fe=${fe##*/}
  test=$(awk -F'\t' -v fe="$fe" '$1 == fe {print $5 "goldens"; found = 1}
                                 END {exit !found}' "$BUILD/front-ends.txt") || {
    echo "$0: no front end '$fe' in $BUILD/front-ends.txt" >&2
    exit 1
  }
  tests=${tests:+$tests|}$test
done

exec ctest --test-dir "$BUILD" -L goldens ${tests:+-R "^($tests)\$"} \
           --no-tests=error --output-on-failure \
           -j "${CTEST_PARALLEL_LEVEL:-$(getconf _NPROCESSORS_ONLN)}"
