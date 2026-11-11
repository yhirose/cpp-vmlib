// The profiling run (vm::run's Profile overload): per function, the calls
// that entered it, the instructions its own frames dispatched (self) and
// those dispatched while it was anywhere on the stack (total) -- counted
// across every way a frame comes onto the executor's stack and leaves it.
// What this pins, a small program each: a recursion, counted once in its
// total, with a helper and a host function that calls back into the
// program; tail calls, one to another function and three to itself, each
// a call, and one whose exit runs a destructor, in the caller's time; a
// generator, whose total stops at each yield and resumes with it; a
// coroutine whose two frames park and come back together; the job queue,
// which runs after the entry frame has gone; a destructor and a defer,
// which run inside the frame whose exit runs them; and a throw that
// unwinds two frames; and a Return of an If, which leaves from each arm.
// Then the table vm::to_string makes of a profile, word for word.
//
// The counts are exact: an instruction count does not move with the
// machine, which is the point of counting them. They are also the
// compiler's -- a change to what it emits for these programs moves them,
// and a failure prints the profile and the bytecode it counted, to re-pin
// from.

// The profiler asserts that every function has left the stack as often as
// it came by the end of a run (~Profiler); this test is where that is
// checked, whatever the build type.
#undef NDEBUG

#include <algorithm>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "vmlib.h"

