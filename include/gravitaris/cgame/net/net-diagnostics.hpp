#pragma once

#include <cstdint>

#include <gravitaris/cgame/net/rolling-history.hpp>

namespace Gravitaris {

// Net debug tab diagnostics: how often/how far the predicted-tick drift
// guard (see PredictedTickClock) has had to resync, and the magnitude of
// each reconciliation correction/snapshot-arrival gap -- recorded only when
// they actually happen (an irregular-event history, like PerfMonitor's own
// sections for code paths that don't run every frame).
struct NetDiagnostics {
    std::uint32_t resyncEventCount = 0;
    std::uint64_t lastResyncDriftTicks = 0;
    RollingHistory driftHistory;
    RollingHistory correctionHistory;
    RollingHistory snapshotIntervalHistory;
};

} // namespace Gravitaris
