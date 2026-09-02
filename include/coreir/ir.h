// coreir -- a closed intermediate representation.
//
// A front end lowers its own grammar into these fifteen node shapes; nothing in
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
#include <cstring>
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
  Intrinsic,  // op = IntrinsicId, children: args...
  // Calling, in two halves. MakeClosure yields a callable value owning the
  // cells its capture map names, resolved in the frame that builds it;
  // CallValue calls whatever a value turns out to be.
  //
  // There is deliberately no third tag for "call this function by index".
  // One existed, forwarding the caller's slots directly, and it was both
  // faster and unable to express a function that outlives the frame it was
  // written in. Keeping it would have meant two ownership rules in one
  // Frame -- a borrowed slot pointer and an owned cell -- with the meaning of
  // VarKind::Capture depending on which kind of call got you there. A front
  // end that wants the old shape builds a closure and calls it immediately;
  // examples/pl0 does exactly that.
  MakeClosure,  // a = func index, b = capture map index
  CallValue,    // children: callee, args...
  // Containers. Index and SetIndex dispatch on what the receiver turns out
  // to be, rather than there being a separate pair per container type: a[0]
  // and o["k"] differ only in what a and o are, and a front end that wants
  // them to be different syntax can still lower both to these.
  ArrayLit,   // children: items...
  ObjectLit,  // children: key, value, key, value, ...
  Index,      // children: receiver, key
  SetIndex,   // children: receiver, key, value
  // A lexical region and the local slots it owns: a = first local,
  // b = one past last. Slot indices are the front end's to assign, so the
  // scope structure they follow is too -- this is how it says it. The
  // compiler releases the range when the region exits, however it exits
  // (falling off the end, or the unwinding a later phase adds), which is
  // what makes a value's lifetime end with its scope rather than with the
  // whole frame. Yields its child's value, like Block.
  Scope,      // a = first local, b = one past last; children: body
  // Non-local exits. Statements, like PL/0's: nothing reads their value.
  // Each one leaves every Scope between it and its target the way the
  // scope's own exit would -- locals released -- which is what earns them a
  // place in the IR rather than being a front-end Jump.
  Return,     // children: value (optional; none returns nil)
  Break,      // no children; verify() requires an enclosing While body
  Continue,   // no children; verify() requires an enclosing While body
  // Exceptions. Throw raises any value; TryCatch guards its first child,
  // lands what a throw (or a trap the executor raises itself -- divide by
  // zero, a wrong-typed operand) carried in the local slot `a`, and resumes
  // in its second. Yields the value of whichever child finished, like If.
  // The handler is not guarded by its own try; a throw there unwinds to the
  // next enclosing one.
  Throw,      // children: value
  TryCatch,   // a = caught local slot; children: body, handler
  // Registers a 0-arity callable to run when the enclosing Scope exits --
  // however it exits: falling through, Break, Continue, Return, or an
  // unwinding throw. LIFO within the scope. verify() requires an enclosing
  // Scope: a front end that wants function-level defers wraps the function
  // body in one.
  Defer,      // children: value (a callable)
  // Replaces one of the frame's cells with a fresh, nil-holding box. What a
  // "loop iteration's own binding" means is exactly this: closures built in
  // earlier iterations keep the old cell (their captures own it), while the
  // scope entering now declares into the new one. A front end emits it at
  // the top of any region whose per-entry bindings are captured.
  CellFresh,  // a = cell index
  // Suspends the enclosing generator function, handing its child's value to
  // whoever resumed it, and yields -- when a later GenResume re-enters --
  // the value that resume was called with. verify() requires the enclosing
  // Func to carry is_generator; calling such a function builds a suspended
  // activation instead of running the body, and the GenResume / GenReturn
  // intrinsics drive it from there.
  Yield,      // children: value
};

enum class UnOp : uint8_t { Neg, BitNot };

enum class BinOp : uint8_t {
  Add, Sub, Mul, Div, Mod,
  Eq, Ne, Lt, Le, Gt, Ge,
  // Int-only. Shift counts are masked to the low six bits (1 << 64 == 1),
  // matching the hardware and Java rather than trapping; Shr is arithmetic.
  BitAnd, BitOr, BitXor, Shl, Shr,
};

