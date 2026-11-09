// The front end whose oracle is the language this library was written to
// rehearse a back end for. Three of vmlib.h's mechanisms exist in the shape
// they do *because* of culebra, and say so in their own comments; none of
// the other front ends here uses any of them, so this is where they are
// shown working against the language that motivated them:
//
//   * **The owned stack.** "Deterministic drop for cycles: the owned stack
//     (culebra's design)." An Object given a callable destructor (DropSet)
//     is a resource: its destructor runs when its count reaches zero, at the
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
// And one recipe that is this front end's whole standard library: **Host
// functions**. culebra's standard library is not part of its language, and
// it is not part of this IR either -- `println`, `type_of`, `.size()`,
// `.push()`, `.map()` and the rest are Tag::NativeRef, declared by the
// module and supplied by the run (stdlib(), at the bottom of this file).
// `map` in particular calls back into the program, which is the half of
// the contract a purely outward-facing native would not show.
//
// What this front end does *not* need, and the contrast is the point:
// mini-js writes `$truthy` and `$seq` in IR because JavaScript disagrees
// with the VM about what is true and what is comparable. culebra does not.
// `if` here takes a Bool, a Long or a Float and nothing else -- which is
// Value::truthy()'s own rule for those three -- so a condition is a bare
// Tag::If with no call in front of it. `/` on two Longs is integer
// division and `%` is C's, which is BinOp::Div and BinOp::Mod -- but for
// a zero divisor, a Float's too, which raises ZeroDivisionError, so the
// two are exact (vmlib.h's kExact) with a helper for that. Beside it,
// equality needed writing (`==` across two types is `false` in culebra
// and a trap in the VM, and arrays and objects compare structurally), and
// display did (culebra prints a whole Float as "4.0" and 1e15 as "1e+15",
// which is double_format.h's, called as a host function).

#include "binder.h"

#include "double_format.h"

#include <cstdlib>
#include <limits>
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

