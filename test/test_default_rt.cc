// The stdio host (VMLIB_DEFAULT_RUNTIME), through a real stdout: the one
// place a string leaves the library as bytes rather than as a callback, so
// the one place an embedded NUL could be lost on the way out.

#define VMLIB_DEFAULT_RUNTIME
#include "vmlib.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
  using namespace coreir;
  const SrcPos p{1, 1};
  Module m;
  Builder b(m);
  m.capture_maps.push_back({});
  const std::string text("ab\0cd", 5);
  m.funcs.push_back(
      {"main", 0, 0,
       b.block({b.intrinsic(IntrinsicId::Print, {b.str_literal(text, p)}, p),
                b.intrinsic(IntrinsicId::Print,
                            {b.intrinsic(IntrinsicId::Len,
                                         {b.str_literal(text, p)}, p)},
                            p)},
               p),
       {}, {}});
  if (auto err = verify(m)) {
    std::fprintf(stderr, "malformed IR: %s\n", err->c_str());
    return 1;
  }

  const std::string path = std::string(argc > 1 ? argv[1] : ".") +
                           "/default_rt_out.txt";
  if (!std::freopen(path.c_str(), "wb", stdout)) return 1;
  {
    Runtime rt;
    vm::run(vm::compile(m), rt, vm::RunOptions{});
  }
  std::fflush(stdout);
  std::fclose(stdout);

  std::string got;
  if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
    char buf[64];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) got.append(buf, n);
    std::fclose(f);
  }
  std::remove(path.c_str());
  const std::string want = text + "\n5\n";
  if (got != want) {
    std::fprintf(stderr, "FAIL: stdout was %zu bytes, want %zu\n", got.size(),
                 want.size());
    return 1;
  }
  std::fputs("default_rt: OK\n", stderr);
  return 0;
}
