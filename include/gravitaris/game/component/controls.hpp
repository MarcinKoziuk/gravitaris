#pragma once

#include <cstdint>

namespace Gravitaris {

// One tick's requested ship actions. Shared by Controls (resolved state the
// sim acts on) and InputCommand (tick-stamped, queued in an InputQueue).
// firePrimary is a held state (true for as long as the button is down); the
// weapon's fire rate is enforced by ShipControlsSystem via Controls::fireCooldown.
struct ControlFlags {
    bool thrustForward : 1 = false;
    bool rotateLeft : 1 = false;
    bool rotateRight : 1 = false;
    bool firePrimary : 1 = false;
    bool fireSecondary : 1 = false;
};

// Written each tick by InputSystem from the entity's InputQueue, consumed by
// ShipControlsSystem.
struct Controls {
    ControlFlags actionFlags;
    // Ticks until the primary weapon can fire again; sim-side state, not input.
    std::uint32_t fireCooldown = 0;
};

} // namespace Gravitaris
