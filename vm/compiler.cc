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

  // The loops and scopes currently open at the point being compiled, so a
  // non-local exit can leave each region it crosses the way the region's own
  // exit would.
  struct OpenLoop {
    int32_t head;                    // where Continue re-enters (the test)
    int32_t stmt_base;               // register floor of the While statement
    size_t scope_depth;              // open_scopes.size() at loop entry
    std::vector<size_t> break_jumps; // patched to the loop's exit
  };
  std::vector<OpenLoop> open_loops;
  struct OpenScope { int32_t first_local, end_local; bool has_defers; };
  std::vector<OpenScope> open_scopes;
  // Where the most recently closed Scope's exit-time DeferRunTo sits, so a
  // TryCatch whose body is that scope can end its guarded region before it
  // (a defer throwing at the body's fall-through exit escapes its own catch,
  // the way culebra's does). -1 when the last scope had no defers.
  int32_t last_scope_defer_run_pc = -1;

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

  // Whether this subtree registers a Defer with the scope being compiled.
  // Nested Scopes own their own defers, so the walk does not descend into
  // them; function literals are separate funcs and never reached.
  bool declares_defers(NodeId id) const {
    const Node& n = m.at(id);
    if (n.tag == Tag::Defer) return true;
    if (n.tag == Tag::Scope) return false;
    for (uint32_t i = 0; i < n.num_children; ++i) {
      if (declares_defers(m.child(id, i))) return true;
    }
    return false;
  }

  // What a jump out of nested regions owes them: each Scope down to (not
  // including) `scope_depth` releases its locals, and the registers above the
  // target statement's floor -- temps of the abandoned regions -- are
  // dropped. num_regs at this point bounds every register a region entered so
  // far can have touched, so the clear cannot miss one.
  void leave_down_to(size_t scope_depth, int32_t regs_floor, uint32_t pos) {
    for (size_t i = open_scopes.size(); i > scope_depth; --i) {
      const OpenScope& sc = open_scopes[i - 1];
      if (sc.has_defers) emit(Op::DeferRunTo, 0, 0, 0, pos);
      emit(Op::ClearLocals, sc.first_local, sc.end_local, 0, pos);
    }
    if (ch.num_regs > regs_floor) {
      emit(Op::ClearRegs, regs_floor, ch.num_regs, 0, pos);
    }
  }

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
        const bool defers = declares_defers(m.child(id, 0));
        const int32_t regs_base = top;
        const int32_t mark_pc = defers ? here() : -1;
        if (defers) emit(Op::DeferMark, 0, 0, 0, sn.pos);
        const int32_t start = here();
        open_scopes.push_back({sn.a, sn.b, defers});
        int32_t r = compile_value(m.child(id, 0));
        open_scopes.pop_back();
        int32_t defer_run_pc = -1;
        if (defers) {
          // The Move puts one instruction between the body's last call and
          // the exit-time defer run: a callee's resume pc then still sits
          // inside every region that must see its throw, while the
          // DeferRunTo's own pc sits outside a fused try's (below).
          const int32_t held = alloc();
          emit(Op::Move, held, r, 0, sn.pos);
          r = held;
          defer_run_pc = here();
          emit(Op::DeferRunTo, 0, 0, 0, sn.pos);
        }
        emit(Op::ClearLocals, sn.a, sn.b, 0, sn.pos);
        ch.cleanups.push_back(
            {start, here(), sn.a, sn.b, regs_base, -1, -1, mark_pc});
        last_scope_defer_run_pc = defer_run_pc;
        return r;
      }
      case Tag::TryCatch: {
        // dst sits below regs_base on purpose: the unwinder drops the
        // region's temps, and the result register must not be one of them.
        const int32_t dst = alloc();
        const int32_t regs_base = top;
        const int32_t start = here();
        last_scope_defer_run_pc = -1;
        branch_into(dst, m.child(id, 0), n.pos);
        // A body that is a defer-declaring Scope runs those defers at its
        // fall-through exit; the guarded region ends before that run, so a
        // defer throwing there escapes its own catch (culebra's rule: a try
        // ends its region before the body's fall-through defer run).
        const int32_t body_end =
            (m.at(m.child(id, 0)).tag == Tag::Scope &&
             last_scope_defer_run_pc >= 0)
                ? last_scope_defer_run_pc
                : -1;
        const size_t jend = emit(Op::Jump, 0, 0, 0, n.pos);
        const int32_t handler_pc = here();
        // Otherwise the region ends where the handler starts: the jump over
        // it still belongs to the guarded range (a callee's resume pc can
        // point at it), while the handler must not be guarded by its own try.
        ch.cleanups.push_back({start, body_end >= 0 ? body_end : handler_pc,
                               0, 0, regs_base, handler_pc, n.a});
        branch_into(dst, m.child(id, 1), n.pos);
        patch(jend, here());
        return dst;
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
        open_loops.push_back({start, base, open_scopes.size(), {}});
        compile_stmt(v.body);
        emit(Op::Jump, start, 0, 0, n.pos);
        patch(jf, here());
        for (size_t j : open_loops.back().break_jumps) patch(j, here());
        open_loops.pop_back();
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

      case Tag::Return: {
        // Slots need no one-at-a-time release -- the Ret frees the whole
        // frame -- but pending defers of every open scope still run, after
        // the return value is computed, innermost first.
        int32_t r;
        if (n.num_children == 1) {
          r = compile_value(m.child(id, 0));
        } else {
          r = alloc();
          emit(Op::LoadNil, r, 0, 0, n.pos);
        }
        for (size_t i = open_scopes.size(); i > 0; --i) {
          if (open_scopes[i - 1].has_defers) {
            emit(Op::DeferRunTo, 0, 0, 0, n.pos);
          }
        }
        emit(Op::Ret, r, 1, 0, n.pos);
        break;
      }

      case Tag::Throw: {
        const int32_t r = compile_expr(m.child(id, 0));
        emit(Op::Throw, r, 0, 0, n.pos);
        break;
      }

      case Tag::Defer: {
        const int32_t r = compile_expr(m.child(id, 0));
        emit(Op::DeferPush, r, 0, 0, n.pos);
        break;
      }

      case Tag::Break:
      case Tag::Continue: {
        OpenLoop& loop = open_loops.back();  // verify(): inside a loop body
        leave_down_to(loop.scope_depth, loop.stmt_base, n.pos);
        if (n.tag == Tag::Continue) {
          emit(Op::Jump, loop.head, 0, 0, n.pos);
        } else {
          loop.break_jumps.push_back(emit(Op::Jump, 0, 0, 0, n.pos));
        }
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
