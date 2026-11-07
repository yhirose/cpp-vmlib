// The smallest front end here, and the one that shows a boundary rather
// than a recipe.
//
// The top-level README's Scope section lists what stays out of reach, and
// one entry is Scheme's by name: "*Multi-shot continuations*: a coroutine
// is one-shot -- its parked frames move, they are not copied -- and
// Scheme's full `call/cc` would need a rule for what a copied cell means
// that nothing here has."
//
// Scheme is the one language that lets that be *shown*. `call/cc` splits
// cleanly in two:
//
//   * The **escape** half -- a continuation invoked while the `call/cc`
//     that made it is still on the stack -- is an unwind to a known point.
//     That is `Tag::TryCatch` and `Tag::Throw`, and it works: see
//     `emit_callcc`, and samples/continuations.scm, which uses it for
//     early exit out of a fold, a generator-shaped search, and a
//     non-local return through several frames.
//   * The **re-entrant** half -- storing a continuation and invoking it
//     after its `call/cc` has already returned -- is what that sentence is
//     about. Nothing here can do it, and the honest thing is to say so
//     rather than to half-do it: invoking a dead escape throws a value
//     nothing will catch.
//
// The second thing this front end is for is **tail calls in a language
// that is nothing but calls**. examples/mini-lua proves the recipe against
// a specification that requires it; Scheme is where iteration *is* tail
// recursion, so `Func::tail_calls` is not an optimization here, it is the
// difference between a `do` loop working and not. Named `let` -- Scheme's
// loop -- is a procedure that tail-calls itself, and that is all it is.
//
// And the third is scale. This binder is a few hundred lines because an
// s-expression needs no expression grammar, no precedence, no statement
// forms and no layout pass. What is left is the part every front end here
// actually has to write: scope resolution, closure conversion, and the
// library.

#include "binder.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
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

namespace mini_scheme {
namespace {

// An escape continuation's two fields, under keys no Scheme symbol can be.
constexpr char kTagKey[] = "\x01" "k";
constexpr char kValKey[] = "\x01" "v";
// A record instance: a plain object under a key no Scheme symbol can
// start with, naming the record type it was made by.
constexpr char kRecKey[] = "\x01" "rec";

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

// A special form always wins over a binding of the same name -- a program
// that rebinds `if` is not in this subset.
bool is_special(const std::string& s) {
  return s == "define" || s == "lambda" || s == "if" || s == "cond" ||
         s == "case" || s == "let" || s == "let*" || s == "letrec" ||
         s == "begin" || s == "set!" || s == "and" || s == "or" ||
         s == "when" || s == "unless" || s == "quote" || s == "else" ||
         s == "define-record-type" || s == "import" ||
         s == "call/cc" || s == "call-with-current-continuation";
}

// How a variadic builtin comes down to its two-argument helper: Left takes
// `(+ a b c)` as `(+ (+ a b) c)`, and Chain takes `(< a b c)` as the
// conjunction of `(< a b)` and `(< b c)`. A direct call folds where it is
// written, since the count is known there; the builtin as a value is a
// procedure with a rest param that folds the same way at run time. Both
// read the rules below, so `(+ a b c)` and `(apply + (list a b c))` cannot
// disagree.
enum class Fold { None, Left, Chain };

// A constant a Fold::Left builtin falls back on.
enum class Unit { None, Zero, One, EmptyString, EmptyList };

// The library, as (name -> the IR func that implements it, arity). Every
// one is an ordinary function, so a symbol naming it is a *value* --
// `(map car xs)` needs `car` to be one, and nothing special had to happen
// for it to be.
struct Builtin {
  const char* scheme;
  const char* rt;
  int32_t arity;  // -1: any count, and a direct call is the binder's own
  Fold fold = Fold::None;
  // Fold::Left only. `none` is what no arguments answer -- `(+)` is 0 --
  // and Unit::None makes that an error. `one_lhs` turns one argument into
  // a two-argument call -- `(- x)` is `(- 0 x)` -- and Unit::None makes
  // one argument answer itself.
  Unit none = Unit::None;
  Unit one_lhs = Unit::None;
};

const std::vector<Builtin>& builtins() {
  static const std::vector<Builtin> v = {
      {"car", "$car", 1},           {"cdr", "$cdr", 1},
      {"cons", "$cons", 2},         {"null?", "$nullp", 1},
      {"pair?", "$pairp", 1},       {"not", "$not", 1},
      {"eq?", "$eqv", 2},           {"eqv?", "$eqv", 2},
      {"equal?", "$equal", 2},
      {"+", "$add", 2, Fold::Left, Unit::Zero},
      {"-", "$sub", 2, Fold::Left, Unit::None, Unit::Zero},
      {"*", "$mul", 2, Fold::Left, Unit::One},
      {"/", "$div", 2, Fold::Left, Unit::None, Unit::One},
      {"<", "$lt", 2, Fold::Chain},
      {">", "$gt", 2, Fold::Chain},
      {"<=", "$le", 2, Fold::Chain},
      {">=", "$ge", 2, Fold::Chain},
      {"=", "$numeq", 2, Fold::Chain},
      {"min", "$min", 2, Fold::Left},
      {"max", "$max", 2, Fold::Left},
      {"append", "$append2", 2, Fold::Left, Unit::EmptyList},
      {"string-append", "$strappend", 2, Fold::Left, Unit::EmptyString},
      {"length", "$length", 1},     {"reverse", "$reverse", 1},
      {"list-ref", "$listref", 2},  {"memq", "$memq", 2},
      {"member", "$member", 2},     {"assoc", "$assoc", 2},
      {"number?", "$numberp", 1},   {"string?", "$stringp", 1},
      {"procedure?", "$procp", 1},  {"boolean?", "$booleanp", 1},
      {"zero?", "$zerop", 1},       {"abs", "$abs", 1},
      {"quotient", "$quotient", 2}, {"remainder", "$remainder", 2},
      {"modulo", "$modulo", 2},     {"expt", "$expt", 2},
      {"number->string", "$numstr", 1},
      {"string-length", "$strlen", 1},
      {"map", "$map1", 2},          {"for-each", "$foreach1", 2},
      {"display", "$display", 1},   {"newline", "$newline", 0},
      // A direct call to either is written in place (emit_form), so the
      // helper is only ever the procedure as a value.
      {"list", "$list", -1},        {"apply", "$apply", -1},
  };
  return v;
}

const Builtin* find_builtin(const std::string& s) {
  for (const auto& b : builtins()) {
    if (s == b.scheme) return &b;
  }
  return nullptr;
}

// The func a builtin is as a value: its helper, or for a folding one the
// procedure with a rest param that runs the fold.
std::string value_rt(const Builtin& bi) {
  return bi.fold == Fold::None ? bi.rt : std::string(bi.rt) + "*";
}

NodeId unit_literal(Unit u, Builder::At& b) {
  switch (u) {
    case Unit::Zero: return b.literal(0);
    case Unit::One: return b.literal(1);
    case Unit::EmptyString: return b.str_literal("");
    case Unit::EmptyList: return b.nil_literal();
    case Unit::None: break;
  }
  coreir_rt::fail("internal error: Unit::None has no literal", 0, 0);
}

struct FnInfo {
  bool is_synth = false;
  std::string name = "lambda";
  std::vector<int32_t> params;
  // The last of `params` collects the arguments past the others, as a
  // list: Func::rest_param, with the array it hands over made a list on
  // the way in (emit_fn).
  bool rest = false;
  const Ast* body = nullptr;  // the enclosing `list`, whose tail is the body
  size_t body_from = 0;
};

// A procedure's formals, read from `from` on: R7RS's three shapes, `(a b)`,
// `(a b . rest)` and a bare `args`, the last two collecting whatever the
// others leave over. The names in order, and whether the last is that one.
struct Formals {
  std::vector<const Ast*> names;
  bool rest = false;
};

Formals formals_of(const Ast& f, size_t from) {
  Formals out;
  if (f.tag == "symbol"_) {
    out.names.push_back(&f);
    out.rest = true;
    return out;
  }
  if (f.tag != "list"_) fail(f, "malformed parameter list");
  for (size_t i = from; i < f.nodes.size(); ++i) {
    const Ast& p = *f.nodes[i];
    if (p.tag != "symbol"_) fail(p, "malformed parameter list");
    if (p.token != ".") {
      out.names.push_back(&p);
      continue;
    }
    // One name after the dot, and nothing after that.
    if (i + 2 != f.nodes.size() || f.nodes[i + 1]->tag != "symbol"_ ||
        f.nodes[i + 1]->token == ".") {
      fail(p, "malformed parameter list");
    }
    out.names.push_back(f.nodes[i + 1].get());
    out.rest = true;
    break;
  }
  return out;
}

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
  HelperTable rt{m};  // the runtime helpers, by name

