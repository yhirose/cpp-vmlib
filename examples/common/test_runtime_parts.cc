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
void coreir_rt_out(int64_t) {}
void coreir_rt_out_str(const char*, int64_t) {}
void coreir_rt_out_raw(const char*, int64_t) {}
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
  if (g_failures != 0) {
    std::fprintf(stderr, "runtime_parts: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("runtime_parts OK: %d comparisons\n", g_compared);
  return 0;
}
