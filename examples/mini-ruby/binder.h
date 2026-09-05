// The binder is the front end's job, not the library's. See examples/pl0's
// own binder.h for why: everything peglib-shaped stops here.

#pragma once

#include <string>
#include <vector>

#include "vmlib.h"

namespace mini_ruby {

coreir::Module bind_source(const std::string& source);

// The two things this front end cannot write in its own IR: output, and
// the case-mapping `String#upcase` owes.
const std::vector<vm::NativeDef>& stdlib();

}  // namespace mini_ruby
