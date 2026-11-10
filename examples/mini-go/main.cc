#include "binder.h"
#include "driver.h"

int main(int argc, char** argv) {
  driver::FrontEnd fe("mini-go", "PROGRAM.go", mini_go::bind_source);
  fe.run.natives = mini_go::stdlib();
  fe.run.describe = mini_go::describe;
  fe.run.deadlock_text = [](int64_t) {
    return std::string("fatal error: all goroutines are asleep - deadlock!");
  };
  return driver::main(argc, argv, fe);
}
