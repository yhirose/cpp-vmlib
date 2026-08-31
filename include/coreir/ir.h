// coreir -- a closed intermediate representation.
//
// A front end lowers its own grammar into these ten node shapes; nothing in
// this header, or in any backend that consumes it, knows what parser produced
// them. That is the whole point: the tag set is closed, so a backend written
// once serves every front end that can reach it.
//
// Node is a fixed-size POD with no virtual functions, no RTTI, no
// std::function and no static initializer. Those are what survive
// --gc-sections as vtable/typeinfo/comdat residue in a binary that never uses
// them, so keeping them out is a size property, not a style preference.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace coreir {

// ---------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------

// Convention: every node produces a value. Statements produce Void. Block is
// the value of its last child, If the value of the branch taken, Call the
// callee's return value. PL/0 never observes this -- everything is Void -- but
// fixing it now keeps an expression-oriented front end from having to redefine
// Block/If/Call later, which would be a breaking change rather than a new tag.
enum class Tag : uint8_t {
  Literal,    // a = const pool index
  VarRef,     // op = VarKind, a = index
  Unary,      // op = UnOp,    children: operand
  Binary,     // op = BinOp,   children: lhs, rhs
  Assign,     // op = VarKind, a = index, children: value
  If,         // children: cond, then [, else]
  While,      // children: cond, body
  Block,      // children: stmts...  (zero children = the empty statement)
  Call,       // a = func index, b = capture map index, children: args...
  Intrinsic,  // op = IntrinsicId, children: args...
  // SPIKE: first-class functions. MakeClosure yields a callable value that
  // owns the cells named by its capture map, resolved in the frame that
  // builds it; CallValue calls whatever a value turns out to be. Together
  // they are what Call cannot express -- a function that outlives the frame
  // it was written in.
  MakeClosure,  // a = func index, b = capture map index
  CallValue,    // children: callee, args...
};

enum class UnOp : uint8_t { Neg };

enum class BinOp : uint8_t {
  Add, Sub, Mul, Div, Mod,
  Eq, Ne, Lt, Le, Gt, Ge,
};

enum class IntrinsicId : uint8_t { Print, ReadInt };

// A variable is either a slot in this frame or a slot borrowed from an
// enclosing one. There is deliberately no "level" -- static links assume the
// defining activation is still on the stack, which closures break. A capture
// list survives first-class functions, so VarRef's meaning does not have to
// change when they arrive.
// SPIKE: Cell joins the two. A local a closure captures cannot stay a slot in
// the frame -- the closure may outlive the frame -- so a front end promotes it
// to a Cell, a heap box the frame and every closure over it share. Which
// locals need promoting is the front end's analysis to do (culebra's
// FnAnalysis calls them captured_locals); the IR only needs the distinction to
// exist. A CaptureSrc therefore names a Cell or a Capture, never a Local.
enum class VarKind : uint8_t { Local, Capture, Cell };

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

inline constexpr uint32_t kNoNodeId = 0xFFFFFFFFu;

struct NodeId {
  uint32_t v = kNoNodeId;
  constexpr bool valid() const { return v != kNoNodeId; }
};

inline constexpr bool operator==(NodeId a, NodeId b) { return a.v == b.v; }
inline constexpr bool operator!=(NodeId a, NodeId b) { return a.v != b.v; }

struct SrcPos {
  uint32_t line = 0;
  uint32_t col = 0;
};

// SPIKE: Str joins Int. `bits` indexes Module::str_consts rather than holding
// the bytes, so Const stays a two-word POD and the pool stays in one place --
// the shape a Double or a Bool would slot into without changing either.
enum class ConstKind : uint8_t { Int, Str };

struct Const {
  ConstKind kind = ConstKind::Int;
  int64_t bits = 0;
};

// Where a callee's capture comes from, expressed in the *caller's* frame. A
// per-function list would not work: fib's captures live in the root frame, but
// fib's own recursive call runs with fib's frame, and finding "the defining
// frame" at run time is exactly the static link this design rejects. So the
// forwarding table belongs to the call site.
struct CaptureSrc {
  VarKind from = VarKind::Local;
  int32_t index = 0;
};

