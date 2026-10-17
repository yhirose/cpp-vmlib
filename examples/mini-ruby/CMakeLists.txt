# The eighth front end. Like the rest of the PEG ones it shares
# examples/pl0's vendored peglib.h.
add_executable(mini-ruby main.cc binder.cc)
target_include_directories(mini-ruby PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                     ${CMAKE_CURRENT_SOURCE_DIR}/../pl0/vendor)
target_link_libraries(mini-ruby PRIVATE vmlib)

add_test(NAME mini-ruby-samples
        COMMAND ${CMAKE_COMMAND}
                -DMINI_RUBY=$<TARGET_FILE:mini-ruby>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/run_samples.cmake)

# An exception nothing catches is reported in the language's own words
# (RunOptions::describe): uncaught/expected/ holds each case's report.
add_test(NAME mini-ruby-uncaught
        COMMAND ${CMAKE_COMMAND}
                -DBIN=$<TARGET_FILE:mini-ruby>
                -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/uncaught
                -DEXT=rb
                -P ${PROJECT_SOURCE_DIR}/test/run_errors.cmake)
