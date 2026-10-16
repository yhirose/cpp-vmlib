// What verify() refuses, for the malformed shapes a front end's bug (or a
// hand-edited Module) can produce but the Builder never would: each is a
// way the compiler or the executor would otherwise read out of bounds. And
// the one shape that looks suspicious and is fine -- a node shared between
// two parents -- stays accepted.

#include <cstdio>
#include <functional>
#include <string>

#include "vmlib.h"

using namespace coreir;

namespace {

int g_failures = 0;

void check_eq(const std::string& got, const std::string& want,
              const char* what) {
  if (got != want) {
    std::fprintf(stderr, "FAIL: %s\n  want [%s]\n  got  [%s]\n", what,
                 want.c_str(), got.c_str());
    ++g_failures;
  }
}

// A one-function module whose body is `print(<expr>)` (or <expr> itself,
// `bare`), with `build` making <expr> and then free to corrupt the module
// before it is verified.
std::string verdict(const std::function<NodeId(Module&, Builder&)>& build,
                    bool bare = false) {
  Module m;
  Builder b(m);
  const SrcPos p{1, 1};
  m.capture_maps.push_back({});
  const NodeId expr = build(m, b);
  m.funcs.push_back(
      {"main", 0, 0,
       bare ? expr : b.intrinsic(IntrinsicId::Print, {expr}, p), {}, {}});
  const auto err = verify(m);
  return err ? *err : "";
}

}  // namespace

extern "C" {
void coreir_rt_out(int64_t) {}
void coreir_rt_out_str(const char*, int64_t) {}
void coreir_rt_out_raw(const char*, int64_t) {}
int64_t coreir_rt_in(int64_t, int64_t) { return 0; }
void coreir_rt_poll(void) {}
[[noreturn]] void coreir_rt_fail(const char*, int64_t, int64_t) {
  std::abort();
}
}

int main() {
  const SrcPos p{1, 1};

  // An operator past the last enumerator would be encoded as some other
  // opcode entirely (BinOp 23 became LoadLocal).
  check_eq(verdict([&](Module& m, Builder& b) {
             const NodeId n =
                 b.binary(BinOp::Add, b.literal(1, p), b.literal(2, p), p);
             m.nodes[n.v].op = static_cast<uint8_t>(kLastBinOp) + 1;
             return n;
           }),
           "unknown binary operator", "binary op out of range");
  check_eq(verdict([&](Module& m, Builder& b) {
             const NodeId n = b.unary(UnOp::Neg, b.literal(1, p), p);
             m.nodes[n.v].op = static_cast<uint8_t>(kLastUnOp) + 1;
             return n;
           }),
           "unknown unary operator", "unary op out of range");
  check_eq(verdict([&](Module& m, Builder& b) {
             const NodeId n = b.intrinsic(IntrinsicId::ReadInt, {}, p);
             m.nodes[n.v].op = static_cast<uint8_t>(kLastIntrinsic) + 1;
             return n;
           }),
           "unknown intrinsic", "intrinsic out of range");
  check_eq(verdict(
               [&](Module& m, Builder& b) {
                 const NodeId n = b.block({}, p);
                 m.nodes[n.v].tag =
                     static_cast<Tag>(static_cast<uint8_t>(kLastTag) + 1);
                 return n;
               },
               /*bare=*/true),
           "unknown node tag", "tag out of range");

  // A string constant whose bytes are not in the string pool.
  check_eq(verdict([&](Module& m, Builder& b) {
             const NodeId s = b.str_literal("hello", p);
             m.consts[static_cast<size_t>(m.nodes[s.v].a)].bits = 1000;
             return s;
           }),
           "literal const index out of range", "string const past the pool");

  // A switch key whose own const index is bad: verify() read it before
  // checking it.
  check_eq(verdict([&](Module& m, Builder& b) {
             const NodeId key = b.literal(1, p);
             const NodeId sw = b.make_switch(b.literal(1, p),
                                             {{key, b.literal(5, p)}},
                                             NodeId{}, p);
             m.nodes[key.v].a = 100000;
             return sw;
           }),
           "literal const index out of range", "switch key const");

  // first_child + num_children wrapping around 32 bits.
  check_eq(verdict([&](Module& m, Builder& b) {
             const NodeId blk = b.block({b.literal(1, p)}, p);
             m.nodes[blk.v].first_child = 0xFFFFFFFFu;
             return blk;
           }),
           "node children out of range", "child range wraps");

  // A node that is its own descendant.
  check_eq(verdict([&](Module& m, Builder& b) {
             const NodeId blk = b.block({b.literal(1, p)}, p);
             m.child_ids[m.nodes[blk.v].first_child] = blk;
             return blk;
           }),
           "node is its own ancestor", "cycle");

  // Sharing is not a cycle: one literal under two parents, and a shared
  // subtree under two siblings.
  check_eq(verdict([&](Module&, Builder& b) {
             const NodeId one = b.literal(1, p);
             const NodeId sum = b.binary(BinOp::Add, one, one, p);
             return b.binary(BinOp::Mul, sum, sum, p);
           }),
           "", "a shared node is fine");

  if (g_failures != 0) {
    std::fprintf(stderr, "verify: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("verify OK\n");
  return 0;
}
