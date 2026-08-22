#include <algorithm>
#include <cmath>
#include <vector>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/freighter.hpp>
#include <gravitaris/game/component/ship-loadout.hpp>
#include <gravitaris/game/component/research-access.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/orbit.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/input/input-command.hpp>
#include <gravitaris/game/resource/body.hpp>
#include <gravitaris/game/resource/common/resource-loader.hpp>

#include <gravitaris/cgame/resource/model.hpp>
#include <gravitaris/cgame/component/renderable.hpp>
#include <gravitaris/cgame/component/hit-flash.hpp>
#include <gravitaris/cgame/component/hit-outline.hpp>
#include <gravitaris/cgame/component/shield-flash.hpp>
#include <gravitaris/cgame/component/remote-smoothing.hpp>
#include <gravitaris/cgame/net/snapshot-applier.hpp>

namespace {

// Same 100ms convention CGame's own-ship reconciliation offset decays over.
constexpr float SMOOTH_TAU_SECONDS = 0.1f;

// Below this, a frame-to-frame gap between "where we predicted this entity
// would be" and "where the snapshot says it actually is" is ordinary
// interpolation/quantization noise, not a real discontinuity worth
// smoothing (see SnapshotApplier::Apply) -- same epsilon-gating idea as
// ClientPrediction::m_positionEpsilon, independently tuned for this much
// lower-stakes cosmetic purpose.
constexpr double JUMP_TOLERANCE = 3.0;

} // namespace

