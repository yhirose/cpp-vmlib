# What the case scripts -- run_samples.cmake, run_errors.cmake,
# run_effects.cmake and run_shapes.cmake, each run with `cmake -P` --
# share, include()d by each.

# Stops the script unless every named -D argument was given. Each script's
# cases are a glob of a directory it was handed, and a directory left out
# would glob from the filesystem's root.
function(require_args script)
  foreach(arg ${ARGN})
    if("${${arg}}" STREQUAL "")
      message(FATAL_ERROR "${script}: -D${arg} is required")
    endif()
  endforeach()
endfunction()

# LEAD -- mini-js's prelude -- is not a case. It is bound ahead of every
# case under its own directory (the samples it is the prelude of) and of
# nothing else: a bench program or an uncaught case is a whole program.
# drop_lead(<list>) takes it out of a list of cases; lead_for(<out> <case>)
# sets <out> to what to bind ahead of <case>, LEAD or nothing.
function(drop_lead list)
  if(LEAD)
    list(REMOVE_ITEM ${list} ${LEAD})
    set(${list} ${${list}} PARENT_SCOPE)
  endif()
endfunction()

# case_files(<out>) sets <out> to the cases of a directory of them, as
# run_errors.cmake and run_shapes.cmake take theirs: every DIR/*.EXT, by
# file name, sorted. A directory with none is a mistake, and stops the
# script.
function(case_files out)
  file(GLOB sources RELATIVE ${DIR} ${DIR}/*.${EXT})
  list(SORT sources)
  if(NOT sources)
    message(FATAL_ERROR "no *.${EXT} cases in ${DIR}")
  endif()
  set(${out} ${sources} PARENT_SCOPE)
endfunction()

# end_cases(<failed> <cases> <failure> <success>): how such a script
# ends, after each case has reported its own failures with SEND_ERROR --
# stopped with <failure> when <failed> is set, and otherwise saying
# "<n> <success>" of the list <cases>.
function(end_cases failed cases failure success)
  if(failed)
    message(FATAL_ERROR "${failure}")
  endif()
  list(LENGTH ${cases} n)
  message(STATUS "${n} ${success}")
endfunction()

function(lead_for out case)
  set(${out} "" PARENT_SCOPE)
  if(LEAD)
    get_filename_component(lead_dir ${LEAD} DIRECTORY)
    string(FIND "${case}" "${lead_dir}/" at)
    if(at EQUAL 0)
      set(${out} ${LEAD} PARENT_SCOPE)
    endif()
  endif()
endfunction()
