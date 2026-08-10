#pragma once

#include <cstdint>
#include <optional>

#include <flecs.h>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/fwd.hpp>

struct cpShape;
struct cpSpace;

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

    // Off by default: a side's rounds and hulls pass through each other. A
    // round-wide rule rather than per-ship, so both the bullet sweep and the
    // ram resolver read the one flag.
    bool m_friendlyFire = false;

    // Counts every hit a shield has resolved, so two rounds landing on the
    // same plate on the same tick roll separately. Sim state like any other:
    // it only advances inside Update, so a replay walks it identically. Never
    // replicated -- only the server resolves damage.
    std::uint32_t m_leakSeq = 0;

    void ResolveShipRams();

    // Burns every beam that the capacitor lit this tick (Controls::laserFiring)
    // and applies what it reaches. A beam is not a projectile and has no entity
    // of its own: it is a ray from each armed mount, resolved fresh every tick,
    // stopping at the first damageable thing along it -- so it lands through
    // the same shield/plate path a round does, and cover works against it the
    // same way.
    //
    // Damage falls off with distance to nothing at the weapon's reach, which is
    // the whole character of the weapon: it rewards closing, and a beam fired
    // across the sector is a light show. `step` throttles the hit events, since
    // one per tick per beam would drown the queue describing a condition the
    // wire already carries.
    void ResolveBeams(std::uint64_t step);

    // A star cooks whatever comes near it, and destroys outright whatever
    // reaches the disc. A rule, not a physical result: a sun is not a place
    // you land badly, it is a place nothing comes back from, so the contact
    // half bypasses the landing curve, the hull's fragility and any shield.
    // The heat half does go through the shield -- carrying one is what buys a
    // few more seconds in the corona.
    void ResolveStarContact();

    // Spends `target`'s shield against radiated heat, which soaks the whole
    // hull rather than striking one face of it, and returns what still
    // reaches the hull.
    float AbsorbHeatWithShield(flecs::entity target, float damage);

    // Spends `target`'s shield charge (if it carries one) against `damage`
    // and returns what still reaches the hull. Weapon hits only: a shield is
    // a defense against fire, not a cushion for flying into a mountain, so
    // landing and ram damage bypass it. `element` is the shield shape the
    // query actually struck (PhysicsBody::SHIELD_BUBBLE, or a plate index),
    // or nullopt when the round reached bare hull.
    float AbsorbWithShield(std::uint64_t step, flecs::entity target, float damage,
                           const Magnum::Vector2& at, std::optional<std::uint8_t> element);

    // The first thing along a line that can actually be hurt, or a dead entity
    // if the line meets nothing. Shared by the bullet sweep and the beams so
    // the two cannot come to disagree about what counts as a hit -- the
    // shooter's own hull, a friendly, a planet and a spent shield plate are all
    // passed through, and the nearest of what is left wins.
    struct HitSearch {
        DamageSystem* self = nullptr;
        flecs::entity ignore; // the round itself; nothing for a beam
        TeamId team = TeamId::Blue;
        bool friendlyFire = false;
        flecs::entity_t shooter = 0;

        flecs::entity target;
        Magnum::Vector2 point{};
        // Fraction along the queried segment, so a caller that cares about
        // range (a beam's falloff does) can recover the distance.
        double alpha = 0.;
        std::optional<std::uint8_t> element;
    };
    void QueryFirstHit(cpSpace* space, const Magnum::Vector2d& from, const Magnum::Vector2d& to,
                       double radius, HitSearch& search);

    // Which of `ent`'s shield shapes `shape` is, or nullopt for plain hull.
    [[nodiscard]] std::optional<std::uint8_t> ShieldElementFor(flecs::entity ent, const cpShape* shape);

    // Whether `ent`'s model authors any shield geometry at all -- what tells a
    // round through a gap apart from a hull that simply has no plates drawn.
    [[nodiscard]] bool HasShieldGeometry(flecs::entity ent);

public:
    DamageSystem(flecs::world& registry, PhysicsSystem& physicsSystem, GameEventQueue& eventQueue,
                 const UpgradeCatalog& catalog);

    LandingParams& GetLandingParams() { return m_landingParams; }

    void SetFriendlyFire(bool enabled) { m_friendlyFire = enabled; }
    [[nodiscard]] bool IsFriendlyFire() const { return m_friendlyFire; }

    // `step` seeds the plating leak roll -- sim state in, outcome out, so a
    // replay and a second peer leak on the same rounds (ADR 0001).
    void Update(std::uint64_t step);
};

} // namespace Gravitaris
