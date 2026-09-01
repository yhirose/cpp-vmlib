#include "vm/exec.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "coreir/rt.h"
#include "coreir/semantics.h"
#include "coreir/value.h"

using namespace coreir;

namespace vm {
namespace {

// What a Throw (or a trap the executor raises itself) travels as, from the
// raise to the unwinder in run(). Typed, so a host exception thrown out of a
// coreir_rt_* hook passes through untouched -- only the VM's own failures
// unwind to a script handler.
//
// `fatal_msg` is what coreir_rt_fail gets if no handler catches this: a
// trap keeps its original diagnostic (so an unguarded program fails with
// byte-identical output to the pre-exception executor), while a user Throw
// leaves it empty and is formatted as "uncaught: <value>" at that point --
// not eagerly, since a caught throw never needs it.
struct Raise {
  Value value;
  SrcPos pos;
  std::string fatal_msg;
};

// One activation record. A frame is a heap object owned by Exec's stack, not
// a C++ stack frame, so its address is stable for as long as it is live --
// which is what lets `captures` be raw pointers into the frame that declared
// each variable.
//
// `regs` and `locals` hold owned references, and nothing in this
// file places a retain or a release to make that work. Value is an RAII
// handle, so an ordinary assignment releases what a register held and retains
// what it now holds, and ~Frame releases the lot. That is also what keeps a
// host throw exception-safe for free: the unwind that destroys Exec's frame
// stack releases every value in every live frame, with no unwind table to
// get wrong.
struct Frame {
  const Chunk* chunk = nullptr;
  size_t pc = 0;
  std::vector<Value> locals;
  std::vector<Value> regs;
  // Cells this frame owns, and cells the closure being run brought with it.
  // Both are Cell values -- shared, refcounted, and not tied to any frame's
  // lifetime, which is what lets a closure be called after the frame that
  // built it has returned.
  std::vector<Value> cells;
  std::vector<Value> captures;
  // The frame's pending defers (owned closure values, LIFO) and the marks
  // its open defer-scopes took: {stack height, the DeferMark's pc}. The pc
  // is what lets the unwinder pair a mark with a Cleanup region, and skip
  // regions whose exit-time run already popped theirs.
  std::vector<Value> defers;
  std::vector<std::pair<size_t, int32_t>> defer_marks;
  int32_t ret_reg = -1;  // where in the caller the result goes
};

struct Exec {
  const Program& p;
  size_t max_frames;

  // unique_ptr rather than a vector<Frame> or a deque<Frame>: a vector moves
  // its elements as it grows, which would invalidate every `captures` pointer
  // aimed at them, and a deque owns its elements outright, which leaves no
  // way to hand one frame's ownership elsewhere -- what a call that can
  // suspend and be resumed later would need. The indirection buys both.
  std::vector<std::unique_ptr<Frame>> frames;

  std::unique_ptr<Frame> make_frame(const Chunk& ch) {
    auto f = std::make_unique<Frame>();
    f->chunk = &ch;
    f->locals.assign(static_cast<size_t>(ch.num_locals), Value::uninit());
    f->regs.resize(static_cast<size_t>(ch.num_regs));
    f->cells.resize(static_cast<size_t>(ch.num_cells));
    return f;
  }

  // A trap: the executor's own failure, catchable like any Throw. The
  // value a handler sees is an object {message, line, col} -- built here,
  // once, rather than each front end inventing its own materialization.
  [[noreturn]] void raise_trap(const std::string& msg, SrcPos pos) {
    Value e = Value::make_object();
    e.as_object()->set("message", Value::make_str(msg));
    e.as_object()->set("line", Value::make_int(pos.line));
    e.as_object()->set("col", Value::make_int(pos.col));
    throw Raise{std::move(e), pos, msg};
  }

  // Shared by both call forms: the depth bound and the interrupt point.
  void check_can_push(SrcPos pos) {
    if (frames.size() > max_frames) {
      raise_trap("recursion limit exceeded", pos);
    }
    coreir_rt_poll();
  }

  // Calling a closure -- the only way a frame is entered. The callee gets the
  // closure's cells, which are shared rather than pointed at, so nothing here
  // depends on the caller still being alive.
  void push_closure(const Value& callee, const Value* args, int32_t argc,
                    int32_t ret_reg, SrcPos pos) {
    if (!callee.is_func()) {
      raise_trap(std::string("cannot call ") + type_name(callee.tag()), pos);
    }
    const ClosureObj* c = callee.as_closure();
    const Chunk& ch = p.chunks[static_cast<size_t>(c->func)];
    if (argc != ch.num_params) {
      raise_trap(ch.name + " takes " + std::to_string(ch.num_params) +
                     " argument(s), given " + std::to_string(argc),
                 pos);
    }
    check_can_push(pos);
    std::unique_ptr<Frame> f = make_frame(ch);
    f->captures = c->cells;  // shared, not copied: each element is a Cell
    for (int32_t i = 0; i < argc; ++i) {
      f->locals[static_cast<size_t>(i)] = args[i];
    }
    f->ret_reg = ret_reg;
    frames.push_back(std::move(f));
  }

