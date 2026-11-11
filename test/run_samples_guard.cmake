# run_samples.cmake's guards against a sample whose output the language
# does not fix, with a fake oracle in place of the language:
#
#   cmake -DWORK=<scratch directory> -P run_samples_guard.cmake
#
# The fake is test/oracle.sh's stand-in beside a copy of the script under
# test (which finds its oracle by its own directory): it prints the same
# thing every run, or, in a case whose name starts with `flaky`, a
# different thing each run (a counter file keeps it).

if("${WORK}" STREQUAL "")
  message(FATAL_ERROR "run_samples_guard.cmake: -DWORK is required")
endif()
get_filename_component(here ${CMAKE_CURRENT_LIST_FILE} DIRECTORY)
file(REMOVE_RECURSE ${WORK})
file(MAKE_DIRECTORY ${WORK}/bin ${WORK}/samples/golden)
file(COPY ${here}/run_samples.cmake ${here}/cases.cmake DESTINATION ${WORK}/bin)
file(WRITE ${WORK}/bin/oracle.sh [=[#!/bin/sh
if [ "$2" = --version ]; then echo fake-1; exit 0; fi
case "$2" in
  flaky*) n=$(cat "$2.count" 2>/dev/null || echo 0); n=$((n + 1))
          echo $n >"$2.count"; echo "run $n" ;;
  *) echo steady ;;
esac
]=])
file(CHMOD ${WORK}/bin/oracle.sh PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
file(WRITE ${WORK}/samples/steady.txt "")
file(WRITE ${WORK}/samples/flaky.txt "")

function(run_script out_var rc_var)
  execute_process(COMMAND ${CMAKE_COMMAND} -DORACLE=ON -DEXT=txt -DLANG=fake
                          -DDIR=${WORK}/samples -P ${WORK}/bin/run_samples.cmake
                  OUTPUT_VARIABLE o ERROR_VARIABLE e RESULT_VARIABLE rc)
  set(${out_var} "${o}${e}" PARENT_SCOPE)
  set(${rc_var} ${rc} PARENT_SCOPE)
endfunction()

# expect(<what went wrong> <if() condition...>)
function(expect what)
  if(${ARGN})
    return()
  endif()
  message(FATAL_ERROR "run_samples_guard: ${what}\n${log}")
endfunction()

# Update: the steady case is written, the flaky one is refused and gets no
# golden.
set(ENV{VMLIB_UPDATE_GOLDENS} 1)
run_script(log rc)
expect("update did not fail on a flaky sample" NOT rc EQUAL 0)
expect("update did not name the flaky sample" log MATCHES "flaky: the oracle's output differs between runs")
expect("update wrote a golden for the flaky sample" NOT EXISTS ${WORK}/samples/golden/flaky.txt)
expect("update did not write the steady golden" EXISTS ${WORK}/samples/golden/steady.txt)
unset(ENV{VMLIB_UPDATE_GOLDENS})

# Check: a golden the flaky oracle does not print is a nondeterminism
# report, not a bare diff; and the steady case is not blamed.
file(WRITE ${WORK}/samples/golden/flaky.txt "run 1\n")
run_script(log rc)
expect("check passed a mismatch" NOT rc EQUAL 0)
expect("check did not report nondeterminism" log MATCHES "flaky: nondeterministic sample")
expect("check blamed the steady sample" NOT log MATCHES "steady: ")

# A plain mismatch on a steady sample stays a plain diff, and still fails.
file(WRITE ${WORK}/samples/golden/steady.txt "wrong\n")
file(REMOVE ${WORK}/samples/flaky.txt ${WORK}/samples/golden/flaky.txt)
run_script(log rc)
expect("check passed a wrong golden" NOT rc EQUAL 0)
expect("a steady mismatch lost its plain message" log MATCHES "steady: output does not match the golden file")
message(STATUS "run_samples guards OK")
