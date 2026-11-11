# run_samples.cmake's guards against a sample whose output the language
# does not fix, with a fake oracle in place of the language:
#
#   cmake -DWORK=<scratch directory> -P run_samples_guard.cmake
#
# The fake stands in for test/oracle.sh beside a copy of the script under
# test (which finds its oracle by its own directory). It prints the same
# thing every run, or, for a case named flaky*, a different thing each run
# (a counter file next to the case keeps it).

if("${WORK}" STREQUAL "")
  message(FATAL_ERROR "run_samples_guard.cmake: -DWORK is required")
endif()
get_filename_component(here ${CMAKE_CURRENT_LIST_FILE} DIRECTORY)
file(REMOVE_RECURSE ${WORK})
file(MAKE_DIRECTORY ${WORK}/bin ${WORK}/samples/golden)
file(COPY ${here}/run_samples.cmake ${here}/cases.cmake
     DESTINATION ${WORK}/bin)
file(WRITE ${WORK}/bin/oracle.sh [=[#!/bin/sh
# Run from the case's directory: oracle.sh LANG FILE, or LANG --version.
if [ "$2" = --version ]; then echo fake-1; exit 0; fi
case "$2" in
  flaky*) n=$(cat "$2.count" 2>/dev/null || echo 0); n=$((n + 1))
          echo $n >"$2.count"; echo "run $n" ;;
  *) echo steady ;;
esac
]=])
file(CHMOD ${WORK}/bin/oracle.sh
     PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
file(WRITE ${WORK}/samples/steady.txt "")
file(WRITE ${WORK}/samples/flaky.txt "")

# Runs the script, setting `log` and `rc` to what it said and exited with.
macro(run_script)
  execute_process(COMMAND ${CMAKE_COMMAND} -DORACLE=ON -DEXT=txt -DLANG=fake
                          -DDIR=${WORK}/samples -P ${WORK}/bin/run_samples.cmake
                  OUTPUT_VARIABLE log ERROR_VARIABLE err RESULT_VARIABLE rc)
  string(APPEND log "${err}")
endmacro()

# expect(<what went wrong> <if() condition...>)
macro(expect what)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "run_samples_guard: ${what}\n${log}")
  endif()
endmacro()

# Update: the steady case is written, the flaky one refused.
set(ENV{VMLIB_UPDATE_GOLDENS} 1)
run_script()
expect("update did not fail" NOT rc EQUAL 0)
expect("update did not name the flaky sample"
       log MATCHES "flaky: the oracle's output differs between runs")
expect("update wrote a golden for the flaky sample"
       NOT EXISTS ${WORK}/samples/golden/flaky.txt)
expect("update did not write the steady golden"
       EXISTS ${WORK}/samples/golden/steady.txt)
unset(ENV{VMLIB_UPDATE_GOLDENS})

# Check: a mismatch on the flaky sample is reported as nondeterminism.
file(WRITE ${WORK}/samples/golden/flaky.txt "not what it prints\n")
run_script()
expect("check passed a mismatch" NOT rc EQUAL 0)
expect("check did not report nondeterminism"
       log MATCHES "flaky: nondeterministic sample")

# A mismatch on a steady sample stays a plain diff (a regression guard: the
# old script said this too). The flaky sample goes first, so that this
# report is about the steady one alone.
file(REMOVE ${WORK}/samples/flaky.txt ${WORK}/samples/golden/flaky.txt)
file(WRITE ${WORK}/samples/golden/steady.txt "wrong\n")
run_script()
expect("check passed a wrong golden" NOT rc EQUAL 0)
expect("a steady mismatch lost its plain message"
       log MATCHES "steady: output does not match the golden file")
expect("a steady mismatch was called nondeterministic"
       NOT log MATCHES "nondeterministic sample:")
message(STATUS "run_samples guards OK")
