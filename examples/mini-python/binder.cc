// The front end for the last recipe in the top-level README that nothing
// else here reached: **Arbitrary-precision integers**.
//
// That section is worth quoting, because this file is its consequence:
// "`Value` holds an `int64` or a `double` and nothing wider, and this is a
// decision rather than a gap... A front end that does [need bignums]
// (Python's `int`, Ruby's `Integer`, Scheme's numbers) carries them itself,
// as a little-endian `Array` of `Int` limbs in base 10^9." Python's `int`
// is unbounded, so `python3` is an oracle for exactly that -- `2 ** 100`
// and `30!` are one line each and neither fits an int64.
//
// `test/test_bigint_recipe.cc` already writes addition, multiplication and
// decimal rendering in IR and checks them against `unsigned __int128`. What
// a *front end* has to add is everything around them: subtraction, ordering,
// the promotion rule that decides when a machine integer stops being enough,
// and the demotion rule that brings a result back down so that ordinary
// arithmetic does not pay for the ones that overflowed. Those five are the
// interesting part of this file.
//
// Two more things this front end is here for:
//
// **`with` is a Scope and a Defer.** A context manager's `__exit__` must
// run however the block is left -- falling through, `return`, `break`, or
// an unwinding exception -- which is Tag::Defer's contract verbatim.
//
// **Python's layout is not a PEG's.** See layout.h: the indentation is
// turned into explicit INDENT/DEDENT/NEWLINE bytes before the parser runs,
// because a PEG has no state to count columns with. Every other front end
// here is delimited by braces or by `end` and needs nothing of the sort.

#include "binder.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <peglib.h>

#include "grammar.h"
#include "layout.h"
#include "vmlib.h"

using namespace peg;
using namespace peg::udl;
using namespace coreir;

namespace mini_python {
namespace {

// A bignum's two fields, a class instance's back-pointer and an
// exception's name and message, all under keys whose first byte no Python
// source can produce -- the trick vmlib.h's own kDropKey uses.
constexpr char kBigKey[] = "\x01" "b";   // the limb array
constexpr char kSignKey[] = "\x01" "s";  // -1 or 1
constexpr char kClassKey[] = "\x01" "c";
constexpr char kExcKey[] = "\x01" "e";
constexpr char kMsgKey[] = "\x01" "m";
constexpr char kNameKey[] = "\x01" "n";
// Inheritance: a class table points at its base's table, carries the class
// value (the constructor closure) so that `isinstance` and `except` have an
// identity to compare, and names the builtin exception it is rooted at --
// `Exception` is not a class here, so a class that derives from one is
// marked rather than linked. See README.md.
constexpr char kBaseKey[] = "\x01" "p";
constexpr char kIdKey[] = "\x01" "k";
constexpr char kRootKey[] = "\x01" "x";
// A tuple is an array wrapped in an object, which is the only way to keep
// `type(t)` and `print(t)` honest: the IR has one sequence, and Python has
// two that print and compare differently.
constexpr char kTupKey[] = "\x01" "t";

// Base 10^9: a limb product plus two carries stays under 2^63, and
// rendering is nine decimal digits per limb with no division at all.
constexpr int64_t kBase = 1000000000;
// Below these, a machine integer cannot overflow: two addends under 2^62
// sum under 2^63, and two factors under 2^31 multiply under 2^62.
constexpr int64_t kAddSafe = int64_t{1} << 62;
constexpr int64_t kMulSafe = int64_t{1} << 31;

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

// The literal text between an f-string's interpolations: the ordinary
// escapes, plus `{{` and `}}` for the braces themselves.
std::string unescape_ftext(const std::string& tok) {
  std::string out;
  for (size_t i = 0; i < tok.size(); ++i) {
    if ((tok[i] == '{' || tok[i] == '}') && i + 1 < tok.size() &&
        tok[i + 1] == tok[i]) {
      out.push_back(tok[i]);
      ++i;
      continue;
    }
    if (tok[i] != '\\' || i + 1 >= tok.size()) {
      out.push_back(tok[i]);
      continue;
    }
    ++i;
    switch (tok[i]) {
      case 'n': out.push_back('\n'); break;
      case 't': out.push_back('\t'); break;
      case 'r': out.push_back('\r'); break;
      case '0': out.push_back('\0'); break;
      default: out.push_back(tok[i]); break;
    }
  }
  return out;
}

bool is_builtin(const std::string& n) {
  return n == "super" || n == "isinstance" || n == "repr" ||
         n == "tuple" || n == "enumerate" || n == "zip" || n == "sorted" ||
         n == "sum" || n == "min" || n == "max" || n == "next" ||
         n == "print" || n == "len" || n == "range" || n == "str" ||
         n == "int" || n == "float" || n == "list" || n == "bool" ||
         n == "abs" || n == "type" || n == "Exception" ||
         n == "ValueError" || n == "TypeError" || n == "RuntimeError" ||
         n == "KeyError" || n == "IndexError" ||
         n == "ZeroDivisionError" || n == "StopIteration" ||
         n == "AttributeError" || n == "NotImplementedError" ||
         n == "AssertionError";
}

// The builtins that can be handed around as values rather than called.
bool is_value_builtin(const std::string& n) {
  return n == "len" || n == "str" || n == "repr" || n == "int" ||
         n == "float" || n == "bool" || n == "list" || n == "type" ||
         n == "tuple" || n == "abs";
}

bool is_exception_name(const std::string& n) {
  return n == "Exception" || n == "ValueError" || n == "TypeError" ||
         n == "RuntimeError" || n == "KeyError" || n == "IndexError" ||
         n == "ZeroDivisionError" || n == "StopIteration" ||
         n == "AttributeError" || n == "NotImplementedError" ||
         n == "AssertionError";
}

// A name that a builtin method answers to on *some* type. It has to lose
// to a method of the program's own on any other, which is why the choice
// is a runtime test on the receiver rather than a bind-time decision --
// `get`, `count` and `index` are the most ordinary method names there are.
bool is_method_name(const std::string& n) {
  return n == "append" || n == "extend" || n == "pop" || n == "insert" ||
         n == "remove" || n == "index" || n == "count" || n == "reverse" ||
         n == "sort" || n == "keys" || n == "items" || n == "values" ||
         n == "get" || n == "update" || n == "split" || n == "strip" ||
         n == "lstrip" || n == "rstrip" || n == "replace" || n == "find" ||
         n == "startswith" || n == "endswith" || n == "upper" ||
         n == "lower" || n == "join";
}

struct VarInfo {
  std::string name;
  int32_t owner = 0;
};

// A parameter, in the four shapes `def` allows. A default is evaluated
// where the `def` *stands*, once, when the `def` runs -- not at each call --
// so its value lives in a cell of the enclosing function that the body
// captures. `def_var` is the synthetic binding that cell belongs to.
struct ParamInfo {
  enum Kind { Plain, Default, Rest, KwRest };
  Kind kind = Plain;
  int32_t var = -1;
  int32_t def_var = -1;
  std::string name;
  const Ast* id = nullptr;
  const Ast* def = nullptr;
};

struct FnInfo {
  int32_t parent = -1;
  int32_t index = -1;
  bool is_generator = false;
  bool is_synth = false;  // a `with`'s exit thunk: no body to walk
  std::string name = "?";
  std::set<int32_t> free;
  std::map<int32_t, int32_t> capture_index;
  std::map<int32_t, int32_t> cell_index;
  std::vector<ParamInfo> params;
  // `global x` and `nonlocal x` -- the two statements that say a name is
  // *not* this function's, which is the only way Python has to say it.
  std::set<std::string> globals;
  std::set<std::string> nonlocals;
  const Ast* body = nullptr;
};

struct FnCtx {
  int32_t fn = 0;
  int32_t next_local = 0;
  int32_t high_local = 0;
  int32_t next_cell = 0;
  std::vector<std::string> local_names;

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
  std::map<const Ast*, std::vector<std::pair<std::string, int32_t>>> class_of;
  // A class declaration binds two things: the name, which holds the
  // constructor closure, and a synthetic binding holding the method table.
  // The second is what `super()` reaches, through the same capture
  // machinery every other free variable uses -- so a method of a class
  // declared inside a function still finds its base.
  struct ClassInfo {
    int32_t table_var = -1;   // the synthetic binding
    int32_t base_var = -1;    // the base class's *table* binding
    std::string root;         // or the builtin exception it derives from
    bool is_exc = false;      // rooted at a builtin exception, at any depth
  };
  std::map<const Ast*, ClassInfo> class_info;
  std::map<int32_t, const Ast*> class_by_var;  // name binding -> its classdef
  std::set<int32_t> force_cells;
  std::vector<const Ast*> class_stack;
  std::vector<int32_t> slot_of;
  std::map<std::string, int32_t> rt;
  std::map<std::string, int32_t> builtin_fn;
  int32_t empty_cmap = -1;
  // The one closure per runtime helper, built once at file scope into an
  // array every function captures -- see build().
  int32_t helpers_var = -1;

  // ==== Pass A: scopes, declarations, captures =============================
  //
  // Python has no declaration form: an assignment *is* one, and its scope
  // is the whole function rather than the block it stands in. So a name
  // assigned anywhere in a function body belongs to that function, which
  // is why this pass pre-scans a body for assigned names before walking
  // it -- the opposite of every other front end here, where a declaration
  // is a statement with a keyword in front of it.

  struct ScopeA {
    int32_t fn;
    std::map<std::string, int32_t> names;
  };
  std::vector<ScopeA> scopes;

  static const Ast* fn_ident(const Ast& a) {
    for (const auto& c : a.nodes) {
      if (c->tag == "ident"_) return c.get();
    }
    return nullptr;
  }

  static const Ast* fn_params(const Ast& a) {
    for (const auto& c : a.nodes) {
      if (c->tag == "params"_) return c.get();
    }
    return nullptr;
  }

  int32_t declare(const std::string& name, int32_t fn) {
    auto it = scopes.back().names.find(name);
    if (it != scopes.back().names.end()) return it->second;
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
    // `global x` skips every enclosing function and lands at the module,
    // even when one of them has an `x` of its own.
    if (fns[static_cast<size_t>(fn)].globals.count(name)) {
      const auto it = scopes[0].names.find(name);
      if (it == scopes[0].names.end()) return std::nullopt;
      use_var(it->second, fn);
      return it->second;
    }
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

  // Every name a body assigns to, at any depth short of a nested `def` --
  // Python's function-wide binding rule, gathered before the body is
  // walked so that a name used above its first assignment still resolves
  // to the local it will become.
  void collect_bindings(const Ast& a, std::vector<const Ast*>& out) {
    if (a.tag == "listcomp"_ || a.tag == "dictcomp"_ ||
        a.tag == "gencomp"_ || a.tag == "bargen"_) {
      return;  // its targets are its own, as in Python 3
    }
    if (a.tag == "funcdef"_ || a.tag == "classdef"_ || a.tag == "lambda"_) {
      if (const Ast* id = fn_ident(a)) out.push_back(id);
      return;  // its own body binds its own names
    }
    if (a.tag == "assign"_) {
      for (const auto& t : a.nodes[0]->nodes) {
        if (t->tag == "ident"_) out.push_back(t.get());
      }
    }
    if (a.tag == "forstmt"_) {
      for (const auto& t : a.nodes[0]->nodes) out.push_back(t.get());
    }
    if (a.tag == "exceptpart"_ && a.nodes.size() > 2) {
      out.push_back(a.nodes[1].get());
    }
    if (a.tag == "withstmt"_ && a.nodes.size() > 2 &&
        a.nodes[1]->tag == "ident"_) {
      out.push_back(a.nodes[1].get());
    }
    for (const auto& c : a.nodes) collect_bindings(*c, out);
  }

  // `global x` and `nonlocal x` are gathered first, because they change
  // what the *rest* of the body's assignments mean: without one, an
  // assignment declares; with one, it does not.
  void collect_scope_decls(const Ast& a, int32_t fn) {
    if (a.tag == "funcdef"_ || a.tag == "classdef"_ || a.tag == "lambda"_ ||
        a.tag == "listcomp"_ || a.tag == "dictcomp"_ ||
        a.tag == "gencomp"_ || a.tag == "bargen"_) {
      return;
    }
    if (a.tag == "globalstmt"_) {
      for (const auto& c : a.nodes) {
        fns[static_cast<size_t>(fn)].globals.insert(std::string(c->token));
      }
    }
    if (a.tag == "nonlocalstmt"_) {
      for (const auto& c : a.nodes) {
        fns[static_cast<size_t>(fn)].nonlocals.insert(std::string(c->token));
      }
    }
    for (const auto& c : a.nodes) collect_scope_decls(*c, fn);
  }

  void bind_names(const Ast& body, int32_t fn) {
    for (const auto& s : body.nodes) collect_scope_decls(*s, fn);
    const FnInfo& fi = fns[static_cast<size_t>(fn)];
    // A `global` name is the module's, and an assignment to one in a
    // function is what creates it there -- so it is declared at the top.
    for (const std::string& g : fi.globals) {
      if (!scopes[0].names.count(g)) {
        const int32_t v = static_cast<int32_t>(vars.size());
        vars.push_back({g, scopes[0].fn});
        scopes[0].names[g] = v;
      }
    }
    std::vector<const Ast*> names;
    for (const auto& s : body.nodes) collect_bindings(*s, names);
    for (const Ast* id : names) {
      const std::string n(id->token);
      if (fns[static_cast<size_t>(fn)].globals.count(n) ||
          fns[static_cast<size_t>(fn)].nonlocals.count(n)) {
        continue;  // it belongs to an enclosing scope
      }
      decl_of[id] = declare(n, fn);
    }
  }

  int32_t resolve_fn(const Ast& node, const Ast& body, int32_t parent,
                     const std::string& name, const Ast* params,
                     bool expr_body) {
    const int32_t f = new_fn(parent, name);
    fns[static_cast<size_t>(f)].body = &body;
    fn_of[&node] = f;
    // A default belongs to the *enclosing* scope, so it is resolved before
    // this function's own scope is pushed -- and it reaches the body as a
    // capture rather than as anything the body computes.
    std::vector<ParamInfo> plist;
    if (params != nullptr) collect_params(*params, parent, f, plist);
    scopes.push_back({f, {}});
    for (ParamInfo& pi : plist) {
      pi.var = declare(pi.name, f);
      decl_of[pi.id] = pi.var;
    }
    fns[static_cast<size_t>(f)].params = plist;
    if (expr_body) {
      resolve_expr(body, f);
    } else {
      bind_names(body, f);
      for (const auto& s : body.nodes) resolve_stmt(*s, f);
    }
    scopes.pop_back();
    return f;
  }

  // A class body holds method definitions, and `pass`. Anything else -- a
  // class attribute, a nested class -- would need a namespace of its own to
  // be evaluated in, which this subset does not have.
  std::vector<const Ast*> class_body(const Ast& a) {
    std::vector<const Ast*> out;
    for (const auto& c : a.nodes.back()->nodes) {
      if (c->tag == "funcdef"_) {
        out.push_back(c.get());
        continue;
      }
      if (c->tag == "passstmt"_) continue;
      if (c->tag == "simpleline"_ && c->nodes.size() == 1 &&
          c->nodes[0]->tag == "passstmt"_) {
        continue;
      }
      fail(*c, "only method definitions are supported in a class body here");
    }
    return out;
  }

  void collect_params(const Ast& params, int32_t parent, int32_t f,
                      std::vector<ParamInfo>& out) {
    for (const auto& pn : params.nodes) {
      ParamInfo pi;
      pi.id = pn.get();
      if (pn->tag == "kwrest"_) {
        pi.kind = ParamInfo::KwRest;
        pi.id = pn->nodes[0].get();
      } else if (pn->tag == "rest"_) {
        pi.kind = ParamInfo::Rest;
        pi.id = pn->nodes[0].get();
      } else if (pn->tag == "defparam"_) {
        pi.kind = ParamInfo::Default;
        pi.id = pn->nodes[0].get();
        pi.def = pn->nodes[1].get();
        resolve_expr(*pi.def, parent);
        // A synthetic binding of the enclosing function, captured by this
        // one: that is what makes the default a def-time value.
        pi.def_var = static_cast<int32_t>(vars.size());
        vars.push_back({"$def." + std::string(pi.id->token), parent});
        fns[static_cast<size_t>(f)].free.insert(pi.def_var);
      }
      pi.name = std::string(pi.id->token);
      out.push_back(pi);
    }
  }

  void resolve_stmt(const Ast& a, int32_t fn) {
    switch (a.tag) {
      case "passstmt"_:
      case "breakstmt"_:
      case "contstmt"_:
        return;
      case "simpleline"_:
        for (const auto& c : a.nodes) resolve_stmt(*c, fn);
        return;
      case "block"_:
        for (const auto& c : a.nodes) resolve_stmt(*c, fn);
        return;
      case "funcdef"_:
        // A decorator is an expression of the enclosing scope, evaluated
        // where the `def` stands -- like a default, and for the same
        // reason: `@dec def f` is `f = dec(f)` and nothing more.
        if (a.nodes[0]->tag == "decorators"_) {
          for (const auto& d : a.nodes[0]->nodes) {
            resolve_expr(*d->nodes[0], fn);
          }
        }
        resolve_fn(a, *a.nodes.back(), fn,
                   std::string(fn_ident(a)->token), fn_params(a), false);
        return;
      case "classdef"_: {
        const std::string cname(a.nodes[0]->token);
        ClassInfo ci;
        // The base, if there is one. A builtin exception name is not a
        // class here, so deriving from one is recorded as a root rather
        // than linked to a table.
        for (const auto& c : a.nodes) {
          if (c->tag != "classargs"_) continue;
          if (c->nodes.size() > 1) {
            fail(*c->nodes[1], "multiple inheritance is not in this subset");
          }
          if (c->nodes.empty()) break;
          const std::string bname(c->nodes[0]->token);
          if (is_exception_name(bname)) {
            ci.root = bname;
            ci.is_exc = true;
            break;
          }
          const auto bv = resolve(bname, fn);
          if (!bv) fail(*c->nodes[0], "name '" + bname + "' is not defined");
          const auto bc = class_by_var.find(*bv);
          if (bc == class_by_var.end()) {
            fail(*c->nodes[0],
                 "a base class must be a class declared in this program");
          }
          ref_of[c->nodes[0].get()] = *bv;
          const ClassInfo& base = class_info.at(bc->second);
          ci.base_var = base.table_var;
          ci.is_exc = base.is_exc;
          use_var(ci.base_var, fn);
          break;
        }
        // The synthetic binding that holds the method table. It is always a
        // cell: the constructor captures it, and so does any method that
        // says `super()`.
        ci.table_var = static_cast<int32_t>(vars.size());
        vars.push_back({"$cls." + cname, fn});
        force_cells.insert(ci.table_var);
        class_info[&a] = ci;
        class_by_var[decl_of.at(a.nodes[0].get())] = &a;

        class_stack.push_back(&a);
        std::vector<std::pair<std::string, int32_t>> methods;
        for (const Ast* mth : class_body(a)) {
          if (mth->nodes[0]->tag == "decorators"_) {
            fail(*mth, "a decorated method is not in this subset");
          }
          methods.emplace_back(
              std::string(fn_ident(*mth)->token),
              resolve_fn(*mth, *mth->nodes.back(), fn,
                         cname + "." + std::string(fn_ident(*mth)->token),
                         fn_params(*mth), false));
        }
        class_stack.pop_back();
        class_of[&a] = methods;
        return;
      }
      case "ifstmt"_:
        for (const auto& c : a.nodes) {
          if (c->tag == "block"_) {
            resolve_stmt(*c, fn);
          } else if (c->tag == "elifpart"_) {
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
        resolve_expr(*a.nodes[0], fn);
        resolve_stmt(*a.nodes[1], fn);
        return;
      case "forstmt"_:
        resolve_expr(*a.nodes[1], fn);
        for (const auto& t : a.nodes[0]->nodes) {
          resolve(std::string(t->token), fn);
        }
        resolve_stmt(*a.nodes[2], fn);
        return;
      case "trystmt"_:
        for (const auto& c : a.nodes) {
          if (c->tag == "exceptpart"_) {
            // The caught name: a builtin exception resolves to nothing and
            // travels as a string, a class of the program's own to a value.
            // A bare `except:` names nothing at all.
            if (c->nodes[0]->tag == "ident"_) resolve_expr(*c->nodes[0], fn);
            for (const auto& g : c->nodes) {
              if (g->tag == "block"_) resolve_stmt(*g, fn);
            }
          } else if (c->tag == "finallypart"_) {
            // The finally block is a Defer's callable, so it is a function
            // of its own -- which is what makes everything it reads a
            // capture, and so a cell.
            resolve_fn(*c, *c->nodes[0], fn, "<finally>", nullptr, false);
          } else {
            resolve_stmt(*c, fn);
          }
        }
        return;
      case "withstmt"_: {
        resolve_expr(*a.nodes[0], fn);
        if (a.nodes.size() > 2) resolve(std::string(a.nodes[1]->token), fn);
        resolve_stmt(*a.nodes.back(), fn);
        // The exit thunk: synthesized, so it has no body to walk, but it
        // does capture the cell holding the context manager.
        const int32_t g = new_fn(fn, "<exit>");
        fns[static_cast<size_t>(g)].is_synth = true;
        fn_of[&a] = g;
        return;
      }
      case "returnstmt"_:
      case "raisestmt"_:
      case "exprstmt"_:
        for (const auto& c : a.nodes) resolve_expr(*c, fn);
        return;
      case "yieldone"_:
      case "yieldfrom"_:
        fns[static_cast<size_t>(fn)].is_generator = true;
        for (const auto& c : a.nodes) resolve_expr(*c, fn);
        return;
      case "globalstmt"_:
      case "nonlocalstmt"_:
        // Gathered before the body was walked; nothing to resolve.
        return;
      case "delstmt"_:
      case "assertstmt"_:
        for (const auto& c : a.nodes) resolve_expr(*c, fn);
        return;
      case "assign"_:
        for (const auto& c : a.nodes[2]->nodes) resolve_expr(*c, fn);
        for (const auto& t : a.nodes[0]->nodes) resolve_expr(*t, fn);
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
        if (auto v = resolve(n, fn)) {
          ref_of[&a] = *v;
          return;
        }
        if (n == "super") {
          // `super()` names the class the method was *declared* in, not the
          // one the instance turned out to be -- so it reaches the table
          // binding, as a capture like any other free variable.
          if (class_stack.empty()) {
            fail(a, "'super' outside a method");
          }
          const int32_t tv = class_info.at(class_stack.back()).table_var;
          ref_of[&a] = tv;
          use_var(tv, fn);
          return;
        }
        if (is_builtin(n)) return;
        fail(a, "name '" + n + "' is not defined");
      }
      case "lambda"_:
        resolve_fn(a, *a.nodes[1], fn, "<lambda>", a.nodes[0].get(), true);
        return;
      case "listcomp"_:
      case "dictcomp"_:
      case "gencomp"_:
      case "bargen"_:
        resolve_comp(a, fn);
        return;
      case "kwarg"_:
        // `f(b=3)`: the name is a parameter of whatever is called, not a
        // binding this scope has anything to say about.
        resolve_expr(*a.nodes[1], fn);
        return;
      default:
        for (const auto& c : a.nodes) {
          if (c->tag == "cmpop"_ || c->tag == "addop"_ ||
              c->tag == "mulop"_ || c->tag == "assignop"_ ||
              c->tag == "dotsfx"_) {
            continue;
          }
          resolve_expr(*c, fn);
        }
        return;
    }
  }

  // A comprehension is a function, exactly as in CPython -- which is the
  // reason its target does not leak into the enclosing scope, and the
  // reason the generator form can be lazy without anything else changing.
  void resolve_comp(const Ast& a, int32_t fn) {
    const int32_t f = new_fn(fn, "<comp>");
    fns[static_cast<size_t>(f)].body = &a;
    fns[static_cast<size_t>(f)].is_generator =
        a.tag == "gencomp"_ || a.tag == "bargen"_;
    fn_of[&a] = f;
    scopes.push_back({f, {}});
    for (const auto& c : a.nodes) {
      if (c->tag != "compfor"_) continue;
      for (const auto& t : c->nodes[0]->nodes) {
        decl_of[t.get()] = declare(std::string(t->token), f);
      }
    }
    for (const auto& c : a.nodes) {
      if (c->tag == "compfor"_) {
        resolve_expr(*c->nodes[1], f);
      } else if (c->tag == "compif"_) {
        resolve_expr(*c->nodes[0], f);
      } else {
        resolve_expr(*c, f);
      }
    }
    scopes.pop_back();
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
    // A class table is a cell whether or not a method captured it, because
    // its constructor always does -- and that closure is built by hand,
    // outside the free-set machinery above.
    for (const int32_t v : force_cells) {
      auto& own =
          fns[static_cast<size_t>(vars[static_cast<size_t>(v)].owner)]
              .cell_index;
      if (!own.count(v)) own[v] = static_cast<int32_t>(own.size());
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
        // The bignum recipe, and the promotion rule around it.
        "$isbig", "$abs", "$tolimbs", "$mkbig", "$biglimbs", "$bigsign",
        "$ucmp", "$uadd", "$usub", "$umul", "$bigadd", "$bigmul", "$bstr",
        "$tofloat", "$neg",
        // Arithmetic and comparison, dispatching on what the operands are.
        "$add", "$sub", "$mul", "$fdiv", "$idiv", "$mod", "$pow", "$cmp",
        "$eq", "$truthy",
        // Display.
        "$fstr", "$str", "$repr", "$liststr", "$dictstr",
        // Containers, attributes, iteration, exceptions.
        "$len", "$idx", "$setidx", "$slice", "$in", "$getattr", "$setattr",
        "$iter", "$iternext", "$exc", "$isexc", "$range", "$listadd",
        "$strmul", "$listmul", "$typename", "$type", "$join", "$dget", "$tolist",
        "$toint", "$intfail",
        // The calling convention this front end writes over the IR's.
        "$acons", "$aext", "$rest", "$kwhas", "$hasname", "$kwrest",
        "$kwcheck", "$kwmerge", "$missing", "$toomany",
        // Inheritance: a chain of ordinary objects, walked.
        "$clsfind", "$isname", "$isinstv", "$isinst", "$supercall",
        "$excinit", "$noinit", "$dunder",
        // Tuples, and the sequence builtins that hand them back.
        "$tuple", "$untup", "$istup", "$tupstr", "$unpack", "$items",
        "$values", "$enumerate", "$zip", "$sorted", "$sum", "$minmax",
        "$next",
        // The methods a program actually reaches for. Every one of them is
        // a scan, so every one of them is written here rather than handed
        // to the host -- which keeps `upper` and `lower` the only two, and
        // they are here because case is a Unicode table and not a scan.
        "$isspace", "$split", "$strip", "$replace", "$find", "$scount",
        "$startswith", "$endswith", "$alldigits", "$apop", "$ainsert",
        "$aremove",
        "$aindex", "$acount", "$areverse", "$asort", "$dpop", "$delitem",
        "$fmt",
        "$specerr",
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

  // -- The bignum recipe ---------------------------------------------------
  //
  // A little-endian Array of Int limbs in base 10^9, no leading zero limbs,
  // wrapped in an object so that TypeOf can tell one from a list. Zero and
  // anything that fits an int64 are *not* big: $mkbig demotes, which is
  // what keeps ordinary arithmetic on ordinary numbers.

  void rt_isbig() {
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(0)), "object"),
                r.ret(r.has(r.L(0), r.S(kBigKey)))));
    r.add(r.ret(r.Bo(false)));
    r.finish("$isbig", 1, 1, {"v"});
  }

