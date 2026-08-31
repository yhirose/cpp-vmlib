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
    case Op::Out:         return "out";
    case Op::In:          return "in";
    case Op::Ret:         return "ret";
    case Op::MakeClosure: return "makeclosure";
    case Op::CallValue:   return "callvalue";
    case Op::CellNew:     return "cellnew";
    case Op::LoadNil:     return "loadnil";
    case Op::Move:        return "move";
    case Op::ClearRegs:   return "clearregs";
  }
  return "?";
}

const char* kind_name(int32_t k) {
  switch (static_cast<coreir::VarKind>(k)) {
    case coreir::VarKind::Local:   return "local";
    case coreir::VarKind::Capture: return "capture";
    case coreir::VarKind::Cell:    return "cell";
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
          out << " r" << in.a << ", " << kind_name(in.b) << "[" << in.c << "]";
          break;
        case Op::StoreVar:
          out << " " << kind_name(in.a) << "[" << in.b << "], r" << in.c;
          break;
        case Op::MakeClosure:
          out << " r" << in.a << ", #" << in.b << " cmap=" << in.c;
          break;
        case Op::CallValue:
          out << " r" << in.a << ", r" << in.b << ", args r" << in.c << ".."
              << (in.c + in.d);
          break;
        case Op::CellNew:
          out << " cell[" << in.a << "]";
          break;
        case Op::ClearRegs:
          out << " r" << in.a << ".." << in.b;
          break;
        case Op::Jump:
          out << " " << in.a;
          break;
        case Op::JumpIfFalse:
          out << " r" << in.a << ", " << in.b;
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
