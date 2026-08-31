#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "binder.h"
#include "coreir/ir.h"
#include "coreir_rt_default.h"
#include "vm/bytecode.h"
#include "vm/compiler.h"
#include "vm/exec.h"

namespace {

void usage() { std::cerr << "usage: pl0 [--dump-ir] [--dump-bc] PROGRAM.pas\n"; }

}  // namespace

int main(int argc, char** argv) {
  std::string path;
  bool dump_ir = false, dump_bc = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--dump-ir") {
      dump_ir = true;
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

  const vm::Program p = vm::compile(m);
  if (dump_bc) {
    std::cout << vm::to_string(p);
    return 0;
  }

  vm::run(p);
  return 0;
}
