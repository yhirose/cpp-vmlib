#include "binder.h"

#include <cerrno>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <peglib.h>

#include "grammar.h"
#include "coreir/rt.h"

using namespace peg;
using namespace peg::udl;
using namespace coreir;

namespace pl0 {
namespace {

SrcPos pos_of(const Ast& a) {
  return {static_cast<uint32_t>(a.line), static_cast<uint32_t>(a.column)};
}

[[noreturn]] void fail(const Ast& a, const std::string& msg) {
  coreir_rt::fail(msg, static_cast<uint32_t>(a.line),
              static_cast<uint32_t>(a.column));
}

// A variable, named globally: which block owns it and which of that block's
// slots it is. Free-variable sets are sets of these, and their sorted order is
// what fixes each function's capture indices.
using VarId = std::pair<int32_t, int32_t>;

struct BlockInfo {
  int32_t parent = -1;
  std::map<std::string, int64_t> consts;
  std::map<std::string, int32_t> var_slot;
  std::vector<std::string> var_names;
  std::map<std::string, int32_t> procs;
  std::vector<int32_t> children;  // nested blocks, in declaration order
  const Ast* body = nullptr;
};

struct VarLookup {
  bool found = false;
  bool is_const = false;
  int64_t cval = 0;
  VarId var{-1, -1};
};

struct Binder {
  Module m;
  std::vector<BlockInfo> blocks;
  std::vector<std::set<VarId>> free;
  std::vector<std::vector<int32_t>> calls;
  std::vector<std::map<VarId, int32_t>> capture_index;

  // -- Phase A: declarations ------------------------------------------------
  //
  // Names are collected before any body is walked, and every sibling
  // procedure is registered before the first of them is descended into. That
  // is what makes a forward call to a later sibling work; pl0.cc registers and
  // descends one at a time, so its forward references fail -- a side effect of
  // its traversal order rather than a decision about the language.

  bool declared_anywhere(int32_t f, const std::string& name) const {
    for (int32_t b = f; b >= 0; b = blocks[b].parent) {
      const BlockInfo& bi = blocks[b];
      if (bi.consts.count(name) || bi.var_slot.count(name) ||
          bi.procs.count(name)) {
        return true;
      }
    }
    return false;
  }

  void check_fresh(int32_t f, const std::string& name, const Ast& at) {
    // Shadowing is rejected, not permitted: pl0.cul's Scope.has walks outward
    // before declaring, and pl0.cc's has_symbol does the same.
    if (declared_anywhere(f, name)) {
      fail(at, "'" + name + "' is already defined");
    }
  }

  static int64_t parse_number(const Ast& a) {
    errno = 0;
    char* end = nullptr;
    const std::string s(a.token);
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end != s.c_str() + s.size()) {
      fail(a, "integer literal out of range");
    }
    return static_cast<int64_t>(v);
  }

  int32_t collect(const Ast& block, int32_t parent, const std::string& name) {
    const int32_t f = static_cast<int32_t>(blocks.size());
    blocks.push_back({});
    blocks[f].parent = parent;
    m.funcs.push_back({});
    m.funcs[f].name = name;

    const Ast& cst = *block.nodes[0];
    const Ast& var = *block.nodes[1];
    const Ast& prc = *block.nodes[2];
    blocks[f].body = block.nodes[3].get();

    for (size_t i = 0; i + 1 < cst.nodes.size(); i += 2) {
      const Ast& id = *cst.nodes[i];
      const std::string n(id.token);
      check_fresh(f, n, id);
      blocks[f].consts[n] = parse_number(*cst.nodes[i + 1]);
    }

    for (const auto& node : var.nodes) {
      const std::string n(node->token);
      check_fresh(f, n, *node);
      const int32_t slot = static_cast<int32_t>(blocks[f].var_names.size());
      blocks[f].var_slot[n] = slot;
      blocks[f].var_names.push_back(n);
    }

    for (size_t i = 0; i + 1 < prc.nodes.size(); i += 2) {
      const Ast& id = *prc.nodes[i];
      const std::string n(id.token);
      check_fresh(f, n, id);
      blocks[f].procs[n] = -1;  // reserved; the index arrives below
    }
    for (size_t i = 0; i + 1 < prc.nodes.size(); i += 2) {
      const std::string n(prc.nodes[i]->token);
      const int32_t sub = collect(*prc.nodes[i + 1], f, n);
      blocks[f].procs[n] = sub;
      blocks[f].children.push_back(sub);
    }

    m.funcs[f].num_locals = static_cast<int32_t>(blocks[f].var_names.size());
    m.funcs[f].local_names = blocks[f].var_names;
    return f;
  }

