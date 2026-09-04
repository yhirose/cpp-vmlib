// Fixed-width int lowering: WrapI8..WrapU32 (UnOp) and UDiv/UMod/UShr/
// ULt/ULe/UGt/UGe (BinOp) -- the primitives a managed static-typed front
// end (a C#/Java/Go int/uint/long/ulong) lowers into, per README's
// Fixed-width integers section and the normalized-value convention it
// documents.
//
// The oracle is C++'s own intN_t/uintN_t arithmetic, not a second
// implementation of the same rules written here: every op is checked over a
// boundary corpus and a batch of pseudo-random operand pairs, each pair
// picked from the range a front end's own lowering promises to hand this
// op (a normalized i32 or u32, say) -- what the batch pins is that the VM's
// answer for that range matches the host's, not that the VM behaves for
// inputs no front end would ever construct.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "vmlib.h"

using namespace coreir;

namespace {

int g_failures = 0;
std::vector<std::string> g_out;

struct Failure : std::runtime_error {
  Failure(std::string msg) : std::runtime_error(std::move(msg)) {}
};

void check_eq(int64_t got, int64_t want, const std::string& what) {
  if (got != want) {
    std::fprintf(stderr, "FAIL: %s: want %lld got %lld\n", what.c_str(),
                 static_cast<long long>(want), static_cast<long long>(got));
    ++g_failures;
  }
}

// Runs a batch of Print(expr) statements and returns the printed lines, in
// order -- true/false stringify through Value::truthy's own to_display
// wording, everything else through coreir_rt_out's int64 path. A batch
// exists so one compile pays for many operand pairs rather than one program
// per pair.
//
// `trap`, when non-null, redirects the "did it fail" question to the
// caller: on a Failure the message lands in *trap and the run ends there
// (no leak check -- a trapped run's heap state is not the property under
// test), rather than this recording a failure of its own. Every other
// caller leaves it null and gets the usual "nothing should have trapped"
// behavior.
std::vector<std::string> run_batch(const std::vector<NodeId>& exprs,
                                   Module& m, Builder& b, SrcPos p,
                                   const std::string& what,
                                   std::string* trap = nullptr) {
  std::vector<NodeId> stmts;
  stmts.reserve(exprs.size());
  for (NodeId e : exprs) {
    stmts.push_back(b.intrinsic(IntrinsicId::Print, {e}, p));
  }
  Func f;
  f.name = "main";
  f.body = b.scope(0, 0, b.block(stmts, p), p);
  m.funcs.push_back(f);

  if (auto err = verify(m)) {
    std::fprintf(stderr, "FAIL: %s: malformed IR: %s\n", what.c_str(),
                 err->c_str());
    ++g_failures;
    return {};
  }
  g_out.clear();
  Runtime rt;
  const vm::Program prog = vm::compile(m);
  try {
    vm::run(prog, rt);
  } catch (const Failure& e) {
    if (trap != nullptr) {
      *trap = e.what();
      return {};
    }
    std::fprintf(stderr, "FAIL: %s: trapped: %s\n", what.c_str(), e.what());
    ++g_failures;
    return {};
  }
  const int64_t left = rt.live_objects();
  if (left != 0) {
    std::fprintf(stderr, "FAIL: %s: leaked %lld heap object(s)\n",
                 what.c_str(), static_cast<long long>(left));
    ++g_failures;
  }
  return g_out;
}

// Asserts running `exprs` traps with a message containing `want`.
void check_trap(const std::vector<NodeId>& exprs, Module& m, Builder& b,
                SrcPos p, const char* want, const std::string& what) {
  std::string trap;
  run_batch(exprs, m, b, p, what, &trap);
  if (trap.find(want) == std::string::npos) {
    std::fprintf(stderr, "FAIL: %s: want a trap mentioning [%s], got [%s]\n",
                 what.c_str(), want, trap.c_str());
    ++g_failures;
  }
}

int64_t parse_line(const std::string& s) {
  if (s == "true") return 1;
  if (s == "false") return 0;
  return std::stoll(s);
}

double parse_double(const std::string& s) { return std::stod(s); }

void check_double_eq(double got, double want, const std::string& what) {
  const bool ok = std::isnan(want) ? std::isnan(got) : got == want;
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s: want %.17g got %.17g\n", what.c_str(),
                 want, got);
    ++g_failures;
  }
}

