// Headless determinism harness (ADR 0001's "cheap test that keeps this
// honest"): links game/ only, never cgame/GL/Audio -- if this target fails
// to link, something pulled a rendering/window/audio dependency into the
// sim, violating ADR 0001 constraint 1.
//
// Runs a short scripted fight twice from a fresh Game/filesystem each time
// and compares Game::ComputeStateChecksum() at the end. A mismatch means the
// sim depends on something outside (state, commands, dt) -- wall-clock,
// unseeded RNG, iteration-order-dependent hashing, etc.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <thread>

#include <chipmunk/chipmunk.h>

#include <gravitaris/game/fs/filesystem-physfs.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/landing-state.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/id.hpp>
#include <gravitaris/game/net/byte-stream.hpp>
#include <gravitaris/game/net/snapshot.hpp>
#include <gravitaris/game/net/loopback-transport.hpp>
#include <gravitaris/game/net/net-server.hpp>
#include <gravitaris/game/net/net-client.hpp>
#include <gravitaris/game/net/client-prediction.hpp>
#include <gravitaris/game/net/webrtc-server-transport.hpp>
#include <gravitaris/game/net/webrtc-transport.hpp>
#include <gravitaris/cgame/net/snapshot-interpolator.hpp>
#include <gravitaris/gravitaris.hpp>

using namespace Gravitaris;

// Normally defined by the client app (src/client/gravitaris.cpp); this
// target has no client, so it owns the definition. Guards id.cpp's hashed
// -string mutex, which must stay unlocked during static initialization --
// see its declaration in gravitaris.hpp.
namespace Gravitaris {
bool HasEnteredMain = false;
}

