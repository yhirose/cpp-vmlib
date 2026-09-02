// A register machine, matching culebra's choice.
//
// A stack machine was the other option; a register machine was chosen because
// it is what culebra's own bytecode is, and this library exists in part to
// rehearse a design culebra could grow into. Lowering a stack machine to SSA
// (for a future backend that wants one) means symbolically executing the
// operand stack across block boundaries, which is knowledge worth not
// acquiring if a register machine sidesteps it for free.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "coreir/ir.h"

namespace vm {

enum class Op : uint8_t {
  LoadConst,    // a = dst, b = const index
  Neg,          // a = dst, b = src
  BitNot,       // a = dst, b = src
  Add, Sub, Mul, Div, Mod,   // a = dst, b = lhs, c = rhs
  Eq, Ne, Lt, Le, Gt, Ge,    // a = dst, b = lhs, c = rhs   (writes 0 or 1)
  BitAnd, BitOr, BitXor, Shl, Shr,  // a = dst, b = lhs, c = rhs
  LoadVar,      // a = dst, b = VarKind, c = index   (rejects Uninit)
  StoreVar,     // a = VarKind, b = index, c = src
  Jump,         // a = target
  JumpIfFalse,  // a = cond reg, b = target
  Out,          // a = src
  OutRaw,       // a = src   (no trailing newline)
  In,           // a = dst
  Ret,          // a = result reg, b = 1 if there is one
  Throw,        // a = value reg; unwinds to the nearest handler
  // Defers. Push registers a callable to run at the owning scope's exit;
  // Mark records the defer-stack height (and its own pc -- the unwinder
  // matches marks to regions by it); RunTo pops the innermost mark and runs
  // the defers above it, LIFO, each as a normal 0-arity call.
  DeferPush,    // a = value reg
  DeferMark,    // no operands
  DeferRunTo,   // no operands
  // A Scope's entry: records the owned stack's mark (and its own pc, the
  // region's identity for the unwinder). The scope's ClearLocals -- at its
  // own exit, or emitted for it by a Break / Return -- pops the mark and
  // resolves the entries above it (Runtime::owned_scope_exit).
  OwnedMark,    // no operands
  // First-class functions.
  MakeClosure,  // a = dst, b = func index, c = capture map index
  CallValue,    // a = dst, b = callee reg, c = first arg reg, d = arg count
  CellNew,      // a = cell index   (fresh box, holding nil)
  LoadNil,      // a = dst
  Move,         // a = dst, b = src
  // Drops registers [a, b) -- the temporaries a statement finished with.
  // Without it a dead temporary keeps whatever it holds alive until its
  // register is reused or the frame returns, which for a value with a
  // release worth timing is too late.
  ClearRegs,    // a = first, b = one past last
  // The same, for a scope's local slots as it exits. Back to Uninit, not
  // nil: the slots are dead until something declares into them again, and a
  // read before that is the same mistake as a read before first assignment.
  ClearLocals,  // a = first, b = one past last
  NewArray,     // a = dst, b = first item reg, c = item count
  Index,        // a = dst, b = receiver reg, c = key reg
  SetIndex,     // a = receiver reg, b = key reg, c = value reg
  Len,          // a = dst, b = src
  ToStr,        // a = dst, b = src   (to_display's formatting)
  ArrayPush,    // a = array reg, b = value reg
  ArrayPop,     // a = dst, b = array reg
  ObjectHas,    // a = dst, b = object reg, c = key reg
  ObjectKeys,   // a = dst, b = object reg
  ObjectRemove, // a = object reg, b = key reg
  ArgCount,     // a = dst   (the frame's supplied argument count)
  Same,         // a = dst, b = lhs, c = rhs   (reference identity)
  FnArity,      // a = dst, b = src   (a Func's num_params; else traps)
  Collect,      // a = dst   (runs Runtime::collect; the count it freed)
  HeapStats,    // a = dst   (fresh {live_objects, heap_bytes} object)
  TypeOf,       // a = dst, b = src   (type_name's vocabulary, as a string)
  ToInt,        // a = dst, b = src   (truncate toward zero; traps off-range)
  ToDouble,     // a = dst, b = src
  FMod,         // a = dst, b = lhs, c = rhs   (IEEE fmod; 0 divisor traps)
  Pow,          // a = dst, b = lhs, c = rhs   (std::pow over doubles)
  NewObject,    // a = dst   (empty; ObjectLit fills it with SetIndex)
  // Generators. Yield suspends the frame back into its GeneratorObj and
  // delivers {value, done: false} to whoever resumed it; the sent value of
  // the next resume lands in `a`. GenResume and GenReturn are ir.h's
  // intrinsics of the same names.
  Yield,        // a = dst (sent value on re-entry), b = value reg
  GenResume,    // a = dst, b = generator reg, c = sent reg
  GenReturn,    // a = dst, b = generator reg, c = value reg
};

// vm::Op's Add..Ge deliberately sit at a fixed offset from coreir::BinOp's own
// Add..Ge, so the compiler and the executor can share one arithmetic
// conversion instead of each hand-writing an eleven-arm switch that could
// silently drift out of step with the other.
inline constexpr int32_t kBinOpOffset =
    static_cast<int32_t>(Op::Add) - static_cast<int32_t>(coreir::BinOp::Add);

inline constexpr Op op_of(coreir::BinOp op) {
  return static_cast<Op>(static_cast<int32_t>(op) + kBinOpOffset);
}
inline constexpr coreir::BinOp binop_of(Op op) {
  return static_cast<coreir::BinOp>(static_cast<int32_t>(op) - kBinOpOffset);
}
static_assert(op_of(coreir::BinOp::Add) == Op::Add);
static_assert(op_of(coreir::BinOp::Ge) == Op::Ge);
static_assert(op_of(coreir::BinOp::Shr) == Op::Shr);

struct Insn {
  Op op;
  int32_t a = 0;
  int32_t b = 0;
  int32_t c = 0;
  int32_t d = 0;  // only CallValue needs a fourth operand (arg count)
};

// One record per lexical region the compiler closed. Children close before
// their parent, so scanning the vector in order visits the regions holding a
// given pc innermost first -- the order an unwinding walk wants -- with no
// parent links to maintain.
//
// Nothing consumes these yet beyond their recording; the exception phase's
// unwinder is the reader this shape is for.
struct Cleanup {
  int32_t start_pc = 0;      // half-open instruction range of the region,
  int32_t end_pc = 0;        // including its own exit-time ClearLocals
  int32_t first_local = 0;   // the local slots the region owns
  int32_t end_local = 0;
  int32_t regs_base = 0;     // registers >= this are the region's temps
  int32_t handler_pc = -1;   // >= 0: a try region; where its handler starts
  int32_t caught_local = -1; // the local slot a caught value lands in
  // >= 0: the region declares defers, and this is its DeferMark's pc. The
  // unwinder runs the region's pending defers only while the mark is still
  // outstanding (top of the frame's mark stack carries the same pc), which
  // is what keeps a throw out of the region's own exit-time defer run from
  // running them twice.
  int32_t defer_mark_pc = -1;
  // >= 0 for a Scope region: its OwnedMark's pc, matched against the top of
  // the frame's mark stack the same way, so a region whose exit already
  // resolved (a Break's ClearLocals, before a later exit-time throw) is not
  // resolved twice.
  int32_t owned_mark_pc = -1;
};

struct Chunk {
  std::string name;
  std::vector<Insn> code;
  // Parallel to code: index into Program::positions. A side table, the way
  // culebra holds PosEntry, so error positions are structural rather than
  // hand-threaded through every instruction.
  std::vector<uint32_t> code_pos;
  int32_t num_locals = 0;
  int32_t num_captures = 0;
  int32_t num_regs = 0;
  int32_t num_cells = 0;
  int32_t num_params = 0;
  bool is_generator = false;
  bool lenient_arity = false;
  std::vector<std::string> local_names;
  std::vector<std::string> capture_names;
  std::vector<Cleanup> cleanups;
};

struct Program {
  std::vector<Chunk> chunks;  // chunks[0] is the entry point
  std::vector<coreir::Const> consts;
  std::vector<std::string> str_consts;  // bytes for ConstKind::Str
  std::vector<coreir::SrcPos> positions;
  std::vector<std::vector<coreir::CaptureSrc>> capture_maps;
};

std::string to_string(const Program& p);

}  // namespace vm
