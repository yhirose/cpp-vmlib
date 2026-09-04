// Host functions: a module declares the names it calls (Module::natives,
// reached through Tag::NativeRef), the run supplies them
// (RunOptions::natives), and a call to one runs C++ on the spot -- no
// frame -- with its answer landing where a closure's return would. What
// this pins: the linkage happens before the first instruction (a missing
// name fails the whole run, not the call site); a native fails a program
// only through NativeCall::error, which the program's own TryCatch can
// catch; a native can call back into the program (NativeCall::call), and a
// throw the callee lets out travels through the native to the caller's
// handler; natives are callable wherever a closure is (Enqueue, Defer, the
// drop key); and every native value is gone from the heap when the run
// ends.

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

std::string run_module(const coreir::Module& m, const std::string& what,
                       const vm::RunOptions& opts) {
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

// --- The host's functions. ------------------------------------------------

using coreir::NativeCall;
using coreir::Value;

// add(a, b): two ints, or a trap-shaped error for anything else.
bool native_add(NativeCall& c) {
  if (!c.arg(0).is_int() || !c.arg(1).is_int()) {
    c.error = c.trap("add wants two ints");
    return false;
  }
  c.result = Value::make_int(c.arg(0).as_int() + c.arg(1).as_int());
  return true;
}

// sum(...): variadic (arity -1); argc is the count actually passed.
bool native_sum(NativeCall& c) {
  int64_t total = 0;
  for (int32_t i = 0; i < c.argc; ++i) total += c.args[i].as_int();
  c.result = Value::make_int(total);
  return true;
}

// boom(): fails with a plain string -- what "uncaught: <value>" shows.
bool native_boom(NativeCall& c) {
  c.error = Value::make_str("kaboom");
  return false;
}

// twice(f, x): f(f(x)), calling back into the program.
bool native_twice(NativeCall& c) {
  const Value once = c.call(c.arg(0), &c.args[1], 1);
  c.result = c.call(c.arg(0), &once, 1);
  return true;
}

// count(): the ctx pointer, verbatim -- a host's own state.
bool native_count(NativeCall& c) {
  auto* n = static_cast<int*>(c.ctx);
  c.result = Value::make_int(++*n);
  return true;
}

// pair(a, b): a fresh heap value built on the host side.
bool native_pair(NativeCall& c) {
  c.result = Value::make_array({c.arg(0), c.arg(1)});
  return true;
}

// say(x): output from the host side, through the same hook the VM uses.
bool native_say(NativeCall& c) {
  const std::string s = "native:" + coreir::to_display(c.arg(0));
  coreir_rt_out_str(s.data(), static_cast<int64_t>(s.size()));
  c.result = Value();
  return true;
}

int g_count = 0;

vm::RunOptions host() {
  vm::RunOptions o;
  o.natives = {{"add", 2, &native_add, nullptr},
               {"sum", -1, &native_sum, nullptr},
               {"boom", 0, &native_boom, nullptr},
               {"twice", 2, &native_twice, nullptr},
               {"count", 0, &native_count, &g_count},
               {"pair", 2, &native_pair, nullptr},
               {"say", 1, &native_say, nullptr}};
  return o;
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

  auto pr = [&p](Builder& b, NodeId v) {
    return b.intrinsic(IntrinsicId::Print, {v}, p);
  };
  // The Static calls recipe over a native: one NativeRef into a cell at
  // the top, every call site a VarRef(Cell) + CallValue.
  auto hoist = [&p](Builder& b, int32_t cell, const char* name) {
    return b.assign(VarKind::Cell, cell,
                    b.native_ref(b.declare_native(name), p), p);
  };
  auto call = [&p](Builder& b, int32_t cell, std::vector<NodeId> args) {
    return b.call_value(b.varref(VarKind::Cell, cell, p), args, p);
  };

  // --- 1. Call, arity, type, variadic count, host state, host value. -----
  {
    Module m;
    Builder b(m);
    const NodeId add = b.varref(VarKind::Cell, 0, p);
    m.funcs.push_back(
        {"main", 1, 0,
         b.block({hoist(b, 0, "add"), hoist(b, 1, "sum"), hoist(b, 2, "count"),
                  hoist(b, 3, "pair"),
                  pr(b, call(b, 0, {b.literal(1, p), b.literal(2, p)})),
                  pr(b, b.intrinsic(IntrinsicId::FnArity, {add}, p)),
                  pr(b, b.intrinsic(IntrinsicId::TypeOf, {add}, p)),
                  pr(b, b.intrinsic(IntrinsicId::FnArity,
                                    {b.varref(VarKind::Cell, 1, p)}, p)),
                  pr(b, call(b, 1, {b.literal(1, p), b.literal(2, p),
                                    b.literal(3, p)})),
                  pr(b, call(b, 1, {})),
                  pr(b, call(b, 2, {})), pr(b, call(b, 2, {})),
                  b.assign(VarKind::Local, 0,
                           call(b, 3, {b.str_literal("x", p), b.literal(9, p)}),
                           p),
                  pr(b, b.index(b.varref(VarKind::Local, 0, p), b.literal(1, p),
                                p)),
                  pr(b, b.intrinsic(IntrinsicId::Len,
                                    {b.varref(VarKind::Local, 0, p)}, p))},
                 p),
         {"pr"}, {}});
    m.funcs[0].num_cells = 4;
    g_count = 0;
    check_eq(run_module(m, "call", host()), "", "call: failure");
    check_eq(joined(), "3|2|function|-1|6|0|1|2|9|2|", "call output");
  }

  // --- 2. A native's error is a throw at the call site: catchable, and
  // trap-shaped when the native asked for that shape. ----------------------
  // try { add("a", 1) } catch e { print e.message; print e.line }
  {
    Module m;
    Builder b(m);
    const NodeId e = b.varref(VarKind::Local, 0, p);
    m.funcs.push_back(
        {"main", 1, 0,
         b.block({hoist(b, 0, "add"),
                  b.make_try(0,
                             call(b, 0, {b.str_literal("a", p), b.literal(1, p)}),
                             b.block({pr(b, b.index(e, b.str_literal("message", p),
                                                    p)),
                                      pr(b, b.index(e, b.str_literal("line", p),
                                                    p))},
                                     p),
                             p),
                  pr(b, b.str_literal("after", p))},
                 p),
         {"e"}, {}});
    m.funcs[0].num_cells = 1;
    check_eq(run_module(m, "caught", host()), "", "caught: failure");
    check_eq(joined(), "add wants two ints|1|after|", "caught output");
  }
  // Uncaught, it ends the run the way an uncaught Throw does.
  {
    Module m;
    Builder b(m);
    m.funcs.push_back({"main", 0, 0,
                       b.block({hoist(b, 0, "boom"), call(b, 0, {}),
                                pr(b, b.str_literal("never", p))},
                               p),
                       {}, {}});
    m.funcs[0].num_cells = 1;
    check_eq(run_module(m, "uncaught", host()), "uncaught: kaboom",
             "uncaught: failure");
    check_eq(joined(), "", "uncaught output");
  }

  // --- 3. Calling back: twice(f, x) = f(f(x)); a throw out of f reaches
  // the program's handler around the native call. ------------------------
  // inc(x) { x + 1 }   thrower(x) { throw "inner" }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back({"inc", 1, 0,
                       b.binary(BinOp::Add, b.varref(VarKind::Local, 0, p),
                                b.literal(1, p), p),
                       {"x"}, {}});
    m.funcs.back().num_params = 1;
    m.funcs.push_back({"thrower", 1, 0,
                       b.make_throw(b.str_literal("inner", p), p), {"x"}, {}});
    m.funcs.back().num_params = 1;
    const NodeId e = b.varref(VarKind::Local, 0, p);
    m.funcs[0] = {"main", 1, 0,
                  b.block({hoist(b, 0, "twice"), hoist(b, 1, "sum"),
                           pr(b, call(b, 0, {b.make_closure(1, 0, p),
                                             b.literal(5, p)})),
                           b.make_try(0,
                                      call(b, 0, {b.make_closure(2, 0, p),
                                                  b.literal(5, p)}),
                                      pr(b, b.binary(BinOp::Add,
                                                     b.str_literal("caught ", p),
                                                     e, p)),
                                      p),
                           // A native calling a native: sum(sum(5)).
                           pr(b, call(b, 0, {b.varref(VarKind::Cell, 1, p),
                                             b.literal(5, p)}))},
                          p),
                  {"e"}, {}};
    m.funcs[0].num_cells = 2;
    check_eq(run_module(m, "callback", host()), "", "callback: failure");
    check_eq(joined(), "7|caught inner|5|", "callback output");
  }

  // --- 4. Callable wherever a closure is: a job, a defer, a destructor. --
  // main { enqueue say-native; scope { defer count-native... } }
  {
    Module m;
    Builder b(m);
    const NodeId o = b.varref(VarKind::Local, 0, p);
    m.funcs.push_back(
        {"main", 1, 0,
         b.scope(0, 1,
                 b.block({hoist(b, 0, "say"), hoist(b, 1, "count"),
                          b.intrinsic(IntrinsicId::Enqueue,
                                      {b.varref(VarKind::Cell, 1, p)}, p),
                          b.make_defer(b.varref(VarKind::Cell, 1, p), p),
                          // {drop: say}: the destructor is the native, and
                          // it is handed the object.
                          b.assign(VarKind::Local, 0,
                                   b.object_lit(
                                       {{b.str_literal("\x01" "drop", p),
                                         b.varref(VarKind::Cell, 0, p)}},
                                       p),
                                   p),
                          pr(b, b.str_literal("body", p))},
                         p),
                 p),
         {"o"}, {}});
    m.funcs[0].num_cells = 2;
    g_count = 0;
    check_eq(run_module(m, "everywhere", host()), "", "everywhere: failure");
    // body; then the scope exits: its defer (count -> 1, unprinted), then
    // its local's destructor (say prints the object); then the job.
    check_eq(joined(), "body|native:<object>|", "everywhere output");
    check_eq(std::to_string(g_count), "2", "everywhere: defer and job ran");
  }

  // --- 5. Arity is checked like a closure's. -----------------------------
  {
    Module m;
    Builder b(m);
    m.funcs.push_back({"main", 0, 0,
                       b.block({hoist(b, 0, "add"),
                                pr(b, call(b, 0, {b.literal(1, p)}))},
                               p),
                       {}, {}});
    m.funcs[0].num_cells = 1;
    check_eq(run_module(m, "arity", host()),
             "add takes 2 argument(s), given 1", "arity: failure");
  }

  // --- 6. A name the host does not supply fails before anything runs. ----
  {
    Module m;
    Builder b(m);
    m.funcs.push_back({"main", 0, 0,
                       b.block({pr(b, b.str_literal("started", p)),
                                hoist(b, 0, "missing")},
                               p),
                       {}, {}});
    m.funcs[0].num_cells = 1;
    check_eq(run_module(m, "unresolved", host()),
             "unresolved native 'missing'", "unresolved: failure");
    check_eq(joined(), "", "unresolved output");
  }

  // --- 7. Native values survive a collection: they are the run's roots. --
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 0, 0,
         b.block({hoist(b, 0, "add"),
                  b.intrinsic(IntrinsicId::Collect, {}, p),
                  pr(b, call(b, 0, {b.literal(20, p), b.literal(22, p)})),
                  pr(b, b.intrinsic(IntrinsicId::Same,
                                    {b.varref(VarKind::Cell, 0, p),
                                     b.native_ref(b.declare_native("add"), p)},
                                    p))},
                 p),
         {}, {}});
    m.funcs[0].num_cells = 1;
    check_eq(run_module(m, "roots", host()), "", "roots: failure");
    check_eq(joined(), "42|true|", "roots output");
  }

  // --- 8. verify() bounds the index. -------------------------------------
  {
    Module m;
    Builder b(m);
    m.funcs.push_back({"main", 0, 0, pr(b, b.native_ref(0, p)), {}, {}});
    const auto err = verify(m);
    check_eq(err ? *err : "", "native index out of range", "verify: index");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("natives: OK");
  return 0;
}
