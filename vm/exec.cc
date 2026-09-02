#include "vm/exec.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
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
  // The owned-stack marks of the frame's open scopes: {Runtime::owned_mark
  // at entry, the OwnedMark's pc} -- the same pairing with Cleanup regions
  // as defer_marks.
  std::vector<std::pair<uint64_t, int32_t>> owned_marks;
  int32_t ret_reg = -1;  // where in the caller the result goes
  int32_t argc = 0;      // arguments the call supplied (Op::ArgCount)
  // The program's own frame -- vm::run's first -- as opposed to a job's,
  // which sits at the same stack depth after it: RunOptions::
  // entry_frame_drops is about this one only.
  bool entry = false;
  // Non-nil exactly when this frame is a generator activation: the
  // GeneratorObj it suspends back into. Owning, so the generator cannot be
  // freed out from under its own running frame.
  Value gen_self;
};

struct Exec {
  const Program& p;
  size_t max_frames;
  Runtime& rt;
  bool entry_frame_drops;  // RunOptions::entry_frame_drops

  // unique_ptr rather than a vector<Frame> or a deque<Frame>: a vector moves
  // its elements as it grows, which would invalidate every `captures` pointer
  // aimed at them, and a deque owns its elements outright, which leaves no
  // way to hand one frame's ownership elsewhere -- what a call that can
  // suspend and be resumed later would need. The indirection buys both.
  std::vector<std::unique_ptr<Frame>> frames;

  // The job queue (IntrinsicId::Enqueue): closures waiting to run after the
  // entry frame, FIFO. C++-side handles, so the collector sees them as
  // roots the way it sees a frame's registers.
  std::deque<Value> jobs;

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

  // The {value, done} object both generator intrinsics answer with.
  Value gen_result(Value v, bool done) {
    Value o = Value::make_object();
    o.as_object()->set("value", v);
    o.as_object()->set("done", Value::make_bool(done));
    return o;
  }

  // Move a frame's storage into a generator's keeping (suspend)...
  void park_frame(GenFrame& gf, Frame& f) {
    gf.locals = std::move(f.locals);
    gf.regs = std::move(f.regs);
    gf.cells = std::move(f.cells);
    gf.captures = std::move(f.captures);
    gf.defers = std::move(f.defers);
    gf.defer_marks = std::move(f.defer_marks);
    gf.owned_marks = std::move(f.owned_marks);
  }

