# A failing program's transcript: each case must exit 1 with exactly the
# stderr its expected/<case>.err holds -- the message and the position.
# PL/0's diagnostics (test/errors/) and each front end's report of an
# exception nothing caught (examples/mini-*/uncaught/) are checked this
# way. The real language reports in a format of its own -- a traceback, a
# stack trace -- so the transcript is this runtime's line, and each case's
# own comment says what the real language prints for it.
#
#   cmake -DBIN=<binary> -DDIR=<case directory> -DEXT=<source extension>
#         -P run_errors.cmake
#
# Every DIR/*.EXT is a case, run from DIR so its position names it by its
# file name alone. DIR/<case>.stdin, when there is one, is its input.

file(GLOB sources RELATIVE ${DIR} ${DIR}/*.${EXT})
list(SORT sources)
if(NOT sources)
  message(FATAL_ERROR "no *.${EXT} cases in ${DIR}")
endif()

set(failed 0)
foreach(src ${sources})
  get_filename_component(c ${src} NAME_WE)
  set(input)
  if(EXISTS ${DIR}/${c}.stdin)
    set(input INPUT_FILE ${DIR}/${c}.stdin)
  endif()
  execute_process(COMMAND ${BIN} ${src}
                  WORKING_DIRECTORY ${DIR}
                  ${input}
                  OUTPUT_VARIABLE out ERROR_VARIABLE err
                  RESULT_VARIABLE rc)

  if(NOT rc EQUAL 1)
    message(SEND_ERROR "${c}: expected exit 1, got ${rc}")
    set(failed 1)
  endif()

  file(READ ${DIR}/expected/${c}.err want)
  if(NOT err STREQUAL want)
    message(SEND_ERROR "${c}: wrong diagnostic\n"
                       "--- want ---\n${want}--- got ---\n${err}")
    set(failed 1)
  endif()
endforeach()

if(failed)
  message(FATAL_ERROR "error case output does not match expected/")
endif()
list(LENGTH sources n)
message(STATUS "${n} error cases OK (message, position and exit code)")
