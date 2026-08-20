#include <algorithm>
#include <cmath>

#include <chipmunk/chipmunk.h>

#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/ship-loadout.hpp>
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
#include <gravitaris/game/upgrade/upgrade-catalog.hpp>
#include <gravitaris/game/net/client-prediction.hpp>

namespace Gravitaris {

ClientPrediction::ClientPrediction(flecs::world& registry, PhysicsSystem& physicsSystem,
                                   EntitySpawner& entitySpawner, GameEventQueue& eventQueue,
                                   ResourceLoader& resourceLoader, const UpgradeCatalog& catalog)
        : m_registry(registry)
        , m_physicsSystem(physicsSystem)
        , m_entitySpawner(entitySpawner)
        , m_eventQueue(eventQueue)
        , m_resourceLoader(resourceLoader)
        , m_catalog(catalog)
{}

bool ClientPrediction::HasOwnShip() const
{
    return m_ownShip.is_alive();
}

void ClientPrediction::DestroyOwnShip()
{
    if (m_ownShip.is_alive()) m_ownShip.destruct();
    m_history.clear();
}

void ClientPrediction::SpawnOwnShip(id_t modelId, Magnum::Vector2d initialPos, TeamId team)
{
    if (HasOwnShip()) return;
    m_ownShip = m_entitySpawner.SpawnPlayer(modelId, initialPos, team);
}

namespace {

// Which of `snapshotEntities` get a collision proxy at all: every planet,
// every structure, and every ship other than this peer's own (already real
// and dynamic in this same registry -- see the class doc comment on why
// never both).
//
// A structure's motion is as deterministic as a planet's -- it rides a
// replicated attachment (EvaluateAttachment) -- so the predicted ship can be
// collided against it exactly, which is what makes landing on a High Port
// deck work client-side rather than being a server-only surprise.
bool NeedsCollisionProxy(const EntityState& state, std::uint32_t ownShipNetId)
{
    if (state.type == NetEntityType::Planet) return true;
    if (state.type == NetEntityType::Structure) return true;
    return state.type == NetEntityType::Ship && state.netId != ownShipNetId;
}

const EntityState* FindState(const std::vector<EntityState>& states, std::uint32_t netId)
{
    const auto it = std::find_if(states.begin(), states.end(),
                                 [&](const EntityState& e) { return e.netId == netId; });
    return it != states.end() ? &*it : nullptr;
}

} // namespace

void ClientPrediction::SyncCollisionProxies(const std::vector<EntityState>& snapshotEntities,
                                            std::uint32_t ownShipNetId, std::uint64_t baseTick, std::uint64_t atTick)
{
    for (const EntityState& state : snapshotEntities) {
        if (!NeedsCollisionProxy(state, ownShipNetId)) continue;

        const auto it = m_collisionProxies.find(state.netId);
        flecs::entity proxy = (it != m_collisionProxies.end()) ? it->second : flecs::entity{};

        if (!proxy.is_alive()) {
            // Same Body resource the real sim loads for this entity (by the
            // already-replicated modelId) -- its kinematic-ness and collision
            // shape come along for free, no separate wire field needed.
            const ResourcePtr<const Body> body = m_resourceLoader.Load<Body>(state.modelId);
            proxy = m_registry.entity();
            proxy.emplace<Transform>(Magnum::Vector2d{state.pos});
            // A remote ship's proxy plays by the same ship<->ship rule the
            // server does (networking-plan Phase 9), so the predicted own
            // ship passes through it instead of bouncing off a proxy the
            // server never bounced off -- the reconciliation thrash Phase 7
            // left unfixed. Planet proxies keep hard contact.
            //
            // Freighters are ship-classed server-side too, so "Ship on the
            // wire" and "plays by the ship rule" mean the same thing here --
            // no client/server disagreement to reason about.
            const CollisionClass collisionClass =
                    state.type == NetEntityType::Ship ? CollisionClass::Ship : CollisionClass::Default;
            proxy.emplace<RigidBodyDesc>("main"_id, body, /*sensor=*/false, collisionClass);
            m_collisionProxies[state.netId] = proxy;
        }

        // (Re)apply GravitySource every sync, not just at creation: harmless
        // if unchanged, but keeps this correct if a planet's replicated
        // mass/multiplier ever changes (e.g. a future gravity-multiplier
        // debug tab affecting the server) without needing extra bookkeeping.
        // Always 0 for a ship (no GravitySource server-side), so this is
        // naturally a no-op for those without a separate type check.
        if (state.gravityMass > 0.f) {
            proxy.set<GravitySource>(GravitySource{state.gravityMass, state.gravityMultiplier});
        }

        // orbitRadius > 0 (not isStar -- that's a camera/minimap color hint,
        // not an "orbit data present" signal, though the two coincide for
        // any real server-generated snapshot) is the same "does this entity
        // actually orbit" guard OrbitSystem itself uses server-side: only a
        // Planet ever has this set (a Ship's is always 0, same reasoning as
        // gravityMass above), so this naturally routes every planet through
        // the exact analytic path and every ship through dead reckoning
        // with no separate type check here either. An orbiting planet's
        // exact position/velocity at `atTick` is re-derived analytically
        // (EvaluateOrbit) instead of trusted from the snapshot's own
        // (necessarily somewhat stale) raw pos/vel; anything else -- a star,
        // sitting at a fixed position with zero velocity, or a remote ship,
        // whose motion has no closed form -- is dead-reckoned by
        // extrapolating its last-replicated velocity forward instead (see
        // the class doc comment on why that's an accepted approximation for
        // a ship specifically).
        if (state.orbitRadius > 0.f) {
            Magnum::Vector2d pos, vel;
            EvaluateOrbit(state, baseTick, atTick, pos, vel);
            m_physicsSystem.SetKinematicMotion(proxy.get<PhysicsRef>(), pos, vel);
        }
        else if (state.attachParentNetId != 0) {
            // Composed on the parent's own analytic position at the same
            // tick, not on its (staler) raw snapshot one -- the same two-step
            // SnapshotInterpolator does for rendering. An orbiting station
            // additionally faces floor-inward, matching
            // StructureAttachmentSystem, so the deck a ship lands on is
            // where the server says it is.
            Magnum::Vector2d parentPos{state.pos}, parentVel{state.vel};
            if (const EntityState* parent = FindState(snapshotEntities, state.attachParentNetId)) {
                if (parent->orbitRadius > 0.f) {
                    EvaluateOrbit(*parent, baseTick, atTick, parentPos, parentVel);
                }
                else {
                    parentPos = Magnum::Vector2d{parent->pos};
                    parentVel = Magnum::Vector2d{parent->vel};
                }
            }

            Magnum::Vector2d pos, vel, localVel;
            EvaluateAttachment(parentPos, parentVel, state, baseTick, atTick, pos, vel, localVel);

            std::optional<double> angle;
            if (state.attachAngularSpeed != 0.f) {
                const Magnum::Vector2d radial = pos - parentPos;
                angle = std::atan2(radial.x(), -radial.y());
            }
            m_physicsSystem.SetKinematicMotion(proxy.get<PhysicsRef>(), pos, vel, angle);
        }
        else {
            const double elapsedSeconds =
                    (static_cast<double>(atTick) - static_cast<double>(baseTick)) * Game::PHYSICS_DELTA;
            const Magnum::Vector2d vel{state.vel};
            const Magnum::Vector2d pos = Magnum::Vector2d{state.pos} + vel * elapsedSeconds;
            m_physicsSystem.SetKinematicMotion(proxy.get<PhysicsRef>(), pos, vel);
        }
    }

    // Prune proxies no longer backed by a live entry (planet gone -- doesn't
    // normally happen; remote ship destroyed, respawned under a new NetId,
    // or -- this tick -- became this peer's own ship).
    for (auto it = m_collisionProxies.begin(); it != m_collisionProxies.end();) {
        const bool stillNeeded = std::any_of(snapshotEntities.begin(), snapshotEntities.end(),
                                             [&](const EntityState& s) {
            return s.netId == it->first && NeedsCollisionProxy(s, ownShipNetId);
        });
        if (stillNeeded) {
            ++it;
        } else {
            if (it->second.is_alive()) it->second.destruct();
            it = m_collisionProxies.erase(it);
        }
    }
}

ClientPrediction::PredictedTick ClientPrediction::CaptureTick(std::uint64_t tick, const ControlFlags& flags,
                                                              bool boosting)
{
    const Transform& t = m_ownShip.get<Transform>();
    return PredictedTick{tick, flags, t.pos, t.rot, t.vel, t.angVel, boosting};
}

void ClientPrediction::Step(std::uint64_t tick, const ControlFlags& flags,
                            const std::vector<EntityState>& snapshotEntities, std::uint64_t snapshotBaseTick,
                            std::uint32_t ownShipNetId)
{
    if (!HasOwnShip()) return;

    // ApplyMovement only touches the Chipmunk body; Controls::actionFlags
    // itself must be set too, or nothing driven by it (the thrust exhaust
    // tag group in ModelRenderer2, notably) ever reflects live input.
    m_ownShip.get_mut<Controls>().actionFlags = flags;

    // Position/velocity the collision proxies (Phase 7/7.1) before this
    // tick's step, so PhysicsSystem::Simulate's own per-space gravity pass
    // and Chipmunk's contact resolution both see them where they actually
    // are this tick -- gravity is no longer a manual force here at all, it
    // falls out of that existing machinery once these proxies exist. `tick`
    // (this Step's target), not `snapshotBaseTick` (the snapshot's own,
    // always somewhat older tick) -- see SyncCollisionProxies/EvaluateOrbit.
    SyncCollisionProxies(snapshotEntities, ownShipNetId, snapshotBaseTick, tick);

    // Off the own ship's own replicated loadout, so a collected fire-rate,
    // weapon tier or overburn changes the predicted cadence, the round itself
    // and the thrust the same tick the server's does -- otherwise the
    // cosmetic tracer would leave the rail at a different speed than the shot
    // the server actually fired.
    const ShipLoadout* ownLoadout = m_ownShip.try_get<ShipLoadout>();
    const ShipStats stats = m_catalog.ResolveStats(ownLoadout ? ownLoadout->levels : UpgradeLevels{});

    PhysicsBody& phys = m_physicsSystem.GetBody(m_ownShip.get<PhysicsRef>());
    Controls& ownControls = m_ownShip.get_mut<Controls>();
    // The bank is predicted for the same reason the burn is: the beams come
    // out of it too, so a client that only predicted the thrust would draw a
    // power bar that disagreed with its own trigger. What a beam does when it
    // lands stays the server's (ClientPrediction runs no DamageSystem).
    const auto laserMounts =
            static_cast<unsigned>(ownLoadout ? MountsArmedWith(*ownLoadout, MountArm::Laser) : 0);
    const ShipControlsSystem::PowerGrant power =
            ShipControlsSystem::AdvanceCapacitor(ownControls, stats, laserMounts);
    const ShipControlsSystem::Motion motion =
            ShipControlsSystem::MotionOf(*phys.body, stats, power.boost);
    ShipControlsSystem::ApplyMovement(phys.cp.body.get(), flags, motion.thrust, motion.maxSpeed);

    m_physicsSystem.Simulate(Game::PHYSICS_DELTA);
    m_physicsSystem.Update();

    // The very same pacing the server runs, off the same Controls, stats and
    // loadout -- including the swap, the armed-mount filter and the fall
    // through a dry magazine. Only what a round *is* differs down here.
    const PhysicsBody& ownPhys = m_physicsSystem.GetBody(m_ownShip.get<PhysicsRef>());
    if (ownPhys.body) {
        ShipLoadout* mutableLoadout = m_ownShip.try_get_mut<ShipLoadout>();
        ShipControlsSystem::AdvancePrimary(ownControls, stats, mutableLoadout, *ownPhys.body, flags,
                                           [&](const WeaponDef& gun, unsigned mount) {
            // The bullet this client actually sees. The server's authoritative
            // copy of this same shot never arrives (GatherSnapshot omits a
            // peer's own bullets), so there is exactly one on screen -- which
            // is what makes predicting it safe: an earlier attempt drew both,
            // and since the own ship renders at ~serverTick + INPUT_LEAD_TICKS
            // while replicated entities render at serverTick - the
            // interpolation delay, the pair showed up ~14 ticks apart as two
            // separate tracers.
            //
            // Cosmetic only -- zero damage, and DamageSystem never runs on
            // this client -- so it flies through whatever the server says it
            // hit and expires on its own; hits and damage stay entirely
            // server-authoritative. The cadence can still drift (a dropped
            // input makes the server skip a shot this client took), and the
            // server's copy of this shot is never drawn here.
            const auto [pos, vel] =
                    ShipControlsSystem::ComputeBulletSpawn(m_ownShip.get<Transform>(),
                                                           ownPhys, gun.speed,
                                                           ShipControlsSystem::WEAPON_HARDPOINT,
                                                           mount);
            // Not cosmetic, unlike the round itself: the server shoves its own
            // copy of this hull back on this same tick, so a client that
            // skipped it would be reconciled backwards once per shot.
            if (m_physicsSystem.GetWeaponRecoil()) {
                ShipControlsSystem::ApplyRecoil(ownPhys.cp.body.get(), gun);
            }

            const flecs::entity bullet =
                    m_entitySpawner.SpawnBullet(gun.modelId, pos, vel, /*sensor=*/true,
                                                static_cast<double>(m_ownShip.get<Transform>().rot));
            bullet.emplace<Bullet>(gun.lifetimeSeconds, m_ownShip.get<Team>().id, 0.f,
                                   m_ownShip.get<NetId>().value, m_ownShip.id());

            m_eventQueue.Emit(GameEventType::BulletFired, m_ownShip,
                              Magnum::Vector2{static_cast<float>(pos.x()),
                                              static_cast<float>(pos.y())},
                              gun.id);
        });
    }

    m_history.push_back(CaptureTick(tick, flags, ownControls.boosting));
    while (m_history.size() > MAX_HISTORY) m_history.pop_front();
}

std::optional<Magnum::Vector2d> ClientPrediction::Reconcile(std::uint64_t authoritativeTick,
                                                            const EntityState& authoritative,
                                                            const std::vector<EntityState>& snapshotEntities,
                                                            std::uint32_t ownShipNetId)
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

