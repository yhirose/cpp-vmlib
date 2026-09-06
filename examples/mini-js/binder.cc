// This binder exists to prove the recipes a *dynamically*-typed front end
// needs, against a real oracle (`node`), the way examples/mini-go proves
// the managed, statically-typed ones against `go run`. The top-level
// README's Scope section calls a JavaScript subset "the easy case" for
// this library; this is that claim turned into running code. What it
// leans on, section by section:
//
//   * **Closures.** JavaScript's own scoping -- a variable a nested
//     function reads outlives the frame that declared it -- is what
//     VarKind::Cell and the per-site capture map exist for. Which
//     declarations get promoted to cells is this binder's analysis
//     (resolve_*), and `let` in a loop body gets its per-iteration
//     binding from CellFresh, which is exactly what JavaScript promises
//     and what `var` (deliberately absent here) does not.
//   * **Exceptions.** `try`/`catch` is Tag::TryCatch; `finally` is a
//     Defer inside the Scope wrapping the try, so it runs on every exit
//     -- falling through, `return`, `break`, or an unwinding throw --
//     rather than being duplicated down each path.
//   * **Generators.** `function*` is Func::is_generator and `yield` is
//     Tag::Yield; `g.next()` is GenResume, whose {value, done} answer is
//     already JavaScript's own iterator-result shape, so the protocol
//     needs no translation at all.
//   * **Maps.** `Map` and `Set` are MapNew -- a value-keyed map, which is
//     what distinguishes them from an object in JavaScript too.
//   * **Strings and slices.** `.length`, indexing and `.slice` go to Len,
//     Index and StrSlice/ArraySlice, with JavaScript's own out-of-range
//     rule (`undefined`, not a trap) and negative-index normalization
//     written here rather than asked of the library.
//
// And what it deliberately does *not* lean on: the VM's own answers to
// the questions a language is supposed to answer for itself. Value::
// truthy() calls an empty string and NaN true, and the header says why --
// "JavaScript calls both falsy, Lua calls neither, and that disagreement
// is what makes it a language's decision rather than the VM's". So
// `$truthy` is written here, in IR, and every condition goes through it.
// The same for `===` (BinOp::Eq refuses two values of different types,
// which is a trap where JavaScript wants `false`), for `typeof` (TypeOf's
// vocabulary is the VM's, not JavaScript's), and for `String(n)` (to_display
// is shortest-round-trip, which is *nearly* ES Number::toString but not
// it). Each of those is a $-named func this binder writes once per module;
// the `$` keeps them out of the source language's reach.

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

namespace mini_js {
namespace {

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

// The source spelling of a string literal, quotes and all, as the bytes it
// stands for. Only the escapes a sample can reach are decoded; an unknown
// one keeps its own character, which is what JavaScript does for the ones
// it has no rule for.
std::string unescape(const std::string& tok, const Ast& at) {
  std::string out;
  for (size_t i = 1; i + 1 < tok.size(); ++i) {
    if (tok[i] != '\\') {
      out.push_back(tok[i]);
      continue;
    }
    if (++i + 1 > tok.size()) fail(at, "unterminated escape");
    switch (tok[i]) {
      case 'n': out.push_back('\n'); break;
      case 't': out.push_back('\t'); break;
      case 'r': out.push_back('\r'); break;
      case '0': out.push_back('\0'); break;
      case '\\': out.push_back('\\'); break;
      case '\'': out.push_back('\''); break;
      case '"': out.push_back('"'); break;
      default: out.push_back(tok[i]); break;
    }
  }
  return out;
}

// The names a program may use without declaring. Resolution does not fail
// on these -- emit_postfix recognizes each in the one shape this front end
// supports it in (`console.log(...)`, `Object.keys(...)`, `new Map()`) --
// and a name that is neither declared nor here is the "undefined" the
// binder reports rather than something the run would discover.
bool is_global(const std::string& n) {
  return n == "console" || n == "Object" || n == "Array" || n == "String" ||
         n == "Map" || n == "Set" || n == "Error" || n == "TypeError" ||
         n == "RangeError" || n == "Promise" || n == "NaN" ||
         n == "Infinity";
}

// -- The resolution model ---------------------------------------------------
//
// coreir::Resolver holds it: one Var per *declaration*, named by an index
// every reference to it resolves to, and per function the free set, the
// capture numbering and the cell numbering. That indirection is what lets
// pass A answer "is anything nested inside this function reading this
// variable?" -- the question that decides between a Local slot and a Cell --
// before any IR exists.
//
// What stays here is what the resolver has no opinion about: a function in
// the sense *this* source language means it -- a declaration, a function
// expression, an arrow, and, because it is compiled to a closure the Scope
// defers, a `finally` block.
struct FnInfo {
  bool is_generator = false;
  bool is_async = false;
  bool is_method = false;  // params[0] is `this`, synthetic -- no source ident
  std::string name = "<anon>";
  std::vector<int32_t> params;  // VarIds, in order
  const Ast* body = nullptr;    // the `block` (or an arrow's expression)
};

// A class declaration: the method table it builds, `constructor` singled
// out from the rest so `new` can find it without a name comparison at
// every instantiation.
struct ClassInfo {
  std::vector<std::pair<std::string, int32_t>> methods;
  int32_t ctor_fn = -1;
};

// While a body is being emitted: where the next local slot comes from, and
// how high the frame has to be.
struct FnCtx : FrameLayout {
  int32_t fn = 0;
  int32_t next_cell = 0;
};

struct Binder {
  Module m;
  Resolver rs;
  std::vector<FnInfo> fns;  // parallel to rs.fns
  std::map<const Ast*, int32_t> ref_of;   // an ident in expression position
  std::map<const Ast*, int32_t> decl_of;  // an ident in declaring position
  std::map<const Ast*, int32_t> fn_of;    // a function-shaped node
  std::map<std::string, int32_t> rt;      // $helper name -> Module::funcs index
  int32_t empty_cmap = -1;

  // ==== Pass A: scopes, declarations, captures =============================


  // `const` is the one fact about a binding this front end needs that
  // Resolver::Var does not carry, so it rides beside it, indexed by the
  // same VarId.
  std::vector<char> is_const;

  int32_t declare(const std::string& name, int32_t fn, bool constant,
                  const Ast& at) {
    if (rs.declared_here(name)) {
      fail(at, "'" + name + "' is already declared in this scope");
    }
    const int32_t v = rs.declare(name, fn);
    is_const.resize(rs.vars.size(), 0);
    is_const[static_cast<size_t>(v)] = constant ? 1 : 0;
    return v;
  }

  int32_t new_fn(int32_t parent, const std::string& name) {
    const int32_t f = rs.new_fn(parent);
    fns.push_back({});
    fns[static_cast<size_t>(f)].name = name;
    return f;
  }

  // A function declaration is visible everywhere in its block, including
  // above itself -- JavaScript hoists both the name and the initialization
  // to the top of the block, which is what makes two functions declared
  // side by side able to call each other. `let`/`const` are not hoisted
  // (their temporal dead zone makes an early read an error anyway), so
  // they are declared where they stand.
  void hoist_funcdecls(const std::vector<std::shared_ptr<Ast>>& stmts,
                       int32_t fn) {
    for (const auto& s : stmts) {
      if (s->tag != "funcdecl"_) continue;
      const Ast* id = find_child(*s, "ident");
      decl_of[id] = declare(std::string(id->token), fn, false, *id);
    }
  }

  void resolve_block(const Ast& block, int32_t fn) {
    rs.push_scope();
    hoist_funcdecls(block.nodes, fn);
    for (const auto& s : block.nodes) resolve_stmt(*s, fn);
    rs.pop_scope();
  }

  // A function-shaped node -- funcdecl, funcexpr, arrow, or a `finally`
  // block -- as its own FnInfo: a scope of its own for the parameters, and
  // the body resolved with `fn` pointing at it, so every reference the body
  // makes to anything outside lands in `free`.
  int32_t resolve_fn(const Ast& node, int32_t parent, const std::string& name,
                     const Ast* params, const Ast& body, bool is_generator,
                     bool is_async, bool is_method = false) {
    const int32_t f = new_fn(parent, name);
    fns[static_cast<size_t>(f)].is_generator = is_generator;
    fns[static_cast<size_t>(f)].is_async = is_async;
    fns[static_cast<size_t>(f)].is_method = is_method;
    fns[static_cast<size_t>(f)].body = &body;
    fn_of[&node] = f;

    rs.push_scope();
    if (is_method) {
      // `this`: synthetic, so there is no source identifier to hang a
      // decl_of entry on -- a method reaches it through `thisexpr`
      // instead, always at params[0].
      const int32_t tv = rs.declare("this", f);
      is_const.resize(rs.vars.size(), 0);
      fns[static_cast<size_t>(f)].params.push_back(tv);
    }
    const auto param = [&](const Ast& id) {
      const int32_t v = declare(std::string(id.token), f, false, id);
      decl_of[&id] = v;
      fns[static_cast<size_t>(f)].params.push_back(v);
    };
    if (params != nullptr) {
      // A "params" node lists them; a bare ident (an arrow written without
      // parentheses) is the one-parameter list itself.
      if (params->tag == "ident"_) {
        param(*params);
      } else {
        for (const auto& q : params->nodes) param(*q);
      }
    }
    if (body.tag == "block"_) {
      resolve_block(body, f);
    } else {
      resolve_expr(body, f);  // an arrow's expression body
    }
    rs.pop_scope();
    return f;
  }

  // No hoisting for a class: JavaScript's own rule (a class lives in the
  // temporal dead zone until its declaration runs), and simpler besides --
  // a plain `declare` in place is all `resolve_stmt` needs.
  std::map<const Ast*, ClassInfo> classes;
  // `new` only reaches a class it can see statically -- the identifier
  // resolves to a binding this map knows -- which is what lets `emit_new`
  // build a `this`-bound wrapper per method with an exact parameter count,
  // rather than a name it would have to trust at run time.
  std::map<int32_t, const Ast*> class_by_var;

  void resolve_classdecl(const Ast& a, int32_t fn) {
    const std::string cname(a.nodes[0]->token);
    const int32_t declv = declare(cname, fn, false, *a.nodes[0]);
    decl_of[a.nodes[0].get()] = declv;
    class_by_var[declv] = &a;
    ClassInfo ci;
    for (size_t i = 1; i < a.nodes.size(); ++i) {
      const Ast& mem = *a.nodes[i];  // classmember: ident, params, block
      const std::string mname(mem.nodes[0]->token);
      const int32_t g =
          resolve_fn(mem, fn, cname + "." + mname, mem.nodes[1].get(),
                    *mem.nodes[2], false, false, /*is_method=*/true);
      if (mname == "constructor") {
        ci.ctor_fn = g;
      } else {
        ci.methods.emplace_back(mname, g);
      }
    }
    classes[&a] = std::move(ci);
  }

