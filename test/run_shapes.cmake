# What a program compiles to, where that is the point: each case's
# bytecode (--dump-bc) holds each text its expected/<case>.txt lists
# exactly as many times as the line says, one `<count> <text>` a line. A
# front end that compiled a subtree once per parent that holds it -- an
# argument in both arms of an If -- passes every output check and
# multiplies its code with each level of nesting; this is where that
# stays caught.
#
#   cmake -DBIN=<binary> -DDIR=<case directory> -DEXT=<source extension>
#         -P run_shapes.cmake
#
# Every DIR/*.EXT is a case, run from DIR.

include(${CMAKE_CURRENT_LIST_DIR}/cases.cmake)
require_args(run_shapes.cmake BIN DIR EXT)

case_files(sources)

set(failed 0)
foreach(src ${sources})
  get_filename_component(c ${src} NAME_WE)
  execute_process(COMMAND ${BIN} --dump-bc ${src}
                  WORKING_DIRECTORY ${DIR}
                  OUTPUT_VARIABLE out ERROR_VARIABLE err
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(SEND_ERROR "${c}: exited ${rc}\n${err}")
    set(failed 1)
  endif()
  file(STRINGS ${DIR}/expected/${c}.txt wants)
  foreach(want ${wants})
    string(REGEX MATCH "^([0-9]+) (.+)$" line "${want}")
    if(NOT line)
      message(FATAL_ERROR "${c}: `${want}` is not `<count> <text>`")
    endif()
    set(n ${CMAKE_MATCH_1})
    set(text ${CMAKE_MATCH_2})
    string(LENGTH "${out}" before)
    string(REPLACE "${text}" "" rest "${out}")
    string(LENGTH "${rest}" after)
    string(LENGTH "${text}" each)
    math(EXPR got "(${before} - ${after}) / ${each}")
    if(NOT got EQUAL n)
      message(SEND_ERROR "${c}: `${text}` ${got} times, want ${n}")
      set(failed 1)
    endif()
  endforeach()
endforeach()

end_cases(${failed} sources "a case compiles to the wrong shape"
          "cases, each the shape it should be")
