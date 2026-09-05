// The front end that turns one paragraph of the top-level README into
// running code. Its Scope section says a managed, statically-typed
// language is in scope, that "a binder that has already type-checked can
// erase types on the way into the IR", and that the runtime covers
// "classes, `using`/`try`-`finally`, `yield return`, `async`" without
// changes. examples/mini-go proves the numeric half of that list; this one
// is the rest of it, checked against `dotnet`.
//
// Two things here are new to this repository:
//
// **Inheritance and virtual dispatch.** No other front end has a base
// class. The mechanism is one link: a class table carries its base's
// table, and a method lookup walks the chain. `override` is then not a
// feature at all -- it is what happens when the derived table has an entry
// the base also has, and the walk stops at the first one. `samples/shapes.cs`
// calls an override through a base-typed variable, which is the whole
// point of the exercise.
//
// **Types are parsed and erased.** `type` exists in the grammar so that a
// sample is real C#; nothing downstream reads it. The binder keeps exactly
// two facts a type annotation cannot give it: whether a method is `static`
// (which decides where the name is bound) and whether it is `async` or an
// iterator (which decides what calling it builds). Everything else --
// `int` versus `long`, `List<int>` versus `List<string>` -- is gone by the
// time the first node is emitted, which is what that sentence in the
// README means.
//
// The rest is the same machinery the other front ends use, pointed at C#'s
// spelling of it: `using` is a Scope and a Defer, `yield return` is
// Func::is_generator and Tag::Yield, and `async`/`await` is a coroutine
// parked on a Task -- the same three-func lowering examples/mini-js uses
// for a Promise, which is worth noticing: the static language and the
// dynamic one needed the same thing.

#include "binder.h"

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

namespace mini_csharp {
namespace {

constexpr char kClassKey[] = "\x01" "c";
constexpr char kExcKey[] = "\x01" "e";
constexpr char kMsgKey[] = "\x01" "m";
constexpr char kNameKey[] = "\x02" "name";
constexpr char kBaseKey[] = "\x02" "base";
constexpr char kCtorKey[] = "\x02" "ctor";
constexpr char kMethodPrefix[] = "\x02";
// An auto property's backing field: a key no source identifier can spell,
// so it cannot collide with a field the source actually declared.
constexpr char kAutoFieldPrefix[] = "\x03";

std::string auto_field_key(const std::string& name) {
  return std::string(kAutoFieldPrefix) + name;
}

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
      case '"': out.push_back('"'); break;
      default: out.push_back(tok[i]); break;
    }
  }
  return out;
}

// The library names a program may use without declaring them.
bool is_library(const std::string& n) {
  return n == "Console" || n == "Exception" || n == "Task" ||
         n == "string" || n == "String" || n == "Math";
}

struct VarInfo {
  std::string name;
  int32_t owner = 0;
};

struct FnInfo {
  int32_t parent = -1;
  int32_t index = -1;
  bool is_generator = false;
  bool is_async = false;
  bool synth = false;  // a `using`'s dispose thunk, built by hand
  std::string name = "?";
  std::set<int32_t> free;
  std::map<int32_t, int32_t> capture_index;
  std::map<int32_t, int32_t> cell_index;
  std::vector<int32_t> params;  // params[0] is always `this`
  int32_t cls = -1;             // the class this method belongs to
  const Ast* body = nullptr;
};

// One class, as the binder needs it: what it inherits from, what fields it
// declares (so the constructor can initialize them), and which of its
// methods are static.
struct ClassInfo {
  std::string name;
  std::string base;
  int32_t var = -1;                       // the file-scope binding
  const Ast* node = nullptr;
  std::vector<const Ast*> fields;
  std::set<std::string> field_names;
  std::set<std::string> static_field_names;
  std::vector<const Ast*> static_fields;
  std::set<std::string> method_names;
  std::set<std::string> property_names;
  std::vector<std::pair<std::string, int32_t>> methods;  // instance
  // A pure auto property: (name, its initializer or null). Its get_/set_
  // accessors are built by hand at class-table time, the same way a
  // missing constructor is -- see emit_auto_get/emit_auto_set.
  std::vector<std::pair<std::string, const Ast*>> auto_props;
  const Ast* ctor = nullptr;
  int32_t ctor_fn = -1;
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
  std::vector<ClassInfo> classes;
  std::map<std::string, int32_t> class_by_name;
  std::map<const Ast*, int32_t> ref_of;
  std::map<const Ast*, int32_t> decl_of;
  std::map<const Ast*, int32_t> fn_of;
  // A static method's file-scope binding, and the function behind it.
  std::map<std::string, int32_t> static_by_name;
  std::map<int32_t, int32_t> static_fn;
  // An identifier that named a field of the enclosing class rather than a
  // local: C# lets `this.` be implicit, so the binder has to put it back.
  std::set<const Ast*> field_refs;
  // `Speak()` inside a method of the same class: C# lets `this.` be
  // implicit for methods too, and an override still has to win, so this
  // becomes a virtual call on `this` rather than a direct one.
  std::set<const Ast*> method_refs;
  // `Value` inside a method of the class that declares the `Value`
  // property: the same implicit-`this` deal as a field, but read/written
  // through get_Value/set_Value rather than a raw slot.
  std::set<const Ast*> prop_refs;
  // A `static` field lives on the class table, not on an instance -- so a
  // reference to one is a read of that table, and the table has to be
  // captured for it.
  std::map<const Ast*, int32_t> static_field_refs;
  // Every property name declared anywhere: dispatch on `.Name` is by name
  // alone, the same way `$mfind` dispatches a method call -- a property
  // never needs to know which class it is reached through.
  std::set<std::string> property_names;
  std::vector<int32_t> slot_of;
  std::map<std::string, int32_t> rt;
  int32_t empty_cmap = -1;
  // One closure per runtime helper, built once at file scope into an array
  // every function captures -- see build().
  int32_t helpers_var = -1;
  int32_t main_fn = -1;

  static bool has_mod(const Ast& member, const std::string& want) {
    for (const auto& c : member.nodes) {
      if (c->tag == "modifier"_ && c->token == want) return true;
    }
    return false;
  }

  // ==== Pass A: scopes, declarations, captures =============================

  struct ScopeA {
    int32_t fn;
    std::map<std::string, int32_t> names;
  };
  std::vector<ScopeA> scopes;

