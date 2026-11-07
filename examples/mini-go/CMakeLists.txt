# Reuses examples/pl0's vendored peglib.h rather than a second copy of the
# same 7000-line header -- every front end uses the same library, just a
# different grammar.
add_executable(mini-go main.cc binder.cc)
target_include_directories(mini-go PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mini-go SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../pl0/vendor)
target_link_libraries(mini-go PRIVATE vmlib)

# Self-contained: unlike PL/0's samples (wired into the top-level test/,
# since PL/0 doubles as that suite's oracle harness), this example is not
# depended on by anything else, so its own sample check lives here rather
# than in test/. (enable_testing() is called once, at the top level, ahead
# of every add_subdirectory -- see the comment there for why it has to run
# that early for this add_test to be found by a plain `ctest` at the top.)
#
# The goldens are `go run`'s output (samples/gen_golden.sh).
add_test(NAME mini-go-samples
        COMMAND ${CMAKE_COMMAND}
                -DBIN=$<TARGET_FILE:mini-go>
                -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/samples
                -DEXT=go
                "-DORACLE=go run"
                -P ${PROJECT_SOURCE_DIR}/test/run_samples.cmake)

# Nothing a program could observe is evaluated twice
# (coreir::shared_effects): every sample, uncaught case and bench program.
add_test(NAME mini-go-sharing
        COMMAND ${CMAKE_COMMAND}
                -DBIN=$<TARGET_FILE:mini-go>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
                -DBENCH=${PROJECT_SOURCE_DIR}/bench/languages
                -DEXT=go
                -P ${PROJECT_SOURCE_DIR}/test/run_sharing.cmake)
