#include "vm/compiler.h"

#include <algorithm>

using namespace coreir;

namespace vm {
namespace {

struct FnCompiler {
  const Module& m;
  const Func& fn;
  Chunk& ch;
  int32_t top = 0;  // next free register

  int32_t alloc() {
    const int32_t r = top++;
    ch.num_regs = std::max(ch.num_regs, top);
    return r;
  }

  size_t emit(Op op, int32_t a, int32_t b, int32_t c, uint32_t pos) {
    ch.code.push_back({op, a, b, c});
    ch.code_pos.push_back(pos);
    return ch.code.size() - 1;
  }

  void patch(size_t at, int32_t target) {
    Insn& in = ch.code[at];
    if (in.op == Op::Jump) {
      in.a = target;
    } else {
      in.b = target;
    }
  }

  int32_t here() const { return static_cast<int32_t>(ch.code.size()); }

  // Every expression lands in a fresh register; a statement releases whatever
  // it used. PL/0 nests shallowly enough that nothing smarter earns its
  // keep -- and mem2reg flattens the result in the LLVM lane anyway.
  int32_t compile_expr(NodeId id) {
    const Node& n = m.at(id);
    switch (n.tag) {
      case Tag::Literal: {
        const int32_t r = alloc();
        emit(Op::LoadConst, r, n.a, 0, n.pos);
        return r;
      }
      case Tag::VarRef: {
        auto v = view_varref(m, id);
        const int32_t r = alloc();
        emit(Op::LoadVar, r, static_cast<int32_t>(v.kind), v.index, n.pos);
        return r;
      }
      case Tag::Unary: {
        auto v = view_unary(m, id);
        const int32_t base = top;
        const int32_t s = compile_expr(v.operand);
        top = base;
        const int32_t r = alloc();
        emit(Op::Neg, r, s, 0, n.pos);
        return r;
      }
      case Tag::Binary: {
        auto v = view_binary(m, id);
        const int32_t base = top;
        const int32_t l = compile_expr(v.lhs);
        const int32_t rr = compile_expr(v.rhs);
        top = base;
        const int32_t r = alloc();
        // A failing division reports at its right operand, so that is the
        // position stamped on the instruction that can trap.
        const uint32_t pos = (v.op == BinOp::Div || v.op == BinOp::Mod)
                                 ? m.at(v.rhs).pos
                                 : n.pos;
        emit(op_of(v.op), r, l, rr, pos);
        return r;
      }
      case Tag::Intrinsic: {
        auto v = view_intrinsic(m, id);
        if (v.id == IntrinsicId::ReadInt) {
          const int32_t r = alloc();
          emit(Op::In, r, 0, 0, n.pos);
          return r;
        }
        // Print is a statement; in value position it yields Void.
        compile_stmt(id);
        const int32_t r = alloc();
        emit(Op::LoadConst, r, 0, 0, n.pos);
        return r;
      }
      default:
        // Statement in value position -- verify() rejects this before we get
        // here, so reaching it means the IR was not verified.
        compile_stmt(id);
        const int32_t r = alloc();
        emit(Op::LoadConst, r, 0, 0, n.pos);
        return r;
    }
  }

  static Op op_of(BinOp op) {
    switch (op) {
      case BinOp::Add: return Op::Add;
      case BinOp::Sub: return Op::Sub;
      case BinOp::Mul: return Op::Mul;
      case BinOp::Div: return Op::Div;
      case BinOp::Mod: return Op::Mod;
      case BinOp::Eq:  return Op::Eq;
      case BinOp::Ne:  return Op::Ne;
      case BinOp::Lt:  return Op::Lt;
      case BinOp::Le:  return Op::Le;
      case BinOp::Gt:  return Op::Gt;
      case BinOp::Ge:  return Op::Ge;
    }
    return Op::Add;
  }

  void compile_stmt(NodeId id) {
    const Node& n = m.at(id);
    const int32_t base = top;
    switch (n.tag) {
      case Tag::Block:
        for (uint32_t i = 0; i < n.num_children; ++i) {
          compile_stmt(m.child(id, i));
        }
        break;

      case Tag::Assign: {
        auto v = view_assign(m, id);
        const int32_t s = compile_expr(v.value);
        emit(Op::StoreVar, static_cast<int32_t>(v.kind), v.index, s, n.pos);
        break;
      }

      case Tag::If: {
        auto v = view_if(m, id);
        const int32_t c = compile_expr(v.cond);
        top = base;
        const size_t jf = emit(Op::JumpIfFalse, c, 0, 0, n.pos);
        compile_stmt(v.then_);
        if (v.els.valid()) {
          const size_t jend = emit(Op::Jump, 0, 0, 0, n.pos);
          patch(jf, here());
          compile_stmt(v.els);
          patch(jend, here());
        } else {
          patch(jf, here());
        }
        break;
      }

      case Tag::While: {
        auto v = view_while(m, id);
        const int32_t start = here();
        const int32_t c = compile_expr(v.cond);
        top = base;
        const size_t jf = emit(Op::JumpIfFalse, c, 0, 0, n.pos);
        compile_stmt(v.body);
        emit(Op::Jump, start, 0, 0, n.pos);
        patch(jf, here());
        break;
      }

      case Tag::Call: {
        auto v = view_call(m, id);
        emit(Op::Call, v.func, v.capture_map, 0, n.pos);
        break;
      }

      case Tag::Intrinsic: {
        auto v = view_intrinsic(m, id);
        if (v.id == IntrinsicId::Print) {
          const int32_t s = compile_expr(m.child(id, 0));
          emit(Op::Out, s, 0, 0, n.pos);
        } else {
          const int32_t r = alloc();
          emit(Op::In, r, 0, 0, n.pos);  // value discarded
        }
        break;
      }

      default: {
        // An expression used as a statement: evaluate it and drop the result.
        compile_expr(id);
        break;
      }
    }
    top = base;
  }
};

}  // namespace

Program compile(const Module& m) {
  Program p;
  p.consts = m.consts;
  p.positions = m.positions;
  p.capture_maps = m.capture_maps;
  p.chunks.resize(m.funcs.size());

  for (size_t i = 0; i < m.funcs.size(); ++i) {
    const Func& fn = m.funcs[i];
    Chunk& ch = p.chunks[i];
    ch.name = fn.name;
    ch.num_locals = fn.num_locals;
    ch.num_captures = fn.num_captures;
    ch.local_names = fn.local_names;
    ch.capture_names = fn.capture_names;

    FnCompiler fc{m, fn, ch};
    fc.compile_stmt(fn.body);
    fc.emit(Op::Ret, 0, 0, 0, m.at(fn.body).pos);
  }
  return p;
}

}  // namespace vm