  [[noreturn]] void fail(const Frame& f, const std::string& msg) {
    raise_trap(msg, p.positions[f.chunk->code_pos[f.pc]]);
  }

  // Scalars cost nothing to rebuild; a string literal allocates on every
  // load, which the real version will not want. culebra's answer is that
  // constants are immortal and LoadConst does not refcount them at all
  // (vm.md 5.2). Deferred on purpose: an immortal object is a second lifetime
  // rule, and the whole point of the RAII Value is that there is one.
  Value const_value(int32_t index) const {
    const Const& c = p.consts[static_cast<size_t>(index)];
    switch (c.kind) {
      case ConstKind::Nil:
        return Value();
      case ConstKind::Bool:
        return Value::make_bool(c.bits != 0);
      case ConstKind::Int:
        return Value::make_int(c.bits);
      case ConstKind::Double: {
        double d;
        std::memcpy(&d, &c.bits, sizeof(double));
        return Value::make_double(d);
      }
      case ConstKind::Str:
        return Value::make_str(p.str_consts[static_cast<size_t>(c.bits)]);
    }
    return Value();
  }

  // Reading and writing a variable, in whichever of the three storage classes
  // it lives. A local starts out Uninit and a cell starts out nil, so "read
  // before assigned" is observable through the first and not the second --
  // which is right: a cell is created by the frame, not by the source-level
  // declaration a diagnostic would name.
  Value& var_ref(Frame& f, int32_t kind, int32_t index, const Chunk& ch) {
    switch (static_cast<VarKind>(kind)) {
      case VarKind::Local: {
        Value& v = f.locals[index];
        if (v.is_uninit()) fail(f, format_uninit_var(ch.local_names[index]));
        return v;
      }
      case VarKind::Capture: return f.captures[index].as_cell()->v;
      case VarKind::Cell:    return f.cells[index].as_cell()->v;
    }
    return f.regs[0];  // unreachable
  }

  void var_store(Frame& f, int32_t kind, int32_t index, const Value& v) {
    switch (static_cast<VarKind>(kind)) {
      case VarKind::Local:   f.locals[index] = v; break;
      case VarKind::Capture: f.captures[index].as_cell()->v = v; break;
      case VarKind::Cell:    f.cells[index].as_cell()->v = v; break;
    }
  }

  // The cells a MakeClosure hands to the closure it builds, resolved in the
  // frame doing the building. A Local is rejected by verify() -- it would die
  // with this frame -- so only these two cases exist.
  Value capture_cell(Frame& f, const CaptureSrc& src) {
    return src.from == VarKind::Cell ? f.cells[src.index]
                                     : f.captures[src.index];
  }

  void run() {
    while (!frames.empty()) {
      try {
        dispatch(0);
        return;
      } catch (Raise& r) {
        if (!unwind(r, 0)) {
          const std::string msg =
              r.fatal_msg.empty() ? "uncaught: " + to_display(r.value)
                                  : r.fatal_msg;
          coreir_rt::fail(msg, r.pos.line, r.pos.col);
        }
        // A handler took the value; dispatch resumes at its pc.
      }
    }
  }

  // Run one frame's pending defers back to `mark`, LIFO, each as a normal
  // 0-arity call driven to completion by a nested, floor-bounded dispatch.
  // The nesting recurses through the host stack once per defer *run* (not
  // per call -- calls inside the defer stay flat), so only pathological
  // defers-spawning-defers chains deepen it.
  //
  // A defer whose own throw is not handled within its frames aborts the
  // run: the remaining defers of the same mark are dropped unrun (their
  // values released), and the throw replaces whatever was unwinding --
  // culebra's rule, minus its quirk of skipping the aborting frame's own
  // remaining handlers.
  void run_defers_now(Frame& f, size_t mark, SrcPos pos) {
    while (f.defers.size() > mark) {
      Value d = std::move(f.defers.back());
      f.defers.pop_back();
      const size_t floor = frames.size();
      try {
        push_closure(d, nullptr, 0, -1, pos);
        while (frames.size() > floor) {
          try {
            dispatch(floor);
          } catch (Raise& r) {
            if (!unwind(r, floor)) throw;
          }
        }
      } catch (Raise&) {
        while (f.defers.size() > mark) f.defers.pop_back();
        throw;
      }
    }
  }

