// Tail calls: with Func::tail_calls on, a CallValue in tail position -- a
// Return's operand, or the body's final value through Block / If / Switch /
// Scope -- replaces the frame instead of stacking on it, so a loop written
// as a call chain runs in one frame however long it goes. What this pins:
// the depth bound stops mattering (a million-deep mutual recursion under a
// 10000-frame limit); the frame exits *before* the callee runs (Rust's
// `become` rule: a local's destructor precedes the callee's output, where
// a plain call has it follow); the compiler leaves a call alone inside a
// try body and a defer-declaring scope, and the executor leaves one alone
// when the callee is a native or a generator function or the frame is the
// entry frame -- each of those keeps its ordinary meaning; and Func::
// tail_calls off changes nothing at all. Zero live heap objects after every
// case.

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

bool native_add(coreir::NativeCall& c) {
  c.result = coreir::Value::make_int(c.arg(0).as_int() + c.arg(1).as_int());
  return true;
}

// `want_left`: what may remain on the heap -- zero, except for a run that
// fails before its own cleanup (the one case below that wants the limit).
std::string run_module(const coreir::Module& m, const std::string& what,
                       int64_t want_left = 0) {
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
    opts.natives = {{"add", 2, &native_add, nullptr}};
    try {
      vm::run(p, rt, opts);
    } catch (const Failure& e) {
      failure = e.what();
    }
    left = rt.live_objects();
  }
  if (left != want_left) {
    std::fprintf(stderr, "FAIL: %s: %lld heap object(s) left, expected %lld\n",
                 what.c_str(), static_cast<long long>(left),
                 static_cast<long long>(want_left));
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
  constexpr int64_t kDeep = 1000000;  // a hundred times the frame limit

  auto pr = [&p](Builder& b, NodeId v) {
    return b.intrinsic(IntrinsicId::Print, {v}, p);
  };
  auto cap = [&p](Builder& b, int32_t i) {
    return b.varref(VarKind::Capture, i, p);
  };
  auto loc = [&p](Builder& b, int32_t i) {
    return b.varref(VarKind::Local, i, p);
  };
  auto cell = [&p](Builder& b, int32_t i) {
    return b.varref(VarKind::Cell, i, p);
  };
  auto dec = [&p](Builder& b, NodeId n) {
    return b.binary(BinOp::Sub, n, b.literal(1, p), p);
  };
  auto is_zero = [&p](Builder& b, NodeId n) {
    return b.binary(BinOp::Eq, n, b.literal(0, p), p);
  };

  // --- 1. Mutual recursion, a million deep, through an If in the body. ---
  // even(n) = n == 0 ? true : odd(n - 1);  odd(n) = n == 0 ? false : even(n-1)
  // Both closures live in main's cells and capture both cells.
  auto even_odd = [&](bool tail) {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({{VarKind::Cell, 0}, {VarKind::Cell, 1}});
    m.funcs.push_back({});
    // captures: 0 = even, 1 = odd
    m.funcs.push_back({"even", 1, 2,
                       b.make_if(is_zero(b, loc(b, 0)), b.bool_literal(true, p),
                                 b.call_value(cap(b, 1), {dec(b, loc(b, 0))}, p),
                                 p),
                       {"n"}, {"even", "odd"}});
    m.funcs.back().num_params = 1;
    m.funcs.back().tail_calls = tail;
    m.funcs.push_back({"odd", 1, 2,
                       b.make_if(is_zero(b, loc(b, 0)), b.bool_literal(false, p),
                                 b.call_value(cap(b, 0), {dec(b, loc(b, 0))}, p),
                                 p),
                       {"n"}, {"even", "odd"}});
    m.funcs.back().num_params = 1;
    m.funcs.back().tail_calls = tail;
    // The two closures and the two cells form a cycle; main breaks it at
    // the end so the heap check below measures the calls, not the cycle.
    m.funcs[0] = {"main", 0, 0,
                  b.block({b.assign(VarKind::Cell, 0, b.make_closure(1, 0, p), p),
                           b.assign(VarKind::Cell, 1, b.make_closure(2, 0, p), p),
                           pr(b, b.call_value(cell(b, 0), {b.literal(kDeep, p)},
                                              p)),
                           b.assign(VarKind::Cell, 0, b.nil_literal(p), p),
                           b.assign(VarKind::Cell, 1, b.nil_literal(p), p)},
                          p),
                  {}, {}};
    m.funcs[0].num_cells = 2;
    return m;
  };
  check_eq(run_module(even_odd(true), "even/odd"), "", "even/odd: failure");
  check_eq(joined(), "true|", "even/odd output");
  // Off, the limit trips first -- before main's cycle-breaking assignments,
  // so the two closures and two cells are still there for ~Runtime.
  check_eq(run_module(even_odd(false), "even/odd off", 4),
           "recursion limit exceeded", "even/odd off: without tail_calls");

  // --- 2. A Return inside a While inside a Scope, with an accumulator. ----
  // sum(n, acc) { while true { if n == 0 { return acc }; return sum(n-1,
  // acc+n) } }  -> 500000500000. The TailCall pops the scope's owned mark
  // and releases its locals itself.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({{VarKind::Cell, 0}});
    m.funcs.push_back({});
    m.funcs.push_back(
        {"sum", 2, 1,
         b.scope(0, 2,
                 b.make_while(
                     b.bool_literal(true, p),
                     b.block({b.make_if(is_zero(b, loc(b, 0)),
                                        b.make_return(loc(b, 1), p), NodeId{},
                                        p),
                              b.make_return(
                                  b.call_value(cap(b, 0),
                                               {dec(b, loc(b, 0)),
                                                b.binary(BinOp::Add, loc(b, 1),
                                                         loc(b, 0), p)},
                                               p),
                                  p)},
                             p),
                     p),
                 p),
         {"n", "acc"}, {"sum"}});
    m.funcs.back().num_params = 2;
    m.funcs.back().tail_calls = true;
    m.funcs[0] = {"main", 0, 0,
                  b.block({b.assign(VarKind::Cell, 0, b.make_closure(1, 0, p), p),
                           pr(b, b.call_value(cell(b, 0),
                                              {b.literal(kDeep, p),
                                               b.literal(0, p)},
                                              p)),
                           b.assign(VarKind::Cell, 0, b.nil_literal(p), p)},
                          p),
                  {}, {}};
    m.funcs[0].num_cells = 1;
    check_eq(run_module(m, "sum"), "", "sum: failure");
    check_eq(joined(), "500000500000|", "sum output");
  }

  // --- 3. Through a Switch arm. -------------------------------------------
  // down(n) = switch n { 0: "done"; default: down(n - 1) }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({{VarKind::Cell, 0}});
    m.funcs.push_back({});
    m.funcs.push_back(
        {"down", 1, 1,
         b.make_switch(loc(b, 0), {{b.literal(0, p), b.str_literal("done", p)}},
                       b.call_value(cap(b, 0), {dec(b, loc(b, 0))}, p), p),
         {"n"}, {"down"}});
    m.funcs.back().num_params = 1;
    m.funcs.back().tail_calls = true;
    m.funcs[0] = {"main", 0, 0,
                  b.block({b.assign(VarKind::Cell, 0, b.make_closure(1, 0, p), p),
                           pr(b, b.call_value(cell(b, 0), {b.literal(kDeep, p)},
                                              p)),
                           b.assign(VarKind::Cell, 0, b.nil_literal(p), p)},
                          p),
                  {}, {}};
    m.funcs[0].num_cells = 1;
    check_eq(run_module(m, "switch"), "", "switch: failure");
    check_eq(joined(), "done|", "switch output");
  }

  // --- 4. The frame exits before the callee: a local's destructor first. --
  // g() { scope { o = {drop: d}; return f() } }   f() { print "f" }
  // d(x) { print "drop" }.  On: drop, f.  Off: f, drop.
  auto drop_order = [&](bool tail) {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.capture_maps.push_back({{VarKind::Cell, 0}, {VarKind::Cell, 1}});
    m.funcs.push_back({});
    m.funcs.push_back({"f", 0, 0, pr(b, b.str_literal("f", p)), {}, {}});
    m.funcs.push_back({"d", 1, 0, pr(b, b.str_literal("drop", p)), {"x"}, {}});
    m.funcs.back().num_params = 1;
    // captures: 0 = f, 1 = d
    m.funcs.push_back(
        {"g", 1, 2,
         b.scope(0, 1,
                 b.block({b.assign(VarKind::Local, 0,
                                   b.object_lit({{b.str_literal("\x01" "drop", p),
                                                  cap(b, 1)}},
                                                p),
                                   p),
                          b.make_return(b.call_value(cap(b, 0), {}, p), p)},
                         p),
                 p),
         {"o"}, {"f", "d"}});
    m.funcs.back().tail_calls = tail;
    m.funcs[0] = {"main", 0, 0,
                  b.block({b.assign(VarKind::Cell, 0, b.make_closure(1, 0, p), p),
                           b.assign(VarKind::Cell, 1, b.make_closure(2, 0, p), p),
                           b.call_value(b.make_closure(3, 1, p), {}, p),
                           pr(b, b.str_literal("end", p))},
                          p),
                  {}, {}};
    m.funcs[0].num_cells = 2;
    return m;
  };
  check_eq(run_module(drop_order(true), "drop order"), "",
           "drop order: failure");
  check_eq(joined(), "drop|f|end|", "drop order: exit precedes the callee");
  check_eq(run_module(drop_order(false), "drop order off"), "",
           "drop order off: failure");
  check_eq(joined(), "f|drop|end|", "drop order off: callee first");

  // --- 4b. A scope that named its own release order keeps it. ---------------
  // The one thing tail_calls is allowed to change is *when* the frame exits
  // (case 4). A Scope built with an explicit release list also fixes *what
  // order* its slots go in -- ascending here, the opposite of the default
  // last-slot-first -- and TailCall's own exit is a blanket release_range
  // that knows neither that order nor a cell refresh. So a scope naming one
  // keeps the ordinary call: same order on and off, only the exit moves.
  // g() { scope[release a, b] { a = {drop:d1}; b = {drop:d2}; return f() } }
  auto release_order = [&](bool tail) {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.capture_maps.push_back(
        {{VarKind::Cell, 0}, {VarKind::Cell, 1}, {VarKind::Cell, 2}});
    m.funcs.push_back({});
    m.funcs.push_back({"f", 0, 0, pr(b, b.str_literal("f", p)), {}, {}});
    m.funcs.push_back({"d1", 1, 0, pr(b, b.str_literal("dropA", p)), {"x"}, {}});
    m.funcs.back().num_params = 1;
    m.funcs.push_back({"d2", 1, 0, pr(b, b.str_literal("dropB", p)), {"x"}, {}});
    m.funcs.back().num_params = 1;
    // captures: 0 = f, 1 = d1, 2 = d2. Locals: 0 = a, 1 = b.
    auto hook = [&](Builder& bb, int32_t cap_i) {
      return bb.object_lit({{bb.str_literal("\x01" "drop", p), cap(bb, cap_i)}}, p);
    };
    m.funcs.push_back(
        {"g", 2, 3,
         b.scope(0, 2,
                 b.block({b.assign(VarKind::Local, 0, hook(b, 1), p),
                          b.assign(VarKind::Local, 1, hook(b, 2), p),
                          b.make_return(b.call_value(cap(b, 0), {}, p), p)},
                         p),
                 {loc(b, 0), loc(b, 1)},  // ascending: a then b
                 p),
         {"a", "b"}, {"f", "d1", "d2"}});
    m.funcs.back().tail_calls = tail;
    m.funcs[0] = {"main", 0, 0,
                  b.block({b.assign(VarKind::Cell, 0, b.make_closure(1, 0, p), p),
                           b.assign(VarKind::Cell, 1, b.make_closure(2, 0, p), p),
                           b.assign(VarKind::Cell, 2, b.make_closure(3, 0, p), p),
                           b.call_value(b.make_closure(4, 1, p), {}, p),
                           pr(b, b.str_literal("end", p))},
                          p),
                  {}, {}};
    m.funcs[0].num_cells = 3;
    return m;
  };
  check_eq(run_module(release_order(true), "release order"), "",
           "release order: failure");
  check_eq(joined(), "f|dropA|dropB|end|",
           "release order: the declared order, tail_calls on");
  check_eq(run_module(release_order(false), "release order off"), "",
           "release order off: failure");
  check_eq(joined(), "f|dropA|dropB|end|",
           "release order off: the same declared order");

  // --- 5. Left alone inside a try body: the handler survives the call. ---
  // h() { try { return thrower() } catch e { print "caught " + e } }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.capture_maps.push_back({{VarKind::Cell, 0}});
    m.funcs.push_back({});
    m.funcs.push_back({"thrower", 0, 0,
                       b.make_throw(b.str_literal("x", p), p), {}, {}});
    m.funcs.push_back(
        {"h", 1, 1,
         b.make_try(0, b.make_return(b.call_value(cap(b, 0), {}, p), p),
                    pr(b, b.binary(BinOp::Add, b.str_literal("caught ", p),
                                   loc(b, 0), p)),
                    p),
         {"e"}, {"thrower"}});
    m.funcs.back().tail_calls = true;
    m.funcs[0] = {"main", 0, 0,
                  b.block({b.assign(VarKind::Cell, 0, b.make_closure(1, 0, p), p),
                           b.call_value(b.make_closure(2, 1, p), {}, p)},
                          p),
                  {}, {}};
    m.funcs[0].num_cells = 1;
    check_eq(run_module(m, "try"), "", "try: failure");
    check_eq(joined(), "caught x|", "try output");
  }

  // --- 6. Left alone inside a defer-declaring scope: the defer follows. --
  // k() { scope { defer dd; return f() } }   -> f, then dd
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.capture_maps.push_back({{VarKind::Cell, 0}, {VarKind::Cell, 1}});
    m.funcs.push_back({});
    m.funcs.push_back({"f", 0, 0, pr(b, b.str_literal("f", p)), {}, {}});
    m.funcs.push_back({"dd", 0, 0, pr(b, b.str_literal("defer", p)), {}, {}});
    m.funcs.push_back(
        {"k", 0, 2,
         b.scope(0, 0,
                 b.block({b.make_defer(cap(b, 1), p),
                          b.make_return(b.call_value(cap(b, 0), {}, p), p)},
                         p),
                 p),
         {}, {"f", "dd"}});
    m.funcs.back().tail_calls = true;
    m.funcs[0] = {"main", 0, 0,
                  b.block({b.assign(VarKind::Cell, 0, b.make_closure(1, 0, p), p),
                           b.assign(VarKind::Cell, 1, b.make_closure(2, 0, p), p),
                           b.call_value(b.make_closure(3, 1, p), {}, p)},
                          p),
                  {}, {}};
    m.funcs[0].num_cells = 2;
    check_eq(run_module(m, "defer"), "", "defer: failure");
    check_eq(joined(), "f|defer|", "defer output");
  }

  // --- 7. A native callee, a generator callee, and the entry frame: each
  // is the ordinary call, and the frame returns what it got. -------------
  // n() { return add(20, 22) }   gen() { return seven() } (is_generator)
  // main's own body ends in a call (tail position, entry frame).
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.capture_maps.push_back({{VarKind::Cell, 0}});
    m.capture_maps.push_back({{VarKind::Capture, 0}});  // from inside show
    m.funcs.push_back({});
    m.funcs.push_back(
        {"n", 0, 0,
         b.make_return(b.call_value(b.native_ref(b.declare_native("add"), p),
                                    {b.literal(20, p), b.literal(22, p)}, p),
                       p),
         {}, {}});
    m.funcs.back().tail_calls = true;
    m.funcs.push_back({"seven", 0, 0, b.literal(7, p), {}, {}});
    m.funcs.push_back({"gen", 0, 1,
                       b.make_return(b.call_value(cap(b, 0), {}, p), p),
                       {}, {"seven"}});
    m.funcs.back().is_generator = true;
    m.funcs.back().tail_calls = true;
    // show(): prints n(), then resumes gen and prints value and done.
    const NodeId res = loc(b, 0);
    m.funcs.push_back(
        {"show", 1, 1,
         b.block({pr(b, b.call_value(b.make_closure(1, 0, p), {}, p)),
                  b.assign(VarKind::Local, 0,
                           b.intrinsic(IntrinsicId::GenResume,
                                       {b.call_value(b.make_closure(3, 2, p), {},
                                                     p),
                                        b.nil_literal(p)},
                                       p),
                           p),
                  pr(b, b.index(res, b.str_literal("value", p), p)),
                  pr(b, b.index(res, b.str_literal("done", p), p)),
                  b.literal(0, p)},
                 p),
         {"res"}, {"seven"}});
    m.funcs[0] = {"main", 0, 0,
                  b.block({b.assign(VarKind::Cell, 0, b.make_closure(2, 0, p), p),
                           pr(b, b.call_value(b.make_closure(4, 1, p), {}, p))},
                          p),
                  {}, {}};
    m.funcs[0].num_cells = 1;
    m.funcs[0].tail_calls = true;
    check_eq(run_module(m, "ordinary"), "", "ordinary: failure");
    check_eq(joined(), "42|7|true|0|", "ordinary output");
  }

  // --- 8. A lenient callee reached by tail call: missing params are nil,
  // and ArgCount is what was passed. ----------------------------------------
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back({"le", 2, 0,
                       b.block({pr(b, loc(b, 0)), pr(b, loc(b, 1)),
                                pr(b, b.intrinsic(IntrinsicId::ArgCount, {}, p))},
                               p),
                       {"a", "b"}, {}});
    m.funcs.back().num_params = 2;
    m.funcs.back().lenient_arity = true;
    m.funcs.push_back({"caller", 0, 0,
                       b.make_return(b.call_value(b.make_closure(1, 0, p),
                                                  {b.literal(1, p)}, p),
                                     p),
                       {}, {}});
    m.funcs.back().tail_calls = true;
    m.funcs[0] = {"main", 0, 0, b.call_value(b.make_closure(2, 0, p), {}, p),
                  {}, {}};
    check_eq(run_module(m, "lenient"), "", "lenient: failure");
    check_eq(joined(), "1|nil|1|", "lenient output");
  }

  // --- 9. Arity is still checked at the replacement. ---------------------
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back({"one", 1, 0, loc(b, 0), {"a"}, {}});
    m.funcs.back().num_params = 1;
    m.funcs.push_back({"caller", 0, 0,
                       b.make_return(b.call_value(b.make_closure(1, 0, p), {}, p),
                                     p),
                       {}, {}});
    m.funcs.back().tail_calls = true;
    m.funcs[0] = {"main", 0, 0, b.call_value(b.make_closure(2, 0, p), {}, p),
                  {}, {}};
    check_eq(run_module(m, "arity"), "one takes 1 argument(s), given 0",
             "arity: failure");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("tailcalls: OK");
  return 0;
}
