# What every front end under examples/ shares: the command line
# (driver.h), and add_front_end(), which registers one.
#
# The driver is one library, built once. It is the translation unit of
# each front end that takes the stdio host (VMLIB_DEFAULT_RUNTIME), and the
# one that instantiates the compiler and the executor -- so a front end's
# main.cc is a few lines of what is its own, and the executor is compiled
# once rather than once per front end.
add_library(front-end-driver STATIC driver.cc)
target_include_directories(front-end-driver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(front-end-driver PUBLIC vmlib)

# add_front_end(<name> [EXT <ext> ORACLE <oracle> [LEAD <file>]])
#
# The front end in the calling directory: the executable <name>, from that
# directory's main.cc and binder.cc, on the driver above. Every front end
# parses with cpp-peglib, from the one copy vendored under examples/pl0 --
# the same library with a different grammar -- included as a system
# header, so that a warning in it is upstream's to fix rather than this
# build's -Werror's.
#
# With EXT, the source extension, its tests too. Their names are an
# interface: CI's sanitizer lane selects a front end's tests by "<name>-".
# They register whether or not VMLIB_BUILD_TESTS is on: nothing else in
# the tree depends on a front end, so its checks live with it rather than
# in test/. (enable_testing() runs once at the top level, ahead of every
# add_subdirectory -- see the comment there for why that timing matters.)
#
#   <name>-samples   Every program under samples/ prints exactly its
#                    golden transcript, samples/golden/<sample>.txt,
#                    captured once from ORACLE (test/run_samples.cmake).
#                    LEAD, a file in the front end's directory, is bound
#                    ahead of each sample: mini-js's prelude.
#   <name>-uncaught  Only with an uncaught/ directory: an exception nothing
#                    catches is reported in the language's own words
#                    (RunOptions::describe), as uncaught/expected/ holds
#                    each case's report (test/run_errors.cmake).
#   <name>-sharing   Nothing a program could observe is evaluated twice
#                    (coreir::shared_effects): every sample, uncaught case
#                    and bench/languages program (test/run_sharing.cmake).
#
# PL/0 passes no EXT. Its binary is the main suite's oracle harness, so its
# tests are the suite's: test/CMakeLists.txt registers them under the
# suite's names, with its diagnostics' transcripts in test/errors/.
function(add_front_end name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "EXT;ORACLE;LEAD" "")
  if(DEFINED arg_UNPARSED_ARGUMENTS OR DEFINED arg_KEYWORDS_MISSING_VALUES
     OR (DEFINED arg_EXT AND NOT DEFINED arg_ORACLE)
     OR (NOT DEFINED arg_EXT AND (DEFINED arg_ORACLE OR DEFINED arg_LEAD)))
    message(FATAL_ERROR
            "add_front_end(<name> [EXT <ext> ORACLE <oracle> [LEAD <file>]])")
  endif()
  set(dir ${CMAKE_CURRENT_SOURCE_DIR})

  add_executable(${name} main.cc binder.cc)
  target_include_directories(${name} PRIVATE ${dir})
  target_include_directories(${name} SYSTEM PRIVATE
                             ${PROJECT_SOURCE_DIR}/examples/pl0/vendor)
  target_link_libraries(${name} PRIVATE front-end-driver)

  if(NOT DEFINED arg_EXT)
    return()
  endif()
  set(lead)
  if(DEFINED arg_LEAD)
    set(lead -DLEAD=${dir}/${arg_LEAD})
  endif()

  add_test(NAME ${name}-samples
           COMMAND ${CMAKE_COMMAND}
                   -DBIN=$<TARGET_FILE:${name}>
                   -DDIR=${dir}/samples
                   -DEXT=${arg_EXT}
                   "-DORACLE=${arg_ORACLE}"
                   ${lead}
                   -P ${PROJECT_SOURCE_DIR}/test/run_samples.cmake)

  if(EXISTS ${dir}/uncaught)
    add_test(NAME ${name}-uncaught
             COMMAND ${CMAKE_COMMAND}
                     -DBIN=$<TARGET_FILE:${name}>
                     -DDIR=${dir}/uncaught
                     -DEXT=${arg_EXT}
                     -P ${PROJECT_SOURCE_DIR}/test/run_errors.cmake)
  endif()

  add_test(NAME ${name}-sharing
           COMMAND ${CMAKE_COMMAND}
                   -DBIN=$<TARGET_FILE:${name}>
                   -DSRC=${dir}
                   -DBENCH=${PROJECT_SOURCE_DIR}/bench/languages
                   -DEXT=${arg_EXT}
                   ${lead}
                   -P ${PROJECT_SOURCE_DIR}/test/run_sharing.cmake)
endfunction()
