// Switch: a branch on one of several literal keys, all one coreir::ConstKind
// (Int or Str) and pairwise distinct. Covers what the Tag comment promises:
// dense and sparse int tables and the Str table all pick the right arm, a
// subject matching nothing falls to the default (or to nil with none), a
// subject of the wrong type traps, a Break inside an arm still targets the
// enclosing While rather than the Switch, and verify() catches the three
// front-end mistakes a hand-built Switch can make (a non-literal key,
// mismatched key kinds, a duplicate key).

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

void check(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

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
  int64_t left = 0;
};

RunResult run_module(const Module& m, const std::string& what) {
  g_out.clear();
  RunResult r;
  if (auto err = verify(m)) {
    std::fprintf(stderr, "FAIL: %s: malformed IR: %s\n", what.c_str(),
                 err->c_str());
    ++g_failures;
    return r;
  }
  Runtime rt;
  const vm::Program p = vm::compile(m);
  try {
    vm::run(p, rt);
  } catch (const Failure& e) {
    r.failure = e.what();
  }
  r.left = rt.live_objects();
  return r;
}

void expect_clean(const RunResult& r, const std::string& what) {
  check_eq(r.failure, "", what + ": unexpected failure");
  check(r.left == 0,
       what + ": leaked " + std::to_string(r.left) + " heap object(s)");
}

