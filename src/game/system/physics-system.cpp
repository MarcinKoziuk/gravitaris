#include <cmath>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/system/physics-system.hpp>

namespace Gravitaris {

static const cpTransform tzero = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };

PhysicsSystem::PhysicsSystem(flecs::world& registry)
    : m_registry(registry)
{
    // OnSet (not OnAdd) mirrors entt's on_construct: it fires once, after the
    // component's value is already assigned -- entity.emplace<Physics>(...)
    // both adds and constructs in one step, so OnSet is the equivalent point.
    m_physicsAddedObserver = m_registry.observer<Physics>()
            .event(flecs::OnSet)
            .each([this](flecs::entity ent, Physics&) { HandlePhysicsAdded(ent); });

    // OnRemove fires both on explicit component removal and on entity
    // destruction, matching entt's on_destroy.
    m_physicsRemovedObserver = m_registry.observer<Physics>()
            .event(flecs::OnRemove)
            .each([this](flecs::entity ent, Physics&) { HandlePhysicsRemoved(ent); });
}

PhysicsSystem::~PhysicsSystem()
{
    // Explicit teardown: these observers close over `this`, and must not
    // outlive it -- the flecs::world (owned by Game, declared before this
    // system) is destroyed after PhysicsSystem, so without this the world
    // would still fire into a dangling PhysicsSystem during later teardown.
    m_physicsAddedObserver.destruct();
    m_physicsRemovedObserver.destruct();
}

void PhysicsSystem::InitSpace(id_t spaceId)
{
    m_spaces.insert(
            std::make_pair(
                    spaceId,
                    std::shared_ptr<cpSpace>(cpSpaceNew(), cpSpaceDeleter())));
}

void PhysicsSystem::InitBody(flecs::entity ent, const Transform& transf, Physics& phys)
{
    const Body& bodyResource = *phys.body;
    cpSpace* space = m_spaces.at(phys.spaceId).get();

    phys.cp.body.reset(cpBodyNew(1.0, 1.0));
    phys.cp.space = m_spaces.at(phys.spaceId);

    cpBody* body = phys.cp.body.get();

    cpFloat moment = 0.0;
    cpFloat mass = bodyResource.GetMass();
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
        /*cpShapeSetMass(shape, 1.0);
        cpShapeSetDensity(shape, 1.0);
        cpShapeSetDensity(shape, 1.0);
        cpShapeSetFriction(shape, 0.0);*/

        phys.cp.shapes.emplace_back(cpShapeUniquePtr(shape));
    }
    for (const Body::CircleShape& circle : bodyResource.GetCircleShapes()) {
        const cpVect offs = cpVect(circle.pos * transf.scale);
        moment += cpMomentForCircle(mass, 0.f, circle.radius, offs);
        cpShape* shape = cpCircleShapeNew(body, circle.radius * transf.scale.x(), offs);
        auto* cshape = (cpCircleShape*)shape;
        phys.cp.shapes.emplace_back(cpShapeUniquePtr(shape));
    }

    if (moment != 0.) cpBodySetMoment(body, moment);
    if (mass != 0.) cpBodySetMass(body, mass);


    cpSpaceAddBody(space, body);
    for (auto& it : phys.cp.shapes) {
        cpShape* shape = it.get();

        const cpFloat friction = phys.body->GetFriction();
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



    //cpBodyApplyForceAtWorldPoint(body, to_cpv(transf.vel), cpvzero);
    cpBodySetVelocity(body, cpVect(transf.vel));
}

void PhysicsSystem::HandlePhysicsAdded(flecs::entity ent)
{
    auto& phys = ent.get_mut<Physics>();
    const auto& transf = ent.get<Transform>();

    if (!m_spaces.count(phys.spaceId)) {
        InitSpace(phys.spaceId);
    }

    InitBody(ent, transf, phys);
}

void PhysicsSystem::HandlePhysicsRemoved(flecs::entity ent)
{
    // we stil leak spaces here.
}

void PhysicsSystem::ApplyGravity(id_t spaceId)
{
    const static cpFloat gravityConstant = 20.0;

    m_registry.each<Physics>([&](flecs::entity tgt, Physics& tgtPhys) {
        if (tgtPhys.spaceId != spaceId || tgt.has<Bullet>()) return;

        cpBody* tgtBody = tgtPhys.cp.body.get();
        const cpFloat tgtMass = cpBodyGetMass(tgtBody);
        const cpVect tgtPos = cpBodyGetPosition(tgtBody);
        const cpVect tgtCenter = cpBodyGetCenterOfGravity(tgtBody);

        m_registry.each<Physics>([&](flecs::entity src, Physics& srcPhys) {
            if (src == tgt) return;
            if (srcPhys.spaceId != spaceId || src.has<Bullet>()) return;

            cpBody* srcBody = srcPhys.cp.body.get();
            const cpFloat srcMass = cpBodyGetMass(srcBody);
            const cpVect srcPos = cpBodyGetPosition(srcBody);
            const cpFloat dist = cpvdist(srcPos, tgtPos);

            cpFloat vel = gravityConstant * ((tgtMass * srcMass) / std::pow(dist, 2));
            cpFloat dir = std::atan2(srcPos.y - tgtPos.y, srcPos.x - tgtPos.x);
            cpVect force = cpv(std::cos(dir) * vel, std::sin(dir) * vel);

            cpBodyApplyForceAtWorldPoint(tgtBody, force, cpvadd(tgtPos, tgtCenter));
        });
    });
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
    m_registry.each([](flecs::entity, Transform& transf, Physics& phys) {
        cpBody* body = phys.cp.body.get();
        transf.prevPos = transf.pos;
        transf.pos = Vector2d(cpBodyGetPosition(body));
        transf.rot = Radd(cpvtoangle(cpBodyGetRotation(body)));
        transf.vel = Vector2d(cpBodyGetVelocity(body));
    });
}

} // namespace Gravitaris
