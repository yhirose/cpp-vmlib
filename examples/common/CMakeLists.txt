# What every front end under examples/ shares: the command line (driver.h).
#
# One library, built once. It is the translation unit of each front end
# that takes the stdio host (VMLIB_DEFAULT_RUNTIME), and the one that
# instantiates the compiler and the executor -- so a front end's main.cc
# is a few lines of what is its own, and the executor is compiled once
# rather than once per front end.
add_library(front-end-driver STATIC driver.cc)
target_include_directories(front-end-driver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(front-end-driver PUBLIC vmlib)
