#include "vm/compiler.h"

#include <algorithm>
#include <cstdlib>

using namespace coreir;

namespace vm {
namespace {

struct FnCompiler {
  const Module& m;
  const Func& fn;
  Chunk& ch;
  int32_t top = 0;  // next free register
  // Highest register the statement being compiled has reached, so its end can
  // drop exactly the range it used rather than every register the function
  // owns.
  int32_t high_water = 0;

  int32_t alloc() {
    const int32_t r = top++;
    if (top > high_water) high_water = top;
    ch.num_regs = std::max(ch.num_regs, top);
    return r;
  }

  size_t emit(Op op, int32_t a, int32_t b, int32_t c, uint32_t pos,
              int32_t d = 0) {
    ch.code.push_back({op, a, b, c, d});
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

  // "Whatever this node is, leave its value in a register." A statement --
  // an Assign, a static Call, a While -- has no value, so it runs and yields
  // nil. Three callers need exactly this (a function body, a block's last
  // child, an If arm), and each of them can be handed a statement by a front
  // end that is not doing anything wrong.
  int32_t compile_value(NodeId id) {
    if (yields_value(m.at(id).tag)) return compile_expr(id);
    compile_stmt(id);
    const int32_t r = alloc();
    emit(Op::LoadNil, r, 0, 0, m.at(id).pos);
    return r;
  }

  // Compile one arm of a value-producing If so its result lands in `dst`
  // rather than wherever the arm's own allocation happened to put it.
  void branch_into(int32_t dst, NodeId arm, uint32_t pos) {
    const int32_t base = top;
    const int32_t r = compile_value(arm);
    if (r != dst) emit(Op::Move, dst, r, 0, pos);
    top = base;
  }

  // Every expression lands in a fresh register; a statement releases whatever
  // it used. PL/0 nests shallowly enough that nothing smarter earns its keep.
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
        emit(v.op == UnOp::BitNot ? Op::BitNot : Op::Neg, r, s, 0, n.pos);
        return r;
      }
      case Tag::Scope: {
        const Node& sn = m.at(id);
        const int32_t regs_base = top;
        const int32_t start = here();
        const int32_t r = compile_value(m.child(id, 0));
        emit(Op::ClearLocals, sn.a, sn.b, 0, sn.pos);
        ch.cleanups.push_back(
            {start, here(), sn.a, sn.b, regs_base, -1, -1});
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
        if (v.id == IntrinsicId::Len) {
          const int32_t base = top;
          const int32_t s = compile_expr(m.child(id, 0));
          top = base;
          const int32_t r = alloc();
          emit(Op::Len, r, s, 0, n.pos);
          return r;
        }
        // Print is a statement; in value position it yields nil. This used to
        // emit LoadConst 0, which read the first entry of a const pool that
        // a program need not have -- latent until Block became a value-
        // producing tag and put Print in value position for the first time.
        compile_stmt(id);
        const int32_t r = alloc();
        emit(Op::LoadNil, r, 0, 0, n.pos);
        return r;
      }
      // A block's value is its last child's; every child before it is a
      // statement. An empty block is nil.
      case Tag::Block: {
        if (n.num_children == 0) {
          const int32_t r = alloc();
          emit(Op::LoadNil, r, 0, 0, n.pos);
          return r;
        }
        for (uint32_t i = 0; i + 1 < n.num_children; ++i) {
          compile_stmt(m.child(id, i));
        }
        return compile_value(m.child(id, n.num_children - 1));
      }

      // An If's value is the branch taken's, so both branches have to land in
      // the same register -- which is why this cannot just be two
      // compile_expr calls. A missing else yields nil.
      case Tag::If: {
        auto v = view_if(m, id);
        const int32_t base = top;
        const int32_t c = compile_expr(v.cond);
        top = base;
        const int32_t r = alloc();
        const size_t jf = emit(Op::JumpIfFalse, c, 0, 0, n.pos);
        branch_into(r, v.then_, n.pos);
        const size_t jend = emit(Op::Jump, 0, 0, 0, n.pos);
        patch(jf, here());
        if (v.els.valid()) {
          branch_into(r, v.els, n.pos);
        } else {
          emit(Op::LoadNil, r, 0, 0, n.pos);
        }
        patch(jend, here());
        return r;
      }

