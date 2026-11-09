// The binder is the front end's job, not the library's. See examples/pl0's
// own binder.h for why: everything peglib-shaped stops here.

#pragma once

#include <string>

#include "vmlib.h"

namespace mini_js {

// Parses and lowers, reporting parse errors and every bind-time diagnostic
// through the same formatter the runtime uses.
coreir::Module bind_source(const std::string& source);

// How an uncaught exception reads (vm::RunOptions::describe): what node
// prints for one, rather than the executor's "<object>".
std::string describe(const coreir::Value& v);

// The host functions this front end's programs call: how a number prints
// (double_format.h's kJs for String(n), kJsInspect for console.log(n)).
const std::vector<vm::NativeDef>& stdlib();

}  // namespace mini_js
