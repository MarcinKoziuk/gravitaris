#pragma once

#include <cstdint>

#include <gravitaris/game/component/team.hpp>

namespace Gravitaris {

struct Bullet {
    double remainingLifetime;
    // Shooter's team, so DamageSystem's hit query can skip friendly fire.
    TeamId team = TeamId::Blue;
    float damage = 10.f;
    // NetId of the ship that fired this (0 = nobody replicated -- structure
    // defenses, death shrapnel). Server-only, deliberately NOT serialized:
    // its whole job is letting GatherSnapshot omit a peer's own bullets from
    // that peer's snapshots, so the client renders its locally-predicted
    // copy instead of a second, time-delayed authoritative one (see
    // ClientPrediction::Step). Omitting an entity needs no wire field.
    std::uint32_t ownerNetId = 0;
};

} // namespace Gravitaris
