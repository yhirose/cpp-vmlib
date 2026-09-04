// Arrays and objects: building them, reading and writing through them, and
// what happens at the edges.
//
// The two share Index and SetIndex rather than having a pair of nodes each,
// so most of what is worth checking is that the dispatch picks the right one
// and says something useful when it cannot. The asymmetry between them --
// a missing array element fails, a missing object property is nil -- is a
// deliberate decision recorded here so that changing it is deliberate too.
//
// Every case asserts exactly what is left on the heap afterwards, which for
// containers is the part that could silently go wrong: an array holds its
// elements, so releasing one has to release what it held. That is zero
// everywhere except the case that deliberately makes a container hold
// itself.

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "vmlib.h"

namespace {

struct Failure : std::runtime_error {
  Failure(std::string m) : std::runtime_error(std::move(m)) {}
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
  for (const auto& line : g_out) s += line + "|";
  return s;
}

// `want_left` is what should still be on the heap when the program ends,
// measured before ~Runtime sweeps it -- zero everywhere except the case that
// deliberately builds a cycle.
std::string run_module(const coreir::Module& m, const std::string& what,
                       int64_t want_left = 0) {
  g_out.clear();
  if (auto err = coreir::verify(m)) {
    std::fprintf(stderr, "FAIL: %s: malformed IR: %s\n", what.c_str(),
                 err->c_str());
    ++g_failures;
    return {};
  }
  std::string failure;
  int64_t left = 0;
  {
    coreir::Runtime rt;
    try {
      vm::run(vm::compile(m), rt);
    } catch (const Failure& e) {
      failure = e.what();
    }
    left = rt.live_objects();
  }
  if (left != want_left) {
    std::fprintf(stderr,
                 "FAIL: %s: %lld heap object(s) left, expected %lld\n",
                 what.c_str(), static_cast<long long>(left),
                 static_cast<long long>(want_left));
    ++g_failures;
  }
  return failure;
}

}  // namespace

extern "C" {
void coreir_rt_out(int64_t v) { g_out.push_back(std::to_string(v)); }
void coreir_rt_out_str(const char* b, int64_t n) {
  g_out.emplace_back(b, static_cast<size_t>(n));
}
void coreir_rt_out_raw(const char*, int64_t) {}
int64_t coreir_rt_in(int64_t, int64_t) { return 0; }
void coreir_rt_poll(void) {}
[[noreturn]] void coreir_rt_fail(const char* m, int64_t, int64_t) {
  throw Failure(m);
}
}

