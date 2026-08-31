// A register machine, matching culebra's choice.
//
// Both remaining lanes read this: the executor runs it, and llvmgen lowers it.
// That is the structural reason they cannot disagree -- not that they are
// tested against each other, but that there is only one instruction stream.
// Lowering a stack machine to SSA means symbolically executing the operand
// stack across block boundaries, which is knowledge worth not acquiring.

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
  LoadVar,      // a = dst, b = VarKind, c = index   (checks the inited flag)
  StoreVar,     // a = VarKind, b = index, c = src
  Jump,         // a = target
  JumpIfFalse,  // a = cond reg, b = target
  Call,         // a = func index, b = capture map index
  Out,          // a = src
  In,           // a = dst
  Ret,
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
  std::vector<std::string> local_names;
  std::vector<std::string> capture_names;
};

struct Program {
  std::vector<Chunk> chunks;  // chunks[0] is the entry point
  std::vector<coreir::Const> consts;
  std::vector<coreir::SrcPos> positions;
  std::vector<std::vector<coreir::CaptureSrc>> capture_maps;
};

std::string to_string(const Program& p);

}  // namespace vm
