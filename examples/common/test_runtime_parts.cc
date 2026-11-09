// runtime_parts.h asks three questions of a value twice over: in IR, by a
// part a helper is written with, and in C++, by its twin a host function
// calls -- is_kind and kind_record, instance_class and instance_class,
// is_exception and exception_record. The two must answer alike, or a
// program and its own report disagree about what a value is. This runs
// each part over a set of values in the VM and hands its answer, with
// the value, to a host function that asks the twin and compares.
//
// The values are every shape a meta takes here, and the shapes around
// them: an instance of a class, the class (its method table), a kind
// record, a record of another kind, a builtin exception and one of a
// program's class, a plain object, metas too short for what their head
// names, a Map with an array meta and one with an object meta, and values
// that are no object at all. A meta that is an empty array is not among
// them: nothing builds one -- every kind record and exception starts with
// its type -- and the IR parts read its head, which traps, where the C++
// twins answer none.
//
// Beside them, the two shapes runtime_parts writes over Builder::let for
// the binders -- `reading` and `read_modify_write` -- each run once, to
// pin that every argument and every operand runs once, in order.

#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "runtime_parts.h"

namespace {

using namespace coreir;
using namespace runtime_parts;

struct Failure : std::runtime_error {
  explicit Failure(const std::string& msg) : std::runtime_error(msg) {}
};

constexpr char kTupleKind[] = "tup";

enum Part : int64_t { kKind, kClass, kException, kParts };
const char* const kPartNames[kParts] = {"is_kind/kind_record",
                                        "instance_class",
                                        "is_exception/exception_record"};

int g_failures = 0;
std::vector<std::string> g_out;

std::string joined() {
  std::string s;
  for (const auto& line : g_out) s += line + "|";
  return s;
}

void check_eq(const std::string& got, const std::string& want,
              const std::string& what) {
  if (got != want) {
    std::fprintf(stderr, "FAIL: %s\n  want [%s]\n  got  [%s]\n", what.c_str(),
                 want.c_str(), got.c_str());
    ++g_failures;
  }
}

// Runs a module whose entry is `body`, with `locals` locals.
void run_body(NodeId body, Module& m, int32_t locals, const std::string& what) {
  m.funcs.push_back({});
  m.funcs[0] = {"main", locals, 0, body,
                std::vector<std::string>(static_cast<size_t>(locals), "x"), {}};
  g_out.clear();
  if (auto err = verify(m)) {
    std::fprintf(stderr, "FAIL: %s: malformed IR: %s\n", what.c_str(),
                 err->c_str());
    ++g_failures;
    return;
  }
  Runtime heap;
  try {
    vm::run(vm::compile(m), heap);
  } catch (const Failure& e) {
    std::fprintf(stderr, "FAIL: %s: %s\n", what.c_str(), e.what());
    ++g_failures;
  }
}

// A converter for the test: the exception named by ctx, carrying the trap's
// message.
Value convert_trap(const Value& trap, const void* ctx) {
  return make_exception(static_cast<const char*>(ctx),
                        *trap.as_object()->find("message"));
}

// report(v): the exception's name and message, or the value as it is.
bool report_native(NativeCall& c) {
  const auto* exc = exception_record(c.arg(0));
  g_out.push_back(exc ? (*exc)[kExcName].as_str() + ":" +
                            (*exc)[kExcMessage].as_str()
                      : to_display(c.arg(0)));
  return true;
}

// run_body with the run's natives.
void run_with_natives(NodeId body, Module& m,
                      std::vector<vm::NativeDef> natives,
                      const std::string& what) {
  m.funcs.push_back({"main", 1, 0, body, {"x"}, {}});
  g_out.clear();
  if (auto err = verify(m)) {
    std::fprintf(stderr, "FAIL: %s: malformed IR: %s\n", what.c_str(),
                 err->c_str());
    ++g_failures;
    return;
  }
  vm::RunOptions o;
  o.natives = std::move(natives);
  Runtime heap;
  try {
    vm::run(vm::compile(m), heap, o);
  } catch (const Failure& e) {
    std::fprintf(stderr, "FAIL: %s: %s\n", what.c_str(), e.what());
    ++g_failures;
  }
}

int g_compared = 0;
int g_positive[kParts] = {};

// twin(part, label, v, ir): the twin's answer for `v`, compared with
// what the IR part answered. An answer is the class for instance_class,
// whether there is one for the other two.
bool twin(NativeCall& c) {
  const auto part = static_cast<Part>(c.arg(0).as_int());
  const std::string label = c.arg(1).as_str();
  const Value& v = c.arg(2);
  const Value& ir = c.arg(3);
  bool same = false;
  bool positive = false;
  switch (part) {
    case kKind:
      positive = kind_record(v, kTupleKind) != nullptr;
      same = ir.is_bool() && ir.as_bool() == positive;
      break;
    case kClass: {
      const Value* cls = instance_class(v);
      positive = cls != nullptr;
      same = cls ? ir.is_object() && ir.as_object() == cls->as_object()
                 : ir.is_nil();
      break;
    }
    case kException:
      positive = exception_record(v) != nullptr;
      same = ir.is_bool() && ir.as_bool() == positive;
      break;
    default:
      break;
  }
  ++g_compared;
  if (positive) ++g_positive[part];
  if (!same) {
    std::fprintf(stderr, "FAIL: %s of %s: the IR part and its twin differ\n",
                 kPartNames[part], label.c_str());
    ++g_failures;
  }
  return true;
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
  Module m;
  HelperTable rt(m);
  m.funcs.push_back({});  // main, written below
  const char* const helpers[kParts] = {"$kind", "$class", "$exception"};
  rt.reserve({helpers[kKind], helpers[kClass], helpers[kException]});
  for (int64_t part = 0; part < kParts; ++part) {
    HelperWriter r(rt);
    const auto [v] = r.params("v");
    r.add(r.ret(part == kKind    ? is_kind(r, v, kTupleKind)
                : part == kClass ? instance_class(r, v)
                                 : is_exception(r, v)));
    r.finish(helpers[part]);
  }

  // main: each value into `x`, then each part's answer for it handed to
  // the twin. `cls` holds a class, its method table, for the shapes that
  // name one.
  FuncWriter w(m);
  const auto [cls, x] = w.locals("cls", "x");
  auto& b = w.b;
  const NodeId check_fn = b.native_ref(b.declare_native("twin"));
  const auto meta_of = [&](NodeId obj, std::vector<NodeId> items) {
    return b.intrinsic(IntrinsicId::MetaSet, {obj, b.array_lit(items)});
  };
  const auto C = [&] { return w.L(cls); };
  const std::vector<std::pair<std::string, std::function<NodeId()>>> values{
      {"an instance", [&] { return new_instance(b, C()); }},
      {"a class", [&] { return C(); }},
      {"a kind record", [&] {
         return of_kind(b, b.object_lit({}), kTupleKind, {b.literal(1)});
       }},
      {"a record of another kind", [&] {
         return of_kind(b, b.object_lit({}), "big", {b.literal(1)});
       }},
      {"a builtin exception", [&] {
         return exception(b, b.str_literal(kExcKind), b.str_literal("E"),
                          b.str_literal("m"));
       }},
      {"an exception of a program's class", [&] {
         return exception(b, C(), b.nil_literal(), b.str_literal("m"));
       }},
      {"a plain object", [&] { return b.object_lit({}); }},
      {"a short exception record", [&] {
         return meta_of(b.object_lit({}), {b.str_literal(kExcKind)});
       }},
      {"a class's record with no state", [&] {
         return meta_of(b.object_lit({}), {C()});
       }},
      {"a kind record with no state", [&] {
         return meta_of(b.object_lit({}), {b.str_literal(kTupleKind)});
       }},
      {"a Map with an array meta", [&] {
         return meta_of(b.intrinsic(IntrinsicId::MapNew, {}),
                        {b.str_literal(kTupleKind)});
       }},
      {"a Map with an object meta", [&] {
         return b.intrinsic(IntrinsicId::MetaSet,
                            {b.intrinsic(IntrinsicId::MapNew, {}), C()});
       }},
      {"an int", [&] { return b.literal(42); }},
      {"a string", [&] { return b.str_literal("s"); }},
      {"nil", [&] { return b.nil_literal(); }},
      {"an array", [&] { return b.array_lit({b.literal(1)}); }},
  };
  w.add(w.set(cls, class_table(b, b.object_lit({}), "C", b.nil_literal())));
  for (const auto& [label, make] : values) {
    w.add(w.set(x, make()));
    for (int64_t part = 0; part < kParts; ++part) {
      w.add(b.call_value(check_fn,
                         {b.literal(part), b.str_literal(label), w.L(x),
                          rt.call(helpers[part], {w.L(x)}, SrcPos{})}));
    }
  }
  w.write(m.funcs[0], "main");

  if (auto err = verify(m)) {
    std::fprintf(stderr, "FAIL: malformed IR: %s\n", err->c_str());
    return 1;
  }
  try {
    Runtime heap;
    vm::RunOptions opts;
    opts.natives = {{"twin", 4, &twin, nullptr}};
    vm::run(vm::compile(m), heap, opts);
  } catch (const Failure& e) {
    std::fprintf(stderr, "FAIL: the run: %s\n", e.what());
    return 1;
  }

  // Every value asked of every part, and each part saying yes of some --
  // an instance, the exceptions, the records of its kind -- so that the
  // agreement is not two parts that answer no to everything.
  const int want = static_cast<int>(values.size()) * kParts;
  if (g_compared != want) {
    std::fprintf(stderr, "FAIL: %d comparisons, want %d\n", g_compared, want);
    ++g_failures;
  }
  const int positives[kParts] = {2, 3, 2};
  for (int part = 0; part < kParts; ++part) {
    if (g_positive[part] != positives[part]) {
      std::fprintf(stderr, "FAIL: %s said yes of %d values, want %d\n",
                   kPartNames[part], g_positive[part], positives[part]);
      ++g_failures;
    }
  }
  // reading: three arguments, each printing its number, to a call that
  // reads one: all three print, first to last, and the call sees the
  // first's value. Given no more than it reads, `call` gets the arguments
  // as they are.
  {
    Module m;
    Builder bld(m);
    auto b = bld.at(SrcPos{1, 1});
    const auto arg = [&](int64_t k) {
      return b.block({b.intrinsic(IntrinsicId::Print, {b.literal(k)}),
                      b.literal(k * 10)});
    };
    const NodeId call =
        reading(b, {arg(1), arg(2), arg(3)}, 1,
                [&](const std::vector<NodeId>& read) {
                  if (read.size() != 1) {
                    std::fprintf(stderr, "FAIL: reading: %zu read\n",
                                 read.size());
                    ++g_failures;
                  }
                  return b.intrinsic(IntrinsicId::Print, {read[0]});
                });
    const NodeId only = arg(4);
    NodeId direct;
    reading(b, {only}, 1, [&](const std::vector<NodeId>& read) {
      direct = read[0];
      return read[0];
    });
    if (direct != only) {
      std::fprintf(stderr, "FAIL: reading held what was all read\n");
      ++g_failures;
    }
    run_body(call, m, 0, "reading");
    check_eq(joined(), "1|2|3|10|", "reading: every argument runs, in order");
  }

  // read_modify_write: `a[k()] += 5` runs the receiver and the key once
  // each, in that order, and reads and writes through what they gave; a
  // plain `=` never makes the read.
  {
    const auto run = [&](bool plain) {
      Module m;
      Builder bld(m);
      auto b = bld.at(SrcPos{1, 1});
      const auto noisy = [&](const char* name, NodeId v) {
        return b.block({b.intrinsic(IntrinsicId::Print, {b.str_literal(name)}),
                        v});
      };
      const NodeId rmw = read_modify_write(
          b, noisy("recv", b.varref(VarKind::Local, 0)),
          noisy("key", b.literal(0)),
          [&](NodeId r, NodeId k) { return b.index(r, k); },
          [&](NodeId r, NodeId k, NodeId v) { return b.set_index(r, k, v); },
          [&](const std::function<NodeId()>& read) {
            return plain ? b.literal(7) : b.binary(BinOp::Add, read(),
                                                   b.literal(5));
          });
      run_body(b.block({b.assign(VarKind::Local, 0,
                                 b.array_lit({b.literal(10)})),
                        rmw,
                        b.intrinsic(IntrinsicId::Print,
                                    {b.index(b.varref(VarKind::Local, 0),
                                             b.literal(0))})}),
               m, 1, "read_modify_write");
      return joined();
    };
    check_eq(run(false), "recv|key|15|", "read_modify_write: +=");
    check_eq(run(true), "recv|key|7|", "read_modify_write: =");
  }

  // read_modify_write as an expression: the store has no value, so the
  // one stored is held by a Let and read after it -- `x = (a[k] += 5)`.
  {
    Module m;
    Builder bld(m);
    auto b = bld.at(SrcPos{1, 1});
    const NodeId rmw = read_modify_write(
        b, b.varref(VarKind::Local, 0), b.literal(0),
        [&](NodeId r, NodeId k) { return b.index(r, k); },
        [&](NodeId r, NodeId k, NodeId v) { return b.set_index(r, k, v); },
        [&](const std::function<NodeId()>& read) {
          return b.binary(BinOp::Add, read(), b.literal(5));
        },
        true);
    run_body(b.block({b.assign(VarKind::Local, 0,
                               b.array_lit({b.literal(10)})),
                      b.intrinsic(IntrinsicId::Print, {rmw}),
                      b.intrinsic(IntrinsicId::Print,
                                  {b.index(b.varref(VarKind::Local, 0),
                                           b.literal(0))})}),
             m, 1, "read_modify_write as a value");
    check_eq(joined(), "15|15|", "read_modify_write: as a value");
  }

  // make_exception is the IR exception()'s twin: the same record, so that
  // the readers (is_exception, exception_record) accept both alike.
  {
    const Value e = make_exception("TypeError", Value::make_str("bad"));
    const auto* rec = exception_record(e);
    check_eq(rec ? (*rec)[kExcName].as_str() + "/" +
                       (*rec)[kExcMessage].as_str()
                 : "(none)",
             "TypeError/bad", "make_exception: read back");
    check_eq(rec && (*rec)[0].is_str() ? (*rec)[0].as_str() : "?", kExcKind,
             "make_exception: headed by kExcKind");
    check_eq(exception_record(Value::make_object()) ? "yes" : "no", "no",
             "make_exception: a plain object is none");
  }

  // The conversion at a catch: a trap becomes the front end's exception, and
  // what is not a trap passes through, so a second catch (or a catch of a
  // thrown string) leaves it alone.
  {
    Module m;
    Builder bld(m);
    auto b = bld.at(SrcPos{1, 1});
    const auto caught = [&](NodeId body) {
      return b.make_try(
          0, body,
          b.call_value(b.native_ref(b.declare_native("report")),
                       {converted(b, b.varref(VarKind::Local, 0))}));
    };
    run_with_natives(
        b.block({caught(b.binary(BinOp::Div, b.literal(1), b.literal(0))),
                 caught(b.make_throw(b.str_literal("plain"))),
                 // converted once: a second catch of what the first made
                 caught(b.make_throw(b.make_try(
                     0, b.binary(BinOp::Div, b.literal(1), b.literal(0)),
                     converted(b, b.varref(VarKind::Local, 0)))))}),
        m,
        {trap_convert_def<&convert_trap>("Conv"),
         {"report", 1, &report_native, nullptr}},
        "converted");
    check_eq(joined(), "Conv:divide by zero|plain|Conv:divide by zero|",
             "converted: a trap, a string, and a converted one");
  }

  // The operator spellings, and the fallback for one nobody spells.
  {
    check_eq(binop_symbol(BinOp::Add), "+", "binop_symbol: add");
    check_eq(binop_symbol(BinOp::FloorDiv), "//", "binop_symbol: floordiv");
    check_eq(binop_symbol(BinOp::FloorMod), "%", "binop_symbol: floormod");
    check_eq(binop_symbol(BinOp::Le), "<=", "binop_symbol: le");
    check_eq(binop_symbol(BinOp::UShr), name_of(BinOp::UShr),
             "binop_symbol: unsigned falls back to its name");
    check_eq(unop_symbol(UnOp::Neg), "-", "unop_symbol: neg");
    check_eq(unop_symbol(UnOp::BitNot), "~", "unop_symbol: bitnot");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "runtime_parts: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("runtime_parts OK: %d comparisons\n", g_compared);
  return 0;
}