namespace {

constexpr int TICKS = 1800; // 30s at the fixed 60Hz tick

void Require(bool condition, const char* what)
{
    if (condition) return;
    std::fprintf(stderr, "sim-test: FAILED: %s\n", what);
    std::exit(1);
}

// docs/networking-plan.md 2.1: the wire primitives must roundtrip exactly
// (quantized floats within their step size).
void TestByteStream()
{
    ByteWriter w;
    w.WriteU8(0xAB);
    w.WriteU16(0xBEEF);
    w.WriteU32(0xDEADBEEFu);
    w.WriteU64(0x0123456789ABCDEFull);
    w.WriteF32(-1234.5678f);
    w.WriteQuantizedFloat(0.33f, -1.f, 1.f, 16);

    ByteReader r(w.Data(), w.Size());
    Require(r.ReadU8() == 0xAB, "u8 roundtrip");
    Require(r.ReadU16() == 0xBEEF, "u16 roundtrip");
    Require(r.ReadU32() == 0xDEADBEEFu, "u32 roundtrip");
    Require(r.ReadU64() == 0x0123456789ABCDEFull, "u64 roundtrip");
    Require(r.ReadF32() == -1234.5678f, "f32 roundtrip");
    const float q = r.ReadQuantizedFloat(-1.f, 1.f, 16);
    Require(std::fabs(q - 0.33f) < 2.f / 65535.f, "quantized f32 within one step");
    Require(r.Ok() && r.Remaining() == 0, "reader consumed exactly what was written");

    // Truncated buffer must latch !Ok(), not crash or return garbage as valid.
    ByteReader truncated(w.Data(), 3);
    (void)truncated.ReadU32();
    Require(!truncated.Ok(), "overrun latches !Ok()");
}

// docs/networking-plan.md Phase 4: SnapshotInterpolator's math, exercised
// directly against hand-built snapshot history -- no transport/Game needed,
// this is pure SnapshotData-in/SnapshotData-out logic.
void TestSnapshotInterpolation()
{
    constexpr float TICK_RATE = 60.f;

    EntityState remote{};
    remote.netId = 1;
    remote.type = NetEntityType::Ship;

    EntityState own{};
    own.netId = 2;
    own.type = NetEntityType::Ship;

    // Straddled lerp: remote entity moves (0,0)->(100,0) and rotates
    // 170deg -> -170deg (the short way, through 180, a 20deg delta -- not
    // the naive 340deg the long way around) between tick 10 and tick 20;
    // own entity (exempt) is present in both but should never appear in the
    // output (Phase 5: rendered via ClientPrediction instead).
    SnapshotData older;
    older.tick = 10;
    remote.pos = {0.f, 0.f};
    remote.rot = 170.f * (3.14159265f / 180.f);
    own.pos = {5.f, 5.f};
    older.entities = {remote, own};

    SnapshotData newer;
    newer.tick = 20;
    remote.pos = {100.f, 0.f};
    remote.rot = -170.f * (3.14159265f / 180.f);
    own.pos = {50.f, 50.f};
    newer.entities = {remote, own};

    std::deque<SnapshotData> history{older, newer};

    {
        const std::optional<SnapshotData> mid = SnapshotInterpolator::Compute(
                history, 15, /*exemptNetId*/ 2, TICK_RATE, SnapshotInterpolator::Params{});
        Require(mid.has_value(), "interp: straddled render tick produces a result");
        const auto find = [&](std::uint32_t netId) -> const EntityState* {
            for (const EntityState& e : mid->entities) {
                if (e.netId == netId) return &e;
            }
            return nullptr;
        };
        const EntityState* remoteMid = find(1);
        Require(remoteMid != nullptr, "interp: remote entity present at the straddled tick");
        Require(std::fabs(remoteMid->pos.x() - 50.f) < 0.01f, "interp: position lerped to the halfway point");
        // Shortest-arc: halfway between 170deg and -170deg (through the
        // 180deg wrap) is 180deg (== -180deg), not 0deg (the naive lerp).
        // Wrap the actual-vs-expected difference into (-pi, pi] before
        // comparing, since 180deg and -180deg are the same angle.
        const float expectedRot = 3.14159265f;
        float rotDiff = std::fmod(remoteMid->rot - expectedRot + 3.14159265f, 2.f * 3.14159265f);
        if (rotDiff < 0.f) rotDiff += 2.f * 3.14159265f;
        rotDiff -= 3.14159265f;
        Require(std::fabs(rotDiff) < 0.01f,
                "interp: rotation takes the shortest arc through the wrap, not the long way round");

        Require(find(2) == nullptr, "interp: exempt (own) entity is omitted, not given a snapshot-derived position");
    }
    {
        // Extrapolation past the newest snapshot, capped: remote entity has
        // vel (50,0) at tick 20; rendering at tick 20 + 6 ticks (0.1s) with
        // a 0.05s cap should only extrapolate 0.05s worth (2.5 units), not
        // the full 0.1s (5 units).
        SnapshotData withVel = newer;
        withVel.entities[0].vel = {50.f, 0.f};
        std::deque<SnapshotData> velHistory{older, withVel};

        SnapshotInterpolator::Params params;
        params.extrapolationCapSeconds = 0.05f;
        const std::optional<SnapshotData> extrap =
                SnapshotInterpolator::Compute(velHistory, 26, /*exemptNetId*/ 0, TICK_RATE, params);
        Require(extrap.has_value(), "interp: extrapolation past the newest snapshot produces a result");
        const EntityState& remoteExtrap = extrap->entities[0];
        Require(std::fabs(remoteExtrap.pos.x() - 102.5f) < 0.01f, "interp: extrapolation is capped, not unbounded");
    }
    {
        // Planets: always the newest known state, never lerped/delayed like
        // everything else (see Compute's own doc comment) -- render at a
        // straddled tick between two snapshots where a planet moved, and the
        // output position must be the *newer* snapshot's raw position
        // (matching what ClientPrediction::SyncPlanetProxies collides
        // against), not the halfway lerped point a Ship-typed entity would get.
        EntityState planetOlder{};
        planetOlder.netId = 5;
        planetOlder.type = NetEntityType::Planet;
        planetOlder.pos = {0.f, 0.f};

        EntityState planetNewer = planetOlder;
        planetNewer.pos = {100.f, 0.f};

        SnapshotData planetA;
        planetA.tick = 200;
        planetA.entities = {planetOlder};
        SnapshotData planetB;
        planetB.tick = 210;
        planetB.entities = {planetNewer};
        std::deque<SnapshotData> planetHistory{planetA, planetB};

        const std::optional<SnapshotData> mid = SnapshotInterpolator::Compute(
                planetHistory, 205, /*exemptNetId*/ 0, TICK_RATE, SnapshotInterpolator::Params{});
        Require(mid.has_value(), "interp: planet straddled render tick produces a result");
        Require(mid->entities.size() == 1 && mid->entities[0].netId == 5,
                "interp: planet entity present at the straddled tick");
        Require(std::fabs(mid->entities[0].pos.x() - 100.f) < 0.01f,
                "interp: planet uses the newest known state, not the lerped halfway point");
    }
    {
        // Presence follows the newer straddling snapshot: an entity
        // destroyed between two snapshots must not appear at a render tick
        // between them; one freshly spawned must appear at its exact state.
        EntityState doomed{};
        doomed.netId = 3;
        doomed.pos = {1.f, 1.f};
        EntityState spawned{};
        spawned.netId = 4;
        spawned.pos = {2.f, 2.f};

        SnapshotData a;
        a.tick = 100;
        a.entities = {doomed};
        SnapshotData b;
        b.tick = 110;
        b.entities = {spawned};
        std::deque<SnapshotData> lifecycleHistory{a, b};

        const std::optional<SnapshotData> mid = SnapshotInterpolator::Compute(
                lifecycleHistory, 105, /*exemptNetId*/ 0, TICK_RATE, SnapshotInterpolator::Params{});
        Require(mid.has_value(), "interp: lifecycle straddled tick produces a result");
        bool hasDoomed = false, hasSpawned = false;
        for (const EntityState& e : mid->entities) {
            if (e.netId == 3) hasDoomed = true;
            if (e.netId == 4) hasSpawned = true;
        }
        Require(!hasDoomed, "interp: an entity destroyed between snapshots doesn't linger");
        Require(hasSpawned, "interp: an entity spawned between snapshots appears at its exact state");
    }
}

// docs/networking-plan.md Phase 5: ClientPrediction's Step/Reconcile against
// a real (headless) Game's PhysicsSystem/EntitySpawner, so this exercises
// actual Chipmunk integration, not just hand-computed math.
void TestClientPrediction()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    // No game.Start() -- ClientPrediction only needs PhysicsSystem/
    // EntitySpawner, not a populated scenario.
    Game game(fs);
    GameEventQueue eventQueue;

