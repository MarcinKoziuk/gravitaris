#include <algorithm>
#include <cmath>

#include <chipmunk/chipmunk.h>

#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/physics-system.hpp>
#include <gravitaris/game/system/ship-controls-system.hpp>
#include <gravitaris/game/net/client-prediction.hpp>

namespace Gravitaris {

ClientPrediction::ClientPrediction(flecs::world& registry, PhysicsSystem& physicsSystem,
                                   EntitySpawner& entitySpawner)
        : m_registry(registry)
        , m_physicsSystem(physicsSystem)
        , m_entitySpawner(entitySpawner)
{}

bool ClientPrediction::HasOwnShip() const
{
    return m_ownShip.is_alive();
}

void ClientPrediction::SpawnOwnShip(id_t modelId, Magnum::Vector2d initialPos)
{
    if (HasOwnShip()) return;
    m_ownShip = m_entitySpawner.SpawnPlayer(modelId, initialPos);
}

void ClientPrediction::ApplyGravity(cpBody* body, const std::vector<EntityState>& planets)
{
    const cpVect shipPos = cpBodyGetPosition(body);
    const cpFloat shipMass = cpBodyGetMass(body);
    const cpFloat multiplier = static_cast<cpFloat>(m_physicsSystem.GetGravityMultiplier());

    cpVect total = cpvzero;
    for (const EntityState& planet : planets) {
        if (planet.type != NetEntityType::Planet || planet.gravityMass <= 0.f) continue;

        const cpVect srcPos = cpv(planet.pos.x(), planet.pos.y());
        const cpVect d = cpvsub(srcPos, shipPos);
        const cpFloat dist2 = cpvlengthsq(d);
        if (dist2 < 1e-6) continue;

        const cpFloat f = PhysicsSystem::GRAVITY_CONSTANT * multiplier * planet.gravityMultiplier
                * (shipMass * static_cast<cpFloat>(planet.gravityMass)) / dist2;
        total = cpvadd(total, cpvmult(d, f / std::sqrt(dist2)));
    }
    cpBodyApplyForceAtWorldPoint(body, total, shipPos);
}

ClientPrediction::PredictedTick ClientPrediction::CaptureTick(std::uint64_t tick, const ControlFlags& flags)
{
    const Transform& t = m_ownShip.get<Transform>();
    return PredictedTick{tick, flags, t.pos, t.rot, t.vel, t.angVel};
}

void ClientPrediction::Step(std::uint64_t tick, const ControlFlags& flags, const std::vector<EntityState>& planets)
{
    if (!HasOwnShip()) return;

    // ApplyMovement only touches the Chipmunk body; Controls::actionFlags
    // itself must be set too, or nothing driven by it (the thrust exhaust
    // tag group in ModelRenderer2, notably) ever reflects live input.
    m_ownShip.get_mut<Controls>().actionFlags = flags;

    cpBody* body = m_physicsSystem.GetBody(m_ownShip.get<PhysicsRef>()).cp.body.get();
    ShipControlsSystem::ApplyMovement(body, flags);
    ApplyGravity(body, planets);

    m_physicsSystem.Simulate(Game::PHYSICS_DELTA);
    m_physicsSystem.Update();

    m_history.push_back(CaptureTick(tick, flags));
    while (m_history.size() > MAX_HISTORY) m_history.pop_front();
}

std::optional<Magnum::Vector2d> ClientPrediction::Reconcile(std::uint64_t authoritativeTick,
                                                            const EntityState& authoritative,
                                                            const std::vector<EntityState>& planets)
{
    if (!HasOwnShip()) return std::nullopt;

    const auto it = std::find_if(m_history.begin(), m_history.end(),
                                 [&](const PredictedTick& p) { return p.tick == authoritativeTick; });
    if (it == m_history.end()) return std::nullopt;

    const Magnum::Vector2d authoritativePos{static_cast<double>(authoritative.pos.x()),
                                            static_cast<double>(authoritative.pos.y())};
    const Magnum::Vector2d preCorrectionPos = it->pos;
    if ((preCorrectionPos - authoritativePos).length() < m_positionEpsilon) {
        // Close enough: this and everything older is settled, no longer needed.
        m_history.erase(m_history.begin(), it);
        return std::nullopt;
    }

    cpBody* body = m_physicsSystem.GetBody(m_ownShip.get<PhysicsRef>()).cp.body.get();
    cpBodySetPosition(body, cpv(authoritativePos.x(), authoritativePos.y()));
    cpBodySetAngle(body, static_cast<cpFloat>(authoritative.rot));
    cpBodySetVelocity(body, cpv(authoritative.vel.x(), authoritative.vel.y()));
    cpBodySetAngularVelocity(body, static_cast<cpFloat>(authoritative.angVel));
    m_physicsSystem.Update();

    std::vector<PredictedTick> toReplay(std::next(it), m_history.end());
    m_history.clear();
    for (const PredictedTick& pending : toReplay) {
        m_ownShip.get_mut<Controls>().actionFlags = pending.flags;
        ShipControlsSystem::ApplyMovement(body, pending.flags);
        ApplyGravity(body, planets);
        m_physicsSystem.Simulate(Game::PHYSICS_DELTA);
        m_physicsSystem.Update();
        m_history.push_back(CaptureTick(pending.tick, pending.flags));
    }

    return preCorrectionPos;
}

} // namespace Gravitaris
