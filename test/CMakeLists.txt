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
# cannot reach. One executable each: every test defines its own host and its
# own main, so they cannot share a binary.
#
# throw_safety: a host whose coreir_rt_fail throws rather than exits, to lock
# in the exception-safety property coreir/rt.h documents.
# deep_calls: call chains deeper than the machine stack, which the executor
# survives because its frames are heap objects rather than C++ activations.
foreach(t throw_safety deep_calls values closures lifetimes containers
        control_flow exceptions defers gc generators arity drops jobs names
        ints fields switch natives tailcalls coroutines bigint_recipe
        singleton binding verify default_rt)
  add_executable(test_${t} test_${t}.cc)
  target_link_libraries(test_${t} PRIVATE vmlib)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()

# The header stays clean under -Wall -Wextra: a host that builds with
# -Werror includes it, so a warning here is that host's broken build.
# verify's own code is warning-free, so anything this reports is vmlib.h's.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test_verify PRIVATE -Wall -Wextra -Werror)
endif()

# two_tus: vmlib.h included from two translation units linked into one
# binary, so a definition that is not inline shows up as a link error here
# rather than in a host's build.
add_executable(test_two_tus test_two_tus.cc test_two_tus_other.cc)
target_link_libraries(test_two_tus PRIVATE vmlib)
add_test(NAME two_tus COMMAND test_two_tus)
