#pragma once

#include <Magnum/Math/Vector2.h>

namespace Gravitaris {

class CGame;

// Autopilot mode switch + FlightController/guidance tuning + telemetry.
void DrawFlightPanel(CGame& game);

// World-space markers: goto target cross, orbit ring, hold anchor.
void DrawFlightOverlay(CGame& game, const Magnum::Vector2& uiSize);

} // namespace Gravitaris
