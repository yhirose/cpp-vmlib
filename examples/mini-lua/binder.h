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

}  // namespace mini_lua
