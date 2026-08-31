// The contract every host implements: four C functions, nothing else.
//
// vm/exec.cc calls these and links against nothing beyond this declaration --
// it does not know or care whether the definitions come from
// runtime/coreir_rt_default.cc (the standalone CLI's stdio implementation) or
// from a host that embeds this library elsewhere and wants its own output and
// error reporting (a script-hosted VM inside an interpreter, say). Whichever
// implementation is linked in is the only one in the binary; there is no
// indirection to pay for and no way for two hosts' behavior to blend.

#pragma once

#include <cstdint>
#include <string>

extern "C" {

// Print one integer, followed by a newline.
void coreir_rt_out(int64_t v);

// Read one line and convert it to an integer. A line that is not an integer,
// or end of input, fails at the position given.
int64_t coreir_rt_in(int64_t line, int64_t col);

// Report and terminate the running program. What "terminate" means is the
// host's choice -- exit the process, or throw a host-level exception -- as
// long as it does not return. Every frame the executor has live across this
// call (vm/exec.cc's Frame) holds non-trivial-destructor members, so a host
// must not implement this with longjmp: unwinding past them without running
// their destructors is undefined behavior, not merely wasteful.
[[noreturn]] void coreir_rt_fail(const char* msg, int64_t line, int64_t col);

// Called on every loop back-edge and call, so a host that wants to interrupt
// a running program (Ctrl+C, a cooperative cancellation flag) has a place to
// do it. The default implementation is a no-op.
void coreir_rt_poll(void);

}  // extern "C"

namespace coreir_rt {

// A std::string convenience over coreir_rt_fail -- not a second formatter,
// just an argument-type adapter, so callers do not each write
// `msg.c_str()` themselves.
[[noreturn]] inline void fail(const std::string& msg, uint32_t line,
                              uint32_t col) {
  coreir_rt_fail(msg.c_str(), line, col);
}

}  // namespace coreir_rt
