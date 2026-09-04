// FieldGet/FieldSet: O(1) struct-field access over the same ObjectObj Index/
// SetIndex already use, at a slot the front end assigns rather than a key
// looked up by comparison. Every case here checks the promise the Tag
// comment makes: reading and writing a slot works exactly like Index/
// SetIndex on the same key would, ObjectKeys and the drop-key destructor
// (built through ObjectLit, per the contract FieldGet/FieldSet leans on) are
// untouched, and the two ways a front end can get this wrong -- an
// out-of-range slot, a non-Object receiver -- trap rather than corrupting
// anything.

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

// A FieldGet no Builder will emit: verify()'s two field checks are about
// the two ints (slot, name_const), so the node built around them is
// written once and the ints are the parameters.
std::optional<std::string> verify_field_node(Module& m, Builder& b,
                                             int32_t slot,
                                             int32_t name_const) {
  const SrcPos p{1, 1};
  Node n;
  n.tag = Tag::FieldGet;
  n.a = slot;
  n.b = name_const;
  n.first_child = static_cast<uint32_t>(m.child_ids.size());
  n.num_children = 1;
  m.child_ids.push_back(b.literal(0, p));
  m.nodes.push_back(n);
  m.funcs.push_back({"main", 0, 0,
                     NodeId{static_cast<uint32_t>(m.nodes.size() - 1)},
                     {}, {}});
  return verify(m);
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

  // --- 1. Get/set round trip, in field-slot order matching ObjectLit's --
  {
    Module m;
    Builder b(m);
    // local 0 = {a: 1, b: 2, c: 3}; slots are ObjectLit's key order: a=0,
    // b=1, c=2.
    const NodeId obj =
        b.object_lit({{b.str_literal("a", p), b.literal(1, p)},
                      {b.str_literal("b", p), b.literal(2, p)},
                      {b.str_literal("c", p), b.literal(3, p)}},
                     p);
    const NodeId body = b.block(
        {b.assign(VarKind::Local, 0, obj, p),
         b.intrinsic(IntrinsicId::Print,
                     {b.field_get(b.varref(VarKind::Local, 0, p), 0, "a",
                                  p)},
                     p),
         b.intrinsic(IntrinsicId::Print,
                     {b.field_get(b.varref(VarKind::Local, 0, p), 1, "b",
                                  p)},
                     p),
         b.field_set(b.varref(VarKind::Local, 0, p), 1, "b",
                     b.literal(99, p), p),
         b.intrinsic(IntrinsicId::Print,
                     {b.field_get(b.varref(VarKind::Local, 0, p), 1, "b",
                                  p)},
                     p),
         b.intrinsic(IntrinsicId::Print,
                     {b.field_get(b.varref(VarKind::Local, 0, p), 2, "c",
                                  p)},
                     p)},
        p);
    m.funcs.push_back({"main", 1, 0, b.scope(0, 1, body, p), {"o"}, {}});
    const RunResult r = run_module(m, "get/set round trip");
    expect_clean(r, "get/set round trip");
    check_eq(joined(), "1|2|99|3|", "get/set round trip output");
  }

  // --- 2. ObjectKeys still enumerates a struct-built object, and a
  //     FieldSet's write is what a later FieldGet reads back -- FieldGet/
  //     FieldSet is a second way to reach ObjectObj's props, not a
  //     different value underneath it. ------------------------------
  {
    Module m;
    Builder b(m);
    const NodeId obj =
        b.object_lit({{b.str_literal("x", p), b.literal(10, p)},
                      {b.str_literal("y", p), b.literal(20, p)}},
                     p);
    const NodeId body = b.block(
        {b.assign(VarKind::Local, 0, obj, p),
         b.field_set(b.varref(VarKind::Local, 0, p), 1, "y",
                     b.literal(99, p), p),
         b.assign(VarKind::Local, 1,
                  b.intrinsic(IntrinsicId::ObjectKeys,
                              {b.varref(VarKind::Local, 0, p)}, p),
                  p),
         // Read the keys out one by one: Print of an array is "<array>",
         // which would pass no matter what the keys turned out to be.
         b.intrinsic(IntrinsicId::Print,
                     {b.index(b.varref(VarKind::Local, 1, p), b.literal(0, p),
                              p)},
                     p),
         b.intrinsic(IntrinsicId::Print,
                     {b.index(b.varref(VarKind::Local, 1, p), b.literal(1, p),
                              p)},
                     p),
         b.intrinsic(IntrinsicId::Print,
                     {b.field_get(b.varref(VarKind::Local, 0, p), 1, "y", p)},
                     p)},
        p);
    m.funcs.push_back(
        {"main", 2, 0, b.scope(0, 2, body, p), {"o", "keys"}, {}});
    const RunResult r = run_module(m, "ObjectKeys after FieldGet/Set");
    expect_clean(r, "ObjectKeys after FieldGet/Set");
    check_eq(joined(), "x|y|99|", "ObjectKeys still names both fields");
  }

  // --- 3. The drop key: a struct built with kDropKey present from its one
  //     ObjectLit keeps working with FieldSet on its other fields, and its
  //     destructor still runs exactly once when the scope releases it --
  //     the destructor call is Runtime's own, over `find(kDropKey)`, and
  //     FieldSet never touches that key. -----------------------------
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    // funcs[0] must be main -- vm::run's fixed entry point -- so main is
    // pushed (with a placeholder body, filled in below) before drop.
    m.funcs.push_back({"main", 2, 0, NodeId{}, {"d", "o"}, {}});

    // drop(self) { print(self.name) } -- self is Local 0, field 1 (name);
    // field 0 is the drop key itself.
    Func drop{"drop", 1, 0, NodeId{}, {"self"}, {}};
    drop.num_params = 1;
    drop.body =
        b.intrinsic(IntrinsicId::Print,
                   {b.field_get(b.varref(VarKind::Local, 0, p), 1, "name",
                                p)},
                   p);
    m.funcs.push_back(drop);  // funcs[1]

    // main: d = closure(drop); o = {"\x01drop": d, name: "widget"};
    // FieldSet o.name = "renamed"; scope exit drops o.
    const NodeId obj = b.object_lit(
        {{b.str_literal("\x01" "drop", p), b.varref(VarKind::Local, 0, p)},
         {b.str_literal("name", p), b.str_literal("widget", p)}},
        p);
    const NodeId body = b.block(
        {b.assign(VarKind::Local, 0, b.make_closure(1, 0, p), p),
         b.scope(
             1, 2,
             b.block(
                 {b.assign(VarKind::Local, 1, obj, p),
                  b.field_set(b.varref(VarKind::Local, 1, p), 1, "name",
                             b.str_literal("renamed", p), p),
                  b.intrinsic(
                      IntrinsicId::Print,
                      {b.field_get(b.varref(VarKind::Local, 1, p), 1,
                                  "name", p)},
                      p)},
                 p),
             p)},
        p);
    m.funcs[0].body = b.scope(0, 1, body, p);
    const RunResult r = run_module(m, "drop key survives FieldSet");
    expect_clean(r, "drop key survives FieldSet");
    // "renamed" printed by the FieldGet right after the FieldSet, then
    // "renamed" again from the destructor at scope exit -- proving the
    // destructor read the field FieldSet actually wrote, not a stale copy.
    check_eq(joined(), "renamed|renamed|", "drop key survives FieldSet");
  }

  // --- 4. Traps: an out-of-range slot, a non-Object receiver, for both
  //     FieldGet and FieldSet. -----------------------------------------
  {
    Module m;
    Builder b(m);
    const NodeId obj = b.object_lit(
        {{b.str_literal("a", p), b.literal(1, p)}}, p);
    const NodeId body = b.block(
        {b.assign(VarKind::Local, 0, obj, p),
         b.intrinsic(IntrinsicId::Print,
                     {b.field_get(b.varref(VarKind::Local, 0, p), 5, "z",
                                  p)},
                     p)},
        p);
    m.funcs.push_back({"main", 1, 0, b.scope(0, 1, body, p), {"o"}, {}});
    const RunResult r = run_module(m, "FieldGet out of range");
    expect_trap(r, "out of range", "FieldGet out of range");
  }
  {
    Module m;
    Builder b(m);
    const NodeId obj = b.object_lit(
        {{b.str_literal("a", p), b.literal(1, p)}}, p);
    const NodeId body = b.block(
        {b.assign(VarKind::Local, 0, obj, p),
         b.field_set(b.varref(VarKind::Local, 0, p), 5, "z",
                     b.literal(0, p), p)},
        p);
    m.funcs.push_back({"main", 1, 0, b.scope(0, 1, body, p), {"o"}, {}});
    const RunResult r = run_module(m, "FieldSet out of range");
    expect_trap(r, "out of range", "FieldSet out of range");
  }
  {
    Module m;
    Builder b(m);
    const NodeId body = b.block(
        {b.intrinsic(IntrinsicId::Print,
                     {b.field_get(b.literal(42, p), 0, "a", p)}, p)},
        p);
    m.funcs.push_back({"main", 0, 0, b.scope(0, 0, body, p), {}, {}});
    const RunResult r = run_module(m, "FieldGet of an int");
    expect_trap(r, "cannot get field", "FieldGet of an int");
  }

  // --- 5. verify() rejects a name const that is not a string, and a
  //     negative slot -- both front-end bugs, not runtime facts. -------
  {
    Module m;
    Builder b(m);
    // A name const that is an Int, not a Str.
    m.consts.push_back({ConstKind::Int, 7});
    const auto err = verify_field_node(m, b, 0, 0);
    check(err.has_value() && err->find("string const") != std::string::npos,
         "verify rejects non-string field name");
  }
  {
    Module m;
    Builder b(m);
    const auto err = verify_field_node(m, b, -1, b.intern_str("x"));
    check(err.has_value() && err->find("non-negative") != std::string::npos,
         "verify rejects a negative field slot");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "fields: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("fields OK\n");
  return 0;
}
