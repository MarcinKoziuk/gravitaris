#pragma once

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Updates every ship's LandingState each tick: landed = resting on a planet
// (live contact, slow relative to it, upright). ConquestSystem consumes the
// result for claiming; Phase 4's respawn rule reads lastFriendlySiteNetId.
class LandingStateSystem {
public:
    // Relative speed (world units/s) below which contact counts as resting
    // rather than an impact still in progress. Well under DamageSystem's
    // UPRIGHT_SAFE_DELTAV (90) so "landed" always implies "took no damage";
    // also the HUD's "safe to land" threshold.
    static constexpr double SAFE_LANDING_SPEED = 20.0;

private:
    flecs::world& m_registry;

    PhysicsSystem& m_physicsSystem;
    FactionSystem& m_factionSystem;

public:
    LandingStateSystem(flecs::world& registry, PhysicsSystem& physicsSystem, FactionSystem& factionSystem);

    void Update();
};

} // namespace Gravitaris
