// The tagged, reference-counted value model: what each tag does, and what
// becomes of the heap ones.
//
// The case most worth having is the *failing* program. The executor used to
// be exception-safe for free, because a Frame held only plain vectors of
// int64; once registers hold owned references that stops being free. What
// coreir/value.h bets is that putting the ownership in the type means the
// ordinary C++ unwind out of vm::run releases everything, with no unwind
// table for a compiler to emit and get wrong. If that bet is wrong, it is
// wrong here.
//
// Every case asserts the live heap-object count is zero afterward, so a
// missing release fails the test rather than waiting for a leak checker.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <new>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "vmlib.h"

// Every allocation through operator new, counted: what 12e asks of a run.
// GCC pairs operator new with the free() inside operator delete and calls
// it a mismatch; here they are one allocator, malloc underneath both.
long g_news = 0;

void* counted_new(std::size_t n) {
  ++g_news;
  if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
  throw std::bad_alloc();
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(std::size_t n) { return counted_new(n); }
void* operator new[](std::size_t n) { return counted_new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

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

// Runs a module built by `build`, returning the host failure message if the
// program failed and an empty string if it ran to completion. Asserts on the
// way out that nothing was left on the heap either way.
std::string run_module(const coreir::Module& m, const std::string& what) {
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
    const vm::Program p = vm::compile(m);
    try {
      vm::run(p, rt);
    } catch (const Failure& e) {
      failure = e.what();
    }
    // Taken before ~Runtime, which would free even a cycle.
    left = rt.live_objects();
  }
  check(left == 0,
        what + ": leaked " + std::to_string(left) + " heap object(s)");
  return failure;
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

  // --- 1. Strings: literal, concatenation, print. -------------------------
  {
    Module m;
    Builder b(m);
    const NodeId greeting =
        b.binary(BinOp::Add,
                 b.binary(BinOp::Add, b.str_literal("hello", p),
                          b.str_literal(" ", p), p),
                 b.str_literal("world", p), p);
    m.funcs.push_back(
        {"main", 0, 0, b.intrinsic(IntrinsicId::Print, {greeting}, p), {}, {}});
    const std::string failed = run_module(m, "concat");
    check_eq(failed, "", "concat: unexpected failure");
    check_eq(joined(), "hello world|", "concat output");
  }

  // --- 2. A string through a variable, overwritten. -----------------------
  // The second store has to release what the slot held, or the first string
  // outlives the program.
  {
    Module m;
    Builder b(m);
    const NodeId body = b.block(
        {b.assign(VarKind::Local, 0, b.str_literal("first", p), p),
         b.intrinsic(IntrinsicId::Print, {b.varref(VarKind::Local, 0, p)}, p),
         b.assign(VarKind::Local, 0,
                  b.binary(BinOp::Add, b.varref(VarKind::Local, 0, p),
                           b.str_literal("+second", p), p),
                  p),
         b.intrinsic(IntrinsicId::Print, {b.varref(VarKind::Local, 0, p)}, p)},
        p);
    m.funcs.push_back({"main", 1, 0, body, {"s"}, {}});
    const std::string failed = run_module(m, "reassign");
    check_eq(failed, "", "reassign: unexpected failure");
    check_eq(joined(), "first|first+second|", "reassign output");
  }

  // --- 3. THE ONE THAT MATTERS: a throw with live strings. ----------------
  // A local holds a string, a register holds a freshly concatenated one, and
  // then a divide by zero fires two frames deep. Nothing runs a destructor
  // for those on purpose; the unwind out of vm::run has to.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({{VarKind::Cell, 0}});  // main -> inner

    const NodeId main_body = b.block(
        {b.assign(VarKind::Cell, 0, b.str_literal("held by main", p), p),
         b.call_value(b.make_closure(1, 0, p), {}, p)},
        p);

    // inner: t = capture[0] + " and by inner"; print(1 / 0)
    const NodeId inner_body = b.block(
        {b.assign(VarKind::Local, 0,
                  b.binary(BinOp::Add, b.varref(VarKind::Capture, 0, p),
                           b.str_literal(" and by inner", p), p),
                  p),
         b.intrinsic(
             IntrinsicId::Print,
             {b.binary(BinOp::Div, b.literal(1, p), b.literal(0, p), p)}, p)},
        p);

    Func main_fn{"main", 0, 0, main_body, {}, {}};
    main_fn.num_cells = 1;
    m.funcs.push_back(main_fn);
    m.funcs.push_back({"inner", 1, 1, inner_body, {"t"}, {"s"}});
    const std::string failed = run_module(m, "throw with live strings");
    check_eq(failed, "divide by zero", "throw: message");
    check_eq(joined(), "", "throw: nothing printed");
  }

  // --- 4. Type errors, which an i64-only IR could not have. ---------------
  {
    Module m;
    Builder b(m);
    m.funcs.push_back({"main",
                       0,
                       0,
                       b.intrinsic(IntrinsicId::Print,
                                   {b.binary(BinOp::Sub,
                                             b.str_literal("a", p),
                                             b.str_literal("b", p), p)},
                                   p),
                       {},
                       {}});
    const std::string failed = run_module(m, "str minus str");
    check_eq(failed, "cannot sub string and string", "type error: message");
  }
  {
    Module m;
    Builder b(m);
    m.funcs.push_back({"main",
                       0,
                       0,
                       b.intrinsic(IntrinsicId::Print,
                                   {b.binary(BinOp::Add, b.literal(1, p),
                                             b.str_literal("x", p), p)},
                                   p),
                       {},
                       {}});
    const std::string failed = run_module(m, "int plus str");
    check_eq(failed, "cannot add int and string", "mixed type error: message");
  }

  // --- 5. Strings compare; truthiness works on them. ----------------------
  {
    Module m;
    Builder b(m);
    const NodeId body = b.block(
        {b.make_if(b.binary(BinOp::Eq, b.str_literal("x", p),
                            b.str_literal("x", p), p),
                   b.intrinsic(IntrinsicId::Print,
                               {b.str_literal("equal", p)}, p),
                   b.intrinsic(IntrinsicId::Print,
                               {b.str_literal("differ", p)}, p),
                   p),
         b.make_if(b.str_literal("truthy", p),
                   b.intrinsic(IntrinsicId::Print, {b.literal(1, p)}, p),
                   NodeId{}, p)},
        p);
    m.funcs.push_back({"main", 0, 0, body, {}, {}});
    const std::string failed = run_module(m, "compare");
    check_eq(failed, "", "compare: unexpected failure");
    check_eq(joined(), "equal|1|", "compare output");
  }

  // --- 6. A string built in a loop: many allocations, all released. -------
  {
    Module m;
    Builder b(m);
    const NodeId s = b.varref(VarKind::Local, 0, p);
    const NodeId i = b.varref(VarKind::Local, 1, p);
    const NodeId body = b.block(
        {b.assign(VarKind::Local, 0, b.str_literal("", p), p),
         b.assign(VarKind::Local, 1, b.literal(0, p), p),
         b.make_while(
             b.binary(BinOp::Lt, i, b.literal(1000, p), p),
             b.block({b.assign(VarKind::Local, 0,
                               b.binary(BinOp::Add, s, b.str_literal("x", p),
                                        p),
                               p),
                      b.assign(VarKind::Local, 1,
                               b.binary(BinOp::Add, i, b.literal(1, p), p), p)},
                     p),
             p),
         b.intrinsic(IntrinsicId::Print, {b.literal(0, p)}, p)},
        p);
    m.funcs.push_back({"main", 2, 0, body, {"s", "i"}, {}});
    const std::string failed = run_module(m, "loop");
    check_eq(failed, "", "loop: unexpected failure");
    check_eq(joined(), "0|", "loop output");
  }

  // --- 7. The scalar tags: nil, bool, double, and how they mix. -----------
  {
    Module m;
    Builder b(m);
    const NodeId body = b.block(
        {b.intrinsic(IntrinsicId::Print, {b.nil_literal(p)}, p),
         b.intrinsic(IntrinsicId::Print, {b.bool_literal(true, p)}, p),
         b.intrinsic(IntrinsicId::Print, {b.bool_literal(false, p)}, p),
         // A comparison is a Bool, not 0/1. PL/0 cannot tell, because its
         // grammar only ever puts one in a condition; a language with a
         // boolean type would have had to undo an integer here.
         b.intrinsic(IntrinsicId::Print,
                     {b.binary(BinOp::Lt, b.literal(1, p), b.literal(2, p), p)},
                     p),
         // Int stays Int; anything else numeric widens to double.
         b.intrinsic(IntrinsicId::Print,
                     {b.binary(BinOp::Div, b.literal(7, p), b.literal(2, p), p)},
                     p),
         b.intrinsic(
             IntrinsicId::Print,
             {b.binary(BinOp::Div, b.literal(7, p), b.double_literal(2.0, p), p)},
             p),
         b.intrinsic(IntrinsicId::Print,
                     {b.binary(BinOp::Add, b.double_literal(0.5, p),
                               b.double_literal(0.25, p), p)},
                     p)},
        p);
    m.funcs.push_back({"main", 0, 0, body, {}, {}});
    const std::string failed = run_module(m, "scalars");
    check_eq(failed, "", "scalars: unexpected failure");
    check_eq(joined(), "nil|true|false|true|3|3.5|0.75|", "scalar output");
  }

  // --- 8. Reading a local before assigning it. ----------------------------
  // Uninit is a tag now rather than a flag beside the value, so this is the
  // case that says the tag never escapes into a program's hands.
  {
    Module m;
    Builder b(m);
    m.funcs.push_back({"main", 1, 0,
                       b.intrinsic(IntrinsicId::Print,
                                   {b.varref(VarKind::Local, 0, p)}, p),
                       {"x"},
                       {}});
    const std::string failed = run_module(m, "uninitialized read");
    check_eq(failed, "uninitialized variable 'x'", "uninit: message");
  }

  // --- 9. Type errors the new tags make possible. -------------------------
  struct Case {
    const char* what;
    BinOp op;
    bool lhs_double;
    const char* want;
  };
  for (const Case& c :
       {Case{"mod on doubles", BinOp::Mod, true, "cannot mod double"},
        Case{"int vs bool", BinOp::Eq, false, "cannot eq int and bool"}}) {
    Module m;
    Builder b(m);
    const NodeId lhs =
        c.lhs_double ? b.double_literal(1.5, p) : b.literal(1, p);
    const NodeId rhs =
        c.lhs_double ? b.double_literal(2.0, p) : b.bool_literal(true, p);
    m.funcs.push_back({"main", 0, 0,
                       b.intrinsic(IntrinsicId::Print,
                                   {b.binary(c.op, lhs, rhs, p)}, p),
                       {},
                       {}});
    const std::string failed = run_module(m, c.what);
    check_eq(failed, c.want, std::string(c.what) + ": message");
  }

  // --- 10. Bit operations: int-only, masked shift counts. -----------------
  {
    Module m;
    Builder b(m);
    auto bin = [&](BinOp op, int64_t l, int64_t r) {
      return b.intrinsic(IntrinsicId::Print,
                         {b.binary(op, b.literal(l, p), b.literal(r, p), p)},
                         p);
    };
    const NodeId body = b.block(
        {bin(BinOp::BitAnd, 6, 3), bin(BinOp::BitOr, 6, 3),
         bin(BinOp::BitXor, 6, 3),
         b.intrinsic(IntrinsicId::Print,
                     {b.unary(UnOp::BitNot, b.literal(7, p), p)}, p),
         // The masked count, on both edges: 64 wraps to 0, -1 wraps to 63,
         // and Shr on a negative int stays arithmetic.
         bin(BinOp::Shl, 1, 2), bin(BinOp::Shl, 1, 64), bin(BinOp::Shl, 1, -1),
         bin(BinOp::Shr, -5, 1), bin(BinOp::Shr, 1, -2)},
        p);
    m.funcs.push_back({"main", 0, 0, body, {}, {}});
    const std::string failed = run_module(m, "bitops");
    check_eq(failed, "", "bitops: unexpected failure");
    check_eq(joined(), "2|7|5|-8|4|1|-9223372036854775808|-3|0|",
             "bitops output");
  }
  {
    Module m;
    Builder b(m);
    m.funcs.push_back({"main", 0, 0,
                       b.intrinsic(IntrinsicId::Print,
                                   {b.binary(BinOp::BitAnd,
                                             b.double_literal(1.5, p),
                                             b.literal(1, p), p)},
                                   p),
                       {},
                       {}});
    check_eq(run_module(m, "bitand on double"),
             "cannot bitand double and int", "bitand on double: message");
  }
  {
    Module m;
    Builder b(m);
    m.funcs.push_back({"main", 0, 0,
                       b.intrinsic(IntrinsicId::Print,
                                   {b.unary(UnOp::BitNot,
                                            b.bool_literal(true, p), p)},
                                   p),
                       {},
                       {}});
    check_eq(run_module(m, "bitnot on bool"), "cannot bitwise-not bool",
             "bitnot on bool: message");
  }

  // --- 11. ToStr: to_display's formatting, as a value. --------------------
  {
    Module m;
    Builder b(m);
    auto pr = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::Print,
                         {b.intrinsic(IntrinsicId::ToStr, {v}, p)}, p);
    };
    const NodeId body = b.block(
        {pr(b.literal(42, p)), pr(b.double_literal(4.0, p)),
         pr(b.double_literal(0.75, p)), pr(b.bool_literal(true, p)),
         pr(b.nil_literal(p)),
         pr(b.binary(BinOp::Add, b.str_literal("a", p),
                     b.str_literal("b", p), p))},
        p);
    m.funcs.push_back({"main", 0, 0, body, {}, {}});
    const std::string failed = run_module(m, "tostr");
    check_eq(failed, "", "tostr: unexpected failure");
    check_eq(joined(), "42|4|0.75|true|nil|ab|", "tostr output");
  }

  // --- 12b. Numeric conversions and the two float operations. ------------
  {
    Module m;
    Builder b(m);
    auto pr1 = [&](IntrinsicId id, NodeId v) {
      return b.intrinsic(IntrinsicId::Print,
                         {b.intrinsic(IntrinsicId::ToStr,
                                      {b.intrinsic(id, {v}, p)}, p)},
                         p);
    };
    auto pr2 = [&](IntrinsicId id, NodeId l, NodeId r) {
      return b.intrinsic(IntrinsicId::Print,
                         {b.intrinsic(IntrinsicId::ToStr,
                                      {b.intrinsic(id, {l, r}, p)}, p)},
                         p);
    };
    const NodeId body = b.block(
        {pr1(IntrinsicId::ToInt, b.double_literal(-2.9, p)),
         pr1(IntrinsicId::ToDouble, b.literal(3, p)),
         pr2(IntrinsicId::FMod, b.double_literal(5.5, p), b.literal(2, p)),
         pr2(IntrinsicId::Pow, b.double_literal(2.0, p),
             b.double_literal(0.5, p))},
        p);
    m.funcs.push_back({"main", 0, 0, body, {}, {}});
    const std::string failed = run_module(m, "numerics");
    check_eq(failed, "", "numerics: unexpected failure");
    check_eq(joined(), "-2|3|1.5|1.4142135623730951|",
             "numerics output");
  }
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 0, 0,
         b.intrinsic(IntrinsicId::Print,
                     {b.intrinsic(IntrinsicId::ToInt,
                                  {b.double_literal(1e300, p)}, p)},
                     p),
         {},
         {}});
    check_eq(run_module(m, "toint range"), "double value out of int range",
             "toint range: message");
  }

  // --- 12. TypeOf: the tag, in type_name's vocabulary. --------------------
  {
    Module m;
    Builder b(m);
    auto pr = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::Print,
                         {b.intrinsic(IntrinsicId::TypeOf, {v}, p)}, p);
    };
    const NodeId body = b.block(
        {pr(b.literal(1, p)), pr(b.double_literal(1.5, p)),
         pr(b.str_literal("s", p)), pr(b.bool_literal(false, p)),
         pr(b.nil_literal(p)), pr(b.array_lit({}, p))},
        p);
    m.funcs.push_back({"main", 0, 0, body, {}, {}});
    const std::string failed = run_module(m, "typeof");
    check_eq(failed, "", "typeof: unexpected failure");
    check_eq(joined(), "int|double|string|bool|nil|array|", "typeof output");
  }

  // --- 12c. TypeOf compared straight against a string literal, or against
  // another TypeOf, is a test of the tags: the answers the string
  // comparison gives, either way round, under Eq and Ne, "function" for a
  // closure, false for a name type_name never answers -- with the operands
  // still evaluated, left first, and no string built on either side.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    auto ty = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::TypeOf, {v}, p);
    };
    auto print = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::Print, {v}, p);
    };
    auto test = [&](BinOp op, NodeId l, NodeId r) {
      return print(b.binary(op, l, r, p));
    };
    // The value v, once `n` has been printed.
    auto after = [&](int64_t n, NodeId v) {
      return b.block({print(b.literal(n, p)), v}, p);
    };
    const NodeId body = b.block(
        {test(BinOp::Eq, ty(after(7, b.literal(1, p))),
              b.str_literal("int", p)),
         test(BinOp::Eq, b.str_literal("int", p), ty(b.double_literal(1.5, p))),
         test(BinOp::Ne, ty(b.nil_literal(p)), b.str_literal("nil", p)),
         test(BinOp::Ne, b.str_literal("nil", p), ty(b.array_lit({}, p))),
         test(BinOp::Eq, ty(b.make_closure(1, 0, p)),
              b.str_literal("function", p)),
         test(BinOp::Eq, ty(b.literal(1, p)), b.str_literal("integer", p)),
         test(BinOp::Eq, ty(after(8, b.literal(1, p))),
              ty(after(9, b.literal(2, p)))),
         test(BinOp::Eq, ty(b.literal(1, p)), ty(b.double_literal(1.0, p))),
         test(BinOp::Ne, ty(b.bool_literal(true, p)), ty(b.array_lit({}, p))),
         test(BinOp::Ne, ty(b.nil_literal(p)), ty(b.nil_literal(p)))},
        p);
    m.funcs.push_back({"main", 0, 0, body, {}, {}});
    m.funcs.push_back({"f", 0, 0, b.nil_literal(p), {}, {}});
    const std::string failed = run_module(m, "type test");
    check_eq(failed, "", "type test: unexpected failure");
    check_eq(joined(),
             "7|true|false|false|true|true|false|8|9|true|false|true|false|",
             "type test output");
    const std::string dump = vm::to_string(vm::compile(m));
    for (const char* want :
         {"\ttypein r0, r0, {int}\t", "\ttypein r0, r0, not {nil}\t",
          "\ttypein r0, r0, {function}\t", "\ttypein r0, r0, {}\t",
          "\tsametype r0, r0, r1\t", "\tnot r0, r0\t"}) {
      check(dump.find(want) != std::string::npos,
            std::string("type test dump lacks '") + want + "'");
    }
    check(dump.find("typeof") == std::string::npos &&
              dump.find("loadconst r0, \"") == std::string::npos,
          "a type test builds no string");
    // A host function's tag is not a closure's, and the two still answer
    // the same name -- which is what SameType compares.
    check(same_type_name(ValueTag::Func, ValueTag::Native),
          "a closure and a host function are the same type");
    check(!same_type_name(ValueTag::Func, ValueTag::Object),
          "a closure and an object are not");
  }

  // --- 12d. An operator with a fallback (Tag::Unary): the operator's answer
  // when every operand's tag is in its set, it does not trap and -- exact
  // -- an int result does not wrap; the fallback, called with the
  // operands, otherwise. The fallback here prints "f" and what it was
  // handed, and answers -1. ------------------------------------------------
  {
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
    const uint32_t kInt = tags_named("int");
    const uint32_t kDbl = tags_named("double");
    const uint32_t kStr = tags_named("string");
    // The module: main prints what `make` builds, over the fallback #1.
    const auto run = [&](const std::function<NodeId(Builder&)>& make,
                         const std::string& what) {
      Module m;
      Builder b(m);
      m.capture_maps.push_back({});
      const auto print = [&](NodeId v) {
        return b.intrinsic(IntrinsicId::Print, {v}, p);
      };
      m.funcs.push_back({"main", 0, 0, make(b), {}, {}});
      Func fb{"fb", 2, 0,
              b.block({print(b.str_literal("f", p)),
                       print(b.varref(VarKind::Local, 0, p)),
                       print(b.varref(VarKind::Local, 1, p)),
                       b.make_return(b.literal(-1, p), p)},
                      p),
              {"x", "y"}, {}};
      fb.num_params = 2;
      fb.lenient_arity = true;
      m.funcs.push_back(fb);
      check_eq(run_module(m, what), "", what + ": unexpected failure");
      return joined();
    };
    const auto print = [&](Builder& b, NodeId v) {
      return b.intrinsic(IntrinsicId::Print, {v}, p);
    };
    const auto fb = [&](Builder& b, uint32_t tags, bool exact = false) {
      return Fallback{b.make_closure(1, 0, p), tags, exact};
    };
    struct Case {
      std::function<NodeId(Builder&)> make;
      const char* want;
      const char* what;
    };
    const auto bin = [&](BinOp op, std::function<NodeId(Builder&)> l,
                         std::function<NodeId(Builder&)> r, uint32_t tags,
                         bool exact = false) {
      return [=, &print, &fb](Builder& b) {
        return print(b, b.binary(op, l(b), r(b), fb(b, tags, exact), p));
      };
    };
    const auto un = [&](UnOp op, std::function<NodeId(Builder&)> x,
                        uint32_t tags, bool exact = false) {
      return [=, &print, &fb](Builder& b) {
        return print(b, b.unary(op, x(b), fb(b, tags, exact), p));
      };
    };
    const auto I = [&](int64_t v) {
      return [=](Builder& b) { return b.literal(v, p); };
    };
    const auto D = [&](double v) {
      return [=](Builder& b) { return b.double_literal(v, p); };
    };
    const auto S = [&](const char* v) {
      return [=](Builder& b) { return b.str_literal(v, p); };
    };
    const Case cases[] = {
        {bin(BinOp::Add, I(1), I(2), kInt), "3|", "ints in the set"},
        {bin(BinOp::Add, I(1), D(2.5), kInt), "f|1|2.5|-1|",
         "a double outside it"},
        {bin(BinOp::Add, I(1), D(2.5), kInt | kDbl), "3.5|",
         "a double inside it"},
        {bin(BinOp::Add, I(kMax), I(1), kInt), "-9223372036854775808|",
         "an add that wraps, not exact"},
        {bin(BinOp::Add, I(kMax), I(1), kInt, true),
         "f|9223372036854775807|1|-1|", "an exact add that would wrap"},
        {bin(BinOp::Add, I(kMax - 1), I(1), kInt, true),
         "9223372036854775807|", "an exact add that fits"},
        {bin(BinOp::Sub, I(kMin), I(1), kInt, true),
         "f|-9223372036854775808|1|-1|", "an exact sub that would wrap"},
        {bin(BinOp::Sub, I(-1), I(kMax), kInt, true),
         "-9223372036854775808|", "an exact sub that fits"},
        {bin(BinOp::Sub, I(kMax - 1), I(-1), kInt, true),
         "9223372036854775807|", "an exact sub of a negative that fits"},
        {bin(BinOp::Mul, I(int64_t{1} << 32), I(int64_t{1} << 31), kInt,
             true),
         "f|4294967296|2147483648|-1|", "an exact mul that would wrap"},
        {bin(BinOp::Mul, I(-1), I(kMin), kInt, true),
         "f|-1|-9223372036854775808|-1|", "-1 times the least int"},
        {bin(BinOp::Mul, I(int64_t{1} << 32), I(-(int64_t{1} << 31)), kInt,
             true),
         "-9223372036854775808|", "an exact mul that just fits"},
        {bin(BinOp::Div, I(1), I(0), kInt), "f|1|0|-1|",
         "a trap is the fallback's"},
        {bin(BinOp::Eq, I(1), S("a"), kInt | kStr), "f|1|a|-1|",
         "so is Eq refusing two types"},
        {bin(BinOp::Lt, S("a"), S("b"), kStr), "true|", "two strings"},
        {un(UnOp::Neg, I(kMin), kInt, true), "f|-9223372036854775808|nil|-1|",
         "an exact Neg that would wrap"},
        {un(UnOp::Neg, I(kMin), kInt), "-9223372036854775808|",
         "a Neg that wraps"},
        {un(UnOp::Neg, I(5), kInt, true), "-5|", "an exact Neg that fits"},
        {un(UnOp::Neg, S("s"), kStr), "f|s|nil|-1|", "a Neg that traps"},
        // Index, with the receiver and the key as its operands: an
        // element in range is the VM's, and an index out of it, which
        // Index traps on, is the fallback's -- as are a receiver or a key
        // outside the set.
        {[&](Builder& b) {
           const auto at = [&](NodeId r, NodeId k, uint32_t tags) {
             return print(b, b.index(r, k, fb(b, tags), p));
           };
           const auto list = [&] {
             return b.array_lit({b.literal(10, p), b.literal(20, p)}, p);
           };
           const uint32_t kArrInt = tags_named({"array", "int"});
           return b.block(
               {at(list(), b.literal(1, p), kArrInt),
                at(list(), b.literal(2, p), kArrInt),
                at(list(), b.literal(-1, p), kArrInt),
                at(b.str_literal("ab", p), b.literal(1, p), kArrInt),
                at(b.str_literal("ab", p), b.literal(1, p),
                   tags_named({"string", "int"})),
                at(list(), b.double_literal(1.0, p), kArrInt)},
               p);
         },
         "20|f|<array>|2|-1|f|<array>|-1|-1|f|ab|1|-1|b|f|<array>|1|-1|",
         "an Index"},
        // The operands run first, each once; the fallback only when called,
        // after them; and a statement's value goes nowhere.
        {[&](Builder& b) {
           const auto seen = [&](const char* s, NodeId v) {
             return b.block({print(b, b.str_literal(s, p)), v}, p);
           };
           const auto sum = [&](uint32_t tags) {
             return b.binary(BinOp::Add, seen("l", b.literal(1, p)),
                             seen("r", b.double_literal(2.5, p)),
                             {seen("callee", b.make_closure(1, 0, p)), tags},
                             p);
           };
           return b.block({print(b, sum(kInt | kDbl)), print(b, sum(kInt)),
                           sum(kDbl), print(b, b.str_literal("end", p))},
                          p);
         },
         "l|r|3.5|l|r|callee|f|1|2.5|-1|l|r|callee|f|1|2.5|end|",
         "what runs, and when"},
    };
    for (const Case& c : cases) {
      check_eq(run(c.make, c.what), c.want, std::string("fallback: ") + c.what);
    }

    // Truth: the truthiness a condition tests, as a bool -- and, with a
    // fallback, a language's own, the VM's answer taken only on the tags
    // in the set. As a condition too, where a bool operand needs no call
    // whatever the rest of the set is.
    const uint32_t kNilBool = tags_named({"nil", "bool"});
    const auto local = [&](Builder& b) {
      return b.varref(VarKind::Local, 0, p);
    };
    const auto cond = [&](Builder& b, NodeId c) {
      return b.make_if(c, print(b, b.str_literal("T", p)),
                       print(b, b.str_literal("F", p)), p);
    };
    const struct {
      std::function<NodeId(Builder&)> make;
      const char* want;
      const char* what;
    } truths[] = {
        {[&](Builder& b) {
           return b.block(
               {print(b, b.unary(UnOp::Truth, b.literal(0, p), p)),
                print(b, b.unary(UnOp::Truth, b.literal(2, p), p)),
                print(b, b.unary(UnOp::Truth, b.str_literal("", p), p)),
                print(b, b.unary(UnOp::Truth, b.nil_literal(p), p)),
                print(b, b.unary(UnOp::Truth, b.double_literal(0.0, p), p))},
               p);
         },
         "false|true|true|false|false|", "the VM's truth, as a bool"},
        {un(UnOp::Truth, I(0), kNilBool), "f|0|nil|-1|",
         "an int is the language's"},
        {un(UnOp::Truth, [&](Builder& b) { return b.nil_literal(p); },
            kNilBool),
         "false|", "nil is the VM's"},
        {[&](Builder& b) {
           return b.block(
               {b.assign(VarKind::Local, 0, b.literal(0, p), p),
                cond(b, b.unary(UnOp::Truth, local(b), fb(b, kNilBool), p)),
                cond(b, b.unary(UnOp::Not, local(b), fb(b, kNilBool), p)),
                cond(b, b.unary(UnOp::Truth,
                                b.binary(BinOp::Lt, local(b),
                                         b.literal(1, p), p),
                                fb(b, kNilBool), p))},
               p);
         },
         "f|0|nil|T|f|0|nil|T|T|", "as a condition"},
        // A comparison with a fallback answers whatever the fallback does
        // -- #2's 0 here -- so it is not a bool to skip the truth test on.
        {[&](Builder& b) {
           return cond(
               b, b.unary(UnOp::Truth,
                          b.binary(BinOp::Lt, b.str_literal("a", p),
                                   b.literal(1, p),
                                   {b.make_closure(2, 0, p), kInt}, p),
                          fb(b, kNilBool), p));
         },
         "f|0|nil|T|", "over a comparison with a fallback"},
        // The same comparison where it answers for itself: its bool
        // decides, with no call of either fallback, under Truth and Not,
        // as a condition that jumps on false and on true -- and where it
        // does not, the test's own fallback takes what the comparison's
        // answered.
        {[&](Builder& b) {
           const auto lt = [&](NodeId x) {
             return b.binary(BinOp::Lt, x, b.literal(1, p),
                             {b.make_closure(2, 0, p), kInt}, p);
           };
           const auto test = [&](UnOp op, NodeId x) {
             return b.unary(op, lt(x), fb(b, kNilBool), p);
           };
           const auto either = [&](NodeId c) {
             return b.make_if(c, b.bool_literal(true, p),
                              b.bool_literal(false, p), p);
           };
           std::vector<NodeId> out;
           for (const NodeId x : {b.literal(0, p), b.literal(5, p),
                                  b.str_literal("a", p)}) {
             out.push_back(cond(b, test(UnOp::Truth, x)));
             out.push_back(cond(b, test(UnOp::Not, x)));
             out.push_back(cond(b, either(test(UnOp::Truth, x))));
           }
           // The comparison as the last child of a Block that stores its
           // operand first, as Python's chained comparisons do.
           out.push_back(cond(
               b, b.unary(UnOp::Truth,
                          b.block({b.assign(VarKind::Local, 0,
                                            b.literal(7, p), p),
                                   lt(local(b))},
                                  p),
                          fb(b, kNilBool), p)));
           return b.block(out, p);
         },
         "T|F|T|F|T|F|f|0|nil|T|f|0|nil|T|f|0|nil|T|F|",
         "over a comparison with a fallback that answers"},
    };
    for (const auto& c : truths) {
      Module m;
      Builder b(m);
      m.capture_maps.push_back({});
      m.funcs.push_back({"main", 1, 0, c.make(b), {"x"}, {}});
      Func f{"fb", 2, 0,
             b.block({print(b, b.str_literal("f", p)),
                      print(b, b.varref(VarKind::Local, 0, p)),
                      print(b, b.varref(VarKind::Local, 1, p)),
                      b.make_return(b.literal(-1, p), p)},
                     p),
             {"x", "y"}, {}};
      f.num_params = 2;
      f.lenient_arity = true;
      m.funcs.push_back(f);
      Func zero{"zero", 2, 0, b.literal(0, p), {"x", "y"}, {}};
      zero.num_params = 2;
      m.funcs.push_back(zero);
      check_eq(run_module(m, c.what), "", std::string(c.what) + ": failed");
      check_eq(joined(), c.want, std::string("truth: ") + c.what);
    }

    // A comparison handed to a language's truthiness is the comparison
    // alone: a bool, which a set holding bool answers without a call, as
    // a condition and as a value -- and only then.
    {
      Module m;
      Builder b(m);
      m.capture_maps.push_back({});
      const auto lt = [&] {
        return b.binary(BinOp::Lt, local(b), b.literal(1, p), p);
      };
      const auto body = [&](NodeId stmt) {
        m.funcs.clear();
        m.funcs.push_back({"main", 1, 0, b.block({stmt, b.nil_literal(p)}, p),
                           {"x"}, {}});
        m.funcs.push_back({"fb", 0, 0, b.literal(0, p), {}, {}});
        return vm::to_string(vm::compile(m));
      };
      const std::string branch =
          body(cond(b, b.unary(UnOp::Truth, lt(), fb(b, kNilBool), p)));
      check(branch.find("\ttryunary") == std::string::npos &&
                branch.find("\ttruth") == std::string::npos &&
                branch.find("\tlt r") != std::string::npos,
            "truth: a comparison as a condition is the comparison");
      const std::string value =
          body(print(b, b.unary(UnOp::Not, lt(), fb(b, kNilBool), p)));
      check(value.find("\ttryunary") == std::string::npos &&
                value.find("\tnot r") != std::string::npos,
            "truth: a Not of a comparison is a Not");
      const std::string other = body(
          cond(b, b.unary(UnOp::Truth, lt(), fb(b, tags_named("nil")), p)));
      check(other.find("\ttryunary") != std::string::npos,
            "truth: without bool in the set, the fallback stays");
      // Over a comparison with a fallback, the comparison's Try exits
      // straight to a jump on its own register: the test's Try is on the
      // fallback's path alone.
      // So too a comparison a Block ends in.
      const std::string tried = body(cond(
          b, b.unary(UnOp::Truth,
                     b.block({b.assign(VarKind::Local, 0, b.literal(7, p), p),
                              b.binary(BinOp::Lt, local(b), b.literal(1, p),
                                       fb(b, kInt), p)},
                             p),
                     fb(b, kNilBool), p)));
      const size_t at = tried.find("\ttrybinary r");
      bool straight = false;
      if (at != std::string::npos) {
        const std::string reg = tried.substr(
            at + 11, tried.find(',', at) - at - 11);
        const size_t exit = tried.find(", ", tried.find("}, ", at)) + 2;
        const std::string pc =
            tried.substr(exit, tried.find('\t', exit) - exit);
        // The instruction at the exit: `\n  <pc>\tjumpif... <reg>,`.
        const std::string head = "\n  " + pc + "\t";
        const size_t line = tried.find(head);
        const auto is = [&](const std::string& insn) {
          return tried.compare(line + head.size(), insn.size(), insn) == 0;
        };
        straight = line != std::string::npos &&
                   (is("jumpiffalse " + reg + ",") ||
                    is("jumpiftrue " + reg + ","));
      }
      check(straight, "truth: a comparison's own answer goes to the jump");
    }

    // The code: the operands in the registers after the result's, the
    // Try, and the fallback's call over those same registers, which the
    // Try jumps past when it answers.
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back(
        {"main", 2, 0,
         print(b, b.binary(BinOp::Add, b.varref(VarKind::Local, 0, p),
                           b.varref(VarKind::Local, 1, p), fb(b, kInt, true),
                           p)),
         {"a", "b"}, {}});
    m.funcs.push_back({"fb", 0, 0, b.literal(0, p), {}, {}});
    check(to_string(m).find("add or-call {int} exact") != std::string::npos,
          "fallback: the IR dump shows the operator's set");
    {
      // An Index's Try, and its fallback's call over the same run.
      Module mi;
      Builder bi(mi);
      mi.capture_maps.push_back({});
      mi.funcs.push_back(
          {"main", 2, 0,
           print(bi, bi.index(bi.varref(VarKind::Local, 0, p),
                              bi.varref(VarKind::Local, 1, p),
                              {bi.make_closure(1, 0, p),
                               tags_named({"array", "int"})},
                              p)),
           {"a", "i"}, {}});
      mi.funcs.push_back({"fb", 0, 0, bi.literal(0, p), {}, {}});
      check(to_string(mi).find("index or-call {int,array}") !=
                std::string::npos,
            "fallback: the IR dump shows an Index's set");
      const std::string d = vm::to_string(vm::compile(mi));
      check(d.find("\ttryindex r0, index r1, r2, {int,array}, 5\t") !=
                    std::string::npos &&
                d.find("\tcallvalue r0, r3, args r1..3\t") !=
                    std::string::npos,
            "fallback: an Index's Try and its call");
    }
    const std::string dump = vm::to_string(vm::compile(m));
    const size_t at = dump.find("\ttrybinary r0, add r1, r2, {int} exact, ");
    const size_t call = dump.find("\tcallvalue r0, r3, args r1..3\t");
    check(at != std::string::npos && call != std::string::npos && at < call,
          "fallback: the Try, then the call over its operands");
    if (at != std::string::npos && call != std::string::npos) {
      // `<pc>\tcallvalue`: the Try's target is the pc after it.
      const size_t line = dump.rfind('\n', call) + 1;
      const int call_pc = std::stoi(dump.substr(line + 2));
      const size_t target = dump.find(", ", at + 30) + 2;
      check_eq(dump.substr(target, dump.find('\t', target) - target),
               std::to_string(call_pc + 1),
               "fallback: the Try jumps past the call");
    }
  }

  // --- 12e. A fallback is reached without the diagnostic of the trap it
  // stands in for being built: a loop whose every operation traps and so
  // falls back -- an Eq Eq refuses, an index out of range, a Neg of a
  // string, each message longer than a short string holds -- allocates
  // what the same loop does when every operand's tag is outside the set,
  // which falls back without asking the operation at all. The calls of the
  // fallback are the same in both. -------------------------------------
  {
    const auto allocations = [&](bool trap) {
      Module m;
      Builder b(m);
      m.capture_maps.push_back({});
      const auto L = [&](int32_t i) { return b.varref(VarKind::Local, i, p); };
      const auto set = [&](int32_t i, NodeId v) {
        return b.assign(VarKind::Local, i, v, p);
      };
      // The operands' own set when the operation is to be asked; nil's,
      // which none of them is but Eq's left, when it is not.
      const auto fb = [&](std::initializer_list<std::string_view> tags) {
        return Fallback{b.make_closure(1, 0, p),
                        trap ? tags_named(tags) : tags_named("bool")};
      };
      const NodeId body = b.block(
          {set(1, b.binary(BinOp::Eq, b.nil_literal(p), b.literal(5, p),
                           fb({"nil", "int"}), p)),
           set(1, b.index(L(2), b.literal(2, p), fb({"array", "int"}), p)),
           set(1, b.unary(UnOp::Neg, L(3), fb({"string"}), p)),
           set(0, b.binary(BinOp::Add, L(0), b.literal(1, p), p))},
          p);
      m.funcs.push_back(
          {"main", 4, 0,
           b.block({set(0, b.literal(0, p)),
                    set(2, b.array_lit({b.literal(1, p)}, p)),
                    set(3, b.str_literal("s", p)),
                    b.make_while(
                        b.binary(BinOp::Lt, L(0), b.literal(100, p), p), body,
                        p)},
                   p),
           {"i", "x", "a", "s"}, {}});
      Func f{"fb", 2, 0, b.bool_literal(false, p), {"x", "y"}, {}};
      f.num_params = 2;
      f.lenient_arity = true;
      m.funcs.push_back(f);
      check_eq(verify(m).value_or(""), "", "fallback allocations: verify");
      const vm::Program prog = vm::compile(m);
      Runtime rt;
      const long before = g_news;
      vm::run(prog, rt);
      return g_news - before;
    };
    check_eq(std::to_string(allocations(true)),
             std::to_string(allocations(false)),
             "a fallback builds no diagnostic");
  }

  // --- 13. Same: identity, where Eq refuses to answer. --------------------
  // a = [1]; b = [1]; c = a; print(same(a, b)); print(same(a, c));
  // print(same(1, 1)); print(same(1, 1.0)); print(same(nil, nil))
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    auto same = [&](NodeId l, NodeId r) {
      return b.intrinsic(IntrinsicId::Same, {l, r}, p);
    };
    auto print = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::Print, {v}, p);
    };
    auto local = [&](int32_t i) { return b.varref(VarKind::Local, i, p); };
    m.funcs.push_back(
        {"main", 3, 0,
         b.block({b.assign(VarKind::Local, 0,
                           b.array_lit({b.literal(1, p)}, p), p),
                  b.assign(VarKind::Local, 1,
                           b.array_lit({b.literal(1, p)}, p), p),
                  b.assign(VarKind::Local, 2, local(0), p),
                  print(same(local(0), local(1))),
                  print(same(local(0), local(2))),
                  print(same(b.literal(1, p), b.literal(1, p))),
                  print(same(b.literal(1, p), b.double_literal(1.0, p))),
                  print(same(b.nil_literal(p), b.nil_literal(p)))},
                 p),
         {"a", "b", "c"},
         {}});
    const std::string failed = run_module(m, "same");
    check_eq(failed, "", "same: unexpected failure");
    check_eq(joined(), "false|true|true|false|true|", "same output");
  }

  // --- The Builder's constant pool: one entry per distinct constant, found
  // through an index that is a cache, not the truth -- a pool cut back or
  // edited by hand costs a duplicate at worst, never a wrong constant. ----
  {
    Module m;
    Builder b(m);
    const SrcPos p{1, 1};
    const int32_t a = m.nodes[b.literal(7, p).v].a;
    check(m.nodes[b.literal(7, p).v].a == a, "int constants share an entry");
    const int32_t s1 = m.nodes[b.str_literal("hi", p).v].a;
    check(m.nodes[b.str_literal("hi", p).v].a == s1,
          "string constants share an entry");
    check(m.consts.size() == 2, "two distinct constants");
    check(m.nodes[b.bool_literal(true, p).v].a != a, "a bool is not the int 1");

    // Cut the pool back to nothing: the stale index entries name entries
    // that are gone, and must be rebuilt rather than trusted.
    m.consts.clear();
    m.str_consts.clear();
    const NodeId again = b.str_literal("hi", p);
    const NodeId num = b.literal(7, p);
    check_eq(m.str_const(again), "hi", "a string after the pool was cleared");
    check_eq(std::to_string(m.int_const(num)), "7",
             "an int after the pool was cleared");

    // Overwrite an entry in place: the index still says "hi" lives there.
    m.str_consts[static_cast<size_t>(m.consts[static_cast<size_t>(
        m.nodes[again.v].a)].bits)] = "other";
    check_eq(m.str_const(b.str_literal("hi", p)), "hi",
             "a string after its entry was overwritten");
  }

  // --- The bytecode dump shows each instruction's own operands: a unary op,
  // Move, LoadNil, NewObject and Yield used to print three registers
  // whatever they read, Ret none, and an intrinsic as many as an Insn has
  // rather than its arity. ------------------------------------------------
  {
    Module m;
    Builder b(m);
    const SrcPos p{1, 1};
    m.capture_maps.push_back({});
    const auto x = [&] { return b.varref(VarKind::Local, 0, p); };
    const auto a = [&] { return b.varref(VarKind::Local, 1, p); };
    const auto print = [&](NodeId v) {
      return b.intrinsic(IntrinsicId::Print, {v}, p);
    };
    m.funcs.push_back(
        {"main", 2, 0,
         b.block({b.assign(VarKind::Local, 0, b.literal(5, p), p),
                  print(b.unary(UnOp::BitNot, x(), p)),
                  print(b.unary(UnOp::WrapI8, x(), p)),
                  b.assign(VarKind::Local, 1, b.array_lit({}, p), p),
                  b.intrinsic(IntrinsicId::ArrayPush, {a(), x()}, p),
                  print(b.intrinsic(IntrinsicId::Len, {a()}, p)),
                  print(b.object_lit({}, p))},
                 p),
         {"x", "a"}, {}});
    Func gen{"gen", 0, 0, b.make_yield(b.literal(1, p), p), {}, {}};
    gen.is_generator = true;
    m.funcs.push_back(gen);
    const std::string dump = vm::to_string(vm::compile(m));
    for (const char* want :
         {"\tbitnot r0, r0\t", "\twrapi8 r0, r0\t", "\tloadnil r0\t",
          "\tret r0\t", "\tlen r0, r0\t", "\tarraypush r0, r1\t",
          "\tnewobject r0\t", "\tyield r0, r0\t"}) {
      check(dump.find(want) != std::string::npos,
            std::string("bytecode dump lacks '") + want + "'");
    }
  }

  // --- A NaN prints as "nan" whatever its sign bit: 0.0/0.0 sets it on x86
  // and not on ARM, so "-nan" made the same program print differently. --
  {
    const double pos_nan = std::nan("");
    check_eq(coreir::to_display(coreir::Value::make_double(pos_nan)), "nan",
             "positive NaN displays as nan");
    check_eq(coreir::to_display(coreir::Value::make_double(-pos_nan)), "nan",
             "negative NaN displays as nan");
    check_eq(coreir::to_display(coreir::Value::make_double(-INFINITY)), "-inf",
             "the sign of an infinity stays");
  }

  // --- Map keys are strict (README, "Maps"): 1 and 1.0 differ, 0.0 and -0.0
  // differ, and every NaN is one key whatever its sign bit. ----------------
  {
    using coreir::Value;
    coreir::Runtime rt;
    coreir::Runtime::Scope scope(rt);
    Value mv = Value::make_map();
    coreir::MapObj* map = mv.as_map();
    const double pos_nan = std::nan("");
    const double neg_nan = -pos_nan;
    map->set(Value::make_double(neg_nan), Value::make_int(1));
    map->set(Value::make_double(pos_nan), Value::make_int(2));
    check(map->live == 1, "every NaN is one key");
    check(map->find(Value::make_double(neg_nan)) != nullptr &&
              map->find(Value::make_double(neg_nan))->as_int() == 2,
          "either NaN finds the entry");

    map->set(Value::make_int(1), Value::make_int(10));
    map->set(Value::make_double(1.0), Value::make_int(11));
    map->set(Value::make_double(0.0), Value::make_int(12));
    map->set(Value::make_double(-0.0), Value::make_int(13));
    check(map->live == 5, "1, 1.0, 0.0, -0.0 and NaN are five keys");

    // The index is rebuilt when the entries are compacted; NaN must still
    // be found through the canonical key afterwards.
    for (int64_t i = 100; i < 120; ++i) map->set(Value::make_int(i), Value());
    for (int64_t i = 100; i < 120; ++i) map->remove(Value::make_int(i));
    check(map->find(Value::make_double(neg_nan)) != nullptr,
          "NaN survives compaction");
  }

  // --- The IR and bytecode dumps print a constant by value: a string, a
  // double, a bool and nil used to print as their pool index or bit pattern.
  {
    Module m;
    Builder b(m);
    const SrcPos p{1, 1};
    m.capture_maps.push_back({});
    m.funcs.push_back(
        {"main", 0, 0,
         b.block({b.intrinsic(IntrinsicId::Print, {b.literal(7, p)}, p),
                  b.intrinsic(IntrinsicId::Print, {b.str_literal("hi", p)}, p),
                  b.intrinsic(IntrinsicId::Print,
                              {b.double_literal(1.5, p)}, p),
                  b.intrinsic(IntrinsicId::Print,
                              {b.bool_literal(true, p)}, p),
                  b.intrinsic(IntrinsicId::Print, {b.nil_literal(p)}, p)},
                 p),
         {}, {}});
    const std::string ir = to_string(m);
    for (const char* want :
         {"literal 7 ", "literal \"hi\" ", "literal 1.5 ", "literal true ",
          "literal nil "}) {
      check(ir.find(want) != std::string::npos,
            std::string("IR dump lacks '") + want + "'");
    }
    const std::string bc = vm::to_string(vm::compile(m));
    for (const char* want :
         {"r0, 7\t", "r0, \"hi\"\t", "r0, 1.5\t", "r0, true\t"}) {
      check(bc.find(want) != std::string::npos,
            std::string("bytecode dump lacks '") + want + "'");
    }
  }

  // --- An int against a double compares the numbers, not the int rounded to
  // a double: past 2^53 the two disagree, and Eq stopped being transitive
  // (2^53 and 2^53+1 were both equal to 2^53.0, and not to each other).
  // Every row is checked in both operand orders. ----------------------------
  {
    using coreir::BinOp;
    using coreir::Value;
    const auto mirror = [](BinOp op) {
      switch (op) {
        case BinOp::Lt: return BinOp::Gt;
        case BinOp::Le: return BinOp::Ge;
        case BinOp::Gt: return BinOp::Lt;
        case BinOp::Ge: return BinOp::Le;
        default: return op;  // Eq, Ne
      }
    };
    const auto holds = [&](BinOp op, int64_t i, double d, bool want,
                           const char* what) {
      const Value vi = Value::make_int(i);
      const Value vd = Value::make_double(d);
      const Value a = coreir::apply_binop(op, vi, vd);
      const Value b = coreir::apply_binop(mirror(op), vd, vi);
      check(a.is_bool() && a.as_bool() == want, std::string(what));
      check(b.is_bool() && b.as_bool() == want,
            std::string(what) + " (operands swapped)");
    };
    const int64_t p53 = int64_t{1} << 53;
    const double d53 = static_cast<double>(p53);

    holds(BinOp::Eq, p53, d53, true, "2^53 == 2^53.0");
    holds(BinOp::Eq, p53 + 1, d53, false, "2^53+1 != 2^53.0");
    holds(BinOp::Gt, p53 + 1, d53, true, "2^53+1 > 2^53.0");
    check(!coreir::apply_binop(BinOp::Eq, Value::make_int(p53),
                               Value::make_int(p53 + 1)).as_bool(),
          "2^53 != 2^53+1");

    holds(BinOp::Lt, INT64_MAX, coreir::kTwo63, true, "INT64_MAX < 2^63.0");
    holds(BinOp::Eq, INT64_MAX, coreir::kTwo63, false, "INT64_MAX != 2^63.0");
    holds(BinOp::Eq, INT64_MIN, -coreir::kTwo63, true, "INT64_MIN == -2^63.0");
    holds(BinOp::Gt, INT64_MIN, -coreir::kTwo63 - 2048.0, true,
          "INT64_MIN > the double below -2^63");
    holds(BinOp::Lt, INT64_MAX, INFINITY, true, "INT64_MAX < inf");
    holds(BinOp::Gt, INT64_MIN, -INFINITY, true, "INT64_MIN > -inf");

    holds(BinOp::Lt, 1, 1.5, true, "1 < 1.5");
    holds(BinOp::Gt, 2, 1.5, true, "2 > 1.5");
    holds(BinOp::Gt, -1, -1.5, true, "-1 > -1.5");
    holds(BinOp::Lt, -2, -1.5, true, "-2 < -1.5");
    holds(BinOp::Eq, 1, 1.0, true, "1 == 1.0");
    holds(BinOp::Eq, 0, -0.0, true, "0 == -0.0");
    holds(BinOp::Le, 1, 1.0, true, "1 <= 1.0");

    // NaN is unordered: false for everything but Ne.
    for (const BinOp op : {BinOp::Eq, BinOp::Lt, BinOp::Le, BinOp::Gt,
                           BinOp::Ge}) {
      holds(op, 1, NAN, false, "int vs NaN");
    }
    holds(BinOp::Ne, 1, NAN, true, "int != NaN");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "values: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("values OK\n");
  return 0;
}
