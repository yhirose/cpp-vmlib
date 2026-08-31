// Lane 1: walk the Core-IR directly.
//
// This lane exists to keep the other two honest. exec and llvm both consume
// the bytecode, so a bug in the Core-IR -> bytecode compiler makes them agree
// on the wrong answer; only a consumer that reads the IR itself can catch
// that. It is a verification oracle, not a shipping engine -- culebra deleted
// its own tree-walker for 6 MB of driver, and this one is not meant to follow
// the Core-IR into anybody's product.

#pragma once

#include "coreir/ir.h"

namespace interp {

void run(const coreir::Module& m);

}  // namespace interp
