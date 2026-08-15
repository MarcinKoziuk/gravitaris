#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/upgrade/upgrade-def.hpp>

namespace Gravitaris {

// One tick's requested ship actions. Shared by Controls (resolved state the
// sim acts on) and InputCommand (tick-stamped, queued in an InputQueue).
// firePrimary is a held state (true for as long as the button is down); the
// fire rate is enforced by ShipControlsSystem, per mount (Controls::gunCooldown).
//
// Not all bits, despite the name: `aim` rides here too, because every path that
// carries a tick's input already carries one of these -- the queue, the wire,
// the replay log, the prediction history -- and a second struct threaded
// alongside would have to be kept in step with all of them.
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
    // ShipControlsSystem grants only while the engine is lit and there is
    // something left in the bank (see Controls::capacitorSpent).
    bool boost : 1 = false;
    // Held, like firePrimary, and paced by nothing: the laser mounts burn for
    // as long as the button is down and the capacitor has charge to give them.
    bool fireLaser : 1 = false;

    // Where the pilot is pointing the gimballed mounts, as an absolute world
    // angle in 65536 steps of a turn -- about 0.005 degrees, far finer than a
    // hull can hold a beam. Quantised here rather than at the wire so a client
    // predicts its own beam off exactly the number the server will resolve it
    // from, instead of off a float the wire then rounds.
    std::uint16_t aim = 0;
};

// A whole turn in 65536 steps. Absolute world angle, in the same frame as
// Transform::rot, so nothing has to know a ship's heading to read one.
inline std::uint16_t PackAim(double radians)
{
    constexpr double TURN = 6.283185307179586;
    const double turns = radians / TURN;
    return static_cast<std::uint16_t>(
            static_cast<std::int64_t>(std::floor(turns * 65536.0)) & 0xFFFF);
}

inline double UnpackAim(std::uint16_t packed)
{
    constexpr double TURN = 6.283185307179586;
    return static_cast<double>(packed) * TURN / 65536.0;
}

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
    // Which hull hole a ship-tab pick is arming, or NO_MOUNT for the fittings
    // that aren't mounted anywhere (a shield, the overburn). Which family that
    // indexes is the node's own business -- a weapon line goes in a weapon
    // mount, a launcher in a missile bay -- so it is not spelled again here.
    static constexpr std::uint8_t NO_MOUNT = 0xFF;
    std::uint8_t mount = NO_MOUNT;
    // A pick that empties rather than fills: the hole or the system named
    // comes back off the hull. `rank` stays 0, which is what tells the two
    // apart -- there is no rank 0 to fit.
    bool strip = false;
    // Fills every magazine the hull carries, for Supplies. Names no node --
    // it buys rounds for whatever is already fitted -- so it is the one form
    // of pick that sets nothing else, and IsSet() answers to it alone.
    bool resupply = false;

    [[nodiscard]] bool IsSet() const
    { return resupply || (node != 0 && (rank != 0 || strip)); }
};

// Which primary the trigger fires. A hull can carry both a cannon and its
// wing guns, and the choice between them is the pilot's in flight rather than
// a property of the loadout -- so it lives here, next to the trigger, not in
// ShipLoadout.
//
// Both is the default: a hull that has paid for two lines should shoot with
// two lines unless its pilot says otherwise, and holding one back is the
// deliberate act (saving the magazine, or staying quiet on the heavy mounts).
enum class ActiveWeapon : std::uint8_t {
    Both,
    Cannon,
    Gun,
};

// What the toggle key steps to. Round and round rather than there and back:
// three states need a cycle, and Both leads so one press off the default is
// the cannon alone.
inline ActiveWeapon NextWeapon(ActiveWeapon current)
{
    switch (current) {
    case ActiveWeapon::Both:   return ActiveWeapon::Cannon;
    case ActiveWeapon::Cannon: return ActiveWeapon::Gun;
    case ActiveWeapon::Gun:    return ActiveWeapon::Both;
    }
    return ActiveWeapon::Both;
}

// How many primary lines one trigger can work at once: the light guns and the
// heavy cannon (see ActiveWeapon::Both).
inline constexpr std::size_t MAX_PRIMARY_LINES = 2;

// Written each tick by InputSystem from the entity's InputQueue, consumed by
// ShipControlsSystem.
struct Controls {
    ControlFlags actionFlags;

    // What the pilot asked for. What actually fires can differ: a dry cannon
    // falls through to the guns without changing this, so the moment a
    // magazine is refilled the heavy mount is live again with no second key
    // press (see ShipControlsSystem::PrimaryWeapons).
    ActiveWeapon activeWeapon = ActiveWeapon::Both;

    // Ticks until each mount can fire again; sim-side state, not input, and
    // never serialized (only the flags travel -- see GatherSnapshot).
    //
    // Indexed by hull mount, not by firing slot: with both lines live the
    // mounts one of them is armed on are not a contiguous run, and packing
    // them would have a gun and a cannon pacing each other off one entry.
    //
    // One per mount, not one per ship: every barrel runs its own cadence, so a
    // hull carrying two guns fires twice as often as one carrying one. What
    // stops them landing on the same tick is the phase they are seeded with
    // when the trigger goes down (ShipControlsSystem::SeedPhasesAt) -- held
    // fire would otherwise lock them together for good.
    std::array<std::uint32_t, MAX_WEAPON_MOUNTS> gunCooldown{};
    std::array<std::uint32_t, MAX_WEAPON_MOUNTS> missileCooldown{};

    // Which weapons the mount cooldowns above were last phased for, one entry
    // per line the trigger works. A swap between primaries has to re-deal
    // them, and comparing the def pointers is enough: the catalog owns them
    // for the process's life.
    std::array<const void*, MAX_PRIMARY_LINES> firingWeaponIds{};

    // Last tick's trigger states, so the rising edge can be spotted. That edge
    // is the only moment the phases above are dealt out; from then on each
    // mount reloads its own cycle and the spacing maintains itself.
    bool firePrimaryWasHeld : 1 = false;
    bool fireMissileWasHeld : 1 = false;
    // One-shot, unlike actionFlags: ResearchSystem clears it the tick it acts
    // on it, so one click can't spend two purchases.
    TechPick techPick;

    // The capacitor is a bank, not a timer (see UpgradeDef::Capacitor).
    // `capacitorSpent` is how much of ShipStats::capacitorCharge has been drawn
    // out, so zero is full and a fresh hull needs no initialising. It only
    // rises while something is actually drawing -- the injector feeding a lit
    // engine, and in time a laser holding its trigger -- which is what makes a
    // tap cost a tap: letting go, or holding the button while coasting, spends
    // nothing.
    float capacitorSpent = 0.f;
    // Whether the overburn is actually running this tick -- what the movement
    // integrator, the wire (PackControlFlags) and the exhaust all read, as
    // opposed to actionFlags.boost, which is only the request.
    bool boosting = false;
    // The same distinction for the beams: the trigger is a request, and this
    // is what the bank actually granted. DamageSystem burns whatever this says
    // is burning, and it is what travels so a peer draws real beams only.
    bool laserFiring = false;
    // Ticks of emitter charge still to run, and then of burn owed whatever the
    // trigger does next -- the two halves of one commitment, which is why they
    // are counters here rather than anything derived from the trigger. Nonzero
    // `laserWindup` is a charge nothing can call off; nonzero `laserBurnOwed`
    // is a beam that keeps burning after a released trigger. See
    // ShipControlsSystem::AdvanceCapacitor.
    std::uint16_t laserWindup = 0;
    std::uint16_t laserBurnOwed = 0;
};

} // namespace Gravitaris
