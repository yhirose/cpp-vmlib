// The binder is the front end's job, not the library's. See examples/pl0's
// own binder.h for why: everything peglib-shaped stops here.

#pragma once

#include <string>
#include <vector>

#include "vmlib.h"

namespace mini_csharp {

coreir::Module bind_source(const std::string& source);

// The one thing this front end cannot write in its own IR: output.
const std::vector<vm::NativeDef>& stdlib();

}  // namespace mini_csharp
