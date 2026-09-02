// Func::lenient_arity and IntrinsicId::ArgCount: a call that supplies the
// wrong number of arguments traps under the default convention and is
// accepted under the lenient one -- extras dropped, missing params nil --
// with ArgCount reporting what the caller actually passed. The generator
// path packages its arguments separately, so it is pinned separately.
// FnArity is the other side of the same count: what a function value
// declares, read without calling it.

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

// #1: fn (a, b) { print(argcount()); print(typeof(a)); print(typeof(b)) }
// main calls it with the given arguments and prints its own argcount too.
coreir::Module two_param_module(const std::vector<int64_t>& args,
                                bool lenient) {
  using namespace coreir;
  Module m;
  Builder b(m);
  const SrcPos p{1, 1};
  m.capture_maps.push_back({});

  std::vector<NodeId> arg_nodes;
  for (const int64_t v : args) arg_nodes.push_back(b.literal(v, p));
  m.funcs.push_back(
      {"main", 0, 0,
       b.block({b.intrinsic(IntrinsicId::Print,
                            {b.intrinsic(IntrinsicId::ArgCount, {}, p)}, p),
                b.call_value(b.make_closure(1, 0, p), arg_nodes, p)},
               p),
       {},
       {}});

  Func f{"f", 2, 0, NodeId{}, {"a", "b"}, {}};
  f.num_params = 2;
  f.lenient_arity = lenient;
  f.body = b.block(
      {b.intrinsic(IntrinsicId::Print,
                   {b.intrinsic(IntrinsicId::ArgCount, {}, p)}, p),
       b.intrinsic(IntrinsicId::Print,
                   {b.intrinsic(IntrinsicId::TypeOf,
                                {b.varref(VarKind::Local, 0, p)}, p)},
                   p),
       b.intrinsic(IntrinsicId::Print,
                   {b.intrinsic(IntrinsicId::TypeOf,
                                {b.varref(VarKind::Local, 1, p)}, p)},
                   p)},
      p);
  m.funcs.push_back(f);
  return m;
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

  // --- 1. The default convention still traps on a mismatch. ---------------
  {
    const RunResult r = run_module(two_param_module({7}, false), "strict");
    if (r.failure.find("takes 2 argument(s), given 1") == std::string::npos) {
      std::fprintf(stderr, "FAIL: strict: expected an arity trap, got [%s]\n",
                   r.failure.c_str());
      ++g_failures;
    }
    if (r.leaked != 0) {
      std::fprintf(stderr, "FAIL: strict: leaked %lld\n",
                   static_cast<long long>(r.leaked));
      ++g_failures;
    }
  }

  // --- 2. Lenient, too few: the missing param is nil, not Uninit. --------
  {
    const RunResult r = run_module(two_param_module({7}, true), "too few");
    expect_clean(r, "too few");
    check_eq(joined(), "0|1|int|nil|", "too few output");
  }

  // --- 3. Lenient, too many: extras dropped, count still reported. -------
  {
    const RunResult r =
        run_module(two_param_module({1, 2, 3, 4}, true), "too many");
    expect_clean(r, "too many");
    check_eq(joined(), "0|4|int|int|", "too many output");
  }

  // --- 4. Exact arity under lenient is the ordinary case. -----------------
  {
    const RunResult r = run_module(two_param_module({1, 2}, true), "exact");
    expect_clean(r, "exact");
    check_eq(joined(), "0|2|int|int|", "exact output");
  }

  // --- 5. A lenient generator: the count survives park and unpark. --------
  // gen (a, b) { yield argcount(); yield typeof(b) }, called with one
  // argument, resumed twice.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    auto print_field = [&](int32_t local, const char* key) {
      return b.intrinsic(
          IntrinsicId::Print,
          {b.index(b.varref(VarKind::Local, local, p), b.str_literal(key, p),
                   p)},
          p);
    };
    std::vector<NodeId> stmts;
    stmts.push_back(b.assign(
        VarKind::Local, 0,
        b.call_value(b.make_closure(1, 0, p), {b.literal(5, p)}, p), p));
    for (int i = 0; i < 2; ++i) {
      stmts.push_back(b.assign(
          VarKind::Local, 1,
          b.intrinsic(IntrinsicId::GenResume,
                      {b.varref(VarKind::Local, 0, p), b.nil_literal(p)}, p),
          p));
      stmts.push_back(print_field(1, "value"));
    }
    m.funcs.push_back({"main", 2, 0, b.block(stmts, p), {"g", "r"}, {}});

    Func g{"g", 2, 0, NodeId{}, {"a", "b"}, {}};
    g.num_params = 2;
    g.is_generator = true;
    g.lenient_arity = true;
    g.body = b.block(
        {b.make_yield(b.intrinsic(IntrinsicId::ArgCount, {}, p), p),
         b.make_yield(b.intrinsic(IntrinsicId::TypeOf,
                                  {b.varref(VarKind::Local, 1, p)}, p),
                      p),
         b.make_return(b.literal(0, p), p)},
        p);
    m.funcs.push_back(g);

    const RunResult r = run_module(m, "generator");
    expect_clean(r, "generator");
    check_eq(joined(), "1|nil|", "generator output");
  }

  // --- 6. FnArity: num_params, and nothing else. --------------------------
  // A capture is not a parameter, a generator's params count the same way,
  // the entry point has none, and a non-function traps -- catchably.
  // print(fnarity(f)); print(fnarity(main)); print(fnarity(g));
  // try { fnarity(7) } catch e { print(e.message) }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.capture_maps.push_back({{VarKind::Cell, 0}});
    auto arity = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::FnArity, {v}, p);
    };
    auto print = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::Print, {v}, p);
    };
    Func main{"main", 1, 0, NodeId{}, {"e"}, {}};
    main.num_cells = 1;
    main.body = b.block(
        {b.cell_fresh(0, p),
         print(arity(b.make_closure(1, 1, p))),
         print(arity(b.make_closure(0, 0, p))),
         print(arity(b.make_closure(2, 0, p))),
         b.make_try(0, arity(b.literal(7, p)),
                    print(b.index(b.varref(VarKind::Local, 0, p),
                                  b.str_literal("message", p), p)),
                    p)},
        p);
    m.funcs.push_back(main);

    Func f{"f", 2, 1, NodeId{}, {"a", "b"}, {"c"}};
    f.num_params = 2;
    f.body = b.block({}, p);
    m.funcs.push_back(f);

    Func g{"g", 1, 0, NodeId{}, {"a"}, {}};
    g.num_params = 1;
    g.is_generator = true;
    g.body = b.make_yield(b.literal(0, p), p);
    m.funcs.push_back(g);

    const RunResult r = run_module(m, "fnarity");
    expect_clean(r, "fnarity");
    check_eq(joined(), "2|0|1|cannot take the arity of int|",
             "fnarity output");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("arity: all cases passed");
  return 0;
}
