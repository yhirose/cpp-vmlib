#pragma once

#include "coreir/ir.h"
#include "vm/bytecode.h"

namespace vm {

// The second independent consumer of Core-IR (interp is the first). Having two
// is what keeps the IR from quietly shaping itself around one backend.
Program compile(const coreir::Module& m);

}  // namespace vm