  // `define-record-type`: the constructor's field order, and each field's
  // name paired with its accessor's (and optional mutator's) synthesized
  // function -- resolved once, used only at emit time.
  struct RecordField {
    std::string name;
    int32_t accessor_fn;   // fns[] index -- the closure's own body
    int32_t accessor_var;  // vars[] index -- what the name is bound to
    int32_t mutator_fn = -1;
    int32_t mutator_var = -1;
  };
  struct RecordInfo {
    std::string type_name;
    std::vector<std::string> ctor_fields;
    std::vector<RecordField> fields;
  };
  std::map<const Ast*, RecordInfo> records;

  static std::string head_of(const Ast& l) {
    if (l.tag != "list"_ || l.nodes.empty()) return {};
    if (l.nodes[0]->tag != "symbol"_) return {};
    return std::string(l.nodes[0]->token);
  }

  // ==== Pass A: scopes, declarations, captures =============================


  // A second `define` of the same name in one body names the binding that
  // is already there rather than making a fresh one.
  int32_t declare(const std::string& name, int32_t fn) {
    if (const auto v = rs.declared_here(name)) return *v;
    return rs.declare(name, fn);
  }

  int32_t new_fn(int32_t parent, const std::string& name) {
    const int32_t f = rs.new_fn(parent);
    fns.push_back({});
    fns[static_cast<size_t>(f)].name = name;
    return f;
  }

  // Internal `define`s are letrec, not sequential: every name a body
  // defines is visible to every other, which is what lets two of them
  // recurse into each other.
  void predeclare(const Ast& body, size_t from, int32_t fn) {
    for (size_t i = from; i < body.nodes.size(); ++i) {
      const Ast& f = *body.nodes[i];
      if (head_of(f) != "define" || f.nodes.size() < 2) continue;
      const Ast& t = *f.nodes[1];
      const Ast& id = t.tag == "list"_ ? *t.nodes[0] : t;
      decl_of[&id] = declare(std::string(id.token), fn);
    }
  }

  // A procedure: its formals read from `formals_from` on -- a `define`'s
  // signature `(f a b)` starts with the name -- and its body from `from`
  // on.
  int32_t resolve_lambda(const Ast& node, const Ast& formals,
                         size_t formals_from, const Ast& body, size_t from,
                         int32_t parent, const std::string& name) {
    const int32_t f = new_fn(parent, name);
    fns[static_cast<size_t>(f)].body = &body;
    fns[static_cast<size_t>(f)].body_from = from;
    fn_of[&node] = f;
    rs.push_scope();
    const Formals fs = formals_of(formals, formals_from);
    for (const Ast* p : fs.names) {
      const int32_t v = declare(std::string(p->token), f);
      decl_of[p] = v;
      fns[static_cast<size_t>(f)].params.push_back(v);
    }
    fns[static_cast<size_t>(f)].rest = fs.rest;
    predeclare(body, from, f);
    for (size_t i = from; i < body.nodes.size(); ++i) {
      resolve_form(*body.nodes[i], f);
    }
    rs.pop_scope();
    return f;
  }

  void resolve_record_type(const Ast& a, int32_t fn) {
    if (a.nodes.size() < 4) fail(a, "malformed define-record-type");
    RecordInfo ri;
    ri.type_name = std::string(a.nodes[1]->token);
    decl_of[a.nodes[1].get()] = declare(ri.type_name, fn);
    const Ast& ctorspec = *a.nodes[2];
    decl_of[ctorspec.nodes[0].get()] =
        declare(std::string(ctorspec.nodes[0]->token), fn);
    for (size_t i = 1; i < ctorspec.nodes.size(); ++i) {
      ri.ctor_fields.push_back(std::string(ctorspec.nodes[i]->token));
    }
    decl_of[a.nodes[3].get()] = declare(std::string(a.nodes[3]->token), fn);
    for (size_t i = 4; i < a.nodes.size(); ++i) {
      const Ast& fs = *a.nodes[i];
      RecordField rf;
      rf.name = std::string(fs.nodes[0]->token);
      rf.accessor_fn = new_fn(fn, std::string(fs.nodes[1]->token));
      fns[static_cast<size_t>(rf.accessor_fn)].is_synth = true;
      rf.accessor_var = declare(std::string(fs.nodes[1]->token), fn);
      decl_of[fs.nodes[1].get()] = rf.accessor_var;
      if (fs.nodes.size() > 2) {
        rf.mutator_fn = new_fn(fn, std::string(fs.nodes[2]->token));
        fns[static_cast<size_t>(rf.mutator_fn)].is_synth = true;
        rf.mutator_var = declare(std::string(fs.nodes[2]->token), fn);
        decl_of[fs.nodes[2].get()] = rf.mutator_var;
      }
      ri.fields.push_back(rf);
    }
    records[&a] = std::move(ri);
  }

  void resolve_form(const Ast& a, int32_t fn) {
    if (a.tag == "symbol"_) {
      const std::string s(a.token);
      if (auto v = rs.resolve(s, fn)) {
        ref_of[&a] = *v;
        return;
      }
      if (find_builtin(s) != nullptr || is_special(s)) return;
      fail(a, "unbound variable: " + s);
    }
    if (a.tag != "list"_) return;  // an atom, or a quoted form
    const std::string h = head_of(a);
    if (h == "quote") return;
    if (h == "define") {
      if (a.nodes.size() < 2) fail(a, "malformed define");
      const Ast& t = *a.nodes[1];
      if (t.tag == "list"_) {
        // (define (f a b) body...) -- the sugar for a lambda.
        const Ast& id = *t.nodes[0];
        if (!decl_of.count(&id)) {
          decl_of[&id] = declare(std::string(id.token), fn);
        }
        resolve_lambda(a, t, 1, a, 2, fn, std::string(id.token));
        return;
      }
      if (!decl_of.count(&t)) decl_of[&t] = declare(std::string(t.token), fn);
      for (size_t i = 2; i < a.nodes.size(); ++i) resolve_form(*a.nodes[i], fn);
      return;
    }
    if (h == "lambda") {
      if (a.nodes.size() < 2) fail(a, "malformed lambda");
      resolve_lambda(a, *a.nodes[1], 0, a, 2, fn, "lambda");
      return;
    }
    if (h == "define-record-type") {
      resolve_record_type(a, fn);
      return;
    }
    // `(import (scheme base) ...)` -- library names, not expressions;
    // this front end has one namespace and nothing to import, so it is
    // the R7RS boilerplate a program needs to run under `guile` unchanged
    // and this binder simply does not have to act on.
    if (h == "import") return;
    if (h == "case") {
      resolve_form(*a.nodes[1], fn);
      for (size_t i = 2; i < a.nodes.size(); ++i) {
        const Ast& clause = *a.nodes[i];
        // clause.nodes[0] is a list of literal data, not forms -- nothing
        // in it names a variable, so only the body needs resolving.
        for (size_t k = 1; k < clause.nodes.size(); ++k) {
          resolve_form(*clause.nodes[k], fn);
        }
      }
      return;
    }
    if (h == "let" || h == "let*" || h == "letrec") {
      resolve_let(a, h, fn);
      return;
    }
    if (h == "set!") {
      if (a.nodes.size() != 3) fail(a, "malformed set!");
      resolve_form(*a.nodes[1], fn);
      resolve_form(*a.nodes[2], fn);
      return;
    }
    for (size_t i = (h.empty() ? 0 : 1); i < a.nodes.size(); ++i) {
      resolve_form(*a.nodes[i], fn);
    }
    if (!h.empty() && !is_special(h)) resolve_form(*a.nodes[0], fn);
  }

