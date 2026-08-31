#include "vm/bytecode.h"

#include <sstream>

namespace vm {
namespace {

const char* name_of(Op op) {
  switch (op) {
    case Op::LoadConst:   return "loadconst";
    case Op::Neg:         return "neg";
    // The arithmetic/compare range shares coreir's name table via binop_of
    // rather than re-typing the same eleven strings.
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
    case Op::Eq:  case Op::Ne:  case Op::Lt:  case Op::Le:
    case Op::Gt:  case Op::Ge:
      return coreir::name_of(binop_of(op));
    case Op::LoadVar:     return "loadvar";
    case Op::StoreVar:    return "storevar";
    case Op::Jump:        return "jump";
    case Op::JumpIfFalse: return "jumpiffalse";
    case Op::Call:        return "call";
    case Op::Out:         return "out";
    case Op::In:          return "in";
    case Op::Ret:         return "ret";
  }
  return "?";
}

}  // namespace

std::string to_string(const Program& p) {
  std::ostringstream out;
  for (size_t i = 0; i < p.chunks.size(); ++i) {
    const Chunk& ch = p.chunks[i];
    out << "chunk #" << i << " " << ch.name << "  locals=" << ch.num_locals
        << " captures=" << ch.num_captures << " regs=" << ch.num_regs << "\n";
    for (size_t j = 0; j < ch.code.size(); ++j) {
      const Insn& in = ch.code[j];
      const coreir::SrcPos sp = p.positions[ch.code_pos[j]];
      out << "  " << j << "\t" << name_of(in.op);
      switch (in.op) {
        case Op::LoadConst:
          out << " r" << in.a << ", " << p.consts[in.b].bits;
          break;
        case Op::Neg:
          out << " r" << in.a << ", r" << in.b;
          break;
        case Op::LoadVar:
          out << " r" << in.a << ", " << (in.b == 0 ? "local" : "capture")
              << "[" << in.c << "]";
          break;
        case Op::StoreVar:
          out << " " << (in.a == 0 ? "local" : "capture") << "[" << in.b
              << "], r" << in.c;
          break;
        case Op::Jump:
          out << " " << in.a;
          break;
        case Op::JumpIfFalse:
          out << " r" << in.a << ", " << in.b;
          break;
        case Op::Call:
          out << " #" << in.a << " cmap=" << in.b;
          break;
        case Op::Out:
        case Op::In:
          out << " r" << in.a;
          break;
        case Op::Ret:
          break;
        default:
          out << " r" << in.a << ", r" << in.b << ", r" << in.c;
          break;
      }
      out << "\t; " << sp.line << ":" << sp.col << "\n";
    }
  }
  return out.str();
}

}  // namespace vm
