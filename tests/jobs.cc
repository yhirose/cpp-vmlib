// The job queue: Enqueue takes a 0-argument closure, and vm::run drains the
// queue once the entry frame has returned -- FIFO, each job to completion,
// a job's own enqueues after every job already waiting. A job runs on the
// same executor as the entry frame: it can throw (uncaught, that ends the
// run), its objects get their destructors, and under entry_frame_drops ==
// false it is not the entry frame -- its values still drop. Zero live heap
// objects after every case.

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

std::string run_module(const coreir::Module& m, const std::string& what,
                       vm::RunOptions opts = {}) {
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
      vm::run(p, rt, opts);
    } catch (const Failure& e) {
      failure = e.what();
    }
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

  auto say = [&p](Builder& b, const char* s) {
    return b.intrinsic(IntrinsicId::Print, {b.str_literal(s, p)}, p);
  };
  auto enqueue = [&p](Builder& b, int32_t func, int32_t cmap) {
    return b.intrinsic(IntrinsicId::Enqueue, {b.make_closure(func, cmap, p)},
                       p);
  };

  // --- 1. Order: the entry frame first, then FIFO, a job's enqueues last. -
  // main { enqueue A; enqueue B; print "main" }
  // A { print "A"; enqueue C }   B { print "B" }   C { print "C" }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back({"A", 0, 0, b.block({say(b, "A"), enqueue(b, 3, 0)}, p),
                       {}, {}});
    m.funcs.push_back({"B", 0, 0, say(b, "B"), {}, {}});
    m.funcs.push_back({"C", 0, 0, say(b, "C"), {}, {}});
    m.funcs[0] = {"main", 0, 0,
                  b.block({enqueue(b, 1, 0), enqueue(b, 2, 0), say(b, "main")},
                          p),
                  {}, {}};
    check_eq(run_module(m, "order"), "", "order: failure");
    check_eq(joined(), "main|A|B|C|", "order output");
  }

  // --- 2. Jobs share state through cells, and see each other's writes. ---
  // main { n = cell 0; enqueue inc; enqueue inc; enqueue show }
  // inc { n = n + 1 }   show { print n }   -> 2
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.capture_maps.push_back({{VarKind::Cell, 0}});
    m.funcs.push_back({});
    m.funcs.push_back(
        {"inc", 0, 1,
         b.assign(VarKind::Capture, 0,
                  b.binary(BinOp::Add, b.varref(VarKind::Capture, 0, p),
                           b.literal(1, p), p),
                  p),
         {}, {"n"}});
    m.funcs.push_back({"show", 0, 1,
                       b.intrinsic(IntrinsicId::Print,
                                   {b.varref(VarKind::Capture, 0, p)}, p),
                       {}, {"n"}});
    m.funcs[0] = {"main", 0, 0,
                  b.block({b.assign(VarKind::Cell, 0, b.literal(0, p), p),
                           enqueue(b, 1, 1), enqueue(b, 1, 1),
                           enqueue(b, 2, 1)},
                          p),
                  {}, {}};
    m.funcs[0].num_cells = 1;
    check_eq(run_module(m, "cells"), "", "cells: failure");
    check_eq(joined(), "2|", "cells output");
  }

  // --- 3. A job's uncaught throw ends the run; later jobs never run. -----
  // main { enqueue boom; enqueue never; print "main" }
  // boom { throw "boom" }   never { print "never" }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back({"boom", 0, 0,
                       b.make_throw(b.str_literal("boom", SrcPos{7, 3}),
                                    SrcPos{7, 3}),
                       {}, {}});
    m.funcs.push_back({"never", 0, 0, say(b, "never"), {}, {}});
    m.funcs[0] = {"main", 0, 0,
                  b.block({enqueue(b, 1, 0), enqueue(b, 2, 0), say(b, "main")},
                          p),
                  {}, {}};
    check_eq(run_module(m, "uncaught"), "uncaught: boom", "uncaught: failure");
    check_eq(joined(), "main|", "uncaught output");
  }

  // --- 4. A job catches its own throw and the queue goes on. -------------
  // main { enqueue safe; enqueue after }
  // safe { try { throw "x" } catch e { print e } }   after { print "after" }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back(
        {"safe", 1, 0,
         b.make_try(0, b.make_throw(b.str_literal("x", p), p),
                    b.intrinsic(IntrinsicId::Print,
                                {b.varref(VarKind::Local, 0, p)}, p),
                    p),
         {"e"}, {}});
    m.funcs.push_back({"after", 0, 0, say(b, "after"), {}, {}});
    m.funcs[0] = {"main", 0, 0, b.block({enqueue(b, 1, 0), enqueue(b, 2, 0)}, p),
                  {}, {}};
    check_eq(run_module(m, "caught"), "", "caught: failure");
    check_eq(joined(), "x|after|", "caught output");
  }

  // --- 5. Enqueue wants a function; anything else traps at the call. -----
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({"main", 0, 0,
                       b.intrinsic(IntrinsicId::Enqueue, {b.literal(3, p)}, p),
                       {}, {}});
    check_eq(run_module(m, "not-fn"), "enqueue needs a function, not int",
             "not-fn: failure");
  }

  // --- 6. A job that takes a parameter cannot be given one: the run fails.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back({"needs", 1, 0, say(b, "ran"), {"x"}, {}});
    m.funcs.back().num_params = 1;
    m.funcs[0] = {"main", 0, 0, enqueue(b, 1, 0), {}, {}};
    check_eq(run_module(m, "arity"), "needs takes 1 argument(s), given 0",
             "arity: failure");
    check_eq(joined(), "", "arity output");
  }

  // --- 7. A job's object gets its destructor, whatever entry_frame_drops.
  // main { o = {drop: d}; enqueue job }   job { j = {drop: d} }
  // d(x) { print "drop" }
  // Default: both drop -- main's when the entry frame returns, which is
  // before the queue drains, the job's at the job's own return.
  // entry_frame_drops == false: only the job's.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.capture_maps.push_back({{VarKind::Cell, 0}});
    m.funcs.push_back({});
    m.funcs.push_back({"d", 1, 0, say(b, "drop"), {"x"}, {}});
    m.funcs.back().num_params = 1;
    auto with_drop = [&](const char* tag) {
      return b.object_lit(
          {{b.str_literal("tag", p), b.str_literal(tag, p)},
           {b.str_literal("\x01" "drop", p), b.varref(VarKind::Capture, 0, p)}},
          p);
    };
    m.funcs.push_back(
        {"job", 1, 1,
         b.block({b.assign(VarKind::Local, 0, with_drop("job"), p),
                  say(b, "job")},
                 p),
         {"j"}, {"d"}});
    // main's slot 0 holds the object; its drop closure is cell 0.
    m.funcs[0] = {"main", 1, 0,
                  b.block({b.assign(VarKind::Cell, 0, b.make_closure(1, 0, p), p),
                           b.assign(VarKind::Local, 0,
                                    b.object_lit({{b.str_literal("tag", p),
                                                   b.str_literal("main", p)},
                                                  {b.str_literal("\x01" "drop", p),
                                                   b.varref(VarKind::Cell, 0, p)}},
                                                 p),
                                    p),
                           enqueue(b, 2, 1), say(b, "main")},
                          p),
                  {"o"}, {}};
    m.funcs[0].num_cells = 1;
    check_eq(run_module(m, "drops"), "", "drops: failure");
    check_eq(joined(), "main|drop|job|drop|", "drops output");
    vm::RunOptions opts;
    opts.entry_frame_drops = false;
    check_eq(run_module(m, "drops-bare", opts), "", "drops-bare: failure");
    check_eq(joined(), "main|job|drop|", "drops-bare output");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("jobs: OK");
  return 0;
}
