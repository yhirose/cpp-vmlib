#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// The stdio host (coreir_rt_*) is part of vmlib.h; this is the one
// translation unit of each front end that asks for it, since the driver is
// what tells it the program's name (coreir_rt_default::set_path).
#define VMLIB_DEFAULT_RUNTIME
#include "vmlib.h"

#include "driver.h"

namespace driver {

coreir::Module Bind::operator()(const std::string& source,
                                const std::string& path) const {
  return source_and_path_ ? source_and_path_(source, path) : source_(source);
}

namespace {

void usage(const FrontEnd& fe) {
  std::cerr << "usage: " << fe.name
            << " [--dump-ir] [--dump-bc] [--check-sharing] " << fe.program;
  if (fe.concatenate) std::cerr << " [" << fe.program << " ...]";
  std::cerr << "\n";
}

}  // namespace

int main(int argc, char** argv, const FrontEnd& fe) {
  std::vector<std::string> paths;
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
      usage(fe);
      return 2;
    } else if (fe.concatenate || paths.empty()) {
      paths.push_back(a);
    } else {
      paths.back() = a;
    }
  }
  if (paths.empty()) {
    usage(fe);
    return 2;
  }

  std::ostringstream ss;
  for (const std::string& path : paths) {
    std::ifstream in(path);
    if (!in) {
      std::cerr << fe.name << ": cannot open " << path << "\n";
      return 2;
    }
    ss << in.rdbuf();
    if (fe.concatenate) ss << "\n";
  }

  // The last file names the program: in every position the stdio host
  // reports, and to a binder that takes the path itself.
  const std::string& path = paths.back();
  coreir_rt_default::set_path(path);
  const coreir::Module m = fe.bind(ss.str(), path);

  if (dump_ir) {
    std::cout << coreir::to_string(m);
    return 0;
  }

  // Whether anything the program could observe -- a call, a store, a fresh
  // object -- is reached twice on one path, and so would run twice
  // (coreir::shared_effects): what test/run_sharing.cmake asks of every
  // program a front end ships.
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

  coreir::Runtime rt;
  vm::run(p, rt, fe.run);
  return 0;
}

}  // namespace driver
