# Error cases are not golden-compared against culebra: the binder reports
# undefined names, constant assignment and duplicate declarations at bind time
# where pl0.cul reports them at run time, and this runtime writes to stderr
# where pl0.cul writes to stdout. Both are deliberate. What is checked is that
# each case's message, position and exit code match tests/errors/expected/.

set(CASES
  badinput bigliteral constassign divovf divzero
  dupname recursion shadow undefproc undefvar uninit)

set(dir ${SRC}/tests/errors)
set(failed 0)

foreach(c ${CASES})
  set(stdin_file ${dir}/${c}.stdin)

  if(EXISTS ${stdin_file})
    execute_process(COMMAND ${PL0} ${c}.pas
                    WORKING_DIRECTORY ${dir}
                    INPUT_FILE ${stdin_file}
                    OUTPUT_VARIABLE out ERROR_VARIABLE err
                    RESULT_VARIABLE rc)
  else()
    execute_process(COMMAND ${PL0} ${c}.pas
                    WORKING_DIRECTORY ${dir}
                    OUTPUT_VARIABLE out ERROR_VARIABLE err
                    RESULT_VARIABLE rc)
  endif()

  if(NOT rc EQUAL 1)
    message(SEND_ERROR "${c}: expected exit 1, got ${rc}")
    set(failed 1)
  endif()

  file(READ ${dir}/expected/${c}.err want)
  if(NOT err STREQUAL want)
    message(SEND_ERROR "${c}: wrong diagnostic\n"
                       "--- want ---\n${want}--- got ---\n${err}")
    set(failed 1)
  endif()
endforeach()

if(failed)
  message(FATAL_ERROR "error case output does not match expected/")
endif()
message(STATUS "errors OK (message, position and exit code match expected/)")