    ClientPrediction prediction(game.GetRegistry(), game.GetPhysicsSystem(), game.GetEntitySpawner(), eventQueue,
                                game.GetResourceLoader());
    prediction.SpawnOwnShip("models/ships/fighter-1"_id, Vector2d{0., 0.});
    Require(prediction.HasOwnShip(), "prediction: own ship spawns");

    EntityState planet{};
    planet.netId = 99;
    planet.type = NetEntityType::Planet;
    planet.pos = {1000.f, 0.f};
    planet.gravityMass = 5000000.f; // large enough to produce a measurable pull over a few ticks
    planet.gravityMultiplier = 1.f;
    const std::vector<EntityState> planets{planet};

    ControlFlags noInput{};
    EntityState closeMatch{}; // captured at tick 5, below, for the no-correction case
    for (std::uint64_t tick = 0; tick < 10; ++tick) {
        prediction.Step(tick, noInput, planets);
        if (tick == 5) {
            const Transform& t = prediction.GetOwnShip().get<Transform>();
            closeMatch.pos = Magnum::Vector2{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())};
            closeMatch.rot = static_cast<float>(static_cast<double>(t.rot));
            closeMatch.vel = Magnum::Vector2{static_cast<float>(t.vel.x()), static_cast<float>(t.vel.y())};
            closeMatch.angVel = static_cast<float>(t.angVel);
        }
    }
    const Transform& predicted = prediction.GetOwnShip().get<Transform>();
    Require(predicted.pos.x() > 0.0, "prediction: gravity from a replicated planet pulls the predicted ship toward it");

    // A close authoritative match at an already-predicted tick (exactly what
    // was predicted for tick 5, captured above) should not trigger a
    // correction.
    const std::optional<Vector2d> noCorrection = prediction.Reconcile(5, closeMatch, planets);
    Require(!noCorrection.has_value(), "prediction: an authoritative state matching the prediction triggers no correction");

    // A divergent authoritative state should snap + replay.
    EntityState divergent{};
    divergent.pos = {500.f, 500.f}; // far from wherever tick 5 was actually predicted
    divergent.rot = 0.f;
    divergent.vel = {0.f, 0.f};
    divergent.angVel = 0.f;
    const std::optional<Vector2d> correction = prediction.Reconcile(5, divergent, planets);
    Require(correction.has_value(), "prediction: a large divergence triggers a correction");

    const Transform& corrected = prediction.GetOwnShip().get<Transform>();
    // Ticks 6-9 replay from (500,500) under the same gravity/input as
    // before, so the result won't be exactly (500,500) but should stay
    // close to it (a handful of PHYSICS_DELTA ticks of drift, not a jump
    // back toward the old, pre-correction predicted path).
    Require((corrected.pos - Vector2d{500., 500.}).length() < 20.0,
            "prediction: after reconciliation the ship is near the authoritative correction, replayed forward");

    // Re-querying the same tick again finds nothing -- it was consumed by
    // the replay above (history now only holds ticks after it).
    const std::optional<Vector2d> stale = prediction.Reconcile(5, divergent, planets);
    Require(!stale.has_value(), "prediction: re-reconciling an already-replayed tick finds nothing");

    // Phase 6: local fire feedback (same ClientPrediction/Game, continuing
    // from tick 10). No cosmetic bullet entity is spawned (removed
    // 2026-07-19 -- see ClientPrediction::Step's own doc comment: one used
    // to be, but firing at the ship's current local position/rotation while
    // the real bullet fires INPUT_LEAD_TICKS later at wherever the ship has
    // since moved routinely showed as two clearly separate, non-aligned
    // bullets). Only the instant BulletFired event (driving the fire sound)
    // is checked here now -- cooldown cadence is observable via how many
    // times that event's seq actually advances.
    auto countBulletEntities = [&]() {
        std::size_t count = 0;
        game.GetRegistry().each([&](flecs::entity, Bullet&) { ++count; });
        return count;
    };

    ControlFlags firing{};
    firing.firePrimary = true;
    prediction.Step(10, firing, planets);
    Require(eventQueue.LatestSeq() == 1, "prediction: firing emits a local BulletFired event");
    Require(countBulletEntities() == 0,
            "prediction: no cosmetic bullet entity is spawned, only the sound-driving event");

    // Cooldown gates the cadence: holding the trigger for FIRE_COOLDOWN_TICKS
    // - 1 more ticks must not emit another event yet.
    for (std::uint64_t tick = 11; tick < 10 + ShipControlsSystem::FIRE_COOLDOWN_TICKS; ++tick) {
        prediction.Step(tick, firing, planets);
    }
    Require(eventQueue.LatestSeq() == 1, "prediction: fire cooldown gates cadence, no event mid-cooldown");

    prediction.Step(10 + ShipControlsSystem::FIRE_COOLDOWN_TICKS, firing, planets);
    Require(eventQueue.LatestSeq() == 2, "prediction: cooldown expiring lets the next held shot fire");

    // Phase 7: planet collision proxies have real collision geometry, not
    // just gravitational pull -- place a real planet body (not the earlier
    // placeholder-modelId one, which has no shape) exactly at the ship's own
    // current position, with zero gravitational mass to isolate the effect,
    // so any resulting displacement can only be Chipmunk's own contact
    // resolution against the proxy's shape pushing the ship back out.
    const Magnum::Vector2d beforeCollision = prediction.GetOwnShip().get<Transform>().pos;
    EntityState collidingPlanet{};
    collidingPlanet.netId = 100;
    collidingPlanet.type = NetEntityType::Planet;
    collidingPlanet.modelId = "models/planets/simple"_id;
    collidingPlanet.pos = Magnum::Vector2{static_cast<float>(beforeCollision.x()),
                                         static_cast<float>(beforeCollision.y())};
    collidingPlanet.gravityMass = 0.f;
    const std::vector<EntityState> collidingPlanets{collidingPlanet};

    const std::uint64_t collideTick = 10 + ShipControlsSystem::FIRE_COOLDOWN_TICKS + 1;
    prediction.Step(collideTick, ControlFlags{}, collidingPlanets);
    const Magnum::Vector2d afterCollision = prediction.GetOwnShip().get<Transform>().pos;
    Require((afterCollision - beforeCollision).length() > 0.1,
            "prediction: planet collision proxy has real shape, pushes the ship out of a deep overlap");

    // Bug fix found via real multiplayer playtesting (2026-07-19):
    // Reconcile() must return where prediction currently says the ship is
    // ("now"), not the historical position at the reconciled tick -- using
    // the latter conflated real correction error with pure travel distance
    // covered since then, producing a "teleport backward, then re-catch-up
    // forward" visual artifact on every correction (worse the more the ship
    // had moved between the reconciled tick and now). Thrust in a straight
    // line, no gravity, for many ticks so "now" is clearly far from an
    // earlier reconciled tick; reconcile against a target only *just* past
    // epsilon from what was actually predicted at that old tick (a real but
    // tiny error, not a large intentional jump, and otherwise matching the
    // real predicted rot/vel/angVel so the replay continues realistically)
    // -- the returned position must be close to where the ship is right
    // now, not close to the old tick's position (which, after many ticks of
    // sustained thrust, is far away and would fail this check under the old
    // buggy behavior).
    const std::vector<EntityState> noPlanets{};
    const std::uint64_t straightStart = collideTick + 1;
    ControlFlags thrustOnly{};
    thrustOnly.thrustForward = true;

    Vector2d posAtReconcileTick{};
    Magnum::Radd rotAtReconcileTick{0.};
    Vector2d velAtReconcileTick{};
    double angVelAtReconcileTick = 0.;
    const std::uint64_t reconcileTick = straightStart + 5;
    for (std::uint64_t tick = straightStart; tick < straightStart + 60; ++tick) {
        prediction.Step(tick, thrustOnly, noPlanets);
        if (tick == reconcileTick) {
            const Transform& rt = prediction.GetOwnShip().get<Transform>();
            posAtReconcileTick = rt.pos;
            rotAtReconcileTick = rt.rot;
            velAtReconcileTick = rt.vel;
            angVelAtReconcileTick = rt.angVel;
        }
    }
    const Vector2d posNow = prediction.GetOwnShip().get<Transform>().pos;
    Require((posNow - posAtReconcileTick).length() > 50.0,
            "prediction: sustained thrust moved the ship well past its position at the reconciled tick (test setup check)");

    EntityState tinyDivergence{};
    tinyDivergence.pos = Magnum::Vector2{static_cast<float>(posAtReconcileTick.x() + 9.0),
                                        static_cast<float>(posAtReconcileTick.y())};
    tinyDivergence.rot = static_cast<float>(static_cast<double>(rotAtReconcileTick));
    tinyDivergence.vel = Magnum::Vector2{static_cast<float>(velAtReconcileTick.x()),
                                        static_cast<float>(velAtReconcileTick.y())};
    tinyDivergence.angVel = static_cast<float>(angVelAtReconcileTick);
    const std::optional<Vector2d> preCorrectionNow = prediction.Reconcile(reconcileTick, tinyDivergence, noPlanets);
    Require(preCorrectionNow.has_value(), "prediction: a just-past-epsilon divergence still triggers a correction");
    Require((*preCorrectionNow - posNow).length() < 5.0,
            "prediction: Reconcile's returned position reflects 'now', not the far-away reconciled tick");

    fs.Shutdown();
}

