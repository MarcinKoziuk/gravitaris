#pragma once

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Homing for every live Missile: lock the nearest hostile ship or structure,
// then each tick turn the missile's velocity toward it at a bounded rate
// while it accelerates up to its top speed. Runs inside the physics block,
// before Simulate(), so the steer it applies is what that tick integrates.
//
// Gravity is effectively overridden while a missile is under guidance: the
// speed is re-derived every tick rather than accumulated, so a well only
// bends the flight through the heading it inherits.
class MissileSystem {
public:
    // The homing envelope -- turn rate, acceleration, top speed -- belongs to
    // the WeaponDef that fired the round (data/upgrades.toml), reached through
    // Missile::weaponId, so two missile types can fly differently.
    MissileSystem(flecs::world& registry, EntitySpawner& entitySpawner, PhysicsSystem& physicsSystem,
                  const UpgradeCatalog& catalog);

    void Update();

private:
    flecs::world& m_registry;
    EntitySpawner& m_entitySpawner;
    PhysicsSystem& m_physicsSystem;
    const UpgradeCatalog& m_catalog;
};

} // namespace Gravitaris
