# The only target that knows what a PEG is. peglib.h lives under this
# directory and is on nobody else's include path.
add_executable(pl0 main.cc binder.cc)
target_include_directories(pl0 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                       ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
target_link_libraries(pl0 PRIVATE coreir interp vm pl0rt)

# The JIT resolves pl0_rt_out / pl0_rt_in / pl0_rt_fail out of this process, so
# they have to be in the dynamic symbol table.
set_target_properties(pl0 PROPERTIES ENABLE_EXPORTS ON)

if(PL0_ENABLE_LLVM)
  target_link_libraries(pl0 PRIVATE llvmgen)
  target_compile_definitions(pl0 PRIVATE PL0_ENABLE_LLVM=1)
else()
  target_compile_definitions(pl0 PRIVATE PL0_ENABLE_LLVM=0)
endif()

find_package(Threads REQUIRED)
target_link_libraries(pl0 PRIVATE Threads::Threads)
