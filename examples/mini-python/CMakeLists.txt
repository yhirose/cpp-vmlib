# The sixth front end, and the fifth to need a PEG -- all five share
# examples/pl0's vendored peglib.h. This one also needs a pass in front of
# the parser (layout.h), because Python's block structure is its
# indentation and a PEG has no state to count columns with.
add_executable(mini-python main.cc binder.cc)
target_include_directories(mini-python PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mini-python SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../pl0/vendor)
target_link_libraries(mini-python PRIVATE vmlib)

# The goldens are `python3`'s output (samples/gen_golden.sh) -- whose
# integers are unbounded by definition, which samples/bignums.py checks.
add_test(NAME mini-python-samples
        COMMAND ${CMAKE_COMMAND}
                -DBIN=$<TARGET_FILE:mini-python>
                -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/samples
                -DEXT=py
                -DORACLE=python3
                -P ${PROJECT_SOURCE_DIR}/test/run_samples.cmake)

# An exception nothing catches is reported in the language's own words
# (RunOptions::describe): uncaught/expected/ holds each case's report.
add_test(NAME mini-python-uncaught
        COMMAND ${CMAKE_COMMAND}
                -DBIN=$<TARGET_FILE:mini-python>
                -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/uncaught
                -DEXT=py
                -P ${PROJECT_SOURCE_DIR}/test/run_errors.cmake)

# Nothing a program could observe is evaluated twice
# (coreir::shared_effects): every sample, uncaught case and bench program.
add_test(NAME mini-python-sharing
        COMMAND ${CMAKE_COMMAND}
                -DBIN=$<TARGET_FILE:mini-python>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
                -DBENCH=${PROJECT_SOURCE_DIR}/bench/languages
                -DEXT=py
                -P ${PROJECT_SOURCE_DIR}/test/run_sharing.cmake)