// docs/gravity-well-mode-plan.md Phase 1: safe-landing detection + claiming.
// A ship settling gently, upright, on a planet becomes landed and claims it
// after ConquestSystem::CLAIM_TICKS; a fast crash damages and does NOT claim.
void TestLandingAndClaiming()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    Game game(fs); // no Start() -- hand-built minimal scene

    EntitySpawner& spawner = game.GetEntitySpawner();

    // Near-zero center mass: the Orbit component marks the planet claimable
    // (suns have none) while its derived orbital speed stays ~0, so the
    // "surface" barely moves under the ship.
    flecs::entity planet = spawner.SpawnOrbitingPlanet("models/planets/simple"_id,
                                                       Vector2d{0., 0.}, 1e-9, 800., 1.0, 0.0);
    const float planetRadius = planet.get<Planet>().radius
            * static_cast<float>(planet.get<Transform>().scale.x());
    Require(planetRadius > 0.f, "landing: planet body has a radius");
    Require(planet.get<Team>().id == TeamId::None, "landing: planet starts unowned");

    // Clear of the surface on the planet's +Y side (a tight spawn overlaps
    // the fighter's own shape into the planet -- Chipmunk resolves that as a
    // huge impulse that kills the ship instantly), rotated so the legs
    // (local +Y) point down at the center (rot = pi), descending well below
    // the safe-landing speed.
    flecs::entity ship = spawner.SpawnPlayer("models/ships/fighter-1"_id,
                                             Vector2d{800., planetRadius + 40.});
    cpBody* shipBody = game.GetPhysicsSystem().GetBody(ship.get<PhysicsRef>()).cp.body.get();
    cpBodySetAngle(shipBody, CP_PI);
    cpBodySetVelocity(shipBody, cpv(0., -8.));

    bool sawLanded = false;
    for (int tick = 0; tick < 900 && ship.is_alive(); ++tick) {
        game.Update();
        if (ship.get<LandingState>().landed) sawLanded = true;
        if (planet.get<Team>().id != TeamId::None) break;
    }
    Require(ship.is_alive(), "landing: the descending ship survives touchdown");
    Require(sawLanded, "landing: gentle upright contact registers as landed");
    Require(planet.get<Team>().id == TeamId::Blue, "landing: staying landed claims the planet");
    Require(ship.get<LandingState>().lastFriendlySiteNetId == planet.get<NetId>().value,
            "landing: the claimed planet becomes the ship's friendly respawn site");

    bool claimedEventSeen = false;
    game.GetEventQueue().ConsumeSince(0, [&](const GameEvent& event) {
        if (event.type == GameEventType::PlanetClaimed) claimedEventSeen = true;
    });
    Require(claimedEventSeen, "landing: PlanetClaimed event was emitted");

    // Crash case: a second planet and a ship slamming into it upright but
    // far above the safe speed -- damage, no claim at the moment of impact.
    flecs::entity planet2 = spawner.SpawnOrbitingPlanet("models/planets/simple"_id,
                                                        Vector2d{0., -20000.}, 1e-9, 800., 1.0, 0.0);
    flecs::entity crasher = spawner.SpawnPlayer("models/ships/fighter-1"_id,
                                                Vector2d{800., -20000. + planetRadius + 120.});
    cpBody* crasherBody = game.GetPhysicsSystem().GetBody(crasher.get<PhysicsRef>()).cp.body.get();
    cpBodySetAngle(crasherBody, CP_PI);
    cpBodySetVelocity(crasherBody, cpv(0., -150.));

    const float hpBefore = crasher.get<Damageable>().hp;
    bool damaged = false;
    for (int tick = 0; tick < 120 && !damaged && crasher.is_alive(); ++tick) {
        game.Update();
        damaged = crasher.is_alive() ? crasher.get<Damageable>().hp < hpBefore
                                     : true; // died outright: definitely damaged
    }
    Require(damaged, "landing: crashing into a planet at speed damages the ship");
    Require(planet2.get<Team>().id == TeamId::None, "landing: a crash does not claim the planet");

    fs.Shutdown();
}

