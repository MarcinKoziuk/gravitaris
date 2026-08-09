#include <algorithm>
#include <cmath>
#include <cassert>
#include <cstdint>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/resource/body.hpp>
#include <gravitaris/game/system/core/physics-system.hpp>

namespace Gravitaris {

static const cpTransform tzero = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };

// A body counts as landing "upright" if its legs (local +Y) point within
// this cosine of the contact direction; below it the landing is a tip-over.
static constexpr cpFloat UPRIGHT_DOT_THRESHOLD = 0.82; // ~35 degrees

// Half-thickness of an ablative plate's collision segment, in model units
// before the entity's scale. Wide enough that a plate stops the shots that
// visually strike it, narrow enough that the gaps between plates stay real
// gaps -- shots through those are meant to reach the hull.
static constexpr cpFloat PLATE_THICKNESS = 0.6;

PhysicsSystem::PhysicsSystem(flecs::world& registry)
    : m_registry(registry)
{
    // OnSet (not OnAdd) mirrors entt's on_construct: it fires once, after the
    // component's value is already assigned -- entity.emplace<RigidBodyDesc>()
    // both adds and constructs in one step, so OnSet is the equivalent point.
    m_bodyAddedObserver = m_registry.observer<RigidBodyDesc>()
            .event(flecs::OnSet)
            .each([this](flecs::entity ent, RigidBodyDesc& desc) { HandleBodyAdded(ent, desc); });

    // OnRemove fires both on explicit component removal and on entity
    // destruction, matching entt's on_destroy.
    m_bodyRemovedObserver = m_registry.observer<PhysicsRef>()
            .event(flecs::OnRemove)
            .each([this](flecs::entity, PhysicsRef& ref) { HandleBodyRemoved(ref); });
}

PhysicsSystem::~PhysicsSystem()
{
    // Explicit teardown: these observers close over `this`, and must not
    // outlive it -- the flecs::world (owned by Game, declared before this
    // system) is destroyed after PhysicsSystem, so without this the world
    // would still fire into a dangling PhysicsSystem during later teardown.
    m_bodyAddedObserver.destruct();
    m_bodyRemovedObserver.destruct();
}

PhysicsBody& PhysicsSystem::GetBody(const PhysicsRef& ref)
{
    PhysicsBody& slot = m_bodies.at(ref.index);
    assert(slot.generation == ref.generation && slot.IsAlive());
    return slot;
}

std::uint32_t PhysicsSystem::Allocate()
{
    if (!m_freeList.empty()) {
        std::uint32_t index = m_freeList.back();
        m_freeList.pop_back();
        return index;
    }

    m_bodies.emplace_back();
    return static_cast<std::uint32_t>(m_bodies.size() - 1);
}

void PhysicsSystem::InitSpace(id_t spaceId)
{
    auto space = std::shared_ptr<cpSpace>(cpSpaceNew(), cpSpaceDeleter());

    // Wildcard handler for landing/ram damage. Sensor pairs (bullets) never
    // reach postSolve, so this only sees real, impulse-carrying contacts.
    cpCollisionHandler* handler = cpSpaceAddDefaultCollisionHandler(space.get());
    handler->postSolveFunc = &PhysicsSystem::PostSolveImpact;
    handler->userData = this;

    // Ship<->ship is the one pair that doesn't resolve as physics
    // (networking-plan Phase 9). A specific handler replaces the wildcard
    // one for this pair only -- ship<->planet and ship<->structure still
    // fall through to the default above, so landing and crash damage are
    // untouched.
    cpCollisionHandler* ships = cpSpaceAddCollisionHandler(space.get(), SHIP_COLLISION_TYPE, SHIP_COLLISION_TYPE);
    ships->preSolveFunc = &PhysicsSystem::PreSolveShipPair;
    ships->userData = this;

    m_spaces.insert(std::make_pair(spaceId, std::move(space)));
}

