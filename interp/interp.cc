#include "interp/interp.h"

#include <string>
#include <vector>

#include "coreir/semantics.h"
#include "pl0rt.h"

using namespace coreir;

namespace interp {
namespace {

// Frames live on the C++ stack, one per activation. A capture is a raw pointer
// into an enclosing frame's locals: `locals` is a std::vector, so its heap
// buffer does not move when the Frame object does, and PL/0 has no upward
// funarg -- every frame a capture points into is still running. First-class
// functions are what would force these into heap cells (culebra's CellNew),
// and that change stays inside this file: VarRef{Capture, i} does not move.
struct Frame {
  std::vector<Slot> locals;
  std::vector<Slot*> captures;
};

struct Interp {
  const Module& m;

  Slot& slot_of(Frame& f, VarKind kind, int32_t index) {
    return kind == VarKind::Local ? f.locals[index] : *f.captures[index];
  }

  const std::string& var_name(const Func& fn, VarKind kind, int32_t index) {
    return kind == VarKind::Local ? fn.local_names[index]
                                  : fn.capture_names[index];
  }

  int64_t eval(NodeId id, Frame& f, const Func& fn) {
    const Node& n = m.at(id);
    switch (n.tag) {
      case Tag::Literal:
        return m.int_const(id);

      case Tag::VarRef: {
        auto v = view_varref(m, id);
        Slot& s = slot_of(f, v.kind, v.index);
        if (!s.inited) {
          const SrcPos p = m.pos_of(id);
          pl0rt::fail("uninitialized variable '" +
                          var_name(fn, v.kind, v.index) + "'",
                      p.line, p.col);
        }
        return s.value;
      }

      case Tag::Unary:
        return wrap_neg(eval(view_unary(m, id).operand, f, fn));

      case Tag::Binary: {
        auto v = view_binary(m, id);
        const int64_t l = eval(v.lhs, f, fn);
        const int64_t r = eval(v.rhs, f, fn);
        if (const char* trap = binop_trap(v.op, l, r)) {
          // The position of a failing division is its right operand, not the
          // operator -- pl0.cul reports `divide by zero` there.
          const SrcPos p = m.pos_of(v.rhs);
          pl0rt::fail(trap, p.line, p.col);
        }
        return apply_binop(v.op, l, r);
      }

      case Tag::Assign: {
        auto v = view_assign(m, id);
        const int64_t val = eval(v.value, f, fn);
        Slot& s = slot_of(f, v.kind, v.index);
        s.value = val;
        s.inited = 1;
        return 0;
      }

      case Tag::If: {
        auto v = view_if(m, id);
        if (truthy(eval(v.cond, f, fn))) return eval(v.then_, f, fn);
        if (v.els.valid()) return eval(v.els, f, fn);
        return 0;
      }

      case Tag::While: {
        auto v = view_while(m, id);
        while (truthy(eval(v.cond, f, fn))) eval(v.body, f, fn);
        return 0;
      }

      case Tag::Block: {
        int64_t last = 0;
        for (uint32_t i = 0; i < n.num_children; ++i) {
          last = eval(m.child(id, i), f, fn);
        }
        return last;
      }

      case Tag::Call: {
        auto v = view_call(m, id);
        return call(v.func, m.capture_maps[v.capture_map], f);
      }

      case Tag::Intrinsic: {
        auto v = view_intrinsic(m, id);
        switch (v.id) {
          case IntrinsicId::Print:
            pl0_rt_out(eval(m.child(id, 0), f, fn));
            return 0;
          case IntrinsicId::ReadInt: {
            const SrcPos p = m.pos_of(id);
            return pl0_rt_in(p.line, p.col);
          }
        }
        return 0;
      }
    }
    return 0;
  }

  int64_t call(int32_t func, const std::vector<CaptureSrc>& cmap,
               Frame& caller) {
    const Func& fn = m.funcs[func];
    Frame f;
    f.locals.resize(static_cast<size_t>(fn.num_locals));
    f.captures.reserve(cmap.size());
    // Each entry names where the callee's capture comes from *in this frame*.
    for (const CaptureSrc& src : cmap) {
      f.captures.push_back(src.from == VarKind::Local
                               ? &caller.locals[src.index]
                               : caller.captures[src.index]);
    }
    return eval(fn.body, f, fn);
  }
};

}  // namespace

void run(const Module& m) {
  Interp in{m};
  const Func& entry = m.funcs[0];
  Frame f;
  f.locals.resize(static_cast<size_t>(entry.num_locals));
  in.eval(entry.body, f, entry);
}

}  // namespace interp