      case Tag::MakeClosure: {
        auto v = view_make_closure(m, id);
        const int32_t r = alloc();
        emit(Op::MakeClosure, r, v.func, v.capture_map, n.pos);
        return r;
      }
      case Tag::ArrayLit: {
        // Items go in one contiguous run, the same arrangement CallValue's
        // arguments use, so the instruction needs only a start and a count.
        const int32_t base = top;
        const int32_t items_at = top;
        for (uint32_t i = 0; i < n.num_children; ++i) {
          compile_expr(m.child(id, i));
        }
        top = base;
        const int32_t r = alloc();
        emit(Op::NewArray, r, items_at, static_cast<int32_t>(n.num_children),
             n.pos);
        return r;
      }
      case Tag::ObjectLit: {
        // Built empty and filled with the same SetIndex a later assignment
        // uses, rather than a second construction path that could disagree
        // with it about duplicate keys or key types.
        const int32_t r = alloc();
        emit(Op::NewObject, r, 0, 0, n.pos);
        for (uint32_t i = 0; i < n.num_children; i += 2) {
          const int32_t base = top;
          const int32_t k = compile_expr(m.child(id, i));
          const int32_t v = compile_expr(m.child(id, i + 1));
          emit(Op::SetIndex, r, k, v, n.pos);
          top = base;
        }
        return r;
      }
      case Tag::Index: {
        const int32_t base = top;
        const int32_t recv = compile_expr(m.child(id, 0));
        const int32_t key = compile_expr(m.child(id, 1));
        top = base;
        const int32_t r = alloc();
        emit(Op::Index, r, recv, key, n.pos);
        return r;
      }
      case Tag::CallValue: {
        // The callee and the arguments go into one contiguous run of
        // registers, so the instruction needs only where the run starts and
        // how long it is. `top` is already a stack, so "contiguous" costs
        // nothing to arrange -- just do not reset it between operands.
        const int32_t base = top;
        const int32_t callee = compile_expr(m.child(id, 0));
        const int32_t argc = static_cast<int32_t>(n.num_children) - 1;
        const int32_t args_at = top;
        for (int32_t i = 0; i < argc; ++i) {
          compile_expr(m.child(id, static_cast<uint32_t>(i + 1)));
        }
        top = base;
        const int32_t r = alloc();
        emit(Op::CallValue, r, callee, args_at, n.pos, argc);
        return r;
      }
      default:
        // A statement in value position: verify() rejects this shape before
        // compilation ever runs (main.cc always verifies first), so there is
        // no legitimate value to fabricate here.
        std::abort();
    }
  }

  void compile_stmt(NodeId id) {
    const Node& n = m.at(id);
    const int32_t base = top;
    // A statement's temporaries die with the statement. Resetting `top` alone
    // only tells the compiler the registers are reusable; whatever they hold
    // stays held until something overwrites them or the frame returns, which
    // is not when a source-level scope ended.
    const int32_t outer_high_water = high_water;
    high_water = top;
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

      case Tag::Intrinsic: {
        auto v = view_intrinsic(m, id);
        if (v.id == IntrinsicId::Print) {
          const int32_t s = compile_expr(m.child(id, 0));
          emit(Op::Out, s, 0, 0, n.pos);
        } else {
          compile_expr(id);  // value discarded
        }
        break;
      }

      case Tag::SetIndex: {
        const int32_t recv = compile_expr(m.child(id, 0));
        const int32_t key = compile_expr(m.child(id, 1));
        const int32_t val = compile_expr(m.child(id, 2));
        emit(Op::SetIndex, recv, key, val, n.pos);
        break;
      }

      default: {
        // An expression used as a statement: evaluate it and drop the result.
        compile_expr(id);
        break;
      }
    }
    if (high_water > base) emit(Op::ClearRegs, base, high_water, 0, n.pos);
    high_water = std::max(outer_high_water, high_water);
    top = base;
  }
};

}  // namespace

Program compile(const Module& m) {
  Program p;
  p.consts = m.consts;
  p.str_consts = m.str_consts;
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

    ch.num_cells = fn.num_cells;
    ch.num_params = fn.num_params;

    FnCompiler fc{m, fn, ch};
    const uint32_t body_pos = m.at(fn.body).pos;

    // Cells are boxes, and a fresh frame needs fresh ones -- sharing
    // them across activations is exactly the bug that makes a recursive
    // closure see the wrong variable.
    for (int32_t c = 0; c < fn.num_cells; ++c) {
      fc.emit(Op::CellNew, c, 0, 0, body_pos);
    }

    // A function returns its body's value, whatever that is. PL/0's bodies
    // are blocks ending in statements, so they return nil and nothing reads
    // it -- one path rather than a "does this body produce a value" fork that
    // Block, now value-producing, no longer answers usefully anyway.
    const int32_t r = fc.compile_value(fn.body);
    fc.emit(Op::Ret, r, 1, 0, body_pos);
  }
  return p;
}

}  // namespace vm
