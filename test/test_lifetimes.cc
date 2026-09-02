// *When* a value is released, not just whether.
//
// Reference counting was chosen over tracing for one reason: a language whose
// `drop` or `defer` has to run at a predictable moment cannot be built on a
// collector that runs whenever it likes. That reason only holds if the
// release actually happens where the source says it should, so this measures
// it rather than trusting it.
//
// The host reports the live heap-object count at each print, so a case reads
// as a sequence: how many objects were alive at each step of the program.
//
// One case here documents a limit rather than a guarantee: a local that is
// never reassigned lives until its frame returns, because Core-IR has no
// scope narrower than a function for it to die at the end of. A front end
// with block scopes needs the IR to carry them; until one exists, inventing
// the representation would be guessing at what it wants.

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "vmlib.h"

namespace {

struct Failure : std::runtime_error {
  Failure(std::string m) : std::runtime_error(std::move(m)) {}
};

coreir::Runtime* g_rt = nullptr;
std::vector<int64_t> g_live_at_step;
int g_failures = 0;

std::string steps() {
  std::string s;
  for (int64_t n : g_live_at_step) s += std::to_string(n) + ",";
  return s;
}

void run_case(const coreir::Module& m, const std::string& want,
              const std::string& what) {
  g_live_at_step.clear();
  if (auto err = coreir::verify(m)) {
    std::fprintf(stderr, "FAIL: %s: malformed IR: %s\n", what.c_str(),
                 err->c_str());
    ++g_failures;
    return;
  }
  int64_t after = -1;
  {
    coreir::Runtime rt;
    g_rt = &rt;
    vm::run(vm::compile(m), rt);
    after = rt.live_objects();
    g_rt = nullptr;
  }
  if (steps() != want || after != 0) {
    std::fprintf(stderr,
                 "FAIL: %s\n  want live-at-each-step [%s], 0 at the end\n"
                 "  got  [%s], %lld at the end\n",
                 what.c_str(), want.c_str(), steps().c_str(),
                 static_cast<long long>(after));
    ++g_failures;
  }
}

}  // namespace

extern "C" {
void coreir_rt_out(int64_t) { g_live_at_step.push_back(g_rt->live_objects()); }
void coreir_rt_out_str(const char*, int64_t) {
  g_live_at_step.push_back(g_rt->live_objects());
}
void coreir_rt_out_raw(const char*, int64_t) {}
int64_t coreir_rt_in(int64_t, int64_t) { return 0; }
void coreir_rt_poll(void) {}
[[noreturn]] void coreir_rt_fail(const char* m, int64_t, int64_t) {
  throw Failure(m);
}
}

int main() {
  using namespace coreir;
  const SrcPos p{1, 1};

  // A temporary nothing holds dies with the statement that made it -- not
  // when its register is next reused, and not at frame exit. This is what
  // Op::ClearRegs exists for: before it, the concatenated string here stayed
  // alive through both later steps.
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 0, 0,
         b.block({b.intrinsic(IntrinsicId::Print,
                              {b.binary(BinOp::Add, b.str_literal("x", p),
                                        b.str_literal("y", p), p)},
                              p),
                  b.intrinsic(IntrinsicId::Print, {b.literal(1, p)}, p),
                  b.intrinsic(IntrinsicId::Print, {b.literal(2, p)}, p)},
                 p),
         {},
         {}});
    // Step 1 is inside the statement that owns the string, so two are live
    // (the concatenation and one of its operands); by step 2 none are.
    run_case(m, "2,0,0,", "a temporary dies with its statement");
  }

  // Overwriting a variable releases what it held, at the assignment.
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 1, 0,
         b.block({b.assign(VarKind::Local, 0, b.str_literal("held", p), p),
                  b.intrinsic(IntrinsicId::Print, {b.literal(1, p)}, p),
                  b.assign(VarKind::Local, 0, b.literal(0, p), p),
                  b.intrinsic(IntrinsicId::Print, {b.literal(2, p)}, p)},
                 p),
         {"s"},
         {}});
    run_case(m, "1,0,", "assignment releases the old value");
  }

  // A callee's locals go when its frame returns, without waiting for the
  // caller to finish.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back(
        {"main", 0, 0,
         b.block({b.call_value(b.make_closure(1, 0, p), {}, p),
                  b.intrinsic(IntrinsicId::Print, {b.literal(2, p)}, p)},
                 p),
         {},
         {}});
    m.funcs.push_back(
        {"inner", 1, 0,
         b.block({b.assign(VarKind::Local, 0, b.str_literal("in inner", p), p),
                  b.intrinsic(IntrinsicId::Print, {b.literal(1, p)}, p)},
                 p),
         {"s"},
         {}});
    // Step 1 is inside inner: its string plus the closure being called.
    run_case(m, "2,0,", "a frame's locals go when it returns");
  }

  // The default, stated as a test so that changing it is deliberate: a
  // local that is never reassigned and belongs to no Scope is held until the
  // frame returns. A front end that wants an earlier release states its
  // block structure with Tag::Scope -- the next case.
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 1, 0,
         b.block({b.assign(VarKind::Local, 0, b.str_literal("held", p), p),
                  b.intrinsic(IntrinsicId::Print, {b.literal(1, p)}, p),
                  b.intrinsic(IntrinsicId::Print, {b.literal(2, p)}, p)},
                 p),
         {"s"},
         {}});
    run_case(m, "1,1,", "a live local is held to the end of its function");
  }

  // The same program with the declaration inside a Scope: the local is
  // released where the scope ends, not where the frame does.
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 1, 0,
         b.block(
             {b.scope(0, 1,
                      b.block({b.assign(VarKind::Local, 0,
                                        b.str_literal("held", p), p),
                               b.intrinsic(IntrinsicId::Print,
                                           {b.literal(1, p)}, p)},
                              p),
                      p),
              b.intrinsic(IntrinsicId::Print, {b.literal(2, p)}, p)},
             p),
         {"s"},
         {}});
    run_case(m, "1,0,", "a scoped local is released where its scope ends");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "lifetimes: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("lifetimes OK\n");
  return 0;
}
