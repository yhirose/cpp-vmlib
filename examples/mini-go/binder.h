// The binder is the front end's job, not the library's. See examples/pl0's
// own binder.h for why: everything peglib-shaped stops here.

#pragma once

#include <string>

#include "vmlib.h"

namespace mini_go {

// Parses and lowers, reporting parse errors and every bind-time diagnostic
// through the same formatter the runtime uses.
coreir::Module bind_source(const std::string& source);

// The host functions this front end's programs call: how a float64 and a
// float32 print (double_format.h's kGo).
const std::vector<vm::NativeDef>& stdlib();

}  // namespace mini_go