  void resolve_let(const Ast& a, const std::string& kind, int32_t fn) {
    // Named let: (let loop ((i 0)) body...) -- a procedure that calls
    // itself, which is Scheme's loop and the reason tail calls matter.
    const bool named = a.nodes.size() > 1 && a.nodes[1]->tag == "symbol"_;
    const size_t bind_at = named ? 2 : 1;
    if (a.nodes.size() <= bind_at) fail(a, "malformed let");
    const Ast& binds = *a.nodes[bind_at];

    if (named) {
      // The initializers are evaluated outside, the body inside.
      for (const auto& bpair : binds.nodes) {
        if (bpair->nodes.size() > 1) resolve_form(*bpair->nodes[1], fn);
      }
      rs.push_scope();
      const Ast& id = *a.nodes[1];
      decl_of[&id] = declare(std::string(id.token), fn);
      const int32_t f = new_fn(fn, std::string(id.token));
      fns[static_cast<size_t>(f)].body = &a;
      fns[static_cast<size_t>(f)].body_from = bind_at + 1;
      fn_of[&a] = f;
      rs.push_scope();
      for (const auto& bpair : binds.nodes) {
        const Ast& p = *bpair->nodes[0];
        const int32_t v = declare(std::string(p.token), f);
        decl_of[&p] = v;
        fns[static_cast<size_t>(f)].params.push_back(v);
      }
      predeclare(a, bind_at + 1, f);
      for (size_t i = bind_at + 1; i < a.nodes.size(); ++i) {
        resolve_form(*a.nodes[i], f);
      }
      rs.pop_scope();
      rs.pop_scope();
      return;
    }

    if (kind == "letrec") {
      rs.push_scope();
      for (const auto& bpair : binds.nodes) {
        const Ast& p = *bpair->nodes[0];
        decl_of[&p] = declare(std::string(p.token), fn);
      }
      for (const auto& bpair : binds.nodes) {
        if (bpair->nodes.size() > 1) resolve_form(*bpair->nodes[1], fn);
      }
    } else if (kind == "let*") {
      // Each binding opens a scope of its own around the rest, so a name
      // bound twice is two variables: an initializer between the two saw
      // the first, and a closure made there keeps it. (`declare` would
      // reuse the first, which is what a second internal define means.)
      rs.push_scope();
      for (const auto& bpair : binds.nodes) {
        if (bpair->nodes.size() > 1) resolve_form(*bpair->nodes[1], fn);
        const Ast& p = *bpair->nodes[0];
        decl_of[&p] = rs.declare(std::string(p.token), fn);
      }
    } else {
      for (const auto& bpair : binds.nodes) {
        if (bpair->nodes.size() > 1) resolve_form(*bpair->nodes[1], fn);
      }
      rs.push_scope();
      for (const auto& bpair : binds.nodes) {
        const Ast& p = *bpair->nodes[0];
        decl_of[&p] = declare(std::string(p.token), fn);
      }
    }
    predeclare(a, bind_at + 1, fn);
    for (size_t i = bind_at + 1; i < a.nodes.size(); ++i) {
      resolve_form(*a.nodes[i], fn);
    }
    rs.pop_scope();
  }

  // ==== The library, written in this front end's own IR ====================
  //
  // A pair is a two-element Array: `car` is [0] and `cdr` is [1], and the
  // empty list is nil. Nothing else in this subset produces an Array, so
  // TypeOf tells a pair from everything else with no tag of its own.

  static const std::vector<std::string>& rt_names() {
    static std::vector<std::string> names;
    if (names.empty()) {
      for (const auto& b : builtins()) {
        for (const std::string& n : {std::string(b.rt), value_rt(b)}) {
          if (std::find(names.begin(), names.end(), n) == names.end()) {
            names.push_back(n);
          }
        }
      }
      for (const char* extra : {"$true", "$disp", "$listbody", "$fstr",
                                "$escape", "$arrlist", "$listarr"}) {
        names.push_back(extra);
      }
    }
    return names;
  }

  // One helper of the runtime above, with the front end's own additions to
  // coreir::HelperWriter.
  struct RT : HelperWriter {
    explicit RT(Binder& bd) : HelperWriter(bd.rt) {
      // Scheme's own rule, and the one every library procedure here is
      // written to rely on: a call in tail position reuses the frame.
      tail_calls = true;
    }

    NodeId pairp(NodeId v) { return is(typ(v), "array"); }

    NodeId car(NodeId v) { return idx(v, I(0)); }

    NodeId cdr(NodeId v) { return idx(v, I(1)); }

    NodeId err(const std::string& msg) { return b.make_throw(S(msg)); }
  };

  void one(const std::string& name, int32_t nparams,
           const std::function<NodeId(RT&)>& body,
           const std::vector<std::string>& names) {
    RT r(*this);
    for (int32_t i = 0; i < nparams; ++i)
      r.param(names[static_cast<size_t>(i)]);
    for (size_t i = static_cast<size_t>(nparams); i < names.size(); ++i) {
      r.local(names[i]);
    }
    r.add(r.ret(body(r)));
    r.finish(name);
  }

  void emit_runtime() {
    // Pairs.
    one("$car", 1, [&](RT& r) {
      return r.iff(r.pairp(r.L(0)), r.car(r.L(0)),
                   r.blk({r.err("car: not a pair"), r.Nil()}));
    }, {"p"});
    one("$cdr", 1, [&](RT& r) {
      return r.iff(r.pairp(r.L(0)), r.cdr(r.L(0)),
                   r.blk({r.err("cdr: not a pair"), r.Nil()}));
    }, {"p"});
    one("$cons", 2, [&](RT& r) { return r.arr({r.L(0), r.L(1)}); }, {"a", "d"});
    one("$nullp", 1, [&](RT& r) { return r.is(r.typ(r.L(0)), "nil"); }, {"v"});
    one("$pairp", 1, [&](RT& r) { return r.pairp(r.L(0)); }, {"v"});

    // Only #f is false: `'()` and 0 are both true, where Value::truthy()
    // calls both false. Its comment says why it refuses to choose.
    one("$true", 1, [&](RT& r) {
      return r.iff(r.is(r.typ(r.L(0)), "bool"), r.L(0), r.Bo(true));
    }, {"v"});
    one("$not", 1, [&](RT& r) {
      return r.bin(BinOp::Eq, r.call("$true", {r.L(0)}), r.Bo(false));
    }, {"v"});

    // Numbers. Two exact integers stay exact, which is eval_binop's own
    // rule; `/` on two exact integers would be a *rational* in Scheme, and
    // this subset has none -- see README.md.
    one("$add", 2, [&](RT& r) { return r.bin(BinOp::Add, r.L(0), r.L(1)); },
        {"a", "b"});
    one("$sub", 2, [&](RT& r) { return r.bin(BinOp::Sub, r.L(0), r.L(1)); },
        {"a", "b"});
    one("$mul", 2, [&](RT& r) { return r.bin(BinOp::Mul, r.L(0), r.L(1)); },
        {"a", "b"});
    one("$div", 2, [&](RT& r) {
      return r.bin(BinOp::Div, r.in(IntrinsicId::ToDouble, {r.L(0)}),
                   r.in(IntrinsicId::ToDouble, {r.L(1)}));
    }, {"a", "b"});
    one("$lt", 2, [&](RT& r) { return r.bin(BinOp::Lt, r.L(0), r.L(1)); },
        {"a", "b"});
    one("$gt", 2, [&](RT& r) { return r.bin(BinOp::Gt, r.L(0), r.L(1)); },
        {"a", "b"});
    one("$le", 2, [&](RT& r) { return r.bin(BinOp::Le, r.L(0), r.L(1)); },
        {"a", "b"});
    one("$ge", 2, [&](RT& r) { return r.bin(BinOp::Ge, r.L(0), r.L(1)); },
        {"a", "b"});
    one("$numeq", 2, [&](RT& r) { return r.bin(BinOp::Eq, r.L(0), r.L(1)); },
        {"a", "b"});
    one("$zerop", 1, [&](RT& r) { return r.bin(BinOp::Eq, r.L(0), r.I(0)); },
        {"v"});
    one("$abs", 1,
        [&](RT& r) {
          return r.iff(r.bin(BinOp::Lt, r.L(0), r.I(0)),
                       r.b.unary(UnOp::Neg, r.L(0)), r.L(0));
        },
        {"v"});
    one("$min", 2, [&](RT& r) {
      return r.iff(r.bin(BinOp::Lt, r.L(0), r.L(1)), r.L(0), r.L(1));
    }, {"a", "b"});
    one("$max", 2, [&](RT& r) {
      return r.iff(r.bin(BinOp::Gt, r.L(0), r.L(1)), r.L(0), r.L(1));
    }, {"a", "b"});
    one("$quotient", 2, [&](RT& r) {
      return r.bin(BinOp::Div, r.L(0), r.L(1));
    }, {"a", "b"});
    one("$remainder", 2, [&](RT& r) {
      return r.bin(BinOp::Mod, r.L(0), r.L(1));
    }, {"a", "b"});
    // `modulo` follows the divisor's sign; `remainder` follows the
    // dividend's -- BinOp::Mod is the second, so the first is a correction.
    rt_modulo();
    rt_expt();

    // Predicates.
    one("$numberp", 1, [&](RT& r) {
      return r.either(r.is(r.typ(r.L(0)), "int"), r.is(r.typ(r.L(0)), "double"));
    }, {"v"});
    one("$stringp", 1, [&](RT& r) { return r.is(r.typ(r.L(0)), "string"); },
        {"v"});
    one("$procp", 1, [&](RT& r) { return r.is(r.typ(r.L(0)), "function"); },
        {"v"});
    one("$booleanp", 1, [&](RT& r) { return r.is(r.typ(r.L(0)), "bool"); },
        {"v"});

    rt_eqv();
    rt_equal();
    rt_length();
    rt_append2();
    rt_reverse();
    rt_listref();
    rt_mem("$memq", "$eqv");
    rt_mem("$member", "$equal");
    rt_assoc();
    rt_map1();
    rt_foreach1();
    rt_numstr();
    one("$strappend", 2, [&](RT& r) {
      return r.bin(BinOp::Add, r.L(0), r.L(1));
    }, {"a", "b"});
    one("$strlen", 1, [&](RT& r) { return r.len(r.L(0)); }, {"s"});
    rt_fstr();
    rt_disp();
    rt_listbody();
    one("$display", 1, [&](RT& r) {
      return r.blk({r.nat("write", {r.call("$disp", {r.L(0)})}), r.Nil()});
    }, {"v"});
    one("$newline", 0, [&](RT& r) {
      return r.blk({r.nat("write", {r.S("\n")}), r.Nil()});
    }, {});
    rt_escape();

    // What a rest param and a spread need: an array and a list each way,
    // `list` and `apply` as values, and every folding builtin as one.
    rt_arrlist();
    rt_listarr();
    rt_list();
    rt_apply();
    for (const Builtin& bi : builtins()) {
      if (bi.fold != Fold::None) rt_fold_value(bi);
    }
  }

