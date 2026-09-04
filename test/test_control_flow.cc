// Return, Break, Continue: the three non-local exits, and what they owe the
// regions they jump out of. Output order pins where control actually went;
// the zero-live assert at the end of every case pins that leaving a Scope
// sideways released the same locals leaving it normally would.

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

  // --- 1. Break leaves the loop; what follows the loop still runs. --------
  // i = 0; while true { print i; if i == 2 { break }; i = i + 1 }; print 9
  {
    Module m;
    Builder b(m);
    const NodeId body = b.block(
        {b.intrinsic(IntrinsicId::Print, {b.varref(VarKind::Local, 0, p)}, p),
         b.make_if(b.binary(BinOp::Eq, b.varref(VarKind::Local, 0, p),
                            b.literal(2, p), p),
                   b.make_break(p), NodeId{}, p),
         b.assign(VarKind::Local, 0,
                  b.binary(BinOp::Add, b.varref(VarKind::Local, 0, p),
                           b.literal(1, p), p),
                  p)},
        p);
    m.funcs.push_back(
        {"main", 1, 0,
         b.block({b.assign(VarKind::Local, 0, b.literal(0, p), p),
                  b.make_while(b.bool_literal(true, p), body, p),
                  b.intrinsic(IntrinsicId::Print, {b.literal(9, p)}, p)},
                 p),
         {"i"},
         {}});
    check_eq(run_module(m, "break"), "", "break: unexpected failure");
    check_eq(joined(), "0|1|2|9|", "break output");
  }

  // --- 2. Continue skips the rest of the body, not the loop. --------------
  // i = 0; while i < 5 { i = i + 1; if i mod 2 == 1 { continue }; print i }
  {
    Module m;
    Builder b(m);
    const NodeId body = b.block(
        {b.assign(VarKind::Local, 0,
                  b.binary(BinOp::Add, b.varref(VarKind::Local, 0, p),
                           b.literal(1, p), p),
                  p),
         b.make_if(b.binary(BinOp::Eq,
                            b.binary(BinOp::Mod,
                                     b.varref(VarKind::Local, 0, p),
                                     b.literal(2, p), p),
                            b.literal(1, p), p),
                   b.make_continue(p), NodeId{}, p),
         b.intrinsic(IntrinsicId::Print, {b.varref(VarKind::Local, 0, p)},
                     p)},
        p);
    m.funcs.push_back(
        {"main", 1, 0,
         b.block({b.assign(VarKind::Local, 0, b.literal(0, p), p),
                  b.make_while(b.binary(BinOp::Lt,
                                        b.varref(VarKind::Local, 0, p),
                                        b.literal(5, p), p),
                               body, p)},
                 p),
         {"i"},
         {}});
    check_eq(run_module(m, "continue"), "", "continue: unexpected failure");
    check_eq(joined(), "2|4|", "continue output");
  }

  // --- 3. Return hands its value back and skips the rest of the body. -----
  // f() { return 42; print 7 }  main { print f() }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back(
        {"main", 0, 0,
         b.intrinsic(IntrinsicId::Print,
                     {b.call_value(b.make_closure(1, 0, p), {}, p)}, p),
         {},
         {}});
    m.funcs.push_back(
        {"f", 0, 0,
         b.block({b.make_return(b.literal(42, p), p),
                  b.intrinsic(IntrinsicId::Print, {b.literal(7, p)}, p)},
                 p),
         {},
         {}});
    check_eq(run_module(m, "return"), "", "return: unexpected failure");
    check_eq(joined(), "42|", "return output");
  }

  // --- 4. A bare Return returns nil. --------------------------------------
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back(
        {"main", 0, 0,
         b.intrinsic(IntrinsicId::Print,
                     {b.call_value(b.make_closure(1, 0, p), {}, p)}, p),
         {},
         {}});
    m.funcs.push_back({"f", 0, 0, b.make_return(NodeId{}, p), {}, {}});
    check_eq(run_module(m, "bare return"), "", "bare return: failure");
    check_eq(joined(), "nil|", "bare return output");
  }

  // --- 5. Return from deep inside a loop, out of a Scope holding a heap
  //        value: the frame goes at once, and nothing leaks (the case-wide
  //        zero-live assert is the point).
  // f() { while true { scope(s) { s = "held"; return 1 } } }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back(
        {"main", 0, 0,
         b.intrinsic(IntrinsicId::Print,
                     {b.call_value(b.make_closure(1, 0, p), {}, p)}, p),
         {},
         {}});
    const NodeId body = b.scope(
        0, 1,
        b.block({b.assign(VarKind::Local, 0, b.str_literal("held", p), p),
                 b.make_return(b.literal(1, p), p)},
                p),
        p);
    m.funcs.push_back(
        {"f", 1, 0, b.make_while(b.bool_literal(true, p), body, p),
         {"s"},
         {}});
    check_eq(run_module(m, "return out of scope"), "",
             "return out of scope: failure");
    check_eq(joined(), "1|", "return out of scope output");
  }

  // --- 6. Break out of a Scope releases the scope's local, at the break. --
  // Proven by reading it after the loop: the slot is back to Uninit, so the
  // read fails the same way a read before first assignment does.
  // while true { scope(s) { s = "held"; break } }; print s
  {
    Module m;
    Builder b(m);
    const NodeId body = b.scope(
        0, 1,
        b.block({b.assign(VarKind::Local, 0, b.str_literal("held", p), p),
                 b.make_break(p)},
                p),
        p);
    m.funcs.push_back(
        {"main", 1, 0,
         b.block({b.make_while(b.bool_literal(true, p), body, p),
                  b.intrinsic(IntrinsicId::Print,
                              {b.varref(VarKind::Local, 0, p)}, p)},
                 p),
         {"s"},
         {}});
    check_eq(run_module(m, "break clears scope"), "uninitialized variable 's'",
             "break clears scope: message");
    check_eq(joined(), "", "break clears scope output");
  }

  // --- 7. What verify() refuses. ------------------------------------------
  {
    Module m;
    Builder b(m);
    m.funcs.push_back({"main", 0, 0, b.make_break(p), {}, {}});
    const auto err = coreir::verify(m);
    check_eq(err ? *err : "", "Break outside a loop body",
             "break outside loop: verify message");
  }
  {
    // A function boundary ends the loop context: a Break in a closure's own
    // body has no loop, even when every call site sits inside one.
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back(
        {"main", 0, 0,
         b.make_while(b.bool_literal(true, p),
                      b.call_value(b.make_closure(1, 0, p), {}, p), p),
         {},
         {}});
    m.funcs.push_back({"f", 0, 0, b.make_break(p), {}, {}});
    const auto err = coreir::verify(m);
    check_eq(err ? *err : "", "Break outside a loop body",
             "break across a function: verify message");
  }

  // --- 8. A labeled Break leaves two loops at once, and releases the inner
  // Scope's local on the way out; a labeled Continue re-enters the outer
  // loop's test. -------------------------------------------------------
  // d(x) { print "drop" }
  // main {
  //   i = 0
  //   while i < 3 { scope(1, 3) { o = {drop: d}; j = 0
  //     while j < 3 { if j == 1 { break ^1 }; print j; j = j + 1 } }
  //     i = i + 1 }
  //   print "after"
  //   i = 0
  //   while i < 2 { i = i + 1; j = 0
  //     while j < 3 { if j == 1 { continue ^1 }; print i * 10 + j; j = j + 1 }
  //     print "never" }
  //   print "end"
  // }
  {
    Module m;
    Builder b(m);
    const SrcPos p{8, 1};
    auto lit = [&](int64_t v) { return b.literal(v, p); };
    auto loc = [&](int32_t i) { return b.varref(VarKind::Local, i, p); };
    auto set = [&](int32_t i, NodeId v) {
      return b.assign(VarKind::Local, i, v, p);
    };
    auto say = [&](const char* s) {
      return b.intrinsic(IntrinsicId::Print, {b.str_literal(s, p)}, p);
    };
    auto pr = [&](NodeId v) { return b.intrinsic(IntrinsicId::Print, {v}, p); };
    auto add = [&](NodeId x, NodeId y) { return b.binary(BinOp::Add, x, y, p); };
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back({"d", 1, 0, say("drop"), {"x"}, {}});
    m.funcs.back().num_params = 1;
    // locals: 0 i, 1 o, 2 j
    const NodeId first = b.make_while(
        b.binary(BinOp::Lt, loc(0), lit(3), p),
        b.block(
            {b.scope(1, 3,
                     b.block({set(1, b.object_lit({{b.str_literal("\x01" "drop", p),
                                                    b.varref(VarKind::Cell, 0, p)}},
                                                  p)),
                              set(2, lit(0)),
                              b.make_while(
                                  b.binary(BinOp::Lt, loc(2), lit(3), p),
                                  b.block({b.make_if(b.binary(BinOp::Eq, loc(2),
                                                              lit(1), p),
                                                     b.make_break(p, 1),
                                                     NodeId{}, p),
                                           pr(loc(2)), set(2, add(loc(2), lit(1)))},
                                          p),
                                  p)},
                             p),
                     p),
             set(0, add(loc(0), lit(1)))},
            p),
        p);
    const NodeId second = b.make_while(
        b.binary(BinOp::Lt, loc(0), lit(2), p),
        b.block({set(0, add(loc(0), lit(1))), set(2, lit(0)),
                 b.make_while(
                     b.binary(BinOp::Lt, loc(2), lit(3), p),
                     b.block({b.make_if(b.binary(BinOp::Eq, loc(2), lit(1), p),
                                        b.make_continue(p, 1), NodeId{}, p),
                              pr(add(b.binary(BinOp::Mul, loc(0), lit(10), p),
                                     loc(2))),
                              set(2, add(loc(2), lit(1)))},
                             p),
                     p),
                 say("never")},
                p),
        p);
    m.funcs[0] = {"main", 3, 0,
                  b.block({b.assign(VarKind::Cell, 0, b.make_closure(1, 0, p), p),
                           set(0, lit(0)), first, say("after"), set(0, lit(0)),
                           second, say("end")},
                          p),
                  {"i", "o", "j"}, {}};
    m.funcs[0].num_cells = 1;
    check_eq(run_module(m, "labeled"), "", "labeled: unexpected failure");
    check_eq(joined(), "0|drop|after|10|20|end|", "labeled output");
  }
  // A depth that names no open loop is verify()'s to refuse.
  {
    Module m;
    Builder b(m);
    const SrcPos p{9, 1};
    m.funcs.push_back({"main", 0, 0,
                       b.make_while(b.bool_literal(true, p), b.make_break(p, 1),
                                    p),
                       {}, {}});
    const auto err = verify(m);
    check_eq(err ? *err : "", "Break names a loop that is not open",
             "labeled: verify");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "control_flow: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("control_flow OK\n");
  return 0;
}