// docs/networking-plan.md 2.3: gather -> serialize -> parse -> re-serialize
// must be byte-identical (proves the reader reconstructs exactly what the
// writer meant, field for field, with no drift or truncation).
void TestSnapshotRoundtrip(Game& game)
{
    SnapshotData original;
    GatherSnapshot(game.GetRegistry(), game.GetEventQueue(), game.GetStep(), 0, original);
    Require(!original.entities.empty(), "snapshot gathered entities");
    for (std::size_t i = 1; i < original.entities.size(); ++i) {
        Require(original.entities[i - 1].netId < original.entities[i].netId,
                "snapshot entities strictly NetId-ascending");
    }

    ByteWriter first;
    SerializeSnapshot(original, first);

    ByteReader reader(first.Data(), first.Size());
    SnapshotData parsed;
    Require(ReadSnapshot(reader, parsed), "snapshot parses");
    Require(reader.Remaining() == 0, "snapshot parse consumed the whole buffer");
    Require(parsed.entities.size() == original.entities.size(), "entity count survives");
    Require(parsed.events.size() == original.events.size(), "event count survives");

    ByteWriter second;
    SerializeSnapshot(parsed, second);
    Require(first.Size() == second.Size()
                    && std::memcmp(first.Data(), second.Data(), first.Size()) == 0,
            "re-serialized snapshot is byte-identical");
}

