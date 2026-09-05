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
struct VarInfo {
  std::string name;
  int32_t owner = 0;
  bool is_mut = false;
};

struct FnInfo {
  int32_t parent = -1;
  int32_t index = -1;  // into Module::funcs
  bool is_generator = false;
  // A class's constructor: synthesized, so it has no body to walk and
  // emit_class builds its Func by hand.
  bool is_ctor = false;
  std::string name = "<anon>";
  std::set<int32_t> free;
  std::map<int32_t, int32_t> capture_index;
  std::map<int32_t, int32_t> cell_index;
  std::vector<int32_t> params;  // VarIds; params[0] is always `self`
  const Ast* body = nullptr;
};

struct FnCtx {
  int32_t fn = 0;
  int32_t next_local = 0;
  int32_t high_local = 0;
  int32_t next_cell = 0;
  std::vector<std::string> local_names;
  std::map<std::string, int32_t> helper_cells;

  int32_t alloc_local(const std::string& name) {
    const int32_t s = next_local++;
    if (next_local > high_local) high_local = next_local;
    if (static_cast<size_t>(s) >= local_names.size()) {
      local_names.resize(static_cast<size_t>(s) + 1, "");
    }
    local_names[static_cast<size_t>(s)] = name;
    return s;
  }
};

struct Binder {
  Module m;
  std::vector<VarInfo> vars;
  std::vector<FnInfo> fns;
  std::map<const Ast*, int32_t> ref_of;
  std::map<const Ast*, int32_t> decl_of;
  std::map<const Ast*, int32_t> fn_of;
  // A classdecl's methods, by name, in declaration order.
  std::map<const Ast*, std::vector<std::pair<std::string, int32_t>>> class_of;
  std::vector<int32_t> slot_of;
  std::map<std::string, int32_t> rt;
  int32_t empty_cmap = -1;

  // ==== Pass A: scopes, declarations, captures =============================

  struct ScopeA {
    int32_t fn;
    std::map<std::string, int32_t> names;
    std::vector<int32_t> order;  // declaration order, for the release list
  };
  std::vector<ScopeA> scopes;
  // Every lexical scope's declaration order, keyed by the node that owns it
  // -- what emit needs to build a Scope's release list without walking the
  // statements a second time.
  std::map<const Ast*, std::vector<int32_t>> block_decls;

  int32_t declare(const std::string& name, int32_t fn, bool is_mut,
                  const Ast& at) {
    // culebra rejects shadowing outright (its "three-tier shadow rules"
    // design note); this subset rejects it within one scope, which is what
    // the samples exercise.
    if (scopes.back().names.count(name)) {
      fail(at, "'" + name + "' is already defined in this scope");
    }
    const int32_t v = static_cast<int32_t>(vars.size());
    vars.push_back({name, fn, is_mut});
    scopes.back().names[name] = v;
    scopes.back().order.push_back(v);
    return v;
  }

  std::optional<int32_t> resolve(const std::string& name, int32_t fn) {
    for (size_t i = scopes.size(); i-- > 0;) {
      auto it = scopes[i].names.find(name);
      if (it == scopes[i].names.end()) continue;
      const int32_t v = it->second;
      const int32_t owner = vars[static_cast<size_t>(v)].owner;
      if (owner != fn) {
        for (int32_t k = fn; k != owner && k >= 0;
             k = fns[static_cast<size_t>(k)].parent) {
          fns[static_cast<size_t>(k)].free.insert(v);
        }
      }
      return v;
    }
    return std::nullopt;
  }

  int32_t new_fn(int32_t parent, const std::string& name) {
    const int32_t f = static_cast<int32_t>(fns.size());
    fns.push_back({});
    fns[static_cast<size_t>(f)].parent = parent;
    fns[static_cast<size_t>(f)].name = name;
    return f;
  }

