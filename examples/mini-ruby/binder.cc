// The front end for the two intrinsics nothing else here uses -- and,
// since classes joined it, for a second recipe: a class is an object
// holding its base's table and its methods, and an instance is a plain
// object pointing back at it, the same shape examples/mini-python and
// examples/mini-csharp reach for theirs.
//
// **FnArity.** `vmlib.h` introduces it as "The one fact a front end needs
// to check 'this callback takes two arguments' before calling it". Ruby
// makes that fact a *program-visible* one: `Proc#arity` is FnArity, minus
// the one implicit parameter this front end adds (see below).
//
// **ArgCount.** "How many arguments the running function was called with
// -- the count the caller supplied, not num_params. Only interesting under
// Func::lenient_arity, where the two can differ." Ruby's own distinction
// between a proc (extra arguments dropped, missing ones nil) and a lambda
// (a mismatch raises ArgumentError) is exactly lenient_arity plus an
// ArgCount check -- and a default parameter's value is the same test,
// once more, per parameter.
//
// And **`ensure` is a Defer** -- the third front end to reach that tag from
// a third direction (culebra's `defer`, Python's `with`, and now this).
//
// One structural convention runs through the whole file: **every function's
// parameter 0 is the block it was passed**, nil when there was none. Ruby
// passes blocks out of band, and this is the cheapest way to model that
// over a calling convention that has no out-of-band channel:
//
//   * `yield a` is a call of parameter 0;
//   * `block_given?` is parameter 0 not being nil;
//   * `&blk` names parameter 0 rather than declaring a new one;
//   * `Proc#arity` is FnArity minus one;
//   * and a lambda's ArgCount check compares against num_params, both of
//     which count it, so the one cancels the other.
//
// An instance method adds a second implicit parameter, `self`, right
// after the block -- so a method's own ArgCount/arity story is one more
// than a plain method's, which is why `attr_accessor` and `initialize`
// never ask either.

#include "binder.h"

#include <cctype>
#include <cstdlib>
#include <functional>
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

namespace mini_ruby {
namespace {

// An exception's kind and message, a Range's two ends, and a class's own
// bookkeeping, all under keys no Ruby identifier can be -- kMethodPrefix
// is the trick every class-bearing front end here uses for a method's own
// key, so `"\x02" + name` never collides with a field a program set.
constexpr char kExcKey[] = "\x01" "e";
constexpr char kMsgKey[] = "\x01" "m";
constexpr char kRangeKey[] = "\x01" "r";
constexpr char kClassKey[] = "\x01" "c";
constexpr char kBaseKey[] = "\x01" "p";
constexpr char kNameKey[] = "\x01" "n";
constexpr char kMethodPrefix = '\x02';

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
      case '\\': out.push_back('\\'); break;
      case '\'': out.push_back('\''); break;
      case '"': out.push_back('"'); break;
      case '#': out.push_back('#'); break;
      default: out.push_back(tok[i]); break;
    }
  }
  return out;
}

bool is_global(const std::string& n) {
  return n == "lambda" || n == "proc" || n == "block_given?" ||
         n == "Proc" || n == "Integer" || n == "String" || n == "Array" ||
         n == "Hash";
}

struct VarInfo {
  std::string name;
  int32_t owner = 0;
};

// A parameter with a default: the value is computed in the *method's own*
// scope, after its earlier parameters, so it may read them -- unlike
// examples/mini-python's, which closes over the enclosing scope once at
// `def` time. `defaults[i]` is null unless `params[i]` has one.
struct FnInfo {
  int32_t parent = -1;
  int32_t index = -1;
  bool strict = false;  // a lambda: an ArgCount mismatch raises
  std::string name = "?";
  std::set<int32_t> free;
  std::map<int32_t, int32_t> capture_index;
  std::map<int32_t, int32_t> cell_index;
  std::vector<int32_t> params;  // params[0] is always the block
  std::vector<const Ast*> defaults;
  const Ast* body = nullptr;
  // Set on an instance method: params[1] is `self`, `cls_base` is the
  // enclosing class's own base-table binding (what `super` looks up on),
  // and `method_name` is what it looks for there.
  bool is_method = false;
  int32_t cls_base = -1;
  std::string method_name;
};

// A class declaration binds a name to a table object (its own methods,
// its base's table, and its name); `base_var` is that same binding on the
// base, resolved once and reused everywhere a subclass needs it.
struct ClassInfo {
  int32_t base_var = -1;
  std::string name;
};

struct FnCtx {
  int32_t fn = 0;
  int32_t next_local = 0;
  int32_t next_cell = 0;
  std::vector<std::string> local_names;

