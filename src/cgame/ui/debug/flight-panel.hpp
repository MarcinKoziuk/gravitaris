#pragma once

namespace Gravitaris {

class CGame;

// Autopilot mode switch + FlightController gain tuning + telemetry. The
// tuning UI for docs/ai-ships.md phase 2.
void DrawFlightPanel(CGame& game);

} // namespace Gravitaris
