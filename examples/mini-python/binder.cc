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
// here is delimited by braces, by `end` or by parentheses.

#include "binder.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <peglib.h>

#include "grammar.h"
#include "layout.h"
#include "runtime_parts.h"
#include "vmlib.h"

using namespace peg;
using namespace peg::udl;
using namespace coreir;
using namespace runtime_parts;

namespace mini_python {
namespace {

// What the implementation keeps on an object is its meta (runtime_parts.h's
// Hidden state), where no attribute reaches it. Classes are
// runtime_parts.h's layout: a class's facts are in its kind record and an
// instance's class is its meta, so a method's key is its name. A bignum, a
// tuple, an exception and a type object each have a kind record of their
// own, and no attributes beside it but a type's `__name__`.
//
// A bignum: its limb array and its sign, -1 or 1.
constexpr char kBigKind[] = "big";
constexpr int64_t kBigLimbs = 1;
constexpr int64_t kBigSign = 2;
// An exception is runtime_parts.h's: [kExcKind, name, message] for a
// builtin one, and [its class, nil, message] for an instance of a class of
// the program's, which keeps its message beside its fields and is named
// by its class -- runtime_parts' instance_class reads either shape.
// Inheritance: a class's kind record holds its base's table
// (kClassBase), the builtin exception it is rooted at -- `Exception` is not
// a class here, so a class that derives from one is marked rather than
// linked -- and the class value (the constructor closure), so that
// `isinstance` and `except` have an identity to compare. See README.md.
constexpr int64_t kClassRoot = kClassOwn;
constexpr int64_t kClassId = kClassOwn + 1;
// A tuple is its array in an object's kind record, which is the only way
// to keep `type(t)` and `print(t)` honest: the IR has one sequence, and
// Python has two that print and compare differently.
constexpr char kTupleKind[] = "tuple";
constexpr int64_t kTupleItems = 1;
// What `type(v)` answers is an object with the program-visible `__name__`,
// marked as a type by its kind record: marked by `__name__` itself, as it
// once was, an instance with an attribute of that name printed as a class.
constexpr char kTypeKind[] = "type";

// Base 10^9: a limb product plus two carries stays under 2^63, and
// rendering is nine decimal digits per limb with no division at all.
constexpr int64_t kBase = 1000000000;

SrcPos pos_of(const Ast& a) {
  return {static_cast<uint32_t>(a.line), static_cast<uint32_t>(a.column)};
}

[[noreturn]] void fail(const Ast& a, const std::string& msg) {
  coreir_rt::fail(msg, static_cast<uint32_t>(a.line),
                  static_cast<uint32_t>(a.column));
}

// One escape sequence, Python's: `tok[i]` is the backslash. Appends what
// it stands for to `out` and answers the index of its last character. An
// escape Python does not know keeps its backslash, as Python's does.
size_t unescape_at(const std::string& tok, size_t i, std::string& out) {
  const auto hexval = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  // \xhh, \uhhhh and \Uhhhhhhhh: exactly that many hex digits.
  const auto hex = [&](size_t from, size_t n, uint32_t& cp) {
    if (from + n > tok.size()) return false;
    cp = 0;
    for (size_t k = 0; k < n; ++k) {
      const int d = hexval(tok[from + k]);
      if (d < 0) return false;
      cp = cp * 16 + static_cast<uint32_t>(d);
    }
    return true;
  };
  const auto utf8 = [&](uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  };
  if (i + 1 >= tok.size()) {
    out.push_back('\\');
    return i;
  }
  const char c = tok[i + 1];
  switch (c) {
    case 'n': out.push_back('\n'); return i + 1;
    case 't': out.push_back('\t'); return i + 1;
    case 'r': out.push_back('\r'); return i + 1;
    case 'a': out.push_back('\a'); return i + 1;
    case 'b': out.push_back('\b'); return i + 1;
    case 'f': out.push_back('\f'); return i + 1;
    case 'v': out.push_back('\v'); return i + 1;
    case '\\': out.push_back('\\'); return i + 1;
    case '\'': out.push_back('\''); return i + 1;
    case '"': out.push_back('"'); return i + 1;
    case '\n': return i + 1;  // a backslash-newline continues the line
    default: break;
  }
  if (c >= '0' && c <= '7') {  // up to three octal digits
    uint32_t cp = 0;
    size_t k = i + 1;
    for (; k < tok.size() && k < i + 4 && tok[k] >= '0' && tok[k] <= '7'; ++k) {
      cp = cp * 8 + static_cast<uint32_t>(tok[k] - '0');
    }
    utf8(cp);
    return k - 1;
  }
  const size_t n = c == 'x' ? 2 : c == 'u' ? 4 : c == 'U' ? 8 : 0;
  uint32_t cp = 0;
  if (n > 0 && hex(i + 2, n, cp)) {
    utf8(cp);
    return i + 1 + n;
  }
  out.push_back('\\');
  out.push_back(c);
  return i + 1;
}

std::string unescape(const std::string& tok) {
  std::string out;
  for (size_t i = 0; i < tok.size(); ++i) {
    if (tok[i] == '\\') {
      i = unescape_at(tok, i, out);
    } else {
      out.push_back(tok[i]);
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
    if (tok[i] == '\\') {
      i = unescape_at(tok, i, out);
    } else {
      out.push_back(tok[i]);
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
  int32_t args_slot = -1;  // `$a`, the positional arguments' array
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
  HelperTable rt{m};  // the runtime helpers, by name
  Operators ops{rt, operators()};  // the VM's operations standing in for them
  std::map<std::string, int32_t> builtin_fn;

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
  // Its first `for`'s iterable is CPython's exception: evaluated where the
  // comprehension stands, in that scope, and handed to the function as an
  // iterator -- so `(x for x in f())` calls f when the generator is made.
  void resolve_comp(const Ast& a, int32_t fn) {
    resolve_expr(*first_for(a).nodes[1], fn);
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
        if (c.get() != &first_for(a)) resolve_expr(*c->nodes[1], f);
      } else if (c->tag == "compif"_) {
        resolve_expr(*c->nodes[0], f);
      } else {
        resolve_expr(*c, f);
      }
    }
    rs.pop_scope();
  }

  static const Ast& first_for(const Ast& comp) {
    for (const auto& c : comp.nodes) {
      if (c->tag == "compfor"_) return *c;
    }
    fail(comp, "a comprehension needs a `for`");
  }

  // ==== The runtime this front end writes in its own IR ====================

  static const std::vector<std::string>& rt_names() {
    static const std::vector<std::string> names = {
        // The bignum recipe, and the promotion rule around it.
        "$isbig", "$abs", "$tolimbs", "$mkbig", "$biglimbs", "$bigsign",
        "$ucmp", "$uadd", "$usub", "$umul", "$bigadd", "$bigmul", "$bstr",
        "$tofloat", "$neg", "$pyabs",
        // Arithmetic and comparison, dispatching on what the operands are.
        "$add", "$sub", "$mul", "$fdiv", "$idiv", "$mod", "$pow", "$cmp",
        "$lt", "$le", "$gt", "$ge", "$eq", "$truthy",
        // Display.
        "$fstr", "$str", "$strrepr", "$repr", "$liststr", "$dictstr",
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
        "$excinit", "$excmsg", "$noinit", "$dunder",
        // Tuples, and the sequence builtins that hand them back.
        "$tuple", "$untup", "$istup", "$tupstr", "$unpack", "$items",
        "$values", "$enumerate", "$zip", "$sorted", "$sum", "$minmax",
        "$next",
        // The methods a program actually reaches for. Every one of them is
        // a scan, so every one of them is written here rather than handed
        // to the host -- `upper` and `lower` included, being the VM's own
        // ASCII StrUpper/StrLower.
        "$isspace", "$split", "$strip", "$replace", "$find", "$scount",
        "$startswith", "$endswith", "$alldigits", "$apop", "$ainsert",
        "$aremove", "$afind",
        "$aindex", "$acount", "$areverse", "$asort", "$dpop", "$delitem",
        "$fmt",
        "$specerr",
    };
    return names;
  }

  // Python's rules that are the VM's own operation on some values: the
  // VM's answer where the two agree, the helper's on the rest
  // (runtime_parts.h's Operators).
  //
  //   $truthy         a condition: None, a bool, a number and a function
  //                   are false or true as Value::truthy() says; a string,
  //                   a list, a dict and a tuple (an object here), false
  //                   when empty, and any other object are $truthy's.
  //   $add $sub $mul  `+`, `-` and `*` on two numbers, and `+` on two
  //   $neg            strings, and `-x` on a number -- an int answer
  //                   exactly, since one that would wrap goes to the limbs
  //                   (the promotion rule, at rt_add).
  //   $idiv $mod      `//` and `%` on two ints, Python's floor; a zero
  //                   divisor (ZeroDivisionError), a float, whose floor is
  //                   CPython's own, or a bignum is the helper's.
  //   $lt $le $gt $ge an ordering of two ints or two strings; a float is
  //                   not, as $cmp orders it through ToDouble beside an
  //                   int, and a bignum, a tuple or an object are $cmp's.
  //   $eq             `==` on None, bools, ints and strings; two types Eq
  //                   refuses, a bignum, a float, a container or an
  //                   object's own `__eq__` are $eq's.
  //   $idx            `c[k]` on a list or a string and an index in range;
  //                   a negative one, one out of range (IndexError), a
  //                   tuple and a dict are $idx's.
  static Operators::Table operators() {
    const uint32_t ints = tags_named("int");
    const uint32_t order = tags_named({"int", "string"});
    return {
        {"$truthy",
         VmOp::unary(UnOp::Truth, truthy_tags_except(
                                      {"string", "array", "map", "object"}))},
        {"$add", VmOp::binary(BinOp::Add, add_tags(), true)},
        {"$sub", VmOp::binary(BinOp::Sub, number_tags(), true)},
        {"$mul", VmOp::binary(BinOp::Mul, number_tags(), true)},
        {"$neg", VmOp::unary(UnOp::Neg, number_tags(), true)},
        {"$idiv", VmOp::binary(BinOp::FloorDiv, ints)},
        {"$mod", VmOp::binary(BinOp::FloorMod, ints)},
        {"$lt", VmOp::binary(BinOp::Lt, order)},
        {"$le", VmOp::binary(BinOp::Le, order)},
        {"$gt", VmOp::binary(BinOp::Gt, order)},
        {"$ge", VmOp::binary(BinOp::Ge, order)},
        {"$eq", VmOp::binary(BinOp::Eq, tags_named({"nil", "bool", "int",
                                                    "string"}))},
        {"$idx", VmOp::index(tags_named({"array", "string", "int"}))},
    };
  }

  // One helper of the runtime above under construction.
  struct RT : HelperWriter {
    explicit RT(Binder& bd) : HelperWriter(bd.rt) {}
  };


  // -- The bignum recipe ---------------------------------------------------
  //
  // A little-endian Array of Int limbs in base 10^9, no leading zero limbs,
  // kept with the sign in an object's kind record so that nothing takes one
  // for a list. Zero and
  // anything that fits an int64 are *not* big: $mkbig demotes, which is
  // what keeps ordinary arithmetic on ordinary numbers.

  void rt_isbig() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.ret(is_kind(r, v, kBigKind)));
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
    r.add(r.ret(of_kind(r, r.obj({}), kBigKind, {r.L(limbs), r.L(sign)})));
    r.finish("$mkbig");
  }

