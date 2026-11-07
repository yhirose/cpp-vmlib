# Every program a front end ships, bound with --check-sharing: a node the
# program could observe being evaluated -- a call, a store, a fresh object
# -- reached from two parents on one path runs twice, and fails its case
# (coreir::shared_effects). The examples' audits found that bug class in
# every front end, one probe at a time; this asks it of all of them at once.
#
#   cmake -DBIN=<binary> -DSRC=<front end directory> -DBENCH=<bench/languages>
#         -DEXT=<source extension> [-DLEAD=<file>] -P run_sharing.cmake
#
# The cases are SRC/samples (and its subdirectories), SRC/uncaught and
# BENCH's own programs, every *.EXT among them. LEAD -- mini-js's prelude --
# is bound ahead of each case in its own directory, and is not a case.

# Every argument is required: the cases are recursive globs of SRC and
# BENCH, and a call that left BENCH out would walk the whole filesystem
# from its root.
foreach(arg BIN SRC BENCH EXT)
  if("${${arg}}" STREQUAL "")
    message(FATAL_ERROR "run_sharing.cmake: -D${arg} is required")
  endif()
endforeach()

file(GLOB_RECURSE sources ${SRC}/samples/*.${EXT} ${SRC}/uncaught/*.${EXT}
     ${BENCH}/*.${EXT})
if(LEAD)
  list(REMOVE_ITEM sources ${LEAD})
  get_filename_component(lead_dir ${LEAD} DIRECTORY)
endif()
list(SORT sources)
if(NOT sources)
  message(FATAL_ERROR "no *.${EXT} programs under ${SRC} or ${BENCH}")
endif()

set(failed 0)
foreach(src ${sources})
  get_filename_component(dir ${src} DIRECTORY)
  set(lead)
  if(LEAD AND dir STREQUAL lead_dir)
    set(lead ${LEAD})
  endif()
  execute_process(COMMAND ${BIN} --check-sharing ${lead} ${src}
                  WORKING_DIRECTORY ${dir}
                  OUTPUT_VARIABLE out ERROR_VARIABLE err
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(SEND_ERROR "${src}: exited ${rc}\n${err}")
    set(failed 1)
  endif()
endforeach()

if(failed)
  message(FATAL_ERROR "a node is evaluated twice")
endif()
list(LENGTH sources n)
message(STATUS "${n} programs, nothing evaluated twice")
