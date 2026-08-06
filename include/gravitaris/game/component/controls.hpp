#pragma once

#include <array>
#include <cstdint>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/upgrade/upgrade-def.hpp>

namespace Gravitaris {

// One tick's requested ship actions. Shared by Controls (resolved state the
// sim acts on) and InputCommand (tick-stamped, queued in an InputQueue).
// firePrimary is a held state (true for as long as the button is down); the
// fire rate is enforced by ShipControlsSystem, per mount (Controls::gunCooldown).
struct ControlFlags {
    bool thrustForward : 1 = false;
    bool rotateLeft : 1 = false;
    bool rotateRight : 1 = false;
    bool firePrimary : 1 = false;
    bool fireSecondary : 1 = false;
    bool fireMissile : 1 = false;
    // One-shot, unlike the fire bits: swaps which of the two primaries the
    // trigger works. Held would toggle every tick the key was down.
    bool toggleWeapon : 1 = false;
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
    // Which weapon mount a ship-tab pick is arming, or NO_MOUNT for the
    // fittings that aren't mounted anywhere (a shield, the overburn).
    static constexpr std::uint8_t NO_MOUNT = 0xFF;
    std::uint8_t mount = NO_MOUNT;

    [[nodiscard]] bool IsSet() const { return node != 0 && rank != 0; }
};

// Which primary the trigger fires. A hull can carry both a cannon and its
// wing guns, and the choice between them is the pilot's in flight rather than
// a property of the loadout -- so it lives here, next to the trigger, not in
// ShipLoadout.
enum class ActiveWeapon : std::uint8_t {
    Cannon, // the default while there is a cannon with rounds in it
    Gun,
};

// Written each tick by InputSystem from the entity's InputQueue, consumed by
// ShipControlsSystem.
struct Controls {
    ControlFlags actionFlags;

    // What the pilot asked for. What actually fires can differ: a dry cannon
    // falls through to the guns without changing this, so the moment a
    // magazine is refilled the heavy mount is live again with no second key
    // press (see ShipControlsSystem::PrimaryWeapon).
    ActiveWeapon activeWeapon = ActiveWeapon::Cannon;

    // Ticks until each mount can fire again; sim-side state, not input, and
    // never serialized (only the flags travel -- see GatherSnapshot).
    //
    // One per mount, not one per ship: every barrel runs its own cadence, so a
    // hull carrying two guns fires twice as often as one carrying one. What
    // stops them landing on the same tick is the phase they are seeded with
    // when the trigger goes down (ShipControlsSystem::SeedMountPhases) -- held
    // fire would otherwise lock them together for good.
    std::array<std::uint32_t, MAX_WEAPON_MOUNTS> gunCooldown{};
    std::array<std::uint32_t, MAX_WEAPON_MOUNTS> missileCooldown{};

    // Which weapon the mount cooldowns above were last phased for. A swap
    // between primaries has to re-deal them, and comparing the def pointer is
    // enough: the catalog owns them for the process's life.
    const void* firingWeaponId = nullptr;

    // Last tick's trigger states, so the rising edge can be spotted. That edge
    // is the only moment the phases above are dealt out; from then on each
    // mount reloads its own cycle and the spacing maintains itself.
    bool firePrimaryWasHeld : 1 = false;
    bool fireMissileWasHeld : 1 = false;
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