  // -- Name resolution ------------------------------------------------------
  //
  // Three lookups, not one, because pl0.cul uses three: reading a name sees
  // constants and variables, assigning sees the same but rejects constants,
  // and CALL sees only procedures. A procedure name in an expression is
  // therefore "undefined variable", which is what pl0.cul reports.

  VarLookup lookup_value(int32_t f, const std::string& name) const {
    for (int32_t b = f; b >= 0; b = blocks[b].parent) {
      auto ci = blocks[b].consts.find(name);
      if (ci != blocks[b].consts.end()) return {true, true, ci->second, {}};
      auto vi = blocks[b].var_slot.find(name);
      if (vi != blocks[b].var_slot.end()) {
        return {true, false, 0, VarId{b, vi->second}};
      }
    }
    return {};
  }

  VarId lookup_assign(int32_t f, const std::string& name, const Ast& at) const {
    for (int32_t b = f; b >= 0; b = blocks[b].parent) {
      if (blocks[b].consts.count(name)) {
        fail(at, "cannot assign to constant '" + name + "'");
      }
      auto vi = blocks[b].var_slot.find(name);
      if (vi != blocks[b].var_slot.end()) return VarId{b, vi->second};
    }
    fail(at, "undefined variable '" + name + "'");
  }

  int32_t lookup_proc(int32_t f, const std::string& name) const {
    for (int32_t b = f; b >= 0; b = blocks[b].parent) {
      auto pi = blocks[b].procs.find(name);
      if (pi != blocks[b].procs.end()) return pi->second;
    }
    return -1;
  }

  // -- Phase B: free variables ----------------------------------------------

  void note_ref(int32_t f, VarId v) {
    if (v.first != f) free[f].insert(v);
  }

  void scan_expr(const Ast& a, int32_t f) {
    switch (a.tag) {
      case "number"_:
        parse_number(a);  // range-check now, at bind time
        return;
      case "ident"_: {
        const std::string n(a.token);
        const VarLookup r = lookup_value(f, n);
        if (!r.found) fail(a, "undefined variable '" + n + "'");
        if (!r.is_const) note_ref(f, r.var);
        return;
      }
      case "expression"_:
      case "term"_:
        for (const auto& node : a.nodes) {
          if (node->tag == "sign"_ || node->tag == "term_op"_ ||
              node->tag == "factor_op"_) {
            continue;
          }
          scan_expr(*node, f);
        }
        return;
      default:
        fail(a, "cannot evaluate " + a.name);
    }
  }

  void scan_cond(const Ast& a, int32_t f) {
    switch (a.tag) {
      case "odd"_:
        scan_expr(*a.nodes[0], f);
        return;
      case "compare"_:
        scan_expr(*a.nodes[0], f);
        scan_expr(*a.nodes[2], f);
        return;
      default:
        fail(a, "cannot evaluate " + a.name + " as a condition");
    }
  }

  void scan_stmt(const Ast& a, int32_t f) {
    switch (a.tag) {
      case "statements"_:
        for (const auto& node : a.nodes) scan_stmt(*node, f);
        return;
      case "assignment"_:
        note_ref(f, lookup_assign(f, std::string(a.nodes[0]->token),
                                  *a.nodes[0]));
        scan_expr(*a.nodes[1], f);
        return;
      case "call"_: {
        const Ast& nm = *a.nodes[0];
        const std::string n(nm.token);
        const int32_t g = lookup_proc(f, n);
        if (g < 0) fail(nm, "undefined procedure '" + n + "'");
        calls[f].push_back(g);
        return;
      }
      case "if"_:
      case "while"_:
        scan_cond(*a.nodes[0], f);
        scan_stmt(*a.nodes[1], f);
        return;
      case "out"_:
        scan_expr(*a.nodes[0], f);
        return;
      case "in"_:
        note_ref(f, lookup_assign(f, std::string(a.nodes[0]->token),
                                  *a.nodes[0]));
        return;
      case "statement"_:
        return;  // the empty statement
      default:
        fail(a, "cannot execute " + a.name);
    }
  }

  // Nested procedures are scanned before the block's own body, so diagnostics
  // come out in source order (PL/0 writes PROCEDURE declarations ahead of the
  // statement they precede).
  void scan_block(int32_t f) {
    for (int32_t sub : blocks[f].children) scan_block(sub);
    scan_stmt(*blocks[f].body, f);
  }

