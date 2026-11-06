# The fourth front end, and the third to need a PEG. Reuses examples/pl0's
# vendored peglib.h, like mini-go and mini-js -- four grammars, one parser
# library.
add_executable(mini-culebra main.cc binder.cc)
target_include_directories(mini-culebra PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mini-culebra SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../pl0/vendor)
target_link_libraries(mini-culebra PRIVATE vmlib vmlib_warnings)

add_test(NAME mini-culebra-samples
        COMMAND ${CMAKE_COMMAND}
                -DMINI_CULEBRA=$<TARGET_FILE:mini-culebra>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/run_samples.cmake)

# Nothing a program could observe is evaluated twice
# (coreir::shared_effects): every sample, uncaught case and bench program.
add_test(NAME mini-culebra-sharing
        COMMAND ${CMAKE_COMMAND}
                -DBIN=$<TARGET_FILE:mini-culebra>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
                -DBENCH=${PROJECT_SOURCE_DIR}/bench/languages
                -DEXT=cul
                -P ${PROJECT_SOURCE_DIR}/test/run_sharing.cmake)