struct Func {
  std::string name;
  int32_t num_locals = 0;
  int32_t num_captures = 0;
  NodeId body;
  // Diagnostics only -- never read to execute anything. Kept the way culebra
  // keeps Chunk::positions: information the run does not need, held
  // structurally so error messages do not have to thread it by hand.
  std::vector<std::string> local_names;
  std::vector<std::string> capture_names;
  // SPIKE: cells are storage the frame shares with closures built inside it;
  // params are the first `num_params` locals, so an argument that needs
  // capturing is copied into a cell by the front end and the calling
  // convention itself stays about locals only. Both sit after the existing
  // members so that every brace initializer already written still means what
  // it did.
  int32_t num_cells = 0;
  int32_t num_params = 0;
};

struct Node {
  Tag tag = Tag::Block;
  uint8_t op = 0;        // UnOp / BinOp / IntrinsicId / VarKind, per tag
  uint32_t pos = 0;      // index into Module::positions
  int32_t a = 0;         // const index / var index / func index
  int32_t b = 0;         // capture map index
  uint32_t first_child = 0;
  uint32_t num_children = 0;
};

struct Module {
  std::vector<Node> nodes;
  std::vector<NodeId> child_ids;  // flat backing for every node's children
  std::vector<SrcPos> positions;
  std::vector<Const> consts;
  std::vector<std::string> str_consts;  // SPIKE: bytes for ConstKind::Str
  std::vector<Func> funcs;                      // funcs[0] is the entry point
  std::vector<std::vector<CaptureSrc>> capture_maps;

  const Node& at(NodeId id) const { return nodes[id.v]; }
  NodeId child(NodeId id, uint32_t i) const {
    return child_ids[nodes[id.v].first_child + i];
  }
  uint32_t num_children(NodeId id) const { return nodes[id.v].num_children; }
  SrcPos pos_of(NodeId id) const { return positions[nodes[id.v].pos]; }
  int64_t int_const(NodeId id) const { return consts[nodes[id.v].a].bits; }
};

// ---------------------------------------------------------------------------
// Arity
//
// -1 means variadic, or -- for If and Intrinsic -- "constrained, but not by a
// single number." The six fixed-arity tags have their shape stated exactly
// once, here. If's 2-or-3 and Intrinsic's per-id count are still centralized,
// just in Verifier::check_node and intrinsic_arity() respectively rather than
// in this table. Call's shape (capture map length against the callee) has no
// single-number arity at all and lives entirely in check_node.
// ---------------------------------------------------------------------------

inline constexpr int arity_of(Tag t) {
  switch (t) {
    case Tag::Literal:   return 0;
    case Tag::VarRef:    return 0;
    case Tag::Unary:     return 1;
    case Tag::Binary:    return 2;
    case Tag::Assign:    return 1;
    case Tag::If:        return -1;  // 2 or 3
    case Tag::While:     return 2;
    case Tag::Block:     return -1;
    case Tag::Call:      return -1;
    case Tag::Intrinsic: return -1;  // per IntrinsicId
    case Tag::MakeClosure: return 0;
    case Tag::CallValue:   return -1;  // callee, then args
  }
  return -1;
}

// How many slots of a given kind a function has. One place, because the
// verifier bounds-checks three different things against it (a VarRef, an
// Assign and a capture map entry) and a fourth kind would otherwise mean
// finding all three.
inline int32_t slot_limit(const Func& f, VarKind k) {
  switch (k) {
    case VarKind::Local:   return f.num_locals;
    case VarKind::Capture: return f.num_captures;
    case VarKind::Cell:    return f.num_cells;
  }
  return 0;
}

inline constexpr uint32_t intrinsic_arity(IntrinsicId id) {
  switch (id) {
    case IntrinsicId::Print:   return 1;
    case IntrinsicId::ReadInt: return 0;
  }
  return 0;
}

