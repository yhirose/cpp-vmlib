// A Lua front end, for the two recipes nothing else here reaches.
//
// **Tail calls.** Lua is one of the few languages that *guarantees* proper
// tail calls, which makes `lua` a real oracle for them rather than an
// implementation that happens to agree. `Func::tail_calls` is on for every
// function this binder emits, and samples/tailcalls.lua recurses two
// hundred thousand deep -- twenty times RunOptions::max_call_depth, so the
// sample does not merely exercise the feature, it *fails* without it.
//
// **Coroutines, in their user-facing form.** vmlib.h's own comment on
// CoroCreate names Lua first: "What Lua's coroutines, Ruby's Fibers and a
// goroutine are made of." mini-go uses the scheduler and mini-js uses
// async/await, so both drive coroutines from underneath; Lua hands
// `coroutine.create` / `resume` / `yield` / `status` / `wrap` to the
// program, which is the shape those intrinsics were named for.
//
// And a third thing, which is not a recipe in the README but is the most
// instructive part of writing this: **Lua's calling convention is not the
// IR's, so the binder writes its own over it.** A function here returns a
// *value*; a Lua function returns any number of them. The convention is
// that every function returns an array of its results and every call site
// adjusts -- `$one` for a value context, positional reads for a multiple
// assignment, and the array passed straight through for `return f(x)`,
// which is what makes a tail call still a tail call. It costs an
// allocation per return, which is the honest price of putting one calling
// convention on top of another; the hot loop of a tail-recursive function
// pays nothing extra, because a returned array is what it hands back.
//
// Three of Lua's own rules disagree with the VM's defaults, and each is a
// func this binder writes:
//   * **Truthiness.** Only `nil` and `false` are false -- `0` and `""` are
//     *true*. Value::truthy() calls 0 false, and its comment says why it
//     refuses to decide: "Lua calls neither [falsy]". So `$truthy`.
//   * **`%` and `//` floor.** `-7 % 3` is 2 in Lua and -1 in C, and
//     BinOp::Mod is C's. So `$mod` and `$idiv`.
//   * **`==` across types is false**, where BinOp::Eq traps. So `$eq`.
//
// A table is a value-keyed Map (IntrinsicId::MapNew), which is what a Lua
// table is: one container with an array part and a hash part, keyed by
// anything. Its metatable hangs under a key no source program can spell.

#include "binder.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <peglib.h>

#include "grammar.h"
#include "vmlib.h"

using namespace peg;
using namespace peg::udl;
using namespace coreir;

namespace mini_lua {
namespace {

// A table's metatable, under a key whose first byte no Lua source can
// produce -- the same trick vmlib.h's own kDropKey uses.
constexpr char kMetaKey[] = "\x01" "mt";

SrcPos pos_of(const Ast& a) {
  return {static_cast<uint32_t>(a.line), static_cast<uint32_t>(a.column)};
}

[[noreturn]] void fail(const Ast& a, const std::string& msg) {
  coreir_rt::fail(msg, static_cast<uint32_t>(a.line),
                  static_cast<uint32_t>(a.column));
}

std::string unescape(const std::string& tok) {
  std::string out;
  for (size_t i = 0; i < tok.size(); ++i) {
    if (tok[i] != '\\' || i + 1 == tok.size()) {
      out.push_back(tok[i]);
      continue;
    }
    switch (tok[++i]) {
      case 'n': out.push_back('\n'); break;
      case 't': out.push_back('\t'); break;
      case 'r': out.push_back('\r'); break;
      case '\\': out.push_back('\\'); break;
      case '\'': out.push_back('\''); break;
      case '"': out.push_back('"'); break;
      default: out.push_back(tok[i]); break;
    }
  }
  return out;
}

// The library names a program may use without declaring them. Each is
// recognized in the one shape this front end supports it in, not bound as
// a value -- `print` alone is an error, `print(x)` is a NativeRef.
bool is_global(const std::string& n) {
  return n == "print" || n == "type" || n == "tostring" || n == "ipairs" ||
         n == "pairs" || n == "setmetatable" || n == "getmetatable" ||
         n == "pcall" || n == "error" || n == "select" || n == "table" ||
         n == "string" || n == "math" || n == "io" || n == "coroutine" ||
         n == "rawset" || n == "rawget";
}

// A `:name(...)` whose receiver is a string. Resolved by name at bind time,
// because a subset without a string metatable has no runtime lookup to do
// it with -- README.md lists the consequence.
bool is_string_method(const std::string& n) {
  return n == "rep" || n == "sub" || n == "upper" || n == "lower" ||
         n == "len" || n == "byte";
}

// What this front end knows about a function beyond the closure conversion
// coreir::Resolver does -- kept parallel to Resolver::fns, which owns the
// parent link, the Module::funcs index and the capture/cell numbering.
struct FnInfo {
  std::string name = "?";
  std::vector<int32_t> params;
  const Ast* body = nullptr;
};

struct FnCtx : FrameLayout {
  int32_t fn = 0;
};

struct Binder {
  Module m;
  Resolver rs;
  std::vector<FnInfo> fns;  // parallel to rs.fns
  std::map<const Ast*, int32_t> ref_of;
  std::map<const Ast*, int32_t> decl_of;
  std::map<const Ast*, int32_t> fn_of;
  std::map<const Ast*, std::vector<int32_t>> block_decls;
  std::map<std::string, int32_t> rt;
  int32_t empty_cmap = -1;
  // Lua's error messages name the chunk, which is the path as it was
  // given on the command line -- so the binder has to be told it.
  std::string chunk;

  // ==== Pass A: scopes, declarations, captures =============================

  // The order a block declared its bindings in is Resolver's -- Lua permits
  // shadowing within one block (`local x` twice is two bindings, and the
  // second hides the first from there on), so the name map holds one of
  // them and declared_order holds both.
  const std::vector<int32_t>& scope_order() const {
    return rs.declared_order(rs.depth() - 1);
  }

  int32_t new_fn(int32_t parent, const std::string& name) {
    const int32_t f = rs.new_fn(parent);
    fns.push_back({});
    fns[static_cast<size_t>(f)].name = name;
    return f;
  }

  void resolve_block(const Ast& block, int32_t fn) {
    rs.push_scope();
    for (const auto& s : block.nodes) resolve_stmt(*s, fn);
    block_decls[&block] = scope_order();
    rs.pop_scope();
  }

  // `funcbody` is params + block + the `end`. A method declared with `:`
  // gets an implicit `self` in front, which is Lua's own rule and the only
  // place a parameter appears that the source did not write.
  int32_t resolve_fn(const Ast& node, const Ast& body, int32_t parent,
                     const std::string& name, bool implicit_self) {
    const int32_t f = new_fn(parent, name);
    fns[static_cast<size_t>(f)].body = body.nodes[1].get();
    fn_of[&node] = f;
    rs.push_scope();
    if (implicit_self) {
      fns[static_cast<size_t>(f)].params.push_back(rs.declare("self", f));
    }
    for (const auto& p : body.nodes[0]->nodes) {
      const int32_t v = rs.declare(std::string(p->token), f);
      decl_of[p.get()] = v;
      fns[static_cast<size_t>(f)].params.push_back(v);
    }
    resolve_block(*body.nodes[1], f);
    rs.pop_scope();
    return f;
  }

  void resolve_stmt(const Ast& a, int32_t fn) {
    switch (a.tag) {
      case "emptystmt"_:
      case "breakstat"_:
        return;
      case "localfn"_: {
        // The name is visible inside the body, so it is declared first --
        // which is what makes `local function f() return f() end` recurse.
        const Ast& id = *a.nodes[0];
        decl_of[&id] = rs.declare(std::string(id.token), fn);
        resolve_fn(a, *a.nodes[1], fn, std::string(id.token), false);
        return;
      }
      case "localdecl"_: {
        if (a.nodes.size() > 1) resolve_expr(*a.nodes[1], fn);
        for (const auto& id : a.nodes[0]->nodes) {
          decl_of[id.get()] = rs.declare(std::string(id->token), fn);
        }
        return;
      }
      case "fnstat"_: {
        const Ast& nm = *a.nodes[0];
        const bool method = nm.nodes.size() > 1 &&
                            nm.nodes.back()->tag == "colonname"_;
        // `function f()` assigns to whatever `f` already names; only the
        // base identifier is a reference, the rest are keys.
        resolve_expr(*nm.nodes[0], fn);
        resolve_fn(a, *a.nodes[1], fn, std::string(nm.nodes[0]->token),
                   method);
        return;
      }
      case "ifstat"_:
        for (const auto& c : a.nodes) {
          if (c->tag == "block"_) {
            resolve_block(*c, fn);
          } else if (c->tag == "elseifpart"_) {
            resolve_expr(*c->nodes[0], fn);
            resolve_block(*c->nodes[1], fn);
          } else if (c->tag == "elsepart"_) {
            resolve_block(*c->nodes[0], fn);
          } else {
            resolve_expr(*c, fn);
          }
        }
        return;
      case "whilestat"_:
        resolve_expr(*a.nodes[0], fn);
        resolve_block(*a.nodes[1], fn);
        return;
      case "repeatstat"_: {
        // `until`'s condition can see the block's own locals -- Lua's one
        // scope that outlives its braces, so the block and the condition
        // are resolved in the same scope rather than in two.
        rs.push_scope();
        for (const auto& s : a.nodes[0]->nodes) resolve_stmt(*s, fn);
        resolve_expr(*a.nodes[1], fn);
        block_decls[a.nodes[0].get()] = scope_order();
        rs.pop_scope();
        return;
      }
      case "fornum"_: {
        for (size_t i = 1; i + 1 < a.nodes.size(); ++i) {
          if (a.nodes[i]->tag == "block"_) break;
          if (a.nodes[i]->tag == "forstep"_) {
            resolve_expr(*a.nodes[i]->nodes[0], fn);
          } else {
            resolve_expr(*a.nodes[i], fn);
          }
        }
        rs.push_scope();
        decl_of[a.nodes[0].get()] =
            rs.declare(std::string(a.nodes[0]->token), fn);
        resolve_block(*a.nodes.back(), fn);
        block_decls[&a] = scope_order();
        rs.pop_scope();
        return;
      }
      case "forin"_: {
        resolve_expr(*a.nodes[1], fn);
        rs.push_scope();
        for (const auto& id : a.nodes[0]->nodes) {
          decl_of[id.get()] = rs.declare(std::string(id->token), fn);
        }
        resolve_block(*a.nodes[2], fn);
        block_decls[&a] = scope_order();
        rs.pop_scope();
        return;
      }
      case "dostat"_:
        resolve_block(*a.nodes[0], fn);
        return;
      case "retstat"_:
      case "callstmt"_:
        for (const auto& c : a.nodes) resolve_expr(*c, fn);
        return;
      case "assign"_:
        for (const auto& c : a.nodes) resolve_expr(*c, fn);
        return;
      default:
        fail(a, "cannot resolve statement " + a.name);
    }
  }

