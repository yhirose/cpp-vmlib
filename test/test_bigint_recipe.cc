// Arbitrary-precision integers, as a front-end recipe rather than a Value
// tag -- the README's "Arbitrary-precision integers" section, checked
// against C++'s own unsigned __int128 arithmetic as the oracle, the way
// test_ints.cc checks the fixed-width recipe against intN_t.
//
// The representation: a little-endian Array of Int limbs in base 10^9,
// no leading zero limbs, zero as the empty array. Base 10^9 rather than
// 2^32 because a limb product plus two carries then stays under 2^63
// (10^18 + 2 * 10^9) with room to spare, and because printing is then
// nine decimal digits per limb with no division at all -- the conversion
// a language's `str(bigint)` cannot avoid paying somewhere. Addition,
// schoolbook multiplication and decimal rendering are the three funcs
// below, written once in IR the way a front end would emit them; the
// program reads limb pairs from the host (coreir_rt_in), prints a sum and
// a product per pair, and the oracle side compares. Nothing in vmlib.h
// knows a bigint exists.

#include <cstdio>
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>

#include "vmlib.h"

namespace {

struct Failure : std::runtime_error {
  Failure(std::string msg) : std::runtime_error(std::move(msg)) {}
};

std::vector<std::string> g_out;
std::deque<int64_t> g_in;
int g_failures = 0;

constexpr int64_t kBase = 1000000000;

std::string to_decimal(unsigned __int128 v) {
  if (v == 0) return "0";
  std::string s;
  while (v > 0) {
    s.insert(s.begin(), static_cast<char>('0' + static_cast<int>(v % 10)));
    v /= 10;
  }
  return s;
}

// The limbs the program will read for `v`: their count, then each, least
// significant first.
void feed(uint64_t v) {
  std::vector<int64_t> limbs;
  while (v > 0) {
    limbs.push_back(static_cast<int64_t>(v % static_cast<uint64_t>(kBase)));
    v /= static_cast<uint64_t>(kBase);
  }
  g_in.push_back(static_cast<int64_t>(limbs.size()));
  for (int64_t l : limbs) g_in.push_back(l);
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
int64_t coreir_rt_in(int64_t, int64_t) {
  if (g_in.empty()) throw Failure("input exhausted");
  const int64_t v = g_in.front();
  g_in.pop_front();
  return v;
}
void coreir_rt_poll(void) {}
[[noreturn]] void coreir_rt_fail(const char* msg, int64_t, int64_t) {
  throw Failure(msg);
}
}

int main() {
  using namespace coreir;
  using I = IntrinsicId;
  Module m;
  Builder b(m);
  const SrcPos p{1, 1};

  auto lit = [&](int64_t v) { return b.literal(v, p); };
  auto L = [&](int32_t i) { return b.varref(VarKind::Local, i, p); };
  auto set = [&](int32_t i, NodeId v) { return b.assign(VarKind::Local, i, v, p); };
  auto bin = [&](BinOp op, NodeId x, NodeId y) { return b.binary(op, x, y, p); };
  auto len = [&](NodeId a) { return b.intrinsic(I::Len, {a}, p); };
  auto at = [&](NodeId a, NodeId i) { return b.index(a, i, p); };
  auto push = [&](NodeId a, NodeId v) {
    return b.intrinsic(I::ArrayPush, {a, v}, p);
  };
  auto inc = [&](int32_t i) { return set(i, bin(BinOp::Add, L(i), lit(1))); };
  auto blk = [&](std::vector<NodeId> s) { return b.block(s, p); };
  auto when = [&](NodeId c, NodeId then_) {
    return b.make_if(c, then_, NodeId{}, p);
  };
  auto loop = [&](NodeId c, std::vector<NodeId> body) {
    return b.make_while(c, blk(body), p);
  };
  m.capture_maps.push_back({});

  // --- big_add(a, b): locals 0 a, 1 b, 2 r, 3 i, 4 carry, 5 n, 6 x -------
  // r = []; n = max(len a, len b); i = 0; carry = 0
  // while true { if i < n {} else { if carry == 0 { break } }
  //   x = carry; if i < len a { x = x + a[i] }; if i < len b { x = x + b[i] }
  //   push r, x % B; carry = x / B; i = i + 1 }
  // return r
  {
    const NodeId body = blk({
        set(2, b.array_lit({}, p)),
        set(5, len(L(0))),
        when(bin(BinOp::Gt, len(L(1)), L(5)), set(5, len(L(1)))),
        set(3, lit(0)), set(4, lit(0)),
        loop(b.bool_literal(true, p),
             {b.make_if(bin(BinOp::Lt, L(3), L(5)), blk({}),
                        when(bin(BinOp::Eq, L(4), lit(0)), b.make_break(p)), p),
              set(6, L(4)),
              when(bin(BinOp::Lt, L(3), len(L(0))),
                   set(6, bin(BinOp::Add, L(6), at(L(0), L(3))))),
              when(bin(BinOp::Lt, L(3), len(L(1))),
                   set(6, bin(BinOp::Add, L(6), at(L(1), L(3))))),
              push(L(2), bin(BinOp::Mod, L(6), lit(kBase))),
              set(4, bin(BinOp::Div, L(6), lit(kBase))), inc(3)}),
        b.make_return(L(2), p)});
    m.funcs.push_back({"big_add", 7, 0, b.scope(0, 7, body, p),
                       {"a", "b", "r", "i", "carry", "n", "x"}, {}});
    m.funcs.back().num_params = 2;
  }

  // --- big_mul(a, b): locals 0 a, 1 b, 2 r, 3 i, 4 j, 5 carry, 6 x -------
  // r = zeros(len a + len b)
  // for i in a: carry = 0
  //   for j in b: x = r[i+j] + a[i]*b[j] + carry; r[i+j] = x % B; carry = x/B
  //   j = i + len b; while carry > 0 { x = r[j] + carry; r[j] = x % B;
  //                                    carry = x / B; j = j + 1 }
  // while len r > 0 and r[len r - 1] == 0: pop r
  // return r
  {
    auto ij = [&] { return bin(BinOp::Add, L(3), L(4)); };
    auto last = [&] { return bin(BinOp::Sub, len(L(2)), lit(1)); };
    const NodeId body = blk({
        set(2, b.array_lit({}, p)), set(3, lit(0)),
        loop(bin(BinOp::Lt, L(3), bin(BinOp::Add, len(L(0)), len(L(1)))),
             {push(L(2), lit(0)), inc(3)}),
        set(3, lit(0)),
        loop(bin(BinOp::Lt, L(3), len(L(0))),
             {set(5, lit(0)), set(4, lit(0)),
              loop(bin(BinOp::Lt, L(4), len(L(1))),
                   {set(6, bin(BinOp::Add,
                               bin(BinOp::Add, at(L(2), ij()),
                                   bin(BinOp::Mul, at(L(0), L(3)),
                                       at(L(1), L(4)))),
                               L(5))),
                    b.set_index(L(2), ij(), bin(BinOp::Mod, L(6), lit(kBase)), p),
                    set(5, bin(BinOp::Div, L(6), lit(kBase))), inc(4)}),
              set(4, bin(BinOp::Add, L(3), len(L(1)))),
              loop(bin(BinOp::Gt, L(5), lit(0)),
                   {set(6, bin(BinOp::Add, at(L(2), L(4)), L(5))),
                    b.set_index(L(2), L(4), bin(BinOp::Mod, L(6), lit(kBase)), p),
                    set(5, bin(BinOp::Div, L(6), lit(kBase))), inc(4)}),
              inc(3)}),
        loop(b.bool_literal(true, p),
             {when(bin(BinOp::Eq, len(L(2)), lit(0)), b.make_break(p)),
              when(bin(BinOp::Ne, at(L(2), last()), lit(0)), b.make_break(p)),
              b.intrinsic(I::ArrayPop, {L(2)}, p)}),
        b.make_return(L(2), p)});
    m.funcs.push_back({"big_mul", 7, 0, b.scope(0, 7, body, p),
                       {"a", "b", "r", "i", "j", "carry", "x"}, {}});
    m.funcs.back().num_params = 2;
  }

  // --- big_str(a): locals 0 a, 1 s, 2 i, 3 t --------------------------------
  // if len a == 0 { return "0" }
  // i = len a - 1; s = tostr(a[i]); i = i - 1
  // while i >= 0 { t = tostr(a[i]); while len t < 9 { t = "0" + t }
  //                s = s + t; i = i - 1 }
  // return s
  {
    const NodeId body = blk({
        when(bin(BinOp::Eq, len(L(0)), lit(0)),
             b.make_return(b.str_literal("0", p), p)),
        set(2, bin(BinOp::Sub, len(L(0)), lit(1))),
        set(1, b.intrinsic(I::ToStr, {at(L(0), L(2))}, p)),
        set(2, bin(BinOp::Sub, L(2), lit(1))),
        loop(bin(BinOp::Ge, L(2), lit(0)),
             {set(3, b.intrinsic(I::ToStr, {at(L(0), L(2))}, p)),
              loop(bin(BinOp::Lt, len(L(3)), lit(9)),
                   {set(3, bin(BinOp::Add, b.str_literal("0", p), L(3)))}),
              set(1, bin(BinOp::Add, L(1), L(3))),
              set(2, bin(BinOp::Sub, L(2), lit(1)))}),
        b.make_return(L(1), p)});
    m.funcs.push_back({"big_str", 4, 0, b.scope(0, 4, body, p),
                       {"a", "s", "i", "t"}, {}});
    m.funcs.back().num_params = 1;
  }

  // --- main: locals 0 n, 1 c, 2 count, 3 k, 4 a, 5 b; cells 0 add, 1 mul,
  // 2 str (the Static calls recipe). -------------------------------------
  // n = readint; c = 0
  // while c < n { a = read_limbs(); b = read_limbs();
  //   print str(add(a, b)); print str(mul(a, b)); c = c + 1 }
  {
    auto cell = [&](int32_t i) { return b.varref(VarKind::Cell, i, p); };
    auto read = [&] { return b.intrinsic(I::ReadInt, {}, p); };
    auto read_limbs = [&](int32_t into) {
      return std::vector<NodeId>{
          set(2, read()), set(into, b.array_lit({}, p)), set(3, lit(0)),
          loop(bin(BinOp::Lt, L(3), L(2)), {push(L(into), read()), inc(3)})};
    };
    std::vector<NodeId> body{
        b.assign(VarKind::Cell, 0, b.make_closure(1, 0, p), p),
        b.assign(VarKind::Cell, 1, b.make_closure(2, 0, p), p),
        b.assign(VarKind::Cell, 2, b.make_closure(3, 0, p), p),
        set(0, read()), set(1, lit(0))};
    std::vector<NodeId> per_case = read_limbs(4);
    const auto rb = read_limbs(5);
    per_case.insert(per_case.end(), rb.begin(), rb.end());
    per_case.push_back(b.intrinsic(
        I::Print,
        {b.call_value(cell(2), {b.call_value(cell(0), {L(4), L(5)}, p)}, p)},
        p));
    per_case.push_back(b.intrinsic(
        I::Print,
        {b.call_value(cell(2), {b.call_value(cell(1), {L(4), L(5)}, p)}, p)},
        p));
    per_case.push_back(inc(1));
    body.push_back(loop(bin(BinOp::Lt, L(1), L(0)), per_case));
    m.funcs.insert(m.funcs.begin(),
                   Func{"main", 6, 0, b.scope(0, 6, blk(body), p),
                        {"n", "c", "count", "k", "a", "b"}, {}});
    m.funcs[0].num_cells = 3;
  }
  // main was inserted at 0 after the others were numbered from 0: the
  // closures above already name 1, 2, 3, which is now where they sit.

  if (auto err = verify(m)) {
    std::fprintf(stderr, "FAIL: malformed IR: %s\n", err->c_str());
    return 1;
  }

  // --- The cases: edges plus a deterministic random sweep. ---------------
  std::vector<std::pair<uint64_t, uint64_t>> cases = {
      {0, 0}, {0, 1}, {1, 0}, {1, 1}, {999999999, 1}, {999999999, 999999999},
      {1000000000, 1000000000}, {999999999999999999ULL, 1},
      {9223372036854775807ULL, 9223372036854775807ULL},
      {18446744073709551615ULL, 18446744073709551615ULL},
      {18446744073709551615ULL, 0}, {123456789012345678ULL, 987654321098765432ULL}};
  uint64_t seed = 0x9E3779B97F4A7C15ULL;
  auto next = [&seed] {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
  };
  for (int i = 0; i < 300; ++i) {
    // Mix magnitudes: full 64-bit, 32-bit, and small.
    const uint64_t x = next();
    const uint64_t y = next();
    const int shape = i % 4;
    cases.emplace_back(shape == 1 ? (x >> 32) : x,
                       shape == 2 ? (y >> 40) : shape == 3 ? (y >> 60) : y);
  }

  std::vector<std::string> want;
  g_in.push_back(static_cast<int64_t>(cases.size()));
  for (const auto& [x, y] : cases) {
    feed(x);
    feed(y);
    const unsigned __int128 a = x;
    const unsigned __int128 c = y;
    want.push_back(to_decimal(a + c));
    want.push_back(to_decimal(a * c));
  }

  std::string failure;
  int64_t left = 0;
  {
    Runtime rt;
    try {
      vm::run(vm::compile(m), rt);
    } catch (const Failure& e) {
      failure = e.what();
    }
    left = rt.live_objects();
  }
  if (!failure.empty()) {
    std::fprintf(stderr, "FAIL: run failed: %s\n", failure.c_str());
    ++g_failures;
  }
  if (left != 0) {
    std::fprintf(stderr, "FAIL: leaked %lld heap object(s)\n",
                 static_cast<long long>(left));
    ++g_failures;
  }
  if (g_out.size() != want.size()) {
    std::fprintf(stderr, "FAIL: %zu lines out, %zu expected\n", g_out.size(),
                 want.size());
    ++g_failures;
  }
  for (size_t i = 0; i < g_out.size() && i < want.size(); ++i) {
    if (g_out[i] != want[i]) {
      const auto& [x, y] = cases[i / 2];
      std::fprintf(stderr, "FAIL: %llu %s %llu: want %s, got %s\n",
                   static_cast<unsigned long long>(x), i % 2 ? "*" : "+",
                   static_cast<unsigned long long>(y), want[i].c_str(),
                   g_out[i].c_str());
      if (++g_failures > 10) break;
    }
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("bigint recipe: OK (%zu cases against __int128)\n", cases.size());
  return 0;
}