  // Walks frames top-down, and within each the cleanup regions holding its
  // pc innermost-out (the vector's own order -- children were recorded
  // first). Every region crossed is left the way its own exit would leave
  // it: temps above its register floor dropped, its scope locals back to
  // Uninit. A region with a handler ends the walk: the carried value lands
  // in the caught slot and the frame resumes there. Frames without one are
  // popped, their values released by ~Frame.
  bool unwind(Raise& r, size_t floor) {
    while (frames.size() > floor) {
      Frame& f = *frames.back();
      for (const Cleanup& cl : f.chunk->cleanups) {
        if (f.pc < static_cast<size_t>(cl.start_pc) ||
            f.pc >= static_cast<size_t>(cl.end_pc)) {
          continue;
        }
        // The region's pending defers run first (culebra's order: defers,
        // then the scope's own releases), and only while its mark is still
        // outstanding -- a throw out of the region's exit-time DeferRunTo
        // arrives here with the mark already popped, and must not run them
        // twice. A defer throwing *here* replaces the in-flight value and
        // the walk continues with it: the rest of this region still tears
        // down, and the enclosing regions' handlers stay eligible.
        if (cl.defer_mark_pc >= 0 && !f.defer_marks.empty() &&
            f.defer_marks.back().second == cl.defer_mark_pc) {
          const size_t mark = f.defer_marks.back().first;
          f.defer_marks.pop_back();
          try {
            run_defers_now(f, mark, r.pos);
          } catch (Raise& replacement) {
            r = std::move(replacement);
          }
        }
        for (size_t i = static_cast<size_t>(cl.regs_base); i < f.regs.size();
             ++i) {
          f.regs[i] = Value();
        }
        for (int32_t i = cl.first_local; i < cl.end_local; ++i) {
          f.locals[i] = Value::uninit();
        }
        if (cl.handler_pc >= 0) {
          f.locals[cl.caught_local] = r.value;
          f.pc = static_cast<size_t>(cl.handler_pc);
          return true;
        }
      }
      frames.pop_back();
    }
    return false;
  }

