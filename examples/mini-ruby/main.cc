#include "binder.h"
#include "driver.h"

int main(int argc, char** argv) {
  driver::FrontEnd fe("mini-ruby", "PROGRAM.rb", mini_ruby::bind_source);
  fe.run.natives = mini_ruby::stdlib();
  fe.run.describe = mini_ruby::describe;
  // max_call_depth is left at its default, so a runaway recursion fails
  // cleanly here rather than growing the heap.
  return driver::main(argc, argv, fe);
}
