#include <algorithm>
#include <cmath>

#include <chipmunk/chipmunk.h>

#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/resource/body.hpp>
#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/physics-system.hpp>
#include <gravitaris/game/system/ship-controls-system.hpp>
#include <gravitaris/game/net/client-prediction.hpp>

namespace Gravitaris {

ClientPrediction::ClientPrediction(flecs::world& registry, PhysicsSystem& physicsSystem,
                                   EntitySpawner& entitySpawner, GameEventQueue& eventQueue,
                                   ResourceLoader& resourceLoader)
        : m_registry(registry)
        , m_physicsSystem(physicsSystem)
        , m_entitySpawner(entitySpawner)
        , m_eventQueue(eventQueue)
        , m_resourceLoader(resourceLoader)
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

void ClientPrediction::SyncPlanetProxies(const std::vector<EntityState>& planets)
{
    for (const EntityState& state : planets) {
        if (state.type != NetEntityType::Planet) continue;

        const auto it = m_planetProxies.find(state.netId);
        flecs::entity proxy = (it != m_planetProxies.end()) ? it->second : flecs::entity{};

        if (!proxy.is_alive()) {
            // Same Body resource the real sim loads for this planet (by the
            // already-replicated modelId) -- its kinematic-ness and collision
            // shape come along for free, no separate wire field needed.
            const ResourcePtr<const Body> body = m_resourceLoader.Load<Body>(state.modelId);
            proxy = m_registry.entity();
            proxy.emplace<Transform>(Magnum::Vector2d{state.pos});
            proxy.emplace<RigidBodyDesc>("main"_id, body);
            m_planetProxies[state.netId] = proxy;
        }

        // (Re)apply GravitySource every sync, not just at creation: harmless
        // if unchanged, but keeps this correct if a planet's replicated
        // mass/multiplier ever changes (e.g. a future gravity-multiplier
        // debug tab affecting the server) without needing extra bookkeeping.
        if (state.gravityMass > 0.f) {
            proxy.set<GravitySource>(GravitySource{state.gravityMass, state.gravityMultiplier});
        }

        m_physicsSystem.SetKinematicMotion(proxy.get<PhysicsRef>(), Magnum::Vector2d{state.pos},
                                           Magnum::Vector2d{state.vel});
    }

    // Prune proxies for planets no longer present. Doesn't normally happen
    // (planets don't despawn), but keeps this correct if it ever does.
    for (auto it = m_planetProxies.begin(); it != m_planetProxies.end();) {
        const bool stillPresent = std::any_of(planets.begin(), planets.end(), [&](const EntityState& s) {
            return s.type == NetEntityType::Planet && s.netId == it->first;
        });
        if (stillPresent) {
            ++it;
        } else {
            if (it->second.is_alive()) it->second.destruct();
            it = m_planetProxies.erase(it);
        }
    }
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

    // Position/velocity the planet collision proxies (Phase 7) before this
    // tick's step, so PhysicsSystem::Simulate's own per-space gravity pass
    // and Chipmunk's contact resolution both see them where they actually
    // are this tick -- gravity is no longer a manual force here at all, it
    // falls out of that existing machinery once these proxies exist.
    SyncPlanetProxies(planets);

    cpBody* body = m_physicsSystem.GetBody(m_ownShip.get<PhysicsRef>()).cp.body.get();
    ShipControlsSystem::ApplyMovement(body, flags);

    m_physicsSystem.Simulate(Game::PHYSICS_DELTA);
    m_physicsSystem.Update();

    if (m_fireCooldown > 0) --m_fireCooldown;
    if (flags.firePrimary && m_fireCooldown == 0) {
        m_fireCooldown = ShipControlsSystem::FIRE_COOLDOWN_TICKS;

        const auto [pos, vel] = ShipControlsSystem::ComputeBulletSpawn(
                m_ownShip.get<Transform>(), m_physicsSystem.GetBody(m_ownShip.get<PhysicsRef>()));
        const flecs::entity bullet = m_entitySpawner.SpawnBullet(
                "models/bullets/bullet-0"_id, pos, vel, /*sensor=*/true);
        bullet.emplace<Bullet>(COSMETIC_BULLET_LIFETIME_SECONDS, TeamId::Blue, 0.f);

        m_eventQueue.Emit(GameEventType::BulletFired, m_ownShip,
                          Magnum::Vector2{static_cast<float>(pos.x()), static_cast<float>(pos.y())});
    }

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

    // Same current-snapshot planet positions for every replayed tick below,
    // not each one's own historical position -- an accepted approximation
    // (see the class doc comment), same as the gravity-from-current-snapshot
    // one this replaces.
    SyncPlanetProxies(planets);

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
        m_physicsSystem.Simulate(Game::PHYSICS_DELTA);
        m_physicsSystem.Update();
        m_history.push_back(CaptureTick(pending.tick, pending.flags));
    }

    return preCorrectionPos;
}

} // namespace Gravitaris
