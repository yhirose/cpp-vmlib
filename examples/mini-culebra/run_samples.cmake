# Each sample's output must match the golden file, captured once from
# `culebra` itself -- the language this front end is a slice of, and an
# implementation this repository does not contain. See README.md for what
# it exists to prove: the owned stack, a Scope's explicit release order,
# RunOptions::entry_frame_drops and the Host functions recipe, all of which
# vmlib.h describes in culebra's own terms and none of which any other
# front end here uses.

set(SAMPLES basics closures objects classes errors generators iterators drops)

set(dir ${SRC}/samples)
set(failed 0)

foreach(s ${SAMPLES})
  execute_process(COMMAND ${MINI_CULEBRA} ${s}.cul
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
  message(FATAL_ERROR "mini-culebra sample output does not match culebra")
endif()
message(STATUS "mini-culebra samples OK (matches culebra)")
