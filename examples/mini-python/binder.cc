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
  bool is_generator = false;
  bool is_synth = false;  // a `with`'s exit thunk: no body to walk
  std::string name = "?";
  std::vector<ParamInfo> params;
  // `global x` and `nonlocal x` -- the two statements that say a name is
  // *not* this function's, which is the only way Python has to say it.
  std::set<std::string> globals;
  std::set<std::string> nonlocals;
  const Ast* body = nullptr;
};

struct FnCtx : FrameLayout {
  int32_t fn = 0;
  int32_t next_cell = 0;
};

struct Binder {
  Module m;
  Resolver rs;
  std::vector<FnInfo> fns;  // parallel to rs.fns
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
  std::vector<const Ast*> class_stack;
  std::map<std::string, int32_t> rt;
  std::map<std::string, int32_t> builtin_fn;
  int32_t empty_cmap = -1;

  // ==== Pass A: scopes, declarations, captures =============================
  //
  // Python has no declaration form: an assignment *is* one, and its scope
  // is the whole function rather than the block it stands in. So a name
  // assigned anywhere in a function body belongs to that function, which
  // is why this pass pre-scans a body for assigned names before walking
  // it -- the opposite of every other front end here, where a declaration
  // is a statement with a keyword in front of it.


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

  // Python has no declaration keyword: an assignment binds, and a second
  // assignment to the same name in the same scope is the same binding.
  std::optional<int32_t> resolve(const std::string& name, int32_t fn) {
    // `global x` skips every enclosing function and lands at the module,
    // even when one of them has an `x` of its own -- which is why this
    // looks the name up itself rather than calling Resolver::resolve.
    const std::optional<int32_t> v =
        fns[static_cast<size_t>(fn)].globals.count(name) ? rs.lookup(name, 0)
                                                         : rs.lookup(name);
    if (v) rs.use(*v, fn);
    return v;
  }

  int32_t new_fn(int32_t parent, const std::string& name) {
    const int32_t f = rs.new_fn(parent);
    fns.push_back({});
    fns[static_cast<size_t>(f)].name = name;
    return f;
  }

  int32_t declare(const std::string& name, int32_t fn) {
    if (const auto v = rs.declared_here(name)) return *v;
    return rs.declare(name, fn);
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
      // Scope 0 is the module's, and funcs[0] is the function that owns
      // it -- build() pushes the two together.
      if (!rs.declared_at(0, g)) rs.declare_in(0, g, 0);
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
    rs.push_scope();
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
    rs.pop_scope();
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
        pi.def_var = static_cast<int32_t>(rs.vars.size());
        rs.vars.push_back({"$def." + std::string(pi.id->token), parent, -1});
        rs.fns[static_cast<size_t>(f)].free.insert(pi.def_var);
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
          rs.use(ci.base_var, fn);
          break;
        }
        // The synthetic binding that holds the method table. It is always a
        // cell: the constructor captures it, and so does any method that
        // says `super()`.
        ci.table_var = static_cast<int32_t>(rs.vars.size());
        rs.vars.push_back({"$cls." + cname, fn, -1});
        rs.force_cell(ci.table_var);
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
          rs.use(tv, fn);
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
    rs.push_scope();
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
    rs.pop_scope();
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

  // The front end's own additions to coreir::FuncWriter: the helpers
  // that have to reach this binder's own tables.
  struct RT : FuncWriter {
    Binder& bd;

    explicit RT(Binder& bd_) : FuncWriter(bd_.m), bd(bd_) {}

    NodeId call(const std::string& name, const std::vector<NodeId>& a) {
      return b.call_value(b.make_closure(bd.rt.at(name), bd.empty_cmap), a);
    }

    // The counts and the name table come from param()/local().
    void finish(const std::string& name, int32_t ncells = 0,
                int32_t ncaps = 0) {
      write(bd.m.funcs[static_cast<size_t>(bd.rt.at(name))], name, ncells,
            ncaps);
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
    const auto [v] = r.params("v");
    r.add(r.iff(r.is(r.typ(r.L(v)), "object"),
                r.ret(r.has(r.L(v), r.S(kBigKey)))));
    r.add(r.ret(r.Bo(false)));
    r.finish("$isbig");
  }

  void rt_abs() {
    RT r(*this);
    const auto [i] = r.params("i");
    r.add(r.iff(r.bin(BinOp::Lt, r.L(i), r.I(0)),
                r.ret(r.b.unary(UnOp::Neg, r.L(i)))));
    r.add(r.ret(r.L(i)));
    r.finish("$abs");
  }

  void rt_tolimbs() {
    RT r(*this);
    const auto [n] = r.params("n");
    const auto [out, i] = r.locals("out", "i");
    r.add(r.set(out, r.arr({})));
    r.add(r.set(i, r.L(n)));
    r.add(r.wh(r.bin(BinOp::Gt, r.L(i), r.I(0)),
               r.blk({r.push(r.L(out), r.bin(BinOp::Mod, r.L(i), r.I(kBase))),
                      r.set(i, r.bin(BinOp::Div, r.L(i), r.I(kBase)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$tolimbs");
  }

  // The demotion rule, and the reason ordinary arithmetic does not pay for
  // the operations that overflowed: two limbs is under 10^18, which fits an
  // int64 with room to spare, so anything that short comes back as one.
  void rt_mkbig() {
    RT r(*this);
    const auto [limbs, sign] = r.params("limbs", "sign");
    const auto [n, v] = r.locals("n", "v");
    r.add(r.set(n, r.len(r.L(limbs))));
    r.add(
        r.wh(r.both(r.bin(BinOp::Gt, r.L(n), r.I(0)),
                    r.bin(BinOp::Eq,
                          r.idx(r.L(limbs), r.bin(BinOp::Sub, r.L(n), r.I(1))),
                          r.I(0))),
             r.set(n, r.bin(BinOp::Sub, r.L(n), r.I(1)))));
    r.add(r.iff(r.bin(BinOp::Lt, r.L(n), r.len(r.L(limbs))),
                r.set(limbs, r.in(IntrinsicId::ArraySlice,
                                  {r.L(limbs), r.I(0), r.L(n)}))));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(n), r.I(0)), r.ret(r.I(0))));
    r.add(r.iff(
        r.bin(BinOp::Le, r.L(n), r.I(2)),
        r.blk({r.set(v, r.idx(r.L(limbs), r.I(0))),
               r.iff(r.bin(BinOp::Eq, r.L(n), r.I(2)),
                     r.set(v, r.bin(BinOp::Add, r.L(v),
                                    r.bin(BinOp::Mul, r.idx(r.L(limbs), r.I(1)),
                                          r.I(kBase))))),
               r.iff(r.bin(BinOp::Lt, r.L(sign), r.I(0)),
                     r.set(v, r.b.unary(UnOp::Neg, r.L(v)))),
               r.ret(r.L(v))})));
    r.add(r.ret(r.obj({{kBigKey, r.L(limbs)}, {kSignKey, r.L(sign)}})));
    r.finish("$mkbig");
  }

  void rt_biglimbs() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.iff(r.call("$isbig", {r.L(v)}), r.ret(r.idx(r.L(v), kBigKey))));
    r.add(r.ret(r.call("$tolimbs", {r.call("$abs", {r.L(v)})})));
    r.finish("$biglimbs");
  }

  void rt_bigsign() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.iff(r.call("$isbig", {r.L(v)}), r.ret(r.idx(r.L(v), kSignKey))));
    r.add(r.iff(r.bin(BinOp::Lt, r.L(v), r.I(0)), r.ret(r.I(-1))));
    r.add(r.ret(r.I(1)));
    r.finish("$bigsign");
  }

  void rt_ucmp() {
    RT r(*this);
    const auto [x, y] = r.params("x", "y");
    const auto [i] = r.locals("i");
    r.add(r.iff(r.bin(BinOp::Ne, r.len(r.L(x)), r.len(r.L(y))),
                r.ret(r.iff(r.bin(BinOp::Lt, r.len(r.L(x)), r.len(r.L(y))),
                            r.I(-1), r.I(1)))));
    r.add(r.set(i, r.bin(BinOp::Sub, r.len(r.L(x)), r.I(1))));
    r.add(r.wh(r.bin(BinOp::Ge, r.L(i), r.I(0)),
               r.blk({r.iff(r.bin(BinOp::Ne, r.idx(r.L(x), r.L(i)),
                                  r.idx(r.L(y), r.L(i))),
                            r.ret(r.iff(r.bin(BinOp::Lt, r.idx(r.L(x), r.L(i)),
                                              r.idx(r.L(y), r.L(i))),
                                        r.I(-1), r.I(1)))),
                      r.set(i, r.bin(BinOp::Sub, r.L(i), r.I(1)))})));
    r.add(r.ret(r.I(0)));
    r.finish("$ucmp");
  }