std::mt19937_64 g_rng(0xC0FFEE);

int64_t rand_i32() {
  return static_cast<int64_t>(
      static_cast<int32_t>(static_cast<uint32_t>(g_rng())));
}
int64_t rand_u32() {
  return static_cast<int64_t>(static_cast<uint32_t>(g_rng()));
}
int64_t rand_u64_bits() {
  return static_cast<int64_t>(g_rng());  // arbitrary 64-bit bit pattern
}

constexpr int kRandomPairs = 300;

// ---------------------------------------------------------------------------
// Wrap unary ops: for an arbitrary int64 x, WrapXN must match sign- or
// zero-extending x's low N bits -- the same cast C++ itself would perform,
// which is exactly what a front end promises these mean. Templated on the
// oracle's own type so the six widths are one function, not six lambdas
// that differ only by a type name; the op's name comes from name_of(UnOp)
// (test_names.cc pins that it cannot drift from the enum) rather than being
// retyped as a string literal here.
// ---------------------------------------------------------------------------

template <typename T>
void test_wrap(UnOp op) {
  const char* name = name_of(op);
  std::vector<int64_t> xs = {
      0, 1, -1, 127, -128, 128, 255, 256, -256,
      32767, -32768, 32768, 65535, 65536, -65536,
      2147483647LL, -2147483648LL, 2147483648LL, 4294967295LL, 4294967296LL,
      -4294967296LL, std::numeric_limits<int64_t>::min(),
      std::numeric_limits<int64_t>::max(),
  };
  for (int i = 0; i < kRandomPairs; ++i) {
    xs.push_back(static_cast<int64_t>(g_rng()));
  }

  Module m;
  Builder b(m);
  const SrcPos p{1, 1};
  std::vector<NodeId> exprs;
  exprs.reserve(xs.size());
  for (int64_t x : xs) exprs.push_back(b.unary(op, b.literal(x, p), p));

  const auto lines = run_batch(exprs, m, b, p, name);
  if (lines.size() != xs.size()) return;  // run_batch already flagged it
  for (size_t i = 0; i < xs.size(); ++i) {
    check_eq(parse_line(lines[i]), static_cast<int64_t>(static_cast<T>(xs[i])),
             std::string(name) + "(" + std::to_string(xs[i]) + ")");
  }
}

// ---------------------------------------------------------------------------
// Binary ops over a range of normalized operand pairs -- the shape a front
// end's own lowering hands this VM, per the README recipe. `wrap_after`,
// when set, wraps the result the way a front end must (WrapI32 after Add,
// say) -- one optional parameter for "is there a wrap, and which", rather
// than a dummy UnOp plus a bool saying whether to believe it. `mask_rhs` is
// the same idea on the way in: a narrower width's own shift-count mask,
// which the front end applies itself because the VM's own & 63 is always
// wider (README's recipe) -- built as Binary(op, a, BitAnd(b, mask_rhs))
// rather than trusting the VM's own mask to happen to agree.
// ---------------------------------------------------------------------------

void test_binop(BinOp op, const char* name,
                const std::vector<std::pair<int64_t, int64_t>>& pairs,
                int64_t (*oracle)(int64_t, int64_t),
                std::optional<UnOp> wrap_after = std::nullopt,
                std::optional<int64_t> mask_rhs = std::nullopt) {
  Module m;
  Builder b(m);
  const SrcPos p{1, 1};
  std::vector<NodeId> exprs;
  exprs.reserve(pairs.size());
  for (auto [a, bb] : pairs) {
    NodeId rhs = b.literal(bb, p);
    if (mask_rhs) {
      rhs = b.binary(BinOp::BitAnd, rhs, b.literal(*mask_rhs, p), p);
    }
    NodeId e = b.binary(op, b.literal(a, p), rhs, p);
    if (wrap_after) e = b.unary(*wrap_after, e, p);
    exprs.push_back(e);
  }

  const auto lines = run_batch(exprs, m, b, p, name);
  if (lines.size() != pairs.size()) return;
  for (size_t i = 0; i < pairs.size(); ++i) {
    const auto [a, bb] = pairs[i];
    check_eq(parse_line(lines[i]), oracle(a, bb),
             std::string(name) + "(" + std::to_string(a) + ", " +
                 std::to_string(bb) + ")");
  }
}

