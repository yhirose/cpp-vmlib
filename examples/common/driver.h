#pragma once

// The command line every front end under examples/ shares:
//
//   <name> [--dump-ir] [--dump-bc] [--check-effects] [--profile] PROGRAM
//
// It reads the program, binds it, compiles it and runs it -- or stops at
// the bound module (--dump-ir), at the compiled program (--dump-bc), or at
// the list of what one run would evaluate twice and what was built and
// dropped (--check-effects). With --profile the run is counted (vm::run's
// Profile overload), and once the program has finished, where its
// instructions went -- function by function, helpers included -- is
// printed to stderr as vm::to_string's table; a program that fails prints
// its failure and no table. It exits 0 when the program ran (or a dump was
// printed), 1 when the program failed or --check-effects found something,
// and 2 when the command line names no program it can read.
//
// A front end's main.cc says only what is its own -- its name, its binder,
// what its programs run with and why -- and hands the rest to
// driver::main.

#include <string>
#include <utility>

#include "vmlib.h"

namespace driver {

// A binder's entry point. Most take the source text alone; mini-lua's also
// takes the program's path, the chunk name Lua's own messages give.
class Bind {
 public:
  using Source = coreir::Module (*)(const std::string& source);
  using SourceAndPath = coreir::Module (*)(const std::string& source,
                                           const std::string& path);

  Bind(Source f) : source_(f) {}
  Bind(SourceAndPath f) : source_and_path_(f) {}

  coreir::Module operator()(const std::string& source,
                            const std::string& path) const;

 private:
  Source source_ = nullptr;
  SourceAndPath source_and_path_ = nullptr;
};

struct FrontEnd {
  FrontEnd(std::string name, std::string program, Bind bind)
      : name(std::move(name)), program(std::move(program)), bind(bind) {}

  // As the usage line and the error messages spell them: "mini-lua",
  // "PROGRAM.lua".
  std::string name;
  std::string program;
  Bind bind;
  // What the program runs with. Whatever a front end leaves alone is
  // vm::run's default.
  vm::RunOptions run;
  // Whether several files make one program: they are concatenated in
  // order, each followed by a line break, and the last one names the
  // program. Without it a program is one file, and a second is a usage
  // error.
  bool concatenate = false;
};

// The whole of a front end's main, once the front end has said what is its
// own.
int main(int argc, char** argv, const FrontEnd& fe);

}  // namespace driver
