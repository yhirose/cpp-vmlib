#include "binder.h"
#include "driver.h"

int main(int argc, char** argv) {
  return driver::main(argc, argv,
                      {"mini-go", "PROGRAM.go", mini_go::bind_source});
}
