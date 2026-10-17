# The fifth front end, and the fourth to need a PEG -- all four share
# examples/pl0's vendored peglib.h.
add_executable(mini-lua main.cc binder.cc)
target_include_directories(mini-lua PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                    ${CMAKE_CURRENT_SOURCE_DIR}/../pl0/vendor)
target_link_libraries(mini-lua PRIVATE vmlib)

add_test(NAME mini-lua-samples
        COMMAND ${CMAKE_COMMAND}
                -DMINI_LUA=$<TARGET_FILE:mini-lua>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/run_samples.cmake)

# An exception nothing catches is reported in the language's own words
# (RunOptions::describe): uncaught/expected/ holds each case's report.
add_test(NAME mini-lua-uncaught
        COMMAND ${CMAKE_COMMAND}
                -DBIN=$<TARGET_FILE:mini-lua>
                -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/uncaught
                -DEXT=lua
                -P ${PROJECT_SOURCE_DIR}/test/run_errors.cmake)
