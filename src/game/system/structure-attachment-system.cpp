#include <cmath>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/planet-attachment.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/physics-system.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/system/structure-attachment-system.hpp>

namespace Gravitaris {

StructureAttachmentSystem::StructureAttachmentSystem(flecs::world& registry, EntitySpawner& entitySpawner,
                                                      PhysicsSystem& physicsSystem)
        : m_registry(registry)
        , m_entitySpawner(entitySpawner)
        , m_physicsSystem(physicsSystem)
{}

void StructureAttachmentSystem::Update()
{
    m_registry.each([&](flecs::entity, Transform& transf, PhysicsRef& ref, PlanetSurfaceAttachment& attach) {
        const flecs::entity planet = m_entitySpawner.EntityForNetId(attach.planetNetId);
        if (!planet.is_alive()) return; // planet destroyed -- leave the structure where it last was

        const Transform& planetTransf = planet.get<Transform>();
        const Vector2d pos = planetTransf.pos + attach.localOffset;
        const Vector2d vel = planetTransf.vel;

        m_physicsSystem.SetKinematicMotion(ref, pos, vel);
        transf.pos = pos;
        transf.vel = vel;
    });

    const double gravityMultiplier = m_physicsSystem.GetGravityMultiplier();

    m_registry.each([&](flecs::entity, Transform& transf, PhysicsRef& ref, PlanetOrbitAttachment& attach) {
        const flecs::entity planet = m_entitySpawner.EntityForNetId(attach.planetNetId);
        if (!planet.is_alive()) return;

        const Transform& planetTransf = planet.get<Transform>();

        const double angularSpeed = attach.direction * std::sqrt(
                PhysicsSystem::GRAVITY_CONSTANT * gravityMultiplier * attach.centerMass
                / (attach.radius * attach.radius * attach.radius));
        attach.theta += angularSpeed * Game::PHYSICS_DELTA;

        const double c = std::cos(attach.theta);
        const double s = std::sin(attach.theta);

        const Vector2d pos = planetTransf.pos + Vector2d{c, s} * attach.radius;
        const Vector2d vel = planetTransf.vel + Vector2d{-s, c} * (angularSpeed * attach.radius);

        m_physicsSystem.SetKinematicMotion(ref, pos, vel);
        transf.pos = pos;
        transf.vel = vel;
    });
}

} // namespace Gravitaris
