# Each sample's output must match the golden file, captured once from
# `guile` -- an independent implementation this repository does not
# contain. See README.md for what this front end is for: the *boundary*
# around `call/cc` that the top-level README states in prose, shown here as
# a working escape continuation and a documented limit; tail calls in a
# language where iteration is nothing else; and how small a front end gets
# when the syntax stops fighting.

set(SAMPLES basics tailcalls continuations closures lists records)

set(dir ${SRC}/samples)
set(failed 0)

foreach(s ${SAMPLES})
  execute_process(COMMAND ${MINI_SCHEME} ${s}.scm
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
  message(FATAL_ERROR "mini-scheme sample output does not match culebra")
endif()
message(STATUS "mini-scheme samples OK (matches culebra)")
