// Lane 3: lower the bytecode to LLVM IR and run it under ORC.
//
// Note what this does *not* consume: the Core-IR tree. Taking the bytecode
// instead is the whole reason this lane and the executor cannot disagree about
// control flow -- there is one instruction stream and one set of jump targets,
// so basic-block construction here is label resolution rather than a second
// interpretation of `if` and `while`.

#pragma once

#include <string>

#include "vm/bytecode.h"

namespace llvmgen {

std::string emit_ir(const vm::Program& p);
void run(const vm::Program& p);

}  // namespace llvmgen
