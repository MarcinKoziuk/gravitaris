#pragma once

namespace Gravitaris {

// Prints a symbolized stack trace to the log when the process faults, then
// lets it die as it would have. Exists because there is no debugger installed
// on the machines this is developed on, and the startup crash it was written
// for (docs/client-startup-crash.md) only shows up one launch in three -- too
// rare to catch by hand, and invisible without a stack.
//
// A no-op off Windows for now; the platform half is encapsulated in the .cpp
// so nothing else has to see <windows.h>.
void InstallCrashHandler();

} // namespace Gravitaris
