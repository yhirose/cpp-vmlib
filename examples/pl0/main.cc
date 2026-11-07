#include "binder.h"
#include "driver.h"

int main(int argc, char** argv) {
  return driver::main(argc, argv, {"pl0", "PROGRAM.pas", pl0::bind_source});
}
