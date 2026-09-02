// vmlib.h included from two translation units linked into one binary. The
// property is at link time: every definition the header carries is inline,
// so two inclusions produce one program rather than a multiple-definition
// error. Each unit builds and runs a module of its own through the same host,
// and the dumps taken from both sides must agree, which is the same code and
// the same Runtime::current() being reached from both.

#include <cstdio>
#include <string>
#include <vector>

#include "vmlib.h"

// Defined in test_two_tus_other.cc.
std::string other_tu_run(int64_t value);
std::string other_tu_dump(const coreir::Module& m);

namespace {

std::vector<std::string> g_out;
int g_failures = 0;

void check_eq(const std::string& got, const std::string& want,
              const std::string& what) {
  if (got != want) {
    std::fprintf(stderr, "FAIL: %s\n  want [%s]\n  got  [%s]\n", what.c_str(),
                 want.c_str(), got.c_str());
    ++g_failures;
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
  std::fprintf(stderr, "unexpected failure: %s\n", msg);
  std::exit(1);
}
}

int main() {
  using namespace coreir;
  const SrcPos p{1, 1};

  // This unit: print 1 + 2.
  Module m;
  Builder b(m);
  m.funcs.push_back(
      {"main", 0, 0,
       b.block({b.intrinsic(IntrinsicId::Print,
                            {b.binary(BinOp::Add, b.literal(1, p),
                                      b.literal(2, p), p)},
                            p)},
               p),
       {},
       {}});
  if (auto err = verify(m)) {
    std::fprintf(stderr, "FAIL: malformed IR: %s\n", err->c_str());
    return 1;
  }
  const vm::Program prog = vm::compile(m);
  {
    Runtime rt;
    vm::run(prog, rt);
    check_eq(std::to_string(rt.live_objects()), "0", "this unit: live");
  }
  check_eq(g_out.size() == 1 ? g_out[0] : "", "3", "this unit: output");

  // The other unit, running its own module through the same host.
  g_out.clear();
  const std::string other = other_tu_run(40);
  check_eq(g_out.size() == 1 ? g_out[0] : "", "42", "other unit: output");
  check_eq(other, "0", "other unit: live");

  // The same module dumped from both sides.
  check_eq(other_tu_dump(m), to_string(m) + vm::to_string(prog),
           "dumps agree across units");

  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("two_tus OK");
  return 0;
}
