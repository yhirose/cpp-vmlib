// SPIKE (Phase 0b) -- does a tagged, reference-counted Value hold up?
//
// Two questions, and the second is the one worth the spike:
//
//   1. Do strings flow through the IR at all -- literal, concatenation,
//      assignment, print -- and does the count of live heap objects return to
//      zero when the program ends?
//
//   2. Does a *failed* program leak? This is where the design could have gone
//      wrong quietly. Today's executor is exception-safe for free because a
//      Frame holds only plain vectors of int64; once registers hold owned
//      references that stops being free. The bet coreir/value.h makes is that
//      putting the ownership in the type means the ordinary C++ unwind out of
//      vm::run releases everything, with no unwind table for the compiler to
//      emit and get wrong. If that bet is wrong, it is wrong here.
//
// Every case asserts g_live_heap_objects == 0 afterward, so a missing release
// fails the test rather than waiting for a leak checker to notice.

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "coreir/ir.h"
#include "coreir/value.h"
#include "vm/compiler.h"
#include "vm/exec.h"

namespace {

struct Failure : std::runtime_error {
  Failure(std::string msg) : std::runtime_error(std::move(msg)) {}
};

std::vector<std::string> g_out;
int g_failures = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

void check_eq(const std::string& got, const std::string& want,
              const std::string& what) {
  if (got != want) {
    std::fprintf(stderr, "FAIL: %s\n  want [%s]\n  got  [%s]\n", what.c_str(),
                 want.c_str(), got.c_str());
    ++g_failures;
  }
}

std::string joined() {
  std::string s;
  for (const auto& line : g_out) {
    s += line;
    s += "|";
  }
  return s;
}

// Runs a module built by `build`, returning the host failure message if the
// program failed and an empty string if it ran to completion. Asserts on the
// way out that nothing was left on the heap either way.
std::string run_module(const coreir::Module& m, const std::string& what) {
  g_out.clear();
  if (auto err = coreir::verify(m)) {
    std::fprintf(stderr, "FAIL: %s: malformed IR: %s\n", what.c_str(),
                 err->c_str());
    ++g_failures;
    return {};
  }
  std::string failure;
  {
    const vm::Program p = vm::compile(m);
    try {
      vm::run(p);
    } catch (const Failure& e) {
      failure = e.what();
    }
  }
  check(coreir::g_live_heap_objects == 0,
        what + ": leaked " + std::to_string(coreir::g_live_heap_objects) +
            " heap object(s)");
  return failure;
}

}  // namespace

extern "C" {
void coreir_rt_out(int64_t v) { g_out.push_back(std::to_string(v)); }
void coreir_rt_out_str(const char* bytes, int64_t len) {
  g_out.emplace_back(bytes, static_cast<size_t>(len));
}
int64_t coreir_rt_in(int64_t, int64_t) { return 0; }
void coreir_rt_poll(void) {}
[[noreturn]] void coreir_rt_fail(const char* msg, int64_t, int64_t) {
  throw Failure(msg);
}
}