  void rt_uadd() {
    RT r(*this);
    const auto [x, y] = r.params("x", "y");
    const auto [out, carry, i, s] = r.locals("out", "carry", "i", "s");
    r.add(r.set(out, r.arr({})));
    r.add(r.set(carry, r.I(0)));  // carry
    r.add(r.set(i, r.I(0)));      // i
    r.add(r.wh(
        r.either(r.bin(BinOp::Lt, r.L(i), r.len(r.L(x))),
                 r.either(r.bin(BinOp::Lt, r.L(i), r.len(r.L(y))),
                          r.bin(BinOp::Gt, r.L(carry), r.I(0)))),
        r.blk(
            {r.set(s, r.L(carry)),
             r.iff(r.bin(BinOp::Lt, r.L(i), r.len(r.L(x))),
                   r.set(s, r.bin(BinOp::Add, r.L(s), r.idx(r.L(x), r.L(i))))),
             r.iff(r.bin(BinOp::Lt, r.L(i), r.len(r.L(y))),
                   r.set(s, r.bin(BinOp::Add, r.L(s), r.idx(r.L(y), r.L(i))))),
             r.push(r.L(out), r.bin(BinOp::Mod, r.L(s), r.I(kBase))),
             r.set(carry, r.bin(BinOp::Div, r.L(s), r.I(kBase))),
             r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$uadd");
  }

  // x >= y, which every caller checks with $ucmp first.
  void rt_usub() {
    RT r(*this);
    const auto [x, y] = r.params("x", "y");
    const auto [out, borrow, i, d] = r.locals("out", "borrow", "i", "d");
    r.add(r.set(out, r.arr({})));
    r.add(r.set(borrow, r.I(0)));  // borrow
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(x))),
        r.blk(
            {r.set(d, r.bin(BinOp::Sub, r.idx(r.L(x), r.L(i)), r.L(borrow))),
             r.iff(r.bin(BinOp::Lt, r.L(i), r.len(r.L(y))),
                   r.set(d, r.bin(BinOp::Sub, r.L(d), r.idx(r.L(y), r.L(i))))),
             r.iff(r.bin(BinOp::Lt, r.L(d), r.I(0)),
                   r.blk({r.set(d, r.bin(BinOp::Add, r.L(d), r.I(kBase))),
                          r.set(borrow, r.I(1))}),
                   r.set(borrow, r.I(0))),
             r.push(r.L(out), r.L(d)),
             r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$usub");
  }

  // Schoolbook, which is what base 10^9 was chosen for: one limb product
  // plus two carries is under 10^18 + 2*10^9, comfortably inside int64.
  void rt_umul() {
    RT r(*this);
    const auto [x, y] = r.params("x", "y");
    const auto [out, i, j, carry, cur, k] =
        r.locals("out", "i", "j", "carry", "cur", "k");
    r.add(r.iff(r.either(r.bin(BinOp::Eq, r.len(r.L(x)), r.I(0)),
                         r.bin(BinOp::Eq, r.len(r.L(y)), r.I(0))),
                r.ret(r.arr({}))));
    r.add(r.set(out, r.arr({})));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(i),
                     r.bin(BinOp::Add, r.len(r.L(x)), r.len(r.L(y)))),
               r.blk({r.push(r.L(out), r.I(0)),
                      r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(x))),
        r.blk(
            {r.set(carry, r.I(0)),  // carry
             r.set(j, r.I(0)),      // j
             r.wh(r.bin(BinOp::Lt, r.L(j), r.len(r.L(y))),
                  r.blk({r.set(k, r.bin(BinOp::Add, r.L(i), r.L(j))),
                         r.set(cur,
                               r.bin(BinOp::Add,
                                     r.bin(BinOp::Add, r.idx(r.L(out), r.L(k)),
                                           r.bin(BinOp::Mul,
                                                 r.idx(r.L(x), r.L(i)),
                                                 r.idx(r.L(y), r.L(j)))),
                                     r.L(carry))),
                         r.sidx(r.L(out), r.L(k),
                                r.bin(BinOp::Mod, r.L(cur), r.I(kBase))),
                         r.set(carry, r.bin(BinOp::Div, r.L(cur), r.I(kBase))),
                         r.set(j, r.bin(BinOp::Add, r.L(j), r.I(1)))})),
             r.set(k, r.bin(BinOp::Add, r.L(i), r.len(r.L(y)))),
             r.wh(r.bin(BinOp::Gt, r.L(carry), r.I(0)),
                  r.blk({r.set(cur, r.bin(BinOp::Add, r.idx(r.L(out), r.L(k)),
                                          r.L(carry))),
                         r.sidx(r.L(out), r.L(k),
                                r.bin(BinOp::Mod, r.L(cur), r.I(kBase))),
                         r.set(carry, r.bin(BinOp::Div, r.L(cur), r.I(kBase))),
                         r.set(k, r.bin(BinOp::Add, r.L(k), r.I(1)))})),
             r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$umul");
  }

  void rt_bigadd() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [sa, sb, xa, xb, c] = r.locals("sa", "sb", "xa", "xb", "c");
    r.add(r.set(sa, r.call("$bigsign", {r.L(a)})));
    r.add(r.set(sb, r.call("$bigsign", {r.L(b)})));
    r.add(r.set(xa, r.call("$biglimbs", {r.L(a)})));
    r.add(r.set(xb, r.call("$biglimbs", {r.L(b)})));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(sa), r.L(sb)),
                r.ret(r.call("$mkbig",
                             {r.call("$uadd", {r.L(xa), r.L(xb)}), r.L(sa)}))));
    r.add(r.set(c, r.call("$ucmp", {r.L(xa), r.L(xb)})));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(c), r.I(0)), r.ret(r.I(0))));
    r.add(r.iff(r.bin(BinOp::Gt, r.L(c), r.I(0)),
                r.ret(r.call("$mkbig",
                             {r.call("$usub", {r.L(xa), r.L(xb)}), r.L(sa)}))));
    r.add(r.ret(
        r.call("$mkbig", {r.call("$usub", {r.L(xb), r.L(xa)}), r.L(sb)})));
    r.finish("$bigadd");
  }

  void rt_bigmul() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    r.add(r.ret(
        r.call("$mkbig", {r.call("$umul", {r.call("$biglimbs", {r.L(a)}),
                                           r.call("$biglimbs", {r.L(b)})}),
                          r.bin(BinOp::Mul, r.call("$bigsign", {r.L(a)}),
                                r.call("$bigsign", {r.L(b)}))})));
    r.finish("$bigmul");
  }

  // Nine decimal digits per limb, zero-padded except for the top one --
  // the printing that base 10^9 makes free of division.
  void rt_bstr() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [x, i, out, t] = r.locals("x", "i", "out", "t");
    r.add(r.iff(r.bin(BinOp::Eq, r.call("$isbig", {r.L(v)}), r.Bo(false)),
                r.ret(r.in(IntrinsicId::ToStr, {r.L(v)}))));
    r.add(r.set(x, r.idx(r.L(v), kBigKey)));
    r.add(r.set(i, r.bin(BinOp::Sub, r.len(r.L(x)), r.I(1))));
    r.add(r.set(out, r.in(IntrinsicId::ToStr, {r.idx(r.L(x), r.L(i))})));
    r.add(r.set(i, r.bin(BinOp::Sub, r.L(i), r.I(1))));
    r.add(
        r.wh(r.bin(BinOp::Ge, r.L(i), r.I(0)),
             r.blk({r.set(t, r.in(IntrinsicId::ToStr, {r.idx(r.L(x), r.L(i))})),
                    r.wh(r.bin(BinOp::Lt, r.len(r.L(t)), r.I(9)),
                         r.set(t, r.bin(BinOp::Add, r.S("0"), r.L(t)))),
                    r.set(out, r.bin(BinOp::Add, r.L(out), r.L(t))),
                    r.set(i, r.bin(BinOp::Sub, r.L(i), r.I(1)))})));
    r.add(r.iff(r.bin(BinOp::Lt, r.idx(r.L(v), kSignKey), r.I(0)),
                r.set(out, r.bin(BinOp::Add, r.S("-"), r.L(out)))));
    r.add(r.ret(r.L(out)));
    r.finish("$bstr");
  }

  void rt_tofloat() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [x, acc, i] = r.locals("x", "acc", "i");
    r.add(r.iff(r.is(r.typ(r.L(v)), "double"), r.ret(r.L(v))));
    r.add(r.iff(
        r.call("$isbig", {r.L(v)}),
        r.blk(
            {r.set(x, r.idx(r.L(v), kBigKey)), r.set(acc, r.D(0.0)),
             r.set(i, r.bin(BinOp::Sub, r.len(r.L(x)), r.I(1))),
             r.wh(r.bin(BinOp::Ge, r.L(i), r.I(0)),
                  r.blk({r.set(acc, r.bin(BinOp::Add,
                                          r.bin(BinOp::Mul, r.L(acc), r.D(1e9)),
                                          r.in(IntrinsicId::ToDouble,
                                               {r.idx(r.L(x), r.L(i))}))),
                         r.set(i, r.bin(BinOp::Sub, r.L(i), r.I(1)))})),
             r.iff(r.bin(BinOp::Lt, r.idx(r.L(v), kSignKey), r.I(0)),
                   r.set(acc, r.b.unary(UnOp::Neg, r.L(acc)))),
             r.ret(r.L(acc))})));
    r.add(r.ret(r.in(IntrinsicId::ToDouble, {r.L(v)})));
    r.finish("$tofloat");
  }

  void rt_neg() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.iff(
        r.call("$isbig", {r.L(v)}),
        r.ret(r.obj(
            {{kBigKey, r.idx(r.L(v), kBigKey)},
             {kSignKey, r.b.unary(UnOp::Neg, r.idx(r.L(v), kSignKey))}}))));
    r.add(r.ret(r.b.unary(UnOp::Neg, r.L(v))));
    r.finish("$neg");
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
    const auto [a, b] = r.params("a", "b");
    const auto [ta, tb] = r.locals("ta", "tb");
    r.add(r.set(ta, r.typ(r.L(a))));
    r.add(r.set(tb, r.typ(r.L(b))));
    r.add(r.iff(r.both(r.is(r.L(ta), "string"), r.is(r.L(tb), "string")),
                r.ret(r.bin(BinOp::Add, r.L(a), r.L(b)))));
    r.add(r.iff(r.both(r.is(r.L(ta), "array"), r.is(r.L(tb), "array")),
                r.ret(r.call("$listadd", {r.L(a), r.L(b)}))));
    r.add(r.iff(r.both(r.call("$istup", {r.L(a)}), r.call("$istup", {r.L(b)})),
                r.ret(r.call("$tuple",
                             {r.call("$listadd", {r.idx(r.L(a), kTupKey),
                                                  r.idx(r.L(b), kTupKey)})}))));
    r.add(r.iff(r.either(r.is(r.L(ta), "double"), r.is(r.L(tb), "double")),
                r.ret(r.bin(BinOp::Add, r.call("$tofloat", {r.L(a)}),
                            r.call("$tofloat", {r.L(b)})))));
    r.add(r.iff(
        int_pair(r, r.L(ta), r.L(tb)),
        r.iff(r.both(r.bin(BinOp::Lt, r.call("$abs", {r.L(a)}), r.I(kAddSafe)),
                     r.bin(BinOp::Lt, r.call("$abs", {r.L(b)}), r.I(kAddSafe))),
              r.ret(r.bin(BinOp::Add, r.L(a), r.L(b))))));
    r.add(r.ret(r.call("$bigadd", {r.L(a), r.L(b)})));
    r.finish("$add");
  }

  void rt_sub() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    r.add(r.ret(r.call("$add", {r.L(a), r.call("$neg", {r.L(b)})})));
    r.finish("$sub");
  }

  void rt_mul() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [ta, tb] = r.locals("ta", "tb");
    r.add(r.set(ta, r.typ(r.L(a))));
    r.add(r.set(tb, r.typ(r.L(b))));
    r.add(r.iff(r.both(r.is(r.L(ta), "string"), r.is(r.L(tb), "int")),
                r.ret(r.call("$strmul", {r.L(a), r.L(b)}))));
    r.add(r.iff(r.both(r.is(r.L(ta), "array"), r.is(r.L(tb), "int")),
                r.ret(r.call("$listmul", {r.L(a), r.L(b)}))));
    r.add(r.iff(r.either(r.is(r.L(ta), "double"), r.is(r.L(tb), "double")),
                r.ret(r.bin(BinOp::Mul, r.call("$tofloat", {r.L(a)}),
                            r.call("$tofloat", {r.L(b)})))));
    r.add(r.iff(
        int_pair(r, r.L(ta), r.L(tb)),
        r.iff(r.both(r.bin(BinOp::Lt, r.call("$abs", {r.L(a)}), r.I(kMulSafe)),
                     r.bin(BinOp::Lt, r.call("$abs", {r.L(b)}), r.I(kMulSafe))),
              r.ret(r.bin(BinOp::Mul, r.L(a), r.L(b))))));
    r.add(r.ret(r.call("$bigmul", {r.L(a), r.L(b)})));
    r.finish("$mul");
  }

  void rt_strmul() {
    RT r(*this);
    const auto [s, n] = r.params("s", "n");
    const auto [out, i] = r.locals("out", "i");
    r.add(r.set(out, r.S("")));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(i), r.L(n)),
               r.blk({r.set(out, r.bin(BinOp::Add, r.L(out), r.L(s))),
                      r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$strmul");
  }

  void rt_listmul() {
    RT r(*this);
    const auto [a, n] = r.params("a", "n");
    const auto [out, i, j] = r.locals("out", "i", "j");
    r.add(r.set(out, r.arr({})));
    r.add(r.set(i, r.I(0)));
    r.add(
        r.wh(r.bin(BinOp::Lt, r.L(i), r.L(n)),
             r.blk({r.set(j, r.I(0)),
                    r.wh(r.bin(BinOp::Lt, r.L(j), r.len(r.L(a))),
                         r.blk({r.push(r.L(out), r.idx(r.L(a), r.L(j))),
                                r.set(j, r.bin(BinOp::Add, r.L(j), r.I(1)))})),
                    r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$listmul");
  }

  void rt_listadd() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [out, i] = r.locals("out", "i");
    r.add(r.set(out, r.arr({})));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(a))),
               r.blk({r.push(r.L(out), r.idx(r.L(a), r.L(i))),
                      r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(b))),
               r.blk({r.push(r.L(out), r.idx(r.L(b), r.L(i))),
                      r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$listadd");
  }

  void rt_fdiv() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [d] = r.locals("d");
    r.add(r.set(d, r.call("$tofloat", {r.L(b)})));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(d), r.D(0.0)),
                r.ret(r.call("$exc", {r.S("ZeroDivisionError"),
                                      r.S("division by zero")}))));
    r.add(r.ret(r.bin(BinOp::Div, r.call("$tofloat", {r.L(a)}), r.L(d))));
    r.finish("$fdiv");
  }

  // `//` and `%` floor, like Lua's and unlike C's, so both correct the
  // truncating forms BinOp gives when the signs disagree.
  void rt_idiv() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [q] = r.locals("q");
    r.add(r.iff(r.bin(BinOp::Eq, r.L(b), r.I(0)),
                r.ret(r.call("$exc", {r.S("ZeroDivisionError"),
                                      r.S("integer division or modulo by "
                                          "zero")}))));
    r.add(r.set(q, r.bin(BinOp::Div, r.L(a), r.L(b))));
    r.add(r.iff(
        r.both(r.bin(BinOp::Ne, r.bin(BinOp::Mul, r.L(q), r.L(b)), r.L(a)),
               r.bin(BinOp::Lt, r.bin(BinOp::Mul, r.L(a), r.L(b)), r.I(0))),
        r.set(q, r.bin(BinOp::Sub, r.L(q), r.I(1)))));
    r.add(r.ret(r.L(q)));
    r.finish("$idiv");
  }

  void rt_mod() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [m] = r.locals("m");
    r.add(r.iff(r.bin(BinOp::Eq, r.L(b), r.I(0)),
                r.ret(r.call("$exc", {r.S("ZeroDivisionError"),
                                      r.S("integer division or modulo by "
                                          "zero")}))));
    r.add(r.set(m, r.bin(BinOp::Mod, r.L(a), r.L(b))));
    r.add(r.iff(
        r.both(r.bin(BinOp::Ne, r.L(m), r.I(0)),
               r.bin(BinOp::Lt, r.bin(BinOp::Mul, r.L(m), r.L(b)), r.I(0))),
        r.set(m, r.bin(BinOp::Add, r.L(m), r.L(b)))));
    r.add(r.ret(r.L(m)));
    r.finish("$mod");
  }

  // Repeated squaring, over $mul -- so the promotion rule applies at every
  // step and `2 ** 100` is exact without anything here knowing it will be.
  void rt_pow() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [acc, sq, e] = r.locals("acc", "sq", "e");
    r.add(r.iff(r.either(r.is(r.typ(r.L(a)), "double"),
                         r.either(r.is(r.typ(r.L(b)), "double"),
                                  r.bin(BinOp::Lt, r.L(b), r.I(0)))),
                r.ret(r.in(IntrinsicId::Pow, {r.call("$tofloat", {r.L(a)}),
                                              r.call("$tofloat", {r.L(b)})}))));
    r.add(r.set(acc, r.I(1)));
    r.add(r.set(sq, r.L(a)));
    r.add(r.set(e, r.L(b)));
    r.add(r.wh(r.bin(BinOp::Gt, r.L(e), r.I(0)),
               r.blk({r.iff(r.bin(BinOp::Eq, r.bin(BinOp::Mod, r.L(e), r.I(2)),
                                  r.I(1)),
                            r.set(acc, r.call("$mul", {r.L(acc), r.L(sq)}))),
                      r.set(sq, r.call("$mul", {r.L(sq), r.L(sq)})),
                      r.set(e, r.bin(BinOp::Div, r.L(e), r.I(2)))})));
    r.add(r.ret(r.L(acc)));
    r.finish("$pow");
  }

  // -1, 0 or 1 -- what every ordering operator is built from.
  void rt_cmp() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [ta, tb, fa, fb, sa, sb, c, xa, xb, i, e] =
        r.locals("ta", "tb", "fa", "fb", "sa", "sb", "c", "xa", "xb", "i", "e");
    r.add(r.set(ta, r.typ(r.L(a))));
    r.add(r.set(tb, r.typ(r.L(b))));
    // A list or a tuple compares element by element, which is what makes
    // `sorted` work on a list of pairs.
    r.add(r.iff(
        r.both(r.either(r.is(r.L(ta), "array"), r.call("$istup", {r.L(a)})),
               r.either(r.is(r.L(tb), "array"), r.call("$istup", {r.L(b)}))),
        r.blk(
            {r.set(xa, r.call("$untup", {r.L(a)})),
             r.set(xb, r.call("$untup", {r.L(b)})), r.set(i, r.I(0)),
             r.wh(r.both(r.bin(BinOp::Lt, r.L(i), r.len(r.L(xa))),
                         r.bin(BinOp::Lt, r.L(i), r.len(r.L(xb)))),
                  r.blk({r.set(e, r.call("$cmp", {r.idx(r.L(xa), r.L(i)),
                                                  r.idx(r.L(xb), r.L(i))})),
                         r.iff(r.bin(BinOp::Ne, r.L(e), r.I(0)), r.ret(r.L(e))),
                         r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})),
             r.ret(r.iff(r.bin(BinOp::Lt, r.len(r.L(xa)), r.len(r.L(xb))),
                         r.I(-1),
                         r.iff(r.bin(BinOp::Gt, r.len(r.L(xa)), r.len(r.L(xb))),
                               r.I(1), r.I(0))))})));
    r.add(r.iff(
        r.both(r.is(r.L(ta), "string"), r.is(r.L(tb), "string")),
        r.ret(r.iff(r.bin(BinOp::Lt, r.L(a), r.L(b)), r.I(-1),
                    r.iff(r.bin(BinOp::Gt, r.L(a), r.L(b)), r.I(1), r.I(0))))));
    // Two integers that both fit are compared directly; anything wider goes
    // through the limbs, sign first.
    r.add(r.iff(
        int_pair(r, r.L(ta), r.L(tb)),
        r.ret(r.iff(r.bin(BinOp::Lt, r.L(a), r.L(b)), r.I(-1),
                    r.iff(r.bin(BinOp::Gt, r.L(a), r.L(b)), r.I(1), r.I(0))))));
    r.add(r.iff(r.either(r.is(r.L(ta), "double"), r.is(r.L(tb), "double")),
                r.blk({r.set(fa, r.call("$tofloat", {r.L(a)})),
                       r.set(fb, r.call("$tofloat", {r.L(b)})),
                       r.ret(r.iff(r.bin(BinOp::Lt, r.L(fa), r.L(fb)), r.I(-1),
                                   r.iff(r.bin(BinOp::Gt, r.L(fa), r.L(fb)),
                                         r.I(1), r.I(0))))})));
    r.add(r.set(sa, r.call("$bigsign", {r.L(a)})));
    r.add(r.set(sb, r.call("$bigsign", {r.L(b)})));
    r.add(r.iff(
        r.bin(BinOp::Ne, r.L(sa), r.L(sb)),
        r.ret(r.iff(r.bin(BinOp::Lt, r.L(sa), r.L(sb)), r.I(-1), r.I(1)))));
    r.add(r.set(c, r.call("$ucmp", {r.call("$biglimbs", {r.L(a)}),
                                    r.call("$biglimbs", {r.L(b)})})));
    r.add(r.ret(r.bin(BinOp::Mul, r.L(c), r.L(sa))));
    r.finish("$cmp");
  }

  void rt_eq() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [ta, tb, i, ks, k, f_] =
        r.locals("ta", "tb", "i", "ks", "k", "f");
    r.add(r.set(ta, r.typ(r.L(a))));
    r.add(r.set(tb, r.typ(r.L(b))));
    const auto numeric = [&](NodeId t, NodeId v) {
      return r.either(r.is(t, "int"),
                      r.either(r.is(t, "double"), r.call("$isbig", {v})));
    };
    r.add(r.iff(
        r.both(numeric(r.L(ta), r.L(a)), numeric(r.L(tb), r.L(b))),
        r.ret(r.bin(BinOp::Eq, r.call("$cmp", {r.L(a), r.L(b)}), r.I(0)))));
    r.add(r.iff(r.bin(BinOp::Ne, r.L(ta), r.L(tb)), r.ret(r.Bo(false))));
    r.add(r.iff(r.is(r.L(ta), "nil"), r.ret(r.Bo(true))));
    r.add(r.iff(r.either(r.is(r.L(ta), "bool"), r.is(r.L(ta), "string")),
                r.ret(r.bin(BinOp::Eq, r.L(a), r.L(b)))));
    r.add(r.iff(
        r.is(r.L(ta), "array"),
        r.blk({r.iff(r.bin(BinOp::Ne, r.len(r.L(a)), r.len(r.L(b))),
                     r.ret(r.Bo(false))),
               r.set(i, r.I(0)),
               r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(a))),
                    r.blk({r.iff(r.bin(BinOp::Eq,
                                       r.call("$eq", {r.idx(r.L(a), r.L(i)),
                                                      r.idx(r.L(b), r.L(i))}),
                                       r.Bo(false)),
                                 r.ret(r.Bo(false))),
                           r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})),
               r.ret(r.Bo(true))})));
    r.add(r.iff(
        r.is(r.L(ta), "map"),
        r.blk({r.set(ks, r.in(IntrinsicId::ObjectKeys, {r.L(a)})),
               r.iff(r.bin(BinOp::Ne, r.len(r.L(ks)),
                           r.len(r.in(IntrinsicId::ObjectKeys, {r.L(b)}))),
                     r.ret(r.Bo(false))),
               r.set(i, r.I(0)),
               r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(ks))),
                    r.blk({r.set(k, r.idx(r.L(ks), r.L(i))),
                           r.iff(r.bin(BinOp::Eq, r.has(r.L(b), r.L(k)),
                                       r.Bo(false)),
                                 r.ret(r.Bo(false))),
                           r.iff(r.bin(BinOp::Eq,
                                       r.call("$eq", {r.idx(r.L(a), r.L(k)),
                                                      r.idx(r.L(b), r.L(k))}),
                                       r.Bo(false)),
                                 r.ret(r.Bo(false))),
                           r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})),
               r.ret(r.Bo(true))})));
    r.add(
        r.iff(r.either(r.call("$istup", {r.L(a)}), r.call("$istup", {r.L(b)})),
              r.blk({r.iff(r.bin(BinOp::Eq,
                                 r.both(r.call("$istup", {r.L(a)}),
                                        r.call("$istup", {r.L(b)})),
                                 r.Bo(false)),
                           r.ret(r.Bo(false))),
                     r.ret(r.call("$eq", {r.idx(r.L(a), kTupKey),
                                          r.idx(r.L(b), kTupKey)}))})));
    r.add(r.set(f_, r.call("$dunder", {r.L(a), r.S("\x02__eq__")})));
    r.add(r.iff(r.isnt(r.typ(r.L(f_)), "nil"),
                r.ret(r.call("$truthy",
                             {r.b.call_value(r.L(f_), {r.arr({r.L(a), r.L(b)}),
                                                       r.Nil()})}))));
    r.add(r.ret(r.in(IntrinsicId::Same, {r.L(a), r.L(b)})));
    r.finish("$eq");
  }

  // Python's truthiness: 0, "", [], {} and None are false. Value::truthy()
  // agrees about 0 and None and disagrees about the other three, which its
  // own comment names as the reason it will not decide.
  void rt_truthy() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [t] = r.locals("t");
    r.add(r.set(t, r.typ(r.L(v))));
    r.add(r.iff(r.is(r.L(t), "nil"), r.ret(r.Bo(false))));
    r.add(r.iff(r.is(r.L(t), "bool"), r.ret(r.L(v))));
    r.add(r.iff(r.is(r.L(t), "int"), r.ret(r.bin(BinOp::Ne, r.L(v), r.I(0)))));
    r.add(r.iff(r.is(r.L(t), "double"),
                r.ret(r.bin(BinOp::Ne, r.L(v), r.D(0.0)))));
    r.add(r.iff(r.either(r.is(r.L(t), "string"),
                         r.either(r.is(r.L(t), "array"), r.is(r.L(t), "map"))),
                r.ret(r.bin(BinOp::Gt, r.len(r.L(v)), r.I(0)))));
    r.add(r.ret(r.Bo(true)));
    r.finish("$truthy");
  }

  // -- Display -------------------------------------------------------------
  //
  // Python's float repr is shortest-round-trip with a ".0" forced onto a
  // whole value -- which is to_display's output plus the one rule
  // to_display's comment says a front end should add for itself.
  void rt_fstr() {
    const double lim = 9007199254740992.0;
    RT r(*this);
    const auto [d] = r.params("d");
    const auto [i] = r.locals("i");
    r.add(r.iff(r.bin(BinOp::Ne, r.L(d), r.L(d)), r.ret(r.S("nan"))));
    r.add(r.iff(r.in(IntrinsicId::Same, {r.L(d), r.D(-0.0)}),
                r.ret(r.S("-0.0"))));
    r.add(r.iff(
        r.both(r.bin(BinOp::Gt, r.L(d), r.D(-lim)),
               r.bin(BinOp::Lt, r.L(d), r.D(lim))),
        r.blk({r.set(i, r.in(IntrinsicId::ToInt, {r.L(d)})),
               r.iff(r.bin(BinOp::Eq, r.in(IntrinsicId::ToDouble, {r.L(i)}),
                           r.L(d)),
                     r.ret(r.bin(BinOp::Add, r.in(IntrinsicId::ToStr, {r.L(i)}),
                                 r.S(".0"))))})));
    r.add(r.ret(r.in(IntrinsicId::ToStr, {r.L(d)})));
    r.finish("$fstr");
  }

  void rt_str() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [t, f] = r.locals("t", "f");
    r.add(r.set(t, r.typ(r.L(v))));
    r.add(r.iff(r.is(r.L(t), "string"), r.ret(r.L(v))));
    r.add(r.iff(r.is(r.L(t), "nil"), r.ret(r.S("None"))));
    r.add(r.iff(r.is(r.L(t), "bool"),
                r.ret(r.iff(r.L(v), r.S("True"), r.S("False")))));
    r.add(
        r.iff(r.is(r.L(t), "int"), r.ret(r.in(IntrinsicId::ToStr, {r.L(v)}))));
    r.add(r.iff(r.is(r.L(t), "double"), r.ret(r.call("$fstr", {r.L(v)}))));
    r.add(r.iff(r.is(r.L(t), "array"), r.ret(r.call("$liststr", {r.L(v)}))));
    r.add(r.iff(r.is(r.L(t), "map"), r.ret(r.call("$dictstr", {r.L(v)}))));
    r.add(r.iff(r.is(r.L(t), "function"), r.ret(r.S("<function>"))));
    r.add(r.iff(r.is(r.L(t), "generator"), r.ret(r.S("<generator>"))));
    r.add(r.iff(r.call("$isbig", {r.L(v)}), r.ret(r.call("$bstr", {r.L(v)}))));
    r.add(r.iff(r.has(r.L(v), r.S(kTupKey)),
                r.ret(r.call("$tupstr", {r.idx(r.L(v), kTupKey)}))));
    r.add(r.iff(r.has(r.L(v), r.S(kExcKey)),
                r.ret(r.call("$str", {r.idx(r.L(v), kMsgKey)}))));
    r.add(r.set(f, r.call("$dunder", {r.L(v), r.S("\x02__str__")})));
    r.add(r.iff(r.isnt(r.typ(r.L(f)), "nil"),
                r.ret(r.call("$str", {r.b.call_value(r.L(f), {r.arr({r.L(v)}),
                                                              r.Nil()})}))));
    // A class of the program's own that derives from a builtin exception
    // displays as its message, the way the builtin ones do.
    r.add(r.iff(r.has(r.L(v), r.S(kMsgKey)),
                r.ret(r.call("$str", {r.idx(r.L(v), kMsgKey)}))));
    r.add(r.iff(r.has(r.L(v), r.S("__name__")),
                r.ret(r.bin(BinOp::Add,
                            r.bin(BinOp::Add, r.S("<class '"),
                                  r.idx(r.L(v), r.S("__name__"))),
                            r.S("'>")))));
    r.add(r.iff(r.has(r.L(v), r.S(kClassKey)),
                r.ret(r.bin(BinOp::Add,
                            r.bin(BinOp::Add, r.S("<"),
                                  r.idx(r.idx(r.L(v), kClassKey), kNameKey)),
                            r.S(" object>")))));
    r.add(r.ret(r.S("<object>")));
    r.finish("$str");
  }

  void rt_repr() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [f] = r.locals("f");
    r.add(r.iff(r.is(r.typ(r.L(v)), "string"),
                r.ret(r.bin(BinOp::Add, r.bin(BinOp::Add, r.S("'"), r.L(v)),
                            r.S("'")))));
    r.add(r.set(f, r.call("$dunder", {r.L(v), r.S("\x02__repr__")})));
    r.add(r.iff(r.isnt(r.typ(r.L(f)), "nil"),
                r.ret(r.call("$str", {r.b.call_value(r.L(f), {r.arr({r.L(v)}),
                                                              r.Nil()})}))));
    r.add(r.ret(r.call("$str", {r.L(v)})));
    r.finish("$repr");
  }

  void rt_liststr() {
    RT r(*this);
    const auto [a] = r.params("a");
    const auto [out, i] = r.locals("out", "i");
    r.add(r.set(out, r.S("[")));
    r.add(r.set(i, r.I(0)));
    r.add(
        r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(a))),
             r.blk({r.iff(r.bin(BinOp::Gt, r.L(i), r.I(0)),
                          r.set(out, r.bin(BinOp::Add, r.L(out), r.S(", ")))),
                    r.set(out, r.bin(BinOp::Add, r.L(out),
                                     r.call("$repr", {r.idx(r.L(a), r.L(i))}))),
                    r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.bin(BinOp::Add, r.L(out), r.S("]"))));
    r.finish("$liststr");
  }

  void rt_dictstr() {
    RT r(*this);
    const auto [d] = r.params("d");
    const auto [out, i, ks, k] = r.locals("out", "i", "ks", "k");
    r.add(r.set(out, r.S("{")));
    r.add(r.set(i, r.I(0)));
    r.add(r.set(ks, r.in(IntrinsicId::ObjectKeys, {r.L(d)})));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(ks))),
        r.blk(
            {r.iff(r.bin(BinOp::Gt, r.L(i), r.I(0)),
                   r.set(out, r.bin(BinOp::Add, r.L(out), r.S(", ")))),
             r.set(k, r.idx(r.L(ks), r.L(i))),
             r.set(out, r.bin(BinOp::Add, r.L(out),
                              r.bin(BinOp::Add,
                                    r.bin(BinOp::Add, r.call("$repr", {r.L(k)}),
                                          r.S(": ")),
                                    r.call("$repr", {r.idx(r.L(d), r.L(k))})))),
             r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.bin(BinOp::Add, r.L(out), r.S("}"))));
    r.finish("$dictstr");
  }

  // -- Containers, attributes, iteration, exceptions ----------------------

  void rt_exc() {
    RT r(*this);
    const auto [name, msg] = r.params("name", "msg");
    r.add(r.b.make_throw(r.obj({{kExcKey, r.L(name)}, {kMsgKey, r.L(msg)}})));
    r.finish("$exc");
  }

  // `except Exception` catches everything, which is close enough to
  // Python's hierarchy for a subset with no inheritance.
  void rt_isexc() {
    RT r(*this);
    const auto [e, name] = r.params("e", "name");
    // A builtin exception travels as its name; a class of the program's own
    // travels as its value, and then the test is the identity walk.
    r.add(r.iff(r.isnt(r.typ(r.L(name)), "string"),
                r.ret(r.call("$isinstv", {r.L(e), r.L(name)}))));
    r.add(r.iff(r.is(r.L(name), "Exception"), r.ret(r.Bo(true))));
    r.add(r.ret(r.call("$isname", {r.L(e), r.L(name)})));
    r.finish("$isexc");
  }

  void rt_len() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [t] = r.locals("t");
    r.add(r.set(v, r.call("$untup", {r.L(v)})));
    r.add(r.set(t, r.typ(r.L(v))));
    r.add(r.iff(r.either(r.is(r.L(t), "string"),
                         r.either(r.is(r.L(t), "array"), r.is(r.L(t), "map"))),
                r.ret(r.len(r.L(v)))));
    r.add(r.ret(r.call("$exc", {r.S("TypeError"),
                                r.S("object has no len()")})));
    r.finish("$len");
  }

  // A negative index counts from the end, and out of range raises rather
  // than trapping -- Python's rules, written here because index_error()'s
  // own comment says a language that wants them normalizes first.
  void rt_idx() {
    RT r(*this);
    const auto [v, k] = r.params("v", "k");
    const auto [t, i] = r.locals("t", "i");
    r.add(r.set(v, r.call("$untup", {r.L(v)})));
    r.add(r.set(t, r.typ(r.L(v))));
    r.add(r.iff(
        r.either(r.is(r.L(t), "array"), r.is(r.L(t), "string")),
        r.blk({r.set(i, r.L(k)),
               r.iff(r.bin(BinOp::Lt, r.L(i), r.I(0)),
                     r.set(i, r.bin(BinOp::Add, r.L(i), r.len(r.L(v))))),
               r.iff(r.either(r.bin(BinOp::Lt, r.L(i), r.I(0)),
                              r.bin(BinOp::Ge, r.L(i), r.len(r.L(v)))),
                     r.ret(r.call("$exc",
                                  {r.S("IndexError"),
                                   r.iff(r.is(r.L(t), "array"),
                                         r.S("list index out of range"),
                                         r.S("string index out of range"))}))),
               r.ret(r.idx(r.L(v), r.L(i)))})));
    r.add(r.iff(
        r.is(r.L(t), "map"),
        r.blk({r.iff(r.has(r.L(v), r.L(k)), r.ret(r.idx(r.L(v), r.L(k)))),
               r.ret(r.call("$exc",
                            {r.S("KeyError"), r.call("$repr", {r.L(k)})}))})));
    r.add(r.ret(r.call("$exc", {r.S("TypeError"),
                                r.S("object is not subscriptable")})));
    r.finish("$idx");
  }

  void rt_setidx() {
    RT r(*this);
    const auto [v, k, val] = r.params("v", "k", "val");
    const auto [t, i] = r.locals("t", "i");
    r.add(r.set(t, r.typ(r.L(v))));
    r.add(r.iff(
        r.is(r.L(t), "array"),
        r.blk({r.set(i, r.L(k)),
               r.iff(r.bin(BinOp::Lt, r.L(i), r.I(0)),
                     r.set(i, r.bin(BinOp::Add, r.L(i), r.len(r.L(v))))),
               r.iff(r.either(r.bin(BinOp::Lt, r.L(i), r.I(0)),
                              r.bin(BinOp::Ge, r.L(i), r.len(r.L(v)))),
                     r.ret(r.call("$exc", {r.S("IndexError"),
                                           r.S("list assignment index out of "
                                               "range")}))),
               r.sidx(r.L(v), r.L(i), r.L(val)), r.ret(r.L(val))})));
    r.add(r.iff(r.is(r.L(t), "map"),
                r.blk({r.sidx(r.L(v), r.L(k), r.L(val)), r.ret(r.L(val))})));
    r.add(r.ret(r.call("$exc", {r.S("TypeError"),
                                r.S("object does not support item "
                                    "assignment")})));
    r.finish("$setidx");
  }

  // Python's slice: absent ends default, a negative one counts from the
  // end, and both are clamped rather than refused.
  void rt_slice() {
    RT r(*this);
    const auto [v, i, j] = r.params("v", "i", "j");
    const auto [n, a, b] = r.locals("n", "a", "b");
    const auto norm = [&](FuncWriter::Slot out, FuncWriter::Slot arg,
                          NodeId dflt) {
      return r.iff(
          r.is(r.typ(r.L(arg)), "nil"), r.set(out, dflt),
          r.blk(
              {r.set(out, r.L(arg)),
               r.iff(r.bin(BinOp::Lt, r.L(out), r.I(0)),
                     r.set(out, r.bin(BinOp::Add, r.L(n), r.L(out)))),
               r.iff(r.bin(BinOp::Lt, r.L(out), r.I(0)), r.set(out, r.I(0))),
               r.iff(r.bin(BinOp::Gt, r.L(out), r.L(n)), r.set(out, r.L(n)))}));
    };
    // A slice of a tuple is a tuple.
    r.add(
        r.iff(r.call("$istup", {r.L(v)}),
              r.ret(r.call("$tuple", {r.call("$slice", {r.idx(r.L(v), kTupKey),
                                                        r.L(i), r.L(j)})}))));
    r.add(r.set(n, r.len(r.L(v))));
    r.add(norm(a, i, r.I(0)));
    r.add(norm(b, j, r.L(n)));
    r.add(r.iff(r.bin(BinOp::Lt, r.L(b), r.L(a)), r.set(b, r.L(a))));
    r.add(
        r.iff(r.is(r.typ(r.L(v)), "string"),
              r.ret(r.in(IntrinsicId::StrSlice, {r.L(v), r.L(a), r.L(b)})),
              r.ret(r.in(IntrinsicId::ArraySlice, {r.L(v), r.L(a), r.L(b)}))));
    r.finish("$slice");
  }

  void rt_in() {
    RT r(*this);
    const auto [needle, hay] = r.params("needle", "hay");
    const auto [t, i] = r.locals("t", "i");
    r.add(r.set(hay, r.call("$untup", {r.L(hay)})));
    r.add(r.set(t, r.typ(r.L(hay))));
    r.add(r.iff(r.is(r.L(t), "map"), r.ret(r.has(r.L(hay), r.L(needle)))));
    // `"e" in "hello"` is a substring test, not a membership one.
    r.add(
        r.iff(r.is(r.L(t), "string"),
              r.blk({r.set(i, r.I(0)),
                     r.wh(r.bin(BinOp::Le,
                                r.bin(BinOp::Add, r.L(i), r.len(r.L(needle))),
                                r.len(r.L(hay))),
                          r.blk({r.iff(r.bin(BinOp::Eq,
                                             r.in(IntrinsicId::StrSlice,
                                                  {r.L(hay), r.L(i),
                                                   r.bin(BinOp::Add, r.L(i),
                                                         r.len(r.L(needle)))}),
                                             r.L(needle)),
                                       r.ret(r.Bo(true))),
                                 r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})),
                     r.ret(r.Bo(false))})));
    r.add(r.iff(
        r.is(r.L(t), "array"),
        r.blk({r.set(i, r.I(0)),
               r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(hay))),
                    r.blk({r.iff(r.call("$eq",
                                        {r.L(needle), r.idx(r.L(hay), r.L(i))}),
                                 r.ret(r.Bo(true))),
                           r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})),
               r.ret(r.Bo(false))})));
    r.add(r.ret(r.Bo(false)));
    r.finish("$in");
  }

  void rt_getattr() {
    RT r(*this);
    const auto [o, name] = r.params("o", "name");
    const auto [cls, key] = r.locals("cls", "key");
    r.add(r.iff(
        r.is(r.typ(r.L(o)), "object"),
        r.blk(
            {r.iff(r.has(r.L(o), r.L(name)), r.ret(r.idx(r.L(o), r.L(name)))),
             r.iff(
                 r.has(r.L(o), r.S(kClassKey)),
                 r.blk({r.set(key, r.bin(BinOp::Add, r.S("\x02"), r.L(name))),
                        r.set(cls, r.call("$clsfind", {r.idx(r.L(o), kClassKey),
                                                       r.L(key)})),
                        r.iff(r.isnt(r.typ(r.L(cls)), "nil"),
                              r.ret(r.L(cls)))}))})));
    r.add(r.ret(
        r.call("$exc", {r.S("AttributeError"),
                        r.bin(BinOp::Add, r.S("no attribute "), r.L(name))})));
    r.finish("$getattr");
  }

  void rt_setattr() {
    RT r(*this);
    const auto [o, name, val] = r.params("o", "name", "val");
    r.add(r.iff(r.is(r.typ(r.L(o)), "object"),
                r.blk({r.sidx(r.L(o), r.L(name), r.L(val)), r.ret(r.L(val))})));
    r.add(r.ret(r.call("$exc", {r.S("AttributeError"),
                                r.S("cannot set attribute")})));
    r.finish("$setattr");
  }

  void rt_iter() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [t] = r.locals("t");
    r.add(r.set(v, r.call("$untup", {r.L(v)})));
    r.add(r.set(t, r.typ(r.L(v))));
    r.add(r.iff(r.is(r.L(t), "generator"),
                r.ret(r.obj({{"k", r.S("g")}, {"v", r.L(v)}}))));
    r.add(r.iff(r.is(r.L(t), "map"),
                r.ret(r.obj({{"k", r.S("a")},
                             {"v", r.in(IntrinsicId::ObjectKeys, {r.L(v)})},
                             {"i", r.I(0)}}))));
    r.add(r.iff(r.either(r.is(r.L(t), "array"), r.is(r.L(t), "string")),
                r.ret(r.obj({{"k", r.S("a")}, {"v", r.L(v)}, {"i", r.I(0)}}))));
    r.add(r.ret(r.call("$exc", {r.S("TypeError"),
                                r.S("object is not iterable")})));
    r.finish("$iter");
  }

  void rt_iternext() {
    RT r(*this);
    const auto [it] = r.params("it");
    const auto [a, i] = r.locals("a", "i");
    r.add(r.iff(
        r.is(r.idx(r.L(it), "k"), "g"),
        r.ret(r.in(IntrinsicId::GenResume, {r.idx(r.L(it), "v"), r.Nil()}))));
    r.add(r.set(a, r.idx(r.L(it), "v")));
    r.add(r.set(i, r.idx(r.L(it), "i")));
    r.add(r.iff(r.bin(BinOp::Ge, r.L(i), r.len(r.L(a))),
                r.ret(r.obj({{"value", r.Nil()}, {"done", r.Bo(true)}}))));
    r.add(r.sidx(r.L(it), r.S("i"), r.bin(BinOp::Add, r.L(i), r.I(1))));
    r.add(r.ret(
        r.obj({{"value", r.idx(r.L(a), r.L(i))}, {"done", r.Bo(false)}})));
    r.finish("$iternext");
  }

  void rt_range() {
    RT r(*this);
    const auto [start, stop, step] = r.params("start", "stop", "step");
    const auto [out, i] = r.locals("out", "i");
    r.add(r.set(out, r.arr({})));
    r.add(r.set(i, r.L(start)));
    r.add(r.wh(r.iff(r.bin(BinOp::Gt, r.L(step), r.I(0)),
                     r.bin(BinOp::Lt, r.L(i), r.L(stop)),
                     r.bin(BinOp::Gt, r.L(i), r.L(stop))),
               r.blk({r.push(r.L(out), r.L(i)),
                      r.set(i, r.bin(BinOp::Add, r.L(i), r.L(step)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$range");
  }

  // Python's type names, which are neither the VM's nor visible to it: a
  // bignum is an `int` here even though it is an object, and a class
  // instance answers its own class's name.
  void rt_typename() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [t] = r.locals("t");
    r.add(r.set(t, r.typ(r.L(v))));
    r.add(r.iff(r.is(r.L(t), "nil"), r.ret(r.S("NoneType"))));
    r.add(r.iff(r.is(r.L(t), "bool"), r.ret(r.S("bool"))));
    r.add(r.iff(r.is(r.L(t), "int"), r.ret(r.S("int"))));
    r.add(r.iff(r.is(r.L(t), "double"), r.ret(r.S("float"))));
    r.add(r.iff(r.is(r.L(t), "string"), r.ret(r.S("str"))));
    r.add(r.iff(r.is(r.L(t), "array"), r.ret(r.S("list"))));
    r.add(r.iff(r.is(r.L(t), "map"), r.ret(r.S("dict"))));
    r.add(r.iff(r.is(r.L(t), "function"), r.ret(r.S("function"))));
    r.add(r.iff(r.is(r.L(t), "generator"), r.ret(r.S("generator"))));
    r.add(r.iff(r.call("$isbig", {r.L(v)}), r.ret(r.S("int"))));
    r.add(r.iff(r.call("$istup", {r.L(v)}), r.ret(r.S("tuple"))));
    r.add(r.iff(r.has(r.L(v), r.S(kExcKey)), r.ret(r.idx(r.L(v), kExcKey))));
    r.add(r.iff(r.has(r.L(v), r.S(kClassKey)),
                r.ret(r.idx(r.idx(r.L(v), kClassKey), kNameKey))));
    r.add(r.ret(r.S("object")));
    r.finish("$typename");
  }

  void rt_type() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.ret(r.obj({{"__name__", r.call("$typename", {r.L(v)})}})));
    r.finish("$type");
  }

  void rt_join() {
    RT r(*this);
    const auto [sep, xs] = r.params("sep", "xs");
    const auto [out, i] = r.locals("out", "i");
    r.add(r.set(out, r.S("")));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(xs))),
        r.blk({r.iff(r.bin(BinOp::Gt, r.L(i), r.I(0)),
                     r.set(out, r.bin(BinOp::Add, r.L(out), r.L(sep)))),
               r.set(out, r.bin(BinOp::Add, r.L(out), r.idx(r.L(xs), r.L(i)))),
               r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$join");
  }

  void rt_dget() {
    RT r(*this);
    const auto [d, k, dflt] = r.params("d", "k", "dflt");
    r.add(r.iff(r.has(r.L(d), r.L(k)), r.ret(r.idx(r.L(d), r.L(k)))));
    r.add(r.ret(r.L(dflt)));
    r.finish("$dget");
  }

  void rt_tolist() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [out, it, st] = r.locals("out", "it", "st");
    r.add(r.set(out, r.arr({})));
    r.add(r.set(it, r.call("$iter", {r.L(v)})));
    r.add(
        r.wh(r.Bo(true), r.blk({r.set(st, r.call("$iternext", {r.L(it)})),
                                r.iff(r.idx(r.L(st), "done"), r.b.make_break()),
                                r.push(r.L(out), r.idx(r.L(st), "value"))})));
    r.add(r.ret(r.L(out)));
    r.finish("$tolist");
  }

  void rt_toint() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [s, i, neg, acc, c] = r.locals("s", "i", "neg", "acc", "c");
    r.add(r.iff(r.is(r.typ(r.L(v)), "double"),
                r.ret(r.in(IntrinsicId::ToInt, {r.L(v)}))));
    r.add(r.iff(r.is(r.typ(r.L(v)), "bool"),
                r.ret(r.iff(r.L(v), r.I(1), r.I(0)))));
    // `int("...")` accumulates through $mul and $add, so a literal too big
    // for an int64 becomes a bignum with nothing further to say about it.
    r.add(r.iff(
        r.is(r.typ(r.L(v)), "string"),
        r.blk(
            {r.set(s, r.call("$strip", {r.L(v), r.I(0)})), r.set(i, r.I(0)),
             r.set(neg, r.Bo(false)),
             r.iff(r.bin(BinOp::Gt, r.len(r.L(s)), r.I(0)),
                   r.blk({r.iff(r.bin(BinOp::Eq,
                                      r.in(IntrinsicId::StrSlice,
                                           {r.L(s), r.I(0), r.I(1)}),
                                      r.S("-")),
                                r.blk({r.set(neg, r.Bo(true)),
                                       r.set(i, r.I(1))})),
                          r.iff(r.bin(BinOp::Eq,
                                      r.in(IntrinsicId::StrSlice,
                                           {r.L(s), r.I(0), r.I(1)}),
                                      r.S("+")),
                                r.set(i, r.I(1)))})),
             r.iff(r.bin(BinOp::Ge, r.L(i), r.len(r.L(s))),
                   r.call("$intfail", {r.L(v)})),
             r.set(acc, r.I(0)),
             r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(s))),
                  r.blk(
                      {r.set(c, r.in(IntrinsicId::StrByte, {r.L(s), r.L(i)})),
                       r.iff(r.either(r.bin(BinOp::Lt, r.L(c), r.I(48)),
                                      r.bin(BinOp::Gt, r.L(c), r.I(57))),
                             r.call("$intfail", {r.L(v)})),
                       r.set(acc, r.call("$add",
                                         {r.call("$mul", {r.L(acc), r.I(10)}),
                                          r.bin(BinOp::Sub, r.L(c), r.I(48))})),
                       r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})),
             r.ret(r.iff(r.L(neg), r.call("$neg", {r.L(acc)}), r.L(acc)))})));
    r.add(r.ret(r.L(v)));
    r.finish("$toint");
  }

  void rt_intfail() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.call("$exc", {r.S("ValueError"),
                          r.bin(BinOp::Add,
                                r.S("invalid literal for int() with base 10: "),
                                r.call("$repr", {r.L(v)}))}));
    r.add(r.ret(r.Nil()));
    r.finish("$intfail");
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
    const auto [x, a] = r.params("x", "a");
    const auto [out, i] = r.locals("out", "i");
    r.add(r.set(out, r.arr({r.L(x)})));
    r.add(r.iff(r.isnt(r.typ(r.L(a)), "array"), r.ret(r.L(out))));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(a))),
               r.blk({r.push(r.L(out), r.idx(r.L(a), r.L(i))),
                      r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$acons");
  }

  void rt_aext() {  // `f(*xs)` -- any iterable, not only a list
    RT r(*this);
    const auto [dst, src] = r.params("dst", "src");
    const auto [it, st] = r.locals("it", "st");
    r.add(r.set(it, r.call("$iter", {r.L(src)})));
    r.add(
        r.wh(r.Bo(true), r.blk({r.set(st, r.call("$iternext", {r.L(it)})),
                                r.iff(r.idx(r.L(st), "done"), r.b.make_break()),
                                r.push(r.L(dst), r.idx(r.L(st), "value"))})));
    r.add(r.ret(r.L(dst)));
    r.finish("$aext");
  }

  void rt_rest() {  // `*rest` -- what the declared parameters did not take
    RT r(*this);
    const auto [a, i] = r.params("a", "i");
    const auto [n] = r.locals("n");
    r.add(r.set(n, r.len(r.L(a))));
    r.add(r.iff(r.bin(BinOp::Le, r.L(n), r.L(i)), r.ret(r.arr({}))));
    r.add(r.ret(r.in(IntrinsicId::ArraySlice, {r.L(a), r.L(i), r.L(n)})));
    r.finish("$rest");
  }

  void rt_kwhas() {
    RT r(*this);
    const auto [k, n] = r.params("k", "n");
    r.add(r.iff(r.is(r.typ(r.L(k)), "nil"), r.ret(r.Bo(false))));
    r.add(r.ret(r.has(r.L(k), r.L(n))));
    r.finish("$kwhas");
  }

  void rt_hasname() {
    RT r(*this);
    const auto [names, n] = r.params("names", "n");
    const auto [i] = r.locals("i");
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(names))),
               r.blk({r.iff(r.bin(BinOp::Eq, r.idx(r.L(names), r.L(i)), r.L(n)),
                            r.ret(r.Bo(true))),
                      r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.Bo(false)));
    r.finish("$hasname");
  }

  void rt_kwrest() {  // `**opts` -- a real dict, so the body can iterate it
    RT r(*this);
    const auto [k, names] = r.params("k", "names");
    const auto [out, ks, i, key] = r.locals("out", "ks", "i", "key");
    r.add(r.set(out, r.in(IntrinsicId::MapNew, {})));
    r.add(r.iff(r.is(r.typ(r.L(k)), "nil"), r.ret(r.L(out))));
    r.add(r.set(ks, r.in(IntrinsicId::ObjectKeys, {r.L(k)})));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(ks))),
        r.blk(
            {r.set(key, r.idx(r.L(ks), r.L(i))),
             r.iff(r.bin(BinOp::Eq, r.call("$hasname", {r.L(names), r.L(key)}),
                         r.Bo(false)),
                   r.sidx(r.L(out), r.L(key), r.idx(r.L(k), r.L(key)))),
             r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$kwrest");
  }

  void rt_kwcheck() {  // no `**kwargs`: an unexpected keyword is a TypeError
    RT r(*this);
    const auto [k, names, fn] = r.params("k", "names", "fn");
    const auto [ks, i, key] = r.locals("ks", "i", "key");
    r.add(r.iff(r.is(r.typ(r.L(k)), "nil"), r.ret(r.Nil())));
    r.add(r.set(ks, r.in(IntrinsicId::ObjectKeys, {r.L(k)})));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(ks))),
        r.blk(
            {r.set(key, r.idx(r.L(ks), r.L(i))),
             r.iff(r.bin(BinOp::Eq, r.call("$hasname", {r.L(names), r.L(key)}),
                         r.Bo(false)),
                   r.call("$exc",
                          {r.S("TypeError"),
                           r.bin(BinOp::Add,
                                 r.bin(BinOp::Add, r.L(fn),
                                       r.S("() got an unexpected keyword "
                                           "argument '")),
                                 r.bin(BinOp::Add, r.L(key), r.S("'")))})),
             r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.Nil()));
    r.finish("$kwcheck");
  }

  void rt_kwmerge() {  // `f(**d)`
    RT r(*this);
    const auto [dst, src] = r.params("dst", "src");
    const auto [ks, i, key] = r.locals("ks", "i", "key");
    r.add(r.iff(r.is(r.typ(r.L(src)), "nil"), r.ret(r.L(dst))));
    r.add(r.set(ks, r.in(IntrinsicId::ObjectKeys, {r.L(src)})));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(ks))),
               r.blk({r.set(key, r.idx(r.L(ks), r.L(i))),
                      r.sidx(r.L(dst), r.L(key), r.idx(r.L(src), r.L(key))),
                      r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(dst)));
    r.finish("$kwmerge");
  }

  // Both diagnostics are Python's own, down to the comma before "and" and
  // the singular "argument" -- because `python3` is the oracle, and a
  // message is output like any other. Neither is something the executor
  // could have raised: its arity check knows a count, and these know which
  // parameter, by name, and whether it had a default.
  void rt_missing() {
    RT r(*this);
    const auto [fn, names, idxs, a, k] =
        r.params("fn", "names", "idxs", "a", "k");
    const auto [miss, i, n, c, s] = r.locals("miss", "i", "n", "c", "s");
    // Every required parameter no positional and no keyword answered.
    r.add(r.set(miss, r.arr({})));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(names))),
               r.blk({r.set(n, r.idx(r.L(names), r.L(i))),
                      r.iff(r.bin(BinOp::Eq,
                                  r.either(r.bin(BinOp::Gt, r.len(r.L(a)),
                                                 r.idx(r.L(idxs), r.L(i))),
                                           r.call("$kwhas", {r.L(k), r.L(n)})),
                                  r.Bo(false)),
                            r.push(r.L(miss),
                                   r.bin(BinOp::Add,
                                         r.bin(BinOp::Add, r.S("'"), r.L(n)),
                                         r.S("'")))),
                      r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.set(c, r.len(r.L(miss))));
    // "'a'", "'a' and 'b'", "'a', 'b', and 'c'" -- three shapes, and the
    // last one keeps the serial comma.
    r.add(r.set(s, r.idx(r.L(miss), r.I(0))));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(c), r.I(2)),
                r.set(s, r.bin(BinOp::Add, r.L(s),
                               r.bin(BinOp::Add, r.S(" and "),
                                     r.idx(r.L(miss), r.I(1)))))));
    r.add(r.iff(
        r.bin(BinOp::Gt, r.L(c), r.I(2)),
        r.blk({r.set(i, r.I(1)),
               r.wh(r.bin(BinOp::Lt, r.L(i), r.bin(BinOp::Sub, r.L(c), r.I(1))),
                    r.blk({r.set(s, r.bin(BinOp::Add, r.L(s),
                                          r.bin(BinOp::Add, r.S(", "),
                                                r.idx(r.L(miss), r.L(i))))),
                           r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})),
               r.set(s, r.bin(BinOp::Add, r.L(s),
                              r.bin(BinOp::Add, r.S(", and "),
                                    r.idx(r.L(miss), r.L(i)))))})));
    r.add(r.call(
        "$exc",
        {r.S("TypeError"),
         r.bin(BinOp::Add, r.bin(BinOp::Add, r.L(fn), r.S("() missing ")),
               r.bin(BinOp::Add, r.call("$str", {r.L(c)}),
                     r.bin(BinOp::Add,
                           r.iff(r.bin(BinOp::Eq, r.L(c), r.I(1)),
                                 r.S(" required positional argument: "),
                                 r.S(" required positional arguments: ")),
                           r.L(s))))}));
    r.finish("$missing");
  }

  void rt_toomany() {
    RT r(*this);
    const auto [fn, lo, hi, got] = r.params("fn", "lo", "hi", "got");
    const auto [s] = r.locals("s");
    // "takes 2 positional arguments", or "takes from 1 to 3" when some of
    // them had defaults.
    r.add(r.set(s, r.bin(BinOp::Add, r.call("$str", {r.L(hi)}),
                         r.iff(r.bin(BinOp::Eq, r.L(hi), r.I(1)),
                               r.S(" positional argument but "),
                               r.S(" positional arguments but ")))));
    r.add(r.iff(r.bin(BinOp::Ne, r.L(lo), r.L(hi)),
                r.set(s, r.bin(BinOp::Add,
                               r.bin(BinOp::Add, r.S("from "),
                                     r.call("$str", {r.L(lo)})),
                               r.bin(BinOp::Add,
                                     r.bin(BinOp::Add, r.S(" to "),
                                           r.call("$str", {r.L(hi)})),
                                     r.S(" positional arguments but "))))));
    r.add(r.call(
        "$exc",
        {r.S("TypeError"),
         r.bin(BinOp::Add, r.bin(BinOp::Add, r.L(fn), r.S("() takes ")),
               r.bin(BinOp::Add, r.L(s),
                     r.bin(BinOp::Add, r.call("$str", {r.L(got)}),
                           r.iff(r.bin(BinOp::Eq, r.L(got), r.I(1)),
                                 r.S(" was given"), r.S(" were given")))))}));
    r.finish("$toomany");
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
    const auto [t, key] = r.params("t", "key");
    const auto [c] = r.locals("c");
    r.add(r.set(c, r.L(t)));
    r.add(r.wh(
        r.is(r.typ(r.L(c)), "object"),
        r.blk(
            {r.iff(r.has(r.L(c), r.L(key)), r.ret(r.idx(r.L(c), r.L(key)))),
             r.iff(r.bin(BinOp::Eq, r.has(r.L(c), r.S(kBaseKey)), r.Bo(false)),
                   r.b.make_break()),
             r.set(c, r.idx(r.L(c), kBaseKey))})));
    r.add(r.ret(r.Nil()));
    r.finish("$clsfind");
  }

  // A builtin exception is not a class here, so `except ValueError` matches
  // by name -- and a class of the program's own that derives from one is
  // marked with the name it was rooted at, which is what this also finds.
  void rt_isname() {
    RT r(*this);
    const auto [v, name] = r.params("v", "name");
    const auto [t] = r.locals("t");
    r.add(r.iff(r.isnt(r.typ(r.L(v)), "object"), r.ret(r.Bo(false))));
    r.add(r.iff(r.has(r.L(v), r.S(kExcKey)),
                r.ret(r.bin(BinOp::Eq, r.idx(r.L(v), kExcKey), r.L(name)))));
    r.add(r.iff(r.bin(BinOp::Eq, r.has(r.L(v), r.S(kClassKey)), r.Bo(false)),
                r.ret(r.Bo(false))));
    r.add(r.set(t, r.idx(r.L(v), kClassKey)));
    r.add(r.wh(
        r.is(r.typ(r.L(t)), "object"),
        r.blk(
            {r.iff(r.both(r.has(r.L(t), r.S(kRootKey)),
                          r.bin(BinOp::Eq, r.idx(r.L(t), kRootKey), r.L(name))),
                   r.ret(r.Bo(true))),
             r.iff(r.bin(BinOp::Eq, r.has(r.L(t), r.S(kBaseKey)), r.Bo(false)),
                   r.b.make_break()),
             r.set(t, r.idx(r.L(t), kBaseKey))})));
    r.add(r.ret(r.Bo(false)));
    r.finish("$isname");
  }

  // The identity walk: a class value *is* its constructor closure, so two
  // classes are the same class when `Same` says the closures are.
  void rt_isinstv() {
    RT r(*this);
    const auto [v, cls] = r.params("v", "cls");
    const auto [t] = r.locals("t");
    r.add(r.iff(r.isnt(r.typ(r.L(v)), "object"), r.ret(r.Bo(false))));
    r.add(r.iff(r.bin(BinOp::Eq, r.has(r.L(v), r.S(kClassKey)), r.Bo(false)),
                r.ret(r.Bo(false))));
    r.add(r.set(t, r.idx(r.L(v), kClassKey)));
    r.add(r.wh(r.is(r.typ(r.L(t)), "object"),
               r.blk({r.iff(r.both(r.has(r.L(t), r.S(kIdKey)),
                                   r.in(IntrinsicId::Same,
                                        {r.idx(r.L(t), kIdKey), r.L(cls)})),
                            r.ret(r.Bo(true))),
                      r.iff(r.bin(BinOp::Eq, r.has(r.L(t), r.S(kBaseKey)),
                                  r.Bo(false)),
                            r.b.make_break()),
                      r.set(t, r.idx(r.L(t), kBaseKey))})));
    r.add(r.ret(r.Bo(false)));
    r.finish("$isinstv");
  }

  void rt_isinst() {
    RT r(*this);
    const auto [v, cls] = r.params("v", "cls");
    r.add(r.iff(r.is(r.typ(r.L(cls)), "string"),
                r.ret(r.either(
                    r.bin(BinOp::Eq, r.call("$typename", {r.L(v)}), r.L(cls)),
                    r.call("$isname", {r.L(v), r.L(cls)})))));
    r.add(r.ret(r.call("$isinstv", {r.L(v), r.L(cls)})));
    r.finish("$isinst");
  }

  // `super().m(...)`. The base may be nothing at all -- a class rooted at a
  // builtin exception has a name to be caught by and no table to inherit
  // from -- and then `__init__` is the one method that still means
  // something: it is what stores the message.
  void rt_supercall() {
    RT r(*this);
    const auto [self, base, key, a, k] =
        r.params("self", "base", "key", "a", "k");
    const auto [f] = r.locals("f");
    r.add(r.iff(r.is(r.typ(r.L(base)), "object"),
                r.blk({r.set(f, r.call("$clsfind", {r.L(base), r.L(key)})),
                       r.iff(r.isnt(r.typ(r.L(f)), "nil"),
                             r.ret(r.b.call_value(
                                 r.L(f), {r.call("$acons", {r.L(self), r.L(a)}),
                                          r.L(k)})))})));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(key), r.S("\x02__init__")),
                r.ret(r.call("$excinit", {r.L(self), r.L(a)}))));
    r.add(r.ret(r.call(
        "$exc", {r.S("AttributeError"),
                 r.bin(BinOp::Add, r.S("'super' object has no attribute "),
                       r.in(IntrinsicId::StrSlice,
                            {r.L(key), r.I(1), r.len(r.L(key))}))})));
    r.finish("$supercall");
  }

  void rt_excinit() {
    RT r(*this);
    const auto [self, a] = r.params("self", "a");
    r.add(r.sidx(r.L(self), r.S(kMsgKey),
                 r.iff(r.bin(BinOp::Gt, r.len(r.L(a)), r.I(0)),
                       r.idx(r.L(a), r.I(0)), r.S(""))));
    r.add(r.ret(r.Nil()));
    r.finish("$excinit");
  }

  void rt_noinit() {
    RT r(*this);
    const auto [cls, a] = r.params("cls", "a");
    r.add(r.iff(r.bin(BinOp::Gt, r.len(r.L(a)), r.I(0)),
                r.call("$exc", {r.S("TypeError"),
                                r.bin(BinOp::Add, r.L(cls),
                                      r.S("() takes no arguments"))})));
    r.add(r.ret(r.Nil()));
    r.finish("$noinit");
  }

  // `__str__`, `__repr__`, `__eq__`: found the same way any method is, and
  // called through the same convention.
  void rt_dunder() {
    RT r(*this);
    const auto [v, key] = r.params("v", "key");
    r.add(r.iff(r.isnt(r.typ(r.L(v)), "object"), r.ret(r.Nil())));
    r.add(r.iff(r.bin(BinOp::Eq, r.has(r.L(v), r.S(kClassKey)), r.Bo(false)),
                r.ret(r.Nil())));
    r.add(r.ret(r.call("$clsfind", {r.idx(r.L(v), kClassKey), r.L(key)})));
    r.finish("$dunder");
  }

  // -- Tuples --------------------------------------------------------------
  //
  // The IR has one sequence type and Python has two, which would be a
  // detail if they printed and compared the same -- and they do not. So a
  // tuple is its array under a key no source can write, and the sequence
  // funcs unwrap before they look. See README.md.

  void rt_tuple() {
    RT r(*this);
    const auto [a] = r.params("a");
    r.add(r.ret(r.obj({{kTupKey, r.L(a)}})));
    r.finish("$tuple");
  }

  void rt_untup() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.iff(r.call("$istup", {r.L(v)}), r.ret(r.idx(r.L(v), kTupKey))));
    r.add(r.ret(r.L(v)));
    r.finish("$untup");
  }

  void rt_istup() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.iff(r.isnt(r.typ(r.L(v)), "object"), r.ret(r.Bo(false))));
    r.add(r.ret(r.has(r.L(v), r.S(kTupKey))));
    r.finish("$istup");
  }

  void rt_tupstr() {
    RT r(*this);
    const auto [a] = r.params("a");
    const auto [out, i] = r.locals("out", "i");
    r.add(r.set(out, r.S("(")));
    r.add(r.set(i, r.I(0)));
    r.add(
        r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(a))),
             r.blk({r.iff(r.bin(BinOp::Gt, r.L(i), r.I(0)),
                          r.set(out, r.bin(BinOp::Add, r.L(out), r.S(", ")))),
                    r.set(out, r.bin(BinOp::Add, r.L(out),
                                     r.call("$repr", {r.idx(r.L(a), r.L(i))}))),
                    r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    // `(1,)` -- the comma is what makes a one-element tuple a tuple.
    r.add(r.iff(r.bin(BinOp::Eq, r.len(r.L(a)), r.I(1)),
                r.set(out, r.bin(BinOp::Add, r.L(out), r.S(",")))));
    r.add(r.ret(r.bin(BinOp::Add, r.L(out), r.S(")"))));
    r.finish("$tupstr");
  }

  // `a, b = expr`: any iterable, and exactly as many values as targets.
  void rt_unpack() {
    RT r(*this);
    const auto [v, n] = r.params("v", "n");
    const auto [a] = r.locals("a");
    r.add(r.set(a, r.call("$tolist", {r.L(v)})));
    r.add(r.iff(
        r.bin(BinOp::Lt, r.len(r.L(a)), r.L(n)),
        r.call("$exc", {r.S("ValueError"),
                        r.bin(BinOp::Add,
                              r.bin(BinOp::Add,
                                    r.S("not enough values to unpack "
                                        "(expected "),
                                    r.call("$str", {r.L(n)})),
                              r.bin(BinOp::Add,
                                    r.bin(BinOp::Add, r.S(", got "),
                                          r.call("$str", {r.len(r.L(a))})),
                                    r.S(")")))})));
    r.add(r.iff(r.bin(BinOp::Gt, r.len(r.L(a)), r.L(n)),
                r.call("$exc", {r.S("ValueError"),
                                r.bin(BinOp::Add,
                                      r.bin(BinOp::Add,
                                            r.S("too many values to unpack "
                                                "(expected "),
                                            r.call("$str", {r.L(n)})),
                                      r.S(")"))})));
    r.add(r.ret(r.L(a)));
    r.finish("$unpack");
  }

  void rt_items() {
    RT r(*this);
    const auto [d] = r.params("d");
    const auto [ks, out, i] = r.locals("ks", "out", "i");
    r.add(r.set(ks, r.in(IntrinsicId::ObjectKeys, {r.L(d)})));
    r.add(r.set(out, r.arr({})));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(ks))),
        r.blk({r.push(r.L(out),
                      r.call("$tuple",
                             {r.arr({r.idx(r.L(ks), r.L(i)),
                                     r.idx(r.L(d), r.idx(r.L(ks), r.L(i)))})})),
               r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$items");
  }

  void rt_values() {
    RT r(*this);
    const auto [d] = r.params("d");
    const auto [ks, out, i] = r.locals("ks", "out", "i");
    r.add(r.set(ks, r.in(IntrinsicId::ObjectKeys, {r.L(d)})));
    r.add(r.set(out, r.arr({})));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(ks))),
               r.blk({r.push(r.L(out), r.idx(r.L(d), r.idx(r.L(ks), r.L(i)))),
                      r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$values");
  }

  void rt_enumerate() {
    RT r(*this);
    const auto [v, start] = r.params("v", "start");
    const auto [a, out, i] = r.locals("a", "out", "i");
    r.add(r.set(a, r.call("$tolist", {r.L(v)})));
    r.add(r.set(out, r.arr({})));
    r.add(r.set(i, r.I(0)));
    r.add(
        r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(a))),
             r.blk({r.push(r.L(out),
                           r.call("$tuple",
                                  {r.arr({r.bin(BinOp::Add, r.L(start), r.L(i)),
                                          r.idx(r.L(a), r.L(i))})})),
                    r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$enumerate");
  }

  void rt_zip() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [xs, ys, out, i] = r.locals("xs", "ys", "out", "i");
    r.add(r.set(xs, r.call("$tolist", {r.L(a)})));
    r.add(r.set(ys, r.call("$tolist", {r.L(b)})));
    r.add(r.set(out, r.arr({})));
    r.add(r.set(i, r.I(0)));
    r.add(
        r.wh(r.both(r.bin(BinOp::Lt, r.L(i), r.len(r.L(xs))),
                    r.bin(BinOp::Lt, r.L(i), r.len(r.L(ys)))),
             r.blk({r.push(r.L(out),
                           r.call("$tuple", {r.arr({r.idx(r.L(xs), r.L(i)),
                                                    r.idx(r.L(ys), r.L(i))})})),
                    r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$zip");
  }

  // An insertion sort over the values and their keys at once. Python's is
  // a stable merge sort; this one is stable too, which is the property a
  // program can see -- see README.md for what it is not.
  void rt_sorted() {
    RT r(*this);
    const auto [v, key, rev] = r.params("v", "key", "rev");
    const auto [a, ks, i, vx, kx, j] =
        r.locals("a", "ks", "i", "vx", "kx", "j");
    r.add(r.set(a, r.call("$tolist", {r.L(v)})));
    r.add(r.set(ks, r.arr({})));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(a))),
        r.blk({r.push(r.L(ks),
                      r.iff(r.is(r.typ(r.L(key)), "nil"), r.idx(r.L(a), r.L(i)),
                            r.b.call_value(
                                r.L(key),
                                {r.arr({r.idx(r.L(a), r.L(i))}), r.Nil()}))),
               r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.set(i, r.I(1)));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(a))),
        r.blk({r.set(vx, r.idx(r.L(a), r.L(i))),
               r.set(kx, r.idx(r.L(ks), r.L(i))),
               r.set(j, r.bin(BinOp::Sub, r.L(i), r.I(1))),
               r.wh(r.both(
                        r.bin(BinOp::Ge, r.L(j), r.I(0)),
                        r.bin(BinOp::Gt,
                              r.call("$cmp", {r.idx(r.L(ks), r.L(j)), r.L(kx)}),
                              r.I(0))),
                    r.blk({r.sidx(r.L(a), r.bin(BinOp::Add, r.L(j), r.I(1)),
                                  r.idx(r.L(a), r.L(j))),
                           r.sidx(r.L(ks), r.bin(BinOp::Add, r.L(j), r.I(1)),
                                  r.idx(r.L(ks), r.L(j))),
                           r.set(j, r.bin(BinOp::Sub, r.L(j), r.I(1)))})),
               r.sidx(r.L(a), r.bin(BinOp::Add, r.L(j), r.I(1)), r.L(vx)),
               r.sidx(r.L(ks), r.bin(BinOp::Add, r.L(j), r.I(1)), r.L(kx)),
               r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.iff(r.call("$truthy", {r.L(rev)}),
                r.blk({r.set(ks, r.arr({})), r.set(i, r.len(r.L(a))),
                       r.wh(r.bin(BinOp::Gt, r.L(i), r.I(0)),
                            r.blk({r.set(i, r.bin(BinOp::Sub, r.L(i), r.I(1))),
                                   r.push(r.L(ks), r.idx(r.L(a), r.L(i)))})),
                       r.ret(r.L(ks))})));
    r.add(r.ret(r.L(a)));
    r.finish("$sorted");
  }

  // `next(it)` -- and `next(it, default)`, which is the difference between
  // a StopIteration and a value.
  void rt_next() {
    RT r(*this);
    const auto [g, dflt, hasdflt] = r.params("g", "dflt", "hasdflt");
    const auto [st] = r.locals("st");
    r.add(r.set(st, r.call("$iternext", {r.call("$iter", {r.L(g)})})));
    r.add(r.iff(r.idx(r.L(st), "done"),
                r.blk({r.iff(r.L(hasdflt), r.ret(r.L(dflt))),
                       r.call("$exc", {r.S("StopIteration"), r.S("")})})));
    r.add(r.ret(r.idx(r.L(st), "value")));
    r.finish("$next");
  }

  void rt_sum() {
    RT r(*this);
    const auto [v, start] = r.params("v", "start");
    const auto [a, acc, i] = r.locals("a", "acc", "i");
    r.add(r.set(a, r.call("$tolist", {r.L(v)})));
    r.add(r.set(acc, r.L(start)));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(a))),
        r.blk({r.set(acc, r.call("$add", {r.L(acc), r.idx(r.L(a), r.L(i))})),
               r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(acc)));
    r.finish("$sum");
  }

  void rt_minmax() {
    RT r(*this);
    const auto [v, ismax] = r.params("v", "ismax");
    const auto [a, best, i, c] = r.locals("a", "best", "i", "c");
    r.add(r.set(a, r.call("$tolist", {r.L(v)})));
    r.add(r.iff(
        r.bin(BinOp::Eq, r.len(r.L(a)), r.I(0)),
        r.call("$exc", {r.S("ValueError"), r.S("arg is an empty sequence")})));
    r.add(r.set(best, r.idx(r.L(a), r.I(0))));
    r.add(r.set(i, r.I(1)));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(a))),
        r.blk({r.set(c, r.call("$cmp", {r.idx(r.L(a), r.L(i)), r.L(best)})),
               r.iff(r.iff(r.L(ismax), r.bin(BinOp::Gt, r.L(c), r.I(0)),
                           r.bin(BinOp::Lt, r.L(c), r.I(0))),
                     r.set(best, r.idx(r.L(a), r.L(i)))),
               r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(best)));
    r.finish("$minmax");
  }

  // -- The string, list and dict methods -----------------------------------

  void rt_isspace() {
    RT r(*this);
    const auto [c] = r.params("c");
    r.add(r.ret(
        r.either(r.bin(BinOp::Eq, r.L(c), r.S(" ")),
                 r.either(r.bin(BinOp::Eq, r.L(c), r.S("\t")),
                          r.either(r.bin(BinOp::Eq, r.L(c), r.S("\n")),
                                   r.bin(BinOp::Eq, r.L(c), r.S("\r")))))));
    r.finish("$isspace");
  }

  // `s.split()` runs on whitespace and drops the empties; `s.split(sep)`
  // keeps them. They are different functions wearing one name, which is
  // Python's choice and not this one's.
  void rt_split() {
    RT r(*this);
    const auto [s, sep, hassep] = r.params("s", "sep", "hassep");
    const auto [out, i, cur, st] = r.locals("out", "i", "cur", "st");
    const auto ch = [&](NodeId str, NodeId i) {
      return r.in(IntrinsicId::StrSlice,
                  {str, i, r.bin(BinOp::Add, i, r.I(1))});
    };
    r.add(r.set(out, r.arr({})));
    r.add(r.set(i, r.I(0)));
    r.add(r.iff(
        r.bin(BinOp::Eq, r.L(hassep), r.Bo(false)),
        r.blk({r.set(cur, r.S("")),
               r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(s))),
                    r.blk({r.iff(r.call("$isspace", {ch(r.L(s), r.L(i))}),
                                 r.blk({r.iff(
                                     r.bin(BinOp::Gt, r.len(r.L(cur)), r.I(0)),
                                     r.blk({r.push(r.L(out), r.L(cur)),
                                            r.set(cur, r.S(""))}))}),
                                 r.set(cur, r.bin(BinOp::Add, r.L(cur),
                                                  ch(r.L(s), r.L(i))))),
                           r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})),
               r.iff(r.bin(BinOp::Gt, r.len(r.L(cur)), r.I(0)),
                     r.push(r.L(out), r.L(cur))),
               r.ret(r.L(out))})));
    r.add(r.iff(r.bin(BinOp::Eq, r.len(r.L(sep)), r.I(0)),
                r.call("$exc", {r.S("ValueError"), r.S("empty separator")})));
    r.add(r.set(st, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Le, r.bin(BinOp::Add, r.L(i), r.len(r.L(sep))),
                     r.len(r.L(s))),
               r.blk({r.iff(
                   r.bin(BinOp::Eq,
                         r.in(IntrinsicId::StrSlice,
                              {r.L(s), r.L(i),
                               r.bin(BinOp::Add, r.L(i), r.len(r.L(sep)))}),
                         r.L(sep)),
                   r.blk({r.push(r.L(out), r.in(IntrinsicId::StrSlice,
                                                {r.L(s), r.L(st), r.L(i)})),
                          r.set(i, r.bin(BinOp::Add, r.L(i), r.len(r.L(sep)))),
                          r.set(st, r.L(i))}),
                   r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1))))})));
    r.add(r.push(r.L(out), r.in(IntrinsicId::StrSlice,
                                {r.L(s), r.L(st), r.len(r.L(s))})));
    r.add(r.ret(r.L(out)));
    r.finish("$split");
  }

  void rt_strip() {  // mode 0 both, 1 left, 2 right
    RT r(*this);
    const auto [s, mode] = r.params("s", "mode");
    const auto [i, j] = r.locals("i", "j");
    const auto ch = [&](NodeId i) {
      return r.in(IntrinsicId::StrSlice,
                  {r.L(s), i, r.bin(BinOp::Add, i, r.I(1))});
    };
    r.add(r.set(i, r.I(0)));
    r.add(r.set(j, r.len(r.L(s))));
    r.add(r.iff(r.bin(BinOp::Ne, r.L(mode), r.I(2)),
                r.wh(r.both(r.bin(BinOp::Lt, r.L(i), r.L(j)),
                            r.call("$isspace", {ch(r.L(i))})),
                     r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1))))));
    r.add(r.iff(r.bin(BinOp::Ne, r.L(mode), r.I(1)),
                r.wh(r.both(r.bin(BinOp::Gt, r.L(j), r.L(i)),
                            r.call("$isspace",
                                   {ch(r.bin(BinOp::Sub, r.L(j), r.I(1)))})),
                     r.set(j, r.bin(BinOp::Sub, r.L(j), r.I(1))))));
    r.add(r.ret(r.in(IntrinsicId::StrSlice, {r.L(s), r.L(i), r.L(j)})));
    r.finish("$strip");
  }

  void rt_replace() {
    RT r(*this);
    const auto [s, a, b] = r.params("s", "a", "b");
    const auto [out, i] = r.locals("out", "i");
    r.add(r.set(out, r.S("")));
    r.add(r.set(i, r.I(0)));
    r.add(r.iff(r.bin(BinOp::Eq, r.len(r.L(a)), r.I(0)), r.ret(r.L(s))));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(s))),
        r.blk({r.iff(
            r.both(r.bin(BinOp::Le, r.bin(BinOp::Add, r.L(i), r.len(r.L(a))),
                         r.len(r.L(s))),
                   r.bin(BinOp::Eq,
                         r.in(IntrinsicId::StrSlice,
                              {r.L(s), r.L(i),
                               r.bin(BinOp::Add, r.L(i), r.len(r.L(a)))}),
                         r.L(a))),
            r.blk({r.set(out, r.bin(BinOp::Add, r.L(out), r.L(b))),
                   r.set(i, r.bin(BinOp::Add, r.L(i), r.len(r.L(a))))}),
            r.blk({r.set(out, r.bin(BinOp::Add, r.L(out),
                                    r.in(IntrinsicId::StrSlice,
                                         {r.L(s), r.L(i),
                                          r.bin(BinOp::Add, r.L(i), r.I(1))}))),
                   r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))}))})));
    r.add(r.ret(r.L(out)));
    r.finish("$replace");
  }

  void rt_find() {
    RT r(*this);
    const auto [s, sub] = r.params("s", "sub");
    const auto [i] = r.locals("i");
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(
        r.bin(BinOp::Le, r.bin(BinOp::Add, r.L(i), r.len(r.L(sub))),
              r.len(r.L(s))),
        r.blk({r.iff(r.bin(BinOp::Eq,
                           r.in(IntrinsicId::StrSlice,
                                {r.L(s), r.L(i),
                                 r.bin(BinOp::Add, r.L(i), r.len(r.L(sub)))}),
                           r.L(sub)),
                     r.ret(r.L(i))),
               r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.I(-1)));
    r.finish("$find");
  }

  void rt_scount() {
    RT r(*this);
    const auto [s, sub] = r.params("s", "sub");
    const auto [i, n] = r.locals("i", "n");
    r.add(r.set(i, r.I(0)));
    r.add(r.set(n, r.I(0)));
    r.add(r.iff(r.bin(BinOp::Eq, r.len(r.L(sub)), r.I(0)), r.ret(r.I(0))));
    r.add(
        r.wh(r.bin(BinOp::Le, r.bin(BinOp::Add, r.L(i), r.len(r.L(sub))),
                   r.len(r.L(s))),
             r.blk({r.iff(
                 r.bin(BinOp::Eq,
                       r.in(IntrinsicId::StrSlice,
                            {r.L(s), r.L(i),
                             r.bin(BinOp::Add, r.L(i), r.len(r.L(sub)))}),
                       r.L(sub)),
                 r.blk({r.set(n, r.bin(BinOp::Add, r.L(n), r.I(1))),
                        r.set(i, r.bin(BinOp::Add, r.L(i), r.len(r.L(sub))))}),
                 r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1))))})));
    r.add(r.ret(r.L(n)));
    r.finish("$scount");
  }

  void rt_startswith() {
    RT r(*this);
    const auto [s, p] = r.params("s", "p");
    r.add(r.iff(r.bin(BinOp::Gt, r.len(r.L(p)), r.len(r.L(s))),
                r.ret(r.Bo(false))));
    r.add(r.ret(r.bin(
        BinOp::Eq, r.in(IntrinsicId::StrSlice, {r.L(s), r.I(0), r.len(r.L(p))}),
        r.L(p))));
    r.finish("$startswith");
  }

  void rt_endswith() {
    RT r(*this);
    const auto [s, p] = r.params("s", "p");
    r.add(r.iff(r.bin(BinOp::Gt, r.len(r.L(p)), r.len(r.L(s))),
                r.ret(r.Bo(false))));
    r.add(r.ret(
        r.bin(BinOp::Eq,
              r.in(IntrinsicId::StrSlice,
                   {r.L(s), r.bin(BinOp::Sub, r.len(r.L(s)), r.len(r.L(p))),
                    r.len(r.L(s))}),
              r.L(p))));
    r.finish("$endswith");
  }

  void rt_apop() {
    RT r(*this);
    const auto [a, i, hasi] = r.params("a", "i", "hasi");
    const auto [n, k, v] = r.locals("n", "k", "v");
    r.add(r.set(n, r.len(r.L(a))));
    r.add(
        r.iff(r.bin(BinOp::Eq, r.L(n), r.I(0)),
              r.call("$exc", {r.S("IndexError"), r.S("pop from empty list")})));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(hasi), r.Bo(false)),
                r.ret(r.in(IntrinsicId::ArrayPop, {r.L(a)}))));
    r.add(r.set(k, r.L(i)));
    r.add(r.iff(r.bin(BinOp::Lt, r.L(k), r.I(0)),
                r.set(k, r.bin(BinOp::Add, r.L(k), r.L(n)))));
    r.add(r.iff(
        r.either(r.bin(BinOp::Lt, r.L(k), r.I(0)),
                 r.bin(BinOp::Ge, r.L(k), r.L(n))),
        r.call("$exc", {r.S("IndexError"), r.S("pop index out of range")})));
    r.add(r.set(v, r.idx(r.L(a), r.L(k))));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(k), r.bin(BinOp::Sub, r.L(n), r.I(1))),
               r.blk({r.sidx(r.L(a), r.L(k),
                             r.idx(r.L(a), r.bin(BinOp::Add, r.L(k), r.I(1)))),
                      r.set(k, r.bin(BinOp::Add, r.L(k), r.I(1)))})));
    r.add(r.in(IntrinsicId::ArrayPop, {r.L(a)}));
    r.add(r.ret(r.L(v)));
    r.finish("$apop");
  }

  void rt_ainsert() {
    RT r(*this);
    const auto [a, i, v] = r.params("a", "i", "v");
    const auto [n, k, j] = r.locals("n", "k", "j");
    r.add(r.set(n, r.len(r.L(a))));
    r.add(r.set(k, r.L(i)));
    r.add(r.iff(r.bin(BinOp::Lt, r.L(k), r.I(0)),
                r.set(k, r.bin(BinOp::Add, r.L(k), r.L(n)))));
    r.add(r.iff(r.bin(BinOp::Lt, r.L(k), r.I(0)), r.set(k, r.I(0))));
    r.add(r.iff(r.bin(BinOp::Gt, r.L(k), r.L(n)), r.set(k, r.L(n))));
    r.add(r.push(r.L(a), r.L(v)));
    r.add(r.set(j, r.L(n)));
    r.add(r.wh(r.bin(BinOp::Gt, r.L(j), r.L(k)),
               r.blk({r.sidx(r.L(a), r.L(j),
                             r.idx(r.L(a), r.bin(BinOp::Sub, r.L(j), r.I(1)))),
                      r.set(j, r.bin(BinOp::Sub, r.L(j), r.I(1)))})));
    r.add(r.sidx(r.L(a), r.L(k), r.L(v)));
    r.add(r.ret(r.Nil()));
    r.finish("$ainsert");
  }

  void rt_aindex() {
    RT r(*this);
    const auto [a, v] = r.params("a", "v");
    const auto [i] = r.locals("i");
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(a))),
               r.blk({r.iff(r.call("$eq", {r.idx(r.L(a), r.L(i)), r.L(v)}),
                            r.ret(r.L(i))),
                      r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.call(
        "$exc", {r.S("ValueError"), r.bin(BinOp::Add, r.call("$repr", {r.L(v)}),
                                          r.S(" is not in list"))}));
    r.add(r.ret(r.Nil()));
    r.finish("$aindex");
  }

  void rt_aremove() {
    RT r(*this);
    const auto [a, v] = r.params("a", "v");
    r.add(r.call("$apop",
                 {r.L(a), r.call("$aindex", {r.L(a), r.L(v)}), r.Bo(true)}));
    r.add(r.ret(r.Nil()));
    r.finish("$aremove");
  }

  void rt_acount() {
    RT r(*this);
    const auto [a, v] = r.params("a", "v");
    const auto [i, n] = r.locals("i", "n");
    r.add(r.set(i, r.I(0)));
    r.add(r.set(n, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(a))),
               r.blk({r.iff(r.call("$eq", {r.idx(r.L(a), r.L(i)), r.L(v)}),
                            r.set(n, r.bin(BinOp::Add, r.L(n), r.I(1)))),
                      r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(n)));
    r.finish("$acount");
  }

  void rt_areverse() {
    RT r(*this);
    const auto [a] = r.params("a");
    const auto [i, j, t] = r.locals("i", "j", "t");
    r.add(r.set(i, r.I(0)));
    r.add(r.set(j, r.bin(BinOp::Sub, r.len(r.L(a)), r.I(1))));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(i), r.L(j)),
               r.blk({r.set(t, r.idx(r.L(a), r.L(i))),
                      r.sidx(r.L(a), r.L(i), r.idx(r.L(a), r.L(j))),
                      r.sidx(r.L(a), r.L(j), r.L(t)),
                      r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1))),
                      r.set(j, r.bin(BinOp::Sub, r.L(j), r.I(1)))})));
    r.add(r.ret(r.Nil()));
    r.finish("$areverse");
  }

  void rt_asort() {  // `xs.sort()` -- in place, so the result is copied back
    RT r(*this);
    const auto [a, key, rev] = r.params("a", "key", "rev");
    const auto [s, i] = r.locals("s", "i");
    r.add(r.set(s, r.call("$sorted", {r.L(a), r.L(key), r.L(rev)})));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(s))),
               r.blk({r.sidx(r.L(a), r.L(i), r.idx(r.L(s), r.L(i))),
                      r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.Nil()));
    r.finish("$asort");
  }

  void rt_delitem() {
    RT r(*this);
    const auto [c, k] = r.params("c", "k");
    r.add(r.iff(
        r.is(r.typ(r.L(c)), "map"),
        r.blk({r.iff(r.bin(BinOp::Eq, r.has(r.L(c), r.L(k)), r.Bo(false)),
                     r.call("$exc",
                            {r.S("KeyError"), r.call("$repr", {r.L(k)})})),
               r.in(IntrinsicId::ObjectRemove, {r.L(c), r.L(k)}),
               r.ret(r.Nil())})));
    r.add(r.iff(r.is(r.typ(r.L(c)), "array"),
                r.blk({r.call("$apop", {r.L(c), r.L(k), r.Bo(true)}),
                       r.ret(r.Nil())})));
    r.add(r.call("$exc", {r.S("TypeError"),
                          r.S("object does not support item deletion")}));
    r.add(r.ret(r.Nil()));
    r.finish("$delitem");
  }

  void rt_dpop() {
    RT r(*this);
    const auto [d, k, dflt, hasdflt] = r.params("d", "k", "dflt", "hasdflt");
    const auto [v] = r.locals("v");
    r.add(r.iff(r.has(r.L(d), r.L(k)),
                r.blk({r.set(v, r.idx(r.L(d), r.L(k))),
                       r.in(IntrinsicId::ObjectRemove, {r.L(d), r.L(k)}),
                       r.ret(r.L(v))})));
    r.add(r.iff(r.L(hasdflt), r.ret(r.L(dflt))));
    r.add(r.call("$exc", {r.S("KeyError"), r.call("$repr", {r.L(k)})}));
    r.add(r.ret(r.Nil()));
    r.finish("$dpop");
  }

  void rt_alldigits() {
    RT r(*this);
    const auto [s] = r.params("s");
    const auto [i, c] = r.locals("i", "c");
    r.add(r.iff(r.bin(BinOp::Eq, r.len(r.L(s)), r.I(0)), r.ret(r.Bo(false))));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(s))),
               r.blk({r.set(c, r.in(IntrinsicId::StrByte, {r.L(s), r.L(i)})),
                      r.iff(r.either(r.bin(BinOp::Lt, r.L(c), r.I(48)),
                                     r.bin(BinOp::Gt, r.L(c), r.I(57))),
                            r.ret(r.Bo(false))),
                      r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.Bo(true)));
    r.finish("$alldigits");
  }

  void rt_specerr() {
    RT r(*this);
    const auto [spec] = r.params("spec");
    r.add(r.call("$exc", {r.S("ValueError"),
                          r.bin(BinOp::Add, r.S("unsupported format spec: "),
                                r.L(spec))}));
    r.add(r.ret(r.Nil()));
    r.finish("$specerr");
  }

  // An f-string's format spec: a fill, an alignment, a width, and `.Nf`.
  // Everything else is refused rather than approximated, because `python3`
  // is the oracle and a near miss is a wrong answer.
  void rt_fmt() {
    RT r(*this);
    const auto [v, spec] = r.params("v", "spec");
    const auto [s, fill, align, i, rest, t, half, w, pad, dot, prec] =
        r.locals("s", "fill", "align", "i", "rest", "t", "half", "w", "pad",
                 "dot", "prec");
    const auto ch = [&](NodeId str, NodeId i) {
      return r.in(IntrinsicId::StrSlice,
                  {str, i, r.bin(BinOp::Add, i, r.I(1))});
    };
    const auto is_align = [&](NodeId c) {
      return r.either(r.bin(BinOp::Eq, c, r.S("<")),
                      r.either(r.bin(BinOp::Eq, c, r.S(">")),
                               r.bin(BinOp::Eq, c, r.S("^"))));
    };
    r.add(r.iff(r.bin(BinOp::Eq, r.len(r.L(spec)), r.I(0)),
                r.ret(r.call("$str", {r.L(v)}))));
    // A number right-aligns by default and everything else left-aligns,
    // which is Python's rule and not a choice this front end gets to make.
    r.add(r.set(fill, r.S(" ")));
    r.add(r.set(align, r.iff(r.either(r.is(r.typ(r.L(v)), "int"),
                                      r.either(r.is(r.typ(r.L(v)), "double"),
                                               r.call("$isbig", {r.L(v)}))),
                             r.S(">"), r.S("<"))));
    r.add(r.set(i, r.I(0)));
    r.add(r.iff(
        r.both(r.bin(BinOp::Ge, r.len(r.L(spec)), r.I(2)),
               is_align(ch(r.L(spec), r.I(1)))),
        r.blk({r.set(fill, ch(r.L(spec), r.I(0))),
               r.set(align, ch(r.L(spec), r.I(1))), r.set(i, r.I(2))}),
        r.iff(is_align(ch(r.L(spec), r.I(0))),
              r.blk({r.set(align, ch(r.L(spec), r.I(0))), r.set(i, r.I(1))}))));
    r.add(r.set(rest, r.in(IntrinsicId::StrSlice,
                           {r.L(spec), r.L(i), r.len(r.L(spec))})));
    r.add(r.set(prec, r.I(-1)));
    r.add(r.iff(
        r.both(r.bin(BinOp::Gt, r.len(r.L(rest)), r.I(0)),
               r.bin(BinOp::Eq,
                     ch(r.L(rest), r.bin(BinOp::Sub, r.len(r.L(rest)), r.I(1))),
                     r.S("f"))),
        r.blk({r.set(rest, r.in(IntrinsicId::StrSlice,
                                {r.L(rest), r.I(0),
                                 r.bin(BinOp::Sub, r.len(r.L(rest)), r.I(1))})),
               r.set(dot, r.call("$find", {r.L(rest), r.S(".")})),
               r.iff(r.bin(BinOp::Lt, r.L(dot), r.I(0)),
                     r.call("$specerr", {r.L(spec)})),
               r.set(t, r.in(IntrinsicId::StrSlice,
                             {r.L(rest), r.bin(BinOp::Add, r.L(dot), r.I(1)),
                              r.len(r.L(rest))})),
               r.iff(r.bin(BinOp::Eq, r.call("$alldigits", {r.L(t)}),
                           r.Bo(false)),
                     r.call("$specerr", {r.L(spec)})),
               r.set(prec, r.call("$toint", {r.L(t)})),
               r.set(rest, r.in(IntrinsicId::StrSlice,
                                {r.L(rest), r.I(0), r.L(dot)}))})));
    r.add(r.iff(r.both(r.bin(BinOp::Gt, r.len(r.L(rest)), r.I(0)),
                       r.bin(BinOp::Eq, r.call("$alldigits", {r.L(rest)}),
                             r.Bo(false))),
                r.call("$specerr", {r.L(spec)})));
    r.add(r.set(w, r.iff(r.bin(BinOp::Eq, r.len(r.L(rest)), r.I(0)), r.I(0),
                         r.call("$toint", {r.L(rest)}))));
    // A precision is the C library's exact decimal expansion, which is
    // what CPython's is too -- and the one thing here that is not a scan.
    r.add(
        r.set(s, r.iff(r.bin(BinOp::Ge, r.L(prec), r.I(0)),
                       r.nat("ffmt", {r.call("$tofloat", {r.L(v)}), r.L(prec)}),
                       r.call("$str", {r.L(v)}))));
    r.add(r.set(pad, r.bin(BinOp::Sub, r.L(w), r.len(r.L(s)))));
    r.add(r.iff(r.bin(BinOp::Le, r.L(pad), r.I(0)), r.ret(r.L(s))));
    r.add(
        r.iff(r.bin(BinOp::Eq, r.L(align), r.S(">")),
              r.ret(r.bin(BinOp::Add, r.call("$strmul", {r.L(fill), r.L(pad)}),
                          r.L(s)))));
    r.add(r.iff(
        r.bin(BinOp::Eq, r.L(align), r.S("^")),
        r.blk({r.set(t, r.call("$str", {r.bin(BinOp::Div, r.L(pad), r.I(2))})),
               r.set(half, r.bin(BinOp::Div, r.L(pad), r.I(2))),
               r.ret(r.bin(
                   BinOp::Add,
                   r.bin(BinOp::Add, r.call("$strmul", {r.L(fill), r.L(half)}),
                         r.L(s)),
                   r.call("$strmul", {r.L(fill), r.bin(BinOp::Sub, r.L(pad),
                                                       r.L(half))})))})));
    r.add(r.ret(
        r.bin(BinOp::Add, r.L(s), r.call("$strmul", {r.L(fill), r.L(pad)}))));
    r.finish("$fmt");
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
    auto b = Builder(m).at(p);
    if (es == nullptr) return b.nil_literal();
    if (es->nodes.size() == 1) return emit_expr(*es->nodes[0], ctx);
    std::vector<NodeId> items;
    for (const auto& c : es->nodes) items.push_back(emit_expr(*c, ctx));
    return helper("$tuple", {b.array_lit(items)}, p);
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
    auto b = Builder(m).at(p);
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
      A = b.array_lit(items);
    } else {
      // A `*xs` decides a count at run time, so the array is built rather
      // than written -- which is the whole reason this convention exists.
      const int32_t t = ctx.alloc_local("$args");
      const NodeId T = b.varref(VarKind::Local, t);
      std::vector<NodeId> steps{
          b.assign(VarKind::Local, t, b.array_lit(front))};
      for (const Ast* e : pos) {
        steps.push_back(
            e->tag == "splat"_
                ? helper("$aext", {T, emit_expr(*e->nodes[0], ctx)}, p)
                : b.intrinsic(IntrinsicId::ArrayPush, {T, emit_expr(*e, ctx)}));
      }
      steps.push_back(T);
      A = b.block(steps);
    }
    NodeId K;
    if (kw.empty()) {
      K = b.nil_literal();
    } else if (!kwsplat) {
      std::vector<std::pair<NodeId, NodeId>> kvs;
      for (const Ast* e : kw) {
        kvs.emplace_back(b.str_literal(std::string(e->nodes[0]->token)),
                         emit_expr(*e->nodes[1], ctx));
      }
      K = b.object_lit(kvs);
    } else {
      const int32_t t = ctx.alloc_local("$kw");
      const NodeId T = b.varref(VarKind::Local, t);
      std::vector<NodeId> steps{b.assign(VarKind::Local, t, b.object_lit({}))};
      for (const Ast* e : kw) {
        steps.push_back(
            e->tag == "kwsplat"_
                ? helper("$kwmerge", {T, emit_expr(*e->nodes[0], ctx)}, p)
                : b.set_index(T, b.str_literal(std::string(e->nodes[0]->token)),
                              emit_expr(*e->nodes[1], ctx)));
      }
      steps.push_back(T);
      K = b.block(steps);
    }
    return {A, K};
  }

  NodeId emit_pycall(NodeId callee, const Ast* args, FnCtx& ctx,
                     const std::vector<NodeId>& front, SrcPos p) {
    auto b = Builder(m).at(p);
    const auto [A, K] = emit_callargs(args, ctx, front, p);
    return b.call_value(callee, {A, K});
  }

  // The def-time half of a default: computed where the `def` stands, into a
  // cell made fresh right there, so two closures built in a loop do not
  // share one box.
  void emit_defaults(int32_t g, FnCtx& ctx, std::vector<NodeId>& out,
                     SrcPos p) {
    auto b = Builder(m).at(p);
    for (const ParamInfo& pi : fns[static_cast<size_t>(g)].params) {
      if (pi.kind != ParamInfo::Default) continue;
      const auto [k, i] = rs.access(ctx.fn, pi.def_var);
      out.push_back(b.cell_fresh(i));
      out.push_back(b.assign(k, i, emit_expr(*pi.def, ctx)));
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
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    switch (a.tag) {
      case "passstmt"_:
        return b.block({});
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
                                 {b.array_lit({value}), b.nil_literal()});
          }
        }
        out.push_back(write_var(decl_of.at(fn_ident(a)), value, ctx, p));
        return b.block(out);
      }
      case "classdef"_:
        return emit_class(a, ctx);
      case "breakstmt"_:
        return b.make_break();
      case "contstmt"_:
        return b.make_continue();
      case "returnstmt"_:
        return b.make_return(
            emit_exprs(a.nodes.empty() ? nullptr : a.nodes[0].get(), ctx, p));
      case "raisestmt"_:
        return b.make_throw(emit_expr(*a.nodes[0], ctx));
      case "yieldone"_:
        return b.make_yield(emit_exprs(a.nodes[0].get(), ctx, p));
      case "yieldfrom"_: {
        // `yield from it` is the loop it stands for. What the sub-generator
        // *returns* is dropped -- see README.md.
        const int32_t it = ctx.alloc_local("$it");
        const int32_t st = ctx.alloc_local("$step");
        const NodeId I = b.varref(VarKind::Local, it);
        const NodeId S = b.varref(VarKind::Local, st);
        return b.block(
            {b.assign(VarKind::Local, it,
                      helper("$iter", {emit_expr(*a.nodes[0], ctx)}, p)),
             b.make_while(
                 b.bool_literal(true),
                 b.block(
                     {b.assign(VarKind::Local, st, helper("$iternext", {I}, p)),
                      b.make_if(b.index(S, b.str_literal("done")),
                                b.make_break(), NodeId{}),
                      b.make_yield(b.index(S, b.str_literal("value")))}))});
      }
      case "globalstmt"_:
      case "nonlocalstmt"_:
        return b.block({});
      case "delstmt"_: {
        std::vector<NodeId> out;
        for (const auto& c : a.nodes) out.push_back(emit_del(*c, ctx, p));
        return b.block(out);
      }
      case "assertstmt"_:
        return b.make_if(
            b.binary(BinOp::Eq,
                     helper("$truthy", {emit_expr(*a.nodes[0], ctx)}, p),
                     b.bool_literal(false)),
            helper("$exc",
                   {b.str_literal("AssertionError"),
                    a.nodes.size() > 1
                        ? helper("$str", {emit_expr(*a.nodes[1], ctx)}, p)
                        : b.str_literal("")},
                   p),
            NodeId{});
      case "exprstmt"_:
        return emit_expr(*a.nodes[0], ctx);
      case "ifstmt"_:
        return emit_if(a, ctx);
      case "whilestmt"_:
        return b.make_while(helper("$truthy", {emit_expr(*a.nodes[0], ctx)}, p),
                            emit_block(*a.nodes[1], ctx));
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
    return helper("$delitem",
                  {emit_postfix(t, t.nodes.size() - 1, ctx),
                   emit_expr(sub, ctx)},
                  p);
  }

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
      if (c.tag == "elifpart"_) {
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

  NodeId emit_for(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const int32_t it = ctx.alloc_local("$it");
    const int32_t st = ctx.alloc_local("$step");
    const NodeId I = b.varref(VarKind::Local, it);
    const NodeId S = b.varref(VarKind::Local, st);
    const Ast& tg = *a.nodes[0];
    std::vector<NodeId> loop{
        b.assign(VarKind::Local, st, helper("$iternext", {I}, p)),
        b.make_if(b.index(S, b.str_literal("done")), b.make_break(), NodeId{})};
    const NodeId value = b.index(S, b.str_literal("value"));
    if (tg.nodes.size() == 1) {
      loop.push_back(write_var(decl_of.at(tg.nodes[0].get()), value, ctx, p));
    } else {
      // `for k, v in d.items()`: the same unpack an assignment does.
      const int32_t u = ctx.alloc_local("$unp");
      const NodeId U = b.varref(VarKind::Local, u);
      loop.push_back(b.assign(
          VarKind::Local, u,
          helper("$unpack",
                 {value, b.literal(static_cast<int64_t>(tg.nodes.size()))},
                 p)));
      for (size_t k = 0; k < tg.nodes.size(); ++k) {
        loop.push_back(write_var(decl_of.at(tg.nodes[k].get()),
                                 b.index(U, b.literal(static_cast<int64_t>(k))),
                                 ctx, p));
      }
    }
    loop.push_back(emit_block(*a.nodes[2], ctx));
    return b.block({b.assign(VarKind::Local, it,
                             helper("$iter", {emit_expr(*a.nodes[1], ctx)}, p)),
                    b.make_while(b.bool_literal(true), b.block(loop))});
  }

  // try/except/finally. `finally` is a Defer inside the Scope wrapping the
  // try, so it runs however the block is left; `except NAME` re-raises
  // what it does not match, which is how a subset with no exception
  // hierarchy gets the selective behaviour right.
  NodeId emit_try(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
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
    const NodeId E = b.varref(VarKind::Local, slot);
    std::vector<NodeId> out;
    if (fin != nullptr) {
      out.push_back(b.make_defer(emit_closure(fn_of.at(fin), ctx, p)));
    }
    const NodeId body = emit_block(*a.nodes[0], ctx);
    // The clauses are tried in order and what none of them claims is
    // re-thrown: one nested If per clause, and nothing else.
    NodeId handler = b.make_throw(E);
    for (size_t k = excs.size(); k-- > 0;) {
      const Ast& ex = *excs[k];
      const bool bare = ex.nodes[0]->tag == "block"_;
      std::vector<NodeId> hs;
      if (!bare && ex.nodes.size() > 2) {
        hs.push_back(write_var(decl_of.at(ex.nodes[1].get()), E, ctx, p));
      }
      hs.push_back(emit_block(*ex.nodes.back(), ctx));
      if (bare) {
        handler = b.block(hs);
        continue;
      }
      // A builtin exception is not a class here, so it travels as its
      // name; a class of the program's own travels as its value.
      const Ast& caught = *ex.nodes[0];
      const NodeId cls = ref_of.count(&caught)
                             ? read_var(ref_of.at(&caught), ctx, p)
                             : b.str_literal(std::string(caught.token));
      handler = b.make_if(helper("$isexc", {E, cls}, p), b.block(hs), handler);
    }
    out.push_back(b.make_try(slot, body, handler));
    const int32_t n = ctx.mark();
    return b.scope(n, n, b.block(out));
  }

  // `with cm as x:` -- the context manager goes into a cell so the exit
  // thunk can capture it, `__enter__` runs, and `__exit__` is a Defer:
  // "however it exits -- falling through, Break, Continue, Return, or an
  // unwinding throw", which is exactly what a context manager promises.
  NodeId emit_with(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const int32_t cell = ctx.next_cell++;
    const NodeId C = b.varref(VarKind::Cell, cell);
    std::vector<NodeId> out{
        b.cell_fresh(cell),
        b.assign(VarKind::Cell, cell, emit_expr(*a.nodes[0], ctx))};
    const NodeId entered =
        b.call_value(helper("$getattr", {C, b.str_literal("__enter__")}, p),
                     {b.array_lit({C}), b.nil_literal()});
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
    out.push_back(
        b.make_defer(b.make_closure(rs.fns[static_cast<size_t>(g)].index, cm)));
    out.push_back(emit_block(*a.nodes.back(), ctx));
    const int32_t n = ctx.mark();
    return b.scope(n, n, b.block(out));
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
    m.funcs[static_cast<size_t>(rs.fns[static_cast<size_t>(g)].index)] =
        std::move(f);
  }

  NodeId emit_assign(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const std::string op(a.nodes[1]->token);
    const Ast& targets = *a.nodes[0];
    const Ast& values = *a.nodes[2];
    // `a, b = b, a + b`: every value is computed into a slot before any of
    // them is stored, which is what makes the swap a swap.
    // `a, b = expr`: one value, taken apart.
    if (targets.nodes.size() > 1 && values.nodes.size() == 1) {
      if (op != "=") fail(a, "cannot augment a multiple assignment");
      const int32_t u = ctx.alloc_local("$unp");
      const NodeId U = b.varref(VarKind::Local, u);
      std::vector<NodeId> out{b.assign(
          VarKind::Local, u,
          helper("$unpack",
                 {emit_expr(*values.nodes[0], ctx),
                  b.literal(static_cast<int64_t>(targets.nodes.size()))},
                 p))};
      for (size_t i = 0; i < targets.nodes.size(); ++i) {
        out.push_back(emit_store(*targets.nodes[i],
                                 b.index(U, b.literal(static_cast<int64_t>(i))),
                                 ctx, p));
      }
      return b.block(out);
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
        out.push_back(b.assign(VarKind::Local, t, emit_expr(*v, ctx)));
      }
      for (size_t i = 0; i < targets.nodes.size(); ++i) {
        out.push_back(emit_store(*targets.nodes[i],
                                 b.varref(VarKind::Local, temps[i]), ctx, p));
      }
      return b.block(out);
    }
    const Ast& target = *targets.nodes[0];
    const auto combine = [&](NodeId cur) -> NodeId {
      const NodeId v = emit_exprs(&values, ctx, p);
      if (op == "=") return v;
      if (op == "+=") return helper("$add", {cur, v}, p);
      if (op == "-=") return helper("$sub", {cur, v}, p);
      if (op == "*=") return helper("$mul", {cur, v}, p);
      if (op == "//=") return helper("$idiv", {cur, v}, p);
      if (op == "/=") return helper("$fdiv", {cur, v}, p);
      return helper("$mod", {cur, v}, p);
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
    const NodeId key = attr ? b.str_literal(std::string(last.nodes[0]->token))
                            : emit_expr(*last.nodes[0], ctx);
    const int32_t tr = ctx.alloc_local("$recv");
    const int32_t tk = ctx.alloc_local("$key");
    const NodeId recv = emit_postfix(target, target.nodes.size() - 1, ctx);
    const NodeId R = b.varref(VarKind::Local, tr);
    const NodeId K = b.varref(VarKind::Local, tk);
    const NodeId cur = op == "="
                           ? b.nil_literal()
                           : helper(attr ? "$getattr" : "$idx", {R, K}, p);
    return b.block(
        {b.assign(VarKind::Local, tr, recv), b.assign(VarKind::Local, tk, key),
         helper(attr ? "$setattr" : "$setidx", {R, K, combine(cur)}, p)});
  }

  NodeId emit_store(const Ast& target, NodeId value, FnCtx& ctx, SrcPos p) {
    auto b = Builder(m).at(p);
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
    const NodeId key = attr ? b.str_literal(std::string(last.nodes[0]->token))
                            : emit_expr(*last.nodes[0], ctx);
    return helper(attr ? "$setattr" : "$setidx",
                  {emit_postfix(target, target.nodes.size() - 1, ctx), key,
                   value},
                  p);
  }

  // class C: -- a method table in a cell, and a constructor closure over
  // it, exactly as examples/mini-culebra does. Python's methods declare
  // `self` themselves, so there is no implicit parameter here.
  NodeId emit_class(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
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
        {b.str_literal(kNameKey), b.str_literal(cname)}};
    if (ci.base_var >= 0) {
      kvs.emplace_back(b.str_literal(kBaseKey), read_var(ci.base_var, ctx, p));
    }
    if (!ci.root.empty()) {
      kvs.emplace_back(b.str_literal(kRootKey), b.str_literal(ci.root));
    }
    for (const auto& [name, g] : methods) {
      kvs.emplace_back(b.str_literal("\x02" + name), emit_closure(g, ctx, p));
    }
    // The table lives in a cell -- the constructor captures it, and so does
    // any method that says `super()`.
    const auto [ck, cell] = rs.access(ctx.fn, ci.table_var);
    (void)ck;
    const NodeId T = b.varref(VarKind::Cell, cell);
    const int32_t ctor = new_fn(ctx.fn, cname);
    rs.fns[static_cast<size_t>(ctor)].index = static_cast<int32_t>(m.funcs.size());
    m.funcs.push_back({});
    emit_ctor(ctor, cname, ci.is_exc);
    std::vector<CaptureSrc> cs{{VarKind::Cell, cell}};
    const int32_t cm = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back(cs);
    out.push_back(b.cell_fresh(cell));
    out.push_back(b.assign(VarKind::Cell, cell, b.object_lit(kvs)));
    // The class value is its constructor closure, and the table keeps a
    // reference to it: that is the identity `isinstance` and `except`
    // compare, since there is nothing else a class could be.
    const int32_t t = ctx.alloc_local("$class");
    const NodeId K = b.varref(VarKind::Local, t);
    out.push_back(
        b.assign(VarKind::Local, t,
                 b.make_closure(rs.fns[static_cast<size_t>(ctor)].index, cm)));
    out.push_back(b.set_index(T, b.str_literal(kIdKey), K));
    out.push_back(write_var(v, K, ctx, p));
    return b.block(out);
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
    m.funcs[static_cast<size_t>(rs.fns[static_cast<size_t>(g)].index)] =
        std::move(f);
  }

  // -- Expressions --------------------------------------------------------
  NodeId emit_expr(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
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
            std::strtod(std::string(a.token).c_str(), nullptr));
      case "string"_:
        return b.str_literal(unescape(std::string(a.token)));
      case "literal"_: {
        const std::string t(a.token);
        if (t == "True") return b.bool_literal(true);
        if (t == "False") return b.bool_literal(false);
        return b.nil_literal();
      }
      case "ident"_: {
        const auto it = ref_of.find(&a);
        if (it != ref_of.end()) return read_var(it->second, ctx, p);
        // A builtin in value position -- `sorted(xs, key=len)` -- is not a
        // call site, so there is nothing to emit inline. It gets a function
        // of its own instead, made once and shared.
        const std::string g(a.token);
        if (is_value_builtin(g)) {
          return b.make_closure(builtin_func(g), empty_cmap);
        }
        fail(a, "'" + g + "' must be called here");
      }
      case "fstring"_: {
        // One concatenation per piece. `$fmt` is what applies a spec, and
        // it is called even for an empty one so that a number and a string
        // reach `$str` the same way.
        NodeId acc = b.str_literal("");
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
            if (as_repr) v = helper("$repr", {v}, p);
            piece = helper("$fmt", {v, b.str_literal(spec)}, p);
          } else {
            piece = b.str_literal(unescape_ftext(std::string(c->token)));
          }
          acc = b.binary(BinOp::Add, acc, piece);
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
                            {b.array_lit({}), b.nil_literal()});
      case "tuplelit"_: {
        std::vector<NodeId> items;
        for (const auto& c : a.nodes) items.push_back(emit_expr(*c, ctx));
        return helper("$tuple", {b.array_lit(items)}, p);
      }
      case "lambda"_: {
        const int32_t g = fn_of.at(&a);
        if (!has_defaults(g)) return emit_closure(g, ctx, p);
        std::vector<NodeId> out;
        emit_defaults(g, ctx, out, p);
        out.push_back(emit_closure(g, ctx, p));
        return b.block(out);
      }
      case "negexp"_:
        return helper("$neg", {emit_expr(*a.nodes[0], ctx)}, p);
      case "notop"_:
        return b.binary(BinOp::Eq,
                        helper("$truthy", {emit_expr(*a.nodes[0], ctx)}, p),
                        b.bool_literal(false));
      case "powexp"_:
        return helper("$pow",
                      {emit_expr(*a.nodes[0], ctx),
                       emit_expr(*a.nodes[1]->nodes[0], ctx)},
                      p);
      case "ternary"_:
        // `a if c else b`: the condition is the middle child.
        return b.make_if(helper("$truthy", {emit_expr(*a.nodes[1], ctx)}, p),
                         emit_expr(*a.nodes[0], ctx),
                         emit_expr(*a.nodes[2], ctx));
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
              b.assign(VarKind::Local, t, emit_expr(*a.nodes[k], ctx)));
        }
        const auto link = [&](size_t k) {
          const Ast& op = *a.nodes[k * 2 + 1];
          std::string t(op.token);
          if (t.rfind("not", 0) == 0) t = "not in";
          if (t.rfind("is", 0) == 0) {
            t = t.find("not") != std::string::npos ? "is not" : "is";
          }
          const SrcPos op_p = pos_of(op);
          const NodeId lhs = b.varref(VarKind::Local, slots[k]);
          const NodeId rhs = b.varref(VarKind::Local, slots[k + 1]);
          if (t == "==") return helper("$eq", {lhs, rhs}, op_p);
          if (t == "!=") {
            return b.at(op_p).binary(BinOp::Eq, helper("$eq", {lhs, rhs}, op_p),
                                     b.bool_literal(false));
          }
          // `is` is identity, which `Same` answers.
          if (t == "is" || t == "is not") {
            const NodeId same =
                b.at(op_p).intrinsic(IntrinsicId::Same, {lhs, rhs});
            return t == "is" ? same
                             : b.at(op_p).binary(BinOp::Eq, same,
                                                 b.bool_literal(false));
          }
          if (t == "in") return helper("$in", {lhs, rhs}, op_p);
          if (t == "not in") {
            return b.at(op_p).binary(BinOp::Eq, helper("$in", {lhs, rhs}, op_p),
                                     b.bool_literal(false));
          }
          const BinOp o = t == "<"    ? BinOp::Lt
                          : t == "<=" ? BinOp::Le
                          : t == ">"  ? BinOp::Gt
                                      : BinOp::Ge;
          return b.at(op_p).binary(o, helper("$cmp", {lhs, rhs}, op_p),
                                   b.literal(0));
        };
        NodeId chain = link(links - 1);
        for (size_t k = links - 1; k-- > 0;) {
          chain = b.make_if(link(k), chain, b.bool_literal(false));
        }
        pre.push_back(chain);
        return b.block(pre);
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
          acc = helper(h, {acc, rhs}, op_p);
        }
        return acc;
      }
      case "listlit"_: {
        std::vector<NodeId> items;
        for (const auto& c : a.nodes) items.push_back(emit_expr(*c, ctx));
        return b.array_lit(items);
      }
      case "dictlit"_: {
        const int32_t t = ctx.alloc_local("$dict");
        const NodeId T = b.varref(VarKind::Local, t);
        std::vector<NodeId> out{
            b.assign(VarKind::Local, t, b.intrinsic(IntrinsicId::MapNew, {}))};
        for (const auto& c : a.nodes) {
          out.push_back(b.set_index(T, emit_expr(*c->nodes[0], ctx),
                                    emit_expr(*c->nodes[1], ctx)));
        }
        out.push_back(T);
        return b.block(out);
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
    auto b = Builder(m).at(p);
    errno = 0;
    char* end = nullptr;
    const long long v = std::strtoll(digits.c_str(), &end, 10);
    if (errno == 0 && end == digits.c_str() + digits.size()) {
      return b.literal(static_cast<int64_t>(v));
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
      limbs.push_back(b.literal(rem));
      rest = q.empty() ? "0" : q;
    }
    return b.object_lit({{b.str_literal(kBigKey), b.array_lit(limbs)},
                         {b.str_literal(kSignKey), b.literal(1)}});
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
          "$supercall",
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
          cur = helper("$getattr", {cur, b.str_literal(name, p)}, p);
          break;
        }
        case "indexsfx"_: {
          const Ast& sub = *sfx.nodes[0];
          // Which side of the ':' an expression fell on is a *rule*, not
          // a position: `slicelo` and `slicehi` are separate rules for
          // exactly that reason, since one child says nothing on its own.
          if (sub.tag == "sliceboth"_) {
            cur = helper("$slice",
                         {cur, emit_expr(*sub.nodes[0], ctx),
                          emit_expr(*sub.nodes[1], ctx)},
                         p);
            break;
          }
          if (sub.tag == "slicelo"_) {
            cur = helper("$slice",
                         {cur, emit_expr(*sub.nodes[0], ctx),
                          b.nil_literal(p)},
                         p);
            break;
          }
          if (sub.tag == "slicehi"_) {
            cur = helper("$slice",
                         {cur, b.nil_literal(p),
                          emit_expr(*sub.nodes[0], ctx)},
                         p);
            break;
          }
          if (sub.tag == "sliceall"_) {
            cur = helper("$slice",
                         {cur, b.nil_literal(p), b.nil_literal(p)}, p);
            break;
          }
          cur = helper("$idx", {cur, emit_expr(sub, ctx)}, p);
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
    auto b = Builder(m).at(p);
    const Ast* argnode = call.nodes.empty() ? nullptr : call.nodes[0].get();
    // The receiver lands in a slot first, because both branches below
    // read it and one of them reads it twice.
    const int32_t t = ctx.alloc_local("$self");
    const NodeId T = b.varref(VarKind::Local, t);
    // A method declares `self` itself, so the receiver goes in front.
    if (!is_method_name(name)) {
      return b.block(
          {b.assign(VarKind::Local, t, recv),
           emit_pycall(helper("$getattr", {T, b.str_literal(name)}, p), argnode,
                       ctx, {T}, p)});
    }
    // `sort` takes key= and reverse=, exactly as `sorted` does.
    NodeId sortkey = b.nil_literal();
    NodeId sortrev = b.bool_literal(false);
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
      return k < args.size() ? args[k] : b.nil_literal();
    };
    const auto got = [&](size_t k) { return b.bool_literal(k < args.size()); };
    std::vector<NodeId> call_args{T};
    call_args.insert(call_args.end(), args.begin(), args.end());
    const NodeId user =
        b.call_value(helper("$getattr", {T, b.str_literal(name)}, p),
                     {b.array_lit(call_args), b.nil_literal()});

    std::vector<std::pair<const char*, NodeId>> cands;
    const auto add = [&](const char* want, NodeId impl) {
      cands.emplace_back(want, impl);
    };
    const auto h = [&](const char* n, std::vector<NodeId> as) {
      return helper(n, as, p);
    };
    if (name == "append") {
      add("array", b.intrinsic(IntrinsicId::ArrayPush, {T, a0(0)}));
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
      add("map", b.intrinsic(IntrinsicId::ObjectKeys, {T}));
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
      add("string", h("$strip", {T, b.literal(0)}));
    } else if (name == "lstrip") {
      add("string", h("$strip", {T, b.literal(1)}));
    } else if (name == "rstrip") {
      add("string", h("$strip", {T, b.literal(2)}));
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
      cur = b.make_if(b.binary(BinOp::Eq, b.intrinsic(IntrinsicId::TypeOf, {T}),
                               b.str_literal(cands[k].first)),
                      cands[k].second, cur);
    }
    return b.block({b.assign(VarKind::Local, t, recv), cur});
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
      return helper("$sorted", {seq, key, rev}, p);
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
      return helper("$isinst", {emit_expr(what, ctx), cv}, p);
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
      for (const NodeId v : args) parts.push_back(helper("$str", {v}, p));
      return native("print", {b.array_lit(parts, p)}, p);
    }
    if (g == "len") return helper("$len", {a0(0)}, p);
    if (g == "str") return helper("$str", {a0(0)}, p);
    if (g == "int") return helper("$toint", {a0(0)}, p);
    if (g == "float") return helper("$tofloat", {a0(0)}, p);
    if (g == "list") return helper("$tolist", {a0(0)}, p);
    if (g == "bool") return helper("$truthy", {a0(0)}, p);
    if (g == "abs") {
      return b.make_if(
          b.binary(BinOp::Lt, helper("$cmp", {a0(0), b.literal(0, p)}, p),
                   b.literal(0, p), p),
          helper("$neg", {a0(0)}, p), a0(0), p);
    }
    if (g == "type") return helper("$type", {a0(0)}, p);
    if (g == "repr") return helper("$repr", {a0(0)}, p);
    if (g == "next") {
      return helper("$next",
                    {a0(0), a0(1), b.bool_literal(args.size() > 1, p)}, p);
    }
    if (g == "tuple") {
      return helper("$tuple", {helper("$tolist", {a0(0)}, p)}, p);
    }
    if (g == "enumerate") {
      return helper("$enumerate",
                    {a0(0), args.size() > 1 ? args[1] : b.literal(0, p)}, p);
    }
    if (g == "zip") {
      if (args.size() != 2) fail(prim, "zip() takes two iterables here");
      return helper("$zip", {args[0], args[1]}, p);
    }
    if (g == "sum") {
      return helper("$sum",
                    {a0(0), args.size() > 1 ? args[1] : b.literal(0, p)}, p);
    }
    if (g == "min" || g == "max") {
      const NodeId ismax = b.bool_literal(g == "max", p);
      // `min(xs)` scans one iterable; `min(a, b)` scans its own arguments.
      return helper("$minmax",
                    {args.size() == 1 ? args[0] : b.array_lit(args, p), ismax},
                    p);
    }
    if (g == "range") {
      if (args.size() == 1) {
        return helper("$range",
                      {b.literal(0, p), args[0], b.literal(1, p)}, p);
      }
      return helper("$range",
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
    // Same reason, and the same growth: emit_class appends to rs.fns too.
    const Resolver::Fn rf = rs.fns[static_cast<size_t>(f)];
    if (fi.is_synth) return;  // a `with`'s exit thunk, built by emit_with
    FnCtx ctx;
    ctx.fn = f;
    ctx.next_cell = rs.num_cells(f);
    const SrcPos p = fi.body != nullptr ? pos_of(*fi.body) : SrcPos{0, 0};
    auto b = Builder(m).at(p);

    std::vector<NodeId> pre;
    // Every cell this function owns is made once, at entry. Python binds
    // per function rather than per block, and its closures are *late*
    // binding -- a lambda made in a loop sees the loop variable's final
    // value -- so a CellFresh per iteration would be wrong here in a way
    // it is right in examples/mini-js.
    for (const auto& [v, c] : rf.cell_index) {
      (void)v;
      pre.push_back(b.cell_fresh(c));
    }
    // Before the prologue below, which calls helpers itself.
    // The two slots every Python function is called with, and the prologue
    // that turns them back into the parameters the source declared.
    const int32_t sa = ctx.alloc_local("$a");
    const int32_t sk = ctx.alloc_local("$k");
    emit_prologue(fi, rf, ctx, pre, sa, sk, p);
    // Every other binding of this function gets a slot up front too, since
    // its scope is the whole body whatever block it was assigned in.
    for (size_t v = 0; v < rs.vars.size(); ++v) {
      if (rs.vars[v].owner != f) continue;
      if (rs.vars[v].slot >= 0) continue;
      if (rf.cell_index.count(static_cast<int32_t>(v))) continue;
      rs.vars[v].slot = ctx.alloc_local(rs.vars[v].name);
    }

    NodeId body;
    if (fi.body->tag == "program"_ || fi.body->tag == "block"_) {
      body = emit_block(*fi.body, ctx);
    } else if (fi.body->tag == "listcomp"_ || fi.body->tag == "dictcomp"_ ||
               fi.body->tag == "gencomp"_ || fi.body->tag == "bargen"_) {
      body = emit_comp(*fi.body, ctx, p);
    } else {
      body = b.make_return(emit_expr(*fi.body, ctx));  // a lambda
    }

    std::vector<NodeId> stmts;
    stmts.insert(stmts.end(), pre.begin(), pre.end());
    stmts.push_back(body);
    stmts.push_back(b.make_return(b.nil_literal()));

    Func fn;
    fn.name = fi.name;
    fn.num_params = 2;  // the convention, not what the source declared
    fn.num_locals = ctx.high_local;
    fn.local_names = ctx.names();
    fn.num_cells = ctx.next_cell;
    fn.lenient_arity = true;
    fn.is_generator = fi.is_generator;
    fn.num_captures = m.funcs[static_cast<size_t>(rf.index)].num_captures;
    fn.capture_names = m.funcs[static_cast<size_t>(rf.index)].capture_names;
    fn.body = b.scope(0, ctx.high_local, b.block(stmts));
    m.funcs[static_cast<size_t>(rf.index)] = std::move(fn);
  }

  // Unpack the convention into the declared parameters, applying defaults,
  // matching keywords by name, and collecting what `*rest`/`**kw` asked for.
  void emit_prologue(const FnInfo& fi, const Resolver::Fn& rf, FnCtx& ctx,
                     std::vector<NodeId>& pre, int32_t sa, int32_t sk,
                     SrcPos p) {
    auto b = Builder(m).at(p);
    const NodeId A = b.varref(VarKind::Local, sa);
    const NodeId K = b.varref(VarKind::Local, sk);
    const auto alen = [&] { return b.intrinsic(IntrinsicId::Len, {A}); };
    // The entry frame is the one activation the VM builds itself rather than
    // through a call, so `lenient_arity`'s nil-fill never runs for it and
    // both slots would still be Uninit -- which the read-before-init check
    // catches on the first look. A `finally` thunk *is* called (by Defer,
    // with no arguments), so nil is what it gets and the test suffices.
    if (rf.parent < 0) {
      pre.push_back(b.assign(VarKind::Local, sa, b.array_lit({})));
      pre.push_back(b.assign(VarKind::Local, sk, b.nil_literal()));
    } else {
      pre.push_back(
          b.make_if(b.binary(BinOp::Eq, b.intrinsic(IntrinsicId::TypeOf, {A}),
                             b.str_literal("nil")),
                    b.assign(VarKind::Local, sa, b.array_lit({})), NodeId{}));
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
      names.push_back(b.str_literal(pi.name));
      if (pi.kind != ParamInfo::Default) {
        reqname.push_back(b.str_literal(pi.name));
        reqidx.push_back(b.literal(pos));
        required = pos + 1;
      }
      ++pos;
    }
    const NodeId all_names = b.array_lit(names);
    const NodeId req_names = b.array_lit(reqname);
    const NodeId req_idx = b.array_lit(reqidx);

    int32_t at = 0;
    for (const ParamInfo& pi : fi.params) {
      NodeId value;
      if (pi.kind == ParamInfo::Rest) {
        value = helper("$rest", {A, b.literal(pos)}, p);
      } else if (pi.kind == ParamInfo::KwRest) {
        value = helper("$kwrest", {K, all_names}, p);
      } else {
        // Positional, then by name, then the default -- and a TypeError
        // when none of the three answered.
        // `$missing` is handed every required parameter rather than this
        // one, because Python's message names all of them at once.
        const NodeId fallback =
            pi.kind == ParamInfo::Default
                ? read_var(pi.def_var, ctx, p)
                : helper("$missing",
                         {b.str_literal(fi.name), req_names, req_idx, A, K}, p);
        value = b.make_if(
            b.binary(BinOp::Gt, alen(), b.literal(at)),
            b.index(A, b.literal(at)),
            b.make_if(helper("$kwhas", {K, b.str_literal(pi.name)}, p),
                      b.index(K, b.str_literal(pi.name)), fallback));
        ++at;
      }
      const auto it = rf.cell_index.find(pi.var);
      if (it != rf.cell_index.end()) {
        pre.push_back(b.assign(VarKind::Cell, it->second, value));
      } else {
        const int32_t s = ctx.alloc_local(pi.name);
        rs.vars[static_cast<size_t>(pi.var)].slot = s;
        pre.push_back(b.assign(VarKind::Local, s, value));
      }
    }
    if (!has_rest) {
      pre.push_back(
          b.make_if(b.binary(BinOp::Gt, alen(), b.literal(pos)),
                    helper("$toomany",
                           {b.str_literal(fi.name), b.literal(required),
                            b.literal(pos), alen()},
                           p),
                    NodeId{}));
    }
    if (!has_kwrest) {
      pre.push_back(
          helper("$kwcheck", {K, all_names, b.str_literal(fi.name)}, p));
    }
  }

  // The body of a comprehension's function: the clauses nest outward-in,
  // and what the innermost one reaches is a push, a store or a yield.
  NodeId emit_comp(const Ast& a, FnCtx& ctx, SrcPos p) {
    auto b = Builder(m).at(p);
    const bool dict = a.tag == "dictcomp"_;
    const bool gen = a.tag == "gencomp"_ || a.tag == "bargen"_;
    const size_t head = dict ? 2 : 1;  // the element expression(s)

    std::vector<const Ast*> clauses;
    for (size_t i = head; i < a.nodes.size(); ++i) {
      clauses.push_back(a.nodes[i].get());
    }
    int32_t acc = -1;
    if (!gen) acc = ctx.alloc_local("$acc");
    const NodeId ACC = gen ? NodeId{} : b.varref(VarKind::Local, acc);

    // Built innermost-first, so each clause wraps what it produces.
    NodeId inner;
    if (gen) {
      inner = b.make_yield(emit_expr(*a.nodes[0], ctx));
    } else if (dict) {
      inner = b.set_index(ACC, emit_expr(*a.nodes[0], ctx),
                          emit_expr(*a.nodes[1], ctx));
    } else {
      inner = b.intrinsic(IntrinsicId::ArrayPush,
                          {ACC, emit_expr(*a.nodes[0], ctx)});
    }
    for (size_t k = clauses.size(); k-- > 0;) {
      const Ast& c = *clauses[k];
      if (c.tag == "compif"_) {
        inner = b.make_if(helper("$truthy", {emit_expr(*c.nodes[0], ctx)}, p),
                          inner, NodeId{});
        continue;
      }
      inner = emit_comp_for(c, inner, ctx, p);
    }
    std::vector<NodeId> out;
    if (!gen) {
      out.push_back(b.assign(
          VarKind::Local, acc,
          dict ? b.intrinsic(IntrinsicId::MapNew, {}) : b.array_lit({})));
    }
    out.push_back(inner);
    out.push_back(b.make_return(gen ? b.nil_literal() : ACC));
    return b.block(out);
  }

  NodeId emit_comp_for(const Ast& c, NodeId inner, FnCtx& ctx, SrcPos p) {
    auto b = Builder(m).at(p);
    const int32_t it = ctx.alloc_local("$it");
    const int32_t st = ctx.alloc_local("$step");
    const NodeId I = b.varref(VarKind::Local, it);
    const NodeId S = b.varref(VarKind::Local, st);
    const Ast& tg = *c.nodes[0];
    std::vector<NodeId> loop{
        b.assign(VarKind::Local, st, helper("$iternext", {I}, p)),
        b.make_if(b.index(S, b.str_literal("done")), b.make_break(), NodeId{})};
    const NodeId value = b.index(S, b.str_literal("value"));
    if (tg.nodes.size() == 1) {
      loop.push_back(write_var(decl_of.at(tg.nodes[0].get()), value, ctx, p));
    } else {
      const int32_t u = ctx.alloc_local("$unp");
      const NodeId U = b.varref(VarKind::Local, u);
      loop.push_back(b.assign(
          VarKind::Local, u,
          helper("$unpack",
                 {value, b.literal(static_cast<int64_t>(tg.nodes.size()))},
                 p)));
      for (size_t k = 0; k < tg.nodes.size(); ++k) {
        loop.push_back(write_var(decl_of.at(tg.nodes[k].get()),
                                 b.index(U, b.literal(static_cast<int64_t>(k))),
                                 ctx, p));
      }
    }
    loop.push_back(inner);
    return b.block({b.assign(VarKind::Local, it,
                             helper("$iter", {emit_expr(*c.nodes[1], ctx)}, p)),
                    b.make_while(b.bool_literal(true), b.block(loop))});
  }

  Module build(const Ast& program) {
    const int32_t top = new_fn(-1, "main");
    fns[static_cast<size_t>(top)].body = &program;

    rs.push_scope();
    bind_names(program, top);
    for (const auto& s : program.nodes) resolve_stmt(*s, top);
    rs.pop_scope();

    m.funcs.push_back({});
    rs.fns[static_cast<size_t>(top)].index = 0;
    for (const std::string& n : rt_names()) {
      rt[n] = static_cast<int32_t>(m.funcs.size());
      m.funcs.push_back({});
    }
    const size_t declared = fns.size();
    for (size_t f = 1; f < declared; ++f) {
      rs.fns[f].index = static_cast<int32_t>(m.funcs.size());
      m.funcs.push_back({});
    }


    rs.number_captures(m);
    empty_cmap = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back({});
    emit_runtime();

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