namespace {

struct Failure : std::runtime_error {
  Failure(std::string msg) : std::runtime_error(std::move(msg)) {}
};

std::vector<std::string> g_out;
int g_failures = 0;

void fail(const std::string& what, const std::string& detail) {
  std::fprintf(stderr, "FAIL: %s\n%s", what.c_str(), detail.c_str());
  ++g_failures;
}

std::string joined() {
  std::string s;
  for (const auto& line : g_out) s += line + "|";
  return s;
}

using coreir::NativeCall;

// twice(f, x): f(x) and then f(x) again, through NativeCall::call -- a host
// function with program code running under it.
bool native_twice(NativeCall& c) {
  c.call(c.arg(0), &c.args[1], 1);
  c.result = c.call(c.arg(0), &c.args[1], 1);
  return true;
}

// One function's counts, as a case expects them.
struct Want {
  const char* name;
  uint64_t calls, self, total;
};

struct Profiled {
  vm::Program program;
  vm::Profile profile;
};

// Runs `m` profiled, checks its output, and returns the program and its
// profile.
Profiled run_profiled(const coreir::Module& m, const std::string& what,
                      const std::string& want_out) {
  Profiled r;
  g_out.clear();
  if (auto err = coreir::verify(m)) {
    fail(what + ": malformed IR", *err + "\n");
    return r;
  }
  r.program = vm::compile(m);
  coreir::Runtime rt;
  vm::RunOptions opts;
  opts.natives = {{"twice", 2, &native_twice, nullptr}};
  try {
    vm::run(r.program, rt, opts, r.profile);
  } catch (const Failure& e) {
    fail(what + ": run failed", std::string(e.what()) + "\n");
  }
  if (joined() != want_out) {
    fail(what + ": output",
         "  want [" + want_out + "]\n  got  [" + joined() + "]\n");
  }
  return r;
}

// Every function the case names has exactly its counts and every other one
// none, and the run dispatched `instructions` in all: as the selves add up,
// which are settled whenever the top of the stack changes, and as the
// opcodes count them, one at each instruction.
void expect(const Profiled& r, const std::string& what, uint64_t instructions,
            const std::vector<Want>& want) {
  const vm::Program& p = r.program;
  const vm::Profile& got = r.profile;
  uint64_t ops = 0;
  for (const uint64_t n : got.ops) ops += n;
  bool ok = got.instructions() == instructions && ops == instructions &&
            got.chunks.size() == p.chunks.size();
  for (size_t i = 0; ok && i < p.chunks.size(); ++i) {
    vm::Profile::Counts w;
    for (const Want& x : want) {
      if (p.chunks[i].name == x.name) w = {x.calls, x.self, x.total};
    }
    const vm::Profile::Counts& c = got.chunks[i];
    ok = c.calls == w.calls && c.self == w.self && c.total == w.total;
  }
  if (!ok) {
    fail(what + ": profile", vm::to_string(p, got) + "\n" + vm::to_string(p));
  }
}

// What an intrinsic's name, arity, destination and opcode say once lowered.
// Each id is compiled in value position with literal arguments, and the
// instruction compile_intrinsic emitted is read back through the dump: the
// destination (when there is one) and then the arguments, which is the
// operand list the dump reads off arity + has_dst; the fields past them
// zero. (That every id has an opcode of its own is a static_assert.)
void check_intrinsic_layout(coreir::IntrinsicId id) {
  using namespace coreir;
  const char* name = name_of(id);
  const SrcPos p{1, 1};
  const uint32_t arity = intrinsic_arity(id);
  const bool dst = vm::intrinsic_has_dst(id);
  Module m;
  Builder b(m);
  m.capture_maps.push_back({});
  std::vector<NodeId> args;
  for (uint32_t i = 0; i < arity; ++i) args.push_back(b.literal(i + 1, p));
  m.funcs.push_back(
      {"main", 1, 0,
       b.block({b.assign(VarKind::Local, 0, b.intrinsic(id, args, p), p)}, p),
       {"t"}, {}});
  if (auto err = verify(m)) return fail(name, "malformed IR: " + *err + "\n");
  const vm::Program prog = vm::compile(m);
  const vm::Insn* in = nullptr;
  for (const vm::Insn& x : prog.chunks[0].code) {
    if (x.op == vm::op_of(id)) in = &x;
  }
  if (in == nullptr) return fail(name, "its opcode was not emitted\n");
  // The registers the layout names: a destination, then arguments, in the
  // fields from a; the argument registers are the ones the arguments were
  // compiled into, from the destination's own, which is released first.
  std::vector<int32_t> want;
  if (dst) want.push_back(in->a);
  for (uint32_t i = 0; i < arity; ++i) want.push_back(in->a + i);
  want.resize(4, 0);
  const int32_t got[4] = {in->a, in->b, in->c, in->d};
  for (int f = 0; f < 4; ++f) {
    if (got[f] != want[f]) {
      fail(name, "operand field " + std::to_string(f) + " is r" +
                     std::to_string(got[f]) + ", want r" +
                     std::to_string(want[f]) + "\n");
    }
  }
  std::string line = std::string("\t") + vm::detail::name_of(in->op);
  for (uint32_t k = 0; k < arity + (dst ? 1u : 0u); ++k) {
    line += (k ? ", r" : " r") + std::to_string(want[k]);
  }
  line += "\t;";
  if (vm::to_string(prog).find(line) == std::string::npos) {
    fail(name, "the dump has no line " + line + "\n");
  }
}

// One Insn per opcode through the dump: every opcode has a name of its own
// and a format that reads nothing outside the program it was given.
void check_every_opcode_dumps() {
  std::map<std::string, int> seen;
  vm::Program prog;
  prog.consts.push_back(coreir::Const{});
  prog.natives.push_back("native");
  prog.positions.push_back(coreir::SrcPos{1, 1});
  vm::Chunk ch;
  ch.name = "all";
  ch.switch_tables.emplace_back();
  for (int i = 0; i <= static_cast<int>(vm::kLastOp); ++i) {
    vm::Insn in;
    in.op = static_cast<vm::Op>(i);
    const std::string name = vm::detail::name_of(in.op);
    if (name == "?") {
      fail("opcode names", "opcode " + std::to_string(i) + " has none\n");
    }
    if (++seen[name] > 1) fail("opcode names", name + " is shared\n");
    ch.code.push_back(in);
    ch.code_pos.push_back(0);
  }
  // kLastOp is the last: the opcode past it is none.
  if (std::string(vm::detail::name_of(static_cast<vm::Op>(
          static_cast<int>(vm::kLastOp) + 1))) != "?") {
    fail("kLastOp", "an opcode follows it\n");
  }
  prog.chunks.push_back(std::move(ch));
  const std::string text = vm::to_string(prog);
  for (const auto& [name, n] : seen) {
    if (text.find("\t" + name) == std::string::npos) {
      fail("opcode dump", "never printed " + name + "\n");
    }
  }
}

}  // namespace

extern "C" {
void coreir_rt_out(int64_t v) { g_out.push_back(std::to_string(v)); }
void coreir_rt_out_str(const char* bytes, int64_t len) {
  g_out.emplace_back(bytes, static_cast<size_t>(len));
}
void coreir_rt_out_raw(const char* bytes, int64_t len) {
  g_out.emplace_back(bytes, static_cast<size_t>(len));
}
int64_t coreir_rt_in(int64_t, int64_t) { return 0; }
void coreir_rt_poll(void) {}
[[noreturn]] void coreir_rt_fail(const char* msg, int64_t, int64_t) {
  throw Failure(msg);
}
}

