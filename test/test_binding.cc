// The two optional binder utilities: coreir::Resolver (closure conversion)
// and coreir::FuncWriter (a whole function written directly in IR).
//
// Neither is reachable from `vm`; a front end may ignore both. What they are
// is the bookkeeping every front end in examples/ that nests functions
// lexically was writing identically, and the terse builder each of them
// arrived at independently for writing its own runtime library. So the
// questions here are about the *rules* they encode, not about any one
// language:
//
//   1. Does a name resolve outward, and does reading it from two levels in
//      make every function on the way carry it? (The propagation, which a
//      per-function capture list would get wrong.)
//   2. Does a binding nobody captures stay a local slot, and one somebody
//      does become a cell -- and does access() answer with the right kind
//      from each side?
//   3. Does force_cell() reach a binding the free-variable walk cannot see?
//   4. Does mark/release let sibling blocks share slots?
//   5. Does FuncWriter's name table come out exactly as long as the slot
//      count, whatever order param()/local() were called in? That is the
//      invariant verify() checks and the one hand-numbered slots kept
//      breaking.
//
// Tests 1-4 are checked by running the module they describe, not by reading
// the tables back: a capture numbered right but emitted wrong would pass an
// inspection and fail here.

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

void run_module(const coreir::Module& m, const std::string& what) {
  g_out.clear();
  if (auto err = coreir::verify(m)) {
    std::fprintf(stderr, "FAIL: %s: malformed IR: %s\n", what.c_str(),
                 err->c_str());
    ++g_failures;
    return;
  }
  coreir::Runtime rt;
  try {
    vm::run(vm::compile(m), rt);
  } catch (const Failure& e) {
    std::fprintf(stderr, "FAIL: %s: %s\n", what.c_str(), e.what());
    ++g_failures;
  }
  if (rt.live_objects() != 0) {
    std::fprintf(stderr, "FAIL: %s: leaked %lld heap object(s)\n", what.c_str(),
                 static_cast<long long>(rt.live_objects()));
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
  const SrcPos pos{1, 1};

  // --- 1-2. Propagation through two levels, and cell vs local. ------------
  // The source this stands for:
  //
  //   let outer = 7;        // captured two levels in -> a cell in main
  //   let plain = 1;        // nobody captures it     -> a local slot
  //   function mid() { function inner() { return outer; } return inner(); }
  //   print(mid()); print(plain);
  //
  // `mid` never mentions `outer` itself, but has to carry it: `inner`'s
  // capture map is written in mid's frame, and mid cannot name a cell it
  // does not own. That is the whole of Resolver::use.
  {
    Module m;
    Resolver rs;

    const int32_t f_main = rs.new_fn(-1);
    const int32_t f_mid = rs.new_fn(f_main);
    const int32_t f_inner = rs.new_fn(f_mid);
    m.funcs.resize(3);
    rs.fns[0].index = 0;
    rs.fns[1].index = 1;
    rs.fns[2].index = 2;

    rs.push_scope();
    const int32_t v_outer = rs.declare("outer", f_main);
    const int32_t v_plain = rs.declare("plain", f_main);
    // inner reads `outer`; main reads `plain`.
    rs.resolve("outer", f_inner);
    rs.resolve("plain", f_main);
    rs.pop_scope();

    if (rs.fns[static_cast<size_t>(f_mid)].free.count(v_outer) == 0) {
      std::fprintf(stderr, "FAIL: mid does not carry the capture it must\n");
      ++g_failures;
    }
    if (!rs.fns[static_cast<size_t>(f_mid)].free.count(v_plain) == 0) {
      std::fprintf(stderr, "FAIL: mid carries a capture it should not\n");
      ++g_failures;
    }

    rs.number_captures(m);
    // `outer` is captured, so it is one of main's cells; `plain` is not, so
    // it keeps the local slot the front end assigns.
    if (rs.cell_of(f_main, v_outer) < 0) {
      std::fprintf(stderr, "FAIL: a captured binding did not become a cell\n");
      ++g_failures;
    }
    if (rs.cell_of(f_main, v_plain) >= 0) {
      std::fprintf(stderr, "FAIL: an uncaptured binding became a cell\n");
      ++g_failures;
    }
    rs.vars[static_cast<size_t>(v_plain)].slot = 0;

    // The capture maps, each written in the frame that builds the closure.
    Builder bld(m);
    auto b = bld.at(pos);
    const auto cmap = [&](int32_t builder_fn, int32_t callee) {
      std::vector<CaptureSrc> cs;
      for (const int32_t v : rs.fns[static_cast<size_t>(callee)].free) {
        const auto [k, i] = rs.access(builder_fn, v);
        cs.push_back({k, i});
      }
      m.capture_maps.push_back(cs);
      return static_cast<int32_t>(m.capture_maps.size() - 1);
    };
    const int32_t cm_inner = cmap(f_mid, f_inner);
    const int32_t cm_mid = cmap(f_main, f_mid);

    const auto read = [&](int32_t fn, int32_t v) {
      const auto [k, i] = rs.access(fn, v);
      return b.varref(k, i);
    };
    const auto write = [&](int32_t fn, int32_t v, NodeId value) {
      const auto [k, i] = rs.access(fn, v);
      return b.assign(k, i, value);
    };

    Func& mainf = m.funcs[0];
    mainf.name = "main";
    mainf.num_locals = 1;
    mainf.local_names = {"plain"};
    mainf.num_cells = rs.num_cells(f_main);
    mainf.body = b.scope(
        0, 1,
        b.block({b.cell_fresh(rs.cell_of(f_main, v_outer)),
                 write(f_main, v_outer, b.literal(7)),
                 write(f_main, v_plain, b.literal(1)),
                 b.intrinsic(IntrinsicId::Print,
                             {b.call_value(b.make_closure(1, cm_mid), {})}),
                 b.intrinsic(IntrinsicId::Print, {read(f_main, v_plain)})}));

    Func& mid = m.funcs[1];
    mid.name = "mid";
    mid.num_cells = rs.num_cells(f_mid);
    mid.body = b.make_return(b.call_value(b.make_closure(2, cm_inner), {}));

    Func& inner = m.funcs[2];
    inner.name = "inner";
    inner.body = b.make_return(read(f_inner, v_outer));

    run_module(m, "resolver propagation");
    check_eq(joined(), "7|1|", "resolver propagation output");
  }

  // --- 3. force_cell reaches what the walk cannot see. --------------------
  // A binding whose only reader is a closure the front end builds by hand,
  // outside the resolve walk -- a class table, in the front ends that have
  // classes. Nobody called resolve() for it, so `free` stays empty and the
  // walk would leave it a plain local.
  {
    Module m;
    Resolver rs;
    rs.new_fn(-1);
    m.funcs.resize(1);
    rs.fns[0].index = 0;

    rs.push_scope();
    const int32_t v = rs.declare("table", 0);
    rs.pop_scope();

    rs.number_captures(m);
    if (rs.cell_of(0, v) >= 0) {
      std::fprintf(stderr, "FAIL: an unread binding became a cell on its own\n");
      ++g_failures;
    }
    rs.forced_cells.clear();
    rs.force_cell(v);
    rs.fns[0].cell_index.clear();
    rs.number_captures(m);
    if (rs.cell_of(0, v) != 0) {
      std::fprintf(stderr, "FAIL: force_cell did not make a cell\n");
      ++g_failures;
    }
    const auto [k, i] = rs.access(0, v);
    if (k != VarKind::Cell || i != 0) {
      std::fprintf(stderr, "FAIL: access does not see the forced cell\n");
      ++g_failures;
    }
  }

  // --- 4. Sibling blocks share slots. -------------------------------------
  // Two blocks in sequence, each declaring one local: the frame is two wide
  // (the parameter-less body's own, plus one), not three, because the first
  // block gave its slot back. The value printed proves the second block got
  // a slot of its own rather than reading the first one's leftovers.
  {
    FrameLayout f;
    const int32_t a = f.alloc_local("keeps");
    const int32_t m1 = f.mark();
    const int32_t x = f.alloc_local("x");
    const int32_t e1 = f.release(m1);
    const int32_t m2 = f.mark();
    const int32_t y = f.alloc_local("y");
    const int32_t e2 = f.release(m2);
    if (a != 0 || x != 1 || y != 1 || m1 != 1 || m2 != 1 || e1 != 2 ||
        e2 != 2) {
      std::fprintf(stderr,
                   "FAIL: slot reuse: a=%d x=%d y=%d m1=%d m2=%d e1=%d e2=%d\n",
                   a, x, y, m1, m2, e1, e2);
      ++g_failures;
    }
    if (f.high_local != 2) {
      std::fprintf(stderr, "FAIL: frame width is %d, want 2\n", f.high_local);
      ++g_failures;
    }
    // The name table is as wide as the frame, whichever slots were reused.
    const std::vector<std::string> names = f.names();
    if (names.size() != 2 || names[0] != "keeps" || names[1] != "y") {
      std::fprintf(stderr, "FAIL: name table after slot reuse is wrong\n");
      ++g_failures;
    }
  }

  // --- 5. FuncWriter: named slots, and a body that runs. ------------------
  // `sum(n)`: the loop a helper library is full of. Written with named
  // slots, so the frame's shape is stated once and finish() derives the
  // counts -- the invariant that a hand-numbered body has to maintain in two
  // places at once.
  {
    Module m;
    m.funcs.resize(2);

    FuncWriter w(m);
    const auto [n] = w.params("n");
    const auto [acc, i] = w.locals("acc", "i");
    w.add(w.set(acc, w.I(0)));
    w.add(w.set(i, w.I(1)));
    w.add(w.wh(w.bin(BinOp::Le, w.L(i), w.L(n)),
               w.blk({w.set(acc, w.bin(BinOp::Add, w.L(acc), w.L(i))),
                      w.set(i, w.bin(BinOp::Add, w.L(i), w.I(1)))})));
    w.add(w.ret(w.L(acc)));
    w.write(m.funcs[1], "sum");

    const Func& f = m.funcs[1];
    if (f.num_params != 1 || f.num_locals != 3) {
      std::fprintf(stderr, "FAIL: FuncWriter counts: params=%d locals=%d\n",
                   f.num_params, f.num_locals);
      ++g_failures;
    }
    if (f.local_names != std::vector<std::string>{"n", "acc", "i"}) {
      std::fprintf(stderr, "FAIL: FuncWriter name table is wrong\n");
      ++g_failures;
    }
    if (!f.singleton) {
      std::fprintf(stderr, "FAIL: a capture-free helper is not a singleton\n");
      ++g_failures;
    }

    m.capture_maps.push_back({});
    Builder bld(m);
    auto b = bld.at(pos);
    Func& mainf = m.funcs[0];
    mainf.name = "main";
    mainf.body = b.intrinsic(
        IntrinsicId::Print,
        {b.call_value(b.make_closure(1, 0), {b.literal(10)})});

    run_module(m, "funcwriter loop");
    check_eq(joined(), "55|", "funcwriter loop output");
  }

  // --- 5b. A slot claimed after the body started still lands in the table.
  // The case hand-numbering gets wrong: a temporary added in the middle. Its
  // slot is the next one, and the name table grows with it -- nothing before
  // it is renumbered, and finish() cannot disagree about how many there are.
  {
    Module m;
    m.funcs.resize(1);
    FuncWriter w(m);
    const auto [x] = w.params("x");
    w.add(w.set(x, w.I(1)));
    const auto [tmp] = w.locals("tmp");
    w.add(w.set(tmp, w.L(x)));
    w.add(w.ret(w.L(tmp)));
    w.write(m.funcs[0], "late");
    const Func& f = m.funcs[0];
    if (f.num_params != 1 || f.num_locals != 2 ||
        f.local_names != std::vector<std::string>{"x", "tmp"}) {
      std::fprintf(stderr, "FAIL: a late local did not extend the table\n");
      ++g_failures;
    }
  }

  // --- 6. A closure built where its captures cannot be reached is refused.
  // The rule the propagation rests on: resolve() records a free name in
  // every function between the reader and the owner, so the frame a
  // function is *written* in can always name what that function captures.
  // A frame elsewhere -- a sibling, a wrapper the walk never nested the
  // function inside -- generally cannot, and the capture map it would get
  // is silently wrong. capture_map says so instead.
  //
  //   let x = 1;
  //   function owner() { function reader() { return x; } }  // reader: free x
  //   function other() { /* builds reader's closure -- with what? */ }
  {
    Resolver rs;
    Module m;
    m.funcs.resize(4);
    const int32_t main_fn = rs.new_fn(-1);
    rs.fns[static_cast<size_t>(main_fn)].index = 0;
    rs.push_scope();
    const int32_t x = rs.declare("x", main_fn);
    rs.vars[static_cast<size_t>(x)].slot = 0;

    const int32_t owner = rs.new_fn(main_fn);
    rs.fns[static_cast<size_t>(owner)].index = 1;
    const int32_t reader = rs.new_fn(owner);
    rs.fns[static_cast<size_t>(reader)].index = 2;
    rs.resolve("x", reader);
    const int32_t other = rs.new_fn(main_fn);
    rs.fns[static_cast<size_t>(other)].index = 3;
    rs.number_captures(m);

    // owner is where reader is written: it carries x, so this is fine.
    rs.capture_map(m, owner, reader);

    // other is not, and never asked for x.
    bool refused = false;
    try {
      rs.capture_map(m, other, reader);
    } catch (const Failure& e) {
      refused = std::string(e.what()).find("cannot supply 'x'") !=
                std::string::npos;
    }
    if (!refused) {
      std::fprintf(stderr,
                   "FAIL: a closure built out of reach was not refused\n");
      ++g_failures;
    }
  }

  // --- 7. The declaration order a block keeps beside its name map.
  // A name declared twice in one block is two bindings wherever the
  // language allows it (Lua's `local x` twice), so the map holds the
  // second and the order holds both -- which is what a front end releasing
  // a block's slots, or refreshing them per loop iteration, has to walk.
  // An alias is not a declaration and does not appear.
  {
    Resolver rs;
    const int32_t fn = rs.new_fn(-1);
    rs.push_scope();
    const int32_t x1 = rs.declare("x", fn);
    const int32_t y = rs.declare("y", fn);
    const int32_t x2 = rs.declare("x", fn);
    rs.alias("z", y);

    const std::vector<int32_t> want{x1, y, x2};
    if (rs.declared_order(rs.depth() - 1) != want) {
      std::fprintf(stderr, "FAIL: declaration order lost a shadowed binding\n");
      ++g_failures;
    }
    if (rs.declared_here("x") != x2 || rs.declared_here("z") != y) {
      std::fprintf(stderr, "FAIL: the name map answered the wrong binding\n");
      ++g_failures;
    }
    // A block that closes takes its order with it.
    rs.pop_scope();
    rs.push_scope();
    if (!rs.declared_order(rs.depth() - 1).empty()) {
      std::fprintf(stderr, "FAIL: a new scope inherited an order table\n");
      ++g_failures;
    }
  }

  // --- 7. mark/rollback: a prelude bound once, programs compiled on top.
  // The prelude's own analysis survives; everything the program added is
  // gone, including a free-set entry naming a variable that no longer is.
  {
    Resolver rs;
    const int32_t main_fn = rs.new_fn(-1);
    rs.fns[static_cast<size_t>(main_fn)].index = 0;
    rs.push_scope();
    const int32_t g = rs.declare("$global", main_fn);
    const int32_t helper = rs.new_fn(main_fn);
    rs.fns[static_cast<size_t>(helper)].index = 1;
    rs.resolve("$global", helper);
    const Resolver::Mark after_prelude = rs.mark();

    // a program: one more function, one more variable, and it reading both
    const int32_t prog_fn = rs.new_fn(main_fn);
    rs.fns[static_cast<size_t>(prog_fn)].index = 2;
    rs.declare("x", main_fn);
    rs.resolve("x", prog_fn);
    rs.number_captures();
    if (rs.fns[static_cast<size_t>(main_fn)].cell_index.size() != 2) {
      std::fprintf(stderr, "FAIL: the program's own cell was not counted\n");
      ++g_failures;
    }

    rs.rollback(after_prelude);
    if (rs.fns.size() != 2 || rs.vars.size() != 1) {
      std::fprintf(stderr, "FAIL: rollback kept the program's fns or vars\n");
      ++g_failures;
    }
    if (rs.fns[static_cast<size_t>(helper)].free.count(g) != 1) {
      std::fprintf(stderr, "FAIL: rollback dropped the prelude's own free\n");
      ++g_failures;
    }
    // the entry function is rebuilt per program, so it starts over
    rs.reset_fn(main_fn, -1);
    rs.number_captures();
    const auto& cells = rs.fns[static_cast<size_t>(main_fn)].cell_index;
    if (cells.size() != 1 || cells.count(g) != 1) {
      std::fprintf(stderr,
                   "FAIL: renumbering after rollback kept a stale cell\n");
      ++g_failures;
    }
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
