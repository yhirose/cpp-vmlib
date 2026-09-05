// The binder is the front end's job, not the library's. See examples/pl0's
// own binder.h for why: everything peglib-shaped stops here.

#pragma once

#include <string>
#include <vector>

#include "vmlib.h"

namespace mini_python {

coreir::Module bind_source(const std::string& source);

// Python's builtins, as host functions -- the top-level README's Host
// functions recipe. Only the ones an IR-level helper cannot do: output,
// and the float formatting `str()` owes.
const std::vector<vm::NativeDef>& stdlib();

}  // namespace mini_python