// The three corpora below have the same shape -- every interesting value
// against every other, then a random tail -- and differ only in which
// values are interesting and which generator supplies the tail. One
// function, so a fourth width is a values list rather than another copy of
// this.
std::vector<std::pair<int64_t, int64_t>> pairs_of(
    const std::vector<int64_t>& vals, int64_t (*rnd)(), bool nonzero_b) {
  std::vector<std::pair<int64_t, int64_t>> out;
  for (int64_t a : vals)
    for (int64_t bb : vals) {
      if (nonzero_b && bb == 0) continue;
      out.push_back({a, bb});
    }
  for (int i = 0; i < kRandomPairs; ++i) {
    const int64_t a = rnd();
    int64_t bb = rnd();
    if (nonzero_b && bb == 0) bb = 1;
    out.push_back({a, bb});
  }
  return out;
}

std::vector<std::pair<int64_t, int64_t>> i32_pairs(bool nonzero_b) {
  return pairs_of(
      {0, 1, -1, 2147483647LL, -2147483648LL, -2, 1000000, -1000000},
      rand_i32, nonzero_b);
}

std::vector<std::pair<int64_t, int64_t>> u32_pairs(bool nonzero_b) {
  return pairs_of({0, 1, 4294967295LL, 2147483648LL, 1000000}, rand_u32,
                  nonzero_b);
}

std::vector<std::pair<int64_t, int64_t>> u64_pairs(bool nonzero_b) {
  return pairs_of({0, 1, 2, static_cast<int64_t>(0x8000000000000000ULL),
                   static_cast<int64_t>(0xFFFFFFFFFFFFFFFFULL)},
                  rand_u64_bits, nonzero_b);
}

// ---------------------------------------------------------------------------
// ToFloat32: rounds a double to what it would be after passing through an
// actual float, per README's `float` section. In range, C++'s own
// static_cast<float> is the independent oracle -- the same hardware
// rounding Java's and C#'s narrowing conversion use. Out of range and NaN
// are the documented boundary behavior (saturate to +-infinity, NaN passes
// through), checked by hand in test_to_float32_boundaries rather than
// folded into the fuzz corpus: a bare cast out of a float's range is
// undefined behavior in C++, so it cannot serve as an oracle there.
// ---------------------------------------------------------------------------

void test_to_float32() {
  std::vector<double> xs = {
      0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 0.1, 1.0 / 3.0, 100.0, -100.0,
      1e10, -1e10, 1e-10, -1e-10, 3.14159265358979,
      static_cast<double>(std::numeric_limits<float>::min()),
      static_cast<double>(std::numeric_limits<float>::max()),
      static_cast<double>(std::numeric_limits<float>::denorm_min()),
  };
  // Well inside float's +-3.4e38 range, so the oracle cast below stays
  // defined for every value in the corpus.
  std::uniform_real_distribution<double> dist(-1e30, 1e30);
  for (int i = 0; i < kRandomPairs; ++i) xs.push_back(dist(g_rng));

  Module m;
  Builder b(m);
  const SrcPos p{1, 1};
  std::vector<NodeId> exprs;
  exprs.reserve(xs.size());
  for (double x : xs) {
    exprs.push_back(
        b.intrinsic(IntrinsicId::ToFloat32, {b.double_literal(x, p)}, p));
  }
  const auto lines = run_batch(exprs, m, b, p, "ToFloat32");
  if (lines.size() != xs.size()) return;
  for (size_t i = 0; i < xs.size(); ++i) {
    const double want = static_cast<double>(static_cast<float>(xs[i]));
    check_double_eq(parse_double(lines[i]), want,
                    "ToFloat32(" + std::to_string(xs[i]) + ")");
  }
}

