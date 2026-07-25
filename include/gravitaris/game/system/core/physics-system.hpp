#pragma once

#include <optional>
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

// A ship-against-ship contact fast enough to count as a ram rather than a
// harmless overlap (networking-plan Phase 9). Recorded during the step and
// drained by DamageSystem, which owns the destroy rule -- unlike ImpactEvent
// this names *both* parties, since that rule compares them (and has to check
// their teams) rather than damaging one in isolation. Masses are captured
// here because the bodies are already at hand; `closingSpeed` is their
// approach speed along the contact normal, sampled pre-solve.
struct ShipRamEvent {
    flecs::entity_t a;
    flecs::entity_t b;
    double closingSpeed;
    double massA;
    double massB;
    Magnum::Vector2d contact;
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

    // Chipmunk collision type for CollisionClass::Ship shapes. 0 (every other
    // body) is Chipmunk's own default, so only the ship<->ship pair gets a
    // specific handler and every other combination keeps falling through to
    // the wildcard one.
    static constexpr cpCollisionType SHIP_COLLISION_TYPE = 1;

    // Runtime-tunable ship-against-ship rule (networking-plan Phase 9). Real
    // values want playtesting, hence the debug panel rather than constants.
    struct ShipContactParams {
        // Approach speed along the contact normal at or above which a
        // contact is a ram instead of an overlap.
        double ramClosingSpeed = 55.0;
        // Acceleration each overlapping ship gets along the contact normal,
        // easing the pair apart. 0 = pure pass-through. Deliberately applied
        // as a force before the next step rather than inside the solver: an
        // impulse in the contact solve is exactly the thing prediction can't
        // reconcile, which is what Phase 9 exists to remove.
        double separationAccel = 90.0;
        // Ram momentum (lighter ship's mass x closing speed) past which both
        // ships die outright, however tough either is.
        double bothDieMomentum = 900.0;
        // Survivor damage per unit of the loser's mass x closing speed.
        double survivorDamageScale = 0.02;
    };

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

    ShipContactParams m_shipContact;

    // Ship pairs found overlapping during the last step, and the separating
    // direction for each. Consumed by Simulate itself (the nudge is applied
    // as a force just before the next cpSpaceStep), not drained by anyone.
    struct ShipOverlap {
        flecs::entity_t a;
        flecs::entity_t b;
        Magnum::Vector2d normal; // world, points from a's body toward b's
    };
    std::vector<ShipOverlap> m_shipOverlaps;

    std::vector<ShipRamEvent> m_shipRams;

    flecs::observer m_bodyAddedObserver;
    flecs::observer m_bodyRemovedObserver;

    float m_gravityMultiplier = 1.667f; // this game's tuned default; 1 = GRAVITY_CONSTANT unmodified

    void InitSpace(id_t spaceId);

    // Chipmunk postSolve callback (wildcard). Records ImpactEvents for the
    // colliding bodies; static because Chipmunk calls it with C linkage.
    static void PostSolveImpact(cpArbiter* arb, cpSpace* space, cpDataPointer userData);

    void RecordImpact(cpShape* shape, cpBody* body, cpFloat impulse, cpVect contact);

    // Chipmunk preSolve callback for the ship<->ship pair specifically.
    // Always returns cpFalse -- two ships never push each other -- after
    // recording the overlap (for the separating nudge) and, once per contact
    // episode, a ram event if they met fast enough.
    static cpBool PreSolveShipPair(cpArbiter* arb, cpSpace* space, cpDataPointer userData);

    // Applies m_shipOverlaps as forces and clears it. Called at the top of
    // Simulate, i.e. against the overlaps the previous step observed.
    void ApplyShipSeparation();

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

    // Drives a kinematic body directly (position + velocity + optional
    // angle), for externally scripted motion like OrbitSystem's
    // pre-calculated orbits. No effect on dynamic bodies' integration --
    // meant for CP_BODY_TYPE_KINEMATIC only. `angle` is left untouched when
    // omitted -- PhysicsSystem::Update() reads the body's angle back into
    // Transform::rot every tick regardless, so a caller that doesn't care
    // about facing needs no extra call.
    void SetKinematicMotion(const PhysicsRef& ref, Magnum::Vector2d pos, Magnum::Vector2d vel,
                            std::optional<double> angle = std::nullopt);

    void Simulate(double dt);

    void Update();

    // Moves out the impacts recorded during the last Simulate; caller applies
    // damage from them. Leaves the buffer empty for the next step.
    [[nodiscard]] std::vector<ImpactEvent> DrainImpacts();

    // Same contract as DrainImpacts, for ship-against-ship rams. Note these
    // never appear in DrainImpacts: a ship pair is handled by its own
    // collision handler, which suppresses the contact before postSolve (and
    // so before any ImpactEvent) can run.
    [[nodiscard]] std::vector<ShipRamEvent> DrainShipRams();

    [[nodiscard]] ShipContactParams& GetShipContactParams() { return m_shipContact; }
    [[nodiscard]] const ShipContactParams& GetShipContactParams() const { return m_shipContact; }

    // Calls `fn` for each entity whose shape is currently touching `ref`'s
    // body (live contact arbiters only -- Chipmunk keeps separated arbiters
    // cached a few steps, those are skipped). For sustained-contact checks
    // like landed-state detection, as opposed to DrainImpacts' one-shot
    // first-contact events.
    void ForEachTouching(const PhysicsRef& ref, void (*fn)(flecs::entity touched, void* ctx), void* ctx);

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
