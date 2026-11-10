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
//     BinOp::Mod is C's. So BinOp::FloorMod and BinOp::FloorDiv.
//   * **`==` across types is false**, where BinOp::Eq traps. So `$eq`.
//
// A table is a value-keyed Map (IntrinsicId::MapNew), which is what a Lua
// table is: one container with an array part and a hash part, keyed by
// anything. Its metatable is the map's meta (MetaGet/MetaSet), the slot no
// key reaches: Lua's metatable is the model that slot was made after.

#include "binder.h"

#include "double_format.h"
#include "escapes.h"
#include "literals.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <peglib.h>

#include "grammar.h"
#include "runtime_parts.h"
#include "vmlib.h"

using namespace peg;
using namespace peg::udl;
using namespace coreir;
using namespace runtime_parts;

namespace mini_lua {
std::string& chunk_name();
namespace {

SrcPos pos_of(const Ast& a) {
  return {static_cast<uint32_t>(a.line), static_cast<uint32_t>(a.column)};
}

[[noreturn]] void fail(const Ast& a, const std::string& msg) {
  coreir_rt::fail(msg, static_cast<uint32_t>(a.line),
                  static_cast<uint32_t>(a.column));
}

// The escapes Lua's `simple` table does not name, with u.i at the letter
// after the backslash (see escapes::decode). A line break is a newline.
void lua_escape(escapes::Unescaper& u) {
  const std::string_view raw = u.raw;
  const char e = raw[u.i];
  uint32_t v = 0;
  if (e == '\n' || e == '\r') {
    u.out.push_back('\n');
    u.i = escapes::skip_line_break(raw, u.i);
  } else if (e == 'z') {  // skips the whitespace after it, line breaks too
    ++u.i;
    while (u.i < raw.size() &&
           std::isspace(static_cast<unsigned char>(raw[u.i]))) {
      ++u.i;
    }
  } else if (e == 'x') {
    if (escapes::hex_digits(raw, u.i + 1, 2, v) != 2) {
      u.fail_at("hexadecimal digit expected");
    } else {
      u.out.push_back(static_cast<char>(v));
      u.i += 3;
    }
  } else if (e >= '0' && e <= '9') {
    u.i += escapes::digits(raw, u.i, 10, 3, v);
    if (v > 255) {
      u.fail_at("decimal escape too large");
    } else {
      u.out.push_back(static_cast<char>(v));
    }
  } else if (e == 'u') {
    if (u.i + 1 >= raw.size() || raw[u.i + 1] != '{') {
      u.fail_at("missing '{'");
    } else if (const size_t n = escapes::braced_hex(raw, u.i + 1, 64, v);
               n == 0) {
      u.fail_at("hexadecimal digit expected, or missing '}'");
    } else if (v > 0x7FFFFFFFu) {
      u.fail_at("UTF-8 value too large");
    } else {
      escapes::append_utf8(u.out, v);
      u.i += 3 + n;
    }
  } else {
    u.fail_at(std::string("invalid escape sequence '\\") + e + "'");
  }
}

// A string literal's body as the bytes it stands for -- Lua 5.4's rules
// (manual 3.1): the one-letter escapes, a backslash before a line break (a
// newline), `\z` (skips the whitespace after it), `\xXX` (two hex digits,
// a byte), `\ddd` (one to three decimal digits, at most 255), `\u{X...}`
// (a code point up to 2^31 - 1, in UTF-8). Anything else after a backslash
// is an error, as in Lua, which has no pass-through.
std::string unescape(std::string_view raw, const Ast& at) {
  static constexpr escapes::Simple kSimple[] = {
      {'a', '\a'}, {'b', '\b'}, {'f', '\f'}, {'n', '\n'}, {'r', '\r'},
      {'t', '\t'}, {'v', '\v'}, {'\\', '\\'}, {'"', '"'}, {'\'', '\''},
  };
  return escapes::decode(raw, at, kSimple, lua_escape);
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

// How many arguments each library function that takes one value each
// reads, by its qualified name: kReadsEvery for one that reads all it is
// given, the string natives, which check their own. The arguments past
// those still run -- runtime_parts' reading. A function missing here is
// not bound at all, rather than lowered without its count and so losing
// the arguments it does not read. One that takes every value as one array
// (print, io.write, select, pcall, table.pack) has none past them, and is
// not asked.
constexpr size_t kReadsEvery = std::string::npos;
std::optional<size_t> library_reads(const std::string& name) {
  static const std::map<std::string, size_t> reads = {
      {"type", 1},         {"tostring", 1},      {"setmetatable", 2},
      {"getmetatable", 1}, {"error", 2},         {"rawset", 3},
      {"rawget", 2},       {"coroutine.create", 1},
      {"coroutine.resume", 2},                   {"coroutine.yield", 1},
      {"coroutine.status", 1},                   {"coroutine.wrap", 1},
      {"table.concat", 2}, {"table.insert", 3},  {"table.unpack", 3},
      {"string.upper", 1}, {"string.lower", 1},
      {"string.rep", kReadsEvery},               {"string.sub", kReadsEvery},
      {"string.len", kReadsEvery},               {"string.byte", kReadsEvery},
      {"math.floor", 1},   {"math.type", 1},
  };
  const auto it = reads.find(name);
  if (it == reads.end()) return std::nullopt;
  return it->second;
}

// A `:name(...)` whose receiver is a string: one of the string library's.
// Resolved by name at bind time, because a subset without a string
// metatable has no runtime lookup to do it with -- README.md lists the
// consequence.
bool is_string_method(const std::string& n) {
  return library_reads("string." + n).has_value();
}

// What this front end knows about a function beyond the closure conversion
// coreir::Resolver does -- kept parallel to Resolver::fns, which owns the
// parent link, the Module::funcs index and the capture/cell numbering.
struct FnInfo {
  std::string name = "?";
  std::vector<int32_t> params;
  // A vararg function's `...`: its last param, a Func::rest_param under a
  // name no Lua identifier can be, holding the extra arguments as an
  // array. -1 for a function that takes none.
  int32_t varargs = -1;
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
  HelperTable rt{m};  // the runtime helpers, by name
  Operators ops{rt, operators()};  // the VM's operations standing in for them
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
      const bool va = p->tag == "vararg"_;
      const int32_t v = rs.declare(va ? "..." : std::string(p->token), f);
      decl_of[p.get()] = v;
      fns[static_cast<size_t>(f)].params.push_back(v);
      if (va) fns[static_cast<size_t>(f)].varargs = v;
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
      case "vararg"_: {
        // The enclosing function's own -- a nested function does not see
        // its parent's, which is Lua's rule too. The main chunk is vararg
        // in Lua, and given no arguments here (emit_val).
        const int32_t va = fns[static_cast<size_t>(fn)].varargs;
        if (va >= 0) {
          ref_of[&a] = va;
        } else if (fn != 0) {
          fail(a, "cannot use '...' outside a vararg function");
        }
        return;
      }
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
        "$truthy", "$eq",   "$div",  "$cat",  "$str",
        "$get",  "$set",  "$len",  "$one",  "$nth",   "$append",
        "$resume", "$costatus", "$wrap", "$wrapped", "$concat", "$insert",
        "$spread", "$error", "$mm", "$add", "$subm", "$mulm",
        "$select", "$pack", "$unpack", "$insertat", "$calltable",
    };
    return names;
  }

  // A key a table takes: an int, a string, a bool or a float, a float that
  // is a whole number being the int. That rule is the VM's alone: the Index
  // and SetIndex of $get and $set have kIntegralKey, and their fallbacks,
  // rawget and rawset take the key through IntegralKey. A nil is no key,
  // and NaN, which the VM leaves to the fallback, is none either.
  static uint32_t lua_keys() {
    return tags_named({"int", "string", "bool", "double"});
  }

  // Lua's rules that are the VM's own operation on some values: the VM's
  // answer where the two agree, the helper's on the rest (runtime_parts.h's
  // Operators).
  //
  //   $truthy        a condition: everything but a number is
  //                  Value::truthy()'s (nil_false_tags), a number $truthy's
  //                  -- 0, which the VM calls false, is true in Lua.
  //   $add $subm     `+`, `-` and `*` on two numbers, integers wrapping
  //   $mulm          and a float on either side making a float, as Lua's
  //                  do; anything else looks for a metamethod.
  //   $div           `/` on two floats, IEEE as Lua's is; an integer,
  //                  which `/` makes a float first, is $div's.
  //   $cat           `..` on two strings; a number, written the way
  //                  tostring writes it, is $cat's.
  //   $eq            `==` (equality_tags), a table's being the helper's,
  //                  for __eq.
  //   $one           the first of a call's results, Index's on an array
  //                  that has one and $one's nil on an empty one.
  //   $get $set      `t[k]` and `t[k] = v` on a table, by a key of
  //                  lua_keys: a key the table has, and a
  //                  miss when it has no metatable (kMissNil; with one the
  //                  miss is __index's or __newindex's), and a store of
  //                  anything but nil, which removes the key.
  static Operators::Table operators() {
    return {
        {"$truthy", VmOp::unary(UnOp::Truth, nil_false_tags())},
        {"$add", VmOp::binary(BinOp::Add, number_tags())},
        {"$subm", VmOp::binary(BinOp::Sub, number_tags())},
        {"$mulm", VmOp::binary(BinOp::Mul, number_tags())},
        {"$div", VmOp::binary(BinOp::Div, tags_named("double"))},
        {"$cat", VmOp::binary(BinOp::Add, tags_named("string"))},
        {"$eq", VmOp::binary(BinOp::Eq, equality_tags())},
        {"$one", VmOp::index(tags_named("array"), 0, tags_named("int"))},
        {"$get", VmOp::index(tags_named("map"), kMissNil | kIntegralKey,
                             lua_keys())},
        {"$set", VmOp::set_index(tags_named("map"), kMissNil | kIntegralKey,
                                 lua_keys(),
                                 kAllTags & ~tags_named("nil"))},
    };
  }

