#pragma once

#include <cstdint>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/upgrade/upgrade-def.hpp>

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

// One tech-tree purchase this tick's command is committing. Kept out of
// ControlFlags because it isn't a held state and needs far more than a bit --
// see ResearchSystem.
//
// All three parts are load-bearing. The same def id names a node in both
// trees, so `tab` is what separates "the faction learns this" from "this hull
// fits it"; and the ship tree sells a rank outright rather than the next one
// up, so `rank` is what lets a pilot buy II while holding III unlocked.
struct TechPick {
    id_t node = 0; // zero means no purchase this tick
    TechTab tab = TechTab::Ship;
    std::uint8_t rank = 0;

    [[nodiscard]] bool IsSet() const { return node != 0 && rank != 0; }
};

// Written each tick by InputSystem from the entity's InputQueue, consumed by
// ShipControlsSystem.
struct Controls {
    ControlFlags actionFlags;
    // Ticks until the primary weapon can fire again; sim-side state, not input.
    std::uint32_t fireCooldown = 0;
    // Same, for missiles -- their own cadence, so emptying the rack takes
    // several seconds however hard the button is held.
    std::uint32_t missileCooldown = 0;
    // Which mount the next round of each kind leaves from, counted up on every
    // shot so a hull with more than one of a family alternates across them
    // (Body::FindMount wraps, so these never need clamping to a hull's count).
    std::uint8_t gunMount = 0;
    std::uint8_t missileMount = 0;
    // One-shot, unlike actionFlags: ResearchSystem clears it the tick it acts
    // on it, so one click can't spend two purchases.
    TechPick techPick;

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
