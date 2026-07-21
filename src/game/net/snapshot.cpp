#include <algorithm>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/orbit.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/input/input-command.hpp>
#include <gravitaris/game/net/byte-stream.hpp>
#include <gravitaris/game/net/snapshot.hpp>

namespace Gravitaris {

// Bump on any wire-layout change; ReadSnapshot rejects mismatches outright
// (no cross-version compatibility until there's a reason to have it).
static constexpr std::uint8_t SNAPSHOT_VERSION = 4; // v4: +structureType/rawMaterials/finishedMaterials

// Sanity caps so a garbage buffer can't make ReadSnapshot allocate wildly.
static constexpr std::uint32_t MAX_ENTITIES = 4096;
static constexpr std::uint32_t MAX_EVENTS = GameEventQueue::CAPACITY;

void GatherSnapshot(flecs::world& world, const GameEventQueue& eventQueue, std::uint64_t tick,
                    std::uint32_t eventsSinceSeq, SnapshotData& out, std::uint32_t suppressBulletsOwnedBy)
{
    out.tick = tick;
    out.entities.clear();
    out.events.clear();

    world.each([&](flecs::entity entity, const NetId& netId, const Transform& t) {
        if (suppressBulletsOwnedBy != 0) {
            if (const Bullet* bullet = entity.try_get<Bullet>();
                bullet && bullet->ownerNetId == suppressBulletsOwnedBy) {
                return;
            }
        }

        EntityState state;
        state.netId = netId.value;
        state.type = entity.has<Planet>() ? NetEntityType::Planet
                   : entity.has<Bullet>() ? NetEntityType::Bullet
                   : entity.has<Structure>() ? NetEntityType::Structure
                                             : NetEntityType::Ship;
        if (const RigidBodyDesc* desc = entity.try_get<RigidBodyDesc>()) {
            state.modelId = desc->body.Id();
        }
        if (const Team* team = entity.try_get<Team>()) {
            state.teamId = team->id;
        }
        state.pos = Magnum::Vector2{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())};
        state.rot = static_cast<float>(static_cast<double>(t.rot));
        state.scale = Magnum::Vector2{static_cast<float>(t.scale.x()), static_cast<float>(t.scale.y())};
        state.vel = Magnum::Vector2{static_cast<float>(t.vel.x()), static_cast<float>(t.vel.y())};
        state.angVel = static_cast<float>(t.angVel);
        if (const Controls* controls = entity.try_get<Controls>()) {
            state.controlsFlags = PackControlFlags(controls->actionFlags);
        }
        if (const Damageable* damageable = entity.try_get<Damageable>()) {
            state.hp = damageable->hp;
        }
        if (const GravitySource* source = entity.try_get<GravitySource>()) {
            state.gravityMass = static_cast<float>(source->mass);
            state.gravityMultiplier = source->multiplier;
        }
        if (state.type == NetEntityType::Planet) {
            state.isStar = !entity.has<Orbit>();
        }
        if (const Structure* structure = entity.try_get<Structure>()) {
            state.structureType = structure->type;
            state.rawMaterials = structure->rawMaterials;
            state.finishedMaterials = structure->finishedMaterials;
        }
        out.entities.push_back(state);
    });

    std::sort(out.entities.begin(), out.entities.end(),
              [](const EntityState& a, const EntityState& b) { return a.netId < b.netId; });

    eventQueue.ConsumeSince(eventsSinceSeq, [&](const GameEvent& event) {
        out.events.push_back(event);
    });
}

