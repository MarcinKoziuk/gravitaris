#pragma once

#include <cstdint>
#include <vector>

#include <flecs.h>

#include <Magnum/Magnum.h>
#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/event/game-event.hpp>

namespace Gravitaris {

class ByteWriter;
class ByteReader;

enum class NetEntityType : std::uint8_t {
    Ship,
    Bullet,
    Planet, // any celestial (suns included) -- they replicate identically
};

// One entity's replicated state (docs/networking-plan.md 2.2), the v1
// brutal-simple schema: full state, f32 wire precision, no deltas. The sim's
// doubles are truncated to f32 here on purpose -- this is the network
// contract's precision, and ADR 0001 constraint 8 allows full fat snapshots
// until bandwidth says otherwise.
struct EntityState {
    std::uint32_t netId = 0;
    NetEntityType type = NetEntityType::Ship;
    id_t modelId = 0; // asset-path FNV hash, stable across builds/platforms
    TeamId teamId = TeamId::None;
    Magnum::Vector2 pos{};
    float rot = 0.f;
    Magnum::Vector2 scale{1.f, 1.f};
    Magnum::Vector2 vel{};
    float angVel = 0.f;
    std::uint8_t controlsFlags = 0; // PackControlFlags(); drives remote thruster visuals
    float hp = 0.f;
};

// One decoded snapshot: entities in ascending-NetId order (flecs iteration
// order is not stable, and a canonical order makes snapshots byte-comparable
// and delta-able later), plus every buffered event newer than the seq the
// writer was asked for.
struct SnapshotData {
    std::uint64_t tick = 0;
    std::vector<EntityState> entities;
    std::vector<GameEvent> events;
};

// Collects the replicated state of every NetId-bearing entity plus events
// with seq > eventsSinceSeq. Reads only replicated components.
void GatherSnapshot(flecs::world& world, const GameEventQueue& eventQueue, std::uint64_t tick,
                    std::uint32_t eventsSinceSeq, SnapshotData& out);

void SerializeSnapshot(const SnapshotData& snapshot, ByteWriter& out);

// Gather + Serialize in one call (the common server-side path).
void WriteSnapshot(flecs::world& world, const GameEventQueue& eventQueue, std::uint64_t tick,
                   std::uint32_t eventsSinceSeq, ByteWriter& out);

// False on a truncated/garbage buffer (wrong version byte, counts that don't
// fit the remaining bytes); `out` contents are unspecified then.
bool ReadSnapshot(ByteReader& in, SnapshotData& out);

} // namespace Gravitaris
