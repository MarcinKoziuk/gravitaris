#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

#include <flecs.h>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/id.hpp>
#include <gravitaris/game/net/snapshot.hpp>

namespace Gravitaris {

// Client-side prediction + reconciliation for the local player's own ship
// only (docs/networking-plan.md Phase 5; every other entity stays on Phase
// 4's interpolation path via SnapshotInterpolator/SnapshotApplier). Owns a
// single locally-simulated entity, spawned via the same EntitySpawner::
// SpawnPlayer real single-player uses -- real Chipmunk body, real
// RigidBodyDesc("main"_id, ...). Nothing but this ship and the planet
// -collision proxies below (Phase 7) is ever spawned into this registry, so
// there is no *other ship* to collide with, satisfying ADR 0001 constraint
// 6's "no ship-ship contacts during replay" by construction rather than by
// filtering.
//
// Planets are real kinematic collision proxies now (Phase 7,
// SyncPlanetProxies), positioned each tick from the replicated Planet
// -typed EntityStates in the latest snapshot rather than by running a
// second, independently-seeded OrbitSystem simulation locally: two orbit
// simulations starting from different wall-clock moments (server boot vs.
// this client's join time) would need their phase synchronized somehow, and
// simply reading the already-correct, already-interpolated replicated
// position sidesteps that entirely. Gravity is no longer a manual force
// hack either -- PhysicsSystem::Simulate's own per-space ApplyGravity finds
// these proxies' GravitySource components and just works, same as
// single-player. Landing on a planet during prediction now behaves
// correctly (real contact response against the moving kinematic surface);
// ship-ship contact is still not predicted (see the proxies' own doc
// comment on why that one stays out of scope) -- any discrepancy there is
// corrected on the next reconciliation once the server's real collision
// response arrives in a snapshot.
//
// Headless (game/-level, ADR constraint 1): built entirely from
// PhysicsSystem/EntitySpawner/ResourceLoader (which a real Game already
// owns) plus plain EntityState/ControlFlags data, so it's testable in
// gravitaris-sim-test with no cgame/GL dependency -- CGame only wires input
// sampling and rendering around it.
class ClientPrediction {
public:
    struct PredictedTick {
        std::uint64_t tick = 0;
        ControlFlags flags{};
        Magnum::Vector2d pos;
        Magnum::Radd rot{0.};
        Magnum::Vector2d vel;
        double angVel = 0.;
    };

    // Position error (world units) past which Reconcile() snaps and replays
    // instead of accepting the divergence as prediction noise. Runtime
    // -tunable (Net debug tab) rather than a constant: 1.0 (the original
    // guess) turned out far tighter than real ship speeds warrant --
    // ordinary f32-wire/quantization noise routinely exceeds it, especially
    // while thrusting/turning, triggering a correction on nearly every
    // snapshot and reading as camera jitter (each one nudges the visual
    // -correction offset). Exposed live instead of re-guessing a constant.
    double m_positionEpsilon = 8.0;
    [[nodiscard]] double GetPositionEpsilon() const { return m_positionEpsilon; }
    void SetPositionEpsilon(double epsilon) { m_positionEpsilon = std::max(epsilon, 0.0); }

    // How many predicted ticks are kept -- generous headroom (3s @ 60Hz) for
    // however long a snapshot round-trip takes; older entries are dropped
    // (Reconcile() then has nothing to compare against for that tick and
    // just leaves the discarded prediction uncorrected, same as if it had
    // matched).
    static constexpr std::size_t MAX_HISTORY = 180;

    ClientPrediction(flecs::world& registry, PhysicsSystem& physicsSystem, EntitySpawner& entitySpawner,
                     GameEventQueue& eventQueue, ResourceLoader& resourceLoader);

    // Idempotent: only the first call actually spawns anything. `team`
    // matters for local rendering (team color) -- pass NetClient::
    // GetYourTeam() so it matches what the server actually assigned.
    void SpawnOwnShip(id_t modelId, Magnum::Vector2d initialPos, TeamId team = TeamId::Blue);