void SerializeSnapshot(const SnapshotData& snapshot, ByteWriter& out)
{
    out.WriteU8(SNAPSHOT_VERSION);
    out.WriteU64(snapshot.tick);

    out.WriteU32(static_cast<std::uint32_t>(snapshot.entities.size()));
    for (const EntityState& e : snapshot.entities) {
        out.WriteU32(e.netId);
        out.WriteU8(static_cast<std::uint8_t>(e.type));
        out.WriteU32(e.modelId);
        out.WriteU8(static_cast<std::uint8_t>(e.teamId));
        out.WriteF32(e.pos.x());
        out.WriteF32(e.pos.y());
        out.WriteF32(e.rot);
        out.WriteF32(e.scale.x());
        out.WriteF32(e.scale.y());
        out.WriteF32(e.vel.x());
        out.WriteF32(e.vel.y());
        out.WriteF32(e.angVel);
        out.WriteU8(e.controlsFlags);
        out.WriteF32(e.hp);
        out.WriteF32(e.gravityMass);
        out.WriteF32(e.gravityMultiplier);
        out.WriteU8(e.isStar ? 1 : 0);
        out.WriteU8(static_cast<std::uint8_t>(e.structureType));
        out.WriteF32(e.rawMaterials);
        out.WriteF32(e.finishedMaterials);
    }

    out.WriteU32(static_cast<std::uint32_t>(snapshot.events.size()));
    for (const GameEvent& event : snapshot.events) {
        out.WriteU32(event.seq);
        out.WriteU64(event.tick);
        out.WriteU8(static_cast<std::uint8_t>(event.type));
        out.WriteU32(event.sourceNetId);
        out.WriteF32(event.pos.x());
        out.WriteF32(event.pos.y());
        out.WriteU32(event.param);
    }
}

void WriteSnapshot(flecs::world& world, const GameEventQueue& eventQueue, std::uint64_t tick,
                   std::uint32_t eventsSinceSeq, ByteWriter& out)
{
    SnapshotData snapshot;
    GatherSnapshot(world, eventQueue, tick, eventsSinceSeq, snapshot);
    SerializeSnapshot(snapshot, out);
}

bool ReadSnapshot(ByteReader& in, SnapshotData& out)
{
    if (const std::uint8_t version = in.ReadU8(); version != SNAPSHOT_VERSION) {
        // Symptom of a version-mismatched rejection is a client that welcomes
        // fine but never syncs (e.g. a stale cached wasm build) -- worth one
        // loud line instead of silence.
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG(warning) << "net: rejecting snapshot version " << int(version)
                         << " (this build expects " << int(SNAPSHOT_VERSION)
                         << ") -- mismatched client/server builds? Stale cached wasm?";
        }
        return false;
    }
    out.tick = in.ReadU64();

    const std::uint32_t entityCount = in.ReadU32();
    if (entityCount > MAX_ENTITIES) return false;
    out.entities.clear();
    out.entities.reserve(entityCount);
    for (std::uint32_t i = 0; i < entityCount; ++i) {
        EntityState e;
        e.netId = in.ReadU32();
        e.type = static_cast<NetEntityType>(in.ReadU8());
        e.modelId = in.ReadU32();
        e.teamId = static_cast<TeamId>(in.ReadU8());
        e.pos.x() = in.ReadF32();
        e.pos.y() = in.ReadF32();
        e.rot = in.ReadF32();
        e.scale.x() = in.ReadF32();
        e.scale.y() = in.ReadF32();
        e.vel.x() = in.ReadF32();
        e.vel.y() = in.ReadF32();
        e.angVel = in.ReadF32();
        e.controlsFlags = in.ReadU8();
        e.hp = in.ReadF32();
        e.gravityMass = in.ReadF32();
        e.gravityMultiplier = in.ReadF32();
        e.isStar = in.ReadU8() != 0;
        e.structureType = static_cast<StructureType>(in.ReadU8());
        e.rawMaterials = in.ReadF32();
        e.finishedMaterials = in.ReadF32();
        out.entities.push_back(e);
    }

    const std::uint32_t eventCount = in.ReadU32();
    if (eventCount > MAX_EVENTS) return false;
    out.events.clear();
    out.events.reserve(eventCount);
    for (std::uint32_t i = 0; i < eventCount; ++i) {
        GameEvent event;
        event.seq = in.ReadU32();
        event.tick = in.ReadU64();
        event.type = static_cast<GameEventType>(in.ReadU8());
        event.sourceNetId = in.ReadU32();
        event.pos.x() = in.ReadF32();
        event.pos.y() = in.ReadF32();
        event.param = in.ReadU32();
        out.events.push_back(event);
    }

    return in.Ok();
}

} // namespace Gravitaris
