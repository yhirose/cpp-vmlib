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

void check(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
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

  // --- 9. A condition built of `&&`, `||` and `!` -- the If(x, y, false),
  // If(x, true, y) and Not that FuncWriter's both()/either() and every
  // front end's `!` write -- decides what the value it stands for would,
  // evaluating exactly what that value would have evaluated, in the same
  // order. Every shape compile_branch takes apart, over every pair of
  // operands drawn from both sides of the VM's truthiness (0 and nil are
  // false, "" is true), as an If statement, an If value and a While. ------
  {
    struct Operand {
      const char* name;
      bool truth;
    };
    const Operand ops[] = {{"false", false}, {"true", true}, {"0", false},
                           {"0.0", false},   {"nil", false},  {"\"\"", true}};
    for (const Operand& x : ops) {
      for (const Operand& y : ops) {
        Module m;
        Builder b(m);
        const auto lit = [&](const Operand& o) {
          const std::string n = o.name;
          if (n == "false" || n == "true") return b.bool_literal(n == "true", p);
          if (n == "0") return b.literal(0, p);
          if (n == "0.0") return b.double_literal(0.0, p);
          if (n == "nil") return b.nil_literal(p);
          return b.str_literal("", p);
        };
        const auto X = [&] { return b.varref(VarKind::Local, 0, p); };
        const auto Y = [&] { return b.varref(VarKind::Local, 1, p); };
        const auto I = [&] { return b.varref(VarKind::Local, 2, p); };
        const auto say = [&](const char* s) {
          return b.intrinsic(IntrinsicId::Print, {b.str_literal(s, p)}, p);
        };
        // `v`, once `s` has been printed: which operands ran, and when.
        const auto seen = [&](const char* s, NodeId v) {
          return b.block({say(s), v}, p);
        };
        const auto both = [&](NodeId l, NodeId r) {
          return b.make_if(l, r, b.bool_literal(false, p), p);
        };
        const auto either = [&](NodeId l, NodeId r) {
          return b.make_if(l, b.bool_literal(true, p), r, p);
        };
        const auto stmt = [&](NodeId c) {
          return b.make_if(c, say("T"), say("F"), p);
        };
        const auto value = [&](NodeId c) {
          return b.intrinsic(IntrinsicId::Print,
                             {b.make_if(c, b.str_literal("T", p),
                                        b.str_literal("F", p), p)},
                             p);
        };
        const NodeId body = b.block(
            {b.assign(VarKind::Local, 0, lit(x), p),
             b.assign(VarKind::Local, 1, lit(y), p),
             value(both(X(), seen("y", Y()))),
             stmt(either(X(), seen("y", Y()))),
             stmt(b.unary(UnOp::Not, both(X(), seen("y", Y())), p)),
             // No else: nil when x is false.
             stmt(b.make_if(X(), seen("y", Y()), NodeId{}, p)),
             // Neither arm a literal.
             value(b.make_if(X(), seen("y", Y()),
                             seen("z", b.unary(UnOp::Not, Y(), p)), p)),
             stmt(either(both(X(), Y()), b.unary(UnOp::Not, X(), p))),
             // A literal condition, of each kind; an arm that is a
             // statement (nil, false), and one that leaves; and a Block
             // that decides by its last child.
             stmt(both(lit(y), X())),
             stmt(either(lit(y), X())),
             stmt(either(X(), b.block({say("a"),
                                       b.assign(VarKind::Local, 2,
                                                b.literal(0, p), p)},
                                      p))),
             stmt(either(X(), b.block({say("s"), b.make_break(p)}, p))),
             b.assign(VarKind::Local, 2, b.literal(0, p), p),
             b.make_while(
                 both(b.binary(BinOp::Lt, I(), b.literal(2, p), p), X()),
                 b.assign(VarKind::Local, 2,
                          b.binary(BinOp::Add, I(), b.literal(1, p), p), p),
                 p),
             b.intrinsic(IntrinsicId::Print, {I()}, p)},
            p);
        // The arm that is a statement is a Break, so it needs a loop to
        // name: the whole body runs once inside one.
        m.funcs.push_back(
            {"main", 3, 0,
             b.make_while(b.bool_literal(true, p),
                          b.block({body, b.make_break(p)}, p), p),
             {"x", "y", "i"}, {}});
        const std::string what =
            std::string("branch on ") + x.name + ", " + y.name;
        check_eq(run_module(m, what), "", what + ": unexpected failure");
        const auto tf = [](bool v) { return v ? "T|" : "F|"; };
        std::string want;
        if (x.truth) want += "y|";
        want += tf(x.truth && y.truth);
        if (!x.truth) want += "y|";
        want += tf(x.truth || y.truth);
        if (x.truth) want += "y|";
        want += tf(!(x.truth && y.truth));
        if (x.truth) want += "y|";
        want += tf(x.truth && y.truth);
        want += x.truth ? "y|" : "z|";
        want += tf(x.truth ? y.truth : !y.truth);
        want += tf((x.truth && y.truth) || !x.truth);
        want += tf(y.truth && x.truth);
        want += tf(y.truth || x.truth);
        if (!x.truth) want += "a|";
        want += tf(x.truth);
        if (!x.truth) want += "s|";
        else want += "T|";
        if (x.truth) want += "2|";
        check_eq(joined(), want, what + ": output");
      }
    }
  }

  // ... and costs what the hand-nested ifs would: each test jumps straight
  // to where its answer is decided, so no bool is materialized for a
  // second test, `||` jumps on true, `!` flips the jump rather than
  // computing a Not, and a literal condition is no test at all.
  {
    Module m;
    Builder b(m);
    const auto L = [&](int32_t i) { return b.varref(VarKind::Local, i, p); };
    const auto say = [&] {
      return b.intrinsic(IntrinsicId::Print, {b.literal(1, p)}, p);
    };
    // Each shape as a statement, so that nothing moves its value anywhere.
    const auto dump_of = [&](NodeId stmt) {
      m.funcs.clear();
      m.funcs.push_back({"main", 2, 0, b.block({stmt, b.nil_literal(p)}, p),
                         {"a", "b"}, {}});
      check_eq(verify(m).value_or(""), "", "branch shape: verify");
      return vm::to_string(vm::compile(m));
    };
    const auto count = [](const std::string& s, const std::string& what) {
      size_t n = 0;
      for (size_t at = s.find(what); at != std::string::npos;
           at = s.find(what, at + 1)) {
        ++n;
      }
      return n;
    };
    const std::string both = dump_of(b.make_if(
        b.make_if(L(0), L(1), b.bool_literal(false, p), p), say(), NodeId{},
        p));
    // Where each `jumpiffalse rN, T` goes.
    const auto targets = [](const std::string& s) {
      std::vector<std::string> out;
      const std::string op = "\tjumpiffalse r";
      for (size_t at = s.find(op); at != std::string::npos;
           at = s.find(op, at + 1)) {
        const size_t from = s.find(", ", at) + 2;
        out.push_back(s.substr(from, s.find('\t', from) - from));
      }
      return out;
    };
    const std::vector<std::string> ends = targets(both);
    check(ends.size() == 2 && ends[0] == ends[1],
          "a && b: either operand false jumps past the body");
    check(both.find("\tmove ") == std::string::npos &&
              both.find(", false\t") == std::string::npos,
          "a && b: no bool built between the tests");
    const std::string either = dump_of(b.make_if(
        b.make_if(L(0), b.bool_literal(true, p), L(1), p), say(), NodeId{},
        p));
    check_eq(std::to_string(count(either, "\tjumpiftrue ")), "1",
             "a || b: a true jumps to the body");
    check(either.find("\tmove ") == std::string::npos &&
              either.find(", true\t") == std::string::npos,
          "a || b: no bool built between the tests");
    const std::string negated = dump_of(
        b.make_if(b.unary(UnOp::Not, L(0), p), say(), NodeId{}, p));
    check(negated.find("\tnot ") == std::string::npos &&
              count(negated, "\tjumpiftrue ") == 1,
          "!a: the jump flips, no Not");
    const std::string forever = dump_of(b.make_while(
        b.bool_literal(true, p), b.make_break(p), p));
    check(forever.find("\tjumpif") == std::string::npos &&
              forever.find(", true\t") == std::string::npos,
          "while (true): no test");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "control_flow: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("control_flow OK\n");
  return 0;
}