int main() {
  using namespace coreir;
  using I = IntrinsicId;
  const SrcPos p{1, 1};
  const SrcPos none{};  // a helper's: code with no source position

  auto loc = [&p](Builder& b, int32_t i) {
    return b.varref(VarKind::Local, i, p);
  };
  auto cap = [&p](Builder& b, int32_t i) {
    return b.varref(VarKind::Capture, i, p);
  };
  auto lit = [&p](Builder& b, int64_t v) { return b.literal(v, p); };
  auto pr = [&p](Builder& b, NodeId v) {
    return b.intrinsic(I::Print, {v}, p);
  };
  auto say = [&](Builder& b, const char* s) {
    return pr(b, b.str_literal(s, p));
  };
  auto field = [&p](Builder& b, NodeId o, const char* k) {
    return b.index(o, b.str_literal(k, p), p);
  };

  // --- 1. Calls: a recursion, a helper, a host function calling back. ----
  // main { print(fact(3)); print(twice(inc, 5)) }
  // fact(n) { if n == 0 { return 1 } return $mul(n, fact(n - 1)) }
  // $mul(a, b) { return a * b }   inc(x) { return x + 1 }
  // fact's total is the whole recursion once, not once per level; inc runs
  // under twice, which is no frame.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.capture_maps.push_back({{VarKind::Cell, 0}});
    m.funcs.push_back({});
    // captures: 0 = fact
    m.funcs.push_back(
        {"fact", 1, 1,
         b.block({b.make_if(b.binary(BinOp::Eq, loc(b, 0), lit(b, 0), p),
                            b.make_return(lit(b, 1), p), NodeId{}, p),
                  b.make_return(
                      b.call_value(
                          b.make_closure(2, 0, p),
                          {loc(b, 0),
                           b.call_value(cap(b, 0),
                                        {b.binary(BinOp::Sub, loc(b, 0),
                                                  lit(b, 1), p)},
                                        p)},
                          p),
                      p)},
                 p),
         {"n"}, {"fact"}});
    m.funcs.back().num_params = 1;
    m.funcs.push_back(
        {"$mul", 2, 0,
         b.make_return(b.binary(BinOp::Mul, b.varref(VarKind::Local, 0, none),
                                b.varref(VarKind::Local, 1, none), none),
                       none),
         {"a", "b"}, {}});
    m.funcs.back().num_params = 2;
    m.funcs.back().singleton = true;
    m.funcs.push_back(
        {"inc", 1, 0,
         b.make_return(b.binary(BinOp::Add, loc(b, 0), lit(b, 1), p), p),
         {"x"}, {}});
    m.funcs.back().num_params = 1;
    m.funcs[0] = {
        "main", 0, 0,
        b.block({b.assign(VarKind::Cell, 0, b.make_closure(1, 1, p), p),
                 pr(b, b.call_value(b.varref(VarKind::Cell, 0, p),
                                    {lit(b, 3)}, p)),
                 pr(b, b.call_value(
                           b.native_ref(b.declare_native("twice"), p),
                           {b.make_closure(3, 0, p), lit(b, 5)}, p)),
                 // fact's closure holds the cell it is in.
                 b.assign(VarKind::Cell, 0, b.nil_literal(p), p)},
                p),
        {}, {}};
    m.funcs[0].num_cells = 1;
    const Profiled r = run_profiled(m, "calls", "6|6|");
    expect(r, "calls", 97,
           {{"main", 1, 20, 97},
            {"fact", 4, 52, 67},
            {"$mul", 3, 15, 15},
            {"inc", 2, 10, 10}});
    if (r.profile.natives != std::map<std::string, uint64_t>{{"twice", 1}}) {
      fail("calls: natives", vm::to_string(r.program, r.profile));
    }
    // The table: most self instructions first, a function with a source
    // position labelled by it and a helper by its name alone, the host
    // functions' calls, then the opcodes. Which opcodes, and how many of
    // each, is what the compiler emitted and moves with it: their sum is
    // pinned above, and here only that each one that ran has its row, and
    // the two that count this program's calls and returns.
    const std::string table = vm::to_string(r.program, r.profile);
    const std::string want =
        "97 instructions, 11 calls\n"
        "\n"
        " self   self%  total  total%  calls  per call  function\n"
        "   52   53.6%     67   69.1%      4      13.0  fact 1:1\n"
        "   20   20.6%     97  100.0%      1      20.0  main 1:1\n"
        "   15   15.5%     15   15.5%      3       5.0  $mul\n"
        "   10   10.3%     10   10.3%      2       5.0  inc 1:1\n"
        "\n"
        "calls  native\n"
        "    1  twice\n"
        "\n"
        "count   share  opcode\n";
    size_t ran = 0;
    for (const uint64_t n : r.profile.ops) ran += n != 0;
    const size_t rows =
        table.size() < want.size()
            ? 0
            : static_cast<size_t>(std::count(table.begin() + want.size(),
                                              table.end(), '\n'));
    if (table.compare(0, want.size(), want) != 0 || rows != ran) {
      fail("calls: table", "  want\n" + want + "  got\n" + table);
    }
    const auto ops = [&](vm::Op op) {
      return r.profile.ops[static_cast<uint8_t>(op)];
    };
    if (ops(vm::Op::CallValue) != 8 || ops(vm::Op::Ret) != 10) {
      fail("calls: opcodes", table);
    }
  }

  // --- 2. Tail calls: each is a call, and each moves the total along. ----
  // main { print(bounce(3)) }
  // bounce(n) { return loop(n, 0) }      -- in tail position: replaces
  // loop(n, acc) { if n == 0 { return acc } return loop(n - 1, acc + n) }
  // One frame does all of it: bounce's total ends where loop's begins.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({{VarKind::Cell, 0}});
    m.funcs.push_back({});
    // captures: 0 = loop
    m.funcs.push_back(
        {"bounce", 1, 1,
         b.make_return(b.call_value(cap(b, 0), {loc(b, 0), lit(b, 0)}, p), p),
         {"n"}, {"loop"}});
    m.funcs.back().num_params = 1;
    m.funcs.back().tail_calls = true;
    m.funcs.push_back(
        {"loop", 2, 1,
         b.block({b.make_if(b.binary(BinOp::Eq, loc(b, 0), lit(b, 0), p),
                            b.make_return(loc(b, 1), p), NodeId{}, p),
                  b.make_return(
                      b.call_value(
                          cap(b, 0),
                          {b.binary(BinOp::Sub, loc(b, 0), lit(b, 1), p),
                           b.binary(BinOp::Add, loc(b, 1), loc(b, 0), p)},
                          p),
                      p)},
                 p),
         {"n", "acc"}, {"loop"}});
    m.funcs.back().num_params = 2;
    m.funcs.back().tail_calls = true;
    m.funcs[0] = {
        "main", 0, 0,
        b.block({b.assign(VarKind::Cell, 0, b.make_closure(2, 0, p), p),
                 pr(b, b.call_value(b.make_closure(1, 0, p), {lit(b, 3)}, p)),
                 b.assign(VarKind::Cell, 0, b.nil_literal(p), p)},
                p),
        {}, {}};
    m.funcs[0].num_cells = 1;
    const Profiled r = run_profiled(m, "tail calls", "6|");
    expect(r, "tail calls", 64,
           {{"main", 1, 14, 64}, {"bounce", 1, 4, 4}, {"loop", 4, 46, 46}});
  }

  // --- 3. A tail call's exit runs a destructor before the callee starts. -
  // main { print(f(1)) }   g(n) { return n + 1 }   drop(self) { say "drop" }
  // f(n) { c = dropset({}, drop); return g(n) }   -- c a cell, the call a
  // tail call. The object goes with f's cells as the callee's replace
  // them: drop runs in f's time on the stack, not g's.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    const NodeId obj =
        b.intrinsic(IntrinsicId::DropSet,
                    {b.object_lit({}, p), b.make_closure(3, 0, p)}, p);
    m.funcs.push_back(
        {"f", 1, 0,
         b.block({b.assign(VarKind::Cell, 0, obj, p),
                  b.make_return(b.call_value(b.make_closure(2, 0, p),
                                             {loc(b, 0)}, p),
                                p)},
                 p),
         {"n"}, {}});
    m.funcs.back().num_params = 1;
    m.funcs.back().num_cells = 1;
    m.funcs.back().tail_calls = true;
    m.funcs.push_back(
        {"g", 1, 0,
         b.make_return(b.binary(BinOp::Add, loc(b, 0), lit(b, 1), p), p),
         {"n"}, {}});
    m.funcs.back().num_params = 1;
    m.funcs.push_back({"drop", 1, 0, say(b, "drop"), {"self"}, {}});
    m.funcs.back().num_params = 1;
    m.funcs[0] = {
        "main", 0, 0,
        pr(b, b.call_value(b.make_closure(1, 0, p), {lit(b, 1)}, p)), {}, {}};
    const Profiled r = run_profiled(m, "tail call exit", "drop|2|");
    expect(r, "tail call exit", 26,
           {{"main", 1, 7, 26}, {"f", 1, 9, 14}, {"g", 1, 5, 5},
            {"drop", 1, 5, 5}});
  }

  // --- 4. A generator: its total runs only while it does. ---------------
  // gen() { yield 1; yield 2; return 3 }
  // main { g = gen(); print(resume(g).value) three times }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back({"gen", 0, 0,
                       b.block({b.make_yield(lit(b, 1), p),
                                b.make_yield(lit(b, 2), p),
                                b.make_return(lit(b, 3), p)},
                               p),
                       {}, {}});
    m.funcs.back().is_generator = true;
    auto next = [&](Builder& bb) {
      return pr(bb, field(bb, bb.intrinsic(I::GenResume,
                                           {loc(bb, 0), bb.nil_literal(p)}, p),
                          "value"));
    };
    const NodeId make = b.call_value(b.make_closure(1, 0, p), {}, p);
    m.funcs[0] = {"main", 1, 0,
                  b.block({b.assign(VarKind::Local, 0, make, p), next(b),
                           next(b), next(b)},
                          p),
                  {"g"}, {}};
    const Profiled r = run_profiled(m, "generator", "1|2|3|");
    expect(r, "generator", 35, {{"main", 1, 27, 35}, {"gen", 1, 8, 8}});
  }

  // --- 5. A coroutine: two frames park at a yield and come back. ---------
  // body(x) { print(h()); return 0 }   h() { return yield 7 }
  // main { co = coroutine(body); print(resume(co, nil).value);
  //        print(resume(co, 5).done) }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back(
        {"body", 1, 0,
         b.block({pr(b, b.call_value(b.make_closure(2, 0, p), {}, p)),
                  b.make_return(lit(b, 0), p)},
                 p),
         {"x"}, {}});
    m.funcs.back().num_params = 1;
    m.funcs.push_back(
        {"h", 0, 0,
         b.make_return(b.intrinsic(I::CoroYield, {lit(b, 7)}, p), p), {}, {}});
    auto resume = [&](Builder& bb, NodeId v, const char* k) {
      return pr(bb, field(bb, bb.intrinsic(I::CoroResume, {loc(bb, 0), v}, p),
                          k));
    };
    m.funcs[0] = {
        "main", 1, 0,
        b.block({b.assign(VarKind::Local, 0,
                          b.intrinsic(I::CoroCreate,
                                      {b.make_closure(1, 0, p)}, p),
                          p),
                 resume(b, b.nil_literal(p), "value"),
                 resume(b, lit(b, 5), "done")},
                p),
        {"co"}, {}};
    const Profiled r = run_profiled(m, "coroutine", "7|5|true|");
    expect(r, "coroutine", 29,
           {{"main", 1, 20, 29}, {"body", 1, 6, 9}, {"h", 1, 3, 3}});
  }

  // --- 6. The job queue: jobs run after the entry frame has returned. ----
  // main { enqueue(job); enqueue(job); say "main" }   job() { say "job" }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back({"job", 0, 0, say(b, "job"), {}, {}});
    auto enqueue = [&](Builder& bb) {
      return bb.intrinsic(I::Enqueue, {bb.make_closure(1, 0, p)}, p);
    };
    m.funcs[0] = {"main", 0, 0,
                  b.block({enqueue(b), enqueue(b), say(b, "main")}, p), {},
                  {}};
    const Profiled r = run_profiled(m, "jobs", "main|job|job|");
    expect(r, "jobs", 23, {{"main", 1, 13, 13}, {"job", 2, 10, 10}});
  }

  // --- 7. A destructor and a defer, inside the frame that runs them. -----
  // main { scope { o = dropset({}, drop) } ; f(); say "end" }
  // drop(self) { say "drop" }
  // f() { scope { defer d; say "f" } }   d() { say "defer" }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back({"drop", 1, 0, say(b, "drop"), {"self"}, {}});
    m.funcs.back().num_params = 1;
    m.funcs.push_back(
        {"f", 0, 0,
         b.scope(0, 0,
                 b.block({b.make_defer(b.make_closure(3, 0, p), p),
                          say(b, "f")},
                         p),
                 p),
         {}, {}});
    m.funcs.push_back({"d", 0, 0, say(b, "defer"), {}, {}});
    const NodeId obj =
        b.intrinsic(IntrinsicId::DropSet,
                    {b.object_lit({}, p), b.make_closure(1, 0, p)}, p);
    m.funcs[0] = {
        "main", 1, 0,
        b.block({b.scope(0, 1, b.assign(VarKind::Local, 0, obj, p), p),
                 b.call_value(b.make_closure(2, 0, p), {}, p), say(b, "end")},
                p),
        {"o"}, {}};
    const Profiled r = run_profiled(m, "drop and defer", "drop|f|defer|end|");
    expect(r, "drop and defer", 39,
           {{"main", 1, 16, 39}, {"drop", 1, 5, 5}, {"f", 1, 13, 18},
            {"d", 1, 5, 5}});
  }

  // --- 8. A throw unwinds two frames into main's handler. ----------------
  // main { try { outer() } catch e { print e }; say "end" }
  // outer() { inner(); say "unreached" }   inner() { throw "boom" }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back(
        {"outer", 0, 0,
         b.block({b.call_value(b.make_closure(2, 0, p), {}, p),
                  say(b, "unreached")},
                 p),
         {}, {}});
    m.funcs.push_back(
        {"inner", 0, 0, b.make_throw(b.str_literal("boom", p), p), {}, {}});
    m.funcs[0] = {
        "main", 1, 0,
        b.block({b.make_try(0, b.call_value(b.make_closure(1, 0, p), {}, p),
                            pr(b, loc(b, 0)), p),
                 say(b, "end")},
                p),
        {"e"}, {}};
    const Profiled r = run_profiled(m, "throw", "boom|end|");
    expect(r, "throw", 17,
           {{"main", 1, 13, 17}, {"outer", 1, 2, 4}, {"inner", 1, 2, 2}});
  }

  // --- 9. A returned If leaves the frame from each arm, running what the
  //        frame's exit runs -- the scope's destructor -- in its time.
  // main { print(sum(3)) }   drop(self) { say "drop" }
  // sum(n) { scope { o = dropset({}, drop);
  //   return if n == 0 { 0 } else { n + sum(n - 1) } } }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.capture_maps.push_back({{VarKind::Cell, 0}});
    m.funcs.push_back({});
    m.funcs.push_back({"drop", 1, 0, say(b, "drop"), {"self"}, {}});
    m.funcs.back().num_params = 1;
    const NodeId obj =
        b.intrinsic(IntrinsicId::DropSet,
                    {b.object_lit({}, p), b.make_closure(1, 0, p)}, p);
    const NodeId ret = b.make_return(
        b.make_if(b.binary(BinOp::Eq, loc(b, 0), lit(b, 0), p), lit(b, 0),
                  b.binary(BinOp::Add, loc(b, 0),
                           b.call_value(cap(b, 0),
                                        {b.binary(BinOp::Sub, loc(b, 0),
                                                  lit(b, 1), p)},
                                        p),
                           p),
                  p),
        p);
    m.funcs.push_back(
        {"sum", 2, 1,
         b.scope(1, 2, b.block({b.assign(VarKind::Local, 1, obj, p), ret}, p),
                 p),
         {"n", "o"}, {"sum"}});
    m.funcs.back().num_params = 1;
    m.funcs[0] = {
        "main", 0, 0,
        b.block({b.assign(VarKind::Cell, 0, b.make_closure(2, 1, p), p),
                 pr(b, b.call_value(b.varref(VarKind::Cell, 0, p),
                                    {lit(b, 3)}, p)),
                 b.assign(VarKind::Cell, 0, b.nil_literal(p), p)},
                p),
        {}, {}};
    m.funcs[0].num_cells = 1;
    const Profiled r =
        run_profiled(m, "returned if", "drop|drop|drop|drop|6|");
    // sum: 14 instructions for the arm that returns 0, 20 for each that
    // recurses, its own exit's drop among its total each time.
    expect(r, "returned if", 108,
           {{"main", 1, 14, 108}, {"sum", 4, 74, 94}, {"drop", 4, 20, 20}});
  }

  // --- 10. What an intrinsic's facts say once lowered, and the dump of
  //         every opcode (vmlib.h's checklist for a new one).
  for (uint8_t i = 0; i <= static_cast<uint8_t>(kLastIntrinsic); ++i) {
    check_intrinsic_layout(static_cast<I>(i));
  }
  check_every_opcode_dumps();

  return g_failures == 0 ? 0 : 1;
}
