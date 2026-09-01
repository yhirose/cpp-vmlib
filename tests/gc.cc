// The tracing backstop, driven directly: build the cycles reference counting
// cannot free, drop every outside handle, and watch collect() take exactly
// them -- and nothing that is still held, from the heap or from C++.

#include <cstdio>

#include "coreir/value.h"

using namespace coreir;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}

}  // namespace

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

  if (g_failures != 0) {
    std::fprintf(stderr, "gc: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("gc OK\n");
  return 0;
}
