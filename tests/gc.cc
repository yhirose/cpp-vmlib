// The tracing backstop, driven directly: build the cycles reference counting
// cannot free, drop every outside handle, and watch collect() take exactly
// them -- and nothing that is still held, from the heap or from C++. Then
// the same collector driven from a program, through the Collect and
// HeapStats intrinsics.

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "coreir/ir.h"
#include "coreir/value.h"
#include "vm/compiler.h"
#include "vm/exec.h"

using namespace coreir;

namespace {

struct Failure : std::runtime_error {
  Failure(std::string msg) : std::runtime_error(std::move(msg)) {}
};

std::vector<std::string> g_out;
int g_failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

void check_eq(const std::string& got, const std::string& want,
              const char* what) {
  if (got != want) {
    std::fprintf(stderr, "FAIL: %s\n  want [%s]\n  got  [%s]\n", what,
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

// Runs a module, returning the host failure message (empty if none) and
// asserting the heap ended empty -- before ~Runtime, which would hide a
// cycle the program left behind.
std::string run_module(const Module& m, const char* what) {
  g_out.clear();
  if (auto err = verify(m)) {
    std::fprintf(stderr, "FAIL: %s: malformed IR: %s\n", what, err->c_str());
    ++g_failures;
    return {};
  }
  std::string failure;
  int64_t left = 0;
  {
    Runtime rt;
    const vm::Program p = vm::compile(m);
    try {
      vm::run(p, rt);
    } catch (const Failure& e) {
      failure = e.what();
    }
    left = rt.live_objects();
  }
  check(left == 0, what);
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
  // --- 1. A self-referential array: one line of cycle. --------------------
  {
    Runtime rt;
    Runtime::Scope scope(rt);
    {
      Value a = Value::make_array({});
      a.as_array()->items.push_back(a);
    }
    check(rt.live_objects() == 1, "self-cycle survives counting");
    rt.collect();
    check(rt.live_objects() == 0, "self-cycle collected");
  }

  // --- 2. The closure cycle: cell -> closure -> cell. ---------------------
  {
    Runtime rt;
    Runtime::Scope scope(rt);
    {
      Value cell = Value::make_cell();
      Value clo = Value::make_closure(0, {cell});
      cell.as_cell()->v = clo;
    }
    check(rt.live_objects() == 2, "closure cycle survives counting");
    rt.collect();
    check(rt.live_objects() == 0, "closure cycle collected");
  }

  // --- 3. A held cycle stays; releasing the handle frees it next time. ----
  {
    Runtime rt;
    Runtime::Scope scope(rt);
    Value a = Value::make_array({});
    a.as_array()->items.push_back(a);
    Value keep = Value::make_str("held");
    rt.collect();
    check(rt.live_objects() == 2, "held cycle and string survive a collect");
    a = Value();
    rt.collect();
    check(rt.live_objects() == 1, "released cycle goes, held string stays");
  }

  // --- 4. A cycle reachable FROM a root is not garbage. -------------------
  {
    Runtime rt;
    Runtime::Scope scope(rt);
    Value holder = Value::make_array({});
    {
      Value a = Value::make_array({});
      a.as_array()->items.push_back(a);
      holder.as_array()->items.push_back(a);
    }
    rt.collect();
    check(rt.live_objects() == 2, "cycle hanging off a held array survives");
    holder = Value();
    rt.collect();
    check(rt.live_objects() == 0, "and goes when the holder does");
  }

  // --- 5. The finalize hook sees each condemned object, before any frees. -
  {
    Runtime rt;
    Runtime::Scope scope(rt);
    {
      Value a = Value::make_array({});
      a.as_array()->items.push_back(a);
      Value b = Value::make_cell();
      Value c = Value::make_closure(0, {b});
      b.as_cell()->v = c;
    }
    int seen = 0;
    rt.set_finalize_fn(&seen, [](void* ctx, HeapObj*) {
      ++*static_cast<int*>(ctx);
    });
    rt.collect();
    check(seen == 3, "finalize saw all three condemned objects");
    check(rt.live_objects() == 0, "and they were freed");
  }

  // --- 6. The drop hook fires at refcount zero, pinned; a resurrecting
  //        hook skips the free and the object drops again later. ----------
  {
    Runtime rt;
    Runtime::Scope scope(rt);
    static int fired = 0;
    static Value* keeper = nullptr;
    Value kv = Value();
    keeper = &kv;
    fired = 0;
    rt.set_drop_fn(nullptr, [](void*, HeapObj* h) {
      ++fired;
      if (fired == 1) *keeper = Value::make_ref(h);  // resurrect once
    });
    {
      Value o = Value::make_object();
      o.as_object()->set("\x01" "drop", Value::make_str("marker"));
    }
    check(fired == 1, "hook fired at zero");
    check(rt.live_objects() == 2, "resurrected object still alive");
    kv = Value();
    check(fired == 2, "hook fired again on re-release");
    rt.set_drop_fn(nullptr, nullptr);
    keeper = nullptr;
  }

  // --- 7. Collect and HeapStats from a program: a dropped cycle is what
  //        collect() frees, and what is gone after. No allocation sits
  //        between the drop and the collect: under COREIR_GC_STRESS one
  //        would take the cycle first, and the count is what is pinned. -
  // a = []; arraypush(a, a); a = nil
  // print(collect()); print(heapstats().live_objects); print(collect())
  {
    const SrcPos p{1, 1};
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    auto print = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::Print, {v}, p);
    };
    auto stat = [&](const char* key) {
      return b.index(b.intrinsic(IntrinsicId::HeapStats, {}, p),
                     b.str_literal(key, p), p);
    };
    auto local = [&](int32_t i) { return b.varref(VarKind::Local, i, p); };
    m.funcs.push_back(
        {"main", 1, 0,
         b.block({b.assign(VarKind::Local, 0, b.array_lit({}, p), p),
                  b.intrinsic(IntrinsicId::ArrayPush, {local(0), local(0)},
                              p),
                  b.assign(VarKind::Local, 0, b.nil_literal(p), p),
                  print(b.intrinsic(IntrinsicId::Collect, {}, p)),
                  print(stat("live_objects")),
                  print(b.intrinsic(IntrinsicId::Collect, {}, p))},
                 p),
         {"a"},
         {}});
    check_eq(run_module(m, "collect: heap not empty"), "",
             "collect: unexpected failure");
    check_eq(joined(), "1|0|0|", "collect output");
  }

  // --- 8. heap_bytes follows what is held: an array's storage counts while
  //        the array is live, and the number is 0 once nothing is. --------
  // a = [1, 2, 3]; n = heapstats().heap_bytes; a = nil;
  // m = heapstats().heap_bytes; print(n > m); print(m)
  {
    const SrcPos p{1, 1};
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    auto print = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::Print, {v}, p);
    };
    auto stat = [&](const char* key) {
      return b.index(b.intrinsic(IntrinsicId::HeapStats, {}, p),
                     b.str_literal(key, p), p);
    };
    auto local = [&](int32_t i) { return b.varref(VarKind::Local, i, p); };
    m.funcs.push_back(
        {"main", 3, 0,
         b.block({b.assign(VarKind::Local, 0,
                           b.array_lit({b.literal(1, p), b.literal(2, p),
                                        b.literal(3, p)},
                                       p),
                           p),
                  b.assign(VarKind::Local, 1, stat("heap_bytes"), p),
                  b.assign(VarKind::Local, 0, b.nil_literal(p), p),
                  b.assign(VarKind::Local, 2, stat("heap_bytes"), p),
                  print(b.binary(BinOp::Gt, local(1), local(2), p)),
                  print(local(2))},
                 p),
         {"a", "n", "m"},
         {}});
    check_eq(run_module(m, "heap_bytes: heap not empty"), "",
             "heap_bytes: unexpected failure");
    check_eq(joined(), "true|0|", "heap_bytes output");
  }

