#include "binder.h"
#include "driver.h"

int main(int argc, char** argv) {
  driver::FrontEnd fe("mini-lua", "PROGRAM.lua", mini_lua::bind_source);
  fe.run.natives = mini_lua::stdlib();
  fe.run.describe = mini_lua::describe;
  fe.run.describe_traps = true;
  // max_call_depth is left at its default: a tail call reuses the frame, so
  // a runaway recursion that is *not* in tail position still fails cleanly
  // here rather than growing the heap -- which is what samples/tailcalls.lua
  // shows from both sides.
  return driver::main(argc, argv, fe);
}