  int32_t alloc_local(const std::string& name) {
    const int32_t s = next_local++;
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
  std::vector<int32_t> slot_of;
  std::map<std::string, int32_t> rt;
  int32_t empty_cmap = -1;
  // One closure per runtime helper, built once at file scope into an array
  // every function captures -- see build().
  int32_t helpers_var = -1;

  // ==== Pass A: scopes, declarations, captures =============================
  //
  // A Ruby local is created by assignment, and its scope is the method (or
  // block) it appears in -- so, like Python's, the names a body assigns are
  // gathered before it is walked. A block, unlike a method, *shares* its
  // enclosing scope: a name the block assigns that already exists outside
  // is the outer one, which is why `resolve_block` does not push a fresh
  // function scope for its own assignments the way `resolve_def` does.

  struct ScopeA {
    int32_t fn;
    std::map<std::string, int32_t> names;
    bool porous = false;  // a block: assignments may reach outward
  };
  std::vector<ScopeA> scopes;

  int32_t declare(const std::string& name, int32_t fn) {
    // A block's assignment lands outside if the name is already bound
    // there -- Ruby's rule, and the reason a counter incremented inside
    // `each` is visible after it.
    for (size_t i = scopes.size(); i-- > 0;) {
      auto it = scopes[i].names.find(name);
      if (it != scopes[i].names.end()) return it->second;
      if (!scopes[i].porous) break;
    }
    const int32_t v = static_cast<int32_t>(vars.size());
    vars.push_back({name, fn});
    scopes.back().names[name] = v;
    return v;
  }

  // A *parameter* is always local to its own function, porous scope or
  // not: Ruby's block leaks its assignments outward but its `|x|` still
  // shadows an outer x, and the implicit `__block` must never resolve to
  // the enclosing function's. Going through `declare` for these was this
  // binder's first bug -- every block ended up sharing its caller's block
  // parameter, and the IR came out with more parameters than slots.
  int32_t declare_param(const std::string& name, int32_t fn) {
    const int32_t v = static_cast<int32_t>(vars.size());
    vars.push_back({name, fn});
    scopes.back().names[name] = v;
    return v;
  }

  // Reading a binding from `fn`: if it belongs to an enclosing function it
  // becomes a cell there and a capture in every function on the way down.
  void use_var(int32_t v, int32_t fn) {
    const int32_t owner = vars[static_cast<size_t>(v)].owner;
    if (owner == fn) return;
    for (int32_t k = fn; k != owner && k >= 0;
         k = fns[static_cast<size_t>(k)].parent) {
      fns[static_cast<size_t>(k)].free.insert(v);
    }
  }

  std::optional<int32_t> resolve(const std::string& name, int32_t fn) {
    for (size_t i = scopes.size(); i-- > 0;) {
      auto it = scopes[i].names.find(name);
      if (it == scopes[i].names.end()) continue;
      use_var(it->second, fn);
      return it->second;
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

  // A `def` binds a name too: at the top level, with no classes around it,
  // a method is a closure in an ordinary variable, which is what lets one
  // method call another -- the callee is a *capture* of the caller. Inside
  // a class body, `resolve_classdef` handles a `def` itself instead of
  // going through this generic statement path.
  std::set<int32_t> method_vars;

  // Pass A's per-node bookkeeping for classes: which classdef a name binds
  // to, so a subclass or a `ClassName.new` can find its table.
  std::map<const Ast*, ClassInfo> class_info;
  std::map<int32_t, const Ast*> class_by_var;
  std::map<const Ast*, std::vector<std::pair<std::string, int32_t>>> class_of;

  void collect(const Ast& a, std::vector<const Ast*>& out) {
    if (a.tag == "defstmt"_ || a.tag == "classdef"_) {
      out.push_back(a.nodes[0].get());
      return;  // its own body binds its own names
    }
    if (a.tag == "assign"_ && a.nodes[0]->tag == "ident"_) {
      out.push_back(a.nodes[0].get());
    }
    for (const auto& c : a.nodes) collect(*c, out);
  }

  void bind_names(const Ast& body, int32_t fn) {
    std::vector<const Ast*> names;
    for (const auto& s : body.nodes) collect(*s, names);
    for (const Ast* id : names) {
      decl_of[id] = declare(std::string(id->token), fn);
    }
  }

  // A method: its own scope, and its own function. Parameter 0 is the
  // block, always; an instance method also gets `self` at parameter 1.
  int32_t resolve_def(const Ast& node, const Ast& params, const Ast& body,
                      int32_t parent, const std::string& name,
                      bool is_method = false, int32_t cls_base = -1) {
    const int32_t f = new_fn(parent, name);
    FnInfo& fi = fns[static_cast<size_t>(f)];
    fi.body = &body;
    fn_of[&node] = f;
    scopes.push_back({f, {}, false});
    const int32_t blk = declare_param("__block", f);
    fi.params.push_back(blk);
    fi.defaults.push_back(nullptr);
    if (is_method) {
      fi.is_method = true;
      fi.cls_base = cls_base;
      fi.method_name = name;
      if (cls_base >= 0) use_var(cls_base, f);
      const int32_t selfv = declare_param("self", f);
      fi.params.push_back(selfv);
      fi.defaults.push_back(nullptr);
    }
    for (const auto& p : params.nodes) {
      if (p->tag == "blockparam"_) {
        // `&blk` names the block this method was passed, which is already
        // parameter 0 -- so it is an alias, not a new parameter.
        scopes.back().names[std::string(p->nodes[0]->token)] = blk;
        decl_of[p->nodes[0].get()] = blk;
        continue;
      }
      const bool has_default = p->tag == "defparam"_;
      const Ast& id = has_default ? *p->nodes[0] : *p;
      const int32_t v = declare_param(std::string(id.token), f);
      decl_of[&id] = v;
      fi.params.push_back(v);
      if (has_default) {
        resolve_expr(*p->nodes[1], f);
        fi.defaults.push_back(p->nodes[1].get());
      } else {
        fi.defaults.push_back(nullptr);
      }
    }
    bind_names(body, f);
    for (const auto& s : body.nodes) resolve_stmt(*s, f);
    scopes.pop_back();
    return f;
  }

  // A block: a function of its own for closure purposes, but a scope that
  // leaks outward for assignment.
  int32_t resolve_block(const Ast& node, int32_t parent, bool strict) {
    const Ast& params = *node.nodes[0];
    const Ast& body = *node.nodes[1];
    const int32_t f = new_fn(parent, strict ? "lambda" : "block");
    fns[static_cast<size_t>(f)].body = &body;
    fns[static_cast<size_t>(f)].strict = strict;
    fn_of[&node] = f;
    scopes.push_back({f, {}, true});
    fns[static_cast<size_t>(f)].params.push_back(declare_param("__block", f));
    fns[static_cast<size_t>(f)].defaults.push_back(nullptr);
    for (const auto& p : params.nodes) {
      const int32_t v = declare_param(std::string(p->token), f);
      decl_of[p.get()] = v;
      fns[static_cast<size_t>(f)].params.push_back(v);
      fns[static_cast<size_t>(f)].defaults.push_back(nullptr);
    }
    for (const auto& s : body.nodes) resolve_stmt(*s, f);
    scopes.pop_back();
    return f;
  }

  // A class body holds `def`s and `attr_accessor`/`attr_reader`/
  // `attr_writer`; each attr name becomes a getter and/or setter with no
  // source body of its own -- built directly in emit_classdef, the way
  // examples/mini-python's constructor and examples/mini-csharp's dispose
  // thunk are.
  void resolve_classdef(const Ast& a, int32_t fn) {
    const std::string cname(a.nodes[0]->token);
    ClassInfo ci;
    ci.name = cname;
    size_t bi = 1;
    if (a.nodes[1]->tag == "superclass"_) {
      const Ast& bid = *a.nodes[1]->nodes[0];
      const auto bv = resolve(std::string(bid.token), fn);
      if (!bv) fail(bid, "uninitialized constant " + std::string(bid.token));
      if (!class_by_var.count(*bv)) {
        fail(bid, "superclass must be a class declared in this program");
      }
      ref_of[&bid] = *bv;
      ci.base_var = *bv;
      bi = 2;
    }
    const int32_t declv = declare(cname, fn);
    decl_of[a.nodes[0].get()] = declv;
    class_by_var[declv] = &a;
    class_info[&a] = ci;

    std::vector<std::pair<std::string, int32_t>> methods;
    const Ast& body = *a.nodes[bi];
    for (const auto& c : body.nodes) {
      if (c->tag == "defstmt"_) {
        const std::string name(std::string(c->nodes[0]->token));
        methods.emplace_back(
            name, resolve_def(*c, *c->nodes[1], *c->nodes[2], fn, name, true,
                              ci.base_var));
        continue;
      }
      if (c->tag == "attrstmt"_) {
        const std::string kind(c->nodes[0]->token);
        for (size_t i = 1; i < c->nodes.size(); ++i) {
          const std::string aname(c->nodes[i]->token);
          if (kind != "attr_writer") {
            methods.emplace_back(aname, new_fn(fn, cname + "#" + aname));
          }
          if (kind != "attr_reader") {
            methods.emplace_back(aname + "=",
                                 new_fn(fn, cname + "#" + aname + "="));
          }
        }
        continue;
      }
      fail(*c, "only methods and attr_accessor are supported in a class "
               "body here");
    }
    class_of[&a] = methods;
  }

  void resolve_stmt(const Ast& a, int32_t fn) {
    switch (a.tag) {
      case "defstmt"_: {
        const int32_t v = declare(std::string(a.nodes[0]->token), fn);
        decl_of[a.nodes[0].get()] = v;
        method_vars.insert(v);
        resolve_def(a, *a.nodes[1], *a.nodes[2], fn,
                    std::string(a.nodes[0]->token));
        return;
      }
      case "classdef"_:
        resolve_classdef(a, fn);
        return;
      case "attrstmt"_:
        fail(a, "attr_accessor is only supported inside a class body");
      case "body"_:
        for (const auto& s : a.nodes) resolve_stmt(*s, fn);
        return;
      case "ifstmt"_:
      case "unlessstmt"_:
        for (const auto& c : a.nodes) {
          if (c->tag == "body"_) {
            resolve_stmt(*c, fn);
          } else if (c->tag == "elsifpart"_) {
            resolve_expr(*c->nodes[0], fn);
            resolve_stmt(*c->nodes[1], fn);
          } else if (c->tag == "elsepart"_) {
            resolve_stmt(*c->nodes[0], fn);
          } else {
            resolve_expr(*c, fn);
          }
        }
        return;
      case "whilestmt"_:
      case "untilstmt"_:
        resolve_expr(*a.nodes[0], fn);
        resolve_stmt(*a.nodes[1], fn);
        return;
      case "casestmt"_: {
        resolve_expr(*a.nodes[0], fn);
        for (size_t i = 1; i < a.nodes.size(); ++i) {
          const Ast& c = *a.nodes[i];
          if (c.tag == "whenpart"_) {
            for (const auto& v : c.nodes[0]->nodes) resolve_expr(*v, fn);
            resolve_stmt(*c.nodes[1], fn);
          } else if (c.tag == "elsepart"_) {
            resolve_stmt(*c.nodes[0], fn);
          }
        }
        return;
      }
      case "modstmt"_:
        resolve_stmt(*a.nodes[0], fn);
        resolve_expr(*a.nodes[1]->nodes[0], fn);
        return;
      case "beginstmt"_:
        for (const auto& c : a.nodes) {
          if (c->tag == "rescuepart"_) {
            for (const auto& g : c->nodes) {
              if (g->tag == "rescuevar"_) {
                decl_of[g->nodes[0].get()] =
                    declare(std::string(g->nodes[0]->token), fn);
              } else {
                resolve_stmt(*g, fn);
              }
            }
          } else if (c->tag == "ensurepart"_) {
            // A Defer takes a callable, so the ensure block is a function.
            const int32_t g = new_fn(fn, "ensure");
            fns[static_cast<size_t>(g)].body = c->nodes[0].get();
            fn_of[c.get()] = g;
            // The scope goes on *first*: `declare` reuses a name already
            // bound in an enclosing porous scope, and every function here
            // declares a `__block`, so declaring before pushing would hand
            // this one the enclosing function's parameter instead of its
            // own -- which then has a slot in the wrong frame.
            scopes.push_back({g, {}, true});
            fns[static_cast<size_t>(g)].params.push_back(
                declare_param("__block", g));
            fns[static_cast<size_t>(g)].defaults.push_back(nullptr);
            for (const auto& s : c->nodes[0]->nodes) resolve_stmt(*s, g);
            scopes.pop_back();
          } else {
            resolve_stmt(*c, fn);
          }
        }
        return;
      case "assign"_:
        resolve_expr(*a.nodes[2], fn);
        resolve_expr(*a.nodes[0], fn);
        return;
      case "exprstmt"_:
        resolve_expr(*a.nodes[0], fn);
        return;
      default:
        resolve_expr(a, fn);
        return;
    }
  }

  void resolve_expr(const Ast& a, int32_t fn) {
    switch (a.tag) {
      case "number"_:
      case "float"_:
      case "string"_:
      case "literal"_:
      case "itext"_:
      case "symbol"_:
      case "selfexpr"_:
      case "ivar"_:
        return;
      // `identh` is the same identifier with a tighter whitespace rule --
      // see grammar.h. Resolution does not care which one it came from.
      case "identh"_:
      case "ident"_: {
        const std::string n(a.token);
        if (auto v = resolve(n, fn)) {
          ref_of[&a] = *v;
          return;
        }
        if (is_global(n)) return;
        // Not a local: a paren-less call of a method defined somewhere in
        // the program, which emit resolves by name.
        return;
      }
      case "braceblock"_:
      case "doblock"_:
        resolve_block(a, fn, false);
        return;
      case "supercall"_:
        if (!a.nodes.empty()) {
          for (const auto& x : a.nodes[0]->nodes) resolve_expr(*x, fn);
        }
        return;
      default:
        for (const auto& c : a.nodes) {
          if (c->tag == "cmpop"_ || c->tag == "addop"_ ||
              c->tag == "mulop"_ || c->tag == "assignop"_ ||
              c->tag == "cmdname"_) {
            continue;
          }
          if (c->tag == "methodsfx"_) {
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

  static const std::vector<std::string>& rt_names() {
    static const std::vector<std::string> names = {
        "$truthy", "$eq",   "$cmp",   "$add",  "$sub",  "$mul", "$div",
        "$mod",    "$pow",  "$fstr",  "$str",  "$inspect", "$arrstr",
        "$hashstr", "$len", "$idx",   "$setidx", "$range", "$iter",
        "$iternext", "$each", "$map", "$select", "$inject", "$inject1",
        "$eachwi", "$push", "$keys", "$sort", "$include", "$join", "$toa",
        "$first",  "$last", "$exc",  "$mkexc", "$puts", "$putsone",
        "$arity",  "$times", "$reverse", "$argerr",
        // Classes: a table of ordinary objects, walked.
        "$classof", "$mfind", "$clsname", "$isa", "$isexact", "$nometh",
        "$caseeq",
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
    NodeId arr(const std::vector<NodeId>& v) { return b.array_lit(v, p); }
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
    NodeId wh(NodeId c, NodeId body_) { return b.make_while(c, body_, p); }
    NodeId idx(NodeId r, NodeId k) { return b.index(r, k, p); }
    NodeId idx(NodeId r, const std::string& k) { return b.index(r, S(k), p); }
    NodeId sidx(NodeId r, NodeId k, NodeId v) {
      return b.set_index(r, k, v, p);
    }
    NodeId obj(const std::vector<std::pair<std::string, NodeId>>& kvs) {
      std::vector<std::pair<NodeId, NodeId>> out;
      for (const auto& kv : kvs) out.emplace_back(S(kv.first), kv.second);
      return b.object_lit(out, p);
    }
    NodeId typ(NodeId v) { return in(IntrinsicId::TypeOf, {v}); }
    NodeId len(NodeId v) { return in(IntrinsicId::Len, {v}); }
    NodeId push(NodeId a, NodeId v) {
      return in(IntrinsicId::ArrayPush, {a, v});
    }
    NodeId is(NodeId v, const std::string& s) { return bin(BinOp::Eq, v, S(s)); }
    NodeId isnt(NodeId v, const std::string& s) {
      return bin(BinOp::Ne, v, S(s));
    }
    NodeId has(NodeId t, NodeId k) {
      return in(IntrinsicId::ObjectHas, {t, k});
    }
    NodeId both(NodeId x, NodeId y) { return b.make_if(x, y, Bo(false), p); }
    NodeId either(NodeId x, NodeId y) { return b.make_if(x, Bo(true), y, p); }
    NodeId call(const std::string& name, const std::vector<NodeId>& a) {
      return b.call_value(
          b.make_closure(bd.rt.at(name), bd.empty_cmap, p), a, p);
    }
    NodeId nat(const std::string& name, const std::vector<NodeId>& a) {
      return b.call_value(b.native_ref(b.declare_native(name), p), a, p);
    }
    // Calling a Proc/block value: its own parameter 0 is a block of its
    // own (nil, since we never pass one), and the real arguments follow.
    NodeId callb(NodeId f, const std::vector<NodeId>& a) {
      std::vector<NodeId> full{Nil()};
      full.insert(full.end(), a.begin(), a.end());
      return b.call_value(f, full, p);
    }
    void add(NodeId n) { body.push_back(n); }

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

  void one(const std::string& name, int32_t nparams,
          const std::function<NodeId(RT&)>& f,
          std::vector<std::string> names) {
    RT r(*this);
    r.add(r.ret(f(r)));
    r.finish(name, nparams, nparams, std::move(names));
  }

  void rt_truthy() {
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(0)), "nil"), r.ret(r.Bo(false))));
    r.add(r.iff(r.is(r.typ(r.L(0)), "bool"), r.ret(r.L(0))));
    r.add(r.ret(r.Bo(true)));
    r.finish("$truthy", 1, 1, {"v"});
  }

  void rt_eq() {
    RT r(*this);
    r.add(r.set(2, r.typ(r.L(0))));
    r.add(r.set(3, r.typ(r.L(1))));
    const auto num = [&](NodeId t) {
      return r.either(r.is(t, "int"), r.is(t, "double"));
    };
    r.add(r.iff(r.both(num(r.L(2)), num(r.L(3))),
                r.ret(r.bin(BinOp::Eq, r.L(0), r.L(1)))));
    r.add(r.iff(r.bin(BinOp::Ne, r.L(2), r.L(3)), r.ret(r.Bo(false))));
    r.add(r.iff(r.is(r.L(2), "nil"), r.ret(r.Bo(true))));
    r.add(r.iff(r.either(r.is(r.L(2), "bool"), r.is(r.L(2), "string")),
                r.ret(r.bin(BinOp::Eq, r.L(0), r.L(1)))));
    r.add(r.iff(
        r.is(r.L(2), "array"),
        r.blk({r.iff(r.bin(BinOp::Ne, r.len(r.L(0)), r.len(r.L(1))),
                     r.ret(r.Bo(false))),
               r.set(4, r.I(0)),
               r.wh(r.bin(BinOp::Lt, r.L(4), r.len(r.L(0))),
                    r.blk({r.iff(r.bin(BinOp::Eq,
                                       r.call("$eq", {r.idx(r.L(0), r.L(4)),
                                                      r.idx(r.L(1), r.L(4))}),
                                       r.Bo(false)),
                                 r.ret(r.Bo(false))),
                           r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1)))})),
               r.ret(r.Bo(true))})));
    r.add(r.iff(
        r.is(r.L(2), "map"),
        r.blk({r.set(5, r.in(IntrinsicId::ObjectKeys, {r.L(0)})),
               r.iff(r.bin(BinOp::Ne, r.len(r.L(5)),
                           r.len(r.in(IntrinsicId::ObjectKeys, {r.L(1)}))),
                     r.ret(r.Bo(false))),
               r.set(4, r.I(0)),
               r.wh(r.bin(BinOp::Lt, r.L(4), r.len(r.L(5))),
                    r.blk({r.set(6, r.idx(r.L(5), r.L(4))),
                           r.iff(r.bin(BinOp::Eq, r.has(r.L(1), r.L(6)),
                                       r.Bo(false)),
                                 r.ret(r.Bo(false))),
                           r.iff(r.bin(BinOp::Eq,
                                       r.call("$eq", {r.idx(r.L(0), r.L(6)),
                                                      r.idx(r.L(1), r.L(6))}),
                                       r.Bo(false)),
                                 r.ret(r.Bo(false))),
                           r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1)))})),
               r.ret(r.Bo(true))})));
    r.add(r.set(7, r.call("$classof", {r.L(0)})));
    r.add(r.iff(
        r.isnt(r.typ(r.L(7)), "nil"),
        r.blk({r.set(8, r.call("$mfind", {r.L(7), r.S("\x02==")})),
               r.iff(r.isnt(r.typ(r.L(8)), "nil"),
                     r.ret(r.call("$truthy",
                                  {r.callb(r.L(8), {r.L(0), r.L(1)})})))})));
    r.add(r.ret(r.in(IntrinsicId::Same, {r.L(0), r.L(1)})));
    r.finish("$eq", 2, 9,
             {"a", "b", "ta", "tb", "i", "ks", "k", "c", "m"});
  }

  void rt_cmp() {
    RT r(*this);
    r.add(r.iff(r.bin(BinOp::Lt, r.L(0), r.L(1)), r.ret(r.I(-1))));
    r.add(r.iff(r.bin(BinOp::Gt, r.L(0), r.L(1)), r.ret(r.I(1))));
    r.add(r.ret(r.I(0)));
    r.finish("$cmp", 2, 2, {"a", "b"});
  }

  // `+` concatenates strings and arrays; `/` and `%` on two Integers floor,
  // where BinOp::Div truncates and BinOp::Mod is C's.
  void rt_add() {
    RT r(*this);
    r.add(r.set(2, r.typ(r.L(0))));
    r.add(r.iff(r.is(r.L(2), "string"),
                r.ret(r.bin(BinOp::Add, r.L(0), r.call("$str", {r.L(1)})))));
    r.add(r.iff(
        r.is(r.L(2), "array"),
        r.blk({r.set(3, r.arr({})), r.set(4, r.I(0)),
               r.wh(r.bin(BinOp::Lt, r.L(4), r.len(r.L(0))),
                    r.blk({r.push(r.L(3), r.idx(r.L(0), r.L(4))),
                           r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1)))})),
               r.set(4, r.I(0)),
               r.wh(r.bin(BinOp::Lt, r.L(4), r.len(r.L(1))),
                    r.blk({r.push(r.L(3), r.idx(r.L(1), r.L(4))),
                           r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1)))})),
               r.ret(r.L(3))})));
    r.add(r.ret(r.bin(BinOp::Add, r.L(0), r.L(1))));
    r.finish("$add", 2, 5, {"a", "b", "t", "out", "i"});
  }

  void rt_sub() {
    one("$sub", 2, [&](RT& r) { return r.bin(BinOp::Sub, r.L(0), r.L(1)); },
        {"a", "b"});
  }

  void rt_mul() {
    RT r(*this);
    r.add(r.iff(
        r.is(r.typ(r.L(0)), "string"),
        r.blk({r.set(2, r.S("")), r.set(3, r.I(0)),
               r.wh(r.bin(BinOp::Lt, r.L(3), r.L(1)),
                    r.blk({r.set(2, r.bin(BinOp::Add, r.L(2), r.L(0))),
                           r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})),
               r.ret(r.L(2))})));
    r.add(r.ret(r.bin(BinOp::Mul, r.L(0), r.L(1))));
    r.finish("$mul", 2, 4, {"a", "b", "out", "i"});
  }

  void rt_div() {
    RT r(*this);
    r.add(r.iff(r.either(r.is(r.typ(r.L(0)), "double"),
                         r.is(r.typ(r.L(1)), "double")),
                r.ret(r.bin(BinOp::Div, r.L(0), r.L(1)))));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(1), r.I(0)),
                r.ret(r.call("$exc", {r.S("ZeroDivisionError"),
                                      r.S("divided by 0")}))));
    r.add(r.set(2, r.bin(BinOp::Div, r.L(0), r.L(1))));
    r.add(r.iff(r.both(r.bin(BinOp::Ne, r.bin(BinOp::Mul, r.L(2), r.L(1)),
                             r.L(0)),
                       r.bin(BinOp::Lt, r.bin(BinOp::Mul, r.L(0), r.L(1)),
                             r.I(0))),
                r.set(2, r.bin(BinOp::Sub, r.L(2), r.I(1)))));
    r.add(r.ret(r.L(2)));
    r.finish("$div", 2, 3, {"a", "b", "q"});
  }

  void rt_mod() {
    RT r(*this);
    r.add(r.set(2, r.bin(BinOp::Mod, r.L(0), r.L(1))));
    r.add(r.iff(r.both(r.bin(BinOp::Ne, r.L(2), r.I(0)),
                       r.bin(BinOp::Lt, r.bin(BinOp::Mul, r.L(2), r.L(1)),
                             r.I(0))),
                r.set(2, r.bin(BinOp::Add, r.L(2), r.L(1)))));
    r.add(r.ret(r.L(2)));
    r.finish("$mod", 2, 3, {"a", "b", "m"});
  }

  void rt_pow() {
    RT r(*this);
    r.add(r.iff(r.either(r.is(r.typ(r.L(0)), "double"),
                         r.either(r.is(r.typ(r.L(1)), "double"),
                                  r.bin(BinOp::Lt, r.L(1), r.I(0)))),
                r.ret(r.in(IntrinsicId::Pow, {r.L(0), r.L(1)}))));
    r.add(r.set(2, r.I(1)));
    r.add(r.set(3, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(3), r.L(1)),
               r.blk({r.set(2, r.bin(BinOp::Mul, r.L(2), r.L(0))),
                      r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})));
    r.add(r.ret(r.L(2)));
    r.finish("$pow", 2, 4, {"a", "b", "acc", "i"});
  }

  void rt_fstr() {
    const double lim = 9007199254740992.0;
    RT r(*this);
    r.add(r.iff(r.bin(BinOp::Ne, r.L(0), r.L(0)), r.ret(r.S("NaN"))));
    r.add(r.iff(r.in(IntrinsicId::Same, {r.L(0), r.D(-0.0)}),
                r.ret(r.S("-0.0"))));
    r.add(r.iff(
        r.both(r.bin(BinOp::Gt, r.L(0), r.D(-lim)),
               r.bin(BinOp::Lt, r.L(0), r.D(lim))),
        r.blk({r.set(1, r.in(IntrinsicId::ToInt, {r.L(0)})),
               r.iff(r.bin(BinOp::Eq, r.in(IntrinsicId::ToDouble, {r.L(1)}),
                           r.L(0)),
                     r.ret(r.bin(BinOp::Add,
                                 r.in(IntrinsicId::ToStr, {r.L(1)}),
                                 r.S(".0"))))})));
    r.add(r.ret(r.in(IntrinsicId::ToStr, {r.L(0)})));
    r.finish("$fstr", 1, 2, {"d", "i"});
  }

  // Ruby's to_s: nil is the empty string, and a container is its inspect
  // form. A class instance's own `to_s` wins over the generic `#<Name>`.
  void rt_str() {
    RT r(*this);
    r.add(r.set(1, r.typ(r.L(0))));
    r.add(r.iff(r.is(r.L(1), "string"), r.ret(r.L(0))));
    r.add(r.iff(r.is(r.L(1), "nil"), r.ret(r.S(""))));
    r.add(r.iff(r.is(r.L(1), "bool"),
                r.ret(r.iff(r.L(0), r.S("true"), r.S("false")))));
    r.add(r.iff(r.is(r.L(1), "int"), r.ret(r.in(IntrinsicId::ToStr, {r.L(0)}))));
    r.add(r.iff(r.is(r.L(1), "double"), r.ret(r.call("$fstr", {r.L(0)}))));
    r.add(r.iff(r.is(r.L(1), "array"), r.ret(r.call("$arrstr", {r.L(0)}))));
    r.add(r.iff(r.is(r.L(1), "map"), r.ret(r.call("$hashstr", {r.L(0)}))));
    r.add(r.iff(r.is(r.L(1), "function"), r.ret(r.S("#<Proc>"))));
    r.add(r.iff(r.isnt(r.L(1), "object"), r.ret(r.S("#<Object>"))));
    r.add(r.iff(r.has(r.L(0), r.S(kExcKey)),
                r.ret(r.call("$str", {r.idx(r.L(0), kMsgKey)}))));
    r.add(r.iff(r.has(r.L(0), r.S(kRangeKey)),
                r.ret(r.bin(BinOp::Add,
                            r.bin(BinOp::Add,
                                  r.call("$str", {r.idx(r.L(0), "lo")}),
                                  r.S("..")),
                            r.call("$str", {r.idx(r.L(0), "hi")})))));
    r.add(r.set(2, r.call("$classof", {r.L(0)})));
    r.add(r.iff(
        r.isnt(r.typ(r.L(2)), "nil"),
        r.blk({r.set(3, r.call("$mfind", {r.L(2), r.S("\x02to_s")})),
               r.iff(r.isnt(r.typ(r.L(3)), "nil"),
                     r.ret(r.call("$str", {r.callb(r.L(3), {r.L(0)})}))),
               r.ret(r.bin(BinOp::Add,
                           r.bin(BinOp::Add, r.S("#<"), r.idx(r.L(2), kNameKey)),
                           r.S(">")))})));
    r.add(r.ret(r.S("#<Object>")));
    r.finish("$str", 1, 4, {"v", "t", "c", "m"});
  }

  void rt_inspect() {
    RT r(*this);
    r.add(r.set(1, r.typ(r.L(0))));
    r.add(r.iff(r.is(r.L(1), "string"),
                r.ret(r.bin(BinOp::Add,
                            r.bin(BinOp::Add, r.S("\""), r.L(0)), r.S("\"")))));
    r.add(r.iff(r.is(r.L(1), "nil"), r.ret(r.S("nil"))));
    r.add(r.iff(
        r.is(r.L(1), "object"),
        r.blk({r.set(2, r.call("$classof", {r.L(0)})),
               r.iff(r.isnt(r.typ(r.L(2)), "nil"),
                     r.blk({r.set(3, r.call("$mfind",
                                            {r.L(2), r.S("\x02inspect")})),
                            r.iff(r.isnt(r.typ(r.L(3)), "nil"),
                                  r.ret(r.call("$str",
                                               {r.callb(r.L(3), {r.L(0)})})))}))})));
    r.add(r.ret(r.call("$str", {r.L(0)})));
    r.finish("$inspect", 1, 4, {"v", "t", "c", "m"});
  }

  void rt_arrstr() {
    RT r(*this);
    r.add(r.set(1, r.S("[")));
    r.add(r.set(2, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(2), r.len(r.L(0))),
               r.blk({r.iff(r.bin(BinOp::Gt, r.L(2), r.I(0)),
                            r.set(1, r.bin(BinOp::Add, r.L(1), r.S(", ")))),
                      r.set(1, r.bin(BinOp::Add, r.L(1),
                                     r.call("$inspect",
                                            {r.idx(r.L(0), r.L(2))}))),
                      r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1)))})));
    r.add(r.ret(r.bin(BinOp::Add, r.L(1), r.S("]"))));
    r.finish("$arrstr", 1, 3, {"a", "out", "i"});
  }

  void rt_hashstr() {
    RT r(*this);
    r.add(r.set(1, r.S("{")));
    r.add(r.set(2, r.I(0)));
    r.add(r.set(3, r.in(IntrinsicId::ObjectKeys, {r.L(0)})));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(2), r.len(r.L(3))),
        r.blk({r.iff(r.bin(BinOp::Gt, r.L(2), r.I(0)),
                     r.set(1, r.bin(BinOp::Add, r.L(1), r.S(", ")))),
               r.set(4, r.idx(r.L(3), r.L(2))),
               r.set(1, r.bin(BinOp::Add, r.L(1),
                              r.bin(BinOp::Add,
                                    r.call("$inspect", {r.L(4)}),
                                    r.bin(BinOp::Add, r.S("=>"),
                                          r.call("$inspect",
                                                 {r.idx(r.L(0), r.L(4))}))))),
               r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1)))})));
    r.add(r.ret(r.bin(BinOp::Add, r.L(1), r.S("}"))));
    r.finish("$hashstr", 1, 5, {"h", "out", "i", "ks", "k"});
  }

  void rt_len() {
    RT r(*this);
    r.add(r.set(1, r.typ(r.L(0))));
    r.add(r.iff(r.either(r.is(r.L(1), "string"),
                         r.either(r.is(r.L(1), "array"), r.is(r.L(1), "map"))),
                r.ret(r.len(r.L(0)))));
    r.add(r.ret(r.call("$exc", {r.S("NoMethodError"),
                                r.S("undefined method 'length'")})));
    r.finish("$len", 1, 2, {"v", "t"});
  }

  // `xs[i]` and `xs[-1]` never raise: an out-of-range array index, or a
  // missing hash key, is nil in Ruby -- unlike every other language here.
  void rt_idx() {
    RT r(*this);
    r.add(r.set(2, r.typ(r.L(0))));
    r.add(r.iff(
        r.either(r.is(r.L(2), "array"), r.is(r.L(2), "string")),
        r.blk({r.set(3, r.L(1)),
               r.iff(r.bin(BinOp::Lt, r.L(3), r.I(0)),
                     r.set(3, r.bin(BinOp::Add, r.L(3), r.len(r.L(0))))),
               r.iff(r.either(r.bin(BinOp::Lt, r.L(3), r.I(0)),
                              r.bin(BinOp::Ge, r.L(3), r.len(r.L(0)))),
                     r.ret(r.Nil())),
               r.ret(r.idx(r.L(0), r.L(3)))})));
    r.add(r.iff(r.is(r.L(2), "map"), r.ret(r.idx(r.L(0), r.L(1)))));
    r.add(r.ret(r.call("$exc", {r.S("TypeError"),
                                r.S("no implicit conversion into an "
                                    "index")})));
    r.finish("$idx", 2, 4, {"v", "k", "t", "i"});
  }

  void rt_setidx() {
    RT r(*this);
    r.add(r.set(3, r.typ(r.L(0))));
    r.add(r.iff(
        r.is(r.L(3), "array"),
        r.blk({r.set(4, r.L(1)),
               r.iff(r.bin(BinOp::Lt, r.L(4), r.I(0)),
                     r.set(4, r.bin(BinOp::Add, r.L(4), r.len(r.L(0))))),
               // Assigning at or past the end extends the array with nil.
               r.wh(r.bin(BinOp::Le, r.len(r.L(0)), r.L(4)),
                    r.push(r.L(0), r.Nil())),
               r.sidx(r.L(0), r.L(4), r.L(2)),
               r.ret(r.L(2))})));
    r.add(r.iff(r.is(r.L(3), "map"),
                r.blk({r.sidx(r.L(0), r.L(1), r.L(2)), r.ret(r.L(2))})));
    r.add(r.ret(r.call("$exc", {r.S("TypeError"),
                                r.S("cannot index this")})));
    r.finish("$setidx", 3, 5, {"v", "k", "val", "t", "i"});
  }

  void rt_range() {
    one("$range", 2, [&](RT& r) {
      return r.obj({{kRangeKey, r.Bo(true)},
                    {"lo", r.L(0)},
                    {"hi", r.L(1)}});
    }, {"lo", "hi"});
  }

  // The shared iteration cursor: `{k:"a", v:array, i:index}` for an array
  // or a Range (expanded once, up front), and `{k:"h", v:keys, src:hash,
  // i:index}` for a Hash -- so `$each`/`$map`/etc. all walk the one shape,
  // and only a Hash's step differs (it hands back a key and a value).
  void rt_iter() {
    RT r(*this);
    r.add(r.set(1, r.typ(r.L(0))));
    r.add(r.iff(r.is(r.L(1), "array"),
                r.ret(r.obj({{"k", r.S("a")}, {"v", r.L(0)}, {"i", r.I(0)}}))));
    r.add(r.iff(
        r.is(r.L(1), "map"),
        r.ret(r.obj({{"k", r.S("h")},
                     {"v", r.in(IntrinsicId::ObjectKeys, {r.L(0)})},
                     {"src", r.L(0)},
                     {"i", r.I(0)}}))));
    r.add(r.iff(
        r.both(r.is(r.L(1), "object"), r.has(r.L(0), r.S(kRangeKey))),
        r.ret(r.obj({{"k", r.S("a")},
                     {"v", r.call("$toa", {r.L(0)})},
                     {"i", r.I(0)}}))));
    r.add(r.ret(r.call("$exc", {r.S("TypeError"),
                                r.S("not iterable")})));
    r.finish("$iter", 1, 2, {"v", "t"});
  }

  void rt_iternext() {
    RT r(*this);
    r.add(r.set(1, r.idx(r.L(0), "v")));
    r.add(r.set(2, r.idx(r.L(0), "i")));
    r.add(r.iff(r.bin(BinOp::Ge, r.L(2), r.len(r.L(1))),
                r.ret(r.obj({{"done", r.Bo(true)}}))));
    r.add(r.sidx(r.L(0), r.S("i"), r.bin(BinOp::Add, r.L(2), r.I(1))));
    r.add(r.iff(
        r.is(r.idx(r.L(0), "k"), "h"),
        r.blk({r.set(3, r.idx(r.L(1), r.L(2))),
               r.ret(r.obj({{"done", r.Bo(false)},
                            {"key", r.L(3)},
                            {"value", r.idx(r.idx(r.L(0), "src"), r.L(3))}}))})));
    r.add(r.ret(
        r.obj({{"done", r.Bo(false)}, {"value", r.idx(r.L(1), r.L(2))}})));
    r.finish("$iternext", 1, 4, {"it", "a", "i", "k"});
  }

  // `.each`: a Hash's step hands back a key and a value (two block args);
  // an Array's or a Range's hands back one.
  void rt_each() {
    RT r(*this);
    r.add(r.set(2, r.call("$iter", {r.L(0)})));
    r.add(r.wh(
        r.Bo(true),
        r.blk({r.set(3, r.call("$iternext", {r.L(2)})),
               r.iff(r.idx(r.L(3), "done"), r.b.make_break(r.p)),
               r.iff(r.has(r.L(3), r.S("key")),
                     r.callb(r.L(1), {r.idx(r.L(3), "key"),
                                      r.idx(r.L(3), "value")}),
                     r.callb(r.L(1), {r.idx(r.L(3), "value")}))})));
    r.add(r.ret(r.L(0)));
    r.finish("$each", 2, 4, {"v", "blk", "it", "st"});
  }

  void rt_map() {
    RT r(*this);
    r.add(r.set(2, r.arr({})));
    r.add(r.set(3, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(3), r.len(r.L(0))),
               r.blk({r.push(r.L(2), r.callb(r.L(1), {r.idx(r.L(0), r.L(3))})),
                      r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})));
    r.add(r.ret(r.L(2)));
    r.finish("$map", 2, 4, {"a", "blk", "out", "i"});
  }

  void rt_select() {
    RT r(*this);
    r.add(r.set(2, r.arr({})));
    r.add(r.set(3, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(3), r.len(r.L(0))),
               r.blk({r.iff(r.call("$truthy",
                                   {r.callb(r.L(1), {r.idx(r.L(0), r.L(3))})}),
                            r.push(r.L(2), r.idx(r.L(0), r.L(3)))),
                      r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})));
    r.add(r.ret(r.L(2)));
    r.finish("$select", 2, 4, {"a", "blk", "out", "i"});
  }

  void rt_inject() {
    RT r(*this);
    r.add(r.set(3, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(3), r.len(r.L(0))),
               r.blk({r.set(1, r.callb(r.L(2), {r.L(1), r.idx(r.L(0), r.L(3))})),
                      r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})));
    r.add(r.ret(r.L(1)));
    r.finish("$inject", 3, 4, {"a", "acc", "blk", "i"});
  }

  void rt_inject1() {
    RT r(*this);
    r.add(r.iff(r.bin(BinOp::Eq, r.len(r.L(0)), r.I(0)), r.ret(r.Nil())));
    r.add(r.set(2, r.idx(r.L(0), r.I(0))));
    r.add(r.set(3, r.I(1)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(3), r.len(r.L(0))),
               r.blk({r.set(2, r.callb(r.L(1), {r.L(2), r.idx(r.L(0), r.L(3))})),
                      r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})));
    r.add(r.ret(r.L(2)));
    r.finish("$inject1", 2, 4, {"a", "blk", "acc", "i"});
  }

  void rt_eachwi() {
    RT r(*this);
    r.add(r.set(2, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(2), r.len(r.L(0))),
               r.blk({r.callb(r.L(1), {r.idx(r.L(0), r.L(2)), r.L(2)}),
                      r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1)))})));
    r.add(r.ret(r.L(0)));
    r.finish("$eachwi", 2, 3, {"a", "blk", "i"});
  }

  void rt_times() {
    RT r(*this);
    r.add(r.set(2, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(2), r.L(0)),
               r.blk({r.callb(r.L(1), {r.L(2)}),
                      r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1)))})));
    r.add(r.ret(r.L(0)));
    r.finish("$times", 2, 3, {"n", "blk", "i"});
  }

  void rt_push() {
    RT r(*this);
    r.add(r.push(r.L(0), r.L(1)));
    r.add(r.ret(r.L(0)));
    r.finish("$push", 2, 2, {"a", "v"});
  }

  void rt_keys() {
    one("$keys", 1, [&](RT& r) {
      return r.in(IntrinsicId::ObjectKeys, {r.L(0)});
    }, {"h"});
  }

  // A stable insertion sort over a copy -- Ruby's own is a quicksort
  // variant, but a program can only observe the ordering, which agrees.
  void rt_sort() {
    RT r(*this);
    r.add(r.set(1, r.arr({})));
    r.add(r.set(2, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(2), r.len(r.L(0))),
               r.blk({r.push(r.L(1), r.idx(r.L(0), r.L(2))),
                      r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1)))})));
    r.add(r.set(2, r.I(1)));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(2), r.len(r.L(1))),
        r.blk({r.set(3, r.idx(r.L(1), r.L(2))),
               r.set(4, r.bin(BinOp::Sub, r.L(2), r.I(1))),
               r.wh(r.both(r.bin(BinOp::Ge, r.L(4), r.I(0)),
                           r.bin(BinOp::Gt, r.call("$cmp",
                                                   {r.idx(r.L(1), r.L(4)),
                                                    r.L(3)}),
                                 r.I(0))),
                    r.blk({r.sidx(r.L(1), r.bin(BinOp::Add, r.L(4), r.I(1)),
                                  r.idx(r.L(1), r.L(4))),
                           r.set(4, r.bin(BinOp::Sub, r.L(4), r.I(1)))})),
               r.sidx(r.L(1), r.bin(BinOp::Add, r.L(4), r.I(1)), r.L(3)),
               r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1)))})));
    r.add(r.ret(r.L(1)));
    r.finish("$sort", 1, 5, {"a", "out", "i", "v", "j"});
  }

  void rt_include() {
    RT r(*this);
    r.add(r.set(2, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(2), r.len(r.L(0))),
               r.blk({r.iff(r.call("$eq", {r.idx(r.L(0), r.L(2)), r.L(1)}),
                            r.ret(r.Bo(true))),
                      r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1)))})));
    r.add(r.ret(r.Bo(false)));
    r.finish("$include", 2, 3, {"a", "v", "i"});
  }

  void rt_join() {
    RT r(*this);
    r.add(r.set(2, r.S("")));
    r.add(r.set(3, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(3), r.len(r.L(0))),
               r.blk({r.iff(r.bin(BinOp::Gt, r.L(3), r.I(0)),
                            r.set(2, r.bin(BinOp::Add, r.L(2), r.L(1)))),
                      r.set(2, r.bin(BinOp::Add, r.L(2),
                                     r.call("$str", {r.idx(r.L(0), r.L(3))}))),
                      r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})));
    r.add(r.ret(r.L(2)));
    r.finish("$join", 2, 4, {"a", "sep", "out", "i"});
  }

  void rt_toa() {
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(0)), "array"), r.ret(r.L(0))));
    r.add(r.iff(
        r.both(r.is(r.typ(r.L(0)), "object"), r.has(r.L(0), r.S(kRangeKey))),
        r.blk({r.set(1, r.arr({})), r.set(2, r.idx(r.L(0), "lo")),
               r.wh(r.bin(BinOp::Le, r.L(2), r.idx(r.L(0), "hi")),
                    r.blk({r.push(r.L(1), r.L(2)),
                           r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1)))})),
               r.ret(r.L(1))})));
    r.add(r.ret(r.arr({r.L(0)})));
    r.finish("$toa", 1, 3, {"v", "out", "i"});
  }

  void rt_first() {
    one("$first", 1, [&](RT& r) {
      return r.iff(r.bin(BinOp::Eq, r.len(r.L(0)), r.I(0)), r.Nil(),
                   r.idx(r.L(0), r.I(0)));
    }, {"a"});
  }

  void rt_last() {
    one("$last", 1, [&](RT& r) {
      return r.iff(r.bin(BinOp::Eq, r.len(r.L(0)), r.I(0)), r.Nil(),
                   r.idx(r.L(0), r.bin(BinOp::Sub, r.len(r.L(0)), r.I(1))));
    }, {"a"});
  }

  void rt_mkexc() {
    one("$mkexc", 2, [&](RT& r) {
      return r.obj({{kExcKey, r.L(0)}, {kMsgKey, r.L(1)}});
    }, {"kind", "msg"});
  }

  void rt_exc() {
    RT r(*this);
    r.add(r.b.make_throw(r.call("$mkexc", {r.L(0), r.L(1)}), r.p));
    r.finish("$exc", 2, 2, {"kind", "msg"});
  }

  void rt_argerr() {
    RT r(*this);
    r.add(r.b.make_throw(
        r.call("$mkexc", {r.S("ArgumentError"), r.S("wrong number of "
                                                   "arguments")}),
        r.p));
    r.finish("$argerr", 0, 0, {});
  }

  // Proc#arity. FnArity counts the implicit block parameter this front end
  // gives every function, so the answer is one less than the intrinsic's
  // -- and one less again for an instance method's implicit `self`, but no
  // sample here asks a bound method its arity, so that case is not tried.
  void rt_arity() {
    one("$arity", 1, [&](RT& r) {
      return r.bin(BinOp::Sub, r.in(IntrinsicId::FnArity, {r.L(0)}), r.I(1));
    }, {"f"});
  }

  void rt_reverse() {
    RT r(*this);
    r.add(r.set(1, r.arr({})));
    r.add(r.set(2, r.bin(BinOp::Sub, r.len(r.L(0)), r.I(1))));
    r.add(r.wh(r.bin(BinOp::Ge, r.L(2), r.I(0)),
               r.blk({r.push(r.L(1), r.idx(r.L(0), r.L(2))),
                      r.set(2, r.bin(BinOp::Sub, r.L(2), r.I(1)))})));
    r.add(r.ret(r.L(1)));
    r.finish("$reverse", 1, 3, {"a", "out", "i"});
  }

  // `puts` flattens an array argument and gives every line its own
  // newline, and an empty argument list prints one blank line.
  void rt_puts() {
    RT r(*this);
    r.add(r.iff(r.bin(BinOp::Eq, r.len(r.L(0)), r.I(0)), r.ret(r.S("\n"))));
    r.add(r.set(1, r.S("")));
    r.add(r.set(2, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(2), r.len(r.L(0))),
               r.blk({r.set(1, r.bin(BinOp::Add, r.L(1),
                                     r.call("$putsone",
                                            {r.idx(r.L(0), r.L(2))}))),
                      r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1)))})));
    r.add(r.ret(r.L(1)));
    r.finish("$puts", 1, 3, {"args", "out", "i"});
  }

  void rt_putsone() {
    RT r(*this);
    r.add(r.iff(
        r.is(r.typ(r.L(0)), "array"),
        r.blk({r.set(1, r.S("")), r.set(2, r.I(0)),
               r.wh(r.bin(BinOp::Lt, r.L(2), r.len(r.L(0))),
                    r.blk({r.set(1, r.bin(BinOp::Add, r.L(1),
                                          r.call("$putsone",
                                                 {r.idx(r.L(0), r.L(2))}))),
                           r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1)))})),
               r.ret(r.L(1))})));
    r.add(r.set(1, r.call("$str", {r.L(0)})));
    r.add(r.iff(r.both(r.bin(BinOp::Gt, r.len(r.L(1)), r.I(0)),
                       r.bin(BinOp::Eq,
                             r.in(IntrinsicId::StrByte,
                                  {r.L(1), r.bin(BinOp::Sub, r.len(r.L(1)),
                                                 r.I(1))}),
                             r.I(10))),
                r.ret(r.L(1))));
    r.add(r.ret(r.bin(BinOp::Add, r.L(1), r.S("\n"))));
    r.finish("$putsone", 1, 3, {"v", "out", "i"});
  }

  // -- Classes --------------------------------------------------------------
  //
  // There is no class in this IR and nothing was added for one: a class is
  // an object holding its base's table and its methods, keyed with a
  // prefix no Ruby identifier can start with; an instance is a plain
  // object pointing back at it under kClassKey. Every question a class
  // system answers is a walk up that chain. See README.md.

  void rt_classof() {
    RT r(*this);
    r.add(r.iff(r.isnt(r.typ(r.L(0)), "object"), r.ret(r.Nil())));
    r.add(r.iff(r.bin(BinOp::Eq, r.has(r.L(0), r.S(kClassKey)), r.Bo(false)),
                r.ret(r.Nil())));
    r.add(r.ret(r.idx(r.L(0), kClassKey)));
    r.finish("$classof", 1, 1, {"v"});
  }

  void rt_mfind() {
    RT r(*this);
    r.add(r.set(2, r.L(0)));
    r.add(r.wh(r.is(r.typ(r.L(2)), "object"),
               r.blk({r.iff(r.has(r.L(2), r.L(1)),
                            r.ret(r.idx(r.L(2), r.L(1)))),
                      r.iff(r.bin(BinOp::Eq, r.has(r.L(2), r.S(kBaseKey)),
                                  r.Bo(false)),
                            r.b.make_break(r.p)),
                      r.set(2, r.idx(r.L(2), kBaseKey))})));
    r.add(r.ret(r.Nil()));
    r.finish("$mfind", 2, 3, {"t", "key", "c"});
  }

  void rt_clsname() {
    RT r(*this);
    r.add(r.set(1, r.call("$classof", {r.L(0)})));
    r.add(r.iff(r.isnt(r.typ(r.L(1)), "nil"), r.ret(r.idx(r.L(1), kNameKey))));
    r.add(r.iff(r.both(r.is(r.typ(r.L(0)), "object"),
                       r.has(r.L(0), r.S(kExcKey))),
                r.ret(r.idx(r.L(0), kExcKey))));
    r.add(r.set(2, r.typ(r.L(0))));
    r.add(r.iff(r.is(r.L(2), "nil"), r.ret(r.S("NilClass"))));
    r.add(r.iff(r.is(r.L(2), "bool"),
                r.ret(r.iff(r.L(0), r.S("TrueClass"), r.S("FalseClass")))));
    r.add(r.iff(r.is(r.L(2), "int"), r.ret(r.S("Integer"))));
    r.add(r.iff(r.is(r.L(2), "double"), r.ret(r.S("Float"))));
    r.add(r.iff(r.is(r.L(2), "string"), r.ret(r.S("String"))));
    r.add(r.iff(r.is(r.L(2), "array"), r.ret(r.S("Array"))));
    r.add(r.iff(r.is(r.L(2), "map"), r.ret(r.S("Hash"))));
    r.add(r.iff(r.is(r.L(2), "function"), r.ret(r.S("Proc"))));
    r.add(r.ret(r.S("Object")));
    r.finish("$clsname", 1, 3, {"v", "c", "t"});
  }

  // `is_a?`/`kind_of?`: a walk up the same chain, testing identity against
  // the class value itself -- which is why a bare class name has to
  // resolve to something, not a method call, at every other point in this
  // front end too.
  void rt_isa() {
    RT r(*this);
    r.add(r.set(2, r.call("$classof", {r.L(0)})));
    r.add(r.wh(r.is(r.typ(r.L(2)), "object"),
               r.blk({r.iff(r.in(IntrinsicId::Same, {r.L(2), r.L(1)}),
                            r.ret(r.Bo(true))),
                      r.iff(r.bin(BinOp::Eq, r.has(r.L(2), r.S(kBaseKey)),
                                  r.Bo(false)),
                            r.b.make_break(r.p)),
                      r.set(2, r.idx(r.L(2), kBaseKey))})));
    r.add(r.ret(r.Bo(false)));
    r.finish("$isa", 2, 3, {"v", "cls", "t"});
  }

  void rt_isexact() {
    one("$isexact", 2, [&](RT& r) {
      return r.in(IntrinsicId::Same, {r.call("$classof", {r.L(0)}), r.L(1)});
    }, {"v", "cls"});
  }

  void rt_nometh() {
    RT r(*this);
    r.add(r.call("$exc", {r.S("NoMethodError"),
                          r.bin(BinOp::Add,
                                r.bin(BinOp::Add, r.S("undefined method '"),
                                      r.L(1)),
                                r.bin(BinOp::Add,
                                      r.S("' for "),
                                      r.call("$inspect", {r.L(0)})))}));
    r.add(r.ret(r.Nil()));
    r.finish("$nometh", 2, 2, {"recv", "name"});
  }

  // `case`/`when`: a Range matches by inclusion, a class by `is_a?`,
  // anything else by `==` -- Ruby's own `===`, in the shape samples need.
  void rt_caseeq() {
    RT r(*this);
    r.add(r.iff(
        r.both(r.is(r.typ(r.L(0)), "object"), r.has(r.L(0), r.S(kRangeKey))),
        r.ret(r.both(r.bin(BinOp::Ge, r.call("$cmp", {r.L(1), r.idx(r.L(0), "lo")}),
                           r.I(0)),
                     r.bin(BinOp::Le, r.call("$cmp", {r.L(1), r.idx(r.L(0), "hi")}),
                           r.I(0))))));
    r.add(r.iff(
        r.both(r.is(r.typ(r.L(0)), "object"), r.has(r.L(0), r.S(kNameKey))),
        r.ret(r.call("$isa", {r.L(1), r.L(0)}))));
    r.add(r.ret(r.call("$eq", {r.L(0), r.L(1)})));
    r.finish("$caseeq", 2, 2, {"pattern", "value"});
  }

  void emit_runtime() {
    rt_truthy(); rt_eq(); rt_cmp(); rt_add(); rt_sub(); rt_mul(); rt_div();
    rt_mod(); rt_pow(); rt_fstr(); rt_str(); rt_inspect(); rt_arrstr();
    rt_hashstr(); rt_len(); rt_idx(); rt_setidx(); rt_range(); rt_iter();
    rt_iternext(); rt_each(); rt_map(); rt_select(); rt_inject();
    rt_inject1(); rt_eachwi(); rt_push(); rt_keys(); rt_sort(); rt_include();
    rt_join(); rt_toa(); rt_first(); rt_last(); rt_exc(); rt_mkexc();
    rt_puts(); rt_putsone(); rt_arity(); rt_times(); rt_reverse();
    rt_argerr();
    rt_classof(); rt_mfind(); rt_clsname(); rt_isa(); rt_isexact();
    rt_nometh(); rt_caseeq();
  }

  // ==== Pass B: emit ======================================================

  static int32_t helper_slot(const std::string& name) {
    const auto& names = rt_names();
    for (size_t i = 0; i < names.size(); ++i) {
      if (names[i] == name) return static_cast<int32_t>(i);
    }
    coreir_rt::fail("unknown runtime helper " + name, 0, 0);
  }

  // A helper call reads the one closure that already exists rather than
  // building another: these capture nothing, so a per-function cell meant
  // one allocation per helper per *call* of every function that used one.
  NodeId helper(FnCtx& ctx, const std::string& name,
                const std::vector<NodeId>& args, SrcPos p) {
    Builder b(m);
    return b.call_value(
        b.index(read_var(helpers_var, ctx, p),
                b.literal(helper_slot(name), p), p),
        args, p);
  }


  // File scope builds every helper's closure once, into the array above.
  void fill_helpers(std::vector<NodeId>& out, SrcPos p) {
    Builder b(m);
    std::vector<NodeId> vals;
    for (const std::string& n : rt_names()) {
      // A helper that takes captures is built at the site that has them,
      // never fetched from here, so its slot stays nil.
      const size_t idx = static_cast<size_t>(rt.at(n));
      vals.push_back(m.funcs[idx].num_captures == 0
                         ? b.make_closure(rt.at(n), empty_cmap, p)
                         : b.nil_literal(p));
    }
    const int32_t c = fns[0].cell_index.at(helpers_var);
    out.push_back(b.cell_fresh(c, p));
    out.push_back(b.assign(VarKind::Cell, c, b.array_lit(vals, p), p));
  }

  NodeId native(const std::string& name, const std::vector<NodeId>& args,
                SrcPos p) {
    Builder b(m);
    return b.call_value(b.native_ref(b.declare_native(name), p), args, p);
  }

  NodeId emit_closure(int32_t g, FnCtx& ctx, SrcPos p) {
    Builder b(m);
    std::vector<CaptureSrc> cs;
    for (const int32_t v : fns[static_cast<size_t>(g)].free) {
      const auto [k, i] = access(ctx.fn, v);
      cs.push_back({k, i});
    }
    const int32_t cm = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back(cs);
    return b.make_closure(fns[static_cast<size_t>(g)].index, cm, p);
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

  NodeId truthy(NodeId v, FnCtx& ctx, SrcPos p) {
    return helper(ctx, "$truthy", {v}, p);
  }

  NodeId block_of(FnCtx& ctx, SrcPos p) {
    return read_var(fns[static_cast<size_t>(ctx.fn)].params[0], ctx, p);
  }

  // `self` inside the method the given node sits in -- `params[1]`, always,
  // since parameter 0 is the block and every instance method's resolve_def
  // call put self right after it.
  NodeId self_of(const Ast& a, FnCtx& ctx, SrcPos p) {
    const FnInfo& fi = fns[static_cast<size_t>(ctx.fn)];
    if (!fi.is_method) fail(a, "'self' and '@' are only valid in a method");
    return read_var(fi.params[1], ctx, p);
  }

  // A method call this front end cannot resolve by name at bind time,
  // because the receiver may be any class's instance: `$classof` and
  // `$mfind` are the walk, done at every call site that reaches here.
  NodeId emit_dynamic_call(NodeId recv, const std::string& name,
                           const std::vector<NodeId>& args, NodeId blk,
                           FnCtx& ctx, SrcPos p) {
    Builder b(m);
    const int32_t t = ctx.alloc_local("$self");
    const int32_t mv = ctx.alloc_local("$m");
    const NodeId T = b.varref(VarKind::Local, t, p);
    const NodeId M = b.varref(VarKind::Local, mv, p);
    std::vector<NodeId> callargs{blk, T};
    callargs.insert(callargs.end(), args.begin(), args.end());
    return b.block(
        {b.assign(VarKind::Local, t, recv, p),
         b.assign(VarKind::Local, mv,
                  helper(ctx, "$mfind",
                         {helper(ctx, "$classof", {T}, p),
                          b.str_literal(std::string(1, kMethodPrefix) + name,
                                       p)},
                         p),
                  p),
         b.make_if(
             b.binary(BinOp::Ne, b.intrinsic(IntrinsicId::TypeOf, {M}, p),
                      b.str_literal("nil", p), p),
             b.call_value(M, callargs, p),
             helper(ctx, "$nometh", {T, b.str_literal(name, p)}, p), p)},
        p);
  }

  NodeId emit_classdef(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    const ClassInfo& ci = class_info.at(&a);
    const auto& methods = class_of.at(&a);
    std::vector<std::pair<NodeId, NodeId>> kvs{
        {b.str_literal(kNameKey, p), b.str_literal(ci.name, p)}};
    if (ci.base_var >= 0) {
      kvs.emplace_back(b.str_literal(kBaseKey, p),
                       read_var(ci.base_var, ctx, p));
    }
    for (const auto& [name, g] : methods) {
      // An attr_accessor method has no source body to walk -- new_fn made
      // it directly -- so it is built the same way, directly.
      const NodeId method = fns[static_cast<size_t>(g)].body == nullptr
                                ? emit_attr_method(g, name, p)
                                : emit_closure(g, ctx, p);
      kvs.emplace_back(
          b.str_literal(std::string(1, kMethodPrefix) + name, p), method);
    }
    const int32_t declv = decl_of.at(a.nodes[0].get());
    return write_var(declv, b.object_lit(kvs, p), ctx, p);
  }

  NodeId emit_attr_method(int32_t g, const std::string& name, SrcPos p) {
    Builder b(m);
    const bool setter = !name.empty() && name.back() == '=';
    const std::string field = setter ? name.substr(0, name.size() - 1) : name;
    Func f;
    f.name = field;
    f.num_params = setter ? 3 : 2;
    f.num_locals = f.num_params;
    f.local_names = setter ? std::vector<std::string>{"$blk", "self", "v"}
                           : std::vector<std::string>{"$blk", "self"};
    f.lenient_arity = true;
    const NodeId self = b.varref(VarKind::Local, 1, p);
    const NodeId key = b.str_literal(field, p);
    f.body = b.scope(
        0, f.num_params,
        b.make_return(
            setter ? b.set_index(self, key, b.varref(VarKind::Local, 2, p), p)
                   : b.index(self, key, p),
            p),
        p);
    m.funcs[static_cast<size_t>(fns[static_cast<size_t>(g)].index)] =
        std::move(f);
    // The closure has nothing to capture -- it only ever reads `self`.
    return b.make_closure(fns[static_cast<size_t>(g)].index, empty_cmap, p);
  }

  NodeId emit_case(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    const int32_t t = ctx.alloc_local("$case");
    const NodeId T = b.varref(VarKind::Local, t, p);
    NodeId chain;
    const Ast* elsepart = nullptr;
    std::vector<const Ast*> whens;
    for (size_t i = 1; i < a.nodes.size(); ++i) {
      if (a.nodes[i]->tag == "whenpart"_) {
        whens.push_back(a.nodes[i].get());
      } else if (a.nodes[i]->tag == "elsepart"_) {
        elsepart = a.nodes[i].get();
      }
    }
    chain = elsepart != nullptr ? emit_body(*elsepart->nodes[0], ctx)
                                : b.nil_literal(p);
    for (size_t k = whens.size(); k-- > 0;) {
      const Ast& w = *whens[k];
      NodeId cond = b.bool_literal(false, p);
      for (const auto& v : w.nodes[0]->nodes) {
        cond = b.make_if(cond, b.bool_literal(true, p),
                         helper(ctx, "$caseeq", {emit_expr(*v, ctx), T}, p),
                         p);
      }
      chain = b.make_if(cond, emit_body(*w.nodes[1], ctx), chain, p);
    }
    return b.block(
        {b.assign(VarKind::Local, t, emit_expr(*a.nodes[0], ctx), p), chain},
        p);
  }

  NodeId emit_body(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    std::vector<NodeId> out;
    for (const auto& s : a.nodes) out.push_back(emit_stmt(*s, ctx));
    return b.block(out, pos_of(a));
  }

  // -- Statements ---------------------------------------------------------
  NodeId emit_stmt(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    switch (a.tag) {
      case "body"_:
        return emit_body(a, ctx);
      case "defstmt"_:
        return write_var(decl_of.at(a.nodes[0].get()),
                         emit_closure(fn_of.at(&a), ctx, p), ctx, p);
      case "classdef"_:
        return emit_classdef(a, ctx);
      case "casestmt"_:
        return emit_case(a, ctx);
      case "attrstmt"_:
        fail(a, "attr_accessor is only supported inside a class body");
      case "ifstmt"_:
      case "unlessstmt"_:
        return emit_if(a, a.tag == "unlessstmt"_, ctx);
      case "whilestmt"_:
      case "untilstmt"_: {
        NodeId c = truthy(emit_expr(*a.nodes[0], ctx), ctx, p);
        if (a.tag == "untilstmt"_) {
          c = b.binary(BinOp::Eq, c, b.bool_literal(false, p), p);
        }
        return b.make_while(c, emit_body(*a.nodes[1], ctx), p);
      }
      case "modstmt"_: {
        const Ast& mod = *a.nodes[1];
        NodeId cond = truthy(emit_expr(*mod.nodes[0], ctx), ctx, p);
        if (mod.tag == "unlessmod"_ || mod.tag == "untilmod"_) {
          cond = b.binary(BinOp::Eq, cond, b.bool_literal(false, p), p);
        }
        if (mod.tag == "whilemod"_ || mod.tag == "untilmod"_) {
          return b.make_while(cond, emit_stmt(*a.nodes[0], ctx), p);
        }
        return b.make_if(cond, emit_stmt(*a.nodes[0], ctx), NodeId{}, p);
      }
      case "beginstmt"_:
        return emit_begin(a, ctx);
      case "assign"_:
        return emit_assign(a, ctx);
      case "exprstmt"_:
        return emit_expr(*a.nodes[0], ctx);
      default:
        return emit_expr(a, ctx);
    }
  }

  NodeId emit_if(const Ast& a, bool negate, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    NodeId cond = truthy(emit_expr(*a.nodes[0], ctx), ctx, p);
    if (negate) cond = b.binary(BinOp::Eq, cond, b.bool_literal(false, p), p);
    NodeId chain;
    const Ast* elsepart = nullptr;
    std::vector<const Ast*> elsifs;
    for (size_t i = 2; i < a.nodes.size(); ++i) {
      if (a.nodes[i]->tag == "elsifpart"_) {
        elsifs.push_back(a.nodes[i].get());
      } else if (a.nodes[i]->tag == "elsepart"_) {
        elsepart = a.nodes[i].get();
      }
    }
    chain = elsepart != nullptr ? emit_body(*elsepart->nodes[0], ctx)
                                : b.nil_literal(p);
    for (size_t k = elsifs.size(); k-- > 0;) {
      const Ast& e = *elsifs[k];
      chain = b.make_if(truthy(emit_expr(*e.nodes[0], ctx), ctx, p),
                        emit_body(*e.nodes[1], ctx), chain, p);
    }
    return b.make_if(cond, emit_body(*a.nodes[1], ctx), chain, p);
  }

  // `begin ... rescue => e ... ensure ... end`: `ensure` is a Defer, and
  // `rescue` a TryCatch whose handler re-throws what it did not claim --
  // every `rescue` here catches everything, since there is no exception
  // hierarchy.
  NodeId emit_begin(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    const Ast* rescue_ = nullptr;
    const Ast* ensure_ = nullptr;
    for (size_t i = 1; i < a.nodes.size(); ++i) {
      if (a.nodes[i]->tag == "rescuepart"_) {
        rescue_ = a.nodes[i].get();
      } else if (a.nodes[i]->tag == "ensurepart"_) {
        ensure_ = a.nodes[i].get();
      }
    }
    const int32_t slot = ctx.alloc_local("$exc");
    const NodeId E = b.varref(VarKind::Local, slot, p);
    std::vector<NodeId> out;
    if (ensure_ != nullptr) {
      out.push_back(b.make_defer(emit_closure(fn_of.at(ensure_), ctx, p), p));
    }
    const NodeId body = emit_body(*a.nodes[0], ctx);
    NodeId handler = b.make_throw(E, p);
    if (rescue_ != nullptr) {
      std::vector<NodeId> hs;
      const Ast* bodypart = nullptr;
      for (const auto& g : rescue_->nodes) {
        if (g->tag == "rescuevar"_) {
          hs.push_back(write_var(decl_of.at(g->nodes[0].get()), E, ctx, p));
        } else {
          bodypart = g.get();
        }
      }
      hs.push_back(emit_body(*bodypart, ctx));
      handler = b.block(hs, p);
    }
    out.push_back(b.make_try(slot, body, handler, p));
    const int32_t n = ctx.next_local;
    return b.scope(n, n, b.block(out, p), p);
  }

  NodeId emit_assign(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    const std::string op(a.nodes[1]->token);
    const Ast& target = *a.nodes[0];
    const auto combine = [&](NodeId cur) -> NodeId {
      const NodeId v = emit_expr(*a.nodes[2], ctx);
      if (op == "=") return v;
      const char* h = op == "+="   ? "$add"
                      : op == "-=" ? "$sub"
                      : op == "*=" ? "$mul"
                      : op == "/=" ? "$div"
                                   : "$mod";
      return helper(ctx, h, {cur, v}, p);
    };
    if (target.tag == "ident"_) {
      // ref_of first, not decl_of: a block *parameter* shadows an outer
      // name of the same spelling, and the pre-scan that gathers assigned
      // names cannot know that -- it ran before the block's parameters
      // existed. Resolution did know, so its answer is the one to use.
      const auto it = ref_of.find(&target);
      const int32_t v = it != ref_of.end() ? it->second : decl_of.at(&target);
      // An assignment *is* an expression in Ruby, and its value is what
      // was assigned -- which matters because a method (or a block)
      // answers its body's last value, so `lambda { n += 1 }` returns the
      // new n. Tag::Assign yields nothing, so the read back is the value.
      return b.block(
          {write_var(v, combine(read_var(v, ctx, p)), ctx, p),
           read_var(v, ctx, p)},
          p);
    }
    if (target.tag == "ivar"_) {
      const NodeId self = self_of(target, ctx, p);
      const NodeId key = b.str_literal(std::string(target.token), p);
      const NodeId cur =
          op == "=" ? b.nil_literal(p) : b.index(self, key, p);
      const NodeId v = b.set_index(self, key, combine(cur), p);
      return b.block({v, b.index(self, key, p)}, p);
    }
    if (target.tag == "postfix"_ &&
        target.nodes.back()->tag == "methodsfx"_ &&
        target.nodes.back()->nodes.size() == 1) {
      // `obj.attr = v` -- a call of the `attr=` method an attr_accessor (or
      // a program's own `def attr=`) generates. Only plain `=` is
      // supported here; `obj.attr += 1` would need to call the getter
      // first, which this front end does not do.
      if (op != "=") {
        fail(target, "only plain assignment is supported through a "
                     "method here");
      }
      const std::string name(target.nodes.back()->nodes[0]->token);
      const NodeId recv = emit_postfix(target, target.nodes.size() - 1, ctx);
      const NodeId v = emit_expr(*a.nodes[2], ctx);
      return emit_dynamic_call(recv, name + "=", {v}, b.nil_literal(p), ctx,
                               p);
    }
    if (target.tag != "postfix"_ ||
        target.nodes.back()->tag != "indexsfx"_) {
      fail(target, "cannot assign to this expression");
    }
    const int32_t tr = ctx.alloc_local("$recv");
    const int32_t tk = ctx.alloc_local("$key");
    const NodeId key = emit_expr(*target.nodes.back()->nodes[0], ctx);
    const NodeId recv = emit_postfix(target, target.nodes.size() - 1, ctx);
    const NodeId R = b.varref(VarKind::Local, tr, p);
    const NodeId K = b.varref(VarKind::Local, tk, p);
    const NodeId cur =
        op == "=" ? b.nil_literal(p) : helper(ctx, "$idx", {R, K}, p);
    return b.block({b.assign(VarKind::Local, tr, recv, p),
                    b.assign(VarKind::Local, tk, key, p),
                    helper(ctx, "$setidx", {R, K, combine(cur)}, p)},
                   p);
  }

  // -- Expressions --------------------------------------------------------
  NodeId emit_expr(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    switch (a.tag) {
      case "number"_:
        return b.literal(std::strtoll(std::string(a.token).c_str(), nullptr,
                                      10),
                         p);
      case "float"_:
        return b.double_literal(
            std::strtod(std::string(a.token).c_str(), nullptr), p);
      case "string"_:
        return b.str_literal(unescape(std::string(a.token)), p);
      case "istring"_: {
        NodeId acc = b.str_literal("", p);
        for (const auto& part : a.nodes) {
          const NodeId piece =
              part->tag == "itext"_
                  ? b.str_literal(unescape(std::string(part->token)), p)
                  : helper(ctx, "$str", {emit_expr(*part->nodes[0], ctx)}, p);
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
      case "identh"_:
      case "ident"_:
        return emit_ident(a, ctx);
      case "paren"_:
        return emit_expr(*a.nodes[0], ctx);
      case "selfexpr"_:
        return self_of(a, ctx, p);
      case "ivar"_:
        return b.index(self_of(a, ctx, p),
                       b.str_literal(std::string(a.token), p), p);
      case "symbol"_:
        // A symbol is a string here, same as examples/mini-scheme's own --
        // see README.md.
        return b.str_literal(std::string(a.token), p);
      case "ternary"_:
        return b.make_if(truthy(emit_expr(*a.nodes[0], ctx), ctx, p),
                         emit_expr(*a.nodes[1], ctx), emit_expr(*a.nodes[2], ctx),
                         p);
      case "supercall"_: {
        const FnInfo& fi = fns[static_cast<size_t>(ctx.fn)];
        if (!fi.is_method) fail(a, "'super' used outside a method");
        std::vector<NodeId> args;
        if (!a.nodes.empty()) {
          for (const auto& c : a.nodes[0]->nodes) args.push_back(emit_expr(*c, ctx));
        }
        if (fi.cls_base < 0) return b.nil_literal(p);  // no explicit superclass
        const NodeId base = read_var(fi.cls_base, ctx, p);
        const NodeId self = read_var(fi.params[1], ctx, p);
        const NodeId blk = read_var(fi.params[0], ctx, p);
        const int32_t mv = ctx.alloc_local("$super");
        const NodeId M = b.varref(VarKind::Local, mv, p);
        std::vector<NodeId> callargs{blk, self};
        callargs.insert(callargs.end(), args.begin(), args.end());
        return b.block(
            {b.assign(VarKind::Local, mv,
                      helper(ctx, "$mfind",
                             {base, b.str_literal(std::string(1, kMethodPrefix) +
                                                      fi.method_name,
                                                  p)},
                             p),
                      p),
             b.make_if(
                 b.binary(BinOp::Ne, b.intrinsic(IntrinsicId::TypeOf, {M}, p),
                          b.str_literal("nil", p), p),
                 b.call_value(M, callargs, p),
                 helper(ctx, "$nometh", {self, b.str_literal(fi.method_name, p)}, p),
                 p)},
            p);
      }
      case "notop"_:
        return b.binary(BinOp::Eq,
                        truthy(emit_expr(*a.nodes[0], ctx), ctx, p),
                        b.bool_literal(false, p), p);
      case "negexp"_:
        return b.unary(UnOp::Neg, emit_expr(*a.nodes[0], ctx), p);
      case "rangeexp"_:
        return helper(ctx, "$range",
                      {emit_expr(*a.nodes[0], ctx),
                       emit_expr(*a.nodes[1], ctx)},
                      p);
      case "returnstmt"_:
        return b.make_return(
            a.nodes.empty() ? b.nil_literal(p) : emit_expr(*a.nodes[0], ctx),
            p);
      case "breakstmt"_:
        return b.make_break(p);
      case "nextstmt"_:
        return b.make_continue(p);
      case "yieldexpr"_: {
        std::vector<NodeId> args{b.nil_literal(p)};
        if (!a.nodes.empty()) {
          for (const auto& c : a.nodes[0]->nodes) {
            args.push_back(emit_expr(*c, ctx));
          }
        }
        return b.call_value(block_of(ctx, p), args, p);
      }
      case "cmdcall"_:
        return emit_cmd(a, ctx);
      case "orexp"_:
      case "andexp"_: {
        const bool is_or = a.tag == "orexp"_;
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i < a.nodes.size(); ++i) {
          const int32_t t = ctx.alloc_local(is_or ? "$or" : "$and");
          const NodeId rhs = emit_expr(*a.nodes[i], ctx);
          const NodeId keep = b.varref(VarKind::Local, t, p);
          acc = b.block({b.assign(VarKind::Local, t, acc, p),
                         b.make_if(truthy(keep, ctx, p), is_or ? keep : rhs,
                                   is_or ? rhs : keep, p)},
                        p);
        }
        return acc;
      }
      case "cmpexp"_: {
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i + 1 < a.nodes.size(); i += 2) {
          const Ast& op = *a.nodes[i];
          const std::string t(op.token);
          const NodeId rhs = emit_expr(*a.nodes[i + 1], ctx);
          const SrcPos op_p = pos_of(op);
          if (t == "==") {
            acc = helper(ctx, "$eq", {acc, rhs}, op_p);
          } else if (t == "!=") {
            acc = b.binary(BinOp::Eq, helper(ctx, "$eq", {acc, rhs}, op_p),
                           b.bool_literal(false, p), op_p);
          } else {
            const BinOp o = t == "<"    ? BinOp::Lt
                            : t == "<=" ? BinOp::Le
                            : t == ">"  ? BinOp::Gt
                                        : BinOp::Ge;
            acc = b.binary(o, acc, rhs, op_p);
          }
        }
        return acc;
      }
      case "addexp"_:
      case "mulexp"_: {
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i + 1 < a.nodes.size(); i += 2) {
          const Ast& op = *a.nodes[i];
          const std::string t(op.token);
          const NodeId rhs = emit_expr(*a.nodes[i + 1], ctx);
          const char* h = t == "+"    ? "$add"
                          : t == "-"  ? "$sub"
                          : t == "*"  ? "$mul"
                          : t == "/"  ? "$div"
                          : t == "**" ? "$pow"
                                      : "$mod";
          acc = helper(ctx, h, {acc, rhs}, pos_of(op));
        }
        return acc;
      }
      case "arraylit"_: {
        std::vector<NodeId> items;
        for (const auto& c : a.nodes) items.push_back(emit_expr(*c, ctx));
        return b.array_lit(items, p);
      }
      case "hashlit"_: {
        const int32_t t = ctx.alloc_local("$hash");
        const NodeId T = b.varref(VarKind::Local, t, p);
        std::vector<NodeId> out{b.assign(
            VarKind::Local, t, b.intrinsic(IntrinsicId::MapNew, {}, p), p)};
        for (const auto& c : a.nodes) {
          out.push_back(b.set_index(T, emit_expr(*c->nodes[0], ctx),
                                    emit_expr(*c->nodes[1], ctx), p));
        }
        out.push_back(T);
        return b.block(out, p);
      }
      case "callexpr"_:
        return emit_callexpr(a, ctx);
      case "braceblock"_:
      case "doblock"_:
        return emit_closure(fn_of.at(&a), ctx, p);
      case "postfix"_:
        return emit_postfix(a, a.nodes.size(), ctx);
      default:
        fail(a, "cannot evaluate " + a.name);
    }
  }

  // A bare identifier is either a local's value or a no-argument call of a
  // method -- a `def` binds a name like an assignment does, so both are
  // variables and only the kind tells them apart.
  NodeId emit_ident(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    const auto it = ref_of.find(&a);
    if (it != ref_of.end()) {
      if (method_vars.count(it->second)) {
        return b.call_value(read_var(it->second, ctx, p),
                            {b.nil_literal(p)}, p);
      }
      return read_var(it->second, ctx, p);
    }
    const std::string n(a.token);
    if (n == "block_given?") {
      return b.binary(BinOp::Ne,
                      b.intrinsic(IntrinsicId::TypeOf, {block_of(ctx, p)}, p),
                      b.str_literal("nil", p), p);
    }
    fail(a, "undefined local variable or method '" + n + "'");
  }

  NodeId emit_cmd(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    const std::string name(a.nodes[0]->token);
    std::vector<NodeId> args;
    if (a.nodes.size() > 1) {
      for (const auto& c : a.nodes[1]->nodes) args.push_back(emit_expr(*c, ctx));
    }
    if (name == "puts") {
      return native("write",
                    {helper(ctx, "$puts", {b.array_lit(args, p)}, p)}, p);
    }
    if (name == "print") {
      NodeId acc = b.str_literal("", p);
      for (const NodeId v : args) {
        acc = b.binary(BinOp::Add, acc, helper(ctx, "$str", {v}, p), p);
      }
      return native("write", {acc}, p);
    }
    if (name == "p") {
      NodeId acc = b.str_literal("", p);
      for (const NodeId v : args) {
        acc = b.binary(
            BinOp::Add, acc,
            b.binary(BinOp::Add, helper(ctx, "$inspect", {v}, p),
                     b.str_literal("\n", p), p),
            p);
      }
      return b.block({native("write", {acc}, p),
                      args.empty() ? b.nil_literal(p) : args[0]},
                     p);
    }
    // raise
    if (args.empty()) {
      return helper(ctx, "$exc",
                    {b.str_literal("RuntimeError", p),
                     b.str_literal("unhandled exception", p)},
                    p);
    }
    return helper(ctx, "$exc",
                  {b.str_literal("RuntimeError", p), args[0]}, p);
  }

  // `f(args)` / `f(args) { }` / `f { }`, and the three library names that
  // make a callable: `lambda`, `proc` and `Proc.new`.
  NodeId emit_callexpr(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    const Ast& id = *a.nodes[0];
    const Ast* argsn = nullptr;
    const Ast* blockn = nullptr;
    for (size_t i = 1; i < a.nodes.size(); ++i) {
      if (a.nodes[i]->tag == "callargs"_) {
        argsn = a.nodes[i]->nodes.empty() ? nullptr : a.nodes[i]->nodes[0].get();
      } else {
        blockn = a.nodes[i].get();
      }
    }
    const std::string n(id.token);
    if (blockn != nullptr && (n == "lambda" || n == "proc")) {
      // The block itself is the value; `lambda` is the one that gets the
      // ArgCount check (emit_fn), which is Ruby's whole proc/lambda
      // distinction.
      const int32_t g = fn_of.at(blockn);
      fns[static_cast<size_t>(g)].strict = n == "lambda";
      return emit_closure(g, ctx, p);
    }
    NodeId blk = blockn != nullptr ? emit_closure(fn_of.at(blockn), ctx, p)
                                   : b.nil_literal(p);
    std::vector<NodeId> rest;
    if (argsn != nullptr) {
      for (const auto& c : argsn->nodes) {
        // `f(&blk)` hands a callable over as the block rather than as an
        // argument -- Ruby's own way of forwarding one.
        if (c->tag == "blockpass"_) {
          blk = emit_expr(*c->nodes[0], ctx);
          continue;
        }
        rest.push_back(emit_expr(*c, ctx));
      }
    }
    std::vector<NodeId> args{blk};
    args.insert(args.end(), rest.begin(), rest.end());
    const auto it = ref_of.find(&id);
    if (it == ref_of.end()) {
      fail(id, "undefined method '" + n + "'");
    }
    return b.call_value(read_var(it->second, ctx, p), args, p);
  }

  NodeId emit_postfix(const Ast& a, size_t limit, FnCtx& ctx) {
    Builder b(m);
    NodeId cur;
    const Ast& prim = *a.nodes[0];
    if (prim.tag == "ident"_ && !ref_of.count(&prim) &&
        std::string(prim.token) == "Proc") {
      // Proc.new { ... }
      if (limit < 2 || a.nodes[1]->tag != "methodsfx"_) {
        fail(prim, "Proc is only supported as Proc.new { ... }");
      }
      const Ast& sfx = *a.nodes[1];
      const Ast* blockn = nullptr;
      for (size_t i = 1; i < sfx.nodes.size(); ++i) {
        if (sfx.nodes[i]->tag != "callargs"_) blockn = sfx.nodes[i].get();
      }
      if (blockn == nullptr) fail(sfx, "Proc.new needs a block");
      cur = emit_closure(fn_of.at(blockn), ctx, pos_of(sfx));
      for (size_t i = 2; i < limit; ++i) {
        cur = emit_suffix(cur, *a.nodes[i], ctx);
      }
      return cur;
    }
    if (prim.tag == "ident"_ && ref_of.count(&prim) &&
        class_by_var.count(ref_of.at(&prim))) {
      // `ClassName.new(args)` -- the one shape a class name is a receiver
      // rather than an ordinary value, because it is the constructor.
      if (limit >= 2 && a.nodes[1]->tag == "methodsfx"_ &&
          std::string(a.nodes[1]->nodes[0]->token) == "new") {
        cur = emit_new(read_var(ref_of.at(&prim), ctx, pos_of(prim)),
                       *a.nodes[1], ctx, pos_of(prim));
        for (size_t i = 2; i < limit; ++i) {
          cur = emit_suffix(cur, *a.nodes[i], ctx);
        }
        return cur;
      }
    }
    cur = emit_expr(prim, ctx);
    for (size_t i = 1; i < limit; ++i) {
      cur = emit_suffix(cur, *a.nodes[i], ctx);
    }
    return cur;
  }

  // `ClassName.new(args)`: build the instance, then call `initialize` if
  // the class (or a base of it) defines one -- found by the same walk
  // every other method call goes through.
  NodeId emit_new(NodeId cls, const Ast& sfx, FnCtx& ctx, SrcPos p) {
    Builder b(m);
    std::vector<NodeId> args;
    for (size_t i = 1; i < sfx.nodes.size(); ++i) {
      if (sfx.nodes[i]->tag != "callargs"_ || sfx.nodes[i]->nodes.empty()) {
        continue;
      }
      for (const auto& x : sfx.nodes[i]->nodes[0]->nodes) {
        args.push_back(emit_expr(*x, ctx));
      }
    }
    const int32_t t = ctx.alloc_local("$new");
    const int32_t mv = ctx.alloc_local("$init");
    const NodeId T = b.varref(VarKind::Local, t, p);
    const NodeId M = b.varref(VarKind::Local, mv, p);
    std::vector<NodeId> callargs{b.nil_literal(p), T};
    callargs.insert(callargs.end(), args.begin(), args.end());
    return b.block(
        {b.assign(VarKind::Local, t,
                  b.object_lit({{b.str_literal(kClassKey, p), cls}}, p), p),
         b.assign(VarKind::Local, mv,
                  helper(ctx, "$mfind",
                         {cls, b.str_literal(
                                  std::string(1, kMethodPrefix) + "initialize",
                                  p)},
                         p),
                  p),
         b.make_if(
             b.binary(BinOp::Ne, b.intrinsic(IntrinsicId::TypeOf, {M}, p),
                      b.str_literal("nil", p), p),
             b.call_value(M, callargs, p), NodeId{}, p),
         T},
        p);
  }

  // A class instance's own method wins over everything below -- tried by
  // `$classof`/`$mfind` first, and only what that does not claim falls
  // through to `emit_suffix_builtin`, which is a plain library method,
  // resolved by name -- and Ruby's iterators take their block the same way
  // every other call in this front end does.
  NodeId emit_suffix(NodeId recv, const Ast& sfx, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(sfx);
    if (sfx.tag == "indexsfx"_) {
      return helper(ctx, "$idx", {recv, emit_expr(*sfx.nodes[0], ctx)}, p);
    }
    const std::string name(sfx.nodes[0]->token);
    std::vector<NodeId> args;
    NodeId blk = b.nil_literal(p);
    for (size_t i = 1; i < sfx.nodes.size(); ++i) {
      const Ast& c = *sfx.nodes[i];
      if (c.tag == "callargs"_) {
        if (!c.nodes.empty()) {
          for (const auto& x : c.nodes[0]->nodes) {
            if (x->tag == "blockpass"_) {
              blk = emit_expr(*x->nodes[0], ctx);
              continue;
            }
            args.push_back(emit_expr(*x, ctx));
          }
        }
      } else {
        blk = emit_closure(fn_of.at(&c), ctx, p);
      }
    }
    const std::function<NodeId(size_t)> a0 = [&](size_t k) {
      return k < args.size() ? args[k] : b.nil_literal(p);
    };
    const auto fallback = [&]() -> NodeId {
      return emit_suffix_builtin(recv, sfx, name, args, blk, a0, ctx, p);
    };
    const int32_t t = ctx.alloc_local("$self");
    const int32_t mv = ctx.alloc_local("$m");
    const NodeId T = b.varref(VarKind::Local, t, p);
    const NodeId M = b.varref(VarKind::Local, mv, p);
    std::vector<NodeId> callargs{blk, T};
    callargs.insert(callargs.end(), args.begin(), args.end());
    return b.block(
        {b.assign(VarKind::Local, t, recv, p),
         b.assign(VarKind::Local, mv,
                  helper(ctx, "$mfind",
                         {helper(ctx, "$classof", {T}, p),
                          b.str_literal(std::string(1, kMethodPrefix) + name,
                                       p)},
                         p),
                  p),
         b.make_if(
             b.binary(BinOp::Ne, b.intrinsic(IntrinsicId::TypeOf, {M}, p),
                      b.str_literal("nil", p), p),
             b.call_value(M, callargs, p), fallback(), p)},
        p);
  }

  NodeId emit_suffix_builtin(NodeId recv, const Ast& sfx,
                             const std::string& name,
                             const std::vector<NodeId>& args, NodeId blk,
                             const std::function<NodeId(size_t)>& a0,
                             FnCtx& ctx, SrcPos p) {
    Builder b(m);
    if (name == "length" || name == "size") {
      return helper(ctx, "$len", {recv}, p);
    }
    if (name == "push" || name == "<<") {
      return helper(ctx, "$push", {recv, a0(0)}, p);
    }
    if (name == "each") return helper(ctx, "$each", {recv, blk}, p);
    if (name == "map" || name == "collect") {
      return helper(ctx, "$map", {recv, blk}, p);
    }
    if (name == "select" || name == "filter") {
      return helper(ctx, "$select", {recv, blk}, p);
    }
    if (name == "inject" || name == "reduce") {
      if (args.empty()) return helper(ctx, "$inject1", {recv, blk}, p);
      return helper(ctx, "$inject", {recv, args[0], blk}, p);
    }
    if (name == "each_with_index") {
      return helper(ctx, "$eachwi", {recv, blk}, p);
    }
    if (name == "times") return helper(ctx, "$times", {recv, blk}, p);
    if (name == "keys") return helper(ctx, "$keys", {recv}, p);
    if (name == "sort") return helper(ctx, "$sort", {recv}, p);
    if (name == "reverse") return helper(ctx, "$reverse", {recv}, p);
    if (name == "include?") return helper(ctx, "$include", {recv, a0(0)}, p);
    if (name == "join") {
      return helper(ctx, "$join",
                    {recv, args.empty() ? b.str_literal("", p) : args[0]}, p);
    }
    if (name == "to_a") return helper(ctx, "$toa", {recv}, p);
    if (name == "first") return helper(ctx, "$first", {recv}, p);
    if (name == "last") return helper(ctx, "$last", {recv}, p);
    if (name == "inspect") return helper(ctx, "$inspect", {recv}, p);
    if (name == "to_s") return helper(ctx, "$str", {recv}, p);
    if (name == "message") {
      return helper(ctx, "$str", {recv}, p);
    }
    if (name == "class") return helper(ctx, "$clsname", {recv}, p);
    if (name == "is_a?" || name == "kind_of?") {
      return helper(ctx, "$isa", {recv, a0(0)}, p);
    }
    if (name == "instance_of?") return helper(ctx, "$isexact", {recv, a0(0)}, p);
    if (name == "upcase") return native("upcase", {recv}, p);
    if (name == "downcase") return native("downcase", {recv}, p);
    if (name == "arity") return helper(ctx, "$arity", {recv}, p);
    if (name == "call") {
      // A Proc's own call passes no block, so the block slot is nil and
      // the declared parameters line up behind it.
      std::vector<NodeId> ca{b.nil_literal(p)};
      ca.insert(ca.end(), args.begin(), args.end());
      return b.call_value(recv, ca, p);
    }
    if (name == "new") {
      fail(sfx, "'.new' only works on a class known at compile time, or as "
                "Proc.new { ... }");
    }
    return helper(ctx, "$nometh", {recv, b.str_literal(name, p)}, p);
  }

  // -- One function's body -------------------------------------------------
  void emit_fn(int32_t f) {
    const FnInfo& fi = fns[static_cast<size_t>(f)];
    // An attr_accessor method has no source body -- emit_classdef built it
    // directly, at the point the `class` statement itself was emitted.
    if (fi.body == nullptr) return;
    FnCtx ctx;
    ctx.fn = f;
    ctx.next_cell = static_cast<int32_t>(fi.cell_index.size());
    Builder b(m);
    const SrcPos p = pos_of(*fi.body);

    std::vector<NodeId> pre;
    for (const auto& [v, c] : fi.cell_index) {
      (void)v;
      pre.push_back(b.cell_fresh(c, p));
    }
    // After the freshes above, which would otherwise reset it, and before
    // the prologue below, which may call a helper itself.
    if (f == 0) fill_helpers(pre, p);
    for (const int32_t v : fi.params) {
      if (slot_of[static_cast<size_t>(v)] >= 0) continue;  // &blk aliases
      const int32_t s = ctx.alloc_local(vars[static_cast<size_t>(v)].name);
      slot_of[static_cast<size_t>(v)] = s;
      const auto it = fi.cell_index.find(v);
      if (it != fi.cell_index.end()) {
        pre.push_back(b.assign(VarKind::Cell, it->second,
                               b.varref(VarKind::Local, s, p), p));
      }
    }
    const int32_t nparams = ctx.next_local;
    // A default is computed in the method's own scope, after its earlier
    // parameters, so it may read them. Every call already supplies the
    // block as argument 0, so ArgCount lines up with a parameter's
    // position in `fi.params` exactly: parameter i was supplied iff
    // ArgCount > i. Only resolve_def populates `defaults`, parallel to
    // `params`; a block, a lambda, and the synthesized bodies have none.
    for (size_t i = 0; i < fi.defaults.size(); ++i) {
      if (fi.defaults[i] == nullptr) continue;
      pre.push_back(b.make_if(
          b.binary(BinOp::Le, b.intrinsic(IntrinsicId::ArgCount, {}, p),
                   b.literal(static_cast<int64_t>(i), p), p),
          write_var(fi.params[i], emit_expr(*fi.defaults[i], ctx), ctx, p),
          NodeId{}, p));
    }
    for (size_t v = 0; v < vars.size(); ++v) {
      if (vars[v].owner != f) continue;
      if (slot_of[v] >= 0) continue;
      if (fi.cell_index.count(static_cast<int32_t>(v))) continue;
      slot_of[v] = ctx.alloc_local(vars[v].name);
    }

    std::vector<NodeId> stmts;
    // A lambda is strict about its arity where a proc is not. That is
    // lenient_arity plus one ArgCount test -- the intrinsic's own comment
    // says it is "Only interesting under Func::lenient_arity, where the
    // two can differ", and this is the language where the difference has a
    // name.
    if (fi.strict) {
      stmts.push_back(b.make_if(
          b.binary(BinOp::Ne, b.intrinsic(IntrinsicId::ArgCount, {}, p),
                   b.literal(static_cast<int64_t>(fi.params.size()), p), p),
          helper(ctx, "$argerr", {}, p), NodeId{}, p));
    }
    const NodeId body = emit_body(*fi.body, ctx);

    std::vector<NodeId> head;
    head.insert(head.end(), pre.begin(), pre.end());
    head.insert(head.end(), stmts.begin(), stmts.end());
    // A Ruby method answers its body's last value.
    head.push_back(b.make_return(body, p));

    Func fn;
    fn.name = fi.name;
    fn.num_params = static_cast<int32_t>(fi.params.size());
    // funcs[0] is the entry point and is called with no arguments at all,
    // so its block parameter is a slot nothing fills -- reading it would
    // be an "uninitialized variable". At the top level there is never a
    // block, so nil is the whole of it.
    if (f == 0) {
      fn.num_params = 0;
      head.insert(head.begin(),
                  b.assign(VarKind::Local, slot_of[static_cast<size_t>(
                                               fi.params[0])],
                           b.nil_literal(p), p));
    }
    fn.num_locals = ctx.next_local;
    ctx.local_names.resize(static_cast<size_t>(ctx.next_local), "");
    fn.local_names = ctx.local_names;
    fn.num_cells = ctx.next_cell;
    fn.lenient_arity = true;  // Ruby's proc rule; a lambda checks above
    fn.num_captures = m.funcs[static_cast<size_t>(fi.index)].num_captures;
    fn.capture_names = m.funcs[static_cast<size_t>(fi.index)].capture_names;
    fn.body = b.scope(0, nparams, b.block(head, p), p);
    m.funcs[static_cast<size_t>(fi.index)] = std::move(fn);
  }

  Module build(const Ast& program) {
    const int32_t top = new_fn(-1, "main");
    fns[static_cast<size_t>(top)].body = &program;

    scopes.push_back({top, {}, false});
    fns[static_cast<size_t>(top)].params.push_back(
        declare_param("__block", top));
    fns[static_cast<size_t>(top)].defaults.push_back(nullptr);
    bind_names(program, top);
    for (const auto& s : program.nodes) resolve_stmt(*s, top);
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

    // One binding, owned by file scope and captured by every function: the
    // array of runtime-helper closures. Declared after resolution so no
    // source name can collide with it, and before number_captures so the
    // ordinary capture machinery threads it like any other free variable.
    helpers_var = static_cast<int32_t>(vars.size());
    vars.push_back({"$helpers", 0});
    for (size_t f = 1; f < fns.size(); ++f) fns[f].free.insert(helpers_var);

    number_captures();
    // A cell whether or not anything captured it: file scope reads it
    // itself, and a program with no nested function has no free set to put
    // it in.
    if (!fns[0].cell_index.count(helpers_var)) {
      fns[0].cell_index[helpers_var] =
          static_cast<int32_t>(fns[0].cell_index.size());
    }
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

// ==== The builtins that have to be host functions ==========================

bool nat_write(NativeCall& c) {
  const std::string& s = c.arg(0).as_str();
  coreir_rt_out_raw(s.data(), static_cast<int64_t>(s.size()));
  c.result = Value();
  return true;
}

bool nat_upper(NativeCall& c) {
  std::string s = c.arg(0).as_str();
  for (char& ch : s) ch = static_cast<char>(std::toupper(ch));
  c.result = Value::make_str(std::move(s));
  return true;
}

bool nat_lower(NativeCall& c) {
  std::string s = c.arg(0).as_str();
  for (char& ch : s) ch = static_cast<char>(std::tolower(ch));
  c.result = Value::make_str(std::move(s));
  return true;
}

}  // namespace

const std::vector<vm::NativeDef>& stdlib() {
  static const std::vector<vm::NativeDef> defs = {
      {"write", 1, nat_write, nullptr},
      {"upcase", 1, nat_upper, nullptr},
      {"downcase", 1, nat_lower, nullptr},
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

}  // namespace mini_ruby
