cmake_minimum_required(VERSION 3.16)
project(cpp-vmlib CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# vmlib -- the IR, the compiler and the executor, in one header. Note what is
# absent from this target: any mention of PL/0, and any path that reaches
# peglib.h. A front end is a consumer of this library, never the other way
# round.
# ---------------------------------------------------------------------------
add_library(vmlib INTERFACE)
target_include_directories(vmlib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

option(VMLIB_BUILD_TESTS "Build cpp-vmlib tests" ON)
option(VMLIB_BUILD_EXAMPLES "Build the example front ends" OFF)
option(VMLIB_BUILD_BENCH "Build the benchmark harness" ON)

# Unconditional, and up here before any add_subdirectory: a subdirectory's
# own add_test only reaches `ctest` run from the top build directory if the
# *top-level* CTestTestfile.cmake was generated, which needs this to have
# already run by the time that subdirectory is added -- calling it again
# inside `if(VMLIB_BUILD_TESTS)` below would be too late for
# example/go_mini's own test to be discovered when VMLIB_BUILD_EXAMPLES is
# on but VMLIB_BUILD_TESTS is off. Enabling testing adds no tests by itself.
enable_testing()

# The PL/0 front end is an example, and also the test suite's oracle
# harness: the sample and error transcripts run its binary.
if(VMLIB_BUILD_EXAMPLES OR VMLIB_BUILD_TESTS)
  add_subdirectory(example/pl0)
endif()

# go_mini is a second example, proving the managed-language recipes
# (Fixed-width integers, `float`, Static calls, Struct fields, Switch)
# against a real `go run` rather than culebra's dynamic PL/0. It plays no
# role in the main test suite the way PL/0 does, so it stays behind
# VMLIB_BUILD_EXAMPLES alone --
# but its own sample check (example/go_mini/CMakeLists.txt) still
# registers with ctest regardless of VMLIB_BUILD_TESTS, same as any other
# add_test once testing is enabled.
if(VMLIB_BUILD_EXAMPLES)
  add_subdirectory(example/go_mini)
endif()

if(VMLIB_BUILD_TESTS)
  add_subdirectory(test)
endif()

if(VMLIB_BUILD_BENCH)
  add_subdirectory(bench)
endif()

install(FILES vmlib.h DESTINATION include)
