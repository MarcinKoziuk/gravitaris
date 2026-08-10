#include <cmath>
#include <optional>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/component/freighter.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/planet-attachment.hpp>
#include <gravitaris/game/scenario/structure-layout.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/core/physics-system.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/system/core/structure-attachment-system.hpp>

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

        // Diagnostic (2026-08-10): see SpawnStructure's own note. Says which
        // of the two terms is the non-finite one, which the death log alone
        // could not. Capped so a permanent NaN can't fill the log at 60 Hz.
        if (!std::isfinite(pos.x()) && m_nanReports < NAN_REPORT_LIMIT) {
            ++m_nanReports;
            LOG(error) << "attach NaN: planet netId " << attach.planetNetId << " pos ("
                       << planetTransf.pos.x() << ", " << planetTransf.pos.y() << ") offset ("
                       << attach.localOffset.x() << ", " << attach.localOffset.y() << ")";
        }

        m_physicsSystem.SetKinematicMotion(ref, pos, vel, std::nullopt);
        transf.pos = pos;
        transf.vel = vel;
    });

    const double gravityMultiplier = m_physicsSystem.GetGravityMultiplier();

    m_registry.each([&](flecs::entity entity, Transform& transf, PhysicsRef& ref, PlanetOrbitAttachment& attach) {
        const flecs::entity planet = m_entitySpawner.EntityForNetId(attach.planetNetId);
        if (!planet.is_alive()) return;

        const Transform& planetTransf = planet.get<Transform>();

        // A freighter parks in a real ballistic orbit; a station flies its
        // ring under thrust, deliberately slower (ORBIT_SPEED_FACTOR) so a
        // ship can match it and land on the deck.
        const double speedFactor = entity.has<Freighter>() ? 1.0 : StructureLayout::ORBIT_SPEED_FACTOR;
        const double angularSpeed = attach.direction * speedFactor * std::sqrt(
                PhysicsSystem::GRAVITY_CONSTANT * gravityMultiplier * attach.centerMass
                / (attach.radius * attach.radius * attach.radius));
        attach.angularSpeed = angularSpeed; // cached for GatherSnapshot; see the field's own doc comment
        attach.theta += angularSpeed * Game::PHYSICS_DELTA;

        const double c = std::cos(attach.theta);
        const double s = std::sin(attach.theta);

        const Vector2d localVel = Vector2d{-s, c} * (angularSpeed * attach.radius);
        const Vector2d pos = planetTransf.pos + Vector2d{c, s} * attach.radius;
        const Vector2d vel = planetTransf.vel + localVel;

        // A Freighter faces prograde (nose is local -Y, see
        // ShipControlsSystem::ApplyMovement's thrust direction, so that's
        // rot = atan2(vel.x, -vel.y)). Faces the *local* orbital velocity,
        // not the combined `vel` used for motion -- the planet's own drift
        // around its star can dwarf the freighter's tight local orbit, and
        // facing the combined vector then reads as flying at a constant
        // offset angle rather than tangent to the visible circle around the
        // planet.
        //
        // A station instead keeps its floor (local +Y) toward the planet all
        // the way around, so local -Y points along the radial {c, s}.
        //
        // Either way the facing turns at the same rate the ring does, which
        // the solver needs told: it is what drags a ship parked on a station's
        // deck round with it instead of leaving it holding a world-frame
        // attitude the deck has turned out from under.
        const double rot = entity.has<Freighter>()
                ? std::atan2(localVel.x(), -localVel.y())
                : std::atan2(c, -s);

        m_physicsSystem.SetKinematicMotion(ref, pos, vel, rot, angularSpeed);
        transf.pos = pos;
        transf.vel = vel;
    });
}

} // namespace Gravitaris