  void resolve_expr(const Ast& a, int32_t fn) {
    switch (a.tag) {
      case "number"_:
      case "float"_:
      case "string"_:
      case "literal"_:
        return;
      case "ident"_: {
        const std::string n(a.token);
        if (auto v = rs.resolve(n, fn)) {
          ref_of[&a] = *v;
          return;
        }
        if (is_global(n)) return;
        fail(a, "undefined variable '" + n + "'");
      }
      case "funcexpr"_:
        resolve_fn(a, *a.nodes[0], fn, "<anon>", false);
        return;
      case "namedfield"_:
        resolve_expr(*a.nodes[1], fn);
        return;
      default:
        for (const auto& c : a.nodes) {
          if (c->tag == "cmpop"_ || c->tag == "addop"_ ||
              c->tag == "mulop"_ || c->tag == "dotsfx"_) {
            continue;
          }
          if (c->tag == "methodsfx"_) {
            // children: the method name, then the args -- the name is a key.
            for (size_t i = 1; i < c->nodes.size(); ++i) {
              resolve_expr(*c->nodes[i], fn);
            }
            continue;
          }
          resolve_expr(*c, fn);
        }
        return;
    }
  }

  // ==== The runtime this front end writes in its own IR ====================

  static const std::vector<std::string>& rt_names() {
    static const std::vector<std::string> names = {
        "$truthy", "$eq",   "$mod",  "$idiv", "$div",  "$cat",   "$str",
        "$key",    "$get",  "$set",  "$len",  "$one",  "$nth",   "$append",
        "$resume", "$costatus", "$wrap", "$wrapped", "$concat", "$insert",
        "$setmt",  "$getmt", "$spread", "$error",
        "$mt", "$mm", "$add", "$subm", "$mulm", "$strall",
    };
    return names;
  }

  // The front end's own additions to coreir::FuncWriter: the helpers
  // that have to reach this binder's own tables.
  struct RT : FuncWriter {
    Binder& bd;

    explicit RT(Binder& bd_) : FuncWriter(bd_.m), bd(bd_) {}

    // `t` is a value's *type string*, from `typ(v)`: true when it names
    // one of Lua's two number types.
    NodeId is_num(NodeId t) { return b.make_if(is(t, "int"), Bo(true), is(t, "double")); }

    NodeId call(const std::string& name, const std::vector<NodeId>& a) {
      return b.call_value(b.make_closure(bd.rt.at(name), bd.empty_cmap), a);
    }

    NodeId clos(const std::string& name, const std::vector<int32_t>& cells) {
      return FuncWriter::clos(bd.rt.at(name), cells);
    }

    // The counts and the name table come from param()/local().
    void finish(const std::string& name, int32_t ncells = 0,
                int32_t ncaps = 0) {
      write(bd.m.funcs[static_cast<size_t>(bd.rt.at(name))], name, ncells,
            ncaps);
    }
  };

  // Only nil and false are false. Value::truthy() calls 0 false and says in
  // its own comment that this is the disagreement that makes truthiness a
  // language's decision: "JavaScript calls both [""and NaN] falsy, Lua calls
  // neither."
  void rt_truthy() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.iff(r.is(r.typ(r.L(v)), "nil"), r.ret(r.Bo(false))));
    r.add(r.iff(r.is(r.typ(r.L(v)), "bool"), r.ret(r.L(v))));
    r.add(r.ret(r.Bo(true)));
    r.finish("$truthy");
  }

