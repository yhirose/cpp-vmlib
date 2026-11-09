#include "binder.h"
#include "driver.h"

int main(int argc, char** argv) {
  driver::FrontEnd fe("mini-ruby", "PROGRAM.rb", mini_ruby::bind_source);
  fe.run.natives = mini_ruby::stdlib();
  fe.run.describe = mini_ruby::describe;
  fe.convert_trap = mini_ruby::convert_trap;
  return driver::main(argc, argv, fe);
}
