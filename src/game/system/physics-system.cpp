#include <cmath>
#include <cassert>
#include <cstdint>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/system/physics-system.hpp>

namespace Gravitaris {

static const cpTransform tzero = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };

// A body counts as landing "upright" if its legs (local +Y) point within
// this cosine of the contact direction; below it the landing is a tip-over.
static constexpr cpFloat UPRIGHT_DOT_THRESHOLD = 0.82; // ~35 degrees

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
    m_impacts.push_back(ImpactEvent{static_cast<flecs::entity_t>(raw), impulse / mass, upright});
}

std::vector<ImpactEvent> PhysicsSystem::DrainImpacts()
{
    std::vector<ImpactEvent> out = std::move(m_impacts);
    m_impacts.clear();
    return out;
}

void PhysicsSystem::SetMassMultiplier(const PhysicsRef& ref, float multiplier)
{
    PhysicsBody& slot = GetBody(ref);
    if (slot.baseMass <= 0.0) return;
    cpBodySetMass(slot.cp.body.get(), slot.baseMass * multiplier);
}

void PhysicsSystem::InitBody(PhysicsBody& slot, const Transform& transf)
{
    const Body& bodyResource = *slot.body;
    cpSpace* space = m_spaces.at(slot.spaceId).get();

    slot.cp.body.reset(cpBodyNew(1.0, 1.0));
    slot.cp.space = m_spaces.at(slot.spaceId);

    cpBody* body = slot.cp.body.get();

    cpFloat moment = 0.0;
    cpFloat mass = bodyResource.GetMass();
    slot.baseMass = mass;
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

    if (moment != 0.) cpBodySetMoment(body, moment);
    if (mass != 0.) cpBodySetMass(body, mass);

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

    cpBodySetAngle(body, cpFloat(transf.rot));
    cpBodySetPosition(body, cpv(transf.pos.x(), transf.pos.y()));

    cpBodySetVelocity(body, cpVect(transf.vel));
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
    const static cpFloat gravityConstant = GRAVITY_CONSTANT;

    // Gather the space's gravity-participating bodies in a single ECS pass,
    // then do the O(n^2) pairwise attraction over the plain vector.
    struct GravBody {
        cpBody* body;
        cpFloat mass;
        cpVect pos;
        cpVect center;
    };
    std::vector<GravBody> bodies;

    m_registry.each<PhysicsRef>([&](flecs::entity ent, PhysicsRef& ref) {
        PhysicsBody& slot = GetBody(ref);
        if (slot.spaceId != spaceId || ent.has<Bullet>()) return;
        cpBody* body = slot.cp.body.get();
        bodies.push_back({body, cpBodyGetMass(body), cpBodyGetPosition(body),
                          cpBodyGetCenterOfGravity(body)});
    });

    for (const GravBody& tgt : bodies) {
        for (const GravBody& src : bodies) {
            if (src.body == tgt.body) continue;

            const cpFloat dist = cpvdist(src.pos, tgt.pos);
            cpFloat vel = gravityConstant * m_gravityMultiplier * ((tgt.mass * src.mass) / std::pow(dist, 2));
            cpFloat dir = std::atan2(src.pos.y - tgt.pos.y, src.pos.x - tgt.pos.x);
            cpVect force = cpv(std::cos(dir) * vel, std::sin(dir) * vel);

            cpBodyApplyForceAtWorldPoint(tgt.body, force, cpvadd(tgt.pos, tgt.center));
        }
    }
}

void PhysicsSystem::Simulate(double dt)
{
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
