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
  Add, Sub, Mul, Div, Mod,   // a = dst, b = lhs, c = rhs
  Eq, Ne, Lt, Le, Gt, Ge,    // a = dst, b = lhs, c = rhs   (writes 0 or 1)
  LoadVar,      // a = dst, b = VarKind, c = index   (rejects Uninit)
  StoreVar,     // a = VarKind, b = index, c = src
  Jump,         // a = target
  JumpIfFalse,  // a = cond reg, b = target
  Out,          // a = src
  In,           // a = dst
  Ret,          // a = result reg, b = 1 if there is one
  // First-class functions.
  MakeClosure,  // a = dst, b = func index, c = capture map index
  CallValue,    // a = dst, b = callee reg, c = first arg reg, d = arg count
  CellNew,      // a = cell index   (fresh box, holding nil)
  LoadNil,      // a = dst
  Move,         // a = dst, b = src
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

struct Insn {
  Op op;
  int32_t a = 0;
  int32_t b = 0;
  int32_t c = 0;
  int32_t d = 0;  // only CallValue needs a fourth operand (arg count)
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
  std::vector<std::string> local_names;
  std::vector<std::string> capture_names;
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
