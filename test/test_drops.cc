// Deterministic drop, pinned against culebra's rules: on every kind of
// scope exit -- fall-through, Return, Break, an unwinding throw -- the
// scope runs its defers (LIFO) and then releases its locals last-declared-
// first, innermost scope first. Locals a then b, both carrying a drop hook
// that prints their name, must therefore print "b" then "a" in every case.
// Then the program's end: under RunOptions::entry_frame_drops == false the
// entry frame's bindings are released without their destructors, whether
// the program returns or throws -- only its defers run.
// Then the owned stack (Runtime::owned_scope_exit): a cycle of drop-bearing
// objects that a scope's releases could not free drops at that scope's
// exit, newest first; one still held from outside waits for the scope that
// last held it; one merely hanging off a plain cycle waits for the
// collector; and a destructor runs at most once, resurrection included.
// Then a scope's explicit release list, which puts a captured local (a
// cell) back into declaration order with the plain ones; the frame's dead
// temporaries, which must not keep a scope's cycle alive; and a container's
// own order, first to last, when it is freed with drop-bearing values in it.

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

void check(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

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

// A module whose funcs[1] is the shared destructor
//   drop = fn (self) { print(self.name) }
// and whose funcs[0] (main) is left for the case to fill in. `mk(slot,
// name)` builds `slot = {name: "<name>", "\x01drop": <local 0>}`, so every
// case's main keeps the closure in local 0 and its objects after it.
struct Prog {
  coreir::Module m;
  coreir::Builder b{m};
  const coreir::SrcPos p{1, 1};

  Prog() {
    using namespace coreir;
    m.capture_maps.push_back({});
    m.funcs.push_back({"main", 0, 0, NodeId{}, {}, {}});
    Func d{"drop", 1, 0, NodeId{}, {"self"}, {}};
    d.num_params = 1;
    d.body = print(b.index(local(0), str("name"), p));
    m.funcs.push_back(d);
  }
  coreir::NodeId str(const char* s) { return b.str_literal(s, p); }
  coreir::NodeId local(int32_t i) {
    return b.varref(coreir::VarKind::Local, i, p);
  }
  coreir::NodeId print(coreir::NodeId v) {
    return b.intrinsic(coreir::IntrinsicId::Print, {v}, p);
  }
  coreir::NodeId print_str(const char* s) { return print(str(s)); }
  coreir::NodeId set(int32_t slot, const char* k, coreir::NodeId v) {
    return b.set_index(local(slot), str(k), v, p);
  }
  // a.o = b; b.o = a
  coreir::NodeId cycle(int32_t a, int32_t b) {
    return b_pair(set(a, "o", local(b)), set(b, "o", local(a)));
  }
  coreir::NodeId b_pair(coreir::NodeId x, coreir::NodeId y) {
    return b.block({x, y}, p);
  }
  coreir::NodeId collect() {
    return print(b.intrinsic(coreir::IntrinsicId::Collect, {}, p));
  }
  // {name: "<name>", "\x01drop": <local 0>}
  coreir::NodeId obj(const char* name) {
    return b.object_lit({{str("name"), str(name)},
                         {str("\x01" "drop"), local(0)}},
                        p);
  }
  coreir::NodeId mk(int32_t slot, const char* name) {
    return b.block({b.assign(coreir::VarKind::Local, slot, obj(name), p)}, p);
  }
  coreir::NodeId bind_drop() {
    return b.assign(coreir::VarKind::Local, 0, b.make_closure(1, 0, p), p);
  }
  // main's body and slot names; the drop closure is slot 0.
  void main(coreir::NodeId body, std::vector<std::string> names) {
    names.insert(names.begin(), "d");
    m.funcs[0].num_locals = static_cast<int32_t>(names.size());
    m.funcs[0].local_names = std::move(names);
    m.funcs[0].body = body;
  }
};

struct RunResult {
  std::string failure;
  int64_t left = 0;  // live objects after the run, before ~Runtime
};

RunResult run_module(const coreir::Module& m, const std::string& what,
                     vm::RunOptions opts = {}) {
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
      vm::run(p, rt, opts);
    } catch (const Failure& e) {
      r.failure = e.what();
    }
    r.left = rt.live_objects();
  }
  return r;
}