  void rt_biglimbs() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.iff(r.call("$isbig", {r.L(v)}),
                r.ret(kind_field(r, r.L(v), kBigLimbs))));
    r.add(r.ret(r.call("$tolimbs", {r.call("$abs", {r.L(v)})})));
    r.finish("$biglimbs");
  }

  void rt_bigsign() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.iff(r.call("$isbig", {r.L(v)}),
                r.ret(kind_field(r, r.L(v), kBigSign))));
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
    r.add(r.iff(r.b.unary(UnOp::Not, r.call("$isbig", {r.L(v)})),
                r.ret(r.in(IntrinsicId::ToStr, {r.L(v)}))));
    r.add(r.set(x, kind_field(r, r.L(v), kBigLimbs)));
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
    r.add(r.iff(r.bin(BinOp::Lt, kind_field(r, r.L(v), kBigSign), r.I(0)),
                r.set(out, r.bin(BinOp::Add, r.S("-"), r.L(out)))));
    r.add(r.ret(r.L(out)));
    r.finish("$bstr");
  }

  void rt_tofloat() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [x, acc, i] = r.locals("x", "acc", "i");
    r.add(r.iff(r.is(r.typ(v), "double"), r.ret(r.L(v))));
    r.add(r.iff(
        r.call("$isbig", {r.L(v)}),
        r.blk(
            {r.set(x, kind_field(r, r.L(v), kBigLimbs)), r.set(acc, r.D(0.0)),
             r.set(i, r.bin(BinOp::Sub, r.len(r.L(x)), r.I(1))),
             r.wh(r.bin(BinOp::Ge, r.L(i), r.I(0)),
                  r.blk({r.set(acc, r.bin(BinOp::Add,
                                          r.bin(BinOp::Mul, r.L(acc), r.D(1e9)),
                                          r.in(IntrinsicId::ToDouble,
                                               {r.idx(r.L(x), r.L(i))}))),
                         r.set(i, r.bin(BinOp::Sub, r.L(i), r.I(1)))})),
             r.iff(r.bin(BinOp::Lt, kind_field(r, r.L(v), kBigSign), r.I(0)),
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
        r.ret(of_kind(
            r, r.obj({}), kBigKind,
            {kind_field(r, r.L(v), kBigLimbs),
             r.b.unary(UnOp::Neg, kind_field(r, r.L(v), kBigSign))}))));
    r.add(r.ret(r.b.unary(UnOp::Neg, r.L(v))));
    r.finish("$neg");
  }

  // Python's abs, for every number this subset has. Both `abs(x)` and abs
  // as a value come here, so the argument is a parameter -- evaluated
  // once, by the call -- rather than an expression the test and the
  // answer would each run.
  void rt_pyabs() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.iff(r.bin(BinOp::Lt, r.call("$cmp", {r.L(v), r.I(0)}), r.I(0)),
                r.ret(r.call("$neg", {r.L(v)}))));
    r.add(r.ret(r.L(v)));
    r.finish("$pyabs");
  }

  // -- Arithmetic, dispatching on what the operands turn out to be --------
  //
  // The promotion rule lives here: an operation on two machine integers
  // whose answer fits int64 is done with BinOp, and everything else goes to
  // the limbs. That is what keeps a loop counter a loop counter while
  // `2 ** 100` is exact. The rule is an exact operator with the limbs'
  // helper as its fallback (vmlib.h's Tag::Unary, kExact) -- the same
  // operator a `+`, `-` or `*` in the program is (Binder::operators), so
  // the two cannot disagree about where int64 ends.

  NodeId int_pair(RT& r, NodeId ta, NodeId tb) {
    return r.both(r.is(ta, "int"), r.is(tb, "int"));
  }

  NodeId exact_or(RT& r, BinOp op, FuncWriter::Slot a, FuncWriter::Slot b,
                  const char* limbs) {
    return r.b.binary(op, r.L(a), r.L(b),
                      Fallback{r.ref(limbs), tags_named("int"), true});
  }

  void rt_add() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    r.add(r.iff(r.both(r.is(r.typ(a), "string"), r.is(r.typ(b), "string")),
                r.ret(r.bin(BinOp::Add, r.L(a), r.L(b)))));
    r.add(r.iff(r.both(r.is(r.typ(a), "array"), r.is(r.typ(b), "array")),
                r.ret(r.call("$listadd", {r.L(a), r.L(b)}))));
    const auto items = [&](FuncWriter::Slot t) {
      return kind_field(r, r.L(t), kTupleItems);
    };
    r.add(r.iff(r.both(r.call("$istup", {r.L(a)}), r.call("$istup", {r.L(b)})),
                r.ret(r.call("$tuple", {r.call("$listadd",
                                               {items(a), items(b)})}))));
    r.add(r.iff(r.either(r.is(r.typ(a), "double"), r.is(r.typ(b), "double")),
                r.ret(r.bin(BinOp::Add, r.call("$tofloat", {r.L(a)}),
                            r.call("$tofloat", {r.L(b)})))));
    // Two ints the exact Add answers; anything else, and an int sum past
    // int64, is the limbs'.
    r.add(r.ret(exact_or(r, BinOp::Add, a, b, "$bigadd")));
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
    r.add(r.iff(r.both(r.is(r.typ(a), "string"), r.is(r.typ(b), "int")),
                r.ret(r.call("$strmul", {r.L(a), r.L(b)}))));
    r.add(r.iff(r.both(r.is(r.typ(a), "array"), r.is(r.typ(b), "int")),
                r.ret(r.call("$listmul", {r.L(a), r.L(b)}))));
    r.add(r.iff(r.either(r.is(r.typ(a), "double"), r.is(r.typ(b), "double")),
                r.ret(r.bin(BinOp::Mul, r.call("$tofloat", {r.L(a)}),
                            r.call("$tofloat", {r.L(b)})))));
    r.add(r.ret(exact_or(r, BinOp::Mul, a, b, "$bigmul")));
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

  // `//` and `%` floor, like Lua's and unlike C's: on two ints they are
  // FloorDiv and FloorMod, and the operators in the program are those, with
  // these behind them (Binder::operators) for a zero divisor, which raises
  // ZeroDivisionError where a float's would be inf or nan, and the rest.
  // A float's (an int beside it widening) are CPython's float_floor_div
  // and float_rem, which differ from FloorDiv's and FloorMod's in two
  // places, each said where it falls; a bignum's this subset leaves out,
  // and the operators refuse it.
  void rt_idiv() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [t, q, f] = r.locals("t", "q", "f");
    r.add(r.iff(r.bin(BinOp::Eq, r.L(b), r.I(0)),
                r.ret(r.call("$exc", {r.S("ZeroDivisionError"),
                                      r.S("division by zero")}))));
    r.add(r.iff(int_pair(r, r.typ(a), r.typ(b)),
                r.ret(r.bin(BinOp::FloorDiv, r.L(a), r.L(b)))));
    // FloorDiv floors the rounded quotient, which may have rounded up to a
    // whole number: 1 // 0.2 is 5.0 there and 4.0 here. CPython's is the
    // quotient of a - fmod(a, b), a whole multiple of b that the rounding
    // leaves only near a whole number: a step down when the remainder is
    // on the other side of zero from b, then snapped to the nearest, and a
    // zero signed as a / b is.
    r.add(r.set(t, r.in(IntrinsicId::FMod, {r.L(a), r.L(b)})));
    r.add(r.set(q, r.bin(BinOp::Div, r.bin(BinOp::Sub, r.L(a), r.L(t)),
                         r.L(b))));
    r.add(r.iff(r.both(r.bin(BinOp::Ne, r.L(t), r.D(0.0)),
                       r.bin(BinOp::Ne, r.bin(BinOp::Lt, r.L(t), r.D(0.0)),
                             r.bin(BinOp::Lt, r.L(b), r.D(0.0)))),
                r.set(q, r.bin(BinOp::Sub, r.L(q), r.D(1.0)))));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(q), r.D(0.0)),
                r.ret(r.bin(BinOp::Mul, r.D(0.0),
                            r.bin(BinOp::Div, r.L(a), r.L(b))))));
    r.add(r.set(f, r.bin(BinOp::FloorDiv, r.L(q), r.D(1.0))));
    r.add(r.iff(r.bin(BinOp::Gt, r.bin(BinOp::Sub, r.L(q), r.L(f)), r.D(0.5)),
                r.set(f, r.bin(BinOp::Add, r.L(f), r.D(1.0)))));
    r.add(r.ret(r.L(f)));
    r.finish("$idiv");
  }

  void rt_mod() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [m] = r.locals("m");
    r.add(r.iff(r.bin(BinOp::Eq, r.L(b), r.I(0)),
                r.ret(r.call("$exc", {r.S("ZeroDivisionError"),
                                      r.S("division by zero")}))));
    r.add(r.iff(int_pair(r, r.typ(a), r.typ(b)),
                r.ret(r.bin(BinOp::FloorMod, r.L(a), r.L(b)))));
    // FloorMod's, but for a zero remainder, which keeps the dividend's sign
    // there and takes the divisor's here.
    r.add(r.set(m, r.bin(BinOp::FloorMod, r.L(a), r.L(b))));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(m), r.D(0.0)),
                r.ret(r.b.make_if(r.bin(BinOp::Lt, r.L(b), r.D(0.0)),
                                  r.D(-0.0), r.D(0.0)))));
    r.add(r.ret(r.L(m)));
    r.finish("$mod");
  }

  // Repeated squaring, over $mul -- so the promotion rule applies at every
  // step and `2 ** 100` is exact without anything here knowing it will be.
  void rt_pow() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [acc, sq, e, fa, fb] = r.locals("acc", "sq", "e", "fa", "fb");
    const double inf = std::numeric_limits<double>::infinity();
    // A negative exponent always lands in the double branch, so that is
    // where zero to a negative power -- no value, in either number kind --
    // is refused. Zero to -inf is inf, the way Python and Pow both say.
    r.add(r.iff(
        r.either(r.is(r.typ(a), "double"),
                 r.either(r.is(r.typ(b), "double"),
                          r.bin(BinOp::Lt, r.L(b), r.I(0)))),
        r.blk({r.set(fa, r.call("$tofloat", {r.L(a)})),
               r.set(fb, r.call("$tofloat", {r.L(b)})),
               r.iff(r.both(r.bin(BinOp::Eq, r.L(fa), r.D(0.0)),
                            r.both(r.bin(BinOp::Lt, r.L(fb), r.D(0.0)),
                                   r.bin(BinOp::Gt, r.L(fb), r.D(-inf)))),
                     r.ret(r.call("$exc", {r.S("ZeroDivisionError"),
                                           r.S("zero to a negative power")}))),
               r.ret(r.in(IntrinsicId::Pow, {r.L(fa), r.L(fb)}))})));
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
    const auto [fa, fb, sa, sb, c, xa, xb, i, e] =
        r.locals("fa", "fb", "sa", "sb", "c", "xa", "xb", "i", "e");
    // A list or a tuple compares element by element, which is what makes
    // `sorted` work on a list of pairs.
    r.add(r.iff(
        r.both(r.either(r.is(r.typ(a), "array"), r.call("$istup", {r.L(a)})),
               r.either(r.is(r.typ(b), "array"), r.call("$istup", {r.L(b)}))),
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
        r.both(r.is(r.typ(a), "string"), r.is(r.typ(b), "string")),
        r.ret(r.iff(r.bin(BinOp::Lt, r.L(a), r.L(b)), r.I(-1),
                    r.iff(r.bin(BinOp::Gt, r.L(a), r.L(b)), r.I(1), r.I(0))))));
    // Two integers that both fit are compared directly; anything wider goes
    // through the limbs, sign first.
    r.add(r.iff(
        int_pair(r, r.typ(a), r.typ(b)),
        r.ret(r.iff(r.bin(BinOp::Lt, r.L(a), r.L(b)), r.I(-1),
                    r.iff(r.bin(BinOp::Gt, r.L(a), r.L(b)), r.I(1), r.I(0))))));
    r.add(r.iff(r.either(r.is(r.typ(a), "double"), r.is(r.typ(b), "double")),
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

  // `<`, `<=`, `>` and `>=` by $cmp, each the fallback of the operator a
  // comparison in the program is (Binder::order).
  void rt_order(const char* name, BinOp op) {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    r.add(r.ret(r.bin(op, r.call("$cmp", {r.L(a), r.L(b)}), r.I(0))));
    r.finish(name);
  }

  void rt_eq() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [i, ks, k, f_] = r.locals("i", "ks", "k", "f");
    const auto numeric = [&](NodeId t, NodeId v) {
      return r.either(r.is(t, "int"),
                      r.either(r.is(t, "double"), r.call("$isbig", {v})));
    };
    r.add(r.iff(
        r.both(numeric(r.typ(a), r.L(a)), numeric(r.typ(b), r.L(b))),
        r.ret(r.bin(BinOp::Eq, r.call("$cmp", {r.L(a), r.L(b)}), r.I(0)))));
    r.add(r.iff(r.bin(BinOp::Ne, r.typ(a), r.typ(b)), r.ret(r.Bo(false))));
    r.add(r.iff(r.is(r.typ(a), "nil"), r.ret(r.Bo(true))));
    r.add(r.iff(r.either(r.is(r.typ(a), "bool"), r.is(r.typ(a), "string")),
                r.ret(r.bin(BinOp::Eq, r.L(a), r.L(b)))));
    r.add(r.iff(r.is(r.typ(a), "array"), equal_elements(r, a, b, i, "$eq")));
    r.add(r.iff(r.is(r.typ(a), "map"),
                equal_entries(r, a, b, i, ks, k, "$eq")));
    r.add(r.iff(
        r.either(r.call("$istup", {r.L(a)}), r.call("$istup", {r.L(b)})),
        r.blk({r.iff(r.b.unary(UnOp::Not, r.both(r.call("$istup", {r.L(a)}),
                                                 r.call("$istup", {r.L(b)}))),
                     r.ret(r.Bo(false))),
               r.ret(r.call("$eq", {kind_field(r, r.L(a), kTupleItems),
                                    kind_field(r, r.L(b), kTupleItems)}))})));
    r.add(r.set(f_, r.call("$dunder", {r.L(a), r.S("__eq__")})));
    r.add(r.iff(r.isnt(r.typ(f_), "nil"),
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
    r.add(r.iff(r.is(r.typ(v), "nil"), r.ret(r.Bo(false))));
    r.add(r.iff(r.is(r.typ(v), "bool"), r.ret(r.L(v))));
    r.add(
        r.iff(r.is(r.typ(v), "int"), r.ret(r.bin(BinOp::Ne, r.L(v), r.I(0)))));
    r.add(r.iff(r.is(r.typ(v), "double"),
                r.ret(r.bin(BinOp::Ne, r.L(v), r.D(0.0)))));
    r.add(r.iff(
        r.either(r.is(r.typ(v), "string"),
                 r.either(r.is(r.typ(v), "array"), r.is(r.typ(v), "map"))),
        r.ret(r.bin(BinOp::Gt, r.len(r.L(v)), r.I(0)))));
    // A tuple is an object here, false when empty as a list is.
    r.add(r.iff(r.call("$istup", {r.L(v)}),
                r.ret(r.bin(BinOp::Gt, r.len(r.call("$untup", {r.L(v)})),
                            r.I(0)))));
    r.add(r.ret(r.Bo(true)));
    r.finish("$truthy");
  }

  // -- Display -------------------------------------------------------------
  //
  // Python's float repr is shortest-round-trip with a ".0" forced onto a
  // whole value -- which is to_display's output plus the one rule
  // to_display's comment says a front end should add for itself, and the
  // rule runtime_parts.h's float_str writes for Ruby, Guile and culebra too.
  void rt_fstr() {
    RT r(*this);
    float_str(r, {"nan", "inf", "-inf"});
    r.finish("$fstr");
  }

  void rt_str() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [f] = r.locals("f");
    r.add(r.iff(r.is(r.typ(v), "string"), r.ret(r.L(v))));
    r.add(r.iff(r.is(r.typ(v), "nil"), r.ret(r.S("None"))));
    r.add(r.iff(r.is(r.typ(v), "bool"),
                r.ret(r.iff(r.L(v), r.S("True"), r.S("False")))));
    r.add(r.iff(r.is(r.typ(v), "int"),
                r.ret(r.in(IntrinsicId::ToStr, {r.L(v)}))));
    r.add(r.iff(r.is(r.typ(v), "double"), r.ret(r.call("$fstr", {r.L(v)}))));
    r.add(r.iff(r.is(r.typ(v), "array"), r.ret(r.call("$liststr", {r.L(v)}))));
    r.add(r.iff(r.is(r.typ(v), "map"), r.ret(r.call("$dictstr", {r.L(v)}))));
    r.add(r.iff(r.is(r.typ(v), "function"), r.ret(r.S("<function>"))));
    r.add(r.iff(r.is(r.typ(v), "generator"), r.ret(r.S("<generator>"))));
    r.add(r.iff(r.call("$isbig", {r.L(v)}), r.ret(r.call("$bstr", {r.L(v)}))));
    r.add(r.iff(is_kind(r, v, kTupleKind),
                r.ret(r.call("$tupstr",
                             {kind_field(r, r.L(v), kTupleItems)}))));
    // A builtin exception displays as its message.
    r.add(r.iff(
        is_kind(r, v, kExcKind),
        r.ret(r.call("$str", {kind_field(r, r.L(v), kExcMessage)}))));
    r.add(r.set(f, r.call("$dunder", {r.L(v), r.S("__str__")})));
    r.add(r.iff(r.isnt(r.typ(f), "nil"),
                r.ret(r.call("$str", {r.b.call_value(r.L(f), {r.arr({r.L(v)}),
                                                              r.Nil()})}))));
    // A class of the program's own that derives from a builtin exception
    // displays as its message, the way the builtin ones do.
    r.add(r.iff(
        r.both(is_exception(r, v),
               r.isnt(r.typ(kind_field(r, r.L(v), kExcMessage)), "nil")),
        r.ret(r.call("$str", {kind_field(r, r.L(v), kExcMessage)}))));
    r.add(r.iff(is_kind(r, v, kTypeKind),
                r.ret(r.bin(BinOp::Add,
                            r.bin(BinOp::Add, r.S("<class '"),
                                  r.idx(r.L(v), r.S("__name__"))),
                            r.S("'>")))));
    r.add(r.set(f, instance_class(r, v)));
    r.add(r.iff(r.isnt(r.typ(f), "nil"),
                r.ret(r.bin(BinOp::Add,
                            r.bin(BinOp::Add, r.S("<"), class_name(r, r.L(f))),
                            r.S(" object>")))));
    r.add(r.ret(r.S("<object>")));
    r.finish("$str");
  }

  // A string's repr, as CPython writes it: in single quotes, or double ones
  // when it holds a ' and no ", with the quote, the backslash, \n, \t, \r
  // and any other control byte escaped. Bytes above 0x7f are UTF-8 and
  // printable, and stay as they are.
  void rt_strrepr() {
    RT r(*this);
    const auto [s] = r.params("s");
    const auto [q, out, i, c, n] = r.locals("q", "out", "i", "c", "n");
    const auto is = [&](const char* ch) {
      return r.bin(BinOp::Eq, r.L(c), r.S(ch));
    };
    const auto put = [&](NodeId x) {
      return r.set(out, r.bin(BinOp::Add, r.L(out), x));
    };
    const NodeId hex = r.S("0123456789abcdef");
    const auto digit = [&](NodeId d) {
      return r.in(IntrinsicId::StrSlice,
                  {hex, d, r.bin(BinOp::Add, d, r.I(1))});
    };
    r.add(r.set(q, r.S("'")));
    r.add(
        r.iff(r.both(r.call("$in", {r.S("'"), r.L(s)}),
                     r.b.unary(UnOp::Not, r.call("$in", {r.S("\""), r.L(s)}))),
              r.set(q, r.S("\""))));
    r.add(r.set(out, r.L(q)));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(s))),
        r.blk({r.set(c, r.in(IntrinsicId::StrSlice,
                             {r.L(s), r.L(i), r.bin(BinOp::Add, r.L(i), r.I(1))})),
               r.set(n, r.in(IntrinsicId::StrByte, {r.L(s), r.L(i)})),
               r.iff(r.either(is("\\"), r.bin(BinOp::Eq, r.L(c), r.L(q))),
                     put(r.bin(BinOp::Add, r.S("\\"), r.L(c))),
                     r.iff(is("\n"), put(r.S("\\n")),
                           r.iff(is("\t"), put(r.S("\\t")),
                                 r.iff(is("\r"), put(r.S("\\r")),
                                       r.iff(r.either(r.bin(BinOp::Lt, r.L(n), r.I(0x20)),
                                                      r.bin(BinOp::Eq, r.L(n), r.I(0x7f))),
                                             put(r.bin(BinOp::Add, r.S("\\x"),
                                                       r.bin(BinOp::Add,
                                                             digit(r.bin(BinOp::Div, r.L(n), r.I(16))),
                                                             digit(r.bin(BinOp::Mod, r.L(n), r.I(16)))))),
                                             put(r.L(c))))))),
               r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.bin(BinOp::Add, r.L(out), r.L(q))));
    r.finish("$strrepr");
  }

  void rt_repr() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [f] = r.locals("f");
    r.add(r.iff(r.is(r.typ(v), "string"), r.ret(r.call("$strrepr", {r.L(v)}))));
    r.add(r.set(f, r.call("$dunder", {r.L(v), r.S("__repr__")})));
    r.add(r.iff(r.isnt(r.typ(f), "nil"),
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
    r.add(r.b.make_throw(
        exception(r, r.S(kExcKind), r.L(name), r.L(msg))));
    r.finish("$exc");
  }

  // `except Exception` catches everything; a class of the program's own
  // matches its subclasses too. The builtin names have no hierarchy.
  void rt_isexc() {
    RT r(*this);
    const auto [e, name] = r.params("e", "name");
    // A builtin exception travels as its name; a class of the program's own
    // travels as its value, and then the test is the identity walk.
    r.add(r.iff(r.isnt(r.typ(name), "string"),
                r.ret(r.call("$isinstv", {r.L(e), r.L(name)}))));
    r.add(r.iff(r.is(r.L(name), "Exception"), r.ret(r.Bo(true))));
    r.add(r.ret(r.call("$isname", {r.L(e), r.L(name)})));
    r.finish("$isexc");
  }

  void rt_len() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.set(v, r.call("$untup", {r.L(v)})));
    r.add(r.iff(
        r.either(r.is(r.typ(v), "string"),
                 r.either(r.is(r.typ(v), "array"), r.is(r.typ(v), "map"))),
        r.ret(r.len(r.L(v)))));
    r.add(r.ret(r.call("$exc", {r.S("TypeError"),
                                r.S("object has no len()")})));
    r.finish("$len");
  }

  // A negative index counts from the end, and out of range raises rather
  // than trapping -- Python's rules, written here because index_fault()'s
  // own comment says a language that wants them normalizes first.
  void rt_idx() {
    RT r(*this);
    const auto [v, k] = r.params("v", "k");
    const auto [i] = r.locals("i");
    r.add(r.set(v, r.call("$untup", {r.L(v)})));
    r.add(r.iff(
        r.either(r.is(r.typ(v), "array"), r.is(r.typ(v), "string")),
        r.blk({r.set(i, r.L(k)),
               r.iff(r.bin(BinOp::Lt, r.L(i), r.I(0)),
                     r.set(i, r.bin(BinOp::Add, r.L(i), r.len(r.L(v))))),
               r.iff(r.either(r.bin(BinOp::Lt, r.L(i), r.I(0)),
                              r.bin(BinOp::Ge, r.L(i), r.len(r.L(v)))),
                     r.ret(r.call("$exc",
                                  {r.S("IndexError"),
                                   r.iff(r.is(r.typ(v), "array"),
                                         r.S("list index out of range"),
                                         r.S("string index out of range"))}))),
               r.ret(r.idx(r.L(v), r.L(i)))})));
    r.add(r.iff(
        r.is(r.typ(v), "map"),
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
    const auto [i] = r.locals("i");
    r.add(r.iff(
        r.is(r.typ(v), "array"),
        r.blk({r.set(i, r.L(k)),
               r.iff(r.bin(BinOp::Lt, r.L(i), r.I(0)),
                     r.set(i, r.bin(BinOp::Add, r.L(i), r.len(r.L(v))))),
               r.iff(r.either(r.bin(BinOp::Lt, r.L(i), r.I(0)),
                              r.bin(BinOp::Ge, r.L(i), r.len(r.L(v)))),
                     r.ret(r.call("$exc", {r.S("IndexError"),
                                           r.S("list assignment index out of "
                                               "range")}))),
               r.sidx(r.L(v), r.L(i), r.L(val)), r.ret(r.L(val))})));
    r.add(r.iff(r.is(r.typ(v), "map"),
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
          r.is(r.typ(arg), "nil"), r.set(out, dflt),
          r.blk(
              {r.set(out, r.L(arg)),
               r.iff(r.bin(BinOp::Lt, r.L(out), r.I(0)),
                     r.set(out, r.bin(BinOp::Add, r.L(n), r.L(out)))),
               r.iff(r.bin(BinOp::Lt, r.L(out), r.I(0)), r.set(out, r.I(0))),
               r.iff(r.bin(BinOp::Gt, r.L(out), r.L(n)), r.set(out, r.L(n)))}));
    };
    // A slice of a tuple is a tuple.
    r.add(r.iff(
        r.call("$istup", {r.L(v)}),
        r.ret(r.call("$tuple",
                     {r.call("$slice", {kind_field(r, r.L(v), kTupleItems),
                                        r.L(i), r.L(j)})}))));
    r.add(r.set(n, r.len(r.L(v))));
    r.add(norm(a, i, r.I(0)));
    r.add(norm(b, j, r.L(n)));
    r.add(r.iff(r.bin(BinOp::Lt, r.L(b), r.L(a)), r.set(b, r.L(a))));
    r.add(
        r.iff(r.is(r.typ(v), "string"),
              r.ret(r.in(IntrinsicId::StrSlice, {r.L(v), r.L(a), r.L(b)})),
              r.ret(r.in(IntrinsicId::ArraySlice, {r.L(v), r.L(a), r.L(b)}))));
    r.finish("$slice");
  }

  void rt_in() {
    RT r(*this);
    const auto [needle, hay] = r.params("needle", "hay");
    const auto [i] = r.locals("i");
    r.add(r.set(hay, r.call("$untup", {r.L(hay)})));
    r.add(r.iff(r.is(r.typ(hay), "map"), r.ret(r.has(r.L(hay), r.L(needle)))));
    // `"e" in "hello"` is a substring test, not a membership one.
    r.add(
        r.iff(r.is(r.typ(hay), "string"),
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
        r.is(r.typ(hay), "array"),
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
    const auto [found] = r.locals("found");
    // An instance's meta is its class, and the walk ends at once at
    // anything else's -- nil, or a kind record.
    r.add(r.iff(
        r.is(r.typ(o), "object"),
        r.blk(
            {r.iff(r.has(r.L(o), r.L(name)), r.ret(r.idx(r.L(o), r.L(name)))),
             r.set(found,
                   r.call("$clsfind", {instance_class(r, o), r.L(name)})),
             r.iff(r.isnt(r.typ(found), "nil"), r.ret(r.L(found)))})));
    r.add(r.ret(
        r.call("$exc", {r.S("AttributeError"),
                        r.bin(BinOp::Add, r.S("no attribute "), r.L(name))})));
    r.finish("$getattr");
  }

  void rt_setattr() {
    RT r(*this);
    const auto [o, name, val] = r.params("o", "name", "val");
    r.add(r.iff(r.is(r.typ(o), "object"),
                r.blk({r.sidx(r.L(o), r.L(name), r.L(val)), r.ret(r.L(val))})));
    r.add(r.ret(r.call("$exc", {r.S("AttributeError"),
                                r.S("cannot set attribute")})));
    r.finish("$setattr");
  }

  void rt_iter() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.set(v, r.call("$untup", {r.L(v)})));
    r.add(r.iff(r.is(r.typ(v), "generator"),
                r.ret(generator_cursor(r, r.L(v)))));
    r.add(r.iff(r.is(r.typ(v), "map"),
                r.ret(sequence_cursor(
                    r, r.in(IntrinsicId::ObjectKeys, {r.L(v)})))));
    r.add(r.iff(r.either(r.is(r.typ(v), "array"), r.is(r.typ(v), "string")),
                r.ret(sequence_cursor(r, r.L(v)))));
    r.add(r.ret(r.call("$exc", {r.S("TypeError"),
                                r.S("object is not iterable")})));
    r.finish("$iter");
  }

  void rt_iternext() {
    RT r(*this);
    const auto [it] = r.params("it");
    iter_step(r, it);
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
    const auto [c] = r.locals("c");
    r.add(r.iff(r.is(r.typ(v), "nil"), r.ret(r.S("NoneType"))));
    r.add(r.iff(r.is(r.typ(v), "bool"), r.ret(r.S("bool"))));
    r.add(r.iff(r.is(r.typ(v), "int"), r.ret(r.S("int"))));
    r.add(r.iff(r.is(r.typ(v), "double"), r.ret(r.S("float"))));
    r.add(r.iff(r.is(r.typ(v), "string"), r.ret(r.S("str"))));
    r.add(r.iff(r.is(r.typ(v), "array"), r.ret(r.S("list"))));
    r.add(r.iff(r.is(r.typ(v), "map"), r.ret(r.S("dict"))));
    r.add(r.iff(r.is(r.typ(v), "function"), r.ret(r.S("function"))));
    r.add(r.iff(r.is(r.typ(v), "generator"), r.ret(r.S("generator"))));
    r.add(r.iff(r.call("$isbig", {r.L(v)}), r.ret(r.S("int"))));
    r.add(r.iff(r.call("$istup", {r.L(v)}), r.ret(r.S("tuple"))));
    r.add(r.iff(is_kind(r, v, kExcKind),
                r.ret(kind_field(r, r.L(v), kExcName))));
    r.add(r.set(c, instance_class(r, v)));
    r.add(r.iff(r.isnt(r.typ(c), "nil"), r.ret(class_name(r, r.L(c)))));
    r.add(r.ret(r.S("object")));
    r.finish("$typename");
  }

  void rt_type() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.ret(of_kind(
        r, r.obj({{"__name__", r.call("$typename", {r.L(v)})}}), kTypeKind,
        {})));
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
    add_for_each(r, it, st, r.call("$iter", {r.L(v)}),
                   {r.push(r.L(out), value_of(r, st))});
    r.add(r.ret(r.L(out)));
    r.finish("$tolist");
  }

  void rt_toint() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [s, i, neg, acc, c] = r.locals("s", "i", "neg", "acc", "c");
    r.add(r.iff(r.is(r.typ(v), "double"),
                r.ret(r.in(IntrinsicId::ToInt, {r.L(v)}))));
    r.add(r.iff(r.is(r.typ(v), "bool"), r.ret(r.iff(r.L(v), r.I(1), r.I(0)))));
    // `int("...")` accumulates through $mul and $add, so a literal too big
    // for an int64 becomes a bignum with nothing further to say about it.
    r.add(r.iff(
        r.is(r.typ(v), "string"),
        r.blk(
            {r.set(s, r.call("$strip", {r.L(v), r.I(0), r.Nil()})),
             r.set(i, r.I(0)),
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
    r.add(r.iff(r.isnt(r.typ(a), "array"), r.ret(r.L(out))));
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
    add_for_each(r, it, st, r.call("$iter", {r.L(src)}),
                   {r.push(r.L(dst), value_of(r, st))});
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
    r.add(r.iff(r.is(r.typ(k), "nil"), r.ret(r.Bo(false))));
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
    r.add(r.iff(r.is(r.typ(k), "nil"), r.ret(r.L(out))));
    r.add(r.set(ks, r.in(IntrinsicId::ObjectKeys, {r.L(k)})));
    r.add(r.set(i, r.I(0)));
    r.add(
        r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(ks))),
             r.blk({r.set(key, r.idx(r.L(ks), r.L(i))),
                    r.iff(r.b.unary(UnOp::Not,
                                    r.call("$hasname", {r.L(names), r.L(key)})),
                          r.sidx(r.L(out), r.L(key), r.idx(r.L(k), r.L(key)))),
                    r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$kwrest");
  }

  void rt_kwcheck() {  // no `**kwargs`: an unexpected keyword is a TypeError
    RT r(*this);
    const auto [k, names, fn] = r.params("k", "names", "fn");
    const auto [ks, i, key] = r.locals("ks", "i", "key");
    r.add(r.iff(r.is(r.typ(k), "nil"), r.ret(r.Nil())));
    r.add(r.set(ks, r.in(IntrinsicId::ObjectKeys, {r.L(k)})));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(ks))),
        r.blk({r.set(key, r.idx(r.L(ks), r.L(i))),
               r.iff(r.b.unary(UnOp::Not,
                               r.call("$hasname", {r.L(names), r.L(key)})),
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
    r.add(r.iff(r.is(r.typ(src), "nil"), r.ret(r.L(dst))));
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
  // `what` is "positional" or "keyword-only", and a keyword-only
  // parameter's position is -1: no positional argument reaches it.
  void rt_missing() {
    RT r(*this);
    const auto [fn, what, names, idxs, a, k] =
        r.params("fn", "what", "names", "idxs", "a", "k");
    const auto [miss, i, n, c, s] = r.locals("miss", "i", "n", "c", "s");
    // Every required parameter no positional and no keyword answered.
    r.add(r.set(miss, r.arr({})));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(names))),
        r.blk(
            {r.set(n, r.idx(r.L(names), r.L(i))),
             r.iff(r.b.unary(
                       UnOp::Not,
                       r.either(r.both(r.bin(BinOp::Ge,
                                             r.idx(r.L(idxs), r.L(i)), r.I(0)),
                                       r.bin(BinOp::Gt, r.len(r.L(a)),
                                             r.idx(r.L(idxs), r.L(i)))),
                                r.call("$kwhas", {r.L(k), r.L(n)}))),
                   r.push(r.L(miss),
                          r.bin(BinOp::Add, r.bin(BinOp::Add, r.S("'"), r.L(n)),
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
                           r.bin(BinOp::Add,
                                 r.bin(BinOp::Add, r.S(" required "), r.L(what)),
                                 r.iff(r.bin(BinOp::Eq, r.L(c), r.I(1)),
                                       r.S(" argument: "), r.S(" arguments: "))),
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
    find_in_chain(r);
    r.finish("$clsfind");
  }

  // A builtin exception is not a class here, so `except ValueError` matches
  // by name -- and a class of the program's own that derives from one is
  // marked with the name it was rooted at, which is what this also finds.
  void rt_isname() {
    RT r(*this);
    const auto [v, name] = r.params("v", "name");
    const auto [t] = r.locals("t");
    r.add(r.iff(r.isnt(r.typ(v), "object"), r.ret(r.Bo(false))));
    r.add(r.iff(is_kind(r, v, kExcKind),
                r.ret(r.bin(BinOp::Eq, kind_field(r, r.L(v), kExcName),
                            r.L(name)))));
    r.add(r.set(t, instance_class(r, v)));
    r.add(walk_bases(
        r, t,
        r.iff(r.both(r.is(r.typ(kind_field(r, r.L(t), kClassRoot)), "string"),
                     r.bin(BinOp::Eq, kind_field(r, r.L(t), kClassRoot),
                           r.L(name))),
              r.ret(r.Bo(true)))));
    r.add(r.ret(r.Bo(false)));
    r.finish("$isname");
  }

  // The identity walk: a class value *is* its constructor closure, so two
  // classes are the same class when `Same` says the closures are.
  void rt_isinstv() {
    RT r(*this);
    const auto [v, cls] = r.params("v", "cls");
    const auto [t] = r.locals("t");
    r.add(r.set(t, instance_class(r, v)));
    r.add(walk_bases(r, t,
                     r.iff(r.in(IntrinsicId::Same,
                                {kind_field(r, r.L(t), kClassId), r.L(cls)}),
                           r.ret(r.Bo(true)))));
    r.add(r.ret(r.Bo(false)));
    r.finish("$isinstv");
  }

  void rt_isinst() {
    RT r(*this);
    const auto [v, cls] = r.params("v", "cls");
    r.add(r.iff(r.is(r.typ(cls), "string"),
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
    r.add(r.iff(r.is(r.typ(base), "object"),
                r.blk({r.set(f, r.call("$clsfind", {r.L(base), r.L(key)})),
                       r.iff(r.isnt(r.typ(f), "nil"),
                             r.ret(r.b.call_value(
                                 r.L(f), {r.call("$acons", {r.L(self), r.L(a)}),
                                          r.L(k)})))})));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(key), r.S("__init__")),
                r.ret(r.call("$excinit", {r.L(self), r.L(a)}))));
    r.add(r.ret(r.call(
        "$exc", {r.S("AttributeError"),
                 r.bin(BinOp::Add, r.S("'super' object has no attribute "),
                       r.L(key))})));
    r.finish("$supercall");
  }

  // What str() of an exception made with these arguments says, as
  // BaseException.__str__ does: nothing for none, the one argument, or the
  // tuple of them all.
  void rt_excmsg() {
    RT r(*this);
    const auto [a] = r.params("a");
    r.add(r.iff(r.bin(BinOp::Eq, r.len(r.L(a)), r.I(0)), r.ret(r.S(""))));
    r.add(r.iff(r.bin(BinOp::Eq, r.len(r.L(a)), r.I(1)),
                r.ret(r.idx(r.L(a), r.I(0)))));
    r.add(r.ret(r.call("$str", {r.call("$tuple", {r.L(a)})})));
    r.finish("$excmsg");
  }

  void rt_excinit() {
    RT r(*this);
    const auto [self, a] = r.params("self", "a");
    // Only an exception has a message to keep: `super().__init__()` from a
    // class with no base reaches here too.
    r.add(r.iff(is_exception(r, self),
                set_kind_field(r, r.L(self), kExcMessage,
                               r.call("$excmsg", {r.L(a)}))));
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
    r.add(r.ret(r.call("$clsfind", {instance_class(r, v), r.L(key)})));
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
    r.add(r.ret(of_kind(r, r.obj({}), kTupleKind, {r.L(a)})));
    r.finish("$tuple");
  }

  void rt_untup() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.iff(r.call("$istup", {r.L(v)}),
                r.ret(kind_field(r, r.L(v), kTupleItems))));
    r.add(r.ret(r.L(v)));
    r.finish("$untup");
  }

  void rt_istup() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.ret(is_kind(r, v, kTupleKind)));
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
  // Too many is reported with the count only for a list, a tuple or a
  // dict: the exact types CPython 3.14 counts, not everything with a len.
  void rt_unpack() {
    RT r(*this);
    const auto [v, n] = r.params("v", "n");
    const auto [a] = r.locals("a");
    const auto cat = [&](NodeId x, NodeId y) {
      return r.bin(BinOp::Add, x, y);
    };
    const auto expected = [&](const char* what) {
      return cat(r.S(std::string(what) + " values to unpack (expected "),
                 r.call("$str", {r.L(n)}));
    };
    const auto got = [&] {
      return cat(r.S(", got "), cat(r.call("$str", {r.len(r.L(a))}), r.S(")")));
    };
    const auto sized = r.either(
        r.is(r.typ(v), "array"),
        r.either(r.is(r.typ(v), "map"), r.call("$istup", {r.L(v)})));
    r.add(r.set(a, r.call("$tolist", {r.L(v)})));
    r.add(r.iff(r.bin(BinOp::Lt, r.len(r.L(a)), r.L(n)),
                r.call("$exc", {r.S("ValueError"),
                                cat(expected("not enough"), got())})));
    r.add(r.iff(r.bin(BinOp::Gt, r.len(r.L(a)), r.L(n)),
                r.call("$exc", {r.S("ValueError"),
                                cat(expected("too many"),
                                    r.iff(sized, got(), r.S(")")))})));
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
                      r.iff(r.is(r.typ(key), "nil"), r.idx(r.L(a), r.L(i)),
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
    r.add(r.iff(done_of(r, st),
                r.blk({r.iff(r.L(hasdflt), r.ret(r.L(dflt))),
                       r.call("$exc", {r.S("StopIteration"), r.S("")})})));
    r.add(r.ret(value_of(r, st)));
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
        r.b.unary(UnOp::Not, r.L(hassep)),
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

  // mode 0 both, 1 left, 2 right; `chars` the characters to strip, or
  // None for whitespace.
  void rt_strip() {
    RT r(*this);
    const auto [s, mode, chars] = r.params("s", "mode", "chars");
    const auto [i, j] = r.locals("i", "j");
    const auto ch = [&](NodeId i) {
      return r.in(IntrinsicId::StrSlice,
                  {r.L(s), i, r.bin(BinOp::Add, i, r.I(1))});
    };
    const auto strips = [&](NodeId c) {
      return r.iff(r.is(r.typ(chars), "nil"), r.call("$isspace", {c}),
                   r.call("$in", {c, r.L(chars)}));
    };
    r.add(r.iff(r.both(r.isnt(r.typ(chars), "nil"),
                       r.isnt(r.typ(chars), "string")),
                r.call("$exc", {r.S("TypeError"),
                                r.S("strip arg must be None or str")})));
    r.add(r.set(i, r.I(0)));
    r.add(r.set(j, r.len(r.L(s))));
    r.add(r.iff(r.bin(BinOp::Ne, r.L(mode), r.I(2)),
                r.wh(r.both(r.bin(BinOp::Lt, r.L(i), r.L(j)), strips(ch(r.L(i)))),
                     r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1))))));
    r.add(r.iff(r.bin(BinOp::Ne, r.L(mode), r.I(1)),
                r.wh(r.both(r.bin(BinOp::Gt, r.L(j), r.L(i)),
                            strips(ch(r.bin(BinOp::Sub, r.L(j), r.I(1))))),
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
    r.add(r.iff(r.b.unary(UnOp::Not, r.L(hasi)),
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

  // The first index holding a value equal to v, or -1.
  void rt_afind() {
    RT r(*this);
    const auto [a, v] = r.params("a", "v");
    const auto [i] = r.locals("i");
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(a))),
               r.blk({r.iff(r.call("$eq", {r.idx(r.L(a), r.L(i)), r.L(v)}),
                            r.ret(r.L(i))),
                      r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.I(-1)));
    r.finish("$afind");
  }

  // `list.index(x)` and `list.remove(x)` search the same way and each
  // names itself when x is absent.
  void rt_aindex() {
    RT r(*this);
    const auto [a, v] = r.params("a", "v");
    const auto [i] = r.locals("i");
    r.add(r.set(i, r.call("$afind", {r.L(a), r.L(v)})));
    r.add(r.iff(r.bin(BinOp::Lt, r.L(i), r.I(0)),
                r.call("$exc", {r.S("ValueError"),
                                r.S("list.index(x): x not in list")})));
    r.add(r.ret(r.L(i)));
    r.finish("$aindex");
  }

  void rt_aremove() {
    RT r(*this);
    const auto [a, v] = r.params("a", "v");
    const auto [i] = r.locals("i");
    r.add(r.set(i, r.call("$afind", {r.L(a), r.L(v)})));
    r.add(r.iff(r.bin(BinOp::Lt, r.L(i), r.I(0)),
                r.call("$exc", {r.S("ValueError"),
                                r.S("list.remove(x): x not in list")})));
    r.add(r.call("$apop", {r.L(a), r.L(i), r.Bo(true)}));
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
    r.add(r.iff(r.is(r.typ(c), "map"),
                r.blk({r.iff(r.b.unary(UnOp::Not, r.has(r.L(c), r.L(k))),
                             r.call("$exc", {r.S("KeyError"),
                                             r.call("$repr", {r.L(k)})})),
                       r.in(IntrinsicId::ObjectRemove, {r.L(c), r.L(k)}),
                       r.ret(r.Nil())})));
    r.add(r.iff(r.is(r.typ(c), "array"),
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
    r.add(r.set(align, r.iff(r.either(r.is(r.typ(v), "int"),
                                      r.either(r.is(r.typ(v), "double"),
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
               r.iff(r.b.unary(UnOp::Not, r.call("$alldigits", {r.L(t)})),
                     r.call("$specerr", {r.L(spec)})),
               r.set(prec, r.call("$toint", {r.L(t)})),
               r.set(rest, r.in(IntrinsicId::StrSlice,
                                {r.L(rest), r.I(0), r.L(dot)}))})));
    r.add(r.iff(r.both(r.bin(BinOp::Gt, r.len(r.L(rest)), r.I(0)),
                       r.b.unary(UnOp::Not, r.call("$alldigits", {r.L(rest)}))),
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
    rt_bigmul(); rt_bstr(); rt_tofloat(); rt_neg(); rt_pyabs();
    rt_add(); rt_sub(); rt_mul(); rt_fdiv(); rt_idiv(); rt_mod(); rt_pow();
    rt_cmp(); rt_eq(); rt_truthy();
    rt_order("$lt", BinOp::Lt); rt_order("$le", BinOp::Le);
    rt_order("$gt", BinOp::Gt); rt_order("$ge", BinOp::Ge);
    rt_fstr(); rt_str(); rt_strrepr(); rt_repr(); rt_liststr(); rt_dictstr();
    rt_len(); rt_idx(); rt_setidx(); rt_slice(); rt_in(); rt_getattr();
    rt_setattr(); rt_iter(); rt_iternext(); rt_exc(); rt_isexc(); rt_range();
    rt_listadd(); rt_strmul(); rt_listmul();
    rt_acons(); rt_aext(); rt_rest(); rt_kwhas(); rt_hasname(); rt_kwrest();
    rt_kwcheck(); rt_kwmerge(); rt_missing(); rt_toomany();
    rt_clsfind(); rt_isname(); rt_isinstv(); rt_isinst(); rt_supercall();
    rt_excmsg(); rt_excinit(); rt_noinit(); rt_dunder();
    rt_tuple(); rt_untup(); rt_istup(); rt_tupstr(); rt_unpack(); rt_items();
    rt_values(); rt_enumerate(); rt_zip(); rt_sorted(); rt_sum();
    rt_minmax(); rt_next();
    rt_isspace(); rt_split(); rt_strip(); rt_replace(); rt_find();
    rt_scount(); rt_startswith(); rt_endswith(); rt_apop(); rt_ainsert();
    rt_afind(); rt_aindex(); rt_aremove(); rt_acount(); rt_areverse();
    rt_asort(); rt_dpop(); rt_delitem(); rt_alldigits(); rt_specerr(); rt_fmt();
  }

  // ==== Pass B: emit ======================================================

  // A value used as a condition: $truthy's entry in operators().
  NodeId truthy(NodeId v, SrcPos p) { return ops.call("$truthy", {v}, p); }

  NodeId native(const std::string& name, const std::vector<NodeId>& args,
                SrcPos p) {
    auto b = Builder(m).at(p);
    return b.call_value(b.native_ref(b.declare_native(name)), args);
  }

  NodeId emit_closure(int32_t g, FnCtx& ctx, SrcPos p) {
    return rs.closure(m, ctx.fn, g, p);
  }

  NodeId read_var(int32_t v, FnCtx& ctx, SrcPos p) {
    return rs.read(m, ctx.fn, v, p);
  }

  NodeId write_var(int32_t v, NodeId value, FnCtx& ctx, SrcPos p) {
    return rs.write(m, ctx.fn, v, value, p);
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
    return rt.call("$tuple", {b.array_lit(items)}, p);
  }

  // -- A builtin's arguments ----------------------------------------------
  //
  // A builtin is emitted inline, by name, rather than reached through the
  // calling convention, so its arguments are emitted here: each once, in
  // source order -- which, with no `*` or `**` among them (refused when
  // the program is bound), is the positional ones and then the keywords.
  struct Given {
    std::vector<NodeId> pos;
    std::vector<std::pair<std::string, NodeId>> kw;
  };
  Given emit_given(const Ast* args, FnCtx& ctx) {
    Given g;
    if (args == nullptr || args->tag != "args"_) return g;
    for (const auto& c : args->nodes) {
      if (c->tag == "splat"_ || c->tag == "kwsplat"_) {
        fail(*c, "a builtin takes no * or ** argument here");
      }
      if (c->tag == "kwarg"_) {
        g.kw.emplace_back(std::string(c->nodes[0]->token),
                          emit_expr(*c->nodes[1], ctx));
      } else {
        g.pos.push_back(emit_expr(*c, ctx));
      }
    }
    return g;
  }

  // What a builtin takes, and how CPython words a count it refuses -- which
  // depends on how the builtin parses its arguments, not on what it is:
  //   One     "len() takes exactly one argument (2 given)"
  //   NoArgs  "str.upper() takes no arguments (1 given)"
  //   Clinic  "pop expected at most 1 argument, got 2"
  //   Old     "split() takes at most 2 arguments (3 given)"
  // `min`..`max` positional arguments are what this front end implements;
  // `py_max` is CPython's own top (npos: any number), and a count between
  // the two is refused as unsupported rather than as Python's TypeError,
  // which it would not be. `kw` names the keywords taken, in the order
  // `make` wants them; `py_kw` any more that CPython takes, `py_any_kw`
  // that it takes any at all. `few` and `many` replace the wording outright
  // for the few builtins with a sentence of their own.
  enum class Shape { One, NoArgs, Clinic, Old };
  struct Takes {
    size_t min = 0;
    size_t max = 0;
    size_t py_max = 0;
    Shape shape = Shape::Clinic;
    std::vector<std::string> kw = {};
    std::vector<std::string> py_kw = {};
    bool py_any_kw = false;
    std::string few = {};
    std::string many = {};
    std::string kw_owner = {};  // the name a keyword error gives, if not its own
  };

  static std::string plural(size_t n, const std::string& word) {
    return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
  }

  // `make` over a builtin's arguments, when it takes them: the positional
  // ones as given, then one entry per keyword `t.kw` names -- invalid where
  // the call did not pass it. Every argument runs once, in source order,
  // whatever order `make` reads them in: when the keywords came in another
  // order, they all go into fresh temporaries first. A count or a keyword
  // the builtin refuses runs them all and raises CPython's TypeError, and
  // one CPython takes but this front end does not implement raises
  // NotImplementedError. `qual` is CPython's name for it, `list.append` or
  // `len`.
  NodeId takes(const Given& g, const Takes& t, const std::string& qual,
               FnCtx& ctx, SrcPos p,
               const std::function<NodeId(const std::vector<NodeId>&,
                                          const std::vector<NodeId>&)>& make) {
    auto b = Builder(m).at(p);
    const std::string name = qual.substr(qual.rfind('.') + 1);
    const auto refuse = [&](const std::string& kind, const std::string& msg) {
      std::vector<NodeId> run(g.pos);
      for (const auto& [k, v] : g.kw) run.push_back(v);
      run.push_back(
          rt.call("$exc", {b.str_literal(kind), b.str_literal(msg)}, p));
      return b.block(run);
    };
    const auto has = [](const std::vector<std::string>& v, const std::string& k) {
      return std::find(v.begin(), v.end(), k) != v.end();
    };
    for (const auto& [k, v] : g.kw) {
      if (has(t.kw, k)) continue;
      if (t.py_any_kw || has(t.py_kw, k)) {
        return refuse("NotImplementedError",
                      name + "() with the keyword '" + k +
                          "' is not supported here");
      }
      if (t.kw.empty() && t.py_kw.empty()) {
        return refuse("TypeError", qual + "() takes no keyword arguments");
      }
      return refuse("TypeError",
                    "'" + k + "' is an invalid keyword argument for " +
                        (t.kw_owner.empty() ? name : t.kw_owner) + "()");
    }
    const size_t n = g.pos.size();
    if (n < t.min || n > t.py_max) {
      if (n < t.min && !t.few.empty()) return refuse("TypeError", t.few);
      if (n > t.py_max && !t.many.empty()) return refuse("TypeError", t.many);
      if (t.max == 0 && t.py_max == 0 && !t.kw.empty()) {
        return refuse("TypeError", name + "() takes no positional arguments");
      }
      const bool over = n > t.py_max;
      const size_t lim = over ? t.py_max : t.min;
      std::string msg;
      switch (t.shape) {
        case Shape::One:
          msg = qual + "() takes exactly one argument (" + std::to_string(n) +
                " given)";
          break;
        case Shape::NoArgs:
          msg = qual + "() takes no arguments (" + std::to_string(n) +
                " given)";
          break;
        case Shape::Clinic:
          msg = name + " expected " +
                (t.min == t.py_max ? "" : over ? "at most " : "at least ") +
                plural(lim, "argument") + ", got " + std::to_string(n);
          break;
        case Shape::Old:
          msg = name + "() takes " + (over ? "at most " : "at least ") +
                plural(lim, "argument") + " (" + std::to_string(n) + " given)";
          break;
      }
      return refuse("TypeError", msg);
    }
    if (n > t.max) {
      return refuse("NotImplementedError",
                    name + "() with " + plural(n, "argument") +
                        " is not supported here");
    }
    // Where each keyword goes, and whether they came in that order.
    std::vector<NodeId> kwv(t.kw.size());
    std::vector<size_t> at;
    for (const auto& [k, v] : g.kw) {
      const size_t i = static_cast<size_t>(
          std::find(t.kw.begin(), t.kw.end(), k) - t.kw.begin());
      kwv[i] = v;
      at.push_back(i);
    }
    if (std::is_sorted(at.begin(), at.end())) return make(g.pos, kwv);
    std::vector<NodeId> pre;
    std::vector<NodeId> pos;
    const auto spill = [&](NodeId v) {
      const int32_t s = ctx.alloc_fresh_local("$arg");
      pre.push_back(b.assign(VarKind::Local, s, v));
      return b.varref(VarKind::Local, s);
    };
    for (const NodeId v : g.pos) pos.push_back(spill(v));
    for (size_t j = 0; j < g.kw.size(); ++j) kwv[at[j]] = spill(g.kw[j].second);
    return b.sequence(pre, make(pos, kwv));
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
                ? rt.call("$aext", {T, emit_expr(*e->nodes[0], ctx)}, p)
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
                ? rt.call("$kwmerge", {T, emit_expr(*e->nodes[0], ctx)}, p)
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
        // `@a @b def f` is `f = a(b(f))`: the decorator expressions run
        // first, top to bottom, then the defaults, and the nearest
        // decorator is applied first.
        std::vector<NodeId> decorators;
        if (a.nodes[0]->tag == "decorators"_) {
          for (const auto& d : a.nodes[0]->nodes) {
            const int32_t t = ctx.alloc_local("$dec");
            out.push_back(
                b.assign(VarKind::Local, t, emit_expr(*d->nodes[0], ctx)));
            decorators.push_back(b.varref(VarKind::Local, t));
          }
        }
        emit_defaults(g, ctx, out, p);
        NodeId value = emit_closure(g, ctx, p);
        for (size_t k = decorators.size(); k-- > 0;) {
          value = b.call_value(decorators[k],
                               {b.array_lit({value}), b.nil_literal()});
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
        return for_each(rt, p, it, st,
                        rt.call("$iter", {emit_expr(*a.nodes[0], ctx)}, p),
                        {b.make_yield(value_of(b, st))});
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
            b.unary(UnOp::Not, truthy(emit_expr(*a.nodes[0], ctx), p)),
            rt.call("$exc",
                    {b.str_literal("AssertionError"),
                     a.nodes.size() > 1
                         ? rt.call("$str", {emit_expr(*a.nodes[1], ctx)}, p)
                         : b.str_literal("")},
                    p),
            NodeId{});
      case "exprstmt"_:
        return emit_expr(*a.nodes[0], ctx);
      case "ifstmt"_:
        return emit_if(a, ctx);
      case "whilestmt"_:
        return b.make_while(
            truthy(emit_expr(*a.nodes[0], ctx), p),
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
    return rt.call("$delitem",
                   {emit_postfix(t, t.nodes.size() - 1, ctx),
                    emit_expr(sub, ctx)},
                   p);
  }

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
      if (c.tag == "elifpart"_) {
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

  NodeId emit_for(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const int32_t it = ctx.alloc_local("$it");
    const int32_t st = ctx.alloc_local("$step");
    const Ast& tg = *a.nodes[0];
    std::vector<NodeId> loop;
    const NodeId value = value_of(b, st);
    if (tg.nodes.size() == 1) {
      loop.push_back(write_var(decl_of.at(tg.nodes[0].get()), value, ctx, p));
    } else {
      // `for k, v in d.items()`: the same unpack an assignment does.
      const int32_t u = ctx.alloc_local("$unp");
      const NodeId U = b.varref(VarKind::Local, u);
      loop.push_back(b.assign(
          VarKind::Local, u,
          rt.call("$unpack",
                  {value, b.literal(static_cast<int64_t>(tg.nodes.size()))},
                  p)));
      for (size_t k = 0; k < tg.nodes.size(); ++k) {
        loop.push_back(write_var(decl_of.at(tg.nodes[k].get()),
                                 b.index(U, b.literal(static_cast<int64_t>(k))),
                                 ctx, p));
      }
    }
    loop.push_back(emit_block(*a.nodes[2], ctx));
    return for_each(rt, p, it, st,
                    rt.call("$iter", {emit_expr(*a.nodes[1], ctx)}, p), loop);
  }

  // try/except/finally. `finally` is a Defer inside the Scope wrapping the
  // try, so it runs however the block is left; `except NAME` re-raises
  // what it does not match, which is how the selective behaviour comes
  // out right without a handler table.
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
    // The clauses are tried in order, one nested If per clause that names
    // what it catches. What none of them claims goes to a bare `except:`,
    // which Python allows only last, or else is re-thrown.
    const auto is_bare = [](const Ast& ex) {
      return ex.nodes[0]->tag == "block"_;
    };
    const auto clause = [&](const Ast& ex) {
      std::vector<NodeId> hs;
      if (!is_bare(ex) && ex.nodes.size() > 2) {
        hs.push_back(write_var(decl_of.at(ex.nodes[1].get()), E, ctx, p));
      }
      hs.push_back(emit_block(*ex.nodes.back(), ctx));
      return b.block(hs);
    };
    size_t k = excs.size();
    NodeId handler = k > 0 && is_bare(*excs[k - 1]) ? clause(*excs[--k])
                                                    : b.make_throw(E);
    while (k-- > 0) {
      const Ast& ex = *excs[k];
      if (is_bare(ex)) fail(ex, "default 'except:' must be last");
      const NodeId then = clause(ex);
      // A builtin exception is not a class here, so it travels as its
      // name; a class of the program's own travels as its value.
      const Ast& caught = *ex.nodes[0];
      const NodeId cls = ref_of.count(&caught)
                             ? read_var(ref_of.at(&caught), ctx, p)
                             : b.str_literal(std::string(caught.token));
      handler = b.make_if(rt.call("$isexc", {E, cls}, p), then, handler);
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
        b.call_value(rt.call("$getattr", {C, b.str_literal("__enter__")}, p),
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
            rt.call("$getattr", {C, b.str_literal("__exit__", p)}, p),
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
          rt.call("$unpack",
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
    // `cur` reads the target's present value; `pure` says reading it twice
    // is free of effects, which a variable's is and a subscript's is not.
    const auto combine = [&](const std::function<NodeId()>& cur,
                             bool pure) -> NodeId {
      const NodeId v = emit_exprs(&values, ctx, p);
      if (op == "=") return v;
      // `a += b` on a list extends it in place from any iterable and it
      // stays the same list (list.__iadd__), which every other name for it
      // sees; anything else is `a + b`. Tested here rather than in a
      // helper of its own, which would put a second call on every
      // `n += 1`.
      if (op == "+=") {
        std::vector<NodeId> pre;
        std::function<NodeId()> now = cur;
        if (!pure) {
          const int32_t c = ctx.alloc_local("$cur");
          pre.push_back(b.assign(VarKind::Local, c, cur()));
          now = [&b, c] { return b.varref(VarKind::Local, c); };
        }
        return b.sequence(
            pre, b.make_if(b.binary(BinOp::Eq,
                                    b.intrinsic(IntrinsicId::TypeOf, {now()}),
                                    b.str_literal("array")),
                           rt.call("$aext", {now(), v}, p),
                           ops.call("$add", {now(), v}, p)));
      }
      if (op == "-=") return ops.call("$sub", {cur(), v}, p);
      if (op == "*=") return ops.call("$mul", {cur(), v}, p);
      if (op == "//=") return ops.call("$idiv", {cur(), v}, p);
      if (op == "/=") return rt.call("$fdiv", {cur(), v}, p);
      return ops.call("$mod", {cur(), v}, p);
    };

    if (target.tag == "ident"_) {
      const auto it = decl_of.find(&target);
      const int32_t v = it != decl_of.end() ? it->second : ref_of.at(&target);
      return write_var(
          v, combine([&] { return read_var(v, ctx, p); }, true), ctx, p);
    }
    if (target.tag != "postfix"_ || target.nodes.size() < 2) {
      fail(target, "cannot assign to this expression");
    }
    const Ast& last = *target.nodes.back();
    const bool attr = last.tag == "dotsfx"_;
    if (!attr && last.tag != "indexsfx"_) {
      fail(last, "cannot assign to this expression");
    }
    // `t[k] = v` runs v first, then t and k (Python's assignment statement:
    // the value, then the target); `t[k] += v` reads t[k] before v runs.
    if (op == "=") {
      const int32_t tv = ctx.alloc_local("$val");
      return b.block(
          {b.assign(VarKind::Local, tv, emit_exprs(&values, ctx, p)),
           emit_store(target, b.varref(VarKind::Local, tv), ctx, p)});
    }
    const NodeId key = attr ? b.str_literal(std::string(last.nodes[0]->token))
                            : emit_expr(*last.nodes[0], ctx);
    const int32_t tr = ctx.alloc_local("$recv");
    const int32_t tk = ctx.alloc_local("$key");
    const NodeId recv = emit_postfix(target, target.nodes.size() - 1, ctx);
    const NodeId R = b.varref(VarKind::Local, tr);
    const NodeId K = b.varref(VarKind::Local, tk);
    const auto cur = [&] {
      return rt.call(attr ? "$getattr" : "$idx", {R, K}, p);
    };
    return b.block(
        {b.assign(VarKind::Local, tr, recv), b.assign(VarKind::Local, tk, key),
         rt.call(attr ? "$setattr" : "$setidx", {R, K, combine(cur, false)},
                 p)});
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
    return rt.call(attr ? "$setattr" : "$setidx",
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
    std::vector<std::pair<NodeId, NodeId>> kvs;
    for (const auto& [name, g] : methods) {
      kvs.emplace_back(b.str_literal(name), emit_closure(g, ctx, p));
    }
    // The identity is filled in below, once the closure that is the class
    // value exists.
    const NodeId table = class_table(
        b, b.object_lit(kvs), cname,
        ci.base_var >= 0 ? read_var(ci.base_var, ctx, p) : b.nil_literal(),
        {ci.root.empty() ? b.nil_literal() : b.str_literal(ci.root),
         b.nil_literal()});
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
    out.push_back(b.assign(VarKind::Cell, cell, table));
    // The class value is its constructor closure, and the table keeps a
    // reference to it: that is the identity `isinstance` and `except`
    // compare, since there is nothing else a class could be.
    const int32_t t = ctx.alloc_local("$class");
    const NodeId K = b.varref(VarKind::Local, t);
    out.push_back(
        b.assign(VarKind::Local, t,
                 b.make_closure(rs.fns[static_cast<size_t>(ctor)].index, cm)));
    out.push_back(set_kind_field(b, T, kClassId, K));
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
    // `__init__` may be the base's, so it is looked up rather than indexed.
    // An exception of the program's keeps a message beside its class, so
    // its meta is its kind record, naming the class.
    auto bp = b.at(p);
    std::vector<NodeId> body{
        b.assign(VarKind::Local, 2,
                 is_exc ? exception(bp, C, bp.nil_literal(), bp.nil_literal())
                        : new_instance(bp, C),
                 p),
        b.assign(VarKind::Local, 3,
                 rt.call("$clsfind", {C, b.str_literal("__init__", p)}, p),
                 p),
        b.make_if(
            b.binary(BinOp::Ne,
                     b.intrinsic(IntrinsicId::TypeOf, {F}, p),
                     b.str_literal("nil", p), p),
            b.call_value(F, {rt.call("$acons", {O, A}, p), K}, p),
            is_exc ? rt.call("$excinit", {O, A}, p)
                   : rt.call("$noinit", {b.str_literal(cname, p), A}, p),
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
          return b.make_closure(builtin_func(g), rt.empty_cmap());
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
            if (as_repr) v = rt.call("$repr", {v}, p);
            piece = rt.call("$fmt", {v, b.str_literal(spec)}, p);
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
        // The first iterable, as an iterator, is the one argument (see
        // resolve_comp).
        return b.call_value(
            emit_closure(fn_of.at(&a), ctx, p),
            {b.array_lit({rt.call(
                 "$iter", {emit_expr(*first_for(a).nodes[1], ctx)}, p)}),
             b.nil_literal()});
      case "tuplelit"_: {
        std::vector<NodeId> items;
        for (const auto& c : a.nodes) items.push_back(emit_expr(*c, ctx));
        return rt.call("$tuple", {b.array_lit(items)}, p);
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
        return ops.call("$neg", {emit_expr(*a.nodes[0], ctx)}, p);
      case "notop"_:
        return b.unary(UnOp::Not, truthy(emit_expr(*a.nodes[0], ctx), p));
      case "powexp"_:
        return rt.call("$pow",
                       {emit_expr(*a.nodes[0], ctx),
                        emit_expr(*a.nodes[1]->nodes[0], ctx)},
                       p);
      case "ternary"_:
        // `a if c else b`: the condition is the middle child.
        return b.make_if(truthy(emit_expr(*a.nodes[1], ctx), p),
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
                         b.make_if(truthy(keep, p),
                                   is_or ? keep : rhs, is_or ? rhs : keep)});
        }
        return acc;
      }
      // Comparisons *chain*: `a < b <= c` means `a < b and b <= c` with
      // `b` evaluated once, which no left fold can express -- the fold
      // would compare a bool with c. Each operand lands in a slot, and
      // each operand after the second runs only once the link before it
      // held, so `1 > 2 < f()` never calls f: the short-circuit is
      // Python's too.
      case "cmpexp"_: {
        const size_t links = (a.nodes.size() - 1) / 2;
        std::vector<int32_t> slots;
        std::vector<NodeId> sets;
        for (size_t k = 0; k < a.nodes.size(); k += 2) {
          const int32_t t = ctx.alloc_local("$cmp");
          slots.push_back(t);
          sets.push_back(
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
          if (t == "==") return ops.call("$eq", {lhs, rhs}, op_p);
          if (t == "!=") {
            return b.at(op_p).unary(UnOp::Not,
                                    ops.call("$eq", {lhs, rhs}, op_p));
          }
          // `is` is identity, which `Same` answers.
          if (t == "is" || t == "is not") {
            const NodeId same =
                b.at(op_p).intrinsic(IntrinsicId::Same, {lhs, rhs});
            return t == "is" ? same : b.at(op_p).unary(UnOp::Not, same);
          }
          if (t == "in") return rt.call("$in", {lhs, rhs}, op_p);
          if (t == "not in") {
            return b.at(op_p).unary(UnOp::Not,
                                    rt.call("$in", {lhs, rhs}, op_p));
          }
          const char* h = t == "<"    ? "$lt"
                          : t == "<=" ? "$le"
                          : t == ">"  ? "$gt"
                                      : "$ge";
          return ops.call(h, {lhs, rhs}, op_p);
        };
        // Link k compares operands k and k+1; operand k+1 is set just
        // before it, except the first two, which run before anything.
        NodeId chain = link(links - 1);
        for (size_t k = links - 1; k-- > 0;) {
          chain = b.make_if(link(k), b.block({sets[k + 2], chain}),
                            b.bool_literal(false));
        }
        return b.block({sets[0], sets[1], chain});
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
          acc = ops.call(h, {acc, rhs}, op_p);
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
    return of_kind(b, b.object_lit({}), kBigKind,
                   {b.array_lit(limbs), b.literal(1)});
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
      auto bp = b.at(pos_of(prim));
      const Ast& callnode = *a.nodes[3];
      const auto [A, K] = emit_callargs(
          callnode.nodes.empty() ? nullptr : callnode.nodes[0].get(), ctx, {},
          pos_of(prim));
      cur = rt.call(
          "$supercall",
          {read_var(params[0].var, ctx, pos_of(prim)),
           kind_field(bp, table, kClassBase),
           b.str_literal(std::string(a.nodes[2]->nodes[0]->token),
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
          cur = rt.call("$getattr", {cur, b.str_literal(name, p)}, p);
          break;
        }
        case "indexsfx"_: {
          const Ast& sub = *sfx.nodes[0];
          // Which side of the ':' an expression fell on is a *rule*, not
          // a position: `slicelo` and `slicehi` are separate rules for
          // exactly that reason, since one child says nothing on its own.
          if (sub.tag == "sliceboth"_) {
            cur = rt.call("$slice",
                          {cur, emit_expr(*sub.nodes[0], ctx),
                           emit_expr(*sub.nodes[1], ctx)},
                          p);
            break;
          }
          if (sub.tag == "slicelo"_) {
            cur = rt.call("$slice",
                          {cur, emit_expr(*sub.nodes[0], ctx),
                           b.nil_literal(p)},
                          p);
            break;
          }
          if (sub.tag == "slicehi"_) {
            cur = rt.call("$slice",
                          {cur, b.nil_literal(p),
                           emit_expr(*sub.nodes[0], ctx)},
                          p);
            break;
          }
          if (sub.tag == "sliceall"_) {
            cur = rt.call("$slice",
                          {cur, b.nil_literal(p), b.nil_literal(p)}, p);
            break;
          }
          cur = ops.call("$idx", {cur, emit_expr(sub, ctx)}, p);
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
           emit_pycall(rt.call("$getattr", {T, b.str_literal(name)}, p),
                       argnode, ctx, {T}, p)});
    }
    // The arguments run once, in source order, whichever method takes them.
    const Given g = emit_given(argnode, ctx);
    // A method of the program's own by this name takes them as any call
    // does, keywords and all.
    std::vector<NodeId> front{T};
    front.insert(front.end(), g.pos.begin(), g.pos.end());
    std::vector<std::pair<NodeId, NodeId>> kvs;
    for (const auto& [k, v] : g.kw) kvs.emplace_back(b.str_literal(k), v);
    const NodeId user =
        b.call_value(rt.call("$getattr", {T, b.str_literal(name)}, p),
                     {b.array_lit(front),
                      kvs.empty() ? b.nil_literal() : b.object_lit(kvs)});

    // Each builtin by this name, on the type that has it: what it takes,
    // and the call over what it was given (`pos`, then `kw` by `Takes::kw`).
    using Args = const std::vector<NodeId>&;
    std::vector<std::pair<const char*, NodeId>> cands;
    const auto add = [&](const char* want, const char* tname, const Takes& t,
                         const std::function<NodeId(Args, Args)>& make) {
      cands.emplace_back(
          want, takes(g, t, std::string(tname) + "." + name, ctx, p, make));
    };
    const auto h = [&](const char* n, std::vector<NodeId> as) {
      return rt.call(n, as, p);
    };
    const auto opt = [&](Args v, size_t k, NodeId dflt) {
      return k < v.size() && v[k].valid() ? v[k] : dflt;
    };
    const auto given = [&](Args v, size_t k) {
      return b.bool_literal(k < v.size() && v[k].valid());
    };
    const Takes one{1, 1, 1, Shape::One};
    const Takes noargs{0, 0, 0, Shape::NoArgs};
    const auto clinic = [](size_t lo, size_t hi, size_t py_hi) {
      return Takes{lo, hi, py_hi, Shape::Clinic};
    };
    const auto old = [](size_t lo, size_t hi, size_t py_hi) {
      return Takes{lo, hi, py_hi, Shape::Old};
    };
    if (name == "append") {
      add("array", "list", one, [&](Args a, Args) {
        return b.intrinsic(IntrinsicId::ArrayPush, {T, a[0]});
      });
    } else if (name == "extend") {
      add("array", "list", one, [&](Args a, Args) { return h("$aext", {T, a[0]}); });
    } else if (name == "pop") {
      add("array", "list", clinic(0, 1, 1), [&](Args a, Args) {
        return h("$apop", {T, opt(a, 0, b.nil_literal()), given(a, 0)});
      });
      add("map", "dict", clinic(1, 2, 2), [&](Args a, Args) {
        return h("$dpop", {T, a[0], opt(a, 1, b.nil_literal()), given(a, 1)});
      });
    } else if (name == "insert") {
      add("array", "list", clinic(2, 2, 2),
          [&](Args a, Args) { return h("$ainsert", {T, a[0], a[1]}); });
    } else if (name == "remove") {
      add("array", "list", one,
          [&](Args a, Args) { return h("$aremove", {T, a[0]}); });
    } else if (name == "index") {
      add("array", "list", clinic(1, 1, 3),
          [&](Args a, Args) { return h("$aindex", {T, a[0]}); });
    } else if (name == "count") {
      add("array", "list", one,
          [&](Args a, Args) { return h("$acount", {T, a[0]}); });
      add("string", "str", old(1, 1, 3),
          [&](Args a, Args) { return h("$scount", {T, a[0]}); });
    } else if (name == "reverse") {
      add("array", "list", noargs,
          [&](Args, Args) { return h("$areverse", {T}); });
    } else if (name == "sort") {
      Takes t{0, 0, 0, Shape::Clinic, {"key", "reverse"}};
      add("array", "list", t, [&](Args, Args kw) {
        return h("$asort", {T, opt(kw, 0, b.nil_literal()),
                            opt(kw, 1, b.bool_literal(false))});
      });
    } else if (name == "keys") {
      add("map", "dict", noargs, [&](Args, Args) {
        return b.intrinsic(IntrinsicId::ObjectKeys, {T});
      });
    } else if (name == "items") {
      add("map", "dict", noargs, [&](Args, Args) { return h("$items", {T}); });
    } else if (name == "values") {
      add("map", "dict", noargs, [&](Args, Args) { return h("$values", {T}); });
    } else if (name == "get") {
      add("map", "dict", clinic(1, 2, 2), [&](Args a, Args) {
        return h("$dget", {T, a[0], opt(a, 1, b.nil_literal())});
      });
    } else if (name == "update") {
      Takes t = clinic(0, 1, 1);
      t.py_any_kw = true;
      add("map", "dict", t, [&](Args a, Args) {
        return h("$kwmerge", {T, opt(a, 0, b.nil_literal())});
      });
    } else if (name == "split") {
      // `sep` by position or by name; `maxsplit` is not implemented.
      Takes t = old(0, 1, 2);
      t.kw = {"sep"};
      t.py_kw = {"maxsplit"};
      add("string", "str", t, [&](Args a, Args kw) {
        const NodeId sep = a.empty() ? opt(kw, 0, b.nil_literal()) : a[0];
        return h("$split",
                 {T, sep, b.bool_literal(!a.empty() || kw[0].valid())});
      });
    } else if (name == "strip" || name == "lstrip" || name == "rstrip") {
      const int64_t mode = name == "strip" ? 0 : name == "lstrip" ? 1 : 2;
      add("string", "str", clinic(0, 1, 1), [&](Args a, Args) {
        return h("$strip", {T, b.literal(mode), opt(a, 0, b.nil_literal())});
      });
    } else if (name == "replace") {
      add("string", "str", clinic(2, 2, 3),
          [&](Args a, Args) { return h("$replace", {T, a[0], a[1]}); });
    } else if (name == "find") {
      add("string", "str", old(1, 1, 3),
          [&](Args a, Args) { return h("$find", {T, a[0]}); });
    } else if (name == "startswith") {
      add("string", "str", old(1, 1, 3),
          [&](Args a, Args) { return h("$startswith", {T, a[0]}); });
    } else if (name == "endswith") {
      add("string", "str", old(1, 1, 3),
          [&](Args a, Args) { return h("$endswith", {T, a[0]}); });
    } else if (name == "upper") {
      add("string", "str", noargs, [&](Args, Args) {
        return b.intrinsic(IntrinsicId::StrUpper, {T});
      });
    } else if (name == "lower") {
      add("string", "str", noargs, [&](Args, Args) {
        return b.intrinsic(IntrinsicId::StrLower, {T});
      });
    } else {  // join
      add("string", "str", one, [&](Args a, Args) { return h("$join", {T, a[0]}); });
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
      return rt.call("$isinst", {emit_expr(what, ctx), cv}, p);
    }
    // Every other builtin: its arguments run once, in source order, and
    // each states what it takes beside the call it builds (see takes).
    const Given given =
        emit_given(call.nodes.empty() ? nullptr : call.nodes[0].get(), ctx);
    ++i;
    using Args = const std::vector<NodeId>&;
    const auto opt = [&](Args v, size_t k, NodeId dflt) {
      return k < v.size() && v[k].valid() ? v[k] : dflt;
    };
    const auto call_it = [&](const Takes& t,
                             const std::function<NodeId(Args, Args)>& make) {
      return takes(given, t, g, ctx, p, make);
    };
    const size_t any = std::string::npos;
    const Takes one{1, 1, 1, Shape::One};
    const auto clinic = [](size_t lo, size_t hi, size_t py_hi) {
      return Takes{lo, hi, py_hi, Shape::Clinic};
    };
    const auto old = [](size_t lo, size_t hi, size_t py_hi) {
      return Takes{lo, hi, py_hi, Shape::Old};
    };

    if (is_exception_name(g)) {
      return call_it(Takes{0, any, any}, [&](Args a, Args) {
        auto bp = b.at(p);
        return exception(bp, bp.str_literal(kExcKind), bp.str_literal(g),
                         rt.call("$excmsg", {bp.array_lit(a)}, p));
      });
    }
    if (g == "sorted") {
      Takes t{1, 1, 1, Shape::Clinic, {"key", "reverse"}};
      t.kw_owner = "sort";
      return call_it(t, [&](Args a, Args kw) {
        return rt.call("$sorted",
                       {a[0], opt(kw, 0, b.nil_literal(p)),
                        opt(kw, 1, b.bool_literal(false, p))},
                       p);
      });
    }
    if (g == "print") {
      Takes t{0, any, any};
      t.py_kw = {"sep", "end", "file", "flush"};
      return call_it(t, [&](Args a, Args) {
        std::vector<NodeId> parts;
        for (const NodeId v : a) parts.push_back(rt.call("$str", {v}, p));
        return native("print", {b.array_lit(parts, p)}, p);
      });
    }
    // A conversion called with nothing answers its type's empty value, made
    // only then: list's is a fresh array, which a call given an argument
    // would build and drop.
    const auto convert = [&](const Takes& t, const char* fn,
                             const std::function<NodeId()>& empty) {
      return call_it(t, [&, fn](Args a, Args) {
        return a.empty() ? empty() : rt.call(fn, {a[0]}, p);
      });
    };
    if (g == "len") {
      return call_it(one, [&](Args a, Args) {
        return rt.call("$len", {a[0]}, p);
      });
    }
    if (g == "str") {
      Takes t = old(0, 1, 3);
      t.py_kw = {"object", "encoding", "errors"};
      return convert(t, "$str", [&] { return b.str_literal("", p); });
    }
    if (g == "int") {
      Takes t = old(0, 1, 2);
      t.py_kw = {"base"};
      return convert(t, "$toint", [&] { return b.literal(0, p); });
    }
    if (g == "float") {
      return convert(clinic(0, 1, 1), "$tofloat",
                     [&] { return b.double_literal(0.0, p); });
    }
    if (g == "list") {
      return convert(clinic(0, 1, 1), "$tolist",
                     [&] { return b.array_lit({}, p); });
    }
    if (g == "bool") {
      return convert(clinic(0, 1, 1), "$truthy",
                     [&] { return b.bool_literal(false, p); });
    }
    if (g == "tuple") {
      return call_it(clinic(0, 1, 1), [&](Args a, Args) {
        return rt.call("$tuple",
                       {a.empty() ? b.array_lit({}, p)
                                  : rt.call("$tolist", {a[0]}, p)},
                       p);
      });
    }
    if (g == "abs") {
      return call_it(one, [&](Args a, Args) {
        return rt.call("$pyabs", {a[0]}, p);
      });
    }
    if (g == "repr") {
      return call_it(one, [&](Args a, Args) {
        return rt.call("$repr", {a[0]}, p);
      });
    }
    if (g == "type") {
      // type takes one argument or three, never two; type(name, bases,
      // dict) makes a class, which is not implemented.
      Takes t{1, 1, given.pos.size() == 3 ? size_t{3} : size_t{1}};
      t.few = t.many = "type() takes 1 or 3 arguments";
      return call_it(t, [&](Args a, Args) {
        return rt.call("$type", {a[0]}, p);
      });
    }
    if (g == "next") {
      return call_it(clinic(1, 2, 2), [&](Args a, Args) {
        return rt.call("$next",
                       {a[0], opt(a, 1, b.nil_literal(p)),
                        b.bool_literal(a.size() > 1, p)},
                       p);
      });
    }
    if (g == "enumerate") {
      Takes t = old(1, 2, 2);
      t.py_kw = {"iterable", "start"};
      t.few = "enumerate() missing required argument 'iterable'";
      return call_it(t, [&](Args a, Args) {
        return rt.call("$enumerate", {a[0], opt(a, 1, b.literal(0, p))}, p);
      });
    }
    if (g == "zip") {
      return call_it(Takes{2, 2, any}, [&](Args a, Args) {
        return rt.call("$zip", {a[0], a[1]}, p);
      });
    }
    if (g == "sum") {
      Takes t = old(1, 2, 2);
      t.py_kw = {"start"};
      t.few = "sum() takes at least 1 positional argument (0 given)";
      return call_it(t, [&](Args a, Args) {
        return rt.call("$sum", {a[0], opt(a, 1, b.literal(0, p))}, p);
      });
    }
    if (g == "min" || g == "max") {
      Takes t = clinic(1, any, any);
      t.py_kw = {"key", "default"};
      return call_it(t, [&](Args a, Args) {
        // `min(xs)` scans one iterable; `min(a, b)` scans its own arguments.
        return rt.call("$minmax",
                       {a.size() == 1 ? a[0] : b.array_lit(a, p),
                        b.bool_literal(g == "max", p)},
                       p);
      });
    }
    if (g == "range") {
      return call_it(clinic(1, 3, 3), [&](Args a, Args) {
        if (a.size() == 1) {
          return rt.call("$range", {b.literal(0, p), a[0], b.literal(1, p)}, p);
        }
        return rt.call("$range", {a[0], a[1], opt(a, 2, b.literal(1, p))}, p);
      });
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
      return rt.call(n, {arg}, p);
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
      body = rt.call("$tuple", {rtc("$tolist")}, p);
    } else {  // abs
      body = rtc("$pyabs");
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
    ctx.args_slot = sa;
    // A comprehension's function is called by its own expression alone,
    // with its first iterator as the one argument (resolve_comp), so there
    // is nothing for a prologue to check or unpack.
    const bool comp = fi.body != nullptr &&
                      (fi.body->tag == "listcomp"_ || fi.body->tag == "dictcomp"_ ||
                       fi.body->tag == "gencomp"_ || fi.body->tag == "bargen"_);
    if (!comp) emit_prologue(fi, rf, ctx, pre, sa, sk, p);
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
    } else if (comp) {
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
    // A parameter after `*rest` is keyword-only: no position reaches it.
    std::vector<NodeId> names;    // every name a keyword may pass, in order
    std::vector<NodeId> reqname;  // the positional ones with no default,
    std::vector<NodeId> reqidx;   // with the position each one wanted
    std::vector<NodeId> kwreqname;  // the keyword-only ones with none
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
      if (has_rest) {
        if (pi.kind != ParamInfo::Default) {
          kwreqname.push_back(b.str_literal(pi.name));
        }
        continue;
      }
      if (pi.kind != ParamInfo::Default) {
        reqname.push_back(b.str_literal(pi.name));
        reqidx.push_back(b.literal(pos));
        required = pos + 1;
      }
      ++pos;
    }
    const NodeId all_names = b.array_lit(names);

    int32_t at = 0;
    bool kwonly = false;
    for (const ParamInfo& pi : fi.params) {
      NodeId value;
      if (pi.kind == ParamInfo::Rest) {
        // A tuple, as Python's `*args` is.
        value = rt.call("$tuple", {rt.call("$rest", {A, b.literal(pos)}, p)},
                        p);
        kwonly = true;
      } else if (pi.kind == ParamInfo::KwRest) {
        value = rt.call("$kwrest", {K, all_names}, p);
      } else if (kwonly) {
        // By name, then the default, and a TypeError naming every
        // keyword-only parameter that neither answered.
        const NodeId fallback =
            pi.kind == ParamInfo::Default
                ? read_var(pi.def_var, ctx, p)
                : rt.call("$missing",
                          {b.str_literal(fi.name), b.str_literal("keyword-only"),
                           b.array_lit(kwreqname),
                           b.array_lit(std::vector<NodeId>(kwreqname.size(),
                                                           b.literal(-1))),
                           A, K},
                          p);
        value = b.make_if(rt.call("$kwhas", {K, b.str_literal(pi.name)}, p),
                          b.index(K, b.str_literal(pi.name)), fallback);
      } else {
        // Positional, then by name, then the default -- and a TypeError
        // when none of the three answered.
        // `$missing` is handed every required parameter rather than this
        // one, because Python's message names all of them at once.
        const NodeId fallback =
            pi.kind == ParamInfo::Default
                ? read_var(pi.def_var, ctx, p)
                : rt.call("$missing",
                          {b.str_literal(fi.name), b.str_literal("positional"),
                           b.array_lit(reqname), b.array_lit(reqidx), A, K},
                          p);
        value = b.make_if(
            b.binary(BinOp::Gt, alen(), b.literal(at)),
            b.index(A, b.literal(at)),
            b.make_if(rt.call("$kwhas", {K, b.str_literal(pi.name)}, p),
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
                    rt.call("$toomany",
                            {b.str_literal(fi.name), b.literal(required),
                             b.literal(pos), alen()},
                            p),
                    NodeId{}));
    }
    if (!has_kwrest) {
      // Only a keyword can be one the function does not take, so a call
      // that passed none -- K is nil, the way every positional call
      // arrives -- skips $kwcheck, and the names it would be handed.
      pre.push_back(b.make_if(
          b.binary(BinOp::Ne, b.intrinsic(IntrinsicId::TypeOf, {K}),
                   b.str_literal("nil")),
          rt.call("$kwcheck", {K, all_names, b.str_literal(fi.name)}, p),
          NodeId{}));
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
        inner = b.make_if(truthy(emit_expr(*c.nodes[0], ctx), p),
                          inner, NodeId{});
        continue;
      }
      // The first `for` walks the iterator it was handed.
      const NodeId given =
          &c == &first_for(a)
              ? b.index(b.varref(VarKind::Local, ctx.args_slot), b.literal(0))
              : NodeId{};
      inner = emit_comp_for(c, inner, given, ctx, p);
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

  // `given` is the iterator to walk, when the caller already made it.
  NodeId emit_comp_for(const Ast& c, NodeId inner, NodeId given, FnCtx& ctx,
                       SrcPos p) {
    auto b = Builder(m).at(p);
    const int32_t it = ctx.alloc_local("$it");
    const int32_t st = ctx.alloc_local("$step");
    const Ast& tg = *c.nodes[0];
    std::vector<NodeId> loop;
    const NodeId value = value_of(b, st);
    if (tg.nodes.size() == 1) {
      loop.push_back(write_var(decl_of.at(tg.nodes[0].get()), value, ctx, p));
    } else {
      const int32_t u = ctx.alloc_local("$unp");
      const NodeId U = b.varref(VarKind::Local, u);
      loop.push_back(b.assign(
          VarKind::Local, u,
          rt.call("$unpack",
                  {value, b.literal(static_cast<int64_t>(tg.nodes.size()))},
                  p)));
      for (size_t k = 0; k < tg.nodes.size(); ++k) {
        loop.push_back(write_var(decl_of.at(tg.nodes[k].get()),
                                 b.index(U, b.literal(static_cast<int64_t>(k))),
                                 ctx, p));
      }
    }
    loop.push_back(inner);
    const NodeId iter = given.valid()
                            ? given
                            : rt.call("$iter",
                                      {emit_expr(*c.nodes[1], ctx)}, p);
    return for_each(rt, p, it, st, iter, loop);
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
    rt.reserve(rt_names());
    const size_t declared = fns.size();
    for (size_t f = 1; f < declared; ++f) {
      rs.fns[f].index = static_cast<int32_t>(m.funcs.size());
      m.funcs.push_back({});
    }


    rs.number_captures(m);
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

}  // namespace

const std::vector<vm::NativeDef>& stdlib() {
  static const std::vector<vm::NativeDef> defs = {
      {"print", 1, nat_print, nullptr},
      {"ffmt", 2, nat_ffmt, nullptr},
  };
  return defs;
}

// python3's last line for an uncaught exception: the class, then the
// message when there is one -- a builtin's name travels in the object, a
// program's own class names itself in its table.
std::string describe(const Value& v) {
  const auto* exc = runtime_parts::exception_record(v);
  if (!exc) return to_display(v);
  const Value& msg = (*exc)[kExcMessage];
  std::string name;
  if (const Value& k = (*exc)[kExcName]; k.is_str()) {
    name = k.as_str();
  } else if (const std::string* n = runtime_parts::class_name((*exc)[0])) {
    name = *n;
  }
  if (name.empty() || msg.is_nil()) return to_display(v);
  const std::string text = to_display(msg);
  return text.empty() ? name : name + ": " + text;
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
