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
    // Held, like thrustForward: a request for the overburn, which
    // ShipControlsSystem grants only while there is boost left and the
    // cooldown has expired (see Controls::boostTicks).
    bool boost : 1 = false;
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

    // The overburn's two timers, both in ticks (see UpgradeDef::Boost).
    // `boostTicks` is what is left of the current burn and is only ever
    // non-zero on a ship carrying the upgrade; `boostCooldown` is the wait
    // before another one can start, and runs down whether or not the button
    // is held. A burn that is cut short still costs the full cooldown, so
    // tapping it is not free.
    std::uint16_t boostTicks = 0;
    std::uint16_t boostCooldown = 0;
    // Whether the overburn is actually running this tick -- what the movement
    // integrator, the wire (PackControlFlags) and the exhaust all read, as
    // opposed to actionFlags.boost, which is only the request.
    bool boosting = false;
};

} // namespace Gravitaris
