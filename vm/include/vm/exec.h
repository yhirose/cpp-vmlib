#pragma once

#include "vm/bytecode.h"

namespace vm {

// Lane 2: run the bytecode.
void run(const Program& p);

}  // namespace vm