// ToStr formats any value the way to_display (coreir/semantics.h) does --
// shortest-round-trip doubles included. A language whose display rules
// differ (culebra prints whole doubles as "4.0") post-processes the result
// rather than this growing a mode; what it cannot do in-language is produce
// digits from a number at all, which is why this is an intrinsic.
// TypeOf yields the value's tag as a string -- type_name's vocabulary
// ("int", "double", "string", ...). A dynamic front end needs it for any
// dispatch its own semantics do on a value's kind; its own type names are
// its own mapping to write over this one.
// The numeric conversions and the two float operations a front end cannot
// write in-language: ToInt truncates a double toward zero (and traps on
// NaN, an infinity, or a value outside int64's range); ToDouble widens an
// int; FMod is IEEE fmod over doubles (int operands widen; a zero divisor
// traps, like integer Mod); Pow is std::pow over doubles. Integer
// exponentiation is deliberately absent -- a loop over wrapping Mul writes
// it in-language, in whatever overflow discipline the language wants.
enum class IntrinsicId : uint8_t {
  Print, ReadInt, Len, ToStr, TypeOf, ToInt, ToDouble, FMod, Pow,
  // Print without the trailing newline, through coreir_rt_out_raw. The value
  // is formatted the way ToStr formats it.
  PrintRaw,
  // The container primitives a front end cannot write over Index/SetIndex
  // alone: growing and shrinking an array, and asking an object what it
  // holds. Everything else -- insert, slices, maps, filters -- is a rebuild
  // loop a front end writes in its own language.
  ArrayPush,    // (array, value) -> nil, appends
  ArrayPop,     // (array) -> last value, removed; an empty array fails
  ObjectHas,    // (object, key) -> bool (Index reads a missing key as nil,
                //  which cannot tell absent from nil-valued)
  ObjectKeys,   // (object) -> array of keys, insertion order
  ObjectRemove, // (object, key) -> nil; removing an absent key is a no-op
  // How many arguments the running function was called with -- the count
  // the caller supplied, not num_params. Only interesting under
  // Func::lenient_arity, where the two can differ; 0 at the entry point.
  ArgCount,     // () -> int
  // Reference identity: whether two values are the same heap object (or,
  // for scalars, the same tag and payload). Eq deliberately refuses two
  // objects -- what "equal" means for them is the language's call -- but
  // every language needs this one primitive underneath its answer.
  Same,         // (a, b) -> bool
  // A function value's declared parameter count -- Func::num_params, which
  // is every parameter (there are no hidden ones: captures travel as cells,
  // and a generator's parameters are the same locals). The one fact a front
  // end needs to check "this callback takes two arguments" before calling
  // it; anything but a Func traps.
  FnArity,      // (f) -> int
  // The tracing collector, on demand: a full collection right now, on top
  // of the ones the allocators run on their own (Runtime::collect). Answers
  // how many objects it freed. A condemned Object carrying the drop key
  // gets its destructor -- the same closure call a refcount death makes,
  // over a still-whole cycle, newest object first -- and one that stores
  // itself somewhere reachable is spared (Runtime::collect has the rule).
  // Called from inside a destructor it answers 0: one collection at a
  // time.
  Collect,      // () -> int (objects freed)
  // The heap's current size, as a fresh {live_objects, heap_bytes} object:
  // live_objects is Runtime::live_objects(), heap_bytes is
  // Runtime::heap_bytes(). Both are read before the result object is
  // allocated, so it does not count itself. The keys are the contract; a
  // host may add to them but not rename them.
  HeapStats,    // () -> {live_objects: int, heap_bytes: int}
  // Generators. Both answer with a fresh {value, done} object -- the JS
  // result shape, chosen because it carries "finished" and "what came out"
  // in one allocation a front end can destructure however its own protocol
  // likes. Resume re-enters at the Yield (whose value becomes the sent
  // argument; a first resume's is ignored) and runs to the next Yield
  // ({value, done: false}) or to the body's return ({value, done: true}).
  // A finished generator answers {value: nil, done: true}; resuming one
  // that is already running traps. Return closes early: it runs the
  // suspended frame's pending defers -- innermost first, as the yield
  // point's own Return would -- then discards the frame and answers
  // {value: <arg>, done: true}. A dropped generator runs nothing.
  GenResume,    // (generator, sent) -> {value, done}
  GenReturn,    // (generator, value) -> {value, done: true}
};