  void solve_captures() {
    // A call from F to G obliges F to hand G every capture G needs, so
    // anything G captures that F does not own must itself be captured by F.
    // Recursion and mutual recursion make the call graph cyclic, hence the
    // fixpoint rather than a single post-order sweep -- pl0.cc's one-shot
    // propagation is exactly what misses the self-recursive case.
    bool changed = true;
    while (changed) {
      changed = false;
      for (size_t f = 0; f < free.size(); ++f) {
        for (int32_t g : calls[f]) {
          for (const VarId& v : free[g]) {
            if (v.first != static_cast<int32_t>(f) &&
                free[f].insert(v).second) {
              changed = true;
            }
          }
        }
      }
    }

    for (size_t f = 0; f < free.size(); ++f) {
      int32_t i = 0;
      for (const VarId& v : free[f]) {  // std::set: a stable, sorted order
        capture_index[f][v] = i++;
        m.funcs[f].capture_names.push_back(
            blocks[v.first].var_names[v.second]);
      }
      m.funcs[f].num_captures = i;
    }
  }

  // -- Phase C: emit --------------------------------------------------------

  std::pair<VarKind, int32_t> access(int32_t f, VarId v) const {
    if (v.first == f) return {VarKind::Local, v.second};
    return {VarKind::Capture, capture_index[f].at(v)};
  }

  int32_t make_capture_map(int32_t f, int32_t g) {
    std::vector<CaptureSrc> cs;
    cs.reserve(free[g].size());
    for (const VarId& v : free[g]) {
      const auto [kind, index] = access(f, v);
      cs.push_back({kind, index});
    }
    m.capture_maps.push_back(std::move(cs));
    return static_cast<int32_t>(m.capture_maps.size() - 1);
  }

  static BinOp binop_of(std::string_view t) {
    if (t == "+") return BinOp::Add;
    if (t == "-") return BinOp::Sub;
    if (t == "*") return BinOp::Mul;
    return BinOp::Div;
  }

  static BinOp cmp_of(std::string_view t) {
    if (t == "=") return BinOp::Eq;
    if (t == "#") return BinOp::Ne;
    if (t == "<") return BinOp::Lt;
    if (t == "<=") return BinOp::Le;
    if (t == ">") return BinOp::Gt;
    return BinOp::Ge;
  }

  NodeId emit_expr(const Ast& a, int32_t f) {
    Builder bd(m);
    switch (a.tag) {
      case "number"_:
        return bd.literal(parse_number(a), pos_of(a));
      case "ident"_: {
        const VarLookup r = lookup_value(f, std::string(a.token));
        if (r.is_const) return bd.literal(r.cval, pos_of(a));
        const auto [kind, index] = access(f, r.var);
        return bd.varref(kind, index, pos_of(a));
      }
      case "expression"_:
      case "term"_:
        return emit_chain(a, f);
      default:
        fail(a, "cannot evaluate " + a.name);
    }
  }

  // `expression <- sign? term (term_op term)*` and `term <- factor (factor_op
  // factor)*` are the same shape: an optional leading sign, a first operand,
  // then operator/operand pairs, left-associated. The sign binds to the first
  // term only, so -a*b+c is ((-(a*b)) + c).
  NodeId emit_chain(const Ast& a, int32_t f) {
    const auto& ns = a.nodes;
    size_t i = 0;
    NodeId acc;
    if (ns[0]->tag == "sign"_) {
      acc = emit_expr(*ns[1], f);
      if (ns[0]->token == "-") {
        Builder bd(m);
        acc = bd.unary(UnOp::Neg, acc, pos_of(*ns[0]));
      }
      i = 2;
    } else {
      acc = emit_expr(*ns[0], f);
      i = 1;
    }
    for (; i + 1 < ns.size(); i += 2) {
      const Ast& op = *ns[i];
      const NodeId rhs = emit_expr(*ns[i + 1], f);
      Builder bd(m);
      acc = bd.binary(binop_of(op.token), acc, rhs, pos_of(op));
    }
    return acc;
  }

