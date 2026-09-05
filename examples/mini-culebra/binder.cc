// The front end whose oracle is the language this library was written to
// rehearse a back end for. Three of vmlib.h's mechanisms exist in the shape
// they do *because* of culebra, and say so in their own comments; none of
// the other front ends here uses any of them, so this is where they are
// shown working against the language that motivated them:
//
//   * **The owned stack.** "Deterministic drop for cycles: the owned stack
//     (culebra's design)." An Object whose kDropKey holds a callable is a
//     resource: its destructor runs when its count reaches zero, at the
//     exit of the scope that owns the cycle it is in, or in the collection
//     that condemns it. `class R { drop() {...} }` and `{drop: fn () {...}}`
//     are the two ways a program here makes one, and samples/drops.cul is
//     the timing, checked line for line against `culebra`.
//   * **A Scope's explicit release order.** "A front end that needs reverse
//     declaration order across the two hands the scope its release list as
//     an optional second child." culebra releases a block's bindings in
//     reverse declaration order, captured ones included -- and a captured
//     binding lives in a cell, which is not in the Scope's local range at
//     all. So every block here spells its order out.
//   * **`entry_frame_drops = false`.** "That is culebra's rule for
//     top-level bindings (only top-level defers run at exit), and a front
//     end wanting both under this option gives the entry frame a Scope over
//     an empty local range, [0, 0)." That is exactly what emit_fn does for
//     funcs[0].
//
// And one recipe no other front end here reaches at all: **Host functions**.
// culebra's standard library is not part of its language, and it is not
// part of this IR either -- `println`, `type_of`, `.size()`, `.push()`,
// `.map()` and the rest are Tag::NativeRef, declared by the module and
// supplied by the run (stdlib(), at the bottom of this file). `map` in
// particular calls back into the program, which is the half of the contract
// a purely outward-facing native would not show.
//
// What this front end does *not* need, and the contrast is the point:
// mini-js writes `$truthy` and `$seq` in IR because JavaScript disagrees
// with the VM about what is true and what is comparable. culebra does not.
// `if` here takes a Bool, a Long or a Float and nothing else -- which is
// Value::truthy()'s own rule for those three -- so a condition is a bare
// Tag::If with no call in front of it. `/` on two Longs is integer
// division and `%` is C's, which is BinOp::Div and BinOp::Mod unchanged.
// Only equality needed writing (`==` across two types is `false` in
// culebra and a trap in the VM, and arrays and objects compare
// structurally), and only display did (culebra prints a whole Float as
// "4.0", which to_display's comment names as the reason it does not grow a
// mode).

#include "binder.h"

#include <cstdlib>
#include <limits>
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

namespace mini_culebra {
namespace {

// The class table an instance points back at, and the destructor key the
// runtime looks for. Both are spelled with a leading 0x01 for the reason
// vmlib.h's kDropKey is: unspellable from a language whose identifiers are
// printable, so a program cannot collide with them. (Split across two
// string literals because "\x01class" would lex as one hex escape.)
constexpr char kClassKey[] = "\x01" "class";
// Methods hang on the class table under a second prefix, not the first:
// "\x01" "drop" is the runtime's own destructor key, and a class whose
// method table used it would make the *class* a resource. 0x02 also keeps
// every method out of `keys()` and out of a printed object, which is what
// culebra shows -- a class value prints as `{new: [function]}`, its methods
// being no more enumerable than an Error's fields are in JavaScript.
constexpr char kMethodPrefix[] = "\x02";
constexpr char kInitKey[] = "\x02" "init";
constexpr char kNameKey[] = "\x02" "name";

SrcPos pos_of(const Ast& a) {
  return {static_cast<uint32_t>(a.line), static_cast<uint32_t>(a.column)};
}

[[noreturn]] void fail(const Ast& a, const std::string& msg) {
  coreir_rt::fail(msg, static_cast<uint32_t>(a.line),
                  static_cast<uint32_t>(a.column));
}

const Ast* find_child(const Ast& a, std::string_view name) {
  for (const auto& n : a.nodes) {
    if (n->name == name) return n.get();
  }
  return nullptr;
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
      case '{': out.push_back('{'); break;
      case '}': out.push_back('}'); break;
      default: out.push_back(tok[i]); break;
    }
  }
  return out;
}

// culebra's standard library, as this front end reaches it. A free function
// is called with its arguments alone; a method with its receiver first.
// Both are Tag::NativeRef -- the difference is only in what the call site
// puts in front of the arguments.
bool is_global_fn(const std::string& n) {
  return n == "println" || n == "print" || n == "type_of";
}

bool is_builtin_method(const std::string& n) {
  return n == "size" || n == "push" || n == "pop" || n == "keys" ||
         n == "map";
}

// -- The resolution model ---------------------------------------------------
//
// The same two passes examples/mini-js uses, and for the same reason: "does
// anything nested inside this function read this binding" decides between a
// Local slot and a Cell, and it cannot be answered until the whole program
// has been walked.
struct FnInfo {
  bool is_generator = false;
  // A class's constructor: synthesized, so it has no body to walk and
  // emit_class builds its Func by hand.
  bool is_ctor = false;
  std::string name = "<anon>";
  std::vector<int32_t> params;  // VarIds; params[0] is always `self`
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
  // A classdecl's methods, by name, in declaration order.
  std::map<const Ast*, std::vector<std::pair<std::string, int32_t>>> class_of;
  std::map<std::string, int32_t> rt;
  int32_t empty_cmap = -1;

  // ==== Pass A: scopes, declarations, captures =============================

  // Every lexical scope's declaration order, keyed by the node that owns it
  // -- what emit needs to build a Scope's release list without walking the
  // statements a second time. `scope_order` is the open-scope stack that
  // feeds it, parallel to Resolver::scopes (whose name map cannot answer
  // "in what order").
  std::map<const Ast*, std::vector<int32_t>> block_decls;
  std::vector<std::vector<int32_t>> scope_order;

  // `mut` is the one fact about a binding this front end needs that
  // Resolver::Var does not carry, so it rides beside it, indexed by the
  // same VarId.
  std::vector<char> is_mut;

  int32_t declare(const std::string& name, int32_t fn, bool mut,
                  const Ast& at) {
    // culebra rejects shadowing outright (its "three-tier shadow rules"
    // design note); this subset rejects it within one scope, which is what
    // the samples exercise.
    if (rs.declared_here(name)) {
      fail(at, "'" + name + "' is already defined in this scope");
    }
    const int32_t v = rs.declare(name, fn);
    is_mut.resize(rs.vars.size(), 0);
    is_mut[static_cast<size_t>(v)] = mut ? 1 : 0;
    scope_order.back().push_back(v);
    return v;
  }

  int32_t new_fn(int32_t parent, const std::string& name) {
    const int32_t f = rs.new_fn(parent);
    fns.push_back({});
    fns[static_cast<size_t>(f)].name = name;
    return f;
  }

  // A block, and the record of what it declared: emit needs the order to
  // release in, and culebra's order is the reverse of this one.
  void resolve_block(const Ast& block, int32_t fn) {
    rs.push_scope();
    scope_order.emplace_back();
    for (const auto& s : block.nodes) resolve_stmt(*s, fn);
    block_decls[&block] = scope_order.back();
    scope_order.pop_back();
    rs.pop_scope();
  }

  // Every function's parameter 0 is `self`, declared here rather than
  // written in the source. That one convention buys three things at once:
  // a method reaches its receiver (`self.x`), an object literal's own
  // functions do too (`{iter: fn () { self }}` is culebra, not an
  // invention), and the drop protocol needs no special case -- the runtime
  // calls a destructor "with the object as its one argument", which lands
  // exactly in `self`.
  int32_t resolve_fn(const Ast& node, int32_t parent, const std::string& name,
                     const Ast* params, const Ast& body) {
    const int32_t f = new_fn(parent, name);
    fns[static_cast<size_t>(f)].body = &body;
    fn_of[&node] = f;

    rs.push_scope();
    scope_order.emplace_back();
    fns[static_cast<size_t>(f)].params.push_back(
        declare("self", f, false, node));
    if (params != nullptr) {
      for (const auto& p : params->nodes) {
        const int32_t v = declare(std::string(p->token), f, true, *p);
        decl_of[p.get()] = v;
        fns[static_cast<size_t>(f)].params.push_back(v);
      }
    }
    if (body.tag == "block"_) {
      resolve_block(body, f);
    } else {
      resolve_expr(body, f);  // a lambda's expression body
    }
    scope_order.pop_back();
    rs.pop_scope();
    // A body containing `yield` is a generator; the flag is set by
    // resolve_stmt as it walks, so it is already correct here.
    return f;
  }