void PhysicsSystem::PostSolveImpact(cpArbiter* arb, cpSpace*, cpDataPointer userData)
{
    // One event per contact pair, on the step it begins, so a ship resting on
    // a planet under gravity doesn't accrue damage every frame.
    if (!cpArbiterIsFirstContact(arb)) return;

    const cpFloat impulse = cpvlength(cpArbiterTotalImpulse(arb));
    if (impulse <= 0.0) return;

    auto* self = static_cast<PhysicsSystem*>(userData);

    cpShape *shapeA, *shapeB;
    cpArbiterGetShapes(arb, &shapeA, &shapeB);
    cpBody *bodyA, *bodyB;
    cpArbiterGetBodies(arb, &bodyA, &bodyB);
    const cpVect contact = cpArbiterGetPointA(arb, 0);

    self->RecordImpact(shapeA, bodyA, impulse, contact);
    self->RecordImpact(shapeB, bodyB, impulse, contact);
}

void PhysicsSystem::RecordImpact(cpShape* shape, cpBody* body, cpFloat impulse, cpVect contact)
{
    const cpFloat mass = cpBodyGetMass(body);
    if (mass <= 0.0) return;

    // Legs are local +Y (thrust pushes local -Y); a landing is upright when
    // they point toward the surface the body just hit.
    const cpVect legs = cpvrotate(cpBodyGetRotation(body), cpv(0.0, 1.0));
    const cpVect toContact = cpvnormalize(cpvsub(contact, cpBodyGetPosition(body)));
    const bool upright = cpvdot(legs, toContact) > UPRIGHT_DOT_THRESHOLD;

    const auto raw = reinterpret_cast<std::uintptr_t>(cpShapeGetUserData(shape));
    m_impacts.push_back(ImpactEvent{static_cast<flecs::entity_t>(raw), impulse / mass, upright,
                                    Magnum::Vector2d{contact.x, contact.y}});
}

std::vector<ImpactEvent> PhysicsSystem::DrainImpacts()
{
    std::vector<ImpactEvent> out = std::move(m_impacts);
    m_impacts.clear();
    return out;
}

cpBool PhysicsSystem::PreSolveShipPair(cpArbiter* arb, cpSpace*, cpDataPointer userData)
{
    auto* self = static_cast<PhysicsSystem*>(userData);

    cpShape *shapeA, *shapeB;
    cpArbiterGetShapes(arb, &shapeA, &shapeB);
    cpBody *bodyA, *bodyB;
    cpArbiterGetBodies(arb, &bodyA, &bodyB);

    const auto entityFor = [](cpShape* shape) {
        return static_cast<flecs::entity_t>(reinterpret_cast<std::uintptr_t>(cpShapeGetUserData(shape)));
    };
    const flecs::entity_t entA = entityFor(shapeA);
    const flecs::entity_t entB = entityFor(shapeB);

    // Normal points from A toward B, so A separates along -n and B along +n.
    const cpVect normal = cpArbiterGetNormal(arb);

    // One nudge per ship pair per step, not per overlapping shape pair.
    const bool overlapRecorded =
            std::any_of(self->m_shipOverlaps.begin(), self->m_shipOverlaps.end(),
                        [&](const ShipOverlap& o) { return SamePair(o.a, o.b, entA, entB); });
    if (!overlapRecorded) {
        self->m_shipOverlaps.push_back(ShipOverlap{entA, entB, Magnum::Vector2d{normal.x, normal.y}});
    }

    // Velocities here are pre-solve, i.e. the speed they actually met at.
    // Positive = approaching.
    const cpFloat closing = cpvdot(cpvsub(cpBodyGetVelocity(bodyA), cpBodyGetVelocity(bodyB)), normal);
    if (closing < self->m_shipContact.ramClosingSpeed || cpArbiterGetCount(arb) == 0) return cpFalse;

    const bool alreadyRammed =
            std::any_of(self->m_recentRams.begin(), self->m_recentRams.end(),
                        [&](const RecentRam& r) { return SamePair(r.a, r.b, entA, entB); });
    if (!alreadyRammed) {
        self->m_recentRams.push_back(RecentRam{entA, entB, self->m_stepIndex});
        const cpVect contact = cpArbiterGetPointA(arb, 0);
        self->m_shipRams.push_back(ShipRamEvent{entA, entB, closing, cpBodyGetMass(bodyA), cpBodyGetMass(bodyB),
                                                Magnum::Vector2d{contact.x, contact.y}});
    }

    // Never resolve the contact: no impulse, no bounce, nothing for client
    // prediction to disagree with the server about.
    return cpFalse;
}

