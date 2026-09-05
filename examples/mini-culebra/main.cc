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

void usage() {
  std::cerr << "usage: mini-culebra [--dump-ir] [--dump-bc] PROGRAM.cul\n";
}

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
    std::cerr << "mini-culebra: cannot open " << path << "\n";
    return 2;
  }
  std::ostringstream ss;
  ss << in.rdbuf();

  coreir_rt_default::set_path(path);
  const std::string source = ss.str();
  const coreir::Module m = mini_culebra::bind_source(source);

  if (dump_ir) {
    std::cout << coreir::to_string(m);
    return 0;
  }

  const vm::Program p = vm::compile(m);
  if (dump_bc) {
    std::cout << vm::to_string(p);
    return 0;
  }

  vm::RunOptions opts;
  opts.natives = mini_culebra::stdlib();
  // culebra's rule for a top-level binding: it lives to the end of the
  // program and is released without its destructor running -- only a
  // top-level `defer` runs at exit. RunOptions names this exact case, and
  // the binder does the other half (a [0, 0) Scope on the entry frame).
  opts.entry_frame_drops = false;

  coreir::Runtime rt;
  vm::run(p, rt, opts);
  return 0;
}