// docs/networking-plan.md 3.2-3.4: a NetServer/NetClient pair talking over a
// LoopbackTransport (no sockets -- proves the protocol/spawn/broadcast wiring
// itself, independent of whatever real transport Phase 3.1 eventually picks).
// Runs entirely inside RunSimulation()'s own Game, so it shares that Game's
// determinism gate rather than needing a second one.
void TestNetRoundtrip(Game& game)
{
    auto [serverTransport, clientTransport] = LoopbackTransport::CreatePair();
    NetServer server(game.GetRegistry(), game.GetEntitySpawner(), game.GetEventQueue(), *serverTransport);
    NetClient client(*clientTransport, "sim-test-client");

    // A few ticks to land the handshake (Connected -> ClientHello ->
    // ServerWelcome), then hold thrust for a while so the round-tripped
    // snapshot shows real motion, not just a spawn position.
    for (int i = 0; i < 5; ++i) {
        server.IngestInput(game.GetStep());
        game.Update();
        server.BroadcastSnapshot(game.GetStep());
        client.Update();
    }
    Require(client.IsWelcomed(), "net: client welcomed after handshake");
    Require(client.GetYourShipNetId() != 0, "net: client got a real ship NetId");
    Require(server.PeerCount() == 1, "net: server sees exactly one peer");

    const std::uint32_t shipNetId = client.GetYourShipNetId();
    const flecs::entity shipEntity = game.GetEntitySpawner().EntityForNetId(shipNetId);
    Require(shipEntity.is_alive(), "net: server-side entity for the welcomed NetId exists");

    ControlFlags thrust{};
    thrust.thrustForward = true;
    for (int i = 0; i < 30; ++i) {
        server.IngestInput(game.GetStep());
        client.SendInput(client.EstimateCurrentServerTick() + NetClient::INPUT_LEAD_TICKS, thrust);
        game.Update();
        server.BroadcastSnapshot(game.GetStep());
        client.Update();
    }

    Require(client.GetLatestSnapshot().has_value(), "net: client received at least one snapshot");
    const SnapshotData& snapshot = *client.GetLatestSnapshot();

    const auto it = std::find_if(snapshot.entities.begin(), snapshot.entities.end(),
                                 [&](const EntityState& e) { return e.netId == shipNetId; });
    Require(it != snapshot.entities.end(), "net: latest snapshot contains the client's own ship");

    const Transform& serverTransform = shipEntity.get<Transform>();
    const float serverSpeed = static_cast<float>(serverTransform.vel.length());
    Require(serverSpeed > 1.f, "net: sustained thrust actually moved the server-side ship");

    // Cross-check the replicated state against the server's own truth: f32
    // wire precision should track a double to well under 1 world unit here.
    const Magnum::Vector2 serverPos{static_cast<float>(serverTransform.pos.x()),
                                    static_cast<float>(serverTransform.pos.y())};
    Require((it->pos - serverPos).length() < 0.5f, "net: replicated position matches server truth");

    // Dead-man switch (NetServer::INPUT_TIMEOUT_TICKS): stop sending input
    // entirely -- as if the client's tab got throttled and its input ticks
    // went permanently stale -- and the server must zero the ship's controls
    // rather than let repeat-last-command keep the last held thrust applied
    // forever. The last real command above held thrustForward, so without
    // the timeout Controls::actionFlags.thrustForward would stay true
    // indefinitely.
    for (int i = 0; i < 30; ++i) { // 30 ticks of silence > INPUT_TIMEOUT_TICKS (15)
        server.IngestInput(game.GetStep());
        game.Update();
        server.BroadcastSnapshot(game.GetStep());
        client.Update();
    }
    const Controls& controls = shipEntity.get<Controls>();
    Require(!controls.actionFlags.thrustForward,
            "net: input dead-man timeout zeroed a silent peer's held thrust");
}

// docs/networking-plan.md's known-gap fix: a peer whose ship dies must not
// become a permanent ghost. Own NetServer/NetClient pair (a second peer in
// `game`, independent of TestNetRoundtrip's) so killing this ship can't
// affect that test's assertions.
void TestPeerRespawn(Game& game)
{
    auto [serverTransport, clientTransport] = LoopbackTransport::CreatePair();
    NetServer server(game.GetRegistry(), game.GetEntitySpawner(), game.GetEventQueue(), *serverTransport);
    NetClient client(*clientTransport, "sim-test-respawn-client");

    for (int i = 0; i < 5; ++i) {
        server.IngestInput(game.GetStep());
        game.Update();
        server.BroadcastSnapshot(game.GetStep());
        client.Update();
    }
    Require(client.IsWelcomed(), "respawn: client welcomed");

    const std::uint32_t firstNetId = client.GetYourShipNetId();
    const flecs::entity firstShip = game.GetEntitySpawner().EntityForNetId(firstNetId);
    Require(firstShip.is_alive(), "respawn: first ship exists");

    // Kill it outright -- DeathSystem destructs any entity at hp <= 0 on the
    // next Update().
    firstShip.get_mut<Damageable>().hp = 0.f;
    for (int i = 0; i < 10; ++i) {
        server.IngestInput(game.GetStep());
        game.Update();
        server.BroadcastSnapshot(game.GetStep());
        client.Update();
    }
    Require(!firstShip.is_alive(), "respawn: the killed ship is actually gone (test setup check)");

    // Ticks up to (but not past) NetServer::RESPAWN_DELAY_TICKS: still a
    // ghost -- input for the dead ship must stay refused, not silently
    // queued into nothing, and the client must not have been re-welcomed
    // yet.
    ControlFlags thrust{};
    thrust.thrustForward = true;
    for (int i = 0; i < 60; ++i) {
        server.IngestInput(game.GetStep());
        client.SendInput(client.EstimateCurrentServerTick() + NetClient::INPUT_LEAD_TICKS, thrust);
        game.Update();
        server.BroadcastSnapshot(game.GetStep());
        client.Update();
    }
    Require(client.GetYourShipNetId() == firstNetId,
            "respawn: not yet re-welcomed mid-timer (test setup check)");

    // Cross the respawn delay.
    for (int i = 0; i < 40; ++i) {
        server.IngestInput(game.GetStep());
        client.SendInput(client.EstimateCurrentServerTick() + NetClient::INPUT_LEAD_TICKS, thrust);
        game.Update();
        server.BroadcastSnapshot(game.GetStep());
        client.Update();
    }

    const std::uint32_t secondNetId = client.GetYourShipNetId();
    Require(secondNetId != firstNetId, "respawn: client was re-welcomed with a new ship NetId");

    const flecs::entity secondShip = game.GetEntitySpawner().EntityForNetId(secondNetId);
    Require(secondShip.is_alive(), "respawn: the new ship exists server-side");
    Require(secondShip != firstShip, "respawn: it's a genuinely new entity, not the old id reused");

    // Held thrust since before the respawn should already be driving the new
    // ship -- confirms input flows again rather than staying refused forever
    // (the actual permanent-ghost bug this fixes).
    for (int i = 0; i < 20; ++i) {
        server.IngestInput(game.GetStep());
        client.SendInput(client.EstimateCurrentServerTick() + NetClient::INPUT_LEAD_TICKS, thrust);
        game.Update();
        server.BroadcastSnapshot(game.GetStep());
        client.Update();
    }
    const Transform& secondTransform = secondShip.get<Transform>();
    Require(secondTransform.vel.length() > 1.0, "respawn: the new ship actually responds to input");
}