  void rt_abs() {
    RT r(*this);
    r.add(r.iff(r.bin(BinOp::Lt, r.L(0), r.I(0)),
                r.ret(r.b.unary(UnOp::Neg, r.L(0), r.p))));
    r.add(r.ret(r.L(0)));
    r.finish("$abs", 1, 1, {"i"});
  }

  void rt_tolimbs() {
    RT r(*this);
    r.add(r.set(1, r.arr({})));
    r.add(r.set(2, r.L(0)));
    r.add(r.wh(r.bin(BinOp::Gt, r.L(2), r.I(0)),
               r.blk({r.push(r.L(1), r.bin(BinOp::Mod, r.L(2), r.I(kBase))),
                      r.set(2, r.bin(BinOp::Div, r.L(2), r.I(kBase)))})));
    r.add(r.ret(r.L(1)));
    r.finish("$tolimbs", 1, 3, {"n", "out", "i"});
  }

  // The demotion rule, and the reason ordinary arithmetic does not pay for
  // the operations that overflowed: two limbs is under 10^18, which fits an
  // int64 with room to spare, so anything that short comes back as one.
  void rt_mkbig() {
    RT r(*this);
    r.add(r.set(2, r.len(r.L(0))));
    r.add(r.wh(r.both(r.bin(BinOp::Gt, r.L(2), r.I(0)),
                      r.bin(BinOp::Eq,
                            r.idx(r.L(0), r.bin(BinOp::Sub, r.L(2), r.I(1))),
                            r.I(0))),
               r.set(2, r.bin(BinOp::Sub, r.L(2), r.I(1)))));
    r.add(r.iff(r.bin(BinOp::Lt, r.L(2), r.len(r.L(0))),
                r.set(0, r.in(IntrinsicId::ArraySlice,
                              {r.L(0), r.I(0), r.L(2)}))));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(2), r.I(0)), r.ret(r.I(0))));
    r.add(r.iff(
        r.bin(BinOp::Le, r.L(2), r.I(2)),
        r.blk({r.set(3, r.idx(r.L(0), r.I(0))),
               r.iff(r.bin(BinOp::Eq, r.L(2), r.I(2)),
                     r.set(3, r.bin(BinOp::Add, r.L(3),
                                    r.bin(BinOp::Mul, r.idx(r.L(0), r.I(1)),
                                          r.I(kBase))))),
               r.iff(r.bin(BinOp::Lt, r.L(1), r.I(0)),
                     r.set(3, r.b.unary(UnOp::Neg, r.L(3), r.p))),
               r.ret(r.L(3))})));
    r.add(r.ret(r.obj({{kBigKey, r.L(0)}, {kSignKey, r.L(1)}})));
    r.finish("$mkbig", 2, 4, {"limbs", "sign", "n", "v"});
  }

  void rt_biglimbs() {
    RT r(*this);
    r.add(r.iff(r.call("$isbig", {r.L(0)}), r.ret(r.idx(r.L(0), kBigKey))));
    r.add(r.ret(r.call("$tolimbs", {r.call("$abs", {r.L(0)})})));
    r.finish("$biglimbs", 1, 1, {"v"});
  }

  void rt_bigsign() {
    RT r(*this);
    r.add(r.iff(r.call("$isbig", {r.L(0)}), r.ret(r.idx(r.L(0), kSignKey))));
    r.add(r.iff(r.bin(BinOp::Lt, r.L(0), r.I(0)), r.ret(r.I(-1))));
    r.add(r.ret(r.I(1)));
    r.finish("$bigsign", 1, 1, {"v"});
  }

  void rt_ucmp() {
    RT r(*this);
    r.add(r.iff(r.bin(BinOp::Ne, r.len(r.L(0)), r.len(r.L(1))),
                r.ret(r.iff(r.bin(BinOp::Lt, r.len(r.L(0)), r.len(r.L(1))),
                            r.I(-1), r.I(1)))));
    r.add(r.set(2, r.bin(BinOp::Sub, r.len(r.L(0)), r.I(1))));
    r.add(r.wh(r.bin(BinOp::Ge, r.L(2), r.I(0)),
               r.blk({r.iff(r.bin(BinOp::Ne, r.idx(r.L(0), r.L(2)),
                                  r.idx(r.L(1), r.L(2))),
                            r.ret(r.iff(r.bin(BinOp::Lt,
                                              r.idx(r.L(0), r.L(2)),
                                              r.idx(r.L(1), r.L(2))),
                                        r.I(-1), r.I(1)))),
                      r.set(2, r.bin(BinOp::Sub, r.L(2), r.I(1)))})));
    r.add(r.ret(r.I(0)));
    r.finish("$ucmp", 2, 3, {"x", "y", "i"});
  }

  void rt_uadd() {
    RT r(*this);
    r.add(r.set(2, r.arr({})));
    r.add(r.set(3, r.I(0)));  // carry
    r.add(r.set(4, r.I(0)));  // i
    r.add(r.wh(
        r.either(r.bin(BinOp::Lt, r.L(4), r.len(r.L(0))),
                 r.either(r.bin(BinOp::Lt, r.L(4), r.len(r.L(1))),
                          r.bin(BinOp::Gt, r.L(3), r.I(0)))),
        r.blk({r.set(5, r.L(3)),
               r.iff(r.bin(BinOp::Lt, r.L(4), r.len(r.L(0))),
                     r.set(5, r.bin(BinOp::Add, r.L(5),
                                    r.idx(r.L(0), r.L(4))))),
               r.iff(r.bin(BinOp::Lt, r.L(4), r.len(r.L(1))),
                     r.set(5, r.bin(BinOp::Add, r.L(5),
                                    r.idx(r.L(1), r.L(4))))),
               r.push(r.L(2), r.bin(BinOp::Mod, r.L(5), r.I(kBase))),
               r.set(3, r.bin(BinOp::Div, r.L(5), r.I(kBase))),
               r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1)))})));
    r.add(r.ret(r.L(2)));
    r.finish("$uadd", 2, 6, {"x", "y", "out", "carry", "i", "s"});
  }

  // x >= y, which every caller checks with $ucmp first.
  void rt_usub() {
    RT r(*this);
    r.add(r.set(2, r.arr({})));
    r.add(r.set(3, r.I(0)));  // borrow
    r.add(r.set(4, r.I(0)));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(4), r.len(r.L(0))),
        r.blk({r.set(5, r.bin(BinOp::Sub, r.idx(r.L(0), r.L(4)), r.L(3))),
               r.iff(r.bin(BinOp::Lt, r.L(4), r.len(r.L(1))),
                     r.set(5, r.bin(BinOp::Sub, r.L(5),
                                    r.idx(r.L(1), r.L(4))))),
               r.iff(r.bin(BinOp::Lt, r.L(5), r.I(0)),
                     r.blk({r.set(5, r.bin(BinOp::Add, r.L(5), r.I(kBase))),
                            r.set(3, r.I(1))}),
                     r.set(3, r.I(0))),
               r.push(r.L(2), r.L(5)),
               r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1)))})));
    r.add(r.ret(r.L(2)));
    r.finish("$usub", 2, 6, {"x", "y", "out", "borrow", "i", "d"});
  }

  // Schoolbook, which is what base 10^9 was chosen for: one limb product
  // plus two carries is under 10^18 + 2*10^9, comfortably inside int64.
  void rt_umul() {
    RT r(*this);
    r.add(r.iff(r.either(r.bin(BinOp::Eq, r.len(r.L(0)), r.I(0)),
                         r.bin(BinOp::Eq, r.len(r.L(1)), r.I(0))),
                r.ret(r.arr({}))));
    r.add(r.set(2, r.arr({})));
    r.add(r.set(3, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(3),
                     r.bin(BinOp::Add, r.len(r.L(0)), r.len(r.L(1)))),
               r.blk({r.push(r.L(2), r.I(0)),
                      r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})));
    r.add(r.set(3, r.I(0)));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(3), r.len(r.L(0))),
        r.blk({
            r.set(5, r.I(0)),  // carry
            r.set(4, r.I(0)),  // j
            r.wh(r.bin(BinOp::Lt, r.L(4), r.len(r.L(1))),
                 r.blk({r.set(7, r.bin(BinOp::Add, r.L(3), r.L(4))),
                        r.set(6, r.bin(BinOp::Add,
                                       r.bin(BinOp::Add,
                                             r.idx(r.L(2), r.L(7)),
                                             r.bin(BinOp::Mul,
                                                   r.idx(r.L(0), r.L(3)),
                                                   r.idx(r.L(1), r.L(4)))),
                                       r.L(5))),
                        r.sidx(r.L(2), r.L(7),
                               r.bin(BinOp::Mod, r.L(6), r.I(kBase))),
                        r.set(5, r.bin(BinOp::Div, r.L(6), r.I(kBase))),
                        r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1)))})),
            r.set(7, r.bin(BinOp::Add, r.L(3), r.len(r.L(1)))),
            r.wh(r.bin(BinOp::Gt, r.L(5), r.I(0)),
                 r.blk({r.set(6, r.bin(BinOp::Add, r.idx(r.L(2), r.L(7)),
                                       r.L(5))),
                        r.sidx(r.L(2), r.L(7),
                               r.bin(BinOp::Mod, r.L(6), r.I(kBase))),
                        r.set(5, r.bin(BinOp::Div, r.L(6), r.I(kBase))),
                        r.set(7, r.bin(BinOp::Add, r.L(7), r.I(1)))})),
            r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})));
    r.add(r.ret(r.L(2)));
    r.finish("$umul", 2, 8, {"x", "y", "out", "i", "j", "carry", "cur", "k"});
  }

  void rt_bigadd() {
    RT r(*this);
    r.add(r.set(2, r.call("$bigsign", {r.L(0)})));
    r.add(r.set(3, r.call("$bigsign", {r.L(1)})));
    r.add(r.set(4, r.call("$biglimbs", {r.L(0)})));
    r.add(r.set(5, r.call("$biglimbs", {r.L(1)})));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(2), r.L(3)),
                r.ret(r.call("$mkbig",
                             {r.call("$uadd", {r.L(4), r.L(5)}), r.L(2)}))));
    r.add(r.set(6, r.call("$ucmp", {r.L(4), r.L(5)})));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(6), r.I(0)), r.ret(r.I(0))));
    r.add(r.iff(r.bin(BinOp::Gt, r.L(6), r.I(0)),
                r.ret(r.call("$mkbig",
                             {r.call("$usub", {r.L(4), r.L(5)}), r.L(2)}))));
    r.add(r.ret(r.call("$mkbig",
                       {r.call("$usub", {r.L(5), r.L(4)}), r.L(3)})));
    r.finish("$bigadd", 2, 7, {"a", "b", "sa", "sb", "xa", "xb", "c"});
  }

  void rt_bigmul() {
    RT r(*this);
    r.add(r.ret(r.call(
        "$mkbig",
        {r.call("$umul", {r.call("$biglimbs", {r.L(0)}),
                          r.call("$biglimbs", {r.L(1)})}),
         r.bin(BinOp::Mul, r.call("$bigsign", {r.L(0)}),
               r.call("$bigsign", {r.L(1)}))})));
    r.finish("$bigmul", 2, 2, {"a", "b"});
  }

  // Nine decimal digits per limb, zero-padded except for the top one --
  // the printing that base 10^9 makes free of division.
  void rt_bstr() {
    RT r(*this);
    r.add(r.iff(r.bin(BinOp::Eq, r.call("$isbig", {r.L(0)}), r.Bo(false)),
                r.ret(r.in(IntrinsicId::ToStr, {r.L(0)}))));
    r.add(r.set(1, r.idx(r.L(0), kBigKey)));
    r.add(r.set(2, r.bin(BinOp::Sub, r.len(r.L(1)), r.I(1))));
    r.add(r.set(3, r.in(IntrinsicId::ToStr, {r.idx(r.L(1), r.L(2))})));
    r.add(r.set(2, r.bin(BinOp::Sub, r.L(2), r.I(1))));
    r.add(r.wh(
        r.bin(BinOp::Ge, r.L(2), r.I(0)),
        r.blk({r.set(4, r.in(IntrinsicId::ToStr, {r.idx(r.L(1), r.L(2))})),
               r.wh(r.bin(BinOp::Lt, r.len(r.L(4)), r.I(9)),
                    r.set(4, r.bin(BinOp::Add, r.S("0"), r.L(4)))),
               r.set(3, r.bin(BinOp::Add, r.L(3), r.L(4))),
               r.set(2, r.bin(BinOp::Sub, r.L(2), r.I(1)))})));
    r.add(r.iff(r.bin(BinOp::Lt, r.idx(r.L(0), kSignKey), r.I(0)),
                r.set(3, r.bin(BinOp::Add, r.S("-"), r.L(3)))));
    r.add(r.ret(r.L(3)));
    r.finish("$bstr", 1, 5, {"v", "x", "i", "out", "t"});
  }

  void rt_tofloat() {
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(0)), "double"), r.ret(r.L(0))));
    r.add(r.iff(
        r.call("$isbig", {r.L(0)}),
        r.blk({r.set(1, r.idx(r.L(0), kBigKey)),
               r.set(2, r.D(0.0)),
               r.set(3, r.bin(BinOp::Sub, r.len(r.L(1)), r.I(1))),
               r.wh(r.bin(BinOp::Ge, r.L(3), r.I(0)),
                    r.blk({r.set(2, r.bin(BinOp::Add,
                                          r.bin(BinOp::Mul, r.L(2),
                                                r.D(1e9)),
                                          r.in(IntrinsicId::ToDouble,
                                               {r.idx(r.L(1), r.L(3))}))),
                           r.set(3, r.bin(BinOp::Sub, r.L(3), r.I(1)))})),
               r.iff(r.bin(BinOp::Lt, r.idx(r.L(0), kSignKey), r.I(0)),
                     r.set(2, r.b.unary(UnOp::Neg, r.L(2), r.p))),
               r.ret(r.L(2))})));
    r.add(r.ret(r.in(IntrinsicId::ToDouble, {r.L(0)})));
    r.finish("$tofloat", 1, 4, {"v", "x", "acc", "i"});
  }

  void rt_neg() {
    RT r(*this);
    r.add(r.iff(r.call("$isbig", {r.L(0)}),
                r.ret(r.obj({{kBigKey, r.idx(r.L(0), kBigKey)},
                             {kSignKey, r.b.unary(UnOp::Neg,
                                                  r.idx(r.L(0), kSignKey),
                                                  r.p)}}))));
    r.add(r.ret(r.b.unary(UnOp::Neg, r.L(0), r.p)));
    r.finish("$neg", 1, 1, {"v"});
  }

  // -- Arithmetic, dispatching on what the operands turn out to be --------
  //
  // The promotion rule lives here: two machine integers small enough that
  // the operation cannot leave int64 are done with BinOp, and everything
  // else goes to the limbs. That is what keeps a loop counter a loop
  // counter while `2 ** 100` is exact.

  NodeId int_pair(RT& r, NodeId ta, NodeId tb) {
    return r.both(r.is(ta, "int"), r.is(tb, "int"));
  }

  void rt_add() {
    RT r(*this);
    r.add(r.set(2, r.typ(r.L(0))));
    r.add(r.set(3, r.typ(r.L(1))));
    r.add(r.iff(r.both(r.is(r.L(2), "string"), r.is(r.L(3), "string")),
                r.ret(r.bin(BinOp::Add, r.L(0), r.L(1)))));
    r.add(r.iff(r.both(r.is(r.L(2), "array"), r.is(r.L(3), "array")),
                r.ret(r.call("$listadd", {r.L(0), r.L(1)}))));
    r.add(r.iff(r.both(r.call("$istup", {r.L(0)}), r.call("$istup", {r.L(1)})),
                r.ret(r.call("$tuple",
                             {r.call("$listadd", {r.idx(r.L(0), kTupKey),
                                                  r.idx(r.L(1), kTupKey)})}))));
    r.add(r.iff(r.either(r.is(r.L(2), "double"), r.is(r.L(3), "double")),
                r.ret(r.bin(BinOp::Add, r.call("$tofloat", {r.L(0)}),
                            r.call("$tofloat", {r.L(1)})))));
    r.add(r.iff(
        int_pair(r, r.L(2), r.L(3)),
        r.iff(r.both(r.bin(BinOp::Lt, r.call("$abs", {r.L(0)}), r.I(kAddSafe)),
                     r.bin(BinOp::Lt, r.call("$abs", {r.L(1)}),
                           r.I(kAddSafe))),
              r.ret(r.bin(BinOp::Add, r.L(0), r.L(1))))));
    r.add(r.ret(r.call("$bigadd", {r.L(0), r.L(1)})));
    r.finish("$add", 2, 4, {"a", "b", "ta", "tb"});
  }

  void rt_sub() {
    RT r(*this);
    r.add(r.ret(r.call("$add", {r.L(0), r.call("$neg", {r.L(1)})})));
    r.finish("$sub", 2, 2, {"a", "b"});
  }

  void rt_mul() {
    RT r(*this);
    r.add(r.set(2, r.typ(r.L(0))));
    r.add(r.set(3, r.typ(r.L(1))));
    r.add(r.iff(r.both(r.is(r.L(2), "string"), r.is(r.L(3), "int")),
                r.ret(r.call("$strmul", {r.L(0), r.L(1)}))));
    r.add(r.iff(r.both(r.is(r.L(2), "array"), r.is(r.L(3), "int")),
                r.ret(r.call("$listmul", {r.L(0), r.L(1)}))));
    r.add(r.iff(r.either(r.is(r.L(2), "double"), r.is(r.L(3), "double")),
                r.ret(r.bin(BinOp::Mul, r.call("$tofloat", {r.L(0)}),
                            r.call("$tofloat", {r.L(1)})))));
    r.add(r.iff(
        int_pair(r, r.L(2), r.L(3)),
        r.iff(r.both(r.bin(BinOp::Lt, r.call("$abs", {r.L(0)}), r.I(kMulSafe)),
                     r.bin(BinOp::Lt, r.call("$abs", {r.L(1)}),
                           r.I(kMulSafe))),
              r.ret(r.bin(BinOp::Mul, r.L(0), r.L(1))))));
    r.add(r.ret(r.call("$bigmul", {r.L(0), r.L(1)})));
    r.finish("$mul", 2, 4, {"a", "b", "ta", "tb"});
  }

  void rt_strmul() {
    RT r(*this);
    r.add(r.set(2, r.S("")));
    r.add(r.set(3, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(3), r.L(1)),
               r.blk({r.set(2, r.bin(BinOp::Add, r.L(2), r.L(0))),
                      r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})));
    r.add(r.ret(r.L(2)));
    r.finish("$strmul", 2, 4, {"s", "n", "out", "i"});
  }

  void rt_listmul() {
    RT r(*this);
    r.add(r.set(2, r.arr({})));
    r.add(r.set(3, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(3), r.L(1)),
               r.blk({r.set(4, r.I(0)),
                      r.wh(r.bin(BinOp::Lt, r.L(4), r.len(r.L(0))),
                           r.blk({r.push(r.L(2), r.idx(r.L(0), r.L(4))),
                                  r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1)))})),
                      r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})));
    r.add(r.ret(r.L(2)));
    r.finish("$listmul", 2, 5, {"a", "n", "out", "i", "j"});
  }

  void rt_listadd() {
    RT r(*this);
    r.add(r.set(2, r.arr({})));
    r.add(r.set(3, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(3), r.len(r.L(0))),
               r.blk({r.push(r.L(2), r.idx(r.L(0), r.L(3))),
                      r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})));
    r.add(r.set(3, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(3), r.len(r.L(1))),
               r.blk({r.push(r.L(2), r.idx(r.L(1), r.L(3))),
                      r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})));
    r.add(r.ret(r.L(2)));
    r.finish("$listadd", 2, 4, {"a", "b", "out", "i"});
  }

  void rt_fdiv() {
    RT r(*this);
    r.add(r.set(2, r.call("$tofloat", {r.L(1)})));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(2), r.D(0.0)),
                r.ret(r.call("$exc", {r.S("ZeroDivisionError"),
                                      r.S("division by zero")}))));
    r.add(r.ret(r.bin(BinOp::Div, r.call("$tofloat", {r.L(0)}), r.L(2))));
    r.finish("$fdiv", 2, 3, {"a", "b", "d"});
  }

  // `//` and `%` floor, like Lua's and unlike C's, so both correct the
  // truncating forms BinOp gives when the signs disagree.
  void rt_idiv() {
    RT r(*this);
    r.add(r.iff(r.bin(BinOp::Eq, r.L(1), r.I(0)),
                r.ret(r.call("$exc", {r.S("ZeroDivisionError"),
                                      r.S("integer division or modulo by "
                                          "zero")}))));
    r.add(r.set(2, r.bin(BinOp::Div, r.L(0), r.L(1))));
    r.add(r.iff(r.both(r.bin(BinOp::Ne, r.bin(BinOp::Mul, r.L(2), r.L(1)),
                             r.L(0)),
                       r.bin(BinOp::Lt, r.bin(BinOp::Mul, r.L(0), r.L(1)),
                             r.I(0))),
                r.set(2, r.bin(BinOp::Sub, r.L(2), r.I(1)))));
    r.add(r.ret(r.L(2)));
    r.finish("$idiv", 2, 3, {"a", "b", "q"});
  }

  void rt_mod() {
    RT r(*this);
    r.add(r.iff(r.bin(BinOp::Eq, r.L(1), r.I(0)),
                r.ret(r.call("$exc", {r.S("ZeroDivisionError"),
                                      r.S("integer division or modulo by "
                                          "zero")}))));
    r.add(r.set(2, r.bin(BinOp::Mod, r.L(0), r.L(1))));
    r.add(r.iff(r.both(r.bin(BinOp::Ne, r.L(2), r.I(0)),
                       r.bin(BinOp::Lt, r.bin(BinOp::Mul, r.L(2), r.L(1)),
                             r.I(0))),
                r.set(2, r.bin(BinOp::Add, r.L(2), r.L(1)))));
    r.add(r.ret(r.L(2)));
    r.finish("$mod", 2, 3, {"a", "b", "m"});
  }

  // Repeated squaring, over $mul -- so the promotion rule applies at every
  // step and `2 ** 100` is exact without anything here knowing it will be.
  void rt_pow() {
    RT r(*this);
    r.add(r.iff(r.either(r.is(r.typ(r.L(0)), "double"),
                         r.either(r.is(r.typ(r.L(1)), "double"),
                                  r.bin(BinOp::Lt, r.L(1), r.I(0)))),
                r.ret(r.in(IntrinsicId::Pow,
                           {r.call("$tofloat", {r.L(0)}),
                            r.call("$tofloat", {r.L(1)})}))));
    r.add(r.set(2, r.I(1)));
    r.add(r.set(3, r.L(0)));
    r.add(r.set(4, r.L(1)));
    r.add(r.wh(r.bin(BinOp::Gt, r.L(4), r.I(0)),
               r.blk({r.iff(r.bin(BinOp::Eq, r.bin(BinOp::Mod, r.L(4), r.I(2)),
                                  r.I(1)),
                            r.set(2, r.call("$mul", {r.L(2), r.L(3)}))),
                      r.set(3, r.call("$mul", {r.L(3), r.L(3)})),
                      r.set(4, r.bin(BinOp::Div, r.L(4), r.I(2)))})));
    r.add(r.ret(r.L(2)));
    r.finish("$pow", 2, 5, {"a", "b", "acc", "sq", "e"});
  }

  // -1, 0 or 1 -- what every ordering operator is built from.
  void rt_cmp() {
    RT r(*this);
    r.add(r.set(2, r.typ(r.L(0))));
    r.add(r.set(3, r.typ(r.L(1))));
    // A list or a tuple compares element by element, which is what makes
    // `sorted` work on a list of pairs.
    r.add(r.iff(
        r.both(r.either(r.is(r.L(2), "array"), r.call("$istup", {r.L(0)})),
               r.either(r.is(r.L(3), "array"), r.call("$istup", {r.L(1)}))),
        r.blk({r.set(9, r.call("$untup", {r.L(0)})),
               r.set(10, r.call("$untup", {r.L(1)})),
               r.set(11, r.I(0)),
               r.wh(r.both(r.bin(BinOp::Lt, r.L(11), r.len(r.L(9))),
                           r.bin(BinOp::Lt, r.L(11), r.len(r.L(10)))),
                    r.blk({r.set(12, r.call("$cmp",
                                            {r.idx(r.L(9), r.L(11)),
                                             r.idx(r.L(10), r.L(11))})),
                           r.iff(r.bin(BinOp::Ne, r.L(12), r.I(0)),
                                 r.ret(r.L(12))),
                           r.set(11, r.bin(BinOp::Add, r.L(11), r.I(1)))})),
               r.ret(r.iff(r.bin(BinOp::Lt, r.len(r.L(9)), r.len(r.L(10))),
                           r.I(-1),
                           r.iff(r.bin(BinOp::Gt, r.len(r.L(9)),
                                       r.len(r.L(10))),
                                 r.I(1), r.I(0))))})));
    r.add(r.iff(r.both(r.is(r.L(2), "string"), r.is(r.L(3), "string")),
                r.ret(r.iff(r.bin(BinOp::Lt, r.L(0), r.L(1)), r.I(-1),
                            r.iff(r.bin(BinOp::Gt, r.L(0), r.L(1)), r.I(1),
                                  r.I(0))))));
    // Two integers that both fit are compared directly; anything wider goes
    // through the limbs, sign first.
    r.add(r.iff(
        int_pair(r, r.L(2), r.L(3)),
        r.ret(r.iff(r.bin(BinOp::Lt, r.L(0), r.L(1)), r.I(-1),
                    r.iff(r.bin(BinOp::Gt, r.L(0), r.L(1)), r.I(1), r.I(0))))));
    r.add(r.iff(
        r.either(r.is(r.L(2), "double"), r.is(r.L(3), "double")),
        r.blk({r.set(4, r.call("$tofloat", {r.L(0)})),
               r.set(5, r.call("$tofloat", {r.L(1)})),
               r.ret(r.iff(r.bin(BinOp::Lt, r.L(4), r.L(5)), r.I(-1),
                           r.iff(r.bin(BinOp::Gt, r.L(4), r.L(5)), r.I(1),
                                 r.I(0))))})));
    r.add(r.set(6, r.call("$bigsign", {r.L(0)})));
    r.add(r.set(7, r.call("$bigsign", {r.L(1)})));
    r.add(r.iff(r.bin(BinOp::Ne, r.L(6), r.L(7)),
                r.ret(r.iff(r.bin(BinOp::Lt, r.L(6), r.L(7)), r.I(-1),
                            r.I(1)))));
    r.add(r.set(8, r.call("$ucmp", {r.call("$biglimbs", {r.L(0)}),
                                    r.call("$biglimbs", {r.L(1)})})));
    r.add(r.ret(r.bin(BinOp::Mul, r.L(8), r.L(6))));
    r.finish("$cmp", 2, 13,
             {"a", "b", "ta", "tb", "fa", "fb", "sa", "sb", "c", "xa", "xb",
              "i", "e"});
  }

  void rt_eq() {
    RT r(*this);
    r.add(r.set(2, r.typ(r.L(0))));
    r.add(r.set(3, r.typ(r.L(1))));
    const auto numeric = [&](NodeId t, NodeId v) {
      return r.either(r.is(t, "int"),
                      r.either(r.is(t, "double"), r.call("$isbig", {v})));
    };
    r.add(r.iff(r.both(numeric(r.L(2), r.L(0)), numeric(r.L(3), r.L(1))),
                r.ret(r.bin(BinOp::Eq, r.call("$cmp", {r.L(0), r.L(1)}),
                            r.I(0)))));
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
    r.add(r.iff(
        r.either(r.call("$istup", {r.L(0)}), r.call("$istup", {r.L(1)})),
        r.blk({r.iff(r.bin(BinOp::Eq,
                           r.both(r.call("$istup", {r.L(0)}),
                                  r.call("$istup", {r.L(1)})),
                           r.Bo(false)),
                     r.ret(r.Bo(false))),
               r.ret(r.call("$eq", {r.idx(r.L(0), kTupKey),
                                    r.idx(r.L(1), kTupKey)}))})));
    r.add(r.set(7, r.call("$dunder", {r.L(0), r.S("\x02__eq__")})));
    r.add(r.iff(r.isnt(r.typ(r.L(7)), "nil"),
                r.ret(r.call("$truthy",
                             {r.b.call_value(
                                 r.L(7), {r.arr({r.L(0), r.L(1)}), r.Nil()},
                                 r.p)}))));
    r.add(r.ret(r.in(IntrinsicId::Same, {r.L(0), r.L(1)})));
    r.finish("$eq", 2, 8, {"a", "b", "ta", "tb", "i", "ks", "k", "f"});
  }

  // Python's truthiness: 0, "", [], {} and None are false. Value::truthy()
  // agrees about 0 and None and disagrees about the other three, which its
  // own comment names as the reason it will not decide.
  void rt_truthy() {
    RT r(*this);
    r.add(r.set(1, r.typ(r.L(0))));
    r.add(r.iff(r.is(r.L(1), "nil"), r.ret(r.Bo(false))));
    r.add(r.iff(r.is(r.L(1), "bool"), r.ret(r.L(0))));
    r.add(r.iff(r.is(r.L(1), "int"), r.ret(r.bin(BinOp::Ne, r.L(0), r.I(0)))));
    r.add(r.iff(r.is(r.L(1), "double"),
                r.ret(r.bin(BinOp::Ne, r.L(0), r.D(0.0)))));
    r.add(r.iff(r.either(r.is(r.L(1), "string"),
                         r.either(r.is(r.L(1), "array"), r.is(r.L(1), "map"))),
                r.ret(r.bin(BinOp::Gt, r.len(r.L(0)), r.I(0)))));
    r.add(r.ret(r.Bo(true)));
    r.finish("$truthy", 1, 2, {"v", "t"});
  }

  // -- Display -------------------------------------------------------------
  //
  // Python's float repr is shortest-round-trip with a ".0" forced onto a
  // whole value -- which is to_display's output plus the one rule
  // to_display's comment says a front end should add for itself.
  void rt_fstr() {
    const double lim = 9007199254740992.0;
    RT r(*this);
    r.add(r.iff(r.bin(BinOp::Ne, r.L(0), r.L(0)), r.ret(r.S("nan"))));
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

  void rt_str() {
    RT r(*this);
    r.add(r.set(1, r.typ(r.L(0))));
    r.add(r.iff(r.is(r.L(1), "string"), r.ret(r.L(0))));
    r.add(r.iff(r.is(r.L(1), "nil"), r.ret(r.S("None"))));
    r.add(r.iff(r.is(r.L(1), "bool"),
                r.ret(r.iff(r.L(0), r.S("True"), r.S("False")))));
    r.add(r.iff(r.is(r.L(1), "int"), r.ret(r.in(IntrinsicId::ToStr, {r.L(0)}))));
    r.add(r.iff(r.is(r.L(1), "double"), r.ret(r.call("$fstr", {r.L(0)}))));
    r.add(r.iff(r.is(r.L(1), "array"), r.ret(r.call("$liststr", {r.L(0)}))));
    r.add(r.iff(r.is(r.L(1), "map"), r.ret(r.call("$dictstr", {r.L(0)}))));
    r.add(r.iff(r.is(r.L(1), "function"), r.ret(r.S("<function>"))));
    r.add(r.iff(r.is(r.L(1), "generator"), r.ret(r.S("<generator>"))));
    r.add(r.iff(r.call("$isbig", {r.L(0)}), r.ret(r.call("$bstr", {r.L(0)}))));
    r.add(r.iff(r.has(r.L(0), r.S(kTupKey)),
                r.ret(r.call("$tupstr", {r.idx(r.L(0), kTupKey)}))));
    r.add(r.iff(r.has(r.L(0), r.S(kExcKey)),
                r.ret(r.call("$str", {r.idx(r.L(0), kMsgKey)}))));
    r.add(r.set(2, r.call("$dunder", {r.L(0), r.S("\x02__str__")})));
    r.add(r.iff(r.isnt(r.typ(r.L(2)), "nil"),
                r.ret(r.call("$str", {r.b.call_value(
                                 r.L(2), {r.arr({r.L(0)}), r.Nil()}, r.p)}))));
    // A class of the program's own that derives from a builtin exception
    // displays as its message, the way the builtin ones do.
    r.add(r.iff(r.has(r.L(0), r.S(kMsgKey)),
                r.ret(r.call("$str", {r.idx(r.L(0), kMsgKey)}))));
    r.add(r.iff(r.has(r.L(0), r.S("__name__")),
                r.ret(r.bin(BinOp::Add,
                            r.bin(BinOp::Add, r.S("<class '"),
                                  r.idx(r.L(0), r.S("__name__"))),
                            r.S("'>")))));
    r.add(r.iff(r.has(r.L(0), r.S(kClassKey)),
                r.ret(r.bin(BinOp::Add,
                            r.bin(BinOp::Add, r.S("<"),
                                  r.idx(r.idx(r.L(0), kClassKey), kNameKey)),
                            r.S(" object>")))));
    r.add(r.ret(r.S("<object>")));
    r.finish("$str", 1, 3, {"v", "t", "f"});
  }

  void rt_repr() {
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(0)), "string"),
                r.ret(r.bin(BinOp::Add,
                            r.bin(BinOp::Add, r.S("'"), r.L(0)), r.S("'")))));
    r.add(r.set(1, r.call("$dunder", {r.L(0), r.S("\x02__repr__")})));
    r.add(r.iff(r.isnt(r.typ(r.L(1)), "nil"),
                r.ret(r.call("$str", {r.b.call_value(
                                 r.L(1), {r.arr({r.L(0)}), r.Nil()}, r.p)}))));
    r.add(r.ret(r.call("$str", {r.L(0)})));
    r.finish("$repr", 1, 2, {"v", "f"});
  }

  void rt_liststr() {
    RT r(*this);
    r.add(r.set(1, r.S("[")));
    r.add(r.set(2, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(2), r.len(r.L(0))),
               r.blk({r.iff(r.bin(BinOp::Gt, r.L(2), r.I(0)),
                            r.set(1, r.bin(BinOp::Add, r.L(1), r.S(", ")))),
                      r.set(1, r.bin(BinOp::Add, r.L(1),
                                     r.call("$repr", {r.idx(r.L(0), r.L(2))}))),
                      r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1)))})));
    r.add(r.ret(r.bin(BinOp::Add, r.L(1), r.S("]"))));
    r.finish("$liststr", 1, 3, {"a", "out", "i"});
  }

  void rt_dictstr() {
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
                                    r.bin(BinOp::Add,
                                          r.call("$repr", {r.L(4)}),
                                          r.S(": ")),
                                    r.call("$repr", {r.idx(r.L(0), r.L(4))})))),
               r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1)))})));
    r.add(r.ret(r.bin(BinOp::Add, r.L(1), r.S("}"))));
    r.finish("$dictstr", 1, 5, {"d", "out", "i", "ks", "k"});
  }

  // -- Containers, attributes, iteration, exceptions ----------------------

  void rt_exc() {
    RT r(*this);
    r.add(r.b.make_throw(r.obj({{kExcKey, r.L(0)}, {kMsgKey, r.L(1)}}), r.p));
    r.finish("$exc", 2, 2, {"name", "msg"});
  }

  // `except Exception` catches everything, which is close enough to
  // Python's hierarchy for a subset with no inheritance.
  void rt_isexc() {
    RT r(*this);
    // A builtin exception travels as its name; a class of the program's own
    // travels as its value, and then the test is the identity walk.
    r.add(r.iff(r.isnt(r.typ(r.L(1)), "string"),
                r.ret(r.call("$isinstv", {r.L(0), r.L(1)}))));
    r.add(r.iff(r.is(r.L(1), "Exception"), r.ret(r.Bo(true))));
    r.add(r.ret(r.call("$isname", {r.L(0), r.L(1)})));
    r.finish("$isexc", 2, 2, {"e", "name"});
  }

  void rt_len() {
    RT r(*this);
    r.add(r.set(0, r.call("$untup", {r.L(0)})));
    r.add(r.set(1, r.typ(r.L(0))));
    r.add(r.iff(r.either(r.is(r.L(1), "string"),
                         r.either(r.is(r.L(1), "array"), r.is(r.L(1), "map"))),
                r.ret(r.len(r.L(0)))));
    r.add(r.ret(r.call("$exc", {r.S("TypeError"),
                                r.S("object has no len()")})));
    r.finish("$len", 1, 2, {"v", "t"});
  }

  // A negative index counts from the end, and out of range raises rather
  // than trapping -- Python's rules, written here because index_error()'s
  // own comment says a language that wants them normalizes first.
  void rt_idx() {
    RT r(*this);
    r.add(r.set(0, r.call("$untup", {r.L(0)})));
    r.add(r.set(2, r.typ(r.L(0))));
    r.add(r.iff(
        r.either(r.is(r.L(2), "array"), r.is(r.L(2), "string")),
        r.blk({r.set(3, r.L(1)),
               r.iff(r.bin(BinOp::Lt, r.L(3), r.I(0)),
                     r.set(3, r.bin(BinOp::Add, r.L(3), r.len(r.L(0))))),
               r.iff(r.either(r.bin(BinOp::Lt, r.L(3), r.I(0)),
                              r.bin(BinOp::Ge, r.L(3), r.len(r.L(0)))),
                     r.ret(r.call(
                         "$exc",
                         {r.S("IndexError"),
                          r.iff(r.is(r.L(2), "array"),
                                r.S("list index out of range"),
                                r.S("string index out of range"))}))),
               r.ret(r.idx(r.L(0), r.L(3)))})));
    r.add(r.iff(r.is(r.L(2), "map"),
                r.blk({r.iff(r.has(r.L(0), r.L(1)),
                             r.ret(r.idx(r.L(0), r.L(1)))),
                       r.ret(r.call("$exc", {r.S("KeyError"),
                                             r.call("$repr", {r.L(1)})}))})));
    r.add(r.ret(r.call("$exc", {r.S("TypeError"),
                                r.S("object is not subscriptable")})));
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
               r.iff(r.either(r.bin(BinOp::Lt, r.L(4), r.I(0)),
                              r.bin(BinOp::Ge, r.L(4), r.len(r.L(0)))),
                     r.ret(r.call("$exc",
                                  {r.S("IndexError"),
                                   r.S("list assignment index out of "
                                       "range")}))),
               r.sidx(r.L(0), r.L(4), r.L(2)), r.ret(r.L(2))})));
    r.add(r.iff(r.is(r.L(3), "map"),
                r.blk({r.sidx(r.L(0), r.L(1), r.L(2)), r.ret(r.L(2))})));
    r.add(r.ret(r.call("$exc", {r.S("TypeError"),
                                r.S("object does not support item "
                                    "assignment")})));
    r.finish("$setidx", 3, 5, {"v", "k", "val", "t", "i"});
  }

  // Python's slice: absent ends default, a negative one counts from the
  // end, and both are clamped rather than refused.
  void rt_slice() {
    RT r(*this);
    auto norm = [&](int32_t out, int32_t arg, NodeId dflt) {
      return r.iff(r.is(r.typ(r.L(arg)), "nil"), r.set(out, dflt),
                   r.blk({r.set(out, r.L(arg)),
                          r.iff(r.bin(BinOp::Lt, r.L(out), r.I(0)),
                                r.set(out, r.bin(BinOp::Add, r.L(3),
                                                 r.L(out)))),
                          r.iff(r.bin(BinOp::Lt, r.L(out), r.I(0)),
                                r.set(out, r.I(0))),
                          r.iff(r.bin(BinOp::Gt, r.L(out), r.L(3)),
                                r.set(out, r.L(3)))}));
    };
    // A slice of a tuple is a tuple.
    r.add(r.iff(r.call("$istup", {r.L(0)}),
                r.ret(r.call("$tuple",
                             {r.call("$slice", {r.idx(r.L(0), kTupKey),
                                                r.L(1), r.L(2)})}))));
    r.add(r.set(3, r.len(r.L(0))));
    r.add(norm(4, 1, r.I(0)));
    r.add(norm(5, 2, r.L(3)));
    r.add(r.iff(r.bin(BinOp::Lt, r.L(5), r.L(4)), r.set(5, r.L(4))));
    r.add(r.iff(r.is(r.typ(r.L(0)), "string"),
                r.ret(r.in(IntrinsicId::StrSlice, {r.L(0), r.L(4), r.L(5)})),
                r.ret(r.in(IntrinsicId::ArraySlice,
                           {r.L(0), r.L(4), r.L(5)}))));
    r.finish("$slice", 3, 6, {"v", "i", "j", "n", "a", "b"});
  }

  void rt_in() {
    RT r(*this);
    r.add(r.set(1, r.call("$untup", {r.L(1)})));
    r.add(r.set(2, r.typ(r.L(1))));
    r.add(r.iff(r.is(r.L(2), "map"), r.ret(r.has(r.L(1), r.L(0)))));
    // `"e" in "hello"` is a substring test, not a membership one.
    r.add(r.iff(
        r.is(r.L(2), "string"),
        r.blk({r.set(3, r.I(0)),
               r.wh(r.bin(BinOp::Le, r.bin(BinOp::Add, r.L(3), r.len(r.L(0))),
                          r.len(r.L(1))),
                    r.blk({r.iff(r.bin(BinOp::Eq,
                                       r.in(IntrinsicId::StrSlice,
                                            {r.L(1), r.L(3),
                                             r.bin(BinOp::Add, r.L(3),
                                                   r.len(r.L(0)))}),
                                       r.L(0)),
                                 r.ret(r.Bo(true))),
                           r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})),
               r.ret(r.Bo(false))})));
    r.add(r.iff(
        r.is(r.L(2), "array"),
        r.blk({r.set(3, r.I(0)),
               r.wh(r.bin(BinOp::Lt, r.L(3), r.len(r.L(1))),
                    r.blk({r.iff(r.call("$eq", {r.L(0), r.idx(r.L(1), r.L(3))}),
                                 r.ret(r.Bo(true))),
                           r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})),
               r.ret(r.Bo(false))})));
    r.add(r.ret(r.Bo(false)));
    r.finish("$in", 2, 4, {"needle", "hay", "t", "i"});
  }

  void rt_getattr() {
    RT r(*this);
    r.add(r.iff(
        r.is(r.typ(r.L(0)), "object"),
        r.blk({r.iff(r.has(r.L(0), r.L(1)), r.ret(r.idx(r.L(0), r.L(1)))),
               r.iff(r.has(r.L(0), r.S(kClassKey)),
                     r.blk({r.set(3, r.bin(BinOp::Add, r.S("\x02"), r.L(1))),
                            r.set(2, r.call("$clsfind",
                                            {r.idx(r.L(0), kClassKey),
                                             r.L(3)})),
                            r.iff(r.isnt(r.typ(r.L(2)), "nil"),
                                  r.ret(r.L(2)))}))})));
    r.add(r.ret(r.call("$exc",
                       {r.S("AttributeError"),
                        r.bin(BinOp::Add, r.S("no attribute "), r.L(1))})));
    r.finish("$getattr", 2, 4, {"o", "name", "cls", "key"});
  }

  void rt_setattr() {
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(0)), "object"),
                r.blk({r.sidx(r.L(0), r.L(1), r.L(2)), r.ret(r.L(2))})));
    r.add(r.ret(r.call("$exc", {r.S("AttributeError"),
                                r.S("cannot set attribute")})));
    r.finish("$setattr", 3, 3, {"o", "name", "val"});
  }

  void rt_iter() {
    RT r(*this);
    r.add(r.set(0, r.call("$untup", {r.L(0)})));
    r.add(r.set(1, r.typ(r.L(0))));
    r.add(r.iff(r.is(r.L(1), "generator"),
                r.ret(r.obj({{"k", r.S("g")}, {"v", r.L(0)}}))));
    r.add(r.iff(r.is(r.L(1), "map"),
                r.ret(r.obj({{"k", r.S("a")},
                             {"v", r.in(IntrinsicId::ObjectKeys, {r.L(0)})},
                             {"i", r.I(0)}}))));
    r.add(r.iff(r.either(r.is(r.L(1), "array"), r.is(r.L(1), "string")),
                r.ret(r.obj({{"k", r.S("a")},
                             {"v", r.L(0)},
                             {"i", r.I(0)}}))));
    r.add(r.ret(r.call("$exc", {r.S("TypeError"),
                                r.S("object is not iterable")})));
    r.finish("$iter", 1, 2, {"v", "t"});
  }

  void rt_iternext() {
    RT r(*this);
    r.add(r.iff(r.is(r.idx(r.L(0), "k"), "g"),
                r.ret(r.in(IntrinsicId::GenResume,
                           {r.idx(r.L(0), "v"), r.Nil()}))));
    r.add(r.set(1, r.idx(r.L(0), "v")));
    r.add(r.set(2, r.idx(r.L(0), "i")));
    r.add(r.iff(r.bin(BinOp::Ge, r.L(2), r.len(r.L(1))),
                r.ret(r.obj({{"value", r.Nil()}, {"done", r.Bo(true)}}))));
    r.add(r.sidx(r.L(0), r.S("i"), r.bin(BinOp::Add, r.L(2), r.I(1))));
    r.add(r.ret(r.obj({{"value", r.idx(r.L(1), r.L(2))},
                       {"done", r.Bo(false)}})));
    r.finish("$iternext", 1, 3, {"it", "a", "i"});
  }

  void rt_range() {
    RT r(*this);
    r.add(r.set(3, r.arr({})));
    r.add(r.set(4, r.L(0)));
    r.add(r.wh(r.iff(r.bin(BinOp::Gt, r.L(2), r.I(0)),
                     r.bin(BinOp::Lt, r.L(4), r.L(1)),
                     r.bin(BinOp::Gt, r.L(4), r.L(1))),
               r.blk({r.push(r.L(3), r.L(4)),
                      r.set(4, r.bin(BinOp::Add, r.L(4), r.L(2)))})));
    r.add(r.ret(r.L(3)));
    r.finish("$range", 3, 5, {"start", "stop", "step", "out", "i"});
  }

  // Python's type names, which are neither the VM's nor visible to it: a
  // bignum is an `int` here even though it is an object, and a class
  // instance answers its own class's name.
  void rt_typename() {
    RT r(*this);
    r.add(r.set(1, r.typ(r.L(0))));
    r.add(r.iff(r.is(r.L(1), "nil"), r.ret(r.S("NoneType"))));
    r.add(r.iff(r.is(r.L(1), "bool"), r.ret(r.S("bool"))));
    r.add(r.iff(r.is(r.L(1), "int"), r.ret(r.S("int"))));
    r.add(r.iff(r.is(r.L(1), "double"), r.ret(r.S("float"))));
    r.add(r.iff(r.is(r.L(1), "string"), r.ret(r.S("str"))));
    r.add(r.iff(r.is(r.L(1), "array"), r.ret(r.S("list"))));
    r.add(r.iff(r.is(r.L(1), "map"), r.ret(r.S("dict"))));
    r.add(r.iff(r.is(r.L(1), "function"), r.ret(r.S("function"))));
    r.add(r.iff(r.is(r.L(1), "generator"), r.ret(r.S("generator"))));
    r.add(r.iff(r.call("$isbig", {r.L(0)}), r.ret(r.S("int"))));
    r.add(r.iff(r.call("$istup", {r.L(0)}), r.ret(r.S("tuple"))));
    r.add(r.iff(r.has(r.L(0), r.S(kExcKey)), r.ret(r.idx(r.L(0), kExcKey))));
    r.add(r.iff(r.has(r.L(0), r.S(kClassKey)),
                r.ret(r.idx(r.idx(r.L(0), kClassKey), kNameKey))));
    r.add(r.ret(r.S("object")));
    r.finish("$typename", 1, 2, {"v", "t"});
  }

  void rt_type() {
    RT r(*this);
    r.add(r.ret(r.obj({{"__name__", r.call("$typename", {r.L(0)})}})));
    r.finish("$type", 1, 1, {"v"});
  }

  void rt_join() {
    RT r(*this);
    r.add(r.set(2, r.S("")));
    r.add(r.set(3, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(3), r.len(r.L(1))),
               r.blk({r.iff(r.bin(BinOp::Gt, r.L(3), r.I(0)),
                            r.set(2, r.bin(BinOp::Add, r.L(2), r.L(0)))),
                      r.set(2, r.bin(BinOp::Add, r.L(2),
                                     r.idx(r.L(1), r.L(3)))),
                      r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})));
    r.add(r.ret(r.L(2)));
    r.finish("$join", 2, 4, {"sep", "xs", "out", "i"});
  }

  void rt_dget() {
    RT r(*this);
    r.add(r.iff(r.has(r.L(0), r.L(1)), r.ret(r.idx(r.L(0), r.L(1)))));
    r.add(r.ret(r.L(2)));
    r.finish("$dget", 3, 3, {"d", "k", "dflt"});
  }

  void rt_tolist() {
    RT r(*this);
    r.add(r.set(1, r.arr({})));
    r.add(r.set(2, r.call("$iter", {r.L(0)})));
    r.add(r.wh(r.Bo(true),
               r.blk({r.set(3, r.call("$iternext", {r.L(2)})),
                      r.iff(r.idx(r.L(3), "done"), r.b.make_break(r.p)),
                      r.push(r.L(1), r.idx(r.L(3), "value"))})));
    r.add(r.ret(r.L(1)));
    r.finish("$tolist", 1, 4, {"v", "out", "it", "st"});
  }

  void rt_toint() {
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(0)), "double"),
                r.ret(r.in(IntrinsicId::ToInt, {r.L(0)}))));
    r.add(r.iff(r.is(r.typ(r.L(0)), "bool"),
                r.ret(r.iff(r.L(0), r.I(1), r.I(0)))));
    // `int("...")` accumulates through $mul and $add, so a literal too big
    // for an int64 becomes a bignum with nothing further to say about it.
    r.add(r.iff(
        r.is(r.typ(r.L(0)), "string"),
        r.blk({r.set(1, r.call("$strip", {r.L(0), r.I(0)})),
               r.set(2, r.I(0)),
               r.set(3, r.Bo(false)),
               r.iff(r.bin(BinOp::Gt, r.len(r.L(1)), r.I(0)),
                     r.blk({r.iff(r.bin(BinOp::Eq,
                                        r.in(IntrinsicId::StrSlice,
                                             {r.L(1), r.I(0), r.I(1)}),
                                        r.S("-")),
                                  r.blk({r.set(3, r.Bo(true)),
                                         r.set(2, r.I(1))})),
                            r.iff(r.bin(BinOp::Eq,
                                        r.in(IntrinsicId::StrSlice,
                                             {r.L(1), r.I(0), r.I(1)}),
                                        r.S("+")),
                                  r.set(2, r.I(1)))})),
               r.iff(r.bin(BinOp::Ge, r.L(2), r.len(r.L(1))),
                     r.call("$intfail", {r.L(0)})),
               r.set(4, r.I(0)),
               r.wh(r.bin(BinOp::Lt, r.L(2), r.len(r.L(1))),
                    r.blk({r.set(5, r.in(IntrinsicId::StrByte,
                                         {r.L(1), r.L(2)})),
                           r.iff(r.either(r.bin(BinOp::Lt, r.L(5), r.I(48)),
                                          r.bin(BinOp::Gt, r.L(5), r.I(57))),
                                 r.call("$intfail", {r.L(0)})),
                           r.set(4, r.call("$add",
                                           {r.call("$mul", {r.L(4), r.I(10)}),
                                            r.bin(BinOp::Sub, r.L(5),
                                                  r.I(48))})),
                           r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1)))})),
               r.ret(r.iff(r.L(3), r.call("$neg", {r.L(4)}), r.L(4)))})));
    r.add(r.ret(r.L(0)));
    r.finish("$toint", 1, 6, {"v", "s", "i", "neg", "acc", "c"});
  }

  void rt_intfail() {
    RT r(*this);
    r.add(r.call("$exc",
                 {r.S("ValueError"),
                  r.bin(BinOp::Add,
                        r.S("invalid literal for int() with base 10: "),
                        r.call("$repr", {r.L(0)}))}));
    r.add(r.ret(r.Nil()));
    r.finish("$intfail", 1, 1, {"v"});
  }

  // -- The calling convention, written over the IR's ----------------------
  //
  // A CallValue fixes its argument count at the call site and a Func fixes
  // num_params, which is everything Python's convention is not: `f(*xs)`
  // decides a count at run time, `f(b=3)` passes by name, and `def f(*rest)`
  // collects what was left over -- while `lenient_arity` *drops* a surplus
  // rather than handing it anywhere. A call site cannot specialize its way
  // out either, because `obj.method(x)` calls whatever `$getattr` returned.
  //
  // So every Python function takes exactly two IR arguments -- the
  // positional array and the keyword object, nil when the call site had no
  // keywords -- and unpacks them in its prologue. It is the same trade
  // examples/mini-ruby makes for a block and examples/mini-lua for multiple
  // results: a convention the IR does not have, written over the one it
  // does. See README.md.

  void rt_acons() {  // [x] + a, for a method call's receiver
    RT r(*this);
    r.add(r.set(2, r.arr({r.L(0)})));
    r.add(r.iff(r.isnt(r.typ(r.L(1)), "array"), r.ret(r.L(2))));
    r.add(r.set(3, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(3), r.len(r.L(1))),
               r.blk({r.push(r.L(2), r.idx(r.L(1), r.L(3))),
                      r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})));
    r.add(r.ret(r.L(2)));
    r.finish("$acons", 2, 4, {"x", "a", "out", "i"});
  }

  void rt_aext() {  // `f(*xs)` -- any iterable, not only a list
    RT r(*this);
    r.add(r.set(2, r.call("$iter", {r.L(1)})));
    r.add(r.wh(r.Bo(true),
               r.blk({r.set(3, r.call("$iternext", {r.L(2)})),
                      r.iff(r.idx(r.L(3), "done"), r.b.make_break(r.p)),
                      r.push(r.L(0), r.idx(r.L(3), "value"))})));
    r.add(r.ret(r.L(0)));
    r.finish("$aext", 2, 4, {"dst", "src", "it", "st"});
  }

  void rt_rest() {  // `*rest` -- what the declared parameters did not take
    RT r(*this);
    r.add(r.set(2, r.len(r.L(0))));
    r.add(r.iff(r.bin(BinOp::Le, r.L(2), r.L(1)), r.ret(r.arr({}))));
    r.add(r.ret(r.in(IntrinsicId::ArraySlice, {r.L(0), r.L(1), r.L(2)})));
    r.finish("$rest", 2, 3, {"a", "i", "n"});
  }

  void rt_kwhas() {
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(0)), "nil"), r.ret(r.Bo(false))));
    r.add(r.ret(r.has(r.L(0), r.L(1))));
    r.finish("$kwhas", 2, 2, {"k", "n"});
  }

  void rt_hasname() {
    RT r(*this);
    r.add(r.set(2, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(2), r.len(r.L(0))),
               r.blk({r.iff(r.bin(BinOp::Eq, r.idx(r.L(0), r.L(2)), r.L(1)),
                            r.ret(r.Bo(true))),
                      r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1)))})));
    r.add(r.ret(r.Bo(false)));
    r.finish("$hasname", 2, 3, {"names", "n", "i"});
  }

  void rt_kwrest() {  // `**opts` -- a real dict, so the body can iterate it
    RT r(*this);
    r.add(r.set(2, r.in(IntrinsicId::MapNew, {})));
    r.add(r.iff(r.is(r.typ(r.L(0)), "nil"), r.ret(r.L(2))));
    r.add(r.set(3, r.in(IntrinsicId::ObjectKeys, {r.L(0)})));
    r.add(r.set(4, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(4), r.len(r.L(3))),
               r.blk({r.set(5, r.idx(r.L(3), r.L(4))),
                      r.iff(r.bin(BinOp::Eq,
                                  r.call("$hasname", {r.L(1), r.L(5)}),
                                  r.Bo(false)),
                            r.sidx(r.L(2), r.L(5), r.idx(r.L(0), r.L(5)))),
                      r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1)))})));
    r.add(r.ret(r.L(2)));
    r.finish("$kwrest", 2, 6, {"k", "names", "out", "ks", "i", "key"});
  }

  void rt_kwcheck() {  // no `**kwargs`: an unexpected keyword is a TypeError
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(0)), "nil"), r.ret(r.Nil())));
    r.add(r.set(3, r.in(IntrinsicId::ObjectKeys, {r.L(0)})));
    r.add(r.set(4, r.I(0)));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(4), r.len(r.L(3))),
        r.blk({r.set(5, r.idx(r.L(3), r.L(4))),
               r.iff(r.bin(BinOp::Eq, r.call("$hasname", {r.L(1), r.L(5)}),
                           r.Bo(false)),
                     r.call("$exc",
                            {r.S("TypeError"),
                             r.bin(BinOp::Add,
                                   r.bin(BinOp::Add, r.L(2),
                                         r.S("() got an unexpected keyword "
                                             "argument '")),
                                   r.bin(BinOp::Add, r.L(5), r.S("'")))})),
               r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1)))})));
    r.add(r.ret(r.Nil()));
    r.finish("$kwcheck", 3, 6, {"k", "names", "fn", "ks", "i", "key"});
  }

  void rt_kwmerge() {  // `f(**d)`
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(1)), "nil"), r.ret(r.L(0))));
    r.add(r.set(2, r.in(IntrinsicId::ObjectKeys, {r.L(1)})));
    r.add(r.set(3, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(3), r.len(r.L(2))),
               r.blk({r.set(4, r.idx(r.L(2), r.L(3))),
                      r.sidx(r.L(0), r.L(4), r.idx(r.L(1), r.L(4))),
                      r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})));
    r.add(r.ret(r.L(0)));
    r.finish("$kwmerge", 2, 5, {"dst", "src", "ks", "i", "key"});
  }

  // Both diagnostics are Python's own, down to the comma before "and" and
  // the singular "argument" -- because `python3` is the oracle, and a
  // message is output like any other. Neither is something the executor
  // could have raised: its arity check knows a count, and these know which
  // parameter, by name, and whether it had a default.
  void rt_missing() {
    RT r(*this);
    // Every required parameter no positional and no keyword answered.
    r.add(r.set(5, r.arr({})));
    r.add(r.set(6, r.I(0)));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(6), r.len(r.L(1))),
        r.blk({r.set(7, r.idx(r.L(1), r.L(6))),
               r.iff(r.bin(BinOp::Eq,
                           r.either(r.bin(BinOp::Gt, r.len(r.L(3)),
                                          r.idx(r.L(2), r.L(6))),
                                    r.call("$kwhas", {r.L(4), r.L(7)})),
                           r.Bo(false)),
                     r.push(r.L(5), r.bin(BinOp::Add,
                                          r.bin(BinOp::Add, r.S("'"), r.L(7)),
                                          r.S("'")))),
               r.set(6, r.bin(BinOp::Add, r.L(6), r.I(1)))})));
    r.add(r.set(8, r.len(r.L(5))));
    // "'a'", "'a' and 'b'", "'a', 'b', and 'c'" -- three shapes, and the
    // last one keeps the serial comma.
    r.add(r.set(9, r.idx(r.L(5), r.I(0))));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(8), r.I(2)),
                r.set(9, r.bin(BinOp::Add, r.L(9),
                               r.bin(BinOp::Add, r.S(" and "),
                                     r.idx(r.L(5), r.I(1)))))));
    r.add(r.iff(
        r.bin(BinOp::Gt, r.L(8), r.I(2)),
        r.blk({r.set(6, r.I(1)),
               r.wh(r.bin(BinOp::Lt, r.L(6),
                          r.bin(BinOp::Sub, r.L(8), r.I(1))),
                    r.blk({r.set(9, r.bin(BinOp::Add, r.L(9),
                                          r.bin(BinOp::Add, r.S(", "),
                                                r.idx(r.L(5), r.L(6))))),
                           r.set(6, r.bin(BinOp::Add, r.L(6), r.I(1)))})),
               r.set(9, r.bin(BinOp::Add, r.L(9),
                              r.bin(BinOp::Add, r.S(", and "),
                                    r.idx(r.L(5), r.L(6)))))})));
    r.add(r.call(
        "$exc",
        {r.S("TypeError"),
         r.bin(BinOp::Add,
               r.bin(BinOp::Add, r.L(0), r.S("() missing ")),
               r.bin(BinOp::Add,
                     r.call("$str", {r.L(8)}),
                     r.bin(BinOp::Add,
                           r.iff(r.bin(BinOp::Eq, r.L(8), r.I(1)),
                                 r.S(" required positional argument: "),
                                 r.S(" required positional arguments: ")),
                           r.L(9))))}));
    r.finish("$missing", 5, 10,
             {"fn", "names", "idxs", "a", "k", "miss", "i", "n", "c", "s"});
  }

  void rt_toomany() {
    RT r(*this);
    // "takes 2 positional arguments", or "takes from 1 to 3" when some of
    // them had defaults.
    r.add(r.set(4, r.bin(BinOp::Add, r.call("$str", {r.L(2)}),
                         r.iff(r.bin(BinOp::Eq, r.L(2), r.I(1)),
                               r.S(" positional argument but "),
                               r.S(" positional arguments but ")))));
    r.add(r.iff(r.bin(BinOp::Ne, r.L(1), r.L(2)),
                r.set(4, r.bin(BinOp::Add,
                               r.bin(BinOp::Add, r.S("from "),
                                     r.call("$str", {r.L(1)})),
                               r.bin(BinOp::Add,
                                     r.bin(BinOp::Add, r.S(" to "),
                                           r.call("$str", {r.L(2)})),
                                     r.S(" positional arguments but "))))));
    r.add(r.call(
        "$exc",
        {r.S("TypeError"),
         r.bin(BinOp::Add, r.bin(BinOp::Add, r.L(0), r.S("() takes ")),
               r.bin(BinOp::Add, r.L(4),
                     r.bin(BinOp::Add, r.call("$str", {r.L(3)}),
                           r.iff(r.bin(BinOp::Eq, r.L(3), r.I(1)),
                                 r.S(" was given"), r.S(" were given")))))}));
    r.finish("$toomany", 4, 5, {"fn", "lo", "hi", "got", "s"});
  }

  // -- Inheritance ---------------------------------------------------------
  //
  // There is no class in this IR and nothing was added for one. A class is
  // an object holding its methods, its base's table and the constructor
  // closure that is its identity; an instance is an object pointing back at
  // it. Every question a class system answers is then a walk up that chain,
  // and the walk is these five funcs. See README.md.

  void rt_clsfind() {  // the method `key` names, from `t` or an ancestor
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
    r.finish("$clsfind", 2, 3, {"t", "key", "c"});
  }

  // A builtin exception is not a class here, so `except ValueError` matches
  // by name -- and a class of the program's own that derives from one is
  // marked with the name it was rooted at, which is what this also finds.
  void rt_isname() {
    RT r(*this);
    r.add(r.iff(r.isnt(r.typ(r.L(0)), "object"), r.ret(r.Bo(false))));
    r.add(r.iff(r.has(r.L(0), r.S(kExcKey)),
                r.ret(r.bin(BinOp::Eq, r.idx(r.L(0), kExcKey), r.L(1)))));
    r.add(r.iff(r.bin(BinOp::Eq, r.has(r.L(0), r.S(kClassKey)), r.Bo(false)),
                r.ret(r.Bo(false))));
    r.add(r.set(2, r.idx(r.L(0), kClassKey)));
    r.add(r.wh(r.is(r.typ(r.L(2)), "object"),
               r.blk({r.iff(r.both(r.has(r.L(2), r.S(kRootKey)),
                                   r.bin(BinOp::Eq, r.idx(r.L(2), kRootKey),
                                         r.L(1))),
                            r.ret(r.Bo(true))),
                      r.iff(r.bin(BinOp::Eq, r.has(r.L(2), r.S(kBaseKey)),
                                  r.Bo(false)),
                            r.b.make_break(r.p)),
                      r.set(2, r.idx(r.L(2), kBaseKey))})));
    r.add(r.ret(r.Bo(false)));
    r.finish("$isname", 2, 3, {"v", "name", "t"});
  }

  // The identity walk: a class value *is* its constructor closure, so two
  // classes are the same class when `Same` says the closures are.
  void rt_isinstv() {
    RT r(*this);
    r.add(r.iff(r.isnt(r.typ(r.L(0)), "object"), r.ret(r.Bo(false))));
    r.add(r.iff(r.bin(BinOp::Eq, r.has(r.L(0), r.S(kClassKey)), r.Bo(false)),
                r.ret(r.Bo(false))));
    r.add(r.set(2, r.idx(r.L(0), kClassKey)));
    r.add(r.wh(r.is(r.typ(r.L(2)), "object"),
               r.blk({r.iff(r.both(r.has(r.L(2), r.S(kIdKey)),
                                   r.in(IntrinsicId::Same,
                                        {r.idx(r.L(2), kIdKey), r.L(1)})),
                            r.ret(r.Bo(true))),
                      r.iff(r.bin(BinOp::Eq, r.has(r.L(2), r.S(kBaseKey)),
                                  r.Bo(false)),
                            r.b.make_break(r.p)),
                      r.set(2, r.idx(r.L(2), kBaseKey))})));
    r.add(r.ret(r.Bo(false)));
    r.finish("$isinstv", 2, 3, {"v", "cls", "t"});
  }

  void rt_isinst() {
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(1)), "string"),
                r.ret(r.either(r.bin(BinOp::Eq, r.call("$typename", {r.L(0)}),
                                     r.L(1)),
                               r.call("$isname", {r.L(0), r.L(1)})))));
    r.add(r.ret(r.call("$isinstv", {r.L(0), r.L(1)})));
    r.finish("$isinst", 2, 2, {"v", "cls"});
  }

  // `super().m(...)`. The base may be nothing at all -- a class rooted at a
  // builtin exception has a name to be caught by and no table to inherit
  // from -- and then `__init__` is the one method that still means
  // something: it is what stores the message.
  void rt_supercall() {
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(1)), "object"),
                r.blk({r.set(5, r.call("$clsfind", {r.L(1), r.L(2)})),
                       r.iff(r.isnt(r.typ(r.L(5)), "nil"),
                             r.ret(r.b.call_value(
                                 r.L(5),
                                 {r.call("$acons", {r.L(0), r.L(3)}), r.L(4)},
                                 r.p)))})));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(2), r.S("\x02__init__")),
                r.ret(r.call("$excinit", {r.L(0), r.L(3)}))));
    r.add(r.ret(r.call("$exc",
                       {r.S("AttributeError"),
                        r.bin(BinOp::Add, r.S("'super' object has no attribute "),
                              r.in(IntrinsicId::StrSlice,
                                   {r.L(2), r.I(1), r.len(r.L(2))}))})));
    r.finish("$supercall", 5, 6, {"self", "base", "key", "a", "k", "f"});
  }

  void rt_excinit() {
    RT r(*this);
    r.add(r.sidx(r.L(0), r.S(kMsgKey),
                 r.iff(r.bin(BinOp::Gt, r.len(r.L(1)), r.I(0)),
                       r.idx(r.L(1), r.I(0)), r.S(""))));
    r.add(r.ret(r.Nil()));
    r.finish("$excinit", 2, 2, {"self", "a"});
  }

  void rt_noinit() {
    RT r(*this);
    r.add(r.iff(r.bin(BinOp::Gt, r.len(r.L(1)), r.I(0)),
                r.call("$exc", {r.S("TypeError"),
                                r.bin(BinOp::Add, r.L(0),
                                      r.S("() takes no arguments"))})));
    r.add(r.ret(r.Nil()));
    r.finish("$noinit", 2, 2, {"cls", "a"});
  }

  // `__str__`, `__repr__`, `__eq__`: found the same way any method is, and
  // called through the same convention.
  void rt_dunder() {
    RT r(*this);
    r.add(r.iff(r.isnt(r.typ(r.L(0)), "object"), r.ret(r.Nil())));
    r.add(r.iff(r.bin(BinOp::Eq, r.has(r.L(0), r.S(kClassKey)), r.Bo(false)),
                r.ret(r.Nil())));
    r.add(r.ret(r.call("$clsfind", {r.idx(r.L(0), kClassKey), r.L(1)})));
    r.finish("$dunder", 2, 2, {"v", "key"});
  }

  // -- Tuples --------------------------------------------------------------
  //
  // The IR has one sequence type and Python has two, which would be a
  // detail if they printed and compared the same -- and they do not. So a
  // tuple is its array under a key no source can write, and the sequence
  // funcs unwrap before they look. See README.md.

  void rt_tuple() {
    RT r(*this);
    r.add(r.ret(r.obj({{kTupKey, r.L(0)}})));
    r.finish("$tuple", 1, 1, {"a"});
  }

  void rt_untup() {
    RT r(*this);
    r.add(r.iff(r.call("$istup", {r.L(0)}), r.ret(r.idx(r.L(0), kTupKey))));
    r.add(r.ret(r.L(0)));
    r.finish("$untup", 1, 1, {"v"});
  }

  void rt_istup() {
    RT r(*this);
    r.add(r.iff(r.isnt(r.typ(r.L(0)), "object"), r.ret(r.Bo(false))));
    r.add(r.ret(r.has(r.L(0), r.S(kTupKey))));
    r.finish("$istup", 1, 1, {"v"});
  }

  void rt_tupstr() {
    RT r(*this);
    r.add(r.set(1, r.S("(")));
    r.add(r.set(2, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(2), r.len(r.L(0))),
               r.blk({r.iff(r.bin(BinOp::Gt, r.L(2), r.I(0)),
                            r.set(1, r.bin(BinOp::Add, r.L(1), r.S(", ")))),
                      r.set(1, r.bin(BinOp::Add, r.L(1),
                                     r.call("$repr",
                                            {r.idx(r.L(0), r.L(2))}))),
                      r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1)))})));
    // `(1,)` -- the comma is what makes a one-element tuple a tuple.
    r.add(r.iff(r.bin(BinOp::Eq, r.len(r.L(0)), r.I(1)),
                r.set(1, r.bin(BinOp::Add, r.L(1), r.S(",")))));
    r.add(r.ret(r.bin(BinOp::Add, r.L(1), r.S(")"))));
    r.finish("$tupstr", 1, 3, {"a", "out", "i"});
  }

  // `a, b = expr`: any iterable, and exactly as many values as targets.
  void rt_unpack() {
    RT r(*this);
    r.add(r.set(2, r.call("$tolist", {r.L(0)})));
    r.add(r.iff(r.bin(BinOp::Lt, r.len(r.L(2)), r.L(1)),
                r.call("$exc",
                       {r.S("ValueError"),
                        r.bin(BinOp::Add,
                              r.bin(BinOp::Add,
                                    r.S("not enough values to unpack "
                                        "(expected "),
                                    r.call("$str", {r.L(1)})),
                              r.bin(BinOp::Add,
                                    r.bin(BinOp::Add, r.S(", got "),
                                          r.call("$str",
                                                 {r.len(r.L(2))})),
                                    r.S(")")))})));
    r.add(r.iff(r.bin(BinOp::Gt, r.len(r.L(2)), r.L(1)),
                r.call("$exc",
                       {r.S("ValueError"),
                        r.bin(BinOp::Add,
                              r.bin(BinOp::Add,
                                    r.S("too many values to unpack "
                                        "(expected "),
                                    r.call("$str", {r.L(1)})),
                              r.S(")"))})));
    r.add(r.ret(r.L(2)));
    r.finish("$unpack", 2, 3, {"v", "n", "a"});
  }

  void rt_items() {
    RT r(*this);
    r.add(r.set(1, r.in(IntrinsicId::ObjectKeys, {r.L(0)})));
    r.add(r.set(2, r.arr({})));
    r.add(r.set(3, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(3), r.len(r.L(1))),
               r.blk({r.push(r.L(2),
                             r.call("$tuple",
                                    {r.arr({r.idx(r.L(1), r.L(3)),
                                            r.idx(r.L(0),
                                                  r.idx(r.L(1), r.L(3)))})})),
                      r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})));
    r.add(r.ret(r.L(2)));
    r.finish("$items", 1, 4, {"d", "ks", "out", "i"});
  }

  void rt_values() {
    RT r(*this);
    r.add(r.set(1, r.in(IntrinsicId::ObjectKeys, {r.L(0)})));
    r.add(r.set(2, r.arr({})));
    r.add(r.set(3, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(3), r.len(r.L(1))),
               r.blk({r.push(r.L(2), r.idx(r.L(0), r.idx(r.L(1), r.L(3)))),
                      r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})));
    r.add(r.ret(r.L(2)));
    r.finish("$values", 1, 4, {"d", "ks", "out", "i"});
  }

  void rt_enumerate() {
    RT r(*this);
    r.add(r.set(2, r.call("$tolist", {r.L(0)})));
    r.add(r.set(3, r.arr({})));
    r.add(r.set(4, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(4), r.len(r.L(2))),
               r.blk({r.push(r.L(3),
                             r.call("$tuple",
                                    {r.arr({r.bin(BinOp::Add, r.L(1), r.L(4)),
                                            r.idx(r.L(2), r.L(4))})})),
                      r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1)))})));
    r.add(r.ret(r.L(3)));
    r.finish("$enumerate", 2, 5, {"v", "start", "a", "out", "i"});
  }

  void rt_zip() {
    RT r(*this);
    r.add(r.set(2, r.call("$tolist", {r.L(0)})));
    r.add(r.set(3, r.call("$tolist", {r.L(1)})));
    r.add(r.set(4, r.arr({})));
    r.add(r.set(5, r.I(0)));
    r.add(r.wh(r.both(r.bin(BinOp::Lt, r.L(5), r.len(r.L(2))),
                      r.bin(BinOp::Lt, r.L(5), r.len(r.L(3)))),
               r.blk({r.push(r.L(4),
                             r.call("$tuple",
                                    {r.arr({r.idx(r.L(2), r.L(5)),
                                            r.idx(r.L(3), r.L(5))})})),
                      r.set(5, r.bin(BinOp::Add, r.L(5), r.I(1)))})));
    r.add(r.ret(r.L(4)));
    r.finish("$zip", 2, 6, {"a", "b", "xs", "ys", "out", "i"});
  }

  // An insertion sort over the values and their keys at once. Python's is
  // a stable merge sort; this one is stable too, which is the property a
  // program can see -- see README.md for what it is not.
  void rt_sorted() {
    RT r(*this);
    r.add(r.set(3, r.call("$tolist", {r.L(0)})));
    r.add(r.set(4, r.arr({})));
    r.add(r.set(5, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(5), r.len(r.L(3))),
               r.blk({r.push(r.L(4),
                             r.iff(r.is(r.typ(r.L(1)), "nil"),
                                   r.idx(r.L(3), r.L(5)),
                                   r.b.call_value(
                                       r.L(1),
                                       {r.arr({r.idx(r.L(3), r.L(5))}),
                                        r.Nil()},
                                       r.p))),
                      r.set(5, r.bin(BinOp::Add, r.L(5), r.I(1)))})));
    r.add(r.set(5, r.I(1)));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(5), r.len(r.L(3))),
        r.blk({r.set(6, r.idx(r.L(3), r.L(5))), r.set(7, r.idx(r.L(4), r.L(5))),
               r.set(8, r.bin(BinOp::Sub, r.L(5), r.I(1))),
               r.wh(r.both(r.bin(BinOp::Ge, r.L(8), r.I(0)),
                           r.bin(BinOp::Gt,
                                 r.call("$cmp", {r.idx(r.L(4), r.L(8)),
                                                 r.L(7)}),
                                 r.I(0))),
                    r.blk({r.sidx(r.L(3), r.bin(BinOp::Add, r.L(8), r.I(1)),
                                  r.idx(r.L(3), r.L(8))),
                           r.sidx(r.L(4), r.bin(BinOp::Add, r.L(8), r.I(1)),
                                  r.idx(r.L(4), r.L(8))),
                           r.set(8, r.bin(BinOp::Sub, r.L(8), r.I(1)))})),
               r.sidx(r.L(3), r.bin(BinOp::Add, r.L(8), r.I(1)), r.L(6)),
               r.sidx(r.L(4), r.bin(BinOp::Add, r.L(8), r.I(1)), r.L(7)),
               r.set(5, r.bin(BinOp::Add, r.L(5), r.I(1)))})));
    r.add(r.iff(r.call("$truthy", {r.L(2)}),
                r.blk({r.set(4, r.arr({})), r.set(5, r.len(r.L(3))),
                       r.wh(r.bin(BinOp::Gt, r.L(5), r.I(0)),
                            r.blk({r.set(5, r.bin(BinOp::Sub, r.L(5), r.I(1))),
                                   r.push(r.L(4), r.idx(r.L(3), r.L(5)))})),
                       r.ret(r.L(4))})));
    r.add(r.ret(r.L(3)));
    r.finish("$sorted", 3, 9,
             {"v", "key", "rev", "a", "ks", "i", "vx", "kx", "j"});
  }

  // `next(it)` -- and `next(it, default)`, which is the difference between
  // a StopIteration and a value.
  void rt_next() {
    RT r(*this);
    r.add(r.set(3, r.call("$iternext", {r.call("$iter", {r.L(0)})})));
    r.add(r.iff(r.idx(r.L(3), "done"),
                r.blk({r.iff(r.L(2), r.ret(r.L(1))),
                       r.call("$exc", {r.S("StopIteration"), r.S("")})})));
    r.add(r.ret(r.idx(r.L(3), "value")));
    r.finish("$next", 3, 4, {"g", "dflt", "hasdflt", "st"});
  }

  void rt_sum() {
    RT r(*this);
    r.add(r.set(2, r.call("$tolist", {r.L(0)})));
    r.add(r.set(3, r.L(1)));
    r.add(r.set(4, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(4), r.len(r.L(2))),
               r.blk({r.set(3, r.call("$add", {r.L(3), r.idx(r.L(2), r.L(4))})),
                      r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1)))})));
    r.add(r.ret(r.L(3)));
    r.finish("$sum", 2, 5, {"v", "start", "a", "acc", "i"});
  }

  void rt_minmax() {
    RT r(*this);
    r.add(r.set(2, r.call("$tolist", {r.L(0)})));
    r.add(r.iff(r.bin(BinOp::Eq, r.len(r.L(2)), r.I(0)),
                r.call("$exc", {r.S("ValueError"),
                                r.S("arg is an empty sequence")})));
    r.add(r.set(3, r.idx(r.L(2), r.I(0))));
    r.add(r.set(4, r.I(1)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(4), r.len(r.L(2))),
               r.blk({r.set(5, r.call("$cmp", {r.idx(r.L(2), r.L(4)), r.L(3)})),
                      r.iff(r.iff(r.L(1), r.bin(BinOp::Gt, r.L(5), r.I(0)),
                                  r.bin(BinOp::Lt, r.L(5), r.I(0))),
                            r.set(3, r.idx(r.L(2), r.L(4)))),
                      r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1)))})));
    r.add(r.ret(r.L(3)));
    r.finish("$minmax", 2, 6, {"v", "ismax", "a", "best", "i", "c"});
  }

  // -- The string, list and dict methods -----------------------------------

  void rt_isspace() {
    RT r(*this);
    r.add(r.ret(r.either(
        r.bin(BinOp::Eq, r.L(0), r.S(" ")),
        r.either(r.bin(BinOp::Eq, r.L(0), r.S("\t")),
                 r.either(r.bin(BinOp::Eq, r.L(0), r.S("\n")),
                          r.bin(BinOp::Eq, r.L(0), r.S("\r")))))));
    r.finish("$isspace", 1, 1, {"c"});
  }

  // `s.split()` runs on whitespace and drops the empties; `s.split(sep)`
  // keeps them. They are different functions wearing one name, which is
  // Python's choice and not this one's.
  void rt_split() {
    RT r(*this);
    const auto ch = [&](NodeId str, NodeId i) {
      return r.in(IntrinsicId::StrSlice,
                  {str, i, r.bin(BinOp::Add, i, r.I(1))});
    };
    r.add(r.set(3, r.arr({})));
    r.add(r.set(4, r.I(0)));
    r.add(r.iff(
        r.bin(BinOp::Eq, r.L(2), r.Bo(false)),
        r.blk({r.set(5, r.S("")),
               r.wh(r.bin(BinOp::Lt, r.L(4), r.len(r.L(0))),
                    r.blk({r.iff(r.call("$isspace", {ch(r.L(0), r.L(4))}),
                                 r.blk({r.iff(r.bin(BinOp::Gt, r.len(r.L(5)),
                                                    r.I(0)),
                                              r.blk({r.push(r.L(3), r.L(5)),
                                                     r.set(5, r.S(""))}))}),
                                 r.set(5, r.bin(BinOp::Add, r.L(5),
                                                ch(r.L(0), r.L(4))))),
                           r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1)))})),
               r.iff(r.bin(BinOp::Gt, r.len(r.L(5)), r.I(0)),
                     r.push(r.L(3), r.L(5))),
               r.ret(r.L(3))})));
    r.add(r.iff(r.bin(BinOp::Eq, r.len(r.L(1)), r.I(0)),
                r.call("$exc", {r.S("ValueError"), r.S("empty separator")})));
    r.add(r.set(6, r.I(0)));
    r.add(r.wh(
        r.bin(BinOp::Le, r.bin(BinOp::Add, r.L(4), r.len(r.L(1))),
              r.len(r.L(0))),
        r.blk({r.iff(r.bin(BinOp::Eq,
                           r.in(IntrinsicId::StrSlice,
                                {r.L(0), r.L(4),
                                 r.bin(BinOp::Add, r.L(4), r.len(r.L(1)))}),
                           r.L(1)),
                     r.blk({r.push(r.L(3), r.in(IntrinsicId::StrSlice,
                                                {r.L(0), r.L(6), r.L(4)})),
                            r.set(4, r.bin(BinOp::Add, r.L(4),
                                           r.len(r.L(1)))),
                            r.set(6, r.L(4))}),
                     r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1))))})));
    r.add(r.push(r.L(3), r.in(IntrinsicId::StrSlice,
                              {r.L(0), r.L(6), r.len(r.L(0))})));
    r.add(r.ret(r.L(3)));
    r.finish("$split", 3, 7, {"s", "sep", "hassep", "out", "i", "cur", "st"});
  }

  void rt_strip() {  // mode 0 both, 1 left, 2 right
    RT r(*this);
    const auto ch = [&](NodeId i) {
      return r.in(IntrinsicId::StrSlice,
                  {r.L(0), i, r.bin(BinOp::Add, i, r.I(1))});
    };
    r.add(r.set(2, r.I(0)));
    r.add(r.set(3, r.len(r.L(0))));
    r.add(r.iff(r.bin(BinOp::Ne, r.L(1), r.I(2)),
                r.wh(r.both(r.bin(BinOp::Lt, r.L(2), r.L(3)),
                            r.call("$isspace", {ch(r.L(2))})),
                     r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1))))));
    r.add(r.iff(
        r.bin(BinOp::Ne, r.L(1), r.I(1)),
        r.wh(r.both(r.bin(BinOp::Gt, r.L(3), r.L(2)),
                    r.call("$isspace",
                           {ch(r.bin(BinOp::Sub, r.L(3), r.I(1)))})),
             r.set(3, r.bin(BinOp::Sub, r.L(3), r.I(1))))));
    r.add(r.ret(r.in(IntrinsicId::StrSlice, {r.L(0), r.L(2), r.L(3)})));
    r.finish("$strip", 2, 4, {"s", "mode", "i", "j"});
  }

  void rt_replace() {
    RT r(*this);
    r.add(r.set(3, r.S("")));
    r.add(r.set(4, r.I(0)));
    r.add(r.iff(r.bin(BinOp::Eq, r.len(r.L(1)), r.I(0)),
                r.ret(r.L(0))));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(4), r.len(r.L(0))),
        r.blk({r.iff(
                   r.both(r.bin(BinOp::Le,
                                r.bin(BinOp::Add, r.L(4), r.len(r.L(1))),
                                r.len(r.L(0))),
                          r.bin(BinOp::Eq,
                                r.in(IntrinsicId::StrSlice,
                                     {r.L(0), r.L(4),
                                      r.bin(BinOp::Add, r.L(4),
                                            r.len(r.L(1)))}),
                                r.L(1))),
                   r.blk({r.set(3, r.bin(BinOp::Add, r.L(3), r.L(2))),
                          r.set(4, r.bin(BinOp::Add, r.L(4),
                                         r.len(r.L(1))))}),
                   r.blk({r.set(3, r.bin(BinOp::Add, r.L(3),
                                         r.in(IntrinsicId::StrSlice,
                                              {r.L(0), r.L(4),
                                               r.bin(BinOp::Add, r.L(4),
                                                     r.I(1))}))),
                          r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1)))}))})));
    r.add(r.ret(r.L(3)));
    r.finish("$replace", 3, 5, {"s", "a", "b", "out", "i"});
  }

  void rt_find() {
    RT r(*this);
    r.add(r.set(2, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Le, r.bin(BinOp::Add, r.L(2), r.len(r.L(1))),
                     r.len(r.L(0))),
               r.blk({r.iff(r.bin(BinOp::Eq,
                                  r.in(IntrinsicId::StrSlice,
                                       {r.L(0), r.L(2),
                                        r.bin(BinOp::Add, r.L(2),
                                              r.len(r.L(1)))}),
                                  r.L(1)),
                            r.ret(r.L(2))),
                      r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1)))})));
    r.add(r.ret(r.I(-1)));
    r.finish("$find", 2, 3, {"s", "sub", "i"});
  }

  void rt_scount() {
    RT r(*this);
    r.add(r.set(2, r.I(0)));
    r.add(r.set(3, r.I(0)));
    r.add(r.iff(r.bin(BinOp::Eq, r.len(r.L(1)), r.I(0)), r.ret(r.I(0))));
    r.add(r.wh(r.bin(BinOp::Le, r.bin(BinOp::Add, r.L(2), r.len(r.L(1))),
                     r.len(r.L(0))),
               r.blk({r.iff(r.bin(BinOp::Eq,
                                  r.in(IntrinsicId::StrSlice,
                                       {r.L(0), r.L(2),
                                        r.bin(BinOp::Add, r.L(2),
                                              r.len(r.L(1)))}),
                                  r.L(1)),
                            r.blk({r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1))),
                                   r.set(2, r.bin(BinOp::Add, r.L(2),
                                                  r.len(r.L(1))))}),
                            r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1))))})));
    r.add(r.ret(r.L(3)));
    r.finish("$scount", 2, 4, {"s", "sub", "i", "n"});
  }

  void rt_startswith() {
    RT r(*this);
    r.add(r.iff(r.bin(BinOp::Gt, r.len(r.L(1)), r.len(r.L(0))),
                r.ret(r.Bo(false))));
    r.add(r.ret(r.bin(BinOp::Eq,
                      r.in(IntrinsicId::StrSlice,
                           {r.L(0), r.I(0), r.len(r.L(1))}),
                      r.L(1))));
    r.finish("$startswith", 2, 2, {"s", "p"});
  }

  void rt_endswith() {
    RT r(*this);
    r.add(r.iff(r.bin(BinOp::Gt, r.len(r.L(1)), r.len(r.L(0))),
                r.ret(r.Bo(false))));
    r.add(r.ret(r.bin(BinOp::Eq,
                      r.in(IntrinsicId::StrSlice,
                           {r.L(0),
                            r.bin(BinOp::Sub, r.len(r.L(0)), r.len(r.L(1))),
                            r.len(r.L(0))}),
                      r.L(1))));
    r.finish("$endswith", 2, 2, {"s", "p"});
  }

  void rt_apop() {
    RT r(*this);
    r.add(r.set(3, r.len(r.L(0))));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(3), r.I(0)),
                r.call("$exc", {r.S("IndexError"),
                                r.S("pop from empty list")})));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(2), r.Bo(false)),
                r.ret(r.in(IntrinsicId::ArrayPop, {r.L(0)}))));
    r.add(r.set(4, r.L(1)));
    r.add(r.iff(r.bin(BinOp::Lt, r.L(4), r.I(0)),
                r.set(4, r.bin(BinOp::Add, r.L(4), r.L(3)))));
    r.add(r.iff(r.either(r.bin(BinOp::Lt, r.L(4), r.I(0)),
                         r.bin(BinOp::Ge, r.L(4), r.L(3))),
                r.call("$exc", {r.S("IndexError"),
                                r.S("pop index out of range")})));
    r.add(r.set(5, r.idx(r.L(0), r.L(4))));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(4), r.bin(BinOp::Sub, r.L(3), r.I(1))),
               r.blk({r.sidx(r.L(0), r.L(4),
                             r.idx(r.L(0), r.bin(BinOp::Add, r.L(4), r.I(1)))),
                      r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1)))})));
    r.add(r.in(IntrinsicId::ArrayPop, {r.L(0)}));
    r.add(r.ret(r.L(5)));
    r.finish("$apop", 3, 6, {"a", "i", "hasi", "n", "k", "v"});
  }

  void rt_ainsert() {
    RT r(*this);
    r.add(r.set(3, r.len(r.L(0))));
    r.add(r.set(4, r.L(1)));
    r.add(r.iff(r.bin(BinOp::Lt, r.L(4), r.I(0)),
                r.set(4, r.bin(BinOp::Add, r.L(4), r.L(3)))));
    r.add(r.iff(r.bin(BinOp::Lt, r.L(4), r.I(0)), r.set(4, r.I(0))));
    r.add(r.iff(r.bin(BinOp::Gt, r.L(4), r.L(3)), r.set(4, r.L(3))));
    r.add(r.push(r.L(0), r.L(2)));
    r.add(r.set(5, r.L(3)));
    r.add(r.wh(r.bin(BinOp::Gt, r.L(5), r.L(4)),
               r.blk({r.sidx(r.L(0), r.L(5),
                             r.idx(r.L(0), r.bin(BinOp::Sub, r.L(5), r.I(1)))),
                      r.set(5, r.bin(BinOp::Sub, r.L(5), r.I(1)))})));
    r.add(r.sidx(r.L(0), r.L(4), r.L(2)));
    r.add(r.ret(r.Nil()));
    r.finish("$ainsert", 3, 6, {"a", "i", "v", "n", "k", "j"});
  }

  void rt_aindex() {
    RT r(*this);
    r.add(r.set(2, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(2), r.len(r.L(0))),
               r.blk({r.iff(r.call("$eq", {r.idx(r.L(0), r.L(2)), r.L(1)}),
                            r.ret(r.L(2))),
                      r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1)))})));
    r.add(r.call("$exc",
                 {r.S("ValueError"),
                  r.bin(BinOp::Add, r.call("$repr", {r.L(1)}),
                        r.S(" is not in list"))}));
    r.add(r.ret(r.Nil()));
    r.finish("$aindex", 2, 3, {"a", "v", "i"});
  }

  void rt_aremove() {
    RT r(*this);
    r.add(r.call("$apop",
                 {r.L(0), r.call("$aindex", {r.L(0), r.L(1)}), r.Bo(true)}));
    r.add(r.ret(r.Nil()));
    r.finish("$aremove", 2, 2, {"a", "v"});
  }

  void rt_acount() {
    RT r(*this);
    r.add(r.set(2, r.I(0)));
    r.add(r.set(3, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(2), r.len(r.L(0))),
               r.blk({r.iff(r.call("$eq", {r.idx(r.L(0), r.L(2)), r.L(1)}),
                            r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))),
                      r.set(2, r.bin(BinOp::Add, r.L(2), r.I(1)))})));
    r.add(r.ret(r.L(3)));
    r.finish("$acount", 2, 4, {"a", "v", "i", "n"});
  }

  void rt_areverse() {
    RT r(*this);
    r.add(r.set(1, r.I(0)));
    r.add(r.set(2, r.bin(BinOp::Sub, r.len(r.L(0)), r.I(1))));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(1), r.L(2)),
               r.blk({r.set(3, r.idx(r.L(0), r.L(1))),
                      r.sidx(r.L(0), r.L(1), r.idx(r.L(0), r.L(2))),
                      r.sidx(r.L(0), r.L(2), r.L(3)),
                      r.set(1, r.bin(BinOp::Add, r.L(1), r.I(1))),
                      r.set(2, r.bin(BinOp::Sub, r.L(2), r.I(1)))})));
    r.add(r.ret(r.Nil()));
    r.finish("$areverse", 1, 4, {"a", "i", "j", "t"});
  }

  void rt_asort() {  // `xs.sort()` -- in place, so the result is copied back
    RT r(*this);
    r.add(r.set(3, r.call("$sorted", {r.L(0), r.L(1), r.L(2)})));
    r.add(r.set(4, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(4), r.len(r.L(3))),
               r.blk({r.sidx(r.L(0), r.L(4), r.idx(r.L(3), r.L(4))),
                      r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1)))})));
    r.add(r.ret(r.Nil()));
    r.finish("$asort", 3, 5, {"a", "key", "rev", "s", "i"});
  }

  void rt_delitem() {
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(0)), "map"),
                r.blk({r.iff(r.bin(BinOp::Eq, r.has(r.L(0), r.L(1)),
                                   r.Bo(false)),
                             r.call("$exc", {r.S("KeyError"),
                                             r.call("$repr", {r.L(1)})})),
                       r.in(IntrinsicId::ObjectRemove, {r.L(0), r.L(1)}),
                       r.ret(r.Nil())})));
    r.add(r.iff(r.is(r.typ(r.L(0)), "array"),
                r.blk({r.call("$apop", {r.L(0), r.L(1), r.Bo(true)}),
                       r.ret(r.Nil())})));
    r.add(r.call("$exc", {r.S("TypeError"),
                          r.S("object does not support item deletion")}));
    r.add(r.ret(r.Nil()));
    r.finish("$delitem", 2, 2, {"c", "k"});
  }

  void rt_dpop() {
    RT r(*this);
    r.add(r.iff(r.has(r.L(0), r.L(1)),
                r.blk({r.set(4, r.idx(r.L(0), r.L(1))),
                       r.in(IntrinsicId::ObjectRemove, {r.L(0), r.L(1)}),
                       r.ret(r.L(4))})));
    r.add(r.iff(r.L(3), r.ret(r.L(2))));
    r.add(r.call("$exc", {r.S("KeyError"), r.call("$repr", {r.L(1)})}));
    r.add(r.ret(r.Nil()));
    r.finish("$dpop", 4, 5, {"d", "k", "dflt", "hasdflt", "v"});
  }

  void rt_alldigits() {
    RT r(*this);
    r.add(r.iff(r.bin(BinOp::Eq, r.len(r.L(0)), r.I(0)), r.ret(r.Bo(false))));
    r.add(r.set(1, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(1), r.len(r.L(0))),
               r.blk({r.set(2, r.in(IntrinsicId::StrByte, {r.L(0), r.L(1)})),
                      r.iff(r.either(r.bin(BinOp::Lt, r.L(2), r.I(48)),
                                     r.bin(BinOp::Gt, r.L(2), r.I(57))),
                            r.ret(r.Bo(false))),
                      r.set(1, r.bin(BinOp::Add, r.L(1), r.I(1)))})));
    r.add(r.ret(r.Bo(true)));
    r.finish("$alldigits", 1, 3, {"s", "i", "c"});
  }

  void rt_specerr() {
    RT r(*this);
    r.add(r.call("$exc", {r.S("ValueError"),
                          r.bin(BinOp::Add, r.S("unsupported format spec: "),
                                r.L(0))}));
    r.add(r.ret(r.Nil()));
    r.finish("$specerr", 1, 1, {"spec"});
  }

  // An f-string's format spec: a fill, an alignment, a width, and `.Nf`.
  // Everything else is refused rather than approximated, because `python3`
  // is the oracle and a near miss is a wrong answer.
  void rt_fmt() {
    RT r(*this);
    const auto ch = [&](NodeId str, NodeId i) {
      return r.in(IntrinsicId::StrSlice,
                  {str, i, r.bin(BinOp::Add, i, r.I(1))});
    };
    const auto is_align = [&](NodeId c) {
      return r.either(r.bin(BinOp::Eq, c, r.S("<")),
                      r.either(r.bin(BinOp::Eq, c, r.S(">")),
                               r.bin(BinOp::Eq, c, r.S("^"))));
    };
    r.add(r.iff(r.bin(BinOp::Eq, r.len(r.L(1)), r.I(0)),
                r.ret(r.call("$str", {r.L(0)}))));
    // A number right-aligns by default and everything else left-aligns,
    // which is Python's rule and not a choice this front end gets to make.
    r.add(r.set(3, r.S(" ")));
    r.add(r.set(4, r.iff(r.either(r.is(r.typ(r.L(0)), "int"),
                                  r.either(r.is(r.typ(r.L(0)), "double"),
                                           r.call("$isbig", {r.L(0)}))),
                         r.S(">"), r.S("<"))));
    r.add(r.set(5, r.I(0)));
    r.add(r.iff(r.both(r.bin(BinOp::Ge, r.len(r.L(1)), r.I(2)),
                       is_align(ch(r.L(1), r.I(1)))),
                r.blk({r.set(3, ch(r.L(1), r.I(0))),
                       r.set(4, ch(r.L(1), r.I(1))), r.set(5, r.I(2))}),
                r.iff(is_align(ch(r.L(1), r.I(0))),
                      r.blk({r.set(4, ch(r.L(1), r.I(0))),
                             r.set(5, r.I(1))}))));
    r.add(r.set(6, r.in(IntrinsicId::StrSlice,
                        {r.L(1), r.L(5), r.len(r.L(1))})));
    r.add(r.set(12, r.I(-1)));
    r.add(r.iff(
        r.both(r.bin(BinOp::Gt, r.len(r.L(6)), r.I(0)),
               r.bin(BinOp::Eq,
                     ch(r.L(6), r.bin(BinOp::Sub, r.len(r.L(6)), r.I(1))),
                     r.S("f"))),
        r.blk({r.set(6, r.in(IntrinsicId::StrSlice,
                             {r.L(6), r.I(0),
                              r.bin(BinOp::Sub, r.len(r.L(6)), r.I(1))})),
               r.set(11, r.call("$find", {r.L(6), r.S(".")})),
               r.iff(r.bin(BinOp::Lt, r.L(11), r.I(0)),
                     r.call("$specerr", {r.L(1)})),
               r.set(7, r.in(IntrinsicId::StrSlice,
                             {r.L(6), r.bin(BinOp::Add, r.L(11), r.I(1)),
                              r.len(r.L(6))})),
               r.iff(r.bin(BinOp::Eq, r.call("$alldigits", {r.L(7)}),
                           r.Bo(false)),
                     r.call("$specerr", {r.L(1)})),
               r.set(12, r.call("$toint", {r.L(7)})),
               r.set(6, r.in(IntrinsicId::StrSlice,
                             {r.L(6), r.I(0), r.L(11)}))})));
    r.add(r.iff(r.both(r.bin(BinOp::Gt, r.len(r.L(6)), r.I(0)),
                       r.bin(BinOp::Eq, r.call("$alldigits", {r.L(6)}),
                             r.Bo(false))),
                r.call("$specerr", {r.L(1)})));
    r.add(r.set(9, r.iff(r.bin(BinOp::Eq, r.len(r.L(6)), r.I(0)), r.I(0),
                         r.call("$toint", {r.L(6)}))));
    // A precision is the C library's exact decimal expansion, which is
    // what CPython's is too -- and the one thing here that is not a scan.
    r.add(r.set(2, r.iff(r.bin(BinOp::Ge, r.L(12), r.I(0)),
                         r.nat("ffmt", {r.call("$tofloat", {r.L(0)}), r.L(12)}),
                         r.call("$str", {r.L(0)}))));
    r.add(r.set(10, r.bin(BinOp::Sub, r.L(9), r.len(r.L(2)))));
    r.add(r.iff(r.bin(BinOp::Le, r.L(10), r.I(0)), r.ret(r.L(2))));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(4), r.S(">")),
                r.ret(r.bin(BinOp::Add,
                            r.call("$strmul", {r.L(3), r.L(10)}), r.L(2)))));
    r.add(r.iff(
        r.bin(BinOp::Eq, r.L(4), r.S("^")),
        r.blk({r.set(7, r.call("$str", {r.bin(BinOp::Div, r.L(10), r.I(2))})),
               r.set(8, r.bin(BinOp::Div, r.L(10), r.I(2))),
               r.ret(r.bin(BinOp::Add,
                           r.bin(BinOp::Add,
                                 r.call("$strmul", {r.L(3), r.L(8)}), r.L(2)),
                           r.call("$strmul",
                                  {r.L(3),
                                   r.bin(BinOp::Sub, r.L(10), r.L(8))})))})));
    r.add(r.ret(r.bin(BinOp::Add, r.L(2),
                      r.call("$strmul", {r.L(3), r.L(10)}))));
    r.finish("$fmt", 2, 13,
             {"v", "spec", "s", "fill", "align", "i", "rest", "t", "half",
              "w", "pad", "dot", "prec"});
  }

  void emit_runtime() {
    rt_typename(); rt_type(); rt_join(); rt_dget(); rt_tolist(); rt_toint();
    rt_intfail();
    rt_isbig(); rt_abs(); rt_tolimbs(); rt_mkbig(); rt_biglimbs();
    rt_bigsign(); rt_ucmp(); rt_uadd(); rt_usub(); rt_umul(); rt_bigadd();
    rt_bigmul(); rt_bstr(); rt_tofloat(); rt_neg();
    rt_add(); rt_sub(); rt_mul(); rt_fdiv(); rt_idiv(); rt_mod(); rt_pow();
    rt_cmp(); rt_eq(); rt_truthy();
    rt_fstr(); rt_str(); rt_repr(); rt_liststr(); rt_dictstr();
    rt_len(); rt_idx(); rt_setidx(); rt_slice(); rt_in(); rt_getattr();
    rt_setattr(); rt_iter(); rt_iternext(); rt_exc(); rt_isexc(); rt_range();
    rt_listadd(); rt_strmul(); rt_listmul();
    rt_acons(); rt_aext(); rt_rest(); rt_kwhas(); rt_hasname(); rt_kwrest();
    rt_kwcheck(); rt_kwmerge(); rt_missing(); rt_toomany();
    rt_clsfind(); rt_isname(); rt_isinstv(); rt_isinst(); rt_supercall();
    rt_excinit(); rt_noinit(); rt_dunder();
    rt_tuple(); rt_untup(); rt_istup(); rt_tupstr(); rt_unpack(); rt_items();
    rt_values(); rt_enumerate(); rt_zip(); rt_sorted(); rt_sum();
    rt_minmax(); rt_next();
    rt_isspace(); rt_split(); rt_strip(); rt_replace(); rt_find();
    rt_scount(); rt_startswith(); rt_endswith(); rt_apop(); rt_ainsert();
    rt_aindex(); rt_aremove(); rt_acount(); rt_areverse(); rt_asort();
    rt_dpop(); rt_delitem(); rt_alldigits(); rt_specerr(); rt_fmt();
  }

  // ==== Pass B: emit ======================================================

  // The index of a helper in the file-scope array build() fills in.
  static int32_t helper_slot(const std::string& name) {
    const auto& names = rt_names();
    for (size_t i = 0; i < names.size(); ++i) {
      if (names[i] == name) return static_cast<int32_t>(i);
    }
    coreir_rt::fail("unknown runtime helper " + name, 0, 0);
  }

  // A helper call reads the one closure that already exists rather than
  // building another: these are captureless, so a closure per function
  // *entry* -- which is what a per-function cell amounts to -- was one
  // allocation per call of every function that used one. fib(28) through
  // this front end made seventeen million of them.
  NodeId helper(FnCtx& ctx, const std::string& name,
                const std::vector<NodeId>& args, SrcPos p) {
    Builder b(m);
    return b.call_value(
        b.index(read_var(helpers_var, ctx, p),
                b.literal(helper_slot(name), p), p),
        args, p);
  }


  // File scope builds every helper's closure once, into the array above.
  void fill_helpers(FnCtx& ctx, std::vector<NodeId>& out, SrcPos p) {
    Builder b(m);
    std::vector<NodeId> vals;
    for (const std::string& n : rt_names()) {
      // A helper that takes captures is built at the site that has them
      // (RT::clos), never fetched from here, so its slot stays nil.
      const size_t idx = static_cast<size_t>(rt.at(n));
      vals.push_back(m.funcs[idx].num_captures == 0
                         ? b.make_closure(rt.at(n), empty_cmap, p)
                         : b.nil_literal(p));
    }
    out.push_back(write_var(helpers_var, b.array_lit(vals, p), ctx, p));
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

  // A Python binding is function-scoped, not block-scoped, so a block is a
  // plain Block and every local belongs to the one Scope the function has.
  // The consequence a sample can see is late binding: a closure made in a
  // loop shares the loop's variable rather than getting one per iteration
  // -- which is why CellFresh runs once, at the function's entry, and not
  // at each `for`.
  NodeId emit_block(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    std::vector<NodeId> out;
    for (const auto& s : a.nodes) out.push_back(emit_stmt(*s, ctx));
    return b.block(out, pos_of(a));
  }

  // `a, b` in value position -- a return, or the right of an assignment --
  // is a tuple; one expression alone is itself.
  NodeId emit_exprs(const Ast* es, FnCtx& ctx, SrcPos p) {
    Builder b(m);
    if (es == nullptr) return b.nil_literal(p);
    if (es->nodes.size() == 1) return emit_expr(*es->nodes[0], ctx);
    std::vector<NodeId> items;
    for (const auto& c : es->nodes) items.push_back(emit_expr(*c, ctx));
    return helper(ctx, "$tuple", {b.array_lit(items, p)}, p);
  }

  // The positional-only path, for the builtins -- which are emitted inline
  // and so are not reached through the calling convention below.
  std::vector<NodeId> emit_args(const Ast* args, FnCtx& ctx) {
    std::vector<NodeId> out;
    if (args == nullptr || args->tag != "args"_) return out;
    for (const auto& c : args->nodes) {
      if (c->tag == "kwarg"_ || c->tag == "splat"_ || c->tag == "kwsplat"_) {
        fail(*c, "a builtin takes positional arguments only here");
      }
      out.push_back(emit_expr(*c, ctx));
    }
    return out;
  }

  // -- The calling convention, at the call site ---------------------------
  //
  // Two arguments, always: the positional array and the keyword object.
  // `front` is what goes in front of the source's own arguments -- a
  // receiver for a method, nothing for a plain call.
  std::pair<NodeId, NodeId> emit_callargs(const Ast* args, FnCtx& ctx,
                                          const std::vector<NodeId>& front,
                                          SrcPos p) {
    Builder b(m);
    std::vector<const Ast*> pos, kw;
    bool splat = false;
    bool kwsplat = false;
    if (args != nullptr && args->tag == "args"_) {
      for (const auto& c : args->nodes) {
        if (c->tag == "kwarg"_) {
          kw.push_back(c.get());
        } else if (c->tag == "kwsplat"_) {
          kw.push_back(c.get());
          kwsplat = true;
        } else {
          if (c->tag == "splat"_) splat = true;
          pos.push_back(c.get());
        }
      }
    }
    NodeId A;
    if (!splat) {
      std::vector<NodeId> items(front);
      for (const Ast* e : pos) items.push_back(emit_expr(*e, ctx));
      A = b.array_lit(items, p);
    } else {
      // A `*xs` decides a count at run time, so the array is built rather
      // than written -- which is the whole reason this convention exists.
      const int32_t t = ctx.alloc_local("$args");
      const NodeId T = b.varref(VarKind::Local, t, p);
      std::vector<NodeId> steps{
          b.assign(VarKind::Local, t, b.array_lit(front, p), p)};
      for (const Ast* e : pos) {
        steps.push_back(
            e->tag == "splat"_
                ? helper(ctx, "$aext", {T, emit_expr(*e->nodes[0], ctx)}, p)
                : b.intrinsic(IntrinsicId::ArrayPush,
                              {T, emit_expr(*e, ctx)}, p));
      }
      steps.push_back(T);
      A = b.block(steps, p);
    }
    NodeId K;
    if (kw.empty()) {
      K = b.nil_literal(p);
    } else if (!kwsplat) {
      std::vector<std::pair<NodeId, NodeId>> kvs;
      for (const Ast* e : kw) {
        kvs.emplace_back(b.str_literal(std::string(e->nodes[0]->token), p),
                         emit_expr(*e->nodes[1], ctx));
      }
      K = b.object_lit(kvs, p);
    } else {
      const int32_t t = ctx.alloc_local("$kw");
      const NodeId T = b.varref(VarKind::Local, t, p);
      std::vector<NodeId> steps{
          b.assign(VarKind::Local, t, b.object_lit({}, p), p)};
      for (const Ast* e : kw) {
        steps.push_back(
            e->tag == "kwsplat"_
                ? helper(ctx, "$kwmerge", {T, emit_expr(*e->nodes[0], ctx)}, p)
                : b.set_index(T, b.str_literal(std::string(e->nodes[0]->token),
                                               p),
                              emit_expr(*e->nodes[1], ctx), p));
      }
      steps.push_back(T);
      K = b.block(steps, p);
    }
    return {A, K};
  }

  NodeId emit_pycall(NodeId callee, const Ast* args, FnCtx& ctx,
                     const std::vector<NodeId>& front, SrcPos p) {
    Builder b(m);
    const auto [A, K] = emit_callargs(args, ctx, front, p);
    return b.call_value(callee, {A, K}, p);
  }

  // The def-time half of a default: computed where the `def` stands, into a
  // cell made fresh right there, so two closures built in a loop do not
  // share one box.
  void emit_defaults(int32_t g, FnCtx& ctx, std::vector<NodeId>& out,
                     SrcPos p) {
    Builder b(m);
    for (const ParamInfo& pi : fns[static_cast<size_t>(g)].params) {
      if (pi.kind != ParamInfo::Default) continue;
      const auto [k, i] = access(ctx.fn, pi.def_var);
      out.push_back(b.cell_fresh(i, p));
      out.push_back(b.assign(k, i, emit_expr(*pi.def, ctx), p));
    }
  }

  bool has_defaults(int32_t g) const {
    for (const ParamInfo& pi : fns[static_cast<size_t>(g)].params) {
      if (pi.kind == ParamInfo::Default) return true;
    }
    return false;
  }

  // -- Statements ---------------------------------------------------------
  NodeId emit_stmt(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    switch (a.tag) {
      case "passstmt"_:
        return b.block({}, p);
      case "simpleline"_:
      case "block"_:
        return emit_block(a, ctx);
      case "funcdef"_: {
        const int32_t g = fn_of.at(&a);
        std::vector<NodeId> out;
        emit_defaults(g, ctx, out, p);
        NodeId value = emit_closure(g, ctx, p);
        // `@a @b def f` is `f = a(b(f))`: the nearest decorator runs first,
        // so they are applied bottom-up.
        if (a.nodes[0]->tag == "decorators"_) {
          const Ast& ds = *a.nodes[0];
          for (size_t k = ds.nodes.size(); k-- > 0;) {
            value = b.call_value(emit_expr(*ds.nodes[k]->nodes[0], ctx),
                                 {b.array_lit({value}, p), b.nil_literal(p)},
                                 p);
          }
        }
        out.push_back(write_var(decl_of.at(fn_ident(a)), value, ctx, p));
        return b.block(out, p);
      }
      case "classdef"_:
        return emit_class(a, ctx);
      case "breakstmt"_:
        return b.make_break(p);
      case "contstmt"_:
        return b.make_continue(p);
      case "returnstmt"_:
        return b.make_return(
            emit_exprs(a.nodes.empty() ? nullptr : a.nodes[0].get(), ctx, p),
            p);
      case "raisestmt"_:
        return b.make_throw(emit_expr(*a.nodes[0], ctx), p);
      case "yieldone"_:
        return b.make_yield(emit_exprs(a.nodes[0].get(), ctx, p), p);
      case "yieldfrom"_: {
        // `yield from it` is the loop it stands for. What the sub-generator
        // *returns* is dropped -- see README.md.
        const int32_t it = ctx.alloc_local("$it");
        const int32_t st = ctx.alloc_local("$step");
        const NodeId I = b.varref(VarKind::Local, it, p);
        const NodeId S = b.varref(VarKind::Local, st, p);
        return b.block(
            {b.assign(VarKind::Local, it,
                      helper(ctx, "$iter", {emit_expr(*a.nodes[0], ctx)}, p),
                      p),
             b.make_while(
                 b.bool_literal(true, p),
                 b.block({b.assign(VarKind::Local, st,
                                   helper(ctx, "$iternext", {I}, p), p),
                          b.make_if(b.index(S, b.str_literal("done", p), p),
                                    b.make_break(p), NodeId{}, p),
                          b.make_yield(
                              b.index(S, b.str_literal("value", p), p), p)},
                         p),
                 p)},
            p);
      }
      case "globalstmt"_:
      case "nonlocalstmt"_:
        return b.block({}, p);
      case "delstmt"_: {
        std::vector<NodeId> out;
        for (const auto& c : a.nodes) out.push_back(emit_del(*c, ctx, p));
        return b.block(out, p);
      }
      case "assertstmt"_:
        return b.make_if(
            b.binary(BinOp::Eq,
                     helper(ctx, "$truthy", {emit_expr(*a.nodes[0], ctx)}, p),
                     b.bool_literal(false, p), p),
            helper(ctx, "$exc",
                   {b.str_literal("AssertionError", p),
                    a.nodes.size() > 1
                        ? helper(ctx, "$str", {emit_expr(*a.nodes[1], ctx)}, p)
                        : b.str_literal("", p)},
                   p),
            NodeId{}, p);
      case "exprstmt"_:
        return emit_expr(*a.nodes[0], ctx);
      case "ifstmt"_:
        return emit_if(a, ctx);
      case "whilestmt"_:
        return b.make_while(
            helper(ctx, "$truthy", {emit_expr(*a.nodes[0], ctx)}, p),
            emit_block(*a.nodes[1], ctx), p);
      case "forstmt"_:
        return emit_for(a, ctx);
      case "trystmt"_:
        return emit_try(a, ctx);
      case "withstmt"_:
        return emit_with(a, ctx);
      case "assign"_:
        return emit_assign(a, ctx);
      default:
        fail(a, "cannot execute " + a.name);
    }
  }

  // `del c[k]`. Deleting a bare name would have to unbind it, which this
  // IR has no way to say, so it is refused rather than approximated.
  NodeId emit_del(const Ast& t, FnCtx& ctx, SrcPos p) {
    if (t.tag != "postfix"_ || t.nodes.size() < 2 ||
        t.nodes.back()->tag != "indexsfx"_) {
      fail(t, "only `del container[key]` is supported here");
    }
    const Ast& sub = *t.nodes.back()->nodes[0];
    if (sub.tag == "sliceboth"_ || sub.tag == "slicelo"_ ||
        sub.tag == "slicehi"_ || sub.tag == "sliceall"_) {
      fail(sub, "deleting a slice is not supported here");
    }
    return helper(ctx, "$delitem",
                  {emit_postfix(t, t.nodes.size() - 1, ctx),
                   emit_expr(sub, ctx)},
                  p);
  }

  NodeId emit_if(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    std::vector<std::pair<NodeId, NodeId>> arms;
    arms.emplace_back(
        helper(ctx, "$truthy", {emit_expr(*a.nodes[0], ctx)}, p),
        emit_block(*a.nodes[1], ctx));
    NodeId els;
    for (size_t i = 2; i < a.nodes.size(); ++i) {
      const Ast& c = *a.nodes[i];
      if (c.tag == "elifpart"_) {
        arms.emplace_back(
            helper(ctx, "$truthy", {emit_expr(*c.nodes[0], ctx)}, p),
            emit_block(*c.nodes[1], ctx));
      } else {
        els = emit_block(*c.nodes[0], ctx);
      }
    }
    for (size_t i = arms.size(); i-- > 0;) {
      els = b.make_if(arms[i].first, arms[i].second, els, p);
    }
    return els;
  }

  NodeId emit_for(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    const int32_t it = ctx.alloc_local("$it");
    const int32_t st = ctx.alloc_local("$step");
    const NodeId I = b.varref(VarKind::Local, it, p);
    const NodeId S = b.varref(VarKind::Local, st, p);
    const Ast& tg = *a.nodes[0];
    std::vector<NodeId> loop{
        b.assign(VarKind::Local, st, helper(ctx, "$iternext", {I}, p), p),
        b.make_if(b.index(S, b.str_literal("done", p), p), b.make_break(p),
                  NodeId{}, p)};
    const NodeId value = b.index(S, b.str_literal("value", p), p);
    if (tg.nodes.size() == 1) {
      loop.push_back(write_var(decl_of.at(tg.nodes[0].get()), value, ctx, p));
    } else {
      // `for k, v in d.items()`: the same unpack an assignment does.
      const int32_t u = ctx.alloc_local("$unp");
      const NodeId U = b.varref(VarKind::Local, u, p);
      loop.push_back(b.assign(
          VarKind::Local, u,
          helper(ctx, "$unpack",
                 {value, b.literal(static_cast<int64_t>(tg.nodes.size()), p)},
                 p),
          p));
      for (size_t k = 0; k < tg.nodes.size(); ++k) {
        loop.push_back(write_var(decl_of.at(tg.nodes[k].get()),
                                 b.index(U, b.literal(static_cast<int64_t>(k),
                                                      p),
                                         p),
                                 ctx, p));
      }
    }
    loop.push_back(emit_block(*a.nodes[2], ctx));
    return b.block(
        {b.assign(VarKind::Local, it,
                  helper(ctx, "$iter", {emit_expr(*a.nodes[1], ctx)}, p), p),
         b.make_while(b.bool_literal(true, p), b.block(loop, p), p)},
        p);
  }

  // try/except/finally. `finally` is a Defer inside the Scope wrapping the
  // try, so it runs however the block is left; `except NAME` re-raises
  // what it does not match, which is how a subset with no exception
  // hierarchy gets the selective behaviour right.
  NodeId emit_try(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    std::vector<const Ast*> excs;
    const Ast* fin = nullptr;
    for (size_t i = 1; i < a.nodes.size(); ++i) {
      if (a.nodes[i]->tag == "exceptpart"_) {
        excs.push_back(a.nodes[i].get());
      } else if (a.nodes[i]->tag == "finallypart"_) {
        fin = a.nodes[i].get();
      }
    }
    const int32_t slot = ctx.alloc_local("$exc");
    const NodeId E = b.varref(VarKind::Local, slot, p);
    std::vector<NodeId> out;
    if (fin != nullptr) {
      out.push_back(b.make_defer(emit_closure(fn_of.at(fin), ctx, p), p));
    }
    const NodeId body = emit_block(*a.nodes[0], ctx);
    // The clauses are tried in order and what none of them claims is
    // re-thrown: one nested If per clause, and nothing else.
    NodeId handler = b.make_throw(E, p);
    for (size_t k = excs.size(); k-- > 0;) {
      const Ast& ex = *excs[k];
      const bool bare = ex.nodes[0]->tag == "block"_;
      std::vector<NodeId> hs;
      if (!bare && ex.nodes.size() > 2) {
        hs.push_back(write_var(decl_of.at(ex.nodes[1].get()), E, ctx, p));
      }
      hs.push_back(emit_block(*ex.nodes.back(), ctx));
      if (bare) {
        handler = b.block(hs, p);
        continue;
      }
      // A builtin exception is not a class here, so it travels as its
      // name; a class of the program's own travels as its value.
      const Ast& caught = *ex.nodes[0];
      const NodeId cls = ref_of.count(&caught)
                             ? read_var(ref_of.at(&caught), ctx, p)
                             : b.str_literal(std::string(caught.token), p);
      handler = b.make_if(helper(ctx, "$isexc", {E, cls}, p),
                          b.block(hs, p), handler, p);
    }
    out.push_back(b.make_try(slot, body, handler, p));
    const int32_t n = ctx.next_local;
    return b.scope(n, n, b.block(out, p), p);
  }

  // `with cm as x:` -- the context manager goes into a cell so the exit
  // thunk can capture it, `__enter__` runs, and `__exit__` is a Defer:
  // "however it exits -- falling through, Break, Continue, Return, or an
  // unwinding throw", which is exactly what a context manager promises.
  NodeId emit_with(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    const int32_t cell = ctx.next_cell++;
    const NodeId C = b.varref(VarKind::Cell, cell, p);
    std::vector<NodeId> out{
        b.cell_fresh(cell, p),
        b.assign(VarKind::Cell, cell, emit_expr(*a.nodes[0], ctx), p)};
    const NodeId entered = b.call_value(
        helper(ctx, "$getattr", {C, b.str_literal("__enter__", p)}, p),
        {b.array_lit({C}, p), b.nil_literal(p)}, p);
    if (a.nodes.size() > 2 && a.nodes[1]->tag == "ident"_) {
      out.push_back(
          write_var(decl_of.at(a.nodes[1].get()), entered, ctx, p));
    } else {
      out.push_back(entered);
    }
    const int32_t g = fn_of.at(&a);
    emit_exit_thunk(g);
    std::vector<CaptureSrc> cs{{VarKind::Cell, cell}};
    const int32_t cm = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back(cs);
    out.push_back(b.make_defer(
        b.make_closure(fns[static_cast<size_t>(g)].index, cm, p), p));
    out.push_back(emit_block(*a.nodes.back(), ctx));
    const int32_t n = ctx.next_local;
    return b.scope(n, n, b.block(out, p), p);
  }

  void emit_exit_thunk(int32_t g) {
    Builder b(m);
    const SrcPos p{0, 0};
    const NodeId C = b.varref(VarKind::Capture, 0, p);
    Func f;
    f.name = "<exit>";
    f.num_params = 0;
    f.num_locals = 1;
    f.local_names = {"$cell"};
    f.num_captures = 1;
    f.capture_names = {"cm"};
    f.lenient_arity = true;
    // __exit__(self, None, None, None): this subset passes no exception
    // information and ignores the result, so a context manager cannot
    // suppress one -- see README.md.
    f.body = b.scope(
        0, 1,
        b.call_value(
            b.call_value(b.make_closure(rt.at("$getattr"), empty_cmap, p),
                         {C, b.str_literal("__exit__", p)}, p),
            {b.array_lit({C, b.nil_literal(p), b.nil_literal(p),
                          b.nil_literal(p)},
                         p),
             b.nil_literal(p)},
            p),
        p);
    m.funcs[static_cast<size_t>(fns[static_cast<size_t>(g)].index)] =
        std::move(f);
  }

  NodeId emit_assign(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    const std::string op(a.nodes[1]->token);
    const Ast& targets = *a.nodes[0];
    const Ast& values = *a.nodes[2];
    // `a, b = b, a + b`: every value is computed into a slot before any of
    // them is stored, which is what makes the swap a swap.
    // `a, b = expr`: one value, taken apart.
    if (targets.nodes.size() > 1 && values.nodes.size() == 1) {
      if (op != "=") fail(a, "cannot augment a multiple assignment");
      const int32_t u = ctx.alloc_local("$unp");
      const NodeId U = b.varref(VarKind::Local, u, p);
      std::vector<NodeId> out{b.assign(
          VarKind::Local, u,
          helper(ctx, "$unpack",
                 {emit_expr(*values.nodes[0], ctx),
                  b.literal(static_cast<int64_t>(targets.nodes.size()), p)},
                 p),
          p)};
      for (size_t i = 0; i < targets.nodes.size(); ++i) {
        out.push_back(emit_store(
            *targets.nodes[i],
            b.index(U, b.literal(static_cast<int64_t>(i), p), p), ctx, p));
      }
      return b.block(out, p);
    }
    // `a, b = c, d`: every value is computed before any is stored. One
    // target and several values is not this -- it is a tuple.
    if (targets.nodes.size() > 1) {
      if (op != "=" || targets.nodes.size() != values.nodes.size()) {
        fail(a, "this multiple assignment is not supported here");
      }
      std::vector<NodeId> out;
      std::vector<int32_t> temps;
      for (const auto& v : values.nodes) {
        const int32_t t = ctx.alloc_local("$tmp");
        temps.push_back(t);
        out.push_back(b.assign(VarKind::Local, t, emit_expr(*v, ctx), p));
      }
      for (size_t i = 0; i < targets.nodes.size(); ++i) {
        out.push_back(emit_store(*targets.nodes[i],
                                 b.varref(VarKind::Local, temps[i], p), ctx,
                                 p));
      }
      return b.block(out, p);
    }
    const Ast& target = *targets.nodes[0];
    const auto combine = [&](NodeId cur) -> NodeId {
      const NodeId v = emit_exprs(&values, ctx, p);
      if (op == "=") return v;
      if (op == "+=") return helper(ctx, "$add", {cur, v}, p);
      if (op == "-=") return helper(ctx, "$sub", {cur, v}, p);
      if (op == "*=") return helper(ctx, "$mul", {cur, v}, p);
      if (op == "//=") return helper(ctx, "$idiv", {cur, v}, p);
      if (op == "/=") return helper(ctx, "$fdiv", {cur, v}, p);
      return helper(ctx, "$mod", {cur, v}, p);
    };

    if (target.tag == "ident"_) {
      const auto it = decl_of.find(&target);
      const int32_t v = it != decl_of.end() ? it->second : ref_of.at(&target);
      return write_var(v, combine(read_var(v, ctx, p)), ctx, p);
    }
    if (target.tag != "postfix"_ || target.nodes.size() < 2) {
      fail(target, "cannot assign to this expression");
    }
    const Ast& last = *target.nodes.back();
    const bool attr = last.tag == "dotsfx"_;
    if (!attr && last.tag != "indexsfx"_) {
      fail(last, "cannot assign to this expression");
    }
    const NodeId key =
        attr ? b.str_literal(std::string(last.nodes[0]->token), p)
             : emit_expr(*last.nodes[0], ctx);
    const int32_t tr = ctx.alloc_local("$recv");
    const int32_t tk = ctx.alloc_local("$key");
    const NodeId recv = emit_postfix(target, target.nodes.size() - 1, ctx);
    const NodeId R = b.varref(VarKind::Local, tr, p);
    const NodeId K = b.varref(VarKind::Local, tk, p);
    const NodeId cur =
        op == "=" ? b.nil_literal(p)
                  : helper(ctx, attr ? "$getattr" : "$idx", {R, K}, p);
    return b.block({b.assign(VarKind::Local, tr, recv, p),
                    b.assign(VarKind::Local, tk, key, p),
                    helper(ctx, attr ? "$setattr" : "$setidx",
                           {R, K, combine(cur)}, p)},
                   p);
  }

  NodeId emit_store(const Ast& target, NodeId value, FnCtx& ctx, SrcPos p) {
    Builder b(m);
    if (target.tag == "ident"_) {
      const auto it = decl_of.find(&target);
      const int32_t v = it != decl_of.end() ? it->second : ref_of.at(&target);
      return write_var(v, value, ctx, p);
    }
    if (target.tag != "postfix"_ || target.nodes.size() < 2) {
      fail(target, "cannot assign to this expression");
    }
    const Ast& last = *target.nodes.back();
    const bool attr = last.tag == "dotsfx"_;
    const NodeId key =
        attr ? b.str_literal(std::string(last.nodes[0]->token), p)
             : emit_expr(*last.nodes[0], ctx);
    return helper(ctx, attr ? "$setattr" : "$setidx",
                  {emit_postfix(target, target.nodes.size() - 1, ctx), key,
                   value},
                  p);
  }

  // class C: -- a method table in a cell, and a constructor closure over
  // it, exactly as examples/mini-culebra does. Python's methods declare
  // `self` themselves, so there is no implicit parameter here.
  NodeId emit_class(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    const std::string cname(a.nodes[0]->token);
    const int32_t v = decl_of.at(a.nodes[0].get());
    const auto& methods = class_of.at(&a);

    // A method's defaults are evaluated when the `class` statement runs,
    // which is where the `def` stands -- so they come first, before any
    // MakeClosure captures the cells they live in.
    std::vector<NodeId> out;
    for (const auto& [name, g] : methods) {
      (void)name;
      emit_defaults(g, ctx, out, p);
    }
    const ClassInfo& ci = class_info.at(&a);
    std::vector<std::pair<NodeId, NodeId>> kvs{
        {b.str_literal(kNameKey, p), b.str_literal(cname, p)}};
    if (ci.base_var >= 0) {
      kvs.emplace_back(b.str_literal(kBaseKey, p),
                       read_var(ci.base_var, ctx, p));
    }
    if (!ci.root.empty()) {
      kvs.emplace_back(b.str_literal(kRootKey, p),
                       b.str_literal(ci.root, p));
    }
    for (const auto& [name, g] : methods) {
      kvs.emplace_back(b.str_literal("\x02" + name, p),
                       emit_closure(g, ctx, p));
    }
    // The table lives in a cell -- the constructor captures it, and so does
    // any method that says `super()`.
    const auto [ck, cell] = access(ctx.fn, ci.table_var);
    (void)ck;
    const NodeId T = b.varref(VarKind::Cell, cell, p);
    const int32_t ctor = new_fn(ctx.fn, cname);
    fns[static_cast<size_t>(ctor)].index = static_cast<int32_t>(m.funcs.size());
    m.funcs.push_back({});
    emit_ctor(ctor, cname, ci.is_exc);
    std::vector<CaptureSrc> cs{{VarKind::Cell, cell}};
    const int32_t cm = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back(cs);
    out.push_back(b.cell_fresh(cell, p));
    out.push_back(b.assign(VarKind::Cell, cell, b.object_lit(kvs, p), p));
    // The class value is its constructor closure, and the table keeps a
    // reference to it: that is the identity `isinstance` and `except`
    // compare, since there is nothing else a class could be.
    const int32_t t = ctx.alloc_local("$class");
    const NodeId K = b.varref(VarKind::Local, t, p);
    out.push_back(b.assign(
        VarKind::Local, t,
        b.make_closure(fns[static_cast<size_t>(ctor)].index, cm, p), p));
    out.push_back(b.set_index(T, b.str_literal(kIdKey, p), K, p));
    out.push_back(write_var(v, K, ctx, p));
    return b.block(out, p);
  }

  // Calling a class is calling this: it makes the instance and hands the
  // very arguments it was given straight to `__init__`, with the instance
  // in front. Because both take the convention, it does not have to know
  // what `__init__` declared.
  void emit_ctor(int32_t g, const std::string& cname, bool is_exc) {
    Builder b(m);
    const SrcPos p{0, 0};
    const NodeId C = b.varref(VarKind::Capture, 0, p);
    const NodeId A = b.varref(VarKind::Local, 0, p);
    const NodeId K = b.varref(VarKind::Local, 1, p);
    const NodeId O = b.varref(VarKind::Local, 2, p);
    const NodeId F = b.varref(VarKind::Local, 3, p);
    const auto rtc = [&](const std::string& name,
                         const std::vector<NodeId>& as) {
      return b.call_value(b.make_closure(rt.at(name), empty_cmap, p), as, p);
    };
    // `__init__` may be the base's, so it is looked up rather than indexed.
    std::vector<NodeId> body{
        b.assign(VarKind::Local, 2,
                 b.object_lit({{b.str_literal(kClassKey, p), C}}, p), p),
        b.assign(VarKind::Local, 3,
                 rtc("$clsfind", {C, b.str_literal("\x02__init__", p)}), p),
        b.make_if(
            b.binary(BinOp::Ne,
                     b.intrinsic(IntrinsicId::TypeOf, {F}, p),
                     b.str_literal("nil", p), p),
            b.call_value(F, {rtc("$acons", {O, A}), K}, p),
            is_exc ? rtc("$excinit", {O, A})
                   : rtc("$noinit", {b.str_literal(cname, p), A}),
            p)};
    body.push_back(b.make_return(O, p));

    Func f;
    f.name = cname;
    f.num_params = 2;
    f.num_locals = 4;
    f.local_names = {"$a", "$k", "$self", "$init"};
    f.num_captures = 1;
    f.capture_names = {cname};
    f.lenient_arity = true;
    f.body = b.scope(0, 0, b.block(body, p), p);
    m.funcs[static_cast<size_t>(fns[static_cast<size_t>(g)].index)] =
        std::move(f);
  }

  // -- Expressions --------------------------------------------------------
  NodeId emit_expr(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    switch (a.tag) {
      case "number"_: {
        std::string t(a.token);
        std::string digits;
        for (const char c : t) {
          if (c != '_') digits.push_back(c);
        }
        return int_literal(digits, p);
      }
      case "float"_:
        return b.double_literal(
            std::strtod(std::string(a.token).c_str(), nullptr), p);
      case "string"_:
        return b.str_literal(unescape(std::string(a.token)), p);
      case "literal"_: {
        const std::string t(a.token);
        if (t == "True") return b.bool_literal(true, p);
        if (t == "False") return b.bool_literal(false, p);
        return b.nil_literal(p);
      }
      case "ident"_: {
        const auto it = ref_of.find(&a);
        if (it != ref_of.end()) return read_var(it->second, ctx, p);
        // A builtin in value position -- `sorted(xs, key=len)` -- is not a
        // call site, so there is nothing to emit inline. It gets a function
        // of its own instead, made once and shared.
        const std::string g(a.token);
        if (is_value_builtin(g)) {
          return b.make_closure(builtin_func(g), empty_cmap, p);
        }
        fail(a, "'" + g + "' must be called here");
      }
      case "fstring"_: {
        // One concatenation per piece. `$fmt` is what applies a spec, and
        // it is called even for an empty one so that a number and a string
        // reach `$str` the same way.
        NodeId acc = b.str_literal("", p);
        for (const auto& c : a.nodes) {
          NodeId piece;
          if (c->tag == "fexpr"_) {
            NodeId v = emit_expr(*c->nodes[0], ctx);
            std::string spec;
            bool as_repr = false;
            for (size_t i = 1; i < c->nodes.size(); ++i) {
              if (c->nodes[i]->tag == "fconv"_) {
                as_repr = std::string(c->nodes[i]->token) == "r";
              } else {
                spec = std::string(c->nodes[i]->token);
              }
            }
            if (as_repr) v = helper(ctx, "$repr", {v}, p);
            piece = helper(ctx, "$fmt", {v, b.str_literal(spec, p)}, p);
          } else {
            piece = b.str_literal(unescape_ftext(std::string(c->token)), p);
          }
          acc = b.binary(BinOp::Add, acc, piece, p);
        }
        return acc;
      }
      case "paren"_:
        return emit_expr(*a.nodes[0], ctx);
      case "listcomp"_:
      case "dictcomp"_:
      case "gencomp"_:
      case "bargen"_:
        return b.call_value(emit_closure(fn_of.at(&a), ctx, p),
                            {b.array_lit({}, p), b.nil_literal(p)}, p);
      case "tuplelit"_: {
        std::vector<NodeId> items;
        for (const auto& c : a.nodes) items.push_back(emit_expr(*c, ctx));
        return helper(ctx, "$tuple", {b.array_lit(items, p)}, p);
      }
      case "lambda"_: {
        const int32_t g = fn_of.at(&a);
        if (!has_defaults(g)) return emit_closure(g, ctx, p);
        std::vector<NodeId> out;
        emit_defaults(g, ctx, out, p);
        out.push_back(emit_closure(g, ctx, p));
        return b.block(out, p);
      }
      case "negexp"_:
        return helper(ctx, "$neg", {emit_expr(*a.nodes[0], ctx)}, p);
      case "notop"_:
        return b.binary(BinOp::Eq,
                        helper(ctx, "$truthy", {emit_expr(*a.nodes[0], ctx)},
                               p),
                        b.bool_literal(false, p), p);
      case "powexp"_:
        return helper(ctx, "$pow",
                      {emit_expr(*a.nodes[0], ctx),
                       emit_expr(*a.nodes[1]->nodes[0], ctx)},
                      p);
      case "ternary"_:
        // `a if c else b`: the condition is the middle child.
        return b.make_if(
            helper(ctx, "$truthy", {emit_expr(*a.nodes[1], ctx)}, p),
            emit_expr(*a.nodes[0], ctx), emit_expr(*a.nodes[2], ctx), p);
      case "orexp"_:
      case "andexp"_: {
        const bool is_or = a.tag == "orexp"_;
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i < a.nodes.size(); ++i) {
          const int32_t t = ctx.alloc_local(is_or ? "$or" : "$and");
          const NodeId rhs = emit_expr(*a.nodes[i], ctx);
          const NodeId keep = b.varref(VarKind::Local, t, p);
          acc = b.block({b.assign(VarKind::Local, t, acc, p),
                         b.make_if(helper(ctx, "$truthy", {keep}, p),
                                   is_or ? keep : rhs, is_or ? rhs : keep, p)},
                        p);
        }
        return acc;
      }
      // Comparisons *chain*: `a < b <= c` means `a < b and b <= c` with
      // `b` evaluated once, which no left fold can express -- the fold
      // would compare a bool with c. Each operand lands in a slot, and
      // each link after the first is guarded by the one before it, so the
      // short-circuit is Python's too.
      case "cmpexp"_: {
        const size_t links = (a.nodes.size() - 1) / 2;
        std::vector<int32_t> slots;
        std::vector<NodeId> pre;
        for (size_t k = 0; k < a.nodes.size(); k += 2) {
          const int32_t t = ctx.alloc_local("$cmp");
          slots.push_back(t);
          pre.push_back(
              b.assign(VarKind::Local, t, emit_expr(*a.nodes[k], ctx), p));
        }
        const auto link = [&](size_t k) {
          const Ast& op = *a.nodes[k * 2 + 1];
          std::string t(op.token);
          if (t.rfind("not", 0) == 0) t = "not in";
          if (t.rfind("is", 0) == 0) {
            t = t.find("not") != std::string::npos ? "is not" : "is";
          }
          const SrcPos op_p = pos_of(op);
          const NodeId lhs = b.varref(VarKind::Local, slots[k], p);
          const NodeId rhs = b.varref(VarKind::Local, slots[k + 1], p);
          if (t == "==") return helper(ctx, "$eq", {lhs, rhs}, op_p);
          if (t == "!=") {
            return b.binary(BinOp::Eq, helper(ctx, "$eq", {lhs, rhs}, op_p),
                            b.bool_literal(false, p), op_p);
          }
          // `is` is identity, which `Same` answers.
          if (t == "is" || t == "is not") {
            const NodeId same =
                b.intrinsic(IntrinsicId::Same, {lhs, rhs}, op_p);
            return t == "is" ? same
                             : b.binary(BinOp::Eq, same,
                                        b.bool_literal(false, p), op_p);
          }
          if (t == "in") return helper(ctx, "$in", {lhs, rhs}, op_p);
          if (t == "not in") {
            return b.binary(BinOp::Eq, helper(ctx, "$in", {lhs, rhs}, op_p),
                            b.bool_literal(false, p), op_p);
          }
          const BinOp o = t == "<"    ? BinOp::Lt
                          : t == "<=" ? BinOp::Le
                          : t == ">"  ? BinOp::Gt
                                      : BinOp::Ge;
          return b.binary(o, helper(ctx, "$cmp", {lhs, rhs}, op_p),
                          b.literal(0, p), op_p);
        };
        NodeId chain = link(links - 1);
        for (size_t k = links - 1; k-- > 0;) {
          chain = b.make_if(link(k), chain, b.bool_literal(false, p), p);
        }
        pre.push_back(chain);
        return b.block(pre, p);
      }
      case "addexp"_:
      case "mulexp"_: {
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i + 1 < a.nodes.size(); i += 2) {
          const Ast& op = *a.nodes[i];
          const std::string t(op.token);
          const NodeId rhs = emit_expr(*a.nodes[i + 1], ctx);
          const SrcPos op_p = pos_of(op);
          const char* h = t == "+"    ? "$add"
                          : t == "-"  ? "$sub"
                          : t == "*"  ? "$mul"
                          : t == "//" ? "$idiv"
                          : t == "/"  ? "$fdiv"
                                      : "$mod";
          acc = helper(ctx, h, {acc, rhs}, op_p);
        }
        return acc;
      }
      case "listlit"_: {
        std::vector<NodeId> items;
        for (const auto& c : a.nodes) items.push_back(emit_expr(*c, ctx));
        return b.array_lit(items, p);
      }
      case "dictlit"_: {
        const int32_t t = ctx.alloc_local("$dict");
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
      case "postfix"_:
        return emit_postfix(a, a.nodes.size(), ctx);
      default:
        fail(a, "cannot evaluate " + a.name);
    }
  }

  // An integer literal too wide for an int64 becomes a bignum constant --
  // built here, at bind time, in the same limb form the runtime uses.
  NodeId int_literal(const std::string& digits, SrcPos p) {
    Builder b(m);
    errno = 0;
    char* end = nullptr;
    const long long v = std::strtoll(digits.c_str(), &end, 10);
    if (errno == 0 && end == digits.c_str() + digits.size()) {
      return b.literal(static_cast<int64_t>(v), p);
    }
    // Long division of the decimal string by 10^9, one pass per limb --
    // the same base the runtime works in, so the constant is already in
    // the shape $bigadd and $bigmul expect.
    std::vector<NodeId> limbs;
    std::string rest = digits;
    while (rest != "0") {
      std::string q;
      int64_t rem = 0;
      for (const char c : rest) {
        const int64_t cur = rem * 10 + (c - '0');
        const int64_t dq = cur / kBase;
        rem = cur % kBase;
        if (!q.empty() || dq != 0) q.push_back(static_cast<char>('0' + dq));
      }
      limbs.push_back(b.literal(rem, p));
      rest = q.empty() ? "0" : q;
    }
    return b.object_lit({{b.str_literal(kBigKey, p), b.array_lit(limbs, p)},
                         {b.str_literal(kSignKey, p), b.literal(1, p)}},
                        p);
  }

  NodeId emit_postfix(const Ast& a, size_t limit, FnCtx& ctx) {
    Builder b(m);
    const Ast& prim = *a.nodes[0];
    size_t i = 1;
    NodeId cur;
    if (prim.tag == "ident"_ && prim.token == "super") {
      // `super().m(args)`, in one shape: the base comes from the class the
      // method was declared in, and `self` from its first parameter -- so
      // neither depends on what the instance turned out to be.
      if (limit < 4 || a.nodes[1]->tag != "callsfx"_ ||
          a.nodes[2]->tag != "dotsfx"_ || a.nodes[3]->tag != "callsfx"_) {
        fail(prim, "'super' is only supported as super().method(...)");
      }
      const auto& params = fns[static_cast<size_t>(ctx.fn)].params;
      if (params.empty()) fail(prim, "'super' outside a method");
      const NodeId table = read_var(ref_of.at(&prim), ctx, pos_of(prim));
      const Ast& callnode = *a.nodes[3];
      const auto [A, K] = emit_callargs(
          callnode.nodes.empty() ? nullptr : callnode.nodes[0].get(), ctx, {},
          pos_of(prim));
      cur = helper(
          ctx, "$supercall",
          {read_var(params[0].var, ctx, pos_of(prim)),
           b.index(table, b.str_literal(kBaseKey, pos_of(prim)),
                   pos_of(prim)),
           b.str_literal("\x02" + std::string(a.nodes[2]->nodes[0]->token),
                         pos_of(prim)),
           A, K},
          pos_of(prim));
      i = 4;
    } else if (prim.tag == "ident"_ && !ref_of.count(&prim)) {
      cur = emit_builtin(a, limit, ctx, i);
    } else {
      cur = emit_expr(prim, ctx);
    }
    for (; i < limit; ++i) {
      const Ast& sfx = *a.nodes[i];
      const SrcPos p = pos_of(sfx);
      switch (sfx.tag) {
        case "dotsfx"_: {
          const std::string name(sfx.nodes[0]->token);
          if (i + 1 < limit && a.nodes[i + 1]->tag == "callsfx"_) {
            cur = emit_method(cur, name, *a.nodes[i + 1], ctx, p);
            ++i;
            break;
          }
          cur = helper(ctx, "$getattr", {cur, b.str_literal(name, p)}, p);
          break;
        }
        case "indexsfx"_: {
          const Ast& sub = *sfx.nodes[0];
          // Which side of the ':' an expression fell on is a *rule*, not
          // a position: `slicelo` and `slicehi` are separate rules for
          // exactly that reason, since one child says nothing on its own.
          if (sub.tag == "sliceboth"_) {
            cur = helper(ctx, "$slice",
                         {cur, emit_expr(*sub.nodes[0], ctx),
                          emit_expr(*sub.nodes[1], ctx)},
                         p);
            break;
          }
          if (sub.tag == "slicelo"_) {
            cur = helper(ctx, "$slice",
                         {cur, emit_expr(*sub.nodes[0], ctx),
                          b.nil_literal(p)},
                         p);
            break;
          }
          if (sub.tag == "slicehi"_) {
            cur = helper(ctx, "$slice",
                         {cur, b.nil_literal(p),
                          emit_expr(*sub.nodes[0], ctx)},
                         p);
            break;
          }
          if (sub.tag == "sliceall"_) {
            cur = helper(ctx, "$slice",
                         {cur, b.nil_literal(p), b.nil_literal(p)}, p);
            break;
          }
          cur = helper(ctx, "$idx", {cur, emit_expr(sub, ctx)}, p);
          break;
        }
        default:  // callsfx
          cur = emit_pycall(
              cur, sfx.nodes.empty() ? nullptr : sfx.nodes[0].get(), ctx, {},
              p);
          break;
      }
    }
    return cur;
  }

  NodeId emit_method(NodeId recv, const std::string& name, const Ast& call,
                     FnCtx& ctx, SrcPos p) {
    Builder b(m);
    const Ast* argnode = call.nodes.empty() ? nullptr : call.nodes[0].get();
    // The receiver lands in a slot first, because both branches below
    // read it and one of them reads it twice.
    const int32_t t = ctx.alloc_local("$self");
    const NodeId T = b.varref(VarKind::Local, t, p);
    // A method declares `self` itself, so the receiver goes in front.
    if (!is_method_name(name)) {
      return b.block(
          {b.assign(VarKind::Local, t, recv, p),
           emit_pycall(helper(ctx, "$getattr", {T, b.str_literal(name, p)}, p),
                       argnode, ctx, {T}, p)},
          p);
    }
    // `sort` takes key= and reverse=, exactly as `sorted` does.
    NodeId sortkey = b.nil_literal(p);
    NodeId sortrev = b.bool_literal(false, p);
    std::vector<NodeId> args;
    if (name == "sort" && argnode != nullptr) {
      for (const auto& c : argnode->nodes) {
        if (c->tag != "kwarg"_) {
          fail(*c, "sort() takes no positional arguments here");
        }
        const std::string kn(c->nodes[0]->token);
        if (kn == "key") {
          sortkey = emit_expr(*c->nodes[1], ctx);
        } else if (kn == "reverse") {
          sortrev = emit_expr(*c->nodes[1], ctx);
        } else {
          fail(*c, "sort() takes key= and reverse= here");
        }
      }
    } else if (name != "sort") {
      args = emit_args(argnode, ctx);
    }
    const auto a0 = [&](size_t k) {
      return k < args.size() ? args[k] : b.nil_literal(p);
    };
    const auto got = [&](size_t k) { return b.bool_literal(k < args.size(), p); };
    std::vector<NodeId> call_args{T};
    call_args.insert(call_args.end(), args.begin(), args.end());
    const NodeId user = b.call_value(
        helper(ctx, "$getattr", {T, b.str_literal(name, p)}, p),
        {b.array_lit(call_args, p), b.nil_literal(p)}, p);

    std::vector<std::pair<const char*, NodeId>> cands;
    const auto add = [&](const char* want, NodeId impl) {
      cands.emplace_back(want, impl);
    };
    const auto h = [&](const char* n, std::vector<NodeId> as) {
      return helper(ctx, n, as, p);
    };
    if (name == "append") {
      add("array", b.intrinsic(IntrinsicId::ArrayPush, {T, a0(0)}, p));
    } else if (name == "extend") {
      add("array", h("$aext", {T, a0(0)}));
    } else if (name == "pop") {
      add("array", h("$apop", {T, a0(0), got(0)}));
      add("map", h("$dpop", {T, a0(0), a0(1), got(1)}));
    } else if (name == "insert") {
      add("array", h("$ainsert", {T, a0(0), a0(1)}));
    } else if (name == "remove") {
      add("array", h("$aremove", {T, a0(0)}));
    } else if (name == "index") {
      add("array", h("$aindex", {T, a0(0)}));
    } else if (name == "count") {
      add("array", h("$acount", {T, a0(0)}));
      add("string", h("$scount", {T, a0(0)}));
    } else if (name == "reverse") {
      add("array", h("$areverse", {T}));
    } else if (name == "sort") {
      add("array", h("$asort", {T, sortkey, sortrev}));
    } else if (name == "keys") {
      add("map", b.intrinsic(IntrinsicId::ObjectKeys, {T}, p));
    } else if (name == "items") {
      add("map", h("$items", {T}));
    } else if (name == "values") {
      add("map", h("$values", {T}));
    } else if (name == "get") {
      add("map", h("$dget", {T, a0(0), a0(1)}));
    } else if (name == "update") {
      add("map", h("$kwmerge", {T, a0(0)}));
    } else if (name == "split") {
      add("string", h("$split", {T, a0(0), got(0)}));
    } else if (name == "strip") {
      add("string", h("$strip", {T, b.literal(0, p)}));
    } else if (name == "lstrip") {
      add("string", h("$strip", {T, b.literal(1, p)}));
    } else if (name == "rstrip") {
      add("string", h("$strip", {T, b.literal(2, p)}));
    } else if (name == "replace") {
      add("string", h("$replace", {T, a0(0), a0(1)}));
    } else if (name == "find") {
      add("string", h("$find", {T, a0(0)}));
    } else if (name == "startswith") {
      add("string", h("$startswith", {T, a0(0)}));
    } else if (name == "endswith") {
      add("string", h("$endswith", {T, a0(0)}));
    } else if (name == "upper") {
      add("string", native("upper", {T}, p));
    } else if (name == "lower") {
      add("string", native("lower", {T}, p));
    } else {  // join
      add("string", h("$join", {T, a0(0)}));
    }
    NodeId cur = user;
    for (size_t k = cands.size(); k-- > 0;) {
      cur = b.make_if(b.binary(BinOp::Eq,
                               b.intrinsic(IntrinsicId::TypeOf, {T}, p),
                               b.str_literal(cands[k].first, p), p),
                      cands[k].second, cur, p);
    }
    return b.block({b.assign(VarKind::Local, t, recv, p), cur}, p);
  }

  NodeId emit_builtin(const Ast& a, size_t limit, FnCtx& ctx, size_t& i) {
    Builder b(m);
    const Ast& prim = *a.nodes[0];
    const SrcPos p = pos_of(prim);
    const std::string g(prim.token);
    if (i >= limit || a.nodes[i]->tag != "callsfx"_) {
      fail(prim, "'" + g + "' must be called here");
    }
    const Ast& call = *a.nodes[i];
    if (g == "sorted") {
      ++i;
      const Ast* al = call.nodes.empty() ? nullptr : call.nodes[0].get();
      if (al == nullptr || al->nodes.empty()) {
        fail(prim, "sorted() takes an iterable here");
      }
      NodeId seq;
      NodeId key = b.nil_literal(p);
      NodeId rev = b.bool_literal(false, p);
      for (const auto& c : al->nodes) {
        if (c->tag == "kwarg"_) {
          const std::string kn(c->nodes[0]->token);
          if (kn == "key") {
            key = emit_expr(*c->nodes[1], ctx);
          } else if (kn == "reverse") {
            rev = emit_expr(*c->nodes[1], ctx);
          } else {
            fail(*c, "sorted() takes key= and reverse= here");
          }
          continue;
        }
        if (seq.v != NodeId{}.v) fail(*c, "sorted() takes one iterable here");
        seq = emit_expr(*c, ctx);
      }
      return helper(ctx, "$sorted", {seq, key, rev}, p);
    }
    if (g == "isinstance") {
      // The second argument is usually a bare type name, which is not a
      // value here -- `int` and `ValueError` are things this front end
      // knows rather than things the program can hold.
      ++i;
      const Ast* al = call.nodes.empty() ? nullptr : call.nodes[0].get();
      if (al == nullptr || al->nodes.size() != 2) {
        fail(prim, "isinstance() takes two arguments here");
      }
      const Ast& what = *al->nodes[0];
      const Ast& cls = *al->nodes[1];
      const NodeId cv =
          cls.tag == "ident"_ && !ref_of.count(&cls)
              ? b.str_literal(std::string(cls.token), p)
              : emit_expr(cls, ctx);
      return helper(ctx, "$isinst", {emit_expr(what, ctx), cv}, p);
    }
    std::vector<NodeId> args =
        emit_args(call.nodes.empty() ? nullptr : call.nodes[0].get(), ctx);
    ++i;
    const auto a0 = [&](size_t k) {
      return k < args.size() ? args[k] : b.nil_literal(p);
    };

    if (is_exception_name(g)) {
      return b.object_lit({{b.str_literal(kExcKey, p), b.str_literal(g, p)},
                           {b.str_literal(kMsgKey, p),
                            args.empty() ? b.str_literal("", p) : args[0]}},
                          p);
    }
    if (g == "print") {
      std::vector<NodeId> parts;
      for (const NodeId v : args) parts.push_back(helper(ctx, "$str", {v}, p));
      return native("print", {b.array_lit(parts, p)}, p);
    }
    if (g == "len") return helper(ctx, "$len", {a0(0)}, p);
    if (g == "str") return helper(ctx, "$str", {a0(0)}, p);
    if (g == "int") return helper(ctx, "$toint", {a0(0)}, p);
    if (g == "float") return helper(ctx, "$tofloat", {a0(0)}, p);
    if (g == "list") return helper(ctx, "$tolist", {a0(0)}, p);
    if (g == "bool") return helper(ctx, "$truthy", {a0(0)}, p);
    if (g == "abs") {
      return b.make_if(
          b.binary(BinOp::Lt, helper(ctx, "$cmp", {a0(0), b.literal(0, p)}, p),
                   b.literal(0, p), p),
          helper(ctx, "$neg", {a0(0)}, p), a0(0), p);
    }
    if (g == "type") return helper(ctx, "$type", {a0(0)}, p);
    if (g == "repr") return helper(ctx, "$repr", {a0(0)}, p);
    if (g == "next") {
      return helper(ctx, "$next",
                    {a0(0), a0(1), b.bool_literal(args.size() > 1, p)}, p);
    }
    if (g == "tuple") {
      return helper(ctx, "$tuple", {helper(ctx, "$tolist", {a0(0)}, p)}, p);
    }
    if (g == "enumerate") {
      return helper(ctx, "$enumerate",
                    {a0(0), args.size() > 1 ? args[1] : b.literal(0, p)}, p);
    }
    if (g == "zip") {
      if (args.size() != 2) fail(prim, "zip() takes two iterables here");
      return helper(ctx, "$zip", {args[0], args[1]}, p);
    }
    if (g == "sum") {
      return helper(ctx, "$sum",
                    {a0(0), args.size() > 1 ? args[1] : b.literal(0, p)}, p);
    }
    if (g == "min" || g == "max") {
      const NodeId ismax = b.bool_literal(g == "max", p);
      // `min(xs)` scans one iterable; `min(a, b)` scans its own arguments.
      return helper(ctx, "$minmax",
                    {args.size() == 1 ? args[0] : b.array_lit(args, p), ismax},
                    p);
    }
    if (g == "range") {
      if (args.size() == 1) {
        return helper(ctx, "$range",
                      {b.literal(0, p), args[0], b.literal(1, p)}, p);
      }
      return helper(ctx, "$range",
                    {a0(0), a0(1),
                     args.size() > 2 ? args[2] : b.literal(1, p)},
                    p);
    }
    fail(prim, "'" + g + "' is not supported here");
  }

  // A one-argument builtin, wrapped in a function that takes the calling
  // convention -- so it can be passed anywhere a function of the program's
  // own can.
  int32_t builtin_func(const std::string& g) {
    const auto it = builtin_fn.find(g);
    if (it != builtin_fn.end()) return it->second;
    Builder b(m);
    const SrcPos p{0, 0};
    const int32_t idx = static_cast<int32_t>(m.funcs.size());
    m.funcs.push_back({});
    const NodeId arg =
        b.index(b.varref(VarKind::Local, 0, p), b.literal(0, p), p);
    const auto rtc = [&](const std::string& n) {
      return b.call_value(b.make_closure(rt.at(n), empty_cmap, p), {arg}, p);
    };
    NodeId body;
    if (g == "len") {
      body = rtc("$len");
    } else if (g == "str") {
      body = rtc("$str");
    } else if (g == "repr") {
      body = rtc("$repr");
    } else if (g == "int") {
      body = rtc("$toint");
    } else if (g == "float") {
      body = rtc("$tofloat");
    } else if (g == "bool") {
      body = rtc("$truthy");
    } else if (g == "list") {
      body = rtc("$tolist");
    } else if (g == "type") {
      body = rtc("$type");
    } else if (g == "tuple") {
      body = b.call_value(b.make_closure(rt.at("$tuple"), empty_cmap, p),
                          {rtc("$tolist")}, p);
    } else {  // abs
      body = b.make_if(
          b.binary(BinOp::Lt,
                   b.call_value(b.make_closure(rt.at("$cmp"), empty_cmap, p),
                                {arg, b.literal(0, p)}, p),
                   b.literal(0, p), p),
          b.call_value(b.make_closure(rt.at("$neg"), empty_cmap, p), {arg}, p),
          arg, p);
    }
    Func f;
    f.name = g;
    f.num_params = 2;
    f.num_locals = 2;
    f.local_names = {"$a", "$k"};
    f.lenient_arity = true;
    f.body = b.scope(0, 0, b.make_return(body, p), p);
    m.funcs[static_cast<size_t>(idx)] = std::move(f);
    builtin_fn[g] = idx;
    return idx;
  }

  // -- One function's body -------------------------------------------------
  void emit_fn(int32_t f) {
    // By value, not by reference: emit_class appends a constructor to
    // `fns` while this body is being emitted, and a vector that grows
    // leaves a reference into it dangling. The symptom was a capture
    // reading an uninitialized cell, three functions away.
    const FnInfo fi = fns[static_cast<size_t>(f)];
    if (fi.is_synth) return;  // a `with`'s exit thunk, built by emit_with
    FnCtx ctx;
    ctx.fn = f;
    ctx.next_cell = static_cast<int32_t>(fi.cell_index.size());
    Builder b(m);
    const SrcPos p = fi.body != nullptr ? pos_of(*fi.body) : SrcPos{0, 0};

    std::vector<NodeId> pre;
    // Every cell this function owns is made once, at entry. Python binds
    // per function rather than per block, and its closures are *late*
    // binding -- a lambda made in a loop sees the loop variable's final
    // value -- so a CellFresh per iteration would be wrong here in a way
    // it is right in examples/mini-js.
    for (const auto& [v, c] : fi.cell_index) {
      (void)v;
      pre.push_back(b.cell_fresh(c, p));
    }
    // Before the prologue below, which calls helpers itself.
    if (f == 0) fill_helpers(ctx, pre, p);
    // The two slots every Python function is called with, and the prologue
    // that turns them back into the parameters the source declared.
    const int32_t sa = ctx.alloc_local("$a");
    const int32_t sk = ctx.alloc_local("$k");
    emit_prologue(fi, ctx, pre, sa, sk, p);
    // Every other binding of this function gets a slot up front too, since
    // its scope is the whole body whatever block it was assigned in.
    for (size_t v = 0; v < vars.size(); ++v) {
      if (vars[v].owner != f) continue;
      if (slot_of[v] >= 0) continue;
      if (fi.cell_index.count(static_cast<int32_t>(v))) continue;
      slot_of[v] = ctx.alloc_local(vars[v].name);
    }

    NodeId body;
    if (fi.body->tag == "program"_ || fi.body->tag == "block"_) {
      body = emit_block(*fi.body, ctx);
    } else if (fi.body->tag == "listcomp"_ || fi.body->tag == "dictcomp"_ ||
               fi.body->tag == "gencomp"_ || fi.body->tag == "bargen"_) {
      body = emit_comp(*fi.body, ctx, p);
    } else {
      body = b.make_return(emit_expr(*fi.body, ctx), p);  // a lambda
    }

    std::vector<NodeId> stmts;
    stmts.insert(stmts.end(), pre.begin(), pre.end());
    stmts.push_back(body);
    stmts.push_back(b.make_return(b.nil_literal(p), p));

    Func fn;
    fn.name = fi.name;
    fn.num_params = 2;  // the convention, not what the source declared
    fn.num_locals = ctx.high_local;
    ctx.local_names.resize(static_cast<size_t>(ctx.high_local), "");
    fn.local_names = ctx.local_names;
    fn.num_cells = ctx.next_cell;
    fn.lenient_arity = true;
    fn.is_generator = fi.is_generator;
    fn.num_captures = m.funcs[static_cast<size_t>(fi.index)].num_captures;
    fn.capture_names = m.funcs[static_cast<size_t>(fi.index)].capture_names;
    fn.body = b.scope(0, ctx.high_local, b.block(stmts, p), p);
    m.funcs[static_cast<size_t>(fi.index)] = std::move(fn);
  }

  // Unpack the convention into the declared parameters, applying defaults,
  // matching keywords by name, and collecting what `*rest`/`**kw` asked for.
  void emit_prologue(const FnInfo& fi, FnCtx& ctx, std::vector<NodeId>& pre,
                     int32_t sa, int32_t sk, SrcPos p) {
    Builder b(m);
    const NodeId A = b.varref(VarKind::Local, sa, p);
    const NodeId K = b.varref(VarKind::Local, sk, p);
    const auto alen = [&] { return b.intrinsic(IntrinsicId::Len, {A}, p); };
    // The entry frame is the one activation the VM builds itself rather than
    // through a call, so `lenient_arity`'s nil-fill never runs for it and
    // both slots would still be Uninit -- which the read-before-init check
    // catches on the first look. A `finally` thunk *is* called (by Defer,
    // with no arguments), so nil is what it gets and the test suffices.
    if (fi.parent < 0) {
      pre.push_back(b.assign(VarKind::Local, sa, b.array_lit({}, p), p));
      pre.push_back(b.assign(VarKind::Local, sk, b.nil_literal(p), p));
    } else {
      pre.push_back(b.make_if(
          b.binary(BinOp::Eq, b.intrinsic(IntrinsicId::TypeOf, {A}, p),
                   b.str_literal("nil", p), p),
          b.assign(VarKind::Local, sa, b.array_lit({}, p), p), NodeId{}, p));
    }

    // Two passes over the parameters, because Python's "missing" message
    // names *every* argument that did not arrive, so the first fallback to
    // be emitted already needs the last parameter's name.
    std::vector<NodeId> names;    // every declared positional, in order
    std::vector<NodeId> reqname;  // and the subset with no default,
    std::vector<NodeId> reqidx;   // with the position each one wanted
    int32_t pos = 0;
    int32_t required = 0;
    bool has_rest = false;
    bool has_kwrest = false;
    for (const ParamInfo& pi : fi.params) {
      if (pi.kind == ParamInfo::Rest) {
        has_rest = true;
        continue;
      }
      if (pi.kind == ParamInfo::KwRest) {
        has_kwrest = true;
        continue;
      }
      names.push_back(b.str_literal(pi.name, p));
      if (pi.kind != ParamInfo::Default) {
        reqname.push_back(b.str_literal(pi.name, p));
        reqidx.push_back(b.literal(pos, p));
        required = pos + 1;
      }
      ++pos;
    }
    const NodeId all_names = b.array_lit(names, p);
    const NodeId req_names = b.array_lit(reqname, p);
    const NodeId req_idx = b.array_lit(reqidx, p);

    int32_t at = 0;
    for (const ParamInfo& pi : fi.params) {
      NodeId value;
      if (pi.kind == ParamInfo::Rest) {
        value = helper(ctx, "$rest", {A, b.literal(pos, p)}, p);
      } else if (pi.kind == ParamInfo::KwRest) {
        value = helper(ctx, "$kwrest", {K, all_names}, p);
      } else {
        // Positional, then by name, then the default -- and a TypeError
        // when none of the three answered.
        // `$missing` is handed every required parameter rather than this
        // one, because Python's message names all of them at once.
        const NodeId fallback =
            pi.kind == ParamInfo::Default
                ? read_var(pi.def_var, ctx, p)
                : helper(ctx, "$missing",
                         {b.str_literal(fi.name, p), req_names, req_idx, A, K},
                         p);
        value = b.make_if(
            b.binary(BinOp::Gt, alen(), b.literal(at, p), p),
            b.index(A, b.literal(at, p), p),
            b.make_if(helper(ctx, "$kwhas", {K, b.str_literal(pi.name, p)}, p),
                      b.index(K, b.str_literal(pi.name, p), p), fallback, p),
            p);
        ++at;
      }
      const auto it = fi.cell_index.find(pi.var);
      if (it != fi.cell_index.end()) {
        pre.push_back(b.assign(VarKind::Cell, it->second, value, p));
      } else {
        const int32_t s = ctx.alloc_local(pi.name);
        slot_of[static_cast<size_t>(pi.var)] = s;
        pre.push_back(b.assign(VarKind::Local, s, value, p));
      }
    }
    if (!has_rest) {
      pre.push_back(b.make_if(
          b.binary(BinOp::Gt, alen(), b.literal(pos, p), p),
          helper(ctx, "$toomany",
                 {b.str_literal(fi.name, p), b.literal(required, p),
                  b.literal(pos, p), alen()},
                 p),
          NodeId{}, p));
    }
    if (!has_kwrest) {
      pre.push_back(helper(ctx, "$kwcheck",
                           {K, all_names, b.str_literal(fi.name, p)}, p));
    }
  }

  // The body of a comprehension's function: the clauses nest outward-in,
  // and what the innermost one reaches is a push, a store or a yield.
  NodeId emit_comp(const Ast& a, FnCtx& ctx, SrcPos p) {
    Builder b(m);
    const bool dict = a.tag == "dictcomp"_;
    const bool gen = a.tag == "gencomp"_ || a.tag == "bargen"_;
    const size_t head = dict ? 2 : 1;  // the element expression(s)

    std::vector<const Ast*> clauses;
    for (size_t i = head; i < a.nodes.size(); ++i) {
      clauses.push_back(a.nodes[i].get());
    }
    int32_t acc = -1;
    if (!gen) acc = ctx.alloc_local("$acc");
    const NodeId ACC = gen ? NodeId{} : b.varref(VarKind::Local, acc, p);

    // Built innermost-first, so each clause wraps what it produces.
    NodeId inner;
    if (gen) {
      inner = b.make_yield(emit_expr(*a.nodes[0], ctx), p);
    } else if (dict) {
      inner = b.set_index(ACC, emit_expr(*a.nodes[0], ctx),
                          emit_expr(*a.nodes[1], ctx), p);
    } else {
      inner = b.intrinsic(IntrinsicId::ArrayPush,
                          {ACC, emit_expr(*a.nodes[0], ctx)}, p);
    }
    for (size_t k = clauses.size(); k-- > 0;) {
      const Ast& c = *clauses[k];
      if (c.tag == "compif"_) {
        inner = b.make_if(helper(ctx, "$truthy", {emit_expr(*c.nodes[0], ctx)},
                                 p),
                          inner, NodeId{}, p);
        continue;
      }
      inner = emit_comp_for(c, inner, ctx, p);
    }
    std::vector<NodeId> out;
    if (!gen) {
      out.push_back(b.assign(VarKind::Local, acc,
                             dict ? b.intrinsic(IntrinsicId::MapNew, {}, p)
                                  : b.array_lit({}, p),
                             p));
    }
    out.push_back(inner);
    out.push_back(b.make_return(gen ? b.nil_literal(p) : ACC, p));
    return b.block(out, p);
  }

  NodeId emit_comp_for(const Ast& c, NodeId inner, FnCtx& ctx, SrcPos p) {
    Builder b(m);
    const int32_t it = ctx.alloc_local("$it");
    const int32_t st = ctx.alloc_local("$step");
    const NodeId I = b.varref(VarKind::Local, it, p);
    const NodeId S = b.varref(VarKind::Local, st, p);
    const Ast& tg = *c.nodes[0];
    std::vector<NodeId> loop{
        b.assign(VarKind::Local, st, helper(ctx, "$iternext", {I}, p), p),
        b.make_if(b.index(S, b.str_literal("done", p), p), b.make_break(p),
                  NodeId{}, p)};
    const NodeId value = b.index(S, b.str_literal("value", p), p);
    if (tg.nodes.size() == 1) {
      loop.push_back(write_var(decl_of.at(tg.nodes[0].get()), value, ctx, p));
    } else {
      const int32_t u = ctx.alloc_local("$unp");
      const NodeId U = b.varref(VarKind::Local, u, p);
      loop.push_back(b.assign(
          VarKind::Local, u,
          helper(ctx, "$unpack",
                 {value, b.literal(static_cast<int64_t>(tg.nodes.size()), p)},
                 p),
          p));
      for (size_t k = 0; k < tg.nodes.size(); ++k) {
        loop.push_back(write_var(
            decl_of.at(tg.nodes[k].get()),
            b.index(U, b.literal(static_cast<int64_t>(k), p), p), ctx, p));
      }
    }
    loop.push_back(inner);
    return b.block(
        {b.assign(VarKind::Local, it,
                  helper(ctx, "$iter", {emit_expr(*c.nodes[1], ctx)}, p), p),
         b.make_while(b.bool_literal(true, p), b.block(loop, p), p)},
        p);
  }

  Module build(const Ast& program) {
    const int32_t top = new_fn(-1, "main");
    fns[static_cast<size_t>(top)].body = &program;

    scopes.push_back({top, {}});
    bind_names(program, top);
    for (const auto& s : program.nodes) resolve_stmt(*s, top);
    scopes.pop_back();

    m.funcs.push_back({});
    fns[static_cast<size_t>(top)].index = 0;
    for (const std::string& n : rt_names()) {
      rt[n] = static_cast<int32_t>(m.funcs.size());
      m.funcs.push_back({});
    }
    const size_t declared = fns.size();
    for (size_t f = 1; f < declared; ++f) {
      fns[f].index = static_cast<int32_t>(m.funcs.size());
      m.funcs.push_back({});
    }

    // One binding, owned by file scope and captured by every function:
    // the array of runtime-helper closures. Declared after resolution so
    // no source name can collide with it, and before number_captures so
    // the ordinary capture machinery threads it like any other free
    // variable.
    helpers_var = static_cast<int32_t>(vars.size());
    vars.push_back({"$helpers", top});
    for (size_t f = 1; f < fns.size(); ++f) fns[f].free.insert(helpers_var);
    // A cell whether or not anything captured it: file scope reads it
    // itself, and a program with no nested function has no free set to
    // put it in.
    force_cells.insert(helpers_var);

    number_captures();
    empty_cmap = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back({});
    emit_runtime();

    slot_of.assign(vars.size(), -1);
    for (size_t f = 0; f < declared; ++f) {
      emit_fn(static_cast<int32_t>(f));
    }
    return std::move(m);
  }
};

