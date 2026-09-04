// Coroutines: CoroCreate packages a callable; the first CoroResume calls
// it, and a CoroYield anywhere in the frames that call builds -- however
// deep, in a function that never heard of coroutines -- parks every frame
// from the resumed one up to the yielding one into the CoroObj and answers
// the resumer {value, done: false}. What this pins: the {value, done}
// protocol and CoroStatus through a whole life; a yield three calls deep
// and the resume that walks back up through all three; the host boundary
// (a native that called back in) a yield cannot cross, and the trap that
// says so; CoroClose running the parked frames' defers innermost first; an
// uncaught throw finishing the coroutine and landing in the resumer's
// handler; a generator activation parked mid-body inside a coroutine; and
// the collector seeing everything a suspended coroutine holds. Zero live
// heap objects after every case.

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "vmlib.h"

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

using coreir::NativeCall;
using coreir::Value;

bool native_sum(NativeCall& c) {
  int64_t total = 0;
  for (int32_t i = 0; i < c.argc; ++i) total += c.args[i].as_int();
  c.result = Value::make_int(total);
  return true;
}

// twice(f, x): f(x) then f(x) again -- a host boundary between the
// coroutine's frames below and whatever f does above.
bool native_twice(NativeCall& c) {
  c.call(c.arg(0), &c.args[1], 1);
  c.result = c.call(c.arg(0), &c.args[1], 1);
  return true;
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
    vm::RunOptions opts;
    opts.natives = {{"sum", -1, &native_sum, nullptr},
                    {"twice", 2, &native_twice, nullptr}};
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
  using I = IntrinsicId;

  auto pr = [&p](Builder& b, NodeId v) { return b.intrinsic(I::Print, {v}, p); };
  auto loc = [&p](Builder& b, int32_t i) {
    return b.varref(VarKind::Local, i, p);
  };
  auto cap = [&p](Builder& b, int32_t i) {
    return b.varref(VarKind::Capture, i, p);
  };
  auto cell = [&p](Builder& b, int32_t i) {
    return b.varref(VarKind::Cell, i, p);
  };
  auto str = [&p](Builder& b, const char* s) { return b.str_literal(s, p); };
  auto field = [&](Builder& b, NodeId o, const char* k) {
    return b.index(o, str(b, k), p);
  };
  auto resume = [&p](Builder& b, NodeId co, NodeId v) {
    return b.intrinsic(I::CoroResume, {co, v}, p);
  };
  auto status = [&p](Builder& b, NodeId co) {
    return b.intrinsic(I::CoroStatus, {co}, p);
  };
  auto set_local = [&p](Builder& b, int32_t i, NodeId v) {
    return b.assign(VarKind::Local, i, v, p);
  };

  // --- 1. The protocol, through a whole life. -----------------------------
  // f(x) { print x; s = yield 1; print s; return "end" }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back(
        {"f", 2, 0,
         b.block({pr(b, loc(b, 0)),
                  set_local(b, 1, b.intrinsic(I::CoroYield, {b.literal(1, p)}, p)),
                  pr(b, loc(b, 1)), b.make_return(str(b, "end"), p)},
                 p),
         {"x", "s"}, {}});
    m.funcs.back().num_params = 1;
    const NodeId co = loc(b, 0);
    const NodeId r = loc(b, 1);
    m.funcs[0] = {
        "main", 2, 0,
        b.block({set_local(b, 0, b.intrinsic(I::CoroCreate,
                                             {b.make_closure(1, 0, p)}, p)),
                 pr(b, status(b, co)),
                 set_local(b, 1, resume(b, co, str(b, "hello"))),
                 pr(b, field(b, r, "value")), pr(b, field(b, r, "done")),
                 pr(b, status(b, co)),
                 set_local(b, 1, resume(b, co, str(b, "sent"))),
                 pr(b, field(b, r, "value")), pr(b, field(b, r, "done")),
                 pr(b, status(b, co)),
                 set_local(b, 1, resume(b, co, b.nil_literal(p))),
                 pr(b, field(b, r, "value")), pr(b, field(b, r, "done")),
                 pr(b, b.intrinsic(I::CoroCurrent, {}, p)),
                 pr(b, b.intrinsic(I::TypeOf, {co}, p))},
                p),
        {"co", "r"}, {}};
    check_eq(run_module(m, "life"), "", "life: failure");
    check_eq(joined(),
             "start|hello|1|false|suspended|sent|end|true|done|nil|true|nil|"
             "coroutine|",
             "life output");
  }

  // --- 2. A yield three calls deep, and the walk back up. ----------------
  // f() { print "f in"; v = g(); print "f " + v; return "F" }
  // g() { print "g in"; w = h(); print "g " + w; return "G" }
  // h() { print "h in"; s = yield "from h"; print "h " + s; return "H" }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.capture_maps.push_back({{VarKind::Cell, 0}, {VarKind::Cell, 1}});
    m.funcs.push_back({});
    auto cat = [&](Builder& bb, const char* s, NodeId v) {
      return bb.binary(BinOp::Add, str(bb, s), v, p);
    };
    // captures in f and g: 0 = g, 1 = h
    m.funcs.push_back({"f", 1, 2,
                       b.block({pr(b, str(b, "f in")),
                                set_local(b, 0, b.call_value(cap(b, 0), {}, p)),
                                pr(b, cat(b, "f ", loc(b, 0))),
                                b.make_return(str(b, "F"), p)},
                               p),
                       {"v"}, {"g", "h"}});
    m.funcs.back().lenient_arity = true;  // called with the first sent value
    m.funcs.push_back({"g", 1, 2,
                       b.block({pr(b, str(b, "g in")),
                                set_local(b, 0, b.call_value(cap(b, 1), {}, p)),
                                pr(b, cat(b, "g ", loc(b, 0))),
                                b.make_return(str(b, "G"), p)},
                               p),
                       {"w"}, {"g", "h"}});
    m.funcs.push_back(
        {"h", 1, 0,
         b.block({pr(b, str(b, "h in")),
                  set_local(b, 0, b.intrinsic(I::CoroYield, {str(b, "from h")}, p)),
                  pr(b, cat(b, "h ", loc(b, 0))), b.make_return(str(b, "H"), p)},
                 p),
         {"s"}, {}});
    const NodeId co = loc(b, 0);
    const NodeId r = loc(b, 1);
    m.funcs[0] = {
        "main", 2, 0,
        b.block({b.assign(VarKind::Cell, 0, b.make_closure(2, 1, p), p),
                 b.assign(VarKind::Cell, 1, b.make_closure(3, 0, p), p),
                 set_local(b, 0, b.intrinsic(I::CoroCreate,
                                             {b.make_closure(1, 1, p)}, p)),
                 set_local(b, 1, resume(b, co, b.nil_literal(p))),
                 pr(b, field(b, r, "value")),
                 set_local(b, 1, resume(b, co, str(b, "S"))),
                 pr(b, field(b, r, "value")), pr(b, field(b, r, "done")),
                 // g's closure captures cell 1 (h) and cell 0 (itself):
                 // break that cycle for the heap check.
                 b.assign(VarKind::Cell, 0, b.nil_literal(p), p)},
                p),
        {"co", "r"}, {}};
    m.funcs[0].num_cells = 2;
    check_eq(run_module(m, "deep"), "", "deep: failure");
    check_eq(joined(), "f in|g in|h in|from h|h S|g H|f G|F|true|",
             "deep output");
  }

  // --- 3. The host boundary: a yield under a native's callback traps, and
  // the trap is the program's to catch. Outside any coroutine, a yield is
  // a trap too. ---------------------------------------------------------
  // body() { try { twice(yielder, 0) } catch e { print e.message }
  //          return "ok" }     yielder(x) { yield x }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back(
        {"body", 2, 0,
         b.block({b.make_try(
                      1,
                      b.call_value(b.native_ref(b.declare_native("twice"), p),
                                   {b.make_closure(2, 0, p), b.literal(0, p)},
                                   p),
                      pr(b, field(b, loc(b, 1), "message")), p),
                  b.make_return(str(b, "ok"), p)},
                 p),
         {"x", "e"}, {}});
    m.funcs.back().num_params = 1;
    m.funcs.push_back({"yielder", 1, 0,
                       b.intrinsic(I::CoroYield, {loc(b, 0)}, p), {"x"}, {}});
    m.funcs.back().num_params = 1;
    const NodeId co = loc(b, 0);
    const NodeId r = loc(b, 1);
    m.funcs[0] = {
        "main", 2, 0,
        b.block({set_local(b, 0, b.intrinsic(I::CoroCreate,
                                             {b.make_closure(1, 0, p)}, p)),
                 set_local(b, 1, resume(b, co, b.nil_literal(p))),
                 pr(b, field(b, r, "value")), pr(b, field(b, r, "done"))},
                p),
        {"co", "r"}, {}};
    check_eq(run_module(m, "boundary"), "", "boundary: failure");
    check_eq(joined(), "cannot yield across a host boundary|ok|true|",
             "boundary output");
  }
  {
    Module m;
    Builder b(m);
    m.funcs.push_back({"main", 0, 0,
                       pr(b, b.intrinsic(I::CoroYield, {b.literal(1, p)}, p)),
                       {}, {}});
    check_eq(run_module(m, "outside"), "yield outside a coroutine",
             "outside: failure");
  }

  // --- 4. CoroClose runs the parked frames' defers, innermost first, and
  // nothing else of them; on a Start coroutine it just finishes it. ------
  // f() { scope { defer d1; g() } }   g() { scope { defer d2; yield 1;
  // print "after" } }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.capture_maps.push_back({{VarKind::Cell, 0}, {VarKind::Cell, 1}});
    m.capture_maps.push_back({{VarKind::Capture, 0}, {VarKind::Capture, 1}});
    m.funcs.push_back({});
    m.funcs.push_back({"d1", 0, 0, pr(b, str(b, "d1")), {}, {}});
    m.funcs.push_back({"d2", 0, 0, pr(b, str(b, "d2")), {}, {}});
    // captures: 0 = d1, 1 = d2 -- from main's cells (map 1) for f, and
    // forwarded from f's own captures (map 2) for g.
    m.funcs.push_back(
        {"g", 0, 2,
         b.scope(0, 0,
                 b.block({b.make_defer(cap(b, 1), p),
                          b.intrinsic(I::CoroYield, {b.literal(1, p)}, p),
                          pr(b, str(b, "after"))},
                         p),
                 p),
         {}, {"d1", "d2"}});
    m.funcs.push_back(
        {"f", 1, 2,
         b.scope(0, 1,
                 b.block({b.make_defer(cap(b, 0), p),
                          b.call_value(b.make_closure(3, 2, p), {}, p)},
                         p),
                 p),
         {"x"}, {"d1", "d2"}});
    m.funcs.back().num_params = 1;
    const NodeId co = loc(b, 0);
    const NodeId co2 = loc(b, 1);
    m.funcs[0] = {
        "main", 2, 0,
        b.block({b.assign(VarKind::Cell, 0, b.make_closure(1, 0, p), p),
                 b.assign(VarKind::Cell, 1, b.make_closure(2, 0, p), p),
                 set_local(b, 0, b.intrinsic(I::CoroCreate,
                                             {b.make_closure(4, 1, p)}, p)),
                 pr(b, field(b, resume(b, co, b.nil_literal(p)), "value")),
                 b.intrinsic(I::CoroClose, {co}, p), pr(b, status(b, co)),
                 b.intrinsic(I::CoroClose, {co}, p),  // done: a no-op
                 set_local(b, 1, b.intrinsic(I::CoroCreate,
                                             {b.make_closure(4, 1, p)}, p)),
                 b.intrinsic(I::CoroClose, {co2}, p), pr(b, status(b, co2)),
                 pr(b, field(b, resume(b, co2, b.nil_literal(p)), "done"))},
                p),
        {"co", "co2"}, {}};
    m.funcs[0].num_cells = 2;
    check_eq(run_module(m, "close"), "", "close: failure");
    check_eq(joined(), "1|d2|d1|done|done|true|", "close output");
  }

  // --- 5. An uncaught throw finishes the coroutine and lands at the
  // resumer, in its handlers. -----------------------------------------------
  // f() { yield 1; throw "boom" }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back({"f", 1, 0,
                       b.block({b.intrinsic(I::CoroYield, {b.literal(1, p)}, p),
                                b.make_throw(str(b, "boom"), p)},
                               p),
                       {"x"}, {}});
    m.funcs.back().num_params = 1;
    const NodeId co = loc(b, 0);
    m.funcs[0] = {
        "main", 2, 0,
        b.block({set_local(b, 0, b.intrinsic(I::CoroCreate,
                                             {b.make_closure(1, 0, p)}, p)),
                 resume(b, co, b.nil_literal(p)),
                 b.make_try(1, resume(b, co, b.nil_literal(p)),
                            pr(b, b.binary(BinOp::Add, str(b, "caught "),
                                           loc(b, 1), p)),
                            p),
                 pr(b, status(b, co)),
                 // Resuming a coroutine from inside itself is a trap, and
                 // an uncaught one ends the run.
                 set_local(b, 0, b.intrinsic(I::CoroCreate,
                                             {b.make_closure(2, 0, p)}, p)),
                 resume(b, co, b.nil_literal(p))},
                p),
        {"co", "e"}, {}};
    m.funcs.push_back({"self", 1, 0,
                       resume(b, b.intrinsic(I::CoroCurrent, {}, p),
                              b.nil_literal(p)),
                       {"x"}, {}});
    m.funcs.back().num_params = 1;
    check_eq(run_module(m, "throw"), "coroutine already running",
             "throw: failure");
    check_eq(joined(), "caught boom|done|", "throw output");
  }

  // --- 6. A generator running inside a coroutine can be parked mid-body
  // (its frame goes with the slice, gen_self and all) and resumed. -------
  // gen() [generator] { coyield "g-coro"; yield "g-gen"; return 0 }
  // f() { it = gen(); r = genresume(it); print r.value; return "done-f" }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back(
        {"gen", 0, 0,
         b.block({b.intrinsic(I::CoroYield, {str(b, "g-coro")}, p),
                  b.make_yield(str(b, "g-gen"), p), b.make_return(b.literal(0, p), p)},
                 p),
         {}, {}});
    m.funcs.back().is_generator = true;
    m.funcs.push_back(
        {"f", 3, 0,
         b.block({set_local(b, 1, b.call_value(b.make_closure(1, 0, p), {}, p)),
                  set_local(b, 2, b.intrinsic(I::GenResume,
                                              {loc(b, 1), b.nil_literal(p)}, p)),
                  pr(b, field(b, loc(b, 2), "value")),
                  b.make_return(str(b, "done-f"), p)},
                 p),
         {"x", "it", "r"}, {}});
    m.funcs.back().num_params = 1;
    const NodeId co = loc(b, 0);
    const NodeId r = loc(b, 1);
    m.funcs[0] = {
        "main", 2, 0,
        b.block({set_local(b, 0, b.intrinsic(I::CoroCreate,
                                             {b.make_closure(2, 0, p)}, p)),
                 set_local(b, 1, resume(b, co, b.nil_literal(p))),
                 pr(b, field(b, r, "value")),
                 set_local(b, 1, resume(b, co, b.nil_literal(p))),
                 pr(b, field(b, r, "value")), pr(b, field(b, r, "done"))},
                p),
        {"co", "r"}, {}};
    check_eq(run_module(m, "generator inside"), "", "generator inside: failure");
    check_eq(joined(), "g-coro|g-gen|done-f|true|", "generator inside output");
  }

  // --- 7. The collector sees a suspended coroutine's frames; a dropped
  // one releases them; a cycle through one is collected. ------------------
  // f() { o = {}; o.co = corocurrent(); yield nil; print "never" }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back(
        {"f", 2, 0,
         b.block({set_local(b, 1, b.object_lit({}, p)),
                  b.set_index(loc(b, 1), str(b, "co"),
                              b.intrinsic(I::CoroCurrent, {}, p), p),
                  b.intrinsic(I::CoroYield, {b.nil_literal(p)}, p),
                  pr(b, str(b, "never"))},
                 p),
         {"x", "o"}, {}});
    m.funcs.back().num_params = 1;
    const NodeId co = loc(b, 0);
    m.funcs[0] = {
        "main", 1, 0,
        b.block({set_local(b, 0, b.intrinsic(I::CoroCreate,
                                             {b.make_closure(1, 0, p)}, p)),
                 resume(b, co, b.nil_literal(p)),
                 // Reachable from the local: a collection frees nothing.
                 pr(b, b.intrinsic(I::Collect, {}, p)),
                 // Now only the cycle (coroutine -> parked o -> coroutine)
                 // holds it: the collector frees exactly those two.
                 set_local(b, 0, b.nil_literal(p)),
                 pr(b, b.intrinsic(I::Collect, {}, p))},
                p),
        {"co"}, {}};
    check_eq(run_module(m, "gc"), "", "gc: failure");
    check_eq(joined(), "0|2|", "gc output");
  }

  // --- 8. Two coroutines interleaved by their resumer. --------------------
  // count(x) { yield x; yield x + 1; return x + 2 }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back(
        {"count", 1, 0,
         b.block({b.intrinsic(I::CoroYield, {loc(b, 0)}, p),
                  b.intrinsic(I::CoroYield,
                              {b.binary(BinOp::Add, loc(b, 0), b.literal(1, p), p)},
                              p),
                  b.make_return(
                      b.binary(BinOp::Add, loc(b, 0), b.literal(2, p), p), p)},
                 p),
         {"x"}, {}});
    m.funcs.back().num_params = 1;
    const NodeId a = loc(b, 0);
    const NodeId c = loc(b, 1);
    std::vector<NodeId> stmts{
        set_local(b, 0, b.intrinsic(I::CoroCreate, {b.make_closure(1, 0, p)}, p)),
        set_local(b, 1, b.intrinsic(I::CoroCreate, {b.make_closure(1, 0, p)}, p))};
    for (int i = 0; i < 3; ++i) {
      stmts.push_back(pr(b, field(b, resume(b, a, b.literal(1, p)), "value")));
      stmts.push_back(pr(b, field(b, resume(b, c, b.literal(10, p)), "value")));
    }
    m.funcs[0] = {"main", 2, 0, b.block(stmts, p), {"a", "c"}, {}};
    check_eq(run_module(m, "interleave"), "", "interleave: failure");
    check_eq(joined(), "1|10|2|11|3|12|", "interleave output");
  }

  // --- 9. A native as the coroutine's function completes on the spot. ---
  {
    Module m;
    Builder b(m);
    const NodeId co = loc(b, 0);
    const NodeId r = loc(b, 1);
    m.funcs.push_back(
        {"main", 2, 0,
         b.block({set_local(b, 0, b.intrinsic(I::CoroCreate,
                                              {b.native_ref(
                                                  b.declare_native("sum"), p)},
                                              p)),
                  set_local(b, 1, resume(b, co, b.literal(5, p))),
                  pr(b, field(b, r, "value")), pr(b, field(b, r, "done")),
                  pr(b, status(b, co))},
                 p),
         {"co", "r"}, {}});
    check_eq(run_module(m, "native fn"), "", "native fn: failure");
    check_eq(joined(), "5|true|done|", "native fn output");
  }

  // --- 10. CoroCreate wants a callable. -----------------------------------
  {
    Module m;
    Builder b(m);
    m.funcs.push_back({"main", 0, 0,
                       pr(b, b.intrinsic(I::CoroCreate, {b.literal(3, p)}, p)),
                       {}, {}});
    check_eq(run_module(m, "not callable"),
             "coroutine needs a function, not int", "not callable: failure");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("coroutines: OK");
  return 0;
}
