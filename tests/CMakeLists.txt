# Two-tier verification.
#
# Tier 1 is the three lanes against each other. Tier 2 is a golden file taken
# from culebra's own PL/0 interpreter, because three lanes agreeing does not
# make them right -- pl0.cc's two lanes agree with each other about ODD and are
# both wrong.
add_test(NAME samples
         COMMAND ${CMAKE_COMMAND}
                 -DPL0=$<TARGET_FILE:pl0>
                 -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/..
                 -DLLVM=${PL0_ENABLE_LLVM}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_samples.cmake)

add_test(NAME errors
         COMMAND ${CMAKE_COMMAND}
                 -DPL0=$<TARGET_FILE:pl0>
                 -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/..
                 -DLLVM=${PL0_ENABLE_LLVM}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_errors.cmake)
