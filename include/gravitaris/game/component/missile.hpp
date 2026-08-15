#pragma once

#include <cstdint>

#include <gravitaris/game/id.hpp>

namespace Gravitaris {

// A guided projectile: carries Bullet too (damage, lifetime, friendly-fire
// team -- everything a bullet needs), and this on top for the homing state
// MissileSystem drives.
//
// Replication class: server-only. Guidance runs server-side and the result is
// already replicated as the missile's own position/rotation, so a client
// never needs to know what it is chasing. Specific player-chosen targeting
// (rather than "nearest hostile") will need that to change.
struct Missile {
    // Locked target's NetId, 0 until acquired. MissileSystem re-acquires when
    // it dies or its team stops being hostile (a planet claim can flip a
    // structure's owner mid-flight).
    std::uint32_t targetNetId = 0;
    // Which WeaponDef fired it, so its own homing envelope (turn rate,
    // acceleration, top speed) travels with the round rather than being one
    // global setting every missile type would have to share.
    id_t weaponId = 0;
    // Ticks since launch, which is what phases the seeker's hunt (see
    // WeaponDef::Guidance::wobble). Counts up regardless of tier, so a round
    // whose wobble is zero simply never reads it.
    std::uint16_t age = 0;
    // What is left of the round's airframe (WeaponDef::hp). Only a beam spends
    // it -- gunfire passes straight through a missile -- and at zero the round
    // comes apart in flight (DamageSystem::ResolveBeams).
    //
    // Here rather than in a Damageable, deliberately: that component is what
    // marks something a target, and every system that walks one would take a
    // missile for a ship -- MissileSystem's own candidates (rounds homing on
    // rounds), the turrets' target list, DeathSystem's explosion and shrapnel,
    // the kill feed, the HUD arrows, the minimap.
    float hp = 0.f;
};

} // namespace Gravitaris
