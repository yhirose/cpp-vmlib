// A benchmark harness for the executor, built the way test/ builds its cases:
// Core-IR by hand, its own coreir_rt host, one binary. Each case is a program
// shaped to load one part of the executor -- calls, variable access, the
// arithmetic dispatch, string constants, allocation -- so a change to that
// part shows up here as a number rather than as an argument.
//
// Timing covers vm::run only: building the IR and compiling it happen once,
// outside the clock, because a front end pays those once per program and this
// is about what a program pays per instruction.
//
// Output is one TSV row per case (name, best ms, mean ms, a checksum of what
// the program printed), so two builds can be diffed by bench/compare.sh. The
// checksum is what makes a comparison honest: a change that makes a case fast
// by making it do less work changes the checksum, and the diff says so.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "vmlib.h"

namespace {

// The host: output goes into a hash rather than to a terminal, so a case may
// print without the I/O landing in the measurement.
uint64_t g_hash = 1469598103934665603ull;  // FNV-1a

void sink(const char* bytes, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    g_hash ^= static_cast<unsigned char>(bytes[i]);
    g_hash *= 1099511628211ull;
  }
}

}  // namespace

extern "C" {
void coreir_rt_out(int64_t v) {
  const std::string s = std::to_string(v);
  sink(s.data(), s.size());
}
void coreir_rt_out_str(const char* bytes, int64_t len) {
  sink(bytes, static_cast<size_t>(len));
}
void coreir_rt_out_raw(const char* bytes, int64_t len) {
  sink(bytes, static_cast<size_t>(len));
}
int64_t coreir_rt_in(int64_t, int64_t) { return 0; }
void coreir_rt_poll(void) {}
[[noreturn]] void coreir_rt_fail(const char* msg, int64_t line, int64_t col) {
  std::fprintf(stderr, "bench: run failed at %lld:%lld: %s\n",
               static_cast<long long>(line), static_cast<long long>(col), msg);
  std::exit(1);
}
}

