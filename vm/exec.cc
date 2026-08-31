#include "vm/exec.h"

#include <string>
#include <vector>

#include "coreir/semantics.h"
#include "pl0rt.h"

using namespace coreir;

namespace vm {
namespace {

struct Frame {
  std::vector<Slot> locals;
  std::vector<Slot*> captures;
  std::vector<int64_t> regs;
};

struct Exec {
  const Program& p;

  void call(int32_t func, const std::vector<CaptureSrc>& cmap, Frame* caller) {
    const Chunk& ch = p.chunks[func];
    Frame f;
    f.locals.resize(static_cast<size_t>(ch.num_locals));
    f.regs.resize(static_cast<size_t>(ch.num_regs));
    f.captures.reserve(cmap.size());
    for (const CaptureSrc& src : cmap) {
      f.captures.push_back(src.from == VarKind::Local
                               ? &caller->locals[src.index]
                               : caller->captures[src.index]);
    }
    run_chunk(ch, f);
  }

  [[noreturn]] void fail(const Chunk& ch, size_t pc, const std::string& msg) {
    const SrcPos sp = p.positions[ch.code_pos[pc]];
    pl0rt::fail(msg, sp.line, sp.col);
  }

  Slot& slot_of(Frame& f, int32_t kind, int32_t index) {
    return static_cast<VarKind>(kind) == VarKind::Local ? f.locals[index]
                                                        : *f.captures[index];
  }

  void run_chunk(const Chunk& ch, Frame& f) {
    size_t pc = 0;
    while (pc < ch.code.size()) {
      const Insn& in = ch.code[pc];
      switch (in.op) {
        case Op::LoadConst:
          f.regs[in.a] = p.consts[in.b].bits;
          break;
        case Op::Neg:
          f.regs[in.a] = wrap_neg(f.regs[in.b]);
          break;
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
        case Op::Eq:  case Op::Ne:  case Op::Lt:  case Op::Le:
        case Op::Gt:  case Op::Ge: {
          const BinOp op = binop_of(in.op);
          const int64_t l = f.regs[in.b];
          const int64_t r = f.regs[in.c];
          if (const char* trap = binop_trap(op, l, r)) fail(ch, pc, trap);
          f.regs[in.a] = apply_binop(op, l, r);
          break;
        }
        case Op::LoadVar: {
          Slot& s = slot_of(f, in.b, in.c);
          if (!s.inited) {
            const auto& names = static_cast<VarKind>(in.b) == VarKind::Local
                                    ? ch.local_names
                                    : ch.capture_names;
            fail(ch, pc, "uninitialized variable '" + names[in.c] + "'");
          }
          f.regs[in.a] = s.value;
          break;
        }
        case Op::StoreVar: {
          Slot& s = slot_of(f, in.a, in.b);
          s.value = f.regs[in.c];
          s.inited = 1;
          break;
        }
        case Op::Jump:
          pc = static_cast<size_t>(in.a);
          continue;
        case Op::JumpIfFalse:
          if (!truthy(f.regs[in.a])) {
            pc = static_cast<size_t>(in.b);
            continue;
          }
          break;
        case Op::Call:
          call(in.a, p.capture_maps[in.b], &f);
          break;
        case Op::Out:
          pl0_rt_out(f.regs[in.a]);
          break;
        case Op::In: {
          const SrcPos sp = p.positions[ch.code_pos[pc]];
          f.regs[in.a] = pl0_rt_in(sp.line, sp.col);
          break;
        }
        case Op::Ret:
          return;
      }
      ++pc;
    }
  }

  static BinOp binop_of(Op op) {
    switch (op) {
      case Op::Add: return BinOp::Add;
      case Op::Sub: return BinOp::Sub;
      case Op::Mul: return BinOp::Mul;
      case Op::Div: return BinOp::Div;
      case Op::Mod: return BinOp::Mod;
      case Op::Eq:  return BinOp::Eq;
      case Op::Ne:  return BinOp::Ne;
      case Op::Lt:  return BinOp::Lt;
      case Op::Le:  return BinOp::Le;
      case Op::Gt:  return BinOp::Gt;
      default:      return BinOp::Ge;
    }
  }
};

}  // namespace

void run(const Program& p) {
  Exec e{p};
  const Chunk& entry = p.chunks[0];
  Frame f;
  f.locals.resize(static_cast<size_t>(entry.num_locals));
  f.regs.resize(static_cast<size_t>(entry.num_regs));
  e.run_chunk(entry, f);
}

}  // namespace vm
