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

# The PL/0 front end is an example, and also the test suite's oracle
# harness: the sample and error transcripts run its binary.
if(VMLIB_BUILD_EXAMPLES OR VMLIB_BUILD_TESTS)
  add_subdirectory(example/pl0)
endif()

if(VMLIB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()

install(FILES vmlib.h DESTINATION include)
