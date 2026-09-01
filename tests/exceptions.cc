// Throw, TryCatch, and the unwinder: that a handler actually resumes
// execution, that a trap is catchable the same way, and -- the property
// throw_safety.cc pins for the uncaught case -- that an unwind releases
// every value in every frame it crosses. Each case asserts zero live heap
// objects afterward, so a slot the unwinder skipped fails here rather than
// in a leak checker.

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
int64_t coreir_rt_in(int64_t, int64_t) { return 0; }
void coreir_rt_poll(void) {}
[[noreturn]] void coreir_rt_fail(const char* msg, int64_t, int64_t) {
  throw Failure(msg);
}
}

int main() {
  using namespace coreir;
  const SrcPos p{1, 1};

  // --- 1. A caught throw resumes: the handler runs, and so does what
  //        follows the try. ------------------------------------------------
  // print (try { throw "boom"; "not reached" } catch e { e }); print "after"
  {
    Module m;
    Builder b(m);
    const NodeId body = b.block({b.make_throw(b.str_literal("boom", p), p),
                                 b.str_literal("not reached", p)},
                                p);
    m.funcs.push_back(
        {"main", 1, 0,
         b.block({b.intrinsic(IntrinsicId::Print,
                              {b.make_try(0, body,
                                          b.varref(VarKind::Local, 0, p), p)},
                              p),
                  b.intrinsic(IntrinsicId::Print,
                              {b.str_literal("after", p)}, p)},
                 p),
         {"e"},
         {}});
    check_eq(run_module(m, "catch resumes"), "", "catch resumes: failure");
    check_eq(joined(), "boom|after|", "catch resumes output");
  }

  // --- 2. A trap is catchable, across a frame: the callee divides by zero,
  //        the caller's handler reads the {message, line, col} object. -----
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    const NodeId handler = b.block(
        {b.intrinsic(IntrinsicId::Print,
                     {b.index(b.varref(VarKind::Local, 0, p),
                              b.str_literal("message", p), p)},
                     p),
         b.intrinsic(IntrinsicId::Print,
                     {b.index(b.varref(VarKind::Local, 0, p),
                              b.str_literal("line", p), p)},
                     p)},
        p);
    m.funcs.push_back(
        {"main", 1, 0,
         b.make_try(0, b.call_value(b.make_closure(1, 0, p), {}, p), handler,
                    p),
         {"e"},
         {}});
    m.funcs.push_back(
        {"div", 0, 0,
         b.intrinsic(IntrinsicId::Print,
                     {b.binary(BinOp::Div, b.literal(1, p),
                               b.literal(0, {7, 3}), p)},
                     p),
         {},
         {}});
    check_eq(run_module(m, "trap caught"), "", "trap caught: failure");
    check_eq(joined(), "divide by zero|7|", "trap caught output");
  }

  // --- 3. Uncaught: a user throw reaches coreir_rt_fail as
  //        "uncaught: <value>"; the unwinder has already freed both frames
  //        (the zero-live assert). -----------------------------------------
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back(
        {"main", 1, 0,
         b.block({b.assign(VarKind::Local, 0,
                           b.str_literal("live in main", p), p),
                  b.call_value(b.make_closure(1, 0, p), {}, p)},
                 p),
         {"s"},
         {}});
    m.funcs.push_back(
        {"thrower", 1, 0,
         b.block({b.assign(VarKind::Local, 0,
                           b.str_literal("live in thrower", p), p),
                  b.make_throw(b.str_literal("boom", p), p)},
                 p),
         {"t"},
         {}});
    check_eq(run_module(m, "uncaught throw"), "uncaught: boom",
             "uncaught throw: message");
  }

  // --- 4. An uncaught trap keeps its original diagnostic, byte for byte
  //        (what PL/0's error transcripts rely on). ------------------------
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 0, 0,
         b.intrinsic(IntrinsicId::Print,
                     {b.binary(BinOp::Div, b.literal(1, p), b.literal(0, p),
                               p)},
                     p),
         {},
         {}});
    check_eq(run_module(m, "uncaught trap"), "divide by zero",
             "uncaught trap: message");
  }

  // --- 5. Nested: the inner handler rethrows; the outer one catches. ------
  // try { try { throw "x" } catch e { throw e } } catch f { print f }
  {
    Module m;
    Builder b(m);
    const NodeId inner =
        b.make_try(0, b.make_throw(b.str_literal("x", p), p),
                   b.make_throw(b.varref(VarKind::Local, 0, p), p), p);
    m.funcs.push_back(
        {"main", 2, 0,
         b.intrinsic(IntrinsicId::Print,
                     {b.make_try(1, inner, b.varref(VarKind::Local, 1, p),
                                 p)},
                     p),
         {"e", "f"},
         {}});
    check_eq(run_module(m, "rethrow"), "", "rethrow: failure");
    check_eq(joined(), "x|", "rethrow output");
  }

  // --- 6. A throw inside a handler is not caught by its own try. ----------
  // try { (try { throw "a" } catch e { throw "from handler" }) }
  // catch f { print f }
  {
    Module m;
    Builder b(m);
    const NodeId inner =
        b.make_try(0, b.make_throw(b.str_literal("a", p), p),
                   b.make_throw(b.str_literal("from handler", p), p), p);
    m.funcs.push_back(
        {"main", 2, 0,
         b.intrinsic(IntrinsicId::Print,
                     {b.make_try(1, inner, b.varref(VarKind::Local, 1, p),
                                 p)},
                     p),
         {"e", "f"},
         {}});
    check_eq(run_module(m, "handler throws"), "", "handler throws: failure");
    check_eq(joined(), "from handler|", "handler throws output");
  }

  // --- 7. Unwinding crosses a Scope: its local is released (back to
  //        Uninit), and live heap values everywhere on the path go with
  //        their frames. The handler still runs and the loop's break path
  //        still works afterwards. -----------------------------------------
  // try { while true { scope(s) { s = "held"; throw "out" } } }
  // catch e { print e }
  {
    Module m;
    Builder b(m);
    const NodeId loop_body = b.scope(
        1, 2,
        b.block({b.assign(VarKind::Local, 1, b.str_literal("held", p), p),
                 b.make_throw(b.str_literal("out", p), p)},
                p),
        p);
    m.funcs.push_back(
        {"main", 2, 0,
         b.intrinsic(IntrinsicId::Print,
                     {b.make_try(0,
                                 b.make_while(b.bool_literal(true, p),
                                              loop_body, p),
                                 b.varref(VarKind::Local, 0, p), p)},
                     p),
         {"e", "s"},
         {}});
    check_eq(run_module(m, "throw across scope"), "",
             "throw across scope: failure");
    check_eq(joined(), "out|", "throw across scope output");
  }

  // --- 8. What verify() refuses. ------------------------------------------
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 0, 0,
         b.make_try(0, b.literal(1, p), b.literal(2, p), p), {}, {}});
    const auto err = coreir::verify(m);
    check_eq(err ? *err : "", "caught local slot out of range",
             "caught slot: verify message");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "exceptions: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("exceptions OK\n");
  return 0;
}
