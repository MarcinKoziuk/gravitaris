#pragma once

#include <cstdint>
#include <optional>

#include <flecs.h>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/fwd.hpp>

struct cpShape;

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
    // threshold does nothing; above it, damage scales linearly. A tipped-over
    // landing takes `tippedMultiplier` on top of that.
    struct LandingParams {
        double uprightThreshold = 70.0; // free impact speed with the legs down
        double tippedThreshold = 70.0;  // free impact speed when tipped over
        double damagePerDeltaV = 0.6;   // hp per unit of speed over the threshold
        float tippedMultiplier = 3.0f;
    };

private:
    flecs::world& m_registry;
    PhysicsSystem& m_physicsSystem;
    GameEventQueue& m_eventQueue;
    const UpgradeCatalog& m_catalog;

    LandingParams m_landingParams;

    void ResolveShipRams();

    // Spends `target`'s shield charge (if it carries one) against `damage`
    // and returns what still reaches the hull. Weapon hits only: a shield is
    // a defense against fire, not a cushion for flying into a mountain, so
    // landing and ram damage bypass it. `element` is the shield shape the
    // query actually struck (PhysicsBody::SHIELD_BUBBLE, or a plate index),
    // or nullopt when the round reached bare hull.
    float AbsorbWithShield(flecs::entity target, float damage, const Magnum::Vector2& at,
                           std::optional<std::uint8_t> element);

    // Which of `ent`'s shield shapes `shape` is, or nullopt for plain hull.
    [[nodiscard]] std::optional<std::uint8_t> ShieldElementFor(flecs::entity ent, const cpShape* shape);

    // Whether `ent`'s model authors any shield geometry at all -- what tells a
    // round through a gap apart from a hull that simply has no plates drawn.
    [[nodiscard]] bool HasShieldGeometry(flecs::entity ent);

public:
    DamageSystem(flecs::world& registry, PhysicsSystem& physicsSystem, GameEventQueue& eventQueue,
                 const UpgradeCatalog& catalog);

    LandingParams& GetLandingParams() { return m_landingParams; }

    void Update();
};

} // namespace Gravitaris
