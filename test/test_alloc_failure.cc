// Allocation failure: every allocation that compiling and running a program
// makes is failed in turn by an operator new that throws std::bad_alloc on
// the Nth call -- that one only, and then that one and every one after it,
// the way memory that has run out stays out. Whatever N is, the run must
// end the way a host's own throw ends it -- the exception leaves vm::run
// with every frame released -- or as a trap (an ArrayFill's is one), or,
// where the failure was absorbed (a destructor's is reported and
// swallowed, a trap can be caught), finish; afterwards a collection
// must leave no object alive, and the Runtime's end must free every block.
// A crash, a std::terminate, an object left pinned or a leak at any N fails
// the case.
//
// The programs are small on purpose: the sweep runs each once per
// allocation it makes. Between them they reach every kind of heap object
// and every way into and out of an activation: values and containers,
// calls of every shape, throws through frames and defers, destructors run
// by a release, a scope exit, a collection and an unwind, generators,
// coroutines and the job queue -- and the intrinsics, control and
// collector paths that allocate on their own way: a filled array, a map
// that compacts, a break out of a try, a tail call with a spread, a
// generator ended or thrown into, a cycle a scope cannot free.

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <optional>
#include <string>

#include "vmlib.h"

namespace {

// -- the allocator ----------------------------------------------------------
// `count` counts the calls since the allocator was armed; the `fail_at`th
// throws instead of allocating, and with `after` every one after it does
// too (fail_at 0: never armed, LONG_MAX: count only). `live` is the number
// of blocks outstanding, so a leak shows as a difference across a run.
struct Alloc {
  long count = 0;
  long fail_at = 0;
  bool after = false;
  bool fired = false;
  long live = 0;
};
Alloc g_alloc;

void* allocate(std::size_t n) {
  if (g_alloc.fail_at != 0) {
    ++g_alloc.count;
    if (g_alloc.count == g_alloc.fail_at ||
        (g_alloc.after && g_alloc.count > g_alloc.fail_at)) {
      g_alloc.fired = true;
      throw std::bad_alloc();
    }
  }
  void* p = std::malloc(n == 0 ? 1 : n);
  if (p == nullptr) throw std::bad_alloc();
  ++g_alloc.live;
  return p;
}

void deallocate(void* p) noexcept {
  if (p == nullptr) return;
  --g_alloc.live;
  std::free(p);
}

}  // namespace

// GCC pairs operator new with the free() inside operator delete and calls
// it a mismatch; here they are one allocator, malloc underneath both.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(std::size_t n) { return allocate(n); }
void* operator new[](std::size_t n) { return allocate(n); }
void operator delete(void* p) noexcept { deallocate(p); }
void operator delete[](void* p) noexcept { deallocate(p); }
void operator delete(void* p, std::size_t) noexcept { deallocate(p); }
void operator delete[](void* p, std::size_t) noexcept { deallocate(p); }

namespace {

// -- the host ---------------------------------------------------------------
// Output goes to a fixed buffer, and a failure is a type with no string in
// it: the host allocates nothing, so every allocation counted is the
// library's.
char g_out[1024];
std::size_t g_out_len = 0;

void out(const char* s, std::size_t n) {
  const std::size_t room = sizeof g_out - g_out_len;
  const std::size_t k = n < room ? n : room;
  std::memcpy(g_out + g_out_len, s, k);
  g_out_len += k;
}

struct Failure {};

int g_failures = 0;
const char* g_case = "";

void check(bool ok, const char* what, long n) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s: %s (allocation %ld)\n", g_case, what, n);
    ++g_failures;
  }
}

}  // namespace

extern "C" {
void coreir_rt_out(int64_t v) {
  char buf[32];
  const int n = std::snprintf(buf, sizeof buf, "%lld|", static_cast<long long>(v));
  out(buf, static_cast<std::size_t>(n));
}
void coreir_rt_out_str(const char* bytes, int64_t len) {
  out(bytes, static_cast<std::size_t>(len));
  out("|", 1);
}
void coreir_rt_out_raw(const char* bytes, int64_t len) {
  out(bytes, static_cast<std::size_t>(len));
}
int64_t coreir_rt_in(int64_t, int64_t) { return 0; }
void coreir_rt_poll(void) {}
[[noreturn]] void coreir_rt_fail(const char*, int64_t, int64_t) {
  throw Failure{};
}
}

