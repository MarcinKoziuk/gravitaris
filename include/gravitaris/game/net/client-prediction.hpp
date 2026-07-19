#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include <flecs.h>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/id.hpp>
#include <gravitaris/game/net/snapshot.hpp>

struct cpBody;

namespace Gravitaris {

// Client-side prediction + reconciliation for the local player's own ship
// only (docs/networking-plan.md Phase 5; every other entity stays on Phase
// 4's interpolation path via SnapshotInterpolator/SnapshotApplier). Owns a
// single locally-simulated entity, spawned via the same EntitySpawner::
// SpawnPlayer real single-player uses -- real Chipmunk body, real
// RigidBodyDesc("main"_id, ...). Since nothing else is ever spawned into
// this registry, that body is alone in its own Chipmunk space: there is
// nothing else to collide with, satisfying ADR 0001 constraint 6's "no
// ship-ship contacts during replay" by construction rather than by
// filtering.
//
// Gravity is computed manually from the Planet-typed EntityStates in the
// latest snapshot (their replicated position + GravitySource fields) rather
// than by running a second, independently-seeded OrbitSystem simulation
// locally: two orbit simulations starting from different wall-clock moments
// (server boot vs. this client's join time) would need their phase
// synchronized somehow, and simply reading the already-correct, already
// -interpolated replicated position sidesteps that entirely. Trade-off:
// this predicted ship never collides with a planet during prediction/replay
// (no local planet shape exists to hit) -- an accepted approximation beyond
// the ADR's own "no ship-ship contacts" one; any discrepancy is corrected
// on the next reconciliation once the server's real collision response
// arrives in a snapshot.
//
// Headless (game/-level, ADR constraint 1): built entirely from
// PhysicsSystem/EntitySpawner (which a real Game already owns) plus plain
// EntityState/ControlFlags data, so it's testable in gravitaris-sim-test
// with no cgame/GL dependency -- CGame only wires input sampling and
// rendering around it.
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

    // Cosmetic bullets only need to live until the server's authoritative
    // bullet arrives and renders (~RTT/2 + input lead + interp delay), not
    // the real BULLET_LIFETIME_SECONDS -- otherwise both streams stay
    // visible side by side for seconds per shot.
    static constexpr double COSMETIC_BULLET_LIFETIME_SECONDS = 0.35;

    // How many predicted ticks are kept -- generous headroom (3s @ 60Hz) for
    // however long a snapshot round-trip takes; older entries are dropped
    // (Reconcile() then has nothing to compare against for that tick and
    // just leaves the discarded prediction uncorrected, same as if it had
    // matched).
    static constexpr std::size_t MAX_HISTORY = 180;

    ClientPrediction(flecs::world& registry, PhysicsSystem& physicsSystem, EntitySpawner& entitySpawner,
                     GameEventQueue& eventQueue);

    // Idempotent: only the first call actually spawns anything.
    void SpawnOwnShip(id_t modelId, Magnum::Vector2d initialPos);

    [[nodiscard]] bool HasOwnShip() const;
    [[nodiscard]] flecs::entity GetOwnShip() const { return m_ownShip; }

    // Advances the local prediction by exactly one Game::PHYSICS_DELTA tick:
    // applies `flags` (rotation/thrust, and now firePrimary -- Phase 6), the
    // same weapon cooldown ShipControlsSystem uses so cadence matches, gravity
    // from `planets` (this snapshot's Planet-typed EntityStates), integrates,
    // and records the result keyed by `tick`. On firing, spawns a sensor
    // bullet directly into the registry (cosmetic only -- expired by the
    // caller's own BulletLifetimeSystem, never reconciled against the
    // server's real bullet) and emits a local BulletFired event into
    // `eventQueue` purely so AudioSystem's existing event-driven one-shot
    // path plays the fire sound; this event is never serialized/sent. The
    // caller owns tick numbering (see NetClient::SendInput -- the same
    // number must be sent on the wire for Reconcile() to later find this
    // entry). No-op if SpawnOwnShip hasn't been called yet.
    void Step(std::uint64_t tick, const ControlFlags& flags, const std::vector<EntityState>& planets);

    // Compares the authoritative state for `authoritativeTick` (from a
    // newly arrived snapshot) against what was predicted for it; if they've
    // diverged past POSITION_EPSILON, snaps to the authoritative state and
    // replays every predicted tick after it (against `planets`, the same
    // snapshot's current known planet positions -- not each replayed tick's
    // own historical ones, another accepted approximation). Returns the
    // pre-correction predicted position (for the caller to blend a visual
    // correction from) if a correction happened, nullopt if the tick wasn't
    // found (already evicted, or not predicted yet) or was within epsilon.
    std::optional<Magnum::Vector2d> Reconcile(std::uint64_t authoritativeTick, const EntityState& authoritative,
                                              const std::vector<EntityState>& planets);

private:
    void ApplyGravity(cpBody* body, const std::vector<EntityState>& planets);
    PredictedTick CaptureTick(std::uint64_t tick, const ControlFlags& flags);

    flecs::world& m_registry;
    PhysicsSystem& m_physicsSystem;
    EntitySpawner& m_entitySpawner;
    GameEventQueue& m_eventQueue;

    flecs::entity m_ownShip;
    std::deque<PredictedTick> m_history;
    std::uint32_t m_fireCooldown = 0;
};

} // namespace Gravitaris
