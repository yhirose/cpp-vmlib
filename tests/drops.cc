// Deterministic drop, pinned against culebra's rules: on every kind of
// scope exit -- fall-through, Return, Break, an unwinding throw -- the
// scope runs its defers (LIFO) and then releases its locals last-declared-
// first, innermost scope first. Locals a then b, both carrying a drop hook
// that prints their name, must therefore print "b" then "a" in every case.

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "coreir/ir.h"
#include "coreir/value.h"
#include "vm/compiler.h"
#include "vm/exec.h"

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
  coreir::NodeId mk(int32_t slot, const char* name) {
    return b.block({b.assign(coreir::VarKind::Local, slot,
                             b.object_lit({{str("name"), str(name)},
                                           {str("\x01" "drop"), local(0)}},
                                          p),
                             p)},
                   p);
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

  if (g_failures != 0) {
    std::fprintf(stderr, "drops: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("drops OK\n");
  return 0;
}