    // Destructs the own ship (if any) and clears prediction state, so a
    // subsequent SpawnOwnShip starts clean. For when the server's ship for
    // this client is gone -- died, or replaced by a respawn under a new
    // NetId -- and the caller (CGame) has detected that by the NetId it was
    // tracking no longer appearing in snapshots. No-op if there's no ship.
    void DestroyOwnShip();

    [[nodiscard]] bool HasOwnShip() const;
    [[nodiscard]] flecs::entity GetOwnShip() const { return m_ownShip; }

    // Advances the local prediction by exactly one Game::PHYSICS_DELTA tick:
    // applies `flags` (rotation/thrust, and now firePrimary -- Phase 6), the
    // same weapon cooldown ShipControlsSystem uses so cadence matches, gravity
    // from `planets` (this snapshot's Planet-typed EntityStates), integrates,
    // and records the result keyed by `tick`. On firing, spawns the
    // cosmetic bullet this client actually sees (zero damage -- hits stay
    // server-authoritative) and emits a local BulletFired event into
    // `eventQueue` so AudioSystem's event-driven one-shot plays the fire
    // sound instantly; neither is ever serialized/sent. The server omits
    // this peer's own bullets from its snapshots (GatherSnapshot's
    // suppressBulletsOwnedBy), so exactly one tracer is ever on screen per
    // shot -- drawing both is what made an earlier attempt show two
    // separate tracers, the own ship rendering ~INPUT_LEAD_TICKS ahead of
    // where replicated entities render. The caller owns tick numbering
    // (see NetClient::SendInput -- the same number must be sent on the wire
    // for Reconcile() to later find this entry). No-op if SpawnOwnShip
    // hasn't been called yet.
    void Step(std::uint64_t tick, const ControlFlags& flags, const std::vector<EntityState>& planets);

    // Compares the authoritative state for `authoritativeTick` (from a
    // newly arrived snapshot) against what was predicted for it; if they've
    // diverged past POSITION_EPSILON, snaps to the authoritative state and
    // replays every predicted tick after it (against `planets`, the same
    // snapshot's current known planet positions -- not each replayed tick's
    // own historical ones, another accepted approximation). If a correction
    // happened, returns where prediction currently says the ship is *right
    // now* (i.e. the most recent predicted tick, before this correction --
    // NOT the historical position at `authoritativeTick`, which can be many
    // ticks in the past due to RTT + interp delay; using that instead was a
    // real bug -- it conflated real correction error with pure travel
    // distance covered since the reconciled tick, producing a systematic
    // backward-then-forward-overshoot visual artifact on every correction).
    // nullopt if the tick wasn't found (already evicted, or not predicted
    // yet) or was within epsilon.
    std::optional<Magnum::Vector2d> Reconcile(std::uint64_t authoritativeTick, const EntityState& authoritative,
                                              const std::vector<EntityState>& planets);

private:
    PredictedTick CaptureTick(std::uint64_t tick, const ControlFlags& flags);

    // Creates/updates/prunes the client-only kinematic collision proxies
    // (Phase 7) for every Planet-typed EntityState in `planets`, keyed by
    // NetId. Idempotent -- safe to call every Step()/Reconcile(). See the
    // class doc comment for why these exist and what they deliberately
    // don't do (no Renderable, no Planet component, no remote-ship
    // equivalent).
    void SyncPlanetProxies(const std::vector<EntityState>& planets);

    flecs::world& m_registry;
    PhysicsSystem& m_physicsSystem;
    EntitySpawner& m_entitySpawner;
    GameEventQueue& m_eventQueue;
    ResourceLoader& m_resourceLoader;

    flecs::entity m_ownShip;
    std::deque<PredictedTick> m_history;
    std::uint32_t m_fireCooldown = 0;

    // Planet NetId -> this client's local collision-proxy entity.
    std::unordered_map<std::uint32_t, flecs::entity> m_planetProxies;
};

} // namespace Gravitaris
