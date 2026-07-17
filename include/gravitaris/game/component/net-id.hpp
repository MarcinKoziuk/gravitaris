#pragma once

#include <cstdint>

namespace Gravitaris {

// Server-assigned, replication-stable identity for an entity (ADR 0001
// constraint 3: never serialize raw flecs ids -- they're process-local).
// Replicated components that reference another entity (projectile owner,
// missile target) store a NetId, not a flecs::entity. Each side keeps a
// NetId <-> entity map (see EntitySpawner's registry).
//
// Replication class: replicated (server -> clients). Plain POD.
struct NetId {
    std::uint32_t value = 0; // 0 = unassigned/invalid
};

} // namespace Gravitaris
