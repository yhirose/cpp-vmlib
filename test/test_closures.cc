// First-class functions.
//
// The thing being tested was already broken before closures were asked for.
// vm/exec.cc's `captures`
// are raw Slot pointers into the caller's frame, sound only because "PL/0 has
// no upward funarg" -- no way to make a function outlive the frame that wrote
// it. A first-class function is exactly that, so the pointers have to become
// something that can outlive a frame: a cell, shared and refcounted.
//
// Three questions:
//
//   1. Does a closure returned from a function still see its captured
//      variable, with the defining frame long gone? (The dangling case.)
//   2. Do two closures over the same variable see each other's writes? (The
//      sharing case -- a cell per closure would pass test 1 and fail this.)
//   3. What does reference counting alone do with a closure that captures
//      itself? Answer: nothing -- it leaks, unavoidably, for as long as the
//      program runs. This test asserts the leak rather than wishing it away,
//      because that measurement is the whole argument for a tracing
//      collector, and a later change that quietly fixed it is worth being
//      told about. The Runtime's own destructor does sweep the cycle up
//      afterwards, so the process does not leak; what it cannot do is free
//      it while the program is still running and might want the memory.

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

// Runs a module and reports what leaked, so a caller can assert either zero
// (the normal expectation) or a specific non-zero count (the cycle case).
struct RunResult {
  std::string failure;
  int64_t leaked = 0;
};

RunResult run_module(const coreir::Module& m, const std::string& what) {
  g_out.clear();
  RunResult r;
  if (auto err = coreir::verify(m)) {
    std::fprintf(stderr, "FAIL: %s: malformed IR: %s\n", what.c_str(),
                 err->c_str());
    ++g_failures;
    return r;
  }
  {
    coreir::Runtime rt;
    const vm::Program p = vm::compile(m);
    try {
      vm::run(p, rt);
    } catch (const Failure& e) {
      r.failure = e.what();
    }
    // Taken before ~Runtime: that destructor frees whatever counting could
    // not, so afterwards even the cycle case would read zero.
    r.leaked = rt.live_objects();
  }
  return r;
}