  // ...and back onto the executor's stack (resume). The new frame owns the
  // generator for as long as it runs.
  std::unique_ptr<Frame> unpark_frame(const Value& gv, int32_t ret_reg) {
    GeneratorObj* go = gv.as_generator();
    GenFrame& gf = go->frame;
    auto f = std::make_unique<Frame>();
    f->chunk = &p.chunks[static_cast<size_t>(gf.func)];
    f->pc = static_cast<size_t>(gf.pc);
    f->locals = std::move(gf.locals);
    f->regs = std::move(gf.regs);
    f->cells = std::move(gf.cells);
    f->captures = std::move(gf.captures);
    f->defers = std::move(gf.defers);
    f->defer_marks = std::move(gf.defer_marks);
    f->owned_marks = std::move(gf.owned_marks);
    f->ret_reg = ret_reg;
    f->argc = gf.argc;
    f->gen_self = gv;
    go->state = GeneratorObj::State::Running;
    return f;
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
    if (argc != ch.num_params && !ch.lenient_arity) {
      raise_trap(ch.name + " takes " + std::to_string(ch.num_params) +
                     " argument(s), given " + std::to_string(argc),
                 pos);
    }
    // Under lenient arity the params window takes what it can: extras stay
    // with the caller, and a param nothing arrived for is nil rather than
    // Uninit, so the body can test it without tripping the read-before-
    // init check.
    const int32_t taken = std::min(argc, ch.num_params);
    // Calling a generator function runs none of it: the arguments and
    // captures are packaged into a Start-state activation and that is the
    // call's value. No frame, so no depth check.
    if (ch.is_generator) {
      Value g = Value::make_generator();
      GenFrame& gf = g.as_generator()->frame;
      gf.func = c->func;
      gf.argc = argc;
      gf.locals.assign(static_cast<size_t>(ch.num_locals), Value::uninit());
      for (int32_t i = 0; i < taken; ++i) {
        gf.locals[static_cast<size_t>(i)] = args[i];
      }
      for (int32_t i = taken; i < ch.num_params; ++i) {
        gf.locals[static_cast<size_t>(i)] = Value();
      }
      gf.regs.resize(static_cast<size_t>(ch.num_regs));
      gf.cells.resize(static_cast<size_t>(ch.num_cells));
      gf.captures = c->cells;
      if (ret_reg >= 0 && !frames.empty()) {
        frames.back()->regs[ret_reg] = std::move(g);
      }
      return;
    }
    check_can_push(pos);
    std::unique_ptr<Frame> f = make_frame(ch);
    f->captures = c->cells;  // shared, not copied: each element is a Cell
    for (int32_t i = 0; i < taken; ++i) {
      f->locals[static_cast<size_t>(i)] = args[i];
    }
    for (int32_t i = taken; i < ch.num_params; ++i) {
      f->locals[static_cast<size_t>(i)] = Value();
    }
    f->argc = argc;
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

  // The entry frame to its end, then the job queue: each job a fresh
  // 0-argument call driven to completion before the next is taken, so the
  // jobs it enqueues run after every one already waiting. A job whose call
  // itself traps (a parameter it cannot be given) fails the run the way an
  // uncaught throw does.
  void run() {
    drive();
    while (!jobs.empty()) {
      Value job = std::move(jobs.front());
      jobs.pop_front();
      try {
        push_closure(job, nullptr, 0, -1, SrcPos{0, 0});
      } catch (Raise& r) {
        report_uncaught(r);
        return;
      }
      drive();
    }
  }

  // Drive the frame stack to empty; an uncaught throw is the run's failure.
  void drive() {
    while (!frames.empty()) {
      try {
        dispatch(0);
        return;
      } catch (Raise& r) {
        if (!unwind(r, 0)) {
          report_uncaught(r);
          return;
        }
        // A handler took the value; dispatch resumes at its pc.
      }
    }
  }

  void report_uncaught(const Raise& r) {
    const std::string msg = r.fatal_msg.empty()
                                ? "uncaught: " + to_display(r.value)
                                : r.fatal_msg;
    coreir_rt::fail(msg, r.pos.line, r.pos.col);
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
        run_nested(floor);
      } catch (Raise&) {
        while (f.defers.size() > mark) f.defers.pop_back();
        throw;
      }
    }
  }

  // A scope's release, the two forms: its local range last-slot-first, or
  // the list it spelled out in list order. A released cell is replaced by
  // a fresh one -- what CellFresh does -- so the slot always holds a cell
  // and a closure that captured the old one keeps it, value and all.
  void release_range(Frame& f, int32_t first, int32_t end) {
    for (int32_t i = end; i-- > first;) f.locals[i] = Value::uninit();
  }
  void release_list(Frame& f, int32_t list) {
    for (const SlotRef& s : f.chunk->release_lists[static_cast<size_t>(list)]) {
      if (s.kind == VarKind::Local) {
        f.locals[s.index] = Value::uninit();
      } else {
        f.cells[s.index] = Value::make_cell();
      }
    }
  }

  // A Scope's exit, after its locals are gone: pop the mark it took and
  // resolve the owned stack above it (Runtime::owned_scope_exit). The entry
  // frame's outermost scope is the program's end, and under
  // entry_frame_drops == false resolves nothing: a cycle discarded at the
  // top level is the collector's, and at exit not even that -- the rule
  // pop_frame applies to the frame's own values.
  void leave_scope_owned(Frame& f) {
    if (f.owned_marks.empty()) return;  // every ClearLocals has its OwnedMark
    const uint64_t mark = f.owned_marks.back().first;
    f.owned_marks.pop_back();
    if (!entry_frame_drops && f.entry && f.owned_marks.empty()) return;
    rt.owned_scope_exit(mark);
  }

  // Drive whatever was just pushed to completion, unwinding within it.
  void run_nested(size_t floor) {
    while (frames.size() > floor) {
      try {
        dispatch(floor);
      } catch (Raise& r) {
        if (!unwind(r, floor)) throw;
      }
    }
  }

  // The drop-contract hook (see Runtime::set_drop_fn): run the object's
  // "\x01" "drop" closure with the object itself as the argument. A throwing
  // destructor is reported on stderr as "drop: <value>" (a trap: its own
  // message) and swallowed -- an object going away must not fail the
  // program that let go of it.
  static void drop_hook(void* ctx, HeapObj* h) {
    auto* self = static_cast<Exec*>(ctx);
    auto* o = static_cast<ObjectObj*>(h);
    Value* dv = o->find(kDropKey);
    if (!dv || !dv->is_func()) return;
    Value closure = *dv;
    Value arg = Value::make_ref(h);
    const size_t floor = self->frames.size();
    try {
      self->push_closure(closure, &arg, 1, -1, SrcPos{0, 0});
      self->run_nested(floor);
    } catch (Raise& r) {
      const std::string what =
          r.fatal_msg.empty() ? to_display(r.value) : r.fatal_msg;
      std::fprintf(stderr, "drop: %s\n", what.c_str());
      while (self->frames.size() > floor) self->frames.pop_back();
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
        if (cl.release_list >= 0) {
          release_list(f, cl.release_list);
        } else {
          release_range(f, cl.first_local, cl.end_local);
        }
        if (cl.owned_mark_pc >= 0 && !f.owned_marks.empty() &&
            f.owned_marks.back().second == cl.owned_mark_pc) {
          leave_scope_owned(f);
        }
        if (cl.handler_pc >= 0) {
          f.locals[cl.caught_local] = r.value;
          f.pc = static_cast<size_t>(cl.handler_pc);
          return true;
        }
      }
      // A generator frame unwound past is finished for good: the throw
      // reaches whoever resumed it, and every later resume answers done.
      if (f.gen_self.is_generator()) {
        f.gen_self.as_generator()->state = GeneratorObj::State::Done;
      }
      pop_frame();
    }
    return false;
  }

  // Releases the top frame: its locals last-declared-first -- the order a
  // scope's own exit uses, so a function's unscoped locals (its parameters,
  // say) go the same way -- and the rest with the Frame. Popping the entry
  // frame is the program's end, and under RunOptions::entry_frame_drops ==
  // false its values go with the drop hook disarmed; the jobs that may run
  // after it get the hook back.
  void pop_frame() {
    Frame& f = *frames.back();
    const bool bare = f.entry && !entry_frame_drops;
    if (bare) rt.set_drop_fn(nullptr, nullptr);
    for (size_t i = f.locals.size(); i-- > 0;) f.locals[i] = Value::uninit();
    frames.pop_back();
    if (bare) rt.set_drop_fn(this, &Exec::drop_hook);
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
        case Op::OutRaw: {
          const std::string s = to_display(f.regs[in.a]);
          coreir_rt_out_raw(s.data(), static_cast<int64_t>(s.size()));
          break;
        }
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
        case Op::ArgCount:
          f.regs[in.a] = Value::make_int(f.argc);
          break;
        case Op::Same: {
          const Value& l = f.regs[in.b];
          const Value& r = f.regs[in.c];
          f.regs[in.a] = Value::make_bool(l.tag() == r.tag() &&
                                          l.raw_data() == r.raw_data());
          break;
        }
        case Op::FnArity: {
          const Value& v = f.regs[in.b];
          if (!v.is_func()) {
            fail(f, std::string("cannot take the arity of ") +
                        type_name(v.tag()));
          }
          const size_t fi = static_cast<size_t>(v.as_closure()->func);
          f.regs[in.a] = Value::make_int(p.chunks[fi].num_params);
          break;
        }
        case Op::Collect:
          // Safe mid-instruction for the same reason a stress collect at
          // every allocation is: every register and local is a C++ handle,
          // and a handle is a root.
          f.regs[in.a] = Value::make_int(Runtime::current()->collect());
          break;
        case Op::HeapStats: {
          const Runtime& rt = *Runtime::current();
          const int64_t live = rt.live_objects();
          const int64_t bytes = rt.heap_bytes();
          Value o = Value::make_object();
          o.as_object()->set("live_objects", Value::make_int(live));
          o.as_object()->set("heap_bytes", Value::make_int(bytes));
          f.regs[in.a] = std::move(o);
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
          // Last declared, first released: a value whose release runs a
          // destructor sees the ones declared after it already gone, the
          // order a language with scoped destructors promises. Then what
          // the releases could not free -- the scope's cycles.
          release_range(f, in.a, in.b);
          leave_scope_owned(f);
          break;
        case Op::ReleaseSlots:
          release_list(f, in.a);
          leave_scope_owned(f);
          break;
        case Op::OwnedMark:
          f.owned_marks.push_back({rt.owned_mark(), static_cast<int32_t>(f.pc)});
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
          // Strings read through Index but do not write: they are immutable
          // values, not containers of cells.
          if (recv.is_str()) fail(f, "cannot assign into a string");
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
        case Op::TypeOf:
          f.regs[in.a] = Value::make_str(type_name(f.regs[in.b].tag()));
          break;
        case Op::ArrayPush: {
          const Value& a = f.regs[in.a];
          if (!a.is_array()) {
            fail(f, std::string("cannot push into ") + type_name(a.tag()));
          }
          a.as_array()->items.push_back(f.regs[in.b]);
          break;
        }
        case Op::ArrayPop: {
          const Value& a = f.regs[in.b];
          if (!a.is_array()) {
            fail(f, std::string("cannot pop from ") + type_name(a.tag()));
          }
          auto& items = a.as_array()->items;
          if (items.empty()) fail(f, "pop from an empty array");
          // Move out before shrinking: dst may alias the array register.
          Value out = std::move(items.back());
          items.pop_back();
          f.regs[in.a] = std::move(out);
          break;
        }
        case Op::ObjectHas: {
          const Value& o = f.regs[in.b];
          const Value& k = f.regs[in.c];
          if (!o.is_object() || !k.is_str()) {
            fail(f, std::string("cannot ask ") + type_name(o.tag()) +
                        " for a " + type_name(k.tag()) + " key");
          }
          f.regs[in.a] = Value::make_bool(o.as_object()->find(k.as_str()) !=
                                          nullptr);
          break;
        }
        case Op::ObjectKeys: {
          const Value& o = f.regs[in.b];
          if (!o.is_object()) {
            fail(f, std::string("cannot list keys of ") + type_name(o.tag()));
          }
          std::vector<Value> keys;
          keys.reserve(o.as_object()->props.size());
          for (const auto& kv : o.as_object()->props) {
            keys.push_back(Value::make_str(kv.first));
          }
          f.regs[in.a] = Value::make_array(std::move(keys));
          break;
        }
        case Op::ObjectRemove: {
          const Value& o = f.regs[in.a];
          const Value& k = f.regs[in.b];
          if (!o.is_object() || !k.is_str()) {
            fail(f, std::string("cannot remove a ") + type_name(k.tag()) +
                        " key from " + type_name(o.tag()));
          }
          o.as_object()->remove(k.as_str());
          break;
        }
        case Op::ToInt: {
          const Value& v = f.regs[in.b];
          if (v.is_int()) {
            f.regs[in.a] = v;
            break;
          }
          if (!v.is_number()) {
            fail(f, std::string("cannot convert ") + type_name(v.tag()) +
                        " to int");
          }
          const double d = v.as_double();
          // The comparison is in double, where int64's max rounds up to
          // 2^63 -- so >= on the top edge, > on the bottom.
          if (std::isnan(d) || d >= 9223372036854775808.0 ||
              d < -9223372036854775808.0) {
            fail(f, "double value out of int range");
          }
          f.regs[in.a] = Value::make_int(static_cast<int64_t>(d));
          break;
        }
        case Op::ToDouble: {
          const Value& v = f.regs[in.b];
          if (!v.is_number()) {
            fail(f, std::string("cannot convert ") + type_name(v.tag()) +
                        " to double");
          }
          f.regs[in.a] = Value::make_double(v.as_number());
          break;
        }
        case Op::FMod: {
          const Value& l = f.regs[in.b];
          const Value& r = f.regs[in.c];
          if (!l.is_number() || !r.is_number()) {
            fail(f, std::string("cannot fmod ") + type_name(l.tag()) +
                        " and " + type_name(r.tag()));
          }
          if (r.as_number() == 0.0) fail(f, "divide by zero");
          f.regs[in.a] =
              Value::make_double(std::fmod(l.as_number(), r.as_number()));
          break;
        }
        case Op::Pow: {
          const Value& l = f.regs[in.b];
          const Value& r = f.regs[in.c];
          if (!l.is_number() || !r.is_number()) {
            fail(f, std::string("cannot pow ") + type_name(l.tag()) +
                        " and " + type_name(r.tag()));
          }
          f.regs[in.a] =
              Value::make_double(std::pow(l.as_number(), r.as_number()));
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
        case Op::Throw: {
          const SrcPos sp = p.positions[ch.code_pos[f.pc]];
          throw Raise{f.regs[in.a], sp, {}};
        }
        case Op::Yield: {
          // Suspend: the frame's storage moves back into the generator, the
          // frame pops, and the resumer gets {value, done: false} -- the
          // same delivery shape as Ret, one level up.
          if (!f.gen_self.is_generator()) {
            fail(f, "yield outside a generator");  // verify() precludes this
          }
          Value self = std::move(f.gen_self);
          GeneratorObj* go = self.as_generator();
          Value out = f.regs[in.b];
          go->frame.pc = static_cast<int64_t>(f.pc) + 1;
          go->frame.yield_reg = in.a;
          park_frame(go->frame, f);
          go->state = GeneratorObj::State::Suspended;
          const int32_t ret_reg = f.ret_reg;
          frames.pop_back();
          Value result = gen_result(std::move(out), false);
          if (frames.size() <= floor) {
            if (ret_reg >= 0 && !frames.empty()) {
              frames.back()->regs[ret_reg] = std::move(result);
            }
            return;
          }
          if (ret_reg >= 0) frames.back()->regs[ret_reg] = std::move(result);
          continue;
        }
        case Op::GenResume: {
          const SrcPos pos = p.positions[ch.code_pos[f.pc]];
          const Value gv = f.regs[in.b];
          if (!gv.is_generator()) {
            fail(f, std::string("cannot resume ") + type_name(gv.tag()));
          }
          GeneratorObj* go = gv.as_generator();
          using St = GeneratorObj::State;
          if (go->state == St::Running) fail(f, "generator already running");
          if (go->state == St::Done) {
            f.regs[in.a] = gen_result(Value(), true);
            break;
          }
          const bool started = go->state == St::Suspended;
          const int32_t yield_reg = go->frame.yield_reg;
          check_can_push(pos);
          auto nf = unpark_frame(gv, in.a);
          // The sent value lands where the Yield's own result goes; a first
          // resume has no yield in flight and its argument is ignored.
          if (started && yield_reg >= 0) {
            nf->regs[static_cast<size_t>(yield_reg)] = f.regs[in.c];
          }
          ++f.pc;
          frames.push_back(std::move(nf));
          continue;
        }
        case Op::GenReturn: {
          const SrcPos pos = p.positions[ch.code_pos[f.pc]];
          const Value gv = f.regs[in.b];
          if (!gv.is_generator()) {
            fail(f, std::string("cannot close ") + type_name(gv.tag()));
          }
          GeneratorObj* go = gv.as_generator();
          using St = GeneratorObj::State;
          if (go->state == St::Running) fail(f, "generator already running");
          Value out = f.regs[in.c];
          if (go->state == St::Suspended) {
            // Close: restore the activation, run its pending defers --
            // innermost first, as the yield point's own Return would --
            // then discard the frame without executing any more of it. A
            // defer that throws propagates to this call; the generator is
            // done either way (the popped frame releases what it held).
            check_can_push(pos);
            frames.push_back(unpark_frame(gv, -1));
            Frame& gf = *frames.back();
            try {
              while (!gf.defer_marks.empty()) {
                const size_t mark = gf.defer_marks.back().first;
                gf.defer_marks.pop_back();
                run_defers_now(gf, mark, pos);
              }
            } catch (Raise&) {
              go->state = St::Done;
              frames.pop_back();
              throw;
            }
            go->state = St::Done;
            frames.pop_back();
          } else {
            // Start never ran (nothing registered, nothing to run); Done
            // holds nothing either. Both just settle the state and drop
            // whatever arguments a Start-state activation still packaged.
            go->state = St::Done;
            go->frame = GenFrame{};
          }
          f.regs[in.a] = gen_result(std::move(out), true);
          break;
        }
        case Op::GenThrow: {
          const SrcPos pos = p.positions[ch.code_pos[f.pc]];
          const Value gv = f.regs[in.b];
          if (!gv.is_generator()) {
            fail(f, std::string("cannot throw into ") + type_name(gv.tag()));
          }
          GeneratorObj* go = gv.as_generator();
          using St = GeneratorObj::State;
          if (go->state == St::Running) fail(f, "generator already running");
          Value v = f.regs[in.c];
          if (go->state != St::Suspended) {
            // No frame for it to land in: the generator is done (a Start
            // activation drops what it packaged) and the throw is this
            // instruction's own, at its own pc like a Throw.
            go->state = St::Done;
            go->frame = GenFrame{};
            throw Raise{std::move(v), pos, {}};
          }
          // Re-enter at the Yield itself rather than after it, so the
          // regions enclosing the yield -- its handlers, its defers -- are
          // the ones the throw crosses; the resumer's pc advances as for a
          // call, which is where the throw lands if the body lets it out.
          check_can_push(pos);
          const size_t at = static_cast<size_t>(go->frame.pc) - 1;
          auto nf = unpark_frame(gv, in.a);
          nf->pc = at;
          const SrcPos yp = p.positions[nf->chunk->code_pos[at]];
          ++f.pc;
          frames.push_back(std::move(nf));
          throw Raise{std::move(v), yp, {}};
        }
        case Op::Enqueue: {
          const Value& v = f.regs[in.a];
          if (!v.is_func()) {
            fail(f, std::string("enqueue needs a function, not ") +
                        type_name(v.tag()));
          }
          jobs.push_back(v);
          break;
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
          // A generator body's return finishes the generator; its resume
          // caller sees {value, done: true} where a plain call would see
          // the bare value.
          if (f.gen_self.is_generator()) {
            f.gen_self.as_generator()->state = GeneratorObj::State::Done;
            result = gen_result(std::move(result), true);
          }
          const int32_t ret_reg = f.ret_reg;
          pop_frame();
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

void run(const Program& p, Runtime& rt, const RunOptions& opts) {
  Runtime::Scope scope(rt);
  const int depth = opts.max_call_depth;
  Exec e{p, depth < 0 ? 0 : static_cast<size_t>(depth), rt,
         opts.entry_frame_drops, {}};
  rt.set_drop_fn(&e, &Exec::drop_hook);
  e.frames.push_back(e.make_frame(p.chunks[0]));
  e.frames.back()->entry = true;
  try {
    e.run();
  } catch (...) {
    rt.set_drop_fn(nullptr, nullptr);
    throw;
  }
  rt.set_drop_fn(nullptr, nullptr);
}

void run(const Program& p, Runtime& rt, int max_call_depth) {
  RunOptions opts;
  opts.max_call_depth = max_call_depth;
  run(p, rt, opts);
}

void run(const Program& p, int max_call_depth) {
  Runtime rt;
  run(p, rt, max_call_depth);
}

}  // namespace vm
