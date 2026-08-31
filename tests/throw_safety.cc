// Locks in the property include/coreir/rt.h documents but the standalone
// CLI never exercises: a host whose coreir_rt_fail throws instead of exiting
// must see vm/exec.cc unwind cleanly, releasing every live frame, rather than
// being coupled to process-exit semantics. The executor owns its frames on
// the heap, so what has to hold is that its own frame stack is destroyed as
// the throw leaves vm::run -- not that the C++ unwinder passes through one
// activation per frame, which it no longer does.
//
// Builds a tiny Core-IR program by hand (no PEG front end needed): a two-deep
// call divides by zero, so the failure fires with two frames live rather than
// one -- the case that would expose frames leaked or bookkeeping skipped by
// an unwind.

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include "coreir/ir.h"
#include "vm/compiler.h"
#include "vm/exec.h"

namespace {

struct Failure : std::runtime_error {
  int64_t line, col;
  Failure(std::string msg, int64_t line, int64_t col)
      : std::runtime_error(std::move(msg)), line(line), col(col) {}
};

coreir::Module build_program() {
  using namespace coreir;
  Module m;
  Builder b(m);
  const SrcPos pos{1, 1};

  // funcs[0] must be the entry point, so main is assigned index 0 and inner
  // index 1 up front -- inner's own body doesn't need to know either index,
  // and main's Call node can name inner's index (1) directly.
  m.capture_maps.push_back({});  // cmap 0: no captures, used by main's call

  const NodeId div =
      b.binary(BinOp::Div, b.literal(1, pos), b.literal(0, pos), pos);
  const NodeId inner_body = b.intrinsic(IntrinsicId::Print, {div}, pos);

  const NodeId call = b.call(1, 0, pos);  // func #1 (inner), cmap #0

  m.funcs.push_back({"main", 0, 0, call, {}, {}});
  m.funcs.push_back({"inner", 0, 0, inner_body, {}, {}});
  return m;
}

}  // namespace

extern "C" {

void coreir_rt_out(int64_t) {}
int64_t coreir_rt_in(int64_t, int64_t) { return 0; }
void coreir_rt_poll(void) {}

[[noreturn]] void coreir_rt_fail(const char* msg, int64_t line, int64_t col) {
  throw Failure(msg, line, col);
}

}  // extern "C"

int main() {
  const coreir::Module m = build_program();
  if (auto err = coreir::verify(m)) {
    std::fprintf(stderr, "throw_safety: malformed test IR: %s\n", err->c_str());
    return 1;
  }
  const vm::Program p = vm::compile(m);

  try {
    vm::run(p);
    std::fprintf(stderr, "throw_safety: expected a divide-by-zero throw\n");
    return 1;
  } catch (const Failure& e) {
    if (std::string(e.what()) != "divide by zero") {
      std::fprintf(stderr, "throw_safety: wrong message: %s\n", e.what());
      return 1;
    }
  }

  // The throw left vm::run with two frames live (main -> inner). Running a
  // second, independent program afterward guards against the regression this
  // test exists to prevent: today vm/exec.cc has no global or process-wide
  // state, so nothing carries over between runs by construction, but that is
  // an invariant worth re-checking mechanically if it is ever true.
  const coreir::Module m2 = build_program();
  const vm::Program p2 = vm::compile(m2);
  try {
    vm::run(p2);
    return 1;
  } catch (const Failure&) {
  }

  std::printf("throw_safety OK\n");
  return 0;
}