  void resolve_stmt(const Ast& a, int32_t fn) {
    switch (a.tag) {
      case "fndecl"_: {
        const Ast& id = *a.nodes[0];
        decl_of[&id] = declare(std::string(id.token), fn, false, id);
        resolve_fn(a, fn, std::string(id.token), a.nodes[1].get(),
                   *a.nodes[2]);
        return;
      }
      case "classdecl"_: {
        const Ast& id = *a.nodes[0];
        // The class object is reached from the constructor this binder
        // synthesizes, so it is captured by definition -- which is what
        // makes it a cell rather than a slot.
        const int32_t v = declare(std::string(id.token), fn, false, id);
        decl_of[&id] = v;
        std::vector<std::pair<std::string, int32_t>> methods;
        for (size_t i = 1; i < a.nodes.size(); ++i) {
          const Ast& mth = *a.nodes[i];
          const std::string mname(mth.nodes[0]->token);
          const int32_t g = resolve_fn(mth, fn,
                                       std::string(id.token) + "." + mname,
                                       mth.nodes[1].get(), *mth.nodes[2]);
          methods.emplace_back(mname, g);
        }
        // The constructor is a function too, and the one thing it reads is
        // the class object -- which is what makes that binding a cell, and
        // what lets `C.new(...)` reach the methods from inside `C` itself
        // without the chicken-and-egg of building the object and the
        // constructor in one expression.
        const int32_t ctor = new_fn(fn, std::string(id.token) + ".new");
        fns[static_cast<size_t>(ctor)].is_ctor = true;
        rs.fns[static_cast<size_t>(ctor)].free.insert(v);
        methods.emplace_back("\x01ctor", ctor);
        class_of[&a] = methods;
        return;
      }
      case "vardecl"_: {
        resolve_expr(*a.nodes[2], fn);
        const Ast& id = *a.nodes[1];
        decl_of[&id] = declare(std::string(id.token), fn,
                               a.nodes[0]->token == "mut", id);
        return;
      }
      case "block"_:
        resolve_block(a, fn);
        return;
      case "deferstmt"_:
        // A Defer takes a callable, so the block is a function -- which is
        // also what makes everything it reads a capture, and so a cell.
        resolve_fn(a, fn, "<defer>", nullptr, *a.nodes[0]);
        return;
      case "whilestmt"_:
        resolve_expr(*a.nodes[0], fn);
        resolve_block(*a.nodes[1], fn);
        return;
      case "forstmt"_: {
        resolve_expr(*a.nodes[1], fn);
        rs.push_scope();
    scope_order.emplace_back();
        decl_of[a.nodes[0].get()] =
            declare(std::string(a.nodes[0]->token), fn, false, *a.nodes[0]);
        resolve_block(*a.nodes[2], fn);
        block_decls[&a] = scope_order.back();
        scope_order.pop_back();
    rs.pop_scope();
        return;
      }
      case "yieldstmt"_:
      case "yieldfrom"_:
        fns[static_cast<size_t>(fn)].is_generator = true;
        resolve_expr(*a.nodes[0], fn);
        return;
      case "breakstmt"_:
      case "contstmt"_:
        return;
      case "returnstmt"_:
      case "throwstmt"_:
      case "exprstmt"_:
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
      case "itext"_:
        return;
      case "ident"_: {
        const std::string n(a.token);
        if (auto v = rs.resolve(n, fn)) {
          ref_of[&a] = *v;
          return;
        }
        if (is_global_fn(n)) return;
        fail(a, "undefined variable '" + n + "'");
      }
      case "fnexpr"_:
        resolve_fn(a, fn, "<fn>", a.nodes[0].get(), *a.nodes[1]);
        return;
      case "lambda"_:
        resolve_fn(a, fn, "<lambda>", a.nodes[0].get(), *a.nodes[1]);
        return;
      case "tryexpr"_: {
        resolve_block(*a.nodes[0], fn);
        rs.push_scope();
    scope_order.emplace_back();
        decl_of[a.nodes[1].get()] =
            declare(std::string(a.nodes[1]->token), fn, false, *a.nodes[1]);
        resolve_block(*a.nodes[2], fn);
        block_decls[&a] = scope_order.back();
        scope_order.pop_back();
    rs.pop_scope();
        return;
      }
      case "ifexpr"_:
        resolve_expr(*a.nodes[0], fn);
        resolve_block(*a.nodes[1], fn);
        if (a.nodes.size() > 2) {
          if (a.nodes[2]->tag == "block"_) {
            resolve_block(*a.nodes[2], fn);
          } else {
            resolve_expr(*a.nodes[2], fn);
          }
        }
        return;
      case "propdef"_:
        resolve_expr(*a.nodes[1], fn);
        return;
      default:
        for (const auto& c : a.nodes) {
          if (c->tag == "assignop"_ || c->tag == "eqop"_ ||
              c->tag == "relop"_ || c->tag == "addop"_ ||
              c->tag == "mulop"_ || c->tag == "membersfx"_ ||
              c->tag == "varkw"_) {
            continue;
          }
          resolve_expr(*c, fn);
        }
        return;
    }
  }

  // ==== The runtime this front end writes in its own IR ====================
  //
  // Short, compared with mini-js's: culebra and the VM already agree about
  // truthiness, about integer division, about `%`, and about refusing to
  // order two values of different types. What is left is the two places
  // they genuinely differ -- `==` across types (false in culebra, a trap in
  // the VM) and how a Float prints ("4.0", not "4") -- plus the container
  // operations that need culebra's own error shapes.

  static const std::vector<std::string>& rt_names() {
    static const std::vector<std::string> names = {
        "$eq",   "$fstr",  "$disp",  "$insp", "$arrstr", "$objstr",
        "$err",  "$idx",   "$setidx", "$mem", "$setmem", "$methodof",
        "$iter", "$iternext",
    };
    return names;
  }

  // The front end's own additions to coreir::FuncWriter: the helpers
  // that have to reach this binder's own tables.
  struct RT : FuncWriter {
    Binder& bd;

    explicit RT(Binder& bd_) : FuncWriter(bd_.m), bd(bd_) {}

    NodeId call(const std::string& name, const std::vector<NodeId>& a) {
      return b.call_value(b.make_closure(bd.rt.at(name), bd.empty_cmap), a);
    }

    // The three checks `xs[i]` owes before the Index itself, shared by the
    // read and the write helper.
    std::vector<NodeId> array_index_checks(NodeId arr, NodeId ix) {
      return {
          iff(isnt(typ(ix), "int"),
              ret(call("$err",
                       {S("TypeError"), S("array index must be a Long")}))),
          iff(bin(BinOp::Lt, ix, I(0)),
              ret(call("$err", {S("IndexError"), S("index out of range")}))),
          iff(bin(BinOp::Ge, ix, len(arr)),
              ret(call("$err", {S("IndexError"), S("index out of range")})))};
    }

    // The counts and the name table come from param()/local().
    void finish(const std::string& name, int32_t ncells = 0,
                int32_t ncaps = 0) {
      write(bd.m.funcs[static_cast<size_t>(bd.rt.at(name))], name, ncells,
            ncaps);
    }
  };

  // culebra's `==`. Two numbers compare numerically whatever their widths
  // (`1 == 1.0` is true); two values of different types are unequal rather
  // than an error, which is where eval_binop stops -- "a question a
  // language answers, not the VM"; and arrays and objects compare
  // *structurally*, which is culebra's rule and the reason this is a
  // recursive function rather than an intrinsic.
  void rt_eq() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [ta, tb, i, ks, k] = r.locals("ta", "tb", "i", "ks", "k");
    const auto numeric = [&](NodeId t) {
      return r.b.make_if(r.is(t, "int"), r.Bo(true), r.is(t, "double"));
    };
    r.add(r.set(ta, r.typ(r.L(a))));
    r.add(r.set(tb, r.typ(r.L(b))));
    r.add(r.iff(
        numeric(r.L(ta)),
        r.iff(numeric(r.L(tb)), r.ret(r.bin(BinOp::Eq, r.L(a), r.L(b))))));
    r.add(r.iff(r.bin(BinOp::Ne, r.L(ta), r.L(tb)), r.ret(r.Bo(false))));