  // One helper of the runtime above.
  struct RT : HelperWriter {
    explicit RT(Binder& bd) : HelperWriter(bd.rt) {}
  };

  // Only nil and false are false. Value::truthy() calls 0 false and says in
  // its own comment that this is the disagreement that makes truthiness a
  // language's decision: "JavaScript calls both [""and NaN] falsy, Lua calls
  // neither."
  void rt_truthy() {
    RT r(*this);
    truthy_nil_false(r);
    r.finish("$truthy");
  }

  // `==`: numbers compare numerically across integer and float, values of
  // different types are unequal rather than an error (which is where
  // binop_fault stops), and everything else is identity.
  void rt_eq() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [m] = r.locals("m");
    r.add(r.iff(
        is_number(r, r.L(a)),
        r.iff(is_number(r, r.L(b)), r.ret(r.bin(BinOp::Eq, r.L(a), r.L(b))))));
    r.add(r.iff(r.bin(BinOp::Ne, r.typ(a), r.typ(b)), r.ret(r.Bo(false))));
    r.add(r.iff(r.is(r.typ(a), "nil"), r.ret(r.Bo(true))));
    r.add(
        r.iff(r.is(r.typ(a), "bool"), r.ret(r.bin(BinOp::Eq, r.L(a), r.L(b)))));
    r.add(r.iff(r.is(r.typ(a), "string"),
                r.ret(r.bin(BinOp::Eq, r.L(a), r.L(b)))));
    // The same table is equal to itself without asking __eq, which Lua
    // consults only for two different ones.
    r.add(r.iff(
        r.is(r.typ(a), "map"),
        r.blk(
            {r.iff(r.in(IntrinsicId::Same, {r.L(a), r.L(b)}), r.ret(r.Bo(true))),
             r.set(m, r.call("$mm", {r.L(a), r.S("__eq")})),
             r.iff(r.isnt(r.typ(m), "nil"),
                   r.ret(r.call(
                       "$truthy",
                       {r.call("$one",
                               {r.b.call_value(r.L(m), {r.L(a), r.L(b)})})}))),
             r.set(m, r.call("$mm", {r.L(b), r.S("__eq")})),
             r.iff(r.isnt(r.typ(m), "nil"),
                   r.ret(r.call(
                       "$truthy",
                       {r.call("$one", {r.b.call_value(
                                           r.L(m), {r.L(a), r.L(b)})})})))})));
    r.add(r.ret(r.in(IntrinsicId::Same, {r.L(a), r.L(b)})));
    r.finish("$eq");
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
    // `__tostring` is the host's to find (lua_tostring_mm), so print reaches
    // the same answer from C++.
    r.add(r.iff(r.is(r.typ(v), "string"), r.ret(r.L(v))));
    r.add(r.ret(r.nat("tostring", {r.L(v)})));
    r.finish("$str");
  }

  // One metamethod off a value's metatable -- the walk every metamethod
  // here shares. `__index` (rt_get, above) predates this and stays written
  // inline; the newer ones (`__add`/`__sub`/`__mul`/`__eq`/`__call`) all go
  // through this. `__tostring` is the host's (lua_tostring_mm).
  void rt_mm() {
    RT r(*this);
    const auto [v, name] = r.params("v", "name");
    const auto [mt] = r.locals("mt");
    // MetaGet answers nil for a value that is no table, and for a table
    // with no metatable.
    r.add(r.set(mt, r.meta(r.L(v))));
    r.add(r.iff(r.is(r.typ(mt), "nil"), r.ret(r.Nil())));
    r.add(r.ret(r.idx(r.L(mt), r.L(name))));
    r.finish("$mm");
  }

  // `+`, `-`, `*` on two numbers stay `BinOp`; on anything else, the
  // matching metamethod off either operand -- Lua's own precedence, left
  // operand first.
  void rt_arith(const std::string& name, BinOp op, const std::string& mm) {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [mv] = r.locals("m");
    r.add(r.iff(is_number(r, r.L(a)),
                r.iff(is_number(r, r.L(b)), r.ret(r.bin(op, r.L(a), r.L(b))))));
    const auto try_mm = [&](FuncWriter::Slot recv) {
      return std::vector<NodeId>{
          r.set(mv, r.call("$mm", {r.L(recv), r.S(mm)})),
          r.iff(r.isnt(r.typ(mv), "nil"),
                r.ret(r.call("$one",
                             {r.b.call_value(r.L(mv), {r.L(a), r.L(b)})})))};
    };
    for (const NodeId n : try_mm(a)) r.add(n);
    for (const NodeId n : try_mm(b)) r.add(n);
    // Neither a number nor a metamethod's: the VM's trap of the operator,
    // whose conversion names the operand that is not a number, the left one
    // first, as Lua's own message does (convert_trap).
    r.add(r.ret(r.bin(op, r.L(a), r.L(b))));
    r.finish(name);
  }

