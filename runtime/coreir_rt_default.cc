#include "coreir_rt_default.h"

#include "coreir/rt.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace {
std::string g_path = "<input>";
}

namespace coreir_rt_default {

void set_path(std::string path) { g_path = std::move(path); }

}  // namespace coreir_rt_default

extern "C" {

void coreir_rt_out(int64_t v) { std::printf("%" PRId64 "\n", v); }

int64_t coreir_rt_in(int64_t line, int64_t col) {
  std::string s;
  if (!std::getline(std::cin, s)) coreir_rt_fail("invalid input", line, col);

  // One line, converted whole -- matching pl0.cul's `to_long(IO.input())`
  // rather than scanf's whitespace-delimited token.
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) coreir_rt_fail("invalid input", line, col);
  size_t e = s.find_last_not_of(" \t\r\n");
  s = s.substr(b, e - b + 1);

  errno = 0;
  char* end = nullptr;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (errno != 0 || end != s.c_str() + s.size()) {
    coreir_rt_fail("invalid input", line, col);
  }
  return static_cast<int64_t>(v);
}

void coreir_rt_fail(const char* msg, int64_t line, int64_t col) {
  std::fflush(stdout);
  std::fprintf(stderr, "%s:%" PRId64 ":%" PRId64 ": %s\n", g_path.c_str(),
               line, col, msg);
  std::exit(1);
}

void coreir_rt_poll(void) {}

}  // extern "C"
