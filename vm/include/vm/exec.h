#pragma once

#include "coreir/value.h"
#include "vm/bytecode.h"

namespace vm {

struct RunOptions {
  // Calls do not recurse through this thread's C++ stack -- the executor
  // keeps its own stack of heap-allocated frames -- so the bound is on how
  // many of those may be live at once, not on how much machine stack they
  // would need. A runaway program still fails cleanly, at its call site,
  // rather than growing the heap until the allocator gives out.
  int max_call_depth = 10000;
  // Whether the entry frame's own values run their drop hooks when the
  // program ends -- at its Ret, or as an uncaught throw unwinds past it.
  // Off, they are released with the hook disarmed: the memory goes, the
  // destructors do not run. That is culebra's rule for top-level bindings
  // (only top-level defers run at exit), and a front end wanting both under
  // this option gives the entry frame a Scope over an empty local range,
  // [0, 0): its defers still run at its exit, while the bindings stay the
  // frame's to release, destructor-free.
  bool entry_frame_drops = true;
};

// Run the bytecode, on a heap the caller owns. Use this to inspect what a
// program left behind: a Runtime frees its remaining objects when it is
// destroyed -- which is how a reference cycle finally goes away -- so a
// count taken after vm::run and before ~Runtime is the one that shows a
// leak.
void run(const Program& p, coreir::Runtime& rt, const RunOptions& opts);

// The same with default options but for the depth bound, and on a heap of
// the run's own.
void run(const Program& p, coreir::Runtime& rt, int max_call_depth = 10000);
void run(const Program& p, int max_call_depth = 10000);

}  // namespace vm
