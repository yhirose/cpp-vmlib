// Generators: calling an is_generator function packages a suspended
// activation instead of running it; GenResume drives it yield to yield,
// each answer a fresh {value, done} object; GenReturn closes one early,
// running its pending defers first. The frame parks its storage inside the
// GeneratorObj between resumes, so the collector sees everything a
// suspended generator holds -- the last two cases pin that. Zero live heap
// objects after every case, as everywhere else.

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "coreir/ir.h"
#include "vm/compiler.h"
#include "vm/exec.h"

namespace {

struct Failure : std::runtime_error {
  Failure(std::string msg) : std::runtime_error(std::move(msg)) {}
};

std::vector<std::string> g_out;
int g_failures = 0;

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

std::string run_module(const coreir::Module& m, const std::string& what) {
  g_out.clear();
  if (auto err = coreir::verify(m)) {
    std::fprintf(stderr, "FAIL: %s: malformed IR: %s\n", what.c_str(),
                 err->c_str());
    ++g_failures;
    return {};
  }
  std::string failure;
  int64_t left = 0;
  {
    coreir::Runtime rt;
    const vm::Program p = vm::compile(m);
    try {
      vm::run(p, rt);
    } catch (const Failure& e) {
      failure = e.what();
    }
    // The counting side alone must get the heap to zero here; the cycle
    // case below manages its own Runtime to exercise collect() instead.
    left = rt.live_objects();
  }
  if (left != 0) {
    std::fprintf(stderr, "FAIL: %s: leaked %lld heap object(s)\n",
                 what.c_str(), static_cast<long long>(left));
    ++g_failures;
  }
  return failure;
}

}  // namespace

