// What verify() refuses, for the malformed shapes a front end's bug (or a
// hand-edited Module) can produce but the Builder never would: each is a
// way the compiler or the executor would otherwise read out of bounds. And
// the one shape that looks suspicious and is fine -- a node shared between
// two parents -- stays accepted; shared_effects is what tells the sharing
// that runs something twice from the sharing that is harmless. Its mirror,
// dropped_effects, is what tells a node built and dropped that a program
// would have seen run from one that costs only the space.

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

// shared_effects or dropped_effects (`list`) over a module whose main body
// `build` makes -- leaving out of it whatever it builds and drops -- its
// findings joined by "|". `edit`, if any, then corrupts the module.
template <class List>
std::string findings(List list, const std::function<NodeId(Builder&)>& build,
                     const std::function<void(Module&)>& edit = {}) {
  Module m;
  Builder b(m);
  m.capture_maps.push_back({});
  m.funcs.push_back({"main", 0, 0, NodeId{}, {}, {}});
  m.funcs.push_back({"f", 0, 0, b.literal(1, {9, 9}), {}, {}});
  m.funcs[0].body = build(b);
  if (edit) edit(m);
  std::string out;
  for (const auto& e : list(m)) out += to_string(m, e) + "|";
  return out;
}

std::string shared(const std::function<NodeId(Builder&)>& build) {
  return findings(shared_effects, build);
}

std::string dropped(const std::function<NodeId(Builder&)>& build,
                    const std::function<void(Module&)>& edit = {}) {
  return findings(dropped_effects, build, edit);
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

  // shared_effects: a call is at 2:2, what holds it at 3:3 and 4:4. Its
  // callee is a read, so the call itself is what makes it observable.
  const auto call = [&](Builder& b) {
    return b.call_value(b.varref(VarKind::Local, 0, p), {}, {2, 2});
  };
  check_eq(shared([&](Builder& b) {
             const NodeId c = call(b);
             return b.binary(BinOp::Add, c, c, {3, 3});
           }),
           "2:2: callvalue runs twice in binary@3:3|",
           "a call under both operands runs twice");
  check_eq(shared([&](Builder& b) {
             const NodeId c = call(b);
             return b.make_if(b.bool_literal(true, p), c, c, {3, 3});
           }),
           "", "a call under then and else runs once");
  check_eq(shared([&](Builder& b) {
             const NodeId c = call(b);
             return b.make_try(0, c, c, {3, 3});
           }),
           "2:2: callvalue runs twice in try@3:3|",
           "a call under a try's body and its handler can run twice");
  check_eq(shared([&](Builder& b) {
             const NodeId c = call(b);
             return b.make_switch(c, {{b.literal(1, p), c}}, NodeId{}, {3, 3});
           }),
           "2:2: callvalue runs twice in switch@3:3|",
           "a call as a switch's subject and in a case runs twice");
  check_eq(shared([&](Builder& b) {
             const NodeId c = call(b);
             return b.make_switch(b.literal(0, p),
                                  {{b.literal(1, p), c}, {b.literal(2, p), c}},
                                  NodeId{}, {3, 3});
           }),
           "", "a call in two cases runs once");
  check_eq(shared([&](Builder& b) {
             const NodeId c = call(b);
             const NodeId both = b.block({c}, p);
             return b.make_if(b.bool_literal(true, p),
                              b.block({both, b.block({c}, p)}, {3, 3}), both,
                              {4, 4});
           }),
           "2:2: callvalue runs twice in block@3:3|",
           "a call under both arms, and again under one of them, runs twice");
  {
    // Shared by two functions' bodies: once per call of either.
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    const NodeId c = call(b);
    m.funcs.push_back({"main", 0, 0, b.block({c}, p), {}, {}});
    m.funcs.push_back({"f", 0, 0, b.block({c}, p), {}, {}});
    check_eq(std::to_string(shared_effects(m).size()), "0",
             "a call in two functions runs once in each");
  }
  check_eq(shared([&](Builder& b) {
             const NodeId c = call(b);
             const NodeId sum = b.binary(BinOp::Add, c, b.literal(1, p), {3, 3});
             return b.array_lit({sum, sum}, {4, 4});
           }),
           "3:3: binary runs twice in arraylit@4:4|",
           "a read over a call is as observable as the call");
  check_eq(shared([&](Builder& b) {
             const NodeId map = b.intrinsic(IntrinsicId::MapNew, {}, {2, 2});
             return b.array_lit({map, map}, {4, 4});
           }),
           "2:2: intrinsic runs twice in arraylit@4:4|",
           "a fresh map is observable");
  check_eq(shared([&](Builder& b) {
             const NodeId x = b.varref(VarKind::Local, 0, p);
             const NodeId s = b.intrinsic(IntrinsicId::ToStr, {x}, p);
             return b.array_lit({s, s, x, x}, {4, 4});
           }),
           "", "a read, or a string built from one, is not");

  // dropped_effects: what a body leaves out. The call is at 2:2 again,
  // and main's body is `print(1)` beside it.
  const auto print_one = [&](Builder& b) {
    return b.intrinsic(IntrinsicId::Print, {b.literal(1, p)}, p);
  };
  check_eq(dropped([&](Builder& b) {
             call(b);
             return print_one(b);
           }),
           "2:2: callvalue is built but no function body reaches it|",
           "a call no body reaches is dropped");
  check_eq(dropped([&](Builder& b) {
             b.binary(BinOp::Add, call(b), b.literal(1, p), {3, 3});
             return print_one(b);
           }),
           "3:3: binary is built but no function body reaches it|",
           "what was dropped is reported at its top, once");
  check_eq(dropped([&](Builder& b) {
             b.intrinsic(IntrinsicId::MapNew, {}, {2, 2});
             b.intrinsic(IntrinsicId::ToStr, {b.varref(VarKind::Local, 0, p)},
                         {3, 3});
             return print_one(b);
           }),
           "2:2: intrinsic is built but no function body reaches it|",
           "a dropped fresh map is lost, a dropped string built from a read "
           "is not");
  check_eq(dropped([&](Builder& b) {
             const NodeId c = call(b);
             b.binary(BinOp::Add, c, b.literal(1, p), {3, 3});
             return b.block({c}, p);
           }),
           "", "a call kept in the body and dropped from a wrapper still runs");
  {
    // f's body is reached as much as main's: each runs when it is called.
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({"main", 0, 0, print_one(b), {}, {}});
    m.funcs.push_back({"f", 0, 0, b.block({call(b)}, p), {}, {}});
    check_eq(std::to_string(dropped_effects(m).size()), "0",
             "a call in any function's body is not dropped");
  }
  // A node no body reaches is one verify() never looked at, and one whose
  // children or position are out of range is read no further.
  const auto corrupt = [&](const std::function<void(Node&, Module&)>& edit) {
    NodeId c;
    return dropped(
        [&](Builder& b) {
          c = call(b);
          return print_one(b);
        },
        [&](Module& m) { edit(m.nodes[c.v], m); });
  };
  check_eq(corrupt([](Node& n, Module&) { n.first_child = 0xFFFFFFFFu; }), "",
           "a dropped node whose children wrap around 32 bits");
  check_eq(corrupt([](Node& n, Module& m) {
             m.child_ids[n.first_child] = NodeId{1000};
           }),
           "", "a dropped node whose child is past the end");
  check_eq(corrupt([](Node& n, Module&) { n.pos = 1000; }), "",
           "a dropped node whose position is past the end");

  if (g_failures != 0) {
    std::fprintf(stderr, "verify: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("verify OK\n");
  return 0;
}
