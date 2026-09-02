# The only target that knows what a PEG is. peglib.h lives under this
# directory and is on nobody else's include path.
add_executable(pl0 main.cc binder.cc)
target_include_directories(pl0 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                       ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
target_link_libraries(pl0 PRIVATE vmlib)