void test_to_float32_boundaries() {
  const double dinf = std::numeric_limits<double>::infinity();
  const double fmax = static_cast<double>(std::numeric_limits<float>::max());
  struct Case { double in, want; const char* label; };
  const std::vector<Case> cases = {
      {1e40, dinf, "1e40 -> +inf"},
      {-1e40, -dinf, "-1e40 -> -inf"},
      {fmax * 2, dinf, "beyond float max -> +inf"},
      // The boundary is not fmax itself: round-to-nearest still rounds a
      // double just past it back down to fmax, and only the true overflow
      // midpoint (2-2^-24)*2^127 and beyond goes to infinity.
      {std::nextafter(fmax, dinf), fmax, "just past float max -> float max"},
      {0x1.ffffffp127, dinf, "the overflow midpoint -> +inf"},
      {std::numeric_limits<double>::max(), dinf, "DBL_MAX -> +inf"},
      {-std::numeric_limits<double>::max(), -dinf, "-DBL_MAX -> -inf"},
      {dinf, dinf, "+inf -> +inf"},
      {-dinf, -dinf, "-inf -> -inf"},
      {std::numeric_limits<double>::quiet_NaN(),
       std::numeric_limits<double>::quiet_NaN(), "NaN -> NaN"},
  };
  for (const auto& c : cases) {
    Module m;
    Builder b(m);
    const SrcPos p{1, 1};
    const NodeId e =
        b.intrinsic(IntrinsicId::ToFloat32, {b.double_literal(c.in, p)}, p);
    const auto lines = run_batch({e}, m, b, p, c.label);
    if (lines.size() == 1) {
      check_double_eq(parse_double(lines[0]), c.want, c.label);
    }
  }
  // An int argument widens first, like ToDouble.
  {
    Module m;
    Builder b(m);
    const SrcPos p{1, 1};
    const NodeId e = b.intrinsic(IntrinsicId::ToFloat32, {b.literal(3, p)}, p);
    const auto lines = run_batch({e}, m, b, p, "ToFloat32(int 3)");
    if (lines.size() == 1) {
      check_double_eq(parse_double(lines[0]), 3.0, "ToFloat32(int 3)");
    }
  }
}

}  // namespace

extern "C" {
void coreir_rt_out(int64_t v) { g_out.push_back(std::to_string(v)); }
void coreir_rt_out_str(const char* bytes, int64_t len) {
  g_out.emplace_back(bytes, static_cast<size_t>(len));
}
void coreir_rt_out_raw(const char*, int64_t) {}
int64_t coreir_rt_in(int64_t, int64_t) { return 0; }
[[noreturn]] void coreir_rt_fail(const char* msg, int64_t, int64_t) {
  throw Failure(msg);
}
void coreir_rt_poll(void) {}
}

