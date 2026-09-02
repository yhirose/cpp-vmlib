// The binder is the front end's job, not the library's.
//
// Everything peglib-shaped stops here: coreir and vm never see a peg::Ast,
// and their include paths do not even reach peglib.h. A front end built on a
// hand-written recursive-descent parser would replace this file and nothing
// else.

#pragma once

#include <string>

#include "vmlib.h"

namespace pl0 {

// Parses and lowers, reporting parse errors and every bind-time diagnostic
// through the same formatter the runtime uses.
coreir::Module bind_source(const std::string& source);

}  // namespace pl0