void expect_trap(const RunResult& r, const std::string& substr,
                 const std::string& what) {
  check(r.failure.find(substr) != std::string::npos,
       what + ": want a trap mentioning [" + substr + "], got [" +
           r.failure + "]");
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
  const SrcPos p{1, 1};

  // --- 1. Dense int keys ({1, 2, 3}, width 3): every key matches its own
  //     arm, and an out-of-table subject falls to the default. ------------
  {
    Module m;
    Builder b(m);
    std::vector<NodeId> stmts;
    auto switch_on = [&](int64_t subject) {
      return b.make_switch(
          b.literal(subject, p),
          {{b.literal(1, p), b.str_literal("one", p)},
           {b.literal(2, p), b.str_literal("two", p)},
           {b.literal(3, p), b.str_literal("three", p)}},
          b.str_literal("other", p), p);
    };
    for (int64_t v : {1, 2, 3, 5}) {
      stmts.push_back(
          b.intrinsic(IntrinsicId::Print, {switch_on(v)}, p));
    }
    m.funcs.push_back({"main", 0, 0, b.block(stmts, p), {}, {}});
    // Pins the representation, not just the output: the dense and sparse
    // forms are otherwise indistinguishable from the outside (both give the
    // right answer), so a threshold change in finish_switch_table could
    // silently stop exercising one path while every check above still
    // passes.
    check(vm::compile(m).chunks[0].switch_tables[0].dense,
         "dense int keys build a base-offset table");
    const RunResult r = run_module(m, "dense int switch");
    expect_clean(r, "dense int switch");
    check_eq(joined(), "one|two|three|other|", "dense int switch output");
  }

  // --- 2. Dense int keys, no default: an unmatched subject yields nil
  //     rather than trapping -- the same "no arm taken" nil an If without
  //     an else yields. ----------------------------------------------------
  {
    Module m;
    Builder b(m);
    const NodeId sw = b.make_switch(
        b.literal(9, p),
        {{b.literal(1, p), b.literal(10, p)}, {b.literal(2, p), b.literal(20, p)}},
        NodeId{}, p);
    const NodeId body =
        b.block({b.intrinsic(IntrinsicId::Print, {sw}, p)}, p);
    m.funcs.push_back({"main", 0, 0, body, {}, {}});
    const RunResult r = run_module(m, "no default, no match");
    expect_clean(r, "no default, no match");
    check_eq(joined(), "nil|", "no default, no match yields nil");
  }

  // --- 3. Sparse int keys (0 and 100000 -- far too wide a span for a
  //     dense array), matched and unmatched. -------------------------------
  {
    Module m;
    Builder b(m);
    auto switch_on = [&](int64_t subject) {
      return b.make_switch(
          b.literal(subject, p),
          {{b.literal(0, p), b.literal(100, p)},
           {b.literal(100000, p), b.literal(200, p)}},
          b.literal(-1, p), p);
    };
    std::vector<NodeId> stmts;
    for (int64_t v : {0, 100000, 42}) {
      stmts.push_back(b.intrinsic(IntrinsicId::Print, {switch_on(v)}, p));
    }
    m.funcs.push_back({"main", 0, 0, b.block(stmts, p), {}, {}});
    check(!vm::compile(m).chunks[0].switch_tables[0].dense,
         "a span too wide for a base-offset table falls back to binary "
         "search");
    const RunResult r = run_module(m, "sparse int switch");
    expect_clean(r, "sparse int switch");
    check_eq(joined(), "100|200|-1|", "sparse int switch output");
  }

  // --- 4. Str keys, matched and unmatched. --------------------------------
  {
    Module m;
    Builder b(m);
    auto switch_on = [&](const std::string& subject) {
      return b.make_switch(
          b.str_literal(subject, p),
          {{b.str_literal("a", p), b.literal(1, p)},
           {b.str_literal("b", p), b.literal(2, p)},
           {b.str_literal("c", p), b.literal(3, p)}},
          b.literal(-1, p), p);
    };
    std::vector<NodeId> stmts;
    for (const std::string& v : {std::string("b"), std::string("z")}) {
      stmts.push_back(b.intrinsic(IntrinsicId::Print, {switch_on(v)}, p));
    }
    m.funcs.push_back({"main", 0, 0, b.block(stmts, p), {}, {}});
    const RunResult r = run_module(m, "str switch");
    expect_clean(r, "str switch");
    check_eq(joined(), "2|-1|", "str switch output");
  }

  // --- 5. Subject type mismatch: an int subject against Str keys, and a
  //     str subject against Int keys, both trap. --------------------------
  {
    Module m;
    Builder b(m);
    const NodeId sw = b.make_switch(
        b.literal(1, p), {{b.str_literal("a", p), b.literal(1, p)}},
        NodeId{}, p);
    m.funcs.push_back(
        {"main", 0, 0,
         b.block({b.intrinsic(IntrinsicId::Print, {sw}, p)}, p), {}, {}});
    const RunResult r = run_module(m, "int subject, str keys");
    expect_trap(r, "not str", "int subject, str keys");
  }
  {
    Module m;
    Builder b(m);
    const NodeId sw = b.make_switch(
        b.str_literal("x", p), {{b.literal(1, p), b.literal(1, p)}},
        NodeId{}, p);
    m.funcs.push_back(
        {"main", 0, 0,
         b.block({b.intrinsic(IntrinsicId::Print, {sw}, p)}, p), {}, {}});
    const RunResult r = run_module(m, "str subject, int keys");
    expect_trap(r, "not int", "str subject, int keys");
  }

  // --- 6. Break inside a Switch arm does not stop at the Switch -- it
  //     still targets the enclosing While, the same as inside an If. ------
  {
    Module m;
    Builder b(m);
    // i = 0; while (i < 3) { switch (i) { case 1: break; }; i = i + 1 }
    // print(i) -- if Break exited the While at i == 1, the increment never
    // runs and i stays 1; if Break were (wrongly) caught by the Switch, the
    // loop would run to completion and i would be 3.
    const NodeId sw = b.make_switch(b.varref(VarKind::Local, 0, p),
                                    {{b.literal(1, p), b.make_break(p)}},
                                    NodeId{}, p);
    const NodeId loop_body = b.block(
        {sw, b.assign(VarKind::Local, 0,
                      b.binary(BinOp::Add, b.varref(VarKind::Local, 0, p),
                               b.literal(1, p), p),
                      p)},
        p);
    const NodeId body = b.block(
        {b.assign(VarKind::Local, 0, b.literal(0, p), p),
         b.make_while(
             b.binary(BinOp::Lt, b.varref(VarKind::Local, 0, p),
                      b.literal(3, p), p),
             loop_body, p),
         b.intrinsic(IntrinsicId::Print, {b.varref(VarKind::Local, 0, p)},
                     p)},
        p);
    m.funcs.push_back({"main", 1, 0, body, {"i"}, {}});
    const RunResult r = run_module(m, "break inside switch arm");
    expect_clean(r, "break inside switch arm");
    check_eq(joined(), "1|", "break inside switch arm exits the while");
  }

  // --- 7. A Switch nested in another's arm: switch_tables entries pushed
  //     while compiling the inner one must not invalidate the outer's own
  //     table index. ---------------------------------------------------
  {
    Module m;
    Builder b(m);
    auto inner = [&](int64_t subject) {
      return b.make_switch(
          b.literal(subject, p),
          {{b.literal(1, p), b.str_literal("inner-one", p)}},
          b.str_literal("inner-other", p), p);
    };
    const NodeId outer = b.make_switch(
        b.literal(1, p),
        {{b.literal(1, p), inner(1)}, {b.literal(2, p), inner(9)}},
        b.str_literal("outer-other", p), p);
    m.funcs.push_back(
        {"main", 0, 0,
         b.block({b.intrinsic(IntrinsicId::Print, {outer}, p)}, p), {}, {}});
    const RunResult r = run_module(m, "nested switch");
    expect_clean(r, "nested switch");
    check_eq(joined(), "inner-one|", "nested switch picks the right arms");
  }

  // --- 8. verify() rejects a non-literal key, mismatched key kinds, and a
  //     duplicate key -- all front-end bugs no runtime fact could excuse. -
  {
    Module m;
    Builder b(m);
    const NodeId sw = b.make_switch(
        b.literal(2, p),
        {{b.binary(BinOp::Add, b.literal(1, p), b.literal(1, p), p),
          b.literal(0, p)}},
        NodeId{}, p);
    m.funcs.push_back({"main", 0, 0, sw, {}, {}});
    const auto err = verify(m);
    check(err.has_value() && err->find("literal") != std::string::npos,
         "verify rejects a non-literal switch key");
  }
  {
    Module m;
    Builder b(m);
    const NodeId sw = b.make_switch(
        b.literal(1, p),
        {{b.literal(1, p), b.literal(0, p)},
         {b.str_literal("x", p), b.literal(0, p)}},
        NodeId{}, p);
    m.funcs.push_back({"main", 0, 0, sw, {}, {}});
    const auto err = verify(m);
    check(err.has_value() && err->find("const kind") != std::string::npos,
         "verify rejects mismatched switch key kinds");
  }
  {
    Module m;
    Builder b(m);
    const NodeId sw = b.make_switch(
        b.literal(1, p),
        {{b.literal(1, p), b.literal(0, p)},
         {b.literal(1, p), b.literal(9, p)}},
        NodeId{}, p);
    m.funcs.push_back({"main", 0, 0, sw, {}, {}});
    const auto err = verify(m);
    check(err.has_value() && err->find("duplicate") != std::string::npos,
         "verify rejects a duplicate switch key");
  }

  // --- 9. A switch with no keys at all -- `switch (x) { default: ... }`,
  //     which every language with a default writes -- has no ConstKind to
  //     hold the subject to, so it takes the default whatever the subject
  //     is rather than trapping on the table's Int default. ---------------
  {
    Module m;
    Builder b(m);
    const NodeId sw = b.make_switch(b.str_literal("x", p), {},
                                    b.literal(7, p), p);
    m.funcs.push_back(
        {"main", 0, 0,
         b.block({b.intrinsic(IntrinsicId::Print, {sw}, p)}, p), {}, {}});
    const RunResult r = run_module(m, "default only, str subject");
    expect_clean(r, "default only, str subject");
    check_eq(joined(), "7|", "default only, str subject takes the default");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "switch: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("switch OK\n");
  return 0;
}