  NodeId emit_cond(const Ast& a, int32_t f) {
    switch (a.tag) {
      case "odd"_: {
        // ODD e is e mod 2 != 0. pl0.cc's two lanes both read it as e != 0,
        // and agree with each other while disagreeing with the language --
        // which is why three-lane cross-checking alone is not enough.
        const NodeId e = emit_expr(*a.nodes[0], f);
        Builder bd(m);
        const SrcPos p = pos_of(a);
        const NodeId two = bd.literal(2, p);
        const NodeId rem = bd.binary(BinOp::Mod, e, two, p);
        const NodeId zero = bd.literal(0, p);
        return bd.binary(BinOp::Ne, rem, zero, p);
      }
      case "compare"_: {
        const NodeId l = emit_expr(*a.nodes[0], f);
        const NodeId r = emit_expr(*a.nodes[2], f);
        Builder bd(m);
        return bd.binary(cmp_of(a.nodes[1]->token), l, r,
                         pos_of(*a.nodes[1]));
      }
      default:
        fail(a, "cannot evaluate " + a.name + " as a condition");
    }
  }

  NodeId emit_stmt(const Ast& a, int32_t f) {
    switch (a.tag) {
      case "statements"_: {
        std::vector<NodeId> kids;
        kids.reserve(a.nodes.size());
        for (const auto& node : a.nodes) kids.push_back(emit_stmt(*node, f));
        Builder bd(m);
        return bd.block(kids, pos_of(a));
      }
      case "assignment"_: {
        const Ast& target = *a.nodes[0];
        const VarId v =
            lookup_assign(f, std::string(target.token), target);
        const NodeId value = emit_expr(*a.nodes[1], f);
        const auto [kind, index] = access(f, v);
        Builder bd(m);
        return bd.assign(kind, index, value, pos_of(a));
      }
      case "call"_: {
        const Ast& nm = *a.nodes[0];
        const int32_t g = lookup_proc(f, std::string(nm.token));
        const int32_t cmap = make_capture_map(f, g);
        Builder bd(m);
        return bd.call(g, cmap, pos_of(a));
      }
      case "if"_: {
        const NodeId c = emit_cond(*a.nodes[0], f);
        const NodeId t = emit_stmt(*a.nodes[1], f);
        Builder bd(m);
        return bd.make_if(c, t, NodeId{}, pos_of(a));
      }
      case "while"_: {
        const NodeId c = emit_cond(*a.nodes[0], f);
        const NodeId body = emit_stmt(*a.nodes[1], f);
        Builder bd(m);
        return bd.make_while(c, body, pos_of(a));
      }
      case "out"_: {
        const NodeId v = emit_expr(*a.nodes[0], f);
        Builder bd(m);
        return bd.intrinsic(IntrinsicId::Print, {v}, pos_of(a));
      }
      case "in"_: {
        const Ast& target = *a.nodes[0];
        const VarId v = lookup_assign(f, std::string(target.token), target);
        Builder bd(m);
        const NodeId read = bd.intrinsic(IntrinsicId::ReadInt, {}, pos_of(a));
        const auto [kind, index] = access(f, v);
        return bd.assign(kind, index, read, pos_of(a));
      }
      case "statement"_: {
        Builder bd(m);
        return bd.block({}, pos_of(a));
      }
      default:
        fail(a, "cannot execute " + a.name);
    }
  }

  Module build(const Ast& root) {
    collect(root, -1, "main");
    free.resize(blocks.size());
    calls.resize(blocks.size());
    capture_index.resize(blocks.size());
    scan_block(0);
    solve_captures();
    for (size_t f = 0; f < blocks.size(); ++f) {
      m.funcs[f].body = emit_stmt(*blocks[f].body, static_cast<int32_t>(f));
    }
    return std::move(m);
  }
};

}  // namespace

Module bind_source(const std::string& source) {
  parser p;
  p.set_logger([](size_t line, size_t col, const std::string& msg,
                  const std::string&) {
    coreir_rt::fail(msg, static_cast<uint32_t>(line), static_cast<uint32_t>(col));
  });
  if (!p.load_grammar(kGrammar)) coreir_rt::fail("invalid grammar", 0, 0);
  p.enable_ast();

  std::shared_ptr<Ast> ast;
  if (!p.parse(source, ast)) coreir_rt::fail("syntax error", 0, 0);
  // The grammar's no_ast_opt annotations are written for this; without it the
  // tree has a different shape than the binder below expects.
  ast = p.optimize_ast(ast);

  Binder b;
  Module m = b.build(*ast);

  if (auto err = verify(m)) {
    coreir_rt::fail("internal error: malformed IR: " + *err, 0, 0);
  }
  return m;
}

}  // namespace pl0