void expect_clean(const RunResult& r, const std::string& what) {
  check_eq(r.failure, "", what + ": unexpected failure");
  check(r.left == 0, what + ": leaked " + std::to_string(r.left) +
                         " heap object(s)");
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

  // --- 1. Fall-through: b then a, before what follows the scope. ---------
  {
    Prog g;
    g.main(g.b.block({g.bind_drop(),
                      g.b.scope(1, 3, g.b.block({g.mk(1, "a"), g.mk(2, "b")},
                                                g.p),
                                g.p),
                      g.print_str("after")},
                     g.p),
           {"a", "b"});
    expect_clean(run_module(g.m, "fall-through"), "fall-through");
    check_eq(joined(), "b|a|after|", "fall-through output");
  }

  // --- 2. Return: the scope's locals go before the frame does, b then a,
  //        and the value survives them. ---------------------------------
  // f = fn () { { d = ...; a = mk("a"); b = mk("b"); return 1 } }
  // print(f())
  {
    Prog g;
    Func f{"f", 3, 0, NodeId{}, {"d", "a", "b"}, {}};
    f.body = g.b.scope(
        0, 3,
        g.b.block({g.bind_drop(), g.mk(1, "a"), g.mk(2, "b"),
                   g.b.make_return(g.b.literal(1, g.p), g.p)},
                  g.p),
        g.p);
    g.m.funcs.push_back(f);
    g.main(g.b.block({g.print(g.b.call_value(g.b.make_closure(2, 0, g.p), {},
                                             g.p))},
                     g.p),
           {});
    expect_clean(run_module(g.m, "return"), "return");
    check_eq(joined(), "b|a|1|", "return output");
  }

  // --- 3. Throw: the unwinder releases the crossed scope's locals the
  //        same way, before the handler runs. ----------------------------
  {
    Prog g;
    g.main(g.b.block(
               {g.bind_drop(),
                g.b.make_try(
                    3,
                    g.b.scope(1, 3,
                              g.b.block({g.mk(1, "a"), g.mk(2, "b"),
                                         g.b.make_throw(g.str("x"), g.p)},
                                        g.p),
                              g.p),
                    g.print(g.local(3)), g.p)},
               g.p),
           {"a", "b", "e"});
    expect_clean(run_module(g.m, "throw"), "throw");
    check_eq(joined(), "b|a|x|", "throw output");
  }

  // --- 4. Break: leaving the loop body's scope. --------------------------
  {
    Prog g;
    g.main(g.b.block(
               {g.bind_drop(),
                g.b.make_while(
                    g.b.bool_literal(true, g.p),
                    g.b.scope(1, 3,
                              g.b.block({g.mk(1, "a"), g.mk(2, "b"),
                                         g.b.make_break(g.p)},
                                        g.p),
                              g.p),
                    g.p),
                g.print_str("after")},
               g.p),
           {"a", "b"});
    expect_clean(run_module(g.m, "break"), "break");
    check_eq(joined(), "b|a|after|", "break output");
  }

  // --- 5. Nested scopes: the inner one first -- at its own exit, and
  //        when one throw crosses both. ---------------------------------
  {
    Prog g;
    g.main(g.b.block(
               {g.bind_drop(),
                g.b.scope(1, 2,
                          g.b.block({g.mk(1, "a"),
                                     g.b.scope(2, 3, g.mk(2, "b"), g.p),
                                     g.print_str("mid")},
                                    g.p),
                          g.p),
                g.print_str("after")},
               g.p),
           {"a", "b"});
    expect_clean(run_module(g.m, "nested"), "nested");
    check_eq(joined(), "b|mid|a|after|", "nested output");
  }
  {
    Prog g;
    g.main(g.b.block(
               {g.bind_drop(),
                g.b.make_try(
                    3,
                    g.b.scope(
                        1, 2,
                        g.b.block(
                            {g.mk(1, "a"),
                             g.b.scope(2, 3,
                                       g.b.block({g.mk(2, "b"),
                                                  g.b.make_throw(g.str("x"),
                                                                 g.p)},
                                                 g.p),
                                       g.p)},
                            g.p),
                        g.p),
                    g.print(g.local(3)), g.p)},
               g.p),
           {"a", "b", "e"});
    expect_clean(run_module(g.m, "nested throw"), "nested throw");
    check_eq(joined(), "b|a|x|", "nested throw output");
  }

  // --- 6. Defer and drops: the scope's defers run first, then its locals
  //        go, b then a -- on the fall-through and on a Return. --------
  // { a = mk("a"); b = mk("b"); defer fn () { print("defer") } }
  {
    for (int with_return = 0; with_return < 2; ++with_return) {
      Prog g;
      Func df{"deferred", 0, 0, NodeId{}, {}, {}};
      df.body = g.print_str("defer");
      g.m.funcs.push_back(df);
      std::vector<NodeId> body{
          g.mk(1, "a"), g.mk(2, "b"),
          g.b.make_defer(g.b.make_closure(2, 0, g.p), g.p)};
      if (with_return) body.push_back(g.b.make_return(NodeId{}, g.p));
      g.main(g.b.block({g.bind_drop(),
                        g.b.scope(1, 3, g.b.block(body, g.p), g.p),
                        g.print_str("after")},
                       g.p),
             {"a", "b"});
      const std::string what = with_return ? "defer+return" : "defer";
      expect_clean(run_module(g.m, what), what);
      check_eq(joined(), with_return ? "defer|b|a|" : "defer|b|a|after|",
               what + " output");
    }
  }

  // --- 7. The entry frame at program exit: with entry_frame_drops off, a
  //        top-level binding's destructor does not run -- on a normal end,
  //        on an uncaught throw -- while a top-level defer (a Scope over
  //        no locals) still does. On by default, which drops as ever. ----
  {
    vm::RunOptions quiet;
    quiet.entry_frame_drops = false;
    // a = mk("a"); print("end")
    Prog g;
    g.main(g.b.block({g.bind_drop(), g.mk(1, "a"), g.print_str("end")}, g.p),
           {"a"});
    expect_clean(run_module(g.m, "exit: default"), "exit: default");
    check_eq(joined(), "end|a|", "exit: default output");
    expect_clean(run_module(g.m, "exit: quiet", quiet), "exit: quiet");
    check_eq(joined(), "end|", "exit: quiet output");
  }
  {
    vm::RunOptions quiet;
    quiet.entry_frame_drops = false;
    // a = mk("a"); throw "boom"
    Prog g;
    g.main(g.b.block({g.bind_drop(), g.mk(1, "a"),
                      g.b.make_throw(g.str("boom"), g.p)},
                     g.p),
           {"a"});
    RunResult r = run_module(g.m, "uncaught: default");
    check_eq(r.failure, "uncaught: boom", "uncaught: default failure");
    check(r.left == 0, "uncaught: default leaked");
    check_eq(joined(), "a|", "uncaught: default output");
    r = run_module(g.m, "uncaught: quiet", quiet);
    check_eq(r.failure, "uncaught: boom", "uncaught: quiet failure");
    check(r.left == 0, "uncaught: quiet leaked");
    check_eq(joined(), "", "uncaught: quiet output");
  }
  {
    vm::RunOptions quiet;
    quiet.entry_frame_drops = false;
    // { defer fn () { print("defer") }; a = mk("a"); print("end") }
    // -- a Scope over [0, 0): the defer is the scope's, the local is not.
    Prog g;
    Func df{"deferred", 0, 0, NodeId{}, {}, {}};
    df.body = g.print_str("defer");
    g.m.funcs.push_back(df);
    g.main(g.b.scope(0, 0,
                     g.b.block({g.bind_drop(),
                                g.b.make_defer(g.b.make_closure(2, 0, g.p),
                                               g.p),
                                g.mk(1, "a"), g.print_str("end")},
                               g.p),
                     g.p),
           {"a"});
    expect_clean(run_module(g.m, "exit: defer", quiet), "exit: defer");
    check_eq(joined(), "end|defer|", "exit: defer output");
  }

  // --- 8. A two-member cycle bound in a scope drops at the scope's exit,
  //        newest first, and is freed. -----------------------------------
  // { a = mk("a"); b = mk("b"); a.o = b; b.o = a }; print("after")
  {
    Prog g;
    g.main(g.b.block({g.bind_drop(),
                      g.b.scope(1, 3,
                                g.b.block({g.mk(1, "a"), g.mk(2, "b"),
                                           g.cycle(1, 2)},
                                          g.p),
                                g.p),
                      g.print_str("after")},
                     g.p),
           {"a", "b"});
    expect_clean(run_module(g.m, "owned cycle"), "owned cycle");
    check_eq(joined(), "b|a|after|", "owned cycle output");
  }

  // --- 9. Pushed into an outer array, the cycle is the outer scope's:
  //        nothing at the inner exit, b then a at the outer one. ---------
  // { keep = []; { a; b; cycle; arraypush(keep, a) }; print("inner") }
  // print("outer")
  {
    Prog g;
    g.main(g.b.block(
               {g.bind_drop(),
                g.b.scope(
                    1, 2,
                    g.b.block(
                        {g.b.assign(VarKind::Local, 1, g.b.array_lit({}, g.p),
                                    g.p),
                         g.b.scope(2, 4,
                                   g.b.block({g.mk(2, "a"), g.mk(3, "b"),
                                              g.cycle(2, 3),
                                              g.b.intrinsic(
                                                  IntrinsicId::ArrayPush,
                                                  {g.local(1), g.local(2)},
                                                  g.p)},
                                             g.p),
                                   g.p),
                         g.print_str("inner")},
                        g.p),
                    g.p),
                g.print_str("outer")},
               g.p),
           {"keep", "a", "b"});
    expect_clean(run_module(g.m, "owned escape"), "owned escape");
    check_eq(joined(), "inner|b|a|outer|", "owned escape output");
  }

  // --- 10. Hanging off a plain-object cycle it is not a member of: not
  //         the scope's to resolve; the collector drops it. -------------
  // { p = {}; p.self = p; r = mk("r"); p.r = r }; print(collect());
  // print("after")
  {
    Prog g;
    g.main(g.b.block(
               {g.bind_drop(),
                g.b.scope(1, 3,
                          g.b.block({g.b.assign(VarKind::Local, 1,
                                                g.b.object_lit({}, g.p), g.p),
                                     g.set(1, "self", g.local(1)),
                                     g.mk(2, "r"), g.set(1, "r", g.local(2))},
                                    g.p),
                          g.p),
                g.collect(), g.print_str("after")},
               g.p),
           {"p", "r"});
    expect_clean(run_module(g.m, "owned hanger"), "owned hanger");
    // The collection frees p, r and r's name string.
    check_eq(joined(), "r|3|after|", "owned hanger output");
  }

  // --- 11. A cycle built in a callee and returned drops at the caller's
  //         scope exit, once the caller lets go. ------------------------
  // f = fn () { { d = ...; a = mk("a"); b = mk("b"); cycle; return a } }
  // { x = f(); print("got") }; print("after")
  {
    Prog g;
    Func f{"f", 3, 0, NodeId{}, {"d", "a", "b"}, {}};
    f.body = g.b.scope(
        0, 3,
        g.b.block({g.bind_drop(), g.mk(1, "a"), g.mk(2, "b"), g.cycle(1, 2),
                   g.b.make_return(g.local(1), g.p)},
                  g.p),
        g.p);
    g.m.funcs.push_back(f);
    g.main(g.b.block(
               {g.b.scope(1, 2,
                          g.b.block({g.b.assign(VarKind::Local, 1,
                                                g.b.call_value(
                                                    g.b.make_closure(2, 0, g.p),
                                                    {}, g.p),
                                                g.p),
                                     g.print_str("got")},
                                    g.p),
                          g.p),
                g.print_str("after")},
               g.p),
           {"x"});
    expect_clean(run_module(g.m, "owned returned"), "owned returned");
    check_eq(joined(), "got|b|a|after|", "owned returned output");
  }

  // --- 12. Resurrection at a scope exit: a destructor that stores its
  //         object into an outer array keeps it, intact; when that array
  //         goes, the collector frees the cycle without dropping again. -
  // keep = []; d = fn (self) { print(self.name); arraypush(keep, self) }
  // { a = mk("a"); b = mk("b"); cycle }
  // print(len(keep)); print(keep[0].name); keep = []; print(collect())
  {
    Prog g;
    g.m.capture_maps.push_back({{VarKind::Cell, 0}});
    Func d2{"drop2", 1, 1, NodeId{}, {"self"}, {"keep"}};
    d2.num_params = 1;
    d2.body = g.b.block(
        {g.print(g.b.index(g.local(0), g.str("name"), g.p)),
         g.b.intrinsic(IntrinsicId::ArrayPush,
                       {g.b.varref(VarKind::Capture, 0, g.p), g.local(0)},
                       g.p)},
        g.p);
    g.m.funcs.push_back(d2);
    auto keep = [&]() { return g.b.varref(VarKind::Cell, 0, g.p); };
    g.main(g.b.block(
               {g.b.cell_fresh(0, g.p),
                g.b.assign(VarKind::Cell, 0, g.b.array_lit({}, g.p), g.p),
                g.b.assign(VarKind::Local, 0, g.b.make_closure(2, 1, g.p),
                           g.p),
                g.b.scope(1, 3,
                          g.b.block({g.mk(1, "a"), g.mk(2, "b"),
                                     g.cycle(1, 2)},
                                    g.p),
                          g.p),
                g.print(g.b.intrinsic(IntrinsicId::Len, {keep()}, g.p)),
                g.print(g.b.index(g.b.index(keep(), g.b.literal(0, g.p), g.p),
                                  g.str("name"), g.p)),
                g.b.assign(VarKind::Cell, 0, g.b.array_lit({}, g.p), g.p),
                g.collect()},
               g.p),
           {"a", "b"});
    g.m.funcs[0].num_cells = 1;
    expect_clean(run_module(g.m, "owned resurrect"), "owned resurrect");
    // The collection frees a, b and their two name strings; d2, the cell
    // and the new array are live.
    check_eq(joined(), "b|a|2|b|4|", "owned resurrect output");
  }

  // --- 13. The entry frame's outermost scope is the program's end: under
  //         entry_frame_drops == false it resolves nothing (the cycle is
  //         reclaimed with the heap, undropped); by default it resolves
  //         like any other scope. --------------------------------------
  // { a = mk("a"); b = mk("b"); cycle; print("end"); a = nil; b = nil }
  {
    vm::RunOptions quiet;
    quiet.entry_frame_drops = false;
    Prog g;
    g.main(g.b.scope(0, 0,
                     g.b.block({g.bind_drop(), g.mk(1, "a"), g.mk(2, "b"),
                                g.cycle(1, 2), g.print_str("end"),
                                g.b.assign(VarKind::Local, 1,
                                           g.b.nil_literal(g.p), g.p),
                                g.b.assign(VarKind::Local, 2,
                                           g.b.nil_literal(g.p), g.p)},
                               g.p),
                     g.p),
           {"a", "b"});
    RunResult r = run_module(g.m, "top-level cycle: quiet", quiet);
    check_eq(r.failure, "", "top-level cycle: quiet failure");
    check_eq(joined(), "end|", "top-level cycle: quiet output");
    // a, b, their two name strings, and the drop closure they hold.
    check(r.left == 5, "top-level cycle: quiet leaves the cycle to ~Runtime");
    r = run_module(g.m, "top-level cycle: default");
    expect_clean(r, "top-level cycle: default");
    check_eq(joined(), "end|b|a|", "top-level cycle: default output");
  }

  // --- 14. A release list puts a captured local back in declaration
  //         order: second-declared first, whether it is the cell or the
  //         plain local -- at the fall-through and under a throw. -------
  // { first = mk("first"); second = mk("second"); (fn () { print("use") })()
  //   [; throw "x"] }        -- one of the two lives in a cell
  {
    for (int captured_second = 0; captured_second < 2; ++captured_second) {
      for (int throws = 0; throws < 2; ++throws) {
        Prog g;
        g.m.capture_maps.push_back({{VarKind::Cell, 0}});
        Func use{"use", 0, 1, NodeId{}, {}, {"c"}};
        use.body = g.print_str("use");
        g.m.funcs.push_back(use);
        // The plain one is local 1; the captured one is cell 0.
        auto mk_cell = [&](const char* name) {
          return g.b.assign(VarKind::Cell, 0,
                            g.b.object_lit({{g.str("name"), g.str(name)},
                                            {g.str("\x01" "drop"), g.local(0)}},
                                           g.p),
                            g.p);
        };
        std::vector<NodeId> body;
        std::vector<NodeId> release;
        if (captured_second) {
          body = {g.mk(1, "first"), mk_cell("second")};
          release = {g.b.varref(VarKind::Cell, 0, g.p), g.local(1)};
        } else {
          body = {mk_cell("first"), g.mk(1, "second")};
          release = {g.local(1), g.b.varref(VarKind::Cell, 0, g.p)};
        }
        body.push_back(g.b.call_value(g.b.make_closure(2, 1, g.p), {}, g.p));
        if (throws) body.push_back(g.b.make_throw(g.str("x"), g.p));
        NodeId scope = g.b.scope(1, 2, g.b.block(body, g.p), release, g.p);
        g.main(g.b.block({g.b.cell_fresh(0, g.p), g.bind_drop(),
                          throws ? g.b.make_try(2, scope, g.print(g.local(2)),
                                                g.p)
                                 : scope,
                          g.print_str("after")},
                         g.p),
               {"plain", "e"});
        g.m.funcs[0].num_cells = 1;
        const std::string what = std::string("release list: ") +
                                 (captured_second ? "cell second" : "cell first") +
                                 (throws ? ", throw" : "");
        expect_clean(run_module(g.m, what), what);
        check_eq(joined(),
                 throws ? "use|second|first|x|after|"
                        : "use|second|first|after|",
                 what + " output");
      }
    }
  }

  // --- 15. What verify() refuses in a release list. ---------------------
  {
    auto refused = [&](const char* what, auto build) {
      Prog g;
      Func main{"main", 2, 0, NodeId{}, {"a", "b"}, {}};
      main.num_cells = 1;
      main.body = build(g);
      g.m.funcs[0] = main;
      const auto err = verify(g.m);
      check(err.has_value(), std::string("verify accepted: ") + what);
    };
    refused("a capture", [](Prog& g) {
      return g.b.scope(0, 2, g.b.block({}, g.p),
                       {g.b.varref(VarKind::Capture, 0, g.p)}, g.p);
    });
    refused("a local outside the range", [](Prog& g) {
      return g.b.scope(1, 2, g.b.block({}, g.p), {g.local(0)}, g.p);
    });
    refused("a cell out of range", [](Prog& g) {
      return g.b.scope(0, 2, g.b.block({}, g.p),
                       {g.b.varref(VarKind::Cell, 1, g.p)}, g.p);
    });
    refused("a duplicate", [](Prog& g) {
      return g.b.scope(0, 2, g.b.block({}, g.p), {g.local(1), g.local(1)},
                       g.p);
    });
    refused("a non-VarRef entry", [](Prog& g) {
      return g.b.scope(0, 2, g.b.block({}, g.p), {g.b.literal(1, g.p)}, g.p);
    });
  }

  // --- 16. A scope's cycle must not be kept alive by the frame's dead
  //         temporaries: the block's own value when it stands as a
  //         statement, a condition's register at a Break, or nothing at
  //         all in a function body -- and a self-cycle. ----------------
  {
    // { a; b; a.peer = b; b.peer = a; a }  (statement position; the
    // trailing `a` is the block's discarded value)
    Prog g;
    g.main(g.b.block({g.bind_drop(),
                      g.b.scope(1, 3,
                                g.b.block({g.mk(1, "a"), g.mk(2, "b"),
                                           g.set(1, "peer", g.local(2)),
                                           g.set(2, "peer", g.local(1)),
                                           g.local(1)},
                                          g.p),
                                g.p),
                      g.print_str("after")},
                     g.p),
           {"a", "b"});
    expect_clean(run_module(g.m, "temps: block value"), "temps: block value");
    check_eq(joined(), "b|a|after|", "temps: block value output");
  }
  {
    // f = fn () { { d; a; b; a.peer = b; b.peer = a } }; f(); print("after")
    Prog g;
    Func f{"f", 3, 0, NodeId{}, {"d", "a", "b"}, {}};
    f.body = g.b.scope(0, 3,
                       g.b.block({g.bind_drop(), g.mk(1, "a"), g.mk(2, "b"),
                                  g.set(1, "peer", g.local(2)),
                                  g.set(2, "peer", g.local(1))},
                                 g.p),
                       g.p);
    g.m.funcs.push_back(f);
    g.main(g.b.block({g.b.call_value(g.b.make_closure(2, 0, g.p), {}, g.p),
                      g.print_str("after")},
                     g.p),
           {});
    expect_clean(run_module(g.m, "temps: body"), "temps: body");
    check_eq(joined(), "b|a|after|", "temps: body output");
  }
  {
    // while true { { a; b; cycle; if a { break } } }; print("after")
    // -- the condition's register holds a across the Break.
    Prog g;
    g.main(g.b.block(
               {g.bind_drop(),
                g.b.make_while(
                    g.b.bool_literal(true, g.p),
                    g.b.scope(1, 3,
                              g.b.block({g.mk(1, "a"), g.mk(2, "b"),
                                         g.cycle(1, 2),
                                         g.b.make_if(g.local(1),
                                                     g.b.make_break(g.p),
                                                     NodeId{}, g.p)},
                                        g.p),
                              g.p),
                    g.p),
                g.print_str("after")},
               g.p),
           {"a", "b"});
    expect_clean(run_module(g.m, "temps: break"), "temps: break");
    check_eq(joined(), "b|a|after|", "temps: break output");
  }
  {
    // { a = mk("a"); a.me = a; a }; print("after")
    Prog g;
    g.main(g.b.block({g.bind_drop(),
                      g.b.scope(1, 2,
                                g.b.block({g.mk(1, "a"),
                                           g.set(1, "me", g.local(1)),
                                           g.local(1)},
                                          g.p),
                                g.p),
                      g.print_str("after")},
                     g.p),
           {"a"});
    expect_clean(run_module(g.m, "temps: self-cycle"), "temps: self-cycle");
    check_eq(joined(), "a|after|", "temps: self-cycle output");
  }

  // A destructor run by a frame on its way off the stack: the frame's
  // last references go as it is discarded, and the destructor they start
  // calls two thousand deep, so the frame stack it pushes onto has to
  // grow -- under a frame that must already be out of it. Three ways off
  // the stack: a Ret whose frame holds the object only in a cell, a throw
  // unwinding a frame whose argument register holds it, and GenReturn
  // closing a generator whose local holds it.
  {
    const auto build = [](Prog& g) {
      using namespace coreir;
      const SrcPos& p = g.p;
      Builder& b = g.b;
      // funcs[1], the destructor: print(self.name); rec(rec, 2000)
      g.m.funcs[1].body = b.block(
          {g.m.funcs[1].body,
           b.call_value(b.make_closure(2, 0, p),
                        {b.make_closure(2, 0, p), b.literal(2000, p)}, p)},
          p);
      // funcs[2]: rec(r, n) = if n == 0 { 0 } else { r(r, n - 1) }
      Func rec{"rec", 2, 0,
               b.make_return(
                   b.make_if(b.binary(BinOp::Eq, g.local(1), b.literal(0, p), p),
                             b.literal(0, p),
                             b.call_value(g.local(0),
                                          {g.local(0),
                                           b.binary(BinOp::Sub, g.local(1),
                                                    b.literal(1, p), p)},
                                          p),
                             p),
                   p),
               {"r", "n"}, {}};
      rec.num_params = 2;
      g.m.funcs.push_back(rec);
    };
    const auto obj = [](Prog& g, const char* name) {
      return g.b.object_lit({{g.str("name"), g.str(name)},
                             {g.str("\x01" "drop"),
                              g.b.make_closure(1, 0, g.p)}},
                            g.p);
    };
    {
      // c() { cell0 = obj("c"); print("body") }; c(); print("after")
      Prog g;
      build(g);
      Func c{"c", 0, 0,
             g.b.block({g.b.assign(coreir::VarKind::Cell, 0, obj(g, "c"), g.p),
                        g.print_str("body")},
                       g.p),
             {}, {}};
      c.num_cells = 1;
      g.m.funcs.push_back(c);  // funcs[3]
      g.main(g.b.block({g.b.call_value(g.b.make_closure(3, 0, g.p), {}, g.p),
                        g.print_str("after")},
                       g.p),
             {});
      expect_clean(run_module(g.m, "frame exit: ret"), "frame exit: ret");
      check_eq(joined(), "body|c|after|", "frame exit: ret output");
    }
    {
      // two(x, y) = 0; boom() { throw "boom" }
      // try { two(obj("h"), boom()) } catch { print("caught") }
      Prog g;
      build(g);
      Func two{"two", 2, 0, g.b.literal(0, g.p), {"x", "y"}, {}};
      two.num_params = 2;
      g.m.funcs.push_back(two);  // funcs[3]
      g.m.funcs.push_back({"boom", 0, 0, g.b.make_throw(g.str("boom"), g.p),
                           {}, {}});  // funcs[4]
      Func h{"h", 0, 0,
             g.b.call_value(g.b.make_closure(3, 0, g.p),
                            {obj(g, "h"),
                             g.b.call_value(g.b.make_closure(4, 0, g.p), {},
                                            g.p)},
                            g.p),
             {}, {}};
      g.m.funcs.push_back(h);  // funcs[5]
      g.main(g.b.block({g.b.make_try(
                            1, g.b.call_value(g.b.make_closure(5, 0, g.p), {},
                                              g.p),
                            g.print_str("caught"), g.p),
                        g.print_str("after")},
                       g.p),
             {"e"});
      expect_clean(run_module(g.m, "frame exit: throw"), "frame exit: throw");
      check_eq(joined(), "h|caught|after|", "frame exit: throw output");
    }
    {
      // gen() { o = obj("gen"); yield 1 }
      // x = gen(); resume(x); return(x); print("after")
      Prog g;
      build(g);
      Func gen{"gen", 1, 0,
               g.b.block({g.b.assign(coreir::VarKind::Local, 0, obj(g, "gen"),
                                     g.p),
                          g.b.make_yield(g.b.literal(1, g.p), g.p)},
                         g.p),
               {"o"}, {}};
      gen.is_generator = true;
      g.m.funcs.push_back(gen);  // funcs[3]
      g.main(g.b.block(
                 {g.b.assign(coreir::VarKind::Local, 1,
                             g.b.call_value(g.b.make_closure(3, 0, g.p), {},
                                            g.p),
                             g.p),
                  g.b.intrinsic(coreir::IntrinsicId::GenResume,
                                {g.local(1), g.b.nil_literal(g.p)}, g.p),
                  g.b.intrinsic(coreir::IntrinsicId::GenReturn,
                                {g.local(1), g.b.nil_literal(g.p)}, g.p),
                  g.print_str("after")},
                 g.p),
             {"x"});
      expect_clean(run_module(g.m, "frame exit: close"), "frame exit: close");
      check_eq(joined(), "gen|after|", "frame exit: close output");
    }
  }

  // A scope exit that strips a dropped cycle (owned_scope_exit) releases the
  // cycle's members while the stripped object is being emptied; a member
  // bound outside the scope has its own destructor, which reads back into
  // the stripped object. The object must already be empty then, not
  // half-way through destroying its own properties.
  //   x = {K: "x", drop: dx}; { c = {K: "c", drop: d}; c.o = x; x.o = c; x = nil }
  //   dx(self) { print(self.o[K]) }  -- d prints self[K]
  // K is long enough to live on the heap, so a read of a destroyed key is
  // visible to a sanitizer rather than reading stale inline bytes.
  {
    const char* K = "a_rather_long_property_key_name";
    Prog g;
    g.m.funcs[1].body = g.print(g.b.index(g.local(0), g.str(K), g.p));
    coreir::Func dx{"dropx", 1, 0,
                    g.print(g.b.index(g.b.index(g.local(0), g.str("o"), g.p),
                                      g.str(K), g.p)),
                    {"self"}, {}};
    dx.num_params = 1;
    g.m.funcs.push_back(dx);  // funcs[2]
    const auto obj = [&](const char* name, int32_t drop_slot) {
      return g.b.object_lit({{g.str(K), g.str(name)},
                             {g.str("\x01" "drop"), g.local(drop_slot)}},
                            g.p);
    };
    g.main(g.b.block(
               {g.bind_drop(),
                g.b.assign(coreir::VarKind::Local, 1,
                           g.b.make_closure(2, 0, g.p), g.p),
                g.b.assign(coreir::VarKind::Local, 2, obj("x-name", 1), g.p),
                g.b.scope(3, 4,
                          g.b.block({g.b.assign(coreir::VarKind::Local, 3,
                                                obj("c-name", 0), g.p),
                                     g.set(3, "o", g.local(2)),
                                     g.set(2, "o", g.local(3)),
                                     g.b.assign(coreir::VarKind::Local, 2,
                                                g.b.nil_literal(g.p), g.p)},
                                    g.p),
                          g.p),
                g.print_str("after")},
               g.p),
           {"dx", "x", "c"});
    expect_clean(run_module(g.m, "strip: member destructor"),
                 "strip: member destructor");
    check_eq(joined(), "c-name|nil|after|", "strip: member destructor output");
  }

  // ObjectRemove releasing a value whose destructor writes back into the
  // object it was removed from -- enough keys to make the object's
  // storage grow, which it must not do under the remove itself.
  //   drop2(self) { self.back.x0 = 0; ... self.back.x15 = 15 }
  //   o = {}; o.a = {back: o, drop: drop2}; o.b0 = 0; o.b1 = 1; o.b2 = 2
  //   ObjectRemove(o, "a"); print(len(ObjectKeys(o)))
  {
    Prog g;
    std::vector<coreir::NodeId> writes;
    for (int i = 0; i < 16; ++i) {
      const std::string k = "x" + std::to_string(i);
      writes.push_back(g.b.set_index(g.b.index(g.local(0), g.str("back"), g.p),
                                     g.b.str_literal(k, g.p),
                                     g.b.literal(i, g.p), g.p));
    }
    coreir::Func d2{"drop2", 1, 0, g.b.block(writes, g.p), {"self"}, {}};
    d2.num_params = 1;
    g.m.funcs.push_back(d2);  // funcs[2]
    g.main(g.b.block(
               {g.b.assign(coreir::VarKind::Local, 1, g.b.object_lit({}, g.p),
                           g.p),
                g.set(1, "a",
                      g.b.object_lit({{g.str("back"), g.local(1)},
                                      {g.str("\x01" "drop"),
                                       g.b.make_closure(2, 0, g.p)}},
                                     g.p)),
                g.set(1, "b0", g.b.literal(0, g.p)),
                g.set(1, "b1", g.b.literal(1, g.p)),
                g.set(1, "b2", g.b.literal(2, g.p)),
                g.b.intrinsic(coreir::IntrinsicId::ObjectRemove,
                              {g.local(1), g.str("a")}, g.p),
                g.print(g.b.intrinsic(
                    coreir::IntrinsicId::Len,
                    {g.b.intrinsic(coreir::IntrinsicId::ObjectKeys,
                                   {g.local(1)}, g.p)},
                    g.p))},
               g.p),
           {"o"});
    expect_clean(run_module(g.m, "remove: drop writes back"),
                 "remove: drop writes back");
    check_eq(joined(), "19|", "remove: drop writes back output");
  }

  // --- 17. A container freed by its count reaching zero drops what it
  //         held first to last -- an array's elements, an object's props,
  //         a map's entries -- whatever order the standard library's own
  //         vector would destroy them in (libc++ goes backwards). --------
  {
    // { s = <container of a, b, c> }; print("after")
    const auto run = [&](const char* what, auto fill) {
      Prog g;
      g.main(g.b.block({g.bind_drop(), g.b.scope(1, 2, fill(g), g.p),
                        g.print_str("after")},
                       g.p),
             {"s"});
      expect_clean(run_module(g.m, what), what);
      check_eq(joined(), "a|b|c|after|", std::string(what) + " output");
    };
    run("order: array", [](Prog& g) {
      return g.b.assign(
          VarKind::Local, 1,
          g.b.array_lit({g.obj("a"), g.obj("b"), g.obj("c")}, g.p), g.p);
    });
    run("order: object", [](Prog& g) {
      return g.b.assign(VarKind::Local, 1,
                        g.b.object_lit({{g.str("x"), g.obj("a")},
                                        {g.str("y"), g.obj("b")},
                                        {g.str("z"), g.obj("c")}},
                                       g.p),
                        g.p);
    });
    run("order: map", [](Prog& g) {
      std::vector<NodeId> body{g.b.assign(
          VarKind::Local, 1, g.b.intrinsic(IntrinsicId::MapNew, {}, g.p), g.p)};
      const char* names[] = {"a", "b", "c"};
      for (int64_t k = 0; k < 3; ++k) {
        body.push_back(g.b.set_index(g.local(1), g.b.literal(k, g.p),
                                     g.obj(names[k]), g.p));
      }
      return g.b.block(body, g.p);
    });
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "drops: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("drops OK\n");
  return 0;
}
