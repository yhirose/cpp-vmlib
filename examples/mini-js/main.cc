#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// The stdio host (coreir_rt_*) is part of vmlib.h; this is the one
// translation unit that asks for it.
#define VMLIB_DEFAULT_RUNTIME
#include "vmlib.h"

#include "binder.h"

namespace {

void usage() {
  std::cerr << "usage: mini-js [--dump-ir] [--dump-bc] FILE.js [FILE.js ...]\n";
}

}  // namespace

// More than one file is concatenated, in order, into one program -- which
// is what `node a.js b.js` does not do, but what makes the samples able to
// share one prelude (samples/prelude.js) without this subset needing a
// module system it has no other use for. The oracle side of the same
// comparison is `cat prelude.js sample.js > tmp.js && node tmp.js`; see
// samples/gen_golden.sh.
int main(int argc, char** argv) {
  std::vector<std::string> paths;
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
      paths.push_back(a);
    }
  }
  if (paths.empty()) {
    usage();
    return 2;
  }

  std::ostringstream ss;
  for (const std::string& path : paths) {
    std::ifstream in(path);
    if (!in) {
      std::cerr << "mini-js: cannot open " << path << "\n";
      return 2;
    }
    ss << in.rdbuf() << "\n";
  }

  coreir_rt_default::set_path(paths.back());
  const std::string source = ss.str();
  const coreir::Module m = mini_js::bind_source(source);

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
