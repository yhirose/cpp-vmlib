# Each sample's output must match the golden file, captured once from
# `python3` -- an independent implementation this repository does not
# contain, and one whose integers are unbounded by definition. See
# README.md for what this front end exists to prove: **Arbitrary-precision
# integers**, the last recipe in the top-level README that no other front
# end reaches, and `with` as a Scope plus a Defer.

set(SAMPLES basics bignums containers closures generators classes errors
            withstmt functions inherit tuples comprehensions strings scoping)

set(dir ${SRC}/samples)
set(failed 0)

foreach(s ${SAMPLES})
  execute_process(COMMAND ${MINI_PYTHON} ${s}.py
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
  message(FATAL_ERROR "mini-python sample output does not match culebra")
endif()
message(STATUS "mini-python samples OK (matches culebra)")