  // A block, and the record of what it declared: emit needs the order to
  // release in, and culebra's order is the reverse of this one.
  void resolve_block(const Ast& block, int32_t fn) {
    scopes.push_back({fn, {}, {}});
    for (const auto& s : block.nodes) resolve_stmt(*s, fn);
    block_decls[&block] = scopes.back().order;
    scopes.pop_back();
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

    scopes.push_back({f, {}, {}});
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
    scopes.pop_back();
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
        fns[static_cast<size_t>(ctor)].free.insert(v);
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
        scopes.push_back({fn, {}, {}});
        decl_of[a.nodes[0].get()] =
            declare(std::string(a.nodes[0]->token), fn, false, *a.nodes[0]);
        resolve_block(*a.nodes[2], fn);
        block_decls[&a] = scopes.back().order;
        scopes.pop_back();
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
        if (auto v = resolve(n, fn)) {
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
        scopes.push_back({fn, {}, {}});
        decl_of[a.nodes[1].get()] =
            declare(std::string(a.nodes[1]->token), fn, false, *a.nodes[1]);
        resolve_block(*a.nodes[2], fn);
        block_decls[&a] = scopes.back().order;
        scopes.pop_back();
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

  void number_captures() {
    for (size_t f = 0; f < fns.size(); ++f) {
      int32_t i = 0;
      for (const int32_t v : fns[f].free) {
        fns[f].capture_index[v] = i++;
        m.funcs[static_cast<size_t>(fns[f].index)].capture_names.push_back(
            vars[static_cast<size_t>(v)].name);
      }
      m.funcs[static_cast<size_t>(fns[f].index)].num_captures = i;
    }
    for (const auto& fi : fns) {
      for (const int32_t v : fi.free) {
        auto& own = fns[static_cast<size_t>(vars[static_cast<size_t>(v)].owner)]
                        .cell_index;
        if (!own.count(v)) own[v] = static_cast<int32_t>(own.size());
      }
    }
  }

  std::pair<VarKind, int32_t> access(int32_t f, int32_t v) const {
    if (vars[static_cast<size_t>(v)].owner == f) {
      const auto& ci = fns[static_cast<size_t>(f)].cell_index;
      const auto it = ci.find(v);
      if (it != ci.end()) return {VarKind::Cell, it->second};
      return {VarKind::Local, slot_of[static_cast<size_t>(v)]};
    }
    return {VarKind::Capture,
            fns[static_cast<size_t>(f)].capture_index.at(v)};
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

  struct RT {
    Binder& bd;
    Builder b;
    SrcPos p{0, 0};
    std::vector<NodeId> body;

    explicit RT(Binder& bd_) : bd(bd_), b(bd_.m) {}

    NodeId L(int32_t i) { return b.varref(VarKind::Local, i, p); }
    NodeId S(const std::string& s) { return b.str_literal(s, p); }
    NodeId D(double d) { return b.double_literal(d, p); }
    NodeId I(int64_t v) { return b.literal(v, p); }
    NodeId Nil() { return b.nil_literal(p); }
    NodeId Bo(bool v) { return b.bool_literal(v, p); }
    NodeId in(IntrinsicId id, const std::vector<NodeId>& a) {
      return b.intrinsic(id, a, p);
    }
    NodeId bin(BinOp op, NodeId x, NodeId y) { return b.binary(op, x, y, p); }
    NodeId set(int32_t s, NodeId v) {
      return b.assign(VarKind::Local, s, v, p);
    }
    NodeId ret(NodeId v) { return b.make_return(v, p); }
    NodeId blk(const std::vector<NodeId>& v) { return b.block(v, p); }
    NodeId iff(NodeId c, NodeId t) { return b.make_if(c, t, NodeId{}, p); }
    NodeId iff(NodeId c, NodeId t, NodeId e) { return b.make_if(c, t, e, p); }
    NodeId idx(NodeId r, NodeId k) { return b.index(r, k, p); }
    NodeId idx(NodeId r, const std::string& k) { return b.index(r, S(k), p); }
    NodeId obj(const std::vector<std::pair<std::string, NodeId>>& kvs) {
      std::vector<std::pair<NodeId, NodeId>> out;
      for (const auto& kv : kvs) out.emplace_back(S(kv.first), kv.second);
      return b.object_lit(out, p);
    }
    NodeId typ(NodeId v) { return in(IntrinsicId::TypeOf, {v}); }
    NodeId len(NodeId v) { return in(IntrinsicId::Len, {v}); }
    NodeId is(NodeId v, const std::string& s) { return bin(BinOp::Eq, v, S(s)); }
    NodeId isnt(NodeId v, const std::string& s) {
      return bin(BinOp::Ne, v, S(s));
    }
    NodeId call(const std::string& name, const std::vector<NodeId>& a) {
      return b.call_value(
          b.make_closure(bd.rt.at(name), bd.empty_cmap, p), a, p);
    }
    void add(NodeId n) { body.push_back(n); }

    // `a[i]` and `a[i] = v` share these three: the index must be an int,
    // and in range.
    std::vector<NodeId> array_index_checks(NodeId arr, NodeId ix) {
      return {iff(isnt(typ(ix), "int"),
                  ret(call("$err", {S("TypeError"),
                                     S("array index must be a Long")}))),
              iff(bin(BinOp::Lt, ix, I(0)),
                  ret(call("$err", {S("IndexError"), S("index out of range")}))),
              iff(bin(BinOp::Ge, ix, len(arr)),
                  ret(call("$err", {S("IndexError"),
                                     S("index out of range")})))};
    }

    void finish(const std::string& name, int32_t nparams, int32_t nlocals,
                std::vector<std::string> names) {
      Func& f = bd.m.funcs[static_cast<size_t>(bd.rt.at(name))];
      f.name = name;
      f.num_params = nparams;
      f.num_locals = nlocals;
      names.resize(static_cast<size_t>(nlocals), "");
      f.local_names = std::move(names);
      f.lenient_arity = true;
      f.body = b.scope(0, nlocals, blk(body), p);
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
    const auto numeric = [&](NodeId t) {
      return r.b.make_if(r.is(t, "int"), r.Bo(true), r.is(t, "double"), r.p);
    };
    r.add(r.set(2, r.typ(r.L(0))));
    r.add(r.set(3, r.typ(r.L(1))));
    r.add(r.iff(numeric(r.L(2)),
                r.iff(numeric(r.L(3)), r.ret(r.bin(BinOp::Eq, r.L(0), r.L(1))))));
    r.add(r.iff(r.bin(BinOp::Ne, r.L(2), r.L(3)), r.ret(r.Bo(false))));

    std::vector<std::pair<NodeId, NodeId>> arms;
    arms.emplace_back(r.S("nil"), r.ret(r.Bo(true)));
    const NodeId scalar = r.ret(r.bin(BinOp::Eq, r.L(0), r.L(1)));
    arms.emplace_back(r.S("bool"), scalar);
    arms.emplace_back(r.S("string"), scalar);
    // Arrays: same length, and equal element by element.
    arms.emplace_back(
        r.S("array"),
        r.blk({r.iff(r.bin(BinOp::Ne, r.len(r.L(0)), r.len(r.L(1))),
                     r.ret(r.Bo(false))),
               r.set(4, r.I(0)),
               r.b.make_while(
                   r.bin(BinOp::Lt, r.L(4), r.len(r.L(0))),
                   r.blk({r.iff(r.bin(BinOp::Eq,
                                      r.call("$eq", {r.idx(r.L(0), r.L(4)),
                                                     r.idx(r.L(1), r.L(4))}),
                                      r.Bo(false)),
                                r.ret(r.Bo(false))),
                          r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1)))}),
                   r.p),
               r.ret(r.Bo(true))}));
    // Objects: same key count, and every key of the left present in the
    // right with an equal value.
    arms.emplace_back(
        r.S("object"),
        r.blk({r.set(5, r.in(IntrinsicId::ObjectKeys, {r.L(0)})),
               r.iff(r.bin(BinOp::Ne, r.len(r.L(5)),
                           r.len(r.in(IntrinsicId::ObjectKeys, {r.L(1)}))),
                     r.ret(r.Bo(false))),
               r.set(4, r.I(0)),
               r.b.make_while(
                   r.bin(BinOp::Lt, r.L(4), r.len(r.L(5))),
                   r.blk({r.set(6, r.idx(r.L(5), r.L(4))),
                          r.iff(r.bin(BinOp::Eq,
                                      r.in(IntrinsicId::ObjectHas,
                                           {r.L(1), r.L(6)}),
                                      r.Bo(false)),
                                r.ret(r.Bo(false))),
                          r.iff(r.bin(BinOp::Eq,
                                      r.call("$eq", {r.idx(r.L(0), r.L(6)),
                                                     r.idx(r.L(1), r.L(6))}),
                                      r.Bo(false)),
                                r.ret(r.Bo(false))),
                          r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1)))}),
                   r.p),
               r.ret(r.Bo(true))}));
    r.add(r.b.make_switch(r.L(2), arms,
                          r.ret(r.in(IntrinsicId::Same, {r.L(0), r.L(1)})),
                          r.p));
    r.finish("$eq", 2, 7, {"a", "b", "ta", "tb", "i", "ks", "k"});
  }

  // How culebra prints a Float. to_display is shortest-round-trip, and its
  // comment names this exact case: "4.0 is '4' -- whether a whole double
  // should show a decimal point is a language's decision, and a front end
  // that cares builds the string". This is that front end caring.
  void rt_fstr() {
    const double lim = 9007199254740992.0;  // 2^53
    RT r(*this);
    r.add(r.iff(r.bin(BinOp::Ne, r.L(0), r.L(0)),
                r.ret(r.in(IntrinsicId::ToStr, {r.L(0)}))));
    // -0.0 is integral and its integer form has lost the sign, so it is
    // the one value the general path below would print as "0.0".
    r.add(r.iff(r.in(IntrinsicId::Same, {r.L(0), r.D(-0.0)}), r.ret(r.S("-0.0"))));
    r.add(r.iff(
        r.bin(BinOp::Gt, r.L(0), r.D(-lim)),
        r.iff(r.bin(BinOp::Lt, r.L(0), r.D(lim)),
              r.blk({r.set(1, r.in(IntrinsicId::ToInt, {r.L(0)})),
                     r.iff(r.bin(BinOp::Eq,
                                 r.in(IntrinsicId::ToDouble, {r.L(1)}),
                                 r.L(0)),
                           r.ret(r.bin(BinOp::Add,
                                       r.in(IntrinsicId::ToStr, {r.L(1)}),
                                       r.S(".0"))))}))));
    // Everything else already carries a '.' or an exponent, and culebra's
    // own spelling of those is to_chars' -- 1e+21, 2.5e-07 -- so ToStr is
    // the answer verbatim.
    r.add(r.ret(r.in(IntrinsicId::ToStr, {r.L(0)})));
    r.finish("$fstr", 1, 2, {"d", "i"});
  }

  // Display conversion (culebra §8): what `"{x}"` and `println` produce. A
  // string is itself; a container is its inspect form, which quotes the
  // strings *inside* it.
  void rt_disp() {
    RT r(*this);
    std::vector<std::pair<NodeId, NodeId>> arms;
    arms.emplace_back(r.S("string"), r.ret(r.L(0)));
    arms.emplace_back(r.S("nil"), r.ret(r.S("nil")));
    arms.emplace_back(r.S("double"), r.ret(r.call("$fstr", {r.L(0)})));
    arms.emplace_back(r.S("array"), r.ret(r.call("$arrstr", {r.L(0)})));
    arms.emplace_back(r.S("object"), r.ret(r.call("$objstr", {r.L(0)})));
    arms.emplace_back(r.S("function"), r.ret(r.S("[function]")));
    arms.emplace_back(r.S("generator"), r.ret(r.S("<generator>")));
    r.add(r.b.make_switch(r.typ(r.L(0)), arms,
                          r.ret(r.in(IntrinsicId::ToStr, {r.L(0)})), r.p));
    r.finish("$disp", 1, 1, {"v"});
  }

  void rt_insp() {
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(0)), "string"),
                r.ret(r.bin(BinOp::Add,
                            r.bin(BinOp::Add, r.S("'"), r.L(0)), r.S("'")))));
    r.add(r.ret(r.call("$disp", {r.L(0)})));
    r.finish("$insp", 1, 1, {"v"});
  }

  void rt_arrstr() {
    RT r(*this);
    r.add(r.set(1, r.S("[")));
    r.add(r.set(2, r.I(0)));
    r.add(r.b.make_while(
        r.bin(BinOp::Lt, r.L(2), r.len(r.L(0))),
        r.blk({r.iff(r.bin(BinOp::Gt, r.L(2), r.I(0)),
                     r.set(1, r.bin(BinOp::Add, r.L(1), r.S(", ")))),
               r.set(1, r.bin(BinOp::Add, r.L(1),
                              r.call("$insp", {r.idx(r.L(0), r.L(2))}))),
               r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1)))}),
        r.p));
    r.add(r.ret(r.bin(BinOp::Add, r.L(1), r.S("]"))));
    r.finish("$arrstr", 1, 3, {"a", "out", "i"});
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
    r.add(r.set(1, r.S("{")));
    r.add(r.set(6, r.S("")));
    r.add(r.iff(r.in(IntrinsicId::ObjectHas, {r.L(0), r.S(kClassKey)}),
                r.blk({r.set(6, r.S("mut ")),
                       r.set(1, r.bin(BinOp::Add,
                                      r.idx(r.idx(r.L(0), kClassKey), kNameKey),
                                      r.S(" {")))})));
    r.add(r.set(2, r.I(0)));
    r.add(r.set(3, r.in(IntrinsicId::ObjectKeys, {r.L(0)})));
    r.add(r.set(5, r.I(0)));
    r.add(r.b.make_while(
        r.bin(BinOp::Lt, r.L(2), r.len(r.L(3))),
        r.blk({
            r.set(4, r.idx(r.L(3), r.L(2))),
            r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1))),
            r.iff(r.bin(BinOp::Lt,
                        r.in(IntrinsicId::StrByte, {r.L(4), r.I(0)}), r.I(32)),
                  r.b.make_continue(r.p)),
            r.iff(r.bin(BinOp::Gt, r.L(5), r.I(0)),
                  r.set(1, r.bin(BinOp::Add, r.L(1), r.S(", ")))),
            r.set(5, r.bin(BinOp::Add, r.L(5), r.I(1))),
            r.set(1, r.bin(BinOp::Add, r.L(1),
                           r.bin(BinOp::Add,
                                 r.bin(BinOp::Add,
                                       r.bin(BinOp::Add, r.L(6), r.L(4)),
                                       r.S(": ")),
                                 r.call("$insp", {r.idx(r.L(0), r.L(4))})))),
        }),
        r.p));
    r.add(r.ret(r.bin(BinOp::Add, r.L(1), r.S("}"))));
    r.finish("$objstr", 1, 7, {"o", "out", "i", "ks", "k", "n", "mut"});
  }

  // culebra's errors carry a kind, a message and a position; a `catch`
  // reads them as fields. The kinds this front end raises itself are the
  // container ones -- everything else is either a value the program threw
  // or a trap the executor raised, which TryCatch lands the same way.
  void rt_err() {
    RT r(*this);
    r.add(r.b.make_throw(
        r.obj({{"kind", r.L(0)}, {"message", r.L(1)}}), r.p));
    r.finish("$err", 2, 2, {"kind", "message"});
  }

  // `a[i]` and `o['k']`: strict, unlike `.` -- an absent key is a KeyError
  // and an out-of-range index an IndexError, both of which culebra raises
  // and this front end therefore raises too, rather than letting Index's
  // own trap message stand in for one of them.
  void rt_idx() {
    RT r(*this);
    std::vector<std::pair<NodeId, NodeId>> arms;
    auto arr_checks = r.array_index_checks(r.L(0), r.L(1));
    arr_checks.push_back(r.ret(r.idx(r.L(0), r.L(1))));
    arms.emplace_back(r.S("array"), r.blk(arr_checks));
    arms.emplace_back(
        r.S("object"),
        r.blk({r.iff(r.isnt(r.typ(r.L(1)), "string"),
                     r.ret(r.call("$err", {r.S("TypeError"),
                                           r.S("object key must be a String")}))),
               r.iff(r.bin(BinOp::Eq,
                           r.in(IntrinsicId::ObjectHas, {r.L(0), r.L(1)}),
                           r.Bo(false)),
                     r.ret(r.call("$err", {r.S("KeyError"),
                                           r.S("key not present")}))),
               r.ret(r.idx(r.L(0), r.L(1)))}));
    r.add(r.b.make_switch(
        r.typ(r.L(0)), arms,
        r.ret(r.call("$err", {r.S("TypeError"), r.S("value is not indexable")})),
        r.p));
    r.finish("$idx", 2, 2, {"recv", "key"});
  }

  void rt_setidx() {
    RT r(*this);
    std::vector<std::pair<NodeId, NodeId>> arms;
    auto arr_checks = r.array_index_checks(r.L(0), r.L(1));
    arr_checks.push_back(r.b.set_index(r.L(0), r.L(1), r.L(2), r.p));
    arr_checks.push_back(r.ret(r.L(2)));
    arms.emplace_back(r.S("array"), r.blk(arr_checks));
    arms.emplace_back(
        r.S("object"),
        r.blk({r.b.set_index(r.L(0), r.L(1), r.L(2), r.p), r.ret(r.L(2))}));
    r.add(r.b.make_switch(
        r.typ(r.L(0)), arms,
        r.ret(r.call("$err", {r.S("TypeError"), r.S("value is not indexable")})),
        r.p));
    r.finish("$setidx", 3, 3, {"recv", "key", "val"});
  }

  // `o.name`: lenient, culebra's own split from `o['name']` -- reading a
  // field that is not there answers nil rather than raising.
  void rt_mem() {
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(0)), "object"), r.ret(r.idx(r.L(0), r.L(1)))));
    r.add(r.ret(r.call("$err", {r.S("TypeError"),
                                r.S("value has no fields")})));
    r.finish("$mem", 2, 2, {"recv", "name"});
  }

  void rt_setmem() {
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(0)), "object"),
                r.blk({r.b.set_index(r.L(0), r.L(1), r.L(2), r.p),
                       r.ret(r.L(2))})));
    r.add(r.ret(r.call("$err", {r.S("TypeError"),
                                r.S("value has no fields")})));
    r.finish("$setmem", 3, 3, {"recv", "name", "val"});
  }

  // Method lookup: the object's own properties first (an object literal
  // holding a function is how culebra writes a protocol), then the class
  // table the constructor hung on the instance.
  void rt_methodof() {
    RT r(*this);
    r.add(r.iff(
        r.is(r.typ(r.L(0)), "object"),
        r.blk({r.iff(r.in(IntrinsicId::ObjectHas, {r.L(0), r.L(1)}),
                     r.ret(r.idx(r.L(0), r.L(1)))),
               r.iff(r.in(IntrinsicId::ObjectHas, {r.L(0), r.S(kClassKey)}),
                     r.blk({r.set(2, r.idx(r.L(0), kClassKey)),
                            r.set(3, r.bin(BinOp::Add, r.S(kMethodPrefix),
                                           r.L(1))),
                            r.iff(r.in(IntrinsicId::ObjectHas,
                                       {r.L(2), r.L(3)}),
                                  r.ret(r.idx(r.L(2), r.L(3))))}))})));
    r.add(r.ret(r.call("$err", {r.S("NameError"),
                                r.bin(BinOp::Add, r.S("no method "), r.L(1))})));
    r.finish("$methodof", 2, 4, {"recv", "name", "cls", "key"});
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
    r.add(r.set(1, r.typ(r.L(0))));
    r.add(r.iff(r.is(r.L(1), "generator"),
                r.ret(r.obj({{"k", r.S("g")}, {"v", r.L(0)}}))));
    r.add(r.iff(r.is(r.L(1), "array"),
                r.ret(r.obj({{"k", r.S("a")},
                             {"v", r.L(0)},
                             {"i", r.I(0)}}))));
    r.add(r.iff(
        r.is(r.L(1), "object"),
        r.iff(r.in(IntrinsicId::ObjectHas, {r.L(0), r.S(kClassKey)}),
              r.blk({r.set(2, r.idx(r.L(0), kClassKey)),
                     r.set(3, r.bin(BinOp::Add, r.S(kMethodPrefix), r.S("iter"))),
                     r.iff(r.in(IntrinsicId::ObjectHas, {r.L(2), r.L(3)}),
                           r.ret(r.obj({{"k", r.S("c")},
                                        {"v", r.b.call_value(
                                                 r.idx(r.L(2), r.L(3)),
                                                 {r.L(0)}, r.p)}})))}))));
    r.add(r.ret(r.call("$err", {r.S("TypeError"),
                                r.S("value is not iterable")})));
    r.finish("$iter", 1, 4, {"v", "t", "cls", "key"});
  }

  void rt_iternext() {
    RT r(*this);
    r.add(r.iff(r.is(r.idx(r.L(0), "k"), "g"),
                r.ret(r.in(IntrinsicId::GenResume,
                           {r.idx(r.L(0), "v"), r.Nil()}))));
    r.add(r.iff(
        r.is(r.idx(r.L(0), "k"), "c"),
        r.blk({r.set(1, r.idx(r.L(0), "v")),
               r.set(4,
                     r.b.call_value(
                         r.call("$methodof", {r.L(1), r.S("has_next")}),
                         {r.L(1)}, r.p)),
               r.iff(r.bin(BinOp::Eq, r.L(4), r.Bo(false)),
                     r.ret(r.obj({{"value", r.Nil()}, {"done", r.Bo(true)}}))),
               r.ret(r.obj(
                   {{"value",
                     r.b.call_value(
                         r.call("$methodof", {r.L(1), r.S("next")}),
                         {r.L(1)}, r.p)},
                    {"done", r.Bo(false)}}))})));
    r.add(r.set(1, r.idx(r.L(0), "v")));
    r.add(r.set(2, r.idx(r.L(0), "i")));
    r.add(r.iff(r.bin(BinOp::Ge, r.L(2), r.len(r.L(1))),
                r.ret(r.obj({{"value", r.Nil()}, {"done", r.Bo(true)}}))));
    r.add(r.b.set_index(r.L(0), r.S("i"), r.bin(BinOp::Add, r.L(2), r.I(1)),
                        r.p));
    r.add(r.ret(r.obj({{"value", r.idx(r.L(1), r.L(2))},
                       {"done", r.Bo(false)}})));
    r.finish("$iternext", 1, 5, {"it", "a", "i", "obj", "hn"});
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

  // ==== Pass B: emit ======================================================

  NodeId helper(FnCtx& ctx, const std::string& name,
                const std::vector<NodeId>& args, SrcPos p) {
    Builder b(m);
    auto it = ctx.helper_cells.find(name);
    const int32_t c = it != ctx.helper_cells.end()
                          ? it->second
                          : (ctx.helper_cells[name] = ctx.next_cell++);
    return b.call_value(b.varref(VarKind::Cell, c, p), args, p);
  }

  NodeId native(const std::string& name, const std::vector<NodeId>& args,
                SrcPos p) {
    Builder b(m);
    return b.call_value(b.native_ref(b.declare_native(name), p), args, p);
  }

  NodeId emit_closure(int32_t g, FnCtx& ctx, SrcPos p) {
    Builder b(m);
    std::vector<CaptureSrc> cs;
    cs.reserve(fns[static_cast<size_t>(g)].free.size());
    for (const int32_t v : fns[static_cast<size_t>(g)].free) {
      const auto [k, i] = access(ctx.fn, v);
      cs.push_back({k, i});
    }
    const int32_t cm = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back(cs);
    return b.make_closure(fns[static_cast<size_t>(g)].index, cm, p);
  }

  NodeId bind_decl(int32_t v, NodeId value, FnCtx& ctx, SrcPos p,
                   bool fresh = true) {
    Builder b(m);
    const auto& ci = fns[static_cast<size_t>(ctx.fn)].cell_index;
    const auto it = ci.find(v);
    if (it != ci.end()) {
      if (!fresh) return b.assign(VarKind::Cell, it->second, value, p);
      return b.block({b.cell_fresh(it->second, p),
                      b.assign(VarKind::Cell, it->second, value, p)},
                     p);
    }
    const int32_t s = ctx.alloc_local(vars[static_cast<size_t>(v)].name);
    slot_of[static_cast<size_t>(v)] = s;
    return b.assign(VarKind::Local, s, value, p);
  }

  NodeId read_var(int32_t v, FnCtx& ctx, SrcPos p) {
    Builder b(m);
    const auto [k, i] = access(ctx.fn, v);
    return b.varref(k, i, p);
  }

  NodeId write_var(int32_t v, NodeId value, FnCtx& ctx, SrcPos p) {
    Builder b(m);
    const auto [k, i] = access(ctx.fn, v);
    return b.assign(k, i, value, p);
  }

  // The release list culebra's rule asks for: the block's own bindings, in
  // reverse declaration order, cells included. Without it a Scope releases
  // its local range last-slot-first and leaves the cells to the frame --
  // which would drop a captured resource at the wrong time, or not at all
  // until the function returned. samples/drops.cul is the difference.
  std::vector<NodeId> release_list(const std::vector<int32_t>& decls,
                                   FnCtx& ctx, SrcPos p) {
    Builder b(m);
    std::vector<NodeId> out;
    for (size_t i = decls.size(); i-- > 0;) {
      const auto [k, idx] = access(ctx.fn, decls[i]);
      out.push_back(b.varref(k, idx, p));
    }
    return out;
  }

  NodeId emit_block(const Ast& block, FnCtx& ctx, bool entry = false) {
    Builder b(m);
    const SrcPos p = pos_of(block);
    const int32_t mark = ctx.next_local;
    const auto& decls = block_decls.at(&block);

    std::vector<NodeId> out;
    // Bindings are created on entry and only initialized where they stand
    // -- see examples/mini-js/README.md for the bug that rule prevents.
    const auto& cells = fns[static_cast<size_t>(ctx.fn)].cell_index;
    for (const int32_t v : decls) {
      const auto c = cells.find(v);
      if (c != cells.end()) out.push_back(b.cell_fresh(c->second, p));
    }
    for (const auto& s : block.nodes) out.push_back(emit_stmt(*s, ctx));

    const int32_t end = ctx.next_local;
    ctx.next_local = mark;
    // funcs[0] runs under entry_frame_drops = false, and culebra's rule for
    // a top-level binding is that it lives to the end of the program and is
    // released without its destructor. So the program's own statements get
    // no releasing Scope at all -- emit_fn wraps them in a [0, 0) one,
    // which is what still lets a top-level `defer` run.
    if (entry) return b.block(out, p);
    // A Scope even when the block declares nothing: Tag::Defer runs at the
    // exit of the *enclosing* Scope, so a block whose only statement is a
    // `defer` would otherwise hand it to the function's scope and run it at
    // the wrong time -- once, at the end, instead of once per block entry.
    // errors.cul's `while` loop is the case that catches it: three
    // iterations, three defers, and without the scope all three fire after
    // the loop with the loop variable already at its final value.
    if (decls.empty()) return b.scope(mark, end, b.block(out, p), p);
    return b.scope(mark, end, b.block(out, p), release_list(decls, ctx, p), p);
  }

  std::vector<NodeId> emit_args(const Ast& args, FnCtx& ctx) {
    std::vector<NodeId> out;
    out.reserve(args.nodes.size());
    for (const auto& c : args.nodes) out.push_back(emit_expr(*c, ctx));
    return out;
  }

  // -- Statements ---------------------------------------------------------
  NodeId emit_stmt(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
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
        return b.block({bind_decl(v, emit_expr(*a.nodes[2], ctx), ctx, p, false),
                        read_var(v, ctx, p)},
                       p);
      }
      case "block"_:
        return emit_block(a, ctx);
      case "deferstmt"_:
        return b.make_defer(emit_closure(fn_of.at(&a), ctx, p), p);
      case "returnstmt"_:
        return b.make_return(
            a.nodes.empty() ? b.nil_literal(p) : emit_expr(*a.nodes[0], ctx),
            p);
      case "throwstmt"_:
        return b.make_throw(emit_expr(*a.nodes[0], ctx), p);
      case "yieldstmt"_:
        return b.make_yield(emit_expr(*a.nodes[0], ctx), p);
      case "yieldfrom"_:
        return emit_yield_from(a, ctx);
      case "breakstmt"_:
        return b.make_break(p);
      case "contstmt"_:
        return b.make_continue(p);
      case "whilestmt"_:
        return b.make_while(emit_expr(*a.nodes[0], ctx),
                            emit_block(*a.nodes[1], ctx), p);
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
    Builder b(m);
    const SrcPos p = pos_of(a);
    const int32_t mark = ctx.next_local;
    const int32_t it = ctx.alloc_local("$it");
    const int32_t st = ctx.alloc_local("$step");
    const NodeId I = b.varref(VarKind::Local, it, p);
    const NodeId S = b.varref(VarKind::Local, st, p);
    std::vector<NodeId> loop{
        b.assign(VarKind::Local, st, helper(ctx, "$iternext", {I}, p), p),
        b.make_if(b.index(S, b.str_literal("done", p), p), b.make_break(p),
                  NodeId{}, p),
        b.make_yield(b.index(S, b.str_literal("value", p), p), p)};
    const NodeId body = b.block(
        {b.assign(VarKind::Local, it,
                  helper(ctx, "$iter", {emit_expr(*a.nodes[0], ctx)}, p), p),
         b.make_while(b.bool_literal(true, p), b.block(loop, p), p)},
        p);
    const int32_t end = ctx.next_local;
    ctx.next_local = mark;
    return b.scope(mark, end, body, p);
  }

  // for x in e { ... }
  //
  // Two nested scopes rather than one, because culebra releases in a
  // documented order -- "body locals, element, dispose, iterator,
  // iterable" -- and nesting is how that order is stated: the body block's
  // own scope goes first, the element's scope around it next, and the
  // iterator's outermost.
  NodeId emit_for(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    const int32_t mark = ctx.next_local;
    const int32_t it = ctx.alloc_local("$it");
    const int32_t st = ctx.alloc_local("$step");
    const NodeId I = b.varref(VarKind::Local, it, p);
    const NodeId S = b.varref(VarKind::Local, st, p);

    const int32_t v = decl_of.at(a.nodes[0].get());
    const int32_t emark = ctx.next_local;
    const auto& cells = fns[static_cast<size_t>(ctx.fn)].cell_index;
    std::vector<NodeId> elem;
    const auto c = cells.find(v);
    if (c != cells.end()) elem.push_back(b.cell_fresh(c->second, p));
    elem.push_back(bind_decl(
        v, b.index(S, b.str_literal("value", p), p), ctx, p, false));
    elem.push_back(emit_block(*a.nodes[2], ctx));
    const int32_t eend = ctx.next_local;
    ctx.next_local = emark;
    const NodeId elem_scope =
        eend > emark
            ? b.scope(emark, eend, b.block(elem, p),
                      release_list({v}, ctx, p), p)
            : b.scope(emark, emark, b.block(elem, p),
                      release_list({v}, ctx, p), p);

    std::vector<NodeId> loop{
        b.assign(VarKind::Local, st, helper(ctx, "$iternext", {I}, p), p),
        b.make_if(b.index(S, b.str_literal("done", p), p), b.make_break(p),
                  NodeId{}, p),
        elem_scope};
    const NodeId body = b.block(
        {b.assign(VarKind::Local, it,
                  helper(ctx, "$iter", {emit_expr(*a.nodes[1], ctx)}, p), p),
         b.make_while(b.bool_literal(true, p), b.block(loop, p), p)},
        p);
    const int32_t end = ctx.next_local;
    ctx.next_local = mark;
    return b.scope(mark, end, body, p);
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
    Builder b(m);
    const SrcPos p = pos_of(a);
    const auto& methods = class_of.at(&a);
    const int32_t v = decl_of.at(a.nodes[0].get());
    const int32_t cell = fns[static_cast<size_t>(ctx.fn)].cell_index.at(v);

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
      kvs.emplace_back(b.str_literal(key, p), emit_closure(g, ctx, p));
    }
    kvs.emplace_back(b.str_literal(kNameKey, p),
                     b.str_literal(std::string(a.nodes[0]->token), p));
    emit_ctor(ctor_fn, init_params, has_drop, std::string(a.nodes[0]->token));

    std::vector<CaptureSrc> cs{{VarKind::Cell, cell}};
    const int32_t cm = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back(cs);
    return b.block(
        {b.assign(VarKind::Cell, cell, b.object_lit(kvs, p), p),
         b.set_index(b.varref(VarKind::Cell, cell, p), b.str_literal("new", p),
                     b.make_closure(fns[static_cast<size_t>(ctor_fn)].index,
                                    cm, p),
                     p)},
        p);
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
    m.funcs[static_cast<size_t>(fns[static_cast<size_t>(g)].index)] =
        std::move(f);
  }

  // -- Expressions --------------------------------------------------------
  NodeId emit_expr(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    switch (a.tag) {
      case "number"_:
        return b.literal(std::strtoll(std::string(a.token).c_str(), nullptr, 10),
                         p);
      case "float"_:
        return b.double_literal(
            std::strtod(std::string(a.token).c_str(), nullptr), p);
      case "string"_:
        return b.str_literal(unescape(std::string(a.token)), p);
      case "istring"_: {
        // "a{x}b" is concatenation, with every hole passed through the
        // display conversion -- culebra's §8, and the one place a Float
        // has to come out as "4.0".
        NodeId acc = b.str_literal("", p);
        for (const auto& part : a.nodes) {
          const NodeId piece =
              part->tag == "itext"_
                  ? b.str_literal(unescape(std::string(part->token)), p)
                  : helper(ctx, "$disp", {emit_expr(*part->nodes[0], ctx)}, p);
          acc = b.binary(BinOp::Add, acc, piece, p);
        }
        return acc;
      }
      case "literal"_: {
        const std::string t(a.token);
        if (t == "true") return b.bool_literal(true, p);
        if (t == "false") return b.bool_literal(false, p);
        return b.nil_literal(p);
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
                        b.bool_literal(false, p), p);
      case "negexpr"_:
        return b.unary(UnOp::Neg, emit_expr(*a.nodes[0], ctx), p);
      case "ifexpr"_: {
        const NodeId c = emit_expr(*a.nodes[0], ctx);
        const NodeId t = emit_block(*a.nodes[1], ctx);
        NodeId e;
        if (a.nodes.size() > 2) {
          e = a.nodes[2]->tag == "block"_ ? emit_block(*a.nodes[2], ctx)
                                          : emit_expr(*a.nodes[2], ctx);
        }
        return b.make_if(c, t, e, p);
      }
      case "tryexpr"_: {
        const int32_t mark = ctx.next_local;
        const int32_t exc = ctx.alloc_local("$exc");
        const NodeId body = emit_block(*a.nodes[0], ctx);
        const int32_t v = decl_of.at(a.nodes[1].get());
        const int32_t hmark = ctx.next_local;
        const auto& cells = fns[static_cast<size_t>(ctx.fn)].cell_index;
        std::vector<NodeId> hs;
        const auto c = cells.find(v);
        if (c != cells.end()) hs.push_back(b.cell_fresh(c->second, p));
        hs.push_back(bind_decl(v, b.varref(VarKind::Local, exc, p), ctx, p,
                               false));
        hs.push_back(emit_block(*a.nodes[2], ctx));
        const int32_t hend = ctx.next_local;
        ctx.next_local = hmark;
        const NodeId handler =
            b.scope(hmark, hend > hmark ? hend : hmark, b.block(hs, p),
                    release_list({v}, ctx, p), p);
        const NodeId out = b.make_try(exc, body, handler, p);
        const int32_t end = ctx.next_local;
        ctx.next_local = mark;
        return b.scope(mark, end, out, p);
      }
      case "logor"_:
      case "logand"_: {
        const bool is_or = a.tag == "logor"_;
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i < a.nodes.size(); ++i) {
          const int32_t t = ctx.alloc_local(is_or ? "$or" : "$and");
          const NodeId rhs = emit_expr(*a.nodes[i], ctx);
          const NodeId keep = b.varref(VarKind::Local, t, p);
          acc = b.block({b.assign(VarKind::Local, t, acc, p),
                         b.make_if(keep, is_or ? keep : rhs,
                                   is_or ? rhs : keep, p)},
                        p);
        }
        return acc;
      }
      case "equality"_: {
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i + 1 < a.nodes.size(); i += 2) {
          const Ast& op = *a.nodes[i];
          const NodeId rhs = emit_expr(*a.nodes[i + 1], ctx);
          const NodeId eq = helper(ctx, "$eq", {acc, rhs}, pos_of(op));
          acc = op.token == "=="
                    ? eq
                    : b.binary(BinOp::Eq, eq, b.bool_literal(false, p),
                               pos_of(op));
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
          acc = b.binary(o, acc, emit_expr(*a.nodes[i + 1], ctx), pos_of(op));
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
          acc = b.binary(o, acc, emit_expr(*a.nodes[i + 1], ctx), pos_of(op));
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
        return b.array_lit(items, p);
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
          kvs.emplace_back(b.str_literal(k, p), emit_expr(*c->nodes[1], ctx));
        }
        return b.object_lit(kvs, p);
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
                                   : helper(ctx, "$disp", {args[0]}, gp)},
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
                helper(ctx, "$methodof", {cur, b.str_literal(name, p)}, p);
            args.insert(args.begin(), cur);
            cur = b.call_value(f, args, p);
            break;
          }
          cur = helper(ctx, "$mem", {cur, b.str_literal(name, p)}, p);
          break;
        }
        case "indexsfx"_:
          cur = helper(ctx, "$idx", {cur, emit_expr(*sfx.nodes[0], ctx)}, p);
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
    Builder b(m);
    const SrcPos p = pos_of(a);
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
      return b.binary(o, cur, v, p);
    };

    if (target.tag == "ident"_) {
      const auto it = ref_of.find(&target);
      if (it == ref_of.end()) fail(target, "cannot assign to this");
      const int32_t v = it->second;
      if (!vars[static_cast<size_t>(v)].is_mut) {
        fail(target, "cannot assign to '" + vars[static_cast<size_t>(v)].name +
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
    const NodeId key = member
                           ? b.str_literal(std::string(last.nodes[0]->token), p)
                           : emit_expr(*last.nodes[0], ctx);
    const int32_t tr = ctx.alloc_local("$recv");
    const int32_t tk = ctx.alloc_local("$key");
    const NodeId recv = emit_postfix(target, limit - 1, ctx);
    const NodeId R = b.varref(VarKind::Local, tr, p);
    const NodeId K = b.varref(VarKind::Local, tk, p);
    const NodeId cur = op == "="
                           ? b.nil_literal(p)
                           : helper(ctx, member ? "$mem" : "$idx", {R, K}, p);
    return b.block({b.assign(VarKind::Local, tr, recv, p),
                    b.assign(VarKind::Local, tk, key, p),
                    helper(ctx, member ? "$setmem" : "$setidx",
                           {R, K, combine(cur)}, p)},
                   p);
  }

  // -- One function's body -------------------------------------------------
  void emit_fn(int32_t f) {
    const FnInfo& fi = fns[static_cast<size_t>(f)];
    if (fi.is_ctor) return;  // emit_class built it by hand
    FnCtx ctx;
    ctx.fn = f;
    ctx.next_cell = static_cast<int32_t>(fi.cell_index.size());
    Builder b(m);
    const SrcPos p = fi.body != nullptr ? pos_of(*fi.body) : SrcPos{0, 0};

    std::vector<NodeId> pre;
    for (const int32_t v : fi.params) {
      const int32_t s = ctx.alloc_local(vars[static_cast<size_t>(v)].name);
      slot_of[static_cast<size_t>(v)] = s;
      const auto it = fi.cell_index.find(v);
      if (it != fi.cell_index.end()) {
        pre.push_back(b.cell_fresh(it->second, p));
        pre.push_back(b.assign(VarKind::Cell, it->second,
                               b.varref(VarKind::Local, s, p), p));
      }
    }
    const int32_t nparams = ctx.next_local;
    const bool entry = f == 0;

    NodeId body;
    if (fi.body->tag == "block"_ || fi.body->tag == "program"_) {
      body = emit_block(*fi.body, ctx, entry);
    } else {
      body = emit_expr(*fi.body, ctx);  // a lambda's expression body
    }

    std::vector<NodeId> stmts;
    for (const auto& [name, cell] : ctx.helper_cells) {
      stmts.push_back(b.assign(VarKind::Cell, cell,
                               b.make_closure(rt.at(name), empty_cmap, p), p));
    }
    stmts.insert(stmts.end(), pre.begin(), pre.end());
    stmts.push_back(body);

    Func fn;
    fn.name = fi.name;
    fn.num_params = nparams;
    fn.num_locals = ctx.high_local;
    ctx.local_names.resize(static_cast<size_t>(ctx.high_local), "");
    fn.local_names = ctx.local_names;
    fn.num_cells = ctx.next_cell;
    fn.lenient_arity = true;
    fn.is_generator = fi.is_generator;
    fn.num_captures = m.funcs[static_cast<size_t>(fi.index)].num_captures;
    fn.capture_names = m.funcs[static_cast<size_t>(fi.index)].capture_names;
    // A culebra function answers its body's last value, so the whole thing
    // is the operand of one Return. The entry point's Scope is [0, 0):
    // with entry_frame_drops = false that is what still runs a top-level
    // `defer` while leaving the top-level bindings to be released without
    // their destructors -- culebra's own rule, stated in RunOptions.
    fn.body = b.make_return(
        b.scope(0, entry ? 0 : nparams, b.block(stmts, p), p), p);
    m.funcs[static_cast<size_t>(fi.index)] = std::move(fn);
  }

  Module build(const Ast& program) {
    const int32_t top = new_fn(-1, "main");
    fns[static_cast<size_t>(top)].body = &program;

    scopes.push_back({top, {}, {}});
    for (const auto& s : program.nodes) resolve_stmt(*s, top);
    block_decls[&program] = scopes.back().order;
    scopes.pop_back();

    m.funcs.push_back({});
    fns[static_cast<size_t>(top)].index = 0;
    for (const std::string& n : rt_names()) {
      rt[n] = static_cast<int32_t>(m.funcs.size());
      m.funcs.push_back({});
    }
    for (size_t f = 1; f < fns.size(); ++f) {
      fns[f].index = static_cast<int32_t>(m.funcs.size());
      m.funcs.push_back({});
    }

    number_captures();
    empty_cmap = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back({});
    emit_runtime();

    slot_of.assign(vars.size(), -1);
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