  // --- 9. What Collect does to a drop hook is the collector's existing
  //        contract, not a new one: the refcount-zero path runs the
  //        destructor, a collection does not (its condemned set goes to
  //        the finalize hook, which the executor does not install). Pinned
  //        so that changing it shows up as a change rather than silence. -
  // o = {}; o["\x01drop"] = fn (self) { print("dropped") }; o = nil
  // o = {}; o["\x01drop"] = <same>; o["self"] = o; o = nil
  // print(collect())
  {
    const SrcPos p{1, 1};
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    auto print = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::Print, {v}, p);
    };
    auto local = [&](int32_t i) { return b.varref(VarKind::Local, i, p); };
    auto set_drop = [&]() {
      return b.set_index(local(0), b.str_literal("\x01" "drop", p),
                         b.make_closure(1, 0, p), p);
    };
    m.funcs.push_back(
        {"main", 1, 0,
         b.block({b.assign(VarKind::Local, 0, b.object_lit({}, p), p),
                  set_drop(),
                  b.assign(VarKind::Local, 0, b.nil_literal(p), p),
                  print(b.str_literal("--", p)),
                  b.assign(VarKind::Local, 0, b.object_lit({}, p), p),
                  set_drop(),
                  b.set_index(local(0), b.str_literal("self", p), local(0),
                              p),
                  b.assign(VarKind::Local, 0, b.nil_literal(p), p),
                  print(b.intrinsic(IntrinsicId::Collect, {}, p))},
                 p),
         {"o"},
         {}});
    Func d{"drop", 1, 0, NodeId{}, {"self"}, {}};
    d.num_params = 1;
    d.body = print(b.str_literal("dropped", p));
    m.funcs.push_back(d);
    check_eq(run_module(m, "collect+drop: heap not empty"), "",
             "collect+drop: unexpected failure");
    check_eq(joined(), "dropped|--|2|", "collect+drop output");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "gc: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("gc OK\n");
  return 0;
}
