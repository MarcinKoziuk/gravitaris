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
};

} // namespace Gravitaris