namespace {

using namespace coreir;
using I = IntrinsicId;

enum class End { Done, OutOfMemory, Trap };

// The run, and then -- with the allocator disarmed, since these allocate --
// a collection: whatever ended the run, nothing it built may still be
// alive after one. An object left pinned, or a reference left in a frame
// nobody released, shows here. `left` is what survived it.
End run_once(const Module& m, const vm::RunOptions& opts, int64_t& left) {
  std::optional<Runtime> rt;
  End end = End::Done;
  try {
    rt.emplace();
    const vm::Program p = vm::compile(m);
    vm::run(p, *rt, opts);
  } catch (const std::bad_alloc&) {
    end = End::OutOfMemory;
  } catch (const Failure&) {
    end = End::Trap;
  }
  g_alloc.fail_at = 0;
  left = 0;
  if (rt) {
    rt->collect();
    left = rt->live_objects();
  }
  return end;
}

long g_at = 0;  // the allocation being failed, for the terminate handler

// The run once whole, which says what it prints and how many allocations it
// makes; then once per allocation, failing that one, and once more failing
// it and every one after it.
void sweep(const char* name, const Module& m, const char* want,
           const vm::RunOptions& opts = {}) {
  g_case = name;
  g_at = 0;
  g_alloc.after = false;
  g_out_len = 0;
  g_alloc.count = 0;
  g_alloc.fail_at = LONG_MAX;
  int64_t left = 0;
  const End whole = run_once(m, opts, left);
  const long total = g_alloc.count;
  check(whole == End::Done, "the whole run does not finish", 0);
  check(left == 0, "objects outlive the whole run", 0);
  const std::string got(g_out, g_out_len);
  if (got != want) {
    std::fprintf(stderr, "FAIL: %s: output\n  want [%s]\n  got  [%s]\n",
                 name, want, got.c_str());
    ++g_failures;
  }

  long ends[2][3] = {};  // per mode: done, out of memory, trap
  for (const bool after : {false, true}) {
    g_alloc.after = after;
    for (long n = 1; n <= total; ++n) {
      g_at = n;
      const long live = g_alloc.live;
      g_out_len = 0;
      g_alloc.count = 0;
      g_alloc.fired = false;
      g_alloc.fail_at = n;
      const End e = run_once(m, opts, left);
      check(g_alloc.fired, "the allocation never came", n);
      check(left == 0, "objects outlive the run", n);
      check(g_alloc.live == live, "blocks leaked", n);
      ++ends[after][static_cast<int>(e)];
    }
  }
  g_alloc.after = false;
  std::printf(
      "%s: %ld allocations; failing one, %ld done, %ld out of memory, %ld "
      "trapped; failing the rest, %ld, %ld, %ld\n",
      name, total, ends[0][0], ends[0][1], ends[0][2], ends[1][0], ends[1][1],
      ends[1][2]);
}

// -- the programs -----------------------------------------------------------

NodeId print(FuncWriter& w, NodeId v) { return w.in(I::Print, {v}); }

// Strings, arrays, objects and maps, built, grown, sliced and read.
Module values() {
  Module m;
  m.capture_maps.push_back({});
  m.funcs.resize(1);
  FuncWriter w(m);
  w.lenient_arity = false;
  w.singleton = false;
  const auto [s, a, o, mp, i, k] = w.locals("s", "a", "o", "mp", "i", "k");
  w.add(w.set(s, w.S("a")));
  w.add(w.set(i, w.I(0)));
  w.add(w.wh(w.bin(BinOp::Lt, w.L(i), w.I(3)),
             w.blk({w.set(s, w.bin(BinOp::Add, w.L(s),
                                   w.in(I::ToStr, {w.L(i)}))),
                    w.set(i, w.bin(BinOp::Add, w.L(i), w.I(1)))})));
  w.add(w.set(a, w.arr({w.L(s), w.S("b"), w.arr({w.L(i), w.L(s)})})));
  w.add(w.push(w.L(a), w.bin(BinOp::Add, w.L(s), w.S("!"))));
  w.add(w.set(o, w.obj({{"name", w.L(s)}, {"list", w.L(a)}})));
  w.add(w.sidx(w.L(o), "extra", w.in(I::ArraySlice, {w.L(a), w.I(0), w.I(2)})));
  w.add(w.set(mp, w.in(I::MapNew, {})));
  w.add(w.sidx(w.L(mp), w.L(s), w.L(a)));
  w.add(w.set(k, w.in(I::ObjectKeys, {w.L(o)})));
  w.add(print(w, w.len(w.L(a))));
  w.add(print(w, w.idx(w.L(o), "name")));
  w.add(print(w, w.len(w.L(k))));
  w.add(print(w, w.in(I::StrSlice, {w.L(s), w.I(0), w.I(2)})));
  w.add(w.in(I::ArrayConcat, {w.L(a), w.L(k)}));
  w.add(print(w, w.len(w.L(a))));
  w.add(w.in(I::ObjectRemove, {w.L(o), w.S("extra")}));
  w.add(print(w, w.len(w.in(I::ObjectKeys, {w.L(o)}))));
  w.add(print(w, w.in(I::ArrayPop, {w.L(a)})));
  w.add(print(w, w.len(w.idx(w.L(mp), w.L(s)))));
  w.write(m.funcs[0], "main");
  return m;
}

bool twice(NativeCall& c) {
  c.result = Value::make_int(c.args[0].as_int() * 2);
  return true;
}

// Calls of every shape: a rest param, a spread, a lenient callee's extras,
// a closure over a cell, a native, and a tail-calling recursion.
Module calls() {
  Module m;
  m.capture_maps.push_back({});
  m.funcs.resize(5);
  {  // 1: add3(x, y, ...rest) = x + y + len(rest)
    FuncWriter w(m);
    w.lenient_arity = false;
    w.singleton = false;
    w.rest_param = true;
    const auto [x, y, rest] = w.params("x", "y", "rest");
    w.add(w.ret(w.bin(BinOp::Add, w.bin(BinOp::Add, w.L(x), w.L(y)),
                      w.len(w.L(rest)))));
    w.write(m.funcs[1], "add3");
  }
  {  // 2: counter() { cell n = 0; return () => n = n + 1 }
    FuncWriter w(m);
    w.singleton = false;
    w.add(w.setc(0, w.I(0)));
    w.add(w.ret(w.clos(3, {0})));
    w.write(m.funcs[2], "counter", 1);
  }
  {  // 3: the counter's step, lenient: extras are dropped
    FuncWriter w(m);
    w.singleton = false;
    w.add(w.b.assign(VarKind::Capture, 0,
                     w.bin(BinOp::Add, w.P(0), w.I(1))));
    w.add(w.ret(w.bin(BinOp::Add, w.P(0), w.I(0))));
    w.write(m.funcs[3], "step", 0, 1);
  }
  {  // 4: sum(n, acc) = n == 0 ? acc : sum(n - 1, acc + n), a tail call
    FuncWriter w(m);
    w.lenient_arity = false;
    const auto [n, acc] = w.params("n", "acc");
    w.add(w.iff(w.bin(BinOp::Eq, w.L(n), w.I(0)), w.ret(w.L(acc))));
    w.add(w.ret(w.b.call_value(
        w.b.make_closure(4, 0),
        {w.bin(BinOp::Sub, w.L(n), w.I(1)),
         w.bin(BinOp::Add, w.L(acc), w.L(n))})));
    w.write(m.funcs[4], "sum");
    m.funcs[4].tail_calls = true;
  }
  FuncWriter w(m);
  w.lenient_arity = false;
  w.singleton = false;
  const auto [f] = w.locals("f");
  const NodeId add3 = w.b.make_closure(1, 0);
  w.add(print(w, w.b.call_value(add3, {w.I(1), w.I(2), w.S("x"), w.S("y")})));
  w.add(print(w, w.b.call_spread(w.b.make_closure(1, 0), {w.I(1)},
                                 w.arr({w.I(2), w.S("z")}))));
  w.add(w.set(f, w.b.call_value(w.b.make_closure(2, 0), {})));
  w.add(w.b.call_value(w.L(f), {w.S("extra"), w.arr({})}));
  w.add(print(w, w.b.call_value(w.L(f), {})));
  w.add(print(w, w.b.call_value(w.b.make_closure(4, 0), {w.I(20), w.I(0)})));
  w.add(print(w, w.nat("twice", {w.I(21)})));
  w.write(m.funcs[0], "main");
  return m;
}

// A function that prints `s + "!"` and returns nil: the body of a defer,
// and allocating, so a defer can fail too.
void printer(Module& m, int32_t at, const char* s) {
  FuncWriter w(m);
  w.singleton = false;
  w.add(print(w, w.bin(BinOp::Add, w.S(s), w.S("!"))));
  w.write(m.funcs[static_cast<size_t>(at)], s);
}

// Throws through frames, caught: a thrown object, a trap, a defer on the
// way out of a scope, both ways.
Module unwinding() {
  Module m;
  m.capture_maps.push_back({});
  m.funcs.resize(5);
  printer(m, 1, "d1");
  printer(m, 2, "d2");
  {  // 3: inner() throws a fresh array through outer()
    FuncWriter w(m);
    w.singleton = false;
    w.add(w.b.make_throw(w.arr({w.S("p"), w.S("q")})));
    w.write(m.funcs[3], "inner");
  }
  {  // 4: outer() = inner()
    FuncWriter w(m);
    w.singleton = false;
    w.add(w.ret(w.b.call_value(w.b.make_closure(3, 0), {})));
    w.write(m.funcs[4], "outer");
  }
  FuncWriter w(m);
  w.lenient_arity = false;
  w.singleton = false;
  const auto [e] = w.locals("e");
  w.add(w.b.make_try(
      e.i, w.b.make_throw(w.obj({{"msg", w.bin(BinOp::Add, w.S("bo"), w.S("om"))}})),
      print(w, w.idx(w.L(e), "msg"))));
  w.add(w.b.make_try(e.i, print(w, w.bin(BinOp::Div, w.I(1), w.I(0))),
                     print(w, w.idx(w.L(e), "message"))));
  w.add(w.b.scope(1, 1,
                  w.blk({w.b.make_defer(w.b.make_closure(1, 0)),
                         print(w, w.S("body"))})));
  w.add(w.b.make_try(
      e.i,
      w.b.scope(1, 1,
                w.blk({w.b.make_defer(w.b.make_closure(2, 0)),
                       w.b.call_value(w.b.make_closure(4, 0), {})})),
      print(w, w.len(w.L(e)))));
  w.write(m.funcs[0], "main");
  return m;
}

// Destructors, run by each thing that can run one: a release, a scope's
// exit -- of an object, and of a cycle, which only a frame other than the
// entry's resolves there -- a collection of a cycle, and an unwind. The
// destructor allocates.
Module drops() {
  Module m;
  m.capture_maps.push_back({});
  m.funcs.resize(3);
  {  // 1: drop(self) = print(self.name + "!")
    FuncWriter w(m);
    w.singleton = false;
    const auto [self] = w.params("self");
    w.add(print(w, w.bin(BinOp::Add, w.idx(w.L(self), "name"), w.S("!"))));
    w.write(m.funcs[1], "drop");
  }
  {  // 2: pair(d) { scope { g = {g}; h = {h}; g.other = h; h.other = g } }
    FuncWriter w(m);
    w.singleton = false;
    w.lenient_arity = false;
    const auto [d] = w.params("d");
    const auto [g, h] = w.locals("g", "h");
    const auto obj = [&](const char* name) {
      return w.set_drop(w.obj({{"name", w.S(name)}}), w.L(d));
    };
    w.add(w.b.scope(g.i, h.i + 1,
                    w.blk({w.set(g, obj("g")), w.set(h, obj("h")),
                           w.sidx(w.L(g), "other", w.L(h)),
                           w.sidx(w.L(h), "other", w.L(g))})));
    w.write(m.funcs[2], "pair");
  }
  FuncWriter w(m);
  w.lenient_arity = false;
  w.singleton = false;
  const auto [d, o, q, e] = w.locals("d", "o", "q", "e");
  const auto obj = [&](const char* name) {
    return w.set_drop(w.obj({{"name", w.S(name)}}), w.L(d));
  };
  w.add(w.set(d, w.b.make_closure(1, 0)));
  w.add(w.set(o, obj("a")));
  w.add(w.set(o, w.Nil()));
  w.add(w.b.scope(2, 3, w.set(q, obj("b"))));
  w.add(w.set(o, obj("c")));
  w.add(w.set(q, obj("d")));
  w.add(w.sidx(w.L(o), "other", w.L(q)));
  w.add(w.sidx(w.L(q), "other", w.L(o)));
  w.add(w.set(o, w.Nil()));
  w.add(w.set(q, w.Nil()));
  w.add(print(w, w.in(I::Collect, {})));
  w.add(w.b.call_value(w.b.make_closure(2, 0), {w.L(d)}));
  w.add(w.b.make_try(
      e.i, w.b.scope(2, 3, w.blk({w.set(q, obj("f")), w.b.make_throw(w.S("x"))})),
      print(w, w.L(e))));
  w.write(m.funcs[0], "main");
  return m;
}

// A generator driven to its end, a coroutine resumed through a yield and
// one closed while suspended, and a job.
Module suspension() {
  Module m;
  m.capture_maps.push_back({});
  m.funcs.resize(4);
  {  // 1: gen() { yield "a" + "b"; yield [1]; return "end" }
    FuncWriter w(m);
    w.singleton = false;
    w.add(w.b.make_yield(w.bin(BinOp::Add, w.S("a"), w.S("b"))));
    w.add(w.b.make_yield(w.arr({w.I(1)})));
    w.add(w.ret(w.S("end")));
    w.write(m.funcs[1], "gen");
    m.funcs[1].is_generator = true;
  }
  {  // 2: co(x) { y = yield(x + "!"); return y }
    FuncWriter w(m);
    w.singleton = false;
    const auto [x] = w.params("x");
    const auto [y] = w.locals("y");
    w.add(w.set(y, w.in(I::CoroYield, {w.bin(BinOp::Add, w.L(x), w.S("!"))})));
    w.add(w.ret(w.L(y)));
    w.write(m.funcs[2], "co");
  }
  printer(m, 3, "job");
  FuncWriter w(m);
  w.lenient_arity = false;
  w.singleton = false;
  const auto [g, r, c] = w.locals("g", "r", "c");
  w.add(w.set(g, w.b.call_value(w.b.make_closure(1, 0), {})));
  w.add(w.set(r, w.in(I::GenResume, {w.L(g), w.Nil()})));
  w.add(print(w, w.idx(w.L(r), "value")));
  w.add(w.set(r, w.in(I::GenResume, {w.L(g), w.Nil()})));
  w.add(print(w, w.len(w.idx(w.L(r), "value"))));
  w.add(w.set(r, w.in(I::GenResume, {w.L(g), w.Nil()})));
  w.add(print(w, w.idx(w.L(r), "value")));
  w.add(w.set(c, w.in(I::CoroCreate, {w.b.make_closure(2, 0)})));
  w.add(w.set(r, w.in(I::CoroResume, {w.L(c), w.S("in")})));
  w.add(print(w, w.idx(w.L(r), "value")));
  w.add(w.set(r, w.in(I::CoroResume, {w.L(c), w.S("back")})));
  w.add(print(w, w.idx(w.L(r), "value")));
  w.add(w.set(c, w.in(I::CoroCreate, {w.b.make_closure(2, 0)})));
  w.add(w.in(I::CoroResume, {w.L(c), w.S("z")}));
  w.add(w.in(I::CoroClose, {w.L(c)}));
  w.add(w.in(I::Enqueue, {w.b.make_closure(3, 0)}));
  w.write(m.funcs[0], "main");
  return m;
}

// The intrinsics that build something -- a filled array (whose failure is a
// trap the program catches), a map emptied until it compacts and its keys,
// strings cased and from bytes, a heap census, a coroutine's status -- and
// struct fields and both kinds of switch table.
Module intrinsics() {
  Module m;
  m.capture_maps.push_back({});
  m.funcs.resize(2);
  {  // 1: idle() = nil, a coroutine body never resumed
    FuncWriter w(m);
    w.singleton = false;
    w.add(w.ret(w.Nil()));
    w.write(m.funcs[1], "idle");
  }
  FuncWriter w(m);
  w.lenient_arity = false;
  w.singleton = false;
  const auto [a, mp, i, e, o, s] = w.locals("a", "mp", "i", "e", "o", "s");
  const auto count_to = [&](NodeId limit, NodeId body) {
    return w.blk({w.set(i, w.I(0)),
                  w.wh(w.bin(BinOp::Lt, w.L(i), limit),
                       w.blk({body, w.set(i, w.bin(BinOp::Add, w.L(i),
                                                   w.I(1)))}))});
  };
  w.add(w.set(a, w.in(I::ArrayFill, {w.I(3), w.S("x")})));
  w.add(print(w, w.len(w.L(a))));
  w.add(w.b.make_try(e.i, w.set(a, w.in(I::ArrayFill, {w.I(4), w.S("y")})),
                     print(w, w.idx(w.L(e), "message"))));
  w.add(w.set(mp, w.in(I::MapNew, {})));
  w.add(count_to(w.I(10), w.sidx(w.L(mp), w.in(I::ToStr, {w.L(i)}), w.L(i))));
  w.add(count_to(w.I(6), w.in(I::ObjectRemove,
                             {w.L(mp), w.in(I::ToStr, {w.L(i)})})));
  w.add(print(w, w.len(w.in(I::ObjectKeys, {w.L(mp)}))));
  w.add(w.set(s, w.bin(BinOp::Add,
                       w.bin(BinOp::Add, w.in(I::StrUpper, {w.S("ab")}),
                             w.in(I::StrLower, {w.S("CD")})),
                       w.in(I::StrFromByte, {w.I(69)}))));
  w.add(print(w, w.L(s)));
  w.add(print(w, w.in(I::StrByte, {w.L(s), w.I(4)})));
  w.add(print(w, w.typ(w.L(a))));
  w.add(print(w, w.typ(w.in(I::HeapStats, {}))));
  w.add(print(w, w.in(I::CoroStatus,
                      {w.in(I::CoroCreate, {w.b.make_closure(1, 0)})})));
  w.add(w.set(o, w.obj({{"x", w.I(1)}, {"y", w.S("why")}})));
  w.add(w.b.field_set(w.L(o), 0, "x", w.S("ex")));
  w.add(print(w, w.bin(BinOp::Add, w.b.field_get(w.L(o), 0, "x"),
                       w.b.field_get(w.L(o), 1, "y"))));
  w.add(w.b.make_switch(w.I(1),
                        {{w.I(0), print(w, w.S("zero"))},
                         {w.I(1), print(w, w.bin(BinOp::Add, w.S("o"), w.S("ne")))},
                         {w.I(2), print(w, w.S("two"))}},
                        NodeId{}));
  w.add(w.b.make_switch(w.I(1000),
                        {{w.I(1), print(w, w.S("1"))},
                         {w.I(1000), print(w, w.bin(BinOp::Add, w.S("k"), w.S("!")))},
                         {w.I(100), print(w, w.S("100"))}},
                        NodeId{}));
  w.add(w.b.make_switch(w.S("b"),
                        {{w.S("a"), print(w, w.S("ay"))},
                         {w.S("b"), print(w, w.bin(BinOp::Add, w.S("b"), w.S("ee")))}},
                        print(w, w.S("none"))));
  w.write(m.funcs[0], "main");
  return m;
}

// Control the first programs do not reach: a break and a continue out of a
// try inside a loop, and out of a try inside a scope inside one -- the
// scope's release then runs outside the try's protection -- a tail call
// with a spread, and a scope that releases a cell.
Module control() {
  Module m;
  m.capture_maps.push_back({});
  m.funcs.resize(6);
  {  // 1: loopy() { i = 0; while true { try { if i == 3 break; i++; continue } catch {} } return i }
    FuncWriter w(m);
    w.singleton = false;
    w.lenient_arity = false;
    const auto [i, e] = w.locals("i", "e");
    w.add(w.set(i, w.I(0)));
    w.add(w.wh(w.Bo(true),
               w.b.make_try(e.i,
                            w.blk({w.iff(w.bin(BinOp::Eq, w.L(i), w.I(3)),
                                         w.b.make_break()),
                                   w.set(i, w.bin(BinOp::Add, w.L(i), w.I(1))),
                                   w.b.make_continue()}),
                            w.Nil())));
    w.add(w.ret(w.L(i)));
    w.write(m.funcs[1], "loopy");
  }
  {  // 2: spread(n, ...rest) = n == 0 ? len(rest) : spread(n - 1, ...[n, "x"])
    FuncWriter w(m);
    w.singleton = false;
    w.lenient_arity = false;
    w.rest_param = true;
    const auto [n, rest] = w.params("n", "rest");
    w.add(w.iff(w.bin(BinOp::Eq, w.L(n), w.I(0)), w.ret(w.len(w.L(rest)))));
    w.add(w.ret(w.b.call_spread(w.b.make_closure(2, 0),
                                {w.bin(BinOp::Sub, w.L(n), w.I(1))},
                                w.arr({w.L(n), w.S("x")}))));
    w.write(m.funcs[2], "spread");
    m.funcs[2].tail_calls = true;
  }
  {  // 3: cells() { scope [release cell 0] { cell 0 = "c"; print(get()) } }
    FuncWriter w(m);
    w.singleton = false;
    const NodeId body =
        w.blk({w.setc(0, w.bin(BinOp::Add, w.S("c"), w.S("ell"))),
               print(w, w.b.call_value(w.clos(4, {0}), {})),
               print(w, w.b.varref(VarKind::Cell, 0))});
    w.add(w.b.scope(0, 0, body, {w.b.varref(VarKind::Cell, 0)}));
    w.write(m.funcs[3], "cells", 1);
  }
  {  // 4: get() = the captured cell + "!"
    FuncWriter w(m);
    w.singleton = false;
    w.add(w.ret(w.bin(BinOp::Add, w.P(0), w.S("!"))));
    w.write(m.funcs[4], "get", 0, 1);
  }
  {  // 5: scoped() { while true { scope { z = "z" + "!"; try { break } catch {} } } return z-less "s" }
    FuncWriter w(m);
    w.singleton = false;
    w.lenient_arity = false;
    const auto [z, e] = w.locals("z", "e");
    w.add(w.wh(w.Bo(true),
               w.b.scope(z.i, z.i + 1,
                         w.blk({w.set(z, w.bin(BinOp::Add, w.S("z"), w.S("!"))),
                                w.b.make_try(e.i, w.b.make_break(), w.Nil())}))));
    w.add(w.ret(w.S("s")));
    w.write(m.funcs[5], "scoped");
  }
  FuncWriter w(m);
  w.lenient_arity = false;
  w.singleton = false;
  w.add(print(w, w.b.call_value(w.b.make_closure(1, 0), {})));
  w.add(print(w, w.b.call_value(w.b.make_closure(2, 0), {w.I(3)})));
  w.add(w.b.call_value(w.b.make_closure(3, 0), {}));
  w.add(print(w, w.b.call_value(w.b.make_closure(5, 0), {})));
  w.write(m.funcs[0], "main");
  return m;
}

// A generator with arguments, ended early by GenReturn while a defer is
// pending in it; one that catches what GenThrow raises at its yield; a
// coroutine closed while a defer is parked in it; and two coroutine bodies
// called by their first resume, which lends its argument rather than
// handing it over: one collects it in a rest param, the other is a
// generator function, so the call packages a generator.
Module generators() {
  Module m;
  m.capture_maps.push_back({});
  m.funcs.resize(7);
  printer(m, 1, "gd");
  printer(m, 2, "cd");
  {  // 3: gen2(a, b) { x = a + b; scope { defer gd; yield x; yield x + "2" } return "r" }
    FuncWriter w(m);
    w.singleton = false;
    w.lenient_arity = false;
    const auto [pa, pb] = w.params("a", "b");
    const auto [x] = w.locals("x");
    w.add(w.set(x, w.bin(BinOp::Add, w.L(pa), w.L(pb))));
    w.add(w.b.scope(x.i + 1, x.i + 1,
                    w.blk({w.b.make_defer(w.b.make_closure(1, 0)),
                           w.b.make_yield(w.L(x)),
                           w.b.make_yield(w.bin(BinOp::Add, w.L(x), w.S("2")))})));
    w.add(w.ret(w.S("r")));
    w.write(m.funcs[3], "gen2");
    m.funcs[3].is_generator = true;
  }
  {  // 4: gen3() { try { yield "t1" } catch e { yield e + "!" } return "end" }
    FuncWriter w(m);
    w.singleton = false;
    const auto [e] = w.locals("e");
    w.add(w.b.make_try(e.i, w.b.make_yield(w.S("t1")),
                       w.b.make_yield(w.bin(BinOp::Add, w.L(e), w.S("!")))));
    w.add(w.ret(w.S("end")));
    w.write(m.funcs[4], "gen3");
    m.funcs[4].is_generator = true;
  }
  {  // 5: co2() { scope { defer cd; yield "y" } }
    FuncWriter w(m);
    w.singleton = false;
    w.add(w.b.scope(0, 0,
                    w.blk({w.b.make_defer(w.b.make_closure(2, 0)),
                           w.in(I::CoroYield, {w.S("y")})})));
    w.write(m.funcs[5], "co2");
  }
  {  // 6: co3(...xs) = len(xs)
    FuncWriter w(m);
    w.singleton = false;
    w.lenient_arity = false;
    w.rest_param = true;
    const auto [xs] = w.params("xs");
    w.add(w.ret(w.len(w.L(xs))));
    w.write(m.funcs[6], "co3");
  }
  FuncWriter w(m);
  w.lenient_arity = false;
  w.singleton = false;
  const auto [g, c] = w.locals("g", "c");
  const auto value = [&](NodeId r) { return w.idx(r, "value"); };
  w.add(w.set(g, w.b.call_value(w.b.make_closure(3, 0), {w.S("p"), w.S("q")})));
  w.add(print(w, value(w.in(I::GenResume, {w.L(g), w.Nil()}))));
  w.add(print(w, value(w.in(I::GenReturn, {w.L(g), w.S("early")}))));
  w.add(w.set(g, w.b.call_value(w.b.make_closure(4, 0), {})));
  w.add(print(w, value(w.in(I::GenResume, {w.L(g), w.Nil()}))));
  w.add(print(w, value(w.in(I::GenThrow, {w.L(g), w.S("boom")}))));
  w.add(print(w, value(w.in(I::GenResume, {w.L(g), w.Nil()}))));
  w.add(w.set(c, w.in(I::CoroCreate, {w.b.make_closure(5, 0)})));
  w.add(w.in(I::CoroResume, {w.L(c), w.Nil()}));
  w.add(w.in(I::CoroClose, {w.L(c)}));
  w.add(print(w, w.in(I::CoroStatus, {w.L(c)})));
  w.add(print(w, value(w.in(I::CoroResume,
                            {w.in(I::CoroCreate, {w.b.make_closure(6, 0)}),
                             w.S("v")}))));
  w.add(print(w, w.typ(value(w.in(
                     I::CoroResume,
                     {w.in(I::CoroCreate, {w.b.make_closure(4, 0)}), w.Nil()})))));
  w.write(m.funcs[0], "main");
  return m;
}

// The collector and the owned stack over what stays reachable: a
// collection marking through a nested structure, and a scope's cycle that
// something outside still holds, which goes back on the stack for the
// frame's own exit to resolve.
Module reachable() {
  Module m;
  m.capture_maps.push_back({});
  m.funcs.resize(3);
  {  // 1: drop(self) = print(self.name + "!")
    FuncWriter w(m);
    w.singleton = false;
    const auto [self] = w.params("self");
    w.add(print(w, w.bin(BinOp::Add, w.idx(w.L(self), "name"), w.S("!"))));
    w.write(m.funcs[1], "drop");
  }
  {  // 2: held(d) { k = {}; scope { g = {g}; h = {h}; g.o = h; h.o = g; k.g = g } print("out") }
    FuncWriter w(m);
    w.singleton = false;
    w.lenient_arity = false;
    const auto [d] = w.params("d");
    const auto [k, g, h] = w.locals("k", "g", "h");
    const auto obj = [&](const char* name) {
      return w.set_drop(w.obj({{"name", w.S(name)}}), w.L(d));
    };
    w.add(w.set(k, w.obj({})));
    w.add(w.b.scope(g.i, h.i + 1,
                    w.blk({w.set(g, obj("g")), w.set(h, obj("h")),
                           w.sidx(w.L(g), "o", w.L(h)),
                           w.sidx(w.L(h), "o", w.L(g)),
                           w.sidx(w.L(k), "g", w.L(g))})));
    w.add(print(w, w.S("out")));
    w.write(m.funcs[2], "held");
  }
  FuncWriter w(m);
  w.lenient_arity = false;
  w.singleton = false;
  const auto [keep, x] = w.locals("keep", "x");
  w.add(w.set(keep, w.obj({{"a", w.obj({{"b", w.arr({w.S("c"), w.arr({})})}})}})));
  w.add(w.set(x, w.obj({})));
  w.add(w.sidx(w.L(x), "self", w.L(x)));
  w.add(w.set(x, w.Nil()));
  w.add(print(w, w.in(I::Collect, {})));
  w.add(print(w, w.len(w.idx(w.idx(w.idx(w.L(keep), "a"), "b"), w.I(1)))));
  w.add(w.b.call_value(w.b.make_closure(2, 0),
                       {w.b.make_closure(1, 0)}));
  w.write(m.funcs[0], "main");
  return m;
}

}  // namespace

int main() {
  std::set_terminate([] {
    std::fprintf(stderr, "terminate: %s, allocation %ld\n", g_case, g_at);
    std::abort();
  });

  vm::RunOptions with_twice;
  with_twice.natives = {{"twice", 1, &twice, nullptr}};

  sweep("values", values(), "4|a012|3|a0|7|2|extra|6|");
  sweep("calls", calls(), "5|4|2|210|42|", with_twice);
  sweep("unwinding", unwinding(),
        "boom|divide by zero|body|d1!|d2!|2|");
  sweep("drops", drops(), "a!|b!|d!|c!|4|h!|g!|f!|x|");
  sweep("suspension", suspension(), "ab|1|end|in!|back|job!|");
  sweep("intrinsics", intrinsics(),
        "3|4|ABcdE|69|array|object|start|exwhy|one|k!|bee|");
  sweep("control", control(), "3|2|cell!|cell|s|");
  sweep("generators", generators(),
        "pq|gd!|early|t1|boom!|end|cd!|done|1|generator|");
  sweep("reachable", reachable(), "1|0|out|h!|g!|");

  if (g_failures != 0) {
    std::fprintf(stderr, "alloc_failure: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("alloc_failure OK\n");
  return 0;
}