void PhysicsSystem::ApplyShipSeparation()
{
    ++m_stepIndex;
    m_recentRams.erase(std::remove_if(m_recentRams.begin(), m_recentRams.end(),
                                      [&](const RecentRam& r) {
                                          return m_stepIndex - r.step > RAM_COOLDOWN_STEPS;
                                      }),
                       m_recentRams.end());

    if (m_shipContact.separationAccel > 0.0) {
        for (const ShipOverlap& overlap : m_shipOverlaps) {
            const cpVect dir = cpv(overlap.normal.x(), overlap.normal.y());
            const auto push = [&](flecs::entity_t raw, cpFloat sign) {
                flecs::entity entity(m_registry, raw);
                if (!entity.is_alive()) return;
                const PhysicsRef* ref = entity.try_get<PhysicsRef>();
                if (!ref) return;
                cpBody* body = GetBody(*ref).cp.body.get();
                const cpFloat mass = cpBodyGetMass(body);
                if (!std::isfinite(mass) || mass <= 0.0) return; // kinematic proxy: nothing to push
                // At the centre of gravity, so separating never also spins
                // the ship.
                cpBodyApplyForceAtLocalPoint(
                        body, cpvmult(dir, sign * m_shipContact.separationAccel * mass), cpvzero);
            };
            push(overlap.a, -1.0);
            push(overlap.b, 1.0);
        }
    }
    m_shipOverlaps.clear();
}

std::vector<ShipRamEvent> PhysicsSystem::DrainShipRams()
{
    std::vector<ShipRamEvent> out = std::move(m_shipRams);
    m_shipRams.clear();
    return out;
}

void PhysicsSystem::ForEachTouching(const PhysicsRef& ref, void (*fn)(flecs::entity, void*), void* ctx)
{
    struct Iter {
        PhysicsSystem* self;
        cpBody* body;
        void (*fn)(flecs::entity, void*);
        void* ctx;
    };

    PhysicsBody& slot = GetBody(ref);
    if (!slot.IsAlive()) return;

    Iter iter{this, slot.cp.body.get(), fn, ctx};
    cpBodyEachArbiter(iter.body, [](cpBody*, cpArbiter* arb, void* data) {
        // A cached-but-separated arbiter has no contact points.
        if (cpArbiterGetCount(arb) == 0) return;

        auto* it = static_cast<Iter*>(data);
        cpShape *shapeA, *shapeB;
        cpArbiterGetShapes(arb, &shapeA, &shapeB);
        // A shield sensor overlapping something is not the hull resting on it:
        // without this a ship whose bubble merely grazed a planet would read
        // as landed.
        if (cpShapeGetSensor(shapeA) || cpShapeGetSensor(shapeB)) return;
        // cpBodyEachArbiter presents the arbiter with the iterated body as A.
        flecs::entity touched = it->self->GetEntityForShape(shapeB);
        if (touched.is_alive()) it->fn(touched, it->ctx);
    }, &iter);
}

void PhysicsSystem::SetMassMultiplier(const PhysicsRef& ref, float multiplier)
{
    PhysicsBody& slot = GetBody(ref);
    if (slot.baseMass <= 0.0) return;
    cpBodySetMass(slot.cp.body.get(), slot.baseMass * multiplier);
}

