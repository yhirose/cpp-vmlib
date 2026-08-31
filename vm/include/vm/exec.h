#pragma once

#include "vm/bytecode.h"

namespace vm {

// Run the bytecode. max_call_depth bounds the executor's own C++ recursion
// (one call() per procedure call) so a runaway program fails cleanly instead
// of overflowing whatever stack this call runs on -- the default assumes an
// 8 MB stack; a host running on a thread with a different budget should pass
// its own.
void run(const Program& p, int max_call_depth = 10000);

}  // namespace vm
