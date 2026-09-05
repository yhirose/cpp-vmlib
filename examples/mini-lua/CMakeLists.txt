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
