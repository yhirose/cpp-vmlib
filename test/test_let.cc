// Let: a value held for one expression. What this pins: the shapes
// verify() refuses -- a LetRef outside its Let's body (after it, in its
// init, in a function built inside it), two Lets binding one temporary,
// a negative id -- and the ones it accepts; that a temporary compiles to
// the register its init landed in, read with no instruction and never
// stored; that the value is what every read sees, through If and Switch
// arms, a Try around the Let and one inside it, a Yield across it, a
// returned and a tail-called Let; that a heap value it holds lives to the
// end of its statement and no longer, as an operand's does; and that the
// compiler's allocator refuses, in every build, to hand out or take back a
// register below a temporary -- what makes a LetRef's free read safe -- and
// a call's result inside its arguments.

#include <cstdio>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "vmlib.h"

namespace {

struct Failure : std::runtime_error {
  explicit Failure(const std::string& msg) : std::runtime_error(msg) {}
};

std::vector<std::string> g_out;
int g_failures = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

void check_eq(const std::string& got, const std::string& want,
              const std::string& what) {
  if (got != want) {
    std::fprintf(stderr, "FAIL: %s\n  want [%s]\n  got  [%s]\n", what.c_str(),
                 want.c_str(), got.c_str());
    ++g_failures;
  }
}

std::string joined() {
  std::string s;
  for (const auto& line : g_out) s += line + "|";
  return s;
}

// inc(x): x + 1, a host function.
bool native_inc(coreir::NativeCall& c) {
  c.result = coreir::Value::make_int(c.arg(0).as_int() + 1);
  return true;
}

// Runs `m`, answering the failure it ended in ("" for none), with the heap
// checked empty after.
std::string run_module(const coreir::Module& m, const std::string& what) {
  g_out.clear();
  if (auto err = coreir::verify(m)) {
    check(false, what + ": malformed IR: " + *err);
    return {};
  }
  std::string failure;
  int64_t left = 0;
  {
    coreir::Runtime rt;
    try {
      vm::RunOptions opts;
      opts.natives = {{"inc", 1, &native_inc, nullptr}};
      vm::run(vm::compile(m), rt, opts);
    } catch (const Failure& e) {
      failure = e.what();
    }
    left = rt.live_objects();
  }
  check(left == 0, what + ": heap objects left after the run");
  return failure;
}

// A node as a front end could build one by hand, past what Builder::let
// hands out: the shapes verify() is there to refuse.
coreir::NodeId raw(coreir::Module& m, coreir::Tag tag, int32_t a,
                   const std::vector<coreir::NodeId>& children = {}) {
  coreir::Node n;
  n.tag = tag;
  n.a = a;
  n.first_child = static_cast<uint32_t>(m.child_ids.size());
  n.num_children = static_cast<uint32_t>(children.size());
  m.child_ids.insert(m.child_ids.end(), children.begin(), children.end());
  m.nodes.push_back(n);
  return coreir::NodeId{static_cast<uint32_t>(m.nodes.size() - 1)};
}

size_t count(const std::string& s, const std::string& what) {
  size_t n = 0;
  for (size_t at = s.find(what); at != std::string::npos;
       at = s.find(what, at + 1)) {
    ++n;
  }
  return n;
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
  const SrcPos p{1, 1};
  using Temp = Builder::Temp;

  // --- 1. What verify() refuses, and what it accepts. -------------------
  {
    const auto verdict = [](const std::function<NodeId(Module&, Builder&)>&
                                build) {
      Module m;
      Builder b(m);
      m.capture_maps.push_back({});
      m.funcs.push_back({"main", 0, 0, NodeId{}, {}, {}});
      m.funcs[0].body = build(m, b);
      return verify(m).value_or("");
    };
    // let t = 1 in t + t, and nested: let u = t * 2 in u + t.
    check_eq(verdict([&](Module&, Builder& b) {
               return b.let(b.literal(1, p), [&](Temp t) {
                 return b.let(b.binary(BinOp::Mul, t(), b.literal(2, p), p),
                              [&](Temp u) {
                                return b.binary(BinOp::Add, u(), t(), p);
                              },
                              p);
               }, p);
             }),
             "", "verify: nested Lets, the inner reading the outer");
    check_eq(verdict([&](Module&, Builder& b) {
               std::optional<Temp> keep;
               const NodeId l = b.let(b.literal(1, p), [&](Temp t) {
                 keep.emplace(t);
                 return t();
               }, p);
               return b.block({l, (*keep)()}, p);
             }),
             "t0 is read outside the body of its Let",
             "verify: a LetRef after its Let");
    check_eq(verdict([&](Module& m, Builder& b) {
               // let t = t in t: the init is not the body.
               const NodeId ref = raw(m, Tag::LetRef, m.next_let);
               return b.let(ref, [&](Temp t) { return t(); }, p);
             }),
             "t0 is read outside the body of its Let",
             "verify: a LetRef in its own Let's init");
    check_eq(verdict([&](Module& m, Builder& b) {
               // let t = 1 in (fn () { t })(): a function of its own.
               return b.let(b.literal(1, p), [&](Temp t) {
                 m.funcs.push_back({"inner", 0, 0, t(), {}, {}});
                 return b.call_value(b.make_closure(1, 0, p), {}, p);
               }, p);
             }),
             "t0 is read outside the body of its Let",
             "verify: a LetRef in a function built inside its Let");
    check_eq(verdict([&](Module& m, Builder& b) {
               const NodeId a =
                   raw(m, Tag::Let, 7, {b.literal(1, p), b.literal(2, p)});
               const NodeId c =
                   raw(m, Tag::Let, 7, {b.literal(3, p), b.literal(4, p)});
               return b.block({a, c}, p);
             }),
             "two Lets bind temporary t7", "verify: one id bound twice");
    check_eq(verdict([&](Module& m, Builder& b) {
               return raw(m, Tag::Let, -1, {b.literal(1, p), b.literal(2, p)});
             }),
             "Let's temporary id is negative", "verify: a negative id");
  }

  // --- 2. A temporary is a register: read with no instruction, never
  //        stored. let t = x + 1 in t * t, x a local holding 3.
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 1, 0,
         b.block({b.assign(VarKind::Local, 0, b.literal(3, p), p),
                  b.intrinsic(IntrinsicId::Print,
                              {b.let(b.binary(BinOp::Add,
                                              b.varref(VarKind::Local, 0, p),
                                              b.literal(1, p), p),
                                     [&](Temp t) {
                                       return b.binary(BinOp::Mul, t(), t(),
                                                       p);
                                     },
                                     p)},
                              p)},
                 p),
         {"x"},
         {}});
    check_eq(run_module(m, "register"), "", "register: failure");
    check_eq(joined(), "16|", "register: t * t");
    const std::string dump = vm::to_string(vm::compile(m));
    check_eq(std::to_string(count(dump, "\tloadlocal ")), "1",
             "register: the one load is x's, none is t's");
    check_eq(std::to_string(count(dump, "\tstorelocal ")), "1",
             "register: the one store is x's");
    check(dump.find("\tadd r0, r0, r1") != std::string::npos &&
              dump.find("\tmul r1, r0, r0") != std::string::npos,
          "register: both reads are r0, where the init landed");
    check(dump.find("\tmove ") == std::string::npos,
          "register: nothing moves the value");
    check(to_string(m).find("let t0") != std::string::npos &&
              count(to_string(m), "      t0  @") == 2,
          "dump-ir: `let t0` over its two reads, `t0`");
  }

  // --- 3. Every read sees the init's value: through an If's arms and a
  //        Switch's, with the body writing registers above it.
  // for k in 0..2: print(let t = k * 10 in
  //   (if t == 0 { t + 1 } else { switch k { 1: t + 2, default: t + 3 } }))
  {
    Module m;
    Builder b(m);
    const auto K = [&] { return b.varref(VarKind::Local, 0, p); };
    const NodeId body = b.intrinsic(
        IntrinsicId::Print,
        {b.let(b.binary(BinOp::Mul, K(), b.literal(10, p), p),
               [&](Temp t) {
                 return b.make_if(
                     b.binary(BinOp::Eq, t(), b.literal(0, p), p),
                     b.binary(BinOp::Add, t(), b.literal(1, p), p),
                     b.make_switch(K(),
                                   {{b.literal(1, p),
                                     b.binary(BinOp::Add, t(),
                                              b.literal(2, p), p)}},
                                   b.binary(BinOp::Add, t(), b.literal(3, p),
                                            p),
                                   p),
                     p);
               },
               p)},
        p);
    m.funcs.push_back(
        {"main", 1, 0,
         b.block({b.assign(VarKind::Local, 0, b.literal(0, p), p),
                  b.make_while(
                      b.binary(BinOp::Lt, K(), b.literal(3, p), p),
                      b.block({body,
                               b.assign(VarKind::Local, 0,
                                        b.binary(BinOp::Add, K(),
                                                 b.literal(1, p), p),
                                        p)},
                              p),
                      p)},
                 p),
         {"k"},
         {}});
    check_eq(run_module(m, "arms"), "", "arms: failure");
    check_eq(joined(), "1|12|23|", "arms: each arm reads the value");
  }

  // --- 4. A Try around a Let and one inside it: the handler reads the
  //        outer temporary, which the unwinding leaves alone.
  // print(let t = 5 in try { let u = t + 1 in throw u } catch e { e + t })
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 1, 0,
         b.intrinsic(
             IntrinsicId::Print,
             {b.let(b.literal(5, p),
                    [&](Temp t) {
                      return b.make_try(
                          0,
                          b.let(b.binary(BinOp::Add, t(), b.literal(1, p), p),
                                [&](Temp u) { return b.make_throw(u(), p); },
                                p),
                          b.binary(BinOp::Add, b.varref(VarKind::Local, 0, p),
                                   t(), p),
                          p);
                    },
                    p)},
             p),
         {"e"},
         {}});
    check_eq(run_module(m, "try"), "", "try: failure");
    check_eq(joined(), "11|", "try: the handler reads the outer temporary");
  }

  // --- 5. A Yield across a Let: the temporary is the frame's, and comes
  //        back with it. gen() { yield (let t = 7 in (yield t) + t) }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back(
        {"gen", 0, 0,
         b.make_yield(b.let(b.literal(7, p),
                            [&](Temp t) {
                              return b.binary(BinOp::Add,
                                              b.make_yield(t(), p), t(), p);
                            },
                            p),
                      p),
         {},
         {}});
    m.funcs.back().is_generator = true;
    const auto G = [&] { return b.varref(VarKind::Local, 0, p); };
    const auto resume = [&](int64_t sent) {
      return b.intrinsic(
          IntrinsicId::Print,
          {b.index(b.intrinsic(IntrinsicId::GenResume,
                               {G(), b.literal(sent, p)}, p),
                   b.str_literal("value", p), p)},
          p);
    };
    m.funcs[0] = {"main", 1, 0,
                  b.block({b.assign(VarKind::Local, 0,
                                    b.call_value(b.make_closure(1, 0, p), {},
                                                 p),
                                    p),
                           resume(0), resume(100)},
                          p),
                  {"g"},
                  {}};
    check_eq(run_module(m, "yield"), "", "yield: failure");
    check_eq(joined(), "7|107|", "yield: the temporary survives the yield");
  }

  // --- 6. A returned Let returns from each arm, and a call in its body
  //        stays a tail call: down(n) = let t = n in
  //        (if t == 0 { 0 } else { down(t - 1) }), a hundred thousand
  //        deep: ten times max_call_depth, so in one frame or not at all.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.capture_maps.push_back({{VarKind::Cell, 0}});
    m.funcs.push_back({});
    m.funcs.push_back(
        {"down", 1, 1,
         b.make_return(
             b.let(b.varref(VarKind::Local, 0, p),
                   [&](Temp t) {
                     return b.make_if(
                         b.binary(BinOp::Eq, t(), b.literal(0, p), p),
                         b.literal(0, p),
                         b.call_value(b.varref(VarKind::Capture, 0, p),
                                      {b.binary(BinOp::Sub, t(),
                                                b.literal(1, p), p)},
                                      p),
                         p);
                   },
                   p),
             p),
         {"n"},
         {"down"}});
    m.funcs.back().num_params = 1;
    m.funcs.back().tail_calls = true;
    m.funcs[0] = {"main", 0, 0,
                  b.block({b.assign(VarKind::Cell, 0,
                                    b.make_closure(1, 1, p), p),
                           b.intrinsic(IntrinsicId::Print,
                                       {b.call_value(
                                           b.varref(VarKind::Cell, 0, p),
                                           {b.literal(100000, p)}, p)},
                                       p),
                           b.assign(VarKind::Cell, 0, b.nil_literal(p), p)},
                          p),
                  {},
                  {}};
    m.funcs[0].num_cells = 1;
    check_eq(run_module(m, "tail"), "", "tail: failure");
    check_eq(joined(), "0|", "tail: every call in one frame");
    const std::string dump = vm::to_string(vm::compile(m));
    check(dump.find("\ttailcall ") != std::string::npos,
          "tail: the call in the Let's body is a tail call");
    check(dump.find("\tmove ") == std::string::npos,
          "tail: each arm returns its own value");
  }

  // --- 7. A heap value a temporary holds lives to the end of its
  //        statement, as an operand's does, and no longer.
  // { print(let t = dropset({}, drop) in "body"); print "next" }
  // drop(self) { print "drop" }
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    m.funcs.push_back({"drop", 1, 0,
                       b.intrinsic(IntrinsicId::Print,
                                   {b.str_literal("drop", p)}, p),
                       {"self"},
                       {}});
    m.funcs.back().num_params = 1;
    const NodeId obj =
        b.intrinsic(IntrinsicId::DropSet,
                    {b.object_lit({}, p), b.make_closure(1, 0, p)}, p);
    m.funcs[0] = {
        "main", 0, 0,
        b.block({b.intrinsic(IntrinsicId::Print,
                             {b.let(obj,
                                    [&](Temp) {
                                      return b.str_literal("body", p);
                                    },
                                    p)},
                             p),
                 b.intrinsic(IntrinsicId::Print, {b.str_literal("next", p)},
                             p)},
                p),
        {},
        {}};
    check_eq(run_module(m, "lifetime"), "", "lifetime: failure");
    check_eq(joined(), "body|drop|next|",
             "lifetime: dropped at the statement's end");
  }

  // --- 8. Several, nested in order: each init runs once, first to last,
  //        and each read sees its own. print(lets [a(), b()] in b - a),
  //        a() printing "a" and answering 1, b() printing "b", 10.
  {
    Module m;
    Builder b(m);
    m.capture_maps.push_back({});
    m.funcs.push_back({});
    const auto noisy = [&](const char* name, int64_t v) {
      m.funcs.push_back(
          {name, 0, 0,
           b.block({b.intrinsic(IntrinsicId::Print, {b.str_literal(name, p)},
                                p),
                    b.make_return(b.literal(v, p), p)},
                   p),
           {},
           {}});
      return b.call_value(
          b.make_closure(static_cast<int32_t>(m.funcs.size() - 1), 0, p), {},
          p);
    };
    const NodeId a = noisy("a", 1);
    const NodeId c = noisy("b", 10);
    m.funcs[0] = {"main", 0, 0,
                  b.intrinsic(IntrinsicId::Print,
                              {b.lets({a, c},
                                      [&](const std::vector<Temp>& t) {
                                        return b.binary(BinOp::Sub, t[1](),
                                                        t[0](), p);
                                      },
                                      p)},
                              p),
                  {},
                  {}};
    check_eq(run_module(m, "lets"), "", "lets: failure");
    check_eq(joined(), "a|b|9|", "lets: in order, each its own");
  }

  // --- 9. A callee a Let holds: the call's result still lands below its
  //        arguments, where a native's borrowed arguments, released after
  //        the result is written, cannot take it with them.
  //        print(let f = inc in f(41))
  {
    Module m;
    Builder b(m);
    m.funcs.push_back(
        {"main", 0, 0,
         b.intrinsic(IntrinsicId::Print,
                     {b.let(b.native_ref(b.declare_native("inc"), p),
                            [&](Temp f) {
                              return b.call_value(f(), {b.literal(41, p)}, p);
                            },
                            p)},
                     p),
         {},
         {}});
    check_eq(run_module(m, "native callee"), "", "native callee: failure");
    check_eq(joined(), "42|", "native callee: its result survives");
  }

  // --- 10. The allocator refuses to hand out or take back a register
  //         below an open Let's temporary, in every build, and a call's
  //         result inside its argument run.
  {
    Module m;
    vm::Chunk ch;
    vm::detail::FnCompiler fc{m, ch};
    const auto failure_of = [&](const std::function<void()>& f) {
      try {
        f();
      } catch (const Failure& e) {
        return std::string(e.what());
      }
      return std::string();
    };
    fc.alloc();
    fc.alloc();                // r0, r1; the Let's temporary is r1
    fc.pins.push_back({0, 1});  // so the floor is r2
    check_eq(failure_of([&] { fc.release_to(1); }),
             "vm::compile: release_to at r1, below r2, where an open Let "
             "keeps its temporaries",
             "pin: taking back its register is refused");
    fc.top = 1;
    check_eq(failure_of([&] { fc.alloc(); }),
             "vm::compile: alloc at r1, below r2, where an open Let keeps "
             "its temporaries",
             "pin: handing it out again is refused");
    fc.top = 2;
    check_eq(failure_of([&] {
               fc.alloc();
               fc.release_to(2);
               fc.alloc();
             }),
             "", "pin: registers above it come and go freely");
    fc.pins.pop_back();
    fc.top = 1;
    check_eq(failure_of([&] { fc.alloc_result(1, 1); }),
             "vm::compile: a call's result r1 is in its argument run from r1",
             "result: a result in the argument run is refused");
    fc.top = 3;
    check_eq(failure_of([&] { fc.alloc_result(1, 2); }), "",
             "result: a result below the run is not");
  }

  if (g_failures != 0) {
    std::fprintf(stderr, "let: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("let OK\n");
  return 0;
}