// A variable is either a slot in this frame or a slot borrowed from an
// enclosing one. There is deliberately no "level" -- static links assume the
// defining activation is still on the stack, which closures break. A capture
// list survives first-class functions, so VarRef's meaning does not have to
// change when they arrive.
// Cell joins the two. A local a closure captures cannot stay a slot in
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

// `bits` is the payload for Int, Bool and Double (the last as its bit
// pattern); for Str it indexes Module::str_consts rather than holding the
// bytes, so Const stays a two-word POD and the pool stays in one place.
enum class ConstKind : uint8_t { Nil, Int, Bool, Double, Str };

struct Const {
  ConstKind kind = ConstKind::Int;
  int64_t bits = 0;
};

// Where a closure's capture comes from, expressed in the frame that builds
// it. A per-function list would not work: fib's captures live in the root
// frame, but fib's own recursive call runs with fib's frame, and finding "the
// defining frame" at run time is exactly the static link this design rejects.
// So the forwarding table belongs to the site that makes the closure.
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
  // Cells are storage the frame shares with closures built inside it;
  // params are the first `num_params` locals, so an argument that needs
  // capturing is copied into a cell by the front end and the calling
  // convention itself stays about locals only. Both sit after the existing
  // members so that every brace initializer already written still means what
  // it did.
  int32_t num_cells = 0;
  int32_t num_params = 0;
  // Calling this function packages an activation instead of running it; its
  // body may Yield. Appended after the two above, for brace-init
  // compatibility.
  bool is_generator = false;
  // Calls tolerate any argument count: extras are dropped, missing params
  // start as nil, and ArgCount tells the body how many were actually
  // passed -- JavaScript's convention, so a front end can raise its own
  // "missing argument" diagnostic (with the parameter's name, which the
  // executor never knew) or fill a default. Off, a mismatch traps.
  bool lenient_arity = false;
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
  std::vector<std::string> str_consts;  // bytes for ConstKind::Str
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
    case Tag::Intrinsic: return -1;  // per IntrinsicId
    case Tag::MakeClosure: return 0;
    case Tag::CallValue:   return -1;  // callee, then args
    case Tag::ArrayLit:    return -1;
    case Tag::ObjectLit:   return -1;  // an even number: key, value, ...
    case Tag::Index:       return 2;
    case Tag::SetIndex:    return 3;
    case Tag::Scope:       return 1;
    case Tag::Return:      return -1;  // 0 or 1
    case Tag::Break:       return 0;
    case Tag::Continue:    return 0;
    case Tag::Throw:       return 1;
    case Tag::TryCatch:    return 2;
    case Tag::Defer:       return 1;
    case Tag::CellFresh:   return 0;
    case Tag::Yield:       return 1;
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
    case IntrinsicId::Len:     return 1;
    case IntrinsicId::ToStr:   return 1;
    case IntrinsicId::TypeOf:  return 1;
    case IntrinsicId::ToInt:   return 1;
    case IntrinsicId::ToDouble: return 1;
    case IntrinsicId::FMod:    return 2;
    case IntrinsicId::Pow:     return 2;
    case IntrinsicId::PrintRaw: return 1;
    case IntrinsicId::ArrayPush: return 2;
    case IntrinsicId::ArrayPop: return 1;
    case IntrinsicId::ObjectHas: return 2;
    case IntrinsicId::ObjectKeys: return 1;
    case IntrinsicId::ObjectRemove: return 2;
    case IntrinsicId::ArgCount: return 0;
    case IntrinsicId::Same: return 2;
    case IntrinsicId::FnArity: return 1;
    case IntrinsicId::Collect: return 0;
    case IntrinsicId::HeapStats: return 0;
    case IntrinsicId::GenResume: return 2;
    case IntrinsicId::GenReturn: return 2;
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
    // Unlike Call, these produce a value. Call is left alone rather
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
    case Tag::ArrayLit:
    case Tag::ObjectLit:
    case Tag::Index:
    case Tag::Scope:
    case Tag::TryCatch:
    case Tag::Yield:
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
struct ClosureView { int32_t func; int32_t capture_map; };
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
inline ClosureView view_make_closure(const Module& m, NodeId n) {
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

  // Same interning shape as intern_int, over the string pool.
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

  // Interning by (kind, bits) rather than a per-kind scan: Bool and Double
  // want the same dedup Int gets, and one comparison covers all three because
  // a Double is stored as its bit pattern.
  int32_t intern_scalar(ConstKind kind, int64_t bits) {
    for (uint32_t i = 0; i < m_.consts.size(); ++i) {
      if (m_.consts[i].kind == kind && m_.consts[i].bits == bits) {
        return static_cast<int32_t>(i);
      }
    }
    m_.consts.push_back({kind, bits});
    return static_cast<int32_t>(m_.consts.size() - 1);
  }

  NodeId literal(int64_t v, SrcPos p) {
    return emit(Tag::Literal, 0, p, intern_int(v), 0, {});
  }
  NodeId bool_literal(bool v, SrcPos p) {
    return emit(Tag::Literal, 0, p, intern_scalar(ConstKind::Bool, v ? 1 : 0),
                0, {});
  }
  NodeId double_literal(double v, SrcPos p) {
    int64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(double));
    return emit(Tag::Literal, 0, p, intern_scalar(ConstKind::Double, bits), 0,
                {});
  }
  NodeId nil_literal(SrcPos p) {
    return emit(Tag::Literal, 0, p, intern_scalar(ConstKind::Nil, 0), 0, {});
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
  NodeId scope(int32_t first_local, int32_t end_local, NodeId body, SrcPos p) {
    return emit(Tag::Scope, 0, p, first_local, end_local, {body});
  }
  NodeId make_return(NodeId value, SrcPos p) {
    if (value.valid()) return emit(Tag::Return, 0, p, 0, 0, {value});
    return emit(Tag::Return, 0, p, 0, 0, {});
  }
  NodeId make_break(SrcPos p) { return emit(Tag::Break, 0, p, 0, 0, {}); }
  NodeId make_throw(NodeId value, SrcPos p) {
    return emit(Tag::Throw, 0, p, 0, 0, {value});
  }
  NodeId make_defer(NodeId value, SrcPos p) {
    return emit(Tag::Defer, 0, p, 0, 0, {value});
  }
  NodeId cell_fresh(int32_t cell, SrcPos p) {
    return emit(Tag::CellFresh, 0, p, cell, 0, {});
  }
  NodeId make_yield(NodeId value, SrcPos p) {
    return emit(Tag::Yield, 0, p, 0, 0, {value});
  }
  NodeId make_try(int32_t caught_local, NodeId body, NodeId handler,
                  SrcPos p) {
    return emit(Tag::TryCatch, 0, p, caught_local, 0, {body, handler});
  }
  NodeId make_continue(SrcPos p) {
    return emit(Tag::Continue, 0, p, 0, 0, {});
  }
  NodeId intrinsic(IntrinsicId id, const std::vector<NodeId>& args, SrcPos p) {
    return emit(Tag::Intrinsic, static_cast<uint8_t>(id), p, 0, 0, args);
  }
  NodeId make_closure(int32_t func, int32_t capture_map, SrcPos p) {
    return emit(Tag::MakeClosure, 0, p, func, capture_map, {});
  }
  NodeId array_lit(const std::vector<NodeId>& items, SrcPos p) {
    return emit(Tag::ArrayLit, 0, p, 0, 0, items);
  }
  // Pairs rather than a side table of names, so a computed key costs nothing
  // extra and the verifier has one shape to check.
  NodeId object_lit(const std::vector<std::pair<NodeId, NodeId>>& kvs,
                    SrcPos p) {
    std::vector<NodeId> children;
    children.reserve(kvs.size() * 2);
    for (const auto& kv : kvs) {
      children.push_back(kv.first);
      children.push_back(kv.second);
    }
    return emit(Tag::ObjectLit, 0, p, 0, 0, children);
  }
  NodeId index(NodeId recv, NodeId key, SrcPos p) {
    return emit(Tag::Index, 0, p, 0, 0, {recv, key});
  }
  NodeId set_index(NodeId recv, NodeId key, NodeId value, SrcPos p) {
    return emit(Tag::SetIndex, 0, p, 0, 0, {recv, key, value});
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
