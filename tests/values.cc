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

  if (g_failures != 0) {
    std::fprintf(stderr, "values: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("values OK\n");
  return 0;
}
