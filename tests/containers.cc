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

#include "coreir/ir.h"
#include "coreir/value.h"
#include "vm/compiler.h"
#include "vm/exec.h"

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

  if (g_failures != 0) {
    std::fprintf(stderr, "containers: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("containers OK\n");
  return 0;
}
