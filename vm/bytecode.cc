#include "vm/bytecode.h"

#include <sstream>

namespace vm {
namespace {

const char* name_of(Op op) {
  switch (op) {
    case Op::LoadConst:   return "loadconst";
    case Op::Neg:         return "neg";
    case Op::Add:         return "add";
    case Op::Sub:         return "sub";
    case Op::Mul:         return "mul";
    case Op::Div:         return "div";
    case Op::Mod:         return "mod";
    case Op::Eq:          return "eq";
    case Op::Ne:          return "ne";
    case Op::Lt:          return "lt";
    case Op::Le:          return "le";
    case Op::Gt:          return "gt";
    case Op::Ge:          return "ge";
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
