#include "vm/exec.h"

#include <string>
#include <vector>

#include "coreir/rt.h"
#include "coreir/semantics.h"

using namespace coreir;

namespace vm {
namespace {

// A capture is a raw pointer into an enclosing frame's locals, sound because
// `locals` is sized once at frame creation and PL/0 has no upward funarg to
// outlive it.
struct Frame {
  std::vector<Slot> locals;
  std::vector<Slot*> captures;
  std::vector<int64_t> regs;
};

struct Exec {
  const Program& p;
  int max_depth;
  int depth = 0;

  // Every procedure call recurses through the host's own C++ stack (call() ->
  // run_chunk() -> call() -> ...), so nothing here can catch a runaway
  // recursion itself -- the host's process would simply overflow its stack,
  // silently, with no exception to catch. This counter turns that into the
  // same kind of reported failure as a divide-by-zero.
  //
  // An RAII guard, not a manual ++/--: coreir_rt_fail is documented to allow
  // a host to throw rather than exit, and a throw from deeper in run_chunk
  // (a divide-by-zero, say) must still leave `depth` correct for whichever
  // frame catches it and keeps running -- a bare `--depth` after the
  // recursive call would be skipped by that unwind.
  struct DepthGuard {
    int& depth;
    explicit DepthGuard(int& d) : depth(d) { ++depth; }
    ~DepthGuard() { --depth; }
  };

  void call(int32_t func, const std::vector<CaptureSrc>& cmap, Frame* caller,
            SrcPos pos) {
    DepthGuard guard(depth);
    if (depth > max_depth) {
      coreir_rt::fail("recursion limit exceeded", pos.line, pos.col);
    }
    coreir_rt_poll();
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
    coreir_rt::fail(msg, sp.line, sp.col);
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
            fail(ch, pc, format_uninit_var(names[in.c]));
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
          if (static_cast<size_t>(in.a) <= pc) coreir_rt_poll();
          pc = static_cast<size_t>(in.a);
          continue;
        case Op::JumpIfFalse:
          if (!truthy(f.regs[in.a])) {
            pc = static_cast<size_t>(in.b);
            continue;
          }
          break;
        case Op::Call:
          call(in.a, p.capture_maps[in.b], &f, p.positions[ch.code_pos[pc]]);
          break;
        case Op::Out:
          coreir_rt_out(f.regs[in.a]);
          break;
        case Op::In: {
          const SrcPos sp = p.positions[ch.code_pos[pc]];
          f.regs[in.a] = coreir_rt_in(sp.line, sp.col);
          break;
        }
        case Op::Ret:
          return;
      }
      ++pc;
    }
  }
};

}  // namespace

void run(const Program& p, int max_call_depth) {
  Exec e{p, max_call_depth};
  const Chunk& entry = p.chunks[0];
  Frame f;
  f.locals.resize(static_cast<size_t>(entry.num_locals));
  f.regs.resize(static_cast<size_t>(entry.num_regs));
  e.run_chunk(entry, f);
}

}  // namespace vm
