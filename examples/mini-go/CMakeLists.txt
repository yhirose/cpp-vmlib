# The only target that knows what a PEG is. Reuses examples/pl0's vendored
# peglib.h rather than a second copy of the same 7000-line header -- both
# front ends use the same library, just different grammars.
add_executable(mini-go main.cc binder.cc)
target_include_directories(mini-go PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                           ${CMAKE_CURRENT_SOURCE_DIR}/../pl0/vendor)
target_link_libraries(mini-go PRIVATE vmlib)

# Self-contained: unlike PL/0's samples (wired into the top-level test/,
# since PL/0 doubles as that suite's oracle harness), this example is not
# depended on by anything else, so its own sample check lives here rather
# than in test/. (enable_testing() is called once, at the top level, ahead
# of every add_subdirectory -- see the comment there for why it has to run
# that early for this add_test to be found by a plain `ctest` at the top.)
add_test(NAME mini-go-samples
        COMMAND ${CMAKE_COMMAND}
                -DMINI_GO=$<TARGET_FILE:mini-go>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/run_samples.cmake)