    PhysicsBody& phys = m_physicsSystem.GetBody(m_ownShip.get<PhysicsRef>());
    cpBody* body = phys.cp.body.get();
    cpBodySetPosition(body, cpv(authoritativePos.x(), authoritativePos.y()));
    cpBodySetAngle(body, static_cast<cpFloat>(authoritative.rot));
    cpBodySetVelocity(body, cpv(authoritative.vel.x(), authoritative.vel.y()));
    cpBodySetAngularVelocity(body, static_cast<cpFloat>(authoritative.angVel));
    m_physicsSystem.Update();

    const ShipLoadout* replayLoadout = m_ownShip.try_get<ShipLoadout>();
    const ShipStats replayStats =
            m_catalog.ResolveStats(replayLoadout ? replayLoadout->levels : UpgradeLevels{});

    std::vector<PredictedTick> toReplay(std::next(it), m_history.end());
    m_history.clear();
    for (const PredictedTick& pending : toReplay) {
        // Proxies positioned fresh for each replayed tick (via EvaluateOrbit
        // for a planet, cheap now that it's a closed form) rather than once
        // for the whole replay against `authoritativeTick`'s positions --
        // this used to be an accepted approximation (see Step's own history
        // on why that mattered) but there's no reason to keep it now. A
        // remote ship proxy still only dead-reckons from `authoritativeTick`
        // (there's no better data mid-replay), so it drifts from wherever
        // the server actually put it further out a replay runs -- an
        // additional, smaller approximation on top of the ship-proxy
        // tradeoffs the class doc comment already accepts.
        SyncCollisionProxies(snapshotEntities, ownShipNetId, authoritativeTick, pending.tick);
        m_ownShip.get_mut<Controls>().actionFlags = pending.flags;
        // Replayed with the grant the tick actually flew with;
        // AdvanceCapacitor is deliberately not called here, since the bank was
        // already drawn against when this tick was first predicted.
        const ShipControlsSystem::BoostEffect replayBoost =
                ShipControlsSystem::BoostEffectOf(pending.boosting, replayStats);
        const ShipControlsSystem::Motion replayMotion =
                ShipControlsSystem::MotionOf(*phys.body, replayStats, replayBoost);
        ShipControlsSystem::ApplyMovement(body, pending.flags, replayMotion.thrust,
                                          replayMotion.maxSpeed);
        m_physicsSystem.Simulate(Game::PHYSICS_DELTA);
        m_physicsSystem.Update();
        m_history.push_back(CaptureTick(pending.tick, pending.flags, pending.boosting));
    }

    return preCorrectionNowPos;
}

} // namespace Gravitaris
