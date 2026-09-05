# Each sample's output must match the golden file, captured once from
# Node -- an independent implementation, not just a second run of this
# one. See README.md for what this front end exists to prove: vmlib's
# Closures, Exceptions, Generators, Maps and Coroutines/Scheduler recipes
# hold against JavaScript itself, not only against this repository's own
# tests.
#
# Every sample is run with samples/prelude.js ahead of it, which is how
# the golden was captured too (samples/gen_golden.sh concatenates the same
# two files for Node). The prelude is the formatting the samples print
# through, written in the source language so that both sides build their
# output with the same code -- see its own header comment for why that is
# the honest comparison.

set(SAMPLES basics closures objects errors generators mapset async classes)

set(dir ${SRC}/samples)
set(failed 0)

foreach(s ${SAMPLES})
  execute_process(COMMAND ${MINI_JS} prelude.js ${s}.js
                  WORKING_DIRECTORY ${dir}
                  OUTPUT_VARIABLE out ERROR_VARIABLE err
                  RESULT_VARIABLE rc)

  if(NOT rc EQUAL 0)
    message(SEND_ERROR "${s}: exited ${rc}\n--stdout--\n${out}--stderr--\n${err}")
    set(failed 1)
    continue()
  endif()

  set(golden ${dir}/golden/${s}.txt)
  if(NOT EXISTS ${golden})
    message(SEND_ERROR "${s}: no golden file at ${golden}")
    set(failed 1)
  else()
    file(READ ${golden} want)
    if(NOT out STREQUAL want)
      message(SEND_ERROR
              "${s}: output does not match the golden file\n"
              "--- want ---\n${want}--- got ---\n${out}")
      set(failed 1)
    endif()
  endif()
endforeach()

if(failed)
  message(FATAL_ERROR "mini-js sample output does not match node")
endif()
message(STATUS "mini-js samples OK (matches node)")
