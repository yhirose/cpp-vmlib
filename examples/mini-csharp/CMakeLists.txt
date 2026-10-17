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

# An exception nothing catches is reported in the language's own words
# (RunOptions::describe): uncaught/expected/ holds each case's report.
add_test(NAME mini-csharp-uncaught
        COMMAND ${CMAKE_COMMAND}
                -DBIN=$<TARGET_FILE:mini-csharp>
                -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/uncaught
                -DEXT=cs
                -P ${PROJECT_SOURCE_DIR}/test/run_errors.cmake)
