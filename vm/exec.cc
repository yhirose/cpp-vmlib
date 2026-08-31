#include "vm/exec.h"

#include <memory>
#include <string>
#include <vector>

#include "coreir/rt.h"
#include "coreir/semantics.h"

using namespace coreir;

namespace vm {
namespace {

// One activation record. A frame is a heap object owned by Exec's stack, not
// a C++ stack frame, so its address is stable for as long as it is live --
// which is what lets `captures` be raw pointers into the frame that declared
// each variable.
struct Frame {
  const Chunk* chunk = nullptr;
  size_t pc = 0;
  std::vector<Slot> locals;
  std::vector<Slot*> captures;
  std::vector<int64_t> regs;
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
    f->locals.resize(static_cast<size_t>(ch.num_locals));
    f->regs.resize(static_cast<size_t>(ch.num_regs));
    return f;
  }

  // The one place a call can fail before it starts. An explicit frame stack
  // cannot overflow the host's C++ stack the way a recursive executor can,
  // but an unbounded one would grow the heap until the allocator gave out --
  // no better a diagnostic than the crash it replaced. The bound keeps a
  // runaway call reported at its call site, the way a divide-by-zero is.
  void push(int32_t func, const std::vector<CaptureSrc>& cmap, Frame& caller,
            SrcPos pos) {
    if (frames.size() > max_frames) {
      coreir_rt::fail("recursion limit exceeded", pos.line, pos.col);
    }
    coreir_rt_poll();
    std::unique_ptr<Frame> f = make_frame(p.chunks[func]);
    f->captures.reserve(cmap.size());
    for (const CaptureSrc& src : cmap) {
      f->captures.push_back(src.from == VarKind::Local
                                ? &caller.locals[src.index]
                                : caller.captures[src.index]);
    }
    frames.push_back(std::move(f));
  }

  [[noreturn]] void fail(const Frame& f, const std::string& msg) {
    const SrcPos sp = p.positions[f.chunk->code_pos[f.pc]];
    coreir_rt::fail(msg, sp.line, sp.col);
  }

  Slot& slot_of(Frame& f, int32_t kind, int32_t index) {
    return static_cast<VarKind>(kind) == VarKind::Local ? f.locals[index]
                                                        : *f.captures[index];
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
          if (const char* trap = binop_trap(op, l, r)) fail(f, trap);
          f.regs[in.a] = apply_binop(op, l, r);
          break;
        }
        case Op::LoadVar: {
          Slot& s = slot_of(f, in.b, in.c);
          if (!s.inited) {
            const auto& names = static_cast<VarKind>(in.b) == VarKind::Local
                                    ? ch.local_names
                                    : ch.capture_names;
            fail(f, format_uninit_var(names[in.c]));
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
          // A backward (or self) jump is a loop iteration -- the one place a
          // program can spin without ever calling or producing output, so
          // it is also the one place a host needs to interrupt one.
          if (static_cast<size_t>(in.a) <= f.pc) coreir_rt_poll();
          f.pc = static_cast<size_t>(in.a);
          continue;
        case Op::JumpIfFalse:
          if (!truthy(f.regs[in.a])) {
            f.pc = static_cast<size_t>(in.b);
            continue;
          }
          break;
        case Op::Call: {
          const SrcPos pos = p.positions[ch.code_pos[f.pc]];
          // Advance before pushing: this is where the caller resumes, and a
          // callee that never returns never reads it.
          ++f.pc;
          push(in.a, p.capture_maps[in.b], f, pos);
          continue;
        }
        case Op::Out:
          coreir_rt_out(f.regs[in.a]);
          break;
        case Op::In: {
          const SrcPos sp = p.positions[ch.code_pos[f.pc]];
          f.regs[in.a] = coreir_rt_in(sp.line, sp.col);
          break;
        }
        case Op::Ret:
          frames.pop_back();
          if (frames.empty()) return;
          continue;
      }
      ++f.pc;
    }
  }
};

}  // namespace

void run(const Program& p, int max_call_depth) {
  Exec e{p, max_call_depth < 0 ? 0 : static_cast<size_t>(max_call_depth), {}};
  e.frames.push_back(e.make_frame(p.chunks[0]));
  e.run();
}

}  // namespace vm
