cmake_minimum_required(VERSION 3.16)
project(cpp-vmlib CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# coreir -- the IR, and nothing else. Note what is absent from this target:
# any mention of PL/0, and any path that reaches peglib.h. A front end is a
# consumer of this library, never the other way round.
# ---------------------------------------------------------------------------
add_library(coreir STATIC include/coreir/ir.cc)
target_include_directories(coreir PUBLIC include)

add_library(vm STATIC vm/compiler.cc vm/exec.cc vm/bytecode.cc)
target_include_directories(vm PUBLIC vm/include)
target_link_libraries(vm PUBLIC coreir)

# The default host runtime (stdio): what a program gets by linking nothing
# else. A host embedding coreir/vm elsewhere (culebra's CodeGen namespace, for
# instance) supplies its own definitions of the four coreir_rt_* symbols
# instead of linking this -- see include/coreir/rt.h.
add_library(coreir_rt_default STATIC runtime/coreir_rt_default.cc)
target_include_directories(coreir_rt_default PUBLIC runtime)
target_link_libraries(coreir_rt_default PRIVATE coreir)

option(CPP_VMLIB_BUILD_EXAMPLES "Build the example front ends" ON)
if(CPP_VMLIB_BUILD_EXAMPLES)
  add_subdirectory(examples/pl0)
  enable_testing()
  add_subdirectory(tests)
endif()
