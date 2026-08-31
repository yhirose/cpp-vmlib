# Each sample/error case runs once, through the one engine there is, and is
# checked against an oracle this repository does not contain: culebra's own
# PL/0 interpreter for samples, a fixed expected/ transcript for errors.
add_test(NAME samples
         COMMAND ${CMAKE_COMMAND}
                 -DPL0=$<TARGET_FILE:pl0>
                 -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/..
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_samples.cmake)

add_test(NAME errors
         COMMAND ${CMAKE_COMMAND}
                 -DPL0=$<TARGET_FILE:pl0>
                 -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/..
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_errors.cmake)

# Not a PL/0 test: exercises coreir/vm directly, with a host coreir_rt
# implementation that throws instead of exiting, to lock in the exception-
# safety property include/coreir/rt.h documents.
add_executable(throw_safety_test throw_safety.cc)
target_link_libraries(throw_safety_test PRIVATE coreir vm)
add_test(NAME throw_safety COMMAND throw_safety_test)