    std::vector<std::pair<NodeId, NodeId>> arms;
    arms.emplace_back(r.S("nil"), r.ret(r.Bo(true)));
    const NodeId scalar = r.ret(r.bin(BinOp::Eq, r.L(a), r.L(b)));
    arms.emplace_back(r.S("bool"), scalar);
    arms.emplace_back(r.S("string"), scalar);
    // Arrays: same length, and equal element by element.
    arms.emplace_back(
        r.S("array"),
        r.blk({r.iff(r.bin(BinOp::Ne, r.len(r.L(a)), r.len(r.L(b))),
                     r.ret(r.Bo(false))),
               r.set(i, r.I(0)),
               r.b.make_while(
                   r.bin(BinOp::Lt, r.L(i), r.len(r.L(a))),
                   r.blk({r.iff(r.bin(BinOp::Eq,
                                      r.call("$eq", {r.idx(r.L(a), r.L(i)),
                                                     r.idx(r.L(b), r.L(i))}),
                                      r.Bo(false)),
                                r.ret(r.Bo(false))),
                          r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})),
               r.ret(r.Bo(true))}));
    // Objects: same key count, and every key of the left present in the
    // right with an equal value.
    arms.emplace_back(
        r.S("object"),
        r.blk({r.set(ks, r.in(IntrinsicId::ObjectKeys, {r.L(a)})),
               r.iff(r.bin(BinOp::Ne, r.len(r.L(ks)),
                           r.len(r.in(IntrinsicId::ObjectKeys, {r.L(b)}))),
                     r.ret(r.Bo(false))),
               r.set(i, r.I(0)),
               r.b.make_while(
                   r.bin(BinOp::Lt, r.L(i), r.len(r.L(ks))),
                   r.blk({r.set(k, r.idx(r.L(ks), r.L(i))),
                          r.iff(r.bin(BinOp::Eq,
                                      r.in(IntrinsicId::ObjectHas,
                                           {r.L(b), r.L(k)}),
                                      r.Bo(false)),
                                r.ret(r.Bo(false))),
                          r.iff(r.bin(BinOp::Eq,
                                      r.call("$eq", {r.idx(r.L(a), r.L(k)),
                                                     r.idx(r.L(b), r.L(k))}),
                                      r.Bo(false)),
                                r.ret(r.Bo(false))),
                          r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})),
               r.ret(r.Bo(true))}));
    r.add(r.b.make_switch(r.L(ta), arms,
                          r.ret(r.in(IntrinsicId::Same, {r.L(a), r.L(b)}))));
    r.finish("$eq");
  }

  // How culebra prints a Float. to_display is shortest-round-trip, and its
  // comment names this exact case: "4.0 is '4' -- whether a whole double
  // should show a decimal point is a language's decision, and a front end
  // that cares builds the string". This is that front end caring.
  void rt_fstr() {
    const double lim = 9007199254740992.0;  // 2^53
    RT r(*this);
    const auto [d] = r.params("d");
    const auto [i] = r.locals("i");
    r.add(r.iff(r.bin(BinOp::Ne, r.L(d), r.L(d)),
                r.ret(r.in(IntrinsicId::ToStr, {r.L(d)}))));
    // -0.0 is integral and its integer form has lost the sign, so it is
    // the one value the general path below would print as "0.0".
    r.add(r.iff(r.in(IntrinsicId::Same, {r.L(d), r.D(-0.0)}),
                r.ret(r.S("-0.0"))));
    r.add(r.iff(
        r.bin(BinOp::Gt, r.L(d), r.D(-lim)),
        r.iff(r.bin(BinOp::Lt, r.L(d), r.D(lim)),
              r.blk({r.set(i, r.in(IntrinsicId::ToInt, {r.L(d)})),
                     r.iff(r.bin(BinOp::Eq,
                                 r.in(IntrinsicId::ToDouble, {r.L(i)}), r.L(d)),
                           r.ret(r.bin(BinOp::Add,
                                       r.in(IntrinsicId::ToStr, {r.L(i)}),
                                       r.S(".0"))))}))));
    // Everything else already carries a '.' or an exponent, and culebra's
    // own spelling of those is to_chars' -- 1e+21, 2.5e-07 -- so ToStr is
    // the answer verbatim.
    r.add(r.ret(r.in(IntrinsicId::ToStr, {r.L(d)})));
    r.finish("$fstr");
  }

  // Display conversion (culebra §8): what `"{x}"` and `println` produce. A
  // string is itself; a container is its inspect form, which quotes the
  // strings *inside* it.
  void rt_disp() {
    RT r(*this);
    const auto [v] = r.params("v");
    std::vector<std::pair<NodeId, NodeId>> arms;
    arms.emplace_back(r.S("string"), r.ret(r.L(v)));
    arms.emplace_back(r.S("nil"), r.ret(r.S("nil")));
    arms.emplace_back(r.S("double"), r.ret(r.call("$fstr", {r.L(v)})));
    arms.emplace_back(r.S("array"), r.ret(r.call("$arrstr", {r.L(v)})));
    arms.emplace_back(r.S("object"), r.ret(r.call("$objstr", {r.L(v)})));
    arms.emplace_back(r.S("function"), r.ret(r.S("[function]")));
    arms.emplace_back(r.S("generator"), r.ret(r.S("<generator>")));
    r.add(r.b.make_switch(r.typ(r.L(v)), arms,
                          r.ret(r.in(IntrinsicId::ToStr, {r.L(v)}))));
    r.finish("$disp");
  }

  void rt_insp() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.iff(r.is(r.typ(r.L(v)), "string"),
                r.ret(r.bin(BinOp::Add, r.bin(BinOp::Add, r.S("'"), r.L(v)),
                            r.S("'")))));
    r.add(r.ret(r.call("$disp", {r.L(v)})));
    r.finish("$insp");
  }

  void rt_arrstr() {
    RT r(*this);
    const auto [a] = r.params("a");
    const auto [out, i] = r.locals("out", "i");
    r.add(r.set(out, r.S("[")));
    r.add(r.set(i, r.I(0)));
    r.add(r.b.make_while(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(a))),
        r.blk({r.iff(r.bin(BinOp::Gt, r.L(i), r.I(0)),
                     r.set(out, r.bin(BinOp::Add, r.L(out), r.S(", ")))),
               r.set(out, r.bin(BinOp::Add, r.L(out),
                                r.call("$insp", {r.idx(r.L(a), r.L(i))}))),
               r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.bin(BinOp::Add, r.L(out), r.S("]"))));
    r.finish("$arrstr");
  }

  // An object's own keys, in insertion order -- skipping the two this
  // front end adds for itself. Their names start with a 0x01 byte, which
  // no source-level key can, so one byte test tells them apart.
  // An object's own keys, in insertion order, skipping the ones this front
  // end adds for itself -- their names start with a byte below 0x20, which
  // no source-level key can, so one byte test tells them apart. A class
  // instance prints its class name first and marks its fields `mut`, which
  // is culebra's own rendering and the reason the class table has to be
  // reachable from the instance at all.
  void rt_objstr() {
    RT r(*this);
    const auto [o] = r.params("o");
    const auto [out, i, ks, k, n, mut] =
        r.locals("out", "i", "ks", "k", "n", "mut");
    r.add(r.set(out, r.S("{")));
    r.add(r.set(mut, r.S("")));
    r.add(
        r.iff(r.in(IntrinsicId::ObjectHas, {r.L(o), r.S(kClassKey)}),
              r.blk({r.set(mut, r.S("mut ")),
                     r.set(out, r.bin(BinOp::Add,
                                      r.idx(r.idx(r.L(o), kClassKey), kNameKey),
                                      r.S(" {")))})));
    r.add(r.set(i, r.I(0)));
    r.add(r.set(ks, r.in(IntrinsicId::ObjectKeys, {r.L(o)})));
    r.add(r.set(n, r.I(0)));
    r.add(r.b.make_while(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(ks))),
        r.blk({
            r.set(k, r.idx(r.L(ks), r.L(i))),
            r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1))),
            r.iff(r.bin(BinOp::Lt, r.in(IntrinsicId::StrByte, {r.L(k), r.I(0)}),
                        r.I(32)),
                  r.b.make_continue()),
            r.iff(r.bin(BinOp::Gt, r.L(n), r.I(0)),
                  r.set(out, r.bin(BinOp::Add, r.L(out), r.S(", ")))),
            r.set(n, r.bin(BinOp::Add, r.L(n), r.I(1))),
            r.set(out, r.bin(BinOp::Add, r.L(out),
                             r.bin(BinOp::Add,
                                   r.bin(BinOp::Add,
                                         r.bin(BinOp::Add, r.L(mut), r.L(k)),
                                         r.S(": ")),
                                   r.call("$insp", {r.idx(r.L(o), r.L(k))})))),
        })));
    r.add(r.ret(r.bin(BinOp::Add, r.L(out), r.S("}"))));
    r.finish("$objstr");
  }

  // culebra's errors carry a kind, a message and a position; a `catch`
  // reads them as fields. The kinds this front end raises itself are the
  // container ones -- everything else is either a value the program threw
  // or a trap the executor raised, which TryCatch lands the same way.
  void rt_err() {
    RT r(*this);
    const auto [kind, message] = r.params("kind", "message");
    r.add(r.b.make_throw(
        r.obj({{"kind", r.L(kind)}, {"message", r.L(message)}})));
    r.finish("$err");
  }

  // `a[i]` and `o['k']`: strict, unlike `.` -- an absent key is a KeyError
  // and an out-of-range index an IndexError, both of which culebra raises
  // and this front end therefore raises too, rather than letting Index's
  // own trap message stand in for one of them.
  void rt_idx() {
    RT r(*this);
    const auto [recv, key] = r.params("recv", "key");
    std::vector<std::pair<NodeId, NodeId>> arms;
    auto arr_checks = r.array_index_checks(r.L(recv), r.L(key));
    arr_checks.push_back(r.ret(r.idx(r.L(recv), r.L(key))));
    arms.emplace_back(r.S("array"), r.blk(arr_checks));
    arms.emplace_back(
        r.S("object"),
        r.blk(
            {r.iff(r.isnt(r.typ(r.L(key)), "string"),
                   r.ret(r.call("$err", {r.S("TypeError"),
                                         r.S("object key must be a String")}))),
             r.iff(r.bin(BinOp::Eq,
                         r.in(IntrinsicId::ObjectHas, {r.L(recv), r.L(key)}),
                         r.Bo(false)),
                   r.ret(r.call("$err",
                                {r.S("KeyError"), r.S("key not present")}))),
             r.ret(r.idx(r.L(recv), r.L(key)))}));
    r.add(r.b.make_switch(
        r.typ(r.L(recv)), arms,
        r.ret(r.call("$err",
                     {r.S("TypeError"), r.S("value is not indexable")}))));
    r.finish("$idx");
  }

  void rt_setidx() {
    RT r(*this);
    const auto [recv, key, val] = r.params("recv", "key", "val");
    std::vector<std::pair<NodeId, NodeId>> arms;
    auto arr_checks = r.array_index_checks(r.L(recv), r.L(key));
    arr_checks.push_back(r.b.set_index(r.L(recv), r.L(key), r.L(val)));
    arr_checks.push_back(r.ret(r.L(val)));
    arms.emplace_back(r.S("array"), r.blk(arr_checks));
    arms.emplace_back(
        r.S("object"),
        r.blk({r.b.set_index(r.L(recv), r.L(key), r.L(val)), r.ret(r.L(val))}));
    r.add(r.b.make_switch(
        r.typ(r.L(recv)), arms,
        r.ret(r.call("$err",
                     {r.S("TypeError"), r.S("value is not indexable")}))));
    r.finish("$setidx");
  }

  // `o.name`: lenient, culebra's own split from `o['name']` -- reading a
  // field that is not there answers nil rather than raising.
  void rt_mem() {
    RT r(*this);
    const auto [recv, name] = r.params("recv", "name");
    r.add(r.iff(r.is(r.typ(r.L(recv)), "object"),
                r.ret(r.idx(r.L(recv), r.L(name)))));
    r.add(r.ret(r.call("$err", {r.S("TypeError"),
                                r.S("value has no fields")})));
    r.finish("$mem");
  }

  void rt_setmem() {
    RT r(*this);
    const auto [recv, name, val] = r.params("recv", "name", "val");
    r.add(r.iff(r.is(r.typ(r.L(recv)), "object"),
                r.blk({r.b.set_index(r.L(recv), r.L(name), r.L(val)),
                       r.ret(r.L(val))})));
    r.add(r.ret(r.call("$err", {r.S("TypeError"),
                                r.S("value has no fields")})));
    r.finish("$setmem");
  }

  // Method lookup: the object's own properties first (an object literal
  // holding a function is how culebra writes a protocol), then the class
  // table the constructor hung on the instance.
  void rt_methodof() {
    RT r(*this);
    const auto [recv, name] = r.params("recv", "name");
    const auto [cls, key] = r.locals("cls", "key");
    r.add(r.iff(
        r.is(r.typ(r.L(recv)), "object"),
        r.blk({r.iff(r.in(IntrinsicId::ObjectHas, {r.L(recv), r.L(name)}),
                     r.ret(r.idx(r.L(recv), r.L(name)))),
               r.iff(r.in(IntrinsicId::ObjectHas, {r.L(recv), r.S(kClassKey)}),
                     r.blk({r.set(cls, r.idx(r.L(recv), kClassKey)),
                            r.set(key, r.bin(BinOp::Add, r.S(kMethodPrefix),
                                             r.L(name))),
                            r.iff(r.in(IntrinsicId::ObjectHas,
                                       {r.L(cls), r.L(key)}),
                                  r.ret(r.idx(r.L(cls), r.L(key))))}))})));
    r.add(r.ret(r.call(
        "$err",
        {r.S("NameError"), r.bin(BinOp::Add, r.S("no method "), r.L(name))})));
    r.finish("$methodof");
  }

  // `for x in v`, over an array, a generator, or -- the custom iterator
  // protocol README.md used to list as out of scope -- any object whose
  // class declares `iter`. culebra's own protocol is `iter`/`has_next`/
  // `next`/`dispose`; this front end reaches the first three. `iter()`
  // answers the iterable (usually `self`), and `has_next()`/`next()`
  // drive it. `dispose()` is not called: wiring it through `Defer` inside
  // `emit_for` needs a cell the array/generator cursors do not, and
  // giving them one moved when their own elements are released -- this
  // front end's flagship destructor-ordering guarantee -- so the safer
  // choice was to leave `dispose()` unreached rather than risk it.
  void rt_iter() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [t, cls, key] = r.locals("t", "cls", "key");
    r.add(r.set(t, r.typ(r.L(v))));
    r.add(r.iff(r.is(r.L(t), "generator"),
                r.ret(r.obj({{"k", r.S("g")}, {"v", r.L(v)}}))));
    r.add(r.iff(r.is(r.L(t), "array"),
                r.ret(r.obj({{"k", r.S("a")}, {"v", r.L(v)}, {"i", r.I(0)}}))));
    r.add(r.iff(
        r.is(r.L(t), "object"),
        r.iff(r.in(IntrinsicId::ObjectHas, {r.L(v), r.S(kClassKey)}),
              r.blk({r.set(cls, r.idx(r.L(v), kClassKey)),
                     r.set(key,
                           r.bin(BinOp::Add, r.S(kMethodPrefix), r.S("iter"))),
                     r.iff(r.in(IntrinsicId::ObjectHas, {r.L(cls), r.L(key)}),
                           r.ret(r.obj(
                               {{"k", r.S("c")},
                                {"v", r.b.call_value(r.idx(r.L(cls), r.L(key)),
                                                     {r.L(v)})}})))}))));
    r.add(r.ret(r.call("$err", {r.S("TypeError"),
                                r.S("value is not iterable")})));
    r.finish("$iter");
  }

  void rt_iternext() {
    RT r(*this);
    const auto [it] = r.params("it");
    const auto [a, i, obj, hn] = r.locals("a", "i", "obj", "hn");
    r.add(r.iff(
        r.is(r.idx(r.L(it), "k"), "g"),
        r.ret(r.in(IntrinsicId::GenResume, {r.idx(r.L(it), "v"), r.Nil()}))));
    r.add(r.iff(
        r.is(r.idx(r.L(it), "k"), "c"),
        r.blk({r.set(a, r.idx(r.L(it), "v")),
               r.set(hn, r.b.call_value(
                             r.call("$methodof", {r.L(a), r.S("has_next")}),
                             {r.L(a)})),
               r.iff(r.bin(BinOp::Eq, r.L(hn), r.Bo(false)),
                     r.ret(r.obj({{"value", r.Nil()}, {"done", r.Bo(true)}}))),
               r.ret(r.obj(
                   {{"value",
                     r.b.call_value(r.call("$methodof", {r.L(a), r.S("next")}),
                                    {r.L(a)})},
                    {"done", r.Bo(false)}}))})));
    r.add(r.set(a, r.idx(r.L(it), "v")));
    r.add(r.set(i, r.idx(r.L(it), "i")));
    r.add(r.iff(r.bin(BinOp::Ge, r.L(i), r.len(r.L(a))),
                r.ret(r.obj({{"value", r.Nil()}, {"done", r.Bo(true)}}))));
    r.add(r.b.set_index(r.L(it), r.S("i"), r.bin(BinOp::Add, r.L(i), r.I(1))));
    r.add(r.ret(
        r.obj({{"value", r.idx(r.L(a), r.L(i))}, {"done", r.Bo(false)}})));
    r.finish("$iternext");
  }

  void emit_runtime() {
    rt_eq();
    rt_fstr();
    rt_disp();
    rt_insp();
    rt_arrstr();
    rt_objstr();
    rt_err();
    rt_idx();
    rt_setidx();
    rt_mem();
    rt_setmem();
    rt_methodof();
    rt_iter();
    rt_iternext();
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

  NodeId bind_decl(int32_t v, NodeId value, FnCtx& ctx, SrcPos p,
                   bool fresh = true) {
    auto b = Builder(m).at(p);
    const auto& ci = rs.fns[static_cast<size_t>(ctx.fn)].cell_index;
    const auto it = ci.find(v);
    if (it != ci.end()) {
      if (!fresh) return b.assign(VarKind::Cell, it->second, value);
      return b.block({b.cell_fresh(it->second),
                      b.assign(VarKind::Cell, it->second, value)});
    }
    const int32_t s = ctx.alloc_local(rs.vars[static_cast<size_t>(v)].name);
    rs.vars[static_cast<size_t>(v)].slot = s;
    return b.assign(VarKind::Local, s, value);
  }

  NodeId read_var(int32_t v, FnCtx& ctx, SrcPos p) {
    auto b = Builder(m).at(p);
    const auto [k, i] = rs.access(ctx.fn, v);
    return b.varref(k, i);
  }

  NodeId write_var(int32_t v, NodeId value, FnCtx& ctx, SrcPos p) {
    auto b = Builder(m).at(p);
    const auto [k, i] = rs.access(ctx.fn, v);
    return b.assign(k, i, value);
  }

  // The release list culebra's rule asks for: the block's own bindings, in
  // reverse declaration order, cells included. Without it a Scope releases
  // its local range last-slot-first and leaves the cells to the frame --
  // which would drop a captured resource at the wrong time, or not at all
  // until the function returned. samples/drops.cul is the difference.
  std::vector<NodeId> release_list(const std::vector<int32_t>& decls,
                                   FnCtx& ctx, SrcPos p) {
    auto b = Builder(m).at(p);
    std::vector<NodeId> out;
    for (size_t i = decls.size(); i-- > 0;) {
      const auto [k, idx] = rs.access(ctx.fn, decls[i]);
      out.push_back(b.varref(k, idx));
    }
    return out;
  }

  NodeId emit_block(const Ast& block, FnCtx& ctx, bool entry = false) {
    const SrcPos p = pos_of(block);
    auto b = Builder(m).at(p);
    const int32_t mark = ctx.mark();
    const auto& decls = block_decls.at(&block);

    std::vector<NodeId> out;
    // Bindings are created on entry and only initialized where they stand
    // -- see examples/mini-js/README.md for the bug that rule prevents.
    const auto& cells = rs.fns[static_cast<size_t>(ctx.fn)].cell_index;
    for (const int32_t v : decls) {
      const auto c = cells.find(v);
      if (c != cells.end()) out.push_back(b.cell_fresh(c->second));
    }
    for (const auto& s : block.nodes) out.push_back(emit_stmt(*s, ctx));

    const int32_t end = ctx.release(mark);
    // funcs[0] runs under entry_frame_drops = false, and culebra's rule for
    // a top-level binding is that it lives to the end of the program and is
    // released without its destructor. So the program's own statements get
    // no releasing Scope at all -- emit_fn wraps them in a [0, 0) one,
    // which is what still lets a top-level `defer` run.
    if (entry) return b.block(out);
    // A Scope even when the block declares nothing: Tag::Defer runs at the
    // exit of the *enclosing* Scope, so a block whose only statement is a
    // `defer` would otherwise hand it to the function's scope and run it at
    // the wrong time -- once, at the end, instead of once per block entry.
    // errors.cul's `while` loop is the case that catches it: three
    // iterations, three defers, and without the scope all three fire after
    // the loop with the loop variable already at its final value.
    if (decls.empty()) return b.scope(mark, end, b.block(out));
    return b.scope(mark, end, b.block(out), release_list(decls, ctx, p));
  }

  std::vector<NodeId> emit_args(const Ast& args, FnCtx& ctx) {
    std::vector<NodeId> out;
    out.reserve(args.nodes.size());
    for (const auto& c : args.nodes) out.push_back(emit_expr(*c, ctx));
    return out;
  }

  // -- Statements ---------------------------------------------------------
  NodeId emit_stmt(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    switch (a.tag) {
      case "fndecl"_:
        return bind_decl(decl_of.at(a.nodes[0].get()),
                         emit_closure(fn_of.at(&a), ctx, p), ctx, p, false);
      case "classdecl"_:
        return emit_class(a, ctx);
      case "vardecl"_: {
        // `let x = v` *evaluates to* v in culebra, which matters here for
        // more than tidiness: a function answers its body's last value, so
        // a body ending in a binding hands that binding's value back -- and
        // a resource bound last therefore outlives the scope, dying at the
        // call site instead of at the scope's exit. samples/drops.cul shows
        // the difference, and without this the last declared resource drops
        // one line early.
        const int32_t v = decl_of.at(a.nodes[1].get());
        return b.block(
            {bind_decl(v, emit_expr(*a.nodes[2], ctx), ctx, p, false),
             read_var(v, ctx, p)});
      }
      case "block"_:
        return emit_block(a, ctx);
      case "deferstmt"_:
        return b.make_defer(emit_closure(fn_of.at(&a), ctx, p));
      case "returnstmt"_:
        return b.make_return(a.nodes.empty() ? b.nil_literal()
                                             : emit_expr(*a.nodes[0], ctx));
      case "throwstmt"_:
        return b.make_throw(emit_expr(*a.nodes[0], ctx));
      case "yieldstmt"_:
        return b.make_yield(emit_expr(*a.nodes[0], ctx));
      case "yieldfrom"_:
        return emit_yield_from(a, ctx);
      case "breakstmt"_:
        return b.make_break();
      case "contstmt"_:
        return b.make_continue();
      case "whilestmt"_:
        return b.make_while(emit_expr(*a.nodes[0], ctx),
                            emit_block(*a.nodes[1], ctx));
      case "forstmt"_:
        return emit_for(a, ctx);
      case "exprstmt"_:
        return emit_expr(*a.nodes[0], ctx);
      default:
        fail(a, "cannot execute " + a.name);
    }
  }

  // `yield from e` is the loop it would be if written out, which is what
  // culebra's own lowering does too: nothing in the IR delegates one
  // generator to another, and nothing needs to.
  NodeId emit_yield_from(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const int32_t mark = ctx.mark();
    const int32_t it = ctx.alloc_local("$it");
    const int32_t st = ctx.alloc_local("$step");
    const NodeId I = b.varref(VarKind::Local, it);
    const NodeId S = b.varref(VarKind::Local, st);
    std::vector<NodeId> loop{
        b.assign(VarKind::Local, st, helper("$iternext", {I}, p)),
        b.make_if(b.index(S, b.str_literal("done")), b.make_break(), NodeId{}),
        b.make_yield(b.index(S, b.str_literal("value")))};
    const NodeId body =
        b.block({b.assign(VarKind::Local, it,
                          helper("$iter", {emit_expr(*a.nodes[0], ctx)}, p)),
                 b.make_while(b.bool_literal(true), b.block(loop))});
    const int32_t end = ctx.release(mark);
    return b.scope(mark, end, body);
  }

  // for x in e { ... }
  //
  // Two nested scopes rather than one, because culebra releases in a
  // documented order -- "body locals, element, dispose, iterator,
  // iterable" -- and nesting is how that order is stated: the body block's
  // own scope goes first, the element's scope around it next, and the
  // iterator's outermost.
  NodeId emit_for(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const int32_t mark = ctx.mark();
    const int32_t it = ctx.alloc_local("$it");
    const int32_t st = ctx.alloc_local("$step");
    const NodeId I = b.varref(VarKind::Local, it);
    const NodeId S = b.varref(VarKind::Local, st);

    const int32_t v = decl_of.at(a.nodes[0].get());
    const int32_t emark = ctx.mark();
    const auto& cells = rs.fns[static_cast<size_t>(ctx.fn)].cell_index;
    std::vector<NodeId> elem;
    const auto c = cells.find(v);
    if (c != cells.end()) elem.push_back(b.cell_fresh(c->second));
    elem.push_back(
        bind_decl(v, b.index(S, b.str_literal("value")), ctx, p, false));
    elem.push_back(emit_block(*a.nodes[2], ctx));
    const int32_t eend = ctx.release(emark);
    const NodeId elem_scope =
        eend > emark
            ? b.scope(emark, eend, b.block(elem), release_list({v}, ctx, p))
            : b.scope(emark, emark, b.block(elem), release_list({v}, ctx, p));

    std::vector<NodeId> loop{
        b.assign(VarKind::Local, st, helper("$iternext", {I}, p)),
        b.make_if(b.index(S, b.str_literal("done")), b.make_break(), NodeId{}),
        elem_scope};
    const NodeId body =
        b.block({b.assign(VarKind::Local, it,
                          helper("$iter", {emit_expr(*a.nodes[1], ctx)}, p)),
                 b.make_while(b.bool_literal(true), b.block(loop))});
    const int32_t end = ctx.release(mark);
    return b.scope(mark, end, body);
  }

  // class C { new(...) {...} m(...) {...} drop() {...} }
  //
  //   C = { "\x01init": <new>, "m": <m>, "drop": <drop> }   -- a cell
  //   C["new"] = MakeClosure(<synthesized constructor>, over C)
  //
  // The constructor is what turns the table into an instance: a fresh
  // object carrying "\x01class" (so a method call can find the table) and,
  // when the class declares one, the runtime's own drop key (so the object
  // *is* a resource). Binding that key is what puts it on the owned stack;
  // everything about when its destructor runs follows from that.
  NodeId emit_class(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const auto& methods = class_of.at(&a);
    const int32_t v = decl_of.at(a.nodes[0].get());
    const int32_t cell = rs.fns[static_cast<size_t>(ctx.fn)].cell_index.at(v);

    int32_t ctor_fn = -1;
    int32_t init_params = 0;
    bool has_drop = false;
    std::vector<std::pair<NodeId, NodeId>> kvs;
    for (const auto& [name, g] : methods) {
      if (name == "\x01ctor") {
        ctor_fn = g;
        continue;
      }
      if (name == "drop") has_drop = true;
      const std::string key =
          name == "new" ? kInitKey : std::string(kMethodPrefix) + name;
      if (name == "new") {
        init_params =
            static_cast<int32_t>(fns[static_cast<size_t>(g)].params.size()) - 1;
      }
      kvs.emplace_back(b.str_literal(key), emit_closure(g, ctx, p));
    }
    kvs.emplace_back(b.str_literal(kNameKey),
                     b.str_literal(std::string(a.nodes[0]->token)));
    emit_ctor(ctor_fn, init_params, has_drop, std::string(a.nodes[0]->token));

    std::vector<CaptureSrc> cs{{VarKind::Cell, cell}};
    const int32_t cm = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back(cs);
    return b.block(
        {b.assign(VarKind::Cell, cell, b.object_lit(kvs)),
         b.set_index(
             b.varref(VarKind::Cell, cell), b.str_literal("new"),
             b.make_closure(rs.fns[static_cast<size_t>(ctor_fn)].index, cm))});
  }

  void emit_ctor(int32_t g, int32_t init_params, bool has_drop,
                 const std::string& cname) {
    Builder b(m);
    const SrcPos p{0, 0};
    const int32_t nparams = 1 + init_params;  // self, then `new`'s own
    const int32_t inst = nparams;
    const NodeId C = b.varref(VarKind::Capture, 0, p);
    const NodeId O = b.varref(VarKind::Local, inst, p);
    std::vector<NodeId> body{b.assign(
        VarKind::Local, inst,
        b.object_lit({{b.str_literal(kClassKey, p), C}}, p), p)};
    if (has_drop) {
      body.push_back(b.set_index(
          O, b.str_literal(coreir::kDropKey, p),
          b.index(C, b.str_literal(std::string(kMethodPrefix) + "drop", p), p),
          p));
    }
    std::vector<NodeId> init_args{O};
    for (int32_t i = 1; i < nparams; ++i) {
      init_args.push_back(b.varref(VarKind::Local, i, p));
    }
    body.push_back(
        b.call_value(b.index(C, b.str_literal(kInitKey, p), p), init_args, p));
    body.push_back(b.make_return(O, p));

    Func f;
    f.name = cname + ".new";
    f.num_params = nparams;
    f.num_locals = nparams + 1;
    f.local_names.assign(static_cast<size_t>(nparams + 1), "arg");
    f.local_names[0] = "self";
    f.local_names[static_cast<size_t>(inst)] = "$inst";
    f.num_captures = 1;
    f.capture_names = {cname};
    f.lenient_arity = true;
    // No releasing Scope: the one local worth releasing is the instance,
    // and it is the return value.
    f.body = b.scope(0, 0, b.block(body, p), p);
    m.funcs[static_cast<size_t>(rs.fns[static_cast<size_t>(g)].index)] =
        std::move(f);
  }

  // -- Expressions --------------------------------------------------------
  NodeId emit_expr(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    switch (a.tag) {
      case "number"_:
        return b.literal(
            std::strtoll(std::string(a.token).c_str(), nullptr, 10));
      case "float"_:
        return b.double_literal(
            std::strtod(std::string(a.token).c_str(), nullptr));
      case "string"_:
        return b.str_literal(unescape(std::string(a.token)));
      case "istring"_: {
        // "a{x}b" is concatenation, with every hole passed through the
        // display conversion -- culebra's §8, and the one place a Float
        // has to come out as "4.0".
        NodeId acc = b.str_literal("");
        for (const auto& part : a.nodes) {
          const NodeId piece =
              part->tag == "itext"_
                  ? b.str_literal(unescape(std::string(part->token)))
                  : helper("$disp", {emit_expr(*part->nodes[0], ctx)}, p);
          acc = b.binary(BinOp::Add, acc, piece);
        }
        return acc;
      }
      case "literal"_: {
        const std::string t(a.token);
        if (t == "true") return b.bool_literal(true);
        if (t == "false") return b.bool_literal(false);
        return b.nil_literal();
      }
      case "ident"_: {
        const auto it = ref_of.find(&a);
        if (it != ref_of.end()) return read_var(it->second, ctx, p);
        fail(a, "'" + std::string(a.token) +
                    "' is only supported as a call here");
      }
      case "paren"_:
        return emit_expr(*a.nodes[0], ctx);
      // culebra's PRIMARY lists these four, so they turn up in expression
      // position as well as statement position -- `|| throw 'boom'` is a
      // lambda whose body is a throw. Nothing reads their value; they do
      // not come back.
      case "throwstmt"_:
      case "returnstmt"_:
      case "breakstmt"_:
      case "contstmt"_:
        return emit_stmt(a, ctx);
      case "fnexpr"_:
      case "lambda"_:
        return emit_closure(fn_of.at(&a), ctx, p);
      case "notexpr"_:
        return b.binary(BinOp::Eq, emit_expr(*a.nodes[0], ctx),
                        b.bool_literal(false));
      case "negexpr"_:
        return b.unary(UnOp::Neg, emit_expr(*a.nodes[0], ctx));
      case "ifexpr"_: {
        const NodeId c = emit_expr(*a.nodes[0], ctx);
        const NodeId t = emit_block(*a.nodes[1], ctx);
        NodeId e;
        if (a.nodes.size() > 2) {
          e = a.nodes[2]->tag == "block"_ ? emit_block(*a.nodes[2], ctx)
                                          : emit_expr(*a.nodes[2], ctx);
        }
        return b.make_if(c, t, e);
      }
      case "tryexpr"_: {
        const int32_t mark = ctx.mark();
        const int32_t exc = ctx.alloc_local("$exc");
        const NodeId body = emit_block(*a.nodes[0], ctx);
        const int32_t v = decl_of.at(a.nodes[1].get());
        const int32_t hmark = ctx.mark();
        const auto& cells = rs.fns[static_cast<size_t>(ctx.fn)].cell_index;
        std::vector<NodeId> hs;
        const auto c = cells.find(v);
        if (c != cells.end()) hs.push_back(b.cell_fresh(c->second));
        hs.push_back(
            bind_decl(v, b.varref(VarKind::Local, exc), ctx, p, false));
        hs.push_back(emit_block(*a.nodes[2], ctx));
        const int32_t hend = ctx.release(hmark);
        const NodeId handler = b.scope(hmark, hend > hmark ? hend : hmark,
                                       b.block(hs), release_list({v}, ctx, p));
        const NodeId out = b.make_try(exc, body, handler);
        const int32_t end = ctx.release(mark);
        return b.scope(mark, end, out);
      }
      case "logor"_:
      case "logand"_: {
        const bool is_or = a.tag == "logor"_;
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i < a.nodes.size(); ++i) {
          const int32_t t = ctx.alloc_local(is_or ? "$or" : "$and");
          const NodeId rhs = emit_expr(*a.nodes[i], ctx);
          const NodeId keep = b.varref(VarKind::Local, t);
          acc = b.block(
              {b.assign(VarKind::Local, t, acc),
               b.make_if(keep, is_or ? keep : rhs, is_or ? rhs : keep)});
        }
        return acc;
      }
      case "equality"_: {
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i + 1 < a.nodes.size(); i += 2) {
          const Ast& op = *a.nodes[i];
          const NodeId rhs = emit_expr(*a.nodes[i + 1], ctx);
          const NodeId eq = helper("$eq", {acc, rhs}, pos_of(op));
          acc = op.token == "=="
                    ? eq
                    : b.at(pos_of(op))
                          .binary(BinOp::Eq, eq, b.bool_literal(false));
        }
        return acc;
      }
      case "relational"_: {
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i + 1 < a.nodes.size(); i += 2) {
          const Ast& op = *a.nodes[i];
          const std::string t(op.token);
          const BinOp o = t == "<"    ? BinOp::Lt
                          : t == "<=" ? BinOp::Le
                          : t == ">"  ? BinOp::Gt
                                      : BinOp::Ge;
          acc =
              b.at(pos_of(op)).binary(o, acc, emit_expr(*a.nodes[i + 1], ctx));
        }
        return acc;
      }
      case "additive"_:
      case "multiplicative"_: {
        // Straight to BinOp, with no helper in the way: culebra's `/` on
        // two Longs is integer division and its `%` is C's, which is what
        // eval_binop already does for two Ints.
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i + 1 < a.nodes.size(); i += 2) {
          const Ast& op = *a.nodes[i];
          const std::string t(op.token);
          const BinOp o = t == "+"   ? BinOp::Add
                          : t == "-" ? BinOp::Sub
                          : t == "*" ? BinOp::Mul
                          : t == "/" ? BinOp::Div
                                     : BinOp::Mod;
          acc =
              b.at(pos_of(op)).binary(o, acc, emit_expr(*a.nodes[i + 1], ctx));
        }
        return acc;
      }
      case "assign"_:
        return emit_assign(a, ctx);
      case "postfix"_:
        return emit_postfix(a, a.nodes.size(), ctx);
      case "arraylit"_: {
        std::vector<NodeId> items;
        items.reserve(a.nodes.size());
        for (const auto& c : a.nodes) items.push_back(emit_expr(*c, ctx));
        return b.array_lit(items);
      }
      case "objectlit"_: {
        // A `drop` property is the runtime's destructor key, not a
        // property named "drop": binding it is what registers the object
        // on the owned stack, which is the whole of "this literal is a
        // resource".
        std::vector<std::pair<NodeId, NodeId>> kvs;
        for (const auto& c : a.nodes) {
          const Ast& key = *c->nodes[0];
          std::string k = key.tag == "string"_
                              ? unescape(std::string(key.token))
                              : std::string(key.token);
          if (k == "drop") k = coreir::kDropKey;
          kvs.emplace_back(b.str_literal(k), emit_expr(*c->nodes[1], ctx));
        }
        return b.object_lit(kvs);
      }
      default:
        fail(a, "cannot evaluate " + a.name);
    }
  }

  NodeId emit_postfix(const Ast& a, size_t limit, FnCtx& ctx) {
    Builder b(m);
    const Ast& prim = *a.nodes[0];
    size_t i = 1;
    NodeId cur;
    // A standard-library name is not a value here, only the head of a
    // call: `println(x)` is a NativeRef, and `println` alone is an error.
    if (prim.tag == "ident"_ && !ref_of.count(&prim)) {
      const std::string g(prim.token);
      if (i >= limit || a.nodes[i]->tag != "callsfx"_) {
        fail(prim, "'" + g + "' must be called here");
      }
      const std::vector<NodeId> args = emit_args(*a.nodes[i]->nodes[0], ctx);
      const SrcPos gp = pos_of(prim);
      ++i;
      if (g == "println" || g == "print") {
        cur = native(g,
                     {args.empty() ? b.str_literal("", gp)
                                   : helper("$disp", {args[0]}, gp)},
                     gp);
      } else {
        cur = native(g, args, gp);
      }
    } else {
      cur = emit_expr(prim, ctx);
    }
    for (; i < limit; ++i) {
      const Ast& sfx = *a.nodes[i];
      const SrcPos p = pos_of(sfx);
      switch (sfx.tag) {
        case "membersfx"_: {
          const std::string name(sfx.nodes[0]->token);
          if (i + 1 < limit && a.nodes[i + 1]->tag == "callsfx"_) {
            std::vector<NodeId> args =
                emit_args(*a.nodes[i + 1]->nodes[0], ctx);
            ++i;
            if (is_builtin_method(name)) {
              // culebra's standard library, supplied by the host: the
              // receiver goes first, and `map` calls back into the program
              // from C++ (see stdlib()).
              args.insert(args.begin(), cur);
              cur = native(name, args, p);
              break;
            }
            // Every function's parameter 0 is `self`, so a method call is
            // an ordinary call with the receiver in front.
            const NodeId f =
                helper("$methodof", {cur, b.str_literal(name, p)}, p);
            args.insert(args.begin(), cur);
            cur = b.call_value(f, args, p);
            break;
          }
          cur = helper("$mem", {cur, b.str_literal(name, p)}, p);
          break;
        }
        case "indexsfx"_:
          cur = helper("$idx", {cur, emit_expr(*sfx.nodes[0], ctx)}, p);
          break;
        default: {  // callsfx: a plain call, with nil for `self`
          std::vector<NodeId> args = emit_args(*sfx.nodes[0], ctx);
          args.insert(args.begin(), b.nil_literal(p));
          cur = b.call_value(cur, args, p);
          break;
        }
      }
    }
    return cur;
  }

  NodeId emit_assign(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const std::string op(a.nodes[1]->token);
    const Ast& target = *a.nodes[0];
    const Ast& rhs = *a.nodes[2];

    const auto combine = [&](NodeId cur) -> NodeId {
      const NodeId v = emit_expr(rhs, ctx);
      if (op == "=") return v;
      const BinOp o = op == "+="   ? BinOp::Add
                      : op == "-=" ? BinOp::Sub
                      : op == "*=" ? BinOp::Mul
                      : op == "/=" ? BinOp::Div
                                   : BinOp::Mod;
      return b.binary(o, cur, v);
    };

    if (target.tag == "ident"_) {
      const auto it = ref_of.find(&target);
      if (it == ref_of.end()) fail(target, "cannot assign to this");
      const int32_t v = it->second;
      if (!is_mut[static_cast<size_t>(v)]) {
        fail(target, "cannot assign to '" +
                         rs.vars[static_cast<size_t>(v)].name +
                         "': it is not mut");
      }
      return write_var(v, combine(read_var(v, ctx, p)), ctx, p);
    }
    if (target.tag != "postfix"_ || target.nodes.size() < 2) {
      fail(target, "cannot assign to this expression");
    }
    const size_t limit = target.nodes.size();
    const Ast& last = *target.nodes[limit - 1];
    const bool member = last.tag == "membersfx"_;
    if (!member && last.tag != "indexsfx"_) {
      fail(last, "cannot assign to this expression");
    }
    const NodeId key = member ? b.str_literal(std::string(last.nodes[0]->token))
                              : emit_expr(*last.nodes[0], ctx);
    const int32_t tr = ctx.alloc_local("$recv");
    const int32_t tk = ctx.alloc_local("$key");
    const NodeId recv = emit_postfix(target, limit - 1, ctx);
    const NodeId R = b.varref(VarKind::Local, tr);
    const NodeId K = b.varref(VarKind::Local, tk);
    const NodeId cur = op == "=" ? b.nil_literal()
                                 : helper(member ? "$mem" : "$idx", {R, K}, p);
    return b.block(
        {b.assign(VarKind::Local, tr, recv), b.assign(VarKind::Local, tk, key),
         helper(member ? "$setmem" : "$setidx", {R, K, combine(cur)}, p)});
  }

  // -- One function's body -------------------------------------------------
  void emit_fn(int32_t f) {
    const FnInfo& fi = fns[static_cast<size_t>(f)];
    const Resolver::Fn& rf = rs.fns[static_cast<size_t>(f)];
    if (fi.is_ctor) return;  // emit_class built it by hand
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
    const bool entry = f == 0;

    NodeId body;
    if (fi.body->tag == "block"_ || fi.body->tag == "program"_) {
      body = emit_block(*fi.body, ctx, entry);
    } else {
      body = emit_expr(*fi.body, ctx);  // a lambda's expression body
    }

    std::vector<NodeId> stmts;
    stmts.insert(stmts.end(), pre.begin(), pre.end());
    stmts.push_back(body);

    Func fn;
    fn.name = fi.name;
    fn.num_params = nparams;
    fn.num_locals = ctx.high_local;
    fn.local_names = ctx.names();
    fn.num_cells = rs.num_cells(f);
    fn.lenient_arity = true;
    fn.is_generator = fi.is_generator;
    fn.num_captures = m.funcs[static_cast<size_t>(rf.index)].num_captures;
    fn.capture_names = m.funcs[static_cast<size_t>(rf.index)].capture_names;
    // A culebra function answers its body's last value, so the whole thing
    // is the operand of one Return. The entry point's Scope is [0, 0):
    // with entry_frame_drops = false that is what still runs a top-level
    // `defer` while leaving the top-level bindings to be released without
    // their destructors -- culebra's own rule, stated in RunOptions.
    fn.body = b.make_return(b.scope(0, entry ? 0 : nparams, b.block(stmts)));
    m.funcs[static_cast<size_t>(rf.index)] = std::move(fn);
  }

  Module build(const Ast& program) {
    const int32_t top = new_fn(-1, "main");
    fns[static_cast<size_t>(top)].body = &program;

    rs.push_scope();
    scope_order.emplace_back();
    for (const auto& s : program.nodes) resolve_stmt(*s, top);
    block_decls[&program] = scope_order.back();
    scope_order.pop_back();
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

}  // namespace

