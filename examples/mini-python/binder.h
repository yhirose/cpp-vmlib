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

// How an uncaught exception reads (vm::RunOptions::describe): what python3
// prints for one, rather than the executor's "<object>".
std::string describe(const coreir::Value& v);

// An executor's trap as the exception this language raises for the same
// fault (runtime_parts.h's converted_or_self applies it).
coreir::Value convert_trap(const coreir::Value& trap, const void* ctx);

}  // namespace mini_python