namespace Gravitaris {

SnapshotApplier::SnapshotApplier(flecs::world& world, ResourceLoader& resourceLoader)
        : m_world(world)
        , m_resourceLoader(resourceLoader)
{}

void SnapshotApplier::Apply(const SnapshotData& snapshot, float dtSeconds)
{
    // Destroy entities absent from this snapshot first (full snapshots: not
    // being in one means the entity is gone). snapshot.entities is sorted by
    // netId, so a binary search per live entity suffices. Collect-then-mutate,
    // as everywhere else that destroys during iteration.
    std::vector<std::uint32_t> presentIds;
    presentIds.reserve(snapshot.entities.size());
    for (const EntityState& state : snapshot.entities) presentIds.push_back(state.netId);

    std::vector<std::uint32_t> gone;
    for (const auto& [netId, entity] : m_byNetId) {
        if (!std::binary_search(presentIds.begin(), presentIds.end(), netId) || !entity.is_alive()) {
            gone.push_back(netId);
        }
    }
    for (const std::uint32_t netId : gone) {
        const auto it = m_byNetId.find(netId);
        if (it->second.is_alive()) it->second.destruct();
        m_byNetId.erase(it);
    }

    for (const EntityState& state : snapshot.entities) {
        const auto it = m_byNetId.find(state.netId);
        flecs::entity entity = (it != m_byNetId.end()) ? it->second : flecs::entity{};

        if (!entity.is_alive()) {
            entity = m_world.entity();
            entity.emplace<NetId>(state.netId);
            entity.emplace<Transform>(Vector2d{state.pos}, Radd{static_cast<double>(state.rot)},
                                      Vector2d{state.scale}, Vector2d{state.vel});
            if (state.teamId != TeamId::None) {
                entity.emplace<Team>(state.teamId);
            }
            if (state.type == NetEntityType::Ship) {
                entity.emplace<Controls>();
                // Ships and structures are Team+Damageable in the real sim
                // (see camera/minimap's "enemy"/"ship dot" queries, which
                // structures aren't part of yet -- only Damageable matters
                // here) -- bullets can carry Team too (friendly-fire check)
                // but never Damageable, so gating like this matches that.
                entity.emplace<Damageable>(state.hp, state.maxHp);
                entity.emplace<ShipLoadout>();
                entity.emplace<ResearchAccess>();
                // Presence-only marker, same idiom as Orbit below: nothing in
                // this world drives a freighter, but the minimap asks
                // `has<Freighter>()` of both worlds, and without this a
                // replicated hauler reads as an ordinary fighter.
                if (state.isFreighter) entity.emplace<Freighter>();
                entity.emplace<HitOutline>(m_resourceLoader.Load<Body>(state.modelId));
                ApplyEntityShipState(entity, state);
            }
            if (state.type == NetEntityType::Structure) {
                entity.emplace<Damageable>(state.hp, state.maxHp);
                entity.emplace<HitOutline>(m_resourceLoader.Load<Body>(state.modelId));
                entity.emplace<Structure>(Structure{state.structureType, state.rawMaterials,
                                                    state.finishedMaterials, state.researchProgress});
            }
            if (state.type == NetEntityType::Planet) {
                entity.emplace<GravitySource>(GravitySource{state.gravityMass, state.gravityMultiplier});
                // Same source PhysicsSystem::GetBody(ref) used to read
                // server-side -- reconstructed once here from the same Body
                // resource by modelId, so camera/minimap radius queries work
                // against this mirror world with no live PhysicsSystem (see
                // Planet's own doc comment). Never re-derived after creation:
                // a planet's model (and so its radius) never changes.
                const ResourcePtr<const Body> body = m_resourceLoader.Load<Body>(state.modelId);
                const float radius = body->GetCircleShapes().empty()
                        ? 0.f : static_cast<float>(body->GetCircleShapes().front().radius);
                entity.emplace<Planet>(radius, state.isStar);
                // Its presence is the star-vs-planet check that camera and
                // minimap make in both worlds (`!entity.has<Orbit>()`), and
                // its PARAMETERS are how the minimap knows how far out a
                // planet swings -- fitting to live positions instead would
                // have the map breathe as the sector turns. Left empty here,
                // the fit saw every replicated planet as a body sitting on the
                // origin and framed a multiplayer sector to its suns alone.
                // centerMass is the one field the wire does not carry and
                // nothing here wants: OrbitSystem never runs against this
                // world, so nothing re-derives an angular speed it already has.
                if (!state.isStar) {
                    entity.emplace<Orbit>(Orbit{
                            Vector2d{state.orbitCenter.x(), state.orbitCenter.y()},
                            /*centerMass=*/0.0,
                            state.orbitRadius,
                            state.orbitTheta,
                            /*direction=*/state.orbitAngularSpeed < 0.f ? -1.0 : 1.0,
                            state.orbitAngularSpeed});
                }
            }
            entity.emplace<Renderable>(m_resourceLoader.Load<Model>(state.modelId));
            entity.emplace<HitFlash>();
            entity.emplace<ShieldFlash>();
            entity.emplace<RemoteSmoothing>();
            m_byNetId[state.netId] = entity;
        }

        Transform& t = entity.get_mut<Transform>();
        const Vector2d newAuthoritativePos{state.pos};

        // Planets are deliberately exempt: they're always drawn at the
        // newest known state already (see SnapshotInterpolator::Compute's
        // own comment on why -- landed-ship/proxy alignment and camera
        // framing both need that, not smoothed motion), so there's no
        // extrapolation-freeze discontinuity to smooth here in the first
        // place.
        if (state.type != NetEntityType::Planet) {
            RemoteSmoothing& smooth = entity.get_mut<RemoteSmoothing>();
            smooth.offset *= std::exp(-static_cast<double>(dtSeconds) / SMOOTH_TAU_SECONDS);

            // Where this entity's own last-reported velocity said it should
            // have ended up, continuing from the last snapshot's true
            // authoritative state -- NOT from t.pos, which already has this
            // (possibly still-decaying) offset baked in. Extrapolating from
            // t.pos would make next frame's prediction chase its own tail:
            // a still-large offset shifts "predicted" away from the real
            // trajectory, so the same discontinuity gets re-detected as new
            // every frame and the offset keeps growing instead of decaying
            // (see RemoteSmoothing's own doc comment). SnapshotInterpolator's
            // straight-line extrapolation freezing at its cap, then a real
            // snapshot snapping past that frozen point, is the jump this
            // catches -- see the Phase 6 follow-up in docs/networking-plan.md.
            if (smooth.hasLast) {
                const Vector2d predicted =
                        smooth.lastAuthoritativePos + smooth.lastAuthoritativeVel * static_cast<double>(dtSeconds);
                const Vector2d jump = newAuthoritativePos - predicted;
                if (jump.length() > JUMP_TOLERANCE) {
                    smooth.offset += predicted - newAuthoritativePos;
                }
            }
            smooth.lastAuthoritativePos = newAuthoritativePos;
            smooth.lastAuthoritativeVel = Vector2d{state.vel};
            smooth.hasLast = true;

            t.prevPos = t.pos;
            t.pos = newAuthoritativePos + smooth.offset;
        }
        else {
            t.prevPos = t.pos;
            t.pos = newAuthoritativePos;
        }
        t.rot = Radd{static_cast<double>(state.rot)};
        t.scale = Vector2d{state.scale};
        t.vel = Vector2d{state.vel};
        t.angVel = static_cast<double>(state.angVel);

        if (Controls* controls = entity.try_get_mut<Controls>()) {
            controls->actionFlags = UnpackControlFlags(state.controlsFlags);
            controls->actionFlags.aim = state.aim;
            // The wire carries the burn the server granted, not the button
            // the pilot held (see GatherSnapshot), so this world's copy is
            // the granted state directly -- nothing here runs the timers that
            // would otherwise decide it.
            controls->boosting = controls->actionFlags.boost;
            controls->laserFiring = controls->actionFlags.fireLaser;
            // Both halves of the commitment arrive as one number: what is left
            // to charge. Nothing owes a burn in this world -- the beam bit
            // above is the server's answer to that already.
            controls->laserWindup = state.laserWindup;
        }
        // Ownership changes mid-round (planet claims); creation-time Team
        // alone would leave the mirror stale.
        if (state.teamId != TeamId::None) {
            entity.set<Team>(Team{state.teamId});
        }
        ApplyEntityShipState(entity, state);
        if (Structure* structure = entity.try_get_mut<Structure>()) {
            structure->rawMaterials = state.rawMaterials;
            structure->finishedMaterials = state.finishedMaterials;
            structure->researchProgress = state.researchProgress;
        }
        if (GravitySource* source = entity.try_get_mut<GravitySource>()) {
            source->mass = state.gravityMass;
            source->multiplier = state.gravityMultiplier;
        }
    }
}

} // namespace Gravitaris