void PhysicsSystem::SetKinematicMotion(const PhysicsRef& ref, Magnum::Vector2d pos, Magnum::Vector2d vel,
                                       std::optional<double> angle, double angularVel)
{
    cpBody* body = GetBody(ref).cp.body.get();
    cpBodySetPosition(body, cpv(pos.x(), pos.y()));
    cpBodySetVelocity(body, cpv(vel.x(), vel.y()));
    if (angle) {
        cpBodySetAngle(body, cpFloat(*angle));
        cpBodySetAngularVelocity(body, cpFloat(angularVel));
    }
}

void PhysicsSystem::Teleport(const PhysicsRef& ref, Magnum::Vector2d pos, Magnum::Vector2d vel)
{
    cpBody* body = GetBody(ref).cp.body.get();
    cpBodySetPosition(body, cpv(pos.x(), pos.y()));
    cpBodySetVelocity(body, cpv(vel.x(), vel.y()));
    cpBodySetAngularVelocity(body, 0.0);
}

void PhysicsSystem::InitBody(PhysicsBody& slot, const Transform& transf)
{
    const Body& bodyResource = *slot.body;
    cpSpace* space = m_spaces.at(slot.spaceId).get();

    const bool kinematic = bodyResource.IsKinematic();
    slot.cp.body.reset(kinematic ? cpBodyNewKinematic() : cpBodyNew(1.0, 1.0));
    slot.cp.space = m_spaces.at(slot.spaceId);

    cpBody* body = slot.cp.body.get();

    cpFloat moment = 0.0;
    cpFloat mass = bodyResource.GetMass();
    // A kinematic body has no meaningful physical mass; leave baseMass 0 so
    // SetMassMultiplier no-ops on it. Its gravitational pull comes from a
    // GravitySource component instead.
    slot.baseMass = kinematic ? 0.0 : mass;
    for (const auto& poly : bodyResource.GetPolygonShapes()) {
        moment += cpMomentForPoly(
                mass,
                static_cast<int>(poly.size()),
                reinterpret_cast<const cpVect*>(&poly.front()),
                cpvzero,
                0.0
        );

        cpTransform trans = cpTransformIdentity;
        trans = cpTransformMult(trans, cpTransformScale(transf.scale.x(), transf.scale.y()));

        cpShape* shape = cpPolyShapeNew(
                body,
                static_cast<int>(poly.size()),
                reinterpret_cast<const cpVect*>(&poly.front()),
                trans,
                0.0
        );

        slot.cp.shapes.emplace_back(cpShapeUniquePtr(shape));
    }
    for (const Body::CircleShape& circle : bodyResource.GetCircleShapes()) {
        const cpVect offs = cpVect(circle.pos * transf.scale);
        moment += cpMomentForCircle(mass, 0.f, circle.radius, offs);
        cpShape* shape = cpCircleShapeNew(body, circle.radius * transf.scale.x(), offs);
        slot.cp.shapes.emplace_back(cpShapeUniquePtr(shape));
    }

    if (!kinematic) {
        if (moment != 0.) cpBodySetMoment(body, moment);
        if (mass != 0.) cpBodySetMass(body, mass);
    }

    cpSpaceAddBody(space, body);
    for (auto& it : slot.cp.shapes) {
        cpShape* shape = it.get();

        const cpFloat friction = slot.body->GetFriction();
        if (friction != 0.) {
            cpShapeSetFriction(shape, friction);
        }
        else {
            cpShapeSetFriction(shape, 0.05);
        }

        cpSpaceAddShape(space, shape);
    }

    InitShieldShapes(slot, transf);

    cpBodySetAngle(body, cpFloat(transf.rot));
    cpBodySetPosition(body, cpv(transf.pos.x(), transf.pos.y()));

    cpBodySetVelocity(body, cpVect(transf.vel));
}

