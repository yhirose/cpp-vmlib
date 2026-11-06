#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

// The stdio host (coreir_rt_*) is part of vmlib.h; this is the one
// translation unit that asks for it.
#define VMLIB_DEFAULT_RUNTIME
#include "vmlib.h"

#include "binder.h"

namespace {

void usage() { std::cerr << "usage: pl0 [--dump-ir] [--dump-bc] [--check-sharing] PROGRAM.pas\n"; }

}  // namespace

int main(int argc, char** argv) {
  std::string path;
  bool dump_ir = false, dump_bc = false, check_sharing = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--dump-ir") {
      dump_ir = true;
    } else if (a == "--check-sharing") {
      check_sharing = true;
    } else if (a == "--dump-bc") {
      dump_bc = true;
    } else if (a.rfind("--", 0) == 0) {
      usage();
      return 2;
    } else {
      path = a;
    }
  }
  if (path.empty()) {
    usage();
    return 2;
  }

  std::ifstream in(path);
  if (!in) {
    std::cerr << "pl0: cannot open " << path << "\n";
    return 2;
  }
  std::ostringstream ss;
  ss << in.rdbuf();

  coreir_rt_default::set_path(path);
  const coreir::Module m = pl0::bind_source(ss.str());

  if (dump_ir) {
    std::cout << coreir::to_string(m);
    return 0;
  }

  // Whether anything the program could observe -- a call, a store, a fresh
  // object -- is reached twice on one path, and so would run twice
  // (coreir::shared_effects): what test/run_sharing.cmake asks of every
  // program this front end ships.
  if (check_sharing) {
    const auto found = coreir::shared_effects(m);
    for (const auto& e : found) {
      std::cerr << path << ":" << coreir::to_string(m, e) << "\n";
    }
    return found.empty() ? 0 : 1;
  }

  const vm::Program p = vm::compile(m);
  if (dump_bc) {
    std::cout << vm::to_string(p);
    return 0;
  }

  vm::run(p);
  return 0;
}
