// The executor keeps its own stack of heap-allocated frames, so a call chain
// is bounded by max_call_depth and by heap, not by the machine stack of the
// thread vm::run happens to be on.
//
// This is worth a test rather than a comment because it was not always true,
// and the failure it replaced was silent. When each Core-IR call was a C++
// call, exec.h invited a host to raise the bound ("a host running on a thread
// with a different budget should pass its own") -- but raising it past what
// the machine stack could hold segfaulted instead of reporting anything. On
// an 8 MB stack that ceiling was around 30,000 frames, well under the kind of
// number a host would reach for. The default of 10,000 stayed below it by
// luck, so nothing in this repository could see the problem.
//
// Two cases, either of which would have crashed the old executor:
//   - 100,000 frames deep and back out again, returning normally
//   - past a bound set higher than the machine stack could ever hold, which
//     must still report "recursion limit exceeded" at the call site

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

#include "coreir/ir.h"
#include "vm/compiler.h"
#include "vm/exec.h"

namespace {

constexpr int64_t kDepth = 100000;

struct Failure : std::runtime_error {
  int64_t line, col;
  Failure(std::string msg, int64_t line, int64_t col)
      : std::runtime_error(std::move(msg)), line(line), col(col) {}
};

int64_t last_out = -1;
long out_count = 0;

// main: n = 0; CALL rec; print(n)     -- n forwarded as rec's capture[0]
// rec:  n = n + 1; IF n < limit THEN CALL rec
//
// The print in main runs only if the whole chain returned, so a frame stack
// that unwinds wrongly shows up as a missing or wrong value, not just as an
// absence of crashing.
coreir::Module build(int64_t limit) {
  using namespace coreir;
  Module m;
  Builder b(m);
  const SrcPos call_pos{7, 3};

  // n is shared with rec, so it is a cell of main rather than a local.
  m.capture_maps.push_back({{VarKind::Cell, 0}});     // cmap 0: main -> rec
  m.capture_maps.push_back({{VarKind::Capture, 0}});  // cmap 1: rec -> rec

  const SrcPos p{1, 1};
  const NodeId main_body =
      b.block({b.assign(VarKind::Cell, 0, b.literal(0, p), p),
               b.call_value(b.make_closure(1, 0, call_pos), {}, call_pos),
               b.intrinsic(IntrinsicId::Print,
                           {b.varref(VarKind::Cell, 0, p)}, p)},
              p);

  const NodeId n = b.varref(VarKind::Capture, 0, p);
  const NodeId rec_body = b.block(
      {b.assign(VarKind::Capture, 0,
                b.binary(BinOp::Add, n, b.literal(1, p), p), p),
       b.make_if(b.binary(BinOp::Lt, n, b.literal(limit, p), p),
                 b.call_value(b.make_closure(1, 1, call_pos), {}, call_pos),
                 NodeId{}, p)},
      p);

  Func main_fn{"main", 0, 0, main_body, {}, {}};
  main_fn.num_cells = 1;
  m.funcs.push_back(main_fn);
  m.funcs.push_back({"rec", 0, 1, rec_body, {}, {"n"}});
  return m;
}

bool compile_and_run(int64_t limit, int max_call_depth, vm::Program* out) {
  const coreir::Module m = build(limit);
  if (auto err = coreir::verify(m)) {
    std::fprintf(stderr, "deep_calls: malformed test IR: %s\n", err->c_str());
    std::exit(1);
  }
  *out = vm::compile(m);
  vm::run(*out, max_call_depth);
  return true;
}

}  // namespace

extern "C" {

void coreir_rt_out(int64_t v) {
  last_out = v;
  ++out_count;
}
void coreir_rt_out_str(const char*, int64_t) { ++out_count; }
void coreir_rt_out_raw(const char*, int64_t) {}
int64_t coreir_rt_in(int64_t, int64_t) { return 0; }
void coreir_rt_poll(void) {}

[[noreturn]] void coreir_rt_fail(const char* msg, int64_t line, int64_t col) {
  throw Failure(msg, line, col);
}

}  // extern "C"

int main() {
  // Deep, but within the bound: must return normally, all the way back out.
  {
    vm::Program p;
    try {
      compile_and_run(kDepth, kDepth + 10, &p);
    } catch (const Failure& e) {
      std::fprintf(stderr, "deep_calls: unexpected failure at depth %lld: %s\n",
                   static_cast<long long>(kDepth), e.what());
      return 1;
    }
    if (out_count != 1 || last_out != kDepth) {
      std::fprintf(stderr,
                   "deep_calls: expected one line of output, %lld; "
                   "got %ld line(s), last %lld\n",
                   static_cast<long long>(kDepth), out_count,
                   static_cast<long long>(last_out));
      return 1;
    }
  }

  // Unbounded recursion under a bound far past what a machine stack holds:
  // still a reported failure, at the call site, with nothing printed.
  {
    out_count = 0;
    vm::Program p;
    try {
      // A limit the counter cannot reach: the IF is always taken.
      compile_and_run(std::numeric_limits<int64_t>::max(), 250000, &p);
      std::fprintf(stderr, "deep_calls: expected a recursion-limit throw\n");
      return 1;
    } catch (const Failure& e) {
      if (std::string(e.what()) != "recursion limit exceeded") {
        std::fprintf(stderr, "deep_calls: wrong message: %s\n", e.what());
        return 1;
      }
      if (e.line != 7 || e.col != 3) {
        std::fprintf(stderr, "deep_calls: wrong position: %lld:%lld\n",
                     static_cast<long long>(e.line),
                     static_cast<long long>(e.col));
        return 1;
      }
    }
    if (out_count != 0) {
      std::fprintf(stderr, "deep_calls: printed %ld line(s) before failing\n",
                   out_count);
      return 1;
    }
  }

  std::printf("deep_calls OK\n");
  return 0;
}
