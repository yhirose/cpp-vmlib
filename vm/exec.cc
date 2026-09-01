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

  // Shared by both call forms: the depth bound and the interrupt point.
  void check_can_push(SrcPos pos) {
    if (frames.size() > max_frames) {
      coreir_rt::fail("recursion limit exceeded", pos.line, pos.col);
    }
    coreir_rt_poll();
  }

  // Calling a closure -- the only way a frame is entered. The callee gets the
  // closure's cells, which are shared rather than pointed at, so nothing here
  // depends on the caller still being alive.
  void push_closure(const Value& callee, const Value* args, int32_t argc,
                    int32_t ret_reg, SrcPos pos) {
    if (!callee.is_func()) {
      coreir_rt::fail(std::string("cannot call ") + type_name(callee.tag()),
                      pos.line, pos.col);
    }
    const ClosureObj* c = callee.as_closure();
    const Chunk& ch = p.chunks[static_cast<size_t>(c->func)];
    if (argc != ch.num_params) {
      coreir_rt::fail(ch.name + " takes " + std::to_string(ch.num_params) +
                          " argument(s), given " + std::to_string(argc),
                      pos.line, pos.col);
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
    const SrcPos sp = p.positions[f.chunk->code_pos[f.pc]];
    coreir_rt::fail(msg, sp.line, sp.col);
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
    while (true) {
      Frame& f = *frames.back();
      const Chunk& ch = *f.chunk;

      // Every chunk the compiler emits ends in Ret; running off the end is
      // the same belt and braces the old loop condition was.
      if (f.pc >= ch.code.size()) {
        frames.pop_back();
        if (frames.empty()) return;
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
        case Op::Ret: {
          // Move the result into the caller before the frame goes: after the
          // pop, the register it lives in is gone.
          Value result;
          if (in.b != 0) result = f.regs[in.a];
          const int32_t ret_reg = f.ret_reg;
          frames.pop_back();
          if (frames.empty()) return;
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
