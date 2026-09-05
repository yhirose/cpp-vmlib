# The ninth front end. Like the rest of the PEG ones it shares
# examples/pl0's vendored peglib.h.
add_executable(mini-csharp main.cc binder.cc)
target_include_directories(mini-csharp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                       ${CMAKE_CURRENT_SOURCE_DIR}/../pl0/vendor)
target_link_libraries(mini-csharp PRIVATE vmlib)

add_test(NAME mini-csharp-samples
        COMMAND ${CMAKE_COMMAND}
                -DMINI_CSHARP=$<TARGET_FILE:mini-csharp>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/run_samples.cmake)
