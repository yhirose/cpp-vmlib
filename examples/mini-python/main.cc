#include "binder.h"
#include "driver.h"

int main(int argc, char** argv) {
  driver::FrontEnd fe("mini-python", "PROGRAM.py", mini_python::bind_source);
  fe.run.natives = mini_python::stdlib();
  fe.run.describe = mini_python::describe;
  fe.convert_trap = mini_python::convert_trap;
  return driver::main(argc, argv, fe);
}