// docs/networking-plan.md 3.1b: same NetServer/NetClient wiring as
// TestNetRoundtrip, but over two real WebRtcTransport instances instead of
// LoopbackTransport -- proves the actual DataChannel path (real localhost
// UDP, DTLS, SCTP) end to end, with signaling shuttled directly between the
// two in-process instances instead of through a real signaling server (which
// doesn't exist yet -- see the class comment on WebRtcTransport).
//
// Runs in its own Game, separate from RunSimulation()'s two determinism-
// compared runs: unlike LoopbackTransport, real ICE/DTLS establishment runs
// on libdatachannel's own worker threads and takes a variable amount of wall
// -clock time, so it can't be part of a bit-exact checksum comparison.
void TestWebRtcRoundtrip()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    Game game(fs);
    game.Start();

    WebRtcTransport serverTransport(WebRtcTransport::Role::Answerer);
    WebRtcTransport clientTransport(WebRtcTransport::Role::Offerer);

    clientTransport.SetLocalDescriptionCallback(
            [&](const std::string& sdp, const std::string& type) { serverTransport.SetRemoteDescription(sdp, type); });
    clientTransport.SetLocalCandidateCallback([&](const std::string& candidate, const std::string& mid) {
        serverTransport.AddRemoteCandidate(candidate, mid);
    });
    serverTransport.SetLocalDescriptionCallback(
            [&](const std::string& sdp, const std::string& type) { clientTransport.SetRemoteDescription(sdp, type); });
    serverTransport.SetLocalCandidateCallback([&](const std::string& candidate, const std::string& mid) {
        clientTransport.AddRemoteCandidate(candidate, mid);
    });

    serverTransport.Connect();
    clientTransport.Connect();

    NetServer server(game.GetRegistry(), game.GetEntitySpawner(), game.GetEventQueue(), serverTransport);
    NetClient client(clientTransport, "sim-test-webrtc-client");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!client.IsWelcomed() && std::chrono::steady_clock::now() < deadline) {
        server.IngestInput(game.GetStep());
        game.Update();
        server.BroadcastSnapshot(game.GetStep());
        client.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Require(client.IsWelcomed(), "webrtc: client welcomed after real DataChannel handshake");
    Require(client.GetYourShipNetId() != 0, "webrtc: client got a real ship NetId");
    Require(server.PeerCount() == 1, "webrtc: server sees exactly one peer");

    const std::uint32_t shipNetId = client.GetYourShipNetId();
    const flecs::entity shipEntity = game.GetEntitySpawner().EntityForNetId(shipNetId);
    Require(shipEntity.is_alive(), "webrtc: server-side entity for the welcomed NetId exists");

    ControlFlags thrust{};
    thrust.thrustForward = true;
    for (int i = 0; i < 60; ++i) {
        server.IngestInput(game.GetStep());
        client.SendInput(client.EstimateCurrentServerTick() + NetClient::INPUT_LEAD_TICKS, thrust);
        game.Update();
        server.BroadcastSnapshot(game.GetStep());
        client.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    Require(client.GetLatestSnapshot().has_value(), "webrtc: client received at least one snapshot");
    const Transform& serverTransform = shipEntity.get<Transform>();
    const float serverSpeed = static_cast<float>(serverTransform.vel.length());
    Require(serverSpeed > 1.f, "webrtc: sustained thrust actually moved the server-side ship");

    fs.Shutdown();
}

// docs/networking-plan.md 3.5.1/3.5.2: proves the WebSocket signaling path
// (WebRtcTransport::ConnectSignaling) against the multi-peer server
// transport (WebRtcServerTransport) that gravitaris-server will drive --
// same NetServer/NetClient assertions as TestWebRtcRoundtrip, but this time
// the client never touches the server's PeerConnection/DataChannel
// directly: it only knows a ws:// URL, exactly like a real remote client
// would. Own Game, same reasoning as TestWebRtcRoundtrip for why it's kept
// out of the two-run determinism comparison.
void TestWebRtcSignalingRoundtrip()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    Game game(fs);
    game.Start();

    constexpr std::uint16_t port = 17890;
    WebRtcServerTransport serverTransport(port);
    NetServer server(game.GetRegistry(), game.GetEntitySpawner(), game.GetEventQueue(), serverTransport);

    WebRtcTransport clientTransport(WebRtcTransport::Role::Offerer);
    NetClient client(clientTransport, "sim-test-signaling-client");
    clientTransport.ConnectSignaling("ws://127.0.0.1:" + std::to_string(port));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!client.IsWelcomed() && std::chrono::steady_clock::now() < deadline) {
        server.IngestInput(game.GetStep());
        game.Update();
        server.BroadcastSnapshot(game.GetStep());
        client.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Require(client.IsWelcomed(), "webrtc-signaling: client welcomed via ws:// signaling + WebRtcServerTransport");
    Require(client.GetYourShipNetId() != 0, "webrtc-signaling: client got a real ship NetId");
    Require(server.PeerCount() == 1, "webrtc-signaling: server sees exactly one peer");

    const std::uint32_t shipNetId = client.GetYourShipNetId();
    const flecs::entity shipEntity = game.GetEntitySpawner().EntityForNetId(shipNetId);
    Require(shipEntity.is_alive(), "webrtc-signaling: server-side entity for the welcomed NetId exists");

    ControlFlags thrust{};
    thrust.thrustForward = true;
    for (int i = 0; i < 60; ++i) {
        server.IngestInput(game.GetStep());
        client.SendInput(client.EstimateCurrentServerTick() + NetClient::INPUT_LEAD_TICKS, thrust);
        game.Update();
        server.BroadcastSnapshot(game.GetStep());
        client.Update();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    Require(client.GetLatestSnapshot().has_value(), "webrtc-signaling: client received at least one snapshot");
    const Transform& serverTransform = shipEntity.get<Transform>();
    const float serverSpeed = static_cast<float>(serverTransform.vel.length());
    Require(serverSpeed > 1.f, "webrtc-signaling: sustained thrust actually moved the server-side ship");

    fs.Shutdown();
}

struct RunResult {
    std::uint64_t stateChecksum;
    std::uint64_t eventChecksum; // FNV over the full GameEvent stream
    std::uint32_t eventCount;
};

RunResult RunSimulation()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }

    Game game(fs);
    game.Start(); // spawns the player + a planet

    game.GetEntitySpawner().SpawnAIShip("models/ships/fighter-1"_id, Magnum::Vector2d{200.0, -150.0});
    game.GetEntitySpawner().SpawnAIShip("models/ships/fighter-1"_id, Magnum::Vector2d{-200.0, 150.0});

    // Consume the event stream every tick (before the 256-entry ring can
    // wrap) and fold it into a running FNV-1a: two identical runs must
    // produce the identical stream (docs/networking-plan.md 1.6).
    std::uint64_t eventHash = 1469598103934665603ull;
    constexpr std::uint64_t FNV_PRIME = 1099511628211ull;
    const auto mix = [&](std::uint64_t v) {
        for (int b = 0; b < 8; ++b) {
            eventHash ^= (v >> (b * 8)) & 0xFFull;
            eventHash *= FNV_PRIME;
        }
    };
    std::uint32_t eventCursor = 0;

    for (int i = 0; i < TICKS; ++i) {
        game.Update();
        eventCursor = game.GetEventQueue().ConsumeSince(eventCursor, [&](const GameEvent& event) {
            mix(event.seq);
            mix(event.tick);
            mix(static_cast<std::uint64_t>(event.type));
            mix(event.sourceNetId);
            mix(event.param);
        });
    }

    TestSnapshotRoundtrip(game);
    TestNetRoundtrip(game);
    TestPeerRespawn(game);

    const RunResult result{game.ComputeStateChecksum(), eventHash, game.GetEventQueue().LatestSeq()};
    fs.Shutdown();
    return result;
}

} // namespace