namespace mini_culebra {
namespace {

// Classes are runtime_parts.h's layout, less the base culebra does not
// have: an instance's meta is its class's method table, holding the
// methods by name, and the class's own facts are the table's kind record
// -- its name, which is what an instance prints and what `type_of`
// answers for one, the declared `new` the synthesized constructor calls,
// and the method table itself. The class value is a third object, the
// one its name evaluates to, because it is a program object: it prints as
// `{new: [function], name: 'Point'}`, its methods being no more
// enumerable than an Error's fields are in JavaScript, and a program can
// index and write it like any object -- so a method named `new` or `name`
// meets neither field, and nothing a program writes on the class reaches
// one. Its meta is the same kind record, which is what makes it a class
// to `type_of`, and how the constructor reaches the method table.
constexpr int64_t kClassInit = kClassOwn;
constexpr int64_t kClassMethods = kClassOwn + 1;
// The two fields a class value shows a program, in the order culebra
// prints them: its constructor and a copy of its name. Writing either is
// an error in culebra and goes unchecked here (README.md); the copy is all
// such a write can reach.
constexpr char kNewField[] = "new";
constexpr char kNameField[] = "name";
// The property that is an object's destructor, in culebra; here it is the
// object's destructor (DropSet), which no key reaches.
constexpr char kDropField[] = "drop";

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
  // A classdecl's methods, by name, in declaration order, and the
  // constructor this binder synthesizes for it.
  std::map<const Ast*, std::vector<std::pair<std::string, int32_t>>> class_of;
  std::map<const Ast*, int32_t> ctor_of;
  HelperTable rt{m};  // the runtime helpers, by name
  Operators ops{rt, operators()};  // the VM's operations standing in for them

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
        class_of[&a] = methods;
        ctor_of[&a] = ctor;
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
        "$eq",   "$disp",  "$insp", "$arrstr", "$objstr",
        "$err",  "$idx",   "$setidx", "$mem", "$setmem", "$methodof",
        "$iter", "$iternext", "$div", "$mod",
    };
    return names;
  }

  // culebra's rules that are the VM's own operation on some values: the
  // VM's answer where the two agree, the helper's on the rest
  // (runtime_parts.h's Operators).
  //
  //   $eq   `==` (equality_tags): two numbers numerically, two types Eq
  //         refuses unequal; arrays and objects, compared structurally,
  //         are $eq's.
  //   $idx  `xs[i]` on an array and an index in range, or `o['k']` on an
  //         object and a key it has; one out of range or not a Long, a
  //         key an object lacks (a KeyError, where Index answers nil), a
  //         string receiver (a TypeError) are $idx's.
  //   $setidx  `xs[i] = v` and `o['k'] = v` on the same, a new key
  //         included; an index out of range is $setidx's.
  //   $setmem  `o.k = v` on an object, a new key included (an object with a
  //         meta leaves that to $setmem).
  //   $div  `/` and `%` on two numbers, exact: a zero divisor, a Float's
  //   $mod  as well as a Long's, raises ZeroDivisionError, which is the
  //         helper's.
  static Operators::Table operators() {
    return {
        {"$eq", VmOp::binary(BinOp::Eq, equality_tags())},
        {"$idx", VmOp::index(tags_named({"array", "object"}), 0,
                             tags_named({"int", "string"}))},
        {"$setidx", VmOp::set_index(tags_named({"array", "object"}), kMissNil,
                                    tags_named({"int", "string"}))},
        {"$setmem", VmOp::set_index(tags_named("object"), kMissNil,
                                    tags_named("string"))},
        {"$div", VmOp::binary(BinOp::Div, number_tags(), kExact)},
        {"$mod", VmOp::binary(BinOp::Mod, number_tags(), kExact)},
    };
  }

  // One helper of the runtime above: coreir::HelperWriter over this binder's
  // runtime.
  struct RT : HelperWriter {
    explicit RT(Binder& bd) : HelperWriter(bd.rt) {}
  };

  // culebra's `==`. Two numbers compare numerically whatever their widths
  // (`1 == 1.0` is true); two values of different types are unequal rather
  // than an error, which is where binop_fault stops -- "a question a
  // language answers, not the VM"; and arrays and objects compare
  // *structurally*, which is culebra's rule and the reason this is a
  // recursive function rather than an intrinsic.
  void rt_eq() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [i, ks, k] = r.locals("i", "ks", "k");
    // Two objects are equal when they hold equal fields and their metas
    // are one: an instance's class, or nothing for a literal -- so an
    // instance is equal to no literal, and to no other class's instance.
    const NodeId objects = r.blk(
        {r.iff(r.bin(BinOp::Eq,
                     r.in(IntrinsicId::Same,
                          {r.meta(r.L(a)), r.meta(r.L(b))}),
                     r.Bo(false)),
               r.ret(r.Bo(false))),
         equal_entries(r, a, b, i, ks, k, "$eq")});
    r.add(r.iff(is_number(r, r.L(a)),
                r.iff(is_number(r, r.L(b)),
                      r.ret(r.bin(BinOp::Eq, r.L(a), r.L(b))))));
    r.add(r.iff(r.bin(BinOp::Ne, r.typ(a), r.typ(b)), r.ret(r.Bo(false))));

    std::vector<std::pair<NodeId, NodeId>> arms;
    arms.emplace_back(r.S("nil"), r.ret(r.Bo(true)));
    // Each key its own body, built alike: one body under two keys would
    // be compiled twice anyway.
    const auto scalar = [&] { return r.ret(r.bin(BinOp::Eq, r.L(a), r.L(b))); };
    arms.emplace_back(r.S("bool"), scalar());
    arms.emplace_back(r.S("string"), scalar());
    // Arrays: same length, and equal element by element. Objects: same
    // key count, and every key of the left present in the right with an
    // equal value.
    arms.emplace_back(r.S("array"), equal_elements(r, a, b, i, "$eq"));
    arms.emplace_back(r.S("object"), objects);
    r.add(r.b.make_switch(r.typ(a), arms,
                          r.ret(r.in(IntrinsicId::Same, {r.L(a), r.L(b)}))));
    r.finish("$eq");
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
    // A Float: the shortest round trip in double_format.h's kCulebra layout
    // (1e+15, 1e-05, 100000.0), written by the host (dstr).
    arms.emplace_back(r.S("double"), r.ret(r.nat("dstr", {r.L(v)})));
    arms.emplace_back(r.S("array"), r.ret(r.call("$arrstr", {r.L(v)})));
    arms.emplace_back(r.S("object"), r.ret(r.call("$objstr", {r.L(v)})));
    arms.emplace_back(r.S("function"), r.ret(r.S("[function]")));
    arms.emplace_back(r.S("generator"), r.ret(r.S("<generator>")));
    r.add(r.b.make_switch(r.typ(v), arms,
                          r.ret(r.in(IntrinsicId::ToStr, {r.L(v)}))));
    r.finish("$disp");
  }

  void rt_insp() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.iff(r.is(r.typ(v), "string"),
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

  // An object's own keys, in insertion order -- every one of them: what
  // this front end keeps on an object, its class and its destructor, is
  // no key. A class instance prints its class name first and marks its
  // fields `mut`, which is culebra's own rendering and the reason the
  // class has to be reachable from the instance at all.
  void rt_objstr() {
    RT r(*this);
    const auto [o] = r.params("o");
    const auto [out, i, ks, k, n, mut, cls] =
        r.locals("out", "i", "ks", "k", "n", "mut", "cls");
    r.add(r.set(out, r.S("{")));
    r.add(r.set(mut, r.S("")));
    r.add(r.set(cls, instance_class(r, o)));
    r.add(r.iff(r.isnt(r.typ(cls), "nil"),
                r.blk({r.set(mut, r.S("mut ")),
                       r.set(out, r.bin(BinOp::Add, class_name(r, r.L(cls)),
                                        r.S(" {")))})));
    r.add(r.set(i, r.I(0)));
    r.add(r.set(ks, r.in(IntrinsicId::ObjectKeys, {r.L(o)})));
    r.add(r.set(n, r.I(0)));
    r.add(r.b.make_while(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(ks))),
        r.blk({
            r.set(k, r.idx(r.L(ks), r.L(i))),
            r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1))),
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

  // `/` and `%` where Div and Mod do not answer for culebra: a zero
  // divisor raises ZeroDivisionError, a Float's too (where Div answers inf
  // and Mod NaN), and anything else -- the least Long over -1, which
  // wraps as culebra's does, or what is no number -- is the operator's.
  void rt_division(const char* name, BinOp op) {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    r.add(r.iff(is_zero_divisor(r, r.L(b)),
                r.ret(r.call("$err", {r.S("ZeroDivisionError"),
                                      r.S("divide by 0 error")}))));
    r.add(r.ret(r.bin(op, r.L(a), r.L(b))));
    r.finish(name);
  }

  // `a[i]` and `o['k']`: strict, unlike `.` -- an absent key is a KeyError
  // and an out-of-range index an IndexError, both of which culebra raises
  // and this front end therefore raises too, rather than letting Index's
  // own trap message stand in for one of them.
  void rt_idx() {
    RT r(*this);
    const auto [recv, key] = r.params("recv", "key");
    std::vector<std::pair<NodeId, NodeId>> arms;
    // An array's index, out of range or no int, is the VM's trap, which
    // the catch converts to culebra's IndexError and TypeError.
    arms.emplace_back(r.S("array"), r.ret(r.idx(r.L(recv), r.L(key))));
    arms.emplace_back(
        r.S("object"),
        r.blk(
            {r.iff(r.isnt(r.typ(key), "string"),
                   r.ret(r.call("$err", {r.S("TypeError"),
                                         r.S("object key must be a String")}))),
             r.iff(r.b.unary(UnOp::Not, r.in(IntrinsicId::ObjectHas,
                                             {r.L(recv), r.L(key)})),
                   r.ret(r.call("$err",
                                {r.S("KeyError"), r.S("key not present")}))),
             r.ret(r.idx(r.L(recv), r.L(key)))}));
    // culebra does not index a String, which the VM would.
    arms.emplace_back(
        r.S("string"),
        r.ret(r.call("$err", {r.S("TypeError"),
                              r.S("type error: expected Array, got String")})));
    r.add(r.b.make_switch(r.typ(recv), arms,
                          r.ret(r.idx(r.L(recv), r.L(key)))));
    r.finish("$idx");
  }

  void rt_setidx() {
    RT r(*this);
    const auto [recv, key, val] = r.params("recv", "key", "val");
    std::vector<std::pair<NodeId, NodeId>> arms;
    arms.emplace_back(
        r.S("array"),
        r.blk({r.b.set_index(r.L(recv), r.L(key), r.L(val)), r.ret(r.L(val))}));
    arms.emplace_back(
        r.S("object"),
        r.blk({r.b.set_index(r.L(recv), r.L(key), r.L(val)), r.ret(r.L(val))}));
    r.add(r.b.make_switch(
        r.typ(recv), arms,
        r.blk({r.b.set_index(r.L(recv), r.L(key), r.L(val)),
               r.ret(r.L(val))})));
    r.finish("$setidx");
  }

  // `o.name`: lenient, culebra's own split from `o['name']` -- reading a
  // field that is not there answers nil rather than raising.
  void rt_mem() {
    RT r(*this);
    const auto [recv, name] = r.params("recv", "name");
    r.add(
        r.iff(r.is(r.typ(recv), "object"), r.ret(r.idx(r.L(recv), r.L(name)))));
    r.add(r.ret(r.call("$err", {r.S("TypeError"),
                                r.S("value has no fields")})));
    r.finish("$mem");
  }

  void rt_setmem() {
    RT r(*this);
    const auto [recv, name, val] = r.params("recv", "name", "val");
    r.add(r.iff(r.is(r.typ(recv), "object"),
                r.blk({r.b.set_index(r.L(recv), r.L(name), r.L(val)),
                       r.ret(r.L(val))})));
    r.add(r.ret(r.call("$err", {r.S("TypeError"),
                                r.S("value has no fields")})));
    r.finish("$setmem");
  }

  // Method lookup: the object's own properties first (an object literal
  // holding a function is how culebra writes a protocol), then the method
  // table that is the instance's meta.
  void rt_methodof() {
    RT r(*this);
    const auto [recv, name] = r.params("recv", "name");
    const auto [cls] = r.locals("cls");
    r.add(r.iff(
        r.is(r.typ(recv), "object"),
        r.iff(r.in(IntrinsicId::ObjectHas, {r.L(recv), r.L(name)}),
              r.ret(r.idx(r.L(recv), r.L(name))))));
    r.add(r.set(cls, instance_class(r, recv)));
    r.add(find_in_chain(r, cls, name));
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
    const auto [cls] = r.locals("cls");
    r.add(r.iff(r.is(r.typ(v), "generator"),
                r.ret(generator_cursor(r, r.L(v)))));
    r.add(r.iff(r.is(r.typ(v), "array"), r.ret(sequence_cursor(r, r.L(v)))));
    r.add(r.set(cls, instance_class(r, v)));
    r.add(r.iff(
        r.is(r.typ(cls), "object"),
        r.iff(r.in(IntrinsicId::ObjectHas, {r.L(cls), r.S("iter")}),
              r.ret(cursor(r, "c",
                           r.b.call_value(r.idx(r.L(cls), r.S("iter")),
                                          {r.L(v)}))))));
    r.add(r.ret(r.call("$err", {r.S("TypeError"),
                                r.S("value is not iterable")})));
    r.finish("$iter");
  }

  // culebra's own kind of cursor, "c" -- an object whose class answered
  // `iter`, driven by `has_next` and `next` -- and then the shared step
  // (runtime_parts.h) for a generator's and an array's.
  void rt_iternext() {
    RT r(*this);
    const auto [it] = r.params("it");
    const auto [obj, hn] = r.locals("obj", "hn");
    r.add(r.iff(
        cursor_is(r, it, "c"),
        r.blk({r.set(obj, cursor_source(r, it)),
               r.set(hn, r.b.call_value(
                             r.call("$methodof", {r.L(obj), r.S("has_next")}),
                             {r.L(obj)})),
               r.iff(r.bin(BinOp::Eq, r.L(hn), r.Bo(false)),
                     r.ret(step_done(r))),
               r.ret(step(r, r.b.call_value(
                                 r.call("$methodof", {r.L(obj), r.S("next")}),
                                 {r.L(obj)})))})));
    iter_step(r, it);
    r.finish("$iternext");
  }

  void emit_runtime() {
    rt_eq();
    rt_disp();
    rt_insp();
    rt_arrstr();
    rt_objstr();
    rt_err();
    rt_division("$div", BinOp::Div);
    rt_division("$mod", BinOp::Mod);
    rt_idx();
    rt_setidx();
    rt_mem();
    rt_setmem();
    rt_methodof();
    rt_iter();
    rt_iternext();
  }

  // ==== Pass B: emit ======================================================

  NodeId native(const std::string& name, const std::vector<NodeId>& args,
                SrcPos p) {
    auto b = Builder(m).at(p);
    return b.call_value(b.native_ref(b.declare_native(name)), args);
  }

  NodeId emit_closure(int32_t g, FnCtx& ctx, SrcPos p) {
    return rs.closure(m, ctx.fn, g, p);
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
    return rs.read(m, ctx.fn, v, p);
  }

  NodeId write_var(int32_t v, NodeId value, FnCtx& ctx, SrcPos p) {
    return rs.write(m, ctx.fn, v, value, p);
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

  // `value` says whether anything reads the block's value -- its last
  // statement's -- which a loop's body, for one, never does.
  NodeId emit_block(const Ast& block, FnCtx& ctx, bool entry = false,
                    bool value = true) {
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
    for (size_t k = 0; k < block.nodes.size(); ++k) {
      const bool last = k + 1 == block.nodes.size();
      out.push_back(emit_stmt(*block.nodes[k], ctx, value && last));
    }

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
  // `value` says whether anything reads the statement's value: only a
  // block's last statement has one that might be read, and a binding or an
  // assignment that nothing reads is the store alone.
  NodeId emit_stmt(const Ast& a, FnCtx& ctx, bool value = true) {
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
        const NodeId bind =
            bind_decl(v, emit_expr(*a.nodes[2], ctx), ctx, p, false);
        return value ? b.block({bind, read_var(v, ctx, p)}) : bind;
      }
      case "block"_:
        return emit_block(a, ctx, false, value);
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
                            emit_block(*a.nodes[1], ctx, false, false));
      case "forstmt"_:
        return emit_for(a, ctx);
      case "exprstmt"_:
        if (a.nodes[0]->tag == "assign"_) {
          return emit_assign(*a.nodes[0], ctx, value);
        }
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
    const NodeId body =
        for_each(rt, p, it, st,
                 rt.call("$iter", {emit_expr(*a.nodes[0], ctx)}, p),
                 {b.make_yield(value_of(b, st))});
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

    const int32_t v = decl_of.at(a.nodes[0].get());
    const int32_t emark = ctx.mark();
    const auto& cells = rs.fns[static_cast<size_t>(ctx.fn)].cell_index;
    std::vector<NodeId> elem;
    const auto c = cells.find(v);
    if (c != cells.end()) elem.push_back(b.cell_fresh(c->second));
    elem.push_back(bind_decl(v, value_of(b, st), ctx, p, false));
    elem.push_back(emit_block(*a.nodes[2], ctx, false, false));
    const int32_t eend = ctx.release(emark);
    const NodeId elem_scope =
        eend > emark
            ? b.scope(emark, eend, b.block(elem), release_list({v}, ctx, p))
            : b.scope(emark, emark, b.block(elem), release_list({v}, ctx, p));

    const NodeId body =
        for_each(rt, p, it, st,
                 rt.call("$iter", {emit_expr(*a.nodes[1], ctx)}, p),
                 {elem_scope});
    const int32_t end = ctx.release(mark);
    return b.scope(mark, end, body);
  }

  // class C { new(...) {...} m(...) {...} drop() {...} }
  //
  //   M = { m: <m>, drop: <drop> }    -- the method table
  //   R = ["class", "C", nil, <new>, M]
  //   C = {}   -- a cell; C's meta and M's are both R
  //   C["new"] = MakeClosure(<synthesized constructor>, over C)
  //   C["name"] = "C"
  //
  // `new` and `name` are the two fields a program sees, in that order, as
  // culebra prints a class value.
  //
  // The constructor is what turns the class into an instance: a fresh
  // object whose meta is M (so a method call can find its methods) and,
  // when the class declares one, `drop` as its destructor (DropSet, so the
  // object *is* a resource). Giving it one is what puts it on the owned
  // stack; everything about when its destructor runs follows from that.
  NodeId emit_class(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const auto& methods = class_of.at(&a);
    const int32_t v = decl_of.at(a.nodes[0].get());
    const int32_t cell = rs.fns[static_cast<size_t>(ctx.fn)].cell_index.at(v);

    const int32_t ctor_fn = ctor_of.at(&a);
    int32_t init_params = 0;
    bool has_drop = false;
    NodeId init = b.nil_literal();
    std::vector<std::pair<NodeId, NodeId>> kvs;
    for (const auto& [name, g] : methods) {
      if (name == kDropField) has_drop = true;
      if (name == kNewField) {
        init_params =
            static_cast<int32_t>(fns[static_cast<size_t>(g)].params.size()) - 1;
        init = emit_closure(g, ctx, p);
        continue;
      }
      kvs.emplace_back(b.str_literal(name), emit_closure(g, ctx, p));
    }
    const std::string cname(a.nodes[0]->token);
    emit_ctor(ctor_fn, init_params, has_drop, cname);

    std::vector<CaptureSrc> cs{{VarKind::Cell, cell}};
    const int32_t cm = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back(cs);
    // The class value first, its record holding the method table; then
    // the method table's meta, the same record.
    const NodeId C = b.varref(VarKind::Cell, cell);
    return b.block(
        {b.assign(VarKind::Cell, cell,
                  class_table(b, b.object_lit({}), cname, b.nil_literal(),
                              {init, b.object_lit(kvs)})),
         b.intrinsic(IntrinsicId::MetaSet,
                     {kind_field(b, C, kClassMethods),
                      b.intrinsic(IntrinsicId::MetaGet, {C})}),
         b.set_index(
             b.varref(VarKind::Cell, cell), b.str_literal(kNewField),
             b.make_closure(rs.fns[static_cast<size_t>(ctor_fn)].index, cm)),
         b.set_index(b.varref(VarKind::Cell, cell), b.str_literal(kNameField),
                     b.str_literal(cname))});
  }

  void emit_ctor(int32_t g, int32_t init_params, bool has_drop,
                 const std::string& cname) {
    Builder b(m);
    const SrcPos p{0, 0};
    const int32_t nparams = 1 + init_params;  // self, then `new`'s own
    const int32_t inst = nparams;
    const NodeId C = b.varref(VarKind::Capture, 0, p);
    const NodeId O = b.varref(VarKind::Local, inst, p);
    auto bp = b.at(p);
    std::vector<NodeId> body{b.assign(
        VarKind::Local, inst,
        new_instance(bp, kind_field(bp, C, kClassMethods)), p)};
    if (has_drop) {
      body.push_back(bp.intrinsic(
          IntrinsicId::DropSet,
          {O, bp.index(kind_field(bp, C, kClassMethods),
                       bp.str_literal(kDropField))}));
    }
    std::vector<NodeId> init_args{O};
    for (int32_t i = 1; i < nparams; ++i) {
      init_args.push_back(b.varref(VarKind::Local, i, p));
    }
    body.push_back(b.call_value(kind_field(bp, C, kClassInit), init_args, p));
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
                  : rt.call("$disp", {emit_expr(*part->nodes[0], ctx)}, p);
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
        // An executor's trap is {message, line, col}; `catch e` reads a
        // kind and a message, which the conversion gives it.
        std::vector<NodeId> hs{
            b.assign(VarKind::Local, exc,
                     converted(b, b.varref(VarKind::Local, exc)))};
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
          const NodeId rhs = emit_expr(*a.nodes[i], ctx);
          acc = b.let(acc, [&](auto keep) {
            return b.make_if(keep(), is_or ? keep() : rhs,
                             is_or ? rhs : keep());
          });
        }
        return acc;
      }
      case "equality"_: {
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i + 1 < a.nodes.size(); i += 2) {
          const Ast& op = *a.nodes[i];
          const NodeId rhs = emit_expr(*a.nodes[i + 1], ctx);
          const NodeId eq = ops.call("$eq", {acc, rhs}, pos_of(op));
          acc = op.token == "==" ? eq : b.at(pos_of(op)).unary(UnOp::Not, eq);
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
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i + 1 < a.nodes.size(); i += 2) {
          const Ast& op = *a.nodes[i];
          acc = arith(std::string(op.token), acc,
                      emit_expr(*a.nodes[i + 1], ctx), pos_of(op));
        }
        return acc;
      }
      case "assign"_:
        return emit_assign(a, ctx, true);
      case "postfix"_:
        return emit_postfix(a, a.nodes.size(), ctx);
      case "arraylit"_: {
        std::vector<NodeId> items;
        items.reserve(a.nodes.size());
        for (const auto& c : a.nodes) items.push_back(emit_expr(*c, ctx));
        return b.array_lit(items);
      }
      case "objectlit"_: {
        // A `drop` property is the object's destructor (DropSet), not a
        // property named "drop": giving it one is what registers the
        // object on the owned stack, which is the whole of "this literal
        // is a resource". The entries before the first `drop` are the
        // literal; that one and any after it are given or stored in
        // source order, as the literal would have run them, on the
        // object a Let holds -- which a literal ending in its `drop`,
        // the usual shape, needs not: DropSet answers the object it gave
        // the destructor, so the literal and its `drop` are one
        // expression.
        const auto key_of = [](const Ast& c) {
          const Ast& key = *c.nodes[0];
          return key.tag == "string"_ ? unescape(std::string(key.token))
                                      : std::string(key.token);
        };
        const size_t n = a.nodes.size();
        size_t first = n;
        for (size_t i = 0; i < n; ++i) {
          if (key_of(*a.nodes[i]) == kDropField) {
            first = i;
            break;
          }
        }
        std::vector<std::pair<NodeId, NodeId>> kvs;
        for (size_t i = 0; i < first; ++i) {
          kvs.emplace_back(b.str_literal(key_of(*a.nodes[i])),
                           emit_expr(*a.nodes[i]->nodes[1], ctx));
        }
        const NodeId obj = b.object_lit(kvs);
        if (first == n) return obj;
        if (first + 1 == n) {
          return b.intrinsic(IntrinsicId::DropSet,
                             {obj, emit_expr(*a.nodes[first]->nodes[1], ctx)});
        }
        return b.let(obj, [&](auto o) {
          std::vector<NodeId> steps;
          for (size_t i = first; i < n; ++i) {
            const std::string k = key_of(*a.nodes[i]);
            const NodeId v = emit_expr(*a.nodes[i]->nodes[1], ctx);
            steps.push_back(k == kDropField
                                ? b.intrinsic(IntrinsicId::DropSet, {o(), v})
                                : b.set_index(o(), b.str_literal(k), v));
          }
          steps.push_back(o());
          return b.block(steps);
        });
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
        // Every argument is displayed and handed on, so a count the native
        // does not take is the executor's arity trap once they have all
        // run. `println()` alone is an empty line, as in culebra; `print()`
        // takes its one argument.
        std::vector<NodeId> shown;
        for (const NodeId v : args) shown.push_back(rt.call("$disp", {v}, gp));
        if (shown.empty() && g == "println") shown.push_back(b.str_literal("", gp));
        cur = native(g, shown, gp);
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
            const Ast& arglist = *a.nodes[i + 1]->nodes[0];
            ++i;
            if (is_builtin_method(name)) {
              // culebra's standard library, supplied by the host: the
              // receiver goes first, and `map` calls back into the program
              // from C++ (see stdlib()).
              std::vector<NodeId> args = emit_args(arglist, ctx);
              args.insert(args.begin(), cur);
              cur = native(name, args, p);
              break;
            }
            // Every function's parameter 0 is `self`, so a method call is
            // an ordinary call with the receiver in front. The receiver is
            // both where the method is found and that `self`, so it runs
            // once, into a temporary both read -- scoped to the call, so
            // a receiver nothing else holds dies when the call returns, as
            // it would have as a temporary value. The slot is claimed
            // before the arguments are emitted: they run after it is
            // written, so a scope of theirs must not hand it out again.
            const int32_t mark = ctx.mark();
            const int32_t t = ctx.alloc_local("$recv");
            std::vector<NodeId> args = emit_args(arglist, ctx);
            const NodeId self = b.varref(VarKind::Local, t, p);
            const NodeId f =
                rt.call("$methodof", {self, b.str_literal(name, p)}, p);
            args.insert(args.begin(), self);
            const NodeId call =
                b.block({b.assign(VarKind::Local, t, cur, p),
                         b.call_value(f, args, p)},
                        p);
            const int32_t end = ctx.release(mark);
            cur = b.scope(mark, end, call, p);
            break;
          }
          cur = rt.call("$mem", {cur, b.str_literal(name, p)}, p);
          break;
        }
        case "indexsfx"_:
          cur = ops.call("$idx", {cur, emit_expr(*sfx.nodes[0], ctx)}, p);
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

  // An arithmetic operator, `+`, `-`, `*`, `/` or `%`, and the same one
  // inside `+=` and the rest: straight to BinOp, as binop_fault is culebra's
  // on two numbers, but for `/` and `%`, whose zero divisor raises.
  NodeId arith(const std::string& t, NodeId l, NodeId r, SrcPos p) {
    if (t == "/") return ops.call("$div", {l, r}, p);
    if (t == "%") return ops.call("$mod", {l, r}, p);
    const BinOp o = t == "+" ? BinOp::Add : t == "-" ? BinOp::Sub : BinOp::Mul;
    return Builder(m).at(p).binary(o, l, r);
  }

  // An assignment in culebra's order (docs/language.md, "Evaluation
  // order"): the value first; then the target -- its receiver, then its
  // key -- once; then, for a compound, the present value read through that
  // target; then the store. The assignment is an expression, worth what it
  // stored -- a block that ends in one answers it -- and `as_value` says
  // whether anything reads that.
  NodeId emit_assign(const Ast& a, FnCtx& ctx, bool as_value) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const std::string op(a.nodes[1]->token);
    const Ast& target = *a.nodes[0];
    const Ast& rhs = *a.nodes[2];
    const auto apply = [&](NodeId cur, NodeId v) -> NodeId {
      if (op == "=") return v;
      return arith(op.substr(0, 1), cur, v, p);
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
      std::vector<NodeId> out;
      if (op == "=") {
        out.push_back(write_var(v, emit_expr(rhs, ctx), ctx, p));
      } else {
        // The value runs before the variable is read: `x += f()` adds to
        // what f left in x.
        out.push_back(b.let(emit_expr(rhs, ctx), [&](auto val) {
          return write_var(v, apply(read_var(v, ctx, p), val()), ctx, p);
        }));
      }
      if (as_value) out.push_back(read_var(v, ctx, p));
      return out.size() == 1 ? out[0] : b.block(out);
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
    // The value, then the receiver, then the key, each run once and held
    // by a Let for the read and the write.
    const NodeId value = emit_expr(rhs, ctx);
    const NodeId recv = emit_postfix(target, limit - 1, ctx);
    const NodeId key = member ? b.str_literal(std::string(last.nodes[0]->token))
                              : emit_expr(*last.nodes[0], ctx);
    return b.lets({value, recv, key}, [&](const std::vector<Builder::Temp>& t) {
      const NodeId cur =
          op == "=" ? NodeId{}
                    : rt.call(member ? "$mem" : "$idx", {t[1](), t[2]()}, p);
      const auto store = [&](NodeId v) {
        return ops.call(member ? "$setmem" : "$setidx",
                        {t[1](), t[2](), v}, p);
      };
      return stored_value(b, apply(cur, t[0]()), store, as_value);
    });
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
// each answer is the language's business. An object is typed by what made
// it: an instance by its class's name (in the class's kind record, not the
// `name` field a program could write), a class -- the object carrying that
// record -- as a Class, anything else -- a literal, however class-like its
// fields -- as an Object.
std::string type_name_of(const Value& v) {
  if (v.is_object()) {
    if (const std::string* n = instance_class_name(v)) return *n;
    return kind_record(v, kClassKind) ? "Class" : "Object";
  }
  switch (v.tag()) {
    case ValueTag::Nil: return "Nil";
    case ValueTag::Bool: return "Bool";
    case ValueTag::Int: return "Long";
    case ValueTag::Double: return "Float";
    case ValueTag::Str: return "String";
    case ValueTag::Array: return "Array";
    case ValueTag::Func:
    case ValueTag::Native: return "Function";
    case ValueTag::Generator: return "Generator";
    default: return "Object";
  }
}

bool native_type_of(NativeCall& c) {
  c.result = Value::make_str(type_name_of(c.arg(0)));
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
    // What keys() lists: every key. An instance's class and a resource's
    // destructor are no keys at all.
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

// The executor's trap as culebra's error: {kind, message}, in the words of
// culebra's own runtime (probed against culebra 0.8.0). What the VM
// traps on is what the front end did not check for itself: an operator on
// the wrong types, a call of a non-function, a field of a non-object.
// `type error: ...` is culebra's prefix on every TypeError.
Value convert_trap(const Value& trap, const void*) {
  const auto ty = [&](TrapFact f) { return type_name_of(trap_fact(trap, f)); };
  const std::string l = ty(TrapFact::Left);
  const std::string r = ty(TrapFact::Right);
  const Verb verb = trap_verb(trap);
  std::string kind = "RuntimeError";
  std::string message = trap.as_object()->find("message")->as_str();
  switch (trap_kind(trap)) {
    case TrapKind::Operands:
      kind = "TypeError";
      if (verb.cls == VerbClass::Binary) {
        const auto op = static_cast<BinOp>(verb.id);
        const bool compare = op == BinOp::Lt || op == BinOp::Le ||
                             op == BinOp::Gt || op == BinOp::Ge;
        const bool bits = op == BinOp::BitAnd || op == BinOp::BitOr ||
                          op == BinOp::BitXor || op == BinOp::Shl ||
                          op == BinOp::Shr;
        if (compare) {
          message = "type error: cannot compare " + l + " and " + r;
        } else if (bits) {
          message = "type error: expected Long, got " +
                    (trap_fact(trap, TrapFact::Left).is_int() ? r : l);
        } else {
          message = "type error: cannot apply '" +
                    std::string(binop_symbol(op)) + "' to " + l + " and " + r;
        }
      } else if (verb.cls == VerbClass::Unary) {
        message = static_cast<UnOp>(verb.id) == UnOp::Neg
                      ? "type error: expected Long or Float, got " + l
                      : "type error: expected Long, got " + l;
      } else {
        message = "type error: " + message;
      }
      break;
    case TrapKind::NotCallable:
      kind = "TypeError";
      message = "type error: expected Function, got " + l;
      break;
    case TrapKind::NotIndexable:
      kind = "TypeError";
      message = "type error: expected Object, Array, or Tensor, got " + l;
      break;
    case TrapKind::KeyType:
      kind = "TypeError";
      message = std::string("type error: expected ") +
                (trap_fact(trap, TrapFact::Left).is_object() ? "String"
                                                             : "Long") +
                ", got " + r;
      break;
    case TrapKind::Immutable:
      kind = "TypeError";
      message = "type error: expected Array, got " + l;
      break;
    case TrapKind::Range:
      kind = "IndexError";
      message = "index out of range";
      break;
    case TrapKind::DivideByZero:
      kind = "ZeroDivisionError";
      message = "divide by 0 error";
      break;
    case TrapKind::Arity:
      kind = "DispatchError";
      message = "no matching method for `" +
                trap_fact(trap, TrapFact::Name).as_str() + "`";
      break;
    case TrapKind::Uninit:
      kind = "NameError";
      message = "undefined variable '" +
                trap_fact(trap, TrapFact::Name).as_str() + "'";
      break;
    case TrapKind::Depth:
      kind = "RecursionError";
      message = "maximum recursion depth exceeded (1000)";
      break;
    case TrapKind::Host:
      // The front end's own natives: size(), push() and the rest, each
      // complaining of a type.
      kind = "TypeError";
      break;
    default:
      break;
  }
  Value e = Value::make_object();
  e.as_object()->set("kind", Value::make_str(kind));
  e.as_object()->set("message", Value::make_str(message));
  return e;
}

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
      double_format::dstr_def(double_format::kCulebra),
      trap_convert_def<&convert_trap>(),
  };
  return defs;
}

// What an uncaught `throw` words its value with: to_display, but a Float
// spelled as culebra prints it (`throw 0.0001` ends with "0.0001"). An
// executor's trap reads as culebra's own error does, "TypeError: type
// error: ..." (the position the report adds is culebra's `at 1:9`).
std::string describe(const Value& v) {
  if (trap_kind(v) != TrapKind::None) {
    const Value e = convert_trap(v, nullptr);
    return e.as_object()->find("kind")->as_str() + ": " +
           e.as_object()->find("message")->as_str();
  }
  return double_format::display_with(v, double_format::kCulebra);
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
