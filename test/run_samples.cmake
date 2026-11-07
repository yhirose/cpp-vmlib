# Each sample's output must match its golden file, the output of an oracle
# this repository does not contain -- the real language, or for PL/0
# culebra's own PL/0 interpreter: an independent implementation, not just a
# second run of this one. Self-consistency alone would not have caught
# cpp-peglib's own ODD bug (pl0.cc's interpreter and JIT agree with each
# other and are both wrong about it). Every front end's samples are
# checked this way, and its goldens are checked in turn, against the
# language itself.
#
#   cmake -DBIN=<command> -DDIR=<samples directory> -DEXT=<source extension>
#         -DLANG=<language> [-DLEAD=<file>] -P run_samples.cmake
#   cmake -DORACLE=ON -DDIR=... -DEXT=... -DLANG=... [-DLEAD=...]
#         -P run_samples.cmake
#
# Every *.EXT under DIR is a case, run from its own directory by its file
# name alone -- the name a position or a chunk name gives -- and must exit
# 0 having printed exactly DIR/golden/<case>.txt. A case may sit in a
# subdirectory of its own (mini-go's do: each is a standalone `package
# main`, and two side by side would both declare `func main`, which `go
# build` rejects). <case>.stdin beside it, when there is one, is its input.
# LEAD -- mini-js's prelude -- is bound ahead of each case and is not one
# (test/cases.cmake). A golden no case makes fails too.
#
# LANG is the key test/languages.sh knows the language by. BIN is the front
# end's binary; with ORACLE, the script runs the language itself in its
# place, `test/oracle.sh LANG`, and takes no BIN: a front end's goldens
# test (front_end_tests), which checks the goldens rather than the front
# end. Then, and only then, VMLIB_UPDATE_GOLDENS in
# the environment makes the run rewrite each golden from what the language
# printed, and remove one no case makes -- by hand, never from the build: a
# golden the front end's own test could refresh would be evidence of
# nothing.

include(${CMAKE_CURRENT_LIST_DIR}/cases.cmake)
if(ORACLE)
  if(DEFINED BIN)
    message(FATAL_ERROR "run_samples.cmake: -DORACLE runs the language "
                        "itself, and takes no -DBIN")
  endif()
  require_args(run_samples.cmake DIR EXT LANG)
  set(BIN ${CMAKE_CURRENT_LIST_DIR}/oracle.sh ${LANG})
else()
  require_args(run_samples.cmake BIN DIR EXT LANG)
endif()

set(update OFF)
if(ORACLE AND "$ENV{VMLIB_UPDATE_GOLDENS}")
  set(update ON)
endif()

# The language at the version the oracle accepts, which it names -- or
# refuses, once for every case.
if(ORACLE)
  execute_process(COMMAND ${BIN} --version
                  OUTPUT_VARIABLE source ERROR_VARIABLE err
                  RESULT_VARIABLE rc OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${err}")
  endif()
endif()

# Absolute, so that the goldens the cases name and the ones the orphan
# check below globs are spelled alike.
get_filename_component(DIR ${DIR} ABSOLUTE)
file(GLOB_RECURSE sources ${DIR}/*.${EXT})
drop_lead(sources)
list(SORT sources)
if(NOT sources)
  message(FATAL_ERROR "no *.${EXT} samples under ${DIR}")
endif()

set(failed 0)
set(made)
foreach(src ${sources})
  get_filename_component(dir ${src} DIRECTORY)
  get_filename_component(name ${src} NAME)
  get_filename_component(s ${src} NAME_WE)
  set(golden ${DIR}/golden/${s}.txt)
  list(APPEND made ${golden})
  lead_for(lead ${src})
  set(input)
  if(EXISTS ${dir}/${s}.stdin)
    set(input INPUT_FILE ${dir}/${s}.stdin)
  endif()
  execute_process(COMMAND ${BIN} ${lead} ${name}
                  WORKING_DIRECTORY ${dir}
                  ${input}
                  OUTPUT_VARIABLE out ERROR_VARIABLE err
                  RESULT_VARIABLE rc)

  if(NOT rc EQUAL 0)
    message(SEND_ERROR "${s}: exited ${rc}\n--stdout--\n${out}--stderr--\n${err}")
    set(failed 1)
    continue()
  endif()

  if(update)
    file(WRITE ${golden} "${out}")
  elseif(NOT EXISTS ${golden})
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

# The goldens no case made. Only after the glob above found a case: an
# empty one stopped the script before it could take every golden for an
# orphan.
file(GLOB orphans ${DIR}/golden/*.txt)
list(REMOVE_ITEM orphans ${made})
foreach(golden ${orphans})
  if(update)
    file(REMOVE ${golden})
    message(STATUS "removed ${golden}, which no sample makes")
  else()
    message(SEND_ERROR "no sample makes ${golden}")
    set(failed 1)
  endif()
endforeach()

list(LENGTH sources n)
if(ORACLE)
  if(failed)
    message(FATAL_ERROR "the goldens are not what ${source} prints")
  elseif(update)
    message(STATUS "${n} goldens written by ${source}")
  else()
    message(STATUS "${n} goldens are what ${source} prints")
  endif()
elseif(failed)
  message(FATAL_ERROR "sample output does not match the goldens, which are "
                      "${LANG}'s output (test/languages.sh)")
else()
  message(STATUS "${n} samples OK (match ${LANG}'s output)")
endif()
