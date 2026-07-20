#pragma once

#include <cstdint>

namespace Gravitaris {

// The seven planetary/orbital installation types (gravity-well-1997.md's
// "Machines of War"). Distinct from Team/Damageable, which every structure
// also carries -- this only says WHICH kind it is; the client can also infer
// that from EntityState::modelId (each type gets its own model), but keeping
// it explicit here means UI/logic never has to reverse-engineer type from a
// model id hash.
enum class StructureType : std::uint8_t {
    Base,
    Colony,
    Lab,
    CommCenter,
    HighPort,
    SpaceDock,
    SensorArray,
};

// Stable membership (a structure's type never changes), so a real component.
//
// Replication class: replicated (server -> clients). rawMaterials/
// finishedMaterials are unused until docs/gravity-well-mode-plan.md Phase 3
// (the freighter/materials economy) -- wired into the wire format now so
// that phase doesn't need a second SNAPSHOT_VERSION bump.
struct Structure {
    StructureType type = StructureType::Base;
    float rawMaterials = 0.f;
    float finishedMaterials = 0.f;
};

// Marks a structure as an auto-firing defense installation (Base, High
// Port) and holds its fire-rate state -- separate from Structure since this
// is pure server-side sim state, not something a client ever needs (unlike
// Structure's fields, which are replicated).
//
// Replication class: server-only.
struct StructureDefense {
    std::uint32_t fireCooldown = 0;
};

} // namespace Gravitaris