  // `==`: numbers compare numerically across integer and float, values of
  // different types are unequal rather than an error (which is where
  // eval_binop stops), and everything else is identity.
  void rt_eq() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [ta, tb, m] = r.locals("ta", "tb", "m");
    r.add(r.set(ta, r.typ(r.L(a))));
    r.add(r.set(tb, r.typ(r.L(b))));
    r.add(r.iff(
        r.is_num(r.L(ta)),
        r.iff(r.is_num(r.L(tb)), r.ret(r.bin(BinOp::Eq, r.L(a), r.L(b))))));
    r.add(r.iff(r.bin(BinOp::Ne, r.L(ta), r.L(tb)), r.ret(r.Bo(false))));
    r.add(r.iff(r.is(r.L(ta), "nil"), r.ret(r.Bo(true))));
    r.add(
        r.iff(r.is(r.L(ta), "bool"), r.ret(r.bin(BinOp::Eq, r.L(a), r.L(b)))));
    r.add(r.iff(r.is(r.L(ta), "string"),
                r.ret(r.bin(BinOp::Eq, r.L(a), r.L(b)))));
    r.add(r.iff(
        r.is(r.L(ta), "map"),
        r.blk(
            {r.set(m, r.call("$mm", {r.L(a), r.S("__eq")})),
             r.iff(r.isnt(r.typ(r.L(m)), "nil"),
                   r.ret(r.call(
                       "$truthy",
                       {r.call("$one",
                               {r.b.call_value(r.L(m), {r.L(a), r.L(b)})})}))),
             r.set(m, r.call("$mm", {r.L(b), r.S("__eq")})),
             r.iff(r.isnt(r.typ(r.L(m)), "nil"),
                   r.ret(r.call(
                       "$truthy",
                       {r.call("$one", {r.b.call_value(
                                           r.L(m), {r.L(a), r.L(b)})})})))})));
    r.add(r.ret(r.in(IntrinsicId::Same, {r.L(a), r.L(b)})));
    r.finish("$eq");
  }

  // Lua's `%` and `//` floor; C's (and BinOp::Mod's, and BinOp::Div's on
  // two ints) truncate toward zero. `-7 % 3` is 2 here and -1 there, and
  // `-7 // 2` is -4 here and -3 there -- so both get a correction when the
  // operands' signs differ and the division was not exact.
  void rt_mod() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [i, f] = r.locals("i", "f");
    r.add(r.iff(
        r.b.make_if(r.is(r.typ(r.L(a)), "int"), r.is(r.typ(r.L(b)), "int"),
                    r.Bo(false)),
        r.blk(
            {r.set(i, r.bin(BinOp::Mod, r.L(a), r.L(b))),
             r.iff(r.b.make_if(r.bin(BinOp::Ne, r.L(i), r.I(0)),
                               r.bin(BinOp::Lt,
                                     r.bin(BinOp::Mul, r.L(i), r.L(b)), r.I(0)),
                               r.Bo(false)),
                   r.set(i, r.bin(BinOp::Add, r.L(i), r.L(b)))),
             r.ret(r.L(i))})));
    // The float path: fmod, corrected the same way.
    r.add(r.set(f, r.in(IntrinsicId::FMod, {r.L(a), r.L(b)})));
    r.add(r.iff(r.b.make_if(r.bin(BinOp::Ne, r.L(f), r.D(0.0)),
                            r.bin(BinOp::Lt, r.bin(BinOp::Mul, r.L(f), r.L(b)),
                                  r.D(0.0)),
                            r.Bo(false)),
                r.set(f, r.bin(BinOp::Add, r.L(f), r.L(b)))));
    r.add(r.ret(r.L(f)));
    r.finish("$mod");
  }

  void rt_idiv() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [q] = r.locals("q");
    r.add(r.iff(
        r.b.make_if(r.is(r.typ(r.L(a)), "int"), r.is(r.typ(r.L(b)), "int"),
                    r.Bo(false)),
        r.blk(
            {r.set(q, r.bin(BinOp::Div, r.L(a), r.L(b))),
             // Truncation and flooring differ exactly when the
             // signs disagree and the remainder is nonzero.
             r.iff(r.b.make_if(r.bin(BinOp::Ne,
                                     r.bin(BinOp::Mul, r.L(q), r.L(b)), r.L(a)),
                               r.bin(BinOp::Lt,
                                     r.bin(BinOp::Mul, r.L(a), r.L(b)), r.I(0)),
                               r.Bo(false)),
                   r.set(q, r.bin(BinOp::Sub, r.L(q), r.I(1)))),
             r.ret(r.L(q))})));
    r.add(r.ret(
        r.nat("floor", {r.bin(BinOp::Div, r.in(IntrinsicId::ToDouble, {r.L(a)}),
                              r.in(IntrinsicId::ToDouble, {r.L(b)}))})));
    r.finish("$idiv");
  }

  // `/` is always float in Lua, even on two integers.
  void rt_div() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    r.add(r.ret(r.bin(BinOp::Div, r.in(IntrinsicId::ToDouble, {r.L(a)}),
                      r.in(IntrinsicId::ToDouble, {r.L(b)}))));
    r.finish("$div");
  }

  // tostring, through the host: Lua formats a float with "%.14g" and then
  // makes sure it still looks like a float, which is neither to_display's
  // shortest round trip nor anything an IR-level helper could produce.
  void rt_str() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [m] = r.locals("m");
    r.add(r.iff(r.is(r.typ(r.L(v)), "string"), r.ret(r.L(v))));
    r.add(r.iff(r.is(r.typ(r.L(v)), "map"),
                r.blk({r.set(m, r.call("$mm", {r.L(v), r.S("__tostring")})),
                       r.iff(r.isnt(r.typ(r.L(m)), "nil"),
                             r.ret(r.call("$one", {r.b.call_value(
                                                      r.L(m), {r.L(v)})})))})));
    r.add(r.ret(r.nat("tostring", {r.L(v)})));
    r.finish("$str");
  }

  // A table's metatable, and one metamethod off it -- the walk every
  // metamethod here shares. `__index` (rt_get, above) predates this and
  // stays written inline; the newer ones (`__add`/`__sub`/`__mul`/`__eq`/
  // `__tostring`/`__call`) all go through these two.
  void rt_mt() {
    RT r(*this);
    const auto [t] = r.params("t");
    r.add(r.iff(r.isnt(r.typ(r.L(t)), "map"), r.ret(r.Nil())));
    // A missing key already reads as nil (vmlib.h's own rule for a map),
    // so no metatable is the same case as one with nothing at this key.
    r.add(r.ret(r.idx(r.L(t), kMetaKey)));
    r.finish("$mt");
  }

  void rt_mm() {
    RT r(*this);
    const auto [v, name] = r.params("v", "name");
    const auto [mt] = r.locals("mt");
    r.add(r.set(mt, r.call("$mt", {r.L(v)})));
    r.add(r.iff(r.is(r.typ(r.L(mt)), "nil"), r.ret(r.Nil())));
    r.add(r.ret(r.idx(r.L(mt), r.L(name))));
    r.finish("$mm");
  }

  // `+`, `-`, `*` on two numbers stay `BinOp`; on anything else, the
  // matching metamethod off either operand -- Lua's own precedence, left
  // operand first.
  void rt_arith(const std::string& name, BinOp op, const std::string& mm) {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [ta, tb, mv] = r.locals("ta", "tb", "m");
    r.add(r.set(ta, r.typ(r.L(a))));
    r.add(r.set(tb, r.typ(r.L(b))));
    r.add(r.iff(r.is_num(r.L(ta)),
                r.iff(r.is_num(r.L(tb)), r.ret(r.bin(op, r.L(a), r.L(b))))));
    const auto try_mm = [&](FuncWriter::Slot recv) {
      return std::vector<NodeId>{
          r.set(mv, r.call("$mm", {r.L(recv), r.S(mm)})),
          r.iff(r.isnt(r.typ(r.L(mv)), "nil"),
                r.ret(r.call("$one",
                             {r.b.call_value(r.L(mv), {r.L(a), r.L(b)})})))};
    };
    for (const NodeId n : try_mm(a)) r.add(n);
    for (const NodeId n : try_mm(b)) r.add(n);
    r.add(r.b.make_throw(r.S("attempt to perform arithmetic on a table "
                             "value")));
    r.finish(name);
  }

  void rt_cat() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    r.add(r.ret(
        r.bin(BinOp::Add, r.call("$str", {r.L(a)}), r.call("$str", {r.L(b)}))));
    r.finish("$cat");
  }

  // A table key: `t[1.0]` and `t[1]` are the same slot in Lua, so an
  // integral float normalizes to the integer it equals before it is used.
  void rt_key() {
    RT r(*this);
    const auto [k] = r.params("k");
    const auto [i] = r.locals("i");
    r.add(r.iff(
        r.is(r.typ(r.L(k)), "double"),
        r.blk({r.iff(r.bin(BinOp::Ne, r.L(k), r.L(k)), r.ret(r.L(k))),
               r.iff(r.b.make_if(
                         r.bin(BinOp::Gt, r.L(k), r.D(-9.007199254740992e15)),
                         r.bin(BinOp::Lt, r.L(k), r.D(9.007199254740992e15)),
                         r.Bo(false)),
                     r.blk({r.set(i, r.in(IntrinsicId::ToInt, {r.L(k)})),
                            r.iff(r.bin(BinOp::Eq,
                                        r.in(IntrinsicId::ToDouble, {r.L(i)}),
                                        r.L(k)),
                                  r.ret(r.L(i)))}))})));
    r.add(r.ret(r.L(k)));
    r.finish("$key");
  }

  // Reading a table, with the one metamethod this subset has. `__index`
  // may be a table (look there instead) or a function (call it) -- Lua's
  // own rule, written here because a metatable is a language's idea and
  // not the IR's.
  void rt_get() {
    RT r(*this);
    const auto [t, k] = r.params("t", "k");
    const auto [key, mt, idx] = r.locals("key", "mt", "idx");
    r.add(r.iff(r.isnt(r.typ(r.L(t)), "map"),
                r.b.make_throw(r.S("attempt to index a non-table value"))));
    r.add(r.set(key, r.call("$key", {r.L(k)})));
    r.add(r.iff(r.has(r.L(t), r.L(key)), r.ret(r.idx(r.L(t), r.L(key)))));
    r.add(r.iff(
        r.has(r.L(t), r.S(kMetaKey)),
        r.blk({r.set(mt, r.idx(r.L(t), kMetaKey)),
               r.iff(r.has(r.L(mt), r.S("__index")),
                     r.blk({r.set(idx, r.idx(r.L(mt), r.S("__index"))),
                            r.iff(r.is(r.typ(r.L(idx)), "map"),
                                  r.ret(r.call("$get", {r.L(idx), r.L(key)}))),
                            r.ret(r.call(
                                "$one",
                                {r.b.call_value(r.L(idx),
                                                {r.L(t), r.L(key)})}))}))})));
    r.add(r.ret(r.Nil()));
    r.finish("$get");
  }

  // Writing: assigning nil removes the key, which is what makes `#t` and
  // `pairs` agree with Lua about what is in the table.
  void rt_set() {
    RT r(*this);
    const auto [t, k, v] = r.params("t", "k", "v");
    const auto [key, m] = r.locals("key", "m");
    r.add(r.iff(r.isnt(r.typ(r.L(t)), "map"),
                r.b.make_throw(r.S("attempt to index a non-table value"))));
    r.add(r.set(key, r.call("$key", {r.L(k)})));
    r.add(r.iff(r.is(r.typ(r.L(v)), "nil"),
                r.blk({r.in(IntrinsicId::ObjectRemove, {r.L(t), r.L(key)}),
                       r.ret(r.L(v))})));
    // __newindex fires only for a key the raw table does not already
    // have -- Lua's own rule, so an overwrite never consults it.
    r.add(r.iff(
        r.bin(BinOp::Eq, r.has(r.L(t), r.L(key)), r.Bo(false)),
        r.blk(
            {r.set(m, r.call("$mm", {r.L(t), r.S("__newindex")})),
             r.iff(
                 r.isnt(r.typ(r.L(m)), "nil"),
                 r.blk({r.iff(r.is(r.typ(r.L(m)), "map"),
                              r.ret(r.call("$set", {r.L(m), r.L(k), r.L(v)}))),
                        r.call("$one", {r.b.call_value(
                                           r.L(m), {r.L(t), r.L(k), r.L(v)})}),
                        r.ret(r.L(v))}))})));
    r.add(r.b.set_index(r.L(t), r.L(key), r.L(v)));
    r.add(r.ret(r.L(v)));
    r.finish("$set");
  }

  // `#`: a string's byte count, or a table's border -- the largest n with
  // t[n] present and t[n+1] absent, found by walking from 1. Len on a Map
  // answers how many keys it has, which is not the same number once a
  // table has a hash part.
  void rt_len() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [n] = r.locals("n");
    r.add(r.iff(r.is(r.typ(r.L(v)), "string"), r.ret(r.len(r.L(v)))));
    r.add(r.iff(r.isnt(r.typ(r.L(v)), "map"),
                r.b.make_throw(r.S("attempt to get length of a non-table"))));
    r.add(r.set(n, r.I(0)));
    r.add(r.b.make_while(r.has(r.L(v), r.bin(BinOp::Add, r.L(n), r.I(1))),
                         r.set(n, r.bin(BinOp::Add, r.L(n), r.I(1)))));
    r.add(r.ret(r.L(n)));
    r.finish("$len");
  }

  // The calling convention this front end writes over the IR's: every
  // function answers an array of its results, and a value context takes
  // the first, or nil when there was none.
  void rt_one() {
    RT r(*this);
    const auto [vals] = r.params("vals");
    r.add(r.iff(r.bin(BinOp::Gt, r.len(r.L(vals)), r.I(0)),
                r.ret(r.idx(r.L(vals), r.I(0)))));
    r.add(r.ret(r.Nil()));
    r.finish("$one");
  }

  void rt_nth() {
    RT r(*this);
    const auto [vals, i] = r.params("vals", "i");
    r.add(r.iff(r.bin(BinOp::Gt, r.len(r.L(vals)), r.L(i)),
                r.ret(r.idx(r.L(vals), r.L(i)))));
    r.add(r.ret(r.Nil()));
    r.finish("$nth");
  }

  void rt_append() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [out, i] = r.locals("out", "i");
    r.add(r.set(
        out, r.in(IntrinsicId::ArraySlice, {r.L(a), r.I(0), r.len(r.L(a))})));
    r.add(r.set(i, r.I(0)));
    r.add(r.b.make_while(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(b))),
        r.blk({r.in(IntrinsicId::ArrayPush, {r.L(out), r.idx(r.L(b), r.L(i))}),
               r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$append");
  }

  // coroutine.resume. CoroResume answers {value, done}; Lua answers
  // `true` followed by the values, or `false` and a message. The one
  // wrinkle is what "the values" means when the coroutine has finished:
  // its body returned under this front end's own convention, so the value
  // is already an array of results and gets appended rather than wrapped.
  void rt_resume() {
    RT r(*this);
    const auto [co, v] = r.params("co", "v");
    const auto [r_, exc] = r.locals("r", "exc");
    r.add(r.iff(
        r.is(r.in(IntrinsicId::CoroStatus, {r.L(co)}), "done"),
        r.ret(r.arr({r.Bo(false), r.S("cannot resume dead coroutine")}))));
    // A throw the coroutine's own frames do not catch continues at the
    // CoroResume, "into the resumer's own handlers" -- so resume is where
    // Lua's rule that a failed coroutine answers `false, err` rather than
    // propagating gets written.
    r.add(r.b.make_try(
        3, r.set(r_, r.in(IntrinsicId::CoroResume, {r.L(co), r.L(v)})),
        r.ret(r.arr({r.Bo(false), r.L(exc)}))));
    r.add(r.iff(r.idx(r.L(r_), "done"),
                r.ret(r.call("$append",
                             {r.arr({r.Bo(true)}), r.idx(r.L(r_), "value")}))));
    r.add(r.ret(r.arr({r.Bo(true), r.idx(r.L(r_), "value")})));
    r.finish("$resume");
  }

  // CoroStatus's vocabulary is the IR's ("start", "done"); Lua's is
  // "suspended" and "dead". A mapping, not a mechanism.
  void rt_costatus() {
    RT r(*this);
    const auto [co] = r.params("co");
    const auto [s] = r.locals("s");
    r.add(r.set(s, r.in(IntrinsicId::CoroStatus, {r.L(co)})));
    r.add(r.iff(r.is(r.L(s), "start"), r.ret(r.S("suspended"))));
    r.add(r.iff(r.is(r.L(s), "done"), r.ret(r.S("dead"))));
    r.add(r.ret(r.L(s)));
    r.finish("$costatus");
  }

  // coroutine.wrap: a closure over a coroutine of its own, which raises
  // instead of answering false. The cell is what the closure captures.
  void rt_wrap() {
    RT r(*this);
    const auto [f] = r.params("f");
    r.add(r.b.cell_fresh(0));
    r.add(
        r.b.assign(VarKind::Cell, 0, r.in(IntrinsicId::CoroCreate, {r.L(f)})));
    r.add(r.ret(r.clos("$wrapped", {0})));
    r.finish("$wrap", 1);
  }

  void rt_wrapped() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [r_] = r.locals("r");
    r.add(r.set(r_, r.call("$resume", {r.P(0), r.L(v)})));
    r.add(r.iff(r.bin(BinOp::Eq, r.idx(r.L(r_), r.I(0)), r.Bo(false)),
                r.b.make_throw(r.call(
                    "$one", {r.call("$append", {r.arr({}), r.L(r_)})}))));
    r.add(r.ret(
        r.in(IntrinsicId::ArraySlice, {r.L(r_), r.I(1), r.len(r.L(r_))})));
    r.finish("$wrapped", 0, 1);
  }

  // table.concat and table.insert: loops over $get/$set/$len, written here
  // rather than as natives because a table is a Map and walking one from
  // C++ would tie this front end to MapObj's layout.
  void rt_concat() {
    RT r(*this);
    const auto [t, sep] = r.params("t", "sep");
    const auto [out, i, n] = r.locals("out", "i", "n");
    r.add(r.set(out, r.S("")));
    r.add(r.set(i, r.I(1)));
    r.add(r.set(n, r.call("$len", {r.L(t)})));
    r.add(r.b.make_while(
        r.bin(BinOp::Le, r.L(i), r.L(n)),
        r.blk({r.iff(r.bin(BinOp::Gt, r.L(i), r.I(1)),
                     r.set(out, r.bin(BinOp::Add, r.L(out), r.L(sep)))),
               r.set(out,
                     r.bin(BinOp::Add, r.L(out),
                           r.call("$str", {r.call("$get", {r.L(t), r.L(i)})}))),
               r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$concat");
  }

  void rt_insert() {
    RT r(*this);
    const auto [t, v] = r.params("t", "v");
    r.add(r.call(
        "$set",
        {r.L(t), r.bin(BinOp::Add, r.call("$len", {r.L(t)}), r.I(1)), r.L(v)}));
    r.add(r.ret(r.Nil()));
    r.finish("$insert");
  }

  // `error(msg)` prefixes a *string* message with the chunk name and the
  // line it was raised at -- Lua's default error level. Anything else is
  // raised unchanged, which is what lets a table carry structured detail.
  void rt_error() {
    RT r(*this);
    const auto [v, where] = r.params("v", "where");
    r.add(r.iff(r.is(r.typ(r.L(v)), "string"),
                r.b.make_throw(r.bin(BinOp::Add, r.L(where), r.L(v)))));
    r.add(r.b.make_throw(r.L(v)));
    r.finish("$error");
  }

  void rt_setmt() {
    RT r(*this);
    const auto [t, mt] = r.params("t", "mt");
    r.add(r.b.set_index(r.L(t), r.S(kMetaKey), r.L(mt)));
    r.add(r.ret(r.L(t)));
    r.finish("$setmt");
  }

  void rt_getmt() {
    RT r(*this);
    const auto [t] = r.params("t");
    r.add(r.iff(r.has(r.L(t), r.S(kMetaKey)), r.ret(r.idx(r.L(t), kMetaKey))));
    r.add(r.ret(r.Nil()));
    r.finish("$getmt");
  }

  // A trailing call inside a table constructor contributes *all* of its
  // results -- `{f()}` is as many entries as f returned -- so the count is
  // not known until it runs.
  void rt_spread() {
    RT r(*this);
    const auto [t, start, vals] = r.params("t", "start", "vals");
    const auto [i] = r.locals("i");
    r.add(r.set(i, r.I(0)));
    r.add(r.b.make_while(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(vals))),
        r.blk({r.call("$set", {r.L(t), r.bin(BinOp::Add, r.L(start), r.L(i)),
                               r.idx(r.L(vals), r.L(i))}),
               r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(t)));
    r.finish("$spread");
  }

  // `print` goes through `$str` per argument before the host ever sees
  // them, because the host's own formatter cannot call a Lua `__tostring`.
  void rt_strall() {
    RT r(*this);
    const auto [vals] = r.params("vals");
    const auto [out, i] = r.locals("out", "i");
    r.add(r.set(out, r.arr({})));
    r.add(r.set(i, r.I(0)));
    r.add(r.b.make_while(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(vals))),
        r.blk({r.in(IntrinsicId::ArrayPush,
                    {r.L(out), r.call("$str", {r.idx(r.L(vals), r.L(i))})}),
               r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$strall");
  }

  void emit_runtime() {
    rt_truthy(); rt_eq(); rt_mod(); rt_idiv(); rt_div(); rt_cat(); rt_str();
    rt_key(); rt_get(); rt_set(); rt_len(); rt_one(); rt_nth(); rt_append();
    rt_resume(); rt_costatus(); rt_wrap(); rt_wrapped(); rt_concat();
    rt_insert(); rt_setmt(); rt_getmt(); rt_spread(); rt_error();
    rt_mt(); rt_mm();
    rt_arith("$add", BinOp::Add, "__add");
    rt_arith("$subm", BinOp::Sub, "__sub");
    rt_arith("$mulm", BinOp::Mul, "__mul");
    rt_strall();
  }

  // A helper call. Every capture-free helper is a Func::singleton (RT::finish
  // sets it), so all these MakeClosures name the one closure the executor
  // built at the first of them -- which is what the array of pre-built
  // closures at file scope used to buy, at the cost of a synthetic variable
  // every function had to capture. One that *does* take captures is built at
  // the site that has them (RT::clos) and never reaches here; saying so
  // beats the "cannot call nil" it would otherwise be at run time.
  NodeId helper(const std::string& name, const std::vector<NodeId>& args,
                SrcPos p) {
    const auto it = rt.find(name);
    if (it == rt.end()) coreir_rt::fail("unknown runtime helper " + name, 0, 0);
    if (m.funcs[static_cast<size_t>(it->second)].num_captures != 0) {
      coreir_rt::fail("runtime helper " + name + " takes captures", 0, 0);
    }
    auto b = Builder(m).at(p);
    return b.call_value(b.make_closure(it->second, empty_cmap), args);
  }

  // ==== Pass B: emit ======================================================

  NodeId native(const std::string& name, const std::vector<NodeId>& args,
                SrcPos p) {
    auto b = Builder(m).at(p);
    return b.call_value(b.native_ref(b.declare_native(name)), args);
  }

  NodeId emit_closure(int32_t g, FnCtx& ctx, SrcPos p) {
    auto b = Builder(m).at(p);
    return b.make_closure(rs.fns[static_cast<size_t>(g)].index,
                          rs.capture_map(m, ctx.fn, g));
  }

  NodeId bind_decl(int32_t v, NodeId value, FnCtx& ctx, SrcPos p) {
    auto b = Builder(m).at(p);
    const auto& ci = rs.fns[static_cast<size_t>(ctx.fn)].cell_index;
    const auto it = ci.find(v);
    if (it != ci.end()) return b.assign(VarKind::Cell, it->second, value);
    const int32_t s = ctx.alloc_local(rs.vars[static_cast<size_t>(v)].name);
    rs.vars[static_cast<size_t>(v)].slot = s;
    return b.assign(VarKind::Local, s, value);
  }

  NodeId read_var(int32_t v, FnCtx& ctx, SrcPos p) {
    auto b = Builder(m).at(p);
    const auto [k, i] = rs.access(ctx.fn, v);
    return b.varref(k, i);
  }

  // Cells for a block's own bindings are made on entry, not at the `local`
  // that initializes them -- ES2023 14.2.3's rule, and Lua's too, and the
  // one that keeps a closure made earlier in the block holding the box the
  // later `local` fills. See examples/mini-js/README.md for the bug.
  void fresh_cells(const std::vector<int32_t>& decls, FnCtx& ctx,
                   std::vector<NodeId>& out, SrcPos p) {
    auto b = Builder(m).at(p);
    const auto& cells = rs.fns[static_cast<size_t>(ctx.fn)].cell_index;
    for (const int32_t v : decls) {
      const auto c = cells.find(v);
      if (c != cells.end()) out.push_back(b.cell_fresh(c->second));
    }
  }

  NodeId emit_block(const Ast& block, FnCtx& ctx) {
    const SrcPos p = pos_of(block);
    auto b = Builder(m).at(p);
    const int32_t mark = ctx.mark();
    std::vector<NodeId> out;
    fresh_cells(block_decls.at(&block), ctx, out, p);
    for (const auto& s : block.nodes) out.push_back(emit_stmt(*s, ctx));
    const int32_t end = ctx.release(mark);
    if (end > mark) return b.scope(mark, end, b.block(out));
    return b.block(out);
  }

  // Every value this front end can produce, plus whether it is already an
  // array of results rather than one value.
  struct Val {
    NodeId n;
    bool multi = false;
  };

  // A value in single-value context. A call answers an array under this
  // front end's convention, so its first element is what a value context
  // means by "the result".
  NodeId emit_expr(const Ast& a, FnCtx& ctx) {
    const Val v = emit_val(a, ctx);
    if (!v.multi) return v.n;
    return helper("$one", {v.n}, pos_of(a));
  }

  bool is_call_expr(const Ast& a) {
    if (a.tag != "suffixedexp"_ && a.tag != "callstmt"_) return false;
    const Ast& s = a.tag == "callstmt"_ ? *a.nodes[0] : a;
    if (s.tag != "suffixedexp"_) return false;
    return s.nodes.back()->tag == "callsfx"_ ||
           s.nodes.back()->tag == "methodsfx"_;
  }

  // An expression list as one array of values -- Lua's rule that only the
  // *last* expression expands to all of its results.
  NodeId emit_exprlist(const Ast& list, FnCtx& ctx) {
    const SrcPos p = pos_of(list);
    auto b = Builder(m).at(p);
    std::vector<NodeId> firsts;
    for (size_t i = 0; i + 1 < list.nodes.size(); ++i) {
      firsts.push_back(emit_expr(*list.nodes[i], ctx));
    }
    const Ast& last = *list.nodes.back();
    const Val lv = emit_val(last, ctx);
    if (!lv.multi) {
      firsts.push_back(lv.n);
      return b.array_lit(firsts);
    }
    if (firsts.empty()) return lv.n;
    return helper("$append", {b.array_lit(firsts), lv.n}, p);
  }

  Val emit_val(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    switch (a.tag) {
      case "number"_:
        return {
            b.literal(std::strtoll(std::string(a.token).c_str(), nullptr, 10))};
      case "float"_:
        return {b.double_literal(
            std::strtod(std::string(a.token).c_str(), nullptr))};
      case "string"_:
        return {b.str_literal(unescape(std::string(a.token)))};
      case "literal"_: {
        const std::string t(a.token);
        if (t == "true") return {b.bool_literal(true)};
        if (t == "false") return {b.bool_literal(false)};
        return {b.nil_literal()};
      }
      case "ident"_: {
        const auto it = ref_of.find(&a);
        if (it != ref_of.end()) return {read_var(it->second, ctx, p)};
        // A library function used as a value rather than called -- what
        // `type(print)` asks for. Tag::NativeRef *is* the value, so this
        // needs nothing but naming it. A namespace (`table`, `string`) is
        // not a value here, because this front end has no table to hand
        // back for one.
        const std::string g(a.token);
        if (g == "print" || g == "type" || g == "tostring") {
          Builder bb(m);
          return {bb.native_ref(bb.declare_native(g == "print" ? "print" : g),
                                p)};
        }
        fail(a, "'" + g + "' must be called here");
      }
      case "paren"_:
        // Lua's parentheses truncate a call to one value, deliberately.
        return {emit_expr(*a.nodes[0], ctx)};
      case "funcexpr"_:
        return {emit_closure(fn_of.at(&a), ctx, p)};
      case "notexp"_:
        return {b.binary(BinOp::Eq,
                         helper("$truthy", {emit_expr(*a.nodes[0], ctx)}, p),
                         b.bool_literal(false))};
      case "negexp"_:
      case "negexpr"_:
        return {b.unary(UnOp::Neg, emit_expr(*a.nodes[0], ctx))};
      case "lenexp"_:
        return {helper("$len", {emit_expr(*a.nodes[0], ctx)}, p)};
      case "powexp"_: {
        const NodeId base = emit_expr(*a.nodes[0], ctx);
        return {b.intrinsic(IntrinsicId::Pow,
                            {base, emit_expr(*a.nodes[1]->nodes[0], ctx)})};
      }
      case "orexp"_:
      case "andexp"_: {
        const bool is_or = a.tag == "orexp"_;
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i < a.nodes.size(); ++i) {
          const int32_t t = ctx.alloc_local(is_or ? "$or" : "$and");
          const NodeId rhs = emit_expr(*a.nodes[i], ctx);
          const NodeId keep = b.varref(VarKind::Local, t);
          acc = b.block({b.assign(VarKind::Local, t, acc),
                         b.make_if(helper("$truthy", {keep}, p),
                                   is_or ? keep : rhs, is_or ? rhs : keep)});
        }
        return {acc};
      }
      case "cmpexp"_: {
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i + 1 < a.nodes.size(); i += 2) {
          const Ast& op = *a.nodes[i];
          const std::string t(op.token);
          const NodeId rhs = emit_expr(*a.nodes[i + 1], ctx);
          if (t == "==") {
            acc = helper("$eq", {acc, rhs}, pos_of(op));
          } else if (t == "~=") {
            acc = b.at(pos_of(op))
                      .binary(BinOp::Eq, helper("$eq", {acc, rhs}, pos_of(op)),
                              b.bool_literal(false));
          } else {
            const BinOp o = t == "<"    ? BinOp::Lt
                            : t == "<=" ? BinOp::Le
                            : t == ">"  ? BinOp::Gt
                                        : BinOp::Ge;
            acc = b.at(pos_of(op)).binary(o, acc, rhs);
          }
        }
        return {acc};
      }
      case "concatexp"_: {
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i < a.nodes.size(); ++i) {
          acc = helper("$cat", {acc, emit_expr(*a.nodes[i], ctx)}, p);
        }
        return {acc};
      }
      case "addexp"_:
      case "mulexp"_: {
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i + 1 < a.nodes.size(); i += 2) {
          const Ast& op = *a.nodes[i];
          const std::string t(op.token);
          const NodeId rhs = emit_expr(*a.nodes[i + 1], ctx);
          const SrcPos op_p = pos_of(op);
          if (t == "+") acc = helper("$add", {acc, rhs}, op_p);
          else if (t == "-") acc = helper("$subm", {acc, rhs}, op_p);
          else if (t == "*") acc = helper("$mulm", {acc, rhs}, op_p);
          else if (t == "/") acc = helper("$div", {acc, rhs}, op_p);
          else if (t == "//") acc = helper("$idiv", {acc, rhs}, op_p);
          else acc = helper("$mod", {acc, rhs}, op_p);
        }
        return {acc};
      }
      case "tablector"_:
        return {emit_table(a, ctx)};
      case "suffixedexp"_:
        return emit_suffixed(a, a.nodes.size(), ctx);
      default:
        fail(a, "cannot evaluate " + a.name);
    }
  }

  // A table constructor. The array part numbers from 1, skipping the keyed
  // and named fields -- Lua's own rule for a mixed constructor.
  NodeId emit_table(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const int32_t t = ctx.alloc_local("$tbl");
    const NodeId T = b.varref(VarKind::Local, t);
    std::vector<NodeId> out{
        b.assign(VarKind::Local, t, b.intrinsic(IntrinsicId::MapNew, {}))};
    int64_t next = 1;
    for (size_t i = 0; i < a.nodes.size(); ++i) {
      const Ast& f = *a.nodes[i];
      if (f.tag == "keyedfield"_) {
        out.push_back(helper("$set",
                             {T, emit_expr(*f.nodes[0], ctx),
                              emit_expr(*f.nodes[1], ctx)},
                             p));
      } else if (f.tag == "namedfield"_) {
        out.push_back(helper("$set",
                             {T, b.str_literal(std::string(f.nodes[0]->token)),
                              emit_expr(*f.nodes[1], ctx)},
                             p));
      } else if (i + 1 == a.nodes.size() && is_call_expr(f)) {
        // The last positional field, when it is a call, contributes every
        // value the call returned -- Lua's rule, and the same one
        // emit_exprlist applies to an expression list.
        out.push_back(
            helper("$spread", {T, b.literal(next), emit_val(f, ctx).n}, p));
      } else {
        out.push_back(
            helper("$set", {T, b.literal(next++), emit_expr(f, ctx)}, p));
      }
    }
    out.push_back(T);
    return b.block(out);
  }

  Val emit_suffixed(const Ast& a, size_t limit, FnCtx& ctx) {
    Builder b(m);
    const Ast& prim = *a.nodes[0];
    size_t i = 1;
    Val cur;
    if (prim.tag == "ident"_ && !ref_of.count(&prim)) {
      cur = emit_library(a, limit, ctx, i);
    } else {
      cur = emit_val(prim, ctx);
    }
    for (; i < limit; ++i) {
      const Ast& sfx = *a.nodes[i];
      const SrcPos p = pos_of(sfx);
      const NodeId recv =
          cur.multi ? helper("$one", {cur.n}, p) : cur.n;
      switch (sfx.tag) {
        case "dotsfx"_:
          cur = {helper("$get",
                        {recv, b.str_literal(std::string(sfx.nodes[0]->token),
                                             p)},
                        p)};
          break;
        case "indexsfx"_:
          cur = {helper("$get",
                        {recv, emit_expr(*sfx.nodes[0], ctx)}, p)};
          break;
        case "callsfx"_: {
          std::vector<NodeId> args;
          if (!sfx.nodes.empty() && sfx.nodes[0]->tag == "args"_) {
            for (const auto& c : sfx.nodes[0]->nodes) {
              args.push_back(emit_expr(*c, ctx));
            }
          } else if (!sfx.nodes.empty()) {
            args.push_back(emit_expr(*sfx.nodes[0], ctx));  // f"lit"
          }
          const int32_t fv = ctx.alloc_local("$fn");
          const NodeId F = b.varref(VarKind::Local, fv, p);
          const int32_t mv = ctx.alloc_local("$callmm");
          const NodeId M = b.varref(VarKind::Local, mv, p);
          std::vector<NodeId> callargs{F};
          callargs.insert(callargs.end(), args.begin(), args.end());
          cur = {b.block(
                     {b.assign(VarKind::Local, fv, recv, p),
                      b.make_if(
                          b.binary(BinOp::Eq,
                                   b.intrinsic(IntrinsicId::TypeOf, {F}, p),
                                   b.str_literal("map", p), p),
                          b.block(
                              {b.assign(VarKind::Local, mv,
                                       helper("$mm",
                                              {F, b.str_literal("__call", p)},
                                              p),
                                       p),
                               b.make_if(b.binary(BinOp::Ne,
                                                  b.intrinsic(IntrinsicId::TypeOf,
                                                              {M}, p),
                                                  b.str_literal("nil", p), p),
                                         b.call_value(M, callargs, p),
                                         b.call_value(F, args, p), p)},
                              p),
                          b.call_value(F, args, p), p)},
                     p),
                 true};
          break;
        }
        default: {  // methodsfx
          const std::string name(sfx.nodes[0]->token);
          std::vector<NodeId> args;
          for (size_t k = 1; k < sfx.nodes.size(); ++k) {
            for (const auto& c : sfx.nodes[k]->nodes) {
              args.push_back(emit_expr(*c, ctx));
            }
          }
          if (is_string_method(name)) {
            // A string has no metatable here, so `("x"):rep(3)` is
            // resolved by name -- README.md lists the consequence.
            args.insert(args.begin(), recv);
            cur = {native("s_" + name, args, p), false};
            break;
          }
          const int32_t t = ctx.alloc_local("$self");
          const NodeId T = b.varref(VarKind::Local, t, p);
          std::vector<NodeId> call_args{T};
          call_args.insert(call_args.end(), args.begin(), args.end());
          cur = {b.block({b.assign(VarKind::Local, t, recv, p),
                          b.call_value(
                              helper("$get",
                                     {T, b.str_literal(name, p)}, p),
                              call_args, p)},
                         p),
                 true};
          break;
        }
      }
    }
    return cur;
  }

  // The standard library, in the shapes this front end knows. A library
  // name is not a value: `print` alone is an error, `print(x)` is a call.
  Val emit_library(const Ast& a, size_t limit, FnCtx& ctx, size_t& i) {
    Builder b(m);
    const Ast& prim = *a.nodes[0];
    const SrcPos p = pos_of(prim);
    const std::string g(prim.token);

    std::string mem;
    const Ast* callsfx = nullptr;
    size_t after = 0;
    if (i < limit && a.nodes[i]->tag == "callsfx"_) {
      callsfx = a.nodes[i].get();
      after = i + 1;
    } else if (i + 1 < limit && a.nodes[i]->tag == "dotsfx"_ &&
               a.nodes[i + 1]->tag == "callsfx"_) {
      mem = std::string(a.nodes[i]->nodes[0]->token);
      callsfx = a.nodes[i + 1].get();
      after = i + 2;
    }
    if (callsfx == nullptr) {
      fail(prim, "'" + g + "' is only supported as a call here");
    }
    const Ast* arglist =
        callsfx->nodes.empty() ? nullptr : callsfx->nodes[0].get();
    std::vector<NodeId> args;
    if (arglist != nullptr && arglist->tag == "args"_) {
      for (const auto& c : arglist->nodes) args.push_back(emit_expr(*c, ctx));
    }
    const auto a0 = [&](size_t k) {
      return k < args.size() ? args[k] : b.nil_literal(p);
    };
    i = after;

    // Everything that takes all of its arguments takes them as one array,
    // which is how `print(f())` expands a call's results the way Lua does.
    const auto all = [&]() -> NodeId {
      if (arglist == nullptr || arglist->tag != "args"_ ||
          arglist->nodes.empty()) {
        return b.array_lit({}, p);
      }
      return emit_exprlist(*arglist, ctx);
    };

    if (mem.empty()) {
      if (g == "print") {
        return {native("print", {helper("$strall", {all()}, p)}, p),
                false};
      }
      if (g == "type") return {native("type", {a0(0)}, p), false};
      if (g == "tostring") return {helper("$str", {a0(0)}, p), false};
      if (g == "setmetatable") {
        return {helper("$setmt", {a0(0), a0(1)}, p), false};
      }
      if (g == "getmetatable") {
        return {helper("$getmt", {a0(0)}, p), false};
      }
      if (g == "error") {
        return {helper("$error",
                       {a0(0), b.str_literal(chunk + ":" +
                                             std::to_string(p.line) + ": ",
                                             p)},
                       p),
                false};
      }
      if (g == "pcall") return {emit_pcall(args, ctx, p), true};
      // rawset/rawget: `$set`/`$get` without the metamethod detour --
      // `__newindex`'s own thunk needs one, on pain of calling itself.
      if (g == "rawset") {
        return {b.block({b.set_index(a0(0), helper("$key", {a0(1)}, p),
                                     a0(2), p),
                         a0(0)},
                        p),
                false};
      }
      if (g == "rawget") {
        return {b.index(a0(0), helper("$key", {a0(1)}, p), p), false};
      }
      fail(prim, "'" + g + "' is not supported here");
    }
    if (g == "coroutine") {
      if (mem == "create") {
        return {b.intrinsic(IntrinsicId::CoroCreate, {a0(0)}, p), false};
      }
      if (mem == "resume") {
        return {helper("$resume", {a0(0), a0(1)}, p), true};
      }
      if (mem == "yield") {
        return {b.array_lit(
                    {b.intrinsic(IntrinsicId::CoroYield, {a0(0)}, p)}, p),
                true};
      }
      if (mem == "status") {
        return {helper("$costatus", {a0(0)}, p), false};
      }
      if (mem == "wrap") return {helper("$wrap", {a0(0)}, p), false};
    }
    if (g == "table") {
      if (mem == "concat") {
        return {helper("$concat",
                       {a0(0), args.size() > 1 ? args[1]
                                               : b.str_literal("", p)},
                       p),
                false};
      }
      if (mem == "insert") {
        return {helper("$insert", {a0(0), a0(1)}, p), false};
      }
    }
    if (g == "string" && is_string_method(mem)) {
      return {native("s_" + mem, args, p), false};
    }
    if (g == "math") {
      if (mem == "floor") return {native("floor", {a0(0)}, p), false};
      if (mem == "type") return {native("mathtype", {a0(0)}, p), false};
    }
    if (g == "io" && mem == "write") {
      return {native("write", {all()}, p), false};
    }
    fail(prim, "'" + g + "." + mem + "' is not supported here");
  }

  // pcall: a TryCatch, which is what it is. The value of the whole thing
  // is the arm that ran, so both arms build the array Lua answers with.
  NodeId emit_pcall(const std::vector<NodeId>& args, FnCtx& ctx, SrcPos p) {
    auto b = Builder(m).at(p);
    const int32_t mark = ctx.mark();
    const int32_t exc = ctx.alloc_local("$exc");
    std::vector<NodeId> callargs(args.begin() + (args.empty() ? 0 : 1),
                                 args.end());
    const NodeId body = helper(
        "$append",
        {b.array_lit({b.bool_literal(true)}),
         b.call_value(args.empty() ? b.nil_literal() : args[0], callargs)},
        p);
    const NodeId handler =
        b.array_lit({b.bool_literal(false), b.varref(VarKind::Local, exc)});
    const NodeId out = b.make_try(exc, body, handler);
    const int32_t end = ctx.release(mark);
    return b.scope(mark, end, out);
  }

  // -- Statements ---------------------------------------------------------
  NodeId emit_stmt(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    switch (a.tag) {
      case "emptystmt"_:
        return b.block({});
      case "localfn"_:
        return bind_decl(decl_of.at(a.nodes[0].get()),
                         emit_closure(fn_of.at(&a), ctx, p), ctx, p);
      case "localdecl"_: {
        const Ast& names = *a.nodes[0];
        if (a.nodes.size() == 1) {
          std::vector<NodeId> out;
          for (const auto& id : names.nodes) {
            out.push_back(
                bind_decl(decl_of.at(id.get()), b.nil_literal(), ctx, p));
          }
          return b.block(out);
        }
        if (names.nodes.size() == 1) {
          return bind_decl(decl_of.at(names.nodes[0].get()),
                           emit_expr(*a.nodes[1]->nodes.back(), ctx) , ctx, p);
        }
        const int32_t t = ctx.alloc_local("$vals");
        const NodeId T = b.varref(VarKind::Local, t);
        std::vector<NodeId> out{
            b.assign(VarKind::Local, t, emit_exprlist(*a.nodes[1], ctx))};
        for (size_t k = 0; k < names.nodes.size(); ++k) {
          out.push_back(bind_decl(
              decl_of.at(names.nodes[k].get()),
              helper("$nth", {T, b.literal(static_cast<int64_t>(k))}, p), ctx,
              p));
        }
        return b.block(out);
      }
      case "fnstat"_:
        return emit_fnstat(a, ctx);
      case "ifstat"_:
        return emit_if(a, ctx);
      case "whilestat"_:
        return b.make_while(helper("$truthy", {emit_expr(*a.nodes[0], ctx)}, p),
                            emit_block(*a.nodes[1], ctx));
      case "repeatstat"_:
        return emit_repeat(a, ctx);
      case "fornum"_:
        return emit_fornum(a, ctx);
      case "forin"_:
        return emit_forin(a, ctx);
      case "dostat"_:
        return emit_block(*a.nodes[0], ctx);
      case "breakstat"_:
        return b.make_break();
      case "retstat"_:
        return b.make_return(a.nodes.empty() ? b.array_lit({})
                                             : emit_exprlist(*a.nodes[0], ctx));
      case "callstmt"_:
        return emit_val(*a.nodes[0], ctx).n;
      case "assign"_:
        return emit_assign(a, ctx);
      default:
        fail(a, "cannot execute " + a.name);
    }
  }

  // `elseif` chains hang off one `ifstat` node as a flat list -- cond,
  // block, elseifpart*, elsepart? -- so the nested Ifs are built from the
  // end backwards rather than by recursing into the parse tree.
  NodeId emit_if(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    std::vector<std::pair<NodeId, NodeId>> arms;
    arms.emplace_back(
        helper("$truthy", {emit_expr(*a.nodes[0], ctx)}, p),
        emit_block(*a.nodes[1], ctx));
    NodeId els;
    for (size_t i = 2; i < a.nodes.size(); ++i) {
      const Ast& c = *a.nodes[i];
      if (c.tag == "elseifpart"_) {
        arms.emplace_back(
            helper("$truthy", {emit_expr(*c.nodes[0], ctx)}, p),
            emit_block(*c.nodes[1], ctx));
      } else {
        els = emit_block(*c.nodes[0], ctx);
      }
    }
    for (size_t i = arms.size(); i-- > 0;) {
      els = b.make_if(arms[i].first, arms[i].second, els);
    }
    return els;
  }

  NodeId emit_repeat(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    // `until`'s condition sees the block's locals, so the block is not
    // given a Scope of its own -- the whole loop body is one.
    const int32_t mark = ctx.mark();
    std::vector<NodeId> body;
    fresh_cells(block_decls.at(a.nodes[0].get()), ctx, body, p);
    for (const auto& s : a.nodes[0]->nodes) body.push_back(emit_stmt(*s, ctx));
    body.push_back(
        b.make_if(helper("$truthy", {emit_expr(*a.nodes[1], ctx)}, p),
                  b.make_break(), NodeId{}));
    const int32_t end = ctx.release(mark);
    const NodeId loop = b.make_while(b.bool_literal(true), b.block(body));
    if (end > mark) return b.scope(mark, end, loop);
    return loop;
  }

  NodeId emit_fornum(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const int32_t mark = ctx.mark();
    const int32_t iv = ctx.alloc_local("$i");
    const int32_t lim = ctx.alloc_local("$limit");
    const int32_t stp = ctx.alloc_local("$step");
    const NodeId I = b.varref(VarKind::Local, iv);
    const NodeId L = b.varref(VarKind::Local, lim);
    const NodeId S = b.varref(VarKind::Local, stp);

    const Ast* step = nullptr;
    const Ast* blk = a.nodes.back().get();
    for (const auto& c : a.nodes) {
      if (c->tag == "forstep"_) step = c->nodes[0].get();
    }
    std::vector<NodeId> out{
        b.assign(VarKind::Local, iv, emit_expr(*a.nodes[1], ctx)),
        b.assign(VarKind::Local, lim, emit_expr(*a.nodes[2], ctx)),
        b.assign(VarKind::Local, stp,
                 step != nullptr ? emit_expr(*step, ctx) : b.literal(1))};

    const int32_t v = decl_of.at(a.nodes[0].get());
    const int32_t bmark = ctx.mark();
    std::vector<NodeId> body;
    // A fresh binding per iteration, so a closure made in the body keeps
    // the number it saw -- Lua's rule, and CellFresh's whole purpose.
    fresh_cells({v}, ctx, body, p);
    body.push_back(bind_decl(v, I, ctx, p));
    body.push_back(emit_block(*blk, ctx));
    body.push_back(b.assign(VarKind::Local, iv, b.binary(BinOp::Add, I, S)));
    const int32_t bend = ctx.release(bmark);

    const NodeId cond =
        b.make_if(b.binary(BinOp::Gt, S, b.literal(0)),
                  b.binary(BinOp::Le, I, L), b.binary(BinOp::Ge, I, L));
    out.push_back(b.make_while(cond, bend > bmark
                                         ? b.scope(bmark, bend, b.block(body))
                                         : b.block(body)));
    const int32_t end = ctx.release(mark);
    return b.scope(mark, end, b.block(out));
  }

  // `for k, v in ipairs(t)` and `for k, v in pairs(t)`, matched whole.
  // Lua's generic-for protocol -- an iterator function, a state and a
  // control value -- is what README.md lists as out of scope.
  NodeId emit_forin(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const Ast& list = *a.nodes[1];
    const bool is_ipairs_or_pairs =
        list.nodes.size() == 1 && is_call_expr(*list.nodes[0]) &&
        list.nodes[0]->nodes[0]->tag == "ident"_ &&
        !ref_of.count(list.nodes[0]->nodes[0].get()) &&
        (std::string(list.nodes[0]->nodes[0]->token) == "ipairs" ||
         std::string(list.nodes[0]->nodes[0]->token) == "pairs");
    if (!is_ipairs_or_pairs) return emit_forin_generic(a, ctx);
    const Ast& call = *list.nodes[0];
    const std::string which(call.nodes[0]->token);
    const Ast& args = *call.nodes[1]->nodes[0];
    if (args.nodes.empty()) fail(a, "ipairs/pairs takes a table");

    const int32_t mark = ctx.mark();
    const int32_t tv = ctx.alloc_local("$t");
    const NodeId T = b.varref(VarKind::Local, tv);
    std::vector<NodeId> out{
        b.assign(VarKind::Local, tv, emit_expr(*args.nodes[0], ctx))};

    const auto& names = a.nodes[0]->nodes;
    const int32_t kv = decl_of.at(names[0].get());
    const int32_t vv =
        names.size() > 1 ? decl_of.at(names[1].get()) : -1;

    std::vector<NodeId> body;
    if (which == "ipairs") {
      const int32_t iv = ctx.alloc_local("$i");
      const NodeId I = b.varref(VarKind::Local, iv);
      out.push_back(b.assign(VarKind::Local, iv, b.literal(1)));
      const int32_t ev = ctx.alloc_local("$e");
      const NodeId E = b.varref(VarKind::Local, ev);
      body.push_back(b.assign(VarKind::Local, ev, helper("$get", {T, I}, p)));
      body.push_back(
          b.make_if(b.binary(BinOp::Eq, b.intrinsic(IntrinsicId::TypeOf, {E}),
                             b.str_literal("nil")),
                    b.make_break(), NodeId{}));
      const int32_t bmark = ctx.mark();
      std::vector<NodeId> inner;
      fresh_cells(vv >= 0 ? std::vector<int32_t>{kv, vv}
                          : std::vector<int32_t>{kv},
                  ctx, inner, p);
      inner.push_back(bind_decl(kv, I, ctx, p));
      if (vv >= 0) inner.push_back(bind_decl(vv, E, ctx, p));
      inner.push_back(emit_block(*a.nodes[2], ctx));
      const int32_t bend = ctx.release(bmark);
      body.push_back(bend > bmark ? b.scope(bmark, bend, b.block(inner))
                                  : b.block(inner));
      body.push_back(
          b.assign(VarKind::Local, iv, b.binary(BinOp::Add, I, b.literal(1))));
    } else {
      const int32_t ks = ctx.alloc_local("$keys");
      const int32_t ix = ctx.alloc_local("$ki");
      const NodeId K = b.varref(VarKind::Local, ks);
      const NodeId X = b.varref(VarKind::Local, ix);
      out.push_back(b.assign(VarKind::Local, ks,
                             b.intrinsic(IntrinsicId::ObjectKeys, {T})));
      out.push_back(b.assign(VarKind::Local, ix, b.literal(0)));
      const int32_t cur = ctx.alloc_local("$k");
      const NodeId C = b.varref(VarKind::Local, cur);
      body.push_back(
          b.make_if(b.binary(BinOp::Ge, X, b.intrinsic(IntrinsicId::Len, {K})),
                    b.make_break(), NodeId{}));
      body.push_back(b.assign(VarKind::Local, cur, b.index(K, X)));
      body.push_back(
          b.assign(VarKind::Local, ix, b.binary(BinOp::Add, X, b.literal(1))));
      // The metatable hangs on the same map, under a key no program can
      // spell -- so `pairs` has to step over it.
      body.push_back(b.make_if(helper("$eq", {C, b.str_literal(kMetaKey)}, p),
                               b.make_continue(), NodeId{}));
      const int32_t bmark = ctx.mark();
      std::vector<NodeId> inner;
      fresh_cells(vv >= 0 ? std::vector<int32_t>{kv, vv}
                          : std::vector<int32_t>{kv},
                  ctx, inner, p);
      inner.push_back(bind_decl(kv, C, ctx, p));
      if (vv >= 0) {
        inner.push_back(bind_decl(vv, helper("$get", {T, C}, p), ctx, p));
      }
      inner.push_back(emit_block(*a.nodes[2], ctx));
      const int32_t bend = ctx.release(bmark);
      body.push_back(bend > bmark ? b.scope(bmark, bend, b.block(inner))
                                  : b.block(inner));
    }
    out.push_back(b.make_while(b.bool_literal(true), b.block(body)));
    const int32_t end = ctx.release(mark);
    return b.scope(mark, end, b.block(out));
  }

  // The real generic-`for` protocol: `exprlist` is (iterator function,
  // state, initial control value), padded with nil past three and
  // truncated past three, exactly as Lua's own -- and `ipairs`/`pairs`
  // above are simply the two library functions this front end special-
  // cases for speed rather than compiling through this path too.
  NodeId emit_forin_generic(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const int32_t mark = ctx.mark();
    const int32_t triple = ctx.alloc_local("$triple");
    const NodeId TR = b.varref(VarKind::Local, triple);
    const int32_t fv = ctx.alloc_local("$iterf");
    const NodeId F = b.varref(VarKind::Local, fv);
    const int32_t sv = ctx.alloc_local("$state");
    const NodeId S = b.varref(VarKind::Local, sv);
    const int32_t cv = ctx.alloc_local("$control");
    const NodeId C = b.varref(VarKind::Local, cv);
    std::vector<NodeId> out{
        b.assign(VarKind::Local, triple, emit_exprlist(*a.nodes[1], ctx)),
        b.assign(VarKind::Local, fv, helper("$nth", {TR, b.literal(0)}, p)),
        b.assign(VarKind::Local, sv, helper("$nth", {TR, b.literal(1)}, p)),
        b.assign(VarKind::Local, cv, helper("$nth", {TR, b.literal(2)}, p))};

    const int32_t rv = ctx.alloc_local("$results");
    const NodeId R = b.varref(VarKind::Local, rv);
    const int32_t first = ctx.alloc_local("$first");
    const NodeId FI = b.varref(VarKind::Local, first);
    std::vector<NodeId> body{
        b.assign(VarKind::Local, rv, b.call_value(F, {S, C})),
        b.assign(VarKind::Local, first, helper("$one", {R}, p)),
        b.make_if(b.binary(BinOp::Eq, b.intrinsic(IntrinsicId::TypeOf, {FI}),
                           b.str_literal("nil")),
                  b.make_break(), NodeId{}),
        b.assign(VarKind::Local, cv, FI)};

    const auto& names = a.nodes[0]->nodes;
    std::vector<int32_t> decls;
    for (const auto& n : names) decls.push_back(decl_of.at(n.get()));
    const int32_t bmark = ctx.mark();
    std::vector<NodeId> inner;
    fresh_cells(decls, ctx, inner, p);
    for (size_t k = 0; k < decls.size(); ++k) {
      inner.push_back(bind_decl(
          decls[k], helper("$nth", {R, b.literal(static_cast<int64_t>(k))}, p),
          ctx, p));
    }
    inner.push_back(emit_block(*a.nodes[2], ctx));
    const int32_t bend = ctx.release(bmark);
    body.push_back(bend > bmark ? b.scope(bmark, bend, b.block(inner))
                                : b.block(inner));

    out.push_back(b.make_while(b.bool_literal(true), b.block(body)));
    const int32_t end = ctx.release(mark);
    return b.scope(mark, end, b.block(out));
  }

  NodeId emit_fnstat(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const Ast& nm = *a.nodes[0];
    const NodeId fn = emit_closure(fn_of.at(&a), ctx, p);
    if (nm.nodes.size() == 1) {
      const auto it = ref_of.find(nm.nodes[0].get());
      if (it == ref_of.end()) fail(nm, "cannot assign to this name");
      const auto [k, i] = rs.access(ctx.fn, it->second);
      return b.assign(k, i, fn);
    }
    NodeId recv = emit_expr(*nm.nodes[0], ctx);
    for (size_t i = 1; i + 1 < nm.nodes.size(); ++i) {
      recv = helper(
          "$get",
          {recv, b.str_literal(std::string(nm.nodes[i]->nodes[0]->token))}, p);
    }
    return helper(
        "$set",
        {recv, b.str_literal(std::string(nm.nodes.back()->nodes[0]->token)),
         fn},
        p);
  }

  NodeId emit_assign(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const Ast& targets = *a.nodes[0];
    const Ast& values = *a.nodes[1];
    if (targets.nodes.size() == 1) {
      return emit_store(*targets.nodes[0],
                        emit_expr(*values.nodes.back(), ctx), ctx, p);
    }
    const int32_t t = ctx.alloc_local("$vals");
    const NodeId T = b.varref(VarKind::Local, t);
    std::vector<NodeId> out{
        b.assign(VarKind::Local, t, emit_exprlist(values, ctx))};
    for (size_t k = 0; k < targets.nodes.size(); ++k) {
      out.push_back(emit_store(
          *targets.nodes[k],
          helper("$nth", {T, b.literal(static_cast<int64_t>(k))}, p), ctx, p));
    }
    return b.block(out);
  }

  NodeId emit_store(const Ast& target, NodeId value, FnCtx& ctx, SrcPos p) {
    auto b = Builder(m).at(p);
    if (target.tag == "ident"_) {
      const auto it = ref_of.find(&target);
      if (it == ref_of.end()) {
        fail(target, "'" + std::string(target.token) + "' is not a variable");
      }
      const auto [k, i] = rs.access(ctx.fn, it->second);
      return b.assign(k, i, value);
    }
    if (target.tag != "suffixedexp"_ || target.nodes.size() < 2) {
      fail(target, "cannot assign to this expression");
    }
    const Ast& last = *target.nodes.back();
    NodeId key;
    if (last.tag == "dotsfx"_) {
      key = b.str_literal(std::string(last.nodes[0]->token));
    } else if (last.tag == "indexsfx"_) {
      key = emit_expr(*last.nodes[0], ctx);
    } else {
      fail(last, "cannot assign to a call");
    }
    const Val recv = emit_suffixed(target, target.nodes.size() - 1, ctx);
    const NodeId R =
        recv.multi ? helper("$one", {recv.n}, p) : recv.n;
    return helper("$set", {R, key, value}, p);
  }

  // -- One function's body -------------------------------------------------
  void emit_fn(int32_t f) {
    const FnInfo& fi = fns[static_cast<size_t>(f)];
    const Resolver::Fn& rf = rs.fns[static_cast<size_t>(f)];
    FnCtx ctx;
    ctx.fn = f;
    const SrcPos p = fi.body != nullptr ? pos_of(*fi.body) : SrcPos{0, 0};
    auto b = Builder(m).at(p);

    std::vector<NodeId> pre;
    for (const int32_t v : fi.params) {
      const int32_t s = ctx.alloc_local(rs.vars[static_cast<size_t>(v)].name);
      rs.vars[static_cast<size_t>(v)].slot = s;
      const auto it = rf.cell_index.find(v);
      if (it != rf.cell_index.end()) {
        pre.push_back(b.cell_fresh(it->second));
        pre.push_back(
            b.assign(VarKind::Cell, it->second, b.varref(VarKind::Local, s)));
      }
    }
    const int32_t nparams = ctx.mark();
    const NodeId body = emit_block(*fi.body, ctx);

    std::vector<NodeId> stmts;
    stmts.insert(stmts.end(), pre.begin(), pre.end());
    stmts.push_back(body);
    // Falling off the end returns no values, which under this front end's
    // convention is the empty array rather than nil.
    stmts.push_back(b.make_return(b.array_lit({})));

    Func fn;
    fn.name = fi.name;
    fn.num_params = nparams;
    fn.num_locals = ctx.high_local;
    fn.local_names = ctx.names();
    fn.num_cells = rs.num_cells(f);
    fn.lenient_arity = true;  // Lua's own arity rule: missing args are nil
    // The headline. Lua guarantees that `return f(...)` reuses the frame,
    // so samples/tailcalls.lua recurses far past max_call_depth and has to
    // still work.
    fn.tail_calls = true;
    fn.num_captures = m.funcs[static_cast<size_t>(rf.index)].num_captures;
    fn.capture_names = m.funcs[static_cast<size_t>(rf.index)].capture_names;
    fn.body = b.scope(0, nparams, b.block(stmts));
    m.funcs[static_cast<size_t>(rf.index)] = std::move(fn);
  }

  Module build(const Ast& program) {
    const int32_t top = new_fn(-1, "main");
    fns[static_cast<size_t>(top)].body = program.nodes[0].get();

    rs.push_scope();
    resolve_block(*program.nodes[0], top);
    rs.pop_scope();

    m.funcs.push_back({});
    rs.fns[static_cast<size_t>(top)].index = 0;
    for (const std::string& n : rt_names()) {
      rt[n] = static_cast<int32_t>(m.funcs.size());
      m.funcs.push_back({});
    }
    for (size_t f = 1; f < fns.size(); ++f) {
      rs.fns[f].index = static_cast<int32_t>(m.funcs.size());
      m.funcs.push_back({});
    }


    rs.number_captures(m);
    empty_cmap = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back({});
    emit_runtime();

    for (size_t f = 0; f < fns.size(); ++f) {
      emit_fn(static_cast<int32_t>(f));
    }
    return std::move(m);
  }
};

// ==== The standard library, as host functions ==============================
//
// Everything here is either output or a string operation -- the two things
// an IR-level helper cannot do. Note in particular `tostring`: Lua formats
// a float with "%.14g" and then makes sure the result still looks like a
// float, which is neither to_display's shortest round trip nor anything
// this IR could compute. Table operations are *not* here (see $concat and
// $insert): a table is a Map, and walking one from C++ would tie this front
// end to MapObj's layout.

std::string lua_tostring(const Value& v) {
  switch (v.tag()) {
    case ValueTag::Nil: return "nil";
    case ValueTag::Bool: return v.as_bool() ? "true" : "false";
    case ValueTag::Int: return std::to_string(v.as_int());
    case ValueTag::Double: {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.14g", v.as_double());
      std::string s(buf);
      // "%.14g" of 4.0 is "4"; Lua then makes it look like a float again.
      if (s.find_first_of(".eEni") == std::string::npos) s += ".0";
      return s;
    }
    case ValueTag::Str: return v.as_str();
    case ValueTag::Map: return "table";
    case ValueTag::Coroutine: return "thread";
    case ValueTag::Func:
    case ValueTag::Native: return "function";
    default: return "value";
  }
}

bool nat_print(NativeCall& c) {
  std::string out;
  const auto& items = c.arg(0).as_array()->items;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) out += "\t";
    out += lua_tostring(items[i]);
  }
  coreir_rt_out_str(out.data(), static_cast<int64_t>(out.size()));
  c.result = Value();
  return true;
}

