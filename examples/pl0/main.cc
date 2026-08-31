#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "binder.h"
#include "coreir/ir.h"
#include "interp/interp.h"
#include "pl0rt.h"
#include "vm/bytecode.h"
#include "vm/compiler.h"
#include "vm/exec.h"

#if PL0_ENABLE_LLVM
#include "llvmgen/codegen.h"
#endif

namespace {

void usage() {
  std::cerr << "usage: pl0 [--engine=interp|vm|llvm] [--dump-ir] [--dump-bc]"
               " [--emit-ir] PROGRAM.pas\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string engine = "vm";
  std::string path;
  bool dump_ir = false, dump_bc = false, emit_ir = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind("--engine=", 0) == 0) {
      engine = a.substr(9);
    } else if (a == "--dump-ir") {
      dump_ir = true;
    } else if (a == "--dump-bc") {
      dump_bc = true;
    } else if (a == "--emit-ir") {
      emit_ir = true;
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

  pl0rt::set_path(path);
  const coreir::Module m = pl0::bind_source(ss.str());

  if (dump_ir) {
    std::cout << coreir::to_string(m);
    return 0;
  }

  if (engine == "interp") {
    if (dump_bc || emit_ir) {
      std::cerr << "pl0: --dump-bc/--emit-ir need a bytecode engine\n";
      return 2;
    }
    interp::run(m);
    return 0;
  }

  const vm::Program p = vm::compile(m);
  if (dump_bc) {
    std::cout << vm::to_string(p);
    return 0;
  }

  if (engine == "vm") {
    if (emit_ir) {
      std::cerr << "pl0: --emit-ir needs --engine=llvm\n";
      return 2;
    }
    vm::run(p);
    return 0;
  }

  if (engine == "llvm") {
#if PL0_ENABLE_LLVM
    if (emit_ir) {
      std::cout << llvmgen::emit_ir(p);
      return 0;
    }
    llvmgen::run(p);
    return 0;
#else
    std::cerr << "pl0: built without the LLVM lane\n";
    return 2;
#endif
  }

  usage();
  return 2;
}