  void resolve_stmt(const Ast& a, int32_t fn) {
    switch (a.tag) {
      case "funcdecl"_: {
        // The name was hoisted; only the function itself is new here.
        const Ast* id = find_child(a, "ident");
        resolve_fn(a, fn, std::string(id->token), find_child(a, "params"),
                   *a.nodes.back(),
                   !find_child(a, "fnstar")->token.empty(),
                   !find_child(a, "fnasync")->token.empty());
        return;
      }
      case "classdecl"_:
        resolve_classdecl(a, fn);
        return;
      case "vardecl"_: {
        const Ast& kw = *a.nodes[0];
        const Ast& id = *a.nodes[1];
        // The initializer is resolved *before* the name is declared, so
        // `let x = x` reads whatever `x` meant outside (or fails) rather
        // than the slot this statement is about to create.
        if (a.nodes.size() > 2) resolve_expr(*a.nodes[2], fn);
        if (kw.token == "var") fail(kw, "'var' is not supported; use let or const");
        decl_of[&id] = declare(std::string(id.token), fn,
                               kw.token == "const", id);
        return;
      }
      case "block"_:
        resolve_block(a, fn);
        return;
      case "emptystmt"_:
      case "breakstmt"_:
      case "contstmt"_:
        return;
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
        // The head's own scope: `for (let i = 0; ...)` declares i for the
        // condition, the update and the body, and for nothing after.
        rs.push_scope();
        // forinit keeps its wrapper (no_ast_opt), so its one child -- a
        // vardeclbare or a plain expression -- is what to walk, and an
        // empty head simply has none.
        for (const auto& c : a.nodes[0]->nodes) {
          if (c->tag == "vardeclbare"_) {
            resolve_expr(*c->nodes[2], fn);
            decl_of[c->nodes[1].get()] =
                declare(std::string(c->nodes[1]->token), fn,
                        c->nodes[0]->token == "const", *c->nodes[1]);
          } else {
            resolve_expr(*c, fn);
          }
        }
        for (const auto& c : a.nodes[1]->nodes) resolve_expr(*c, fn);
        for (const auto& c : a.nodes[2]->nodes) resolve_expr(*c, fn);
        resolve_stmt(*a.nodes[3], fn);
        rs.pop_scope();
        return;
      }
      case "forof"_: {
        resolve_expr(*a.nodes[2], fn);
        rs.push_scope();
        decl_of[a.nodes[1].get()] =
            declare(std::string(a.nodes[1]->token), fn,
                    a.nodes[0]->token == "const", *a.nodes[1]);
        resolve_stmt(*a.nodes[3], fn);
        rs.pop_scope();
        return;
      }
      case "retstmt"_:
      case "throwstmt"_:
      case "exprstmt"_:
        for (const auto& c : a.nodes) resolve_expr(*c, fn);
        return;
      case "trystmt"_: {
        resolve_block(*a.nodes[0], fn);
        for (size_t i = 1; i < a.nodes.size(); ++i) {
          const Ast& c = *a.nodes[i];
          if (c.tag == "catchcl"_) {
            rs.push_scope();
            decl_of[c.nodes[0].get()] = declare(
                std::string(c.nodes[0]->token), fn, false, *c.nodes[0]);
            resolve_block(*c.nodes[1], fn);
            rs.pop_scope();
          } else {  // finallycl -- a function, because Defer takes a callable
            resolve_fn(c, fn, "<finally>", nullptr, *c.nodes[0], false, false);
          }
        }
        return;
      }
      default:
        fail(a, "cannot resolve statement " + a.name);
    }
  }

  void resolve_expr(const Ast& a, int32_t fn) {
    switch (a.tag) {
      case "number"_:
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
        fail(a, "undefined: " + n);
      }
      case "thisexpr"_: {
        // `this` is an ordinary scoped variable, `is_method` planted --
        // which is what makes an arrow inside a method inherit it lexically
        // through the same `rs.resolve()` every other closed-over name uses,
        // JavaScript's own rule for an arrow's `this`.
        const auto v = rs.resolve("this", fn);
        if (!v) fail(a, "'this' used outside a method");
        ref_of[&a] = *v;
        return;
      }
      case "arrow"_: {
        // `(a, b) => ...` gives a "params" node; `x => ...` gives the bare
        // ident, since there were no parentheses for one to come from.
        // resolve_fn takes either, so the two forms differ nowhere else.
        resolve_fn(a, fn, "<arrow>", a.nodes[0].get(), *a.nodes[1], false,
                   false);
        return;
      }
      case "funcexpr"_: {
        // children: fnasync, fnstar, [ident], params, block -- the name is
        // optional, so everything is read from the end rather than by a
        // fixed position.
        const Ast& params = *a.nodes[a.nodes.size() - 2];
        const Ast& body = *a.nodes.back();
        std::string name = "<anon>";
        if (a.nodes.size() == 5) name = std::string(a.nodes[2]->token);
        resolve_fn(a, fn, name, &params, body,
                   !a.nodes[1]->token.empty(), !a.nodes[0]->token.empty());
        return;
      }
      case "yieldexpr"_: {
        if (!a.nodes[0]->token.empty()) {
          fail(a, "'yield*' is not supported");
        }
        if (a.nodes.size() > 1) resolve_expr(*a.nodes[1], fn);
        return;
      }
      case "propdef"_:
        resolve_expr(*a.nodes[1], fn);
        return;
      case "objectlit"_:
        for (const auto& c : a.nodes) resolve_expr(*c, fn);
        return;
      default:
        for (const auto& c : a.nodes) {
          if (c->tag == "assignop"_ || c->tag == "eqop"_ ||
              c->tag == "relop"_ || c->tag == "addop"_ ||
              c->tag == "mulop"_ || c->tag == "incsfx"_ ||
              c->tag == "varkw"_ || c->tag == "membersfx"_) {
            continue;  // operators and member names are not references
          }
          resolve_expr(*c, fn);
        }
        return;
    }
  }

  // ==== The runtime this front end writes in its own IR ====================
  //
  // Every rule here is JavaScript's, not the VM's, which is exactly why it
  // lives in this file: the header's own comments say so for each of them
  // (Value::truthy on the empty string and NaN, eval_binop refusing to
  // compare across types, to_display's shortest-round-trip doubles). The
  // `$` prefix keeps these out of the source language's reach, the same
  // convention examples/mini-go's channel runtime uses.

  static const std::vector<std::string>& rt_names() {
    static const std::vector<std::string> names = {
        "$truthy", "$seq",     "$typeof",  "$numstr",   "$str",
        "$join",   "$add",     "$index",   "$setidx",   "$slice",
        "$push",   "$iter",    "$iternext", "$throwtype", "$minner",
        "$mget",   "$mset",    "$mhas",    "$mdel",     "$sadd",
        "$pnew",   "$ispromise", "$psettle", "$pon",    "$topromise",
        "$presolve", "$adopt", "$then",    "$react",    "$presolve1",
        "$preject1", "$await",   "$prejected",
    };
    return names;
  }

  // One runtime func under construction. The short names are deliberate:
  // these bodies are dense trees, and spelling `b.binary(BinOp::Eq, ...)`
  // at every node buries what the code says under how it is built.
  // The front end's own additions to coreir::FuncWriter: the helpers
  // that have to reach this binder's own tables.
  struct RT : FuncWriter {
    Binder& bd;

    explicit RT(Binder& bd_) : FuncWriter(bd_.m), bd(bd_) {}

    // A closure over this func's own cells: the capture map belongs to the
    // site building it, which is here.
    NodeId clos(const std::string& name, const std::vector<int32_t>& cells) {
      std::vector<CaptureSrc> cs;
      cs.reserve(cells.size());
      for (const int32_t c : cells) cs.push_back({VarKind::Cell, c});
      const int32_t cm = static_cast<int32_t>(bd.m.capture_maps.size());
      bd.m.capture_maps.push_back(cs);
      return b.make_closure(bd.rt.at(name), cm);
    }

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

  // ToBoolean, ES2023 7.1.2. The VM's own Value::truthy() is close but
  // deliberately not this -- it calls "" and NaN true, and its comment
  // names JavaScript as the reason it refuses to decide.
  void rt_truthy() {
    RT r(*this);
    const auto [v] = r.params("v");
    std::vector<std::pair<NodeId, NodeId>> arms;
    arms.emplace_back(r.S("bool"), r.ret(r.L(v)));
    arms.emplace_back(r.S("int"), r.ret(r.bin(BinOp::Ne, r.L(v), r.I(0))));
    // NaN is the one double that is not equal to itself, and it is falsy.
    arms.emplace_back(
        r.S("double"),
        r.ret(r.iff(r.bin(BinOp::Eq, r.L(v), r.L(v)),
                    r.bin(BinOp::Ne, r.L(v), r.D(0.0)), r.Bo(false))));
    arms.emplace_back(r.S("string"),
                      r.ret(r.bin(BinOp::Gt, r.len(r.L(v)), r.I(0))));
    arms.emplace_back(r.S("nil"), r.ret(r.Bo(false)));
    r.add(r.b.make_switch(r.typ(r.L(v)), arms, r.ret(r.Bo(true))));
    r.finish("$truthy");
  }

  // `===`, ES2023 7.2.16. BinOp::Eq refuses two values of different types
  // -- "a question a language answers, not the VM", says eval_binop -- and
  // this is that answer: different types are unequal rather than an error,
  // and two objects are equal only when they are the same object, which is
  // what IntrinsicId::Same is for.
  void rt_seq() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [ta, tb] = r.locals("ta", "tb");
    r.add(r.set(ta, r.typ(r.L(a))));
    r.add(r.set(tb, r.typ(r.L(b))));
    r.add(r.iff(r.bin(BinOp::Ne, r.L(ta), r.L(tb)), r.ret(r.Bo(false))));
    const NodeId scalar = r.ret(r.bin(BinOp::Eq, r.L(a), r.L(b)));
    std::vector<std::pair<NodeId, NodeId>> arms;
    arms.emplace_back(r.S("nil"), r.ret(r.Bo(true)));
    for (const char* t : {"bool", "int", "double", "string"}) {
      arms.emplace_back(r.S(t), scalar);
    }
    r.add(r.b.make_switch(r.L(ta), arms,
                          r.ret(r.in(IntrinsicId::Same, {r.L(a), r.L(b)}))));
    r.finish("$seq");
  }

  // `typeof`, ES2023 13.5.3. TypeOf answers in the VM's own vocabulary
  // ("nil", "double", "array"); this is the mapping onto JavaScript's.
  void rt_typeof() {
    RT r(*this);
    const auto [v] = r.params("v");
    std::vector<std::pair<NodeId, NodeId>> arms;
    arms.emplace_back(r.S("nil"), r.ret(r.S("undefined")));
    arms.emplace_back(r.S("bool"), r.ret(r.S("boolean")));
    const NodeId num = r.ret(r.S("number"));
    arms.emplace_back(r.S("int"), num);
    arms.emplace_back(r.S("double"), num);
    arms.emplace_back(r.S("string"), r.ret(r.S("string")));
    arms.emplace_back(r.S("function"), r.ret(r.S("function")));
    r.add(r.b.make_switch(r.typ(r.L(v)), arms, r.ret(r.S("object"))));
    r.finish("$typeof");
  }