bool nat_write(NativeCall& c) {
  std::string out;
  for (const Value& v : c.arg(0).as_array()->items) out += lua_tostring(v);
  coreir_rt_out_raw(out.data(), static_cast<int64_t>(out.size()));
  c.result = Value();
  return true;
}

bool nat_tostring(NativeCall& c) {
  c.result = Value::make_str(lua_tostring(c.arg(0)));
  return true;
}

// Lua's type names, which are not the VM's: a table is a Map here and a
// coroutine is a "thread" there.
bool nat_type(NativeCall& c) {
  const char* n = "userdata";
  switch (c.arg(0).tag()) {
    case ValueTag::Nil: n = "nil"; break;
    case ValueTag::Bool: n = "boolean"; break;
    case ValueTag::Int:
    case ValueTag::Double: n = "number"; break;
    case ValueTag::Str: n = "string"; break;
    case ValueTag::Map: n = "table"; break;
    case ValueTag::Coroutine: n = "thread"; break;
    case ValueTag::Func:
    case ValueTag::Native: n = "function"; break;
    default: break;
  }
  c.result = Value::make_str(n);
  return true;
}

bool nat_mathtype(NativeCall& c) {
  if (c.arg(0).is_int()) {
    c.result = Value::make_str("integer");
  } else if (c.arg(0).is_double()) {
    c.result = Value::make_str("float");
  } else {
    c.result = Value();
  }
  return true;
}