  int32_t declare(const std::string& name, int32_t fn) {
    auto it = scopes.back().names.find(name);
    if (it != scopes.back().names.end()) return it->second;
    const int32_t v = static_cast<int32_t>(vars.size());
    vars.push_back({name, fn});
    scopes.back().names[name] = v;
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

  // Every method's parameter 0 is `this`, static ones included -- a static
  // call just passes null. One shape for every call site is worth more
  // than the word `this` being absent from two of them.
  int32_t resolve_method(const Ast& node, const Ast& params, const Ast& body,
                         int32_t parent, const std::string& name,
                         int32_t cls) {
    const int32_t f = new_fn(parent, name);
    fns[static_cast<size_t>(f)].cls = cls;
    fns[static_cast<size_t>(f)].body = &body;
    fns[static_cast<size_t>(f)].is_async = has_mod(node, "async");
    fn_of[&node] = f;
    scopes.push_back({f, {}});
    fns[static_cast<size_t>(f)].params.push_back(declare("this", f));
    for (const auto& p : params.nodes) {
      const Ast& id = *p->nodes[1];
      const int32_t v = declare(std::string(id.token), f);
      decl_of[&id] = v;
      fns[static_cast<size_t>(f)].params.push_back(v);
    }
    for (const auto& s : body.nodes) resolve_stmt(*s, f);
    scopes.pop_back();
    return f;
  }

  // A getter takes no source parameter beyond the implicit `this`.
  int32_t resolve_getter(const Ast& body, int32_t parent,
                         const std::string& name, int32_t cls) {
    const int32_t f = new_fn(parent, name);
    fns[static_cast<size_t>(f)].cls = cls;
    fns[static_cast<size_t>(f)].body = &body;
    scopes.push_back({f, {}});
    fns[static_cast<size_t>(f)].params.push_back(declare("this", f));
    for (const auto& s : body.nodes) resolve_stmt(*s, f);
    scopes.pop_back();
    return f;
  }

  // A setter's one parameter is `value`, and it is never spelled in the
  // source -- C# supplies it, so this declares it the same way `this` is.
  int32_t resolve_setter(const Ast& body, int32_t parent,
                         const std::string& name, int32_t cls) {
    const int32_t f = new_fn(parent, name);
    fns[static_cast<size_t>(f)].cls = cls;
    fns[static_cast<size_t>(f)].body = &body;
    scopes.push_back({f, {}});
    fns[static_cast<size_t>(f)].params.push_back(declare("this", f));
    fns[static_cast<size_t>(f)].params.push_back(declare("value", f));
    for (const auto& s : body.nodes) resolve_stmt(*s, f);
    scopes.pop_back();
    return f;
  }

  // A property compiles to the methods C# itself compiles it to --
  // `get_Name`/`set_Name`, found by $mfind like any other method, which is
  // also why an override on one wins the same way. An auto property (no
  // body on either accessor) instead gets both accessors built by hand at
  // class-table time, over a hidden backing field: see emit_auto_get.
  void resolve_propdecl(const Ast& mem, int32_t parent, ClassInfo& ci) {
    const Ast* id = nullptr;
    const Ast* getbody = nullptr;
    const Ast* setbody = nullptr;
    const Ast* init = nullptr;
    bool auto_get = false;
    bool auto_set = false;
    for (const auto& c : mem.nodes) {
      if (c->tag == "ident"_) id = c.get();
      else if (c->tag == "getbody"_) getbody = c->nodes[0].get();
      else if (c->tag == "setbody"_) setbody = c->nodes[0].get();
      else if (c->tag == "getauto"_) auto_get = true;
      else if (c->tag == "setauto"_) auto_set = true;
      else if (c->tag == "fieldinit"_) init = c->nodes[0].get();
    }
    const std::string name(id->token);
    property_names.insert(name);
    const bool any_body = getbody != nullptr || setbody != nullptr;
    const bool any_auto = auto_get || auto_set;
    if (any_body && any_auto) {
      fail(mem, "a property may not mix 'get;'/'set;' with a body here");
    }
    if (any_auto) {
      if (!auto_get || !auto_set) {
        fail(mem, "an auto property needs both 'get;' and 'set;' here");
      }
      ci.auto_props.emplace_back(name, init);
      return;
    }
    if (init != nullptr) {
      fail(mem, "a property with a body cannot also have an initializer");
    }
    const int32_t cls = class_by_name.at(ci.name);
    if (getbody != nullptr) {
      ci.methods.emplace_back(
          "get_" + name,
          resolve_getter(*getbody, parent, ci.name + ".get_" + name, cls));
    }
    if (setbody != nullptr) {
      ci.methods.emplace_back(
          "set_" + name,
          resolve_setter(*setbody, parent, ci.name + ".set_" + name, cls));
    }
  }

  void resolve_program(const Ast& program) {
    const int32_t top = new_fn(-1, "main");
    fns[static_cast<size_t>(top)].body = &program;
    scopes.push_back({top, {}});
    fns[static_cast<size_t>(top)].params.push_back(declare("this", top));

    // Every class name, and every static method's name, is declared before
    // any body is walked -- so a method may call one declared below it,
    // which C# allows and a single pass would not.
    for (const auto& c : program.nodes) {
      if (c->tag != "classdecl"_) continue;
      const std::string cname(c->nodes[0]->token);
      ClassInfo ci;
      ci.name = cname;
      ci.node = c.get();
      for (const auto& n : c->nodes) {
        if (n->tag == "basespec"_) ci.base = std::string(n->nodes[0]->token);
      }
      ci.var = declare(cname, top);
      for (const auto& mem : c->nodes) {
        if (mem->tag == "methoddecl"_ && !has_mod(*mem, "static")) {
          const Ast* id = find_method_name(*mem);
          if (id != nullptr) ci.method_names.insert(std::string(id->token));
        }
        if (mem->tag == "propdecl"_) {
          for (const auto& x : mem->nodes) {
            if (x->tag == "ident"_) {
              ci.property_names.insert(std::string(x->token));
              break;
            }
          }
        }
        if (mem->tag != "fielddecl"_) continue;
        const Ast* prev = nullptr;
        for (const auto& x : mem->nodes) {
          if (x->tag == "fieldinit"_) break;
          prev = x.get();
        }
        if (prev == nullptr) continue;
        if (has_mod(*mem, "static")) {
          ci.static_field_names.insert(std::string(prev->token));
        } else {
          ci.field_names.insert(std::string(prev->token));
        }
      }
      class_by_name[cname] = static_cast<int32_t>(classes.size());
      classes.push_back(ci);
    }
    for (const auto& c : program.nodes) {
      if (c->tag != "classdecl"_) continue;
      for (const auto& mem : c->nodes) {
        if (mem->tag != "methoddecl"_ || !has_mod(*mem, "static")) continue;
        const Ast& id = *find_method_name(*mem);
        const int32_t v = declare(std::string(id.token), top);
        decl_of[&id] = v;
        static_by_name[std::string(id.token)] = v;
      }
    }

    for (const auto& c : program.nodes) {
      if (c->tag != "classdecl"_) continue;
      resolve_class(*c, top);
    }
    scopes.pop_back();
  }

  // A methoddecl is `modifier* type ident ( params ) block`; the name is
  // the identifier that follows the type, which is the second-to-last
  // child before the parameters.
  static const Ast* find_method_name(const Ast& mem) {
    const Ast* prev = nullptr;
    for (const auto& c : mem.nodes) {
      if (c->tag == "params"_) return prev;
      prev = c.get();
    }
    return prev;
  }

  void resolve_class(const Ast& c, int32_t top) {
    ClassInfo& ci = classes[static_cast<size_t>(class_by_name.at(
        std::string(c.nodes[0]->token)))];
    for (const auto& mem : c.nodes) {
      if (mem->tag == "fielddecl"_) {
        if (has_mod(*mem, "static")) {
          ci.static_fields.push_back(mem.get());
        } else {
          ci.fields.push_back(mem.get());
        }
      } else if (mem->tag == "ctordecl"_) {
        ci.ctor = mem.get();
      }
    }
    for (const auto& mem : c.nodes) {
      if (mem->tag == "methoddecl"_) {
        const Ast& id = *find_method_name(*mem);
        const int32_t f = resolve_method(
            *mem, *find_child(*mem, "params"), *mem->nodes.back(), top,
            ci.name + "." + std::string(id.token),
            class_by_name.at(ci.name));
        if (has_mod(*mem, "static")) {
          static_fn[decl_of.at(&id)] = f;
          if (std::string(id.token) == "Main") main_fn = f;
        } else {
          ci.methods.emplace_back(std::string(id.token), f);
        }
      } else if (mem->tag == "propdecl"_) {
        resolve_propdecl(*mem, top, ci);
      } else if (mem->tag == "ctordecl"_) {
        ci.ctor_fn = resolve_method(*mem, *find_child(*mem, "params"),
                                    *mem->nodes.back(), top,
                                    ci.name + ".ctor",
                                    class_by_name.at(ci.name));
        // The base initializer's arguments are resolved in the
        // constructor's own scope by resolve_ctor_extras below, once the
        // whole class (including its base) is known.
      }
    }
    // The field initializers and the base call run inside the constructor,
    // so they resolve there too.
    if (ci.ctor_fn >= 0) resolve_ctor_extras(ci);
  }

  void resolve_ctor_extras(ClassInfo& ci) {
    const int32_t f = ci.ctor_fn;
    scopes.push_back({f, {}});
    for (size_t i = 0; i < fns[static_cast<size_t>(f)].params.size(); ++i) {
      const int32_t v = fns[static_cast<size_t>(f)].params[i];
      scopes.back().names[vars[static_cast<size_t>(v)].name] = v;
    }
    // The base class's table is read by `: base(...)`, so naming it in a
    // base initializer is a reference like any other -- without this the
    // constructor has no capture for it and the lookup fails a long way
    // from here.
    if (!ci.base.empty() && class_by_name.count(ci.base)) resolve(ci.base, f);
    for (const Ast* fd : ci.fields) {
      for (const auto& n : fd->nodes) {
        if (n->tag == "fieldinit"_) resolve_expr(*n->nodes[0], f);
      }
    }
    for (const auto& [name, init] : ci.auto_props) {
      (void)name;
      if (init != nullptr) resolve_expr(*init, f);
    }
    if (ci.ctor != nullptr) {
      for (const auto& n : ci.ctor->nodes) {
        if (n->tag != "baseinit"_) continue;
        for (const auto& x : n->nodes[0]->nodes) resolve_expr(*x, f);
      }
    }
    scopes.pop_back();
  }

  // Walking the base chain is what makes an inherited field reachable
  // without repeating its declaration -- the same walk method lookup does
  // at run time. `is_field`/`is_method`/`is_property`/`static_field_owner`
  // are this one walk, aimed at whichever per-class name set the caller
  // cares about.
  int32_t find_in_chain(int32_t cls, const std::string& name,
                        std::set<std::string> ClassInfo::* member) const {
    for (int32_t c = cls; c >= 0;) {
      const ClassInfo& ci = classes[static_cast<size_t>(c)];
      if ((ci.*member).count(name)) return c;
      if (ci.base.empty()) break;
      const auto it = class_by_name.find(ci.base);
      if (it == class_by_name.end()) break;
      c = it->second;
    }
    return -1;
  }

  bool is_field(int32_t cls, const std::string& name) const {
    return find_in_chain(cls, name, &ClassInfo::field_names) >= 0;
  }

  int32_t static_field_owner(int32_t cls, const std::string& name) const {
    return find_in_chain(cls, name, &ClassInfo::static_field_names);
  }

  bool is_method(int32_t cls, const std::string& name) const {
    return find_in_chain(cls, name, &ClassInfo::method_names) >= 0;
  }

  bool is_property(int32_t cls, const std::string& name) const {
    return find_in_chain(cls, name, &ClassInfo::property_names) >= 0;
  }

  static const Ast* find_child(const Ast& a, std::string_view name) {
    for (const auto& n : a.nodes) {
      if (n->name == name) return n.get();
    }
    return nullptr;
  }

  void resolve_stmt(const Ast& a, int32_t fn) {
    switch (a.tag) {
      case "emptystmt"_:
      case "breakstmt"_:
      case "contstmt"_:
        return;
      case "block"_:
        scopes.push_back({fn, {}});
        for (const auto& s : a.nodes) resolve_stmt(*s, fn);
        scopes.pop_back();
        return;
      case "localdecl"_: {
        for (const auto& n : a.nodes) {
          if (n->tag == "fieldinit"_) resolve_expr(*n->nodes[0], fn);
        }
        const Ast& id = *a.nodes[1];
        decl_of[&id] = declare(std::string(id.token), fn);
        return;
      }
      case "ifstmt"_:
        resolve_expr(*a.nodes[0], fn);
        resolve_stmt(*a.nodes[1], fn);
        if (a.nodes.size() > 2) resolve_stmt(*a.nodes[2], fn);
        return;
      case "whilestmt"_:
        resolve_expr(*a.nodes[0], fn);
        resolve_stmt(*a.nodes[1], fn);
        return;
      case "forstmt"_: {
        scopes.push_back({fn, {}});
        for (const auto& c : a.nodes[0]->nodes) {
          if (c->tag == "localdeclbare"_) {
            resolve_expr(*c->nodes[2], fn);
            decl_of[c->nodes[1].get()] =
                declare(std::string(c->nodes[1]->token), fn);
          } else {
            resolve_expr(*c, fn);
          }
        }
        for (const auto& c : a.nodes[1]->nodes) resolve_expr(*c, fn);
        for (const auto& c : a.nodes[2]->nodes) resolve_expr(*c, fn);
        resolve_stmt(*a.nodes[3], fn);
        scopes.pop_back();
        return;
      }
      case "foreachstmt"_: {
        resolve_expr(*a.nodes[2], fn);
        scopes.push_back({fn, {}});
        decl_of[a.nodes[1].get()] =
            declare(std::string(a.nodes[1]->token), fn);
        resolve_stmt(*a.nodes[3], fn);
        scopes.pop_back();
        return;
      }
      case "usingstmt"_: {
        resolve_expr(*a.nodes[2], fn);
        scopes.push_back({fn, {}});
        decl_of[a.nodes[1].get()] =
            declare(std::string(a.nodes[1]->token), fn);
        resolve_stmt(*a.nodes[3], fn);
        scopes.pop_back();
        // The Dispose thunk: synthesized, so it has no body to walk.
        const int32_t g = new_fn(fn, "<dispose>");
        fns[static_cast<size_t>(g)].synth = true;
        fn_of[&a] = g;
        return;
      }
      case "trystmt"_: {
        resolve_stmt(*a.nodes[0], fn);
        for (size_t i = 1; i < a.nodes.size(); ++i) {
          const Ast& c = *a.nodes[i];
          if (c.tag == "catchcl"_) {
            scopes.push_back({fn, {}});
            for (const auto& n : c.nodes) {
              if (n->tag == "ident"_) {
                decl_of[n.get()] = declare(std::string(n->token), fn);
              }
            }
            resolve_stmt(*c.nodes.back(), fn);
            scopes.pop_back();
          } else {
            // A Defer takes a callable, so `finally` is a function.
            const int32_t g = new_fn(fn, "<finally>");
            fns[static_cast<size_t>(g)].body = c.nodes[0].get();
            fn_of[&c] = g;
            scopes.push_back({g, {}});
            fns[static_cast<size_t>(g)].params.push_back(declare("this", g));
            for (const auto& s : c.nodes[0]->nodes) resolve_stmt(*s, g);
            scopes.pop_back();
          }
        }
        return;
      }
      case "yieldstmt"_:
        fns[static_cast<size_t>(fn)].is_generator = true;
        for (const auto& c : a.nodes) resolve_expr(*c, fn);
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
      case "type"_:
        return;
      case "ident"_: {
        const std::string n(a.token);
        if (auto v = resolve(n, fn)) {
          ref_of[&a] = *v;
          return;
        }
        // A field of the enclosing class, or of one it inherits from --
        // C# writes `Name` where the IR needs `this.Name`.
        if (is_field(fns[static_cast<size_t>(fn)].cls, n)) {
          field_refs.insert(&a);
          return;
        }
        if (is_property(fns[static_cast<size_t>(fn)].cls, n)) {
          prop_refs.insert(&a);
          return;
        }
        if (is_method(fns[static_cast<size_t>(fn)].cls, n)) {
          method_refs.insert(&a);
          return;
        }
        const int32_t sc = static_field_owner(fns[static_cast<size_t>(fn)].cls,
                                              n);
        if (sc >= 0) {
          static_field_refs[&a] = sc;
          resolve(classes[static_cast<size_t>(sc)].name, fn);
          return;
        }
        if (is_library(n)) return;
        fail(a, "the name '" + n + "' does not exist in this context");
      }
      case "newexp"_: {
        // The type names a class or a library container. A class *is* a
        // value here -- the table `new` reads its constructor out of --
        // so naming one has to register the same capture any other
        // reference would, or the class table would be unreachable from
        // inside a method. (It was, on this binder's first pass, and the
        // symptom was a missing capture index three functions away.)
        std::string tn(a.nodes[0]->token);
        const size_t lt = tn.find('<');
        if (lt != std::string::npos) tn = tn.substr(0, lt);
        if (class_by_name.count(tn)) resolve(tn, fn);
        for (size_t i = 1; i < a.nodes.size(); ++i) {
          resolve_expr(*a.nodes[i], fn);
        }
        return;
      }
      // `Klass.Static(...)`: the receiver is a class name and the member
      // is a file-scope binding of its own, so both are references.
      case "postfix"_: {
        if (a.nodes.size() > 1 && a.nodes[0]->tag == "ident"_ &&
            a.nodes[1]->tag == "membersfx"_ &&
            class_by_name.count(std::string(a.nodes[0]->token))) {
          const std::string mem(a.nodes[1]->nodes[0]->token);
          if (static_by_name.count(mem)) resolve(mem, fn);
        }
        for (const auto& c : a.nodes) {
          if (c->tag == "membersfx"_ || c->tag == "incsfx"_) continue;
          resolve_expr(*c, fn);
        }
        return;
      }
      case "castexp"_:
        resolve_expr(*a.nodes[1], fn);
        return;
      default:
        for (const auto& c : a.nodes) {
          if (c->tag == "assignop"_ || c->tag == "eqop"_ ||
              c->tag == "relop"_ || c->tag == "addop"_ ||
              c->tag == "mulop"_ || c->tag == "membersfx"_ ||
              c->tag == "incsfx"_ || c->tag == "type"_) {
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
    // A class binding is read by every `new` and every static call, from
    // functions that are not lexically inside the file scope's own body --
    // so it has to be a cell whether or not the free-variable walk found
    // it.
    for (const auto& ci : classes) {
      auto& own = fns[static_cast<size_t>(vars[static_cast<size_t>(ci.var)]
                                              .owner)]
                      .cell_index;
      if (!own.count(ci.var)) own[ci.var] = static_cast<int32_t>(own.size());
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
  // Shorter than any other front end's here, and the reason is worth
  // saying: C# agrees with the VM about almost everything the others had
  // to override. A condition is a `bool` and nothing else, so there is no
  // `$truthy`. `/` and `%` truncate toward zero, which is BinOp's rule
  // already. And `Console.WriteLine(2.0)` prints "2" -- to_display's
  // output verbatim, so this is the only front end here that needed no
  // float-formatting rule at all.

  static const std::vector<std::string>& rt_names() {
    static const std::vector<std::string> names = {
        "$str",  "$add",  "$eq",   "$mfind", "$getf", "$setf", "$idx",
        "$setidx", "$len", "$iter", "$iternext", "$join", "$exc",
        "$mkexc", "$tnew", "$tsettle", "$ton", "$totask", "$tresolve",
        "$adopt", "$await", "$mainwrap",
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
    NodeId C(int32_t i) { return b.varref(VarKind::Cell, i, p); }
    NodeId P(int32_t i) { return b.varref(VarKind::Capture, i, p); }
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
    NodeId setc(int32_t i, NodeId v) {
      return b.block({b.cell_fresh(i, p), b.assign(VarKind::Cell, i, v, p)}, p);
    }
    NodeId clos(const std::string& name, const std::vector<int32_t>& cells) {
      std::vector<CaptureSrc> cs;
      for (const int32_t c : cells) cs.push_back({VarKind::Cell, c});
      const int32_t cm = static_cast<int32_t>(bd.m.capture_maps.size());
      bd.m.capture_maps.push_back(cs);
      return b.make_closure(bd.rt.at(name), cm, p);
    }
    NodeId ret(NodeId v) { return b.make_return(v, p); }
    NodeId blk(const std::vector<NodeId>& v) { return b.block(v, p); }
    NodeId iff(NodeId c, NodeId t) { return b.make_if(c, t, NodeId{}, p); }
    NodeId iff(NodeId c, NodeId t, NodeId e) { return b.make_if(c, t, e, p); }
    NodeId wh(NodeId c, NodeId x) { return b.make_while(c, x, p); }
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
    NodeId has(NodeId t, NodeId k) {
      return in(IntrinsicId::ObjectHas, {t, k});
    }
    NodeId is(NodeId v, const std::string& s) { return bin(BinOp::Eq, v, S(s)); }
    NodeId isnt(NodeId v, const std::string& s) {
      return bin(BinOp::Ne, v, S(s));
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
                std::vector<std::string> names, int32_t ncells = 0,
                int32_t ncaps = 0) {
      Func& f = bd.m.funcs[static_cast<size_t>(bd.rt.at(name))];
      f.name = name;
      f.num_params = nparams;
      f.num_locals = nlocals;
      names.resize(static_cast<size_t>(nlocals), "");
      f.local_names = std::move(names);
      f.num_cells = ncells;
      f.num_captures = ncaps;
      for (int32_t i = 0; i < ncaps; ++i) f.capture_names.push_back("c");
      f.lenient_arity = true;
      f.body = b.scope(0, nlocals, blk(body), p);
    }
  };

  // Object.ToString, for the values this subset has. Note the double: no
  // rule at all, because to_display already is what C# prints.
  void rt_str() {
    RT r(*this);
    r.add(r.set(1, r.typ(r.L(0))));
    r.add(r.iff(r.is(r.L(1), "string"), r.ret(r.L(0))));
    r.add(r.iff(r.is(r.L(1), "nil"), r.ret(r.S(""))));
    r.add(r.iff(r.is(r.L(1), "bool"),
                r.ret(r.iff(r.L(0), r.S("True"), r.S("False")))));
    r.add(r.iff(r.either(r.is(r.L(1), "int"), r.is(r.L(1), "double")),
                r.ret(r.in(IntrinsicId::ToStr, {r.L(0)}))));
    r.add(r.iff(r.isnt(r.L(1), "object"), r.ret(r.S("<value>"))));
    r.add(r.iff(r.has(r.L(0), r.S(kExcKey)),
                r.ret(r.idx(r.L(0), kMsgKey))));
    r.add(r.iff(r.has(r.L(0), r.S(kClassKey)),
                r.ret(r.idx(r.idx(r.L(0), kClassKey), kNameKey))));
    r.add(r.ret(r.S("<object>")));
    r.finish("$str", 1, 2, {"v", "t"});
  }

  void rt_add() {
    RT r(*this);
    r.add(r.iff(r.either(r.is(r.typ(r.L(0)), "string"),
                         r.is(r.typ(r.L(1)), "string")),
                r.ret(r.bin(BinOp::Add, r.call("$str", {r.L(0)}),
                            r.call("$str", {r.L(1)})))));
    r.add(r.ret(r.bin(BinOp::Add, r.L(0), r.L(1))));
    r.finish("$add", 2, 2, {"a", "b"});
  }

  // `==` compares strings by value and everything else by identity, with
  // numbers meeting across int and double -- which is where BinOp::Eq
  // stops, since it refuses two values of different types.
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
    r.add(r.ret(r.in(IntrinsicId::Same, {r.L(0), r.L(1)})));
    r.finish("$eq", 2, 4, {"a", "b", "ta", "tb"});
  }

  // **Virtual dispatch.** The instance points at its class table, the
  // table points at its base's, and the walk stops at the first entry it
  // finds -- so an `override` wins because the derived table is looked at
  // first, and no other machinery is needed for it at all.
  void rt_mfind() {
    RT r(*this);
    r.add(r.iff(r.isnt(r.typ(r.L(0)), "object"),
                r.ret(r.call("$exc", {r.S("null reference")}))));
    r.add(r.iff(r.bin(BinOp::Eq, r.has(r.L(0), r.S(kClassKey)), r.Bo(false)),
                r.ret(r.call("$exc", {r.S("not an object")}))));
    r.add(r.set(2, r.idx(r.L(0), kClassKey)));
    r.add(r.set(3, r.bin(BinOp::Add, r.S(kMethodPrefix), r.L(1))));
    r.add(r.wh(r.is(r.typ(r.L(2)), "object"),
               r.blk({r.iff(r.has(r.L(2), r.L(3)), r.ret(r.idx(r.L(2), r.L(3)))),
                      r.iff(r.bin(BinOp::Eq, r.has(r.L(2), r.S(kBaseKey)),
                                  r.Bo(false)),
                            r.b.make_break(r.p)),
                      r.set(2, r.idx(r.L(2), kBaseKey))})));
    r.add(r.ret(r.call("$exc",
                       {r.bin(BinOp::Add, r.S("no such method: "), r.L(1))})));
    r.finish("$mfind", 2, 4, {"recv", "name", "cls", "key"});
  }

  void rt_getf() {
    RT r(*this);
    r.add(r.iff(r.isnt(r.typ(r.L(0)), "object"),
                r.ret(r.call("$exc", {r.S("null reference")}))));
    r.add(r.iff(r.has(r.L(0), r.L(1)), r.ret(r.idx(r.L(0), r.L(1)))));
    r.add(r.ret(r.Nil()));
    r.finish("$getf", 2, 2, {"o", "name"});
  }

  void rt_setf() {
    RT r(*this);
    r.add(r.iff(r.isnt(r.typ(r.L(0)), "object"),
                r.ret(r.call("$exc", {r.S("null reference")}))));
    r.add(r.sidx(r.L(0), r.L(1), r.L(2)));
    r.add(r.ret(r.L(2)));
    r.finish("$setf", 3, 3, {"o", "name", "val"});
  }

  void rt_idx() {
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(0)), "map"),
                r.blk({r.iff(r.has(r.L(0), r.L(1)),
                             r.ret(r.idx(r.L(0), r.L(1)))),
                       r.ret(r.call("$exc", {r.S("key not found")}))})));
    r.add(r.iff(r.either(r.bin(BinOp::Lt, r.L(1), r.I(0)),
                         r.bin(BinOp::Ge, r.L(1), r.len(r.L(0)))),
                r.ret(r.call("$exc", {r.S("index out of range")}))));
    r.add(r.ret(r.idx(r.L(0), r.L(1))));
    r.finish("$idx", 2, 2, {"v", "k"});
  }

  void rt_setidx() {
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(0)), "map"),
                r.blk({r.sidx(r.L(0), r.L(1), r.L(2)), r.ret(r.L(2))})));
    r.add(r.iff(r.either(r.bin(BinOp::Lt, r.L(1), r.I(0)),
                         r.bin(BinOp::Ge, r.L(1), r.len(r.L(0)))),
                r.ret(r.call("$exc", {r.S("index out of range")}))));
    r.add(r.sidx(r.L(0), r.L(1), r.L(2)));
    r.add(r.ret(r.L(2)));
    r.finish("$setidx", 3, 3, {"v", "k", "val"});
  }

  void rt_len() {
    RT r(*this);
    r.add(r.ret(r.len(r.L(0))));
    r.finish("$len", 1, 1, {"v"});
  }

  void rt_iter() {
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(0)), "generator"),
                r.ret(r.obj({{"k", r.S("g")}, {"v", r.L(0)}}))));
    r.add(r.ret(r.obj({{"k", r.S("a")}, {"v", r.L(0)}, {"i", r.I(0)}})));
    r.finish("$iter", 1, 1, {"v"});
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

  void rt_join() {
    RT r(*this);
    r.add(r.set(2, r.S("")));
    r.add(r.set(3, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(3), r.len(r.L(1))),
               r.blk({r.iff(r.bin(BinOp::Gt, r.L(3), r.I(0)),
                            r.set(2, r.bin(BinOp::Add, r.L(2), r.L(0)))),
                      r.set(2, r.bin(BinOp::Add, r.L(2),
                                     r.call("$str",
                                            {r.idx(r.L(1), r.L(3))}))),
                      r.set(3, r.bin(BinOp::Add, r.L(3), r.I(1)))})));
    r.add(r.ret(r.L(2)));
    r.finish("$join", 2, 4, {"sep", "xs", "out", "i"});
  }

  void rt_mkexc() {
    RT r(*this);
    r.add(r.ret(r.obj({{kExcKey, r.Bo(true)}, {kMsgKey, r.L(0)}})));
    r.finish("$mkexc", 1, 1, {"msg"});
  }

  void rt_exc() {
    RT r(*this);
    r.add(r.b.make_throw(r.call("$mkexc", {r.L(0)}), r.p));
    r.finish("$exc", 1, 1, {"msg"});
  }

  // -- Task, async and await ----------------------------------------------
  //
  // The same three-func lowering examples/mini-js uses for a Promise, and
  // that is the observation worth keeping: the statically-typed language
  // and the dynamic one wanted exactly the same thing from the scheduler.
  void rt_tnew() {
    RT r(*this);
    r.add(r.ret(r.obj({{"$task", r.Bo(true)},
                       {"s", r.I(0)},
                       {"v", r.Nil()},
                       {"cbs", r.arr({})}})));
    r.finish("$tnew", 0, 0, {});
  }

  void rt_tsettle() {
    RT r(*this);
    r.add(r.iff(r.bin(BinOp::Ne, r.idx(r.L(0), "s"), r.I(0)), r.ret(r.Nil())));
    r.add(r.sidx(r.L(0), r.S("s"), r.L(1)));
    r.add(r.sidx(r.L(0), r.S("v"), r.L(2)));
    r.add(r.set(3, r.idx(r.L(0), "cbs")));
    r.add(r.sidx(r.L(0), r.S("cbs"), r.arr({})));
    r.add(r.set(4, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(4), r.len(r.L(3))),
               r.blk({r.in(IntrinsicId::Enqueue, {r.idx(r.L(3), r.L(4))}),
                      r.set(4, r.bin(BinOp::Add, r.L(4), r.I(1)))})));
    r.add(r.ret(r.Nil()));
    r.finish("$tsettle", 3, 5, {"t", "st", "v", "cbs", "i"});
  }

  void rt_ton() {
    RT r(*this);
    r.add(r.iff(r.bin(BinOp::Ne, r.idx(r.L(0), "s"), r.I(0)),
                r.blk({r.in(IntrinsicId::Enqueue, {r.L(1)}), r.ret(r.Nil())})));
    r.add(r.in(IntrinsicId::ArrayPush, {r.idx(r.L(0), "cbs"), r.L(1)}));
    r.add(r.ret(r.Nil()));
    r.finish("$ton", 2, 2, {"t", "cb"});
  }

  void rt_totask() {
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(0)), "object"),
                r.iff(r.has(r.L(0), r.S("$task")), r.ret(r.L(0)))));
    r.add(r.set(1, r.call("$tnew", {})));
    r.add(r.call("$tsettle", {r.L(1), r.I(1), r.L(0)}));
    r.add(r.ret(r.L(1)));
    r.finish("$totask", 1, 2, {"v", "t"});
  }

  void rt_tresolve() {
    RT r(*this);
    r.add(r.iff(
        r.is(r.typ(r.L(1)), "object"),
        r.iff(r.has(r.L(1), r.S("$task")),
              r.blk({r.setc(0, r.L(0)), r.setc(1, r.L(1)),
                     r.call("$ton", {r.L(1), r.clos("$adopt", {0, 1})}),
                     r.ret(r.Nil())}))));
    r.add(r.call("$tsettle", {r.L(0), r.I(1), r.L(1)}));
    r.add(r.ret(r.Nil()));
    r.finish("$tresolve", 2, 2, {"t", "v"}, 2);
  }

  void rt_adopt() {
    RT r(*this);
    r.add(r.call("$tsettle",
                 {r.P(0), r.idx(r.P(1), "s"), r.idx(r.P(1), "v")}));
    r.finish("$adopt", 0, 0, {}, 0, 2);
  }

  void rt_await() {
    RT r(*this);
    r.add(r.set(1, r.call("$totask", {r.L(0)})));
    r.add(r.call("$ton", {r.L(1), r.in(IntrinsicId::CoroCurrent, {})}));
    r.add(r.in(IntrinsicId::CoroYield, {r.Nil()}));
    r.add(r.iff(r.bin(BinOp::Eq, r.idx(r.L(1), "s"), r.I(2)),
                r.b.make_throw(r.idx(r.L(1), "v"), r.p)));
    r.add(r.ret(r.idx(r.L(1), "v")));
    r.finish("$await", 1, 2, {"v", "t"});
  }

  // Main runs as a coroutine, spawned by the entry frame. That is what
  // lets `.Wait()` park -- a CoroYield in the entry frame would have no
  // coroutine to suspend, which is the same reason examples/mini-go makes
  // Go's `main` a goroutine.
  void rt_mainwrap() {
    RT r(*this);
    r.add(r.b.call_value(r.P(0), {r.Nil()}, r.p));
    r.finish("$mainwrap", 1, 1, {"_"}, 0, 1);
  }

  void emit_runtime() {
    rt_str(); rt_add(); rt_eq(); rt_mfind(); rt_getf(); rt_setf(); rt_idx();
    rt_setidx(); rt_len(); rt_iter(); rt_iternext(); rt_join(); rt_exc();
    rt_mkexc(); rt_tnew(); rt_tsettle(); rt_ton(); rt_totask();
    rt_tresolve(); rt_adopt(); rt_await(); rt_mainwrap();
  }

  // ==== Pass B: emit ======================================================

  // Where a helper sits in the array fill_helpers builds. A helper that
  // takes captures has no closure there to fetch -- it is built at the
  // site that has them -- so asking for one is a mistake in this binder,
  // and saying so here beats the "cannot call nil" it would otherwise be
  // at run time.
  int32_t helper_slot(const std::string& name) const {
    const auto& names = rt_names();
    for (size_t i = 0; i < names.size(); ++i) {
      if (names[i] != name) continue;
      if (m.funcs[static_cast<size_t>(rt.at(name))].num_captures != 0) {
        coreir_rt::fail("runtime helper " + name + " takes captures", 0, 0);
      }
      return static_cast<int32_t>(i);
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
  void fill_helpers(FnCtx& ctx, std::vector<NodeId>& out, SrcPos p) {
    Builder b(m);
    std::vector<NodeId> vals;
    for (const std::string& n : rt_names()) {
      // A helper that takes captures is built at the site that has them,
      // never fetched from here, so its slot stays nil.
      const int32_t g = rt.at(n);
      vals.push_back(m.funcs[static_cast<size_t>(g)].num_captures == 0
                         ? b.make_closure(g, empty_cmap, p)
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

  NodeId this_of(FnCtx& ctx, SrcPos p) {
    const FnInfo& fi = fns[static_cast<size_t>(ctx.fn)];
    if (fi.params.empty()) {
      Builder b(m);
      return b.nil_literal(p);
    }
    return read_var(fi.params[0], ctx, p);
  }

  NodeId emit_block(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    const int32_t mark = ctx.next_local;
    std::vector<NodeId> out;
    for (const auto& s : a.nodes) out.push_back(emit_stmt(*s, ctx));
    const int32_t end = ctx.next_local;
    ctx.next_local = mark;
    if (end > mark) return b.scope(mark, end, b.block(out, p), p);
    return b.block(out, p);
  }

  // -- Statements ---------------------------------------------------------
  NodeId emit_stmt(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    switch (a.tag) {
      case "emptystmt"_:
        return b.block({}, p);
      case "block"_:
        return emit_block(a, ctx);
      case "localdecl"_: {
        NodeId v = b.nil_literal(p);
        for (const auto& n : a.nodes) {
          if (n->tag == "fieldinit"_) v = emit_expr(*n->nodes[0], ctx);
        }
        return bind_decl(decl_of.at(a.nodes[1].get()), v, ctx, p);
      }
      case "ifstmt"_: {
        const NodeId c = emit_expr(*a.nodes[0], ctx);
        const NodeId t = emit_stmt(*a.nodes[1], ctx);
        NodeId e;
        if (a.nodes.size() > 2) e = emit_stmt(*a.nodes[2], ctx);
        return b.make_if(c, t, e, p);
      }
      case "whilestmt"_:
        return b.make_while(emit_expr(*a.nodes[0], ctx),
                            emit_stmt(*a.nodes[1], ctx), p);
      case "forstmt"_:
        return emit_for(a, ctx);
      case "foreachstmt"_:
        return emit_foreach(a, ctx);
      case "usingstmt"_:
        return emit_using(a, ctx);
      case "trystmt"_:
        return emit_try(a, ctx);
      case "returnstmt"_:
        return b.make_return(
            a.nodes.empty() ? b.nil_literal(p) : emit_expr(*a.nodes[0], ctx),
            p);
      case "throwstmt"_:
        return b.make_throw(emit_expr(*a.nodes[0], ctx), p);
      case "yieldstmt"_:
        // `yield break` has no operand; `yield return e` has one.
        if (a.nodes.empty()) return b.make_return(b.nil_literal(p), p);
        return b.make_yield(emit_expr(*a.nodes[0], ctx), p);
      case "breakstmt"_:
        return b.make_break(p);
      case "contstmt"_:
        return b.make_continue(p);
      case "exprstmt"_:
        return emit_expr_stmt(*a.nodes[0], ctx);
      default:
        fail(a, "cannot execute " + a.name);
    }
  }

  NodeId bind_decl(int32_t v, NodeId value, FnCtx& ctx, SrcPos p) {
    Builder b(m);
    const auto& ci = fns[static_cast<size_t>(ctx.fn)].cell_index;
    const auto it = ci.find(v);
    if (it != ci.end()) {
      return b.block({b.cell_fresh(it->second, p),
                      b.assign(VarKind::Cell, it->second, value, p)},
                     p);
    }
    const int32_t s = ctx.alloc_local(vars[static_cast<size_t>(v)].name);
    slot_of[static_cast<size_t>(v)] = s;
    return b.assign(VarKind::Local, s, value, p);
  }

  bool is_incdec(const Ast& a) {
    return a.tag == "postfix"_ && a.nodes.back()->tag == "incsfx"_;
  }

  NodeId emit_expr_stmt(const Ast& a, FnCtx& ctx) {
    if (!is_incdec(a)) return emit_expr(a, ctx);
    Builder b(m);
    const SrcPos p = pos_of(a);
    const bool up = a.nodes.back()->token == "++";
    return emit_store(a, a.nodes.size() - 1, ctx, p, [&](auto&& cur) {
      return b.binary(up ? BinOp::Add : BinOp::Sub, cur(), b.literal(1, p), p);
    });
  }

  NodeId emit_for(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    const int32_t mark = ctx.next_local;
    std::vector<NodeId> out;
    for (const auto& c : a.nodes[0]->nodes) {
      if (c->tag == "localdeclbare"_) {
        out.push_back(bind_decl(decl_of.at(c->nodes[1].get()),
                                emit_expr(*c->nodes[2], ctx), ctx, p));
      } else {
        out.push_back(emit_expr_stmt(*c, ctx));
      }
    }
    const int32_t first = ctx.alloc_local("$first");
    out.push_back(b.assign(VarKind::Local, first, b.bool_literal(true, p), p));
    std::vector<NodeId> step;
    for (const auto& c : a.nodes[2]->nodes) {
      step.push_back(emit_expr_stmt(*c, ctx));
    }
    std::vector<NodeId> loop{b.make_if(
        b.varref(VarKind::Local, first, p),
        b.assign(VarKind::Local, first, b.bool_literal(false, p), p),
        b.block(step, p), p)};
    if (!a.nodes[1]->nodes.empty()) {
      loop.push_back(b.make_if(
          b.binary(BinOp::Eq, emit_expr(*a.nodes[1]->nodes[0], ctx),
                   b.bool_literal(false, p), p),
          b.make_break(p), NodeId{}, p));
    }
    loop.push_back(emit_stmt(*a.nodes[3], ctx));
    out.push_back(b.make_while(b.bool_literal(true, p), b.block(loop, p), p));
    const int32_t end = ctx.next_local;
    ctx.next_local = mark;
    return b.scope(mark, end, b.block(out, p), p);
  }

  NodeId emit_foreach(const Ast& a, FnCtx& ctx) {
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
        bind_decl(decl_of.at(a.nodes[1].get()),
                  b.index(S, b.str_literal("value", p), p), ctx, p),
        emit_stmt(*a.nodes[3], ctx)};
    const NodeId body = b.block(
        {b.assign(VarKind::Local, it,
                  helper(ctx, "$iter", {emit_expr(*a.nodes[2], ctx)}, p), p),
         b.make_while(b.bool_literal(true, p), b.block(loop, p), p)},
        p);
    const int32_t end = ctx.next_local;
    ctx.next_local = mark;
    return b.scope(mark, end, body, p);
  }

  // `using (T x = e) stmt`: the resource goes into a cell so the Dispose
  // thunk can capture it, and the thunk is a Defer -- which runs "however
  // it exits: falling through, Break, Continue, Return, or an unwinding
  // throw", which is IDisposable's contract word for word.
  NodeId emit_using(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    const int32_t mark = ctx.next_local;
    const int32_t cell = ctx.next_cell++;
    const NodeId C = b.varref(VarKind::Cell, cell, p);
    std::vector<NodeId> out{
        b.cell_fresh(cell, p),
        b.assign(VarKind::Cell, cell, emit_expr(*a.nodes[2], ctx), p),
        bind_decl(decl_of.at(a.nodes[1].get()), C, ctx, p)};
    const int32_t g = fn_of.at(&a);
    emit_dispose_thunk(g);
    std::vector<CaptureSrc> cs{{VarKind::Cell, cell}};
    const int32_t cm = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back(cs);
    out.push_back(b.make_defer(
        b.make_closure(fns[static_cast<size_t>(g)].index, cm, p), p));
    out.push_back(emit_stmt(*a.nodes[3], ctx));
    const int32_t end = ctx.next_local;
    ctx.next_local = mark;
    return b.scope(mark, end, b.block(out, p), p);
  }

  void emit_dispose_thunk(int32_t g) {
    Builder b(m);
    const SrcPos p{0, 0};
    const NodeId C = b.varref(VarKind::Capture, 0, p);
    Func f;
    f.name = "<dispose>";
    f.num_params = 0;
    f.num_locals = 1;
    f.local_names = {"$res"};
    f.num_captures = 1;
    f.capture_names = {"res"};
    f.lenient_arity = true;
    f.body = b.scope(
        0, 1,
        b.call_value(
            b.call_value(b.make_closure(rt.at("$mfind"), empty_cmap, p),
                         {C, b.str_literal("Dispose", p)}, p),
            {C}, p),
        p);
    m.funcs[static_cast<size_t>(fns[static_cast<size_t>(g)].index)] =
        std::move(f);
  }

  NodeId emit_try(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    const Ast* cat = nullptr;
    const Ast* fin = nullptr;
    for (size_t i = 1; i < a.nodes.size(); ++i) {
      if (a.nodes[i]->tag == "catchcl"_) {
        cat = a.nodes[i].get();
      } else {
        fin = a.nodes[i].get();
      }
    }
    const int32_t mark = ctx.next_local;
    const int32_t exc = ctx.alloc_local("$exc");
    const NodeId E = b.varref(VarKind::Local, exc, p);
    std::vector<NodeId> out;
    if (fin != nullptr) {
      out.push_back(b.make_defer(emit_closure(fn_of.at(fin), ctx, p), p));
    }
    const NodeId body = emit_block(*a.nodes[0], ctx);
    NodeId handler = b.make_throw(E, p);
    if (cat != nullptr) {
      std::vector<NodeId> hs;
      for (const auto& n : cat->nodes) {
        if (n->tag == "ident"_) {
          hs.push_back(bind_decl(decl_of.at(n.get()), E, ctx, p));
        }
      }
      hs.push_back(emit_block(*cat->nodes.back(), ctx));
      handler = b.block(hs, p);
    }
    out.push_back(b.make_try(exc, body, handler, p));
    const int32_t end = ctx.next_local;
    ctx.next_local = mark;
    return b.scope(mark, end, b.block(out, p), p);
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
      case "literal"_: {
        const std::string t(a.token);
        if (t == "true") return b.bool_literal(true, p);
        if (t == "false") return b.bool_literal(false, p);
        return b.nil_literal(p);
      }
      case "ident"_: {
        const auto it = ref_of.find(&a);
        if (it != ref_of.end()) return read_var(it->second, ctx, p);
        if (field_refs.count(&a)) {
          return helper(ctx, "$getf",
                        {this_of(ctx, p),
                         b.str_literal(std::string(a.token), p)},
                        p);
        }
        if (prop_refs.count(&a)) {
          return emit_prop_get(this_of(ctx, p), std::string(a.token), ctx, p);
        }
        const auto sf = static_field_refs.find(&a);
        if (sf != static_field_refs.end()) {
          return helper(ctx, "$getf",
                        {read_var(classes[static_cast<size_t>(sf->second)].var,
                                  ctx, p),
                         b.str_literal(std::string(a.token), p)},
                        p);
        }
        fail(a, "'" + std::string(a.token) + "' is not a value here");
      }
      case "paren"_:
        return emit_expr(*a.nodes[0], ctx);
      case "castexp"_: {
        // Types are erased, so a cast to a class is the value it was
        // applied to -- sound here because nothing downstream reads a
        // type. The two numeric casts are not annotations, though: they
        // are operations C# performs, so they stay.
        const NodeId v = emit_expr(*a.nodes[1], ctx);
        const std::string t(a.nodes[0]->token);
        if (t == "int" || t == "long") {
          return b.intrinsic(IntrinsicId::ToInt, {v}, p);
        }
        if (t == "double" || t == "float") {
          return b.intrinsic(IntrinsicId::ToDouble, {v}, p);
        }
        return v;
      }
      case "notexp"_:
        return b.binary(BinOp::Eq, emit_expr(*a.nodes[0], ctx),
                        b.bool_literal(false, p), p);
      case "negexp"_:
        return b.unary(UnOp::Neg, emit_expr(*a.nodes[0], ctx), p);
      case "awaitexp"_: {
        if (!fns[static_cast<size_t>(ctx.fn)].is_async) {
          fail(a, "'await' outside an async method");
        }
        return helper(ctx, "$await", {emit_expr(*a.nodes[0], ctx)}, p);
      }
      case "newexp"_:
        return emit_new(a, ctx);
      case "cond"_:
        return b.make_if(emit_expr(*a.nodes[0], ctx),
                         emit_expr(*a.nodes[1], ctx),
                         emit_expr(*a.nodes[2], ctx), p);
      case "logor"_:
      case "logand"_: {
        const bool is_or = a.tag == "logor"_;
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i < a.nodes.size(); ++i) {
          acc = b.make_if(acc,
                          is_or ? b.bool_literal(true, p)
                                : emit_expr(*a.nodes[i], ctx),
                          is_or ? emit_expr(*a.nodes[i], ctx)
                                : b.bool_literal(false, p),
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
        // `/` and `%` truncate toward zero in C#, which is BinOp's own
        // rule -- so unlike every other front end here, nothing corrects
        // them.
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i + 1 < a.nodes.size(); i += 2) {
          const Ast& op = *a.nodes[i];
          const std::string t(op.token);
          const NodeId rhs = emit_expr(*a.nodes[i + 1], ctx);
          const SrcPos op_p = pos_of(op);
          if (t == "+") {
            acc = helper(ctx, "$add", {acc, rhs}, op_p);
          } else {
            const BinOp o = t == "-"   ? BinOp::Sub
                            : t == "*" ? BinOp::Mul
                            : t == "/" ? BinOp::Div
                                       : BinOp::Mod;
            acc = b.binary(o, acc, rhs, op_p);
          }
        }
        return acc;
      }
      case "assign"_:
        return emit_assign(a, ctx);
      case "postfix"_:
        return emit_postfix(a, a.nodes.size(), ctx);
      default:
        fail(a, "cannot evaluate " + a.name);
    }
  }

  NodeId emit_new(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    std::string tname(a.nodes[0]->token);
    const size_t lt = tname.find('<');
    const std::string base = lt == std::string::npos ? tname
                                                     : tname.substr(0, lt);
    std::vector<NodeId> args;
    const Ast* init = nullptr;
    for (size_t i = 1; i < a.nodes.size(); ++i) {
      if (a.nodes[i]->tag == "initlist"_) {
        init = a.nodes[i].get();
      } else {
        for (const auto& c : a.nodes[i]->nodes) {
          args.push_back(emit_expr(*c, ctx));
        }
      }
    }
    if (base == "List") {
      std::vector<NodeId> items;
      if (init != nullptr) {
        for (const auto& c : init->nodes) items.push_back(emit_expr(*c, ctx));
      }
      return b.array_lit(items, p);
    }
    if (base == "Dictionary") {
      return b.intrinsic(IntrinsicId::MapNew, {}, p);
    }
    if (base == "Exception") {
      return helper(ctx, "$mkexc",
                    {args.empty() ? b.str_literal("", p) : args[0]}, p);
    }
    const auto it = class_by_name.find(base);
    if (it == class_by_name.end()) fail(a, "unknown type '" + base + "'");
    const ClassInfo& ci = classes[static_cast<size_t>(it->second)];
    const int32_t t = ctx.alloc_local("$new");
    const NodeId T = b.varref(VarKind::Local, t, p);
    std::vector<NodeId> callargs{T};
    callargs.insert(callargs.end(), args.begin(), args.end());
    return b.block(
        {b.assign(VarKind::Local, t,
                  b.object_lit({{b.str_literal(kClassKey, p),
                                 read_var(ci.var, ctx, p)}}, p),
                  p),
         b.call_value(
             b.index(read_var(ci.var, ctx, p), b.str_literal(kCtorKey, p), p),
             callargs, p),
         T},
        p);
  }

  NodeId emit_postfix(const Ast& a, size_t limit, FnCtx& ctx) {
    Builder b(m);
    const Ast& prim = *a.nodes[0];
    size_t i = 1;
    NodeId cur;
    if (prim.tag == "ident"_ && method_refs.count(&prim)) {
      // An implicit `this.` method call, which goes through the same
      // lookup an explicit one does -- so a base class calling a method
      // the derived class overrode reaches the override.
      if (i >= limit || a.nodes[i]->tag != "callsfx"_) {
        fail(prim, "a method group is not a value here");
      }
      cur = emit_method(this_of(ctx, pos_of(prim)), std::string(prim.token),
                        *a.nodes[i], ctx, pos_of(prim));
      ++i;
    } else if (prim.tag == "ident"_ && is_static_call(a, limit)) {
      // `Klass.Static(...)`. The class name *does* resolve to a variable
      // (the table), so this shape has to be recognized before the
      // generic path turns it into a method lookup on the table itself.
      cur = emit_library(a, limit, ctx, i);
    } else if (prim.tag == "ident"_ && !ref_of.count(&prim) &&
               !field_refs.count(&prim) && !static_field_refs.count(&prim)) {
      cur = emit_library(a, limit, ctx, i);
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
            cur = emit_method(cur, name, *a.nodes[i + 1], ctx, p);
            ++i;
            break;
          }
          // A user-declared property wins before the four built-in ones,
          // exactly as an instance method wins over a library name.
          if (property_names.count(name)) {
            cur = emit_prop_get(cur, name, ctx, p);
            break;
          }
          // `.Count`, `.Length`, `.Message` and `.Result` are the four
          // properties this subset has; everything else is a field.
          if (name == "Count" || name == "Length") {
            cur = helper(ctx, "$len", {cur}, p);
          } else if (name == "Message") {
            cur = helper(ctx, "$str", {cur}, p);
          } else if (name == "Result") {
            cur = helper(ctx, "$await", {cur}, p);
          } else {
            cur = helper(ctx, "$getf", {cur, b.str_literal(name, p)}, p);
          }
          break;
        }
        case "indexsfx"_:
          cur = helper(ctx, "$idx", {cur, emit_expr(*sfx.nodes[0], ctx)}, p);
          break;
        case "callsfx"_: {
          std::vector<NodeId> args{b.nil_literal(p)};
          for (const auto& c : sfx.nodes[0]->nodes) {
            args.push_back(emit_expr(*c, ctx));
          }
          cur = b.call_value(cur, args, p);
          break;
        }
        default:
          fail(sfx, "'++'/'--' is only supported as a statement");
      }
    }
    return cur;
  }

  // `obj.Method(...)`, `obj.Prop`, and `obj.Prop = v` all end in the same
  // `$mfind` walk on the receiver's actual class -- stash the receiver
  // once, then dispatch under whichever name the caller is after. This is
  // also why an override, on a method or an accessor, needs no code of
  // its own to win: the walk always starts at the receiver's own table.
  NodeId emit_dispatch(NodeId recv, const std::string& name,
                       const std::vector<NodeId>& args, FnCtx& ctx, SrcPos p) {
    Builder b(m);
    const int32_t t = ctx.alloc_local("$recv");
    const NodeId T = b.varref(VarKind::Local, t, p);
    std::vector<NodeId> callargs{T};
    callargs.insert(callargs.end(), args.begin(), args.end());
    return b.block(
        {b.assign(VarKind::Local, t, recv, p),
         b.call_value(helper(ctx, "$mfind", {T, b.str_literal(name, p)}, p),
                      callargs, p)},
        p);
  }

  NodeId emit_method(NodeId recv, const std::string& name, const Ast& call,
                     FnCtx& ctx, SrcPos p) {
    Builder b(m);
    std::vector<NodeId> args;
    for (const auto& c : call.nodes[0]->nodes) args.push_back(emit_expr(*c, ctx));
    const auto a0 = [&](size_t k) {
      return k < args.size() ? args[k] : b.nil_literal(p);
    };
    if (name == "Add") {
      return b.intrinsic(IntrinsicId::ArrayPush, {recv, a0(0)}, p);
    }
    if (name == "Wait") return helper(ctx, "$await", {recv}, p);
    if (name == "ToString") return helper(ctx, "$str", {recv}, p);
    // An ordinary method call: the lookup walks the class chain, which is
    // what makes an override win.
    return emit_dispatch(recv, name, args, ctx, p);
  }

  NodeId emit_prop_get(NodeId recv, const std::string& name, FnCtx& ctx,
                       SrcPos p) {
    return emit_dispatch(recv, "get_" + name, {}, ctx, p);
  }

  NodeId emit_prop_set(NodeId recv, const std::string& name, NodeId value,
                       FnCtx& ctx, SrcPos p) {
    return emit_dispatch(recv, "set_" + name, {value}, ctx, p);
  }

  bool is_static_call(const Ast& a, size_t limit) const {
    if (limit < 3) return false;
    if (a.nodes[1]->tag != "membersfx"_ || a.nodes[2]->tag != "callsfx"_) {
      return false;
    }
    if (!class_by_name.count(std::string(a.nodes[0]->token))) return false;
    return static_by_name.count(std::string(a.nodes[1]->nodes[0]->token)) > 0;
  }

  NodeId emit_library(const Ast& a, size_t limit, FnCtx& ctx, size_t& i) {
    Builder b(m);
    const Ast& prim = *a.nodes[0];
    const SrcPos p = pos_of(prim);
    const std::string g(prim.token);
    std::string mem;
    const Ast* call = nullptr;
    if (i + 1 < limit && a.nodes[i]->tag == "membersfx"_ &&
        a.nodes[i + 1]->tag == "callsfx"_) {
      mem = std::string(a.nodes[i]->nodes[0]->token);
      call = a.nodes[i + 1].get();
      i += 2;
    } else {
      fail(prim, "'" + g + "' is not supported in this shape here");
    }
    std::vector<NodeId> args;
    for (const auto& c : call->nodes[0]->nodes) {
      args.push_back(emit_expr(*c, ctx));
    }
    const auto a0 = [&](size_t k) {
      return k < args.size() ? args[k] : b.nil_literal(p);
    };
    if (g == "Console" && (mem == "WriteLine" || mem == "Write")) {
      NodeId s = args.empty() ? b.str_literal("", p)
                              : helper(ctx, "$str", {args[0]}, p);
      if (mem == "WriteLine") {
        s = b.binary(BinOp::Add, s, b.str_literal("\n", p), p);
      }
      return native("write", {s}, p);
    }
    if ((g == "string" || g == "String") && mem == "Join") {
      return helper(ctx, "$join", {helper(ctx, "$str", {a0(0)}, p), a0(1)}, p);
    }
    if (g == "Task" && (mem == "Yield" || mem == "CompletedTask")) {
      return helper(ctx, "$totask", {b.nil_literal(p)}, p);
    }
    if (g == "Task" && mem == "FromResult") {
      return helper(ctx, "$totask", {a0(0)}, p);
    }
    // A static method reached through its class name: `Probe.Helper(x)`.
    if (class_by_name.count(g)) {
      const auto sv = static_by_name.find(mem);
      if (sv != static_by_name.end()) {
        std::vector<NodeId> ca{b.nil_literal(p)};
        ca.insert(ca.end(), args.begin(), args.end());
        return b.call_value(read_var(sv->second, ctx, p), ca, p);
      }
    }
    fail(prim, "'" + g + "." + mem + "' is not supported here");
  }


  template <typename Combine>
  NodeId emit_store(const Ast& target, size_t limit, FnCtx& ctx, SrcPos p,
                    Combine combine) {
    Builder b(m);
    if (target.tag == "postfix"_ && limit == 1) {
      return emit_store(*target.nodes[0], 0, ctx, p, combine);
    }
    if (target.tag == "ident"_) {
      const auto it = ref_of.find(&target);
      if (it != ref_of.end()) {
        const int32_t v = it->second;
        const auto read = [&] { return read_var(v, ctx, p); };
        return write_var(v, combine(read), ctx, p);
      }
      if (field_refs.count(&target) || static_field_refs.count(&target)) {
        const NodeId key = b.str_literal(std::string(target.token), p);
        const auto sf = static_field_refs.find(&target);
        const NodeId owner =
            sf != static_field_refs.end()
                ? read_var(classes[static_cast<size_t>(sf->second)].var, ctx, p)
                : this_of(ctx, p);
        const int32_t t = ctx.alloc_local("$owner");
        const NodeId T = b.varref(VarKind::Local, t, p);
        const auto read = [&] { return helper(ctx, "$getf", {T, key}, p); };
        return b.block({b.assign(VarKind::Local, t, owner, p),
                        helper(ctx, "$setf", {T, key, combine(read)}, p)},
                       p);
      }
      if (prop_refs.count(&target)) {
        const std::string name(target.token);
        const NodeId self = this_of(ctx, p);
        const auto read = [&] { return emit_prop_get(self, name, ctx, p); };
        return emit_prop_set(self, name, combine(read), ctx, p);
      }
      fail(target, "cannot assign to this");
    }
    if (target.tag != "postfix"_ || limit < 2) {
      fail(target, "cannot assign to this expression");
    }
    const Ast& last = *target.nodes[limit - 1];
    const bool member = last.tag == "membersfx"_;
    if (!member && last.tag != "indexsfx"_) {
      fail(last, "cannot assign to this expression");
    }
    if (member && property_names.count(std::string(last.nodes[0]->token))) {
      const std::string name(last.nodes[0]->token);
      const int32_t tr = ctx.alloc_local("$recv");
      const NodeId recv = emit_postfix(target, limit - 1, ctx);
      const NodeId R = b.varref(VarKind::Local, tr, p);
      const auto read = [&] { return emit_prop_get(R, name, ctx, p); };
      return b.block({b.assign(VarKind::Local, tr, recv, p),
                      emit_prop_set(R, name, combine(read), ctx, p)},
                     p);
    }
    const NodeId key =
        member ? b.str_literal(std::string(last.nodes[0]->token), p)
               : emit_expr(*last.nodes[0], ctx);
    const int32_t tr = ctx.alloc_local("$recv");
    const int32_t tk = ctx.alloc_local("$key");
    const NodeId recv = emit_postfix(target, limit - 1, ctx);
    const NodeId R = b.varref(VarKind::Local, tr, p);
    const NodeId K = b.varref(VarKind::Local, tk, p);
    const auto read = [&] {
      return helper(ctx, member ? "$getf" : "$idx", {R, K}, p);
    };
    return b.block({b.assign(VarKind::Local, tr, recv, p),
                    b.assign(VarKind::Local, tk, key, p),
                    helper(ctx, member ? "$setf" : "$setidx",
                           {R, K, combine(read)}, p)},
                   p);
  }

  NodeId emit_assign(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    const std::string op(a.nodes[1]->token);
    const Ast& target = *a.nodes[0];
    const size_t limit = target.tag == "postfix"_ ? target.nodes.size() : 0;
    return emit_store(target, limit, ctx, p, [&](auto&& cur) -> NodeId {
      const NodeId v = emit_expr(*a.nodes[2], ctx);
      if (op == "=") return v;
      if (op == "+=") return helper(ctx, "$add", {cur(), v}, p);
      const BinOp o = op == "-="   ? BinOp::Sub
                      : op == "*=" ? BinOp::Mul
                      : op == "/=" ? BinOp::Div
                                   : BinOp::Mod;
      return b.binary(o, cur(), v, p);
    });
  }

  // -- One function's body -------------------------------------------------
  void emit_fn(int32_t f) {
    const FnInfo& fi = fns[static_cast<size_t>(f)];
    if (fi.synth) return;  // a `using`'s dispose thunk
    if (f == 0) return;    // the file scope, built by build()
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

    std::vector<NodeId> body;
    // A constructor runs its class's field initializers and its `: base(...)`
    // call before its own body.
    if (fi.cls >= 0 && classes[static_cast<size_t>(fi.cls)].ctor_fn == f) {
      emit_ctor_prologue(classes[static_cast<size_t>(fi.cls)], ctx, body, p);
    }
    body.push_back(emit_block(*fi.body, ctx));

    std::vector<NodeId> stmts;
    stmts.insert(stmts.end(), pre.begin(), pre.end());
    stmts.insert(stmts.end(), body.begin(), body.end());
    stmts.push_back(b.make_return(b.nil_literal(p), p));

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
    fn.body = b.scope(0, nparams, b.block(stmts, p), p);
    if (!fi.is_async) {
      m.funcs[static_cast<size_t>(fi.index)] = std::move(fn);
      return;
    }
    emit_async(fi, std::move(fn), nparams, p);
  }

  void emit_ctor_prologue(const ClassInfo& ci, FnCtx& ctx,
                          std::vector<NodeId>& out, SrcPos p) {
    Builder b(m);
    for (const Ast* fd : ci.fields) {
      const Ast* nameNode = nullptr;
      const Ast* initNode = nullptr;
      for (const auto& x : fd->nodes) {
        if (x->tag == "fieldinit"_) {
          initNode = x->nodes[0].get();
          break;
        }
        nameNode = x.get();
      }
      if (nameNode == nullptr) continue;
      out.push_back(helper(
          ctx, "$setf",
          {this_of(ctx, p), b.str_literal(std::string(nameNode->token), p),
           initNode != nullptr ? emit_expr(*initNode, ctx)
                               : b.nil_literal(p)},
          p));
    }
    for (const auto& [name, init] : ci.auto_props) {
      out.push_back(helper(
          ctx, "$setf",
          {this_of(ctx, p), b.str_literal(auto_field_key(name), p),
           init != nullptr ? emit_expr(*init, ctx) : b.nil_literal(p)},
          p));
    }
    if (ci.ctor == nullptr || ci.base.empty()) return;
    for (const auto& n : ci.ctor->nodes) {
      if (n->tag != "baseinit"_) continue;
      const auto it = class_by_name.find(ci.base);
      if (it == class_by_name.end()) break;
      std::vector<NodeId> args{this_of(ctx, p)};
      for (const auto& x : n->nodes[0]->nodes) args.push_back(emit_expr(*x, ctx));
      out.push_back(b.call_value(
          b.index(read_var(classes[static_cast<size_t>(it->second)].var, ctx,
                           p),
                  b.str_literal(kCtorKey, p), p),
          args, p));
    }
  }

  // An async method is three funcs, exactly as in examples/mini-js: the
  // one the source declared (which builds a Task, spawns and returns it),
  // the body, and the coroutine entry that settles the Task.
  void emit_async(const FnInfo& fi, Func body, int32_t nparams, SrcPos p) {
    Builder b(m);
    body.name = fi.name + "$body";
    const int32_t ncaps = body.num_captures;
    const std::vector<std::string> capnames = body.capture_names;
    const int32_t body_idx = static_cast<int32_t>(m.funcs.size());
    m.funcs.push_back(std::move(body));
    const int32_t run_idx = static_cast<int32_t>(m.funcs.size());
    m.funcs.push_back({});

    const auto rtcall = [&](const std::string& name,
                            const std::vector<NodeId>& args) {
      return b.call_value(b.make_closure(rt.at(name), empty_cmap, p), args, p);
    };

    std::vector<NodeId> launch{
        b.cell_fresh(0, p),
        b.assign(VarKind::Cell, 0, rtcall("$tnew", {}), p)};
    std::vector<CaptureSrc> fwd;
    for (int32_t i = 0; i < ncaps; ++i) fwd.push_back({VarKind::Capture, i});
    const int32_t fwd_cmap = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back(fwd);
    launch.push_back(b.cell_fresh(1, p));
    launch.push_back(b.assign(VarKind::Cell, 1,
                              b.make_closure(body_idx, fwd_cmap, p), p));
    std::vector<CaptureSrc> runcap{{VarKind::Cell, 1}, {VarKind::Cell, 0}};
    for (int32_t i = 0; i < nparams; ++i) {
      launch.push_back(b.cell_fresh(2 + i, p));
      launch.push_back(b.assign(VarKind::Cell, 2 + i,
                                b.varref(VarKind::Local, i, p), p));
      runcap.push_back({VarKind::Cell, 2 + i});
    }
    const int32_t run_cmap = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back(runcap);
    // CoroResume, not Enqueue: an async method runs synchronously up to
    // its first `await`, which is C#'s rule as well as JavaScript's.
    launch.push_back(b.intrinsic(
        IntrinsicId::CoroResume,
        {b.intrinsic(IntrinsicId::CoroCreate,
                     {b.make_closure(run_idx, run_cmap, p)}, p),
         b.nil_literal(p)},
        p));
    launch.push_back(b.make_return(b.varref(VarKind::Cell, 0, p), p));

    Func f;
    f.name = fi.name;
    f.num_params = nparams;
    f.num_locals = nparams;
    for (const int32_t v : fi.params) {
      f.local_names.push_back(vars[static_cast<size_t>(v)].name);
    }
    f.local_names.resize(static_cast<size_t>(nparams), "arg");
    f.num_cells = 2 + nparams;
    f.num_captures = ncaps;
    f.capture_names = capnames;
    f.lenient_arity = true;
    f.body = b.scope(0, nparams, b.block(launch, p), p);
    m.funcs[static_cast<size_t>(fi.index)] = std::move(f);

    std::vector<NodeId> callargs;
    for (int32_t i = 0; i < nparams; ++i) {
      callargs.push_back(b.varref(VarKind::Capture, 2 + i, p));
    }
    Func run;
    run.name = fi.name + "$run";
    run.num_params = 0;
    run.num_locals = 1;
    run.local_names = {"$exc"};
    run.num_captures = 2 + nparams;
    run.capture_names.push_back("$body");
    run.capture_names.push_back("$task");
    for (int32_t i = 0; i < nparams; ++i) run.capture_names.push_back("arg");
    run.lenient_arity = true;
    run.body = b.scope(
        0, 1,
        b.make_try(0,
                   rtcall("$tresolve",
                          {b.varref(VarKind::Capture, 1, p),
                           b.call_value(b.varref(VarKind::Capture, 0, p),
                                        callargs, p)}),
                   rtcall("$tsettle", {b.varref(VarKind::Capture, 1, p),
                                       b.literal(2, p),
                                       b.varref(VarKind::Local, 0, p)}),
                   p),
        p);
    m.funcs[static_cast<size_t>(run_idx)] = std::move(run);
  }

  Module build(const Ast& program) {
    resolve_program(program);

    m.funcs.push_back({});
    fns[0].index = 0;
    for (const std::string& n : rt_names()) {
      rt[n] = static_cast<int32_t>(m.funcs.size());
      m.funcs.push_back({});
    }
    const size_t declared = fns.size();
    for (size_t f = 1; f < declared; ++f) {
      fns[f].index = static_cast<int32_t>(m.funcs.size());
      m.funcs.push_back({});
    }

    // Every static method's binding, and the Main one, must be a cell:
    // the file scope hands them to functions that are not lexically
    // inside it.
    for (const auto& c : program.nodes) {
      if (c->tag != "classdecl"_) continue;
      for (const auto& mem : c->nodes) {
        if (mem->tag != "methoddecl"_ || !has_mod(*mem, "static")) continue;
        const Ast* id = find_method_name(*mem);
        const int32_t v = decl_of.at(id);
        auto& own = fns[0].cell_index;
        if (!own.count(v)) own[v] = static_cast<int32_t>(own.size());
      }
    }

    // One binding, owned by file scope and captured by every function: the
    // array of runtime-helper closures. Declared after resolution so no
    // source name can collide with it, and before number_captures so the
    // ordinary capture machinery threads it like any other free variable.
    helpers_var = static_cast<int32_t>(vars.size());
    vars.push_back({"$helpers", 0});
    for (size_t f = 1; f < fns.size(); ++f) fns[f].free.insert(helpers_var);
    // A cell whether or not anything captured it: file scope reads it
    // itself, and a program with no nested function has no free set to put
    // it in. number_captures below finds it already there.
    fns[0].cell_index[helpers_var] =
        static_cast<int32_t>(fns[0].cell_index.size());

    number_captures();
    empty_cmap = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back({});
    emit_runtime();

    slot_of.assign(vars.size(), -1);
    for (size_t f = 1; f < declared; ++f) {
      emit_fn(static_cast<int32_t>(f));
    }
    emit_file_scope(program);
    return std::move(m);
  }

  // funcs[0]: build every class table, bind every static method, then
  // spawn Main as a coroutine -- the same bootstrap examples/mini-go uses
  // for Go's `main`, and for the same reason: a CoroYield in the entry
  // frame would have no coroutine to suspend, so `await` and `.Wait()`
  // would have nowhere to park.
  void emit_file_scope(const Ast& program) {
    FnCtx ctx;
    ctx.fn = 0;
    ctx.next_cell = static_cast<int32_t>(fns[0].cell_index.size());
    Builder b(m);
    const SrcPos p = pos_of(program);
    std::vector<NodeId> out;
    for (const auto& [v, c] : fns[0].cell_index) {
      (void)v;
      out.push_back(b.cell_fresh(c, p));
    }
    // After the freshes above, which would otherwise reset it.
    fill_helpers(ctx, out, p);
    // The tables first, then their `\x02base` links, so a class may name
    // one declared after it.
    for (const ClassInfo& ci : classes) {
      std::vector<std::pair<NodeId, NodeId>> kvs{
          {b.str_literal(kNameKey, p), b.str_literal(ci.name, p)}};
      for (const auto& [name, g] : ci.methods) {
        kvs.emplace_back(b.str_literal(std::string(kMethodPrefix) + name, p),
                         emit_closure(g, ctx, p));
      }
      for (const auto& [name, init] : ci.auto_props) {
        (void)init;
        kvs.emplace_back(
            b.str_literal(std::string(kMethodPrefix) + "get_" + name, p),
            emit_auto_get(name, p));
        kvs.emplace_back(
            b.str_literal(std::string(kMethodPrefix) + "set_" + name, p),
            emit_auto_set(name, p));
      }
      for (const Ast* fd : ci.static_fields) {
        const Ast* nameNode = nullptr;
        const Ast* initNode = nullptr;
        for (const auto& x : fd->nodes) {
          if (x->tag == "fieldinit"_) {
            initNode = x->nodes[0].get();
            break;
          }
          nameNode = x.get();
        }
        if (nameNode == nullptr) continue;
        kvs.emplace_back(b.str_literal(std::string(nameNode->token), p),
                         initNode != nullptr ? emit_expr(*initNode, ctx)
                                             : b.nil_literal(p));
      }
      kvs.emplace_back(b.str_literal(kCtorKey, p),
                       ci.ctor_fn >= 0
                           ? emit_closure(ci.ctor_fn, ctx, p)
                           : emit_default_ctor(ci, ctx, p));
      out.push_back(write_var(ci.var, b.object_lit(kvs, p), ctx, p));
    }
    for (const ClassInfo& ci : classes) {
      if (ci.base.empty()) continue;
      const auto it = class_by_name.find(ci.base);
      if (it == class_by_name.end()) continue;
      out.push_back(b.set_index(
          read_var(ci.var, ctx, p), b.str_literal(kBaseKey, p),
          read_var(classes[static_cast<size_t>(it->second)].var, ctx, p), p));
    }
    for (const auto& [name, v] : static_by_name) {
      (void)name;
      // The static's own function; its index was assigned in build().
      out.push_back(write_var(v, emit_closure(static_fn.at(v), ctx, p), ctx, p));
    }
    if (main_fn < 0) coreir_rt::fail("no static Main method", 0, 0);
    const int32_t main_var = static_by_name.at("Main");
    const auto [mk, mi] = access(0, main_var);
    std::vector<CaptureSrc> cs{{mk, mi}};
    const int32_t cm = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back(cs);
    out.push_back(b.intrinsic(
        IntrinsicId::Enqueue,
        {b.intrinsic(IntrinsicId::CoroCreate,
                     {b.make_closure(rt.at("$mainwrap"), cm, p)}, p)},
        p));


    Func f;
    f.name = "main";
    f.num_params = 0;
    f.num_locals = ctx.high_local;
    ctx.local_names.resize(static_cast<size_t>(ctx.high_local), "");
    f.local_names = ctx.local_names;
    f.num_cells = ctx.next_cell;
    f.lenient_arity = true;
    f.body = b.scope(0, 0, b.block(out, p), p);
    m.funcs[0] = std::move(f);
  }

  // An auto property's accessors touch nothing but their own parameters,
  // so unlike a source method they need no capture at all -- built once
  // per property, the same way emit_default_ctor builds a missing
  // constructor.
  NodeId emit_auto_get(const std::string& name, SrcPos p) {
    Builder b(m);
    const int32_t idx = static_cast<int32_t>(m.funcs.size());
    m.funcs.push_back({});
    Func f;
    f.name = "get_" + name;
    f.num_params = 1;
    f.num_locals = 1;
    f.local_names = {"this"};
    f.lenient_arity = true;
    f.body = b.scope(
        0, 1,
        b.make_return(
            b.index(b.varref(VarKind::Local, 0, p),
                    b.str_literal(auto_field_key(name), p), p),
            p),
        p);
    m.funcs[static_cast<size_t>(idx)] = std::move(f);
    return b.make_closure(idx, empty_cmap, p);
  }

  NodeId emit_auto_set(const std::string& name, SrcPos p) {
    Builder b(m);
    const int32_t idx = static_cast<int32_t>(m.funcs.size());
    m.funcs.push_back({});
    Func f;
    f.name = "set_" + name;
    f.num_params = 2;
    f.num_locals = 2;
    f.local_names = {"this", "value"};
    f.lenient_arity = true;
    f.body = b.scope(
        0, 2,
        b.block({b.set_index(
                     b.varref(VarKind::Local, 0, p),
                     b.str_literal(auto_field_key(name), p),
                     b.varref(VarKind::Local, 1, p), p),
                 b.make_return(b.nil_literal(p), p)},
                p),
        p);
    m.funcs[static_cast<size_t>(idx)] = std::move(f);
    return b.make_closure(idx, empty_cmap, p);
  }

  NodeId emit_default_ctor(const ClassInfo& ci, FnCtx& ctx, SrcPos p) {
    // A class with no constructor still has to initialize its fields and
    // call its base's, so one is synthesized with the same shape.
    Builder b(m);
    const int32_t g = new_fn(0, ci.name + ".ctor");
    fns[static_cast<size_t>(g)].index = static_cast<int32_t>(m.funcs.size());
    m.funcs.push_back({});
    FnCtx cc;
    cc.fn = g;
    const int32_t self = cc.alloc_local("this");
    std::vector<NodeId> body;
    const NodeId T = b.varref(VarKind::Local, self, p);
    int32_t ncaps = 0;
    if (!ci.base.empty() && class_by_name.count(ci.base)) {
      // The default constructor still chains: `class B : A { }` runs A's.
      body.push_back(b.call_value(
          b.index(b.varref(VarKind::Capture, 0, p),
                  b.str_literal(kCtorKey, p), p),
          {T}, p));
      ncaps = 1;
    }
    for (const Ast* fd : ci.fields) {
      const Ast* nameNode = nullptr;
      for (const auto& x : fd->nodes) {
        if (x->tag == "fieldinit"_) break;
        nameNode = x.get();
      }
      if (nameNode == nullptr) continue;
      body.push_back(b.call_value(
          b.make_closure(rt.at("$setf"), empty_cmap, p),
          {T, b.str_literal(std::string(nameNode->token), p),
           b.nil_literal(p)},
          p));
    }
    // A default constructor runs no source expression, so like a plain
    // field's initializer above, an auto property's is skipped too --
    // both need an explicit constructor to take effect.
    for (const auto& [name, init] : ci.auto_props) {
      (void)init;
      body.push_back(b.call_value(
          b.make_closure(rt.at("$setf"), empty_cmap, p),
          {T, b.str_literal(auto_field_key(name), p),
           b.nil_literal(p)},
          p));
    }
    body.push_back(b.make_return(b.nil_literal(p), p));
    Func f;
    f.name = ci.name + ".ctor";
    f.num_params = 1;
    f.num_locals = 1;
    f.local_names = {"this"};
    f.num_captures = ncaps;
    for (int32_t i = 0; i < ncaps; ++i) f.capture_names.push_back("base");
    f.lenient_arity = true;
    f.body = b.scope(0, 1, b.block(body, p), p);
    m.funcs[static_cast<size_t>(fns[static_cast<size_t>(g)].index)] =
        std::move(f);
    if (ncaps == 0) {
      return b.make_closure(fns[static_cast<size_t>(g)].index, empty_cmap, p);
    }
    const auto [bk, bi] =
        access(0, classes[static_cast<size_t>(class_by_name.at(ci.base))].var);
    std::vector<CaptureSrc> cs{{bk, bi}};
    const int32_t cm = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back(cs);
    (void)ctx;
    return b.make_closure(fns[static_cast<size_t>(g)].index, cm, p);
  }
};

bool nat_write(NativeCall& c) {
  const std::string& s = c.arg(0).as_str();
  coreir_rt_out_raw(s.data(), static_cast<int64_t>(s.size()));
  c.result = Value();
  return true;
}

}  // namespace

const std::vector<vm::NativeDef>& stdlib() {
  static const std::vector<vm::NativeDef> defs = {
      {"write", 1, nat_write, nullptr},
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

}  // namespace mini_csharp