// ==== The builtins that have to be host functions ==========================

bool nat_print(NativeCall& c) {
  std::string out;
  const auto& items = c.arg(0).as_array()->items;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) out += " ";
    out += items[i].as_str();
  }
  coreir_rt_out_str(out.data(), static_cast<int64_t>(out.size()));
  c.result = Value();
  return true;
}

bool nat_upper(NativeCall& c) {
  std::string s = c.arg(0).as_str();
  for (char& ch : s) ch = static_cast<char>(std::toupper(ch));
  c.result = Value::make_str(std::move(s));
  return true;
}

// The one number-to-string rule this front end does not write itself: a
// fixed-precision decimal is the C library's exact expansion of the double,
// which is what CPython's `format(x, '.2f')` is too. Everything else about
// f-strings is a scan and lives in the IR.
bool nat_ffmt(NativeCall& c) {
  const double v = c.arg(0).as_double();
  int prec = static_cast<int>(c.arg(1).as_int());
  if (prec < 0) prec = 0;
  if (prec > 100) prec = 100;
  std::vector<char> buf(static_cast<size_t>(prec) + 350);
  const int n = std::snprintf(buf.data(), buf.size(), "%.*f", prec, v);
  c.result = Value::make_str(std::string(buf.data(), static_cast<size_t>(n)));
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
      {"print", 1, nat_print, nullptr},
      {"upper", 1, nat_upper, nullptr},
      {"lower", 1, nat_lower, nullptr},
      {"ffmt", 2, nat_ffmt, nullptr},
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

  // Python's indentation is not something a PEG can see -- see layout.h.
  const std::string normalized = layout(source);
  std::shared_ptr<Ast> ast;
  if (!p.parse(normalized, ast)) coreir_rt::fail("syntax error", 0, 0);
  ast = p.optimize_ast(ast);

  Binder b;
  Module m = b.build(*ast);

  if (auto err = verify(m)) {
    coreir_rt::fail("internal error: malformed IR: " + *err, 0, 0);
  }
  return m;
}

}  // namespace mini_python
