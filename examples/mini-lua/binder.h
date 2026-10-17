// The binder is the front end's job, not the library's. See examples/pl0's
// own binder.h for why: everything peglib-shaped stops here.

#pragma once

#include <string>
#include <vector>

#include "vmlib.h"

namespace mini_lua {

coreir::Module bind_source(const std::string& source,
                           const std::string& chunkname);

// Lua's standard library, as host functions -- the top-level README's Host
// functions recipe. main.cc hands these to vm::run; the binder only
// declares the names it calls.
const std::vector<vm::NativeDef>& stdlib();

// How an uncaught error object reads (vm::RunOptions::describe): what lua
// prints for one ("(error object is a table value)"), rather than the
// executor's "<map>".
std::string describe(const coreir::Value& v);

}  // namespace mini_lua