// Whether a tag yields a value usable as an operand. Under the "every node
// produces a value" convention every tag does in principle, but PL/0's
// statements are Void, and verify() uses this to catch a front end that wires
// a statement into an operand position.
inline constexpr bool yields_value(Tag t) {
  switch (t) {
    case Tag::Literal:
    case Tag::VarRef:
    case Tag::Unary:
    case Tag::Binary:
    case Tag::Intrinsic:
    // SPIKE: unlike Call, these produce a value. Call is left alone rather
    // than widened to match, so PL/0 keeps compiling to exactly the bytecode
    // it did; unifying the two call forms is Phase 1b's job.
    //
    // Block and If were always documented as producing one -- "Block is the
    // value of its last child, If the value of the branch taken" at the top
    // of this header -- and were only false here because PL/0 has no way to
    // observe it. A function whose body is a block that ends in an expression
    // does observe it.
    case Tag::Block:
    case Tag::If:
    case Tag::MakeClosure:
    case Tag::CallValue:
      return true;
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Views -- the only way to read a node's children
//
// Each view is paired with a Builder::make_* below. Nothing outside this
// header should index child_ids directly, so a tag's layout lives in exactly
// one place even though both the binder that builds a Module and the
// compiler that reads it need to agree on that layout.
//
// Views hold NodeId, never const Node* -- the arena grows and pointers into it
// do not survive that.
// ---------------------------------------------------------------------------

struct UnaryView  { UnOp op; NodeId operand; };
struct BinaryView { BinOp op; NodeId lhs, rhs; };
struct AssignView { VarKind kind; int32_t index; NodeId value; };
struct VarRefView { VarKind kind; int32_t index; };
struct IfView     { NodeId cond, then_, els; };  // els may be invalid
struct WhileView  { NodeId cond, body; };
struct CallView   { int32_t func; int32_t capture_map; };
struct IntrinsicView { IntrinsicId id; };

inline UnaryView view_unary(const Module& m, NodeId n) {
  return {static_cast<UnOp>(m.at(n).op), m.child(n, 0)};
}
inline BinaryView view_binary(const Module& m, NodeId n) {
  return {static_cast<BinOp>(m.at(n).op), m.child(n, 0), m.child(n, 1)};
}
inline AssignView view_assign(const Module& m, NodeId n) {
  return {static_cast<VarKind>(m.at(n).op), m.at(n).a, m.child(n, 0)};
}
inline VarRefView view_varref(const Module& m, NodeId n) {
  return {static_cast<VarKind>(m.at(n).op), m.at(n).a};
}
inline IfView view_if(const Module& m, NodeId n) {
  IfView v{m.child(n, 0), m.child(n, 1), NodeId{}};
  if (m.num_children(n) > 2) v.els = m.child(n, 2);
  return v;
}
inline WhileView view_while(const Module& m, NodeId n) {
  return {m.child(n, 0), m.child(n, 1)};
}
inline CallView view_call(const Module& m, NodeId n) {
  return {m.at(n).a, m.at(n).b};
}
inline IntrinsicView view_intrinsic(const Module& m, NodeId n) {
  return {static_cast<IntrinsicId>(m.at(n).op)};
}

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

class Builder {
public:
  explicit Builder(Module& m) : m_(m) {}

  // Linear scan: called from every node emit() builds, so this is O(n) over
  // already-seen positions per node -- fine for PL/0's few hundred nodes, but
  // an O(module size squared) cost if this IR is ever pointed at something
  // large. A hash map keyed on (line, col) is the fix, if that day comes.
  uint32_t intern_pos(SrcPos p) {
    for (uint32_t i = 0; i < m_.positions.size(); ++i) {
      if (m_.positions[i].line == p.line && m_.positions[i].col == p.col) {
        return i;
      }
    }
    m_.positions.push_back(p);
    return static_cast<uint32_t>(m_.positions.size() - 1);
  }

  // Same tradeoff as intern_pos, once per numeric literal rather than per
  // node.
  int32_t intern_int(int64_t v) {
    for (uint32_t i = 0; i < m_.consts.size(); ++i) {
      if (m_.consts[i].kind == ConstKind::Int && m_.consts[i].bits == v) {
        return static_cast<int32_t>(i);
      }
    }
    m_.consts.push_back({ConstKind::Int, v});
    return static_cast<int32_t>(m_.consts.size() - 1);
  }

  // SPIKE: same interning shape as intern_int, over the string pool.
  int32_t intern_str(const std::string& s) {
    for (uint32_t i = 0; i < m_.consts.size(); ++i) {
      if (m_.consts[i].kind == ConstKind::Str &&
          m_.str_consts[static_cast<size_t>(m_.consts[i].bits)] == s) {
        return static_cast<int32_t>(i);
      }
    }
    m_.str_consts.push_back(s);
    m_.consts.push_back(
        {ConstKind::Str, static_cast<int64_t>(m_.str_consts.size() - 1)});
    return static_cast<int32_t>(m_.consts.size() - 1);
  }

  NodeId literal(int64_t v, SrcPos p) {
    return emit(Tag::Literal, 0, p, intern_int(v), 0, {});
  }
  NodeId str_literal(const std::string& s, SrcPos p) {
    return emit(Tag::Literal, 0, p, intern_str(s), 0, {});
  }
  NodeId varref(VarKind k, int32_t index, SrcPos p) {
    return emit(Tag::VarRef, static_cast<uint8_t>(k), p, index, 0, {});
  }
  NodeId unary(UnOp op, NodeId operand, SrcPos p) {
    return emit(Tag::Unary, static_cast<uint8_t>(op), p, 0, 0, {operand});
  }
  NodeId binary(BinOp op, NodeId lhs, NodeId rhs, SrcPos p) {
    return emit(Tag::Binary, static_cast<uint8_t>(op), p, 0, 0, {lhs, rhs});
  }
  NodeId assign(VarKind k, int32_t index, NodeId value, SrcPos p) {
    return emit(Tag::Assign, static_cast<uint8_t>(k), p, index, 0, {value});
  }
  NodeId make_if(NodeId cond, NodeId then_, NodeId els, SrcPos p) {
    if (els.valid()) return emit(Tag::If, 0, p, 0, 0, {cond, then_, els});
    return emit(Tag::If, 0, p, 0, 0, {cond, then_});
  }
  NodeId make_while(NodeId cond, NodeId body, SrcPos p) {
    return emit(Tag::While, 0, p, 0, 0, {cond, body});
  }
  NodeId block(const std::vector<NodeId>& stmts, SrcPos p) {
    return emit(Tag::Block, 0, p, 0, 0, stmts);
  }
  NodeId call(int32_t func, int32_t capture_map, SrcPos p) {
    return emit(Tag::Call, 0, p, func, capture_map, {});
  }
  NodeId intrinsic(IntrinsicId id, const std::vector<NodeId>& args, SrcPos p) {
    return emit(Tag::Intrinsic, static_cast<uint8_t>(id), p, 0, 0, args);
  }
  NodeId make_closure(int32_t func, int32_t capture_map, SrcPos p) {
    return emit(Tag::MakeClosure, 0, p, func, capture_map, {});
  }
  NodeId call_value(NodeId callee, const std::vector<NodeId>& args, SrcPos p) {
    std::vector<NodeId> children{callee};
    children.insert(children.end(), args.begin(), args.end());
    return emit(Tag::CallValue, 0, p, 0, 0, children);
  }

private:
  NodeId emit(Tag tag, uint8_t op, SrcPos p, int32_t a, int32_t b,
              const std::vector<NodeId>& children) {
    Node n;
    n.tag = tag;
    n.op = op;
    n.pos = intern_pos(p);
    n.a = a;
    n.b = b;
    n.first_child = static_cast<uint32_t>(m_.child_ids.size());
    n.num_children = static_cast<uint32_t>(children.size());
    for (NodeId c : children) m_.child_ids.push_back(c);
    m_.nodes.push_back(n);
    return NodeId{static_cast<uint32_t>(m_.nodes.size() - 1)};
  }

  Module& m_;
};

// ---------------------------------------------------------------------------
// verify -- what makes "closed" mean something
// ---------------------------------------------------------------------------

std::optional<std::string> verify(const Module& m);

// A readable dump of the IR, for --dump-ir.
std::string to_string(const Module& m);

// Public so vm's own instruction-name table can share it instead of
// restating the same eleven strings.
const char* name_of(BinOp op);

}  // namespace coreir
