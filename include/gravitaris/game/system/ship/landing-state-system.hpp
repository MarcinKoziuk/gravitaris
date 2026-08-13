#pragma once

#include <cstdint>

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Updates every ship's LandingState each tick: landed = resting on a planet
// (live contact, slow relative to it, upright). ConquestSystem consumes the
// result for claiming; Phase 4's respawn rule reads lastFriendlySiteNetId.
class LandingStateSystem {
public:
    // Relative speed (world units/s) below which contact counts as resting
    // rather than an impact still in progress. Under DamageSystem's landing
    // thresholds (70) so "landed" still implies "took no damage"; also the
    // HUD's "safe to land" threshold.
    static constexpr double SAFE_LANDING_SPEED = 20.0;

    // How many ticks of failed contact a landing survives before it counts as
    // a departure. A hull at rest chatters: Chipmunk drops an arbiter's contact
    // points for a tick at a time, and friction re-grabs a moving site's
    // velocity rather than matching it exactly. Judged raw, each such tick is a
    // full takeoff and re-landing -- which restarts the claim counter below and
    // strobes ResearchAccess::atLab, and with it the client's refit board. Far
    // shorter than the yard's own REFIT_GRACE_TICKS: this bridges chatter, not
    // a round trip.
    static constexpr std::uint8_t LANDING_GRACE_TICKS = 15; // 0.25s at the fixed tick

private:
    flecs::world& m_registry;

    PhysicsSystem& m_physicsSystem;
    FactionSystem& m_factionSystem;

public:
    LandingStateSystem(flecs::world& registry, PhysicsSystem& physicsSystem, FactionSystem& factionSystem);

    void Update();
};

} // namespace Gravitaris