  void dispatch(size_t floor) {
    while (true) {
      Frame& f = *frames.back();
      const Chunk& ch = *f.chunk;

      // Every chunk the compiler emits ends in Ret; running off the end is
      // the same belt and braces the old loop condition was.
      if (f.pc >= ch.code.size()) {
        frames.pop_back();
        if (frames.size() <= floor) return;
        continue;
      }

      const Insn& in = ch.code[f.pc];
      switch (in.op) {
        case Op::LoadConst:
          f.regs[in.a] = const_value(in.b);
          break;
        case Op::Neg:
        case Op::BitNot: {
          const UnOp uop = in.op == Op::BitNot ? UnOp::BitNot : UnOp::Neg;
          const Value& v = f.regs[in.b];
          if (auto err = unop_error(uop, v); !err.empty()) fail(f, err);
          f.regs[in.a] = apply_unop(uop, v);
          break;
        }
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
        case Op::Eq:  case Op::Ne:  case Op::Lt:  case Op::Le:
        case Op::Gt:  case Op::Ge:
        case Op::BitAnd: case Op::BitOr: case Op::BitXor:
        case Op::Shl: case Op::Shr: {
          const BinOp op = binop_of(in.op);
          const Value& l = f.regs[in.b];
          const Value& r = f.regs[in.c];
          if (auto err = binop_error(op, l, r); !err.empty()) fail(f, err);
          f.regs[in.a] = apply_binop(op, l, r);
          break;
        }
        case Op::LoadVar:
          f.regs[in.a] = var_ref(f, in.b, in.c, ch);
          break;
        case Op::StoreVar:
          var_store(f, in.a, in.b, f.regs[in.c]);
          break;
        case Op::Jump:
          // A backward (or self) jump is a loop iteration -- the one place a
          // program can spin without ever calling or producing output, so
          // it is also the one place a host needs to interrupt one.
          if (static_cast<size_t>(in.a) <= f.pc) coreir_rt_poll();
          f.pc = static_cast<size_t>(in.a);
          continue;
        case Op::JumpIfFalse:
          if (!f.regs[in.a].truthy()) {
            f.pc = static_cast<size_t>(in.b);
            continue;
          }
          break;
        case Op::Out: {
          const Value& v = f.regs[in.a];
          // An Int keeps the dedicated integer entry point: it is the one
          // shape the host contract had before values were tagged, and PL/0
          // still goes through it byte for byte.
          if (v.is_int()) {
            coreir_rt_out(v.as_int());
          } else if (v.is_str()) {
            coreir_rt_out_str(v.as_str().data(),
                              static_cast<int64_t>(v.as_str().size()));
          } else {
            const std::string s = to_display(v);
            coreir_rt_out_str(s.data(), static_cast<int64_t>(s.size()));
          }
          break;
        }
        case Op::In: {
          const SrcPos sp = p.positions[ch.code_pos[f.pc]];
          f.regs[in.a] = Value::make_int(coreir_rt_in(sp.line, sp.col));
          break;
        }
        case Op::LoadNil:
          f.regs[in.a] = Value();
          break;
        case Op::Move:
          f.regs[in.a] = f.regs[in.b];
          break;
        case Op::ClearRegs:
          for (int32_t i = in.a; i < in.b; ++i) f.regs[i] = Value();
          break;
        case Op::ClearLocals:
          for (int32_t i = in.a; i < in.b; ++i) f.locals[i] = Value::uninit();
          break;
        case Op::NewArray: {
          std::vector<Value> items;
          items.reserve(static_cast<size_t>(in.c));
          for (int32_t i = 0; i < in.c; ++i) items.push_back(f.regs[in.b + i]);
          f.regs[in.a] = Value::make_array(std::move(items));
          break;
        }
        case Op::NewObject:
          f.regs[in.a] = Value::make_object();
          break;
        case Op::Index: {
          const Value& recv = f.regs[in.b];
          const Value& key = f.regs[in.c];
          if (auto err = index_error(recv, key); !err.empty()) fail(f, err);
          f.regs[in.a] = index_get(recv, key);
          break;
        }
        case Op::SetIndex: {
          const Value& recv = f.regs[in.a];
          const Value& key = f.regs[in.b];
          if (auto err = index_error(recv, key); !err.empty()) fail(f, err);
          index_set(recv, key, f.regs[in.c]);
          break;
        }
        case Op::Len: {
          const Value& v = f.regs[in.b];
          if (auto err = len_error(v); !err.empty()) fail(f, err);
          f.regs[in.a] = length_of(v);
          break;
        }
        case Op::ToStr:
          f.regs[in.a] = Value::make_str(to_display(f.regs[in.b]));
          break;
        case Op::CellNew:
          f.cells[in.a] = Value::make_cell();
          break;
        case Op::MakeClosure: {
          std::vector<Value> cells;
          const auto& cmap = p.capture_maps[static_cast<size_t>(in.c)];
          cells.reserve(cmap.size());
          for (const CaptureSrc& src : cmap) {
            cells.push_back(capture_cell(f, src));
          }
          f.regs[in.a] = Value::make_closure(in.b, std::move(cells));
          break;
        }
        case Op::CallValue: {
          const SrcPos pos = p.positions[ch.code_pos[f.pc]];
          // Copy the callee out first: the result register may be the callee's
          // own, and the frame this pushes outlives the read either way.
          // Advance before pushing -- this is where the caller resumes, and a
          // callee that never returns never reads it.
          const Value callee = f.regs[in.b];
          ++f.pc;
          push_closure(callee, f.regs.data() + in.c, in.d, in.a, pos);
          continue;
        }
        case Op::Throw: {
          const SrcPos sp = p.positions[ch.code_pos[f.pc]];
          throw Raise{f.regs[in.a], sp, {}};
        }
        case Op::DeferPush: {
          const Value& v = f.regs[in.a];
          if (!v.is_func()) {
            fail(f, std::string("defer needs a function, not ") +
                        type_name(v.tag()));
          }
          f.defers.push_back(v);
          break;
        }
        case Op::DeferMark:
          f.defer_marks.push_back(
              {f.defers.size(), static_cast<int32_t>(f.pc)});
          break;
        case Op::DeferRunTo: {
          const size_t mark = f.defer_marks.back().first;
          f.defer_marks.pop_back();
          const SrcPos sp = p.positions[ch.code_pos[f.pc]];
          // Advance first: the nested run pushes frames, and this is where
          // execution resumes when the last defer returns.
          ++f.pc;
          run_defers_now(f, mark, sp);
          continue;
        }
        case Op::Ret: {
          // Move the result into the caller before the frame goes: after the
          // pop, the register it lives in is gone.
          Value result;
          if (in.b != 0) result = f.regs[in.a];
          const int32_t ret_reg = f.ret_reg;
          frames.pop_back();
          if (frames.size() <= floor) {
            if (ret_reg >= 0 && !frames.empty()) {
              frames.back()->regs[ret_reg] = std::move(result);
            }
            return;
          }
          if (ret_reg >= 0) frames.back()->regs[ret_reg] = std::move(result);
          continue;
        }
      }
      ++f.pc;
    }
  }
};

}  // namespace

void run(const Program& p, Runtime& rt, int max_call_depth) {
  Runtime::Scope scope(rt);
  Exec e{p, max_call_depth < 0 ? 0 : static_cast<size_t>(max_call_depth), {}};
  e.frames.push_back(e.make_frame(p.chunks[0]));
  e.run();
}

void run(const Program& p, int max_call_depth) {
  Runtime rt;
  run(p, rt, max_call_depth);
}

}  // namespace vm
