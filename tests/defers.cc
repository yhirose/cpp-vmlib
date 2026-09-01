// Defer: a callable registered against its enclosing Scope, run on every
// exit path -- fall-through, Break, Continue, Return, and an unwinding
// throw -- LIFO within the scope. The cases mirror the semantics pinned
// against culebra itself (tests/test_defer_unwind_carrier.cul over there),
// with one deliberate divergence noted at case 8. Zero live heap objects
// after every case, as everywhere else.

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

// A 0-arity closure printing `text`, as funcs[idx] over an empty capture
// map. Each helper call appends the func; the caller wires indices.
coreir::NodeId print_closure(coreir::Builder& b, coreir::Module& m,
                             const std::string& text, coreir::SrcPos p) {
  m.funcs.push_back(
      {"defer_" + text, 0, 0,
       b.intrinsic(coreir::IntrinsicId::Print, {b.str_literal(text, p)}, p),
       {},
       {}});
  return b.make_closure(static_cast<int32_t>(m.funcs.size() - 1), 0, p);
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

  // --- 1. Fall-through: LIFO, before what follows the scope. --------------
  // scope { defer A; defer B; print "body" }; print "after"
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    const NodeId body = b.block(
        {b.make_defer(print_closure(b, m, "A", p), p),
         b.make_defer(print_closure(b, m, "B", p), p),
         b.intrinsic(IntrinsicId::Print, {b.str_literal("body", p)}, p)},
        p);
    m.funcs.insert(
        m.funcs.begin(),
        {"main", 0, 0,
         b.block({b.scope(0, 0, body, p),
                  b.intrinsic(IntrinsicId::Print,
                              {b.str_literal("after", p)}, p)},
                 p),
         {},
         {}});
    // main was prepended: closure func indices shifted by one.
    for (auto& n : m.nodes) {
      if (n.tag == Tag::MakeClosure) n.a += 1;
    }
    check_eq(run_module(m, "lifo"), "", "lifo: failure");
    check_eq(joined(), "body|B|A|after|", "lifo output");
  }

  // --- 2. Break fires the loop-body scope's defers, each iteration. -------
  // i=0; while i<3 { i=i+1; scope { defer D; if i==2 break } }; print 9
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    const NodeId inner = b.block(
        {b.make_defer(print_closure(b, m, "D", p), p),
         b.make_if(b.binary(BinOp::Eq, b.varref(VarKind::Local, 0, p),
                            b.literal(2, p), p),
                   b.make_break(p), NodeId{}, p)},
        p);
    const NodeId loop_body = b.block(
        {b.assign(VarKind::Local, 0,
                  b.binary(BinOp::Add, b.varref(VarKind::Local, 0, p),
                           b.literal(1, p), p),
                  p),
         b.scope(0, 0, inner, p)},
        p);
    m.funcs.insert(
        m.funcs.begin(),
        {"main", 1, 0,
         b.block({b.assign(VarKind::Local, 0, b.literal(0, p), p),
                  b.make_while(b.binary(BinOp::Lt,
                                        b.varref(VarKind::Local, 0, p),
                                        b.literal(3, p), p),
                               loop_body, p),
                  b.intrinsic(IntrinsicId::Print, {b.literal(9, p)}, p)},
                 p),
         {"i"},
         {}});
    for (auto& n : m.nodes) {
      if (n.tag == Tag::MakeClosure) n.a += 1;
    }
    check_eq(run_module(m, "break defers"), "", "break defers: failure");
    check_eq(joined(), "D|D|9|", "break defers output");
  }

  // --- 3. Return runs every open scope's defers, after the value. ---------
  // f() { scope { defer A; scope { defer B; return 42 } } }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({"main", 0, 0, NodeId{}, {}, {}});  // body wired below
    const NodeId inner = b.scope(
        0, 0,
        b.block({b.make_defer(print_closure(b, m, "B", p), p),
                 b.make_return(b.literal(42, p), p)},
                p),
        p);
    const NodeId fbody = b.scope(
        0, 0,
        b.block({b.make_defer(print_closure(b, m, "A", p), p), inner}, p),
        p);
    m.funcs.push_back({"f", 0, 0, fbody, {}, {}});
    const int32_t f_idx = static_cast<int32_t>(m.funcs.size() - 1);
    m.funcs[0].body = b.intrinsic(
        IntrinsicId::Print,
        {b.call_value(b.make_closure(f_idx, 0, p), {}, p)}, p);
    check_eq(run_module(m, "return defers"), "", "return defers: failure");
    check_eq(joined(), "B|A|42|", "return defers output");
  }

  // --- 4. Unwind runs the crossed scope's defers before the handler. ------
  // try { scope { defer D; throw "x" } } catch e { print e }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({"main", 1, 0, NodeId{}, {}, {}});
    const NodeId body = b.scope(
        0, 0,
        b.block({b.make_defer(print_closure(b, m, "D", p), p),
                 b.make_throw(b.str_literal("x", p), p)},
                p),
        p);
    m.funcs[0].body =
        b.intrinsic(IntrinsicId::Print,
                    {b.make_try(0, body, b.varref(VarKind::Local, 0, p), p)},
                    p);
    m.funcs[0].local_names = {"e"};
    check_eq(run_module(m, "unwind defers"), "", "unwind defers: failure");
    check_eq(joined(), "D|x|", "unwind defers output");
  }

  // --- 5. A defer's own handler, closed inside the defer, is invisible to
  //        the unwind in progress (the culebra carrier bug, pinned sane). --
  // defer body: try { throw "inner" } catch e { print e }; print "done"
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({"main", 1, 0, NodeId{}, {}, {}});
    m.funcs.push_back(
        {"defer_fn", 1, 0,
         b.block({b.intrinsic(IntrinsicId::Print,
                              {b.make_try(0,
                                          b.make_throw(
                                              b.str_literal("inner", p), p),
                                          b.varref(VarKind::Local, 0, p), p)},
                              p),
                  b.intrinsic(IntrinsicId::Print,
                              {b.str_literal("done", p)}, p)},
                 p),
         {"e"},
         {}});
    const NodeId body = b.scope(
        0, 0,
        b.block({b.make_defer(b.make_closure(1, 0, p), p),
                 b.make_throw(b.str_literal("original", p), p)},
                p),
        p);
    m.funcs[0].body =
        b.intrinsic(IntrinsicId::Print,
                    {b.make_try(0, body, b.varref(VarKind::Local, 0, p), p)},
                    p);
    m.funcs[0].local_names = {"e"};
    check_eq(run_module(m, "defer self-catch"), "",
             "defer self-catch: failure");
    check_eq(joined(), "inner|done|original|", "defer self-catch output");
  }

  // --- 6. A defer throwing at the fall-through exit of a try's body scope
  //        escapes its own catch and lands in the enclosing one. -----------
  // try { try { scope { defer(throw "esc"); print "body done" } }
  //       catch e { print "inner (must not happen)" } } catch f { print f }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({"main", 2, 0, NodeId{}, {}, {}});
    m.funcs.push_back(
        {"defer_throw", 0, 0, b.make_throw(b.str_literal("esc", p), p), {},
         {}});
    const NodeId body_scope = b.scope(
        0, 0,
        b.block({b.make_defer(b.make_closure(1, 0, p), p),
                 b.intrinsic(IntrinsicId::Print,
                             {b.str_literal("body done", p)}, p)},
                p),
        p);
    const NodeId inner_try =
        b.make_try(0, body_scope,
                   b.intrinsic(IntrinsicId::Print,
                               {b.str_literal("inner (must not happen)", p)},
                               p),
                   p);
    m.funcs[0].body = b.intrinsic(
        IntrinsicId::Print,
        {b.make_try(1, inner_try, b.varref(VarKind::Local, 1, p), p)}, p);
    m.funcs[0].local_names = {"e", "f"};
    check_eq(run_module(m, "exit defer escapes"), "",
             "exit defer escapes: failure");
    check_eq(joined(), "body done|esc|", "exit defer escapes output");
  }

  // --- 7. A trap in a callee whose resume pc is the scope's own exit
  //        sequence: the scope's defers still run, and the enclosing try
  //        still catches. --------------------------------------------------
  // try { scope { defer D; boom() } } catch e { print "caught" }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({"main", 1, 0, NodeId{}, {}, {}});
    const NodeId dcls = print_closure(b, m, "D", p);
    m.funcs.push_back(
        {"boom", 0, 0,
         b.binary(BinOp::Div, b.literal(1, p), b.literal(0, p), p), {}, {}});
    const int32_t boom_idx = static_cast<int32_t>(m.funcs.size() - 1);
    const NodeId body = b.scope(
        0, 0,
        b.block({b.make_defer(dcls, p),
                 b.call_value(b.make_closure(boom_idx, 0, p), {}, p)},
                p),
        p);
    m.funcs[0].body = b.make_try(
        0, body,
        b.intrinsic(IntrinsicId::Print, {b.str_literal("caught", p)}, p), p);
    m.funcs[0].local_names = {"e"};
    check_eq(run_module(m, "trap at exit seq"), "",
             "trap at exit seq: failure");
    check_eq(joined(), "D|caught|", "trap at exit seq output");
  }

  // --- 8. A defer throwing during unwind replaces the in-flight value.
  //        The remaining defers of the same scope are dropped unrun, and --
  //        deliberately unlike culebra, whose cost ruling keeps its JIT
  //        skipping them -- the same frame's outer handlers stay eligible. -
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({"main", 1, 0, NodeId{}, {}, {}});
    const NodeId skipped = print_closure(b, m, "skipped", p);
    m.funcs.push_back(
        {"defer_throw", 0, 0,
         b.make_throw(b.str_literal("replacement", p), p), {}, {}});
    const int32_t thrower = static_cast<int32_t>(m.funcs.size() - 1);
    const NodeId body = b.scope(
        0, 0,
        b.block({b.make_defer(skipped, p),
                 b.make_defer(b.make_closure(thrower, 0, p), p),
                 b.make_throw(b.str_literal("original", p), p)},
                p),
        p);
    m.funcs[0].body =
        b.intrinsic(IntrinsicId::Print,
                    {b.make_try(0, body, b.varref(VarKind::Local, 0, p), p)},
                    p);
    m.funcs[0].local_names = {"e"};
    check_eq(run_module(m, "defer replaces"), "", "defer replaces: failure");
    check_eq(joined(), "replacement|", "defer replaces output");
  }

  // --- 9. What verify() refuses. ------------------------------------------
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({"d", 0, 0, b.literal(0, p), {}, {}});
    m.funcs.insert(m.funcs.begin(),
                   {"main", 0, 0,
                    b.make_defer(b.make_closure(1, 0, p), p), {}, {}});
    const auto err = coreir::verify(m);
    check_eq(err ? *err : "", "Defer outside a Scope; wrap the region in one",
             "defer outside scope: verify message");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "defers: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("defers OK\n");
  return 0;
}