  // A rest param arrives as an array, which emit_fn turns into a list
  // before the body's first form -- built back to front, so each pair is
  // made once.
  void rt_arrlist() {
    RT r(*this);
    const auto [a] = r.params("a");
    const auto [out, i] = r.locals("out", "i");
    r.add(r.set(out, r.Nil()));
    r.add(r.set(i, r.len(r.L(a))));
    r.add(r.wh(r.bin(BinOp::Gt, r.L(i), r.I(0)),
               r.blk({r.set(i, r.bin(BinOp::Sub, r.L(i), r.I(1))),
                      r.set(out, r.arr({r.idx(r.L(a), r.L(i)), r.L(out)}))})));
    r.add(r.ret(r.L(out)));
    r.finish("$arrlist");
  }

  // apply's last argument, as the array a CallSpread spreads. It has to be
  // a proper list: anything but '() at its end is an error, as in R7RS.
  void rt_listarr() {
    RT r(*this);
    const auto [l] = r.params("l");
    const auto [out, p] = r.locals("out", "p");
    r.add(r.set(out, r.arr({})));
    r.add(r.set(p, r.L(l)));
    r.add(r.wh(r.pairp(r.L(p)), r.blk({r.push(r.L(out), r.car(r.L(p))),
                                       r.set(p, r.cdr(r.L(p)))})));
    r.add(r.iff(r.isnt(r.typ(p), "nil"),
                r.err("apply: last argument is not a list")));
    r.add(r.ret(r.L(out)));
    r.finish("$listarr");
  }

  // `list` as a value. A direct call builds its pairs in place instead.
  void rt_list() {
    RT r(*this);
    r.rest_param = true;
    const auto [xs] = r.params("xs");
    r.add(r.ret(r.call("$arrlist", {r.L(xs)})));
    r.finish("$list");
  }

  // `apply` as a value; a direct call is the same CallSpread written in
  // place (emit_form). The call is in tail position here too, so a tail
  // call through it stays one, as R7RS requires of apply: the caller's
  // frame becomes this one, and this one becomes the callee's.
  void rt_apply() {
    RT r(*this);
    r.rest_param = true;
    const auto [args] = r.params("args");
    const auto [last, spread] = r.locals("last", "spread");
    r.add(r.set(last, r.bin(BinOp::Sub, r.len(r.L(args)), r.I(1))));
    r.add(r.iff(r.bin(BinOp::Lt, r.L(last), r.I(1)),
                r.err("apply needs a procedure and a list")));
    // The arguments between the procedure and the list, then the list's.
    r.add(r.set(spread, r.in(IntrinsicId::ArraySlice,
                             {r.L(args), r.I(1), r.L(last)})));
    r.add(r.in(IntrinsicId::ArrayConcat,
               {r.L(spread),
                r.call("$listarr", {r.idx(r.L(args), r.L(last))})}));
    r.add(r.ret(r.b.call_spread(r.idx(r.L(args), r.I(0)), {}, r.L(spread))));
    r.finish("$apply");
  }