int main() {
  // --- Wrap*: sign/zero extension of the low N bits -----------------------
  test_wrap<int8_t>(UnOp::WrapI8);
  test_wrap<int16_t>(UnOp::WrapI16);
  test_wrap<int32_t>(UnOp::WrapI32);
  test_wrap<uint8_t>(UnOp::WrapU8);
  test_wrap<uint16_t>(UnOp::WrapU16);
  test_wrap<uint32_t>(UnOp::WrapU32);

  // --- i32 arithmetic: Add/Sub/Mul/Div, each followed by WrapI32 ----------
  test_binop(
      BinOp::Add, "i32 Add", i32_pairs(false),
      [](int64_t a, int64_t bb) {
        return static_cast<int64_t>(static_cast<int32_t>(a + bb));
      },
      UnOp::WrapI32);
  test_binop(
      BinOp::Sub, "i32 Sub", i32_pairs(false),
      [](int64_t a, int64_t bb) {
        return static_cast<int64_t>(static_cast<int32_t>(a - bb));
      },
      UnOp::WrapI32);
  test_binop(
      BinOp::Mul, "i32 Mul", i32_pairs(false),
      [](int64_t a, int64_t bb) {
        return static_cast<int64_t>(static_cast<int32_t>(a * bb));
      },
      UnOp::WrapI32);
  test_binop(
      BinOp::Div, "i32 Div", i32_pairs(true),
      [](int64_t a, int64_t bb) {
        return static_cast<int64_t>(static_cast<int32_t>(a / bb));
      },
      UnOp::WrapI32);
  // The one i32 overflow case Div can hit: INT32_MIN / -1. i64 division
  // does not trap on it (only INT64_MIN / -1 does), so it reaches WrapI32
  // holding 2^31, which wraps back to INT32_MIN -- Java's rule.
  {
    Module m;
    Builder b(m);
    const SrcPos p{1, 1};
    const NodeId e = b.unary(
        UnOp::WrapI32,
        b.binary(BinOp::Div, b.literal(-2147483648LL, p),
                 b.literal(-1, p), p),
        p);
    const auto lines = run_batch({e}, m, b, p, "i32 Div INT32_MIN/-1");
    if (lines.size() == 1) {
      check_eq(parse_line(lines[0]), -2147483648LL, "i32 Div INT32_MIN/-1");
    }
  }

  // --- u32 arithmetic: Add/Sub/Mul followed by WrapU32; ~ followed by
  //     WrapU32; Div/Mod/Shr/comparisons need no wrap (operands already
  //     non-negative, so signed and unsigned agree) -- oracles still go
  //     through uint32_t explicitly rather than leaning on that agreement,
  //     so the test does not quietly stop checking anything if that ever
  //     changes -------------------------------------------------------
  test_binop(
      BinOp::Add, "u32 Add", u32_pairs(false),
      [](int64_t a, int64_t bb) {
        return static_cast<int64_t>(static_cast<uint32_t>(a + bb));
      },
      UnOp::WrapU32);
  test_binop(
      BinOp::Sub, "u32 Sub", u32_pairs(false),
      [](int64_t a, int64_t bb) {
        return static_cast<int64_t>(static_cast<uint32_t>(a - bb));
      },
      UnOp::WrapU32);
  test_binop(
      BinOp::Mul, "u32 Mul", u32_pairs(false),
      [](int64_t a, int64_t bb) {
        return static_cast<int64_t>(static_cast<uint32_t>(a * bb));
      },
      UnOp::WrapU32);
  test_binop(BinOp::Div, "u32 Div", u32_pairs(true),
            [](int64_t a, int64_t bb) {
              return static_cast<int64_t>(static_cast<uint32_t>(a) /
                                          static_cast<uint32_t>(bb));
            });
  test_binop(BinOp::Mod, "u32 Mod", u32_pairs(true),
            [](int64_t a, int64_t bb) {
              return static_cast<int64_t>(static_cast<uint32_t>(a) %
                                          static_cast<uint32_t>(bb));
            });
  test_binop(BinOp::Lt, "u32 Lt", u32_pairs(false),
            [](int64_t a, int64_t bb) -> int64_t {
              return static_cast<uint32_t>(a) < static_cast<uint32_t>(bb)
                        ? 1 : 0;
            });
  test_binop(BinOp::Le, "u32 Le", u32_pairs(false),
            [](int64_t a, int64_t bb) -> int64_t {
              return static_cast<uint32_t>(a) <= static_cast<uint32_t>(bb)
                        ? 1 : 0;
            });
  test_binop(BinOp::Gt, "u32 Gt", u32_pairs(false),
            [](int64_t a, int64_t bb) -> int64_t {
              return static_cast<uint32_t>(a) > static_cast<uint32_t>(bb)
                        ? 1 : 0;
            });
  test_binop(BinOp::Ge, "u32 Ge", u32_pairs(false),
            [](int64_t a, int64_t bb) -> int64_t {
              return static_cast<uint32_t>(a) >= static_cast<uint32_t>(bb)
                        ? 1 : 0;
            });

  // u32 Shr: the VM masks its shift count to 63 bits, not 31, so a raw
  // Binary(Shr, a, count) is only correct once the front end has masked
  // count to 31 itself (README's recipe).
  test_binop(BinOp::Shr, "u32 Shr", u32_pairs(false),
            [](int64_t a, int64_t bb) -> int64_t {
              return static_cast<int64_t>(static_cast<uint32_t>(a) >>
                                          (bb & 31));
            },
            std::nullopt, 31);

  // u32 BitNot, wrapped: ~x on the full int64 then truncated to 32 bits.
  {
    std::vector<int64_t> xs = {0, 1, 4294967295LL, 2147483648LL};
    for (int i = 0; i < kRandomPairs; ++i) xs.push_back(rand_u32());
    Module m;
    Builder b(m);
    const SrcPos p{1, 1};
    std::vector<NodeId> exprs;
    for (int64_t x : xs) {
      exprs.push_back(b.unary(
          UnOp::WrapU32, b.unary(UnOp::BitNot, b.literal(x, p), p), p));
    }
    const auto lines = run_batch(exprs, m, b, p, "u32 BitNot+WrapU32");
    if (lines.size() == xs.size()) {
      for (size_t i = 0; i < xs.size(); ++i) {
        const uint64_t bits = ~static_cast<uint64_t>(xs[i]);
        const int64_t want =
            static_cast<int64_t>(static_cast<uint32_t>(bits));
        check_eq(parse_line(lines[i]), want,
                 "u32 BitNot+WrapU32(" + std::to_string(xs[i]) + ")");
      }
    }
  }

  // --- u64 (the unsigned-only BinOp group): UDiv/UMod/UShr/ULt/ULe/UGt/UGe
  // -------------------------------------------------------------------
  test_binop(BinOp::UDiv, "u64 UDiv", u64_pairs(true),
            [](int64_t a, int64_t bb) {
              return static_cast<int64_t>(static_cast<uint64_t>(a) /
                                          static_cast<uint64_t>(bb));
            });
  test_binop(BinOp::UMod, "u64 UMod", u64_pairs(true),
            [](int64_t a, int64_t bb) {
              return static_cast<int64_t>(static_cast<uint64_t>(a) %
                                          static_cast<uint64_t>(bb));
            });
  test_binop(BinOp::UShr, "u64 UShr", u64_pairs(false),
            [](int64_t a, int64_t bb) {
              return static_cast<int64_t>(static_cast<uint64_t>(a) >>
                                          (bb & 63));
            });
  test_binop(BinOp::ULt, "u64 ULt", u64_pairs(false),
            [](int64_t a, int64_t bb) -> int64_t {
              return static_cast<uint64_t>(a) < static_cast<uint64_t>(bb)
                        ? 1 : 0;
            });
  test_binop(BinOp::ULe, "u64 ULe", u64_pairs(false),
            [](int64_t a, int64_t bb) -> int64_t {
              return static_cast<uint64_t>(a) <= static_cast<uint64_t>(bb)
                        ? 1 : 0;
            });
  test_binop(BinOp::UGt, "u64 UGt", u64_pairs(false),
            [](int64_t a, int64_t bb) -> int64_t {
              return static_cast<uint64_t>(a) > static_cast<uint64_t>(bb)
                        ? 1 : 0;
            });
  test_binop(BinOp::UGe, "u64 UGe", u64_pairs(false),
            [](int64_t a, int64_t bb) -> int64_t {
              return static_cast<uint64_t>(a) >= static_cast<uint64_t>(bb)
                        ? 1 : 0;
            });

  // --- ToFloat32 ------------------------------------------------------
  test_to_float32();
  test_to_float32_boundaries();

  // --- Traps: UDiv/UMod by zero, Wrap of a non-int, ToFloat32 of a
  //     non-number ----------------------------------------------------
  {
    Module m;
    Builder b(m);
    const SrcPos p{1, 1};
    check_trap({b.binary(BinOp::UDiv, b.literal(5, p), b.literal(0, p), p)},
              m, b, p, "divide by zero", "UDiv by zero");
  }
  {
    Module m;
    Builder b(m);
    const SrcPos p{1, 1};
    check_trap({b.binary(BinOp::UMod, b.literal(5, p), b.literal(0, p), p)},
              m, b, p, "divide by zero", "UMod by zero");
  }
  {
    Module m;
    Builder b(m);
    const SrcPos p{1, 1};
    check_trap({b.unary(UnOp::WrapI32, b.double_literal(1.5, p), p)}, m, b,
              p, "cannot truncate", "WrapI32 of a double");
  }
  {
    Module m;
    Builder b(m);
    const SrcPos p{1, 1};
    check_trap(
        {b.intrinsic(IntrinsicId::ToFloat32, {b.str_literal("x", p)}, p)},
        m, b, p, "cannot convert", "ToFloat32 of a string");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "ints: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("ints OK\n");
  return 0;
}
