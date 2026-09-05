# The seventh front end, and the smallest -- an s-expression grammar is
# four rules where the others need forty to a hundred and fifty.
add_executable(mini-scheme main.cc binder.cc)
target_include_directories(mini-scheme PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                       ${CMAKE_CURRENT_SOURCE_DIR}/../pl0/vendor)
target_link_libraries(mini-scheme PRIVATE vmlib)

add_test(NAME mini-scheme-samples
        COMMAND ${CMAKE_COMMAND}
                -DMINI_SCHEME=$<TARGET_FILE:mini-scheme>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/run_samples.cmake)
