# Every program a front end ships, bound with --check-effects: a node the
# program could observe being evaluated -- a call, a store, a fresh object
# -- reached from two parents on one path runs twice
# (coreir::shared_effects), and one built and then reached from no
# function body never runs (coreir::dropped_effects); either fails its
# case. The examples' audits found both bug classes in front end after
# front end, one probe at a time; this asks it of all of them at once.
#
#   cmake -DBIN=<binary> -DSRC=<front end directory>
#         -DEXT=<source extension> [-DLEAD=<file>] -P run_effects.cmake
#
# The cases are SRC/samples (and its subdirectories), SRC/uncaught and
# bench/languages' programs, every *.EXT among them. LEAD -- mini-js's
# prelude -- is bound ahead of each of its samples and is not a case
# (test/cases.cmake).

include(${CMAKE_CURRENT_LIST_DIR}/cases.cmake)
require_args(run_effects.cmake BIN SRC EXT)
get_filename_component(bench ${CMAKE_CURRENT_LIST_DIR}/../bench/languages
                       ABSOLUTE)

file(GLOB_RECURSE sources ${SRC}/samples/*.${EXT} ${SRC}/uncaught/*.${EXT}
     ${bench}/*.${EXT})
drop_lead(sources)
list(SORT sources)
if(NOT sources)
  message(FATAL_ERROR "no *.${EXT} programs under ${SRC} or ${bench}")
endif()

set(failed 0)
foreach(src ${sources})
  get_filename_component(dir ${src} DIRECTORY)
  lead_for(lead ${src})
  execute_process(COMMAND ${BIN} --check-effects ${lead} ${src}
                  WORKING_DIRECTORY ${dir}
                  OUTPUT_VARIABLE out ERROR_VARIABLE err
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(SEND_ERROR "${src}: exited ${rc}\n${err}")
    set(failed 1)
  endif()
endforeach()

if(failed)
  message(FATAL_ERROR "a node is evaluated twice, or dropped")
endif()
list(LENGTH sources n)
message(STATUS "${n} programs, nothing evaluated twice or dropped")