int main() {
  using namespace coreir;
  const SrcPos p{1, 1};

  // --- 1. Strings: literal, concatenation, print. -------------------------
  {
    Module m;
    Builder b(m);
    const NodeId greeting =
        b.binary(BinOp::Add,
                 b.binary(BinOp::Add, b.str_literal("hello", p),
                          b.str_literal(" ", p), p),
                 b.str_literal("world", p), p);
    m.funcs.push_back(
        {"main", 0, 0, b.intrinsic(IntrinsicId::Print, {greeting}, p), {}, {}});
    const std::string failed = run_module(m, "concat");
    check_eq(failed, "", "concat: unexpected failure");
    check_eq(joined(), "hello world|", "concat output");
  }

  // --- 2. A string through a variable, overwritten. -----------------------
  // The second store has to release what the slot held, or the first string
  // outlives the program.
  {
    Module m;
    Builder b(m);
    const NodeId body = b.block(
        {b.assign(VarKind::Local, 0, b.str_literal("first", p), p),
         b.intrinsic(IntrinsicId::Print, {b.varref(VarKind::Local, 0, p)}, p),
         b.assign(VarKind::Local, 0,
                  b.binary(BinOp::Add, b.varref(VarKind::Local, 0, p),
                           b.str_literal("+second", p), p),
                  p),
         b.intrinsic(IntrinsicId::Print, {b.varref(VarKind::Local, 0, p)}, p)},
        p);
    m.funcs.push_back({"main", 1, 0, body, {"s"}, {}});
    const std::string failed = run_module(m, "reassign");
    check_eq(failed, "", "reassign: unexpected failure");
    check_eq(joined(), "first|first+second|", "reassign output");
  }

  // --- 3. THE ONE THAT MATTERS: a throw with live strings. ----------------
  // A local holds a string, a register holds a freshly concatenated one, and
  // then a divide by zero fires two frames deep. Nothing runs a destructor
  // for those on purpose; the unwind out of vm::run has to.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({{VarKind::Local, 0}});  // main -> inner

    const NodeId main_body = b.block(
        {b.assign(VarKind::Local, 0, b.str_literal("held by main", p), p),
         b.call(1, 0, p)},
        p);

    // inner: t = capture[0] + " and by inner"; print(1 / 0)
    const NodeId inner_body = b.block(
        {b.assign(VarKind::Local, 0,
                  b.binary(BinOp::Add, b.varref(VarKind::Capture, 0, p),
                           b.str_literal(" and by inner", p), p),
                  p),
         b.intrinsic(
             IntrinsicId::Print,
             {b.binary(BinOp::Div, b.literal(1, p), b.literal(0, p), p)}, p)},
        p);

    m.funcs.push_back({"main", 1, 0, main_body, {"s"}, {}});
    m.funcs.push_back({"inner", 1, 1, inner_body, {"t"}, {"s"}});
    const std::string failed = run_module(m, "throw with live strings");
    check_eq(failed, "divide by zero", "throw: message");
    check_eq(joined(), "", "throw: nothing printed");
  }

  // --- 4. Type errors, which an i64-only IR could not have. ---------------
  {
    Module m;
    Builder b(m);
    m.funcs.push_back({"main",
                       0,
                       0,
                       b.intrinsic(IntrinsicId::Print,
                                   {b.binary(BinOp::Sub,
                                             b.str_literal("a", p),
                                             b.str_literal("b", p), p)},
                                   p),
                       {},
                       {}});
    const std::string failed = run_module(m, "str minus str");
    check_eq(failed, "cannot sub string and string", "type error: message");
  }
  {
    Module m;
    Builder b(m);
    m.funcs.push_back({"main",
                       0,
                       0,
                       b.intrinsic(IntrinsicId::Print,
                                   {b.binary(BinOp::Add, b.literal(1, p),
                                             b.str_literal("x", p), p)},
                                   p),
                       {},
                       {}});
    const std::string failed = run_module(m, "int plus str");
    check_eq(failed, "cannot add int and string", "mixed type error: message");
  }

  // --- 5. Strings compare; truthiness works on them. ----------------------
  {
    Module m;
    Builder b(m);
    const NodeId body = b.block(
        {b.make_if(b.binary(BinOp::Eq, b.str_literal("x", p),
                            b.str_literal("x", p), p),
                   b.intrinsic(IntrinsicId::Print,
                               {b.str_literal("equal", p)}, p),
                   b.intrinsic(IntrinsicId::Print,
                               {b.str_literal("differ", p)}, p),
                   p),
         b.make_if(b.str_literal("truthy", p),
                   b.intrinsic(IntrinsicId::Print, {b.literal(1, p)}, p),
                   NodeId{}, p)},
        p);
    m.funcs.push_back({"main", 0, 0, body, {}, {}});
    const std::string failed = run_module(m, "compare");
    check_eq(failed, "", "compare: unexpected failure");
    check_eq(joined(), "equal|1|", "compare output");
  }

  // --- 6. A string built in a loop: many allocations, all released. -------
  {
    Module m;
    Builder b(m);
    const NodeId s = b.varref(VarKind::Local, 0, p);
    const NodeId i = b.varref(VarKind::Local, 1, p);
    const NodeId body = b.block(
        {b.assign(VarKind::Local, 0, b.str_literal("", p), p),
         b.assign(VarKind::Local, 1, b.literal(0, p), p),
         b.make_while(
             b.binary(BinOp::Lt, i, b.literal(1000, p), p),
             b.block({b.assign(VarKind::Local, 0,
                               b.binary(BinOp::Add, s, b.str_literal("x", p),
                                        p),
                               p),
                      b.assign(VarKind::Local, 1,
                               b.binary(BinOp::Add, i, b.literal(1, p), p), p)},
                     p),
             p),
         b.intrinsic(IntrinsicId::Print, {b.literal(0, p)}, p)},
        p);
    m.funcs.push_back({"main", 2, 0, body, {"s", "i"}, {}});
    const std::string failed = run_module(m, "loop");
    check_eq(failed, "", "loop: unexpected failure");
    check_eq(joined(), "0|", "loop output");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "spike_values: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("spike_values OK\n");
  return 0;
}
