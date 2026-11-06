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
  std::cerr << "usage: mini-lua [--dump-ir] [--dump-bc] [--check-sharing] PROGRAM.lua\n";
}

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
    std::cerr << "mini-lua: cannot open " << path << "\n";
    return 2;
  }
  std::ostringstream ss;
  ss << in.rdbuf();

  coreir_rt_default::set_path(path);
  const std::string source = ss.str();
  const coreir::Module m = mini_lua::bind_source(source, path);

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

  vm::RunOptions opts;
  opts.natives = mini_lua::stdlib();
  opts.describe = mini_lua::describe;
  // Left at its default: a tail call reuses the frame, so a runaway
  // recursion that is *not* in tail position still fails cleanly here
  // rather than growing the heap -- which is what samples/tailcalls.lua
  // shows from both sides.

  coreir::Runtime rt;
  vm::run(p, rt, opts);
  return 0;
}
