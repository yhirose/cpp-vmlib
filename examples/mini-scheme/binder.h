// The binder is the front end's job, not the library's. See examples/pl0's
// own binder.h for why: everything peglib-shaped stops here.

#pragma once

#include <string>
#include <vector>

#include "vmlib.h"

namespace mini_scheme {

coreir::Module bind_source(const std::string& source);

// The two builtins that have to reach the host: everything else this front
// end needs is IR it writes itself.
const std::vector<vm::NativeDef>& stdlib();

}  // namespace mini_scheme