int main() {
  // (string indexing cases are appended at the end; see the tail)
  using namespace coreir;
  const SrcPos p{1, 1};

  // Arrays hold values, including other containers, and release them.
  {
    Module m;
    Builder b(m);
    const NodeId xs = b.varref(VarKind::Local, 0, p);
    m.funcs.push_back(
        {"main", 1, 0,
         b.block(
             {b.assign(VarKind::Local, 0,
                       b.array_lit({b.literal(10, p), b.str_literal("two", p),
                                    b.array_lit({b.literal(3, p)}, p)},
                                   p),
                       p),
              b.intrinsic(IntrinsicId::Print,
                          {b.index(xs, b.literal(0, p), p)}, p),
              b.intrinsic(IntrinsicId::Print,
                          {b.index(xs, b.literal(1, p), p)}, p),
              b.intrinsic(IntrinsicId::Print,
                          {b.index(b.index(xs, b.literal(2, p), p),
                                   b.literal(0, p), p)},
                          p),
              b.intrinsic(IntrinsicId::Print,
                          {b.intrinsic(IntrinsicId::Len, {xs}, p)}, p),
              b.set_index(xs, b.literal(0, p), b.str_literal("ten", p), p),
              b.intrinsic(IntrinsicId::Print,
                          {b.index(xs, b.literal(0, p), p)}, p)},
             p),
         {"xs"},
         {}});
    check_eq(run_module(m, "array"), "", "array: unexpected failure");
    check_eq(joined(), "10|two|3|3|ten|", "array output");
  }

  // Objects keep insertion order, overwrite in place, and read a missing key
  // as nil.
  {
    Module m;
    Builder b(m);
    const NodeId o = b.varref(VarKind::Local, 0, p);
    m.funcs.push_back(
        {"main", 1, 0,
         b.block({b.assign(VarKind::Local, 0,
                           b.object_lit({{b.str_literal("a", p),
                                          b.literal(1, p)},
                                         {b.str_literal("b", p),
                                          b.str_literal("two", p)}},
                                        p),
                           p),
                  b.intrinsic(IntrinsicId::Print,
                              {b.index(o, b.str_literal("a", p), p)}, p),
                  b.intrinsic(IntrinsicId::Print,
                              {b.index(o, b.str_literal("b", p), p)}, p),
                  b.intrinsic(IntrinsicId::Print,
                              {b.index(o, b.str_literal("missing", p), p)}, p),
                  b.set_index(o, b.str_literal("a", p), b.literal(99, p), p),
                  b.intrinsic(IntrinsicId::Print,
                              {b.index(o, b.str_literal("a", p), p)}, p),
                  // Overwriting an existing key does not add one.
                  b.intrinsic(IntrinsicId::Print,
                              {b.intrinsic(IntrinsicId::Len, {o}, p)}, p),
                  b.set_index(o, b.str_literal("c", p), b.literal(3, p), p),
                  b.intrinsic(IntrinsicId::Print,
                              {b.intrinsic(IntrinsicId::Len, {o}, p)}, p)},
                 p),
         {"o"},
         {}});
    check_eq(run_module(m, "object"), "", "object: unexpected failure");
    check_eq(joined(), "1|two|nil|99|2|3|", "object output");
  }

  // A container holding itself is a cycle, same as a self-capturing closure.
  // Nothing collects it while the program runs; the heap still ends empty
  // because ~Runtime sweeps what counting could not.
  {
    Module m;
    Builder b(m);
    const NodeId xs = b.varref(VarKind::Local, 0, p);
    m.funcs.push_back(
        {"main", 1, 0,
         b.block({b.assign(VarKind::Local, 0,
                           b.array_lit({b.literal(0, p)}, p), p),
                  b.set_index(xs, b.literal(0, p), xs, p),
                  b.intrinsic(IntrinsicId::Print, {b.literal(1, p)}, p)},
                 p),
         {"xs"},
         {}});
    // One object left: the array holding itself. Counting cannot free that,
    // and asserting the exact number means a later collector shows up here
    // as a change rather than as silence.
    check_eq(run_module(m, "self-referential array", 1), "",
             "cycle: unexpected failure");
    check_eq(joined(), "1|", "cycle output");
  }

  // The edges.
  struct Case {
    const char* what;
    NodeId (*build)(Builder&, SrcPos);
    const char* want;
  };
  const Case cases[] = {
      {"index out of range",
       [](Builder& b, SrcPos q) {
         return b.index(b.array_lit({b.literal(1, q)}, q), b.literal(5, q), q);
       },
       "array index 5 out of range for length 1"},
      {"negative index",
       [](Builder& b, SrcPos q) {
         return b.index(b.array_lit({b.literal(1, q)}, q), b.literal(-1, q), q);
       },
       "array index -1 out of range for length 1"},
      {"string key on an array",
       [](Builder& b, SrcPos q) {
         return b.index(b.array_lit({}, q), b.str_literal("k", q), q);
       },
       "array index must be an int, not string"},
      {"int key on an object",
       [](Builder& b, SrcPos q) {
         return b.index(b.object_lit({}, q), b.literal(0, q), q);
       },
       "object key must be a string, not int"},
      {"indexing a scalar",
       [](Builder& b, SrcPos q) {
         return b.index(b.literal(1, q), b.literal(0, q), q);
       },
       "cannot index int"},
      {"length of a scalar",
       [](Builder& b, SrcPos q) {
         return b.intrinsic(IntrinsicId::Len, {b.literal(1, q)}, q);
       },
       "cannot take the length of int"},
  };
  for (const Case& c : cases) {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 0, 0,
         b.intrinsic(IntrinsicId::Print, {c.build(b, p)}, p), {}, {}});
    check_eq(run_module(m, c.what), c.want, std::string(c.what) + ": message");
  }

  // --- Strings index like arrays, one byte out, and refuse writes. --------
  {
    Module m;
    Builder b(m);
    const NodeId s0 = b.str_literal("abc", p);
    m.funcs.push_back(
        {"main", 0, 0,
         b.block({b.intrinsic(IntrinsicId::Print,
                              {b.index(s0, b.literal(1, p), p)}, p)},
                 p),
         {},
         {}});
    check_eq(run_module(m, "string index"), "", "string index: failure");
    check_eq(joined(), "b|", "string index output");
  }
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 0, 0,
         b.intrinsic(IntrinsicId::Print,
                     {b.index(b.str_literal("abc", p), b.literal(3, p), p)},
                     p),
         {},
         {}});
    check_eq(run_module(m, "string index range"),
             "string index 3 out of range for length 3",
             "string index range: message");
  }
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 0, 0,
         b.set_index(b.str_literal("abc", p), b.literal(0, p),
                     b.str_literal("z", p), p),
         {},
         {}});
    check_eq(run_module(m, "string set_index"), "cannot assign into a string",
             "string set_index: message");
  }

  // --- The five container intrinsics. -------------------------------------
  // push/pop grow and shrink; has tells absent from nil-valued; keys come
  // back in insertion order; remove erases (absent = no-op).
  {
    Module m;
    Builder b(m);
    auto pr = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::Print, {v}, p);
    };
    const NodeId arr = b.varref(VarKind::Local, 0, p);
    const NodeId obj = b.varref(VarKind::Local, 1, p);
    const NodeId body = b.block(
        {b.assign(VarKind::Local, 0, b.array_lit({b.literal(1, p)}, p), p),
         b.intrinsic(IntrinsicId::ArrayPush, {arr, b.literal(2, p)}, p),
         pr(b.intrinsic(IntrinsicId::Len, {arr}, p)),
         pr(b.intrinsic(IntrinsicId::ArrayPop, {arr}, p)),
         pr(b.intrinsic(IntrinsicId::Len, {arr}, p)),
         b.assign(VarKind::Local, 1,
                  b.object_lit({{b.str_literal("b", p), b.nil_literal(p)},
                                {b.str_literal("a", p), b.literal(9, p)}},
                               p),
                  p),
         pr(b.intrinsic(IntrinsicId::ObjectHas,
                        {obj, b.str_literal("b", p)}, p)),
         pr(b.index(obj, b.str_literal("b", p), p)),
         pr(b.index(b.intrinsic(IntrinsicId::ObjectKeys, {obj}, p),
                    b.literal(0, p), p)),
         b.intrinsic(IntrinsicId::ObjectRemove,
                     {obj, b.str_literal("b", p)}, p),
         pr(b.intrinsic(IntrinsicId::ObjectHas,
                        {obj, b.str_literal("b", p)}, p)),
         pr(b.intrinsic(IntrinsicId::Len, {obj}, p))},
        p);
    m.funcs.push_back({"main", 2, 0, body, {"arr", "obj"}, {}});
    check_eq(run_module(m, "container intrinsics"), "",
             "container intrinsics: failure");
    check_eq(joined(), "2|2|1|true|nil|b|false|1|",
             "container intrinsics output");
  }
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 0, 0,
         b.intrinsic(IntrinsicId::Print,
                     {b.intrinsic(IntrinsicId::ArrayPop,
                                  {b.array_lit({}, p)}, p)},
                     p),
         {},
         {}});
    check_eq(run_module(m, "pop empty"), "pop from an empty array",
             "pop empty: message");
  }

  // --- Maps: any value is a key, by MapKeyRef's rule. --------------------
  // An int, a string, a double, a bool and nil are five distinct keys; a
  // string key matches by content, so ToStr(1) -- a fresh allocation --
  // finds what the literal "1" stored (the case Same gets wrong); Index on
  // a missing key is nil; ObjectHas/ObjectKeys/ObjectRemove accept a map.
  {
    Module m;
    Builder b(m);
    auto pr = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::Print, {v}, p);
    };
    const NodeId mp = b.varref(VarKind::Local, 0, p);
    const NodeId body = b.block(
        {b.assign(VarKind::Local, 0, b.intrinsic(IntrinsicId::MapNew, {}, p), p),
         b.set_index(mp, b.literal(1, p), b.str_literal("int", p), p),
         b.set_index(mp, b.str_literal("1", p), b.str_literal("str", p), p),
         b.set_index(mp, b.double_literal(1.5, p), b.str_literal("dbl", p), p),
         b.set_index(mp, b.bool_literal(true, p), b.str_literal("bool", p), p),
         b.set_index(mp, b.nil_literal(p), b.str_literal("nil", p), p),
         pr(b.intrinsic(IntrinsicId::Len, {mp}, p)),
         pr(b.index(mp, b.literal(1, p), p)),
         pr(b.index(mp, b.intrinsic(IntrinsicId::ToStr, {b.literal(1, p)}, p),
                    p)),
         pr(b.index(mp, b.double_literal(1.5, p), p)),
         pr(b.index(mp, b.bool_literal(true, p), p)),
         pr(b.index(mp, b.nil_literal(p), p)),
         pr(b.index(mp, b.literal(2, p), p)),
         pr(b.intrinsic(IntrinsicId::ObjectHas, {mp, b.literal(2, p)}, p)),
         pr(b.intrinsic(IntrinsicId::ObjectHas, {mp, b.literal(1, p)}, p)),
         pr(b.index(b.intrinsic(IntrinsicId::ObjectKeys, {mp}, p),
                    b.literal(0, p), p)),
         pr(b.intrinsic(IntrinsicId::Len,
                        {b.intrinsic(IntrinsicId::ObjectKeys, {mp}, p)}, p)),
         b.intrinsic(IntrinsicId::ObjectRemove, {mp, b.literal(1, p)}, p),
         b.intrinsic(IntrinsicId::ObjectRemove, {mp, b.literal(99, p)}, p),
         pr(b.intrinsic(IntrinsicId::ObjectHas, {mp, b.literal(1, p)}, p)),
         pr(b.intrinsic(IntrinsicId::Len, {mp}, p)),
         // The first surviving key is now "1" -- removal keeps the order.
         pr(b.index(b.intrinsic(IntrinsicId::ObjectKeys, {mp}, p),
                    b.literal(0, p), p)),
         pr(b.intrinsic(IntrinsicId::TypeOf, {mp}, p))},
        p);
    m.funcs.push_back({"main", 1, 0, body, {"m"}, {}});
    check_eq(run_module(m, "map"), "", "map: failure");
    check_eq(joined(),
             "5|int|str|dbl|bool|nil|nil|false|true|1|5|false|4|1|map|",
             "map output");
  }
  // Objects as keys go by identity: two empty objects are two keys, and
  // the same object read twice is one. Compaction after many removals
  // keeps every survivor findable.
  {
    Module m;
    Builder b(m);
    auto pr = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::Print, {v}, p);
    };
    const NodeId mp = b.varref(VarKind::Local, 0, p);
    const NodeId o1 = b.varref(VarKind::Local, 1, p);
    const NodeId o2 = b.varref(VarKind::Local, 2, p);
    const NodeId i = b.varref(VarKind::Local, 3, p);
    std::vector<NodeId> stmts{
        b.assign(VarKind::Local, 0, b.intrinsic(IntrinsicId::MapNew, {}, p), p),
        b.assign(VarKind::Local, 1, b.object_lit({}, p), p),
        b.assign(VarKind::Local, 2, b.object_lit({}, p), p),
        b.set_index(mp, o1, b.literal(1, p), p),
        b.set_index(mp, o2, b.literal(2, p), p),
        b.set_index(mp, o1, b.literal(11, p), p),
        pr(b.intrinsic(IntrinsicId::Len, {mp}, p)),
        pr(b.index(mp, o1, p)),
        pr(b.index(mp, o2, p)),
        // 100 int keys in, 90 removed: the vector compacts under them.
        b.assign(VarKind::Local, 3, b.literal(0, p), p),
        b.make_while(b.binary(BinOp::Lt, i, b.literal(100, p), p),
                     b.block({b.set_index(mp, i, i, p),
                              b.assign(VarKind::Local, 3,
                                       b.binary(BinOp::Add, i, b.literal(1, p),
                                                p),
                                       p)},
                             p),
                     p),
        b.assign(VarKind::Local, 3, b.literal(0, p), p),
        b.make_while(b.binary(BinOp::Lt, i, b.literal(90, p), p),
                     b.block({b.intrinsic(IntrinsicId::ObjectRemove, {mp, i}, p),
                              b.assign(VarKind::Local, 3,
                                       b.binary(BinOp::Add, i, b.literal(1, p),
                                                p),
                                       p)},
                             p),
                     p),
        pr(b.intrinsic(IntrinsicId::Len, {mp}, p)),
        pr(b.index(mp, b.literal(95, p), p)),
        pr(b.index(mp, o2, p)),
        pr(b.intrinsic(IntrinsicId::ObjectHas, {mp, b.literal(5, p)}, p)),
        pr(b.index(b.intrinsic(IntrinsicId::ObjectKeys, {mp}, p),
                   b.literal(2, p), p))};
    m.funcs.push_back({"main", 4, 0, b.block(stmts, p),
                       {"m", "o1", "o2", "i"}, {}});
    check_eq(run_module(m, "map identity"), "", "map identity: failure");
    check_eq(joined(), "2|11|2|12|95|2|false|90|", "map identity output");
  }
  // A cycle through a map key is the collector's: m[o] = o, o.m = m, both
  // locals dropped -- Collect frees exactly the two of them.
  {
    Module m;
    Builder b(m);
    const NodeId mp = b.varref(VarKind::Local, 0, p);
    const NodeId o = b.varref(VarKind::Local, 1, p);
    m.funcs.push_back(
        {"main", 2, 0,
         b.block({b.assign(VarKind::Local, 0,
                           b.intrinsic(IntrinsicId::MapNew, {}, p), p),
                  b.assign(VarKind::Local, 1, b.object_lit({}, p), p),
                  b.set_index(mp, o, o, p),
                  b.set_index(o, b.str_literal("m", p), mp, p),
                  b.assign(VarKind::Local, 0, b.nil_literal(p), p),
                  b.assign(VarKind::Local, 1, b.nil_literal(p), p),
                  b.intrinsic(IntrinsicId::Print,
                              {b.intrinsic(IntrinsicId::Collect, {}, p)}, p)},
                 p),
         {"m", "o"}, {}});
    check_eq(run_module(m, "map cycle"), "", "map cycle: failure");
    check_eq(joined(), "2|", "map cycle output");
  }
  // A map is not an object: FieldGet needs the slot-shaped receiver.
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 0, 0,
         b.intrinsic(IntrinsicId::Print,
                     {b.field_get(b.intrinsic(IntrinsicId::MapNew, {}, p), 0,
                                  "x", p)},
                     p),
         {}, {}});
    check_eq(run_module(m, "map field"), "cannot get field 'x' of map",
             "map field: message");
  }

  // --- Slices and bytes. ---------------------------------------------------
  {
    Module m;
    Builder b(m);
    auto pr = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::Print, {v}, p);
    };
    const NodeId arr = b.varref(VarKind::Local, 0, p);
    const NodeId sl = b.varref(VarKind::Local, 1, p);
    m.funcs.push_back(
        {"main", 2, 0,
         b.block(
             {pr(b.intrinsic(IntrinsicId::StrSlice,
                             {b.str_literal("hello", p), b.literal(1, p),
                              b.literal(3, p)},
                             p)),
              pr(b.intrinsic(IntrinsicId::StrSlice,
                             {b.str_literal("hello", p), b.literal(5, p),
                              b.literal(5, p)},
                             p)),
              b.assign(VarKind::Local, 0,
                       b.array_lit({b.literal(1, p), b.literal(2, p),
                                    b.literal(3, p), b.literal(4, p)},
                                   p),
                       p),
              b.assign(VarKind::Local, 1,
                       b.intrinsic(IntrinsicId::ArraySlice,
                                   {arr, b.literal(1, p), b.literal(3, p)}, p),
                       p),
              pr(b.intrinsic(IntrinsicId::Len, {sl}, p)),
              pr(b.index(sl, b.literal(0, p), p)),
              // A slice is a copy: writing through it leaves the source be.
              b.set_index(sl, b.literal(0, p), b.literal(99, p), p),
              pr(b.index(arr, b.literal(1, p), p)),
              pr(b.intrinsic(IntrinsicId::StrByte,
                             {b.str_literal("AB", p), b.literal(1, p)}, p)),
              pr(b.intrinsic(IntrinsicId::StrFromByte, {b.literal(67, p)}, p)),
              pr(b.binary(BinOp::Add,
                          b.intrinsic(IntrinsicId::StrFromByte,
                                      {b.literal(0xE3, p)}, p),
                          b.intrinsic(IntrinsicId::StrFromByte,
                                      {b.literal(0x81, p)}, p),
                          p))},
             p),
         {"arr", "sl"}, {}});
    check_eq(run_module(m, "slices"), "", "slices: failure");
    check_eq(joined(), "el||2|2|2|66|C|\xE3\x81|", "slices output");
  }
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 0, 0,
         b.intrinsic(IntrinsicId::Print,
                     {b.intrinsic(IntrinsicId::StrSlice,
                                  {b.str_literal("abc", p), b.literal(2, p),
                                   b.literal(1, p)},
                                  p)},
                     p),
         {}, {}});
    check_eq(run_module(m, "slice bounds"),
             "slice [2, 1) out of range for string of length 3",
             "slice bounds: message");
  }
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 0, 0,
         b.intrinsic(IntrinsicId::Print,
                     {b.intrinsic(IntrinsicId::StrFromByte, {b.literal(256, p)},
                                  p)},
                     p),
         {}, {}});
    check_eq(run_module(m, "byte range"),
             "byte value must be an int in 0..255, not 256",
             "byte range: message");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "containers: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("containers OK\n");
  return 0;
}
