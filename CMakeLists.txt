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
# examples/mini-go's own test to be discovered when VMLIB_BUILD_EXAMPLES is
# on but VMLIB_BUILD_TESTS is off. Enabling testing adds no tests by itself.
enable_testing()

# The PL/0 front end is an example, and also the test suite's oracle
# harness: the sample and error transcripts run its binary.
if(VMLIB_BUILD_EXAMPLES OR VMLIB_BUILD_TESTS)
  add_subdirectory(examples/pl0)
endif()

# mini-go is a second example, proving the managed-language recipes
# (Fixed-width integers, `float`, Static calls, Struct fields, Switch)
# against a real `go run` rather than culebra's dynamic PL/0. It plays no
# role in the main test suite the way PL/0 does, so it stays behind
# VMLIB_BUILD_EXAMPLES alone --
# but its own sample check (examples/mini-go/CMakeLists.txt) still
# registers with ctest regardless of VMLIB_BUILD_TESTS, same as any other
# add_test once testing is enabled.
if(VMLIB_BUILD_EXAMPLES)
  add_subdirectory(examples/mini-go)
endif()

# mini-js is the dynamic-language counterpart to mini-go: where that one
# proves the managed, statically-typed recipes against `go run`, this one
# proves Closures, Exceptions, Generators, Maps and Coroutines + Scheduler
# (async/await over a Promise it writes itself) against `node`. Same
# arrangement as mini-go -- behind VMLIB_BUILD_EXAMPLES, with its own
# add_test that registers regardless of VMLIB_BUILD_TESTS.
if(VMLIB_BUILD_EXAMPLES)
  add_subdirectory(examples/mini-js)
endif()

# mini-culebra is the front end whose oracle is culebra itself. It is the
# only one that exercises the mechanisms vmlib.h describes in culebra's own
# terms -- the owned stack, a Scope's explicit release order and
# RunOptions::entry_frame_drops -- and the only one that uses the Host
# functions recipe (its standard library is vm::RunOptions::natives).
if(VMLIB_BUILD_EXAMPLES)
  add_subdirectory(examples/mini-culebra)
endif()

# mini-lua is the only front end that proves Tail calls -- Lua guarantees
# them, so `lua` is an oracle for the property rather than an
# implementation that happens to agree -- and the only one that hands
# coroutines to the program the way CoroCreate/CoroResume/CoroYield were
# named for.
if(VMLIB_BUILD_EXAMPLES)
  add_subdirectory(examples/mini-lua)
endif()

# mini-python is the only front end that proves Arbitrary-precision
# integers -- Python's `int` is unbounded, so python3 is an oracle for a
# bignum this library deliberately has no Value tag for -- and the only one
# whose lexical structure a PEG cannot express, so it normalizes the
# indentation before parsing.
if(VMLIB_BUILD_EXAMPLES)
  add_subdirectory(examples/mini-python)
endif()

# mini-scheme is the front end that shows a *boundary* rather than a
# recipe: the escape half of call/cc is a TryCatch and works, and the
# re-entrant half is the "multi-shot continuations" entry in this README's
# own list of what stays out of reach.
if(VMLIB_BUILD_EXAMPLES)
  add_subdirectory(examples/mini-scheme)
endif()

# mini-ruby is the only front end that uses FnArity and ArgCount: Ruby
# makes both program-visible, as `Proc#arity` and as the proc/lambda
# distinction (lenient_arity plus one ArgCount test).
if(VMLIB_BUILD_EXAMPLES)
  add_subdirectory(examples/mini-ruby)
endif()

# mini-csharp is the only front end with classes and inheritance, and the
# only statically-typed one -- which is the point: it parses types, erases
# them, and then wants nothing the runtime does not already have. Its
# oracle is `dotnet`.
if(VMLIB_BUILD_EXAMPLES)
  add_subdirectory(examples/mini-csharp)
endif()

if(VMLIB_BUILD_TESTS)
  add_subdirectory(test)
endif()

if(VMLIB_BUILD_BENCH)
  add_subdirectory(bench)
endif()

install(FILES vmlib.h DESTINATION include)