void PhysicsSystem::InitShieldShapes(PhysicsBody& slot, const Transform& transf)
{
    const Body& bodyResource = *slot.body;
    cpSpace* space = m_spaces.at(slot.spaceId).get();
    cpBody* body = slot.cp.body.get();

    const auto add = [&](cpShape* shape, std::uint8_t plate) {
        // Sensor: it reports overlaps but resolves none, so it can never push
        // the hull around, reach postSolve, or be mistaken for a landing.
        cpShapeSetSensor(shape, cpTrue);
        slot.shieldShapes.push_back(PhysicsBody::ShieldShape{shape, plate});
        slot.cp.shapes.emplace_back(cpShapeUniquePtr(shape));
        cpSpaceAddShape(space, shape);
    };

    const std::vector<TVector2<cpFloat>>& outline = bodyResource.GetShieldOutline();
    if (outline.size() >= 3) {
        cpTransform trans = cpTransformScale(transf.scale.x(), transf.scale.y());
        add(cpPolyShapeNew(body, static_cast<int>(outline.size()),
                           reinterpret_cast<const cpVect*>(outline.data()), trans, 0.0),
            PhysicsBody::SHIELD_BUBBLE);
    }

    const std::vector<Body::Plate>& plates = bodyResource.GetPlates();
    for (std::size_t i = 0; i < plates.size(); ++i) {
        const Body::Plate& plate = plates[i];
        for (std::size_t j = 0; j + 1 < plate.size(); ++j) {
            const cpVect a = cpVect(plate[j] * transf.scale);
            const cpVect b = cpVect(plate[j + 1] * transf.scale);
            add(cpSegmentShapeNew(body, a, b, PLATE_THICKNESS * transf.scale.x()),
                static_cast<std::uint8_t>(i));
        }
    }
}

void PhysicsSystem::HandleBodyAdded(flecs::entity ent, const RigidBodyDesc& desc)
{
    const auto& transf = ent.get<Transform>();

    if (!m_spaces.count(desc.spaceId)) {
        InitSpace(desc.spaceId);
    }

    const std::uint32_t index = Allocate();
    PhysicsBody& slot = m_bodies[index];
    slot.spaceId = desc.spaceId;
    slot.body = desc.body;

    InitBody(slot, transf);

    // Tag each shape with its owning entity (for GetEntityForShape) and
    // apply the sensor flag requested by the spawner (see RigidBodyDesc).
    for (auto& shapePtr : slot.cp.shapes) {
        cpShape* shape = shapePtr.get();
        cpShapeSetUserData(shape, reinterpret_cast<void*>(static_cast<std::uintptr_t>(ent.id())));
        // A shield sensor deliberately keeps the default collision type: as a
        // ship shape it would drive PreSolveShipPair, so two ships whose
        // bubbles merely brushed would separate and register a ram.
        const bool shield = slot.ShieldElementOf(shape).has_value();
        if (desc.collisionClass == CollisionClass::Ship && !shield) {
            cpShapeSetCollisionType(shape, SHIP_COLLISION_TYPE);
        }
        if (desc.sensor) {
            cpShapeSetSensor(shape, cpTrue);
            cpShapeSetFilter(shape, cpShapeFilterNew(BULLET_GROUP, CP_ALL_CATEGORIES, CP_ALL_CATEGORIES));
        }
    }

    // Seed prevPos so a freshly spawned body's first swept query (see
    // DamageSystem) is zero-length until Update() has synced a real motion.
    ent.get_mut<Transform>().prevPos = transf.pos;

    ent.set<PhysicsRef>({index, slot.generation});
}

flecs::entity PhysicsSystem::GetEntityForShape(const cpShape* shape)
{
    const auto raw = reinterpret_cast<std::uintptr_t>(cpShapeGetUserData(shape));
    return flecs::entity(m_registry, static_cast<flecs::entity_t>(raw));
}

void PhysicsSystem::HandleBodyRemoved(const PhysicsRef& ref)
{
    PhysicsBody& slot = m_bodies.at(ref.index);

    // Stale ref: the slot was already bulk-freed by UnloadSpace (or recycled
    // since). Nothing to do.
    if (slot.generation != ref.generation || !slot.IsAlive()) {
        return;
    }

    // Individual teardown: the space stays alive, so the deleters' per-object
    // cpSpaceRemove* is required here.
    slot.shieldShapes.clear();
    slot.cp.shapes.clear();
    slot.cp.body.reset();
    slot.cp.space.reset();
    slot.body = {};

    slot.generation++;
    m_freeList.push_back(ref.index);
}