extern "C" {
void coreir_rt_out(int64_t v) { g_out.push_back(std::to_string(v)); }
void coreir_rt_out_str(const char* bytes, int64_t len) {
  g_out.emplace_back(bytes, static_cast<size_t>(len));
}
void coreir_rt_out_raw(const char* bytes, int64_t len) {
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

  // Shared shorthand: print(e["value"]) and print(e["done"]) for a resume
  // result already stored in a local.
  auto print_field = [&p](Builder& b, int32_t local, const char* key) {
    return b.intrinsic(
        IntrinsicId::Print,
        {b.index(b.varref(VarKind::Local, local, p), b.str_literal(key, p),
                 p)},
        p);
  };

  // --- 1. Yield to done: values and flags, then resume past the end. ------
  // gen() { yield 1; yield 2; return 3 }
  // g = gen(); 4x: r = resume(g, nil); print r.value; print r.done
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});  // main fills in below
    m.funcs.push_back({"gen", 0, 0,
                       b.block({b.make_yield(b.literal(1, p), p),
                                b.make_yield(b.literal(2, p), p),
                                b.make_return(b.literal(3, p), p)},
                               p),
                       {},
                       {}});
    m.funcs.back().is_generator = true;

    std::vector<NodeId> stmts{b.assign(
        VarKind::Local, 0,
        b.call_value(b.make_closure(1, 0, p), {}, p), p)};
    for (int i = 0; i < 4; ++i) {
      stmts.push_back(b.assign(
          VarKind::Local, 1,
          b.intrinsic(IntrinsicId::GenResume,
                      {b.varref(VarKind::Local, 0, p), b.nil_literal(p)}, p),
          p));
      stmts.push_back(print_field(b, 1, "value"));
      stmts.push_back(print_field(b, 1, "done"));
    }
    m.funcs[0] = {"main", 2, 0, b.block(stmts, p), {"g", "r"}, {}};
    check_eq(run_module(m, "sequence"), "", "sequence: failure");
    check_eq(joined(),
             "1|false|2|false|3|true|nil|true|",
             "sequence output");
  }

  // --- 2. The sent value is the yield expression's own value. -------------
  // gen() { print (yield 10) + 1; return 0 }
  // g = gen(); resume(g, nil) starts it; resume(g, 41) prints 42.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back(
        {"gen", 0, 0,
         b.block({b.intrinsic(IntrinsicId::Print,
                              {b.binary(BinOp::Add,
                                        b.make_yield(b.literal(10, p), p),
                                        b.literal(1, p), p)},
                              p),
                  b.make_return(b.literal(0, p), p)},
                 p),
         {},
         {}});
    m.funcs.back().is_generator = true;

    auto resume = [&](NodeId sent) {
      return b.intrinsic(IntrinsicId::GenResume,
                         {b.varref(VarKind::Local, 0, p), sent}, p);
    };
    m.funcs[0] = {
        "main", 2, 0,
        b.block({b.assign(VarKind::Local, 0,
                          b.call_value(b.make_closure(1, 0, p), {}, p), p),
                 b.assign(VarKind::Local, 1, resume(b.nil_literal(p)), p),
                 print_field(b, 1, "value"),
                 b.assign(VarKind::Local, 1, resume(b.literal(41, p)), p),
                 print_field(b, 1, "done")},
                p),
        {"g", "r"},
        {}};
    check_eq(run_module(m, "sent"), "", "sent: failure");
    check_eq(joined(), "10|42|true|", "sent output");
  }

  // --- 3. Arguments and captures reach the parked frame. ------------------
  // base is a cell in main; gen(a) { yield a + base; return 0 }
  // g = gen(7) with base=100 -> first resume yields 107.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});                       // 0: empty
    m.capture_maps.push_back({{VarKind::Cell, 0}});     // 1: main's cell 0
    m.funcs.push_back({});
    m.funcs.push_back(
        {"gen", 1, 1,
         b.block({b.make_yield(b.binary(BinOp::Add,
                                        b.varref(VarKind::Local, 0, p),
                                        b.varref(VarKind::Capture, 0, p), p),
                               p),
                  b.make_return(b.literal(0, p), p)},
                 p),
         {"a"},
         {"base"}});
    m.funcs.back().is_generator = true;
    m.funcs.back().num_params = 1;

    m.funcs[0] = {
        "main", 2, 0,
        b.block(
            {b.assign(VarKind::Cell, 0, b.literal(100, p), p),
             b.assign(VarKind::Local, 0,
                      b.call_value(b.make_closure(1, 1, p),
                                   {b.literal(7, p)}, p),
                      p),
             b.assign(VarKind::Local, 1,
                      b.intrinsic(IntrinsicId::GenResume,
                                  {b.varref(VarKind::Local, 0, p),
                                   b.nil_literal(p)},
                                  p),
                      p),
             print_field(b, 1, "value")},
            p),
        {"g", "r"},
        {}};
    m.funcs[0].num_cells = 1;
    check_eq(run_module(m, "captures"), "", "captures: failure");
    check_eq(joined(), "107|", "captures output");
  }

  // --- 4. Defers: natural completion runs them; so does GenReturn. --------
  // gen() { scope { defer print "D"; yield 1; yield 2 } return 0 }
  // First run drives it to completion (D at the body's own exit); second
  // parks it after yield 1 and closes it (D from GenReturn), which also
  // answers {value: 9, done: true}.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back({"deferD", 0, 0,
                       b.intrinsic(IntrinsicId::Print,
                                   {b.str_literal("D", p)}, p),
                       {},
                       {}});
    m.funcs.push_back(
        {"gen", 0, 0,
         b.block({b.scope(0, 0,
                          b.block({b.make_defer(b.make_closure(1, 0, p), p),
                                   b.make_yield(b.literal(1, p), p),
                                   b.make_yield(b.literal(2, p), p)},
                                  p),
                          p),
                  b.make_return(b.literal(0, p), p)},
                 p),
         {},
         {}});
    m.funcs.back().is_generator = true;

    auto resume = [&]() {
      return b.intrinsic(IntrinsicId::GenResume,
                         {b.varref(VarKind::Local, 0, p), b.nil_literal(p)},
                         p);
    };
    auto drop_resume = [&]() {
      return b.assign(VarKind::Local, 1, resume(), p);
    };
    m.funcs[0] = {
        "main", 2, 0,
        b.block(
            {// Drain: 1, 2, then completion runs D before done.
             b.assign(VarKind::Local, 0,
                      b.call_value(b.make_closure(2, 0, p), {}, p), p),
             drop_resume(), print_field(b, 1, "value"),
             drop_resume(), print_field(b, 1, "value"),
             drop_resume(), print_field(b, 1, "done"),
             b.intrinsic(IntrinsicId::Print, {b.str_literal("--", p)}, p),
             // Park after yield 1, then close: D runs, result carries 9.
             b.assign(VarKind::Local, 0,
                      b.call_value(b.make_closure(2, 0, p), {}, p), p),
             drop_resume(),
             b.assign(VarKind::Local, 1,
                      b.intrinsic(IntrinsicId::GenReturn,
                                  {b.varref(VarKind::Local, 0, p),
                                   b.literal(9, p)},
                                  p),
                      p),
             print_field(b, 1, "value"), print_field(b, 1, "done"),
             // Resuming the closed generator: {nil, true}, no more D.
             drop_resume(), print_field(b, 1, "value")},
            p),
        {"g", "r"},
        {}};
    check_eq(run_module(m, "defers"), "", "defers: failure");
    check_eq(joined(), "1|2|D|true|--|D|9|true|nil|", "defers output");
  }

  // --- 5. A throw inside the body lands at the resume, and finishes it. ---
  // gen() { yield 1; throw "boom" }
  // try { resume; resume } catch e { print e }; then resume -> done.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back({"gen", 0, 0,
                       b.block({b.make_yield(b.literal(1, p), p),
                                b.make_throw(b.str_literal("boom", p), p)},
                               p),
                       {},
                       {}});
    m.funcs.back().is_generator = true;

    auto resume = [&]() {
      return b.intrinsic(IntrinsicId::GenResume,
                         {b.varref(VarKind::Local, 0, p), b.nil_literal(p)},
                         p);
    };
    m.funcs[0] = {
        "main", 3, 0,
        b.block(
            {b.assign(VarKind::Local, 0,
                      b.call_value(b.make_closure(1, 0, p), {}, p), p),
             b.make_try(
                 2,
                 b.block({b.assign(VarKind::Local, 1, resume(), p),
                          print_field(b, 1, "value"),
                          b.assign(VarKind::Local, 1, resume(), p),
                          b.intrinsic(IntrinsicId::Print,
                                      {b.str_literal("unreached", p)}, p)},
                         p),
                 b.intrinsic(IntrinsicId::Print,
                             {b.varref(VarKind::Local, 2, p)}, p),
                 p),
             b.assign(VarKind::Local, 1, resume(), p),
             print_field(b, 1, "done")},
            p),
        {"g", "r", "e"},
        {}};
    check_eq(run_module(m, "throw"), "", "throw: failure");
    check_eq(joined(), "1|boom|true|", "throw output");
  }

  // --- 6. Resuming a generator that is already running traps. -------------
  // Main's cell holds the generator; the body reads it through its own
  // capture and resumes it while it is the one running. Catchable trap.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});                    // 0: empty
    m.capture_maps.push_back({{VarKind::Cell, 0}});  // 1: main's cell 0
    m.funcs.push_back({});
    m.funcs.push_back(
        {"gen", 0, 1,
         b.block({b.make_yield(
                      b.intrinsic(IntrinsicId::GenResume,
                                  {b.varref(VarKind::Capture, 0, p),
                                   b.nil_literal(p)},
                                  p),
                      p),
                  b.make_return(b.literal(0, p), p)},
                 p),
         {},
         {"self"}});
    m.funcs.back().is_generator = true;

    m.funcs[0] = {
        "main", 2, 0,
        b.block(
            {b.assign(VarKind::Cell, 0,
                      b.call_value(b.make_closure(1, 1, p), {}, p), p),
             b.make_try(
                 1,
                 b.block(
                     {b.assign(
                          VarKind::Local, 0,
                          b.intrinsic(IntrinsicId::GenResume,
                                      {b.varref(VarKind::Cell, 0, p),
                                       b.nil_literal(p)},
                                      p),
                          p),
                      b.intrinsic(IntrinsicId::Print,
                                  {b.str_literal("unreached", p)}, p)},
                     p),
                 b.intrinsic(
                     IntrinsicId::Print,
                     {b.index(b.varref(VarKind::Local, 1, p),
                              b.str_literal("message", p), p)},
                     p),
                 p),
             // Break the cell -> generator -> capture -> cell cycle so the
             // counting side alone empties the heap for run_module's check.
             b.assign(VarKind::Cell, 0, b.nil_literal(p), p)},
            p),
        {"r", "e"},
        {}};
    m.funcs[0].num_cells = 1;
    check_eq(run_module(m, "self-resume"), "", "self-resume: failure");
    check_eq(joined(), "generator already running|", "self-resume output");
  }

  // --- 7. GenReturn on a Start-state generator: nothing ran, still done. --
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back({"gen", 0, 0,
                       b.block({b.make_yield(b.str_literal("never", p), p)},
                               p),
                       {},
                       {}});
    m.funcs.back().is_generator = true;
    m.funcs[0] = {
        "main", 2, 0,
        b.block(
            {b.assign(VarKind::Local, 0,
                      b.call_value(b.make_closure(1, 0, p), {}, p), p),
             b.assign(VarKind::Local, 1,
                      b.intrinsic(IntrinsicId::GenReturn,
                                  {b.varref(VarKind::Local, 0, p),
                                   b.literal(5, p)},
                                  p),
                      p),
             print_field(b, 1, "value"), print_field(b, 1, "done"),
             b.assign(VarKind::Local, 1,
                      b.intrinsic(IntrinsicId::GenResume,
                                  {b.varref(VarKind::Local, 0, p),
                                   b.nil_literal(p)},
                                  p),
                      p),
             print_field(b, 1, "value"), print_field(b, 1, "done")},
            p),
        {"g", "r"},
        {}};
    check_eq(run_module(m, "close start"), "", "close start: failure");
    check_eq(joined(), "5|true|nil|true|", "close start output");
  }

  // --- 8. The collector sees a parked frame's storage. --------------------
  // A generator suspended holding an array in a local: a collect() while it
  // is parked must keep both (rc alone also keeps them; the point is that
  // gc_refs counts the parked edges, so the generator is not condemned as
  // "externally unreferenced" nor its array freed under it). Then the same
  // shape left as a pure cycle -- the generator parked holding itself with
  // no outside handle -- which only collect() can free.
  {
    coreir::Runtime rt;
    coreir::Runtime::Scope scope(rt);
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    // gen(x) { x2 = yield 0; yield x2; return 0 } -- after the first
    // resume, local x holds the array; after the second, x2 holds whatever
    // was sent (the generator itself, in the cycle step).
    m.funcs.push_back(
        {"gen", 2, 0,
         b.block({b.assign(VarKind::Local, 1,
                           b.make_yield(b.literal(0, p), p), p),
                  b.make_yield(b.varref(VarKind::Local, 1, p), p),
                  b.make_return(b.literal(0, p), p)},
                 p),
         {"x", "x2"},
         {}});
    m.funcs.back().is_generator = true;
    m.funcs.back().num_params = 1;
    // main(): g = gen([1,2,3]); resume(g, nil); resume(g, g); return --
    // leaving g parked holding itself in x2, referenced only by main's
    // local, which the return releases.
    m.funcs[0] = {
        "main", 1, 0,
        b.block(
            {b.assign(VarKind::Local, 0,
                      b.call_value(b.make_closure(1, 0, p),
                                   {b.array_lit({b.literal(1, p),
                                                 b.literal(2, p),
                                                 b.literal(3, p)},
                                                p)},
                                   p),
                      p),
             b.intrinsic(IntrinsicId::GenResume,
                         {b.varref(VarKind::Local, 0, p), b.nil_literal(p)},
                         p),
             b.intrinsic(IntrinsicId::GenResume,
                         {b.varref(VarKind::Local, 0, p),
                          b.varref(VarKind::Local, 0, p)},
                         p)},
            p),
        {"g"},
        {}};
    if (auto err = coreir::verify(m)) {
      std::fprintf(stderr, "FAIL: gc: malformed IR: %s\n", err->c_str());
      ++g_failures;
    } else {
      const vm::Program prog = vm::compile(m);
      vm::run(prog, rt);
      // The run dropped main's handle; the generator survives as a pure
      // cycle (parked local x2 -> itself) that counting cannot free.
      if (rt.live_objects() == 0) {
        std::fprintf(stderr,
                     "FAIL: gc: expected the parked cycle to outlive rc\n");
        ++g_failures;
      }
      rt.collect();
      if (rt.live_objects() != 0) {
        std::fprintf(stderr, "FAIL: gc: collect left %lld object(s)\n",
                     static_cast<long long>(rt.live_objects()));
        ++g_failures;
      }
    }
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("generators: all cases passed");
  return 0;
}
