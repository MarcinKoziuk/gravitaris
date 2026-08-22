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
    // Whatever fired this, as a raw flecs id. A round leaves from a mount
    // inside its own hull, so the first thing the hit sweep meets is the
    // shooter itself -- with friendly fire on the team rule no longer covers
    // that, and every shot would land on the ship that took it. Zero for
    // death shrapnel, which belongs to nobody and hits everyone. Sim state
    // like ownerNetId, and deliberately not serialized for the same reason.
    std::uint64_t shooter = 0;
    // Whether this round went out to clients as a Shot (see
    // event/shot-stream.hpp) rather than as a replicated entity. Set by
    // EntitySpawner::SpawnRound, read by GatherSnapshot, which leaves such a
    // round out of the snapshot entirely -- every client is flying its own
    // copy from the spawn instruction.
    //
    // The flag rather than the inverse: a round nobody thought about is
    // replicated the old, expensive, visible way. A guided round has to be,
    // since no client can extrapolate a seeker, and the box the secondary
    // fire throws out is not a round at all. Server-only, like the two fields
    // above and for the same reason.
    bool clientFlown = false;
};

} // namespace Gravitaris