  // Number::toString, ES2023 6.1.6.1.20, for the range the samples reach.
  // ToStr is shortest-round-trip (to_display), which agrees with
  // JavaScript on 0.1 + 0.2 and on 3.5 but not on a whole number (it has
  // no reason to prefer "3" over "3E0") nor on the exponent forms at the
  // ends of the range -- so integral values within +-2^53 are printed
  // through the integer path instead, and the exponent forms are what
  // README.md lists as out of range for a sample.
  void rt_numstr() {
    const double inf = std::numeric_limits<double>::infinity();
    RT r(*this);
    const auto [d] = r.params("d");
    const auto [i] = r.locals("i");
    r.add(r.iff(r.bin(BinOp::Ne, r.L(d), r.L(d)), r.ret(r.S("NaN"))));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(d), r.D(inf)), r.ret(r.S("Infinity"))));
    r.add(r.iff(r.bin(BinOp::Eq, r.L(d), r.D(-inf)), r.ret(r.S("-Infinity"))));
    const double lim = 9007199254740992.0;  // 2^53
    r.add(r.iff(
        r.bin(BinOp::Gt, r.L(d), r.D(-lim)),
        r.iff(r.bin(BinOp::Lt, r.L(d), r.D(lim)),
              r.blk({r.set(i, r.in(IntrinsicId::ToInt, {r.L(d)})),
                     r.iff(r.bin(BinOp::Eq,
                                 r.in(IntrinsicId::ToDouble, {r.L(i)}), r.L(d)),
                           r.ret(r.in(IntrinsicId::ToStr, {r.L(i)})))}))));
    r.add(r.ret(r.in(IntrinsicId::ToStr, {r.L(d)})));
    r.finish("$numstr");
  }

  // String(v), ES2023 22.1.1.1 -- for the values this subset has.
  void rt_str() {
    RT r(*this);
    const auto [v] = r.params("v");
    std::vector<std::pair<NodeId, NodeId>> arms;
    arms.emplace_back(r.S("string"), r.ret(r.L(v)));
    arms.emplace_back(r.S("nil"), r.ret(r.S("undefined")));
    const NodeId tostr = r.ret(r.in(IntrinsicId::ToStr, {r.L(v)}));
    arms.emplace_back(r.S("bool"), tostr);
    arms.emplace_back(r.S("int"), tostr);
    arms.emplace_back(r.S("double"), r.ret(r.call("$numstr", {r.L(v)})));
    arms.emplace_back(r.S("function"), r.ret(r.S("[Function]")));
    // Array::toString is Array::join with ",", which is why these two are
    // mutually recursive -- and why every $helper's Module::funcs slot is
    // reserved before any body is built.
    arms.emplace_back(r.S("array"), r.ret(r.call("$join", {r.L(v), r.S(",")})));
    r.add(r.b.make_switch(r.typ(r.L(v)), arms, r.ret(r.S("[object Object]"))));
    r.finish("$str");
  }

  // Array::join, ES2023 23.1.3.18: undefined and null render as the empty
  // string rather than as "undefined".
  void rt_join() {
    RT r(*this);
    const auto [a, sep] = r.params("a", "sep");
    const auto [i, out, e] = r.locals("i", "out", "e");
    r.add(r.set(out, r.S("")));
    r.add(r.set(i, r.I(0)));
    r.add(r.b.make_while(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(a))),
        r.blk({
            r.iff(r.bin(BinOp::Gt, r.L(i), r.I(0)),
                  r.set(out, r.bin(BinOp::Add, r.L(out), r.L(sep)))),
            r.set(e, r.idx(r.L(a), r.L(i))),
            r.iff(r.isnt(r.typ(r.L(e)), "nil"),
                  r.set(out,
                        r.bin(BinOp::Add, r.L(out), r.call("$str", {r.L(e)})))),
            r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1))),
        })));
    r.add(r.ret(r.L(out)));
    r.finish("$join");
  }

  // `+`, ES2023 13.15.3: a string on either side makes it concatenation,
  // and everything else is numeric addition. The wider coercion ladder
  // (`[] + {}`, `1 + null`) is what README.md lists as out of scope.
  void rt_add() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const NodeId cat = r.ret(
        r.bin(BinOp::Add, r.call("$str", {r.L(a)}), r.call("$str", {r.L(b)})));
    r.add(r.iff(r.is(r.typ(r.L(a)), "string"), cat));
    r.add(r.iff(r.is(r.typ(r.L(b)), "string"), cat));
    r.add(r.ret(r.bin(BinOp::Add, r.L(a), r.L(b))));
    r.finish("$add");
  }

  void rt_throwtype() {
    RT r(*this);
    const auto [msg] = r.params("msg");
    r.add(r.b.make_throw(
        r.obj({{"name", r.S("TypeError")}, {"message", r.L(msg)}})));
    r.finish("$throwtype");
  }

  // Reading a property, member or indexed -- the same operation in
  // JavaScript, so the same func here. What the library will not do on its
  // own: an out-of-range array or string index is `undefined` rather than
  // the trap index_error() raises, `.length` is a property rather than an
  // intrinsic, and a key that is not an integral number in range is simply
  // absent.
  void rt_index() {
    RT r(*this);
    const auto [recv, key] = r.params("recv", "key");
    const auto [t, tk, i, n, k] = r.locals("t", "tk", "i", "n", "k");
    std::vector<std::pair<NodeId, NodeId>> arms;
    const NodeId seq = r.blk({
        r.set(tk, r.typ(r.L(key))),
        r.iff(r.is(r.L(tk), "string"),
              r.blk({r.iff(r.is(r.L(key), "length"),
                           r.ret(r.in(IntrinsicId::ToDouble,
                                      {r.len(r.L(recv))}))),
                     r.ret(r.Nil())})),
        r.iff(r.isnt(r.L(tk), "double"),
              r.iff(r.isnt(r.L(tk), "int"), r.ret(r.Nil()))),
        r.iff(r.bin(BinOp::Ne, r.L(key), r.L(key)), r.ret(r.Nil())),
        r.set(n, r.len(r.L(recv))),
        r.iff(r.bin(BinOp::Lt, r.L(key), r.I(0)), r.ret(r.Nil())),
        r.iff(r.bin(BinOp::Ge, r.L(key), r.in(IntrinsicId::ToDouble, {r.L(n)})),
              r.ret(r.Nil())),
        r.set(i, r.in(IntrinsicId::ToInt, {r.L(key)})),
        r.iff(r.bin(BinOp::Ne, r.in(IntrinsicId::ToDouble, {r.L(i)}), r.L(key)),
              r.ret(r.Nil())),
        r.ret(r.idx(r.L(recv), r.L(i))),
    });
    arms.emplace_back(r.S("array"), seq);
    arms.emplace_back(r.S("string"), seq);
    // A Map or a Set is an object wrapping the value-keyed map, so that
    // `typeof` can answer "object" (JavaScript's own answer) and the two
    // stay tellable apart -- which one vmlib Map alone could not do.
    arms.emplace_back(
        r.S("object"),
        r.blk({
            r.set(k, r.call("$str", {r.L(key)})),
            r.iff(
                r.is(r.L(k), "size"),
                r.blk({r.iff(r.in(IntrinsicId::ObjectHas,
                                  {r.L(recv), r.S("$map")}),
                             r.ret(r.in(IntrinsicId::ToDouble,
                                        {r.len(r.idx(r.L(recv), "$map"))}))),
                       r.iff(r.in(IntrinsicId::ObjectHas,
                                  {r.L(recv), r.S("$set")}),
                             r.ret(r.in(IntrinsicId::ToDouble,
                                        {r.len(r.idx(r.L(recv), "$set"))})))})),
            r.ret(r.idx(r.L(recv), r.L(k))),
        }));
    arms.emplace_back(
        r.S("nil"),
        r.ret(r.call("$throwtype",
                     {r.S("cannot read a property of undefined")})));
    r.add(r.b.make_switch(r.typ(r.L(recv)), arms, r.ret(r.Nil())));
    r.finish("$index");
  }

  void rt_setidx() {
    RT r(*this);
    const auto [recv, key, val] = r.params("recv", "key", "val");
    const auto [t, n, i] = r.locals("t", "n", "i");
    std::vector<std::pair<NodeId, NodeId>> arms;
    arms.emplace_back(
        r.S("array"),
        r.blk({r.set(n, r.len(r.L(recv))),
               r.set(i, r.in(IntrinsicId::ToInt, {r.L(key)})),
               r.iff(r.bin(BinOp::Eq, r.L(i), r.L(n)),
                     r.in(IntrinsicId::ArrayPush, {r.L(recv), r.L(val)}),
                     r.b.set_index(r.L(recv), r.L(i), r.L(val))),
               r.ret(r.L(val))}));
    arms.emplace_back(
        r.S("object"),
        r.blk({r.b.set_index(r.L(recv), r.call("$str", {r.L(key)}), r.L(val)),
               r.ret(r.L(val))}));
    arms.emplace_back(
        r.S("map"),
        r.blk({r.b.set_index(r.L(recv), r.L(key), r.L(val)), r.ret(r.L(val))}));
    r.add(r.b.make_switch(
        r.typ(r.L(recv)), arms,
        r.ret(r.call("$throwtype", {r.S("cannot set a property on this")}))));
    r.finish("$setidx");
  }

  // Array::prototype::slice / String::prototype::slice, ES2023 23.1.3.27
  // and 22.1.3.24: absent ends default to 0 and the length, a negative end
  // counts from the end, and both are clamped rather than refused --
  // slice_error() in the header names this exact difference ("a language
  // that clamps or counts from the end normalizes in its own lowering").
  void rt_slice() {
    RT r(*this);
    const auto [v, i, j] = r.params("v", "i", "j");
    const auto [n, a, b] = r.locals("n", "a", "b");
    const auto norm = [&](FuncWriter::Slot out, FuncWriter::Slot arg,
                          NodeId dflt) {
      return r.iff(
          r.is(r.typ(r.L(arg)), "nil"), r.set(out, dflt),
          r.blk(
              {r.set(out, r.in(IntrinsicId::ToInt, {r.L(arg)})),
               r.iff(r.bin(BinOp::Lt, r.L(out), r.I(0)),
                     r.set(out, r.bin(BinOp::Add, r.L(n), r.L(out)))),
               r.iff(r.bin(BinOp::Lt, r.L(out), r.I(0)), r.set(out, r.I(0))),
               r.iff(r.bin(BinOp::Gt, r.L(out), r.L(n)), r.set(out, r.L(n)))}));
    };
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

  void rt_push() {
    RT r(*this);
    const auto [a, v] = r.params("a", "v");
    r.add(r.in(IntrinsicId::ArrayPush, {r.L(a), r.L(v)}));
    r.add(r.ret(r.in(IntrinsicId::ToDouble, {r.len(r.L(a))})));
    r.finish("$push");
  }

  // The iteration protocol, ES2023 7.4 -- as a state object rather than as
  // an object with a `next` method, because nothing in this subset can
  // observe the difference and a `for...of` loop is then one call per step
  // instead of two. A generator delegates to GenResume, whose {value,
  // done} answer already *is* the shape this returns.
  void rt_iter() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [t, mm, ks, out, i, k] =
        r.locals("t", "mm", "ks", "out", "i", "k");
    r.add(r.set(t, r.typ(r.L(v))));
    r.add(r.iff(r.is(r.L(t), "generator"),
                r.ret(r.obj({{"k", r.S("g")}, {"v", r.L(v)}}))));
    const NodeId over =
        r.ret(r.obj({{"k", r.S("a")}, {"v", r.L(v)}, {"i", r.I(0)}}));
    r.add(r.iff(r.is(r.L(t), "array"), over));
    r.add(r.iff(r.is(r.L(t), "string"), over));
    r.add(r.iff(
        r.is(r.L(t), "object"),
        r.blk({
            r.iff(r.in(IntrinsicId::ObjectHas, {r.L(v), r.S("$set")}),
                  r.ret(r.obj({{"k", r.S("a")},
                               {"v", r.in(IntrinsicId::ObjectKeys,
                                          {r.idx(r.L(v), "$set")})},
                               {"i", r.I(0)}}))),
            // A Map iterates [key, value] pairs; materializing them up
            // front keeps $iternext one shape rather than three.
            r.iff(r.in(IntrinsicId::ObjectHas, {r.L(v), r.S("$map")}),
                  r.blk({
                      r.set(mm, r.idx(r.L(v), "$map")),
                      r.set(ks, r.in(IntrinsicId::ObjectKeys, {r.L(mm)})),
                      r.set(out, r.b.array_lit({})),
                      r.set(i, r.I(0)),
                      r.b.make_while(
                          r.bin(BinOp::Lt, r.L(i), r.len(r.L(ks))),
                          r.blk({r.set(k, r.idx(r.L(ks), r.L(i))),
                                 r.in(IntrinsicId::ArrayPush,
                                      {r.L(out),
                                       r.b.array_lit(
                                           {r.L(k), r.idx(r.L(mm), r.L(k))})}),
                                 r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})),
                      r.ret(r.obj(
                          {{"k", r.S("a")}, {"v", r.L(out)}, {"i", r.I(0)}})),
                  })),
        })));
    r.add(r.ret(r.call("$throwtype", {r.S("value is not iterable")})));
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
    r.add(r.sidx(r.L(it), "i", r.bin(BinOp::Add, r.L(i), r.I(1))));
    r.add(r.ret(
        r.obj({{"value", r.idx(r.L(a), r.L(i))}, {"done", r.Bo(false)}})));
    r.finish("$iternext");
  }

  // Map and Set. The wrapper object is what makes `typeof m === 'object'`
  // and `m instanceof Map` distinguishable-in-principle; what it wraps is
  // the library's value-keyed Map (IntrinsicId::MapNew), which is the
  // whole point -- an object's keys are strings, a Map's are values, and
  // that is the same distinction JavaScript draws.
  void rt_minner() {
    RT r(*this);
    const auto [o] = r.params("o");
    r.add(
        r.iff(r.is(r.typ(r.L(o)), "object"),
              r.blk({r.iff(r.in(IntrinsicId::ObjectHas, {r.L(o), r.S("$map")}),
                           r.ret(r.idx(r.L(o), "$map"))),
                     r.iff(r.in(IntrinsicId::ObjectHas, {r.L(o), r.S("$set")}),
                           r.ret(r.idx(r.L(o), "$set")))})));
    r.add(r.ret(r.call("$throwtype", {r.S("not a Map or a Set")})));
    r.finish("$minner");
  }

  void rt_mget() {
    RT r(*this);
    const auto [o, k] = r.params("o", "k");
    const auto [mm] = r.locals("mm");
    r.add(r.set(mm, r.call("$minner", {r.L(o)})));
    r.add(r.iff(r.in(IntrinsicId::ObjectHas, {r.L(mm), r.L(k)}),
                r.ret(r.idx(r.L(mm), r.L(k)))));
    r.add(r.ret(r.Nil()));
    r.finish("$mget");
  }

  void rt_mset() {
    RT r(*this);
    const auto [o, k, v] = r.params("o", "k", "v");
    r.add(r.b.set_index(r.call("$minner", {r.L(o)}), r.L(k), r.L(v)));
    r.add(r.ret(r.L(o)));
    r.finish("$mset");
  }

  void rt_mhas() {
    RT r(*this);
    const auto [o, k] = r.params("o", "k");
    r.add(r.ret(
        r.in(IntrinsicId::ObjectHas, {r.call("$minner", {r.L(o)}), r.L(k)})));
    r.finish("$mhas");
  }

  void rt_mdel() {
    RT r(*this);
    const auto [o, k] = r.params("o", "k");
    const auto [mm] = r.locals("mm");
    r.add(r.set(mm, r.call("$minner", {r.L(o)})));
    r.add(r.iff(r.in(IntrinsicId::ObjectHas, {r.L(mm), r.L(k)}),
                r.blk({r.in(IntrinsicId::ObjectRemove, {r.L(mm), r.L(k)}),
                       r.ret(r.Bo(true))})));
    r.add(r.ret(r.Bo(false)));
    r.finish("$mdel");
  }

  void rt_sadd() {
    RT r(*this);
    const auto [o, v] = r.params("o", "v");
    r.add(r.b.set_index(r.call("$minner", {r.L(o)}), r.L(v), r.Bo(true)));
    r.add(r.ret(r.L(o)));
    r.finish("$sadd");
  }

  // -- Promises, async and await ------------------------------------------
  //
  // The top-level README's Coroutines and Scheduler sections, in
  // JavaScript's own vocabulary. Nothing here is in `vmlib.h`, and nothing
  // here needed to be: the library supplies CoroCreate/CoroYield/
  // CoroCurrent and one FIFO of jobs (Enqueue), and *which* queue
  // discipline settles what, in what order, is the language's -- the same
  // division examples/mini-go draws for a Go channel.
  //
  //   * A Promise is an object: {$promise, s: 0|1|2, v, cbs}. `s` is
  //     pending/fulfilled/rejected, `cbs` the reactions waiting on it.
  //   * A reaction is whatever Enqueue accepts -- a closure, or a
  //     *coroutine*. `await` registers the coroutine itself, so settling a
  //     promise wakes the awaiting function directly, with no callback
  //     hop in between.
  //   * `async function f() {...}` is compiled to three funcs: `f` builds
  //     the promise and spawns, `f$body` is the body you wrote, and
  //     `f$run` is the coroutine entry that calls the body and settles the
  //     promise with what it returned or threw (see emit_fn).
  //   * `await e` is $await: park the current coroutine on e's promise,
  //     and on the way back either answer its value or throw its reason.
  //     Every frame between the coroutine's bottom and the CoroYield is
  //     parked, which is why `await` works inside a call the body makes
  //     rather than only at the body's own top level.

  void rt_pnew() {
    RT r(*this);

    r.add(r.ret(r.obj({{"$promise", r.Bo(true)},
                       {"s", r.I(0)},
                       {"v", r.Nil()},
                       {"cbs", r.b.array_lit({})}})));
    r.finish("$pnew");
  }

  void rt_ispromise() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(
        r.iff(r.is(r.typ(r.L(v)), "object"),
              r.ret(r.in(IntrinsicId::ObjectHas, {r.L(v), r.S("$promise")}))));
    r.add(r.ret(r.Bo(false)));
    r.finish("$ispromise");
  }

  // Settling is once and for all: a second resolve, or a reject after a
  // resolve, is ignored -- ES2023 27.2.1.3's "alreadyResolved".
  void rt_psettle() {
    RT r(*this);
    const auto [p, st, v] = r.params("p", "st", "v");
    const auto [cbs, i] = r.locals("cbs", "i");
    r.add(r.iff(r.bin(BinOp::Ne, r.idx(r.L(p), "s"), r.I(0)), r.ret(r.Nil())));
    r.add(r.sidx(r.L(p), "s", r.L(st)));
    r.add(r.sidx(r.L(p), "v", r.L(v)));
    r.add(r.set(cbs, r.idx(r.L(p), "cbs")));
    r.add(r.sidx(r.L(p), "cbs", r.b.array_lit({})));
    r.add(r.set(i, r.I(0)));
    r.add(r.b.make_while(
        r.bin(BinOp::Lt, r.L(i), r.len(r.L(cbs))),
        r.blk({r.in(IntrinsicId::Enqueue, {r.idx(r.L(cbs), r.L(i))}),
               r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.Nil()));
    r.finish("$psettle");
  }

  void rt_pon() {
    RT r(*this);
    const auto [p, cb] = r.params("p", "cb");
    r.add(
        r.iff(r.bin(BinOp::Ne, r.idx(r.L(p), "s"), r.I(0)),
              r.blk({r.in(IntrinsicId::Enqueue, {r.L(cb)}), r.ret(r.Nil())})));
    r.add(r.in(IntrinsicId::ArrayPush, {r.idx(r.L(p), "cbs"), r.L(cb)}));
    r.add(r.ret(r.Nil()));
    r.finish("$pon");
  }

  void rt_topromise() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [p] = r.locals("p");
    r.add(r.iff(r.call("$ispromise", {r.L(v)}), r.ret(r.L(v))));
    r.add(r.set(p, r.call("$pnew", {})));
    r.add(r.call("$psettle", {r.L(p), r.I(1), r.L(v)}));
    r.add(r.ret(r.L(p)));
    r.finish("$topromise");
  }

  // Resolving with a promise adopts its state (ES2023 27.2.1.3.2) rather
  // than fulfilling with the promise as a value -- which is what makes an
  // async function that returns a promise flatten instead of nesting.
  void rt_presolve() {
    RT r(*this);
    const auto [q, v] = r.params("q", "v");
    r.add(r.iff(r.call("$ispromise", {r.L(v)}),
                r.blk({r.setc(0, r.L(q)), r.setc(1, r.L(v)),
                       r.call("$pon", {r.L(v), r.clos("$adopt", {0, 1})}),
                       r.ret(r.Nil())})));
    r.add(r.call("$psettle", {r.L(q), r.I(1), r.L(v)}));
    r.add(r.ret(r.Nil()));
    r.finish("$presolve", 2);
  }

  void rt_adopt() {
    RT r(*this);

    r.add(r.call("$psettle",
                 {r.P(0), r.idx(r.P(1), "s"), r.idx(r.P(1), "v")}));
    r.finish("$adopt", 0, 2);
  }

  void rt_then() {
    RT r(*this);
    const auto [p, onF, onR] = r.params("p", "onF", "onR");
    const auto [q] = r.locals("q");
    r.add(r.set(q, r.call("$pnew", {})));
    r.add(r.setc(0, r.L(p)));
    r.add(r.setc(1, r.L(q)));
    r.add(r.setc(2, r.L(onF)));
    r.add(r.setc(3, r.L(onR)));
    r.add(r.call("$pon", {r.L(p), r.clos("$react", {0, 1, 2, 3})}));
    r.add(r.ret(r.L(q)));
    r.finish("$then", 4);
  }

  // captures: 0 = the settled promise, 1 = the promise this reaction
  // settles, 2 = onFulfilled, 3 = onRejected. A handler that is not a
  // function is skipped and the state passes through -- ES2023 27.2.2.1's
  // "if IsCallable(handler) is false".
  void rt_react() {
    RT r(*this);
    const auto [e] = r.locals("e");
    const auto arm = [&](int32_t h, bool fulfilled) {
      const NodeId run = r.b.make_try(
          0,
          r.call("$presolve",
                 {r.P(1), r.b.call_value(r.P(h), {r.idx(r.P(0), "v")})}),
          r.call("$psettle", {r.P(1), r.I(2), r.L(e)}));
      const NodeId pass =
          fulfilled ? r.call("$presolve", {r.P(1), r.idx(r.P(0), "v")})
                    : r.call("$psettle",
                             {r.P(1), r.I(2), r.idx(r.P(0), "v")});
      return r.iff(r.is(r.typ(r.P(h)), "function"), run, pass);
    };
    r.add(r.iff(r.bin(BinOp::Eq, r.idx(r.P(0), "s"), r.I(1)), arm(2, true),
                arm(3, false)));
    r.finish("$react", 0, 4);
  }

  // The two halves of `new Promise((resolve, reject) => ...)`, each a
  // one-capture closure the `new` site builds over a cell holding the
  // promise (emit_new).
  void rt_presolve1() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.call("$presolve", {r.P(0), r.L(v)}));
    r.finish("$presolve1", 0, 1);
  }

  void rt_preject1() {
    RT r(*this);
    const auto [e] = r.params("e");
    r.add(r.call("$psettle", {r.P(0), r.I(2), r.L(e)}));
    r.finish("$preject1", 0, 1);
  }

  // `await`. CoroCurrent is the coroutine the async call spawned; parking
  // it on the promise and yielding is the whole of it, and the resume
  // comes from $psettle's Enqueue rather than from any callback this
  // front end writes.
  void rt_await() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [p] = r.locals("p");
    r.add(r.set(p, r.call("$topromise", {r.L(v)})));
    r.add(r.call("$pon", {r.L(p), r.in(IntrinsicId::CoroCurrent, {})}));
    r.add(r.in(IntrinsicId::CoroYield, {r.Nil()}));
    r.add(r.iff(r.bin(BinOp::Eq, r.idx(r.L(p), "s"), r.I(2)),
                r.b.make_throw(r.idx(r.L(p), "v"))));
    r.add(r.ret(r.idx(r.L(p), "v")));
    r.finish("$await");
  }

  void rt_prejected() {
    RT r(*this);
    const auto [e] = r.params("e");
    const auto [p] = r.locals("p");
    r.add(r.set(p, r.call("$pnew", {})));
    r.add(r.call("$psettle", {r.L(p), r.I(2), r.L(e)}));
    r.add(r.ret(r.L(p)));
    r.finish("$prejected");
  }

  void emit_runtime() {
    rt_truthy();
    rt_seq();
    rt_typeof();
    rt_numstr();
    rt_str();
    rt_join();
    rt_add();
    rt_throwtype();
    rt_index();
    rt_setidx();
    rt_slice();
    rt_push();
    rt_iter();
    rt_iternext();
    rt_minner();
    rt_mget();
    rt_mset();
    rt_mhas();
    rt_mdel();
    rt_sadd();
    rt_pnew();
    rt_ispromise();
    rt_psettle();
    rt_pon();
    rt_topromise();
    rt_presolve();
    rt_adopt();
    rt_then();
    rt_react();
    rt_presolve1();
    rt_preject1();
    rt_await();
    rt_prejected();
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

  // A value used as a condition. A comparison already produced a bool, and
  // JavaScript's ToBoolean is the identity on those, so the call is
  // skipped there -- which covers `i < n`, the condition most loops have.
  NodeId truthy(NodeId v, FnCtx& ctx, SrcPos p) {
    const Node& n = m.at(v);
    if (n.tag == Tag::Binary && is_comparison(static_cast<BinOp>(n.op))) {
      return v;
    }
    if (n.tag == Tag::Literal && m.const_kind(v) == ConstKind::Bool) return v;
    return helper("$truthy", {v}, p);
  }

  NodeId emit_closure(int32_t g, FnCtx& ctx, SrcPos p) {
    auto b = Builder(m).at(p);
    return b.make_closure(rs.fns[static_cast<size_t>(g)].index,
                          rs.capture_map(m, ctx.fn, g));
  }

  // Giving a declaration its storage, and initializing it. A captured one
  // gets CellFresh first: run again -- a `let` in a loop body, a
  // parameter on a second call -- it is a *new* box, so a closure built by
  // an earlier iteration keeps the value it captured. That is exactly
  // JavaScript's per-iteration binding for `let`, and exactly what `var`
  // (absent here) does not promise.
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

  // A brace block, as a Scope owning exactly the slots its own
  // declarations claimed. Sibling blocks reuse those slots (next_local is
  // restored on the way out), which is what keeps a frame's width the
  // deepest nesting rather than the total number of declarations.
  NodeId emit_block(const Ast& block, FnCtx& ctx) {
    const SrcPos p = pos_of(block);
    auto b = Builder(m).at(p);
    const int32_t mark = ctx.mark();
    std::vector<NodeId> out;

    // Every binding this block declares is *created* on entry, and only
    // initialized where it stands -- ES2023 14.2.3's BlockDeclaration-
    // Instantiation, which is not pedantry here but the thing that makes
    // a hoisted function work: a closure built at the top of the block
    // captures the cell of a `const` declared below it, and a CellFresh
    // run later at that `const` would swap the box out from under the
    // capture the closure is already holding. Doing it here also keeps
    // the per-iteration binding for free -- a loop body *is* a block, so
    // entering it again is exactly when JavaScript wants new bindings.
    const auto& cells = rs.fns[static_cast<size_t>(ctx.fn)].cell_index;
    const auto declared_by = [&](const Ast& s) -> const Ast* {
      if (s.tag == "funcdecl"_) return find_child(s, "ident");
      if (s.tag == "vardecl"_) return s.nodes[1].get();
      if (s.tag == "classdecl"_) return s.nodes[0].get();
      return nullptr;
    };
    for (const auto& s : block.nodes) {
      const Ast* id = declared_by(*s);
      if (id == nullptr) continue;
      const auto d = decl_of.find(id);
      if (d == decl_of.end()) continue;
      const auto c = cells.find(d->second);
      if (c != cells.end()) out.push_back(b.cell_fresh(c->second));
    }
    for (const auto& s : block.nodes) {
      if (s->tag != "funcdecl"_) continue;
      const Ast* id = find_child(*s, "ident");
      out.push_back(bind_decl(decl_of.at(id),
                              emit_closure(fn_of.at(s.get()), ctx, pos_of(*s)),
                              ctx, pos_of(*s), false));
    }
    for (const auto& s : block.nodes) {
      if (s->tag == "funcdecl"_) continue;
      out.push_back(emit_stmt(*s, ctx));
    }
    const int32_t end = ctx.release(mark);
    if (end > mark) return b.scope(mark, end, b.block(out));
    return b.block(out);
  }

  // A statement that is a block in its own right (`if (c) x = 1;` has no
  // braces, and its body still must not leak a declaration outward).
  NodeId emit_body(const Ast& a, FnCtx& ctx) {
    if (a.tag == "block"_) return emit_block(a, ctx);
    return emit_stmt(a, ctx);
  }

  std::vector<NodeId> emit_args(const Ast& args, FnCtx& ctx) {
    std::vector<NodeId> out;
    out.reserve(args.nodes.size());
    for (const auto& c : args.nodes) out.push_back(emit_expr(*c, ctx));
    return out;
  }

  NodeId arg_or_nil(const std::vector<NodeId>& a, size_t i, SrcPos p) {
    if (i < a.size()) return a[i];
    auto b = Builder(m).at(p);
    return b.nil_literal();
  }

  // -- Statements ---------------------------------------------------------
  NodeId emit_stmt(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    switch (a.tag) {
      case "funcdecl"_:
        return b.block({});  // hoisted; emit_block already bound it
      case "classdecl"_:
        return emit_classdecl(a, ctx);
      case "emptystmt"_:
        return b.block({});
      case "block"_:
        return emit_block(a, ctx);
      case "vardecl"_: {
        const int32_t v = decl_of.at(a.nodes[1].get());
        const NodeId value =
            a.nodes.size() > 2 ? emit_expr(*a.nodes[2], ctx) : b.nil_literal();
        // false: the cell already exists -- emit_block created it on the
        // way in, along with every other binding this block declares.
        return bind_decl(v, value, ctx, p, false);
      }
      case "ifstmt"_: {
        const NodeId c = truthy(emit_expr(*a.nodes[0], ctx), ctx, p);
        const NodeId t = emit_body(*a.nodes[1], ctx);
        NodeId e;
        if (a.nodes.size() > 2) e = emit_body(*a.nodes[2], ctx);
        return b.make_if(c, t, e);
      }
      case "whilestmt"_: {
        const NodeId c = truthy(emit_expr(*a.nodes[0], ctx), ctx, p);
        return b.make_while(c, emit_body(*a.nodes[1], ctx));
      }
      case "forstmt"_:
        return emit_for(a, ctx);
      case "forof"_:
        return emit_forof(a, ctx);
      case "retstmt"_:
        return b.make_return(a.nodes.empty() ? b.nil_literal()
                                             : emit_expr(*a.nodes[0], ctx));
      case "throwstmt"_:
        return b.make_throw(emit_expr(*a.nodes[0], ctx));
      case "breakstmt"_:
        return b.make_break();
      case "contstmt"_:
        return b.make_continue();
      case "trystmt"_:
        return emit_try(a, ctx);
      case "exprstmt"_:
        return emit_expr_stmt(*a.nodes[0], ctx);
      default:
        fail(a, "cannot execute " + a.name);
    }
  }

  // `i++` and `i--` are statements here, not expressions: the value form
  // needs a temporary to answer with the *old* value, and no sample needs
  // one -- see README.md. This is also what a `for`'s update clause is.
  bool is_incdec(const Ast& a) {
    return a.tag == "postfix"_ && a.nodes.back()->tag == "incsfx"_;
  }

  NodeId emit_expr_stmt(const Ast& a, FnCtx& ctx) {
    if (is_incdec(a)) return emit_incdec(a, ctx);
    return emit_expr(a, ctx);
  }

  NodeId emit_incdec(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const bool up = a.nodes.back()->token == "++";
    return emit_store(a, a.nodes.size() - 1, ctx, p, [&](auto&& cur) {
      return up ? helper("$add", {cur(), b.double_literal(1.0)}, p)
                : b.binary(BinOp::Sub, cur(), b.double_literal(1.0));
    });
  }

  // for (init; cond; update) body, with `continue` running the update and
  // `let` getting its per-iteration binding.
  //
  //   init
  //   first = true
  //   while (true) {
  //     if (first) first = false
  //     else { carry = i; CellFresh(i); i = carry; update }
  //     if (!cond) break
  //     body
  //   }
  //
  // The flag is what makes `continue` correct: Tag::Continue re-tests the
  // loop's condition, so an update written at the *bottom* of the body
  // would be skipped by it. Putting the update at the top, guarded, runs
  // it on every path back into the loop and on none into the first
  // iteration. The carry/CellFresh/copy trio around it is ES2023 14.7.4.4
  // CreatePerIterationEnvironment: each iteration's `i` is a fresh binding
  // seeded from the previous one, which is why a closure made in iteration
  // 2 sees 2 forever rather than the loop's final value.
  NodeId emit_for(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const int32_t mark = ctx.mark();
    std::vector<NodeId> out;

    int32_t loopvar = -1;
    for (const auto& c : a.nodes[0]->nodes) {
      if (c->tag == "vardeclbare"_) {
        loopvar = decl_of.at(c->nodes[1].get());
        out.push_back(
            bind_decl(loopvar, emit_expr(*c->nodes[2], ctx), ctx, p));
      } else {
        out.push_back(emit_expr_stmt(*c, ctx));
      }
    }
    const int32_t first = ctx.alloc_local("$first");
    out.push_back(b.assign(VarKind::Local, first, b.bool_literal(true)));

    std::vector<NodeId> step;
    const auto& cells = rs.fns[static_cast<size_t>(ctx.fn)].cell_index;
    if (loopvar >= 0 && cells.count(loopvar)) {
      const int32_t c = cells.at(loopvar);
      const int32_t carry = ctx.alloc_local("$carry");
      step.push_back(
          b.assign(VarKind::Local, carry, b.varref(VarKind::Cell, c)));
      step.push_back(b.cell_fresh(c));
      step.push_back(
          b.assign(VarKind::Cell, c, b.varref(VarKind::Local, carry)));
    }
    for (const auto& c : a.nodes[2]->nodes) {
      step.push_back(emit_expr_stmt(*c, ctx));
    }

    std::vector<NodeId> loop;
    loop.push_back(b.make_if(
        b.varref(VarKind::Local, first),
        b.assign(VarKind::Local, first, b.bool_literal(false)), b.block(step)));
    if (!a.nodes[1]->nodes.empty()) {
      const NodeId c = truthy(emit_expr(*a.nodes[1]->nodes[0], ctx), ctx, p);
      loop.push_back(b.make_if(b.binary(BinOp::Eq, c, b.bool_literal(false)),
                               b.make_break(), NodeId{}));
    }
    loop.push_back(emit_body(*a.nodes[3], ctx));
    out.push_back(b.make_while(b.bool_literal(true), b.block(loop)));

    const int32_t end = ctx.release(mark);
    return b.scope(mark, end, b.block(out));
  }

  NodeId emit_forof(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const int32_t mark = ctx.mark();
    const int32_t it = ctx.alloc_local("$it");
    const int32_t st = ctx.alloc_local("$step");
    std::vector<NodeId> out;
    out.push_back(b.assign(VarKind::Local, it,
                           helper("$iter", {emit_expr(*a.nodes[2], ctx)}, p)));
    std::vector<NodeId> loop;
    loop.push_back(
        b.assign(VarKind::Local, st,
                 helper("$iternext", {b.varref(VarKind::Local, it)}, p)));
    loop.push_back(
        b.make_if(b.index(b.varref(VarKind::Local, st), b.str_literal("done")),
                  b.make_break(), NodeId{}));
    loop.push_back(bind_decl(
        decl_of.at(a.nodes[1].get()),
        b.index(b.varref(VarKind::Local, st), b.str_literal("value")), ctx, p));
    loop.push_back(emit_body(*a.nodes[3], ctx));
    out.push_back(b.make_while(b.bool_literal(true), b.block(loop)));

    const int32_t end = ctx.release(mark);
    return b.scope(mark, end, b.block(out));
  }

  // try/catch/finally. `finally` is a Defer, not a copy of the block down
  // each exit path: Tag::Defer runs its callable however the Scope exits
  // -- falling through, `return`, `break`, or a throw nothing here caught
  // -- which is exactly `finally`'s contract, and the reason the block was
  // resolved as a function of its own (a Defer takes a callable, and a
  // callable can only reach the enclosing frame through cells).
  NodeId emit_try(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const Ast* cat = nullptr;
    const Ast* fin = nullptr;
    for (size_t i = 1; i < a.nodes.size(); ++i) {
      if (a.nodes[i]->tag == "catchcl"_) {
        cat = a.nodes[i].get();
      } else {
        fin = a.nodes[i].get();
      }
    }
    const int32_t mark = ctx.mark();
    const int32_t exc = ctx.alloc_local("$exc");
    std::vector<NodeId> out;
    if (fin != nullptr) {
      out.push_back(b.make_defer(emit_closure(fn_of.at(fin), ctx, p)));
    }
    const NodeId body = emit_block(*a.nodes[0], ctx);
    NodeId handler;
    if (cat != nullptr) {
      const int32_t hmark = ctx.mark();
      std::vector<NodeId> hs;
      hs.push_back(bind_decl(decl_of.at(cat->nodes[0].get()),
                             b.varref(VarKind::Local, exc), ctx, p));
      hs.push_back(emit_block(*cat->nodes[1], ctx));
      const int32_t hend = ctx.release(hmark);
      handler = hend > hmark ? b.scope(hmark, hend, b.block(hs)) : b.block(hs);
    } else {
      // `try`/`finally` with no catch: the Defer still has to run, so the
      // throw is caught and re-raised rather than left to unwind past the
      // Scope uncaught.
      handler = b.make_throw(b.varref(VarKind::Local, exc));
    }
    out.push_back(b.make_try(exc, body, handler));
    const int32_t end = ctx.release(mark);
    return b.scope(mark, end, b.block(out));
  }

  // -- Expressions --------------------------------------------------------
  NodeId emit_expr(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    switch (a.tag) {
      case "number"_:
        // Every number is a double: JavaScript has one numeric type, and
        // keeping ints out of the frame is what makes `1 === 1.0` and
        // `String(3)` need no special case.
        return b.double_literal(
            std::strtod(std::string(a.token).c_str(), nullptr));
      case "string"_:
        return b.str_literal(unescape(std::string(a.token), a));
      case "literal"_: {
        const std::string t(a.token);
        if (t == "true") return b.bool_literal(true);
        if (t == "false") return b.bool_literal(false);
        return b.nil_literal();  // null and undefined are the same value here
      }
      case "ident"_: {
        const auto it = ref_of.find(&a);
        if (it != ref_of.end()) return read_var(it->second, ctx, p);
        return emit_global_value(a, ctx);
      }
      case "thisexpr"_:
        return read_var(ref_of.at(&a), ctx, p);
      case "paren"_:
        return emit_expr(*a.nodes[0], ctx);
      case "arrow"_:
      case "funcexpr"_:
        return emit_closure(fn_of.at(&a), ctx, p);
      case "yieldexpr"_: {
        if (!fns[static_cast<size_t>(ctx.fn)].is_generator) {
          fail(a, "'yield' outside a generator function");
        }
        return b.make_yield(a.nodes.size() > 1 ? emit_expr(*a.nodes[1], ctx)
                                               : b.nil_literal());
      }
      case "notexpr"_:
        return b.binary(BinOp::Eq, truthy(emit_expr(*a.nodes[0], ctx), ctx, p),
                        b.bool_literal(false));
      case "negexpr"_:
        return b.unary(UnOp::Neg, emit_expr(*a.nodes[0], ctx));
      case "typeofexpr"_:
        return helper("$typeof", {emit_expr(*a.nodes[0], ctx)}, p);
      case "newexpr"_:
        return emit_new(*a.nodes[0], ctx);
      case "awaitexpr"_: {
        if (!fns[static_cast<size_t>(ctx.fn)].is_async) {
          fail(a, "'await' outside an async function");
        }
        return helper("$await", {emit_expr(*a.nodes[0], ctx)}, p);
      }
      case "cond"_:
        return b.make_if(truthy(emit_expr(*a.nodes[0], ctx), ctx, p),
                         emit_expr(*a.nodes[1], ctx),
                         emit_expr(*a.nodes[2], ctx));
      // `a || b` answers one of its operands, not a bool, so the left one
      // is kept in a slot to be tested and then handed back.
      case "logor"_:
      case "logand"_: {
        const bool is_or = a.tag == "logor"_;
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i < a.nodes.size(); ++i) {
          const int32_t t = ctx.alloc_local(is_or ? "$or" : "$and");
          const NodeId rhs = emit_expr(*a.nodes[i], ctx);
          const NodeId keep = b.varref(VarKind::Local, t);
          acc = b.block({b.assign(VarKind::Local, t, acc),
                         b.make_if(truthy(keep, ctx, p), is_or ? keep : rhs,
                                   is_or ? rhs : keep)});
        }
        return acc;
      }
      case "equality"_: {
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i + 1 < a.nodes.size(); i += 2) {
          const Ast& op = *a.nodes[i];
          const std::string t(op.token);
          if (t == "==" || t == "!=") {
            fail(op, "'" + t + "' is not supported; use '" + t.substr(0, 1) +
                         "==' -- see README.md");
          }
          const NodeId rhs = emit_expr(*a.nodes[i + 1], ctx);
          const NodeId eq = helper("$seq", {acc, rhs}, pos_of(op));
          acc = t == "===" ? eq
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
          const BinOp o = t == "<" ? BinOp::Lt
                          : t == "<=" ? BinOp::Le
                          : t == ">" ? BinOp::Gt
                                     : BinOp::Ge;
          acc =
              b.at(pos_of(op)).binary(o, acc, emit_expr(*a.nodes[i + 1], ctx));
        }
        return acc;
      }
      case "additive"_: {
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i + 1 < a.nodes.size(); i += 2) {
          const Ast& op = *a.nodes[i];
          const NodeId rhs = emit_expr(*a.nodes[i + 1], ctx);
          acc = op.token == "+" ? helper("$add", {acc, rhs}, pos_of(op))
                                : b.at(pos_of(op)).binary(BinOp::Sub, acc, rhs);
        }
        return acc;
      }
      case "multiplicative"_: {
        NodeId acc = emit_expr(*a.nodes[0], ctx);
        for (size_t i = 1; i + 1 < a.nodes.size(); i += 2) {
          const Ast& op = *a.nodes[i];
          const NodeId rhs = emit_expr(*a.nodes[i + 1], ctx);
          const std::string t(op.token);
          // `%` is fmod, not truncation: BinOp::Mod refuses doubles, and
          // JavaScript's own `-7 % 3` is -1, which is fmod's answer.
          acc = t == "*" ? b.at(pos_of(op)).binary(BinOp::Mul, acc, rhs)
                : t == "/"
                    ? b.at(pos_of(op)).binary(BinOp::Div, acc, rhs)
                    : b.at(pos_of(op)).intrinsic(IntrinsicId::FMod, {acc, rhs});
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
        std::vector<std::pair<NodeId, NodeId>> kvs;
        for (const auto& c : a.nodes) {
          const Ast& key = *c->nodes[0];
          const std::string k = key.tag == "string"_
                                    ? unescape(std::string(key.token), key)
                                    : std::string(key.token);
          kvs.emplace_back(b.str_literal(k), emit_expr(*c->nodes[1], ctx));
        }
        return b.object_lit(kvs);
      }
      default:
        fail(a, "cannot evaluate " + a.name);
    }
  }

  // The globals that stand alone as values. Everything else a global name
  // can be (`console.log`, `new Map()`) is a shape emit_postfix or
  // emit_new recognizes, not a value this could hand back.
  NodeId emit_global_value(const Ast& a, FnCtx&) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const std::string n(a.token);
    if (n == "NaN") {
      return b.double_literal(std::numeric_limits<double>::quiet_NaN());
    }
    if (n == "Infinity") {
      return b.double_literal(std::numeric_limits<double>::infinity());
    }
    fail(a, "'" + n + "' is only supported in a call or a `new` here");
  }

  // `new C(args)`, for the three constructors this subset has. A user
  // function is not constructible here: without `this` there is nothing
  // for a constructor body to write into -- see README.md.
  // `class Name { constructor(...) {...} m(...) {...} }` -- a plain
  // object holding one closure per member (`constructor` included, under
  // that name, so `instance.constructor` answers the way real JS's does).
  // The closures are built *here*, at the declaration's own lexical
  // position, which is what a class needs from the capture machinery:
  // every other front end's version of this (examples/mini-python's,
  // examples/mini-ruby's) builds its method table the same way and for
  // the same reason -- built anywhere else, a method that closes over a
  // variable outside the class would be capturing across a boundary Pass
  // A never knew to thread it through.
  NodeId emit_classdecl(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const ClassInfo& ci = classes.at(&a);
    std::vector<std::pair<NodeId, NodeId>> kvs;
    if (ci.ctor_fn >= 0) {
      kvs.emplace_back(b.str_literal("constructor"),
                       emit_closure(ci.ctor_fn, ctx, p));
    }
    for (const auto& [name, g] : ci.methods) {
      kvs.emplace_back(b.str_literal(name), emit_closure(g, ctx, p));
    }
    return bind_decl(decl_of.at(a.nodes[0].get()), b.object_lit(kvs), ctx, p,
                     false);
  }

  // `new Foo(x).bar()`: `new` binds to the *nearest* constructor call in
  // JavaScript's own grammar, and anything chained after it applies to
  // the instance the same way it would to any other value -- so this
  // builds from exactly `a.nodes[0..2)` and hands the rest of `a` to
  // `apply_suffixes`.
  NodeId emit_new(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    if (a.tag != "postfix"_ || a.nodes[0]->tag != "ident"_ ||
        a.nodes.size() < 2 || a.nodes[1]->tag != "callsfx"_) {
      fail(a, "`new` takes a constructor call here");
    }
    const std::string n(a.nodes[0]->token);
    const std::vector<NodeId> args = emit_args(*a.nodes[1]->nodes[0], ctx);
    const auto chain = [&](NodeId built) {
      return apply_suffixes(built, a, 2, a.nodes.size(), ctx);
    };
    if (n == "Map") {
      return chain(b.object_lit(
          {{b.str_literal("$map"), b.intrinsic(IntrinsicId::MapNew, {})}}));
    }
    if (n == "Set") {
      return chain(b.object_lit(
          {{b.str_literal("$set"), b.intrinsic(IntrinsicId::MapNew, {})}}));
    }
    // new Promise((resolve, reject) => ...): the two halves are closures
    // over a cell of *this* frame holding the promise, which is the one
    // place a `new` needs storage of its own. CellFresh gives a `new
    // Promise` inside a loop its own cell per iteration.
    if (n == "Promise") {
      const int32_t c = ctx.next_cell++;
      const std::vector<CaptureSrc> cs{{VarKind::Cell, c}};
      const int32_t cm1 = static_cast<int32_t>(m.capture_maps.size());
      m.capture_maps.push_back(cs);
      const int32_t cm2 = static_cast<int32_t>(m.capture_maps.size());
      m.capture_maps.push_back(cs);
      return chain(b.block(
          {b.cell_fresh(c), b.assign(VarKind::Cell, c, helper("$pnew", {}, p)),
           b.call_value(arg_or_nil(args, 0, p),
                        {b.make_closure(rt.at("$presolve1"), cm1),
                         b.make_closure(rt.at("$preject1"), cm2)}),
           b.varref(VarKind::Cell, c)}));
    }
    if (n == "Error" || n == "TypeError" || n == "RangeError") {
      return chain(
          b.object_lit({{b.str_literal("name"), b.str_literal(n)},
                        {b.str_literal("message"),
                         helper("$str", {arg_or_nil(args, 0, p)}, p)}}));
    }
    // `new SomeClass(...)`: not a builtin, so it has to be a class -- and
    // one this call site can see statically, because binding `this` needs
    // to know each method's exact parameter count. A method's own closure
    // does not take `this` as an argument the way `obj.method()` calls
    // it; it *is* one (params[0], from `resolve_fn`'s `is_method`), so
    // what an instance actually holds under each name is a small wrapper
    // -- captures the class table and this instance, takes the method's
    // declared parameters, and calls the real one with `this` in front.
    // Built once per `new` expression, not per method table: the wrapper
    // itself is a plain closure with a fresh capture map, the same trick
    // `new Promise` above uses for its own per-occurrence state.
    if (!ref_of.count(a.nodes[0].get()) ||
        !class_by_var.count(ref_of.at(a.nodes[0].get()))) {
      fail(*a.nodes[0], "'" + n + "' is not a class known here");
    }
    const ClassInfo& ci = classes.at(class_by_var.at(ref_of.at(a.nodes[0].get())));
    const NodeId cls = read_var(ref_of.at(a.nodes[0].get()), ctx, p);
    const int32_t ccell = ctx.next_cell++;
    const int32_t tcell = ctx.next_cell++;
    const int32_t tv = ctx.alloc_local("$this");
    const NodeId T = b.varref(VarKind::Local, tv);
    std::vector<NodeId> steps{
        b.cell_fresh(ccell), b.assign(VarKind::Cell, ccell, cls),
        b.cell_fresh(tcell), b.assign(VarKind::Cell, tcell, b.object_lit({})),
        b.assign(VarKind::Local, tv, b.varref(VarKind::Cell, tcell))};
    for (const auto& [name, g] : ci.methods) {
      const int32_t nparams =
          static_cast<int32_t>(fns[static_cast<size_t>(g)].params.size()) - 1;
      Func wf;
      wf.name = name + ".bound";
      wf.num_params = nparams;
      wf.num_locals = nparams;
      wf.local_names.assign(static_cast<size_t>(nparams), "arg");
      wf.num_captures = 2;
      wf.capture_names = {"cls", "self"};
      wf.lenient_arity = true;
      std::vector<NodeId> wargs{b.varref(VarKind::Capture, 1)};
      for (int32_t k = 0; k < nparams; ++k) {
        wargs.push_back(b.varref(VarKind::Local, k));
      }
      wf.body = b.scope(
          0, nparams,
          b.make_return(b.call_value(
              b.index(b.varref(VarKind::Capture, 0), b.str_literal(name)),
              wargs)));
      const int32_t widx = static_cast<int32_t>(m.funcs.size());
      m.funcs.push_back(std::move(wf));
      const int32_t wcm = static_cast<int32_t>(m.capture_maps.size());
      m.capture_maps.push_back(
          {{VarKind::Cell, ccell}, {VarKind::Cell, tcell}});
      steps.push_back(helper(
          "$setidx", {T, b.str_literal(name), b.make_closure(widx, wcm)}, p));
    }
    steps.push_back(helper(
        "$setidx",
        {T, b.str_literal("constructor"), b.varref(VarKind::Cell, ccell)}, p));
    if (ci.ctor_fn >= 0) {
      const NodeId ctorClosure = helper(
          "$index",
          {b.varref(VarKind::Cell, ccell), b.str_literal("constructor")}, p);
      std::vector<NodeId> ca{T};
      ca.insert(ca.end(), args.begin(), args.end());
      steps.push_back(b.call_value(ctorClosure, ca));
    }
    steps.push_back(T);
    return chain(b.block(steps));
  }

  // A primary followed by `limit - 1` suffixes. The limit is what lets an
  // assignment target reuse this for everything up to its last suffix --
  // `o.a.b = v` needs `o.a` as a value and `b` as a key, which is this
  // walk stopped one step early.
  // The suffix loop, factored out so `new Foo(x).bar()` can resume it from
  // an instance `emit_new` already built rather than from a primary --
  // JavaScript's own rule is that `new`'s constructor call binds tighter
  // than nothing else in `postfix`, so whatever comes after it chains on
  // exactly the way it would on any other value.
  NodeId apply_suffixes(NodeId cur, const Ast& a, size_t from, size_t limit,
                        FnCtx& ctx) {
    Builder b(m);
    for (size_t i = from; i < limit; ++i) {
      const Ast& sfx = *a.nodes[i];
      const SrcPos p = pos_of(sfx);
      switch (sfx.tag) {
        case "membersfx"_: {
          const std::string name(sfx.nodes[0]->token);
          if (i + 1 < limit && a.nodes[i + 1]->tag == "callsfx"_) {
            cur = emit_method(cur, name, *a.nodes[i + 1], sfx, ctx);
            ++i;
            break;
          }
          cur = helper("$index", {cur, b.str_literal(name, p)}, p);
          break;
        }
        case "indexsfx"_:
          cur = helper("$index", {cur, emit_expr(*sfx.nodes[0], ctx)}, p);
          break;
        case "callsfx"_:
          cur = b.call_value(cur, emit_args(*sfx.nodes[0], ctx), p);
          break;
        default:
          fail(sfx, "'++'/'--' is only supported as a statement");
      }
    }
    return cur;
  }

  NodeId emit_postfix(const Ast& a, size_t limit, FnCtx& ctx) {
    const Ast& prim = *a.nodes[0];
    size_t i = 1;
    NodeId cur;
    if (prim.tag == "ident"_ && !ref_of.count(&prim) && is_global(std::string(prim.token))) {
      cur = emit_global_call(a, limit, ctx, i);
    } else {
      cur = emit_expr(prim, ctx);
    }
    return apply_suffixes(cur, a, i, limit, ctx);
  }

  // `console.log(...)` and friends: a global name is not a value here, it
  // is the head of one of a handful of fixed shapes, matched whole.
  NodeId emit_global_call(const Ast& a, size_t limit, FnCtx& ctx, size_t& i) {
    Builder b(m);
    const Ast& prim = *a.nodes[0];
    const SrcPos p = pos_of(prim);
    const std::string g(prim.token);

    const auto member = [&](size_t k) -> std::string {
      if (k < limit && a.nodes[k]->tag == "membersfx"_) {
        return std::string(a.nodes[k]->nodes[0]->token);
      }
      return {};
    };
    const auto call_at = [&](size_t k) -> const Ast* {
      if (k < limit && a.nodes[k]->tag == "callsfx"_) return a.nodes[k].get();
      return nullptr;
    };

    // String(x)
    if (g == "String") {
      const Ast* c = call_at(1);
      if (c == nullptr) fail(prim, "String must be called here");
      i = 2;
      const std::vector<NodeId> args = emit_args(*c->nodes[0], ctx);
      return helper("$str", {arg_or_nil(args, 0, p)}, p);
    }
    const std::string mem = member(1);
    const Ast* c = call_at(2);
    if (!mem.empty() && c != nullptr) {
      const std::vector<NodeId> args = emit_args(*c->nodes[0], ctx);
      i = 3;
      // console.log(a, b): JavaScript joins its arguments with a space,
      // and this subset's samples hand it a string the program built
      // itself (see samples/prelude.js), so Print's own formatting of a
      // non-string never has to agree with Node's util.inspect.
      if (g == "console" && mem == "log") {
        NodeId msg = b.str_literal("", p);
        for (size_t k = 0; k < args.size(); ++k) {
          if (k > 0) msg = b.binary(BinOp::Add, msg, b.str_literal(" ", p), p);
          msg = b.binary(BinOp::Add, msg,
                         helper("$str", {args[k]}, p), p);
        }
        return b.intrinsic(IntrinsicId::Print, {msg}, p);
      }
      if (g == "Object" && mem == "keys") {
        return b.intrinsic(IntrinsicId::ObjectKeys,
                           {arg_or_nil(args, 0, p)}, p);
      }
      if (g == "Promise" && mem == "resolve") {
        return helper("$topromise", {arg_or_nil(args, 0, p)}, p);
      }
      if (g == "Promise" && mem == "reject") {
        return helper("$prejected", {arg_or_nil(args, 0, p)}, p);
      }
      if (g == "Array" && mem == "isArray") {
        return b.binary(BinOp::Eq,
                        b.intrinsic(IntrinsicId::TypeOf,
                                    {arg_or_nil(args, 0, p)}, p),
                        b.str_literal("array", p), p);
      }
    }
    fail(prim, "'" + g + "' is not supported in this shape here");
  }

  // A method call. The name decides at bind time, which is what a subset
  // without prototypes can do: `x.push(v)` is the array primitive, and a
  // name that is not one of these is an ordinary property holding a
  // function. README.md lists the consequence -- an object of your own
  // must not put a function under one of these names.
  NodeId emit_method(NodeId recv, const std::string& name, const Ast& call,
                     const Ast& at, FnCtx& ctx) {
    const SrcPos p = pos_of(at);
    auto b = Builder(m).at(p);
    const std::vector<NodeId> args = emit_args(*call.nodes[0], ctx);
    const auto a0 = [&] { return arg_or_nil(args, 0, p); };
    const auto a1 = [&] { return arg_or_nil(args, 1, p); };

    if (name == "push") return helper("$push", {recv, a0()}, p);
    if (name == "pop") return b.intrinsic(IntrinsicId::ArrayPop, {recv});
    if (name == "slice") return helper("$slice", {recv, a0(), a1()}, p);
    if (name == "join") {
      return helper("$join",
                    {recv, args.empty() ? b.str_literal(",") : args[0]}, p);
    }
    // The generator protocol is GenResume/GenReturn/GenThrow, whose
    // {value, done} answer already is what `g.next()` evaluates to in
    // JavaScript -- no translation, which is the point.
    if (name == "next") {
      return b.intrinsic(IntrinsicId::GenResume, {recv, a0()});
    }
    if (name == "return") {
      return b.intrinsic(IntrinsicId::GenReturn, {recv, a0()});
    }
    if (name == "throw") {
      return b.intrinsic(IntrinsicId::GenThrow, {recv, a0()});
    }
    // A Promise's two combinators. Everything they do -- adopting a
    // returned promise, passing a rejection through a handler-less link,
    // turning a handler's throw into the next rejection -- is $then/$react,
    // written in IR above.
    if (name == "then") return helper("$then", {recv, a0(), a1()}, p);
    if (name == "catch") {
      return helper("$then", {recv, b.nil_literal(), a0()}, p);
    }
    if (name == "get") return helper("$mget", {recv, a0()}, p);
    if (name == "set") return helper("$mset", {recv, a0(), a1()}, p);
    if (name == "has") return helper("$mhas", {recv, a0()}, p);
    if (name == "delete") return helper("$mdel", {recv, a0()}, p);
    if (name == "add") return helper("$sadd", {recv, a0()}, p);

    return b.call_value(helper("$index", {recv, b.str_literal(name)}, p), args);
  }

  // Assignment, in the one shape both a variable and a property need: let
  // `combine` say what goes back, handing it a way to read the destination
  // out first. It is a callable rather than a value so that a plain `=`,
  // which does not want the old one, does not emit the read at all --
  // reading a property is a $index call, and an orphan one would claim a
  // helper cell for a call nothing makes.
  template <typename Combine>
  NodeId emit_store(const Ast& target, size_t limit, FnCtx& ctx, SrcPos p,
                    Combine combine) {
    auto b = Builder(m).at(p);
    // `i++` hands over the whole postfix node with its `++` cut off, and
    // what is left may be a bare primary -- which optimize_ast folded, so
    // it is an "ident" wearing a "postfix" parent rather than a suffix
    // chain of length zero.
    if (target.tag == "postfix"_ && limit == 1) {
      return emit_store(*target.nodes[0], 0, ctx, p, combine);
    }
    if (target.tag == "ident"_) {
      const auto it = ref_of.find(&target);
      if (it == ref_of.end()) fail(target, "cannot assign to this");
      const int32_t v = it->second;
      if (is_const[static_cast<size_t>(v)]) {
        fail(target, "cannot assign to constant '" +
                         rs.vars[static_cast<size_t>(v)].name + "'");
      }
      const auto read = [&] { return read_var(v, ctx, p); };
      return write_var(v, combine(read), ctx, p);
    }
    if (target.tag != "postfix"_ || limit < 2) {
      fail(target, "cannot assign to this expression");
    }
    const Ast& last = *target.nodes[limit - 1];
    NodeId key;
    if (last.tag == "membersfx"_) {
      key = b.str_literal(std::string(last.nodes[0]->token));
    } else if (last.tag == "indexsfx"_) {
      key = emit_expr(*last.nodes[0], ctx);
    } else {
      fail(last, "cannot assign to this expression");
    }
    // Receiver and key land in slots first: `o[i()] += 1` must evaluate
    // each of them once, not once for the read and again for the write.
    const int32_t tr = ctx.alloc_local("$recv");
    const int32_t tk = ctx.alloc_local("$key");
    const NodeId recv = emit_postfix(target, limit - 1, ctx);
    const NodeId R = b.varref(VarKind::Local, tr);
    const NodeId K = b.varref(VarKind::Local, tk);
    const auto read = [&] { return helper("$index", {R, K}, p); };
    return b.block({b.assign(VarKind::Local, tr, recv),
                    b.assign(VarKind::Local, tk, key),
                    helper("$setidx", {R, K, combine(read)}, p)});
  }

  NodeId emit_assign(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const std::string op(a.nodes[1]->token);
    const Ast& rhs = *a.nodes[2];
    const Ast& target = *a.nodes[0];
    const size_t limit = target.tag == "postfix"_ ? target.nodes.size() : 0;
    return emit_store(target, limit, ctx, p, [&](auto&& cur) -> NodeId {
      const NodeId v = emit_expr(rhs, ctx);
      if (op == "=") return v;
      if (op == "+=") return helper("$add", {cur(), v}, p);
      if (op == "-=") return b.binary(BinOp::Sub, cur(), v);
      if (op == "*=") return b.binary(BinOp::Mul, cur(), v);
      if (op == "/=") return b.binary(BinOp::Div, cur(), v);
      return b.intrinsic(IntrinsicId::FMod, {cur(), v});
    });
  }

  // -- One function's body -------------------------------------------------
  void emit_fn(int32_t f) {
    FnCtx ctx;
    ctx.fn = f;
    const FnInfo& fi = fns[static_cast<size_t>(f)];
    const Resolver::Fn& rf = rs.fns[static_cast<size_t>(f)];
    ctx.next_cell = rs.num_cells(f);
    const SrcPos p = fi.body != nullptr ? pos_of(*fi.body) : SrcPos{0, 0};
    auto b = Builder(m).at(p);

    std::vector<NodeId> pre;
    for (const int32_t v : fi.params) {
      const int32_t s = ctx.alloc_local(rs.vars[static_cast<size_t>(v)].name);
      rs.vars[static_cast<size_t>(v)].slot = s;
      // The calling convention fills locals, so a captured parameter is
      // copied into its cell on entry -- the header's own note that "an
      // argument that needs capturing is copied into a cell by the front
      // end and the calling convention itself stays about locals only".
      const auto it = rf.cell_index.find(v);
      if (it != rf.cell_index.end()) {
        pre.push_back(b.cell_fresh(it->second));
        pre.push_back(
            b.assign(VarKind::Cell, it->second, b.varref(VarKind::Local, s)));
      }
    }
    const int32_t nparams = ctx.mark();

    NodeId body;
    if (fi.body->tag == "block"_ || fi.body->tag == "program"_) {
      body = emit_block(*fi.body, ctx);
    } else {
      body = b.make_return(emit_expr(*fi.body, ctx));  // an arrow's expr
    }

    // The preamble, once the body is known: one MakeClosure per $helper
    // this body reaches, into the cell every call site reads it back from.
    std::vector<NodeId> stmts;
    stmts.insert(stmts.end(), pre.begin(), pre.end());
    stmts.push_back(body);

    Func built;
    built.name = fi.name;
    built.num_params = nparams;
    built.num_locals = ctx.high_local;
    built.local_names = ctx.names();
    built.num_cells = ctx.next_cell;
    built.lenient_arity = true;  // JavaScript's own arity rule
    built.is_generator = fi.is_generator;
    built.body = b.scope(0, nparams, b.block(stmts));

    // number_captures() already put this function's capture numbering on
    // its Module::funcs entry, and that numbering is what the body's own
    // VarKind::Capture nodes were emitted against -- so it travels with
    // the body wherever the body ends up.
    const int32_t ncaps = m.funcs[static_cast<size_t>(rf.index)].num_captures;
    std::vector<std::string> capnames =
        m.funcs[static_cast<size_t>(rf.index)].capture_names;
    built.num_captures = ncaps;
    built.capture_names = capnames;

    if (!fi.is_async) {
      m.funcs[static_cast<size_t>(rf.index)] = std::move(built);
      return;
    }
    emit_async(fi, rf.index, std::move(built), nparams, ncaps,
               std::move(capnames), p);
  }

  // An async function is three funcs, because calling one has to *return*
  // before its body has finished:
  //
  //   f       what the source declared. Builds the promise, packages the
  //           arguments and the body closure into cells, spawns the
  //           coroutine, and answers the promise -- all of it before the
  //           body has run a single statement.
  //   f$body  the body you wrote, unchanged, with f's own captures.
  //   f$run   the coroutine's entry: calls f$body and settles the promise
  //           with what it returned, or with what it threw.
  //
  // Enqueue(CoroCreate(...)) is the spawn -- the same three-word shape
  // examples/mini-go's `go` statement uses, because a goroutine and an
  // async call are the same primitive with different rules layered on
  // top. The rules are the difference: a goroutine's result goes nowhere,
  // where this one settles a promise somebody may be awaiting.
  void emit_async(const FnInfo& fi, int32_t index, Func body,
                  int32_t nparams, int32_t ncaps,
                  std::vector<std::string> capnames, SrcPos p) {
    auto b = Builder(m).at(p);
    body.name = fi.name + "$body";
    const int32_t body_idx = static_cast<int32_t>(m.funcs.size());
    m.funcs.push_back(std::move(body));
    const int32_t run_idx = static_cast<int32_t>(m.funcs.size());
    m.funcs.push_back({});

    const auto rtcall = [&](const std::string& name,
                            const std::vector<NodeId>& args) {
      return b.call_value(b.make_closure(rt.at(name), empty_cmap), args);
    };

    // f: cells 0 = the promise, 1 = the body closure, 2.. = the arguments.
    // The arguments are copied out at the call, JavaScript's own rule, and
    // into fresh cells so that two calls in flight at once do not share.
    std::vector<NodeId> launch;
    launch.push_back(b.cell_fresh(0));
    launch.push_back(b.assign(VarKind::Cell, 0, rtcall("$pnew", {})));
    std::vector<CaptureSrc> fwd;
    fwd.reserve(static_cast<size_t>(ncaps));
    for (int32_t i = 0; i < ncaps; ++i) fwd.push_back({VarKind::Capture, i});
    const int32_t fwd_cmap = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back(fwd);
    launch.push_back(b.cell_fresh(1));
    launch.push_back(
        b.assign(VarKind::Cell, 1, b.make_closure(body_idx, fwd_cmap)));
    std::vector<CaptureSrc> runcap{{VarKind::Cell, 1}, {VarKind::Cell, 0}};
    for (int32_t i = 0; i < nparams; ++i) {
      launch.push_back(b.cell_fresh(2 + i));
      launch.push_back(
          b.assign(VarKind::Cell, 2 + i, b.varref(VarKind::Local, i)));
      runcap.push_back({VarKind::Cell, 2 + i});
    }
    const int32_t run_cmap = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back(runcap);
    // CoroResume, not Enqueue: an async function's body runs *synchronously*
    // up to its first `await` (ES2023 27.7.5.1 -- the call only returns to
    // the caller once the body suspends or finishes), so the coroutine is
    // resumed here and now rather than handed to the scheduler. It is
    // $await's CoroYield that ends this resume and hands control back, and
    // from then on the scheduler owns the coroutine: whoever settles the
    // promise it parked on enqueues it. A body with no `await` at all never
    // yields, so this resume runs it to its end and the promise is already
    // settled when the call returns -- which is what JavaScript does too.
    launch.push_back(
        b.intrinsic(IntrinsicId::CoroResume,
                    {b.intrinsic(IntrinsicId::CoroCreate,
                                 {b.make_closure(run_idx, run_cmap)}),
                     b.nil_literal()}));
    launch.push_back(b.make_return(b.varref(VarKind::Cell, 0)));

    Func f;
    f.name = fi.name;
    f.num_params = nparams;
    f.num_locals = nparams;
    for (const int32_t v : fi.params) {
      f.local_names.push_back(rs.vars[static_cast<size_t>(v)].name);
    }
    f.num_cells = 2 + nparams;
    f.num_captures = ncaps;
    f.capture_names = capnames;
    f.lenient_arity = true;
    f.body = b.scope(0, nparams, b.block(launch));
    m.funcs[static_cast<size_t>(index)] = std::move(f);

    // f$run: captures 0 = the body closure, 1 = the promise, 2.. = args.
    std::vector<NodeId> callargs;
    callargs.reserve(static_cast<size_t>(nparams));
    for (int32_t i = 0; i < nparams; ++i) {
      callargs.push_back(b.varref(VarKind::Capture, 2 + i));
    }
    Func run;
    run.name = fi.name + "$run";
    run.num_params = 0;
    run.num_locals = 1;  // the caught value
    run.local_names = {"$exc"};
    run.num_captures = 2 + nparams;
    run.capture_names.push_back("$body");
    run.capture_names.push_back("$promise");
    for (const int32_t v : fi.params) {
      run.capture_names.push_back(rs.vars[static_cast<size_t>(v)].name);
    }
    run.lenient_arity = true;  // the scheduler resumes it as f(nil)
    run.body = b.scope(
        0, 1,
        b.make_try(
            0,
            rtcall("$presolve",
                   {b.varref(VarKind::Capture, 1),
                    b.call_value(b.varref(VarKind::Capture, 0), callargs)}),
            rtcall("$psettle", {b.varref(VarKind::Capture, 1), b.literal(2),
                                b.varref(VarKind::Local, 0)})));
    m.funcs[static_cast<size_t>(run_idx)] = std::move(run);
  }

  Module build(const Ast& program) {
    // The top level is a function too -- the one Module::funcs[0] names.
    const int32_t top = new_fn(-1, "main");
    fns[static_cast<size_t>(top)].body = &program;

    rs.push_scope();
    hoist_funcdecls(program.nodes, top);
    for (const auto& s : program.nodes) resolve_stmt(*s, top);
    rs.pop_scope();

    // Module slots: the entry point, then the runtime helpers (reserved
    // together so they may call each other), then every function the
    // source declared.
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

}  // namespace mini_js
