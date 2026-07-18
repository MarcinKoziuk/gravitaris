#pragma once

#include <vector>
#include <unordered_map>

#include <flecs.h>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/transform.hpp>

#include <gravitaris/game/util/chipmunk-safe.hpp>

namespace Gravitaris {

// Chipmunk state for one entity. Owned by PhysicsSystem, not stored in the
// ECS: cpSpace holds raw pointers into this, so it must never be relocated
// by archetype moves, and it can be bulk-freed per space on level unload
// (see docs/adr/0002-physics-ownership.md).
struct PhysicsBody {
    id_t spaceId{};
    ResourcePtr<Body> body;
    std::uint32_t generation = 0;

    // Resource-authored mass, captured at InitBody time -- the reference point
    // SetMassMultiplier scales from, so repeated calls with a changing
    // multiplier never compound.
    cpFloat baseMass = 0.0;

    struct {
        std::shared_ptr<cpSpace> space;
        cpBodyUniquePtr body;
        std::vector<cpShapeUniquePtr> shapes;
    } cp;

    [[nodiscard]] bool IsAlive() const { return cp.body != nullptr; }
};

// A resolved hard contact affecting one body, recorded during the physics
// step and drained afterward by DamageSystem (mutating the ECS mid-step is
// unsafe). deltaV = collision impulse / body mass, i.e. the speed change the
// impact imparted; upright is whether the body's legs faced the contact.
struct ImpactEvent {
    flecs::entity_t entity;
    double deltaV;
    bool upright;
    Magnum::Vector2d contact; // world-space contact point (for the LandingCrash event)
};

class PhysicsSystem {
public:
    // Newtonian F = G*m1*m2/d^2. ApplyGravity attracts every dynamic body
    // toward each GravitySource. Public so TrajectoryPredictor integrates
    // against the exact same field.
    static constexpr double GRAVITY_CONSTANT = 20.0;

    // Shared filter group for bullet (sensor) shapes so a bullet's swept
    // segment query skips other bullets rather than being blocked by them.
    static constexpr cpGroup BULLET_GROUP = 1;

private:
    flecs::world& m_registry;

    std::unordered_map<id_t, std::shared_ptr<cpSpace>> m_spaces;

    // Slot storage + free-list; PhysicsRef indexes into m_bodies. Declared
    // after m_spaces so slots (whose deleters may touch their space) are
    // destroyed first.
    std::vector<PhysicsBody> m_bodies;
    std::vector<std::uint32_t> m_freeList;

    // Hard contacts accumulated during Simulate's cpSpaceStep (see
    // PostSolveImpact), drained by DamageSystem the same tick.
    std::vector<ImpactEvent> m_impacts;

    flecs::observer m_bodyAddedObserver;
    flecs::observer m_bodyRemovedObserver;

    float m_gravityMultiplier = 1.667f; // this game's tuned default; 1 = GRAVITY_CONSTANT unmodified

    void InitSpace(id_t spaceId);

    // Chipmunk postSolve callback (wildcard). Records ImpactEvents for the
    // colliding bodies; static because Chipmunk calls it with C linkage.
    static void PostSolveImpact(cpArbiter* arb, cpSpace* space, cpDataPointer userData);

    void RecordImpact(cpShape* shape, cpBody* body, cpFloat impulse, cpVect contact);

    void InitBody(PhysicsBody& slot, const Transform& transf);

    std::uint32_t Allocate();

    void ApplyGravity(id_t spaceId);

    void HandleBodyAdded(flecs::entity ent, const RigidBodyDesc& desc);

    void HandleBodyRemoved(const PhysicsRef& ref);

public:
    explicit PhysicsSystem(flecs::world& registry);

    ~PhysicsSystem();

    [[nodiscard]] PhysicsBody& GetBody(const PhysicsRef& ref);

    // Maps a Chipmunk shape back to its owning entity (tagged at shape
    // creation, see HandleBodyAdded), for segment-query hit detection.
    // Returns a dead/invalid entity for an untagged or stale shape.
    [[nodiscard]] flecs::entity GetEntityForShape(const cpShape* shape);

    // Arena-style level teardown: frees every body/shape in the space and
    // the space itself in one pass, skipping the per-object cpSpaceRemove*
    // the deleters do (pointless when the whole space dies). Stale
    // PhysicsRefs on still-live entities become no-ops via the generation.
    void UnloadSpace(id_t spaceId);

    // Drives a kinematic body directly (position + velocity), for externally
    // scripted motion like OrbitSystem's pre-calculated orbits. No effect on
    // dynamic bodies' integration -- meant for CP_BODY_TYPE_KINEMATIC only.
    void SetKinematicMotion(const PhysicsRef& ref, Magnum::Vector2d pos, Magnum::Vector2d vel);

    void Simulate(double dt);

    void Update();

    // Moves out the impacts recorded during the last Simulate; caller applies
    // damage from them. Leaves the buffer empty for the next step.
    [[nodiscard]] std::vector<ImpactEvent> DrainImpacts();

    // --- Debug/tuning knobs (temporary, for calibrating gameplay feel) ---

    // Scales the force ApplyGravity computes for every source/target pair.
    // 1 = unmodified (GRAVITY_CONSTANT as authored).
    void SetGravityMultiplier(float multiplier) { m_gravityMultiplier = multiplier; }
    [[nodiscard]] float GetGravityMultiplier() const { return m_gravityMultiplier; }

    // Sets a body's live Chipmunk mass to its resource-authored base mass
    // (captured at spawn) times `multiplier`. 1 = unmodified. Safe to call
    // every frame with the same value -- it recomputes from the stored base
    // each time rather than compounding, and a fresh body from a respawn
    // picks up whatever the caller applies next.
    void SetMassMultiplier(const PhysicsRef& ref, float multiplier);
};

} // namespace Gravitaris
