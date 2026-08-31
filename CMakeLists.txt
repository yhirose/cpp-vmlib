cmake_minimum_required(VERSION 3.16)
project(cpp-codegen CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PL0_ENABLE_LLVM "Build the LLVM lane" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# coreir -- the IR, and nothing else. Note what is absent from this target and
# the three below it: any mention of PL/0, and any path that reaches peglib.h.
# A front end is a consumer of these libraries, never the other way round.
# ---------------------------------------------------------------------------
add_library(coreir STATIC include/coreir/ir.cc)
target_include_directories(coreir PUBLIC include)

# The shared runtime: one printer, one reader, one error formatter for all
# three lanes. This is the reason output and diagnostics cannot drift apart.
add_library(pl0rt STATIC runtime/pl0rt.cc)
target_include_directories(pl0rt PUBLIC runtime)

add_library(interp STATIC interp/interp.cc)
target_include_directories(interp PUBLIC interp/include)
target_link_libraries(interp PUBLIC coreir PRIVATE pl0rt)

add_library(vm STATIC vm/compiler.cc vm/exec.cc vm/bytecode.cc)
target_include_directories(vm PUBLIC vm/include)
target_link_libraries(vm PUBLIC coreir PRIVATE pl0rt)

if(PL0_ENABLE_LLVM)
  find_package(LLVM REQUIRED CONFIG)
  message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
  llvm_map_components_to_libnames(llvm_libs core orcjit support native)

  add_library(llvmgen STATIC llvmgen/codegen.cc)
  target_include_directories(llvmgen PUBLIC llvmgen/include)
  target_include_directories(llvmgen SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  separate_arguments(_llvm_defs NATIVE_COMMAND "${LLVM_DEFINITIONS}")
  target_compile_definitions(llvmgen PRIVATE ${_llvm_defs})
  target_link_libraries(llvmgen PUBLIC coreir vm PRIVATE ${llvm_libs})
  if(NOT LLVM_ENABLE_RTTI)
    target_compile_options(llvmgen PRIVATE -fno-rtti)
  endif()
endif()

add_subdirectory(examples/pl0)

enable_testing()
add_subdirectory(tests)
