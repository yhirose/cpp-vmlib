// Func::singleton: one closure object per function, for the whole run.
//
// The README's Static calls section describes hoisting a MakeClosure whose
// target never changes into a cell filled at module initialization. Every
// front end in examples/ that writes an IR-level helper library wrote that
// hoist by hand -- a synthetic variable, a capture of it threaded into every
// function, an array built at file scope, an index per helper. This flag is
// that recipe moved into the executor, so what the front end writes is a
// plain MakeClosure at each call site.
//
// Four questions:
//
//   1. Do two MakeClosures of a singleton yield the same object, and two of
//      an ordinary function still yield distinct ones? (The observable
//      difference, and the reason this is opt-in rather than an automatic
//      optimization for every capture-empty closure.)
//   2. Does it actually stop allocating? Asserted as the *difference*
//      between two otherwise identical modules, so the number does not have
//      to encode how many objects the rest of the program happens to hold.
//   3. Is a singleton generator function still one activation per call? The
//      closure is shared; the generator the call builds must not be.
//   4. Does verify() refuse the one combination that cannot mean anything --
//      a shared closure with per-site captures to forward?

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

  // --- 1. Identity: shared for a singleton, distinct otherwise. -----------
  //   main: a = closure(#1); b = closure(#1); print(Same(a, b))   -> true
  //         a = closure(#2); b = closure(#2); print(Same(a, b))   -> false
  //         print(a())                                            -> 7
  // #1 and #2 have identical bodies; the only difference is the flag, which
  // is what makes this a test of the flag rather than of anything else.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});  // 0: empty

    const auto pair = [&](int32_t fn) {
      return b.block(
          {b.assign(VarKind::Local, 0, b.make_closure(fn, 0, p), p),
           b.assign(VarKind::Local, 1, b.make_closure(fn, 0, p), p),
           b.intrinsic(IntrinsicId::Print,
                       {b.intrinsic(IntrinsicId::Same,
                                    {b.varref(VarKind::Local, 0, p),
                                     b.varref(VarKind::Local, 1, p)},
                                    p)},
                       p)},
          p);
    };
    const NodeId body = b.block(
        {pair(1), pair(2),
         b.intrinsic(IntrinsicId::Print,
                     {b.call_value(b.varref(VarKind::Local, 0, p), {}, p)}, p)},
        p);
    Func main_fn{"main", 2, 0, NodeId{}, {"a", "b"}, {}};
    main_fn.body = b.scope(0, 2, body, p);
    m.funcs.push_back(main_fn);

    Func shared{"shared", 0, 0, b.make_return(b.literal(7, p), p), {}, {}};
    shared.singleton = true;
    m.funcs.push_back(shared);

    Func plain{"plain", 0, 0, b.make_return(b.literal(7, p), p), {}, {}};
    m.funcs.push_back(plain);

    const RunResult r = run_module(m, "singleton identity");
    expect_clean(r, "singleton identity");
    check_eq(joined(), "true|false|7|", "singleton identity output");
  }

  // --- 2. It stops allocating. --------------------------------------------
  // Fifty closures of one function, all held in an array so refcounting
  // cannot quietly free them, then HeapStats. Run twice over the same module
  // shape with the flag off and on: the difference is the whole claim, and
  // stating it as a difference means this test does not have to know how
  // many other objects a run holds.
  const auto alloc_probe = [&](bool singleton) {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});

    // xs = []; i = 0; while i < 50 { push(xs, closure(#1)); i = i + 1 }
    // print(HeapStats().live_objects)
    const NodeId body = b.block(
        {b.assign(VarKind::Local, 0, b.array_lit({}, p), p),
         b.assign(VarKind::Local, 1, b.literal(0, p), p),
         b.make_while(
             b.binary(BinOp::Lt, b.varref(VarKind::Local, 1, p),
                      b.literal(50, p), p),
             b.block({b.intrinsic(IntrinsicId::ArrayPush,
                                  {b.varref(VarKind::Local, 0, p),
                                   b.make_closure(1, 0, p)},
                                  p),
                      b.assign(VarKind::Local, 1,
                               b.binary(BinOp::Add,
                                        b.varref(VarKind::Local, 1, p),
                                        b.literal(1, p), p),
                               p)},
                     p),
             p),
         b.intrinsic(
             IntrinsicId::Print,
             {b.index(b.intrinsic(IntrinsicId::HeapStats, {}, p),
                      b.str_literal("live_objects", p), p)},
             p)},
        p);
    Func main_fn{"main", 2, 0, NodeId{}, {"xs", "i"}, {}};
    main_fn.body = b.scope(0, 2, body, p);
    m.funcs.push_back(main_fn);

    Func target{"target", 0, 0, b.make_return(b.literal(1, p), p), {}, {}};
    target.singleton = singleton;
    m.funcs.push_back(target);

    const RunResult r = run_module(m, "singleton allocation");
    expect_clean(r, "singleton allocation");
    return g_out.empty() ? -1 : std::stoll(g_out.back());
  };
  {
    const int64_t off = alloc_probe(false);
    const int64_t on = alloc_probe(true);
    // Fifty distinct closures become one.
    check_eq(std::to_string(off - on), "49", "singleton allocation delta");
  }

  // --- 3. A singleton generator is still one activation per call. ---------
  // The closure is shared; what the call builds is not. Two generators from
  // the same shared closure, resumed independently, must not see each
  // other's progress.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});

    // g1 = closure(#1)(); g2 = closure(#1)()
    // print(resume(g1).value); print(resume(g1).value); print(resume(g2).value)
    const auto resume_value = [&](int32_t slot) {
      return b.index(b.intrinsic(IntrinsicId::GenResume,
                                 {b.varref(VarKind::Local, slot, p),
                                  b.nil_literal(p)},
                                 p),
                     b.str_literal("value", p), p);
    };
    const NodeId body = b.block(
        {b.assign(VarKind::Local, 0,
                  b.call_value(b.make_closure(1, 0, p), {}, p), p),
         b.assign(VarKind::Local, 1,
                  b.call_value(b.make_closure(1, 0, p), {}, p), p),
         b.intrinsic(IntrinsicId::Print, {resume_value(0)}, p),
         b.intrinsic(IntrinsicId::Print, {resume_value(0)}, p),
         b.intrinsic(IntrinsicId::Print, {resume_value(1)}, p)},
        p);
    Func main_fn{"main", 2, 0, NodeId{}, {"g1", "g2"}, {}};
    main_fn.body = b.scope(0, 2, body, p);
    m.funcs.push_back(main_fn);

    Func gen{"gen", 0, 0, NodeId{}, {}, {}};
    gen.is_generator = true;
    gen.singleton = true;
    gen.body = b.block({b.make_yield(b.literal(10, p), p),
                        b.make_yield(b.literal(20, p), p)},
                       p);
    m.funcs.push_back(gen);

    const RunResult r = run_module(m, "singleton generator");
    expect_clean(r, "singleton generator");
    // g1 advances; g2 starts from the beginning.
    check_eq(joined(), "10|20|10|", "singleton generator output");
  }

  // --- 4. verify() refuses a singleton that takes captures. ---------------
  // One closure shared by every site can only be right when there is
  // nothing per-site to forward into it.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({{VarKind::Cell, 0}});

    Func main_fn{"main", 0, 0, NodeId{}, {}, {}};
    main_fn.num_cells = 1;  // so the capture map below is in range
    main_fn.body = b.block({b.cell_fresh(0, p), b.make_closure(1, 0, p)}, p);
    m.funcs.push_back(main_fn);

    Func bad{"bad", 0, 1, b.varref(VarKind::Capture, 0, p), {}, {"x"}};
    bad.singleton = true;
    m.funcs.push_back(bad);

    const auto err = coreir::verify(m);
    if (!err) {
      std::fprintf(stderr, "FAIL: verify accepted a singleton with captures\n");
      ++g_failures;
    } else {
      check_eq(*err, "singleton func cannot take captures",
               "singleton capture diagnostic");
    }
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("ok\n");
  return 0;
}