void PhysicsSystem::UnloadSpace(id_t spaceId)
{
    for (std::uint32_t i = 0; i < m_bodies.size(); ++i) {
        PhysicsBody& slot = m_bodies[i];
        if (!slot.IsAlive() || slot.spaceId != spaceId) continue;

        // The whole space dies right after: free raw, skipping the deleters'
        // per-object cpSpaceRemove*.
        for (auto& shape : slot.cp.shapes) {
            cpShapeFree(shape.release());
        }
        slot.shieldShapes.clear();
        slot.cp.shapes.clear();
        cpBodyFree(slot.cp.body.release());
        slot.cp.space.reset();
        slot.body = {};

        slot.generation++;
        m_freeList.push_back(i);
    }

    m_spaces.erase(spaceId);
}

void PhysicsSystem::ApplyGravity(id_t spaceId)
{
    // Gravity is sources -> targets, not all-pairs: only GravitySource bodies
    // (stars/planets) attract, and only dynamic bodies (ships/debris) are
    // pulled. Kinematic sources report infinite Chipmunk mass, so their
    // gravitational mass comes from the component, not cpBodyGetMass.
    struct Source {
        cpVect pos;
        cpFloat mass;
    };
    std::vector<Source> sources;

    m_registry.each([&](flecs::entity, const GravitySource& gs, PhysicsRef& ref) {
        PhysicsBody& slot = GetBody(ref);
        if (slot.spaceId != spaceId) return;
        sources.push_back({cpBodyGetPosition(slot.cp.body.get()),
                           gs.mass * static_cast<cpFloat>(gs.multiplier)});
    });

    if (sources.empty()) return;

    m_registry.each([&](flecs::entity ent, PhysicsRef& ref) {
        PhysicsBody& slot = GetBody(ref);
        if (slot.spaceId != spaceId || ent.has<Bullet>()) return;
        cpBody* body = slot.cp.body.get();
        if (cpBodyGetType(body) != CP_BODY_TYPE_DYNAMIC) return;

        const cpVect tpos = cpBodyGetPosition(body);
        const cpFloat tmass = cpBodyGetMass(body);
        cpVect total = cpvzero;
        for (const Source& src : sources) {
            const cpVect d = cpvsub(src.pos, tpos);
            const cpFloat dist2 = cpvlengthsq(d);
            if (dist2 < 1e-6) continue;
            const cpFloat f = GRAVITY_CONSTANT * m_gravityMultiplier * (tmass * src.mass) / dist2;
            total = cpvadd(total, cpvmult(d, f / std::sqrt(dist2)));
        }

        cpBodyApplyForceAtWorldPoint(body, total, cpvadd(tpos, cpBodyGetCenterOfGravity(body)));
    });
}

void PhysicsSystem::Simulate(double dt)
{
    // Against the overlaps the *previous* step observed -- forces have to be
    // applied before cpSpaceStep integrates them, and the overlaps aren't
    // known until it has run.
    ApplyShipSeparation();

    for (const auto& p : m_spaces) {
        cpSpace* space = p.second.get();
        cpSpaceStep(space, dt);

        ApplyGravity(p.first);
    }
}

void PhysicsSystem::Update()
{
    m_registry.each([this](flecs::entity, Transform& transf, PhysicsRef& ref) {
        cpBody* body = GetBody(ref).cp.body.get();
        transf.prevPos = transf.pos;
        transf.pos = Vector2d(cpBodyGetPosition(body));
        transf.rot = Radd(cpvtoangle(cpBodyGetRotation(body)));
        transf.vel = Vector2d(cpBodyGetVelocity(body));
        transf.angVel = cpBodyGetAngularVelocity(body);
    });
}

} // namespace Gravitaris
