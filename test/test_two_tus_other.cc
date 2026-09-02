// The second translation unit of test_two_tus: includes vmlib.h again and
// uses the same inline definitions -- Builder, verify, compile, run, the
// dumps -- so the linker sees each of them from two places.

#include <string>

#include "vmlib.h"

std::string other_tu_run(int64_t value) {
  using namespace coreir;
  const SrcPos p{2, 1};
  Module m;
  Builder b(m);
  m.funcs.push_back(
      {"other", 0, 0,
       b.block({b.intrinsic(IntrinsicId::Print,
                            {b.binary(BinOp::Add, b.literal(value, p),
                                      b.literal(2, p), p)},
                            p)},
               p),
       {},
       {}});
  if (auto err = verify(m)) return "malformed IR: " + *err;
  const vm::Program prog = vm::compile(m);
  Runtime rt;
  vm::run(prog, rt);
  return std::to_string(rt.live_objects());
}

std::string other_tu_dump(const coreir::Module& m) {
  return coreir::to_string(m) + vm::to_string(vm::compile(m));
}