bool nat_floor(NativeCall& c) {
  const Value& v = c.arg(0);
  if (v.is_int()) {
    c.result = v;
    return true;
  }
  if (!v.is_double()) {
    c.error = c.trap("floor wants a number");
    return false;
  }
  const double d = v.as_double();
  double f = static_cast<double>(static_cast<int64_t>(d));
  if (f > d) f -= 1.0;
  c.result = Value::make_int(static_cast<int64_t>(f));
  return true;
}

// Lua string indices are 1-based and a negative one counts from the end.
int64_t str_index(int64_t i, int64_t n) {
  if (i < 0) i = n + i + 1;
  if (i < 1) i = 1;
  return i;
}

bool nat_s_sub(NativeCall& c) {
  if (!c.arg(0).is_str()) {
    c.error = c.trap("sub wants a string");
    return false;
  }
  const std::string& s = c.arg(0).as_str();
  const auto n = static_cast<int64_t>(s.size());
  int64_t i = c.arg(1).is_int() ? c.arg(1).as_int() : 1;
  int64_t j = c.argc > 2 && c.arg(2).is_int() ? c.arg(2).as_int() : -1;
  i = str_index(i, n);
  if (j < 0) j = n + j + 1;
  if (j > n) j = n;
  c.result = Value::make_str(i > j ? std::string()
                                   : s.substr(static_cast<size_t>(i - 1),
                                              static_cast<size_t>(j - i + 1)));
  return true;
}

