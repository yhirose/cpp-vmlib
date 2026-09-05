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
#include "vmlib.h"

using namespace peg;
using namespace peg::udl;
using namespace coreir;

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

// The library, as (name -> the IR func that implements it, arity). Every
// one is an ordinary function, so a symbol naming it is a *value* --
// `(map car xs)` needs `car` to be one, and nothing special had to happen
// for it to be.
struct Builtin {
  const char* scheme;
  const char* rt;
  int32_t arity;
};

const std::vector<Builtin>& builtins() {
  static const std::vector<Builtin> v = {
      {"car", "$car", 1},           {"cdr", "$cdr", 1},
      {"cons", "$cons", 2},         {"null?", "$nullp", 1},
      {"pair?", "$pairp", 1},       {"not", "$not", 1},
      {"eq?", "$eqv", 2},           {"eqv?", "$eqv", 2},
      {"equal?", "$equal", 2},      {"+", "$add", 2},
      {"-", "$sub", 2},             {"*", "$mul", 2},
      {"/", "$div", 2},             {"<", "$lt", 2},
      {">", "$gt", 2},              {"<=", "$le", 2},
      {">=", "$ge", 2},             {"=", "$numeq", 2},
      {"length", "$length", 1},     {"append", "$append2", 2},
      {"reverse", "$reverse", 1},   {"list-ref", "$listref", 2},
      {"memq", "$memq", 2},         {"member", "$memq", 2},
      {"assoc", "$assoc", 2},       {"number?", "$numberp", 1},
      {"string?", "$stringp", 1},   {"procedure?", "$procp", 1},
      {"boolean?", "$booleanp", 1}, {"zero?", "$zerop", 1},
      {"abs", "$abs", 1},           {"min", "$min", 2},
      {"max", "$max", 2},           {"quotient", "$quotient", 2},
      {"remainder", "$remainder", 2}, {"modulo", "$modulo", 2},
      {"expt", "$expt", 2},         {"number->string", "$numstr", 1},
      {"string-append", "$strappend", 2},
      {"string-length", "$strlen", 1},
      {"map", "$map1", 2},          {"for-each", "$foreach1", 2},
      {"display", "$display", 1},   {"newline", "$newline", 0},
      {"list", "$listof1", 1},
  };
  return v;
}

const Builtin* find_builtin(const std::string& s) {
  for (const auto& b : builtins()) {
    if (s == b.scheme) return &b;
  }
  return nullptr;
}

// The forms whose arguments fold pairwise: `(+ a b c)` is `(+ (+ a b) c)`,
// and `(< a b c)` is a conjunction. Scheme's are variadic and this IR's
// calls are not, so the fold happens where the arity is known -- at the
// call site.
bool folds_left(const std::string& s) {
  return s == "+" || s == "-" || s == "*" || s == "/" || s == "append" ||
         s == "string-append" || s == "min" || s == "max";
}

bool folds_chain(const std::string& s) {
  return s == "<" || s == ">" || s == "<=" || s == ">=" || s == "=";
}

struct VarInfo {
  std::string name;
  int32_t owner = 0;
};

struct FnInfo {
  int32_t parent = -1;
  int32_t index = -1;
  bool is_synth = false;
  std::string name = "lambda";
  std::set<int32_t> free;
  std::map<int32_t, int32_t> capture_index;
  std::map<int32_t, int32_t> cell_index;
  std::vector<int32_t> params;
  const Ast* body = nullptr;  // the enclosing `list`, whose tail is the body
  size_t body_from = 0;
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
  std::vector<int32_t> slot_of;
  std::map<std::string, int32_t> rt;
  int32_t empty_cmap = -1;

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

