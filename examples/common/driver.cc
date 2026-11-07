#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
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
            << " [--dump-ir] [--dump-bc] [--check-effects] [--profile] "
            << fe.program;
  if (fe.concatenate) std::cerr << " [" << fe.program << " ...]";
  std::cerr << "\n";
}

}  // namespace

int main(int argc, char** argv, const FrontEnd& fe) {
  std::vector<std::string> paths;
  bool dump_ir = false, dump_bc = false, check_effects = false;
  bool profile = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--dump-ir") {
      dump_ir = true;
    } else if (a == "--check-effects") {
      check_effects = true;
    } else if (a == "--dump-bc") {
      dump_bc = true;
    } else if (a == "--profile") {
      profile = true;
    } else if (a.rfind("--", 0) == 0) {
      usage(fe);
      return 2;
    } else {
      paths.push_back(a);
    }
  }
  if (paths.empty() || (paths.size() > 1 && !fe.concatenate)) {
    usage(fe);
    return 2;
  }

  // Read as text rather than streamed into an ostringstream: an empty
  // file inserts nothing, which sets the stream's failbit and would drop
  // every file after it.
  std::string source;
  for (const std::string& path : paths) {
    std::ifstream in(path);
    if (!in) {
      std::cerr << fe.name << ": cannot open " << path << "\n";
      return 2;
    }
    source.append(std::istreambuf_iterator<char>(in),
                  std::istreambuf_iterator<char>());
    if (fe.concatenate) source += '\n';
  }

  // The last file names the program: in every position the stdio host
  // reports, and to a binder that takes the path itself.
  const std::string& path = paths.back();
  coreir_rt_default::set_path(path);
  const coreir::Module m = fe.bind(source, path);

  if (dump_ir) {
    std::cout << coreir::to_string(m);
    return 0;
  }

  // Whether anything the program could observe -- a call, a store, a fresh
  // object -- is reached twice on one path, and so would run twice
  // (coreir::shared_effects), or was built and is reached by no function
  // body, and so would never run (coreir::dropped_effects): what
  // test/run_effects.cmake asks of every program a front end ships.
  if (check_effects) {
    size_t found = 0;
    const auto report = [&](const auto& effects) {
      for (const auto& e : effects) {
        std::cerr << path << ":" << coreir::to_string(m, e) << "\n";
      }
      found += effects.size();
    };
    report(coreir::shared_effects(m));
    report(coreir::dropped_effects(m));
    return found == 0 ? 0 : 1;
  }

  const vm::Program p = vm::compile(m);
  if (dump_bc) {
    std::cout << vm::to_string(p);
    return 0;
  }

  coreir::Runtime rt;
  if (!profile) {
    vm::run(p, rt, fe.run);
    return 0;
  }
  vm::Profile counts;
  vm::run(p, rt, fe.run, counts);
  // After everything the program printed, wherever the two streams go.
  std::fflush(stdout);
  std::cerr << vm::to_string(p, counts);
  return 0;
}

}  // namespace driver
