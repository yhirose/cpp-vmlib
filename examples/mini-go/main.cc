#include "binder.h"
#include "driver.h"

int main(int argc, char** argv) {
  driver::FrontEnd fe("mini-go", "PROGRAM.go", mini_go::bind_source);
  fe.run.natives = mini_go::stdlib();
  return driver::main(argc, argv, fe);
}
