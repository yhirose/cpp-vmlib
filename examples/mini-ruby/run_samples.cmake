# Each sample's output must match the golden file, captured once from
# `ruby` -- an independent implementation this repository does not
# contain. See README.md for what this front end exists to prove: FnArity
# and ArgCount, the two intrinsics no other front end here uses, which
# Ruby makes program-visible as `Proc#arity` and as the difference between
# a proc and a lambda.

set(SAMPLES basics blocks procs collections errors closures classes control)

set(dir ${SRC}/samples)
set(failed 0)

foreach(s ${SAMPLES})
  execute_process(COMMAND ${MINI_RUBY} ${s}.rb
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
  message(FATAL_ERROR "mini-ruby sample output does not match culebra")
endif()
message(STATUS "mini-ruby samples OK (matches culebra)")