bool nat_s_rep(NativeCall& c) {
  if (!c.arg(0).is_str() || !c.arg(1).is_int()) {
    c.error = c.trap("rep wants a string and an integer");
    return false;
  }
  std::string out;
  for (int64_t k = 0; k < c.arg(1).as_int(); ++k) out += c.arg(0).as_str();
  c.result = Value::make_str(std::move(out));
  return true;
}

bool nat_s_upper(NativeCall& c) {
  std::string s = c.arg(0).as_str();
  for (char& ch : s) ch = static_cast<char>(std::toupper(ch));
  c.result = Value::make_str(std::move(s));
  return true;
}

bool nat_s_lower(NativeCall& c) {
  std::string s = c.arg(0).as_str();
  for (char& ch : s) ch = static_cast<char>(std::tolower(ch));
  c.result = Value::make_str(std::move(s));
  return true;
}

bool nat_s_len(NativeCall& c) {
  c.result = Value::make_int(static_cast<int64_t>(c.arg(0).as_str().size()));
  return true;
}

bool nat_s_byte(NativeCall& c) {
  const std::string& s = c.arg(0).as_str();
  const int64_t i = c.argc > 1 && c.arg(1).is_int() ? c.arg(1).as_int() : 1;
  if (i < 1 || i > static_cast<int64_t>(s.size())) {
    c.result = Value();
    return true;
  }
  c.result = Value::make_int(
      static_cast<unsigned char>(s[static_cast<size_t>(i - 1)]));
  return true;
}

}  // namespace

