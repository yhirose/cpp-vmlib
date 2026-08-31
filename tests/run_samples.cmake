# Each sample's output must match the golden file, taken from culebra's own
# PL/0 interpreter -- an independent implementation in a different language,
# not just a second run of this one. Self-consistency alone would not have
# caught cpp-peglib's own ODD bug (pl0.cc's interpreter and JIT agree with
# each other and are both wrong about it).

set(SAMPLES square gcd fib nested unary odd read)

set(dir ${SRC}/examples/pl0/samples)
set(failed 0)

foreach(s ${SAMPLES})
  set(prog ${s}.pas)
  set(stdin_file ${dir}/${s}.stdin)

  if(EXISTS ${stdin_file})
    execute_process(COMMAND ${PL0} ${prog}
                    WORKING_DIRECTORY ${dir}
                    INPUT_FILE ${stdin_file}
                    OUTPUT_VARIABLE out ERROR_VARIABLE err
                    RESULT_VARIABLE rc)
  else()
    execute_process(COMMAND ${PL0} ${prog}
                    WORKING_DIRECTORY ${dir}
                    OUTPUT_VARIABLE out ERROR_VARIABLE err
                    RESULT_VARIABLE rc)
  endif()

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
  message(FATAL_ERROR "sample output does not match culebra's PL/0 interpreter")
endif()
message(STATUS "samples OK (matches culebra's pl0.cul)")