namespace {

using namespace coreir;

const SrcPos P{1, 1};

NodeId loc(Builder& b, int32_t i) { return b.varref(VarKind::Local, i, P); }
NodeId cell(Builder& b, int32_t i) { return b.varref(VarKind::Cell, i, P); }
NodeId cap(Builder& b, int32_t i) { return b.varref(VarKind::Capture, i, P); }
NodeId lit(Builder& b, int64_t v) { return b.literal(v, P); }
NodeId set_loc(Builder& b, int32_t i, NodeId v) {
  return b.assign(VarKind::Local, i, v, P);
}
NodeId add(Builder& b, NodeId l, NodeId r) {
  return b.binary(BinOp::Add, l, r, P);
}
NodeId lt(Builder& b, NodeId l, NodeId r) {
  return b.binary(BinOp::Lt, l, r, P);
}
NodeId print(Builder& b, NodeId v) {
  return b.intrinsic(IntrinsicId::Print, {v}, P);
}
NodeId len_of(Builder& b, NodeId v) {
  return b.intrinsic(IntrinsicId::Len, {v}, P);
}

// `i` is always local slot 0 in these cases: `i = 0; while (i < n) { body; i =
// i + 1 }`, the shape every loop case here wants.
NodeId counted_loop(Builder& b, int64_t n, const std::vector<NodeId>& body) {
  std::vector<NodeId> stmts = body;
  stmts.push_back(set_loc(b, 0, add(b, loc(b, 0), lit(b, 1))));
  return b.block({set_loc(b, 0, lit(b, 0)),
                  b.make_while(lt(b, loc(b, 0), lit(b, n)), b.block(stmts, P),
                               P)},
                 P);
}

// A module whose only function is `main`: its locals named (their count is
// the function's), and its body wrapped in the Scope that owns them -- the
// five lines every case below would otherwise repeat, with the local count
// written once instead of once per case in two places that could drift.
// `make_body` gets the module too, for a case that adds a function of its
// own.
template <class Fn>
Module main_only(std::vector<std::string> names, Fn make_body) {
  Module m;
  Builder b(m);
  m.funcs.push_back({});  // main, filled in below
  const int32_t n = static_cast<int32_t>(names.size());
  const NodeId body = make_body(m, b);
  m.funcs[0] = {"main", n, 0, b.scope(0, n, body, P), std::move(names), {}};
  return m;
}

// --- The cases ------------------------------------------------------------

// Naive recursive fib: a call per node of the recursion tree, none of them a
// tail call, so this is the frame-per-call path and nothing else.
Module bench_call_fib() {
  Module m;
  Builder b(m);
  m.capture_maps.push_back({{VarKind::Cell, 0}});
  m.funcs.push_back({});  // main, filled in below
  m.funcs.push_back(
      {"fib", 1, 1,
       b.make_if(lt(b, loc(b, 0), lit(b, 2)), loc(b, 0),
                 add(b, b.call_value(cap(b, 0),
                                     {b.binary(BinOp::Sub, loc(b, 0),
                                               lit(b, 1), P)},
                                     P),
                     b.call_value(cap(b, 0),
                                  {b.binary(BinOp::Sub, loc(b, 0), lit(b, 2),
                                            P)},
                                  P)),
                 P),
       {"n"}, {"fib"}});
  m.funcs.back().num_params = 1;
  m.funcs[0] = {"main", 0, 0,
                b.block({b.assign(VarKind::Cell, 0, b.make_closure(1, 0, P), P),
                         print(b, b.call_value(cell(b, 0), {lit(b, 30)}, P)),
                         b.assign(VarKind::Cell, 0, b.nil_literal(P), P)},
                        P),
                {}, {}};
  m.funcs[0].num_cells = 1;
  return m;
}

// A 1-argument closure called in a loop: the same frame-per-call path as
// call_fib, at depth one, so a call's own cost is not mixed with recursion.
Module bench_call_loop() {
  return main_only({"i", "acc", "f"}, [](Module& m, Builder& b) {
    m.capture_maps.push_back({});
    m.funcs.push_back({"add1", 1, 0, add(b, loc(b, 0), lit(b, 1)), {"x"}, {}});
    m.funcs.back().num_params = 1;
    return b.block(
        {set_loc(b, 1, lit(b, 0)), set_loc(b, 2, b.make_closure(1, 0, P)),
         counted_loop(b, 2000000,
                      {set_loc(b, 1, b.call_value(loc(b, 2), {loc(b, 1)}, P))}),
         print(b, loc(b, 1))},
        P);
  });
}

// A self-tail-recursive accumulator: sum(n, acc) = n == 0 ? acc : sum(n -
// 1, acc + n). With Func::tail_calls on, the call in the else branch is a
// TailCall -- it reuses the current frame instead of pushing a new one --
// which is the steady-state shape mini-lua/mini-scheme/mini-culebra/
// mini-csharp's own tail-call recipes actually run at runtime, and until
// now nothing here put a number on it the way call_fib does for the
// frame-per-call path it deliberately avoids.
Module bench_tail_call() {
  Module m;
  Builder b(m);
  m.capture_maps.push_back({{VarKind::Cell, 0}});
  m.funcs.push_back({});  // main, filled in below
  m.funcs.push_back(
      {"sum", 2, 1,
       b.make_if(
           b.binary(BinOp::Eq, loc(b, 0), lit(b, 0), P), loc(b, 1),
           b.call_value(cap(b, 0),
                        {b.binary(BinOp::Sub, loc(b, 0), lit(b, 1), P),
                         add(b, loc(b, 1), loc(b, 0))},
                        P),
           P),
       {"n", "acc"}, {"sum"}});
  m.funcs.back().num_params = 2;
  m.funcs.back().tail_calls = true;
  m.funcs[0] = {"main", 0, 0,
                b.block({b.assign(VarKind::Cell, 0, b.make_closure(1, 0, P), P),
                         print(b, b.call_value(cell(b, 0),
                                               {lit(b, 5000000), lit(b, 0)},
                                               P)),
                         b.assign(VarKind::Cell, 0, b.nil_literal(P), P)},
                        P),
                {}, {}};
  m.funcs[0].num_cells = 1;
  return m;
}

// Arithmetic and comparison through the executor's binop dispatch, over
// locals: the Add/Sub/Mul/Lt path plus the local reads and writes it takes
// to feed it.  sum = sum + (i * 2 - 1)
Module bench_loop_arith() {
  return main_only({"i", "sum"}, [](Module&, Builder& b) {
    return b.block(
        {set_loc(b, 1, lit(b, 0)),
         counted_loop(b, 3000000,
                      {set_loc(b, 1,
                               add(b, loc(b, 1),
                                   b.binary(BinOp::Sub,
                                            b.binary(BinOp::Mul, loc(b, 0),
                                                     lit(b, 2), P),
                                            lit(b, 1), P)))}),
         print(b, loc(b, 1))},
        P);
  });
}

// Variable traffic with as little else as possible: six local reads/writes
// per iteration against one add and one compare, so LoadLocal/StoreLocal is
// what moves the number.
Module bench_var_access() {
  return main_only({"i", "a", "b", "c"}, [](Module&, Builder& b) {
    return b.block(
        {set_loc(b, 1, lit(b, 1)), set_loc(b, 2, lit(b, 2)),
         set_loc(b, 3, lit(b, 3)),
         counted_loop(b, 3000000,
                      {set_loc(b, 1, loc(b, 2)), set_loc(b, 2, loc(b, 3)),
                       set_loc(b, 3, loc(b, 1))}),
         print(b, loc(b, 3))},
        P);
  });
}

// A string literal evaluated a million times. Every other literal kind is a
// payload word; this one is what const_value does with ConstKind::Str.
Module bench_str_const() {
  return main_only({"i", "s"}, [](Module&, Builder& b) {
    return b.block(
        {set_loc(b, 1, b.str_literal("", P)),
         counted_loop(b, 3000000,
                      {set_loc(b, 1, b.str_literal("a benchmark string constant",
                                                   P))}),
         print(b, len_of(b, loc(b, 1)))},
        P);
  });
}

// Repeated concatenation: a fresh string per step, copying what came before.
// Quadratic by construction, so what it measures is the allocator and memcpy
// rather than the dispatch -- which is the point, since that is what a front
// end lowering `s += x` in a loop actually pays. Kept short for the same
// reason: past a certain length it only measures them harder.
Module bench_str_concat() {
  return main_only({"i", "s"}, [](Module&, Builder& b) {
    return b.block(
        {set_loc(b, 1, b.str_literal("", P)),
         counted_loop(b, 100000,
                      {set_loc(b, 1, add(b, loc(b, 1), b.str_literal("x", P)))}),
         print(b, len_of(b, loc(b, 1)))},
        P);
  });
}

// FieldGet/FieldSet against a three-field object: the slot-indexed path a
// front end with structs compiles to.
// o = {x: 0, y: 1, z: 2}; o.x = o.x + o.y
Module bench_field_access() {
  return main_only({"i", "o"}, [](Module&, Builder& b) {
    const NodeId obj = b.object_lit({{b.str_literal("x", P), lit(b, 0)},
                                     {b.str_literal("y", P), lit(b, 1)},
                                     {b.str_literal("z", P), lit(b, 2)}},
                                    P);
    return b.block(
        {set_loc(b, 1, obj),
         counted_loop(b, 3000000,
                      {b.field_set(loc(b, 1), 0, "x",
                                   add(b, b.field_get(loc(b, 1), 0, "x", P),
                                       b.field_get(loc(b, 1), 1, "y", P)),
                                   P)}),
         print(b, b.field_get(loc(b, 1), 0, "x", P))},
        P);
  });
}

// A fresh three-element array per iteration, dropped on the next one: the
// allocator, the refcount release, and whatever the GC threshold makes of a
// million short-lived objects.
Module bench_alloc() {
  return main_only({"i", "a"}, [](Module&, Builder& b) {
    return b.block(
        {set_loc(b, 1, b.array_lit({lit(b, 0)}, P)),
         counted_loop(b, 2000000,
                      {set_loc(b, 1,
                               b.array_lit({loc(b, 0),
                                            add(b, loc(b, 0), lit(b, 1)),
                                            add(b, loc(b, 0), lit(b, 2))},
                                           P))}),
         print(b, len_of(b, loc(b, 1)))},
        P);
  });
}

struct Case {
  const char* name;
  Module (*build)();
};

const Case kCases[] = {
    {"call_fib", bench_call_fib},         {"call_loop", bench_call_loop},
    {"tail_call", bench_tail_call},
    {"loop_arith", bench_loop_arith},     {"var_access", bench_var_access},
    {"str_const", bench_str_const},       {"str_concat", bench_str_concat},
    {"field_access", bench_field_access}, {"alloc", bench_alloc},
};

double run_once(const vm::Program& p) {
  Runtime rt;
  const auto t0 = std::chrono::steady_clock::now();
  vm::run(p, rt);
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
  int reps = 5;
  std::vector<std::string> want;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--reps" && i + 1 < argc) {
      reps = std::atoi(argv[++i]);
    } else if (a == "--help" || a == "-h") {
      std::printf("usage: bench [--reps N] [case ...]\n");
      for (const Case& c : kCases) std::printf("  %s\n", c.name);
      return 0;
    } else {
      want.push_back(a);
    }
  }
  if (reps < 1) reps = 1;

  std::printf("name\tbest_ms\tmean_ms\tcheck\n");
  for (const Case& c : kCases) {
    if (!want.empty() &&
        std::find(want.begin(), want.end(), c.name) == want.end()) {
      continue;
    }
    const Module m = c.build();
    if (auto err = verify(m)) {
      std::fprintf(stderr, "bench: %s: malformed IR: %s\n", c.name,
                   err->c_str());
      return 1;
    }
    const vm::Program p = vm::compile(m);
    double best = 0, total = 0;
    for (int r = 0; r < reps; ++r) {
      g_hash = 1469598103934665603ull;
      const double ms = run_once(p);
      if (r == 0 || ms < best) best = ms;
      total += ms;
    }
    std::printf("%s\t%.1f\t%.1f\t%016llx\n", c.name, best, total / reps,
                static_cast<unsigned long long>(g_hash));
    std::fflush(stdout);
  }
  return 0;
}
