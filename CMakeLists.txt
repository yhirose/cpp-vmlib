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

# -Wall -Wextra -Werror for everything this repository builds -- the front
# ends, the tests, the bench -- set once for the directory, so a target
# added later cannot be left out. A host that includes vmlib.h with -Werror
# builds whatever of it these instantiate, so a warning here is that host's
# broken build. Directory options reach this project's own targets only:
# not vmlib's INTERFACE, which carries no options into a host, and not
# peglib.h, which the front ends include as a system header -- a warning
# there is upstream's to fix.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Werror)
endif()

option(VMLIB_BUILD_TESTS "Build cpp-vmlib tests" ON)
option(VMLIB_BUILD_EXAMPLES "Build the example front ends" OFF)
option(VMLIB_BUILD_BENCH "Build the benchmark harness" ON)
# Each front end's goldens test (front_end_tests, examples/common): its
# samples run through the real language rather than the front end, to
# check the goldens themselves. Off by default, since it needs every
# language installed at the version test/languages.sh pins; CI's goldens
# job turns it on (test/goldens.sh), and builds nothing for it.
option(VMLIB_CHECK_GOLDENS "Register the goldens tests (label: goldens)" OFF)

# Unconditional, and up here before any add_subdirectory: a subdirectory's
# own add_test only reaches `ctest` run from the top build directory if the
# *top-level* CTestTestfile.cmake was generated, which needs this to have
# already run by the time that subdirectory is added -- calling it again
# inside `if(VMLIB_BUILD_TESTS)` below would be too late for
# examples/mini-go's own test to be discovered when VMLIB_BUILD_EXAMPLES is
# on but VMLIB_BUILD_TESTS is off. Enabling testing adds no tests by itself.
enable_testing()

# The PL/0 front end is an example, and also the test suite's oracle
# harness: the sample and error transcripts run its binary. What every
# front end shares comes first, wherever PL/0 is built: examples/common,
# the command-line driver and add_front_end(), which registers one.
if(VMLIB_BUILD_EXAMPLES OR VMLIB_BUILD_TESTS)
  add_subdirectory(examples/common)
  add_subdirectory(examples/pl0)
endif()

# The other front ends are examples alone: none plays a role in the main
# suite the way PL/0 does, so they build only with VMLIB_BUILD_EXAMPLES --
# and their own checks (add_front_end) register with ctest regardless of
# VMLIB_BUILD_TESTS, as any add_test does once testing is enabled. What
# each one proves is said at the top of its own CMakeLists.txt.
if(VMLIB_BUILD_EXAMPLES)
  foreach(front_end mini-go mini-js mini-culebra mini-lua mini-python
                    mini-scheme mini-ruby mini-csharp)
    add_subdirectory(examples/${front_end})
  endforeach()
endif()

if(VMLIB_BUILD_TESTS)
  add_subdirectory(test)
endif()

if(VMLIB_BUILD_BENCH)
  add_subdirectory(bench)
endif()

install(FILES vmlib.h DESTINATION include)
