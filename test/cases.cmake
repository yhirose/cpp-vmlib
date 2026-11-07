# What the three case scripts -- run_samples.cmake, run_errors.cmake and
# run_sharing.cmake, each run with `cmake -P` -- share, include()d by each.

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