const std::vector<vm::NativeDef>& stdlib() {
  static const std::vector<vm::NativeDef> defs = {
      {"print", 1, nat_print, nullptr},
      {"write", 1, nat_write, nullptr},
      {"tostring", 1, nat_tostring, nullptr},
      {"type", 1, nat_type, nullptr},
      {"mathtype", 1, nat_mathtype, nullptr},
      {"floor", 1, nat_floor, nullptr},
      {"s_sub", -1, nat_s_sub, nullptr},
      {"s_rep", -1, nat_s_rep, nullptr},
      {"s_upper", -1, nat_s_upper, nullptr},
      {"s_lower", -1, nat_s_lower, nullptr},
      {"s_len", -1, nat_s_len, nullptr},
      {"s_byte", -1, nat_s_byte, nullptr},
  };
  return defs;
}

Module bind_source(const std::string& source, const std::string& chunkname) {
  parser p;
  p.set_logger([](size_t line, size_t col, const std::string& msg,
                  const std::string&) {
    coreir_rt::fail(msg, static_cast<uint32_t>(line),
                    static_cast<uint32_t>(col));
  });
  if (!p.load_grammar(kGrammar)) coreir_rt::fail("invalid grammar", 0, 0);
  p.enable_ast();

  std::shared_ptr<Ast> ast;
  if (!p.parse(source, ast)) coreir_rt::fail("syntax error", 0, 0);
  ast = p.optimize_ast(ast);

  Binder b;
  b.chunk = chunkname;
  Module m = b.build(*ast);

  if (auto err = verify(m)) {
    coreir_rt::fail("internal error: malformed IR: " + *err, 0, 0);
  }
  return m;
}

}  // namespace mini_lua
