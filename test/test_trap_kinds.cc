// A trap's kind and facts: every way the executor itself fails, caught and
// read back through coreir::trap_kind / trap_verb / trap_fact, with the
// message the default path has always given. One case per failing site, so
// a site that names the wrong kind, drops an operand or words its message
// differently fails the case that exercises it and no other.
//
// Also pinned: the record is invisible to a program (the trap object keeps
// its three keys), a re-raised trap keeps its kind and facts, a host's trap
// is Host unless it names another kind, and RunOptions::describe_traps hands
// an uncaught trap to describe.
//
// Not reachable from a program, so not here: "too many arguments to spread"
// (a count past INT32_MAX), and "yield outside a generator" (verify()
// precludes it).

#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "vmlib.h"

namespace {

using namespace coreir;
using I = IntrinsicId;

struct Failure : std::runtime_error {
  Failure(std::string msg) : std::runtime_error(std::move(msg)) {}
};

std::vector<std::string> g_out;
std::vector<std::string> g_seen;  // what each probe() saw
SrcPos g_fail_pos;
int g_failures = 0;

void check_eq(const std::string& got, const std::string& want,
              const std::string& what) {
  if (got != want) {
    std::fprintf(stderr, "FAIL: %s\n  want [%s]\n  got  [%s]\n", what.c_str(),
                 want.c_str(), got.c_str());
    ++g_failures;
  }
}

const char* kind_name(TrapKind k) {
  switch (k) {
    case TrapKind::None:         return "None";
    case TrapKind::Operands:     return "Operands";
    case TrapKind::NotCallable:  return "NotCallable";
    case TrapKind::NotIndexable: return "NotIndexable";
    case TrapKind::KeyType:      return "KeyType";
    case TrapKind::Range:        return "Range";
    case TrapKind::Immutable:    return "Immutable";
    case TrapKind::DivideByZero: return "DivideByZero";
    case TrapKind::Arity:        return "Arity";
    case TrapKind::Uninit:       return "Uninit";
    case TrapKind::Depth:        return "Depth";
    case TrapKind::BadValue:     return "BadValue";
    case TrapKind::State:        return "State";
    case TrapKind::Host:         return "Host";
    case TrapKind::Internal:     return "Internal";
  }
  return "?";
}

std::string verb_name(const Verb& v) {
  switch (v.cls) {
    case VerbClass::None:      return "-";
    case VerbClass::Binary:    return std::string("B:") + name_of(static_cast<BinOp>(v.id));
    case VerbClass::Unary:     return std::string("U:") + name_of(static_cast<UnOp>(v.id));
    case VerbClass::Intrinsic: return std::string("I:") + name_of(static_cast<I>(v.id));
    case VerbClass::Node:      return std::string("N:") + name_of(static_cast<Tag>(v.id));
  }
  return "?";
}

std::string tname(const Value& v) { return type_name(v.tag()); }

// kind|verb|l|r|name|n|m|message
std::string describe_trap(const Value& e) {
  const Value name = trap_fact(e, TrapFact::Name);
  return std::string(kind_name(trap_kind(e))) + "|" +
         verb_name(trap_verb(e)) + "|" + tname(trap_fact(e, TrapFact::Left)) +
         "|" + tname(trap_fact(e, TrapFact::Right)) + "|" +
         (name.is_str() ? name.as_str() : "") + "|" +
         to_display(trap_fact(e, TrapFact::N)) + "|" +
         to_display(trap_fact(e, TrapFact::M)) + "|" +
         e.as_object()->find("message")->as_str();
}

bool native_probe(NativeCall& c) {
  g_seen.push_back(describe_trap(c.arg(0)));
  return true;
}
// calls(f): f(), from the host -- a boundary a coroutine cannot yield across.
bool native_calls(NativeCall& c) {
  c.call(c.arg(0), nullptr, 0);
  return true;
}
bool native_two(NativeCall&) { return true; }
bool native_host(NativeCall& c) {
  c.error = c.trap("host said no");
  return false;
}
bool native_host_kind(NativeCall& c) {
  c.error = c.trap(TrapKind::Range, "host range");
  return false;
}

enum Native { kProbe, kCalls, kTwo, kHost, kHostKind };

vm::RunOptions options() {
  vm::RunOptions o;
  o.natives = {{"probe", 1, &native_probe, nullptr},
               {"calls", 1, &native_calls, nullptr},
               {"two", 2, &native_two, nullptr},
               {"host", 0, &native_host, nullptr},
               {"hostkind", 0, &native_host_kind, nullptr}};
  return o;
}

std::string run_module(const Module& m, const std::string& what,
                       const vm::RunOptions& opts) {
  g_out.clear();
  if (auto err = verify(m)) {
    std::fprintf(stderr, "FAIL: %s: malformed IR: %s\n", what.c_str(),
                 err->c_str());
    ++g_failures;
    return {};
  }
  std::string failure;
  int64_t left = 0;
  {
    Runtime rt;
    const vm::Program p = vm::compile(m);
    try {
      vm::run(p, rt, opts);
    } catch (const Failure& e) {
      failure = e.what();
    }
    left = rt.live_objects();
  }
  if (left != 0) {
    std::fprintf(stderr, "FAIL: %s: leaked %lld heap object(s)\n",
                 what.c_str(), static_cast<long long>(left));
    ++g_failures;
  }
  return failure;
}

const SrcPos P{4, 2};

// The shapes the cases are built from.
NodeId nil(Builder& b) { return b.nil_literal(P); }
NodeId num(Builder& b, int64_t v) { return b.literal(v, P); }
NodeId dbl(Builder& b, double v) { return b.double_literal(v, P); }
NodeId str(Builder& b, const char* s) { return b.str_literal(s, P); }
NodeId arr(Builder& b, std::vector<NodeId> items = {}) {
  return b.array_lit(items, P);
}
NodeId obj(Builder& b) {
  return b.object_lit({{b.str_literal("k", P), b.literal(1, P)}}, P);
}
NodeId in(Builder& b, I id, std::vector<NodeId> args) {
  return b.intrinsic(id, args, P);
}
NodeId loc(Builder& b, int32_t i) { return b.varref(VarKind::Local, i, P); }
NodeId set(Builder& b, int32_t i, NodeId v) {
  return b.assign(VarKind::Local, i, v, P);
}
NodeId nat(Builder& b, int n, std::vector<NodeId> args) {
  return b.call_value(b.native_ref(n, P), args, P);
}

struct Case {
  const char* name;
  std::function<NodeId(Module&, Builder&)> body;
  std::string want;
  int max_depth = 10000;
  int locals = 1;  // local 0 is the caught value; the rest are the body's
};

// A function that takes `params` parameters and runs `body`.
int32_t add_func(Module& m, Builder& b, const char* name, int params,
                 NodeId body, int locals = -1, bool gen = false) {
  (void)b;
  m.funcs.push_back({name, locals < 0 ? params : locals, 0, body, {}, {}});
  m.funcs.back().num_params = params;
  m.funcs.back().is_generator = gen;
  for (int i = 0; i < m.funcs.back().num_locals; ++i) {
    m.funcs.back().local_names.push_back("a" + std::to_string(i));
  }
  return static_cast<int32_t>(m.funcs.size() - 1);
}

NodeId call_fn(Builder& b, int32_t fn, std::vector<NodeId> args = {}) {
  return b.call_value(b.make_closure(fn, 0, P), args, P);
}

// A case's module: main guards `body` and hands the trap to probe().
void run_case(const Case& c) {
  Module m;
  Builder b(m);
  m.capture_maps.push_back({});
  m.natives = {"probe", "calls", "two", "host", "hostkind"};
  m.funcs.push_back({});  // main, below
  const NodeId body = c.body(m, b);
  std::vector<std::string> names{"e"};
  for (int i = 1; i < c.locals; ++i) names.push_back("x" + std::to_string(i));
  m.funcs[0] = {"main", c.locals, 0,
                b.scope(0, c.locals,
                        b.make_try(0, body, nat(b, kProbe, {loc(b, 0)}), P), P),
                names, {}};
  vm::RunOptions o = options();
  o.max_call_depth = c.max_depth;
  g_seen.clear();
  const std::string failure = run_module(m, c.name, o);
  check_eq(failure, "", std::string(c.name) + ": run");
  check_eq(g_seen.size() == 1 ? g_seen[0] : "(" + std::to_string(g_seen.size()) + " traps)",
           c.want, c.name);
}

void cases() {
  const std::vector<Case> all = {
      // ---- operators ----------------------------------------------------
      {"unary neg", [](Module&, Builder& b) { return b.unary(UnOp::Neg, str(b, "s"), P); },
       "Operands|U:neg|string|nil||0|0|cannot negate string"},
      {"unary bitnot", [](Module&, Builder& b) { return b.unary(UnOp::BitNot, dbl(b, 1.5), P); },
       "Operands|U:bitnot|double|nil||0|0|cannot bitwise-not double"},
      {"unary wrap", [](Module&, Builder& b) { return b.unary(UnOp::WrapI8, str(b, "s"), P); },
       "Operands|U:wrapi8|string|nil||0|0|cannot truncate string"},
      {"binary add", [](Module&, Builder& b) { return b.binary(BinOp::Add, num(b, 1), str(b, "s"), P); },
       "Operands|B:add|int|string||0|0|cannot add int and string"},
      {"binary lt", [](Module&, Builder& b) { return b.binary(BinOp::Lt, arr(b), num(b, 1), P); },
       "Operands|B:lt|array|int||0|0|cannot lt array and int"},
      {"binary div by zero", [](Module&, Builder& b) { return b.binary(BinOp::Div, num(b, 1), num(b, 0), P); },
       "DivideByZero|B:div|int|int||0|0|divide by zero"},
      {"binary mod by zero", [](Module&, Builder& b) { return b.binary(BinOp::Mod, num(b, 1), num(b, 0), P); },
       "DivideByZero|B:mod|int|int||0|0|divide by zero"},
      {"binary floordiv by zero", [](Module&, Builder& b) { return b.binary(BinOp::FloorDiv, num(b, 1), num(b, 0), P); },
       "DivideByZero|B:floordiv|int|int||0|0|divide by zero"},
      // ---- Index and SetIndex --------------------------------------------
      {"index nil", [](Module&, Builder& b) { return b.index(nil(b), num(b, 0), P); },
       "NotIndexable|N:index|nil|int||0|0|cannot index nil"},
      {"index array range", [](Module&, Builder& b) { return b.index(arr(b), num(b, 3), P); },
       "Range|N:index|array|int||3|0|array index 3 out of range for length 0"},
      {"index string range", [](Module&, Builder& b) { return b.index(str(b, "ab"), num(b, 5), P); },
       "Range|N:index|string|int||5|2|string index 5 out of range for length 2"},
      {"index array key", [](Module&, Builder& b) { return b.index(arr(b), str(b, "a"), P); },
       "KeyType|N:index|array|string||0|0|array index must be an int, not string"},
      {"index object key", [](Module&, Builder& b) { return b.index(obj(b), num(b, 1), P); },
       "KeyType|N:index|object|int||0|0|object key must be a string, not int"},
      {"setindex string", [](Module&, Builder& b) { return b.set_index(str(b, "ab"), num(b, 0), str(b, "x"), P); },
       "Immutable|N:setindex|string|int||0|0|cannot assign into a string"},
      {"setindex nil", [](Module&, Builder& b) { return b.set_index(nil(b), num(b, 0), num(b, 1), P); },
       "NotIndexable|N:setindex|nil|int||0|0|cannot index nil"},
      {"setindex array range", [](Module&, Builder& b) { return b.set_index(arr(b), num(b, 2), num(b, 1), P); },
       "Range|N:setindex|array|int||2|0|array index 2 out of range for length 0"},
      // ---- fields ---------------------------------------------------------
      {"field get nil", [](Module&, Builder& b) { return b.field_get(nil(b), 0, "x", P); },
       "NotIndexable|N:fieldget|nil|nil|x|0|0|cannot get field 'x' of nil"},
      {"field set int", [](Module&, Builder& b) { return b.field_set(num(b, 1), 0, "y", num(b, 2), P); },
       "NotIndexable|N:fieldset|int|nil|y|0|0|cannot set field 'y' of int"},
      {"field slot", [](Module&, Builder& b) { return b.field_get(obj(b), 4, "z", P); },
       "Internal|N:fieldget|nil|nil|z|0|0|field 'z' out of range"},
      // ---- builtins -------------------------------------------------------
      {"len", [](Module&, Builder& b) { return in(b, I::Len, {nil(b)}); },
       "Operands|I:len|nil|nil||0|0|cannot take the length of nil"},
      {"push", [](Module&, Builder& b) { return in(b, I::ArrayPush, {nil(b), num(b, 1)}); },
       "Operands|I:arraypush|nil|nil||0|0|cannot push into nil"},
      {"pop", [](Module&, Builder& b) { return in(b, I::ArrayPop, {nil(b)}); },
       "Operands|I:arraypop|nil|nil||0|0|cannot pop from nil"},
      {"pop empty", [](Module&, Builder& b) { return in(b, I::ArrayPop, {arr(b)}); },
       "Range|I:arraypop|array|nil||0|0|pop from an empty array"},
      {"concat", [](Module&, Builder& b) { return in(b, I::ArrayConcat, {arr(b), nil(b)}); },
       "Operands|I:arrayconcat|array|nil||0|0|cannot join nil onto array"},
      {"fill type", [](Module&, Builder& b) { return in(b, I::ArrayFill, {nil(b), num(b, 0)}); },
       "Operands|I:arrayfill|nil|nil||0|0|cannot make an array of nil elements"},
      {"fill negative", [](Module&, Builder& b) { return in(b, I::ArrayFill, {num(b, -2), num(b, 0)}); },
       "BadValue|I:arrayfill|int|nil||-2|0|cannot make an array of fewer than 0"},
      {"fill too large", [](Module&, Builder& b) { return in(b, I::ArrayFill, {num(b, int64_t{1} << 62), num(b, 0)}); },
       "BadValue|I:arrayfill|int|nil||4611686018427387904|0|array too large"},
      {"has", [](Module&, Builder& b) { return in(b, I::ObjectHas, {nil(b), str(b, "k")}); },
       "Operands|I:objecthas|nil|string||0|0|cannot ask nil for a string key"},
      {"keys", [](Module&, Builder& b) { return in(b, I::ObjectKeys, {nil(b)}); },
       "Operands|I:objectkeys|nil|nil||0|0|cannot list keys of nil"},
      {"remove", [](Module&, Builder& b) { return in(b, I::ObjectRemove, {nil(b), str(b, "k")}); },
       "Operands|I:objectremove|nil|string||0|0|cannot remove a string key from nil"},
      {"str slice type", [](Module&, Builder& b) { return in(b, I::StrSlice, {nil(b), num(b, 0), num(b, 0)}); },
       "Operands|I:strslice|nil|nil||0|0|cannot slice nil as a string"},
      {"array slice type", [](Module&, Builder& b) { return in(b, I::ArraySlice, {nil(b), num(b, 0), num(b, 0)}); },
       "Operands|I:arrayslice|nil|nil||0|0|cannot slice nil as an array"},
      {"slice bounds type", [](Module&, Builder& b) { return in(b, I::StrSlice, {str(b, "abc"), str(b, "a"), num(b, 1)}); },
       "KeyType|I:strslice|string|int||0|0|slice bounds must be ints, not string and int"},
      {"slice range", [](Module&, Builder& b) { return in(b, I::StrSlice, {str(b, "abc"), num(b, 2), num(b, 9)}); },
       "Range|I:strslice|string|nil||2|9|slice [2, 9) out of range for string of length 3"},
      {"byte type", [](Module&, Builder& b) { return in(b, I::StrByte, {nil(b), num(b, 0)}); },
       "NotIndexable|I:strbyte|nil|nil||0|0|cannot read a byte of nil"},
      {"byte range", [](Module&, Builder& b) { return in(b, I::StrByte, {str(b, "ab"), num(b, 5)}); },
       "Range|I:strbyte|string|int||5|2|string index 5 out of range for length 2"},
      {"byte key", [](Module&, Builder& b) { return in(b, I::StrByte, {str(b, "ab"), str(b, "x")}); },
       "KeyType|I:strbyte|string|string||0|0|string index must be an int, not string"},
      {"frombyte value", [](Module&, Builder& b) { return in(b, I::StrFromByte, {num(b, 300)}); },
       "BadValue|I:strfrombyte|int|nil||0|0|byte value must be an int in 0..255, not 300"},
      {"frombyte type", [](Module&, Builder& b) { return in(b, I::StrFromByte, {str(b, "x")}); },
       "BadValue|I:strfrombyte|string|nil||0|0|byte value must be an int in 0..255, not string"},
      {"upper", [](Module&, Builder& b) { return in(b, I::StrUpper, {num(b, 1)}); },
       "Operands|I:strupper|int|nil||0|0|cannot change the case of int"},
      {"lower", [](Module&, Builder& b) { return in(b, I::StrLower, {num(b, 1)}); },
       "Operands|I:strlower|int|nil||0|0|cannot change the case of int"},
      {"toint type", [](Module&, Builder& b) { return in(b, I::ToInt, {nil(b)}); },
       "Operands|I:toint|nil|nil||0|0|cannot convert nil to int"},
      {"toint range", [](Module&, Builder& b) { return in(b, I::ToInt, {b.binary(BinOp::Div, dbl(b, 0.0), dbl(b, 0.0), P)}); },
       "BadValue|I:toint|double|nil||0|0|double value out of int range"},
      {"todouble", [](Module&, Builder& b) { return in(b, I::ToDouble, {nil(b)}); },
       "Operands|I:todouble|nil|nil||0|0|cannot convert nil to double"},
      {"tofloat", [](Module&, Builder& b) { return in(b, I::ToFloat32, {nil(b)}); },
       "Operands|I:tofloat32|nil|nil||0|0|cannot convert nil to float"},
      {"pow", [](Module&, Builder& b) { return in(b, I::Pow, {nil(b), num(b, 2)}); },
       "Operands|I:pow|nil|int||0|0|cannot pow nil and int"},
      {"metaset", [](Module&, Builder& b) { return in(b, I::MetaSet, {num(b, 1), num(b, 2)}); },
       "Operands|I:metaset|int|nil||0|0|cannot set the meta of int"},
      {"dropset", [](Module&, Builder& b) { return in(b, I::DropSet, {num(b, 1), nil(b)}); },
       "Operands|I:dropset|int|nil||0|0|cannot give a destructor to int"},
      {"fnarity", [](Module&, Builder& b) { return in(b, I::FnArity, {num(b, 1)}); },
       "Operands|I:fnarity|int|nil||0|0|cannot take the arity of int"},
      // ---- calls ----------------------------------------------------------
      {"call", [](Module&, Builder& b) { return b.call_value(num(b, 1), {}, P); },
       "NotCallable|N:callvalue|int|nil||0|0|cannot call int"},
      {"enqueue", [](Module&, Builder& b) { return in(b, I::Enqueue, {num(b, 1)}); },
       "NotCallable|I:enqueue|int|nil||0|0|enqueue needs a function, not int"},
      {"coroutine", [](Module&, Builder& b) { return in(b, I::CoroCreate, {num(b, 1)}); },
       "NotCallable|I:corocreate|int|nil||0|0|coroutine needs a function, not int"},
      {"defer", [](Module&, Builder& b) { return b.scope(0, 0, b.make_defer(num(b, 1), P), P); },
       "NotCallable|N:defer|int|nil||0|0|defer needs a function, not int"},
      {"spread", [](Module&, Builder& b) { return b.call_spread(nil(b), {}, num(b, 1), P); },
       "Operands|N:callspread|int|nil||0|0|cannot spread int"},
      {"closure arity", [](Module& m, Builder& b) {
         return call_fn(b, add_func(m, b, "f", 0, nil(b)), {num(b, 1)});
       },
       "Arity|-|nil|bool|f|1|0|f takes 0 argument(s), given 1"},
      {"native arity", [](Module&, Builder& b) { return nat(b, kTwo, {num(b, 1)}); },
       "Arity|-|nil|nil|two|1|2|two takes 2 argument(s), given 1"},
      {"switch int", [](Module&, Builder& b) {
         return b.make_switch(str(b, "s"), {{num(b, 1), nil(b)}}, nil(b), P);
       },
       "Operands|N:switch|string|nil||0|0|switch subject is string, not int"},
      {"switch str", [](Module&, Builder& b) {
         return b.make_switch(num(b, 1), {{str(b, "a"), nil(b)}}, nil(b), P);
       },
       "Operands|N:switch|int|nil||1|0|switch subject is int, not str"},
      // ---- the rest -------------------------------------------------------
      {"uninit", [](Module&, Builder& b) { return loc(b, 1); },
       "Uninit|-|nil|nil|x1|0|0|uninitialized variable 'x1'", 10000, 2},
      {"depth", [](Module& m, Builder& b) {
         const int32_t self = static_cast<int32_t>(m.funcs.size());
         add_func(m, b, "r", 0, call_fn(b, self));
         return call_fn(b, self);
       },
       "Depth|-|nil|nil||0|0|recursion limit exceeded", 8},
      {"host", [](Module&, Builder& b) { return nat(b, kHost, {}); },
       "Host|-|nil|nil||0|0|host said no"},
      {"host kind", [](Module&, Builder& b) { return nat(b, kHostKind, {}); },
       "Range|-|nil|nil||0|0|host range"},
      // ---- generators and coroutines ---------------------------------------
      {"gen resume type", [](Module&, Builder& b) { return in(b, I::GenResume, {nil(b), nil(b)}); },
       "Operands|I:genresume|nil|nil||0|0|cannot resume nil"},
      {"gen return type", [](Module&, Builder& b) { return in(b, I::GenReturn, {nil(b), nil(b)}); },
       "Operands|I:genreturn|nil|nil||0|0|cannot close nil"},
      {"gen throw type", [](Module&, Builder& b) { return in(b, I::GenThrow, {nil(b), nil(b)}); },
       "Operands|I:genthrow|nil|nil||0|0|cannot throw into nil"},
      {"gen running", [](Module& m, Builder& b) {
         // g = gen(holder); holder[0] = g; resume g: the body resumes itself.
         const int32_t gen = add_func(
             m, b, "gen", 1,
             in(b, I::GenResume, {b.index(loc(b, 0), num(b, 0), P), nil(b)}), 1,
             true);
         return b.block({set(b, 1, arr(b)),
                         set(b, 2, call_fn(b, gen, {loc(b, 1)})),
                         in(b, I::ArrayPush, {loc(b, 1), loc(b, 2)}),
                         in(b, I::GenResume, {loc(b, 2), nil(b)})},
                        P);
       },
       "State|I:genresume|generator|nil||0|0|generator already running", 10000, 3},
      {"coro resume type", [](Module&, Builder& b) { return in(b, I::CoroResume, {nil(b), nil(b)}); },
       "Operands|I:cororesume|nil|nil||0|0|cannot resume nil"},
      {"coro close type", [](Module&, Builder& b) { return in(b, I::CoroClose, {nil(b)}); },
       "Operands|I:coroclose|nil|nil||0|0|cannot close nil"},
      {"coro status type", [](Module&, Builder& b) { return in(b, I::CoroStatus, {nil(b)}); },
       "Operands|I:corostatus|nil|nil||0|0|cannot ask the status of nil"},
      {"coro running", [](Module& m, Builder& b) {
         const int32_t co = add_func(
             m, b, "co", 1,
             in(b, I::CoroResume, {b.index(loc(b, 0), num(b, 0), P), nil(b)}));
         return b.block(
             {set(b, 1, arr(b)),
              set(b, 2, in(b, I::CoroCreate, {b.make_closure(co, 0, P)})),
              in(b, I::ArrayPush, {loc(b, 1), loc(b, 2)}),
              in(b, I::CoroResume, {loc(b, 2), loc(b, 1)})},
             P);
       },
       "State|I:cororesume|coroutine|nil||0|0|coroutine already running", 10000, 3},
      {"coro close running", [](Module& m, Builder& b) {
         const int32_t co = add_func(
             m, b, "co", 1,
             in(b, I::CoroClose, {b.index(loc(b, 0), num(b, 0), P)}));
         return b.block(
             {set(b, 1, arr(b)),
              set(b, 2, in(b, I::CoroCreate, {b.make_closure(co, 0, P)})),
              in(b, I::ArrayPush, {loc(b, 1), loc(b, 2)}),
              in(b, I::CoroResume, {loc(b, 2), loc(b, 1)})},
             P);
       },
       "State|I:coroclose|coroutine|nil||0|0|cannot close a running coroutine", 10000, 3},
      {"yield outside", [](Module&, Builder& b) { return in(b, I::CoroYield, {num(b, 1)}); },
       "State|I:coroyield|nil|nil||0|0|yield outside a coroutine"},
      {"yield across host", [](Module& m, Builder& b) {
         const int32_t y =
             add_func(m, b, "y", 0, in(b, I::CoroYield, {num(b, 1)}));
         const int32_t co = add_func(
             m, b, "co", 1, nat(b, kCalls, {b.make_closure(y, 0, P)}));
         return in(b, I::CoroResume,
                   {in(b, I::CoroCreate, {b.make_closure(co, 0, P)}), nil(b)});
       },
       "State|I:coroyield|nil|nil||1|0|cannot yield across a host boundary"},
  };
  for (const Case& c : all) run_case(c);
}

// ---- What the record is not --------------------------------------------

void shape() {
  // A trap still has exactly its three keys: the facts are in the meta, which
  // no key reaches.
  Module m;
  Builder b(m);
  m.natives = {"probe", "calls", "two", "host", "hostkind"};
  m.funcs.push_back(
      {"main", 1, 0,
       b.make_try(0, b.binary(BinOp::Div, num(b, 1), num(b, 0), P),
                  in(b, I::Print, {in(b, I::Len, {loc(b, 0)})}), P),
       {"e"}, {}});
  run_module(m, "keys", options());
  check_eq(g_out.empty() ? "" : g_out[0], "3", "a trap keeps its three keys");

  // A trap caught and thrown again is the same trap: kind and facts intact.
  Module r;
  Builder rb(r);
  r.natives = m.natives;
  const NodeId inner =
      rb.make_try(0, rb.binary(BinOp::Add, num(rb, 1), str(rb, "s"), P),
                  rb.make_throw(loc(rb, 0), P), P);
  r.funcs.push_back({"main", 1, 0,
                     rb.make_try(0, inner, nat(rb, kProbe, {loc(rb, 0)}), P),
                     {"e"}, {}});
  g_seen.clear();
  run_module(r, "rethrow", options());
  check_eq(g_seen.size() == 1 ? g_seen[0] : "?",
           "Operands|B:add|int|string||0|0|cannot add int and string",
           "a re-raised trap keeps its kind and facts");

  // A value that is not a trap has no kind and no facts.
  check_eq(kind_name(trap_kind(Value::make_int(1))), "None", "int: kind");
  check_eq(kind_name(trap_kind(Value::make_object())), "None", "object: kind");
  check_eq(tname(trap_fact(Value::make_object(), TrapFact::Left)), "nil",
           "object: fact");
}

// ---- describe_traps ----------------------------------------------------------

std::string g_described;
std::string describe(const Value& v) {
  g_described = describe_trap(v);
  return "T[" + std::string(kind_name(trap_kind(v))) + "]";
}

void uncaught() {
  Module m;
  Builder b(m);
  m.natives = {"probe", "calls", "two", "host", "hostkind"};
  m.funcs.push_back({"main", 1, 0,
                     b.binary(BinOp::Div, num(b, 1), b.literal(0, {7, 3}), P),
                     {"e"}, {}});
  vm::RunOptions o = options();
  o.describe = &describe;
  g_described.clear();
  check_eq(run_module(m, "off", o), "divide by zero",
           "describe_traps off: the trap's own message");
  check_eq(g_described, "", "describe_traps off: describe is not asked");
  check_eq(std::to_string(g_fail_pos.line), "7", "off: the trap's line");
  o.describe_traps = true;
  check_eq(run_module(m, "on", o), "uncaught: T[DivideByZero]",
           "describe_traps on: describe's words");
  check_eq(g_described,
           "DivideByZero|B:div|int|int||0|0|divide by zero",
           "describe is handed the trap itself");
  check_eq(std::to_string(g_fail_pos.line) + ":" + std::to_string(g_fail_pos.col),
           "7:3", "on: the trap's position");
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
[[noreturn]] void coreir_rt_fail(const char* msg, int64_t line, int64_t col) {
  g_fail_pos = {static_cast<uint32_t>(line), static_cast<uint32_t>(col)};
  throw Failure(msg);
}
}

int main() {
  cases();
  shape();
  uncaught();
  if (g_failures != 0) {
    std::fprintf(stderr, "trap_kinds: %d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("trap_kinds: OK");
  return 0;
}
