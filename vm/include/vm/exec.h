#pragma once

#include "coreir/value.h"
#include "vm/bytecode.h"

namespace vm {

// Run the bytecode. Calls do not recurse through this thread's C++ stack --
// the executor keeps its own stack of heap-allocated frames -- so the bound
// is on how many of those may be live at once, not on how much machine stack
// they would need. A runaway program still fails cleanly, at its call site,
// rather than growing the heap until the allocator gives out.
void run(const Program& p, int max_call_depth = 10000);

// The same, on a heap the caller owns. Use this to inspect what a program
// left behind: a Runtime frees its remaining objects when it is destroyed --
// which is how a reference cycle finally goes away -- so a count taken after
// vm::run and before ~Runtime is the one that shows a leak.
void run(const Program& p, coreir::Runtime& rt, int max_call_depth = 10000);

}  // namespace vm
