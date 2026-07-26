#pragma once

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Resolves bullet hits. Bullets are Chipmunk sensors (see RigidBodyDesc), so
// hits aren't detected by Chipmunk's own collision resolution; instead each
// tick this sweeps a segment query from the bullet's previous to current
// position, which stays correct regardless of bullet speed (no tunneling).
//
// Also owns the two non-bullet damage sources, both drained from
// PhysicsSystem: ordinary hard contacts (landing/crash) and ship-against-ship
// rams, which are a gameplay rule rather than a physical one (see
// ResolveShipRams and networking-plan Phase 9).
class DamageSystem {
public:
    // Landing/ram damage tuning. Impact speed (deltaV) below the applicable
    // threshold does nothing; above it, damage scales linearly. Uprightness
    // matters more than speed: an upright landing shrugs off a hard touchdown,
    // while a tipped-over one starts hurting at a much lower speed and takes
    // `tippedMultiplier` on top.
    struct LandingParams {
        double uprightThreshold = 30.0; // free impact speed with the legs down
        double tippedThreshold = 12.0;  // free impact speed when tipped over
        double damagePerDeltaV = 0.6;   // hp per unit of speed over the threshold
        float tippedMultiplier = 3.0f;
    };

private:
    flecs::world& m_registry;
    PhysicsSystem& m_physicsSystem;
    GameEventQueue& m_eventQueue;

    LandingParams m_landingParams;

    void ResolveShipRams();

public:
    DamageSystem(flecs::world& registry, PhysicsSystem& physicsSystem, GameEventQueue& eventQueue);

    LandingParams& GetLandingParams() { return m_landingParams; }

    void Update();
};

} // namespace Gravitaris
