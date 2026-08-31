#pragma once

#include "coreir/ir.h"
#include "vm/bytecode.h"

namespace vm {

Program compile(const coreir::Module& m);

}  // namespace vm
