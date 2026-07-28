#pragma once

#include <cstdint>

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Tactics layer of the pilot stack: a small utility selector picks a guidance
// behavior per AI ship, the GNC stack turns it into control bits, and the
// result is pushed as a tick-stamped InputCommand. Runs before InputSystem so
// commands are consumed the same tick.
class AIPilotSystem {
private:
    flecs::world& m_registry;

    PhysicsSystem& m_physicsSystem;

    TrajectoryPredictor& m_predictor;

    const UpgradeCatalog& m_catalog;

public:
    // `catalog` resolves each pilot's own fitted gun, so a pilot that has
    // collected a heavier round leads its shots at that round's speed rather
    // than at the stock one's.
    AIPilotSystem(flecs::world& registry, PhysicsSystem& physicsSystem, TrajectoryPredictor& predictor,
                  const UpgradeCatalog& catalog);

    ~AIPilotSystem() = default;

    // Target acquisition is registry-driven (nearest live enemy ship), not a
    // caller-supplied entity -- there's no single "the player" to hand it on
    // a dedicated server with any number of connected peers (Game::m_player
    // is single-player-only bookkeeping, always nullopt there). See Update's
    // own doc comment.
    void Update(std::uint64_t step);
};

} // namespace Gravitaris
