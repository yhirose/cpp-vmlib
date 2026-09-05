# Each sample's output must match the golden file, captured once from
# `lua` -- an independent implementation this repository does not contain,
# and one whose language specification *requires* the behaviour
# samples/tailcalls.lua checks. See README.md for what this front end
# exists to prove: Tail calls and Coroutines, the two recipes no other
# front end here reaches, plus the calling convention it writes over the
# IR's fixed-arity one.

set(SAMPLES basics tables closures tailcalls coroutines metatables operators errors)

set(dir ${SRC}/samples)
set(failed 0)

foreach(s ${SAMPLES})
  execute_process(COMMAND ${MINI_LUA} ${s}.lua
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
  message(FATAL_ERROR "mini-lua sample output does not match culebra")
endif()
message(STATUS "mini-lua samples OK (matches culebra)")
