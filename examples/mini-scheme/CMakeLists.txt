# The seventh front end, and the smallest -- an s-expression grammar is
# four rules where the others need forty to a hundred and fifty.
add_executable(mini-scheme main.cc binder.cc)
target_include_directories(mini-scheme PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mini-scheme SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../pl0/vendor)
target_link_libraries(mini-scheme PRIVATE vmlib)

# The goldens are `guile`'s output (samples/gen_golden.sh).
add_test(NAME mini-scheme-samples
        COMMAND ${CMAKE_COMMAND}
                -DBIN=$<TARGET_FILE:mini-scheme>
                -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/samples
                -DEXT=scm
                -DORACLE=guile
                -P ${PROJECT_SOURCE_DIR}/test/run_samples.cmake)

# Nothing a program could observe is evaluated twice
# (coreir::shared_effects): every sample, uncaught case and bench program.
add_test(NAME mini-scheme-sharing
        COMMAND ${CMAKE_COMMAND}
                -DBIN=$<TARGET_FILE:mini-scheme>
                -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
                -DBENCH=${PROJECT_SOURCE_DIR}/bench/languages
                -DEXT=scm
                -P ${PROJECT_SOURCE_DIR}/test/run_sharing.cmake)
