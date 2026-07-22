#include <algorithm>
#include <cmath>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/game.hpp>
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
#include <gravitaris/game/component/planet-attachment.hpp>
#include <gravitaris/game/input/input-command.hpp>
#include <gravitaris/game/net/byte-stream.hpp>
#include <gravitaris/game/net/snapshot.hpp>

namespace Gravitaris {

// Bump on any wire-layout change; ReadSnapshot rejects mismatches outright
// (no cross-version compatibility until there's a reason to have it).
static constexpr std::uint8_t SNAPSHOT_VERSION = 6; // v6: +attachParentNetId/attachRadius/attachTheta/attachAngularSpeed

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
            if (const Orbit* orbit = entity.try_get<Orbit>()) {
                state.orbitCenter = Magnum::Vector2{static_cast<float>(orbit->center.x()),
                                                    static_cast<float>(orbit->center.y())};
                state.orbitRadius = static_cast<float>(orbit->radius);
                state.orbitTheta = static_cast<float>(orbit->theta);
                state.orbitAngularSpeed = static_cast<float>(orbit->angularSpeed);
            }
        }
        if (const Structure* structure = entity.try_get<Structure>()) {
            state.structureType = structure->type;
            state.rawMaterials = structure->rawMaterials;
            state.finishedMaterials = structure->finishedMaterials;
        }
        if (const PlanetSurfaceAttachment* attach = entity.try_get<PlanetSurfaceAttachment>()) {
            state.attachParentNetId = attach->planetNetId;
            state.attachRadius = static_cast<float>(attach->localOffset.length());
            state.attachTheta = static_cast<float>(std::atan2(attach->localOffset.y(), attach->localOffset.x()));
            state.attachAngularSpeed = 0.f; // fixed offset -- a zero-angular-speed "orbit"
        }
        else if (const PlanetOrbitAttachment* attach = entity.try_get<PlanetOrbitAttachment>()) {
            state.attachParentNetId = attach->planetNetId;
            state.attachRadius = static_cast<float>(attach->radius);
            state.attachTheta = static_cast<float>(attach->theta);
            state.attachAngularSpeed = static_cast<float>(attach->angularSpeed);
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
        out.WriteF32(e.orbitCenter.x());
        out.WriteF32(e.orbitCenter.y());
        out.WriteF32(e.orbitRadius);
        out.WriteF32(e.orbitTheta);
        out.WriteF32(e.orbitAngularSpeed);
        out.WriteU8(static_cast<std::uint8_t>(e.structureType));
        out.WriteF32(e.rawMaterials);
        out.WriteF32(e.finishedMaterials);
        out.WriteU32(e.attachParentNetId);
        out.WriteF32(e.attachRadius);
        out.WriteF32(e.attachTheta);
        out.WriteF32(e.attachAngularSpeed);
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
        e.orbitCenter.x() = in.ReadF32();
        e.orbitCenter.y() = in.ReadF32();
        e.orbitRadius = in.ReadF32();
        e.orbitTheta = in.ReadF32();
        e.orbitAngularSpeed = in.ReadF32();
        e.structureType = static_cast<StructureType>(in.ReadU8());
        e.rawMaterials = in.ReadF32();
        e.finishedMaterials = in.ReadF32();
        e.attachParentNetId = in.ReadU32();
        e.attachRadius = in.ReadF32();
        e.attachTheta = in.ReadF32();
        e.attachAngularSpeed = in.ReadF32();
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

void EvaluateOrbit(const EntityState& planet, std::uint64_t baseTick, std::uint64_t atTick,
                   Magnum::Vector2d& outPos, Magnum::Vector2d& outVel)
{
    // Signed tick delta (atTick can be behind baseTick briefly -- e.g. a
    // reconciliation replaying ticks not-yet-newer than the snapshot that
    // just arrived) -- same math either direction, just run the clock
    // backward. atTick/baseTick are uint64_t: casting to double before
    // subtracting is load-bearing, not redundant -- subtracting first would
    // wrap around unsigned and teleport the planet.
    const double elapsedSeconds = (static_cast<double>(atTick) - static_cast<double>(baseTick)) * Game::PHYSICS_DELTA;
    const double theta = planet.orbitTheta + planet.orbitAngularSpeed * elapsedSeconds;

    const double c = std::cos(theta);
    const double s = std::sin(theta);
    const Magnum::Vector2d center{planet.orbitCenter.x(), planet.orbitCenter.y()};
    const double radius = planet.orbitRadius;
    const double angularSpeed = planet.orbitAngularSpeed;

    outPos = center + Magnum::Vector2d{c, s} * radius;
    outVel = Magnum::Vector2d{-s, c} * (angularSpeed * radius);
}

void EvaluateAttachment(const Magnum::Vector2d& parentPos, const Magnum::Vector2d& parentVel,
                        const EntityState& attached, std::uint64_t baseTick, std::uint64_t atTick,
                        Magnum::Vector2d& outPos, Magnum::Vector2d& outVel, Magnum::Vector2d& outLocalVel)
{
    const double elapsedSeconds = (static_cast<double>(atTick) - static_cast<double>(baseTick)) * Game::PHYSICS_DELTA;
    const double theta = attached.attachTheta + attached.attachAngularSpeed * elapsedSeconds;

    const double c = std::cos(theta);
    const double s = std::sin(theta);
    const double radius = attached.attachRadius;
    const double angularSpeed = attached.attachAngularSpeed;

    outLocalVel = Magnum::Vector2d{-s, c} * (angularSpeed * radius);
    outPos = parentPos + Magnum::Vector2d{c, s} * radius;
    outVel = parentVel + outLocalVel;
}

} // namespace Gravitaris