  int32_t resolve_lambda(const Ast& node, const Ast* params, const Ast& body,
                         size_t from, int32_t parent,
                         const std::string& name) {
    const int32_t f = new_fn(parent, name);
    fns[static_cast<size_t>(f)].body = &body;
    fns[static_cast<size_t>(f)].body_from = from;
    fn_of[&node] = f;
    scopes.push_back({f, {}});
    if (params != nullptr) {
      for (const auto& p : params->nodes) {
        const int32_t v = declare(std::string(p->token), f);
        decl_of[p.get()] = v;
        fns[static_cast<size_t>(f)].params.push_back(v);
      }
    }
    predeclare(body, from, f);
    for (size_t i = from; i < body.nodes.size(); ++i) {
      resolve_form(*body.nodes[i], f);
    }
    scopes.pop_back();
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
      if (auto v = resolve(s, fn)) {
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
        resolve_define_proc(a, t, fn);
        return;
      }
      if (!decl_of.count(&t)) decl_of[&t] = declare(std::string(t.token), fn);
      for (size_t i = 2; i < a.nodes.size(); ++i) resolve_form(*a.nodes[i], fn);
      return;
    }
    if (h == "lambda") {
      if (a.nodes.size() < 2) fail(a, "malformed lambda");
      resolve_lambda(a, a.nodes[1].get(), a, 2, fn, "lambda");
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

  // (define (f a b) body...) -- the parameters are the tail of `(f a b)`
  // rather than a list of their own, so this walks them directly instead
  // of going through resolve_lambda.
  void resolve_define_proc(const Ast& node, const Ast& sig, int32_t fn) {
    const int32_t f = new_fn(fn, std::string(sig.nodes[0]->token));
    fns[static_cast<size_t>(f)].body = &node;
    fns[static_cast<size_t>(f)].body_from = 2;
    fn_of[&node] = f;
    scopes.push_back({f, {}});
    for (size_t i = 1; i < sig.nodes.size(); ++i) {
      const Ast& p = *sig.nodes[i];
      const int32_t v = declare(std::string(p.token), f);
      decl_of[&p] = v;
      fns[static_cast<size_t>(f)].params.push_back(v);
    }
    predeclare(node, 2, f);
    for (size_t i = 2; i < node.nodes.size(); ++i) {
      resolve_form(*node.nodes[i], f);
    }
    scopes.pop_back();
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
      scopes.push_back({fn, {}});
      const Ast& id = *a.nodes[1];
      decl_of[&id] = declare(std::string(id.token), fn);
      const int32_t f = new_fn(fn, std::string(id.token));
      fns[static_cast<size_t>(f)].body = &a;
      fns[static_cast<size_t>(f)].body_from = bind_at + 1;
      fn_of[&a] = f;
      scopes.push_back({f, {}});
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
      scopes.pop_back();
      scopes.pop_back();
      return;
    }

    if (kind == "letrec") {
      scopes.push_back({fn, {}});
      for (const auto& bpair : binds.nodes) {
        const Ast& p = *bpair->nodes[0];
        decl_of[&p] = declare(std::string(p.token), fn);
      }
      for (const auto& bpair : binds.nodes) {
        if (bpair->nodes.size() > 1) resolve_form(*bpair->nodes[1], fn);
      }
    } else if (kind == "let*") {
      scopes.push_back({fn, {}});
      for (const auto& bpair : binds.nodes) {
        if (bpair->nodes.size() > 1) resolve_form(*bpair->nodes[1], fn);
        const Ast& p = *bpair->nodes[0];
        decl_of[&p] = declare(std::string(p.token), fn);
      }
    } else {
      for (const auto& bpair : binds.nodes) {
        if (bpair->nodes.size() > 1) resolve_form(*bpair->nodes[1], fn);
      }
      scopes.push_back({fn, {}});
      for (const auto& bpair : binds.nodes) {
        const Ast& p = *bpair->nodes[0];
        decl_of[&p] = declare(std::string(p.token), fn);
      }
    }
    predeclare(a, bind_at + 1, fn);
    for (size_t i = bind_at + 1; i < a.nodes.size(); ++i) {
      resolve_form(*a.nodes[i], fn);
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

  // ==== The library, written in this front end's own IR ====================
  //
  // A pair is a two-element Array: `car` is [0] and `cdr` is [1], and the
  // empty list is nil. Nothing else in this subset produces an Array, so
  // TypeOf tells a pair from everything else with no tag of its own.

  static const std::vector<std::string>& rt_names() {
    static std::vector<std::string> names;
    if (names.empty()) {
      for (const auto& b : builtins()) {
        if (std::find(names.begin(), names.end(), b.rt) == names.end()) {
          names.push_back(b.rt);
        }
      }
      for (const char* extra :
           {"$true", "$disp", "$listbody", "$fstr", "$escape"}) {
        names.push_back(extra);
      }
    }
    return names;
  }

  struct RT {
    Binder& bd;
    Builder b;
    SrcPos p{0, 0};
    std::vector<NodeId> body;

    explicit RT(Binder& bd_) : bd(bd_), b(bd_.m) {}

    NodeId L(int32_t i) { return b.varref(VarKind::Local, i, p); }
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
    NodeId ret(NodeId v) { return b.make_return(v, p); }
    NodeId blk(const std::vector<NodeId>& v) { return b.block(v, p); }
    NodeId iff(NodeId c, NodeId t) { return b.make_if(c, t, NodeId{}, p); }
    NodeId iff(NodeId c, NodeId t, NodeId e) { return b.make_if(c, t, e, p); }
    NodeId wh(NodeId c, NodeId x) { return b.make_while(c, x, p); }
    NodeId idx(NodeId r, NodeId k) { return b.index(r, k, p); }
    NodeId typ(NodeId v) { return in(IntrinsicId::TypeOf, {v}); }
    NodeId len(NodeId v) { return in(IntrinsicId::Len, {v}); }
    NodeId is(NodeId v, const std::string& s) { return bin(BinOp::Eq, v, S(s)); }
    NodeId both(NodeId x, NodeId y) { return b.make_if(x, y, Bo(false), p); }
    NodeId either(NodeId x, NodeId y) { return b.make_if(x, Bo(true), y, p); }
    NodeId pairp(NodeId v) { return is(typ(v), "array"); }
    NodeId car(NodeId v) { return idx(v, I(0)); }
    NodeId cdr(NodeId v) { return idx(v, I(1)); }
    NodeId call(const std::string& name, const std::vector<NodeId>& a) {
      return b.call_value(
          b.make_closure(bd.rt.at(name), bd.empty_cmap, p), a, p);
    }
    NodeId nat(const std::string& name, const std::vector<NodeId>& a) {
      return b.call_value(b.native_ref(b.declare_native(name), p), a, p);
    }
    void add(NodeId n) { body.push_back(n); }
    NodeId err(const std::string& msg) { return b.make_throw(S(msg), p); }

    void finish(const std::string& name, int32_t nparams, int32_t nlocals,
                std::vector<std::string> names, int32_t ncaps = 0) {
      Func& f = bd.m.funcs[static_cast<size_t>(bd.rt.at(name))];
      f.name = name;
      f.num_params = nparams;
      f.num_locals = nlocals;
      names.resize(static_cast<size_t>(nlocals), "");
      f.local_names = std::move(names);
      f.num_captures = ncaps;
      for (int32_t i = 0; i < ncaps; ++i) f.capture_names.push_back("k");
      f.lenient_arity = true;
      f.tail_calls = true;
      f.body = b.scope(0, nlocals, blk(body), p);
    }
  };

  void one(const std::string& name, int32_t nparams,
           const std::function<NodeId(RT&)>& body,
           const std::vector<std::string>& names) {
    RT r(*this);
    r.add(r.ret(body(r)));
    r.finish(name, nparams, static_cast<int32_t>(names.size()), names);
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
    one("$listof1", 1, [&](RT& r) { return r.arr({r.L(0), r.Nil()}); }, {"v"});

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
    one("$abs", 1, [&](RT& r) {
      return r.iff(r.bin(BinOp::Lt, r.L(0), r.I(0)),
                   r.b.unary(UnOp::Neg, r.L(0), r.p), r.L(0));
    }, {"v"});
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
    rt_memq();
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
  }

  void rt_modulo() {
    RT r(*this);
    r.add(r.set(2, r.bin(BinOp::Mod, r.L(0), r.L(1))));
    r.add(r.iff(r.both(r.bin(BinOp::Ne, r.L(2), r.I(0)),
                       r.bin(BinOp::Lt, r.bin(BinOp::Mul, r.L(2), r.L(1)),
                             r.I(0))),
                r.set(2, r.bin(BinOp::Add, r.L(2), r.L(1)))));
    r.add(r.ret(r.L(2)));
    r.finish("$modulo", 2, 3, {"a", "b", "m"});
  }

  void rt_expt() {
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
    r.finish("$expt", 2, 4, {"a", "b", "acc", "i"});
  }

  // eq? on two immediates is their value; on two heap values it is
  // identity -- except that a symbol is a string here (see README.md), so
  // strings compare by value and `(eq? 'a 'a)` answers what Scheme does.
  void rt_eqv() {
    RT r(*this);
    r.add(r.set(2, r.typ(r.L(0))));
    r.add(r.iff(r.bin(BinOp::Ne, r.L(2), r.typ(r.L(1))), r.ret(r.Bo(false))));
    r.add(r.iff(r.is(r.L(2), "nil"), r.ret(r.Bo(true))));
    r.add(r.iff(r.either(r.is(r.L(2), "bool"),
                         r.either(r.is(r.L(2), "int"),
                                  r.either(r.is(r.L(2), "double"),
                                           r.is(r.L(2), "string")))),
                r.ret(r.bin(BinOp::Eq, r.L(0), r.L(1)))));
    r.add(r.ret(r.in(IntrinsicId::Same, {r.L(0), r.L(1)})));
    r.finish("$eqv", 2, 3, {"a", "b", "t"});
  }

  void rt_equal() {
    RT r(*this);
    r.add(r.iff(r.both(r.pairp(r.L(0)), r.pairp(r.L(1))),
                r.ret(r.both(r.call("$equal", {r.car(r.L(0)), r.car(r.L(1))}),
                             r.call("$equal",
                                    {r.cdr(r.L(0)), r.cdr(r.L(1))})))));
    r.add(r.ret(r.call("$eqv", {r.L(0), r.L(1)})));
    r.finish("$equal", 2, 2, {"a", "b"});
  }

  void rt_length() {
    RT r(*this);
    r.add(r.set(1, r.I(0)));
    r.add(r.set(2, r.L(0)));
    r.add(r.wh(r.pairp(r.L(2)),
               r.blk({r.set(1, r.bin(BinOp::Add, r.L(1), r.I(1))),
                      r.set(2, r.cdr(r.L(2)))})));
    r.add(r.ret(r.L(1)));
    r.finish("$length", 1, 3, {"l", "n", "p"});
  }

  void rt_append2() {
    RT r(*this);
    r.add(r.iff(r.bin(BinOp::Eq, r.pairp(r.L(0)), r.Bo(false)),
                r.ret(r.L(1))));
    r.add(r.ret(r.arr({r.car(r.L(0)),
                       r.call("$append2", {r.cdr(r.L(0)), r.L(1)})})));
    r.finish("$append2", 2, 2, {"a", "b"});
  }

  void rt_reverse() {
    RT r(*this);
    r.add(r.set(1, r.Nil()));
    r.add(r.set(2, r.L(0)));
    r.add(r.wh(r.pairp(r.L(2)),
               r.blk({r.set(1, r.arr({r.car(r.L(2)), r.L(1)})),
                      r.set(2, r.cdr(r.L(2)))})));
    r.add(r.ret(r.L(1)));
    r.finish("$reverse", 1, 3, {"l", "out", "p"});
  }

  void rt_listref() {
    RT r(*this);
    r.add(r.set(2, r.L(0)));
    r.add(r.set(3, r.L(1)));
    r.add(r.wh(r.both(r.bin(BinOp::Gt, r.L(3), r.I(0)), r.pairp(r.L(2))),
               r.blk({r.set(2, r.cdr(r.L(2))),
                      r.set(3, r.bin(BinOp::Sub, r.L(3), r.I(1)))})));
    r.add(r.iff(r.bin(BinOp::Eq, r.pairp(r.L(2)), r.Bo(false)),
                r.err("list-ref: index out of range")));
    r.add(r.ret(r.car(r.L(2))));
    r.finish("$listref", 2, 4, {"l", "k", "p", "i"});
  }

  void rt_memq() {
    RT r(*this);
    r.add(r.set(2, r.L(1)));
    r.add(r.wh(r.pairp(r.L(2)),
               r.blk({r.iff(r.call("$equal", {r.L(0), r.car(r.L(2))}),
                            r.ret(r.L(2))),
                      r.set(2, r.cdr(r.L(2)))})));
    r.add(r.ret(r.Bo(false)));
    r.finish("$memq", 2, 3, {"x", "l", "p"});
  }

  void rt_assoc() {
    RT r(*this);
    r.add(r.set(2, r.L(1)));
    r.add(r.wh(r.pairp(r.L(2)),
               r.blk({r.iff(r.both(r.pairp(r.car(r.L(2))),
                                   r.call("$equal",
                                          {r.L(0), r.car(r.car(r.L(2)))})),
                            r.ret(r.car(r.L(2)))),
                      r.set(2, r.cdr(r.L(2)))})));
    r.add(r.ret(r.Bo(false)));
    r.finish("$assoc", 2, 3, {"x", "l", "p"});
  }

  void rt_map1() {
    RT r(*this);
    r.add(r.iff(r.bin(BinOp::Eq, r.pairp(r.L(1)), r.Bo(false)),
                r.ret(r.Nil())));
    r.add(r.ret(r.arr({r.b.call_value(r.L(0), {r.car(r.L(1))}, r.p),
                       r.call("$map1", {r.L(0), r.cdr(r.L(1))})})));
    r.finish("$map1", 2, 2, {"f", "l"});
  }

  void rt_foreach1() {
    RT r(*this);
    r.add(r.set(2, r.L(1)));
    r.add(r.wh(r.pairp(r.L(2)),
               r.blk({r.b.call_value(r.L(0), {r.car(r.L(2))}, r.p),
                      r.set(2, r.cdr(r.L(2)))})));
    r.add(r.ret(r.Nil()));
    r.finish("$foreach1", 2, 3, {"f", "l", "p"});
  }

  void rt_numstr() {
    RT r(*this);
    r.add(r.iff(r.is(r.typ(r.L(0)), "double"),
                r.ret(r.call("$fstr", {r.L(0)}))));
    r.add(r.ret(r.in(IntrinsicId::ToStr, {r.L(0)})));
    r.finish("$numstr", 1, 1, {"n"});
  }

  // Guile prints an inexact number as to_display does, with a ".0" forced
  // onto a whole one -- to_display's own comment says a front end that
  // cares builds that string, and this one cares.
  void rt_fstr() {
    const double lim = 9007199254740992.0;
    RT r(*this);
    r.add(r.iff(r.bin(BinOp::Ne, r.L(0), r.L(0)), r.ret(r.S("+nan.0"))));
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

  void rt_disp() {
    RT r(*this);
    r.add(r.set(1, r.typ(r.L(0))));
    r.add(r.iff(r.is(r.L(1), "nil"), r.ret(r.S("()"))));
    r.add(r.iff(r.is(r.L(1), "bool"),
                r.ret(r.iff(r.L(0), r.S("#t"), r.S("#f")))));
    r.add(r.iff(r.is(r.L(1), "string"), r.ret(r.L(0))));
    r.add(r.iff(r.is(r.L(1), "double"), r.ret(r.call("$fstr", {r.L(0)}))));
    r.add(r.iff(r.is(r.L(1), "int"), r.ret(r.in(IntrinsicId::ToStr, {r.L(0)}))));
    r.add(r.iff(r.is(r.L(1), "array"),
                r.ret(r.bin(BinOp::Add,
                            r.bin(BinOp::Add, r.S("("),
                                  r.call("$listbody", {r.L(0)})),
                            r.S(")")))));
    r.add(r.ret(r.S("#<procedure>")));
    r.finish("$disp", 1, 2, {"v", "t"});
  }

  // The improper tail is what makes this its own function: `(1 . 2)` and
  // `(1 2)` differ only in what the last cdr turns out to be.
  void rt_listbody() {
    RT r(*this);
    r.add(r.set(1, r.call("$disp", {r.car(r.L(0))})));
    r.add(r.set(2, r.cdr(r.L(0))));
    r.add(r.wh(r.pairp(r.L(2)),
               r.blk({r.set(1, r.bin(BinOp::Add, r.L(1),
                                     r.bin(BinOp::Add, r.S(" "),
                                           r.call("$disp",
                                                  {r.car(r.L(2))})))),
                      r.set(2, r.cdr(r.L(2)))})));
    r.add(r.iff(r.bin(BinOp::Ne, r.typ(r.L(2)), r.S("nil")),
                r.set(1, r.bin(BinOp::Add, r.L(1),
                               r.bin(BinOp::Add, r.S(" . "),
                                     r.call("$disp", {r.L(2)}))))));
    r.add(r.ret(r.L(1)));
    r.finish("$listbody", 1, 3, {"p", "out", "rest"});
  }

  // An escape continuation, as a procedure: invoking it throws a value
  // tagged with the identity of the `call/cc` that made it, and that
  // `call/cc`'s TryCatch is what catches it. One capture, the tag.
  void rt_escape() {
    RT r(*this);
    r.add(r.b.make_throw(
        r.b.object_lit({{r.S(kTagKey), r.P(0)}, {r.S(kValKey), r.L(0)}}, r.p),
        r.p));
    r.finish("$escape", 1, 1, {"v"}, 1);
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

  NodeId write_var(int32_t v, NodeId value, FnCtx& ctx, SrcPos p,
                   bool fresh = false) {
    Builder b(m);
    const auto [k, i] = access(ctx.fn, v);
    if (fresh && k == VarKind::Cell) {
      return b.block({b.cell_fresh(i, p), b.assign(k, i, value, p)}, p);
    }
    return b.assign(k, i, value, p);
  }

  NodeId truthy(NodeId v, FnCtx& ctx, SrcPos p) {
    return helper(ctx, "$true", {v}, p);
  }

  // A quoted datum, built at bind time: a list becomes a chain of pairs, a
  // symbol becomes a string (see README.md for what that costs), and every
  // other atom is itself.
  NodeId datum(const Ast& a, SrcPos p) {
    Builder b(m);
    if (a.tag == "list"_) {
      NodeId acc = b.nil_literal(p);
      for (size_t i = a.nodes.size(); i-- > 0;) {
        acc = b.array_lit({datum(*a.nodes[i], p), acc}, p);
      }
      return acc;
    }
    if (a.tag == "quoted"_) return datum(*a.nodes[0], p);
    return atom(a, p);
  }

  NodeId atom(const Ast& a, SrcPos p) {
    Builder b(m);
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
      case "boolean"_:
        return b.bool_literal(a.token == "#t", p);
      default:
        return b.str_literal(std::string(a.token), p);  // a symbol
    }
  }

  // A sequence of forms whose value is the last one's -- a body, a `begin`,
  // a `let`'s tail. The last form stays in tail position, which is what
  // keeps a tail call one.
  NodeId emit_seq(const Ast& a, size_t from, FnCtx& ctx, SrcPos p) {
    Builder b(m);
    if (from >= a.nodes.size()) return b.nil_literal(p);
    std::vector<NodeId> out;
    for (size_t i = from; i < a.nodes.size(); ++i) {
      out.push_back(emit_form(*a.nodes[i], ctx));
    }
    return b.block(out, p);
  }

  NodeId emit_form(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    if (a.tag == "quoted"_) return datum(*a.nodes[0], p);
    if (a.tag == "symbol"_) {
      const auto it = ref_of.find(&a);
      if (it != ref_of.end()) return read_var(it->second, ctx, p);
      const std::string s(a.token);
      const Builtin* bi = find_builtin(s);
      if (bi == nullptr) fail(a, "unbound variable: " + s);
      // A library procedure used as a value, which is how `(map car xs)`
      // reaches `car`. Every builtin is an ordinary func, so this needed
      // nothing special.
      return b.make_closure(rt.at(bi->rt), empty_cmap, p);
    }
    if (a.tag != "list"_) return atom(a, p);
    if (a.nodes.empty()) fail(a, "empty application");

    const std::string h = head_of(a);
    if (h == "quote") return datum(*a.nodes[1], p);
    if (h == "define") return emit_define(a, ctx);
    if (h == "define-record-type") return emit_define_record_type(a, ctx);
    if (h == "import") return b.nil_literal(p);
    if (h == "case") return emit_case(a, ctx);
    if (h == "lambda") return emit_closure(fn_of.at(&a), ctx, p);
    if (h == "begin") return emit_seq(a, 1, ctx, p);
    if (h == "if") {
      return b.make_if(
          truthy(emit_form(*a.nodes[1], ctx), ctx, p),
          emit_form(*a.nodes[2], ctx),
          a.nodes.size() > 3 ? emit_form(*a.nodes[3], ctx) : b.nil_literal(p),
          p);
    }
    if (h == "when" || h == "unless") {
      const NodeId c = truthy(emit_form(*a.nodes[1], ctx), ctx, p);
      const NodeId body = emit_seq(a, 2, ctx, p);
      if (h == "when") return b.make_if(c, body, b.nil_literal(p), p);
      return b.make_if(c, b.nil_literal(p), body, p);
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

    // An application. A direct call to a library procedure goes through
    // the Static calls recipe's cell rather than a fresh MakeClosure.
    std::vector<NodeId> args;
    for (size_t i = 1; i < a.nodes.size(); ++i) {
      args.push_back(emit_form(*a.nodes[i], ctx));
    }
    if (!h.empty() && !ref_of.count(a.nodes[0].get())) {
      if (h == "list") {
        NodeId acc = b.nil_literal(p);
        for (size_t i = args.size(); i-- > 0;) {
          acc = b.array_lit({args[i], acc}, p);
        }
        return acc;
      }
      const Builtin* bi = find_builtin(h);
      if (bi != nullptr) return emit_builtin_call(*bi, h, args, ctx, p, a);
    }
    return b.call_value(emit_form(*a.nodes[0], ctx), args, p);
  }

  // Scheme's arithmetic and comparison are variadic and this IR's calls are
  // not, so the fold happens here, where the arity is known.
  NodeId emit_builtin_call(const Builtin& bi, const std::string& name,
                           const std::vector<NodeId>& args, FnCtx& ctx,
                           SrcPos p, const Ast& at) {
    Builder b(m);
    if (folds_left(name)) {
      if (args.empty()) {
        if (name == "+") return b.literal(0, p);
        if (name == "*") return b.literal(1, p);
        if (name == "string-append") return b.str_literal("", p);
        fail(at, name + " needs at least one argument");
      }
      if (args.size() == 1) {
        // `(- x)` is negation and `(/ x)` is a reciprocal; the rest are
        // the identity on one argument.
        if (name == "-") {
          return helper(ctx, "$sub", {b.literal(0, p), args[0]}, p);
        }
        if (name == "/") {
          return helper(ctx, "$div", {b.literal(1, p), args[0]}, p);
        }
        return args[0];
      }
      NodeId acc = args[0];
      for (size_t i = 1; i < args.size(); ++i) {
        acc = helper(ctx, bi.rt, {acc, args[i]}, p);
      }
      return acc;
    }
    if (folds_chain(name)) {
      if (args.size() < 2) return b.bool_literal(true, p);
      NodeId acc = helper(ctx, bi.rt, {args[0], args[1]}, p);
      for (size_t i = 2; i < args.size(); ++i) {
        acc = b.make_if(acc, helper(ctx, bi.rt, {args[i - 1], args[i]}, p),
                        b.bool_literal(false, p), p);
      }
      return acc;
    }
    if (static_cast<int32_t>(args.size()) != bi.arity) {
      fail(at, name + " takes " + std::to_string(bi.arity) + " arguments");
    }
    return helper(ctx, bi.rt, args, p);
  }

  NodeId emit_define(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    const Ast& t = *a.nodes[1];
    if (t.tag == "list"_) {
      return write_var(decl_of.at(t.nodes[0].get()),
                       emit_closure(fn_of.at(&a), ctx, p), ctx, p);
    }
    return write_var(decl_of.at(&t),
                     a.nodes.size() > 2 ? emit_form(*a.nodes[2], ctx)
                                        : b.nil_literal(p),
                     ctx, p);
  }

  // `(case key ((d1 d2) body...) ... (else body...))` -- each clause's
  // data are literals, compared with `eqv?`, which is Scheme's own rule.
  NodeId emit_case(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    const int32_t t = ctx.alloc_local("$case");
    const NodeId T = b.varref(VarKind::Local, t, p);
    return b.block(
        {b.assign(VarKind::Local, t, emit_form(*a.nodes[1], ctx), p),
         emit_case_clauses(a, 2, T, ctx)},
        p);
  }

  NodeId emit_case_clauses(const Ast& a, size_t i, NodeId T, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    if (i >= a.nodes.size()) return b.nil_literal(p);
    const Ast& clause = *a.nodes[i];
    if (head_of(clause) == "else") return emit_seq(clause, 1, ctx, p);
    const Ast& data = *clause.nodes[0];
    NodeId cond = b.bool_literal(false, p);
    for (const auto& d : data.nodes) {
      cond = b.make_if(cond, b.bool_literal(true, p),
                       helper(ctx, "$eqv", {T, datum(*d, p)}, p), p);
    }
    return b.make_if(cond, emit_seq(clause, 1, ctx, p),
                     emit_case_clauses(a, i + 1, T, ctx), p);
  }

  // `define-record-type`: no source body for any of the names it binds --
  // the constructor, predicate, and each accessor/mutator are built
  // directly, the way examples/mini-python's constructor and
  // examples/mini-ruby's attr_accessor methods are.
  NodeId emit_define_record_type(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
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
      std::vector<std::pair<NodeId, NodeId>> kvs{
          {b.str_literal(kRecKey, p), b.str_literal(ri.type_name, p)}};
      for (size_t k = 0; k < ri.ctor_fields.size(); ++k) {
        kvs.emplace_back(b.str_literal(ri.ctor_fields[k], p),
                         b.varref(VarKind::Local, static_cast<int32_t>(k), p));
      }
      f.body = b.scope(0, f.num_params, b.make_return(b.object_lit(kvs, p), p),
                       p);
      const int32_t idx = static_cast<int32_t>(m.funcs.size());
      m.funcs.push_back(std::move(f));
      out.push_back(write_var(ctor_var, b.make_closure(idx, empty_cmap, p),
                              ctx, p));
    }

    const int32_t pred_var = decl_of.at(a.nodes[3].get());
    {
      Func f;
      f.name = ri.type_name + "?";
      f.num_params = 1;
      f.num_locals = 1;
      f.local_names = {"v"};
      f.lenient_arity = true;
      const NodeId V = b.varref(VarKind::Local, 0, p);
      const NodeId cond = b.make_if(
          b.binary(BinOp::Eq, b.intrinsic(IntrinsicId::TypeOf, {V}, p),
                   b.str_literal("object", p), p),
          b.make_if(b.intrinsic(IntrinsicId::ObjectHas,
                                {V, b.str_literal(kRecKey, p)}, p),
                    b.binary(BinOp::Eq, b.index(V, b.str_literal(kRecKey, p), p),
                             b.str_literal(ri.type_name, p), p),
                    b.bool_literal(false, p), p),
          b.bool_literal(false, p), p);
      f.body = b.scope(0, 1, b.make_return(cond, p), p);
      const int32_t idx = static_cast<int32_t>(m.funcs.size());
      m.funcs.push_back(std::move(f));
      out.push_back(write_var(pred_var, b.make_closure(idx, empty_cmap, p),
                              ctx, p));
    }

    for (const RecordField& rf : ri.fields) {
      Func f;
      f.name = ri.type_name + "-" + rf.name;
      f.num_params = 1;
      f.num_locals = 1;
      f.local_names = {"v"};
      f.lenient_arity = true;
      f.body = b.scope(
          0, 1,
          b.make_return(
              b.index(b.varref(VarKind::Local, 0, p), b.str_literal(rf.name, p),
                     p),
              p),
          p);
      m.funcs[static_cast<size_t>(fns[static_cast<size_t>(rf.accessor_fn)].index)] =
          std::move(f);
      out.push_back(write_var(
          rf.accessor_var,
          b.make_closure(fns[static_cast<size_t>(rf.accessor_fn)].index,
                         empty_cmap, p),
          ctx, p));

      if (rf.mutator_fn < 0) continue;
      Func mf;
      mf.name = "set-" + ri.type_name + "-" + rf.name + "!";
      mf.num_params = 2;
      mf.num_locals = 2;
      mf.local_names = {"v", "x"};
      mf.lenient_arity = true;
      mf.body = b.scope(
          0, 2,
          b.make_return(
              b.set_index(b.varref(VarKind::Local, 0, p),
                         b.str_literal(rf.name, p),
                         b.varref(VarKind::Local, 1, p), p),
              p),
          p);
      m.funcs[static_cast<size_t>(fns[static_cast<size_t>(rf.mutator_fn)].index)] =
          std::move(mf);
      out.push_back(write_var(
          rf.mutator_var,
          b.make_closure(fns[static_cast<size_t>(rf.mutator_fn)].index,
                         empty_cmap, p),
          ctx, p));
    }
    return b.block(out, p);
  }

  NodeId emit_cond(const Ast& a, size_t i, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    if (i >= a.nodes.size()) return b.nil_literal(p);
    const Ast& clause = *a.nodes[i];
    if (head_of(clause) == "else") return emit_seq(clause, 1, ctx, p);
    const NodeId c = truthy(emit_form(*clause.nodes[0], ctx), ctx, p);
    // `(cond (test))` answers the test's own value, which is why a clause
    // with no body is not the same as one whose body is empty.
    const NodeId body = clause.nodes.size() > 1
                            ? emit_seq(clause, 1, ctx, p)
                            : emit_form(*clause.nodes[0], ctx);
    return b.make_if(c, body, emit_cond(a, i + 1, ctx), p);
  }

  // `and` and `or` answer one of their operands, not a boolean.
  NodeId emit_andor(const Ast& a, bool is_and, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
    if (a.nodes.size() == 1) return b.bool_literal(is_and, p);
    NodeId acc = emit_form(*a.nodes[1], ctx);
    for (size_t i = 2; i < a.nodes.size(); ++i) {
      const int32_t t = ctx.alloc_local(is_and ? "$and" : "$or");
      const NodeId rhs = emit_form(*a.nodes[i], ctx);
      const NodeId keep = b.varref(VarKind::Local, t, p);
      acc = b.block({b.assign(VarKind::Local, t, acc, p),
                     b.make_if(truthy(keep, ctx, p), is_and ? rhs : keep,
                               is_and ? keep : rhs, p)},
                    p);
    }
    return acc;
  }

  NodeId emit_let(const Ast& a, FnCtx& ctx) {
    Builder b(m);
    const SrcPos p = pos_of(a);
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
                                            : b.nil_literal(p));
      }
      const int32_t v = decl_of.at(a.nodes[1].get());
      return b.block(
          {write_var(v, emit_closure(fn_of.at(&a), ctx, p), ctx, p, true),
           b.call_value(read_var(v, ctx, p), args, p)},
          p);
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
    const auto& cells = fns[static_cast<size_t>(ctx.fn)].cell_index;
    for (const auto& bp : binds.nodes) {
      const auto c = cells.find(decl_of.at(bp->nodes[0].get()));
      if (c != cells.end()) out.push_back(b.cell_fresh(c->second, p));
    }
    for (const auto& bp : binds.nodes) {
      const int32_t v = decl_of.at(bp->nodes[0].get());
      out.push_back(write_var(v,
                              bp->nodes.size() > 1
                                  ? emit_form(*bp->nodes[1], ctx)
                                  : b.nil_literal(p),
                              ctx, p, false));
    }
    out.push_back(emit_seq(a, bind_at + 1, ctx, p));
    return b.block(out, p);
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
    Builder b(m);
    const SrcPos p = pos_of(a);
    const int32_t mark = ctx.next_local;
    const int32_t exc = ctx.alloc_local("$exc");
    const int32_t cell = ctx.next_cell++;
    const NodeId TAG = b.varref(VarKind::Cell, cell, p);
    const NodeId E = b.varref(VarKind::Local, exc, p);

    std::vector<CaptureSrc> cs{{VarKind::Cell, cell}};
    const int32_t cm = static_cast<int32_t>(m.capture_maps.size());
    m.capture_maps.push_back(cs);

    const NodeId body = b.call_value(
        emit_form(*a.nodes[1], ctx),
        {b.make_closure(rt.at("$escape"), cm, p)}, p);
    const NodeId mine = b.make_if(
        b.binary(BinOp::Eq, b.intrinsic(IntrinsicId::TypeOf, {E}, p),
                 b.str_literal("object", p), p),
        b.make_if(b.intrinsic(IntrinsicId::ObjectHas,
                              {E, b.str_literal(kTagKey, p)}, p),
                  b.intrinsic(IntrinsicId::Same,
                              {b.index(E, b.str_literal(kTagKey, p), p), TAG},
                              p),
                  b.bool_literal(false, p), p),
        b.bool_literal(false, p), p);
    const NodeId handler =
        b.make_if(mine, b.index(E, b.str_literal(kValKey, p), p),
                  b.make_throw(E, p), p);
    const NodeId out = b.block(
        {b.cell_fresh(cell, p),
         b.assign(VarKind::Cell, cell, b.array_lit({}, p), p),
         b.make_try(exc, body, handler, p)},
        p);
    const int32_t end = ctx.next_local;
    ctx.next_local = mark;
    return b.scope(mark, end, out, p);
  }

  // -- One function's body -------------------------------------------------
  void emit_fn(int32_t f) {
    const FnInfo& fi = fns[static_cast<size_t>(f)];
    // A record's accessor/mutator has no source body -- built directly, at
    // the point the `define-record-type` form itself was emitted.
    if (fi.is_synth) return;
    FnCtx ctx;
    ctx.fn = f;
    ctx.next_cell = static_cast<int32_t>(fi.cell_index.size());
    Builder b(m);
    const SrcPos p = fi.body != nullptr ? pos_of(*fi.body) : SrcPos{0, 0};

    std::vector<NodeId> pre;
    for (const auto& [v, c] : fi.cell_index) {
      (void)v;
      pre.push_back(b.cell_fresh(c, p));
    }
    for (const int32_t v : fi.params) {
      const int32_t s = ctx.alloc_local(vars[static_cast<size_t>(v)].name);
      slot_of[static_cast<size_t>(v)] = s;
      const auto it = fi.cell_index.find(v);
      if (it != fi.cell_index.end()) {
        pre.push_back(b.assign(VarKind::Cell, it->second,
                               b.varref(VarKind::Local, s, p), p));
      }
    }
    const int32_t nparams = ctx.next_local;
    for (size_t v = 0; v < vars.size(); ++v) {
      if (vars[v].owner != f) continue;
      if (slot_of[v] >= 0) continue;
      if (fi.cell_index.count(static_cast<int32_t>(v))) continue;
      slot_of[v] = ctx.alloc_local(vars[v].name);
    }

    const NodeId value = emit_seq(*fi.body, fi.body_from, ctx, p);

    std::vector<NodeId> stmts;
    for (const auto& [name, cell] : ctx.helper_cells) {
      stmts.push_back(b.assign(VarKind::Cell, cell,
                               b.make_closure(rt.at(name), empty_cmap, p), p));
    }
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
      const NodeId E = b.varref(VarKind::Local, slot, p);
      stmts.push_back(b.make_try(
          slot, value,
          b.make_if(
              b.make_if(b.binary(BinOp::Eq,
                                 b.intrinsic(IntrinsicId::TypeOf, {E}, p),
                                 b.str_literal("object", p), p),
                        b.intrinsic(IntrinsicId::ObjectHas,
                                    {E, b.str_literal(kTagKey, p)}, p),
                        b.bool_literal(false, p), p),
              b.make_throw(
                  b.str_literal(
                      "call/cc: a continuation was invoked after the "
                      "call/cc that made it had already returned. This "
                      "front end supports escape continuations only -- see "
                      "examples/mini-scheme/README.md",
                      p),
                  p),
              b.make_throw(E, p), p),
          p));
      stmts.push_back(b.make_return(b.nil_literal(p), p));
    } else {
      // The body's last form is the return value, and it stays in tail
      // position -- so a procedure that ends in a call replaces its frame
      // instead of stacking on it.
      stmts.push_back(b.make_return(value, p));
    }

    Func fn;
    fn.name = fi.name;
    fn.num_params = static_cast<int32_t>(fi.params.size());
    fn.num_locals = ctx.high_local;
    ctx.local_names.resize(static_cast<size_t>(ctx.high_local), "");
    fn.local_names = ctx.local_names;
    fn.num_cells = ctx.next_cell;
    fn.lenient_arity = true;
    fn.tail_calls = true;
    fn.num_captures = m.funcs[static_cast<size_t>(fi.index)].num_captures;
    fn.capture_names = m.funcs[static_cast<size_t>(fi.index)].capture_names;
    fn.body = b.scope(0, nparams, b.block(stmts, p), p);
    m.funcs[static_cast<size_t>(fi.index)] = std::move(fn);
  }

  Module build(const Ast& program) {
    const int32_t top = new_fn(-1, "main");
    fns[static_cast<size_t>(top)].body = &program;
    fns[static_cast<size_t>(top)].body_from = 0;

    scopes.push_back({top, {}});
    predeclare(program, 0, top);
    for (const auto& f : program.nodes) resolve_form(*f, top);
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
