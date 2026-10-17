# A target that knows what a PEG is, as every example front end does.
# peglib.h lives under this directory; the other front ends reach it from
# here, and vmlib itself never does.
add_executable(pl0 main.cc binder.cc)
target_include_directories(pl0 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                       ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
target_link_libraries(pl0 PRIVATE vmlib)
