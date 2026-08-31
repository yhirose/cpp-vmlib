# Tier 1: the three lanes must agree on stdout, stderr and exit code.
# Tier 2: that agreed-upon output must match the golden file, which came from
# culebra's own PL/0 interpreter. Tier 1 alone cannot catch a mistake all three
# lanes make together, and that is not hypothetical -- cpp-peglib's pl0.cc has
# two lanes that agree with each other about ODD and are both wrong.

set(SAMPLES square gcd fib nested unary odd read)

set(engines interp vm)
if(LLVM)
  list(APPEND engines llvm)
endif()

set(dir ${SRC}/examples/pl0/samples)
set(failed 0)

foreach(s ${SAMPLES})
  set(prog ${s}.pas)
  set(stdin_file ${dir}/${s}.stdin)
  set(ref "")
  set(ref_engine "")

  foreach(e ${engines})
    if(EXISTS ${stdin_file})
      execute_process(COMMAND ${PL0} --engine=${e} ${prog}
                      WORKING_DIRECTORY ${dir}
                      INPUT_FILE ${stdin_file}
                      OUTPUT_VARIABLE out ERROR_VARIABLE err
                      RESULT_VARIABLE rc)
    else()
      execute_process(COMMAND ${PL0} --engine=${e} ${prog}
                      WORKING_DIRECTORY ${dir}
                      OUTPUT_VARIABLE out ERROR_VARIABLE err
                      RESULT_VARIABLE rc)
    endif()
    set(got "rc=${rc}\n--stdout--\n${out}--stderr--\n${err}")

    if(ref_engine STREQUAL "")
      set(ref "${got}")
      set(ref_engine "${e}")
    elseif(NOT got STREQUAL ref)
      message(SEND_ERROR
              "${s}: ${e} differs from ${ref_engine}\n"
              "--- ${ref_engine} ---\n${ref}\n--- ${e} ---\n${got}")
      set(failed 1)
    endif()
  endforeach()

  set(golden ${dir}/golden/${s}.txt)
  if(NOT EXISTS ${golden})
    message(SEND_ERROR "${s}: no golden file at ${golden}")
    set(failed 1)
  else()
    file(READ ${golden} want)
    string(REGEX REPLACE "^rc=[0-9]+\n--stdout--\n" "" got_out "${ref}")
    string(REGEX REPLACE "--stderr--\n.*$" "" got_out "${got_out}")
    if(NOT got_out STREQUAL want)
      message(SEND_ERROR
              "${s}: output does not match the golden file\n"
              "--- want ---\n${want}--- got ---\n${got_out}")
      set(failed 1)
    endif()
  endif()
endforeach()

if(failed)
  message(FATAL_ERROR "sample lanes disagree")
endif()
message(STATUS "samples OK (${engines}; three lanes agree and match culebra)")
