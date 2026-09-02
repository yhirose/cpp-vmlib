// The tracing backstop, driven directly: build the cycles reference counting
// cannot free, drop every outside handle, and watch collect() take exactly
// them -- and nothing that is still held, from the heap or from C++. Then
// the same collector driven from a program, through the Collect and
// HeapStats intrinsics.

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "vmlib.h"

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
  //        hook skips the free, and the object is not dropped again when
  //        it dies for good -- at most once, whichever path gets there. --
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
    check(fired == 1, "no second drop on re-release");
    check(rt.live_objects() == 0, "and the re-release frees it");
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

  // --- 9. A destructor runs however its object dies: at refcount zero on
  //        the spot, or in the collection that condemns its cycle -- each
  //        member exactly once, newest object first, over a still-whole
  //        cycle (b's destructor still reads b.other.name). --------------
  // d = fn (self) { print(self.name); print(self.other.name) }
  // o = {}; o.name = "solo"; o.other = {name: "-"}; o["\x01drop"] = d
  // o = nil                                   -> solo, - (refcount path)
  // a = {}; a.name = "a"; a["\x01drop"] = d
  // b = {}; b.name = "b"; b["\x01drop"] = d
  // a.other = b; b.other = a; a = nil; b = nil
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
    auto field = [&](NodeId o, const char* k) {
      return b.index(o, b.str_literal(k, p), p);
    };
    auto set = [&](int32_t slot, const char* k, NodeId v) {
      return b.set_index(local(slot), b.str_literal(k, p), v, p);
    };
    // Slots: 0 = d, 1 = o, 2 = a, 3 = b.
    m.funcs.push_back(
        {"main", 4, 0,
         b.block({b.assign(VarKind::Local, 0, b.make_closure(1, 0, p), p),
                  b.assign(VarKind::Local, 1, b.object_lit({}, p), p),
                  set(1, "name", b.str_literal("solo", p)),
                  set(1, "other",
                      b.object_lit({{b.str_literal("name", p),
                                     b.str_literal("-", p)}},
                                   p)),
                  set(1, "\x01" "drop", local(0)),
                  b.assign(VarKind::Local, 1, b.nil_literal(p), p),
                  b.assign(VarKind::Local, 2, b.object_lit({}, p), p),
                  set(2, "name", b.str_literal("a", p)),
                  set(2, "\x01" "drop", local(0)),
                  b.assign(VarKind::Local, 3, b.object_lit({}, p), p),
                  set(3, "name", b.str_literal("b", p)),
                  set(3, "\x01" "drop", local(0)),
                  set(2, "other", local(3)),
                  set(3, "other", local(2)),
                  b.assign(VarKind::Local, 2, b.nil_literal(p), p),
                  b.assign(VarKind::Local, 3, b.nil_literal(p), p),
                  print(b.intrinsic(IntrinsicId::Collect, {}, p))},
                 p),
         {"d", "o", "a", "b"},
         {}});
    Func d{"drop", 1, 0, NodeId{}, {"self"}, {}};
    d.num_params = 1;
    d.body = b.block({print(field(local(0), "name")),
                      print(field(field(local(0), "other"), "name"))},
                     p);
    m.funcs.push_back(d);
    check_eq(run_module(m, "collect+drop: heap not empty"), "",
             "collect+drop: unexpected failure");
    // Freed: a, b, and their two name strings. d is still in its slot.
    check_eq(joined(), "solo|-|b|a|a|b|4|", "collect+drop output");
  }

  // --- 10. Resurrection from a collection: a destructor that stores its
  //         object somewhere reachable spares it (and, through it, the
  //         rest of its cycle), intact; it goes for real the next time
  //         nothing reaches it, without a second destructor. -----------
  // keep = []; n = 0
  // d = fn (self) { n = n + 1; if n == 1 { arraypush(keep, self) } }
  // o = {}; o["\x01drop"] = d; o.self = o; o = nil
  // print(collect()); print(len(keep)); print(n)
  // keep = []; print(collect()); print(n)
  {
    const SrcPos p{1, 1};
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.capture_maps.push_back({{VarKind::Cell, 0}, {VarKind::Cell, 1}});
    auto print = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::Print, {v}, p);
    };
    auto local = [&](int32_t i) { return b.varref(VarKind::Local, i, p); };
    auto cell = [&](int32_t i) { return b.varref(VarKind::Cell, i, p); };
    auto collect = [&]() {
      return print(b.intrinsic(IntrinsicId::Collect, {}, p));
    };
    Func main{"main", 1, 0, NodeId{}, {"o"}, {}};
    main.num_cells = 2;
    main.body = b.block(
        {b.cell_fresh(0, p), b.cell_fresh(1, p),
         b.assign(VarKind::Cell, 0, b.array_lit({}, p), p),
         b.assign(VarKind::Cell, 1, b.literal(0, p), p),
         b.assign(VarKind::Local, 0, b.object_lit({}, p), p),
         b.set_index(local(0), b.str_literal("\x01" "drop", p),
                     b.make_closure(1, 1, p), p),
         b.set_index(local(0), b.str_literal("self", p), local(0), p),
         b.assign(VarKind::Local, 0, b.nil_literal(p), p),
         collect(), print(b.intrinsic(IntrinsicId::Len, {cell(0)}, p)),
         print(cell(1)),
         b.assign(VarKind::Cell, 0, b.array_lit({}, p), p),
         collect(), print(cell(1))},
        p);
    m.funcs.push_back(main);
    Func d{"drop", 1, 2, NodeId{}, {"self"}, {"keep", "n"}};
    d.num_params = 1;
    auto cap = [&](int32_t i) { return b.varref(VarKind::Capture, i, p); };
    d.body = b.block(
        {b.assign(VarKind::Capture, 1,
                  b.binary(BinOp::Add, cap(1), b.literal(1, p), p), p),
         b.make_if(b.binary(BinOp::Eq, cap(1), b.literal(1, p), p),
                   b.intrinsic(IntrinsicId::ArrayPush, {cap(0), local(0)},
                               p),
                   NodeId{}, p)},
        p);
    m.funcs.push_back(d);
    check_eq(run_module(m, "resurrect: heap not empty"), "",
             "resurrect: unexpected failure");
    // First collect: o and its closure condemned, o resurrected into keep
    // (its closure with it), nothing freed. Second: both go, n still 1.
    check_eq(joined(), "0|1|1|2|1|", "resurrect output");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "gc: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("gc OK\n");
  return 0;
}
