# Error cases are not golden-compared against culebra: the binder reports
# undefined names, constant assignment and duplicate declarations at bind time
# where pl0.cul reports them at run time, and this runtime writes to stderr
# where pl0.cul writes to stdout. Both are deliberate. What is checked instead
# is that the three lanes are byte-identical -- message, position and exit
# code -- and that the message is the one recorded in expected/.

set(CASES
  badinput bigliteral constassign divovf divzero
  dupname shadow undefproc undefvar uninit)

set(engines interp vm)
if(LLVM)
  list(APPEND engines llvm)
endif()

set(dir ${SRC}/tests/errors)
set(failed 0)

foreach(c ${CASES})
  set(stdin_file ${dir}/${c}.stdin)
  set(ref "")
  set(ref_engine "")

  foreach(e ${engines})
    if(EXISTS ${stdin_file})
      execute_process(COMMAND ${PL0} --engine=${e} ${c}.pas
                      WORKING_DIRECTORY ${dir}
                      INPUT_FILE ${stdin_file}
                      OUTPUT_VARIABLE out ERROR_VARIABLE err
                      RESULT_VARIABLE rc)
    else()
      execute_process(COMMAND ${PL0} --engine=${e} ${c}.pas
                      WORKING_DIRECTORY ${dir}
                      OUTPUT_VARIABLE out ERROR_VARIABLE err
                      RESULT_VARIABLE rc)
    endif()
    set(got "rc=${rc}\n--stdout--\n${out}--stderr--\n${err}")

    if(ref_engine STREQUAL "")
      set(ref "${got}")
      set(ref_engine "${e}")
      set(ref_rc "${rc}")
      set(ref_err "${err}")
    elseif(NOT got STREQUAL ref)
      message(SEND_ERROR
              "${c}: ${e} differs from ${ref_engine}\n"
              "--- ${ref_engine} ---\n${ref}\n--- ${e} ---\n${got}")
      set(failed 1)
    endif()
  endforeach()

  if(NOT ref_rc EQUAL 1)
    message(SEND_ERROR "${c}: expected exit 1, got ${ref_rc}")
    set(failed 1)
  endif()

  file(READ ${dir}/expected/${c}.err want)
  if(NOT ref_err STREQUAL want)
    message(SEND_ERROR "${c}: wrong diagnostic\n"
                       "--- want ---\n${want}--- got ---\n${ref_err}")
    set(failed 1)
  endif()
endforeach()

if(failed)
  message(FATAL_ERROR "error lanes disagree")
endif()
message(STATUS "errors OK (${engines}; identical message, position and exit)")
