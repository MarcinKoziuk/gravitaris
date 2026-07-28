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
    bool fireMissile : 1 = false;
};

// Which of the Lab's three offers this tick's command accepts: 1..3, or 0 for
// "no pick". Kept out of ControlFlags because it isn't a held state and needs
// more than a bit -- see UpgradeDraft.
using UpgradePick = std::uint8_t;

// Written each tick by InputSystem from the entity's InputQueue, consumed by
// ShipControlsSystem.
struct Controls {
    ControlFlags actionFlags;
    // Ticks until the primary weapon can fire again; sim-side state, not input.
    std::uint32_t fireCooldown = 0;
    // Same, for missiles -- their own cadence, so emptying the rack takes
    // several seconds however hard the button is held.
    std::uint32_t missileCooldown = 0;
    // One-shot, unlike actionFlags: ResearchSystem clears it the tick it acts
    // on it, so a held key can't spend two upgrades.
    UpgradePick upgradePick = 0;
};

} // namespace Gravitaris