  // `..` joins strings and numbers, a number written the way tostring
  // writes it. Anything else is an error -- __tostring plays no part, and
  // __concat is not in this subset -- named by the operand that is
  // neither, the left one first, as Lua's message names it.
  void rt_cat() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto ok = [&](FuncWriter::Slot v) {
      return r.either(r.is(r.typ(v), "string"), is_number(r, r.L(v)));
    };
    r.add(r.iff(r.both(ok(a), ok(b)),
                r.ret(r.bin(BinOp::Add, r.call("$str", {r.L(a)}),
                            r.call("$str", {r.L(b)})))));
    // The VM has no concatenation to trap, so the host function fails it
    // with the same kind of trap the conversion gives a line.
    r.add(r.ret(r.nat("concat_fail", {r.L(a), r.L(b)})));
    r.finish("$cat");
  }

  // Reading a table, with the one metamethod this subset has. `__index`
  // may be a table (look there instead) or a function (call it) -- Lua's
  // own rule, written here because a metatable is a language's idea and
  // not the IR's.
  void rt_get() {
    RT r(*this);
    const auto [t, k] = r.params("t", "k");
    const auto [key, mt, idx] = r.locals("key", "mt", "idx");
    // A string has no keys of its own here (its methods are another path);
    // anything else that is no table is the VM's trap of the index.
    r.add(r.iff(r.isnt(r.typ(t), "map"),
                r.iff(r.is(r.typ(t), "string"), r.ret(r.Nil()),
                      r.ret(r.idx(r.L(t), r.L(k))))));
    r.add(r.set(key, r.in(IntrinsicId::IntegralKey, {r.L(k)})));
    r.add(r.iff(r.has(r.L(t), r.L(key)), r.ret(r.idx(r.L(t), r.L(key)))));
    r.add(r.set(mt, r.meta(r.L(t))));
    r.add(r.iff(
        r.isnt(r.typ(mt), "nil"),
        r.iff(r.has(r.L(mt), r.S("__index")),
              r.blk({r.set(idx, r.idx(r.L(mt), r.S("__index"))),
                     r.iff(r.is(r.typ(idx), "map"),
                           r.ret(r.call("$get", {r.L(idx), r.L(key)}))),
                     // The key as it was written: `t[1.0]` calls __index
                     // with 1.0, not the integer it is looked up as.
                     r.ret(r.call("$one",
                                  {r.b.call_value(r.L(idx),
                                                  {r.L(t), r.L(k)})}))}))));
    r.add(r.ret(r.Nil()));
    r.finish("$get");
  }

  // Writing: assigning nil removes the key, which is what makes `#t` and
  // `pairs` agree with Lua about what is in the table.
  void rt_set() {
    RT r(*this);
    const auto [t, k, v] = r.params("t", "k", "v");
    const auto [key, m] = r.locals("key", "m");
    r.add(r.iff(r.isnt(r.typ(t), "map"),
                r.blk({r.b.set_index(r.L(t), r.L(k), r.L(v)), r.ret(r.L(v))})));
    r.add(r.set(key, r.in(IntrinsicId::IntegralKey, {r.L(k)})));
    r.add(r.iff(r.is(r.typ(v), "nil"),
                r.blk({r.in(IntrinsicId::ObjectRemove, {r.L(t), r.L(key)}),
                       r.ret(r.L(v))})));
    // __newindex fires only for a key the raw table does not already
    // have -- Lua's own rule, so an overwrite never consults it.
    r.add(r.iff(
        r.b.unary(UnOp::Not, r.has(r.L(t), r.L(key))),
        r.blk(
            {r.set(m, r.call("$mm", {r.L(t), r.S("__newindex")})),
             r.iff(
                 r.isnt(r.typ(m), "nil"),
                 r.blk({r.iff(r.is(r.typ(m), "map"),
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
    r.add(r.iff(r.isnt(r.typ(v), "map"), r.ret(r.len(r.L(v)))));
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
        r.ret(r.arr({r.Bo(false), converted(r.b, r.L(exc))}))));
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
    // A failed resume answers {false, err}; wrap raises the err itself.
    r.add(r.iff(r.b.unary(UnOp::Not, r.idx(r.L(r_), r.I(0))),
                r.b.make_throw(r.idx(r.L(r_), r.I(1)))));
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

  // Calling a table: its __call metamethod, with the table in front of
  // the arguments -- which have run by now, and arrive as one array. A
  // table without one is called anyway, for the executor's own error.
  void rt_calltable() {
    RT r(*this);
    const auto [t, args] = r.params("t", "args");
    const auto [mm] = r.locals("mm");
    r.add(r.set(mm, r.call("$mm", {r.L(t), r.S("__call")})));
    r.add(r.iff(r.is(r.typ(mm), "nil"),
                r.ret(r.b.call_spread(r.L(t), {}, r.L(args)))));
    r.add(r.ret(r.b.call_spread(r.L(mm), {r.L(t)}, r.L(args))));
    r.finish("$calltable");
  }

  // table.insert(t, pos, v): every element from pos up moves one place,
  // through $get/$set as lua5.4's own moves go through the metamethods,
  // and v goes in the gap. A pos outside 1..#t+1 is the error Lua raises.
  void rt_insertat() {
    RT r(*this);
    const auto [t, pos, v] = r.params("t", "pos", "v");
    const auto [k] = r.locals("k");
    r.add(r.set(k, r.call("$len", {r.L(t)})));
    r.add(r.iff(
        r.either(r.bin(BinOp::Lt, r.L(pos), r.I(1)),
                 r.bin(BinOp::Gt, r.L(pos), r.bin(BinOp::Add, r.L(k), r.I(1)))),
        r.nat("arg_fail", {r.S("bad argument #2 to 'insert' (position out of "
                               "bounds)")})));
    r.add(r.wh(r.bin(BinOp::Ge, r.L(k), r.L(pos)),
               r.blk({r.call("$set", {r.L(t), r.bin(BinOp::Add, r.L(k), r.I(1)),
                                      r.call("$get", {r.L(t), r.L(k)})}),
                      r.set(k, r.bin(BinOp::Sub, r.L(k), r.I(1)))})));
    r.add(r.call("$set", {r.L(t), r.L(pos), r.L(v)}));
    r.add(r.ret(r.Nil()));
    r.finish("$insertat");
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
    r.add(r.iff(r.is(r.typ(v), "string"),
                r.b.make_throw(r.bin(BinOp::Add, r.L(where), r.L(v)))));
    r.add(r.b.make_throw(r.L(v)));
    r.finish("$error");
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

  // select('#', ...) is how many values follow; select(n, ...) is the
  // values from the nth of them on, counting from the end when n is
  // negative. `all` is every argument, n included, so that n may be among
  // the values an expanding argument brought (`select(...)`).
  void rt_select() {
    RT r(*this);
    const auto [all] = r.params("all");
    const auto [n, k, len] = r.locals("n", "k", "len");
    r.add(r.iff(r.bin(BinOp::Eq, r.len(r.L(all)), r.I(0)),
                r.nat("arg_fail", {r.S("bad argument #1 to 'select' (number "
                                      "expected, got no value)")})));
    r.add(r.set(n, r.idx(r.L(all), r.I(0))));
    r.add(r.set(len, r.bin(BinOp::Sub, r.len(r.L(all)), r.I(1))));
    r.add(r.iff(r.both(r.is(r.typ(n), "string"),
                       r.bin(BinOp::Eq, r.L(n), r.S("#"))),
                r.ret(r.arr({r.L(len)}))));
    r.add(r.set(k, r.L(n)));
    r.add(r.iff(r.bin(BinOp::Lt, r.L(k), r.I(0)),
                r.set(k, r.bin(BinOp::Add, r.L(len),
                               r.bin(BinOp::Add, r.L(k), r.I(1))))));
    r.add(r.iff(r.bin(BinOp::Lt, r.L(k), r.I(1)),
                r.nat("arg_fail", {r.S("bad argument #1 to 'select' (index out "
                                      "of range)")})));
    r.add(r.iff(r.bin(BinOp::Gt, r.L(k), r.L(len)), r.ret(r.arr({}))));
    // The kth value is all[k], n being all[0].
    r.add(r.ret(r.in(IntrinsicId::ArraySlice,
                     {r.L(all), r.L(k), r.len(r.L(all))})));
    r.finish("$select");
  }

  // table.pack: the values at 1..n, and n itself -- which is what keeps a
  // nil among them countable.
  void rt_pack() {
    RT r(*this);
    const auto [vals] = r.params("vals");
    const auto [t] = r.locals("t");
    r.add(r.set(t, r.in(IntrinsicId::MapNew, {})));
    r.add(r.call("$spread", {r.L(t), r.I(1), r.L(vals)}));
    r.add(r.call("$set", {r.L(t), r.S("n"), r.len(r.L(vals))}));
    r.add(r.ret(r.L(t)));
    r.finish("$pack");
  }

  // table.unpack(t, i, j): t[i]..t[j] as values, i defaulting to 1 and j
  // to #t. Each read is `$get`, so __index answers for a missing one, as
  // in Lua 5.4.
  void rt_unpack() {
    RT r(*this);
    const auto [t, i, j] = r.params("t", "i", "j");
    const auto [out, k] = r.locals("out", "k");
    r.add(r.iff(r.is(r.typ(i), "nil"), r.set(i, r.I(1))));
    r.add(r.iff(r.is(r.typ(j), "nil"), r.set(j, r.call("$len", {r.L(t)}))));
    r.add(r.set(out, r.arr({})));
    r.add(r.set(k, r.L(i)));
    r.add(r.wh(r.bin(BinOp::Le, r.L(k), r.L(j)),
               r.blk({r.push(r.L(out), r.call("$get", {r.L(t), r.L(k)})),
                      r.set(k, r.bin(BinOp::Add, r.L(k), r.I(1)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$unpack");
  }

  void emit_runtime() {
    rt_truthy(); rt_eq(); rt_div(); rt_cat(); rt_str();
    rt_get(); rt_set(); rt_len(); rt_one(); rt_nth(); rt_append();
    rt_resume(); rt_costatus(); rt_wrap(); rt_wrapped(); rt_concat();
    rt_insert(); rt_insertat(); rt_calltable(); rt_spread(); rt_error();
    rt_mm();
    rt_select(); rt_pack(); rt_unpack();
    rt_arith("$add", BinOp::Add, "__add");
    rt_arith("$subm", BinOp::Sub, "__sub");
    rt_arith("$mulm", BinOp::Mul, "__mul");
  }

  // ==== Pass B: emit ======================================================

  // A value used as a condition: $truthy's entry in operators().
  NodeId truthy(NodeId v, SrcPos p) { return ops.call("$truthy", {v}, p); }

  // The first of a call's results, which come as an array: $one's entry in
  // operators(), indexing it at 0.
  NodeId one(NodeId results, SrcPos p) {
    return ops.call("$one", {results, Builder(m).at(p).literal(0)}, p);
  }

  NodeId native(const std::string& name, const std::vector<NodeId>& args,
                SrcPos p) {
    auto b = Builder(m).at(p);
    return b.call_value(b.native_ref(b.declare_native(name)), args);
  }

  // A string library function, by name: the two case maps are the VM's
  // own StrUpper/StrLower (ASCII, as Lua's are in the C locale), the rest
  // host functions.
  NodeId string_call(const std::string& name, const std::vector<NodeId>& args,
                     SrcPos p) {
    if (name == "upper" || name == "lower") {
      auto b = Builder(m).at(p);
      return b.intrinsic(
          name == "upper" ? IntrinsicId::StrUpper : IntrinsicId::StrLower,
          {args.empty() ? b.nil_literal() : args[0]});
    }
    return native("s_" + name, args, p);
  }

  NodeId emit_closure(int32_t g, FnCtx& ctx, SrcPos p) {
    return rs.closure(m, ctx.fn, g, p);
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
    return rs.read(m, ctx.fn, v, p);
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
    return one(v.n, pos_of(a));
  }

  bool is_call_expr(const Ast& a) {
    if (a.tag != "suffixedexp"_ && a.tag != "callstmt"_) return false;
    const Ast& s = a.tag == "callstmt"_ ? *a.nodes[0] : a;
    if (s.tag != "suffixedexp"_) return false;
    return s.nodes.back()->tag == "callsfx"_ ||
           s.nodes.back()->tag == "methodsfx"_;
  }

  // Expressions under Lua's rule for the end of a list: every one but the
  // last is one value, and the last, when it has several -- a call, or
  // `...` -- is all of them. `rest` is that last one's array of values,
  // and invalid when it is one value too.
  struct Values {
    std::vector<NodeId> firsts;
    NodeId rest;
  };

  Values emit_values(const std::vector<std::shared_ptr<Ast>>& xs,
                     FnCtx& ctx) {
    Values out;
    for (size_t i = 0; i < xs.size(); ++i) {
      if (i + 1 < xs.size()) {
        out.firsts.push_back(emit_expr(*xs[i], ctx));
        continue;
      }
      const Val v = emit_val(*xs[i], ctx);
      if (v.multi) {
        out.rest = v.n;
      } else {
        out.firsts.push_back(v.n);
      }
    }
    return out;
  }

  // Those values as one array.
  NodeId values_array(const Values& vs, SrcPos p) {
    auto b = Builder(m).at(p);
    if (!vs.rest.valid()) return b.array_lit(vs.firsts);
    if (vs.firsts.empty()) return vs.rest;
    return rt.call("$append", {b.array_lit(vs.firsts), vs.rest}, p);
  }

  // Those values as a call's arguments, after `before` -- a method's
  // receiver, or the table a __call metamethod is handed. An expanding
  // last argument is spread (Tag::CallSpread): how many values it has is
  // not known until it runs.
  NodeId call_with(NodeId callee, std::vector<NodeId> before,
                   const Values& args, SrcPos p) {
    auto b = Builder(m).at(p);
    before.insert(before.end(), args.firsts.begin(), args.firsts.end());
    if (args.rest.valid()) return b.call_spread(callee, before, args.rest);
    return b.call_value(callee, before);
  }

  // A call's argument list, whichever form it takes: `(a, b)`, or the
  // single string of `f"lit"` (a strargsfx around it). Every call reads
  // its arguments here, a library one included.
  Values emit_args(const Ast* arglist, FnCtx& ctx) {
    if (arglist == nullptr) return {};
    if (arglist->tag == "args"_) return emit_values(arglist->nodes, ctx);
    return {{emit_expr(*arglist->nodes[0], ctx)}, NodeId{}};
  }

  // An expression list as one array of values.
  NodeId emit_exprlist(const Ast& list, FnCtx& ctx) {
    return values_array(emit_values(list.nodes, ctx), pos_of(list));
  }

  Val emit_val(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    switch (a.tag) {
      case "number"_: {
        // A decimal integer that does not fit an int64 is a float in Lua,
        // read from the same digits: 9223372036854775808 is 2^63.
        const std::string digits(a.token);
        const auto n = literals::parse_int_literal(digits);
        return {n.fits
                    ? b.literal(n.value)
                    : b.double_literal(literals::parse_float_literal(digits))};
      }
      case "float"_:
        return {b.double_literal(literals::parse_float_literal(a.token))};
      case "string"_:
        return {b.str_literal(unescape(a.token, a))};
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
        // `type(print)` asks for, and what `local p = print; p(x)` calls
        // the way it calls any function (nat_print_value). A namespace
        // (`table`, `string`) is not a value here, because this front end
        // has no table to hand back for one.
        const std::string g(a.token);
        if (g == "print" || g == "type" || g == "tostring") {
          Builder bb(m);
          return {bb.native_ref(bb.declare_native(g + "_value"), p)};
        }
        fail(a, "'" + g + "' must be called here");
      }
      case "paren"_:
        // Lua's parentheses truncate a call to one value, deliberately.
        return {emit_expr(*a.nodes[0], ctx)};
      case "vararg"_: {
        // The array the rest param collected, which nothing writes to:
        // whatever keeps it -- a table constructor, select, a spread call
        // -- copies it. The main chunk's has nothing in it.
        const auto it = ref_of.find(&a);
        if (it == ref_of.end()) return {b.array_lit({}), true};
        return {read_var(it->second, ctx, p), true};
      }
      case "funcexpr"_:
        return {emit_closure(fn_of.at(&a), ctx, p)};
      case "notexp"_:
        return {b.unary(UnOp::Not, truthy(emit_expr(*a.nodes[0], ctx), p))};
      case "negexp"_:
      case "negexpr"_:
        return {b.unary(UnOp::Neg, emit_expr(*a.nodes[0], ctx))};
      case "lenexp"_:
        return {rt.call("$len", {emit_expr(*a.nodes[0], ctx)}, p)};
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
          const NodeId rhs = emit_expr(*a.nodes[i], ctx);
          acc = b.let(acc, [&](auto keep) {
            return b.make_if(truthy(keep(), p), is_or ? keep() : rhs,
                             is_or ? rhs : keep());
          });
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
            acc = ops.call("$eq", {acc, rhs}, pos_of(op));
          } else if (t == "~=") {
            const NodeId eq = ops.call("$eq", {acc, rhs}, pos_of(op));
            acc = b.at(pos_of(op)).unary(UnOp::Not, eq);
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
          acc = ops.call("$cat", {acc, emit_expr(*a.nodes[i], ctx)}, p);
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
          if (t == "+") acc = ops.call("$add", {acc, rhs}, op_p);
          else if (t == "-") acc = ops.call("$subm", {acc, rhs}, op_p);
          else if (t == "*") acc = ops.call("$mulm", {acc, rhs}, op_p);
          else if (t == "/") acc = ops.call("$div", {acc, rhs}, op_p);
          // Lua's `//` and `%` floor, on a float as on an integer: they
          // are FloorDiv and FloorMod, which vmlib.h makes Lua's
          // luai_numidiv and luai_nummod, with no helper behind them. What those refuse --
          // an integer zero divisor, the least integer over -1, what is no
          // number -- they refuse in their own words, at the divisor;
          // Lua's own (an error, a wrapped quotient, arithmetic on a
          // string) are not this subset's.
          else if (t == "//")
            acc = b.at(op_p).binary(BinOp::FloorDiv, acc, rhs);
          else acc = b.at(op_p).binary(BinOp::FloorMod, acc, rhs);
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
    return b.let(b.intrinsic(IntrinsicId::MapNew, {}), [&](auto T) {
      std::vector<NodeId> out;
      int64_t next = 1;
      for (size_t i = 0; i < a.nodes.size(); ++i) {
        const Ast& f = *a.nodes[i];
        if (f.tag == "keyedfield"_) {
          out.push_back(ops.call("$set",
                                {T(), emit_expr(*f.nodes[0], ctx),
                                 emit_expr(*f.nodes[1], ctx)},
                                p));
        } else if (f.tag == "namedfield"_) {
          const std::string name(f.nodes[0]->token);
          out.push_back(ops.call(
              "$set", {T(), b.str_literal(name), emit_expr(*f.nodes[1], ctx)},
              p));
        } else if (i + 1 == a.nodes.size()) {
          // The last positional field, when it is a call or `...`,
          // contributes every value it has -- Lua's rule, and the same one
          // emit_values applies to an expression list.
          const Val v = emit_val(f, ctx);
          out.push_back(
              v.multi ? rt.call("$spread", {T(), b.literal(next), v.n}, p)
                      : ops.call("$set", {T(), b.literal(next++), v.n}, p));
        } else {
          out.push_back(
              ops.call("$set", {T(), b.literal(next++), emit_expr(f, ctx)}, p));
        }
      }
      out.push_back(T());
      return b.block(out);
    });
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
          cur.multi ? one(cur.n, p) : cur.n;
      switch (sfx.tag) {
        case "dotsfx"_:
          cur = {ops.call("$get",
                         {recv, b.str_literal(std::string(sfx.nodes[0]->token),
                                              p)},
                         p)};
          break;
        case "indexsfx"_:
          cur = {ops.call("$get",
                         {recv, emit_expr(*sfx.nodes[0], ctx)}, p)};
          break;
        case "callsfx"_: {
          const Values args =
              emit_args(sfx.nodes.empty() ? nullptr : sfx.nodes[0].get(), ctx);
          cur = {call_any(recv, {}, args, p), true};
          break;
        }
        default: {  // methodsfx
          const std::string name(sfx.nodes[0]->token);
          Values args = emit_args(
              sfx.nodes.size() > 1 ? sfx.nodes[1].get() : nullptr, ctx);
          if (is_string_method(name)) {
            // A string has no metatable here, so `("x"):rep(3)` is
            // resolved by name -- README.md lists the consequence. Like
            // every library call here, it takes its arguments one value
            // each.
            std::vector<NodeId> one_each{recv};
            one_each.insert(one_each.end(), args.firsts.begin(),
                            args.firsts.end());
            if (args.rest.valid()) {
              one_each.push_back(one(args.rest, p));
            }
            // Every argument runs, read or not (reading).
            auto bp = b.at(p);
            cur = {reading(bp, one_each, *library_reads("string." + name),
                           [&](const std::vector<NodeId>& read) {
                             return string_call(name, read, p);
                           }),
                   false};
            break;
          }
          cur = {b.let(recv,
                       [&](auto self) {
                         return call_any(
                             ops.call("$get", {self(), b.str_literal(name, p)},
                                     p),
                             {self()}, args, p);
                       },
                       p),
                 true};
          break;
        }
      }
    }
    return cur;
  }

  // A call of whatever `callee` turns out to be, with `before` ahead of
  // the arguments (a method's self): a function directly, or a table
  // through $calltable, which looks its __call up only after the
  // arguments have run -- as Lua does, so an argument that changes it is
  // seen. That path collects the arguments into an array; a plain call
  // never takes it, and `$fn` is read on both before any argument runs.
  NodeId call_any(NodeId callee, const std::vector<NodeId>& before,
                  const Values& args, SrcPos p) {
    auto b = Builder(m).at(p);
    // The callee, then `before` and the arguments, each run once and held
    // by a Let, which both calls read.
    std::vector<NodeId> values{callee};
    values.insert(values.end(), before.begin(), before.end());
    values.insert(values.end(), args.firsts.begin(), args.firsts.end());
    if (args.rest.valid()) values.push_back(args.rest);
    return b.lets(values, [&](const std::vector<Builder::Temp>& t) {
      const auto held = [&](size_t from, size_t n) {
        return Builder::read_all(t, from, n);
      };
      const size_t nb = before.size();
      const size_t na = args.firsts.size();
      const auto rest = [&] {
        return args.rest.valid() ? t.back()() : NodeId{};
      };
      Values all{held(1, nb + na), rest()};
      const Values given{held(1 + nb, na), rest()};
      return b.make_if(
          b.binary(BinOp::Eq, b.intrinsic(IntrinsicId::TypeOf, {t[0]()}),
                   b.str_literal("map")),
          rt.call("$calltable", {t[0](), values_array(all, p)}, p),
          call_with(t[0](), held(1, nb), given, p));
    });
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
    // The arguments are read once, the way any call reads them, and each
    // library function takes them in the shape it needs: all of them as
    // one array (all), which is how `print(f())` prints every result, or
    // one value each (a0, below).
    const Ast* arglist =
        callsfx->nodes.empty() ? nullptr : callsfx->nodes[0].get();
    const Values vals = emit_args(arglist, ctx);
    const auto all = [&] {
      return values_array(vals, arglist != nullptr ? pos_of(*arglist) : p);
    };
    i = after;
    if (mem.empty()) {
      if (g == "print") return {native("print", {all()}, p), false};
      if (g == "pcall") return {emit_pcall(vals, ctx, p), true};
      if (g == "select") return {rt.call("$select", {all()}, p), true};
    }
    if (g == "table" && mem == "pack") {
      return {rt.call("$pack", {all()}, p), false};
    }
    if (g == "io" && mem == "write") {
      return {native("write", {all()}, p), false};
    }
    // The rest take one value each, the expanding last argument cut to its
    // first. Lua runs every argument of a call, whether the function reads
    // it or not: `type(1, f())` still calls f, after the 1 and before type.
    const std::string name = mem.empty() ? g : g + "." + mem;
    const std::optional<size_t> reads = library_reads(name);
    if (!reads) fail(prim, "'" + name + "' is not supported here");
    std::vector<NodeId> args = vals.firsts;
    if (vals.rest.valid()) args.push_back(one(vals.rest, p));
    // The arguments it reads, held by `reading` while the call is
    // built over them; every argument runs, read or not.
    const std::vector<NodeId>* read = &args;
    const auto a0 = [&](size_t k) {
      return k < read->size() ? (*read)[k] : b.nil_literal(p);
    };
    const auto dispatch = [&]() -> Val {
      if (mem.empty()) {
        if (g == "type") return {native("type", {a0(0)}, p), false};
        if (g == "tostring") return {rt.call("$str", {a0(0)}, p), false};
        if (g == "setmetatable") {
          return {b.intrinsic(IntrinsicId::MetaSet, {a0(0), a0(1)}, p),
                  false};
        }
        if (g == "getmetatable") {
          return {b.intrinsic(IntrinsicId::MetaGet, {a0(0)}, p), false};
        }
        if (g == "error") return {emit_error(arglist, args.size(), a0(0), a0(1), p), false};
        // rawset/rawget: `$set`/`$get` without the metamethod detour --
        // `__newindex`'s own thunk needs one, on pain of calling itself.
        if (g == "rawset") {
          // The table is both where the store goes and the answer, so a
          // Let holds it rather than its being evaluated twice.
          return {b.let(a0(0),
                        [&](auto t) {
                          return b.block(
                              {b.set_index(t(), b.intrinsic(IntrinsicId::IntegralKey, {a0(1)}, p),
                                           a0(2), p),
                               t()},
                              p);
                        },
                        p),
                  false};
        }
        if (g == "rawget") {
          return {b.index(a0(0), b.intrinsic(IntrinsicId::IntegralKey, {a0(1)}, p), p), false};
        }
        fail(prim, "'" + g + "' is not supported here");
      }
      if (g == "coroutine") {
        if (mem == "create") {
          return {b.intrinsic(IntrinsicId::CoroCreate, {a0(0)}, p), false};
        }
        if (mem == "resume") {
          return {rt.call("$resume", {a0(0), a0(1)}, p), true};
        }
        if (mem == "yield") {
          return {b.array_lit(
                      {b.intrinsic(IntrinsicId::CoroYield, {a0(0)}, p)}, p),
                  true};
        }
        if (mem == "status") {
          return {rt.call("$costatus", {a0(0)}, p), false};
        }
        if (mem == "wrap") return {rt.call("$wrap", {a0(0)}, p), false};
      }
      if (g == "table") {
        if (mem == "concat") {
          return {rt.call("$concat",
                          {a0(0), args.size() > 1 ? args[1]
                                                  : b.str_literal("", p)},
                          p),
                  false};
        }
        // table.insert(t, v) appends; table.insert(t, pos, v) opens a
        // gap at pos.
        if (mem == "insert") {
          if (args.size() >= 3) {
            return {rt.call("$insertat", {a0(0), a0(1), a0(2)}, p), false};
          }
          return {rt.call("$insert", {a0(0), a0(1)}, p), false};
        }
        if (mem == "unpack") {
          return {rt.call("$unpack", {a0(0), a0(1), a0(2)}, p), true};
        }
      }
      if (g == "string" && is_string_method(mem)) {
        return {string_call(mem, args, p), false};
      }
      if (g == "math") {
        if (mem == "floor") return {native("floor", {a0(0)}, p), false};
        if (mem == "type") return {native("mathtype", {a0(0)}, p), false};
      }
      fail(prim, "'" + g + "." + mem + "' is not supported here");
    };
    Val v{};
    auto bp = b.at(p);
    const NodeId n = reading(
        bp, args, *reads, [&](const std::vector<NodeId>& held) {
          read = &held;
          v = dispatch();
          return v.n;
        });
    v.n = n;
    return v;
  }

  // pcall: a TryCatch, which is what it is. The value of the whole thing
  // is the arm that ran, so both arms build the array Lua answers with.
  // pcall(f, ...): f called with the rest, by the rule any call's
  // arguments follow -- f included, when it is the first of the expanding
  // last argument's values (`pcall(...)`).
  NodeId emit_pcall(const Values& args, FnCtx& ctx, SrcPos p) {
    auto b = Builder(m).at(p);
    const int32_t mark = ctx.mark();
    // f and its arguments run first, outside the protection: an error in
    // an argument is the caller's own, not the call's, as in Lua. Both are
    // held by Lets, read by the protected call.
    const auto held = [&](const std::function<NodeId(NodeId, NodeId)>& k) {
      if (!args.firsts.empty()) {
        return b.lets(
            {args.firsts[0],
             values_array({{args.firsts.begin() + 1, args.firsts.end()},
                           args.rest},
                          p)},
            [&](const std::vector<Builder::Temp>& t) {
              return k(t[0](), t[1]());
            });
      }
      if (args.rest.valid()) {
        // `pcall(...)`: f is the first of the expanding argument's values.
        return b.let(args.rest, [&](auto all) {
          return b.lets(
              {one(all(), p),
               rt.call("$select", {values_array({{b.literal(2)}, all()}, p)},
                       p)},
              [&](const std::vector<Builder::Temp>& t) {
                return k(t[0](), t[1]());
              });
        });
      }
      return k(b.nil_literal(), b.array_lit({}));
    };
    const int32_t exc = ctx.alloc_local("$exc");
    const NodeId E = b.varref(VarKind::Local, exc);
    // A trap the executor raised is converted to what Lua's pcall answers
    // for the same fault: a string with the chunk and line in front
    // (runtime_parts.h converted(); the words are convert_trap's). Any
    // other error value is answered as it is.
    const NodeId handler =
        b.array_lit({b.bool_literal(false), converted(b, E)});
    const NodeId call = held([&](NodeId f, NodeId a) {
      return b.make_try(exc,
                        rt.call("$append",
                                {b.array_lit({b.bool_literal(true)}),
                                 b.call_spread(f, {}, a)},
                                p),
                        handler);
    });
    const int32_t end = ctx.release(mark);
    return b.scope(mark, end, call);
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
        // One name and one value binds it directly; one name and several
        // values still evaluates them all, and takes the first.
        if (names.nodes.size() == 1 && a.nodes[1]->nodes.size() == 1) {
          return bind_decl(decl_of.at(names.nodes[0].get()),
                           emit_expr(*a.nodes[1]->nodes[0], ctx), ctx, p);
        }
        return b.let(emit_exprlist(*a.nodes[1], ctx), [&](auto vals) {
          std::vector<NodeId> out;
          for (size_t k = 0; k < names.nodes.size(); ++k) {
            out.push_back(bind_decl(
                decl_of.at(names.nodes[k].get()),
                rt.call("$nth", {vals(), b.literal(static_cast<int64_t>(k))},
                        p),
                ctx, p));
          }
          return b.block(out);
        });
      }
      case "fnstat"_:
        return emit_fnstat(a, ctx);
      case "ifstat"_:
        return emit_if(a, ctx);
      case "whilestat"_:
        return b.make_while(
            truthy(emit_expr(*a.nodes[0], ctx), p),
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
        truthy(emit_expr(*a.nodes[0], ctx), p),
        emit_block(*a.nodes[1], ctx));
    NodeId els;
    for (size_t i = 2; i < a.nodes.size(); ++i) {
      const Ast& c = *a.nodes[i];
      if (c.tag == "elseifpart"_) {
        arms.emplace_back(
            truthy(emit_expr(*c.nodes[0], ctx), p),
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
        b.make_if(truthy(emit_expr(*a.nodes[1], ctx), p),
                  b.make_break(), NodeId{}));
    const int32_t end = ctx.release(mark);
    const NodeId loop = b.make_while(b.bool_literal(true), b.block(body));
    if (end > mark) return b.scope(mark, end, loop);
    return loop;
  }

  // `error(v [, level])`. Level 1, the default, is `$error`: the line of
  // this call. Level 0 is the same with no position. Any other level is
  // the native's, called at no position of its own so that the position it
  // blames (NativeExec::attributed of nothing) is the call that entered the
  // function holding the `error`, which is level 2; the call's own position
  // goes along as an argument for a level that turns out to be 1.
  NodeId emit_error(const Ast* arglist, size_t argc, NodeId value,
                    NodeId level, SrcPos p) {
    auto b = Builder(m).at(p);
    const auto level_is = [&](std::string_view digits) {
      const Ast* l = arglist != nullptr && arglist->nodes.size() == 2
                         ? arglist->nodes[1].get()
                         : nullptr;
      return l != nullptr && l->tag == "number"_ && l->token == digits;
    };
    if (argc < 2 || level_is("1")) {
      return rt.call("$error",
                     {value, b.str_literal(chunk + ":" +
                                         std::to_string(p.line) + ": ")},
                     p);
    }
    if (level_is("0")) {
      return rt.call("$error", {value, b.str_literal("")}, p);
    }
    return native("error_level",
                  {value, level, b.literal(static_cast<int64_t>(p.line)),
                   b.literal(static_cast<int64_t>(p.col))},
                  SrcPos{});
  }

  // A numeric literal, or a negated one, that is not zero.
  static bool nonzero_literal(const Ast& e) {
    switch (e.tag) {
      case "number"_: {
        const std::string digits(e.token);
        const auto n = literals::parse_int_literal(digits);
        return !n.fits || n.value != 0;
      }
      case "float"_:
        return literals::parse_float_literal(e.token) != 0;
      case "negexp"_:
        return nonzero_literal(*e.nodes[0]);
      default:
        return false;
    }
  }

  NodeId emit_fornum(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const int32_t mark = ctx.mark();
    const int32_t iv = ctx.alloc_local("$i");
    const auto I = [&] { return b.varref(VarKind::Local, iv); };

    const Ast* step = nullptr;
    const Ast* blk = a.nodes.back().get();
    for (const auto& c : a.nodes) {
      if (c->tag == "forstep"_) step = c->nodes[0].get();
    }
    // The start, the limit and the step run once, in that order, held by
    // Lets; the counter changes every step, so it is a local.
    const NodeId loop = b.lets(
        {emit_expr(*a.nodes[1], ctx), emit_expr(*a.nodes[2], ctx),
         step != nullptr ? emit_expr(*step, ctx) : b.literal(1)},
        [&](const std::vector<Builder::Temp>& t) {
          const auto& limit = t[1];
          const auto& by = t[2];
          const int32_t v = decl_of.at(a.nodes[0].get());
          const int32_t bmark = ctx.mark();
          std::vector<NodeId> body;
          // A fresh binding per iteration, so a closure made in the body
          // keeps the number it saw -- Lua's rule, and CellFresh's whole
          // purpose.
          fresh_cells({v}, ctx, body, p);
          body.push_back(bind_decl(v, I(), ctx, p));
          body.push_back(emit_block(*blk, ctx));
          body.push_back(
              b.assign(VarKind::Local, iv, b.binary(BinOp::Add, I(), by())));
          const int32_t bend = ctx.release(bmark);
          const NodeId cond =
              b.make_if(b.binary(BinOp::Gt, by(), b.literal(0)),
                        b.binary(BinOp::Le, I(), limit()),
                        b.binary(BinOp::Ge, I(), limit()));
          // A step of zero is an error before the first iteration, as in
          // Lua; a literal that is not zero cannot be one, and costs
          // the loop nothing.
          std::vector<NodeId> pre;
          if (step == nullptr || !nonzero_literal(*step)) {
            pre.push_back(b.make_if(
                b.binary(BinOp::Eq, by(), b.literal(0)),
                native("for_step", {}, p), NodeId{}));
          }
          pre.push_back(b.assign(VarKind::Local, iv, t[0]()));
          pre.push_back(
              b.make_while(cond, bend > bmark
                                      ? b.scope(bmark, bend, b.block(body))
                                      : b.block(body)));
          return b.block(pre);
        });
    const int32_t end = ctx.release(mark);
    return b.scope(mark, end, loop);
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
    const auto& names = a.nodes[0]->nodes;
    const int32_t kv = decl_of.at(names[0].get());
    const int32_t vv =
        names.size() > 1 ? decl_of.at(names[1].get()) : -1;
    // One step's bindings and the loop's body, the step's key and value
    // read from what holds them; the value is read only when there is a
    // variable for it.
    const auto step = [&](NodeId key, const std::function<NodeId()>& value) {
      const int32_t bmark = ctx.mark();
      std::vector<NodeId> inner;
      fresh_cells(vv >= 0 ? std::vector<int32_t>{kv, vv}
                          : std::vector<int32_t>{kv},
                  ctx, inner, p);
      inner.push_back(bind_decl(kv, key, ctx, p));
      if (vv >= 0) inner.push_back(bind_decl(vv, value(), ctx, p));
      inner.push_back(emit_block(*a.nodes[2], ctx));
      const int32_t bend = ctx.release(bmark);
      return bend > bmark ? b.scope(bmark, bend, b.block(inner))
                          : b.block(inner);
    };
    // The table is held by a Let for the whole loop; ipairs and pairs
    // read it alone, and anything after it still runs.
    const NodeId loop =
        b.let(emit_expr(*args.nodes[0], ctx), [&](auto table) {
          std::vector<NodeId> out;
          for (size_t k = 1; k < args.nodes.size(); ++k) {
            out.push_back(emit_expr(*args.nodes[k], ctx));
          }
          if (which == "ipairs") {
            const int32_t iv = ctx.alloc_local("$i");
            const auto I = [&] { return b.varref(VarKind::Local, iv); };
            out.push_back(b.assign(VarKind::Local, iv, b.literal(1)));
            // Each element, held by a Let for its test and its binding.
            const NodeId body =
                b.let(ops.call("$get", {table(), I()}, p), [&](auto e) {
                  return b.block(
                      {b.make_if(
                           b.binary(BinOp::Eq,
                                    b.intrinsic(IntrinsicId::TypeOf, {e()}),
                                    b.str_literal("nil")),
                           b.make_break(), NodeId{}),
                       step(I(), [&] { return e(); }),
                       b.assign(VarKind::Local, iv,
                                b.binary(BinOp::Add, I(), b.literal(1)))});
                });
            out.push_back(b.make_while(b.bool_literal(true), body));
            return b.block(out);
          }
          // pairs: the keys, taken once and held, walked by an index.
          out.push_back(b.let(
              b.intrinsic(IntrinsicId::ObjectKeys, {table()}), [&](auto keys) {
                const int32_t ix = ctx.alloc_local("$ki");
                const auto X = [&] { return b.varref(VarKind::Local, ix); };
                const NodeId body = b.block(
                    {b.make_if(
                         b.binary(BinOp::Ge, X(),
                                  b.intrinsic(IntrinsicId::Len, {keys()})),
                         b.make_break(), NodeId{}),
                     b.let(b.index(keys(), X()), [&](auto key) {
                       return b.block(
                           {b.assign(VarKind::Local, ix,
                                     b.binary(BinOp::Add, X(), b.literal(1))),
                            step(key(), [&] {
                              return ops.call("$get", {table(), key()}, p);
                            })});
                     })});
                return b.block(
                    {b.assign(VarKind::Local, ix, b.literal(0)),
                     b.make_while(b.bool_literal(true), body)});
              }));
          return b.block(out);
        });
    const int32_t end = ctx.release(mark);
    return b.scope(mark, end, loop);
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
    // The function and the state are held by Lets for the loop, and each
    // step's results, and the first of them, for the step; the control
    // value changes every step, so it is a local.
    const int32_t cv = ctx.alloc_local("$control");
    const auto C = [&] { return b.varref(VarKind::Local, cv); };
    const auto& names = a.nodes[0]->nodes;
    std::vector<int32_t> decls;
    for (const auto& n : names) decls.push_back(decl_of.at(n.get()));
    const NodeId exprs = emit_exprlist(*a.nodes[1], ctx);
    const NodeId loop = b.let(exprs, [&](auto triple) {
      const auto nth = [&](NodeId v, int64_t k) {
        return rt.call("$nth", {v, b.literal(k)}, p);
      };
      return b.lets({nth(triple(), 0), nth(triple(), 1)},
                    [&](const std::vector<Builder::Temp>& fs) {
        const NodeId body =
            b.let(b.call_value(fs[0](), {fs[1](), C()}), [&](auto results) {
              return b.let(one(results(), p), [&](auto first) {
                const int32_t bmark = ctx.mark();
                std::vector<NodeId> inner;
                fresh_cells(decls, ctx, inner, p);
                for (size_t k = 0; k < decls.size(); ++k) {
                  inner.push_back(bind_decl(
                      decls[k],
                      nth(results(), static_cast<int64_t>(k)), ctx, p));
                }
                inner.push_back(emit_block(*a.nodes[2], ctx));
                const int32_t bend = ctx.release(bmark);
                return b.block(
                    {b.make_if(b.binary(BinOp::Eq,
                                        b.intrinsic(IntrinsicId::TypeOf,
                                                    {first()}),
                                        b.str_literal("nil")),
                               b.make_break(), NodeId{}),
                     b.assign(VarKind::Local, cv, first()),
                     bend > bmark ? b.scope(bmark, bend, b.block(inner))
                                  : b.block(inner)});
              });
            });
        return b.block({b.assign(VarKind::Local, cv, nth(triple(), 2)),
                        b.make_while(b.bool_literal(true), body)});
      });
    });
    const int32_t end = ctx.release(mark);
    return b.scope(mark, end, loop);
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
      recv = ops.call(
          "$get",
          {recv, b.str_literal(std::string(nm.nodes[i]->nodes[0]->token))}, p);
    }
    return ops.call(
        "$set",
        {recv, b.str_literal(std::string(nm.nodes.back()->nodes[0]->token)),
         fn},
        p);
  }

  // An assignment, in lua5.4's order: every target's table and key first,
  // left to right, then every value, then the stores right to left. The
  // manual leaves the order open but fixes the consequence that matters --
  // `i, a[i] = i+1, 20` sets a[3], the i in a[i] read before i changes --
  // and lua5.4's own order is the oracle's. One target and one value is
  // the store on its own, which evaluates in that order already.
  NodeId emit_assign(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const Ast& targets = *a.nodes[0];
    const Ast& values = *a.nodes[1];
    if (targets.nodes.size() == 1 && values.nodes.size() == 1) {
      return emit_store(*targets.nodes[0], emit_expr(*values.nodes[0], ctx),
                        ctx, p);
    }
    // Each target's table and key, then the values, held by Lets in that
    // order for the stores.
    std::vector<Target> ts;
    std::vector<NodeId> inits;
    std::vector<size_t> at;  // where a target's table is among the inits
    for (const auto& t : targets.nodes) {
      ts.push_back(emit_target(*t, ctx));
      at.push_back(inits.size());
      if (ts.back().ident == nullptr) {
        inits.push_back(ts.back().table);
        inits.push_back(ts.back().key);
      }
    }
    inits.push_back(emit_exprlist(values, ctx));
    return b.lets(inits, [&](const std::vector<Builder::Temp>& held) {
      std::vector<NodeId> out;
      for (size_t k = ts.size(); k-- > 0;) {
        const NodeId value = rt.call(
            "$nth", {held.back()(), b.literal(static_cast<int64_t>(k))}, p);
        out.push_back(
            ts[k].ident != nullptr
                ? emit_store(*ts[k].ident, value, ctx, p)
                : ops.call("$set",
                          {held[at[k]](), held[at[k] + 1](), value}, p));
      }
      return b.block(out);
    });
  }

  // An assignment target: a variable, or the table and the key a store
  // into it will take -- the expressions, which emit_assign holds.
  struct Target {
    const Ast* ident = nullptr;
    NodeId table, key;
  };

  Target emit_target(const Ast& target, FnCtx& ctx) {
    const SrcPos p = pos_of(target);
    auto b = Builder(m).at(p);
    if (target.tag == "ident"_) return {&target, NodeId{}, NodeId{}};
    if (target.tag != "suffixedexp"_ || target.nodes.size() < 2) {
      fail(target, "cannot assign to this expression");
    }
    const Val recv = emit_suffixed(target, target.nodes.size() - 1, ctx);
    const NodeId table = recv.multi ? one(recv.n, p) : recv.n;
    const Ast& last = *target.nodes.back();
    NodeId key;
    if (last.tag == "dotsfx"_) {
      key = b.str_literal(std::string(last.nodes[0]->token));
    } else if (last.tag == "indexsfx"_) {
      key = emit_expr(*last.nodes[0], ctx);
    } else {
      fail(last, "cannot assign to a call");
    }
    return {nullptr, table, key};
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
        recv.multi ? one(recv.n, p) : recv.n;
    return ops.call("$set", {R, key, value}, p);
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
    fn.rest_param = fi.varargs >= 0;
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
    rt.reserve(rt_names());
    for (size_t f = 1; f < fns.size(); ++f) {
      rs.fns[f].index = static_cast<int32_t>(m.funcs.size());
      m.funcs.push_back({});
    }


    rs.number_captures(m);
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
// this IR could compute (the host does it: double_format.h's kLua). Table operations are *not* here (see $concat and
// $insert): a table is a Map, and walking one from C++ would tie this front
// end to MapObj's layout. The one table read is `__tostring`'s, two key
// lookups, which `print` has to make from C++ the way Lua's own does.

std::string lua_tostring(const Value& v) {
  switch (v.tag()) {
    case ValueTag::Nil: return "nil";
    case ValueTag::Bool: return v.as_bool() ? "true" : "false";
    case ValueTag::Int: return std::to_string(v.as_int());
    // "%.14g", and Lua then makes 4 look like a float again: 4.0
    // (double_format.h's kLua).
    case ValueTag::Double:
      return double_format::format_double(v.as_double(), double_format::kLua);
    case ValueTag::Str: return v.as_str();
    case ValueTag::Map: return "table";
    case ValueTag::Coroutine: return "thread";
    case ValueTag::Func:
    case ValueTag::Native: return "function";
    default: return "value";
  }
}

// Lua's tostring, `__tostring` included: what print and tostring answer,
// called directly or through a value. The metamethod is a Lua function, so
// it answers its results as an array (the calling convention above).
std::string lua_tostring_mm(NativeCall& c, const Value& v) {
  if (v.tag() == ValueTag::Map) {
    const Value& mt = v.as_map()->meta;
    if (mt.tag() == ValueTag::Map) {
      const Value* mm = mt.as_map()->find(Value::make_str("__tostring"));
      if (mm && mm->is_callable()) {
        const Value fn = *mm;  // the call may rewrite the metatable
        const Value self = v;
        const Value r = c.call(fn, &self, 1);
        if (r.tag() != ValueTag::Array) return lua_tostring(r);
        const auto& items = r.as_array()->items;
        return lua_tostring(items.empty() ? Value() : items[0]);
      }
    }
  }
  return lua_tostring(v);
}

void lua_print(NativeCall& c, const Value* vals, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; ++i) {
    if (i > 0) out += "\t";
    out += lua_tostring_mm(c, vals[i]);
  }
  coreir_rt_out_str(out.data(), static_cast<int64_t>(out.size()));
}

// print(...) at a call site: its arguments as one array, which is how
// `print(f())` expands a call's results.
bool nat_print(NativeCall& c) {
  const auto& items = c.arg(0).as_array()->items;
  lua_print(c, items.data(), items.size());
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
  c.result = Value::make_str(lua_tostring_mm(c, c.arg(0)));
  return true;
}

// Lua's type names, which are not the VM's: a table is a Map here and a
// coroutine is a "thread" there. luaL_typename, in lua's own terms.
const char* lua_typename(const Value& v) {
  switch (v.tag()) {
    case ValueTag::Nil: return "nil";
    case ValueTag::Bool: return "boolean";
    case ValueTag::Int:
    case ValueTag::Double: return "number";
    case ValueTag::Str: return "string";
    case ValueTag::Map: return "table";
    case ValueTag::Coroutine: return "thread";
    case ValueTag::Func:
    case ValueTag::Native: return "function";
    default: return "userdata";
  }
}

bool nat_type(NativeCall& c) {
  c.result = Value::make_str(lua_typename(c.arg(0)));
  return true;
}

// The same three as values a program holds (`local p = print`), so called
// the way every Lua function value is: arguments one by one, results as an
// array. Host functions rather than IR ones because print takes any number
// of arguments, which no function under the fixed-arity convention can.
bool nat_print_value(NativeCall& c) {
  lua_print(c, c.args, static_cast<size_t>(c.argc));
  c.result = Value::make_array({});
  return true;
}

// A one-argument native as a value: the same work, its one result answered
// as a results array.
template <NativeFn F>
bool as_value(NativeCall& c) {
  if (!F(c)) return false;
  c.result = Value::make_array({std::move(c.result)});
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

// A library function's complaint of an argument, in Lua's words ("bad
// argument #1 to 'floor' (number expected, got string)"): an Operands trap
// on that argument, which the conversion gives its chunk and line.
bool bad_arg(NativeCall& c, int index, const char* fn, const char* expected,
             const SrcPos* at = nullptr) {
  const std::string msg =
      "bad argument #" + std::to_string(index + 1) + " to '" + fn + "' (" +
      expected + " expected, got " +
      (index < c.argc ? lua_typename(c.arg(index)) : "no value") + ")";
  const TrapFacts facts{.l = &c.arg(index)};
  c.error = at != nullptr
                ? coreir::make_trap(TrapKind::Operands, msg, *at, facts)
                : c.trap(TrapKind::Operands, msg, facts);
  return false;
}

// `..` of a value that is neither a string nor a number: the operand that is
// neither, the left one first, as Lua's message names it.
bool nat_concat_fail(NativeCall& c) {
  const auto ok = [](const Value& v) { return v.is_str() || v.is_number(); };
  const Value& bad = ok(c.arg(0)) ? c.arg(1) : c.arg(0);
  c.error = c.trap(TrapKind::Operands,
                   std::string("attempt to concatenate a ") + lua_typename(bad) +
                       " value",
                   {.l = &bad});
  return false;
}

// A library function's complaint of an argument that the IR has already
// worded ("bad argument #1 to 'select' (index out of range)"): a BadValue
// trap, which the conversion gives its chunk and line.
bool nat_arg_fail(NativeCall& c) {
  c.error = c.trap(TrapKind::BadValue, c.arg(0).as_str());
  return false;
}

// A numeric for whose step is zero: Lua's own words.
bool nat_for_step(NativeCall& c) {
  c.error = c.trap(TrapKind::BadValue, "'for' step is zero");
  return false;
}

// error(v, level) for a level other than the default one. A string gets
// the position `level` frames up -- 1 is the call itself (line, col), 2 the
// call that entered the function holding it, which is what attributing a
// native with no position of its own blames; a program that is its own
// caller (the main chunk) has none, and no position is added. Deeper
// levels have no frame to read here, and add none. Anything but a string
// is raised as it is.
bool nat_error_level(NativeCall& c) {
  const Value& v = c.arg(0);
  const Value& lv = c.arg(1);
  const SrcPos here{static_cast<uint32_t>(c.arg(2).as_int()),
                    static_cast<uint32_t>(c.arg(3).as_int())};
  int64_t level = 1;
  if (lv.is_int()) {
    level = lv.as_int();
  } else if (lv.is_double()) {
    const double d = lv.as_double();
    if (!(std::floor(d) == d && d >= -0x1p63 && d < 0x1p63)) {
      c.error = coreir::make_trap(
          TrapKind::Operands,
          "bad argument #2 to 'error' (number has no integer representation)",
          here, {.l = &lv});
      return false;
    }
    level = static_cast<int64_t>(d);
  } else if (!lv.is_nil()) {
    bad_arg(c, 1, "error", "number", &here);
    return false;
  }
  SrcPos at{};
  if (level == 1) at = here;
  if (level == 2) at = c.exec->attributed(c.pos);
  if (v.is_str() && at.line != 0) {
    c.error = Value::make_str(chunk_name() + ":" + std::to_string(at.line) +
                              ": " + v.as_str());
  } else {
    c.error = v;
  }
  return false;
}

bool nat_floor(NativeCall& c) {
  const Value& v = c.arg(0);
  if (v.is_int()) {
    c.result = v;
    return true;
  }
  if (!v.is_double()) {
    return bad_arg(c, 0, "floor", "number");
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
    return bad_arg(c, 0, "sub", "string");
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

// string.rep(s, n [, sep]): n copies of s, sep between each two.
bool nat_s_rep(NativeCall& c) {
  if (!c.arg(0).is_str()) return bad_arg(c, 0, "rep", "string");
  if (!c.arg(1).is_int()) return bad_arg(c, 1, "rep", "number");
  if (c.argc > 2 && !c.arg(2).is_nil() && !c.arg(2).is_str()) {
    return bad_arg(c, 2, "rep", "string");
  }
  const bool has_sep = c.argc > 2 && c.arg(2).is_str();
  std::string out;
  for (int64_t k = 0; k < c.arg(1).as_int(); ++k) {
    if (k > 0 && has_sep) out += c.arg(2).as_str();
    out += c.arg(0).as_str();
  }
  c.result = Value::make_str(std::move(out));
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

// The chunk name of the program being run, which the conversion of a trap
// puts in front of its line the way Lua does for any error: set by
// bind_source, one program to a run.
std::string& chunk_name() {
  static std::string name;
  return name;
}

// An executor's trap as the error Lua raises for the same fault: a string,
// "chunk:line: attempt to ..." (Lua 5.4's words). The `(local 'x')` Lua adds
// when it knows the variable is not here: the VM does not know its name.
Value convert_trap(const Value& trap, const void*) {
  const Value L = trap_fact(trap, TrapFact::Left);
  const Value R = trap_fact(trap, TrapFact::Right);
  const Verb verb = trap_verb(trap);
  const auto type = [](const Value& v) { return std::string(lua_typename(v)); };
  // The operand an arithmetic or bitwise error blames: the first that is
  // not a number.
  const std::string culprit = type(L.is_number() ? R : L);
  std::string msg = trap_message(trap);
  switch (trap_kind(trap)) {
    case TrapKind::Operands:
      if (verb.cls == VerbClass::Binary) {
        const auto op = static_cast<BinOp>(verb.id);
        if (is_ordering(op)) {
          // `a > b` is `b < a` in Lua, and its error names them that way.
          const bool swapped = op == BinOp::Gt || op == BinOp::Ge;
          const std::string a = type(swapped ? R : L);
          const std::string b = type(swapped ? L : R);
          msg = a == b ? "attempt to compare two " + a + " values"
                       : "attempt to compare " + a + " with " + b;
        } else if (is_bitwise(op)) {
          msg = "attempt to perform bitwise operation on a " + culprit +
                " value";
        } else {
          msg = "attempt to perform arithmetic on a " + culprit + " value";
        }
      } else if (verb.cls == VerbClass::Unary) {
        msg = verb.is(UnOp::BitNot)
                  ? "attempt to perform bitwise operation on a " + type(L) +
                        " value"
                  : "attempt to perform arithmetic on a " + type(L) + " value";
      } else if (verb.is(IntrinsicId::Len)) {
        msg = "attempt to get length of a " + type(L) + " value";
      } else if (verb.is(IntrinsicId::Pow)) {
        msg = "attempt to perform arithmetic on a " + culprit + " value";
      }
      break;
    case TrapKind::NotCallable:
      msg = "attempt to call a " + type(L) + " value";
      break;
    case TrapKind::NotIndexable:
    case TrapKind::Immutable:
      msg = "attempt to index a " + type(L) + " value";
      break;
    case TrapKind::DivideByZero:
      msg = verb.is(BinOp::FloorMod) || verb.is(BinOp::Mod)
                ? "attempt to perform 'n%0'"
                : "attempt to divide by zero";
      break;
    case TrapKind::Depth:
      msg = "stack overflow";
      break;
    case TrapKind::None:
    case TrapKind::KeyType:
    case TrapKind::Range:
    case TrapKind::Arity:
    case TrapKind::Uninit:
    case TrapKind::BadValue:
    case TrapKind::State:
    case TrapKind::Host:
    case TrapKind::Internal:
      break;  // no words of its own: the executor's message stands
  }
  return Value::make_str(
      chunk_name() + ":" +
      std::to_string(trap.as_object()->find("line")->as_int()) + ": " + msg);
}

const std::vector<vm::NativeDef>& stdlib() {
  static const std::vector<vm::NativeDef> defs = {
      {"print", 1, nat_print, nullptr},
      {"write", 1, nat_write, nullptr},
      {"tostring", 1, nat_tostring, nullptr},
      {"type", 1, nat_type, nullptr},
      {"print_value", -1, nat_print_value, nullptr},
      {"tostring_value", -1, as_value<nat_tostring>, nullptr},
      {"type_value", -1, as_value<nat_type>, nullptr},
      {"mathtype", 1, nat_mathtype, nullptr},
      {"floor", 1, nat_floor, nullptr},
      {"s_sub", -1, nat_s_sub, nullptr},
      {"s_rep", -1, nat_s_rep, nullptr},
      {"s_len", -1, nat_s_len, nullptr},
      {"s_byte", -1, nat_s_byte, nullptr},
      {"concat_fail", 2, nat_concat_fail, nullptr},
      {"arg_fail", 1, nat_arg_fail, nullptr},
      {"for_step", 0, nat_for_step, nullptr},
      {"error_level", 4, nat_error_level, nullptr},
      trap_convert_def<&convert_trap>(),
  };
  return defs;
}

// lua.c's own words for an error object that is not a string or a number.
std::string describe(const Value& v) {
  if (trap_kind(v) != TrapKind::None) {
    return converted_or_self<&convert_trap>(v).as_str();
  }
  if (v.is_str() || v.is_int() || v.is_double()) return lua_tostring(v);
  return std::string("(error object is a ") + lua_typename(v) + " value)";
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
  chunk_name() = chunkname;
  Module m = b.build(*ast);

  if (auto err = verify(m)) {
    coreir_rt::fail("internal error: malformed IR: " + *err, 0, 0);
  }
  return m;
}

}  // namespace mini_lua