namespace {

// ==== The standard library, as host functions ==============================
//
// The top-level README's **Host functions** recipe, and the only front end
// here that uses it. A culebra program's `println` and `[1,2].map(f)` are
// not language constructs and are not IR either: the module declares the
// names (Builder::declare_native, reached by Tag::NativeRef) and the run
// supplies them. The linkage happens before the first instruction -- a name
// this file forgot would fail the whole run, not the call site.
//
// Note what is *not* here. Display conversion ("4.0" for a whole Float) is
// in the binder's own IR, because it is a rule of the language rather than
// a service of the host; `println` is handed a string that the program
// already built. That split -- the host does output, the language does
// meaning -- is the same one examples/mini-go draws between vmlib's
// scheduler primitives and Go's channel rules.

// coreir_rt_out_str is the one that appends a newline and
// coreir_rt_out_raw the one that does not -- which is exactly culebra's
// own println/print split, so each maps to one of them.
bool native_println(NativeCall& c) {
  const std::string& s = c.arg(0).as_str();
  coreir_rt_out_str(s.data(), static_cast<int64_t>(s.size()));
  c.result = Value();
  return true;
}

bool native_print(NativeCall& c) {
  const std::string& s = c.arg(0).as_str();
  coreir_rt_out_raw(s.data(), static_cast<int64_t>(s.size()));
  c.result = Value();
  return true;
}

// culebra's own type names, which are not the VM's ("Long", not "int").
// Mapping one vocabulary onto the other is exactly the kind of thing a
// host function is for -- the IR has TypeOf, and what a language calls
// each answer is the language's business.
bool native_type_of(NativeCall& c) {
  const char* n = "Object";
  switch (c.arg(0).tag()) {
    case ValueTag::Nil: n = "Nil"; break;
    case ValueTag::Bool: n = "Bool"; break;
    case ValueTag::Int: n = "Long"; break;
    case ValueTag::Double: n = "Float"; break;
    case ValueTag::Str: n = "String"; break;
    case ValueTag::Array: n = "Array"; break;
    case ValueTag::Func:
    case ValueTag::Native: n = "Function"; break;
    case ValueTag::Generator: n = "Generator"; break;
    default: break;
  }
  c.result = Value::make_str(n);
  return true;
}

bool native_size(NativeCall& c) {
  const Value& v = c.arg(0);
  if (v.is_str()) {
    c.result = Value::make_int(static_cast<int64_t>(v.as_str().size()));
  } else if (v.is_array()) {
    c.result =
        Value::make_int(static_cast<int64_t>(v.as_array()->items.size()));
  } else if (v.is_object()) {
    c.result =
        Value::make_int(static_cast<int64_t>(v.as_object()->props.size()));
  } else {
    c.error = c.trap("size() wants a String, an Array or an Object");
    return false;
  }
  return true;
}

bool native_push(NativeCall& c) {
  if (!c.arg(0).is_array()) {
    c.error = c.trap("push() wants an Array");
    return false;
  }
  c.arg(0).as_array()->items.push_back(c.arg(1));
  c.result = Value();
  return true;
}

bool native_pop(NativeCall& c) {
  if (!c.arg(0).is_array() || c.arg(0).as_array()->items.empty()) {
    c.error = c.trap("pop() wants a non-empty Array");
    return false;
  }
  auto& items = c.arg(0).as_array()->items;
  c.result = items.back();
  items.pop_back();
  return true;
}

bool native_keys(NativeCall& c) {
  if (!c.arg(0).is_object()) {
    c.error = c.trap("keys() wants an Object");
    return false;
  }
  std::vector<Value> out;
  for (const auto& kv : c.arg(0).as_object()->props) {
    // The two keys this front end adds for itself start with a byte no
    // source-level key can contain; a program must not see them.
    if (!kv.first.empty() &&
        static_cast<unsigned char>(kv.first[0]) < 0x20) {
      continue;
    }
    out.push_back(Value::make_str(kv.first));
  }
  c.result = Value::make_array(std::move(out));
  return true;
}

// The half of the contract an outward-facing native would not show:
// NativeCall::call runs program code from inside C++, and a throw the
// callback lets out travels through this frame to the caller's handler.
// The nil in front of the element is the receiver every function in this
// subset takes as its parameter 0 -- see resolve_fn.
bool native_map(NativeCall& c) {
  if (!c.arg(0).is_array()) {
    c.error = c.trap("map() wants an Array");
    return false;
  }
  const Value f = c.arg(1);
  std::vector<Value> out;
  // By index rather than by iterator: the callback may push onto the very
  // array being walked, and a reallocation would leave an iterator dangling.
  for (size_t i = 0; i < c.arg(0).as_array()->items.size(); ++i) {
    const Value argv[2] = {Value(), c.arg(0).as_array()->items[i]};
    out.push_back(c.call(f, argv, 2));
  }
  c.result = Value::make_array(std::move(out));
  return true;
}

}  // namespace

const std::vector<vm::NativeDef>& stdlib() {
  static const std::vector<vm::NativeDef> defs = {
      {"println", 1, native_println, nullptr},
      {"print", 1, native_print, nullptr},
      {"type_of", 1, native_type_of, nullptr},
      {"size", 1, native_size, nullptr},
      {"push", 2, native_push, nullptr},
      {"pop", 1, native_pop, nullptr},
      {"keys", 1, native_keys, nullptr},
      {"map", 2, native_map, nullptr},
  };
  return defs;
}

Module bind_source(const std::string& source) {
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
  Module m = b.build(*ast);

  if (auto err = verify(m)) {
    coreir_rt::fail("internal error: malformed IR: " + *err, 0, 0);
  }
  return m;
}

}  // namespace mini_culebra
