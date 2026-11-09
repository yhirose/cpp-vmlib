#include "binder.h"
#include "driver.h"

int main(int argc, char** argv) {
  driver::FrontEnd fe("mini-scheme", "PROGRAM.scm", mini_scheme::bind_source);
  fe.run.natives = mini_scheme::stdlib();
  fe.string_throwers = {"*"};  // its errors are strings it words itself
  fe.run.describe = mini_scheme::describe;
  // max_call_depth is left at its default: a tail call reuses the frame, so
  // a runaway recursion that is *not* in tail position still fails cleanly
  // here rather than growing the heap -- which is what samples/tailcalls.scm
  // shows from both sides.
  return driver::main(argc, argv, fe);
}
