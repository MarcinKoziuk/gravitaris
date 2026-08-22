#pragma once

#include <array>
#include <cstdint>

#include <flecs.h>

#include <gravitaris/game/component/structure.hpp>

namespace Gravitaris {

// Ticks left before a planet's build site of each type reopens, counted down
// by EconomySystem. It rides the PLANET rather than the wreck because the
// wreck is gone by the time anything asks -- DeathSystem writes it as the
// structure comes down -- and it is per type because levelling a Lab is no
// reason to stall the Colony standing next to it.
//
// Everything that puts a structure on a planet honours it: freighter
// dispatch, a Base developing its own rock, and the unload at the far end of
// a run that was already in the air when the site was flattened.
//
// Replication class: server-only. What a side cannot build yet is a decision,
// not state a client renders (ADR 0001 constraint 2).
struct RebuildLockout {
    std::array<std::uint32_t, STRUCTURE_TYPE_COUNT> ticks{};

    [[nodiscard]] bool Blocks(StructureType type) const
    {
        return ticks[static_cast<std::size_t>(type)] > 0;
    }
};

// Whether `planet` is currently refusing `type`. Takes an entity rather than
// the component so the (common) case of a planet nothing has been shot off
// yet needs no lookup at the call site.
[[nodiscard]] inline bool RebuildBlocked(flecs::entity planet, StructureType type)
{
    const RebuildLockout* lockout = planet.is_alive() ? planet.try_get<RebuildLockout>() : nullptr;
    return lockout && lockout->Blocks(type);
}

} // namespace Gravitaris
