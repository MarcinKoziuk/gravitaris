#pragma once

#include <cstdint>

#include <flecs.h>

#include <ankerl/unordered_dense.h>

#include <gravitaris/game/fwd.hpp>
#include <gravitaris/game/net/snapshot.hpp>

namespace Gravitaris {

// Applies decoded snapshots into a presentation-only client world: creates
// missing entities by NetId, updates existing ones, destroys absent ones.
// Created entities get Transform/Team/Controls/Renderable/HitFlash but
// deliberately NO RigidBodyDesc -- remote entities have no client physics
// (ADR 0001 constraint 6); their motion comes entirely from snapshots.
// No sim system may run on this world. Owns its own NetId -> entity map
// (there is no EntitySpawner here; NetIds are the server's, applied as-is).
//
// v1 gaps, deliberate: hp is parsed but not applied (nothing renders health
// yet) and events are left to the caller (the live mirror plays audio/flash
// from the real sim's queue; a real remote client wires them in Phase 3).
class SnapshotApplier {
    flecs::world& m_world;
    ResourceLoader& m_resourceLoader;

    ankerl::unordered_dense::map<std::uint32_t, flecs::entity> m_byNetId;

public:
    SnapshotApplier(flecs::world& world, ResourceLoader& resourceLoader);

    void Apply(const SnapshotData& snapshot);

    // The mirror-world entity for a replicated NetId, or an invalid entity if
    // none exists (not yet applied, or absent from the latest snapshot).
    [[nodiscard]] flecs::entity EntityForNetId(std::uint32_t netId) const
    {
        const auto it = m_byNetId.find(netId);
        return it != m_byNetId.end() ? it->second : flecs::entity{};
    }
};

} // namespace Gravitaris
