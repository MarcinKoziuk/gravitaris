#pragma once

#include <cstdint>
#include <vector>

#include <flecs.h>

#include <Magnum/Magnum.h>
#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/event/game-event.hpp>

namespace Gravitaris {

class ByteWriter;
class ByteReader;

enum class NetEntityType : std::uint8_t {
    Ship,
    Bullet,
    Planet,    // any celestial (suns included) -- they replicate identically
    Structure, // a planetary/orbital installation (see the Structure component)
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
    // Only meaningful for NetEntityType::Planet (0 otherwise): GravitySource's
    // fields, replicated so client-side prediction (Phase 5) can compute
    // gravity from known planet positions/masses without running a second,
    // independently-seeded orbit simulation that would drift out of phase
    // with the server's.
    float gravityMass = 0.f;
    float gravityMultiplier = 1.f;
    // Only meaningful for NetEntityType::Planet: true for a body with no
    // Orbit (a sun, sitting still), false for one that does (an orbiting
    // planet) -- camera/minimap's star-vs-planet color/size choice, and
    // whether the orbitX fields below mean anything (a star's are all zero).
    bool isStar = false;
    // Only meaningful when isStar is false: enough of Orbit's state (center,
    // radius, the angle at this snapshot's tick, and the current signed
    // angular speed -- itself derived server-side from centerMass/direction/
    // the live gravity multiplier, so replicating it directly means neither
    // side of the wire needs to agree on those separately) for a client to
    // re-derive this planet's exact position at any tick via the same
    // closed-form circular-orbit math OrbitSystem itself uses (see
    // EvaluateOrbit below) -- instead of only ever knowing its raw position
    // as of whichever snapshot last happened to arrive. That raw-position
    // approach (the v1-v4 wire format) had two real symptoms: the rendered
    // planet's motion inherited raw network arrival jitter frame-to-frame
    // (a planet is exempt from interpolation/extrapolation -- see
    // SnapshotInterpolator::Compute's own comment on why -- so nothing was
    // filtering that out), and ClientPrediction's gravity proxies
    // (SyncPlanetProxies) were always ~INPUT_LEAD_TICKS+RTT behind the tick
    // actually being predicted, a systematic lag in where the client thought
    // the gravity wells were that read as slow drift needing periodic
    // reconciliation even while just coasting in a stable orbit, un-driven
    // by any input.
    Magnum::Vector2 orbitCenter{};
    float orbitRadius = 0.f;
    float orbitTheta = 0.f;
    float orbitAngularSpeed = 0.f;
    // Only meaningful for NetEntityType::Structure.
    StructureType structureType = StructureType::Base;
    float rawMaterials = 0.f;
    float finishedMaterials = 0.f;
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
//
// `suppressBulletsOwnedBy` (0 = suppress nothing) omits bullets fired by that
// ship NetId. A client predicts its own shots locally and renders those (see
// ClientPrediction::Step), so shipping it the authoritative copy too would
// draw the same shot twice, ~14 ticks apart -- the own ship renders at
// roughly serverTick + NetClient::INPUT_LEAD_TICKS while replicated entities
// render at serverTick - the interpolation delay. Filtering per peer here,
// rather than adding an owner field to EntityState and filtering client-side,
// keeps the wire format unchanged: an omitted entity needs no new field.
void GatherSnapshot(flecs::world& world, const GameEventQueue& eventQueue, std::uint64_t tick,
                    std::uint32_t eventsSinceSeq, SnapshotData& out,
                    std::uint32_t suppressBulletsOwnedBy = 0);

void SerializeSnapshot(const SnapshotData& snapshot, ByteWriter& out);

// Gather + Serialize in one call (the common server-side path).
void WriteSnapshot(flecs::world& world, const GameEventQueue& eventQueue, std::uint64_t tick,
                   std::uint32_t eventsSinceSeq, ByteWriter& out);

// False on a truncated/garbage buffer (wrong version byte, counts that don't
// fit the remaining bytes); `out` contents are unspecified then.
bool ReadSnapshot(ByteReader& in, SnapshotData& out);

// Re-derives a replicated orbiting planet's exact position/velocity at
// `atTick`, extrapolating analytically from `planet`'s orbit snapshot
// (captured at `baseTick`, i.e. the SnapshotData::tick it came from) via the
// same closed-form circular-orbit math OrbitSystem uses server-side:
// theta(t) = orbitTheta + orbitAngularSpeed * (t - baseTick) * PHYSICS_DELTA,
// then pos = orbitCenter + (cos theta, sin theta) * orbitRadius. Exact for
// any atTick (past, present, or future) as long as orbitAngularSpeed hasn't
// changed since baseTick, which it doesn't during normal play (see
// EntityState::orbitAngularSpeed's own comment) -- callers re-baseline every
// time a newer snapshot arrives regardless, so a live change would only ever
// be off between one snapshot and the next.
//
// Only meaningful when `planet.isStar` is false; callers must check that
// themselves (a star has no orbit data to evaluate, and isn't moving anyway).
void EvaluateOrbit(const EntityState& planet, std::uint64_t baseTick, std::uint64_t atTick,
                   Magnum::Vector2d& outPos, Magnum::Vector2d& outVel);

} // namespace Gravitaris
