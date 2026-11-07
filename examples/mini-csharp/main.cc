#include "binder.h"
#include "driver.h"

int main(int argc, char** argv) {
  driver::FrontEnd fe("mini-csharp", "PROGRAM.cs", mini_csharp::bind_source);
  fe.run.natives = mini_csharp::stdlib();
  fe.run.describe = mini_csharp::describe;
  // max_call_depth is left at its default, so a runaway recursion fails
  // cleanly here rather than growing the heap.
  return driver::main(argc, argv, fe);
}
