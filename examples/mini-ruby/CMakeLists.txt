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
