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
tests=
for fe in "$@"; do
  fe=${fe%/}
  fe=${fe##*/}
  # PL/0's tests are the main suite's, unprefixed (test/CMakeLists.txt).
  if [ "$fe" = pl0 ]; then fe=goldens; else fe=$fe-goldens; fi
  tests=${tests:+$tests|}$fe
done

cmake -S "$ROOT" -B "$ROOT/build-goldens" -DVMLIB_BUILD_EXAMPLES=ON \
      -DVMLIB_CHECK_GOLDENS=ON -DVMLIB_BUILD_BENCH=OFF >/dev/null
exec ctest --test-dir "$ROOT/build-goldens" -L goldens ${tests:+-R "^($tests)\$"} \
           --no-tests=error --output-on-failure \
           -j "${CTEST_PARALLEL_LEVEL:-$(getconf _NPROCESSORS_ONLN)}"
