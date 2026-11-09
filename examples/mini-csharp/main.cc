#include "binder.h"
#include "driver.h"

int main(int argc, char** argv) {
  driver::FrontEnd fe("mini-csharp", "PROGRAM.cs", mini_csharp::bind_source);
  fe.run.natives = mini_csharp::stdlib();
  fe.run.describe = mini_csharp::describe;
  fe.run.describe_traps = true;
  return driver::main(argc, argv, fe);
}
