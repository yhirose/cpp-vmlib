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

# Not PL/0 tests: these exercise coreir/vm directly, building their Core-IR by
# hand and supplying their own coreir_rt host, to pin properties the samples
# cannot reach.
#
# throw_safety: a host whose coreir_rt_fail throws rather than exits, to lock
# in the exception-safety property include/coreir/rt.h documents.
# deep_calls: call chains deeper than the machine stack, which the executor
# survives because its frames are heap objects rather than C++ activations.
foreach(t throw_safety deep_calls values closures)
  add_executable(${t}_test ${t}.cc)
  target_link_libraries(${t}_test PRIVATE coreir vm)
  add_test(NAME ${t} COMMAND ${t}_test)
endforeach()
