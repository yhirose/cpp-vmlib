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

  // ArrayConcat joins one array onto another, in place: what a front end
  // writes as a loop over ArrayPush when it has to build an argument list
  // or a return list out of two.
  {
    Module m;
    Builder b(m);
    const NodeId xs = b.varref(VarKind::Local, 0, p);
    m.funcs.push_back(
        {"main", 1, 0,
         b.block(
             {b.assign(VarKind::Local, 0,
                       b.array_lit({b.literal(1, p), b.literal(2, p)}, p), p),
              b.intrinsic(IntrinsicId::ArrayConcat,
                          {xs, b.array_lit({b.literal(3, p), b.literal(4, p)},
                                           p)},
                          p),
              b.intrinsic(IntrinsicId::Print,
                          {b.intrinsic(IntrinsicId::Len, {xs}, p)}, p),
              b.intrinsic(IntrinsicId::Print, {b.index(xs, b.literal(3, p), p)},
                          p)},
             p),
         {"xs"},
         {}});
    run_module(m, "concat");
    check_eq(joined(), "4|4|", "concat output");
  }
  // Onto itself: the source is read by index because appending reallocates.
  {
    Module m;
    Builder b(m);
    const NodeId xs = b.varref(VarKind::Local, 0, p);
    m.funcs.push_back(
        {"main", 1, 0,
         b.block(
             {b.assign(VarKind::Local, 0,
                       b.array_lit({b.literal(1, p), b.literal(2, p)}, p), p),
              b.intrinsic(IntrinsicId::ArrayConcat, {xs, xs}, p),
              b.intrinsic(IntrinsicId::Print,
                          {b.intrinsic(IntrinsicId::Len, {xs}, p)}, p),
              b.intrinsic(IntrinsicId::Print, {b.index(xs, b.literal(2, p), p)},
                          p)},
             p),
         {"xs"},
         {}});
    run_module(m, "concat self");
    check_eq(joined(), "4|1|", "concat self output");
  }
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 0, 0,
         b.intrinsic(IntrinsicId::ArrayConcat,
                     {b.array_lit({}, p), b.literal(1, p)}, p),
         {}, {}});
    check_eq(run_module(m, "concat type"), "cannot join int onto array",
             "concat type: message");
  }

  // ArrayFill starts an array at a length, which `new T[n]` needs and a
  // loop over ArrayPush is the long way round.
  {
    Module m;
    Builder b(m);
    const NodeId xs = b.varref(VarKind::Local, 0, p);
    m.funcs.push_back(
        {"main", 1, 0,
         b.block(
             {b.assign(VarKind::Local, 0,
                       b.intrinsic(IntrinsicId::ArrayFill,
                                   {b.literal(3, p), b.literal(7, p)}, p),
                       p),
              b.intrinsic(IntrinsicId::Print,
                          {b.intrinsic(IntrinsicId::Len, {xs}, p)}, p),
              b.intrinsic(IntrinsicId::Print, {b.index(xs, b.literal(2, p), p)},
                          p)},
             p),
         {"xs"},
         {}});
    run_module(m, "fill");
    check_eq(joined(), "3|7|", "fill output");
  }
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 0, 0,
         b.intrinsic(IntrinsicId::Print,
                     {b.intrinsic(IntrinsicId::ArrayFill,
                                  {b.literal(-1, p), b.nil_literal(p)}, p)},
                     p),
         {}, {}});
    check_eq(run_module(m, "fill negative"),
             "cannot make an array of fewer than 0", "fill negative: message");
  }
  {
    // A length no allocator will give is a trap a program can catch, not a
    // C++ exception out of vm::run.
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 1, 0,
         b.make_try(0,
                    b.intrinsic(IntrinsicId::Print,
                                {b.intrinsic(IntrinsicId::ArrayFill,
                                             {b.literal(int64_t{1} << 62, p),
                                              b.nil_literal(p)},
                                             p)},
                                p),
                    b.intrinsic(IntrinsicId::Print,
                                {b.index(b.varref(VarKind::Local, 0, p),
                                         b.str_literal("message", p), p)},
                                p),
                    p),
         {"e"}, {}});
    check_eq(run_module(m, "fill too large"), "", "fill too large: failure");
    check_eq(joined(), "array too large|", "fill too large: caught");
  }

  // Len answers a map's live entry count, tombstones skipped -- the same
  // question it answers for a string and an array, which is why there is no
  // second intrinsic for it.
  {
    Module m;
    Builder b(m);
    const NodeId o = b.varref(VarKind::Local, 0, p);
    m.funcs.push_back(
        {"main", 1, 0,
         b.block(
             {b.assign(VarKind::Local, 0, b.intrinsic(IntrinsicId::MapNew, {},
                                                      p),
                       p),
              b.set_index(o, b.str_literal("a", p), b.literal(1, p), p),
              b.set_index(o, b.literal(2, p), b.literal(2, p), p),
              b.intrinsic(IntrinsicId::Print,
                          {b.intrinsic(IntrinsicId::Len, {o}, p)}, p),
              b.intrinsic(IntrinsicId::ObjectRemove, {o, b.literal(2, p)}, p),
              b.intrinsic(IntrinsicId::Print,
                          {b.intrinsic(IntrinsicId::Len, {o}, p)}, p)},
             p),
         {"xs"},
         {}});
    run_module(m, "map len");
    check_eq(joined(), "2|1|", "map len output");
  }

  // ASCII case, byte by byte: a multi-byte character passes through whole
  // because none of its bytes is in A-Z or a-z.
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 0, 0,
         b.block(
             {b.intrinsic(IntrinsicId::Print,
                          {b.intrinsic(IntrinsicId::StrUpper,
                                       {b.str_literal("aB c1\xC3\xA9", p)},
                                       p)},
                          p),
              b.intrinsic(IntrinsicId::Print,
                          {b.intrinsic(IntrinsicId::StrLower,
                                       {b.str_literal("Ab C1", p)}, p)},
                          p)},
             p),
         {}, {}});
    run_module(m, "case");
    check_eq(joined(), "AB C1\xC3\xA9|ab c1|", "case output");
  }
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 0, 0,
         b.intrinsic(IntrinsicId::Print,
                     {b.intrinsic(IntrinsicId::StrUpper, {b.literal(1, p)}, p)},
                     p),
         {}, {}});
    check_eq(run_module(m, "case type"), "cannot change the case of int",
             "case type: message");
  }

  // Not is the inverse of the truthiness JumpIfFalse tests, which is why it
  // answers for every value rather than only for a Bool.
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 0, 0,
         b.block(
             {b.intrinsic(IntrinsicId::Print,
                          {b.unary(UnOp::Not, b.bool_literal(true, p), p)}, p),
              b.intrinsic(IntrinsicId::Print,
                          {b.unary(UnOp::Not, b.nil_literal(p), p)}, p),
              b.intrinsic(IntrinsicId::Print,
                          {b.unary(UnOp::Not, b.literal(0, p), p)}, p),
              b.intrinsic(IntrinsicId::Print,
                          {b.unary(UnOp::Not, b.str_literal("", p), p)}, p)},
             p),
         {}, {}});
    run_module(m, "not");
    check_eq(joined(), "false|true|true|false|", "not output");
  }

  // A SetIndex whose value expression empties the receiver: the key is
  // checked after the value ran, against the length it has then, so the
  // write traps rather than landing in a slot that is gone.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({"main", 1, 0, NodeId{}, {"xs"}, {}});
    Func clear{"clear", 1, 0, NodeId{}, {"xs"}, {}};
    clear.num_params = 1;
    clear.body = b.block(
        {b.intrinsic(IntrinsicId::ArrayPop, {b.varref(VarKind::Local, 0, p)},
                     p),
         b.make_return(b.literal(7, p), p)},
        p);
    m.funcs.push_back(clear);
    const NodeId xs = b.varref(VarKind::Local, 0, p);
    m.funcs[0].body = b.block(
        {b.assign(VarKind::Local, 0, b.array_lit({b.literal(5, p)}, p), p),
         b.set_index(xs, b.literal(0, p),
                     b.call_value(b.make_closure(1, 0, p), {xs}, p), p)},
        p);
    check_eq(run_module(m, "set_index: value empties the receiver"),
             "array index 0 out of range for length 0",
             "set_index: value empties the receiver");
  }

  // index_fault, read and write: a string is readable and not writable, and
  // the write fault comes before the key is looked at.
  {
    const Value str = Value::make_str("abc");
    check(index_fault(str, Value::make_int(1)) == Fault::None,
          "index_fault: a string reads");
    check(index_fault(str, Value::make_int(1), true) == Fault::Immutable,
          "index_fault: a string does not write");
    check(index_fault(str, Value::make_str("k"), true) == Fault::Immutable,
          "index_fault: the write fault comes first");
    check_eq(fault_text(Fault::Immutable, str, Value::make_int(1)),
             "cannot assign into a string", "index_fault: its text");
  }

  // A SetIndex with a fallback: the VM stores when the receiver and the key
  // are in their sets, the value is in its, and the store does not fault;
  // otherwise the fallback is called with the three operands -- after them,
  // and only then -- and the store is the fallback's to make. Here it only
  // says so ("fb"), which is how the cases below tell the two apart.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({"main", 3, 0, NodeId{}, {"xs", "o", "mp"}, {}});
    Func fb{"fb", 3, 0, NodeId{}, {"r", "k", "v"}, {}};
    fb.num_params = 3;
    fb.lenient_arity = true;
    fb.body = b.intrinsic(IntrinsicId::Print, {b.str_literal("fb", p)}, p);
    m.funcs.push_back(fb);
    const NodeId xs = b.varref(VarKind::Local, 0, p);
    const NodeId o = b.varref(VarKind::Local, 1, p);
    const NodeId mp = b.varref(VarKind::Local, 2, p);
    const auto L = [&](int64_t v) { return b.literal(v, p); };
    const auto S = [&](const char* v) { return b.str_literal(v, p); };
    const auto print = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::Print, {v}, p);
    };
    const auto set = [&](NodeId r, NodeId k, NodeId v, uint32_t tags,
                         uint32_t key_tags = 0, uint32_t value_tags = 0,
                         int32_t flags = 0) {
      Fallback f{b.make_closure(1, 0, p), tags, flags};
      f.key_tags = key_tags;
      f.value_tags = value_tags;
      return b.set_index(r, k, v, f, p);
    };
    const uint32_t kContainers = tags_named({"array", "object", "map"});
    const uint32_t kKeys = tags_named({"int", "string"});
    const NodeId body = b.block(
        {b.assign(VarKind::Local, 0, b.array_lit({L(10), L(20)}, p), p),
         b.assign(VarKind::Local, 1,
                  b.object_lit({{S("a"), L(1)}}, p), p),
         b.assign(VarKind::Local, 2, b.intrinsic(IntrinsicId::MapNew, {}, p),
                  p),
         // In range, an existing key, a new key, a map: the VM's.
         set(xs, L(1), L(21), kContainers | kKeys),
         set(o, S("a"), L(2), kContainers | kKeys),
         set(o, S("b"), L(3), kContainers | kKeys, 0, 0, kMissNil),
         set(mp, S("k"), L(4), kContainers | kKeys, 0, 0, kMissNil),
         print(S(".")),
         // At the length, below zero, a string key on an array, a
         // non-string key on an object: faults, the fallback's.
         set(xs, L(2), L(0), kContainers | kKeys),
         set(xs, L(-1), L(0), kContainers | kKeys),
         set(xs, S("x"), L(0), kContainers | kKeys),
         set(o, L(0), L(0), kContainers | kKeys),
         print(S(".")),
         // A string is no container to write: its fault comes before the
         // set is asked about the key.
         set(S("abc"), L(0), S("x"), kContainers | kKeys | tags_named("string")),
         print(S(".")),
         // A receiver outside the receiver's set, though the key set has
         // its tag; a key outside the key's.
         set(xs, L(0), L(5), tags_named("object"), kKeys),
         set(o, S("a"), L(5), kContainers, tags_named("int")),
         print(S(".")),
         // A value outside the value's set: the fallback's, where the
         // same store of another value is the VM's.
         set(xs, L(0), b.nil_literal(p), kContainers, kKeys,
             kAllTags & ~tags_named("nil")),
         set(xs, L(0), L(6), kContainers, kKeys, kAllTags & ~tags_named("nil")),
         print(S(".")),
         print(b.index(xs, L(0), p)), print(b.index(xs, L(1), p)),
         print(b.index(o, S("a"), p)), print(b.index(o, S("b"), p)),
         print(b.index(mp, S("k"), p))},
        p);
    m.funcs[0].body = b.scope(0, 3, body, p);
    check_eq(run_module(m, "set_index: with a fallback"), "",
             "set_index with a fallback: unexpected failure");
    check_eq(joined(),
             ".|fb|fb|fb|fb|.|fb|.|fb|fb|.|fb|.|6|21|2|3|4|",
             "set_index with a fallback");
  }

  // kMissNil: a miss -- a key the map or object lacks -- is the fallback's
  // unless the flag says the VM reads nil and creates the key, and then
  // only for a receiver with no meta; a hit is the VM's either way, and a
  // key holding nil is a hit, which a read of nil cannot tell from a miss.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    // 0 mp: a plain map; 1 mm: a map with a meta; 2 o, 3 om: the same for
    // an object. Each holds a key "n" with nil.
    m.funcs.push_back({"main", 4, 0, NodeId{}, {"mp", "mm", "o", "om"}, {}});
    Func rd{"read", 2, 0, NodeId{}, {"r", "k"}, {}};
    rd.num_params = 2;
    rd.lenient_arity = true;
    rd.body = b.make_return(b.str_literal("F", p), p);
    m.funcs.push_back(rd);
    Func wr{"write", 3, 0, NodeId{}, {"r", "k", "v"}, {}};
    wr.num_params = 3;
    wr.lenient_arity = true;
    wr.body = b.intrinsic(IntrinsicId::Print, {b.str_literal("W", p)}, p);
    m.funcs.push_back(wr);
    const auto V = [&](int32_t i) { return b.varref(VarKind::Local, i, p); };
    const auto S = [&](const char* v) { return b.str_literal(v, p); };
    const auto print = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::Print, {v}, p);
    };
    const auto meta = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::MetaSet, {v, b.array_lit({S("t")}, p)},
                         p);
    };
    const uint32_t kBoth = tags_named({"map", "object", "string"});
    const auto get = [&](int32_t r, const char* k, int32_t flags) {
      return print(b.index(V(r), S(k),
                           {b.make_closure(1, 0, p), kBoth, flags}, p));
    };
    const auto put = [&](int32_t r, const char* k, int32_t flags) {
      return b.set_index(V(r), S(k), b.literal(1, p),
                         {b.make_closure(2, 0, p), kBoth, flags}, p);
    };
    const auto len = [&](int32_t r) {
      return print(b.intrinsic(IntrinsicId::Len, {V(r)}, p));
    };
    const NodeId nil = b.nil_literal(p);
    std::vector<NodeId> body{
        b.assign(VarKind::Local, 0, b.intrinsic(IntrinsicId::MapNew, {}, p), p),
        b.assign(VarKind::Local, 1,
                 meta(b.intrinsic(IntrinsicId::MapNew, {}, p)), p),
        b.assign(VarKind::Local, 2, b.object_lit({}, p), p),
        b.assign(VarKind::Local, 3, meta(b.object_lit({}, p)), p),
        b.set_index(V(0), S("n"), nil, p), b.set_index(V(1), S("n"), nil, p),
        b.set_index(V(2), S("n"), nil, p), b.set_index(V(3), S("n"), nil, p)};
    const auto add = [&](NodeId n) { body.push_back(n); };
    // Reads. Without the flag every miss is the fallback's; with it, a
    // plain receiver reads nil and a receiver with a meta still does not.
    for (int32_t r = 0; r < 4; ++r) add(get(r, "x", 0));
    add(print(S(".")));
    for (int32_t r = 0; r < 4; ++r) add(get(r, "x", kMissNil));
    add(print(S(".")));
    // A key holding nil is found: nil, never the fallback's.
    for (int32_t r = 0; r < 4; ++r) add(get(r, "n", 0));
    add(print(S(".")));
    // Writes. The same split for a new key; an existing key (holding nil)
    // is written by the VM in every case.
    for (int32_t r = 0; r < 4; ++r) add(put(r, "y", 0));
    add(print(S(".")));
    for (int32_t r = 0; r < 4; ++r) add(put(r, "y", kMissNil));
    add(print(S(".")));
    for (int32_t r = 0; r < 4; ++r) add(put(r, "n", 0));
    add(print(S(".")));
    for (int32_t r = 0; r < 4; ++r) add(len(r));
    for (int32_t r = 0; r < 4; ++r) add(get(r, "n", 0));
    m.funcs[0].body = b.scope(0, 4, b.block(body, p), p);
    check_eq(run_module(m, "kMissNil"), "", "kMissNil: unexpected failure");
    check_eq(joined(),
             "F|F|F|F|.|nil|F|nil|F|.|nil|nil|nil|nil|.|"
             "W|W|W|W|.|W|W|.|.|2|1|2|1|1|1|1|1|",
             "kMissNil");
  }

  // kAppend: a store at the array's length appends; one below zero, past
  // the end, with a key that is no int, or into a string is still the
  // fallback's; and without the flag the length itself is.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({"main", 1, 0, NodeId{}, {"xs"}, {}});
    Func wr{"write", 3, 0, NodeId{}, {"r", "k", "v"}, {}};
    wr.num_params = 3;
    wr.lenient_arity = true;
    wr.body = b.intrinsic(IntrinsicId::Print, {b.str_literal("W", p)}, p);
    m.funcs.push_back(wr);
    const NodeId xs = b.varref(VarKind::Local, 0, p);
    const auto put = [&](NodeId k, int32_t flags) {
      return b.set_index(xs, k, b.literal(7, p),
                         {b.make_closure(1, 0, p),
                          tags_named({"array", "int", "double"}), flags},
                         p);
    };
    const auto len = [&] {
      return b.intrinsic(IntrinsicId::Print,
                         {b.intrinsic(IntrinsicId::Len, {xs}, p)}, p);
    };
    m.funcs[0].body = b.scope(
        0, 1,
        b.block({b.assign(VarKind::Local, 0, b.array_lit({b.literal(1, p)}, p),
                          p),
                 put(b.literal(1, p), 0), len(),
                 put(b.literal(1, p), kAppend), len(),
                 put(b.literal(3, p), kAppend), len(),
                 put(b.literal(-1, p), kAppend), len(),
                 put(b.double_literal(2.0, p), kAppend), len(),
                 put(b.literal(0, p), kAppend), len()},
                p),
        p);
    check_eq(run_module(m, "kAppend"), "", "kAppend: unexpected failure");
    check_eq(joined(), "W|1|2|W|2|W|2|W|2|2|", "kAppend");
  }

  // kIntegralKey: a double that holds an integer in int64's range is that
  // int, as a key, when the key set and the array and the map judge it;
  // anything else -- a fraction, a double past int64, NaN -- stays a double
  // and NaN is never the VM's. The fallback is given the key as it was
  // written, and answers its type, so the cases tell the VM's answer from
  // the fallback's.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({"main", 2, 0, NodeId{}, {"xs", "mp"}, {}});
    Func rd{"read", 2, 0, NodeId{}, {"r", "k"}, {}};
    rd.num_params = 2;
    rd.lenient_arity = true;
    rd.body = b.make_return(
        b.intrinsic(IntrinsicId::TypeOf, {b.varref(VarKind::Local, 1, p)}, p),
        p);
    m.funcs.push_back(rd);
    Func wr{"write", 3, 0, NodeId{}, {"r", "k", "v"}, {}};
    wr.num_params = 3;
    wr.lenient_arity = true;
    wr.body = b.intrinsic(
        IntrinsicId::Print,
        {b.intrinsic(IntrinsicId::TypeOf, {b.varref(VarKind::Local, 1, p)}, p)},
        p);
    m.funcs.push_back(wr);
    const NodeId xs = b.varref(VarKind::Local, 0, p);
    const NodeId mp = b.varref(VarKind::Local, 1, p);
    const auto D = [&](double v) { return b.double_literal(v, p); };
    const auto print = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::Print, {v}, p);
    };
    const uint32_t kList = tags_named({"array", "map"});
    const auto get = [&](NodeId r, NodeId k, int32_t flags, uint32_t keys) {
      return print(b.index(r, k,
                           {b.make_closure(1, 0, p), kList, flags, keys}, p));
    };
    const auto put = [&](NodeId r, NodeId k, int32_t flags, uint32_t keys) {
      return b.set_index(r, k, b.literal(9, p),
                         {b.make_closure(2, 0, p), kList, flags, keys}, p);
    };
    const int32_t kI = kIntegralKey | kAppend | kMissNil;
    const int32_t kG = kIntegralKey | kMissNil;  // Index has no kAppend
    const uint32_t kInt = tags_named("int");
    std::vector<NodeId> body{
        b.assign(VarKind::Local, 0,
                 b.array_lit({b.literal(10, p), b.literal(20, p),
                              b.literal(30, p)},
                             p),
                 p),
        b.assign(VarKind::Local, 1, b.intrinsic(IntrinsicId::MapNew, {}, p),
                 p),
        // 1.0 and -0.0 are elements; 2^53 is out of range and the fallback
        // gets it as it was written, as it does 2^63, a fraction and NaN,
        // and 1.0 without the flag.
        get(xs, D(1.0), kG, kInt), get(xs, D(-0.0), kG, kInt),
        get(xs, D(9007199254740992.0), kG, kInt),
        get(xs, D(9223372036854775808.0), kG, kInt), get(xs, D(1.5), kG, kInt),
        get(xs, D(std::nan("")), kG, kInt), get(xs, D(1.0), kMissNil, kInt),
        print(b.literal(0, p)),
        // A store: 3.0 is the length, so it appends under kAppend; 1.0 is
        // in range; 5.0 is past it; NaN and a fraction are the fallback's.
        put(xs, D(3.0), kI, kInt), put(xs, D(1.0), kI, kInt),
        put(xs, D(5.0), kI, kInt), put(xs, D(std::nan("")), kI, kInt),
        put(xs, D(1.5), kI, kInt),
        print(b.intrinsic(IntrinsicId::Len, {xs}, p)), print(b.index(xs, b.literal(1, p), p)),
        print(b.literal(0, p)),
        // A map: 2.0 makes the int key 2, 1.5 a double key, NaN is the
        // fallback's for a read and a write alike.
        put(mp, D(2.0), kI, kInt | tags_named("double")),
        put(mp, D(1.5), kI, kInt | tags_named("double")),
        put(mp, D(std::nan("")), kI, kInt | tags_named("double")),
        get(mp, D(2.0), kG, kInt | tags_named("double")),
        get(mp, D(1.5), kG, kInt | tags_named("double")),
        get(mp, D(std::nan("")), kG, kInt | tags_named("double")),
        print(b.intrinsic(IntrinsicId::Len, {mp}, p)),
        print(b.index(mp, b.literal(2, p), p)),
        print(b.literal(0, p)),
        // A held key is not changed by the Try that normalised it.
        b.at(p).let(D(1.0), [&](auto k) {
          return b.block({get(xs, k(), kG, kInt),
                          print(b.intrinsic(IntrinsicId::TypeOf, {k()}, p))},
                         p);
        })};
    m.funcs[0].body = b.scope(0, 2, b.block(body, p), p);
    check_eq(run_module(m, "kIntegralKey"), "",
             "kIntegralKey: unexpected failure");
    check_eq(joined(),
             "20|10|double|double|double|double|double|0|"
             "double|double|double|4|9|0|"
             "double|9|9|double|2|9|0|"
             "9|double|",
             "kIntegralKey");
  }

  // IntegralKey is kIntegralKey's rule as an intrinsic: the int a double
  // holds when it holds an integer in int64's range, and any other value as
  // it is.
  {
    Module m;
    Builder b(m);
    const auto key = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::IntegralKey, {v}, p);
    };
    const auto show = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::Print,
                         {b.binary(BinOp::Add,
                                   b.intrinsic(IntrinsicId::TypeOf, {v}, p),
                                   b.str_literal(":", p), p)},
                         p);
    };
    const auto D = [&](double v) { return b.double_literal(v, p); };
    const auto both = [&](NodeId v) {
      return std::vector<NodeId>{show(key(v)),
                                 b.intrinsic(IntrinsicId::Print, {key(v)}, p)};
    };
    std::vector<NodeId> body;
    for (const NodeId v :
         {D(1.0), D(-0.0), D(9007199254740992.0), D(1152921504606846976.0),
          D(-9223372036854775808.0), D(9223372036854775808.0), D(1.5),
          D(std::nan("")), D(HUGE_VAL), b.literal(7, p), b.str_literal("s", p),
          b.nil_literal(p)}) {
      for (const NodeId n : both(v)) body.push_back(n);
    }
    m.funcs.push_back({"main", 0, 0, b.block(body, p), {}, {}});
    check_eq(run_module(m, "IntegralKey"), "", "IntegralKey: failure");
    check_eq(joined(),
             "int:|1|int:|0|int:|9007199254740992|int:|1152921504606846976|"
             "int:|-9223372036854775808|double:|9223372036854775808|"
             "double:|1.5|double:|nan|double:|inf|int:|7|string:|s|nil:|nil|",
             "IntegralKey");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "containers: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("containers OK\n");
  return 0;
}
