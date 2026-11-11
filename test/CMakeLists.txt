# PL/0's programs, checked the way every front end's are
# (front_end_tests, examples/common/CMakeLists.txt) but as the main suite's
# own, unprefixed: `samples`, each against a golden captured from an oracle
# this repository does not contain -- culebra's own PL/0 interpreter --
# `effects`, and `errors`, its diagnostics against fixed transcripts
# (examples/pl0/errors/). Those are not golden-compared against culebra:
# the binder reports undefined names, constant assignment and duplicate
# declarations at bind time where pl0.cul reports them at run time, and
# this runtime writes to stderr where pl0.cul writes to stdout. Both are
# deliberate.
front_end_tests("" pl0 DIR ${PROJECT_SOURCE_DIR}/examples/pl0 EXT pas
                LANG pl0)

# Not PL/0 tests: these exercise coreir/vm directly, building their Core-IR by
# hand and supplying their own coreir_rt host, to pin properties the samples
# cannot reach. One executable each: every test defines its own host and its
# own main, so they cannot share a binary.
#
# throw_safety: a host whose coreir_rt_fail throws rather than exits, to lock
# in the exception-safety property coreir/rt.h documents.
# deep_calls: call chains deeper than the machine stack, which the executor
# survives because its frames are heap objects rather than C++ activations.
# alloc_failure: every allocation a run makes failed in turn, which the run
# survives the way it survives a host's throw.
foreach(t throw_safety deep_calls values closures lifetimes containers
        control_flow exceptions defers gc generators arity drops jobs names
        ints fields switch natives tailcalls coroutines bigint_recipe
        singleton binding verify default_rt alloc_failure profile meta let
        trap_kinds)
  add_executable(test_${t} test_${t}.cc)
  target_link_libraries(test_${t} PRIVATE vmlib)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()

# two_tus: vmlib.h included from two translation units linked into one
# binary, so a definition that is not inline shows up as a link error here
# rather than in a host's build.
add_executable(test_two_tus test_two_tus.cc test_two_tus_other.cc)
target_link_libraries(test_two_tus PRIVATE vmlib)
add_test(NAME two_tus COMMAND test_two_tus)

# run_samples.cmake's guards against a sample whose output the language does
# not fix, with a fake oracle (test/run_samples_guard.cmake).
add_test(NAME run_samples_guard
         COMMAND ${CMAKE_COMMAND} -DWORK=${CMAKE_CURRENT_BINARY_DIR}/run_samples_guard
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_samples_guard.cmake)
