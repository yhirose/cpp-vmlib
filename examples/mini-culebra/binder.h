// The binder is the front end's job, not the library's. See examples/pl0's
// own binder.h for why: everything peglib-shaped stops here.

#pragma once

#include <string>

#include "vmlib.h"

namespace mini_culebra {

// Parses and lowers, reporting parse errors and every bind-time diagnostic
// through the same formatter the runtime uses.
coreir::Module bind_source(const std::string& source);

// The host functions this front end's programs call -- culebra's standard
// library, supplied by the run rather than by the IR (the top-level
// README's Host functions recipe). main.cc hands these to vm::run; the
// binder only declares the names (Module::natives), and a name the host
// does not supply fails the run before its first instruction.
const std::vector<vm::NativeDef>& stdlib();

}  // namespace mini_culebra