  // A folding builtin as a value: the fold emit_builtin_call writes out at
  // a direct call, run over a rest param instead, from the same Builtin
  // fields.
  void rt_fold_value(const Builtin& bi) {
    RT r(*this);
    r.rest_param = true;
    const auto [xs] = r.params("xs");
    const auto [n, i] = r.locals("n", "i");
    auto x = [&](NodeId k) { return r.idx(r.L(xs), k); };
    auto next = [&] { return r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1))); };
    r.add(r.set(n, r.len(r.L(xs))));
    r.add(r.set(i, r.I(1)));
    if (bi.fold == Fold::Chain) {
      const NodeId prev = x(r.bin(BinOp::Sub, r.L(i), r.I(1)));
      r.add(r.wh(r.bin(BinOp::Lt, r.L(i), r.L(n)),
                 r.blk({r.iff(r.bin(BinOp::Eq, r.call(bi.rt, {prev, x(r.L(i))}),
                                    r.Bo(false)),
                              r.ret(r.Bo(false))),
                        next()})));
      r.add(r.ret(r.Bo(true)));
    } else {
      const auto [acc] = r.locals("acc");
      r.add(r.iff(r.bin(BinOp::Eq, r.L(n), r.I(0)),
                  bi.none == Unit::None
                      ? r.err(std::string(bi.scheme) +
                              " needs at least one argument")
                      : r.ret(unit_literal(bi.none, r.b))));
      r.add(r.iff(r.bin(BinOp::Eq, r.L(n), r.I(1)),
                  r.ret(bi.one_lhs == Unit::None
                            ? x(r.I(0))
                            : r.call(bi.rt, {unit_literal(bi.one_lhs, r.b),
                                             x(r.I(0))}))));
      r.add(r.set(acc, x(r.I(0))));
      r.add(r.wh(r.bin(BinOp::Lt, r.L(i), r.L(n)),
                 r.blk({r.set(acc, r.call(bi.rt, {r.L(acc), x(r.L(i))})),
                        next()})));
      r.add(r.ret(r.L(acc)));
    }
    r.finish(value_rt(bi));
  }

  void rt_modulo() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [m] = r.locals("m");
    r.add(r.set(m, r.bin(BinOp::Mod, r.L(a), r.L(b))));
    r.add(r.iff(
        r.both(r.bin(BinOp::Ne, r.L(m), r.I(0)),
               r.bin(BinOp::Lt, r.bin(BinOp::Mul, r.L(m), r.L(b)), r.I(0))),
        r.set(m, r.bin(BinOp::Add, r.L(m), r.L(b)))));
    r.add(r.ret(r.L(m)));
    r.finish("$modulo");
  }

  void rt_expt() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    const auto [acc, i] = r.locals("acc", "i");
    r.add(r.iff(r.either(r.is(r.typ(a), "double"),
                         r.either(r.is(r.typ(b), "double"),
                                  r.bin(BinOp::Lt, r.L(b), r.I(0)))),
                r.ret(r.in(IntrinsicId::Pow, {r.L(a), r.L(b)}))));
    r.add(r.set(acc, r.I(1)));
    r.add(r.set(i, r.I(0)));
    r.add(r.wh(r.bin(BinOp::Lt, r.L(i), r.L(b)),
               r.blk({r.set(acc, r.bin(BinOp::Mul, r.L(acc), r.L(a))),
                      r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))})));
    r.add(r.ret(r.L(acc)));
    r.finish("$expt");
  }

  // eq? on two immediates is their value; on two heap values it is
  // identity -- except that a symbol is a string here (see README.md), so
  // strings compare by value and `(eq? 'a 'a)` answers what Scheme does.
  void rt_eqv() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    r.add(r.iff(r.bin(BinOp::Ne, r.typ(a), r.typ(b)), r.ret(r.Bo(false))));
    r.add(r.iff(r.is(r.typ(a), "nil"), r.ret(r.Bo(true))));
    r.add(r.iff(r.either(r.is(r.typ(a), "bool"),
                         r.either(r.is(r.typ(a), "int"),
                                  r.either(r.is(r.typ(a), "double"),
                                           r.is(r.typ(a), "string")))),
                r.ret(r.bin(BinOp::Eq, r.L(a), r.L(b)))));
    r.add(r.ret(r.in(IntrinsicId::Same, {r.L(a), r.L(b)})));
    r.finish("$eqv");
  }

  void rt_equal() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    r.add(
        r.iff(r.both(r.pairp(r.L(a)), r.pairp(r.L(b))),
              r.ret(r.both(r.call("$equal", {r.car(r.L(a)), r.car(r.L(b))}),
                           r.call("$equal", {r.cdr(r.L(a)), r.cdr(r.L(b))})))));
    r.add(r.ret(r.call("$eqv", {r.L(a), r.L(b)})));
    r.finish("$equal");
  }

  void rt_length() {
    RT r(*this);
    const auto [l] = r.params("l");
    const auto [n, p] = r.locals("n", "p");
    r.add(r.set(n, r.I(0)));
    r.add(r.set(p, r.L(l)));
    r.add(r.wh(r.pairp(r.L(p)),
               r.blk({r.set(n, r.bin(BinOp::Add, r.L(n), r.I(1))),
                      r.set(p, r.cdr(r.L(p)))})));
    r.add(r.ret(r.L(n)));
    r.finish("$length");
  }

  void rt_append2() {
    RT r(*this);
    const auto [a, b] = r.params("a", "b");
    r.add(r.iff(r.bin(BinOp::Eq, r.pairp(r.L(a)), r.Bo(false)), r.ret(r.L(b))));
    r.add(r.ret(
        r.arr({r.car(r.L(a)), r.call("$append2", {r.cdr(r.L(a)), r.L(b)})})));
    r.finish("$append2");
  }

  void rt_reverse() {
    RT r(*this);
    const auto [l] = r.params("l");
    const auto [out, p] = r.locals("out", "p");
    r.add(r.set(out, r.Nil()));
    r.add(r.set(p, r.L(l)));
    r.add(r.wh(r.pairp(r.L(p)),
               r.blk({r.set(out, r.arr({r.car(r.L(p)), r.L(out)})),
                      r.set(p, r.cdr(r.L(p)))})));
    r.add(r.ret(r.L(out)));
    r.finish("$reverse");
  }

  void rt_listref() {
    RT r(*this);
    const auto [l, k] = r.params("l", "k");
    const auto [p, i] = r.locals("p", "i");
    r.add(r.set(p, r.L(l)));
    r.add(r.set(i, r.L(k)));
    r.add(r.wh(r.both(r.bin(BinOp::Gt, r.L(i), r.I(0)), r.pairp(r.L(p))),
               r.blk({r.set(p, r.cdr(r.L(p))),
                      r.set(i, r.bin(BinOp::Sub, r.L(i), r.I(1)))})));
    r.add(r.iff(r.bin(BinOp::Eq, r.pairp(r.L(p)), r.Bo(false)),
                r.err("list-ref: index out of range")));
    r.add(r.ret(r.car(r.L(p))));
    r.finish("$listref");
  }

  // memq and member: the first tail of l whose car is x, compared by eq?
  // or by equal? -- the one difference between them.
  void rt_mem(const std::string& name, const std::string& same) {
    RT r(*this);
    const auto [x, l] = r.params("x", "l");
    const auto [p] = r.locals("p");
    r.add(r.set(p, r.L(l)));
    r.add(r.wh(
        r.pairp(r.L(p)),
        r.blk({r.iff(r.call(same, {r.L(x), r.car(r.L(p))}), r.ret(r.L(p))),
               r.set(p, r.cdr(r.L(p)))})));
    r.add(r.ret(r.Bo(false)));
    r.finish(name);
  }

  void rt_assoc() {
    RT r(*this);
    const auto [x, l] = r.params("x", "l");
    const auto [p] = r.locals("p");
    r.add(r.set(p, r.L(l)));
    r.add(r.wh(
        r.pairp(r.L(p)),
        r.blk({r.iff(r.both(r.pairp(r.car(r.L(p))),
                            r.call("$equal", {r.L(x), r.car(r.car(r.L(p)))})),
                     r.ret(r.car(r.L(p)))),
               r.set(p, r.cdr(r.L(p)))})));
    r.add(r.ret(r.Bo(false)));
    r.finish("$assoc");
  }

  void rt_map1() {
    RT r(*this);
    const auto [f, l] = r.params("f", "l");
    r.add(
        r.iff(r.bin(BinOp::Eq, r.pairp(r.L(l)), r.Bo(false)), r.ret(r.Nil())));
    r.add(r.ret(r.arr({r.b.call_value(r.L(f), {r.car(r.L(l))}),
                       r.call("$map1", {r.L(f), r.cdr(r.L(l))})})));
    r.finish("$map1");
  }

  void rt_foreach1() {
    RT r(*this);
    const auto [f, l] = r.params("f", "l");
    const auto [p] = r.locals("p");
    r.add(r.set(p, r.L(l)));
    r.add(r.wh(r.pairp(r.L(p)), r.blk({r.b.call_value(r.L(f), {r.car(r.L(p))}),
                                       r.set(p, r.cdr(r.L(p)))})));
    r.add(r.ret(r.Nil()));
    r.finish("$foreach1");
  }

  void rt_numstr() {
    RT r(*this);
    const auto [n] = r.params("n");
    r.add(r.iff(r.is(r.typ(n), "double"), r.ret(r.call("$fstr", {r.L(n)}))));
    r.add(r.ret(r.in(IntrinsicId::ToStr, {r.L(n)})));
    r.finish("$numstr");
  }

  // Guile prints an inexact number as to_display does, with a ".0" forced
  // onto a whole one -- to_display's own comment says a front end that
  // cares builds that string, and this one cares, as runtime_parts.h's
  // float_str does for Python, Ruby and culebra.
  void rt_fstr() {
    RT r(*this);
    float_str(r, "+nan.0");
    r.finish("$fstr");
  }

  void rt_disp() {
    RT r(*this);
    const auto [v] = r.params("v");
    const auto [out, keys, i] = r.locals("out", "keys", "i");
    r.add(r.iff(r.is(r.typ(v), "nil"), r.ret(r.S("()"))));
    r.add(r.iff(r.is(r.typ(v), "bool"),
                r.ret(r.iff(r.L(v), r.S("#t"), r.S("#f")))));
    r.add(r.iff(r.is(r.typ(v), "string"), r.ret(r.L(v))));
    r.add(r.iff(r.is(r.typ(v), "double"), r.ret(r.call("$fstr", {r.L(v)}))));
    r.add(r.iff(r.is(r.typ(v), "int"),
                r.ret(r.in(IntrinsicId::ToStr, {r.L(v)}))));
    r.add(r.iff(
        r.is(r.typ(v), "array"),
        r.ret(r.bin(BinOp::Add,
                    r.bin(BinOp::Add, r.S("("), r.call("$listbody", {r.L(v)})),
                    r.S(")")))));
    // A record: `#<type field: value ...>`, guile's own form, fields in
    // the order they were declared.
    {
      const NodeId key = r.idx(r.L(keys), r.L(i));
      const NodeId field =
          r.bin(BinOp::Add, r.bin(BinOp::Add, r.S(" "), key),
                r.bin(BinOp::Add, r.S(": "),
                      r.call("$disp", {r.idx(r.L(v), r.idx(r.L(keys),
                                                           r.L(i)))})));
      const NodeId each = r.blk(
          {r.iff(r.bin(BinOp::Ne, r.idx(r.L(keys), r.L(i)), r.S(kRecKey)),
                 r.set(out, r.bin(BinOp::Add, r.L(out), field))),
           r.set(i, r.bin(BinOp::Add, r.L(i), r.I(1)))});
      r.add(r.iff(
          r.both(r.is(r.typ(v), "object"), r.has(r.L(v), r.S(kRecKey))),
          r.blk({r.set(out, r.bin(BinOp::Add, r.S("#<"),
                                  r.idx(r.L(v), kRecKey))),
                 r.set(keys, r.in(IntrinsicId::ObjectKeys, {r.L(v)})),
                 r.set(i, r.I(0)),
                 r.wh(r.bin(BinOp::Lt, r.L(i), r.len(r.L(keys))), each),
                 r.ret(r.bin(BinOp::Add, r.L(out), r.S(">")))})));
    }
    r.add(r.ret(r.S("#<procedure>")));
    r.finish("$disp");
  }

  // The improper tail is what makes this its own function: `(1 . 2)` and
  // `(1 2)` differ only in what the last cdr turns out to be.
  void rt_listbody() {
    RT r(*this);
    const auto [p] = r.params("p");
    const auto [out, rest] = r.locals("out", "rest");
    r.add(r.set(out, r.call("$disp", {r.car(r.L(p))})));
    r.add(r.set(rest, r.cdr(r.L(p))));
    r.add(r.wh(
        r.pairp(r.L(rest)),
        r.blk({r.set(out, r.bin(BinOp::Add, r.L(out),
                                r.bin(BinOp::Add, r.S(" "),
                                      r.call("$disp", {r.car(r.L(rest))})))),
               r.set(rest, r.cdr(r.L(rest)))})));
    r.add(r.iff(r.bin(BinOp::Ne, r.typ(rest), r.S("nil")),
                r.set(out, r.bin(BinOp::Add, r.L(out),
                                 r.bin(BinOp::Add, r.S(" . "),
                                       r.call("$disp", {r.L(rest)}))))));
    r.add(r.ret(r.L(out)));
    r.finish("$listbody");
  }

  // An escape continuation, as a procedure: invoking it throws a value
  // tagged with the identity of the `call/cc` that made it, and that
  // `call/cc`'s TryCatch is what catches it. One capture, the tag.
  void rt_escape() {
    RT r(*this);
    const auto [v] = r.params("v");
    r.add(r.b.make_throw(
        r.b.object_lit({{r.S(kTagKey), r.P(0)}, {r.S(kValKey), r.L(v)}})));
    r.finish("$escape", 0, 1);
  }

  // ==== Pass B: emit ======================================================

  NodeId emit_closure(int32_t g, FnCtx& ctx, SrcPos p) {
    return rs.closure(m, ctx.fn, g, p);
  }

  NodeId read_var(int32_t v, FnCtx& ctx, SrcPos p) {
    return rs.read(m, ctx.fn, v, p);
  }

  NodeId write_var(int32_t v, NodeId value, FnCtx& ctx, SrcPos p,
                   bool fresh = false) {
    auto b = Builder(m).at(p);
    const auto [k, i] = rs.access(ctx.fn, v);
    if (fresh && k == VarKind::Cell) {
      return b.block({b.cell_fresh(i), b.assign(k, i, value)});
    }
    return b.assign(k, i, value);
  }

  NodeId truthy(NodeId v, SrcPos p) {
    return rt.call("$true", {v}, p);
  }

  // A quoted datum, built at bind time: a list becomes a chain of pairs, a
  // symbol becomes a string (see README.md for what that costs), and every
  // other atom is itself.
  NodeId datum(const Ast& a, SrcPos p) {
    auto b = Builder(m).at(p);
    if (a.tag == "list"_) {
      // `(a b . c)` ends in c rather than '(): the dot, one datum after it,
      // and at least one before it, which is the only place R7RS allows a
      // dot in a list.
      size_t n = a.nodes.size();
      NodeId acc = b.nil_literal();
      for (size_t i = 0; i < n; ++i) {
        const Ast& x = *a.nodes[i];
        if (x.tag != "symbol"_ || x.token != ".") continue;
        if (i == 0 || i + 2 != n) fail(x, "malformed dotted list");
        acc = datum(*a.nodes[i + 1], p);
        n = i;
        break;
      }
      for (size_t i = n; i-- > 0;) {
        acc = b.array_lit({datum(*a.nodes[i], p), acc});
      }
      return acc;
    }
    // A quote inside quoted data stays: `'(a 'b)` is `(a (quote b))`. (The
    // outermost quote is the one emit_form strips before it gets here.)
    if (a.tag == "quoted"_) {
      return b.array_lit(
          {b.str_literal("quote"),
           b.array_lit({datum(*a.nodes[0], p), b.nil_literal()})});
    }
    return atom(a, p);
  }

  NodeId atom(const Ast& a, SrcPos p) {
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
      case "boolean"_:
        return b.bool_literal(a.token == "#t");
      default:
        return b.str_literal(std::string(a.token));  // a symbol
    }
  }

  // A sequence of forms whose value is the last one's -- a body, a `begin`,
  // a `let`'s tail. The last form stays in tail position, which is what
  // keeps a tail call one.
  NodeId emit_seq(const Ast& a, size_t from, FnCtx& ctx, SrcPos p) {
    auto b = Builder(m).at(p);
    if (from >= a.nodes.size()) return b.nil_literal();
    std::vector<NodeId> out;
    for (size_t i = from; i < a.nodes.size(); ++i) {
      out.push_back(emit_form(*a.nodes[i], ctx));
    }
    return b.block(out);
  }

  NodeId emit_form(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    if (a.tag == "quoted"_) return datum(*a.nodes[0], p);
    if (a.tag == "symbol"_) {
      const auto it = ref_of.find(&a);
      if (it != ref_of.end()) return read_var(it->second, ctx, p);
      const std::string s(a.token);
      const Builtin* bi = find_builtin(s);
      if (bi == nullptr) fail(a, "unbound variable: " + s);
      // A library procedure used as a value, which is how `(map car xs)`
      // reaches `car`. Every builtin is an ordinary func, so this needed
      // nothing special -- beyond a folding one being its variadic form.
      return rt.ref(value_rt(*bi), p);
    }
    if (a.tag != "list"_) return atom(a, p);
    if (a.nodes.empty()) fail(a, "empty application");

    const std::string h = head_of(a);
    if (h == "quote") return datum(*a.nodes[1], p);
    if (h == "define") return emit_define(a, ctx);
    if (h == "define-record-type") return emit_define_record_type(a, ctx);
    if (h == "import") return b.nil_literal();
    if (h == "case") return emit_case(a, ctx);
    if (h == "lambda") return emit_closure(fn_of.at(&a), ctx, p);
    if (h == "begin") return emit_seq(a, 1, ctx, p);
    if (h == "if") {
      return b.make_if(
          truthy(emit_form(*a.nodes[1], ctx), p),
          emit_form(*a.nodes[2], ctx),
          a.nodes.size() > 3 ? emit_form(*a.nodes[3], ctx) : b.nil_literal());
    }
    if (h == "when" || h == "unless") {
      const NodeId c = truthy(emit_form(*a.nodes[1], ctx), p);
      const NodeId body = emit_seq(a, 2, ctx, p);
      if (h == "when") return b.make_if(c, body, b.nil_literal());
      return b.make_if(c, b.nil_literal(), body);
    }
    if (h == "cond") return emit_cond(a, 1, ctx);
    if (h == "and" || h == "or") return emit_andor(a, h == "and", ctx);
    if (h == "set!") {
      const auto it = ref_of.find(a.nodes[1].get());
      if (it == ref_of.end()) fail(*a.nodes[1], "set!: unbound variable");
      return write_var(it->second, emit_form(*a.nodes[2], ctx), ctx, p);
    }
    if (h == "let" || h == "let*" || h == "letrec") return emit_let(a, ctx);
    if (h == "call/cc" || h == "call-with-current-continuation") {
      return emit_callcc(a, ctx);
    }

    // An application. A direct call to a library procedure names that
    // procedure's own singleton closure rather than building a fresh one.
    std::vector<NodeId> args;
    for (size_t i = 1; i < a.nodes.size(); ++i) {
      args.push_back(emit_form(*a.nodes[i], ctx));
    }
    if (!h.empty() && !ref_of.count(a.nodes[0].get())) {
      if (h == "list") {
        NodeId acc = b.nil_literal();
        for (size_t i = args.size(); i-- > 0;) {
          acc = b.array_lit({args[i], acc});
        }
        return acc;
      }
      if (h == "apply") {
        // (apply f a ... lst): the arguments between f and lst as they
        // are, then lst's elements -- a CallSpread, which in tail position
        // is a tail call, as R7RS requires of apply.
        if (args.size() < 2) fail(a, "apply needs a procedure and a list");
        const std::vector<NodeId> fixed(args.begin() + 1, args.end() - 1);
        return b.call_spread(args[0], fixed,
                             rt.call("$listarr", {args.back()}, p));
      }
      const Builtin* bi = find_builtin(h);
      if (bi != nullptr) return emit_builtin_call(*bi, h, args, ctx, p, a);
    }
    return b.call_value(emit_form(*a.nodes[0], ctx), args);
  }

  // A folding builtin's call is written out here, where the count is known,
  // as a chain of its two-argument helper (Fold has the rules, and
  // rt_fold_value is the same fold for the builtin as a value).
  NodeId emit_builtin_call(const Builtin& bi, const std::string& name,
                           const std::vector<NodeId>& args, FnCtx& ctx,
                           SrcPos p, const Ast& at) {
    auto b = Builder(m).at(p);
    if (bi.fold == Fold::Left) {
      if (args.empty()) {
        if (bi.none == Unit::None) {
          fail(at, name + " needs at least one argument");
        }
        return unit_literal(bi.none, b);
      }
      if (args.size() == 1) {
        if (bi.one_lhs == Unit::None) return args[0];
        return rt.call(bi.rt, {unit_literal(bi.one_lhs, b), args[0]}, p);
      }
      NodeId acc = args[0];
      for (size_t i = 1; i < args.size(); ++i) {
        acc = rt.call(bi.rt, {acc, args[i]}, p);
      }
      return acc;
    }
    if (bi.fold == Fold::Chain) {
      // The operands are a procedure call's arguments: each runs once, in
      // order, whether or not an earlier comparison has failed. Two need
      // nothing more; with three or more, every middle one is read by two
      // comparisons, so they all run into temporaries first and the
      // comparisons read those.
      if (args.empty()) return b.bool_literal(true);
      if (args.size() == 1) return b.block({args[0], b.bool_literal(true)});
      if (args.size() == 2) return rt.call(bi.rt, {args[0], args[1]}, p);
      std::vector<NodeId> out;
      std::vector<NodeId> vals;
      for (const NodeId v : args) {
        // Fresh: an operand after this one may hold a scope (call/cc's)
        // that claimed and gave back a slot, and runs after this is set.
        const int32_t t = ctx.alloc_fresh_local("$cmp");
        out.push_back(b.assign(VarKind::Local, t, v));
        vals.push_back(b.varref(VarKind::Local, t));
      }
      NodeId acc = rt.call(bi.rt, {vals[0], vals[1]}, p);
      for (size_t i = 2; i < vals.size(); ++i) {
        acc = b.make_if(acc, rt.call(bi.rt, {vals[i - 1], vals[i]}, p),
                        b.bool_literal(false));
      }
      out.push_back(acc);
      return b.block(out);
    }
    if (static_cast<int32_t>(args.size()) != bi.arity) {
      fail(at, name + " takes " + std::to_string(bi.arity) + " arguments");
    }
    return rt.call(bi.rt, args, p);
  }

  NodeId emit_define(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const Ast& t = *a.nodes[1];
    if (t.tag == "list"_) {
      return write_var(decl_of.at(t.nodes[0].get()),
                       emit_closure(fn_of.at(&a), ctx, p), ctx, p);
    }
    return write_var(
        decl_of.at(&t),
        a.nodes.size() > 2 ? emit_form(*a.nodes[2], ctx) : b.nil_literal(), ctx,
        p);
  }

  // `(case key ((d1 d2) body...) ... (else body...))` -- each clause's
  // data are literals, compared with `eqv?`, which is Scheme's own rule.
  NodeId emit_case(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const int32_t t = ctx.alloc_local("$case");
    const NodeId T = b.varref(VarKind::Local, t);
    return b.block({b.assign(VarKind::Local, t, emit_form(*a.nodes[1], ctx)),
                    emit_case_clauses(a, 2, T, ctx)});
  }

  NodeId emit_case_clauses(const Ast& a, size_t i, NodeId T, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    if (i >= a.nodes.size()) return b.nil_literal();
    const Ast& clause = *a.nodes[i];
    if (head_of(clause) == "else") return emit_seq(clause, 1, ctx, p);
    const Ast& data = *clause.nodes[0];
    NodeId cond = b.bool_literal(false);
    for (const auto& d : data.nodes) {
      cond = b.make_if(cond, b.bool_literal(true),
                       rt.call("$eqv", {T, datum(*d, p)}, p));
    }
    return b.make_if(cond, emit_seq(clause, 1, ctx, p),
                     emit_case_clauses(a, i + 1, T, ctx));
  }

  // `define-record-type`: no source body for any of the names it binds --
  // the constructor, predicate, and each accessor/mutator are built
  // directly, the way examples/mini-python's constructor and
  // examples/mini-ruby's attr_accessor methods are.
  NodeId emit_define_record_type(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const RecordInfo& ri = records.at(&a);
    std::vector<NodeId> out;

    const int32_t ctor_var = decl_of.at(a.nodes[2]->nodes[0].get());
    {
      Func f;
      f.name = ri.type_name + "-make";
      f.num_params = static_cast<int32_t>(ri.ctor_fields.size());
      f.num_locals = f.num_params;
      f.local_names = ri.ctor_fields;
      f.lenient_arity = true;
      // Every field, in the order the form declares them -- the order
      // display shows them in -- with #f for one the constructor does not
      // take, which is what guile's accessor and printer answer for it.
      std::vector<std::pair<NodeId, NodeId>> kvs{
          {b.str_literal(kRecKey), b.str_literal(ri.type_name)}};
      for (const RecordField& rf : ri.fields) {
        const auto at = std::find(ri.ctor_fields.begin(), ri.ctor_fields.end(),
                                  rf.name);
        kvs.emplace_back(
            b.str_literal(rf.name),
            at == ri.ctor_fields.end()
                ? b.bool_literal(false)
                : b.varref(VarKind::Local, static_cast<int32_t>(
                                               at - ri.ctor_fields.begin())));
      }
      f.body = b.scope(0, f.num_params, b.make_return(b.object_lit(kvs)));
      const int32_t idx = static_cast<int32_t>(m.funcs.size());
      m.funcs.push_back(std::move(f));
      out.push_back(
          write_var(ctor_var, b.make_closure(idx, rt.empty_cmap()), ctx, p));
    }

    const int32_t pred_var = decl_of.at(a.nodes[3].get());
    {
      Func f;
      f.name = ri.type_name + "?";
      f.num_params = 1;
      f.num_locals = 1;
      f.local_names = {"v"};
      f.lenient_arity = true;
      const NodeId V = b.varref(VarKind::Local, 0);
      const NodeId cond = b.make_if(
          b.binary(BinOp::Eq, b.intrinsic(IntrinsicId::TypeOf, {V}),
                   b.str_literal("object")),
          b.make_if(
              b.intrinsic(IntrinsicId::ObjectHas, {V, b.str_literal(kRecKey)}),
              b.binary(BinOp::Eq, b.index(V, b.str_literal(kRecKey)),
                       b.str_literal(ri.type_name)),
              b.bool_literal(false)),
          b.bool_literal(false));
      f.body = b.scope(0, 1, b.make_return(cond));
      const int32_t idx = static_cast<int32_t>(m.funcs.size());
      m.funcs.push_back(std::move(f));
      out.push_back(
          write_var(pred_var, b.make_closure(idx, rt.empty_cmap()), ctx, p));
    }

    for (const RecordField& rf : ri.fields) {
      Func f;
      f.name = ri.type_name + "-" + rf.name;
      f.num_params = 1;
      f.num_locals = 1;
      f.local_names = {"v"};
      f.lenient_arity = true;
      f.body = b.scope(0, 1,
                       b.make_return(b.index(b.varref(VarKind::Local, 0),
                                             b.str_literal(rf.name))));
      m.funcs[static_cast<size_t>(rs.fns[static_cast<size_t>(rf.accessor_fn)].index)] =
          std::move(f);
      out.push_back(write_var(
          rf.accessor_var,
          b.make_closure(rs.fns[static_cast<size_t>(rf.accessor_fn)].index,
                         rt.empty_cmap()),
          ctx, p));

      if (rf.mutator_fn < 0) continue;
      Func mf;
      mf.name = "set-" + ri.type_name + "-" + rf.name + "!";
      mf.num_params = 2;
      mf.num_locals = 2;
      mf.local_names = {"v", "x"};
      mf.lenient_arity = true;
      mf.body = b.scope(0, 2,
                        b.make_return(b.set_index(
                            b.varref(VarKind::Local, 0), b.str_literal(rf.name),
                            b.varref(VarKind::Local, 1))));
      m.funcs[static_cast<size_t>(rs.fns[static_cast<size_t>(rf.mutator_fn)].index)] =
          std::move(mf);
      out.push_back(write_var(
          rf.mutator_var,
          b.make_closure(rs.fns[static_cast<size_t>(rf.mutator_fn)].index,
                         rt.empty_cmap()),
          ctx, p));
    }
    return b.block(out);
  }

  NodeId emit_cond(const Ast& a, size_t i, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    if (i >= a.nodes.size()) return b.nil_literal();
    const Ast& clause = *a.nodes[i];
    if (head_of(clause) == "else") return emit_seq(clause, 1, ctx, p);
    if (clause.nodes.size() > 1) {
      const NodeId c = truthy(emit_form(*clause.nodes[0], ctx), p);
      return b.make_if(c, emit_seq(clause, 1, ctx, p),
                       emit_cond(a, i + 1, ctx));
    }
    // `(cond (test))` answers the test's own value, which is why a clause
    // with no body is not the same as one whose body is empty -- kept in a
    // temporary, the way `or` keeps its operand, so the test runs once.
    const int32_t t = ctx.alloc_local("$cond");
    const NodeId T = b.varref(VarKind::Local, t);
    const NodeId test = emit_form(*clause.nodes[0], ctx);
    return b.block({b.assign(VarKind::Local, t, test),
                    b.make_if(truthy(T, p), T, emit_cond(a, i + 1, ctx))});
  }

  // `and` and `or` answer one of their operands, not a boolean.
  NodeId emit_andor(const Ast& a, bool is_and, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    if (a.nodes.size() == 1) return b.bool_literal(is_and);
    NodeId acc = emit_form(*a.nodes[1], ctx);
    for (size_t i = 2; i < a.nodes.size(); ++i) {
      const int32_t t = ctx.alloc_local(is_and ? "$and" : "$or");
      const NodeId rhs = emit_form(*a.nodes[i], ctx);
      const NodeId keep = b.varref(VarKind::Local, t);
      acc = b.block({b.assign(VarKind::Local, t, acc),
                     b.make_if(truthy(keep, p), is_and ? rhs : keep,
                               is_and ? keep : rhs)});
    }
    return acc;
  }

  NodeId emit_let(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const bool named = a.nodes[1]->tag == "symbol"_;
    const size_t bind_at = named ? 2 : 1;
    const Ast& binds = *a.nodes[bind_at];

    if (named) {
      // Scheme's loop: a procedure bound to a name, called with the
      // initial values, that tail-calls itself to iterate. Nothing about
      // it is a loop construct -- which is exactly why tail calls are not
      // an optimization in this language.
      std::vector<NodeId> args;
      for (const auto& bp : binds.nodes) {
        args.push_back(bp->nodes.size() > 1 ? emit_form(*bp->nodes[1], ctx)
                                            : b.nil_literal());
      }
      const int32_t v = decl_of.at(a.nodes[1].get());
      return b.block(
          {write_var(v, emit_closure(fn_of.at(&a), ctx, p), ctx, p, true),
           b.call_value(read_var(v, ctx, p), args)});
    }

    std::vector<NodeId> out;
    // Every binding a closure captures gets a fresh cell on entry to the
    // `let`, so two closures made in two iterations of a loop around it do
    // not share one box. All of them are made *before* any initializer
    // runs, which is what `letrec` needs: the closure bound first captures
    // the cell of the one bound second, and a CellFresh in between would
    // swap that box out from under it. (This binder had exactly that bug,
    // and it showed up as two mutually recursive lambdas where the first
    // called nil.)
    const auto& cells = rs.fns[static_cast<size_t>(ctx.fn)].cell_index;
    for (const auto& bp : binds.nodes) {
      const auto c = cells.find(decl_of.at(bp->nodes[0].get()));
      if (c != cells.end()) out.push_back(b.cell_fresh(c->second));
    }
    for (const auto& bp : binds.nodes) {
      const int32_t v = decl_of.at(bp->nodes[0].get());
      out.push_back(write_var(v,
                              bp->nodes.size() > 1
                                  ? emit_form(*bp->nodes[1], ctx)
                                  : b.nil_literal(),
                              ctx, p, false));
    }
    out.push_back(emit_seq(a, bind_at + 1, ctx, p));
    return b.block(out);
  }

  // call/cc, the half of it that is expressible.
  //
  //   Scope {
  //     tag = a fresh object, whose *identity* names this activation
  //     TryCatch(exc,
  //        body: f(<escape closure over tag>),
  //        handler: exc is tagged with our tag ? its value : rethrow)
  //   }
  //
  // Invoking the escape throws; the throw unwinds to here and the handler
  // answers with the value. That is a continuation used the way almost
  // every real program uses one -- to leave early. What it is not is a
  // continuation that outlives its `call/cc`: see README.md.
  NodeId emit_callcc(const Ast& a, FnCtx& ctx) {
    const SrcPos p = pos_of(a);
    auto b = Builder(m).at(p);
    const int32_t mark = ctx.mark();
    const int32_t exc = ctx.alloc_local("$exc");
    const int32_t cell = ctx.next_cell++;
    const NodeId TAG = b.varref(VarKind::Cell, cell);
    const NodeId E = b.varref(VarKind::Local, exc);

    std::vector<CaptureSrc> cs{{VarKind::Cell, cell}};
    const int32_t cm = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back(cs);

    const NodeId body = b.call_value(emit_form(*a.nodes[1], ctx),
                                     {b.make_closure(rt.func("$escape"), cm)});
    const NodeId mine = b.make_if(
        b.binary(BinOp::Eq, b.intrinsic(IntrinsicId::TypeOf, {E}),
                 b.str_literal("object")),
        b.make_if(
            b.intrinsic(IntrinsicId::ObjectHas, {E, b.str_literal(kTagKey)}),
            b.intrinsic(IntrinsicId::Same,
                        {b.index(E, b.str_literal(kTagKey)), TAG}),
            b.bool_literal(false)),
        b.bool_literal(false));
    const NodeId handler =
        b.make_if(mine, b.index(E, b.str_literal(kValKey)), b.make_throw(E));
    const NodeId out = b.block({b.cell_fresh(cell),
                                b.assign(VarKind::Cell, cell, b.array_lit({})),
                                b.make_try(exc, body, handler)});
    const int32_t end = ctx.release(mark);
    return b.scope(mark, end, out);
  }

  // -- One function's body -------------------------------------------------
  void emit_fn(int32_t f) {
    const FnInfo& fi = fns[static_cast<size_t>(f)];
    const Resolver::Fn& rf = rs.fns[static_cast<size_t>(f)];
    // A record's accessor/mutator has no source body -- built directly, at
    // the point the `define-record-type` form itself was emitted.
    if (fi.is_synth) return;
    FnCtx ctx;
    ctx.fn = f;
    ctx.next_cell = rs.num_cells(f);
    const SrcPos p = fi.body != nullptr ? pos_of(*fi.body) : SrcPos{0, 0};
    auto b = Builder(m).at(p);

    std::vector<NodeId> pre;
    for (const auto& [v, c] : rf.cell_index) {
      (void)v;
      pre.push_back(b.cell_fresh(c));
    }
    // After the freshes above, which would otherwise reset it, and before
    // the prologue below, which may call a helper itself.
    for (size_t k = 0; k < fi.params.size(); ++k) {
      const int32_t v = fi.params[k];
      const int32_t s = ctx.alloc_local(rs.vars[static_cast<size_t>(v)].name);
      rs.vars[static_cast<size_t>(v)].slot = s;
      // A rest param arrives as the array Func::rest_param collects, and
      // Scheme's is a list -- made one before anything, a cell included,
      // can see it.
      if (fi.rest && k + 1 == fi.params.size()) {
        pre.push_back(
            b.assign(VarKind::Local, s,
                     rt.call("$arrlist", {b.varref(VarKind::Local, s)}, p)));
      }
      const auto it = rf.cell_index.find(v);
      if (it != rf.cell_index.end()) {
        pre.push_back(
            b.assign(VarKind::Cell, it->second, b.varref(VarKind::Local, s)));
      }
    }
    const int32_t nparams = ctx.mark();
    for (size_t v = 0; v < rs.vars.size(); ++v) {
      if (rs.vars[v].owner != f) continue;
      if (rs.vars[v].slot >= 0) continue;
      if (rf.cell_index.count(static_cast<int32_t>(v))) continue;
      rs.vars[v].slot = ctx.alloc_local(rs.vars[v].name);
    }

    const NodeId value = emit_seq(*fi.body, fi.body_from, ctx, p);

    std::vector<NodeId> stmts;
    stmts.insert(stmts.end(), pre.begin(), pre.end());
    if (f == 0) {
      // The boundary, made legible. An escape continuation invoked after
      // its `call/cc` has returned throws a token no handler is left to
      // catch -- which is the "multi-shot continuations" entry in the
      // top-level README's list of what stays out of reach. Without this
      // the program would die with "uncaught: <object>", which says
      // nothing; with it, it says what the program asked for and why it
      // cannot have it.
      const int32_t slot = ctx.alloc_local("$exc");
      const NodeId E = b.varref(VarKind::Local, slot);
      stmts.push_back(b.make_try(
          slot, value,
          b.make_if(b.make_if(b.binary(BinOp::Eq,
                                       b.intrinsic(IntrinsicId::TypeOf, {E}),
                                       b.str_literal("object")),
                              b.intrinsic(IntrinsicId::ObjectHas,
                                          {E, b.str_literal(kTagKey)}),
                              b.bool_literal(false)),
                    b.make_throw(b.str_literal(
                        "call/cc: a continuation was invoked after the "
                        "call/cc that made it had already returned. This "
                        "front end supports escape continuations only -- see "
                        "examples/mini-scheme/README.md")),
                    b.make_throw(E))));
      stmts.push_back(b.make_return(b.nil_literal()));
    } else {
      // The body's last form is the return value, and it stays in tail
      // position -- so a procedure that ends in a call replaces its frame
      // instead of stacking on it.
      stmts.push_back(b.make_return(value));
    }

    Func fn;
    fn.name = fi.name;
    fn.num_params = static_cast<int32_t>(fi.params.size());
    fn.rest_param = fi.rest;
    fn.num_locals = ctx.high_local;
    fn.local_names = ctx.names();
    fn.num_cells = ctx.next_cell;
    fn.lenient_arity = true;
    fn.tail_calls = true;
    fn.num_captures = m.funcs[static_cast<size_t>(rf.index)].num_captures;
    fn.capture_names = m.funcs[static_cast<size_t>(rf.index)].capture_names;
    fn.body = b.scope(0, nparams, b.block(stmts));
    m.funcs[static_cast<size_t>(rf.index)] = std::move(fn);
  }

  Module build(const Ast& program) {
    const int32_t top = new_fn(-1, "main");
    fns[static_cast<size_t>(top)].body = &program;
    fns[static_cast<size_t>(top)].body_from = 0;

    rs.push_scope();
    predeclare(program, 0, top);
    for (const auto& f : program.nodes) resolve_form(*f, top);
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

// The one thing this front end cannot write in its own IR: output.
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

}  // namespace mini_scheme
