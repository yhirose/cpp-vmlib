// The one place any lane produces output or reports a runtime error.
//
// pl0.cc gets this wrong in all three axes at once: its interpreter writes
// with `cout <<` and reports to `cerr` as "divide by 0 error" with a position,
// while its JIT writes with `printf("%d\n")` and reports to stdout as
// "divide by 0" with no position at all. Every one of those divergences comes
// from each lane formatting for itself. Routing all three through one C
// function makes that class of asymmetry structurally impossible rather than
// merely tested for -- the same move culebra makes with
// culebra_runtime_println.

#pragma once

#include <cstdint>
#include <string>

extern "C" {

// Print one integer, followed by a newline.
void pl0_rt_out(int64_t v);

// Read one line and convert it to an integer. A line that is not an integer,
// or end of input, fails at the position of the `?` statement.
int64_t pl0_rt_in(int64_t line, int64_t col);

// Report and terminate. Writes "<path>:<line>:<col>: <message>" to stderr and
// exits 1.
//
// The plan called for setjmp/longjmp so the lanes could unwind out of JIT
// frames alike; calling exit() directly turned out to be both simpler and
// better defined (longjmp past live std::vector frames in the interpreter is
// UB, and there is nothing to clean up when the process is ending anyway).
// The property that mattered -- one formatter, one exit code, three lanes --
// is unchanged.
[[noreturn]] void pl0_rt_fail(const char* msg, int64_t line, int64_t col);

}  // extern "C"

namespace pl0rt {

// The driver knows the path; the fault sites only carry line and column.
void set_path(std::string path);

// Bind-time diagnostics take the same route as runtime ones, so a program that
// fails to compile and a program that fails to run are reported identically.
[[noreturn]] void fail(const std::string& msg, uint32_t line, uint32_t col);

}  // namespace pl0rt
