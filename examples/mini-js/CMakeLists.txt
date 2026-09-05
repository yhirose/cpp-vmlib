# The third front end, and the second to need a PEG. Like mini-go it reuses
# examples/pl0's vendored peglib.h rather than a third copy of the same
# 7000-line header -- three grammars, one parser library.
add_executable(mini-js main.cc binder.cc)
target_include_directories(mini-js PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                           ${CMAKE_CURRENT_SOURCE_DIR}/../pl0/vendor)
target_link_libraries(mini-js PRIVATE vmlib)

# Self-contained, the same way mini-go's check is: nothing else in the tree
# depends on this front end, so its own sample check lives here rather than
# in test/. (enable_testing() runs once at the top level, ahead of every
# add_subdirectory -- see the comment there for why that timing matters.)
add_test(NAME mini-js-samples
        COMMAND ${CMAKE_COMMAND}
                -DMINI_JS=$<TARGET_FILE:mini-js>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/run_samples.cmake)