int main()
{
    HasEnteredMain = true;

    TestByteStream();
    TestSnapshotInterpolation();
    TestClientPrediction();
    TestLandingAndClaiming();
    TestWebRtcRoundtrip();
    TestWebRtcSignalingRoundtrip();

    const RunResult a = RunSimulation();
    const RunResult b = RunSimulation();

    std::printf("sim-test: run 1 state = 0x%016llx  events = 0x%016llx (%u emitted)\n",
                static_cast<unsigned long long>(a.stateChecksum),
                static_cast<unsigned long long>(a.eventChecksum), a.eventCount);
    std::printf("sim-test: run 2 state = 0x%016llx  events = 0x%016llx (%u emitted)\n",
                static_cast<unsigned long long>(b.stateChecksum),
                static_cast<unsigned long long>(b.eventChecksum), b.eventCount);

    bool ok = true;
    if (a.stateChecksum != b.stateChecksum) {
        std::fprintf(stderr, "sim-test: STATE MISMATCH -- sim is not deterministic across runs\n");
        ok = false;
    }
    if (a.eventChecksum != b.eventChecksum || a.eventCount != b.eventCount) {
        std::fprintf(stderr, "sim-test: EVENT-STREAM MISMATCH -- emitted events differ across runs\n");
        ok = false;
    }
    if (!ok) return 1;

    std::printf("sim-test: OK, deterministic across %d ticks; snapshot roundtrip OK\n", TICKS);
    return 0;
}
