# The benchmark harness. Not a ctest test: a benchmark's answer is a number
# to compare against another build's, not a pass or a fail.
add_executable(bench bench.cc)
target_link_libraries(bench PRIVATE vmlib)
