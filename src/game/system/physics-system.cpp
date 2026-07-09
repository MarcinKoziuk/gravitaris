#include <cmath>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/system/physics-system.hpp>

namespace Gravitaris {

static const cpTransform tzero = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };

PhysicsSystem::PhysicsSystem(entt::registry& registry)
    : m_registry(registry)
{
    m_registry.on_construct<Physics>().connect<&PhysicsSystem::HandlePhysicsAdded>(*this);
    m_registry.on_destroy<Physics>().connect<&PhysicsSystem::HandlePhysicsRemoved>(*this);
}

PhysicsSystem::~PhysicsSystem()
{
    m_registry.on_construct<Physics>().disconnect<&PhysicsSystem::HandlePhysicsAdded>(*this);
    m_registry.on_destroy<Physics>().disconnect<&PhysicsSystem::HandlePhysicsRemoved>(*this);
}

void PhysicsSystem::InitSpace(id_t spaceId)
{
    m_spaces.insert(
            std::make_pair(
                    spaceId,
                    std::shared_ptr<cpSpace>(cpSpaceNew(), cpSpaceDeleter())));
}

void PhysicsSystem::InitBody(const entt::entity& ent, const Transform& transf, Physics& phys)
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

void PhysicsSystem::HandlePhysicsAdded(const entt::entity& ent)
{
    auto& phys = m_registry.get<Physics>(ent);
    const auto& transf = m_registry.get<Transform>(ent);

    if (!m_spaces.count(phys.spaceId)) {
        InitSpace(phys.spaceId);
    }

    InitBody(ent, transf, phys);
}

void PhysicsSystem::HandlePhysicsRemoved(const entt::entity& ent)
{
    // we stil leak spaces here.
}

void PhysicsSystem::ApplyGravity(id_t spaceId)
{
    const static cpFloat gravityConstant = 20.0;

    auto view = m_registry.view<Physics>();
    for (auto tgt : view) {
        const Physics& tgtPhys = view.get<Physics>(tgt);
        if (tgtPhys.spaceId != spaceId || m_registry.all_of<Bullet>(tgt)) continue;

        cpBody* tgtBody = tgtPhys.cp.body.get();
        const cpFloat tgtMass = cpBodyGetMass(tgtBody);
        const cpVect tgtPos = cpBodyGetPosition(tgtBody);
        const cpVect tgtCenter = cpBodyGetCenterOfGravity(tgtBody);

        for (auto src : view) {
            if (src == tgt) continue;

            const Physics& srcPhys = view.get<Physics>(src);
            if (srcPhys.spaceId != spaceId || m_registry.all_of<Bullet>(src)) continue;

            cpBody* srcBody = srcPhys.cp.body.get();
            const cpFloat srcMass = cpBodyGetMass(srcBody);
            const cpVect srcPos = cpBodyGetPosition(srcBody);
            const cpFloat dist = cpvdist(srcPos, tgtPos);

            cpFloat vel = gravityConstant * ((tgtMass * srcMass) / std::pow(dist, 2));
            cpFloat dir = std::atan2(srcPos.y - tgtPos.y, srcPos.x - tgtPos.x);
            cpVect force = cpv(std::cos(dir) * vel, std::sin(dir) * vel);

            cpBodyApplyForceAtWorldPoint(tgtBody, force, cpvadd(tgtPos, tgtCenter));
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
    auto view = m_registry.view<Transform, Physics>();
    for (auto entity : view) {
        Transform& transf = view.get<Transform>(entity);
        Physics& phys = view.get<Physics>(entity);

        cpBody* body = phys.cp.body.get();
        transf.prevPos = transf.pos;
        transf.pos = Vector2d(cpBodyGetPosition(body));
        transf.rot = Radd(cpvtoangle(cpBodyGetRotation(body)));
        transf.vel = Vector2d(cpBodyGetVelocity(body));
    }
}

} // namespace Gravitaris
