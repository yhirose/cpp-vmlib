# What every front end under examples/ shares: the command line
# (driver.h), and add_front_end(), which registers one.
#
# The driver is one library, built once. It is the translation unit of
# each front end that takes the stdio host (VMLIB_DEFAULT_RUNTIME), and the
# one that instantiates the compiler and the executor's run loop -- so a
# front end's main.cc is a few lines of what is its own, and that code is
# compiled once rather than once per front end. (A binder whose host
# functions call back into the executor still instantiates that part.)
add_library(front-end-driver STATIC driver.cc)
target_include_directories(front-end-driver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(front-end-driver PUBLIC vmlib)

# add_front_end(<name> [EXT <ext> LANG <lang> [LEAD <file>]])
#
# The front end in the calling directory: the executable <name>, from that
# directory's main.cc and binder.cc, on the driver above. Every front end
# parses with cpp-peglib, from the one copy vendored under examples/pl0 --
# the same library with a different grammar -- included as a system
# header, so that a warning in it is upstream's to fix rather than this
# build's -Werror's.
#
# With EXT, the source extension, and LANG, the language's key in
# test/languages.sh, its tests too: front_end_tests() below, named
# "<name>-". They register whether or not VMLIB_BUILD_TESTS is on:
# nothing else in the tree depends on a front end, so its checks live with
# it rather than in test/. (enable_testing() runs once at the top level,
# ahead of every add_subdirectory -- see the comment there for why that
# timing matters.) PL/0 passes no EXT: its binary is the main suite's
# oracle harness, so its tests are the suite's, registered by
# test/CMakeLists.txt.
function(add_front_end name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "EXT;LANG;LEAD" "")
  if(DEFINED arg_UNPARSED_ARGUMENTS OR DEFINED arg_KEYWORDS_MISSING_VALUES
     OR (DEFINED arg_EXT AND NOT DEFINED arg_LANG)
     OR (NOT DEFINED arg_EXT AND (DEFINED arg_LANG OR DEFINED arg_LEAD)))
    message(FATAL_ERROR
            "add_front_end(<name> [EXT <ext> LANG <lang> [LEAD <file>]])")
  endif()

  add_executable(${name} main.cc binder.cc)
  target_include_directories(${name} SYSTEM PRIVATE
                             ${PROJECT_SOURCE_DIR}/examples/pl0/vendor)
  target_link_libraries(${name} PRIVATE front-end-driver)

  if(DEFINED arg_EXT)
    set(lead)
    if(DEFINED arg_LEAD)
      set(lead LEAD ${CMAKE_CURRENT_SOURCE_DIR}/${arg_LEAD})
    endif()
    front_end_tests(${name}- ${name} DIR ${CMAKE_CURRENT_SOURCE_DIR}
                    EXT ${arg_EXT} LANG ${arg_LANG} ${lead})
  endif()
endfunction()

# front_end_tests(<prefix> <target> DIR <dir> EXT <ext> LANG <lang>
#                 [LEAD <file>])
#
# The checks over a front end's programs, the <target> binary run on the
# *.<ext> files under <dir> -- add_front_end's, prefixed "<name>-", and
# the main suite's for PL/0, unprefixed. Their names are an interface:
# CI's sanitizer lane selects a front end's tests by "<name>-".
#
#   <prefix>samples   Every program under samples/ prints exactly its
#                     golden transcript, samples/golden/<sample>.txt, the
#                     output of the real language LANG -- its key in
#                     test/languages.sh (test/run_samples.cmake). LEAD, a
#                     file under samples/, is bound ahead of each sample:
#                     mini-js's prelude.
#   <prefix>goldens   Only with VMLIB_CHECK_GOLDENS, and labeled
#                     `goldens`: the same samples run through LANG itself
#                     (test/oracle.sh) rather than <target>, which checks
#                     the goldens against the language instead of the
#                     front end against the goldens.
#   <prefix>uncaught  Only with an uncaught/ directory: an exception
#                     nothing catches is reported in the language's own
#                     words (RunOptions::describe), as uncaught/expected/
#                     holds each case's report (test/run_errors.cmake).
#   <prefix>effects   Nothing a program could observe is evaluated twice
#                     (coreir::shared_effects) or built and dropped
#                     (coreir::dropped_effects): every sample, uncaught
#                     case and bench/languages program
#                     (test/run_effects.cmake).
function(front_end_tests prefix target)
  cmake_parse_arguments(PARSE_ARGV 2 arg "" "DIR;EXT;LANG;LEAD" "")
  if(DEFINED arg_UNPARSED_ARGUMENTS OR DEFINED arg_KEYWORDS_MISSING_VALUES
     OR NOT DEFINED arg_DIR OR NOT DEFINED arg_EXT OR NOT DEFINED arg_LANG)
    message(FATAL_ERROR "front_end_tests(<prefix> <target> DIR <dir> "
                        "EXT <ext> LANG <lang> [LEAD <file>])")
  endif()
  set(run ${CMAKE_COMMAND} -DBIN=$<TARGET_FILE:${target}> -DEXT=${arg_EXT})
  set(lead)
  if(DEFINED arg_LEAD)
    set(lead -DLEAD=${arg_LEAD})
  endif()
  # For what runs a front end outside ctest: the top-level CMakeLists.txt
  # writes these into the build directory, as front-ends.txt. The prefix
  # comes last, since PL/0's is empty.
  string(JOIN "\t" line ${target} $<TARGET_FILE:${target}> ${arg_EXT}
              ${arg_LANG} "${prefix}")
  set_property(GLOBAL APPEND PROPERTY VMLIB_FRONT_ENDS "${line}")
  set(scripts ${PROJECT_SOURCE_DIR}/test)
  set(samples -DDIR=${arg_DIR}/samples -DLANG=${arg_LANG} ${lead}
              -P ${scripts}/run_samples.cmake)

  add_test(NAME ${prefix}samples COMMAND ${run} ${samples})
  if(VMLIB_CHECK_GOLDENS)
    add_test(NAME ${prefix}goldens
             COMMAND ${CMAKE_COMMAND} -DEXT=${arg_EXT} -DORACLE=ON ${samples})
    set_tests_properties(${prefix}goldens PROPERTIES LABELS goldens)
  endif()
  if(EXISTS ${arg_DIR}/uncaught)
    add_test(NAME ${prefix}uncaught
             COMMAND ${run} -DDIR=${arg_DIR}/uncaught
                     -P ${scripts}/run_errors.cmake)
  endif()
  add_test(NAME ${prefix}effects
           COMMAND ${run} -DSRC=${arg_DIR} ${lead}
                   -P ${scripts}/run_effects.cmake)
endfunction()
