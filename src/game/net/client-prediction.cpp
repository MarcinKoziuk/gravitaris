#include <algorithm>
#include <cmath>

#include <chipmunk/chipmunk.h>

#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/resource/body.hpp>
#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/core/physics-system.hpp>
#include <gravitaris/game/system/ship/ship-controls-system.hpp>
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

void ClientPrediction::DestroyOwnShip()
{
    if (m_ownShip.is_alive()) m_ownShip.destruct();
    m_history.clear();
    m_fireCooldown = 0;
}

void ClientPrediction::SpawnOwnShip(id_t modelId, Magnum::Vector2d initialPos, TeamId team)
{
    if (HasOwnShip()) return;
    m_ownShip = m_entitySpawner.SpawnPlayer(modelId, initialPos, team);
}

void ClientPrediction::SyncPlanetProxies(const std::vector<EntityState>& planets, std::uint64_t baseTick,
                                         std::uint64_t atTick)
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

        // orbitRadius > 0 (not isStar -- that's a camera/minimap color hint,
        // not an "orbit data present" signal, though the two coincide for
        // any real server-generated snapshot) is the same "does this entity
        // actually orbit" guard OrbitSystem itself uses server-side: a star
        // has no Orbit component so this stays at its default zero, and it
        // simply sits at its replicated position. An orbiting planet's exact
        // position/velocity at `atTick` is re-derived analytically instead
        // of trusted from the snapshot's own (necessarily somewhat stale)
        // raw pos/vel -- see EvaluateOrbit and this method's own doc comment.
        if (state.orbitRadius > 0.f) {
            Magnum::Vector2d pos, vel;
            EvaluateOrbit(state, baseTick, atTick, pos, vel);
            m_physicsSystem.SetKinematicMotion(proxy.get<PhysicsRef>(), pos, vel);
        }
        else {
            m_physicsSystem.SetKinematicMotion(proxy.get<PhysicsRef>(), Magnum::Vector2d{state.pos},
                                               Magnum::Vector2d{state.vel});
        }
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

void ClientPrediction::Step(std::uint64_t tick, const ControlFlags& flags, const std::vector<EntityState>& planets,
                            std::uint64_t planetsBaseTick)
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
    // falls out of that existing machinery once these proxies exist. `tick`
    // (this Step's target), not `planetsBaseTick` (the snapshot's own,
    // always somewhat older tick) -- see SyncPlanetProxies/EvaluateOrbit.
    SyncPlanetProxies(planets, planetsBaseTick, tick);

    cpBody* body = m_physicsSystem.GetBody(m_ownShip.get<PhysicsRef>()).cp.body.get();
    ShipControlsSystem::ApplyMovement(body, flags);

    m_physicsSystem.Simulate(Game::PHYSICS_DELTA);
    m_physicsSystem.Update();

    if (m_fireCooldown > 0) --m_fireCooldown;
    if (flags.firePrimary && m_fireCooldown == 0) {
        m_fireCooldown = ShipControlsSystem::FIRE_COOLDOWN_TICKS;

        // The bullet this client actually sees. The server's authoritative
        // copy of this same shot never arrives (GatherSnapshot omits a
        // peer's own bullets), so there is exactly one on screen -- which is
        // what makes predicting it safe: an earlier attempt drew both, and
        // since the own ship renders at ~serverTick + INPUT_LEAD_TICKS while
        // replicated entities render at serverTick - the interpolation
        // delay, the pair showed up ~14 ticks apart as two separate tracers.
        // Same muzzle math the server uses, from the same predicted state
        // the server will reach for this tick, so the trajectory matches;
        // bullets take no gravity (see PhysicsSystem::ApplyGravity) and are
        // sensors, so both copies fly identical straight lines.
        //
        // Cosmetic only -- zero damage, and DamageSystem never runs on this
        // client -- so it flies through whatever the server says it hit and
        // expires on its own; hits/damage stay entirely server-authoritative.
        const auto [pos, vel] =
                ShipControlsSystem::ComputeBulletSpawn(m_ownShip.get<Transform>(),
                                                       m_physicsSystem.GetBody(m_ownShip.get<PhysicsRef>()));
        const flecs::entity bullet = m_entitySpawner.SpawnBullet(
                "models/bullets/bullet-0"_id, pos, vel, /*sensor=*/true);
        bullet.emplace<Bullet>(ShipControlsSystem::BULLET_LIFETIME_SECONDS, m_ownShip.get<Team>().id, 0.f,
                               m_ownShip.get<NetId>().value);

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
    const Magnum::Vector2d oldPredictedPos = it->pos; // at the reconciled tick -- divergence check only, see below
    if ((oldPredictedPos - authoritativePos).length() < m_positionEpsilon) {
        // Close enough: this and everything older is settled, no longer needed.
        m_history.erase(m_history.begin(), it);
        return std::nullopt;
    }

    // For the caller's visual smoothing (CGame::m_visualCorrectionOffset),
    // capture where prediction currently says the ship is *right now*
    // (the most recent entry, before this correction) -- not `it->pos`
    // above, which is the historical position at `authoritativeTick`. That
    // tick can be many ticks behind "now" (RTT + interp delay), so using it
    // here was a real bug: it made "how far to visually ease" conflate
    // actual correction error with pure travel distance covered between the
    // reconciled tick and now. Every correction then produced a systematic
    // backward-then-forward-overshoot visual artifact proportional to how
    // far the ship had moved since the reconciled tick, not to the error
    // actually being corrected -- worse the faster the ship was moving.
    const Magnum::Vector2d preCorrectionNowPos = m_history.back().pos;

    cpBody* body = m_physicsSystem.GetBody(m_ownShip.get<PhysicsRef>()).cp.body.get();
    cpBodySetPosition(body, cpv(authoritativePos.x(), authoritativePos.y()));
    cpBodySetAngle(body, static_cast<cpFloat>(authoritative.rot));
    cpBodySetVelocity(body, cpv(authoritative.vel.x(), authoritative.vel.y()));
    cpBodySetAngularVelocity(body, static_cast<cpFloat>(authoritative.angVel));
    m_physicsSystem.Update();

    std::vector<PredictedTick> toReplay(std::next(it), m_history.end());
    m_history.clear();
    for (const PredictedTick& pending : toReplay) {
        // Planet proxies positioned fresh for each replayed tick (via
        // EvaluateOrbit, cheap now that it's a closed form) rather than once
        // for the whole replay against `authoritativeTick`'s positions --
        // this used to be an accepted approximation (see Step's own history
        // on why that mattered) but there's no reason to keep it now.
        SyncPlanetProxies(planets, authoritativeTick, pending.tick);
        m_ownShip.get_mut<Controls>().actionFlags = pending.flags;
        ShipControlsSystem::ApplyMovement(body, pending.flags);
        m_physicsSystem.Simulate(Game::PHYSICS_DELTA);
        m_physicsSystem.Update();
        m_history.push_back(CaptureTick(pending.tick, pending.flags));
    }

    return preCorrectionNowPos;
}

} // namespace Gravitaris
