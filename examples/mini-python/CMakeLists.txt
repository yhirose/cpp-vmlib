# The sixth front end, and the fifth to need a PEG -- all five share
# examples/pl0's vendored peglib.h. This one also needs a pass in front of
# the parser (layout.h), because Python's block structure is its
# indentation and a PEG has no state to count columns with.
add_executable(mini-python main.cc binder.cc)
target_include_directories(mini-python PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                       ${CMAKE_CURRENT_SOURCE_DIR}/../pl0/vendor)
target_link_libraries(mini-python PRIVATE vmlib)

add_test(NAME mini-python-samples
        COMMAND ${CMAKE_COMMAND}
                -DMINI_PYTHON=$<TARGET_FILE:mini-python>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/run_samples.cmake)
