// The stdio host's own knob, not part of the coreir_rt contract.
//
// "<path>:<line>:<col>: <message>" is this implementation's choice of
// wording; a different host formats errors however it formats its own, and
// has no use for a source path threaded in this way. That is why set_path
// lives here rather than in coreir/rt.h.

#pragma once

#include <string>

namespace coreir_rt_default {

void set_path(std::string path);

}  // namespace coreir_rt_default
