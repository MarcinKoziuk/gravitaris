#pragma once

#include <string>

namespace Gravitaris {

// One line identifying this binary, e.g. "Gravitaris windows RelWithDebInfo
// afff5f3* 2026-07-26 12:04". A trailing '*' on the hash means the tree had
// uncommitted changes. Formatted here rather than at the call site so the HUD
// readout and any future about/debug panel show the same text.
const std::string& BuildInfoString();

} // namespace Gravitaris