void expect_clean(const RunResult& r, const std::string& what) {
  if (!r.failure.empty()) {
    std::fprintf(stderr, "FAIL: %s: unexpected failure: %s\n", what.c_str(),
                 r.failure.c_str());
    ++g_failures;
  }
  if (r.leaked != 0) {
    std::fprintf(stderr, "FAIL: %s: leaked %lld heap object(s)\n", what.c_str(),
                 static_cast<long long>(r.leaked));
    ++g_failures;
  }
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

  // --- 1. A closure outliving the frame that built it. --------------------
  // main:  f = make_closure(#1, [cell 0]); cell 0 = 42; print(f())
  // Except the point is that the *builder* returns first, so:
  //   main:  f = make_maker();  print(f())
  //   maker: cell 0 = 42; return make_closure(#2, [cell 0])
  //   getter: return capture[0]
  // maker's frame is gone by the time getter runs. Under the Slot* captures
  // this replaces, getter would read freed stack.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});                    // 0: maker takes nothing
    m.capture_maps.push_back({{VarKind::Cell, 0}});  // 1: getter gets maker's

    // #0 main
    const NodeId f = b.varref(VarKind::Local, 0, p);
    const NodeId main_body = b.block(
        {b.assign(VarKind::Local, 0,
                  b.call_value(b.make_closure(1, 0, p), {}, p), p),
         b.intrinsic(IntrinsicId::Print, {b.call_value(f, {}, p)}, p)},
        p);
    Func main_fn{"main", 1, 0, main_body, {"f"}, {}};
    m.funcs.push_back(main_fn);

    // #1 maker
    Func maker{"maker", 0, 0, NodeId{}, {}, {}};
    maker.num_cells = 1;
    maker.body = b.block({b.assign(VarKind::Cell, 0, b.literal(42, p), p),
                          b.make_closure(2, 1, p)},
                         p);
    m.funcs.push_back(maker);

    // #2 getter
    Func getter{"getter", 0, 1, b.varref(VarKind::Capture, 0, p), {}, {"x"}};
    m.funcs.push_back(getter);

    const RunResult r = run_module(m, "escaping closure");
    expect_clean(r, "escaping closure");
    check_eq(joined(), "42|", "escaping closure output");
  }

  // --- 2. Two closures sharing one variable. ------------------------------
  // A getter and a setter built over the same cell. Giving each closure its
  // own copy of the variable would pass test 1 and fail this one, which is
  // why both tests exist: the point of a cell is the sharing, not just the
  // lifetime.
  //
  //   main: cell 0 = 7
  //         g = closure(getter, [cell 0]); s = closure(setter, [cell 0])
  //         print(g()); s(99); print(g())
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({{VarKind::Cell, 0}});  // 0: getter
    m.capture_maps.push_back({{VarKind::Cell, 0}});  // 1: setter, same cell

    Func main_fn{"main", 2, 0, NodeId{}, {"g", "s"}, {}};
    main_fn.num_cells = 1;
    const NodeId g = b.varref(VarKind::Local, 0, p);
    const NodeId s = b.varref(VarKind::Local, 1, p);
    main_fn.body = b.block(
        {b.assign(VarKind::Cell, 0, b.literal(7, p), p),
         b.assign(VarKind::Local, 0, b.make_closure(1, 0, p), p),
         b.assign(VarKind::Local, 1, b.make_closure(2, 1, p), p),
         b.intrinsic(IntrinsicId::Print, {b.call_value(g, {}, p)}, p),
         b.call_value(s, {b.literal(99, p)}, p),
         b.intrinsic(IntrinsicId::Print, {b.call_value(g, {}, p)}, p)},
        p);
    m.funcs.push_back(main_fn);

    m.funcs.push_back({"getter", 0, 1, b.varref(VarKind::Capture, 0, p), {},
                       {"x"}});
    Func setter{"setter", 1, 1, NodeId{}, {"v"}, {"x"}};
    setter.num_params = 1;
    setter.body =
        b.assign(VarKind::Capture, 0, b.varref(VarKind::Local, 0, p), p);
    m.funcs.push_back(setter);

    const RunResult r = run_module(m, "shared capture");
    expect_clean(r, "shared capture");
    check_eq(joined(), "7|99|", "shared capture: one variable, not two");
  }

  // --- 3. Arguments and a return value. -----------------------------------
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    Func main_fn{"main", 0, 0, NodeId{}, {}, {}};
    main_fn.body = b.intrinsic(
        IntrinsicId::Print,
        {b.call_value(b.make_closure(1, 0, p),
                      {b.literal(3, p), b.literal(4, p)}, p)},
        p);
    m.funcs.push_back(main_fn);

    Func add{"add", 2, 0, NodeId{}, {"a", "b"}, {}};
    add.num_params = 2;
    add.body = b.binary(BinOp::Add, b.varref(VarKind::Local, 0, p),
                        b.varref(VarKind::Local, 1, p), p);
    m.funcs.push_back(add);

    const RunResult r = run_module(m, "args and return");
    expect_clean(r, "args and return");
    check_eq(joined(), "7|", "args and return output");
  }

  // --- 4. Calling something that is not a function, and arity. ------------
  {
    Module m;
    Builder b(m);
    m.funcs.push_back({"main", 0, 0,
                       b.call_value(b.literal(1, p), {}, p), {}, {}});
    const RunResult r = run_module(m, "call an int");
    check_eq(r.failure, "cannot call int", "call an int: message");
    if (r.leaked != 0) {
      std::fprintf(stderr, "FAIL: call an int leaked %lld\n",
                   static_cast<long long>(r.leaked));
      ++g_failures;
    }
  }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({"main", 0, 0,
                       b.call_value(b.make_closure(1, 0, p),
                                    {b.literal(1, p)}, p),
                       {},
                       {}});
    Func f{"f", 0, 0, NodeId{}, {}, {}};
    f.body = b.literal(0, p);
    m.funcs.push_back(f);
    const RunResult r = run_module(m, "wrong arity");
    check_eq(r.failure, "f takes 0 argument(s), given 1", "arity: message");
  }

  // --- 5. verify() rejects capturing a local. -----------------------------
  // A local dies with its frame, so a closure over one is the dangling bug
  // this whole design exists to make unrepresentable. It should not reach the
  // executor at all.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({{VarKind::Local, 0}});
    m.funcs.push_back({"main", 1, 0, b.make_closure(1, 0, p), {"x"}, {}});
    m.funcs.push_back({"f", 0, 1, b.varref(VarKind::Capture, 0, p), {}, {"x"}});
    auto err = coreir::verify(m);
    if (!err) {
      std::fprintf(stderr, "FAIL: verify accepted a closure over a local\n");
      ++g_failures;
    } else {
      check_eq(*err, "a closure cannot capture a local; promote it to a cell",
               "capture-a-local: message");
    }
  }

  // --- 6. THE CYCLE. ------------------------------------------------------
  // main: cell 0 = make_closure(#1, [cell 0])
  // The closure holds the cell; the cell holds the closure. Reference
  // counting frees neither, and no amount of care in the executor changes
  // that -- it is what counting cannot do. Asserting the exact leak here
  // means the tracing-backstop phase has a number to drive to zero, and
  // means nobody later mistakes this for an oversight.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({{VarKind::Cell, 0}});
    Func main_fn{"main", 0, 0, NodeId{}, {}, {}};
    main_fn.num_cells = 1;
    main_fn.body = b.assign(VarKind::Cell, 0, b.make_closure(1, 0, p), p);
    m.funcs.push_back(main_fn);
    m.funcs.push_back({"self", 0, 1, b.varref(VarKind::Capture, 0, p), {},
                       {"me"}});

    const RunResult r = run_module(m, "self-capturing closure");
    check_eq(r.failure, "", "cycle: unexpected failure");
    if (r.leaked != 2) {
      std::fprintf(stderr,
                   "FAIL: self-capturing closure leaked %lld object(s), "
                   "expected exactly 2 (the cell and the closure).\n"
                   "  If this is now 0, reference counting did not do it -- "
                   "something added a cycle collector, and this test should "
                   "become an assertion that it works.\n",
                   static_cast<long long>(r.leaked));
      ++g_failures;
    }
  }

  // --- CellFresh: each loop entry gets its own box; closures built in
  //     earlier iterations keep the old one. --------------------------------
  // i = 0; while i < 2 { cellfresh c0; c0 = i (as x); push closure(read x);
  //                      i = i + 1 }; print f0(); print f1()
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});                     // cmap 0: empty
    m.capture_maps.push_back({{VarKind::Cell, 0}});   // cmap 1: cell 0
    // funcs[1]: () -> its captured x
    m.funcs.push_back({"main", 3, 0, NodeId{}, {"i", "f0", "f1"}, {}});
    m.funcs.push_back(
        {"read", 0, 1, b.varref(VarKind::Capture, 0, p), {}, {"x"}});
    const NodeId body = b.block(
        {b.cell_fresh(0, p),
         b.assign(VarKind::Cell, 0, b.varref(VarKind::Local, 0, p), p),
         b.make_if(b.binary(BinOp::Eq, b.varref(VarKind::Local, 0, p),
                            b.literal(0, p), p),
                   b.assign(VarKind::Local, 1, b.make_closure(1, 1, p), p),
                   b.assign(VarKind::Local, 2, b.make_closure(1, 1, p), p),
                   p),
         b.assign(VarKind::Local, 0,
                  b.binary(BinOp::Add, b.varref(VarKind::Local, 0, p),
                           b.literal(1, p), p),
                  p)},
        p);
    m.funcs[0].body = b.block(
        {b.assign(VarKind::Local, 0, b.literal(0, p), p),
         b.make_while(b.binary(BinOp::Lt, b.varref(VarKind::Local, 0, p),
                               b.literal(2, p), p),
                      body, p),
         b.intrinsic(IntrinsicId::Print,
                     {b.call_value(b.varref(VarKind::Local, 1, p), {}, p)},
                     p),
         b.intrinsic(IntrinsicId::Print,
                     {b.call_value(b.varref(VarKind::Local, 2, p), {}, p)},
                     p)},
        p);
    m.funcs[0].num_cells = 1;
    const RunResult r = run_module(m, "cellfresh");
    expect_clean(r, "cellfresh");
    check_eq(joined(), "0|1|", "cellfresh output");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "closures: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("closures OK\n");
  return 0;
}
