#include "vm/bytecode.h"

#include <sstream>

namespace vm {
namespace {

const char* name_of(Op op) {
  switch (op) {
    case Op::LoadConst:   return "loadconst";
    case Op::Neg:         return "neg";
    case Op::BitNot:      return "bitnot";
    // The arithmetic/compare range shares coreir's name table via binop_of
    // rather than re-typing the same eleven strings.
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
    case Op::Eq:  case Op::Ne:  case Op::Lt:  case Op::Le:
    case Op::Gt:  case Op::Ge:
    case Op::BitAnd: case Op::BitOr: case Op::BitXor:
    case Op::Shl: case Op::Shr:
      return coreir::name_of(binop_of(op));
    case Op::LoadVar:     return "loadvar";
    case Op::StoreVar:    return "storevar";
    case Op::Jump:        return "jump";
    case Op::JumpIfFalse: return "jumpiffalse";
    case Op::Out:         return "out";
    case Op::In:          return "in";
    case Op::Ret:         return "ret";
    case Op::Throw:       return "throw";
    case Op::DeferPush:   return "deferpush";
    case Op::DeferMark:   return "defermark";
    case Op::DeferRunTo:  return "deferrunto";
    case Op::MakeClosure: return "makeclosure";
    case Op::CallValue:   return "callvalue";
    case Op::CellNew:     return "cellnew";
    case Op::LoadNil:     return "loadnil";
    case Op::Move:        return "move";
    case Op::ClearRegs:   return "clearregs";
    case Op::ClearLocals: return "clearlocals";
    case Op::NewArray:    return "newarray";
    case Op::Index:       return "index";
    case Op::SetIndex:    return "setindex";
    case Op::Len:         return "len";
    case Op::ToStr:       return "tostr";
    case Op::NewObject:   return "newobject";
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
        case Op::ClearLocals:
          out << " local[" << in.a << ".." << in.b << ")";
          break;
        case Op::NewArray:
          out << " r" << in.a << ", items r" << in.b << ".."
              << (in.b + in.c);
          break;
        case Op::Jump:
          out << " " << in.a;
          break;
        case Op::JumpIfFalse:
          out << " r" << in.a << ", " << in.b;
          break;
        case Op::Out:
        case Op::In:
        case Op::Throw:
        case Op::DeferPush:
          out << " r" << in.a;
          break;
        case Op::DeferMark:
        case Op::DeferRunTo:
          break;
        case Op::Ret:
          break;
        default:
          out << " r" << in.a << ", r" << in.b << ", r" << in.c;
          break;
      }
      out << "\t; " << sp.line << ":" << sp.col << "\n";
    }
    for (const Cleanup& cl : ch.cleanups) {
      out << "  cleanup [" << cl.start_pc << ".." << cl.end_pc << ") local["
          << cl.first_local << ".." << cl.end_local << ") regs>=" << cl.regs_base;
      if (cl.handler_pc >= 0) {
        out << " handler=" << cl.handler_pc << " caught=local["
            << cl.caught_local << "]";
      }
      if (cl.defer_mark_pc >= 0) out << " defer_mark=" << cl.defer_mark_pc;
      out << "\n";
    }
  }
  return out.str();
}

}  // namespace vm
