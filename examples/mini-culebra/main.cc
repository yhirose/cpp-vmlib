#include "binder.h"
#include "driver.h"

int main(int argc, char** argv) {
  driver::FrontEnd fe("mini-culebra", "PROGRAM.cul",
                      mini_culebra::bind_source);
  fe.run.natives = mini_culebra::stdlib();
  fe.run.describe = mini_culebra::describe;
  fe.run.describe_traps = true;
  // culebra's rule for a top-level binding: it lives to the end of the
  // program and is released without its destructor running -- only a
  // top-level `defer` runs at exit. RunOptions names this exact case, and
  // the binder does the other half (a [0, 0) Scope on the entry frame).
  fe.run.entry_frame_drops = false;
  return driver::main(argc, argv, fe);
}
