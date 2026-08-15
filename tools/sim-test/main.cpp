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
#include <vector>

#include <ankerl/unordered_dense.h>
#include <chipmunk/chipmunk.h>

#include <gravitaris/game/fs/filesystem-physfs.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/callsign.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/faction-state.hpp>
#include <gravitaris/game/system/ship/ship-controls-system.hpp>
#include <gravitaris/game/system/ship/landing-state-system.hpp>
#include <gravitaris/game/component/input-queue.hpp>
#include <gravitaris/game/component/ship-loadout.hpp>
#include <gravitaris/game/component/landing-state.hpp>
#include <gravitaris/game/component/missile.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/orbit.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/freighter.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/planet-attachment.hpp>
#include <gravitaris/game/component/structure.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/research-access.hpp>
#include <gravitaris/game/component/pilot-account.hpp>
#include <gravitaris/game/event/death-report.hpp>
#include <gravitaris/game/ai/ai-preset-library.hpp>
#include <gravitaris/game/resource/body.hpp>
#include <gravitaris/game/resource/body-query.hpp>
#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/upgrade/upgrade-catalog.hpp>
#include <gravitaris/game/cheat/cheat-console.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/id.hpp>
#include <gravitaris/game/scenario/starting-complex.hpp>
#include <gravitaris/game/system/gwell/faction-system.hpp>
#include <gravitaris/game/system/gwell/research-system.hpp>
#include <gravitaris/game/scenario/structure-layout.hpp>
#include <gravitaris/game/net/byte-stream.hpp>
#include <gravitaris/game/net/snapshot.hpp>
#include <gravitaris/game/net/loopback-transport.hpp>
#include <gravitaris/game/net/net-server.hpp>
#include <gravitaris/game/net/net-client.hpp>
#include <gravitaris/game/net/client-prediction.hpp>
#include <gravitaris/game/net/predicted-tick-clock.hpp>
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

// PredictedTickClock is pure integer math (no NetClient/transport needed) --
// exercised directly against hand-picked targets.
void TestPredictedTickClock()
{
    PredictedTickClock clock;
    clock.Reset(100);
    Require(clock.Current() == 100, "reset sets Current() exactly");

    // Small drift (<= threshold): no resync, ticks stay consecutive.
    for (std::uint64_t i = 0; i < 5; ++i) {
        const auto result = clock.Advance(105); // 5 ticks ahead, at the threshold
        Require(!result.resyncDrift, "drift at the threshold does not resync");
        Require(result.tick == 100 + i, "ticks stay consecutive under threshold drift");
    }
    Require(clock.Current() == 105, "five consecutive advances land on the expected tick");

    // Large *forward* drift resyncs to target exactly: tick numbers only
    // increase, so nothing downstream is disturbed by the jump.
    {
        const auto result = clock.Advance(200);
        Require(result.resyncDrift.has_value() && *result.resyncDrift == 95, "large forward drift resyncs, reports magnitude");
        Require(result.tick == 200, "resync returns the target tick itself");
        Require(!result.skip, "a forward resync still steps");
        Require(clock.Current() == 201, "resync still advances by one after resyncing");
    }

    // Running *ahead* of the target must never move the counter backwards
    // (already-predicted ticks would be re-issued and the server would drop
    // every one as stale) -- the clock gives ticks back by skipping steps.
    {
        PredictedTickClock ahead;
        ahead.Reset(1000);
        // The target advances a tick per call, as wall clock does -- holding
        // it still instead would have the clock fall further behind it every
        // step, which is not the situation being modelled here.
        double target = 995.0; // 5 ticks ahead: within the resync threshold
        const auto first = ahead.Advance(target);
        Require(first.skip, "being a tick or more ahead skips a step");
        Require(ahead.Current() == 1000, "a skipped step does not advance the counter");
        Require(!first.resyncDrift, "a skip is not reported as a resync");

        // ...and not again until the rate limit has elapsed.
        std::uint64_t skips = 0;
        for (std::uint64_t i = 0; i < PredictedTickClock::MIN_TICKS_BETWEEN_SKIPS; ++i) {
            target += 1.0;
            if (ahead.Advance(target).skip) ++skips;
        }
        Require(skips == 0, "skips are rate-limited, not taken every call");
        target += 1.0;
        Require(ahead.Advance(target).skip, "a further skip is allowed once the interval has passed");
    }

    // Sub-tick disagreement is tolerated outright: a fractional target within
    // one tick must never trigger the skip machinery.
    {
        PredictedTickClock frac;
        frac.Reset(500);
        for (std::uint64_t i = 0; i < 200; ++i) {
            const auto r = frac.Advance(static_cast<double>(500 + i) + 0.4);
            Require(!r.skip, "a sub-tick lead/lag never skips");
        }
        Require(frac.Current() == 700, "sub-tick drift leaves the counter free-running");
    }
}

// docs/networking-plan.md Phase 10: the input lead has to cover the *full*
// round trip (the client's own server-tick estimate already lags by one
// one-way trip), plus jitter -- see ComputeInputLeadTicks' own derivation.
void TestInputLeadSizing()
{
    constexpr std::uint32_t TICK_RATE = 60; // 16.67ms per tick

    Require(NetClient::ComputeInputLeadTicks(0.f, 0.f, TICK_RATE) == NetClient::MIN_INPUT_LEAD_TICKS,
            "lead: a zero-latency loopback still keeps the minimum slack");

    // 100ms rtt + 2*10ms jitter + one tick (16.67) = 136.67ms -> 9 ticks, plus
    // the stamping clock's own tolerated lag (PredictedTickClock::
    // RESYNC_THRESHOLD_TICKS) on top.
    Require(NetClient::ComputeInputLeadTicks(100.f, 10.f, TICK_RATE)
                    == 9 + PredictedTickClock::RESYNC_THRESHOLD_TICKS,
            "lead: sized off full rtt plus two jitter deviations plus a tick");

    // 90ms rtt + no jitter + one tick = 106.67ms -> 7 ticks (plus the same).
    Require(NetClient::ComputeInputLeadTicks(90.f, 0.f, TICK_RATE)
                    == 7 + PredictedTickClock::RESYNC_THRESHOLD_TICKS,
            "lead: jitter-free sizing");
    // The whole point of that term: a client sitting at the resync threshold
    // stamps that many ticks below the lead this returns, and must still land
    // at or ahead of the server's current tick.
    Require(NetClient::ComputeInputLeadTicks(0.f, 0.f, TICK_RATE) > PredictedTickClock::RESYNC_THRESHOLD_TICKS,
            "lead: covers the stamping clock's tolerated lag even on a perfect line");
    // Jitter alone moves it: same rtt, calmer line, fewer ticks.
    Require(NetClient::ComputeInputLeadTicks(100.f, 0.f, TICK_RATE) <
                    NetClient::ComputeInputLeadTicks(100.f, 10.f, TICK_RATE),
            "lead: a jitter-free line of the same rtt needs less");

    Require(NetClient::ComputeInputLeadTicks(100.f, 10.f, TICK_RATE) >
                    NetClient::ComputeInputLeadTicks(50.f, 10.f, TICK_RATE),
            "lead: grows with rtt");
    Require(NetClient::ComputeInputLeadTicks(1e6f, 1e6f, TICK_RATE) == NetClient::MAX_INPUT_LEAD_TICKS,
            "lead: clamped at the top rather than running away");

    // The LAN case this phase exists for: a fast link must not sit on the
    // fixed 8-tick default, since every unnecessary tick is skew.
    Require(NetClient::ComputeInputLeadTicks(3.f, 1.f, TICK_RATE) < NetClient::INPUT_LEAD_TICKS,
            "lead: a LAN-quality link asks for less than the unknown-wire default");
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
        // Sub-tick lerp between *consecutive* snapshots -- the real shape of
        // the data (the server snapshots every tick), and the case a
        // whole-tick render clock can't express at all: every integer tick
        // lands exactly on a snapshot, so the lerp fraction is always 0 and
        // remote entities step a full tick of travel at a time.
        SnapshotData first;
        first.tick = 100;
        remote.pos = {0.f, 0.f};
        remote.rot = 0.f;
        first.entities = {remote};

        SnapshotData second;
        second.tick = 101;
        remote.pos = {60.f, 0.f};
        second.entities = {remote};

        const std::deque<SnapshotData> consecutive{first, second};
        const std::optional<SnapshotData> quarter = SnapshotInterpolator::Compute(
                consecutive, 100.25, /*exemptNetId*/ 0, TICK_RATE, SnapshotInterpolator::Params{});
        Require(quarter.has_value(), "interp: fractional render tick produces a result");
        Require(std::fabs(quarter->entities[0].pos.x() - 15.f) < 0.01f,
                "interp: consecutive snapshots lerp at the sub-tick fraction, not snapped to the older one");
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

// Playtesting fix (2026-07-21): planets used to always render at their raw
// latest-snapshot position (see TestSnapshotInterpolation's "planet uses the
// newest known state" case above) and ClientPrediction's gravity proxies
// were positioned the same way -- both meant a planet's apparent/simulated
// position was always somewhat stale (raw network jitter unfiltered into
// its rendered motion; a systematic RTT+lead-tick lag in where the client
// thought gravity wells were, read as drift needing periodic reconciliation
// even while just coasting in a stable orbit). Fix: replicate enough of
// Orbit (center/radius/theta/angularSpeed) for EvaluateOrbit to re-derive
// the exact analytic position at any tick. This proves that math directly
// against hand-computed values, independent of SnapshotInterpolator/
// ClientPrediction's own use of it.
void TestOrbitReplication()
{
    EntityState planet{};
    planet.type = NetEntityType::Planet;
    planet.orbitCenter = {1000.f, 0.f};
    planet.orbitRadius = 200.f;
    planet.orbitTheta = 0.f;
    planet.orbitAngularSpeed = 0.5f; // rad/s

    // One full second later (60 ticks at the standard 60Hz tick rate):
    // theta should have advanced by exactly angularSpeed * 1.0s = 0.5 rad.
    Vector2d pos, vel;
    EvaluateOrbit(planet, /*baseTick*/ 1000, /*atTick*/ 1060, pos, vel);

    const double expectedTheta = 0.5;
    const Vector2d expectedPos = Vector2d{1000., 0.} + Vector2d{std::cos(expectedTheta), std::sin(expectedTheta)} * 200.;
    const Vector2d expectedVel = Vector2d{-std::sin(expectedTheta), std::cos(expectedTheta)} * (0.5 * 200.);

    Require((pos - expectedPos).length() < 0.1,
            "orbit: EvaluateOrbit re-derives the exact analytic position after 1s of orbit");
    Require((vel - expectedVel).length() < 0.1,
            "orbit: EvaluateOrbit's velocity matches the analytic tangential velocity");

    // Evaluating at the *same* tick as the baseline must reproduce it
    // exactly (zero elapsed time) -- the degenerate case every real caller
    // (a snapshot's own tick, or a replay of a not-yet-newer tick) can hit.
    Vector2d pos0, vel0;
    EvaluateOrbit(planet, 1000, 1000, pos0, vel0);
    Require((pos0 - Vector2d{1200., 0.}).length() < 0.01,
            "orbit: EvaluateOrbit at baseTick itself reproduces orbitCenter+orbitRadius exactly");

    // SnapshotInterpolator's own planet-override path: a straddled render
    // tick must use this analytic evaluation (based on the newest known
    // snapshot), not freeze to that snapshot's raw (pre-evaluated) pos --
    // this is the wobble fix, proven by checking the *interpolator's*
    // output matches EvaluateOrbit exactly rather than the raw stored pos.
    EntityState raw = planet;
    raw.pos = {1200.f, 0.f}; // whatever the server happened to store at tick 1000
    raw.netId = 7;

    SnapshotData older;
    older.tick = 990;
    older.entities = {raw};
    SnapshotData newer;
    newer.tick = 1000;
    newer.entities = {raw};
    std::deque<SnapshotData> history{older, newer};

    constexpr float TICK_RATE = 60.f;
    const std::optional<SnapshotData> rendered = SnapshotInterpolator::Compute(
            history, /*renderTick*/ 1060, /*exemptNetId*/ 0, TICK_RATE, SnapshotInterpolator::Params{});
    Require(rendered.has_value(), "orbit: interpolator produces a result for an orbiting planet");
    Require(rendered->entities.size() == 1 && rendered->entities[0].netId == 7,
            "orbit: the orbiting planet is present in the interpolator's output");
    const Magnum::Vector2 renderedPos = rendered->entities[0].pos;
    Require(std::fabs(renderedPos.x() - static_cast<float>(expectedPos.x())) < 0.1f
                    && std::fabs(renderedPos.y() - static_cast<float>(expectedPos.y())) < 0.1f,
            "orbit: interpolator evaluates an orbiting planet analytically at renderTick, not frozen to raw pos");

    // Regression (2026-07-21): a landed ship visibly desynced from the
    // rendered planet surface, worse the higher the interpolation delay
    // setting -- ClientPrediction's gravity/collision proxies are
    // positioned at the tick actually being predicted (ahead of
    // renderTick), but the very first version of this fix evaluated the
    // *rendered* planet at renderTick too, silently reintroducing a tick
    // mismatch between what's simulated and what's drawn. `planetTick` (an
    // explicit, separate parameter) must be used for planets instead of
    // renderTick when the caller supplies one.
    const std::uint64_t divergentPlanetTick = 2000; // far from renderTick (1060) above
    const std::optional<SnapshotData> renderedWithPlanetTick = SnapshotInterpolator::Compute(
            history, /*renderTick*/ 1060, /*exemptNetId*/ 0, TICK_RATE, SnapshotInterpolator::Params{},
            divergentPlanetTick);
    Require(renderedWithPlanetTick.has_value(), "orbit: interpolator produces a result with an explicit planetTick");
    Vector2d expectedAtPlanetTick, unusedVel;
    EvaluateOrbit(raw, /*baseTick*/ 1000, divergentPlanetTick, expectedAtPlanetTick, unusedVel);
    const Magnum::Vector2 renderedAtPlanetTick = renderedWithPlanetTick->entities[0].pos;
    Require(std::fabs(renderedAtPlanetTick.x() - static_cast<float>(expectedAtPlanetTick.x())) < 0.1f
                    && std::fabs(renderedAtPlanetTick.y() - static_cast<float>(expectedAtPlanetTick.y())) < 0.1f,
            "orbit: an explicit planetTick overrides renderTick for planet evaluation (landing-alignment fix)");
    Require((renderedAtPlanetTick - renderedPos).length() > 1.f,
            "orbit: planetTick actually changes the result versus renderTick (the two ticks really do diverge)");

    // Regression (2026-07-21): orbit.theta is a double that accumulates
    // unbounded for as long as the server process runs; replicated as f32,
    // a long-running server's large theta values lose real precision in
    // the wire truncation, and since every new snapshot re-bases
    // EvaluateOrbit's calculation from a freshly (and independently)
    // quantized theta, that precision loss reads as the planet wobbling --
    // worse the longer the server's been up. OrbitSystem must wrap theta
    // into [0, 2*PI) every tick so this can't happen regardless of session
    // length. Forces the component directly to a large multi-thousand
    // -radian value (simulating hours of uptime) rather than looping
    // millions of ticks to get there.
    {
        FilesystemPhysFS fs;
        if (!fs.Init()) {
            std::fprintf(stderr, "sim-test: filesystem init failed\n");
            std::exit(1);
        }
        Game game(fs);
        const flecs::entity orbitingPlanet = game.GetEntitySpawner().SpawnOrbitingPlanet(
                "models/planets/simple"_id, Vector2d{0., 0.}, 5000000., 500., 1.0, 0.0);

        Orbit& orbit = orbitingPlanet.get_mut<Orbit>();
        orbit.theta = 12345.6789; // ~1964 full turns -- far past float32's precise-integer range
        game.Update();

        const double wrappedTheta = orbitingPlanet.get<Orbit>().theta;
        Require(wrappedTheta >= 0.0 && wrappedTheta < 2.0 * 3.14159265358979323846,
                "orbit: OrbitSystem wraps theta into [0, 2*PI) regardless of how large it was before this tick");
        fs.Shutdown();
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
                                game.GetResourceLoader(), game.GetUpgradeCatalog());

    // An unupgraded ship's cadence, which is what this prediction fires at.
    const std::uint32_t fireCooldownTicks =
            game.GetUpgradeCatalog().ResolveStats(UpgradeLevels{}).fireCooldownTicks;
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
        prediction.Step(tick, noInput, planets, tick, /*ownShipNetId=*/0);
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
    const std::optional<Vector2d> noCorrection = prediction.Reconcile(5, closeMatch, planets, /*ownShipNetId=*/0);
    Require(!noCorrection.has_value(), "prediction: an authoritative state matching the prediction triggers no correction");

    // A divergent authoritative state should snap + replay.
    EntityState divergent{};
    divergent.pos = {500.f, 500.f}; // far from wherever tick 5 was actually predicted
    divergent.rot = 0.f;
    divergent.vel = {0.f, 0.f};
    divergent.angVel = 0.f;
    const std::optional<Vector2d> correction = prediction.Reconcile(5, divergent, planets, /*ownShipNetId=*/0);
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
    const std::optional<Vector2d> stale = prediction.Reconcile(5, divergent, planets, /*ownShipNetId=*/0);
    Require(!stale.has_value(), "prediction: re-reconciling an already-replayed tick finds nothing");

    // Phase 6: local fire feedback (same ClientPrediction/Game, continuing
    // from tick 10). Firing spawns the cosmetic bullet this client renders
    // (the server omits a peer's own bullets from its snapshots, so this is
    // the only tracer on screen) plus the instant BulletFired event driving
    // the fire sound.
    auto countBulletEntities = [&]() {
        std::size_t count = 0;
        game.GetRegistry().each([&](flecs::entity, Bullet&) { ++count; });
        return count;
    };

    ControlFlags firing{};
    firing.firePrimary = true;
    prediction.Step(10, firing, planets, 10, /*ownShipNetId=*/0);
    Require(eventQueue.LatestSeq() == 1, "prediction: firing emits a local BulletFired event");
    Require(countBulletEntities() == 1, "prediction: firing spawns exactly one locally-predicted bullet");

    // Cosmetic only: damage stays server-authoritative, and it carries the
    // shooter's NetId so GatherSnapshot can suppress the server's copy.
    bool ownedAndHarmless = false;
    game.GetRegistry().each([&](flecs::entity, Bullet& b) {
        ownedAndHarmless = b.damage == 0.f && b.ownerNetId == prediction.GetOwnShip().get<NetId>().value;
    });
    Require(ownedAndHarmless, "prediction: the predicted bullet is zero-damage and tagged with the shooter's NetId");

    // Each mount paces itself. fighter-1 carries its light guns in one forward
    // mount -- the heavy pair either side are cannon mounts -- so held fire is
    // one shot per cooldown, and never two on a tick.
    const ResourcePtr<const Body> hull =
            game.GetResourceLoader().Load<Body>("models/ships/fighter-1"_id);
    Require(ShipControlsSystem::MountsFor(*hull, ShipControlsSystem::WEAPON_HARDPOINT) == 3,
            "hull: fighter-1 carries three weapon mounts, each armed by the loadout");

    const std::uint64_t holdUntil = 10 + fireCooldownTicks * 4;
    std::uint32_t previousSeq = eventQueue.LatestSeq();
    std::size_t shots = 1; // the one already fired on the trigger's first tick
    bool everDoubled = false;
    for (std::uint64_t tick = 11; tick < holdUntil; ++tick) {
        prediction.Step(tick, firing, planets, tick, /*ownShipNetId=*/0);
        const std::uint32_t seq = eventQueue.LatestSeq();
        everDoubled = everDoubled || seq - previousSeq > 1;
        shots += seq - previousSeq;
        previousSeq = seq;
    }
    Require(!everDoubled, "prediction: a mount never fires twice on one tick");
    Require(shots >= 3, "prediction: held fire keeps the mount at its own cadence");

    // The phase rule itself, where it can be read off exactly: mount 0 fires on
    // the pull, and the rest are spread across one cycle behind it. Checked
    // directly because no hull carries two mounts of one family today, so
    // nothing above would notice if the spread broke.
    {
        std::array<std::uint32_t, MAX_WEAPON_MOUNTS> phases{};
        ShipControlsSystem::SeedMountPhases(phases, 2, 8, 1.f);
        // Seeded one above the delay: the tick that deals these also runs the
        // countdown (see SeedMountPhases).
        Require(phases[0] == 0 && phases[1] == 5,
                "controls: two mounts are dealt half a cycle apart");

        ShipControlsSystem::SeedMountPhases(phases, 2, 8, 0.f);
        Require(phases[0] == 0 && phases[1] == 0,
                "controls: zero stagger fires a family as one volley");
    }

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

    const std::uint64_t collideTick = 10 + fireCooldownTicks + 1;
    prediction.Step(collideTick, ControlFlags{}, collidingPlanets, collideTick, /*ownShipNetId=*/0);
    const Magnum::Vector2d afterCollision = prediction.GetOwnShip().get<Transform>().pos;
    Require((afterCollision - beforeCollision).length() > 0.1,
            "prediction: planet collision proxy has real shape, pushes the ship out of a deep overlap");

    // Phase 7.1: a remote ship also gets a real collision proxy now (fixes
    // pass-through-then-teleport on ship-ship contact during prediction --
    // see ClientPrediction's own class doc comment). Same setup as the
    // planet case just above: place a ship-typed EntityState's proxy exactly
    // at the local ship's current position and confirm Chipmunk's contact
    // resolution pushes it back out.
    const Magnum::Vector2d beforeShipCollision = prediction.GetOwnShip().get<Transform>().pos;
    EntityState collidingShip{};
    collidingShip.netId = 200;
    collidingShip.type = NetEntityType::Ship;
    collidingShip.modelId = "models/ships/fighter-1"_id;
    collidingShip.pos = Magnum::Vector2{static_cast<float>(beforeShipCollision.x()),
                                        static_cast<float>(beforeShipCollision.y())};
    const std::vector<EntityState> collidingShips{collidingShip};

    const std::uint64_t shipCollideTick = collideTick + 1;
    prediction.Step(shipCollideTick, ControlFlags{}, collidingShips, shipCollideTick, /*ownShipNetId=*/0);
    const Magnum::Vector2d afterShipCollision = prediction.GetOwnShip().get<Transform>().pos;
    Require((afterShipCollision - beforeShipCollision).length() > 0.1,
            "prediction: a remote ship collision proxy has real shape, pushes the ship out of a deep overlap");

    // The peer's own ship NetId must be excluded, or a client would collide
    // with a proxy standing in for its own real, already-simulated ship.
    // Force a known, motionless baseline first via Reconcile -- the ship
    // exits the collision above carrying whatever residual velocity
    // Chipmunk's contact resolution left it with, and a "no meaningful
    // displacement" check below only means something from a ship truly at
    // rest. `shipCollideTick` is still the latest captured entry (nothing
    // history-side happened since), so there's nothing to replay -- the
    // supplied authoritative state becomes the ship's exact new state, no
    // approximation. One settle tick with an empty entity list follows:
    // Chipmunk's own solver produces a one-time sub-unit position nudge on
    // whatever tick immediately follows a large direct cpBodySetPosition
    // teleport, regardless of what (if anything) is in the entity list --
    // confirmed independently of this proxy-exclusion check -- so the actual
    // measurement below starts from a position already past that artifact,
    // not conflating it with what this check is for.
    EntityState atRest{};
    atRest.pos = Magnum::Vector2{2000.f, 2000.f}; // far from every proxy above
    prediction.Reconcile(shipCollideTick, atRest, {}, /*ownShipNetId=*/0);
    prediction.Step(shipCollideTick + 1, ControlFlags{}, {}, shipCollideTick + 1, /*ownShipNetId=*/0);

    // Fresh NetId at that same motionless position, tagged as `ownShipNetId`
    // this time: no proxy should be created, so no displacement, unlike the
    // identical-in-every-other-way case just above.
    const Magnum::Vector2d beforeOwnCheck = prediction.GetOwnShip().get<Transform>().pos;
    EntityState ownShipState{};
    ownShipState.netId = 201;
    ownShipState.type = NetEntityType::Ship;
    ownShipState.modelId = "models/ships/fighter-1"_id;
    ownShipState.pos = Magnum::Vector2{static_cast<float>(beforeOwnCheck.x()), static_cast<float>(beforeOwnCheck.y())};
    const std::vector<EntityState> ownShipOnly{ownShipState};

    prediction.Step(shipCollideTick + 2, ControlFlags{}, ownShipOnly, shipCollideTick + 2, /*ownShipNetId=*/201);
    const Magnum::Vector2d afterOwnCheck = prediction.GetOwnShip().get<Transform>().pos;
    Require((afterOwnCheck - beforeOwnCheck).length() < 0.1,
            "prediction: a ship EntityState tagged as ownShipNetId gets no collision proxy");

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
    const std::uint64_t straightStart = shipCollideTick + 3;
    ControlFlags thrustOnly{};
    thrustOnly.thrustForward = true;

    Vector2d posAtReconcileTick{};
    Magnum::Radd rotAtReconcileTick{0.};
    Vector2d velAtReconcileTick{};
    double angVelAtReconcileTick = 0.;
    const std::uint64_t reconcileTick = straightStart + 5;
    for (std::uint64_t tick = straightStart; tick < straightStart + 60; ++tick) {
        prediction.Step(tick, thrustOnly, noPlanets, tick, /*ownShipNetId=*/0);
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
    const std::optional<Vector2d> preCorrectionNow =
            prediction.Reconcile(reconcileTick, tinyDivergence, noPlanets, /*ownShipNetId=*/0);
    Require(preCorrectionNow.has_value(), "prediction: a just-past-epsilon divergence still triggers a correction");
    Require((*preCorrectionNow - posNow).length() < 5.0,
            "prediction: Reconcile's returned position reflects 'now', not the far-away reconciled tick");

    fs.Shutdown();
}

// docs/gravity-well-mode-plan.md Phase 1: safe-landing detection + claiming.
// A ship settling gently, upright, on a planet becomes landed and claims it
// after the configured claim dwell; a fast crash damages and does NOT claim.
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
    // the safe-landing speed. The gap is small on purpose: the ship arrives
    // at whatever the live gravity gives it over that fall, and it has to
    // stay under DamageSystem's UPRIGHT_SAFE_DELTAV for this to be a landing
    // rather than the crash case below.
    flecs::entity ship = spawner.SpawnPlayer("models/ships/fighter-1"_id,
                                             Vector2d{800., planetRadius + 15.});
    cpBody* shipBody = game.GetPhysicsSystem().GetBody(ship.get<PhysicsRef>()).cp.body.get();
    cpBodySetAngle(shipBody, CP_PI);
    cpBodySetVelocity(shipBody, cpv(0., -8.));

    bool sawLanded = false;
    for (int tick = 0; tick < 900 && ship.is_alive(); ++tick) {
        game.Update();
        if (!ship.is_alive()) break; // died on impact -- the Require below reports it
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

    // Per-hull fragility: two identical airframes dropped identically, one of
    // them twice as fragile, take damage in that ratio. Overridden on the
    // component rather than by flying a different model, so nothing but the
    // multiplier differs between the two.
    //
    // The probes are given hp far past anything the drop can cost them rather
    // than the drop being tuned gentle enough not to kill them: a clamp at
    // zero hp would hide the ratio, and calibrating the drop speed against it
    // made this a tripwire for the gravity default instead of a test of
    // fragility -- the fall gains speed under gravity, so retuning that
    // constant either killed the fragile probe or stopped hurting either.
    const auto dropAndMeasure = [&](double planetY, float fragility) {
        spawner.SpawnOrbitingPlanet("models/planets/simple"_id, Vector2d{0., planetY}, 1e-9, 800., 1.0, 0.0);
        flecs::entity faller = spawner.SpawnPlayer("models/ships/fighter-1"_id,
                                                   Vector2d{800., planetY + planetRadius + 60.});
        Damageable& hull = faller.get_mut<Damageable>();
        hull.landingFragility = fragility;
        hull.maxHp = 1e6f;
        hull.hp = 1e6f;
        cpBody* body = game.GetPhysicsSystem().GetBody(faller.get<PhysicsRef>()).cp.body.get();
        cpBodySetAngle(body, CP_PI);
        cpBodySetVelocity(body, cpv(0., -110.));

        const float before = faller.get<Damageable>().hp;
        for (int tick = 0; tick < 120 && faller.is_alive(); ++tick) {
            game.Update();
            if (faller.is_alive() && faller.get<Damageable>().hp < before) break;
        }
        Require(faller.is_alive(), "landing: the fragility probe survives its drop");
        return before - faller.get<Damageable>().hp;
    };

    const float toughDamage = dropAndMeasure(-40000., 1.f);
    const float fragileDamage = dropAndMeasure(-60000., 2.f);
    Require(toughDamage > 0.f && std::fabs(fragileDamage - 2.f * toughDamage) < 0.01f * toughDamage,
            "landing: a hull's [landing] fragility scales the damage a hard set-down costs it");

    fs.Shutdown();
}

// A parked hull chatters -- a dropped contact, a velocity excursion -- and
// judged raw every such tick is a takeoff and a fresh landing. That restarts
// the claim counter, and it strobes ResearchAccess::atLab, which the client
// reads as the whole refit board changing state (rebuilt markup, lost hover,
// clicks landing on elements that no longer exist).
void TestLandingChatterGrace()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    Game game(fs);

    EntitySpawner& spawner = game.GetEntitySpawner();
    flecs::entity planet = spawner.SpawnOrbitingPlanet("models/planets/simple"_id,
                                                       Vector2d{0., 0.}, 1e-9, 800., 1.0, 0.0);
    const float planetRadius = planet.get<Planet>().radius
            * static_cast<float>(planet.get<Transform>().scale.x());
    flecs::entity ship = spawner.SpawnPlayer("models/ships/fighter-1"_id,
                                             Vector2d{800., planetRadius + 15.});
    cpBody* shipBody = game.GetPhysicsSystem().GetBody(ship.get<PhysicsRef>()).cp.body.get();
    cpBodySetAngle(shipBody, CP_PI);
    cpBodySetVelocity(shipBody, cpv(0., -8.));

    for (int tick = 0; tick < 900 && ship.is_alive(); ++tick) {
        game.Update();
        if (ship.is_alive() && ship.get<LandingState>().landed) break;
    }
    Require(ship.is_alive() && ship.get<LandingState>().landed,
            "chatter: the ship is standing on the planet (setup check)");

    const std::uint32_t netIdBefore = ship.get<LandingState>().landedOnNetId;
    std::uint32_t ticksBefore = ship.get<LandingState>().landedTicks;

    // Chatter, forced through the speed gate rather than by waiting for a
    // contact to drop on its own: a handful of ticks over SAFE_LANDING_SPEED,
    // well inside the grace.
    const int chatter = LandingStateSystem::LANDING_GRACE_TICKS - 5;
    for (int tick = 0; tick < chatter && ship.is_alive(); ++tick) {
        cpBodySetVelocity(shipBody, cpv(0., LandingStateSystem::SAFE_LANDING_SPEED + 5.));
        game.Update();
        const LandingState& state = ship.get<LandingState>();
        Require(state.landed, "chatter: a momentary loss of contact is not a takeoff");
        Require(state.landedOnNetId == netIdBefore, "chatter: the site is held across the gap");
        Require(state.landedTicks > ticksBefore,
                "chatter: the claim counter keeps running instead of restarting");
        ticksBefore = state.landedTicks;
    }

    // Held past the grace it is a departure, and the counter does restart.
    for (int tick = 0; tick <= LandingStateSystem::LANDING_GRACE_TICKS && ship.is_alive(); ++tick) {
        cpBodySetVelocity(shipBody, cpv(0., LandingStateSystem::SAFE_LANDING_SPEED + 5.));
        game.Update();
    }
    Require(ship.is_alive(), "chatter: the probe survives its hop");
    Require(!ship.get<LandingState>().landed, "chatter: a hull that stays off the surface has left it");
    Require(ship.get<LandingState>().landedTicks == 0, "chatter: leaving restarts the claim counter");

    fs.Shutdown();
}

// docs/networking-plan.md Phase 9: ship-against-ship contact resolves as
// gameplay, not physics -- a slow pair passes through each other, a fast pair
// destroys. Ship-against-*planet* is deliberately untouched, which
// TestLandingAndClaiming above still covers (landing, and the crash damage
// that shares the wildcard handler this phase routes ships around).
void TestShipCollision()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }

    struct Outcome {
        bool aAlive = false;
        bool bAlive = false;
        float hpA = 0.f;
        float hpB = 0.f;
        bool crossed = false; // the two centres passed through each other
        double mass = 0.;
        std::vector<DeathReport> deaths; // what the kill feed was told, in order
    };

    // One head-on pass. Thresholds are set from the pair's *actual* momentum
    // rather than absolute numbers, so no case here depends on what the
    // fighter asset happens to weigh.
    const auto headOn = [&fs](double speed, TeamId teamA, TeamId teamB, float hpA, float hpB,
                              double bothDieFactor, double separationAccel) {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();

        flecs::entity a = spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{-60., 0.}, teamA);
        flecs::entity b = spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{60., 0.}, teamB);
        a.get_mut<Damageable>().hp = hpA;
        b.get_mut<Damageable>().hp = hpB;

        PhysicsSystem& physics = game.GetPhysicsSystem();
        cpBody* bodyA = physics.GetBody(a.get<PhysicsRef>()).cp.body.get();
        cpBody* bodyB = physics.GetBody(b.get<PhysicsRef>()).cp.body.get();
        cpBodySetVelocity(bodyA, cpv(speed, 0.));
        cpBodySetVelocity(bodyB, cpv(-speed, 0.));

        const double momentum = cpBodyGetMass(bodyA) * 2.0 * speed; // mass x closing speed
        PhysicsSystem::ShipContactParams& params = physics.GetShipContactParams();
        params.separationAccel = separationAccel;
        params.bothDieMomentum = momentum * bothDieFactor;
        // Exactly 20 points on the survivor, whatever the asset weighs.
        params.survivorDamageScale = 20.0 / momentum;

        Outcome out;
        out.mass = cpBodyGetMass(bodyA);
        game.OnDeath().connect([&out](const DeathReport& report) { out.deaths.push_back(report); });
        for (int tick = 0; tick < 400; ++tick) {
            game.Update();
            if (!a.is_alive() || !b.is_alive()) break;
            if (a.get<Transform>().pos.x() > b.get<Transform>().pos.x()) out.crossed = true;
        }
        out.aAlive = a.is_alive();
        out.bAlive = b.is_alive();
        if (out.aAlive) out.hpA = a.get<Damageable>().hp;
        if (out.bAlive) out.hpB = b.get<Damageable>().hp;
        return out;
    };

    // Slow enough to be an overlap: closing 120 against the 250 default.
    // A fighter masses 1 and thrusts at 140, so it gains 140 units/s per
    // second of burn -- "slow" here still means a second of thrust each.
    // The nudge is off so "passed through" is unambiguous; under the old
    // hard contact these two could never have swapped sides.
    // the separating nudge off so "passed through" is unambiguous -- under
    // the old hard contact these two could never have swapped sides.
    {
        const Outcome slow = headOn(60., TeamId::Blue, TeamId::Red, 100.f, 100.f, 1.5, 0.);
        Require(slow.aAlive && slow.bAlive, "ram: a slow enemy pair both survive");
        Require(slow.crossed, "ram: a slow enemy pair passes through instead of bouncing");
        Require(slow.hpA == 100.f && slow.hpB == 100.f, "ram: a slow overlap does no damage at all");
    }
    {
        const Outcome slow = headOn(60., TeamId::Blue, TeamId::Blue, 100.f, 100.f, 1.5, 0.);
        Require(slow.aAlive && slow.bAlive && slow.crossed, "ram: a slow friendly pair overlaps too");
        Require(slow.hpA == 100.f && slow.hpB == 100.f, "ram: a slow friendly overlap does no damage");
    }

    // Fast, unequal toughness (mass x *current* hp), below the both-die
    // threshold: the weaker dies, the survivor is damaged but lives.
    {
        const Outcome ram = headOn(200., TeamId::Blue, TeamId::Red, 100.f, 40.f, 1.5, 0.);
        Require(ram.aAlive, "ram: the tougher ship survives");
        Require(!ram.bAlive, "ram: the weaker ship is destroyed");
        // Exactly one ram's worth of damage. The helper scales a *head-on*
        // hit to 20 points; the hulls meet at an angle so the projected
        // closing speed lands it nearer 17. What matters is the band: a
        // ship is several Chipmunk shapes and one pair touching raises 3-4
        // arbiters, so without per-pair dedupe this is 3-4 hits (hp <= 66)
        // and the "survivor" does not survive.
        Require(ram.hpA > 75.f && ram.hpA < 95.f,
                "ram: the survivor is damaged exactly once, not once per overlapping shape pair");

        // The kill feed's side of the same event: one death, blamed on the
        // ship that won the ram rather than on nobody.
        Require(ram.deaths.size() == 1, "death feed: a ram kill reports exactly one death");
        Require(ram.deaths[0].victimTeam == TeamId::Red, "death feed: the victim's side is reported");
        Require(ram.deaths[0].killerTeam == TeamId::Blue, "death feed: the ram is credited to the survivor");
        Require(ram.deaths[0].cause == DamageCause::Ram, "death feed: a ram is reported as a ram");
    }

    // Evenly matched: no weaker party to pick, and choosing by entity id
    // would decide a head-on ram on spawn order. Both die.
    {
        const Outcome ram = headOn(200., TeamId::Blue, TeamId::Red, 100.f, 100.f, 1.5, 0.);
        Require(!ram.aAlive && !ram.bAlive, "ram: an evenly matched pair destroys both");
    }

    // Past the both-die momentum threshold, toughness stops mattering.
    {
        const Outcome ram = headOn(200., TeamId::Blue, TeamId::Red, 100.f, 40.f, 0.5, 0.);
        Require(!ram.aAlive && !ram.bAlive, "ram: a hard enough hit destroys both regardless of toughness");
    }

    // Friendlies never ram-kill, at any speed.
    {
        const Outcome ram = headOn(200., TeamId::Blue, TeamId::Blue, 100.f, 40.f, 0.5, 0.);
        Require(ram.aAlive && ram.bAlive, "ram: friendlies survive a hit that would destroy both enemies");
        Require(ram.hpA == 100.f && ram.hpB == 40.f, "ram: friendlies take no ram damage either");
    }

    // With the nudge on, an overlapping pair is eased apart rather than
    // passing clean through.
    {
        const Outcome nudged = headOn(20., TeamId::Blue, TeamId::Red, 100.f, 100.f, 1.5, 400.);
        Require(nudged.aAlive && nudged.bAlive, "ram: the separating nudge harms nobody");
        Require(!nudged.crossed, "ram: a strong enough separating nudge keeps the pair from swapping sides");
    }

    fs.Shutdown();
}

// docs/gravity-well-mode-plan.md Phase 2: structures. Spawns a full starting
// complex on a real orbiting planet, and checks planetside/orbital
// structures track the planet's own motion over many ticks (rather than
// drifting away in fixed world space) and that a Base's defenses actually
// fire at an enemy ship that wanders into range.
void TestStructures()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    Game game(fs);
    EntitySpawner& spawner = game.GetEntitySpawner();

    // Real, non-negligible mass so the planet actually orbits and moves --
    // the whole point of this test is proving structures follow it.
    flecs::entity planet =
            spawner.SpawnOrbitingPlanet("models/planets/simple"_id, Vector2d{0., 0.}, 5.0e7, 2000., 1.0, 0.0);

    BuildStartingComplex(spawner, planet, TeamId::Blue);

    std::size_t structureCount = 0;
    bool sawEachType = true;
    for (StructureType type : {StructureType::Base, StructureType::Colony, StructureType::Lab,
                               StructureType::CommCenter, StructureType::HighPort}) {
        bool found = false;
        game.GetRegistry().each([&](const Structure& s) { if (s.type == type) found = true; });
        sawEachType = sawEachType && found;
    }
    game.GetRegistry().each([&](const Structure&) { ++structureCount; });
    Require(sawEachType, "structures: all five types were spawned");
    Require(structureCount == 5, "structures: exactly one of each");

    flecs::entity base;
    flecs::entity highPort;
    game.GetRegistry().each([&](flecs::entity e, const Structure& s) {
        if (s.type == StructureType::Base) base = e;
        if (s.type == StructureType::HighPort) highPort = e;
    });
    Require(base.get<Team>().id == TeamId::Blue, "structures: spawned with the requested team");
    Require(base.has<StructureDefense>(), "structures: a Base carries defenses");
    Require(!game.GetEntitySpawner().EntityForNetId(9999999).is_alive(), "structures: sanity -- bogus NetId resolves to nothing");

    const Vector2d baseOffsetAtSpawn = base.get<Transform>().pos - planet.get<Transform>().pos;
    const double highPortRadiusAtSpawn = (highPort.get<Transform>().pos - planet.get<Transform>().pos).length();

    // Run long enough for a 2000-radius, 5e7-mass orbit to move noticeably
    // (a few hundred ticks is already a visible arc at this radius/mass).
    for (int tick = 0; tick < 600; ++tick) {
        game.Update();
    }

    const Vector2d planetPosNow = planet.get<Transform>().pos;
    Require(planetPosNow.length() > 50.0, "structures: the planet itself actually moved (test setup check)");

    const Vector2d baseOffsetNow = base.get<Transform>().pos - planetPosNow;
    Require((baseOffsetNow - baseOffsetAtSpawn).length() < 1.0,
            "structures: a planetside structure keeps its offset as the planet orbits");

    const double highPortRadiusNow = (highPort.get<Transform>().pos - planetPosNow).length();
    Require(std::abs(highPortRadiusNow - highPortRadiusAtSpawn) < 1.0,
            "structures: an orbital structure keeps its orbit radius as the planet orbits");

    // Defense fire: an enemy ship within Base's FIRE_RANGE (400) but well
    // clear of every structure's own collision shape (all within ~260 units
    // of planet center: StructureLayout's 2x-radius orbit plus a High Port's
    // own half-extent) so it doesn't spawn overlapping one and get destroyed
    // by Chipmunk's overlap resolution before ever taking a scripted hit.
    // Matched to the planet's own velocity: this planet is on a real orbit
    // and a ship left at rest in world space is out of range within a few
    // ticks, testing nothing.
    flecs::entity enemy = spawner.SpawnPlayer("models/ships/fighter-1"_id, planetPosNow + Vector2d{350., 0.});
    enemy.set<Team>(Team{TeamId::Red});
    const Vector2d planetVelNow = planet.get<Transform>().vel;
    cpBodySetVelocity(game.GetPhysicsSystem().GetBody(enemy.get<PhysicsRef>()).cp.body.get(),
                      cpv(planetVelNow.x(), planetVelNow.y()));
    const float enemyHpBefore = enemy.get<Damageable>().hp;

    bool enemyDamaged = false;
    for (int tick = 0; tick < 400 && !enemyDamaged && enemy.is_alive(); ++tick) {
        game.Update();
        enemyDamaged = enemy.is_alive() ? enemy.get<Damageable>().hp < enemyHpBefore
                                       : true; // destroyed outright: definitely damaged
    }
    Require(enemyDamaged, "structures: Base defenses hit an enemy ship in range");

    fs.Shutdown();
}

// docs/gravity-well-mode-plan.md Phase 3: freighters + materials economy.
// Seeds a fully developed home complex (BuildStartingComplex) and a second,
// claimed-but-empty planet nearby, then checks: (1) materials actually flow
// on their own (Colony production -> Base conversion) over a short natural
// window; (2) gifting the home Base/High Port funds so dispatch doesn't
// have to wait on that slow ramp, the empty planet grows Base -> Colony ->
// High Port in that order, entirely hands-off, with freighters actually
// consumed on arrival.
void TestFreighterEconomy()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    Game game(fs);
    EntitySpawner& spawner = game.GetEntitySpawner();

    // Near-zero orbit centerMass (same trick TestLandingAndClaiming uses):
    // this is the planet's OWN orbital motion around its (notional) sun,
    // independent of its own GravitySource mass (authored on the Body,
    // pulls ships/structures same as ever) -- keeping it ~stationary here
    // just keeps this test's freighter-transit distance predictable instead
    // of chasing a fast-moving target around a 2000-radius orbit.
    flecs::entity homePlanet =
            spawner.SpawnOrbitingPlanet("models/planets/simple"_id, Vector2d{0., 0.}, 1e-9, 2000., 1.0, 0.0);
    BuildStartingComplex(spawner, homePlanet, TeamId::Blue);

    // Second planet, close by (so freighter transit -- 40 u/s -- doesn't
    // dominate the test's runtime) yet still outside
    // FreighterSystem::ARRIVAL_RADIUS of the home planet, or a freighter
    // would count as arrived the moment it spawned. Pre-claimed (bypassing
    // Phase 1's landing/claiming system, which isn't what this test is
    // about).
    flecs::entity emptyPlanet =
            spawner.SpawnOrbitingPlanet("models/planets/simple"_id, Vector2d{900., 0.}, 1e-9, 2000., 1.0, 0.0);
    emptyPlanet.set<Team>(Team{TeamId::Blue});

    flecs::entity homeColony, homeBase, homeHighPort;
    game.GetRegistry().each([&](flecs::entity e, const Structure& s, const PlanetSurfaceAttachment& attach) {
        if (attach.planetNetId != homePlanet.get<NetId>().value) return;
        if (s.type == StructureType::Colony) homeColony = e;
        if (s.type == StructureType::Base) homeBase = e;
    });
    game.GetRegistry().each([&](flecs::entity e, const Structure& s, const PlanetOrbitAttachment& attach) {
        if (attach.planetNetId == homePlanet.get<NetId>().value && s.type == StructureType::HighPort) homeHighPort = e;
    });
    Require(homeColony.is_alive() && homeBase.is_alive() && homeHighPort.is_alive(),
            "economy: home complex has the structures this test needs (setup check)");

    // (1) Natural production/supply/conversion, no gifting. Colony's own
    // rawMaterials isn't a useful thing to assert on here: local supply
    // drains whatever it produces to Base/High Port the same tick it's
    // produced (by design -- see EconomySystem's per-tick ordering), so it
    // reads ~0 at rest even though production is very much happening.
    // What should visibly grow is Base's stores, fed by that supply.
    for (int tick = 0; tick < 50; ++tick) game.Update();
    const Structure& baseAfter50 = homeBase.get<Structure>();
    Require(baseAfter50.rawMaterials + baseAfter50.finishedMaterials > 0.f,
            "economy: a Colony's production reaches its Base as supplied raw and/or converted finished materials");

    // (2) Gift funds so the Lab/High Port can afford freighters immediately,
    // isolating the build-sequence/consumption assertions from the slow
    // natural ramp (already proven above).
    homeBase.get_mut<Structure>().finishedMaterials = 1000.f;
    homeHighPort.get_mut<Structure>().finishedMaterials = 1000.f;

    const std::uint32_t emptyNetId = emptyPlanet.get<NetId>().value;
    const auto hasStructure = [&](StructureType type) {
        bool found = false;
        game.GetRegistry().each([&](const Structure& s, const PlanetSurfaceAttachment& attach) {
            if (attach.planetNetId == emptyNetId && s.type == type) found = true;
        });
        game.GetRegistry().each([&](const Structure& s, const PlanetOrbitAttachment& attach) {
            if (attach.planetNetId == emptyNetId && s.type == type) found = true;
        });
        return found;
    };
    const auto countFreighters = [&]() {
        std::size_t count = 0;
        game.GetRegistry().each([&](const Freighter&) { ++count; });
        return count;
    };

    bool sawAnyFreighter = false;
    bool baseBuilt = false, colonyBuilt = false, highPortBuilt = false;
    for (int tick = 0; tick < 6000; ++tick) {
        game.Update();
        if (countFreighters() > 0) sawAnyFreighter = true;
        if (!baseBuilt && hasStructure(StructureType::Base)) baseBuilt = true;
        if (!colonyBuilt && hasStructure(StructureType::Colony)) {
            Require(baseBuilt, "economy: Colony never appears before Base");
            colonyBuilt = true;
        }
        if (!highPortBuilt && hasStructure(StructureType::HighPort)) {
            Require(colonyBuilt, "economy: High Port never appears before Colony");
            highPortBuilt = true;
            break; // full sequence done -- no need to keep ticking
        }
    }
    Require(sawAnyFreighter, "economy: at least one freighter was actually dispatched");
    Require(baseBuilt, "economy: the empty planet grew a Base");
    Require(colonyBuilt, "economy: the empty planet grew a Colony");
    Require(highPortBuilt, "economy: the empty planet grew a High Port");
    Require(countFreighters() == 0, "economy: every dispatched freighter was consumed, none left idling");

    fs.Shutdown();
}

// Phase 4's "self-development": a Base grows its own Lab then Comm Center
// from its own finished materials, same-planet and instant (no freighter
// trip). BuildStartingComplex isn't useful here -- it hands out every
// structure already -- so this spawns only a bare Base and lets
// EconomySystem grow the rest.
void TestSelfDevelopment()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    Game game(fs);
    EntitySpawner& spawner = game.GetEntitySpawner();

    flecs::entity planet =
            spawner.SpawnOrbitingPlanet("models/planets/simple"_id, Vector2d{0., 0.}, 1e-9, 2000., 1.0, 0.0);
    planet.set<Team>(Team{TeamId::Blue});

    flecs::entity base = spawner.SpawnStructure(StructureType::Base, "models/structures/base"_id, planet,
                                                TeamId::Blue);

    // Gift funds directly -- isolating the build-sequence assertions from
    // the (already covered elsewhere, in TestFreighterEconomy) production
    // ramp.
    base.get_mut<Structure>().finishedMaterials = 1000.f;

    const std::uint32_t planetNetId = planet.get<NetId>().value;
    const auto hasStructure = [&](StructureType type) {
        bool found = false;
        game.GetRegistry().each([&](const Structure& s, const PlanetSurfaceAttachment& attach) {
            if (attach.planetNetId == planetNetId && s.type == type) found = true;
        });
        game.GetRegistry().each([&](const Structure& s, const PlanetOrbitAttachment& attach) {
            if (attach.planetNetId == planetNetId && s.type == type) found = true;
        });
        return found;
    };

    bool labBuilt = false, commCenterBuilt = false;
    for (int tick = 0; tick < 500; ++tick) {
        game.Update();
        if (!labBuilt && hasStructure(StructureType::Lab)) labBuilt = true;
        if (!commCenterBuilt && hasStructure(StructureType::CommCenter)) {
            Require(labBuilt, "self-development: Comm Center never appears before Lab");
            commCenterBuilt = true;
            break;
        }
    }
    Require(labBuilt, "self-development: Base grew a Lab");
    Require(commCenterBuilt, "self-development: Base grew a Comm Center");

    fs.Shutdown();
}

// Planetside structures sit nested inside their planet's own collision
// circle, so a shot at one crosses the (non-damageable) planet first. It must
// still reach the structure.
void TestPlanetsideStructureHits()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    Game game(fs);
    EntitySpawner& spawner = game.GetEntitySpawner();

    flecs::entity planet =
            spawner.SpawnOrbitingPlanet("models/planets/simple"_id, Vector2d{0., 0.}, 1e-9, 800., 1.0, 0.0);
    BuildStartingComplex(spawner, planet, TeamId::Red);
    game.SettleScenario();

    const auto shootAt = [&](const Vector2d& aim, TeamId team) {
        const Vector2d planetPos = planet.get<Transform>().pos;
        Vector2d dir = aim - planetPos;
        dir = dir.length() > 1e-6 ? dir.normalized() : Vector2d{0., 1.};
        const Vector2d from = aim + dir * 300.;
        flecs::entity bullet = spawner.SpawnBullet("models/bullets/bullet-0"_id, from, -dir * 250.,
                                                  /*sensor=*/true);
        bullet.emplace<Bullet>(3.0, team, 10.f, 0u);
        return bullet;
    };

    // Total hull across the complex, not the aimed structure's own: the
    // complex is packed tightly enough inside the planet that a radial shot
    // can cross a neighbour first, which is fair game. What must not happen
    // (the bug) is a shot damaging nothing at all and flying on through.
    const auto complexHp = [&] {
        float total = 0.f;
        game.GetRegistry().each([&](const Structure&, const Damageable& dmg, const PlanetSurfaceAttachment&) {
            total += dmg.hp;
        });
        return total;
    };

    // Aimed at each planetside type in turn, since the symptom was
    // per-structure: the ones poking out past their planet's rim were
    // hittable and the rest silently were not.
    for (const StructureType type : {StructureType::Base, StructureType::Colony, StructureType::Lab,
                                     StructureType::CommCenter}) {
        flecs::entity structure;
        game.GetRegistry().each([&](flecs::entity e, const Structure& s, const PlanetSurfaceAttachment&) {
            if (s.type == type) structure = e;
        });
        Require(structure.is_alive(), "damage: the starting complex has every planetside structure");

        const float hpBefore = complexHp();
        flecs::entity bullet = shootAt(structure.get<Transform>().pos, TeamId::Blue);
        for (int tick = 0; tick < 150 && bullet.is_alive(); ++tick) game.Update();

        Require(complexHp() < hpBefore,
                "damage: a shot from outside reaches a structure nested in its planet");
        Require(!bullet.is_alive(), "damage: the shot is consumed by the structure it hit");
    }

    fs.Shutdown();
}

// A hull as EntitySpawner::MakeShipLoadout builds one: light guns in the nose
// and its drive fitted, both rank 1. Tests that fit anything hanging off either
// of those need to start from a real ship rather than a zeroed struct -- a bare
// ShipLoadout is a hull with no guns and no engine, which nothing ever flies.
static ShipLoadout StockLoadout()
{
    ShipLoadout loadout;
    loadout.levels.gunTier = 1;
    loadout.mounts[0] = MountArm::Light;
    loadout.levels.engine = 1;
    return loadout;
}

// Fits a rank straight onto a hull, bypassing both currencies and the faction
// gate: a test setting a ship up is exercising what the part does, not the
// shop that sells it.
static void FitFree(const UpgradeCatalog& catalog, const UpgradeDef& def, std::uint8_t rank,
                    ShipLoadout& loadout)
{
    TechUnlocks all;
    for (std::size_t i = 0; i < catalog.Defs().size(); ++i) {
        all.rank[i] = UpgradeCatalog::RankCount(catalog.Defs()[i]);
    }
    std::uint32_t purse = UpgradeCatalog::SupplyCostOf(def, rank);
    catalog.FitRank(def, rank, loadout, all, purse, /*atLab=*/true);
}

// UpgradeCatalog: the pool loads, the two rank tracks gate each other (a hull
// may fit only what its faction has learned), a named rank is bought outright
// at its own price, and the resolved stats move the right way with each rank.
void TestUpgradeCatalog()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    UpgradeCatalog catalog;
    Require(catalog.Load(fs), "catalog: data/upgrades.toml parses");
    Require(!catalog.Defs().empty(), "catalog: the pool is not empty");

    const UpgradeDef* fireRate = catalog.FindKind(UpgradeKind::FireRate);
    Require(fireRate != nullptr, "catalog: the pool has a fire-rate upgrade");

    // Every rank of everything, so the ship-tree assertions below are about
    // the hull's own rules rather than about what the side happens to know.
    TechUnlocks all;
    for (std::size_t i = 0; i < catalog.Defs().size(); ++i) {
        all.rank[i] = UpgradeCatalog::RankCount(catalog.Defs()[i]);
    }
    // Enough Supplies that nothing below is refused for being unaffordable.
    constexpr std::uint32_t RICH = 100000;

    UpgradeLevels levels;
    const ShipStats base = catalog.ResolveStats(levels);
    Require(base.gun != nullptr && base.gun->id == catalog.Fitted().shipGun,
            "catalog: an empty loadout resolves to the ship's fitted gun");
    Require(base.fireCooldownTicks == base.gun->cooldownTicks,
            "catalog: an unupgraded cadence is the fitted gun's own");

    levels.fireRate = fireRate->maxLevel;
    Require(catalog.ResolveStats(levels).fireCooldownTicks < base.fireCooldownTicks,
            "catalog: fire-rate ranks shorten the gun's cooldown");
    {
        ShipLoadout maxed;
        maxed.levels = levels;
        const UpgradeCatalog::ShipContext ctx{&maxed, &all, RICH, true};
        Require(catalog.ShipState(*fireRate, fireRate->maxLevel, ctx) == TechNodeState::Held,
                "catalog: a rank already fitted is not for sale again");
        // The ranks under it are still for sale, because fitting one is a
        // downgrade -- a trade a pilot may want, and a rank is set rather than
        // climbed, so nothing is in the way of it.
        Require(catalog.ShipState(*fireRate, 1, ctx) == TechNodeState::Available,
                "catalog: ...while the ranks under it are offered as a downgrade");

        std::uint32_t spend = RICH;
        Require(catalog.FitRank(*fireRate, 1, maxed, all, spend, true),
                "catalog: ...which can be fitted");
        Require(UpgradeCatalog::LevelOf(*fireRate, maxed.levels) == 1,
                "catalog: ...and leaves the hull carrying the lower rank");
    }

    // Pulling the drive is allowed to strand a hull -- it steers, coasts and
    // falls -- but only ever with feet down, and rank I is issued free to every
    // side, so a stranded pilot is always one click from flying again.
    {
        const UpgradeDef* drive = catalog.FindKind(UpgradeKind::EngineTier);
        Require(drive != nullptr, "catalog: the pool has a drive");

        ShipLoadout hull;
        hull.levels.engine = 1;
        Require(!catalog.StripRank(*drive, hull, /*atLab=*/false),
                "catalog: a drive cannot be pulled away from a yard");
        Require(catalog.StripRank(*drive, hull, /*atLab=*/true),
                "catalog: ...and comes off on the pad");
        Require(catalog.ResolveStats(hull.levels).thrustScale == 0.f,
                "catalog: a hull with its drive pulled cannot accelerate");

        const UpgradeCatalog::ShipContext broke{&hull, &all, 0, true};
        Require(catalog.ShipState(*drive, 1, broke) == TechNodeState::Available,
                "catalog: ...and can fit the issued rank again on an empty account");
    }

    // The missile line: nothing to fire and nowhere to put rounds until a bay
    // is fitted, and each tier widens both. Reloading is not on this table at
    // all -- a yard fills the tubes (ResearchSystem), so there is no restock
    // to buy.
    const UpgradeDef* bay = catalog.FindKind(UpgradeKind::MissileTier);
    Require(bay != nullptr, "catalog: the pool has a missile bay");
    Require(base.missile == nullptr && base.missileCapacity == 0,
            "catalog: an unupgraded hull carries no launcher at all");

    ShipLoadout racked;
    std::uint32_t purse = RICH;
    Require(catalog.FitRank(*bay, 1, racked, all, purse, true), "catalog: the bay can be fitted");
    Require(purse < RICH, "catalog: fitting a rank spends Supplies");
    Require(racked.missileAmmo == catalog.ResolveStats(racked.levels).missileCapacity,
            "catalog: fitting the bay fills the rack it decides the width of");

    // Every def in the pool is something a hull keeps, now that the one
    // repeatable entry is gone -- a restock would need its own rules again.
    for (const UpgradeDef& def : catalog.Defs()) {
        Require(def.maxLevel > 0, "catalog: every entry in the pool holds a rank");
    }

    // The two gates, each reported as itself so the UI can explain which one
    // is in the way.
    {
        ShipLoadout gated;
        TechUnlocks none;
        const UpgradeCatalog::ShipContext locked{&gated, &none, RICH, true};
        Require(catalog.ShipState(*bay, 1, locked) == TechNodeState::NotUnlocked,
                "catalog: a hull cannot fit what its faction has not learned");
        const UpgradeCatalog::ShipContext flying{&gated, &all, RICH, false};
        Require(catalog.ShipState(*bay, 1, flying) == TechNodeState::NeedsLanding,
                "catalog: fitting a part needs a landing");
        const UpgradeCatalog::ShipContext broke{&gated, &all, 0, true};
        Require(catalog.ShipState(*bay, 1, broke) == TechNodeState::Unaffordable,
                "catalog: a rank nobody can pay for is not on offer");
        std::uint32_t nothing = 0;
        Require(!catalog.FitRank(*bay, 1, gated, all, nothing, true),
                "catalog: a refused rank changes neither the hull nor the purse");
        Require(gated.levels.missileTier == 0 && nothing == 0,
                "catalog: ...and leaves both exactly as they were");
    }

    const WeaponDef* previous = nullptr;
    int previousCapacity = 0;
    for (std::uint8_t tier = 1; tier <= bay->maxLevel; ++tier) {
        UpgradeLevels tierLevels;
        tierLevels.missileTier = tier;
        const ShipStats round = catalog.ResolveStats(tierLevels);
        Require(round.missile != nullptr && round.missile->IsGuided(),
                "catalog: every bay tier resolves to a guided round");
        Require(round.missileCapacity > previousCapacity, "catalog: each bay tier widens the rack");
        if (previous) {
            Require(round.missile->damage > previous->damage
                            && round.missile->lifetimeSeconds > previous->lifetimeSeconds
                            && round.missile->guidance.turnRate > previous->guidance.turnRate,
                    "catalog: each missile tier hits harder, flies longer and turns tighter");
            Require(round.missile->guidance.wobble < previous->guidance.wobble,
                    "catalog: ...and hunts less around its solution");
        }
        previous = round.missile;
        previousCapacity = round.missileCapacity;
    }
    // How long a round can chase is what separates the tiers, so it is bounded
    // at the top rather than everywhere: a first-tier missile burns out before
    // it has crossed a dogfight, and the last one flies the whole engagement.
    Require(previous->lifetimeSeconds <= 8.0,
            "catalog: even the top missile eventually burns out");
    Require(previous->guidance.wobble == 0.0, "catalog: ...and the top one never wobbles");
    Require(previousCapacity <= 24, "catalog: the widest rack the bay alone gives is 24 rounds");

    // The PERMANENT tree is a ladder: ranks are learned in order, each spends
    // Tech, and nothing is offered past the last one.
    {
        TechUnlocks unlocked;
        std::uint32_t tech = 1000;
        Require(catalog.PermanentState(*bay, 2, unlocked, tech) == TechNodeState::Locked,
                "catalog: a side cannot skip to rank II of a line it has not started");
        for (std::uint8_t rank = 1; rank <= bay->maxLevel; ++rank) {
            Require(catalog.UnlockRank(*bay, rank, unlocked, tech),
                    "catalog: the faction learns a line one rank at a time");
        }
        Require(catalog.UnlockedRank(*bay, unlocked) == bay->maxLevel,
                "catalog: ...up to the line's last rank");
        Require(!catalog.UnlockRank(*bay, static_cast<std::uint8_t>(bay->maxLevel + 1), unlocked, tech),
                "catalog: and no further");
        Require(tech < 1000, "catalog: learning a rank spends Tech");

        std::uint32_t broke = 0;
        TechUnlocks fresh;
        Require(catalog.PermanentState(*bay, 1, fresh, broke) == TechNodeState::Unaffordable,
                "catalog: a side short of Tech is told so rather than refused silently");
    }

    const UpgradeDef* gun = catalog.FindKind(UpgradeKind::WeaponTier);
    Require(gun != nullptr && !gun->tiers.empty(), "catalog: the pool has a weapon-tier upgrade");
    levels.gunTier = 1;
    const ShipStats stock = catalog.ResolveStats(levels);
    Require(stock.gun != nullptr && stock.gun->id == gun->tiers[0],
            "catalog: a gun rank fits that rank's weapon rather than scaling the one below");
    // Rank 1 of the line is the weapon every hull already flies with, which is
    // what lets a ship spawn with the line fitted rather than above it.
    Require(stock.gun == base.gun, "catalog: the light guns start at the stock weapon");

    levels.gunTier = 2;
    const ShipStats heavier = catalog.ResolveStats(levels);
    Require(heavier.gun->damage > base.gun->damage && heavier.gun->speed > base.gun->speed,
            "catalog: a rank above the first hits harder and throws flatter than stock");

    // The gun line is a wider bore on the same mounts -- each tier outdoes the
    // one below it, and they deliberately share the stock round's model: it is
    // the same ammunition, not a different weapon.
    {
        const WeaponDef* below = nullptr;
        for (std::uint8_t tier = 1; tier <= gun->maxLevel; ++tier) {
            UpgradeLevels tierLevels;
            tierLevels.gunTier = tier;
            const WeaponDef* round = catalog.ResolveStats(tierLevels).gun;

            Require(round != nullptr, "catalog: every gun rank resolves to a weapon");
            if (below) {
                Require(round->damage > below->damage && round->speed > below->speed,
                        "catalog: each gun rank out-damages and outruns the one below it");
            }
            below = round;
        }
    }

    // Holding a rank is a property of a mount, not of the ship. Light guns in
    // the nose leave every other mount buyable, and the hull's own fitting
    // counts as researched enough to repeat -- arming a second mount with the
    // weapon already aboard asks nothing new of the faction.
    {
        ShipLoadout stockHull;
        stockHull.levels.gunTier = 1;
        stockHull.mounts[0] = MountArm::Light;
        TechUnlocks nothingLearned;
        std::uint32_t purse = RICH;

        const UpgradeCatalog::ShipContext inNose{&stockHull, &nothingLearned, purse, true, 0};
        Require(catalog.ShipState(*gun, 1, inNose) == TechNodeState::Held,
                "catalog: the mount holding a line reports it held");

        const UpgradeCatalog::ShipContext inWing{&stockHull, &nothingLearned, purse, true, 1};
        Require(catalog.ShipState(*gun, 1, inWing) == TechNodeState::Available,
                "catalog: ...and an empty mount is still sold the same line");

        Require(catalog.FitRank(*gun, 1, stockHull, nothingLearned, purse, true, /*mount=*/1),
                "catalog: a second mount takes the line the hull already carries");
        Require(stockHull.mounts[1] == MountArm::Light && stockHull.mounts[2] == MountArm::None,
                "catalog: ...into the mount the pick named, and no other");

        // A rank above what the hull carries still needs researching.
        Require(catalog.ShipState(*gun, 2, inWing) == TechNodeState::NotUnlocked,
                "catalog: carrying rank I does not unlock rank II");

        // Pulling a part empties the mount named and nothing else. What the
        // hull paid for stays paid for, so the same rank goes back in without
        // the faction ever having researched it.
        Require(catalog.StripRank(*gun, stockHull, /*atLab=*/true, /*mount=*/1),
                "catalog: a mount can be stripped back to empty");
        Require(stockHull.mounts[1] == MountArm::None && stockHull.mounts[0] == MountArm::Light,
                "catalog: ...the one the pick named, leaving the other armed");
        Require(!catalog.StripRank(*gun, stockHull, /*atLab=*/true, /*mount=*/1),
                "catalog: an empty mount has nothing to strip");
        Require(!catalog.StripRank(*gun, stockHull, /*atLab=*/false, /*mount=*/0),
                "catalog: and a yard is needed to pull one at all");
        Require(catalog.ShipState(*gun, 1, inWing) == TechNodeState::Available
                        && stockHull.levels.gunTier == 1,
                "catalog: a stripped mount is sold the line back at the rank it held");
    }

    // A missile bay is a hole like a weapon mount, not a ship-wide level: the
    // port bay carrying a launcher leaves the starboard one empty and for sale.
    {
        const UpgradeDef* bay = catalog.FindKind(UpgradeKind::MissileTier);
        Require(bay, "catalog: the pool has a missile bay");

        ShipLoadout hull;
        TechUnlocks learned;
        learned.rank[catalog.IndexOf(bay->id)] = 1;
        std::uint32_t purse = RICH;

        Require(catalog.FitRank(*bay, 1, hull, learned, purse, true, /*mount=*/0),
                "catalog: a launcher goes into the bay the pick named");
        Require(MissileBayFitted(hull, 0) && !MissileBayFitted(hull, 1),
                "catalog: ...and into that bay alone");

        const UpgradeCatalog::ShipContext port{&hull, &learned, purse, true, 0};
        const UpgradeCatalog::ShipContext starboard{&hull, &learned, purse, true, 1};
        Require(catalog.ShipState(*bay, 1, port) == TechNodeState::Held,
                "catalog: the bay holding a launcher reports it held");
        Require(catalog.ShipState(*bay, 1, starboard) == TechNodeState::Available,
                "catalog: ...and the empty one is still sold the same launcher");

        Require(catalog.FitRank(*bay, 1, hull, learned, purse, true, /*mount=*/1)
                        && MissileBaysFitted(hull) == 2,
                "catalog: the second bay takes one of its own");

        // The rack is the ship's, as the cannon's magazine is: the last tube
        // gone is what empties it, not the first.
        hull.missileAmmo = 4;
        Require(catalog.StripRank(*bay, hull, true, /*mount=*/0)
                        && MissileBaysFitted(hull) == 1 && hull.missileAmmo == 4,
                "catalog: pulling one launcher leaves the rack loaded for the other");
        Require(catalog.StripRank(*bay, hull, true, /*mount=*/1) && hull.missileAmmo == 0,
                "catalog: pulling the last one leaves nothing to fire the rounds through");
    }

    // The cannon is a second weapon rather than a better gun: a hull carries
    // both, and the guns stay exactly what they were when one is fitted.
    const UpgradeDef* cannonLine = catalog.FindKind(UpgradeKind::CannonTier);
    Require(cannonLine != nullptr && !cannonLine->tiers.empty(),
            "catalog: the pool has a cannon-tier upgrade");
    Require(base.cannon == nullptr && base.cannonCapacity == 0,
            "catalog: a stock hull carries no cannon at all");
    {
        UpgradeLevels both;
        both.gunTier = 1;
        both.cannonTier = 1;
        const ShipStats armed = catalog.ResolveStats(both);
        Require(armed.cannon != nullptr && armed.cannon->id == cannonLine->tiers[0],
                "catalog: a cannon rank fits that rank's weapon");
        Require(armed.gun != nullptr && armed.gun->id == gun->tiers[0],
                "catalog: fitting a cannon leaves the guns alone -- the hull carries both");
        Require(armed.cannonCapacity > armed.missileCapacity,
                "catalog: the magazine is deeper than the missile rack");
    }

    // Each rank hits harder and throws flatter than the one below it, and the
    // whole line is drawn as a streak rather than the stock round's point --
    // the top rank in its own colour. Asserted here because a mistyped model
    // path is otherwise a silent placeholder at runtime.
    ResourceLoader tierLoader(fs);
    {
        const WeaponDef* below = base.gun;
        id_t firstTierModel = 0;
        for (std::uint8_t tier = 1; tier <= cannonLine->maxLevel; ++tier) {
            UpgradeLevels tierLevels;
            tierLevels.cannonTier = tier;
            const ShipStats tierStats = catalog.ResolveStats(tierLevels);
            const WeaponDef* round = tierStats.cannon;

            Require(round != nullptr, "catalog: every cannon rank resolves to a weapon");
            Require(round->modelId != 0 && round->modelId != base.gun->modelId,
                    "catalog: a heavy rank fires its own round, not the stock one");
            Require(tierLoader.Load<Body>(round->modelId)->GetCircleShapes().size() == 1,
                    "catalog: a heavy round's model loads (one hitbox, not a placeholder)");
            Require(round->damage > below->damage && round->speed > below->speed,
                    "catalog: each cannon rank out-damages and outruns the one below it");
            Require(tierStats.cannonCapacity > 0,
                    "catalog: a fitted cannon has a magazine to fire from");

            if (tier == 1) firstTierModel = round->modelId;
            if (tier == cannonLine->maxLevel) {
                Require(round->modelId != firstTierModel,
                        "catalog: the top cannon rank is drawn differently from the first");
            }
            below = round;
        }
    }

    // Fitting the cannon loads it, the way fitting the bay fills the rack --
    // and a dry magazine is what puts the trigger back on the guns.
    {
        ShipLoadout armed = StockLoadout();
        std::uint32_t purse = RICH;
        Require(catalog.FitRank(*cannonLine, 1, armed, all, purse, true),
                "catalog: the cannon can be fitted");
        // Mount 0 is the nose, which a stock hull already flies its light guns
        // in, so the first FREE mount is 1 -- and the guns it fell in beside are
        // what the dry-magazine fallback below needs something to fall back *to*.
        Require(armed.mounts[1] == MountArm::Heavy,
                "catalog: a weapon with no mount named takes the first free one");
        Require(armed.mounts[0] == MountArm::Light,
                "catalog: ...rather than displacing the guns already aboard");
        Require(armed.cannonAmmo == catalog.ResolveStats(armed.levels).cannonCapacity,
                "catalog: fitting the cannon fills the magazine it decides the depth of");

        const ShipStats armedStats = catalog.ResolveStats(armed.levels);
        Controls controls;
        const ShipControlsSystem::PrimarySet both =
                ShipControlsSystem::PrimaryWeapons(controls, armedStats, &armed);
        Require(both.count == 2 && both.lines[0].weapon == armedStats.cannon
                        && both.lines[1].weapon == armedStats.gun,
                "controls: the trigger works both lines by default");

        armed.cannonAmmo = 0;
        const ShipControlsSystem::PrimarySet dry =
                ShipControlsSystem::PrimaryWeapons(controls, armedStats, &armed);
        Require(dry.count == 1 && dry.lines[0].weapon == armedStats.gun && !dry.lines[0].spendsAmmo,
                "controls: a dry cannon drops out and leaves the guns firing");
        Require(controls.activeWeapon == ActiveWeapon::Both,
                "controls: ...without changing what the pilot asked for, so a reload re-arms it");

        armed.cannonAmmo = 10;
        controls.activeWeapon = ActiveWeapon::Cannon;
        const ShipControlsSystem::PrimarySet heavyOnly =
                ShipControlsSystem::PrimaryWeapons(controls, armedStats, &armed);
        Require(heavyOnly.count == 1 && heavyOnly.lines[0].weapon == armedStats.cannon,
                "controls: asking for the cannon alone holds the guns back");

        armed.cannonAmmo = 0;
        Require(ShipControlsSystem::PrimaryWeapons(controls, armedStats, &armed).lines[0].weapon
                        == armedStats.gun,
                "controls: a dry cannon still falls through to the guns");

        controls.activeWeapon = ActiveWeapon::Gun;
        armed.cannonAmmo = 10;
        const ShipControlsSystem::PrimarySet lightOnly =
                ShipControlsSystem::PrimaryWeapons(controls, armedStats, &armed);
        Require(lightOnly.count == 1 && lightOnly.lines[0].weapon == armedStats.gun,
                "controls: asking for the guns is honoured with a loaded cannon aboard");

        // Round and round: three states need a cycle, and the default leads.
        Require(NextWeapon(ActiveWeapon::Both) == ActiveWeapon::Cannon
                        && NextWeapon(ActiveWeapon::Cannon) == ActiveWeapon::Gun
                        && NextWeapon(ActiveWeapon::Gun) == ActiveWeapon::Both,
                "controls: the toggle cycles both -> cannon -> guns -> both");
    }

    // Both shields resolve to a real reservoir, and swapping type resets the
    // tier rather than carrying it across.
    ShipLoadout loadout;
    const UpgradeDef* bubble = catalog.FindKind(UpgradeKind::Shield, ShieldType::Bubble);
    const UpgradeDef* plating = catalog.FindKind(UpgradeKind::Shield, ShieldType::Plating);
    Require(bubble && plating, "catalog: the pool has both shield types");
    std::uint32_t shieldPurse = RICH;
    Require(catalog.FitRank(*bubble, 2, loadout, all, shieldPurse, true),
            "catalog: a rank can be bought outright without owning the ones below it");
    Require(loadout.levels.shield == 2 && loadout.levels.shieldType == ShieldType::Bubble,
            "catalog: ...and the hull holds exactly the rank it paid for");
    Require(RICH - shieldPurse == UpgradeCatalog::SupplyCostOf(*bubble, 2),
            "catalog: a rank costs its own price, not the sum of the ranks below it");
    Require(catalog.ResolveStats(loadout.levels).shieldCapacity > 0.f,
            "catalog: a fitted shield resolves to a real capacity");
    Require(catalog.FitRank(*plating, 1, loadout, all, shieldPurse, true),
            "catalog: the other shield type can be swapped in");
    Require(loadout.levels.shieldType == ShieldType::Plating && loadout.levels.shield == 1,
            "catalog: swapping shield type starts the new emitter at the rank bought");

    // Plating hangs off the bubble, and the two replace each other -- so the
    // gate has to be what the FACTION has learned. Checked against the hull it
    // would lock plating out the moment the swap took the bubble off, which is
    // the only state it is ever fitted from.
    {
        ShipLoadout swapped = StockLoadout();
        std::uint32_t purse = RICH;
        Require(catalog.FitRank(*bubble, 1, swapped, all, purse, true),
                "catalog: the bubble goes on first");
        Require(catalog.FitRank(*plating, 1, swapped, all, purse, true),
                "catalog: plating can be swapped in behind it");
        Require(UpgradeCatalog::LevelOf(*bubble, swapped.levels) == 0,
                "catalog: ...which leaves the bubble reading as unfitted");
        Require(catalog.FitRank(*plating, 2, swapped, all, purse, true),
                "catalog: ...and a deeper plating rank is still on offer afterwards");

        // With nothing learned, it is genuinely locked -- the gate still gates.
        TechUnlocks none;
        ShipLoadout fresh = StockLoadout();
        Require(catalog.ShipState(*plating, 1,
                                 UpgradeCatalog::ShipContext{&fresh, &none, RICH, true})
                        == TechNodeState::Locked,
                "catalog: plating is locked to a side that has not learned the bubble");
    }

    // The whole point of an absolute price: a pilot who has III unlocked but
    // cannot afford it fits II instead of being locked out.
    {
        ShipLoadout thrifty;
        const std::uint16_t cheap = UpgradeCatalog::SupplyCostOf(*bubble, 2);
        const UpgradeCatalog::ShipContext ctx{&thrifty, &all, cheap, true};
        Require(catalog.ShipState(*bubble, 3, ctx) == TechNodeState::Unaffordable
                        && catalog.ShipState(*bubble, 2, ctx) == TechNodeState::Available,
                "catalog: a hull short of the top rank is still sold the one below it");
    }

    // The two ammo lockers: spares on top of the weapon's own magazine, generic
    // stowage bays either of them fits, and rounds that leave with the fitting
    // that held them.
    {
        const UpgradeDef* shells = catalog.FindAmmoStore(AmmoPool::Cannon);
        const UpgradeDef* warheads = catalog.FindAmmoStore(AmmoPool::Missile);
        Require(shells && warheads, "catalog: the pool has a locker for each magazine");

        ShipLoadout hull = StockLoadout();
        std::uint32_t purse = RICH;
        Require(catalog.FitRank(*cannonLine, 1, hull, all, purse, true),
                "catalog: the cannon goes on before its box");
        const int bare = catalog.ResolveStats(hull.levels).cannonCapacity;

        Require(catalog.FitRank(*shells, 1, hull, all, purse, true),
                "catalog: the shell locker can be fitted");
        Require(catalog.ResolveStats(hull.levels).cannonCapacity > bare,
                "catalog: a locker deepens the magazine it feeds");
        Require(hull.cannonAmmo <= catalog.ResolveStats(hull.levels).cannonCapacity,
                "catalog: ...and never leaves more rounds aboard than there is room for");

        // A locker is stocked for a weapon the SIDE has learned, not only for
        // one already on this hull: a pilot refitting from scratch buys the
        // rounds and the launcher in the same visit. With nothing learned the
        // gate still gates.
        Require(catalog.ShipState(*warheads, 1,
                                 UpgradeCatalog::ShipContext{&hull, &all, RICH, true})
                        == TechNodeState::Available,
                "catalog: a locker is sold for a weapon the faction has researched");
        TechUnlocks none;
        Require(catalog.ShipState(*warheads, 1,
                                 UpgradeCatalog::ShipContext{&hull, &none, RICH, true})
                        == TechNodeState::Locked,
                "catalog: ...and locked behind a weapon it has not");
        Require(catalog.FitRank(*bay, 1, hull, all, purse, true),
                "catalog: a launcher for the warheads to feed");

        // A bay each, taken in order, so fitting one says nothing about the
        // other.
        const int deepened = catalog.ResolveStats(hull.levels).cannonCapacity;
        Require(catalog.FitRank(*warheads, 1, hull, all, purse, true),
                "catalog: the other locker goes into the next bay along");
        Require(AmmoStoreRank(hull.levels, AmmoPool::Missile) == 1
                        && AmmoStoreRank(hull.levels, AmmoPool::Cannon) == 1,
                "catalog: a hull can carry both lockers at once");
        Require(catalog.ResolveStats(hull.levels).cannonCapacity == deepened,
                "catalog: ...and the shells are still stowed after the warheads go on");
        Require(UpgradeCatalog::LevelOf(*shells, hull.levels) == 1,
                "catalog: each locker reports its own rank");

        Require(catalog.StripRank(*warheads, hull, true), "catalog: a locker can be pulled");
        Require(AmmoStoreRank(hull.levels, AmmoPool::Missile) == 0
                        && AmmoStoreRank(hull.levels, AmmoPool::Cannon) == 1,
                "catalog: ...which leaves the other one where it was");

        // The bays are interchangeable, which is the whole point of them being
        // generic: two of the same box stack, and a box bought into an occupied
        // bay swaps what was there rather than being refused.
        {
            ShipLoadout twin = StockLoadout();
            std::uint32_t pocket = RICH;
            Require(catalog.FitRank(*cannonLine, 1, twin, all, pocket, true),
                    "catalog: a gun for the shells to feed");
            const int oneGun = catalog.ResolveStats(twin.levels).cannonCapacity;

            Require(catalog.FitRank(*shells, 1, twin, all, pocket, true, 0)
                            && catalog.FitRank(*shells, 1, twin, all, pocket, true, 1),
                    "catalog: both bays will take a shell box");
            Require(AmmoStoreRank(twin.levels, AmmoPool::Cannon) == 2,
                    "catalog: ...and the magazine counts both of them");
            Require(catalog.ResolveStats(twin.levels).cannonCapacity
                            == oneGun + 2 * shells->ammo.capacity,
                    "catalog: a second box is a second box's worth of spares");
            Require(UpgradeCatalog::LevelOf(*shells, twin.levels) == 1,
                    "catalog: ...but a box is still one rank, however many are aboard");

            Require(catalog.FitRank(*bay, 1, twin, all, pocket, true),
                    "catalog: a launcher, so the warheads have somewhere to go");
            Require(catalog.FitRank(*warheads, 1, twin, all, pocket, true, 1),
                    "catalog: a locker bought into an occupied bay swaps into it");
            Require(AmmoStoreRank(twin.levels, AmmoPool::Cannon) == 1
                            && AmmoStoreRank(twin.levels, AmmoPool::Missile) == 1,
                    "catalog: ...and the box it replaced is off the hull");
            Require(twin.cannonAmmo <= catalog.ResolveStats(twin.levels).cannonCapacity,
                    "catalog: ...taking the rounds it was stowing with it");
        }
    }

    // The drive scales the hull's own numbers, and the overburn's ceiling stays
    // a multiple of the hull's rather than of the drive's.
    {
        const UpgradeDef* engine = catalog.FindKind(UpgradeKind::EngineTier);
        const UpgradeDef* bank = catalog.FindKind(UpgradeKind::Capacitor);
        Require(engine && bank, "catalog: the pool has both the drive and the bank");

        // A hull with no drive at all does not move under its own power. Nothing
        // ever flies in that state -- rank 1 is issued and fitted at spawn --
        // but pulling the engine at a yard is allowed, and this is what it buys.
        ShipLoadout drifting;
        Require(catalog.ResolveStats(drifting.levels).thrustScale == 0.f,
                "catalog: a hull with no engine cannot accelerate");

        ShipLoadout hull = StockLoadout();
        std::uint32_t purse = RICH;
        const ShipStats stock = catalog.ResolveStats(hull.levels);
        Require(stock.thrustScale == 1.f && stock.maxSpeedScale == 1.f,
                "catalog: the drive a hull spawns with scales nothing -- it IS stock");

        Require(catalog.FitRank(*engine, 2, hull, all, purse, true),
                "catalog: the drive can be upgraded");
        const ShipStats better = catalog.ResolveStats(hull.levels);
        Require(better.thrustScale > 1.f && better.maxSpeedScale > 1.f,
                "catalog: a better drive scales both thrust and cruise");

        Require(catalog.FitRank(*engine, 3, hull, all, purse, true),
                "catalog: ...and again");
        const ShipStats best = catalog.ResolveStats(hull.levels);
        Require(best.thrustScale > better.thrustScale,
                "catalog: compounding per rank rather than applying once");
    }

    // Rearming is priced off what feeds the magazine, pro-rata for what is
    // actually missing -- and it is the yard's call, not the client's.
    {
        ShipLoadout hull = StockLoadout();
        std::uint32_t purse = RICH;
        Require(catalog.ResupplyCost(hull) == 0,
                "catalog: a hull with nothing that runs out costs nothing to rearm");

        Require(catalog.FitRank(*cannonLine, 1, hull, all, purse, true),
                "catalog: a cannon to run dry");
        Require(catalog.ResupplyCost(hull) == 0,
                "catalog: a full magazine costs nothing to rearm");
        Require(!catalog.Resupply(hull, purse, true),
                "catalog: ...and rearming a full one is refused rather than charged for");

        const int capacity = catalog.ResolveStats(hull.levels).cannonCapacity;
        hull.cannonAmmo = static_cast<std::uint16_t>(capacity / 2);
        const std::uint32_t half = catalog.ResupplyCost(hull);
        hull.cannonAmmo = 0;
        const std::uint32_t whole = catalog.ResupplyCost(hull);
        Require(half > 0 && whole > half,
                "catalog: rearming is priced by how much is missing");
        Require(whole < UpgradeCatalog::SupplyCostOf(*cannonLine, 1),
                "catalog: ...and filling a magazine costs less than the gun that empties it");

        Require(!catalog.Resupply(hull, purse, false),
                "catalog: rearming away from a yard is refused");
        Require(hull.cannonAmmo == 0, "catalog: ...leaving the magazine as it was");

        std::uint32_t broke = whole - 1;
        Require(!catalog.Resupply(hull, broke, true) && broke == whole - 1,
                "catalog: rearming without the supplies for it costs nothing");

        const std::uint32_t before = purse;
        Require(catalog.Resupply(hull, purse, true), "catalog: rearming at a yard fills up");
        Require(hull.cannonAmmo == capacity, "catalog: ...to the capacity the hull has fitted");
        Require(before - purse == whole, "catalog: ...for exactly the price it quoted");

        // The rank survives the mount coming off, so without a gate on what is
        // actually mounted a yard would happily sell rounds for a gun that is
        // no longer aboard.
        Require(catalog.StripRank(*cannonLine, hull, true),
                "catalog: the cannon can come back off");
        Require(catalog.ResupplyCost(hull) == 0,
                "catalog: a magazine with no mount left to fire it is not sold rounds");
    }

    // Layout: a def sits one column right of what it requires, which is what
    // lets the tree draw a connector between them.
    // Nothing in the pool has a prerequisite since the restock left, so this
    // holds the rule against whatever gains one next rather than naming a def.
    {
        const std::size_t bayIndex = catalog.IndexOf(bay->id);
        Require(bayIndex < catalog.Defs().size(), "catalog: every def has an index of its own");
        Require(catalog.SlotOf(bayIndex).col == 0, "catalog: a def with no prerequisite is a root");

        for (const UpgradeDef& def : catalog.Defs()) {
            if (def.requiresId == 0) continue;
            Require(catalog.SlotOf(catalog.IndexOf(def.id)).col
                            == catalog.SlotOf(catalog.IndexOf(def.requiresId)).col + 1,
                    "catalog: a def sits one column right of what it requires");
        }
        Require(catalog.TreeColumns() >= 1 && catalog.TreeRows() >= 1,
                "catalog: the tree has a size to lay out");
    }

    fs.Shutdown();
}

// DamageSystem/ShieldSystem: a fitted shield eats a hit before the hull does,
// and recharges once the fire lets up.
// A round leaves the mount its weapon names (WeaponDef::hardpoint), and a
// client can resolve where it lands from the same authored geometry the server
// built its Chipmunk shapes from -- the two halves of a hit that has to look
// the same on both sides of the wire.
void TestHardpointMounts()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    ResourceLoader loader(fs);
    const ResourcePtr<const Body> body = loader.Load<Body>("models/ships/fighter-1"_id);

    const Body::Hardpoint* forward = body->FindMount("weapon", 0);
    const Body::Hardpoint* wing = body->FindMount("weapon", 1);
    Require(forward && wing, "hardpoints: fighter-1 carries several weapon mounts");
    Require(forward->pos != wing->pos, "hardpoints: the two are distinct points on the hull");
    Require(body->FindMount("plasma", 0) == nullptr,
            "hardpoints: a family the hull doesn't carry resolves to nothing, so a weapon can fall back");

    // One bay, and it is not one of the gun mounts: a launcher leaves from
    // where the model says it does, and there is exactly one place to say.
    const Body::Hardpoint* rack = body->FindMount("missile", 0);
    Require(rack && body->CountMounts("missile") == 1,
            "hardpoints: fighter-1 carries a single missile bay");
    Require(rack->pos != forward->pos && rack->pos != wing->pos,
            "hardpoints: ...at a point of its own on the hull");
    Require(body->FindMount("missile", 1) == rack, "hardpoints: the mount index wraps");

    // Mounts a pilot cannot tell apart are mounts the feature does not have:
    // every pair on a hull has to be far enough apart to read at ship scale.
    // (fighter-1 is ~14 units nose to tail.)
    const auto farApart = [](const Body::Hardpoint* a, const Body::Hardpoint* b) {
        return (a->pos - b->pos).length() > 1.0;
    };
    Require(farApart(forward, wing), "hardpoints: two weapon muzzles are visibly apart");
    Require(farApart(rack, forward), "hardpoints: the bay is visibly clear of the guns");
    Require(farApart(body->FindMount("weapon", 1), body->FindMount("weapon", 2)),
            "hardpoints: the paired wing mounts are visibly apart");
    Require(body->FindMount("weapon", 3) == forward, "hardpoints: the mount index wraps");

    // A weapon that names a family the hull hasn't got falls back to its gun
    // mounts rather than to the origin; one that names nothing at all still
    // leaves from a real point on the hull.
    Game game(fs);
    flecs::entity ship = game.GetEntitySpawner().SpawnPlayer("models/ships/fighter-1"_id, Vector2d{100., 50.});
    const PhysicsBody& phys = game.GetPhysicsSystem().GetBody(ship.get<PhysicsRef>());
    const Transform& transf = ship.get<Transform>();

    const Vector2d fromNose = ShipControlsSystem::ComputeBulletSpawn(transf, phys, 100., "weapon", 0).first;
    const Vector2d fromWing = ShipControlsSystem::ComputeBulletSpawn(transf, phys, 100., "weapon", 1).first;
    Require(fromNose != fromWing, "hardpoints: each mount fires from its own muzzle");
    Require(ShipControlsSystem::ComputeBulletSpawn(transf, phys, 100., "plasma", 0).first
                    == ShipControlsSystem::ComputeBulletSpawn(transf, phys, 100., "gun", 0).first,
            "hardpoints: an unmounted family falls back to the gun mount");

    // Per-mount arming: which mounts fire is the loadout's placement, not the
    // weapon's own family. A hull with a heavy mount on one wing and nothing in
    // the other two fires exactly one round per cycle, out of that wing.
    {
        ShipLoadout& armed = ship.get_mut<ShipLoadout>();
        armed.mounts = {};
        armed.mounts[1] = MountArm::Heavy;
        armed.levels.cannonTier = 1;
        armed.cannonAmmo = 20;

        const ShipStats armedStats = game.GetUpgradeCatalog().ResolveStats(armed.levels);
        Controls controls;
        ControlFlags firing{};
        firing.firePrimary = true;

        std::vector<unsigned> shotMounts;
        ShipControlsSystem::AdvancePrimary(controls, armedStats, &armed, *phys.body, firing,
                                           [&](const WeaponDef&, unsigned mount) {
            shotMounts.push_back(mount);
        });
        Require(shotMounts.size() == 1 && shotMounts[0] == 1,
                "controls: only the armed mount fires, and it fires from its own position");
        Require(armed.cannonAmmo == 19, "controls: a heavy round comes off the magazine");

        // The light guns are owned but mounted nowhere, so asking for them
        // leaves the ship silent -- fitting a line and mounting it are two
        // different things now.
        armed.mounts = {};
        armed.levels.gunTier = 1;
        Controls unarmed;
        std::size_t shots = 0;
        ShipControlsSystem::AdvancePrimary(unarmed, armedStats, &armed, *phys.body, firing,
                                           [&](const WeaponDef&, unsigned) { ++shots; });
        Require(shots == 0, "controls: a line owned but mounted nowhere fires nothing");
    }
    Require((fromNose - transf.pos).length() > 1. && (fromNose - transf.pos).length() < 100.,
            "hardpoints: the muzzle is offset from the hull's center but still on it");

    // The client-side geometry query and the sim's own segment query have to
    // agree about what a round met: a shot across the hull is stopped by the
    // authored bubble, one that misses the hull entirely by nothing.
    const std::optional<BodyHit> across =
            QueryBodySegment(*body, Vector2d{0., 0.}, 0., Vector2d{1., 1.},
                             Vector2d{-200., 0.}, Vector2d{200., 0.}, 2.);
    Require(across.has_value(), "body-query: a shot through the hull registers");
    Require(across->shieldElement && *across->shieldElement == SHIELD_BUBBLE_ELEMENT,
            "body-query: fighter-1's authored bubble is met before its hull");
    Require(across->point.x() < 0., "body-query: the near side is the one that stops it");

    Require(!QueryBodySegment(*body, Vector2d{0., 0.}, 0., Vector2d{1., 1.},
                              Vector2d{-200., 500.}, Vector2d{200., 500.}, 2.),
            "body-query: a shot that passes clear of the hull registers nothing");

    fs.Shutdown();
}

// ADR 0002's slot store must hand out references that survive further spawns.
// A ship with two armed mounts reads its own slot, fires, and reads it again
// within one tick (ShipControlsSystem::Update), so a store that relocated its
// slots as it grew would leave the second read on freed memory -- and it grows
// exactly when a faster or better-mounted gun puts more rounds in the air.
void TestPhysicsSlotStability()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    Game game(fs);

    flecs::entity ship = game.GetEntitySpawner().SpawnPlayer("models/ships/fighter-1"_id, Vector2d{0., 0.});
    const PhysicsRef ref = ship.get<PhysicsRef>();
    const PhysicsBody* slot = &game.GetPhysicsSystem().GetBody(ref);
    const Body* resource = slot->body.Get();
    const cpBody* cp = slot->cp.body.get();

    for (int i = 0; i < 256; ++i) {
        game.GetEntitySpawner().SpawnBullet("models/bullets/bullet-0"_id,
                                            Vector2d{double(i) * 4., 400.}, Vector2d{0., 1.});
    }

    Require(&game.GetPhysicsSystem().GetBody(ref) == slot,
            "physics: a body slot keeps its address while further bodies are spawned");
    Require(slot->body.Get() == resource && slot->cp.body.get() == cp,
            "physics: and a reference taken before those spawns still reads the same body");

    fs.Shutdown();
}

void TestShields()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    Game game(fs);
    EntitySpawner& spawner = game.GetEntitySpawner();

    flecs::entity target = spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{0., 0.}, TeamId::Blue);
    const UpgradeDef* bubble = game.GetUpgradeCatalog().FindKind(UpgradeKind::Shield, ShieldType::Bubble);
    Require(bubble != nullptr, "shields: the pool has a bubble shield");
    FitFree(game.GetUpgradeCatalog(), *bubble, 1, target.get_mut<ShipLoadout>());

    // Charges from empty at the emitter's rate.
    for (int tick = 0; tick < 600; ++tick) game.Update();
    const float charged = target.get<ShipLoadout>().shieldHp;
    Require(charged > 0.f, "shields: a fitted emitter charges up on its own");

    const float hullBefore = target.get<Damageable>().hp;
    flecs::entity bullet = spawner.SpawnBullet("models/bullets/bullet-0"_id, Vector2d{-200., 0.},
                                               Vector2d{400., 0.}, /*sensor=*/true);
    bullet.emplace<Bullet>(3.0, TeamId::Red, 20.f, 0u);
    for (int tick = 0; tick < 60 && bullet.is_alive(); ++tick) game.Update();

    Require(!bullet.is_alive(), "shields: the round is still consumed by the shielded ship");
    Require(target.get<ShipLoadout>().shieldHp < charged, "shields: the hit comes off the shield");
    Require(target.get<Damageable>().hp == hullBefore,
            "shields: a bubble hit the shield can cover never reaches the hull");

    // Plating is the other bargain: most rounds stopped whole, the rest
    // putting a share of themselves into the hull behind. That share is what
    // levels buy down -- so it is asserted on the resolved stats, while the
    // "some, not all" is asserted on a burst of real rounds below.
    const UpgradeDef* plating = game.GetUpgradeCatalog().FindKind(UpgradeKind::Shield, ShieldType::Plating);
    Require(plating != nullptr, "shields: the pool has field plating");
    float leakedBefore = 1.f;
    for (std::uint8_t level = 1; level <= plating->maxLevel; ++level) {
        UpgradeLevels levels;
        levels.shieldType = ShieldType::Plating;
        levels.shield = level;
        const ShipStats stats = game.GetUpgradeCatalog().ResolveStats(levels);
        Require(stats.shieldLeakChance > 0.f && stats.shieldLeakChance < 1.f,
                "shields: plating leaks by chance, neither never nor always");
        Require(stats.shieldLeakFraction < leakedBefore,
                "shields: each plating tier leaks less of the round that gets through");
        leakedBefore = stats.shieldLeakFraction;
    }
    Require(game.GetUpgradeCatalog().ResolveStats(target.get<ShipLoadout>().levels).shieldLeakChance == 0.f,
            "shields: a bubble never leaks while it has charge");

    // A burst into a plated hull, with the plates topped back up between
    // rounds -- one plate holds less than a heavy round, so left alone this
    // would only be measuring how fast a spent plate recharges. What each
    // round does to the hull says which branch it took: nothing at all
    // (stopped whole), or the leak fraction of it (through the plate).
    flecs::entity plated = spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{4000., 0.},
                                               TeamId::Blue);
    FitFree(game.GetUpgradeCatalog(), *plating, 1, plated.get_mut<ShipLoadout>());
    Require(IsPlated(plated.get<ShipLoadout>()), "shields: fighter-1 carries authored plates");

    // Under one plate's own charge, or the plate runs out first and both
    // branches land the same overflow on the hull.
    const float roundDamage = 8.f;
    const float leakFraction =
            game.GetUpgradeCatalog().ResolveStats(plated.get<ShipLoadout>().levels).shieldLeakFraction;
    int stopped = 0;
    int leaked = 0;
    const int rounds = 40;
    for (int shot = 0; shot < rounds; ++shot) {
        ShipLoadout& loadout = plated.get_mut<ShipLoadout>();
        loadout.plates.fill(1000.f);
        loadout.plateRegenDelay = {};
        loadout.shieldHp = 1000.f * static_cast<float>(loadout.plateCount);
        plated.get_mut<Damageable>().hp = 10000.f; // never dies mid-burst

        flecs::entity round = spawner.SpawnBullet("models/bullets/bullet-0"_id,
                                                  Vector2d{4000. - 200., 0.}, Vector2d{400., 0.},
                                                  /*sensor=*/true);
        round.emplace<Bullet>(3.0, TeamId::Red, roundDamage, 0u);
        for (int tick = 0; tick < 60 && round.is_alive(); ++tick) game.Update();

        const float toHull = 10000.f - plated.get<Damageable>().hp;
        if (toHull < 0.01f) ++stopped;
        else if (std::abs(toHull - roundDamage * leakFraction) < 0.01f) ++leaked;
    }
    Require(leaked > 0, "shields: some rounds get through plating");
    Require(stopped > 0, "shields: plating stops rounds whole rather than bleeding every one");
    Require(stopped + leaked == rounds,
            "shields: every round is either stopped whole or leaks exactly the tier's share");

    fs.Shutdown();
}

// A High Port deck is somewhere a pilot can stand and stay standing. The ring
// turns once a minute or so, and the deck used to be judged by the same
// legs-toward-the-body test a planet's surface is -- so a parked ship, whose
// attitude the kinematic station never carried round with it, drifted out of
// the upright cone within seconds and the yard closed under its feet.
void TestHighPortDeck()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    Game game(fs);
    EntitySpawner& spawner = game.GetEntitySpawner();

    flecs::entity planet =
            spawner.SpawnOrbitingPlanet("models/planets/simple"_id, Vector2d{0., 0.}, 1e-9, 800., 1.0, 0.0);
    planet.set<Team>(Team{TeamId::Blue});
    spawner.SpawnStructure(StructureType::Lab, "models/structures/lab"_id, planet, TeamId::Blue);
    spawner.SpawnOrbitingStructure(StructureType::HighPort, "models/structures/high-port-0"_id, planet,
                                   TeamId::Blue,
                                   StructureLayout::OrbitRadius(planet.get<Planet>().radius), 1.0, 0.0);
    game.Update(); // the station takes its orbital velocity on the first tick

    // Set down exactly where a launch puts a pilot: feet on the deck, matched
    // to the ring's motion.
    const std::optional<FactionSystem::SpawnPoint> pad =
            game.GetFactionSystem().SpawnPosition(TeamId::Blue);
    Require(pad.has_value(), "highport: the station is a place to launch from");
    flecs::entity ship = spawner.SpawnPlayer("models/ships/fighter-1"_id, pad->pos, TeamId::Blue,
                                             pad->vel, pad->rot);

    for (int tick = 0; tick < 600 && ship.is_alive() && !ship.get<ResearchAccess>().atLab; ++tick) {
        game.Update();
    }
    Require(ship.is_alive() && ship.get<ResearchAccess>().atLab,
            "highport: a ship settled on the deck is at the yard");

    // Long enough for the ring to carry it well past the arc any upright cone
    // would cover -- the whole of what used to close the yard again.
    int open = 0;
    const int watched = 2400; // 40s
    for (int tick = 0; tick < watched && ship.is_alive(); ++tick) {
        game.Update();
        if (ship.get<ResearchAccess>().atLab) ++open;
    }
    Require(ship.is_alive(), "highport: the parked ship survives the ride");
    Require(open >= watched - 60,
            "highport: ...and the yard stays open all the way round the ring");

    // A deck is a pad, not a hillside: which way the hull happens to be
    // pointing while it sits on one is not what decides whether the yard will
    // serve it.
    cpBody* shipBody = game.GetPhysicsSystem().GetBody(ship.get<PhysicsRef>()).cp.body.get();
    cpBodySetAngle(shipBody, cpBodyGetAngle(shipBody) + CP_PI / 2.0);
    for (int tick = 0; tick < 120 && ship.is_alive(); ++tick) game.Update(); // settle
    int openTipped = 0;
    for (int tick = 0; tick < 300 && ship.is_alive(); ++tick) {
        game.Update();
        if (ship.get<ResearchAccess>().atLab) ++openTipped;
    }
    Require(openTipped >= 250, "highport: a hull sitting across the deck is still at the yard");

    fs.Shutdown();
}

// Everything a pilot has to do about the station being solid. Its deck is one
// face of it, the outward one, and it rides a ring squarely over every radial
// path on the planet -- so the column beneath it is somewhere no climb can go
// and no descent can come from.
void TestHighPortApproach()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }

    // A Blue home planet with the full complex on it, plus wherever its
    // station has got to on the first tick.
    struct Complex {
        flecs::entity planet;
        flecs::entity port;
        Vector2d center;
        double radius = 0.;
    };
    const auto build = [](Game& game) {
        EntitySpawner& spawner = game.GetEntitySpawner();
        Complex c;
        c.planet = spawner.SpawnOrbitingPlanet("models/planets/simple"_id, Vector2d{0., 0.}, 1e-9,
                                               800., 1.0, 0.0);
        BuildStartingComplex(spawner, c.planet, TeamId::Blue);
        game.Update();
        game.GetRegistry().each([&](flecs::entity ent, const Structure& s, const PlanetOrbitAttachment&) {
            if (s.type == StructureType::HighPort && !c.port.is_alive()) c.port = ent;
        });
        c.center = c.planet.get<Transform>().pos;
        c.radius = c.planet.get<Planet>().radius * c.planet.get<Transform>().scale.x();
        return c;
    };

    // Leaving: a pilot standing on the surface directly under the station,
    // with business elsewhere. The departure climb is radial and the station
    // is on it, so this used to end with the hull pressed against the deck's
    // underside burning outward for the rest of the match.
    {
        Game game(fs);
        Complex c = build(game);
        const Vector2d up = (c.port.get<Transform>().pos - c.center).normalized();

        flecs::entity ship = game.GetEntitySpawner().SpawnAIShip(
                "models/ships/fighter-1"_id, c.center + up * (c.radius + 13.),
                game.GetAIPresets().Default(), Vector2d{}, std::atan2(up.x(), -up.y()), TeamId::Blue);
        game.GetEntitySpawner().SpawnPlayer("models/ships/fighter-1"_id,
                                            c.center + Vector2d{40000., 0.}, TeamId::Red);

        double best = 0.;
        for (int tick = 0; tick < 3600 && ship.is_alive(); ++tick) {
            game.Update();
            if (!ship.is_alive()) break;
            best = std::max(best, (ship.get<Transform>().pos - c.center).length());
        }
        Require(ship.is_alive(), "highport: a pilot lifting off under the station survives it");
        Require(best > c.radius + 260.,
                "highport: ...and climbs clear of the complex instead of pinning itself "
                "against the station's underside");
    }

    // Arriving from inside the ring, which is where a pilot that has just
    // taken off from the surface -- or been pushed there -- starts from. The
    // deck is above and behind it, and the way to it is out of the column
    // first.
    {
        Game game(fs);
        Complex c = build(game);
        const Vector2d up = (c.port.get<Transform>().pos - c.center).normalized();

        flecs::entity ship = game.GetEntitySpawner().SpawnAIShip(
                "models/ships/fighter-1"_id, c.center + up * (c.radius + 50.),
                game.GetAIPresets().Default(), Vector2d{}, 0.0, TeamId::Blue);
        Damageable& hull = ship.get_mut<Damageable>();
        hull.hp = hull.maxHp * 0.1f; // hurt: home is the nearest yard, and that is the port

        bool served = false;
        for (int tick = 0; tick < 5400 && ship.is_alive() && !served; ++tick) {
            game.Update();
            served = ship.is_alive() && ship.get<ResearchAccess>().atLab;
        }
        Require(ship.is_alive(), "highport: a pilot sent to the deck from inside the ring survives");
        Require(served,
                "highport: ...and gets its feet on the deck, rather than wedging under the "
                "station between it and the planet");
    }

    // The same trip flown from outside, across the ring's own bearing: the
    // approach that already worked, and has to keep working.
    {
        Game game(fs);
        Complex c = build(game);
        const Vector2d out = (c.port.get<Transform>().pos - c.center).normalized();
        const Vector2d across{-out.y(), out.x()};

        flecs::entity ship = game.GetEntitySpawner().SpawnAIShip(
                "models/ships/fighter-1"_id, c.center + across * 1500., game.GetAIPresets().Default(),
                Vector2d{}, 0.0, TeamId::Blue);
        Damageable& hull = ship.get_mut<Damageable>();
        hull.hp = hull.maxHp * 0.1f;

        bool served = false;
        for (int tick = 0; tick < 5400 && ship.is_alive() && !served; ++tick) {
            game.Update();
            served = ship.is_alive() && ship.get<ResearchAccess>().atLab;
        }
        Require(ship.is_alive(), "highport: a pilot crossing to the deck from outside survives");
        Require(served, "highport: ...and sets down on it");
    }

    fs.Shutdown();
}

void TestResearch()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    Game game(fs);
    EntitySpawner& spawner = game.GetEntitySpawner();

    flecs::entity planet =
            spawner.SpawnOrbitingPlanet("models/planets/simple"_id, Vector2d{0., 0.}, 1e-9, 800., 1.0, 0.0);
    planet.set<Team>(Team{TeamId::Blue});
    flecs::entity lab = spawner.SpawnStructure(StructureType::Lab, "models/structures/lab"_id, planet,
                                               TeamId::Blue);

    const auto research = [&] {
        FactionState found{};
        game.GetRegistry().each([&](const FactionState& fs2) {
            if (fs2.team == TeamId::Blue) found = fs2;
        });
        return found;
    };

    // One lab: a full research period's worth of ticks, no sooner.
    const int soloTicks =
            static_cast<int>(game.GetEconomyConfig().research.secondsPerTech / Game::PHYSICS_DELTA);
    for (int tick = 0; tick < soloTicks - 1; ++tick) game.Update();
    Require(research().techPoints == 0, "research: one lab pays out nothing before its period");
    Require(lab.get<Structure>().researchProgress > 0.9f,
            "research: the lab mirrors its faction's progress for replication");
    for (int tick = 0; tick < 5 && research().techPoints == 0; ++tick) game.Update();
    Require(research().techPoints == static_cast<std::uint32_t>(game.GetEconomyConfig().research.techPerFill),
            "research: a filled bar pays tech_per_fill into the faction's pool");

    // Nothing idles the labs. What grows is not necessarily the pool, though:
    // a side with nobody reading the tree commits its own Tech, which is the
    // only thing stopping an AI faction flying stock hulls all match.
    for (int tick = 0; tick < soloTicks + 5; ++tick) game.Update();
    Require(AnyRankUnlocked(research().unlocked),
            "research: a side with no human pilot researches on its own");

    // Landing: same setup TestLandingAndClaiming uses to get a ship down
    // gently, on the planet the lab sits on.
    const float planetRadius = planet.get<Planet>().radius
            * static_cast<float>(planet.get<Transform>().scale.x());
    flecs::entity ship = spawner.SpawnPlayer("models/ships/fighter-1"_id,
                                             Vector2d{800., planetRadius + 15.}, TeamId::Blue);
    cpBody* shipBody = game.GetPhysicsSystem().GetBody(ship.get<PhysicsRef>()).cp.body.get();
    cpBodySetAngle(shipBody, CP_PI);
    cpBodySetVelocity(shipBody, cpv(0., -8.));

    const auto pressPick = [&](const UpgradeDef& def, TechTab tab, std::uint8_t rank) {
        InputCommand cmd;
        cmd.tick = game.GetStep();
        cmd.techPick = TechPick{def.id, tab, rank};
        ship.get_mut<InputQueue>().Push(cmd);
    };
    const auto supplies = [&] {
        const PilotRef& ref = ship.get<PilotRef>();
        std::uint32_t found = 0;
        game.GetRegistry().each([&](const PilotAccount& account) {
            if (account.pilotId == ref.pilotId) found = account.supplies;
        });
        return found;
    };

    for (int tick = 0; tick < 900 && ship.is_alive() && !ship.get<ResearchAccess>().atLab; ++tick) {
        game.Update();
    }
    Require(ship.is_alive(), "research: the landing ship survives touchdown");
    Require(ship.get<ResearchAccess>().atLab,
            "research: landing at the lab's planet opens the yard to this ship");
    // Supplies accrue slowly enough that a touchdown this quick banks nothing
    // yet, so this is measured over a stretch rather than at the moment of
    // landing.
    const std::uint32_t supplyBefore = supplies();
    for (int tick = 0; tick < 300; ++tick) game.Update();
    Require(supplies() > supplyBefore,
            "research: a pilot accrues Supplies simply for being out there");

    // From here the side has a human pilot, so it stops researching for
    // itself -- but it has been at it for two periods already. Wound back so
    // the assertions below are about what this pilot buys rather than about
    // what the AI got round to first.
    game.GetRegistry().each([&](FactionState& fs2) {
        if (fs2.team != TeamId::Blue) return;
        fs2.unlocked = {};
        fs2.techPoints = 1000;
    });
    // And a purse deep enough that nothing below is refused for being
    // unaffordable -- what is under test here is the two gates, not the price.
    {
        const PilotRef& ref = ship.get<PilotRef>();
        game.GetRegistry().each([&](PilotAccount& account) {
            if (account.pilotId == ref.pilotId) account.supplies = 1000;
        });
    }

    // The PERMANENT tree commits from anywhere, so it is exercised first --
    // and nothing in the SHIP tree can be bought until it has.
    const UpgradeDef* bay = game.GetUpgradeCatalog().FindKind(UpgradeKind::MissileTier);
    Require(bay != nullptr, "research: the pool has a missile bay");

    for (int tick = 0; tick < 10; ++tick) {
        pressPick(*bay, TechTab::Ship, 1);
        game.Update();
    }
    Require(ship.get<ShipLoadout>().levels.missileTier == 0,
            "research: a hull cannot fit what its faction has not learned");

    for (int tick = 0; tick < 10 && research().unlocked.rank[game.GetUpgradeCatalog().IndexOf(bay->id)] == 0;
         ++tick) {
        pressPick(*bay, TechTab::Permanent, 1);
        game.Update();
    }
    Require(research().unlocked.rank[game.GetUpgradeCatalog().IndexOf(bay->id)] == 1,
            "research: the PERMANENT tree spends Tech to unlock a rank for the side");

    // Now the same purchase lands, and costs Supplies rather than Tech.
    const std::uint32_t purseBefore = supplies();
    std::uint32_t collectedSeq = 0;
    for (int tick = 0; tick < 60 && ship.is_alive() && !collectedSeq; ++tick) {
        pressPick(*bay, TechTab::Ship, 1);
        game.Update();
        game.GetEventQueue().ConsumeSince(0, [&](const GameEvent& event) {
            if (event.type == GameEventType::UpgradeCollected) collectedSeq = event.seq;
        });
    }
    Require(collectedSeq != 0, "research: fitting a rank emits UpgradeCollected");
    Require(ship.get<ShipLoadout>().levels.missileTier == 1,
            "research: ...and the hull is carrying it");
    Require(supplies() < purseBefore, "research: fitting a rank spends the pilot's own Supplies");

    // Leaving the pad closes the yard, and the tree cannot be bought from
    // while it is shut -- but Tech still commits, since learning a part needs
    // no landing at all.
    cpBodySetVelocity(shipBody, cpv(0., 30.));
    bool leftThePad = false;
    for (int tick = 0; tick < 120 && !leftThePad; ++tick) {
        game.Update();
        leftThePad = !ship.get<ResearchAccess>().atLab;
    }
    Require(leftThePad, "research: leaving the pad closes the yard");

    const std::uint8_t rankInFlight = 2;
    for (int tick = 0; tick < 10; ++tick) {
        pressPick(*bay, TechTab::Permanent, rankInFlight);
        game.Update();
    }
    Require(research().unlocked.rank[game.GetUpgradeCatalog().IndexOf(bay->id)] == rankInFlight,
            "research: the PERMANENT tree commits in flight");

    // Just off the pad, the yard is still serving: the commonest way to be
    // refused is drifting a metre out of tolerance during the round trip
    // between the click and the server hearing it, and that is latency to
    // absorb rather than a refusal to report (REFIT_GRACE_TICKS).
    for (int tick = 0; tick < 5; ++tick) {
        pressPick(*bay, TechTab::Ship, rankInFlight);
        game.Update();
    }
    Require(ship.get<ShipLoadout>().levels.missileTier == rankInFlight,
            "research: a purchase moments off the pad is still honoured");

    // Long gone, though, and it is shut. Put the ship somewhere it cannot
    // simply fall back onto the pad -- left to itself it settles again within
    // a few seconds, which is the behaviour the grace window is built on.
    const std::uint8_t rankAway = 3;
    for (int tick = 0; tick < 10; ++tick) {
        pressPick(*bay, TechTab::Permanent, rankAway);
        game.Update();
    }
    cpBodySetPosition(shipBody, cpv(40000., 40000.));
    cpBodySetVelocity(shipBody, cpv(0., 0.));
    for (int tick = 0; tick < 90; ++tick) game.Update();
    Require(!ship.get<ResearchAccess>().atLab, "research: the yard is shut out here (setup check)");

    for (int tick = 0; tick < 10; ++tick) {
        pressPick(*bay, TechTab::Ship, rankAway);
        game.Update();
    }
    Require(ship.get<ShipLoadout>().levels.missileTier == rankInFlight,
            "research: the SHIP tree shuts once the ship is really away");

    // Second lab: the pooled bar fills twice as fast.
    const std::uint32_t techBefore = research().techPoints;
    spawner.SpawnStructure(StructureType::Lab, "models/structures/lab"_id, planet, TeamId::Blue);
    int secondCycleTicks = 0;
    for (int tick = 0; tick < soloTicks && research().techPoints == techBefore; ++tick) {
        game.Update();
        ++secondCycleTicks;
    }
    Require(research().techPoints > techBefore,
            "research: a second payout comes round");
    Require(secondCycleTicks < soloTicks * 3 / 4, "research: two labs research faster than one");

    // Missiles from here on, whether or not the draft happened to offer them:
    // the bay is what fits a launcher at all, so it goes on before the rounds.
    UpgradeLevels missileLevels;
    missileLevels.missileTier = 1;
    ship.get_mut<ShipLoadout>().levels.missileTier = 1;
    SetMissileBay(ship.get_mut<ShipLoadout>(), 0, true); // the tube the rack fires through
    ship.get_mut<ShipLoadout>().missileAmmo =
            static_cast<std::uint8_t>(game.GetUpgradeCatalog().ResolveStats(missileLevels).missileCapacity);

    // Firing spends one round per missile cooldown, not one per tick.
    const auto liveMissiles = [&] {
        int count = 0;
        const WeaponDef* round = game.GetUpgradeCatalog().ResolveStats(missileLevels).missile;
        game.GetRegistry().each([&](const Bullet& b) {
            if (round && b.damage == round->damage) ++count;
        });
        return count;
    };
    // A hostile off to the side of where the missile is pointed (the landed
    // ship's nose is straight up, away from the planet), so guidance has to
    // turn it rather than just fly straight into a target already ahead.
    flecs::entity enemy = spawner.SpawnPlayer("models/ships/fighter-1"_id,
                                              Vector2d{800. + 500., planetRadius + 250.}, TeamId::Red);

    const int ammoBeforeFiring = ship.get<ShipLoadout>().missileAmmo;
    for (int tick = 0; tick < 3; ++tick) {
        InputCommand cmd;
        cmd.tick = game.GetStep();
        cmd.flags.fireMissile = true;
        ship.get_mut<InputQueue>().Push(cmd);
        game.Update();
    }
    Require(ship.get<ShipLoadout>().missileAmmo == ammoBeforeFiring - 1,
            "research: three ticks of held fire spends exactly one missile");
    Require(liveMissiles() == 1, "research: firing put a missile in the world");

    // Guidance: this exact missile locks the nearest hostile and closes the
    // angle to it. Tracked by entity, not by "whichever missile is alive" --
    // an empty InputQueue repeats the last command, so fire has to be
    // released below or the rack keeps launching replacements.
    flecs::entity firedMissile;
    game.GetRegistry().each([&](flecs::entity e, const Missile&) { firedMissile = e; });
    Require(firedMissile.is_alive(), "research: the fired missile is trackable");

    const auto angleToEnemy = [&] {
        const Transform& transf = firedMissile.get<Transform>();
        const Vector2d toTarget = enemy.get<Transform>().pos - transf.pos;
        const double speed = transf.vel.length();
        if (toTarget.length() < 1e-6 || speed < 1e-6) return 0.0;
        return std::acos(std::clamp(Magnum::Math::dot(transf.vel / speed, toTarget.normalized()), -1.0, 1.0));
    };

    Require(firedMissile.get<Missile>().targetNetId == enemy.get<NetId>().value,
            "research: the missile locked the nearest hostile ship");
    const double launchAngle = angleToEnemy();
    Require(launchAngle > 0.3, "research: the target starts well off the missile's nose (setup check)");

    for (int tick = 0; tick < 60 && firedMissile.is_alive(); ++tick) {
        InputCommand cmd;
        cmd.tick = game.GetStep();
        ship.get_mut<InputQueue>().Push(cmd); // fire released
        game.Update();
    }
    Require(firedMissile.is_alive(), "research: the missile is still flying a second later");
    Require(angleToEnemy() < launchAngle * 0.5, "research: guidance turned the missile toward its target");

    fs.Shutdown();
}

// Nothing idles a lab. A faction whose pilots never come home keeps earning
// for the whole match -- what a stock airframe costs them is the pressure to
// land, not a bar that stops.
void TestResearchQueue()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    Game game(fs);
    EntitySpawner& spawner = game.GetEntitySpawner();

    flecs::entity planet =
            spawner.SpawnOrbitingPlanet("models/planets/simple"_id, Vector2d{0., 0.}, 1e-9, 800., 1.0, 0.0);
    planet.set<Team>(Team{TeamId::Blue});
    spawner.SpawnStructure(StructureType::Lab, "models/structures/lab"_id, planet, TeamId::Blue);

    const auto research = [&] {
        FactionState found{};
        game.GetRegistry().each([&](const FactionState& fs2) {
            if (fs2.team == TeamId::Blue) found = fs2;
        });
        return found;
    };

    const int soloTicks =
            static_cast<int>(game.GetEconomyConfig().research.secondsPerTech / Game::PHYSICS_DELTA);

    // What the side is worth: banked Tech plus everything it has already spent
    // on learning. Measured together because a side with nobody reading the
    // tree commits its own points, so the pool alone can sit flat while the
    // labs are working perfectly well.
    const auto earned = [&] {
        // Named, not walked straight off the call: research() returns by
        // value, and iterating an array member of that temporary reads it
        // after it has gone (caught by ASan).
        const FactionState state = research();
        int total = static_cast<int>(state.techPoints);
        for (const std::uint8_t rank : state.unlocked.rank) total += rank;
        return total;
    };

    for (int tick = 0; tick < soloTicks * 2 + 10; ++tick) game.Update();
    const int early = earned();
    Require(early > 0, "research: a lab with nobody home still pays out");

    for (int tick = 0; tick < soloTicks * 2 + 10; ++tick) game.Update();
    Require(earned() > early, "research: and keeps paying out -- nothing caps it, nothing idles it");

    fs.Shutdown();
}

// docs/sector-generation-plan.md S1/S2: generation is a pure function of the
// seed, respects its bounds, and hands every faction a home of its own.
void TestSectorGeneration()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }

    // Star/planet positions and the home assignment, in spawn order -- the
    // whole layout as a comparable value.
    const auto describe = [](Game& game) {
        std::vector<double> out;
        game.GetRegistry().each([&](const Transform& t, const Planet&) {
            out.push_back(t.pos.x());
            out.push_back(t.pos.y());
        });
        std::sort(out.begin(), out.end());
        return out;
    };

    SectorParams params;
    params.seed = 12345;
    params.factionCount = 4;
    params.stars = 5;

    Game first(fs);
    first.BuildWorld(params);
    const std::vector<double> firstLayout = describe(first);

    Game again(fs);
    again.BuildWorld(params);
    Require(describe(again) == firstLayout, "sector: the same seed builds the same layout");

    Game other(fs);
    SectorParams otherParams = params;
    otherParams.seed = 999;
    other.BuildWorld(otherParams);
    Require(describe(other) != firstLayout, "sector: a different seed builds a different layout");

    // Bounds: `stars` suns, and every sun carrying between min and max
    // planets. Suns are the celestials without an Orbit.
    std::size_t stars = 0, planets = 0;
    ankerl::unordered_dense::map<double, int> perStar;
    first.GetRegistry().each([&](flecs::entity entity, const Transform& t, const Planet&) {
        if (const Orbit* orbit = entity.try_get<Orbit>()) {
            ++planets;
            // Orbit centers are exact copies of the star position they were
            // generated from, so this groups planets by their sun without
            // needing a parent reference.
            ++perStar[orbit->center.x()];
        }
        else {
            ++stars;
            (void)t;
        }
    });
    Require(stars == static_cast<std::size_t>(params.stars), "sector: star count matches the parameter");
    Require(perStar.size() == stars, "sector: every star carries at least one planet");
    for (const auto& [centerX, count] : perStar) {
        (void)centerX;
        Require(count >= params.minPlanetsPerStar && count <= params.maxPlanetsPerStar,
                "sector: per-star planet count is within bounds");
    }
    Require(planets >= perStar.size(), "sector: planets were generated");

    // One home per faction, each with its own developed complex, no two on
    // the same sun.
    Require(first.GetRoster().size() == static_cast<std::size_t>(params.factionCount),
            "sector: the roster fields one side per faction");

    ankerl::unordered_dense::set<double> homeStars;
    std::size_t homes = 0;
    for (TeamId team : first.GetRoster()) {
        flecs::entity home;
        first.GetRegistry().each([&](flecs::entity entity, const Team& t, const Planet&, const Orbit& orbit) {
            if (t.id != team || home.is_alive()) return;
            home = entity;
            homeStars.insert(orbit.center.x());
        });
        Require(home.is_alive(), "sector: every faction owns a home planet");
        ++homes;

        // BuildStartingComplex hands out all five structure types; finding
        // the Base is enough to prove the complex went up at this team's home
        // rather than the roster merely naming a side.
        bool hasBase = false;
        first.GetRegistry().each([&](const Structure& s, const Team& t) {
            if (s.type == StructureType::Base && t.id == team) hasBase = true;
        });
        Require(hasBase, "sector: every faction's home carries a starting complex");
    }
    Require(homeStars.size() == homes, "sector: no two factions start at the same sun");
    Require(stars - homeStars.size() >= static_cast<std::size_t>(SectorParams::MIN_FREE_STARS),
            "sector: at least one sun is left unclaimed");

    Require(first.GetSectorExtent() > 0., "sector: a real extent is reported for the minimap");

    // Reseeding from the round-setup screen (S6): the old world goes away
    // entirely and the new one is indistinguishable from a freshly built one.
    // Rebuilt on `other`'s params so it can be compared against a Game that
    // only ever knew them.
    first.RebuildWorld(otherParams);
    Require(describe(first) == describe(other),
            "sector: a rebuilt world matches one built from those params outright");

    std::size_t leftovers = 0;
    first.GetRegistry().each([&](const FactionState&) { ++leftovers; });
    Require(leftovers == 0, "sector: the old world's faction bookkeeping is gone after a rebuild");

    // The physics bodies behind the destroyed entities are freed by an
    // OnRemove observer; ticking proves the space is still coherent rather
    // than holding shapes whose entities have gone.
    first.SpawnCombatants(first.GetRoster().front());
    for (int i = 0; i < 120; ++i) first.Update();
    Require(first.GetSectorExtent() > 0., "sector: a rebuilt world still ticks");

    // Every combatant starts at a site, not out in the void: within one
    // spawn offset of the sector extent. A home can be the outermost body
    // there is, so a spawn genuinely does sit at the extent -- what this
    // pins down is that it is never somewhere else entirely (the minimap's
    // own margin is what keeps the marker on the map). Swept over seeds
    // because this is a property of a particular layout, not of the code
    // path any one seed exercises.
    for (std::uint32_t seed = 1; seed <= 128; ++seed) {
        SectorParams sweep;
        sweep.seed = seed;

        Game game(fs);
        game.BuildWorld(sweep);
        game.SpawnCombatants(game.GetRoster().front());

        // Stated separately because the extent check below cannot see it: a
        // faction with no site spawns at the world origin, whose length is
        // zero and so inside any extent this could assert.
        for (const TeamId team : game.GetRoster()) {
            Require(game.GetFactionSystem().SpawnPosition(team).has_value(),
                    "sector: every faction has somewhere to launch from");
        }

        const double extent = game.GetSectorExtent();
        game.GetRegistry().each([&](flecs::entity entity, const Transform& t, const Team&, const Damageable&) {
            if (entity.has<Planet>() || entity.has<Structure>()) return;
            Require(t.pos.length() <= extent + FactionSystem::RESPAWN_OFFSET_RADIUS,
                    "sector: every combatant spawns at a site inside the sector");
        });
    }

    fs.Shutdown();
}

// Phase 4's defeat/win rules (FactionSystem): a faction with zero colonies
// AND zero freighters is defeated; a round is won once every claimed planet
// belongs to one team. Both are sticky, edge-triggered GameEvents.
void TestFactionDefeatAndWin()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    Game game(fs);
    EntitySpawner& spawner = game.GetEntitySpawner();

    // Both planets Blue-claimed from the start -- every claimed planet
    // already belongs to one team, so RoundOver should fire almost
    // immediately without needing any in-sim claiming.
    flecs::entity planetA =
            spawner.SpawnOrbitingPlanet("models/planets/simple"_id, Vector2d{0., 0.}, 1e-9, 2000., 1.0, 0.0);
    BuildStartingComplex(spawner, planetA, TeamId::Blue);
    flecs::entity planetB =
            spawner.SpawnOrbitingPlanet("models/planets/simple"_id, Vector2d{1600., 0.}, 1e-9, 2000., 1.0, 0.0);
    BuildStartingComplex(spawner, planetB, TeamId::Blue);

    std::uint32_t cursor = 0;
    bool sawRoundOver = false;
    TeamId roundWinner = TeamId::None;
    for (int tick = 0; tick < 10; ++tick) {
        game.Update();
        cursor = game.GetEventQueue().ConsumeSince(cursor, [&](const GameEvent& event) {
            if (event.type == GameEventType::RoundOver) {
                sawRoundOver = true;
                roundWinner = static_cast<TeamId>(event.param);
            }
        });
    }
    Require(sawRoundOver, "faction: RoundOver fires once every claimed planet belongs to one team");
    Require(roundWinner == TeamId::Blue, "faction: RoundOver reports the correct winning team");

    // Destroy every Colony Blue owns (both planets') -- Blue has no
    // freighters in flight either (none were dispatched in this short,
    // ungifted window), so this should trip the defeat rule.
    std::vector<flecs::entity> colonies;
    game.GetRegistry().each([&](flecs::entity e, const Structure& s, const Team& t) {
        if (s.type == StructureType::Colony && t.id == TeamId::Blue) colonies.push_back(e);
    });
    Require(!colonies.empty(), "faction: setup check -- Blue has at least one Colony to destroy");
    for (flecs::entity colony : colonies) colony.destruct();

    bool sawDefeated = false;
    for (int tick = 0; tick < 10; ++tick) {
        game.Update();
        cursor = game.GetEventQueue().ConsumeSince(cursor, [&](const GameEvent& event) {
            if (event.type == GameEventType::FactionDefeated && static_cast<TeamId>(event.param) == TeamId::Blue) {
                sawDefeated = true;
            }
        });
    }
    Require(sawDefeated, "faction: FactionDefeated fires once a team has zero colonies and zero freighters");

    fs.Shutdown();
}

// A hull that can land has to be able to leave again: thrust is per-hull
// (Body::GetThrust) precisely so it can be held above the surface gravity of
// anything landable, and the AI's departure climb and its strategy layer both
// depend on that holding.
// A star is not a landing site at any approach speed, and nothing that
// reaches one comes back -- unlike a planet, where a gentle enough touchdown
// is the whole point.
void TestSunIsLethal()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }

    Game game(fs);
    EntitySpawner& spawner = game.GetEntitySpawner();

    flecs::entity sun = spawner.SpawnStar("models/stars/sun"_id, Vector2d{0., 0.});
    Require(sun.get<Planet>().star, "sun: a spawned star is marked as one");

    const double radius = sun.get<Planet>().radius * sun.get<Transform>().scale.x();
    Require(radius > 0., "sun: the star has a real radius (setup check)");

    // The gentlest possible arrival -- the descent a planet would reward with
    // a clean landing. Started just clear of the surface, drifting in.
    flecs::entity ship = spawner.SpawnPlayer("models/ships/fighter-1"_id,
                                             Vector2d{0., radius + 40.}, TeamId::Blue);
    cpBody* body = game.GetPhysicsSystem().GetBody(ship.get<PhysicsRef>()).cp.body.get();
    cpBodySetAngle(body, CP_PI); // legs down, as if setting down on a planet
    cpBodySetVelocity(body, cpv(0., -4.));

    DamageCause cause = DamageCause::Unknown;
    game.OnDeath().connect([&cause](const DeathReport& report) { cause = report.cause; });

    for (int tick = 0; tick < 600 && ship.is_alive(); ++tick) game.Update();

    Require(!ship.is_alive(), "sun: touching a star destroys the ship, however gently it arrives");
    Require(cause == DamageCause::Star, "sun: the kill feed blames the star, not a hard landing");

    fs.Shutdown();
}

// The corona. Held at a fixed radius rather than dropped, so what is measured
// is the heat and not how long the fall takes.
//
// The 1.03R case is the one this file used to miss: a hull's own collision
// shape holds its centre a ship-radius off the surface, which on a 320-unit
// star is further out than STAR_LETHAL_MARGIN reaches -- so a ship that came
// to rest on a sun sat just outside the contact check and cooked forever
// without dying. The test above passed throughout, because an arrival at
// speed penetrates far enough to cross the boundary and a resting one never
// does.
void TestSunHeat()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }

    struct Result {
        bool alive = false;
        double seconds = 0.;
        DamageCause cause = DamageCause::Unknown;
        float shieldLeft = 0.f;
    };

    const auto heldAt = [&](double factor, bool shielded) {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();

        flecs::entity sun = spawner.SpawnStar("models/stars/sun"_id, Vector2d{0., 0.});
        const double radius = sun.get<Planet>().radius * sun.get<Transform>().scale.x();

        flecs::entity ship = spawner.SpawnPlayer("models/ships/fighter-1"_id,
                                                 Vector2d{0., radius * factor}, TeamId::Blue);
        cpBody* body = game.GetPhysicsSystem().GetBody(ship.get<PhysicsRef>()).cp.body.get();

        if (shielded) {
            const UpgradeDef* bubble =
                    game.GetUpgradeCatalog().FindKind(UpgradeKind::Shield, ShieldType::Bubble);
            Require(bubble != nullptr, "sun heat: the pool has a bubble shield (setup check)");
            FitFree(game.GetUpgradeCatalog(), *bubble, 1, ship.get_mut<ShipLoadout>());
            ShipLoadout& loadout = ship.get_mut<ShipLoadout>();
            loadout.shieldHp = game.GetUpgradeCatalog().ResolveStats(loadout.levels).shieldCapacity;
            Require(loadout.shieldHp > 0.f, "sun heat: the bubble starts charged (setup check)");
        }

        Result result;
        game.OnDeath().connect([&result](const DeathReport& r) { result.cause = r.cause; });

        const cpVect held = cpv(0., radius * factor);
        int tick = 0;
        for (; tick < 1800 && ship.is_alive(); ++tick) {
            cpBodySetPosition(body, held);
            cpBodySetVelocity(body, cpvzero);
            game.Update();
        }

        result.alive = ship.is_alive();
        result.seconds = tick / 60.0;
        if (result.alive) result.shieldLeft = ship.get<ShipLoadout>().shieldHp;
        return result;
    };

    // Resting on the surface: the reported bug. 1.03R is where a fighter's
    // own hull holds it, just outside the contact boundary.
    const Result resting = heldAt(1.03, /*shielded=*/false);
    Require(!resting.alive, "sun heat: a ship resting on a star's surface burns up");
    Require(resting.cause == DamageCause::Star, "sun heat: and the star gets the kill-feed line");
    Require(resting.seconds < 5.0, "sun heat: quickly enough that it is not somewhere to park");

    // A shield is worth carrying into a corona, and worth exactly as much as
    // it holds -- it delays, it does not save.
    const Result shielded = heldAt(1.03, /*shielded=*/true);
    Require(!shielded.alive, "sun heat: a shield does not make a star survivable");
    Require(shielded.seconds > resting.seconds,
            "sun heat: but it absorbs the heat until it is gone, so the hull lasts longer");

    // Falls off with distance: a pass through the outer corona costs, and
    // outside it there is nothing to pay.
    const Result grazing = heldAt(2.0, /*shielded=*/false);
    Require(grazing.seconds > resting.seconds * 3.0,
            "sun heat: the outer corona is a cost, not a death sentence");

    const Result clear = heldAt(2.6, /*shielded=*/false); // outside STAR_HEAT_REACH
    Require(clear.alive, "sun heat: beyond the corona a star does nothing at all");

    fs.Shutdown();
}

// The chat cheats (CheatConsole). Everyone can run them and nothing gates
// them, so what's worth proving is that each one actually reaches the sim --
// they run server-side in multiplayer, where a client can't check for itself.
void TestCheats()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }

    Game game(fs);
    game.BuildClassicWorld();
    game.SpawnCombatants(TeamId::Blue);
    game.SettleScenario();

    const flecs::entity ship = *game.GetPlayer();
    const auto run = [&](const char* text) {
        return RunCheatCommand(game, ship, TeamId::Blue, text);
    };

    Require(IsCheatCommand("/god"), "cheat: a leading slash addresses the console");
    Require(!IsCheatCommand("god"), "cheat: ordinary chat is not a command");
    Require(!run("/nonsense").reply.empty(), "cheat: an unknown command still answers");

    // Tech: straight into the faction's pool, which is what a lab would have
    // paid in.
    run("/tech 3");
    std::uint32_t tech = 0;
    game.GetRegistry().each([&](const FactionState& faction) {
        if (faction.team == TeamId::Blue) tech = faction.techPoints;
    });
    Require(tech == 3, "cheat: /tech pays into the faction's pool");

    // Upgrades land on the hull directly, bypassing both currencies and the
    // faction gate entirely.
    run("/upgrade bubble_shield");
    Require(ship.get<ShipLoadout>().levels.shieldType == ShieldType::Bubble,
            "cheat: /upgrade fits a named upgrade straight onto the ship");
    run("/upgrade missile_bay");
    run("/ammo");
    Require(ship.get<ShipLoadout>().missileAmmo > 0, "cheat: /ammo fills the rack it just fitted");

    // Heal has to put the shield back too, not only the hull.
    ship.get_mut<Damageable>().hp = 1.f;
    ship.get_mut<ShipLoadout>().shieldHp = 0.f;
    run("/heal");
    Require(ship.get<Damageable>().hp == ship.get<Damageable>().maxHp,
            "cheat: /heal restores the hull");
    Require(ship.get<ShipLoadout>().shieldHp > 0.f, "cheat: /heal recharges the shield");

    // God: the hull still takes the damage, DeathSystem just refuses the
    // conclusion -- so a hp of zero survives a tick.
    run("/god");
    ship.get_mut<Damageable>().hp = 0.f;
    game.Update();
    Require(ship.is_alive(), "cheat: /god survives a hull driven to zero");
    Require(ship.get<Damageable>().hp > 0.f, "cheat: /god puts the hull back");

    // ...and /kill has to get through it, or a cheated ship could never let go.
    run("/kill");
    game.Update();
    Require(!ship.is_alive(), "cheat: /kill overrides /god");

    // Teleport works on a fresh ship (the old one just died).
    const flecs::entity flown = game.GetEntitySpawner().SpawnPlayer(
            "models/ships/fighter-1"_id, Vector2d{0., 0.}, TeamId::Blue);
    RunCheatCommand(game, flown, TeamId::Blue, "/tp 4321 -1234");
    game.Update();
    Require(std::abs(flown.get<Transform>().pos.x() - 4321.) < 50.
                    && std::abs(flown.get<Transform>().pos.y() + 1234.) < 50.,
            "cheat: /tp moves the ship's physics body, not only its Transform");

    // Callsigns: a cheat aimed with @name lands on that player's ship, and
    // /tp <name> takes you to them at their own velocity. This is the whole
    // multiplayer story -- the server resolves names against Callsign, so the
    // name has to survive onto a fresh hull too.
    game.SetPlayerName("  Ace  ");
    Require(game.GetPlayerName() == "Ace", "callsign: a typed name is trimmed");
    const flecs::entity mine = game.GetEntitySpawner().SpawnPlayer(
            "models/ships/fighter-1"_id, Vector2d{0., 0.}, TeamId::Blue);
    mine.emplace<Callsign>(game.GetPlayerName());

    const flecs::entity theirs = game.GetEntitySpawner().SpawnPlayer(
            "models/ships/fighter-1"_id, Vector2d{9000., 500.}, TeamId::Red);
    theirs.emplace<Callsign>(std::string("Nova"));
    game.GetPhysicsSystem().Teleport(theirs.get<PhysicsRef>(), Vector2d{9000., 500.},
                                     Vector2d{120., -40.});
    game.Update();

    // Aimed at somebody else, by a name typed in whatever case.
    theirs.get_mut<Damageable>().hp = 5.f;
    RunCheatCommand(game, mine, TeamId::Blue, "/heal @NOVA");
    Require(theirs.get<Damageable>().hp == theirs.get<Damageable>().maxHp,
            "callsign: @name runs the cheat on that player's ship, whatever case it is typed in");
    Require(mine.get<Damageable>().hp == mine.get<Damageable>().maxHp,
            "callsign: ...and not on the issuer's own");

    const CheatResult unknown = RunCheatCommand(game, mine, TeamId::Blue, "/heal @nobody");
    Require(!unknown.reply.empty(), "callsign: an unknown name is reported, not silently ignored");

    // Rendezvous: beside them, and at their velocity rather than at rest.
    RunCheatCommand(game, mine, TeamId::Blue, "/tp nova");
    game.Update();
    const Vector2d gap = mine.get<Transform>().pos - theirs.get<Transform>().pos;
    Require(gap.length() < 400., "callsign: /tp <player> arrives alongside them");
    Require((mine.get<Transform>().vel - theirs.get<Transform>().vel).length() < 30.,
            "callsign: /tp <player> matches their velocity rather than arriving at rest");

    Require(RunCheatCommand(game, mine, TeamId::Blue, "/players").reply.size() >= 2,
            "callsign: /players lists everyone flying");

    // Friendly fire is a round-wide rule, and one worth announcing.
    Require(!game.GetDamageSystem().IsFriendlyFire(), "cheat: friendly fire is off by default");
    const CheatResult ff = RunCheatCommand(game, flown, TeamId::Blue, "/ff on");
    Require(game.GetDamageSystem().IsFriendlyFire(), "cheat: /ff on turns friendly fire on");
    Require(ff.announce, "cheat: a rule change for the whole round is announced, not whispered");
    RunCheatCommand(game, flown, TeamId::Blue, "/ff off");
    Require(!game.GetDamageSystem().IsFriendlyFire(), "cheat: /ff off turns it back off");

    fs.Shutdown();
}

// A round with friendly fire on: a side's own rounds have to reach its own
// hulls, which is exactly what the default rule refuses.
void TestFriendlyFire()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }

    const auto shootAlly = [&fs](bool friendlyFire) {
        Game game(fs);
        game.GetDamageSystem().SetFriendlyFire(friendlyFire);

        flecs::entity shooter = game.GetEntitySpawner().SpawnPlayer(
                "models/ships/fighter-1"_id, Vector2d{0., 0.}, TeamId::Blue);
        flecs::entity ally = game.GetEntitySpawner().SpawnPlayer(
                "models/ships/fighter-1"_id, Vector2d{200., 0.}, TeamId::Blue);

        // Pointed at the ally and holding the trigger. Both are weightless
        // here -- no world was built, so nothing pulls the rounds off course.
        cpBody* body = game.GetPhysicsSystem().GetBody(shooter.get<PhysicsRef>()).cp.body.get();
        cpBodySetAngle(body, CP_PI / 2.); // nose at +X, where the ally is
        const float startHp = ally.get<Damageable>().hp;

        for (int tick = 0; tick < 120 && ally.is_alive(); ++tick) {
            InputCommand cmd;
            cmd.tick = game.GetStep();
            cmd.flags.firePrimary = true;
            shooter.get_mut<InputQueue>().Push(cmd);
            game.Update();
        }
        return !ally.is_alive() || ally.get<Damageable>().hp < startHp;
    };

    Require(!shootAlly(false), "friendly fire: off, a side's rounds pass through its own hulls");
    Require(shootAlly(true), "friendly fire: on, they land");

    fs.Shutdown();
}

// The beams: aimed rather than pointed, paid for out of the bank rather than a
// magazine, and worth less the further they have to reach.
void TestLasers()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }

    struct Burn {
        float damage = 0.f;
        float spent = 0.f;
        bool firing = false;
        bool fitted = false;
        int drawable = 0;
        // The windup, measured rather than assumed: how long the emitter was
        // authored to charge for, and the tick the light actually arrived on.
        int windupTicks = 0;
        int firstBurnTick = -1;
        int burnedTicks = 0;
    };

    // One run of the trigger on a hull `at` units along +X (negative puts it
    // astern), with the mounts asked for `aimAngle` in world terms. Held for
    // `ticks`, then followed for `coastTicks` more with the button up -- which
    // is how the half of a shot the pilot no longer controls gets measured.
    const auto burn = [&fs](double at, double aimAngle, bool fitBank, int ticks,
                            int coastTicks = 0) {
        Game game(fs);
        const UpgradeCatalog& catalog = game.GetUpgradeCatalog();
        EntitySpawner& spawner = game.GetEntitySpawner();

        flecs::entity shooter =
                spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{0., 0.}, TeamId::Blue);
        flecs::entity target =
                spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{at, 0.}, TeamId::Red);

        const UpgradeDef* lasers = catalog.FindKind(UpgradeKind::LaserTier);
        const UpgradeDef* bank = catalog.FindKind(UpgradeKind::Capacitor);
        Require(lasers && bank, "lasers: the pool has an emitter and a bank");

        if (fitBank) FitFree(catalog, *bank, 1, shooter.get_mut<ShipLoadout>());
        FitFree(catalog, *lasers, 1, shooter.get_mut<ShipLoadout>());
        const bool fitted = MountsArmedWith(shooter.get<ShipLoadout>(), MountArm::Laser) > 0;

        // Nose at +X, where the target is. Nothing was built here, so no well
        // pulls either hull off its mark.
        cpBody* body = game.GetPhysicsSystem().GetBody(shooter.get<PhysicsRef>()).cp.body.get();
        cpBodySetAngle(body, CP_PI / 2.);

        const ShipStats stats = catalog.ResolveStats(shooter.get<ShipLoadout>().levels);
        const int windupTicks =
                stats.laser ? ShipControlsSystem::BeamWindupTicks(*stats.laser) : 0;

        const float startHp = target.get<Damageable>().hp;
        bool firing = false;
        int firstBurnTick = -1;
        int burnedTicks = 0;
        for (int tick = 0; tick < ticks + coastTicks && target.is_alive(); ++tick) {
            InputCommand cmd;
            cmd.tick = game.GetStep();
            cmd.flags.fireLaser = tick < ticks;
            cmd.flags.aim = PackAim(aimAngle);
            shooter.get_mut<InputQueue>().Push(cmd);
            game.Update();
            if (shooter.get<Controls>().laserFiring) {
                if (firstBurnTick < 0) firstBurnTick = tick;
                ++burnedTicks;
                firing = true;
            }
        }

        // The renderer finds its beams with exactly this query, and it is the
        // one link in the chain no headless test would otherwise cover: a
        // signature that matches nothing draws nothing, silently.
        int drawable = 0;
        game.GetRegistry().each([&](flecs::entity, const Transform&, const Controls& controls,
                                    const ShipLoadout& loadout) {
            if (controls.laserFiring && MountsArmedWith(loadout, MountArm::Laser) > 0) ++drawable;
        });

        const float lost = target.is_alive() ? startHp - target.get<Damageable>().hp : startHp;
        return Burn{lost,          shooter.get<Controls>().capacitorSpent,
                    firing,        fitted,
                    drawable,      windupTicks,
                    firstBurnTick, burnedTicks};
    };

    // Every run below holds the trigger past the charge, since a beam that is
    // still winding up is a beam that has done nothing yet.
    const int windup = burn(/*at=*/600., /*aimAngle=*/0., /*fitBank=*/true, 1).windupTicks;
    Require(windup > 0, "lasers: the emitter is authored a charge to run");
    const int hold = windup + 30;

    // A quarter of laser_1's reach against three quarters of it. It falls off
    // from the muzzle, so the near burn is worth strictly more than the far one
    // for exactly the same charge spent. Distances are shares of that reach and
    // have to move with it -- the ratio is the test, not the numbers.
    const Burn near = burn(/*at=*/600., /*aimAngle=*/0., /*fitBank=*/true, hold);
    const Burn far = burn(/*at=*/1650., /*aimAngle=*/0., /*fitBank=*/true, hold);
    Require(near.fitted, "lasers: an emitter goes into a weapon mount");
    Require(near.firing && near.damage > 0.f, "lasers: a held beam burns what it is pointed at");
    Require(near.drawable == 1, "lasers: a burning beam is findable by what draws it");
    Require(far.damage > 0.f && near.damage > far.damage * 1.5f,
            "lasers: ...and burns it far harder up close than out at range");
    Require(near.spent > 0.f, "lasers: firing spends the bank");

    // The windup: nothing at all leaves the emitter until the charge has run,
    // and the whole of it is spent charging rather than shooting.
    Require(near.firstBurnTick == windup, "lasers: no light leaves the emitter until it has charged");
    const Burn charging = burn(/*at=*/600., /*aimAngle=*/0., /*fitBank=*/true, windup);
    Require(!charging.firing && charging.damage == 0.f,
            "lasers: a trigger held only through the charge burns nothing");
    Require(charging.spent > 0.f, "lasers: ...and charging costs the bank all the same");

    // A tap, and then hands off: the windup cannot be called off, so the shot
    // arrives anyway and burns its minimum. This is the whole commitment --
    // press it by mistake and the beam still goes out.
    const Burn tapped = burn(/*at=*/600., /*aimAngle=*/0., /*fitBank=*/true, /*ticks=*/1,
                             /*coastTicks=*/windup + 60);
    Require(tapped.firing && tapped.damage > 0.f,
            "lasers: a released trigger cannot call the charge off -- the shot still lands");
    Require(tapped.firstBurnTick == windup, "lasers: ...arriving on the tick it would have anyway");
    Require(tapped.burnedTicks > 0 && tapped.burnedTicks < near.burnedTicks,
            "lasers: ...and burns its minimum rather than what a held trigger buys");

    // Past the emitter's reach it lands nothing at all -- while still costing
    // exactly as much to hold, which is the mistake the weapon punishes.
    const Burn beyond = burn(/*at=*/3500., /*aimAngle=*/0., /*fitBank=*/true, hold);
    Require(beyond.damage == 0.f, "lasers: nothing reaches past the emitter's range");
    Require(beyond.firing && beyond.spent > 0.f, "lasers: ...and holding it out there still costs");

    // Directly astern is outside fighter-1's 300-degree gimbal, and asking for
    // it pins the beam to the edge of the arc rather than swinging it around --
    // so the hull on its tail is not hit, however precisely the pilot aims.
    const Burn behind = burn(/*at=*/-750., /*aimAngle=*/CP_PI, /*fitBank=*/true, hold);
    Require(behind.firing, "lasers: the emitter still burns with the aim on its stop");
    Require(behind.damage == 0.f, "lasers: ...but the gimbal will not swing into the blind cone");

    // And none of it without a bank to fire out of: the emitter is not merely
    // quiet, it cannot be fitted at all.
    const Burn unbanked = burn(/*at=*/600., /*aimAngle=*/0., /*fitBank=*/false, hold);
    Require(!unbanked.fitted, "lasers: no capacitor, no emitter to fit");
    Require(!unbanked.firing && unbanked.damage == 0.f, "lasers: ...and nothing burns");

    fs.Shutdown();
}

// Field plating is a mirror: it takes its own share of a beam and throws the
// rest back off the hull as a live beam, which burns whatever it then meets. A
// bubble does no such thing and swallows a beam whole.
//
// Where the bounce GOES is not asserted here, and deliberately: a plate is a
// slanted facet of a real hull, so a beam meeting one square on leaves at twice
// the facet's angle rather than coming back down its own path -- measured at
// nearly ninety degrees off on fighter-1's plates. What the mirror does is
// therefore tested two ways that do not care about direction: the share the
// target keeps, and whether anything standing around it gets burned.
void TestBeamDeflection()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }

    // The witnesses stand in a screen ACROSS the beam's own axis, behind the
    // target, rather than in a ring around it: the bounce off a plate leaves at
    // roughly forty-five degrees back the way it came, and a ring spaced widely
    // enough not to overlap is spaced widely enough for the beam to thread
    // between two of them -- which is what a first attempt did, and it read as
    // no deflection at all.
    constexpr double SCREEN_X = 200.;      // between the shooter and the target
    constexpr double SCREEN_SPAN = 520.;   // either side of the axis
    constexpr double SCREEN_STEP = 40.;    // closer than a hull is wide
    constexpr double SCREEN_CLEAR = 70.;   // the doorway the shot leaves through

    struct Exchange {
        float targetShieldLost = 0.f;
        float ringLost = 0.f; // hull burned off the ships standing around it
        bool fitted = false;
    };

    // `shieldType` None leaves the target bare. Same geometry, same hold, same
    // everything else in all three runs -- the shield is the only variable.
    const auto trade = [&fs](ShieldType shieldType) {
        Game game(fs);
        const UpgradeCatalog& catalog = game.GetUpgradeCatalog();
        EntitySpawner& spawner = game.GetEntitySpawner();

        flecs::entity shooter =
                spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{0., 0.}, TeamId::Blue);
        const Vector2d targetAt{500., 0.};
        flecs::entity target =
                spawner.SpawnPlayer("models/ships/fighter-1"_id, targetAt, TeamId::Red);

        // A ring of bystanders on the target's own side, so whatever the bounce
        // does it runs into one of them. Red, so friendly fire (off here) never
        // enters into it: this is the shooter's beam hitting the enemy team.
        std::vector<flecs::entity> ring;
        float ringStartHp = 0.f;
        for (double y = -SCREEN_SPAN; y <= SCREEN_SPAN; y += SCREEN_STEP) {
            // Nobody stands in the doorway: a witness on the line between the
            // shooter and the target is simply what the beam hits first, and the
            // run then measures a shot that never reached the mirror at all --
            // which is exactly what an earlier version of this did, and it read
            // as the plates absorbing nothing.
            if (std::abs(y) < SCREEN_CLEAR) continue;

            ring.push_back(spawner.SpawnPlayer("models/ships/fighter-1"_id,
                                               Vector2d{SCREEN_X, y}, TeamId::Red));
            ringStartHp += ring.back().get<Damageable>().hp;
        }

        FitFree(catalog, *catalog.FindKind(UpgradeKind::Capacitor), 1,
                shooter.get_mut<ShipLoadout>());
        FitFree(catalog, *catalog.FindKind(UpgradeKind::LaserTier), 1,
                shooter.get_mut<ShipLoadout>());
        bool fitted = false;
        if (shieldType != ShieldType::None) {
            const UpgradeDef* shield = catalog.FindKind(UpgradeKind::Shield, shieldType);
            Require(shield != nullptr, "deflection: the pool has both emitters");
            FitFree(catalog, *shield, 1, target.get_mut<ShipLoadout>());
            fitted = target.get<ShipLoadout>().levels.shield > 0;
        }

        // Nose at the target. Nothing was built out here, so no well pulls any
        // of these hulls off its mark.
        cpBodySetAngle(game.GetPhysicsSystem().GetBody(shooter.get<PhysicsRef>()).cp.body.get(),
                       CP_PI / 2.);


        const ShipStats stats = catalog.ResolveStats(shooter.get<ShipLoadout>().levels);
        const int hold = ShipControlsSystem::BeamWindupTicks(*stats.laser) + 40;

        // Charge it by hand, as the shield tests above do: a field fitted at a
        // yard comes up empty and fills at a few points a second, so a run that
        // merely waited a tick would be measuring an empty emitter -- which is
        // no mirror at all, and read here as the plates absorbing nothing.
        // Deep enough that neither emitter runs dry mid-burn, since a spent one
        // stops deflecting and the ratio below would then measure the gap.
        if (shieldType != ShieldType::None) {
            ShipLoadout& shield = target.get_mut<ShipLoadout>();
            // To its real capacity, not past it: ShieldSystem re-sums the pool
            // from the plates every tick and clamps each to its own share, so an
            // overcharged fill shows up as a colossal "loss" on the next tick
            // and drowns the damage being measured.
            const float capacity = catalog.ResolveStats(shield.levels).shieldCapacity;
            const float perPlate = shield.plateCount > 0
                    ? capacity / static_cast<float>(shield.plateCount) : capacity;
            shield.plates.fill(perPlate);
            shield.plateRegenDelay = {};
            shield.shieldHp = capacity;
        }
        const float startShield = target.get<ShipLoadout>().shieldHp;
        Require(startShield > 0.f || shieldType == ShieldType::None,
                "deflection: a fitted shield carries charge before the shooting starts");

        for (int tick = 0; tick < hold && target.is_alive(); ++tick) {
            InputCommand cmd;
            cmd.tick = game.GetStep();
            cmd.flags.fireLaser = true;
            cmd.flags.aim = PackAim(0.);
            shooter.get_mut<InputQueue>().Push(cmd);
            game.Update();
        }

        float ringHp = 0.f;
        for (flecs::entity witness : ring) {
            if (witness.is_alive()) ringHp += witness.get<Damageable>().hp;
        }
        const float shieldLost = target.is_alive()
                ? startShield - target.get<ShipLoadout>().shieldHp : startShield;
        return Exchange{shieldLost, ringStartHp - ringHp, fitted};
    };

    const Exchange plated = trade(ShieldType::Plating);
    Require(plated.fitted, "deflection: the plates go onto the target");
    Require(plated.targetShieldLost > 0.f, "deflection: a beam spends charge off the plates");
    Require(plated.ringLost > 0.f,
            "deflection: ...and what they do not absorb leaves the hull and burns something else");

    const Exchange bubbled = trade(ShieldType::Bubble);
    Require(bubbled.fitted, "deflection: the bubble goes onto the target");
    Require(bubbled.targetShieldLost > 0.f, "deflection: a bubble spends charge too");
    Require(bubbled.ringLost == 0.f, "deflection: ...but absorbs the beam whole, deflecting none");

    // The split is exact, so the plated hull keeps only its own share of what
    // the bubble kept: laser_absorb is 0.5 at rank I, and nothing is lost in the
    // bounce. Compared as a ratio rather than against a figure, since the beam's
    // own damage numbers are meant to be tuned without breaking this.
    const float share = plated.targetShieldLost / bubbled.targetShieldLost;
    Require(share > 0.4f && share < 0.6f,
            "deflection: the plates keep half the beam and return the other half");

    const Exchange bare = trade(ShieldType::None);
    Require(bare.ringLost == 0.f, "deflection: a bare hull is not a mirror");

    fs.Shutdown();
}

// A beam is aimed at a PLACE, not along a bearing (see CGame::AimAtPoint and
// GravitarisApplication::UpdateAim, which is where the point is latched). The
// client owns the latch, but the claim it rests on is the sim's: a shooter
// re-deriving its aim from one fixed world point holds the beam on what is
// standing there while it flies, where a shooter holding the bearing it started
// with walks the beam off into space.
void TestBeamAimsAtAPlace()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }

    // `trackPoint` false freezes the aim at the bearing the shot started on --
    // exactly the old behaviour, as the control.
    const auto strafe = [&fs](bool trackPoint) {
        Game game(fs);
        const UpgradeCatalog& catalog = game.GetUpgradeCatalog();
        EntitySpawner& spawner = game.GetEntitySpawner();

        flecs::entity shooter =
                spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{0., 0.}, TeamId::Blue);
        flecs::entity target =
                spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{600., 0.}, TeamId::Red);

        FitFree(catalog, *catalog.FindKind(UpgradeKind::Capacitor), 1, shooter.get_mut<ShipLoadout>());
        FitFree(catalog, *catalog.FindKind(UpgradeKind::LaserTier), 1, shooter.get_mut<ShipLoadout>());

        // Nose at the target, and crossing its bearing at a fair clip. No wells
        // out here, so this is the only motion in the run.
        cpBody* body = game.GetPhysicsSystem().GetBody(shooter.get<PhysicsRef>()).cp.body.get();
        cpBodySetAngle(body, CP_PI / 2.);
        cpBodySetVelocity(body, cpv(0., 150.));

        const Vector2d aimPoint = target.get<Transform>().pos;
        const float startHp = target.get<Damageable>().hp;
        const ShipStats stats = catalog.ResolveStats(shooter.get<ShipLoadout>().levels);
        const int windup = ShipControlsSystem::BeamWindupTicks(*stats.laser);

        std::uint16_t held = 0;
        for (int tick = 0; tick < windup + 60 && target.is_alive(); ++tick) {
            const Vector2d offset = aimPoint - shooter.get<Transform>().pos;
            const auto toPoint = PackAim(std::atan2(offset.y(), offset.x()));
            if (tick == 0) held = toPoint;

            InputCommand cmd;
            cmd.tick = game.GetStep();
            cmd.flags.fireLaser = true;
            cmd.flags.aim = trackPoint ? toPoint : held;
            shooter.get_mut<InputQueue>().Push(cmd);
            game.Update();
        }

        return target.is_alive() ? startHp - target.get<Damageable>().hp : startHp;
    };

    Require(strafe(/*trackPoint=*/true) > 0.f,
            "beam aim: a beam re-aimed at one place holds it while the ship flies past");
    Require(strafe(/*trackPoint=*/false) == 0.f,
            "beam aim: ...where the bearing it started on walks off into space");

    fs.Shutdown();
}

// The one thing no gun can do: burn a missile out of the air. A beam is the
// only line allowed to stop on a round, and the falloff applies to one exactly
// as it does to a hull -- so this works close in and not at reach.
void TestBeamsInterceptMissiles()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }

    struct Intercept {
        bool sawRound = false;
        bool defenderHit = false;
        // How far out the round was when it stopped existing: a whole reach for
        // one burned out of the sky, nothing at all for one that arrived.
        double diedAt = 0.;
    };

    // A round launched at a defender `at` units away, with the defender holding
    // a beam straight back down the line it is coming in on. `useBeam` false
    // fires the hull's guns instead, which must NOT bring it down.
    const auto defend = [&fs](double at, bool useBeam) {
        Game game(fs);
        const UpgradeCatalog& catalog = game.GetUpgradeCatalog();
        EntitySpawner& spawner = game.GetEntitySpawner();

        flecs::entity attacker =
                spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{at, 0.}, TeamId::Red);
        flecs::entity defender =
                spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{0., 0.}, TeamId::Blue);

        const UpgradeDef* lasers = catalog.FindKind(UpgradeKind::LaserTier);
        const UpgradeDef* bank = catalog.FindKind(UpgradeKind::Capacitor);
        const UpgradeDef* bay = catalog.FindKind(UpgradeKind::MissileTier);
        Require(lasers && bank && bay, "intercept: the pool has an emitter, a bank and a rack");

        FitFree(catalog, *bay, 1, attacker.get_mut<ShipLoadout>());
        FitFree(catalog, *bank, 1, defender.get_mut<ShipLoadout>());
        FitFree(catalog, *lasers, 1, defender.get_mut<ShipLoadout>());

        // Both nose-on to each other along the X axis, so the missile flies up
        // the same line the beam is pointed down.
        cpBodySetAngle(game.GetPhysicsSystem().GetBody(attacker.get<PhysicsRef>()).cp.body.get(),
                       -CP_PI / 2.);
        cpBodySetAngle(game.GetPhysicsSystem().GetBody(defender.get<PhysicsRef>()).cp.body.get(),
                       CP_PI / 2.);

        // The defender charges its emitter before the round is away: point
        // defence a full second late would never be point defence.
        const ShipStats stats = catalog.ResolveStats(defender.get<ShipLoadout>().levels);
        const int windup = stats.laser ? ShipControlsSystem::BeamWindupTicks(*stats.laser) : 0;

        const float defenderHp = defender.get<Damageable>().hp;

        flecs::entity round;
        bool sawRound = false;
        double lastSeenAt = at;
        double diedAt = 0.;
        for (int tick = 0; tick < windup + 900 && defender.is_alive(); ++tick) {
            InputCommand defence;
            defence.tick = game.GetStep();
            defence.flags.aim = PackAim(0.);
            defence.flags.fireLaser = useBeam;
            defence.flags.firePrimary = !useBeam;
            defender.get_mut<InputQueue>().Push(defence);

            // One round only: the rack keeps launching while the button is held,
            // and a stream of them says nothing about whether any single one
            // was stopped.
            InputCommand attack;
            attack.tick = game.GetStep();
            attack.flags.fireMissile = !sawRound;
            attacker.get_mut<InputQueue>().Push(attack);

            game.Update();

            if (round.is_alive()) {
                lastSeenAt = round.get<Transform>().pos.length();
                continue;
            }
            if (sawRound) { // it was here last tick and is not now
                diedAt = lastSeenAt;
                break;
            }
            game.GetRegistry().each([&](flecs::entity ent, const Missile&) {
                if (!round.is_alive()) round = ent;
            });
            sawRound = round.is_alive();
        }

        return Intercept{sawRound, defender.is_alive()
                                           ? defender.get<Damageable>().hp < defenderHp
                                           : true,
                         diedAt};
    };

    const Intercept beamed = defend(/*at=*/700., /*useBeam=*/true);
    Require(beamed.sawRound, "intercept: a launched round is there to be shot at");
    Require(!beamed.defenderHit, "intercept: a beam stops a missile before it arrives");
    Require(beamed.diedAt > 150., "intercept: ...and stops it well out from the hull");

    // Gunfire deliberately passes straight through: a beam is the point-defence
    // weapon, and a round trading itself for a missile would take that away
    // from it.
    const Intercept shot = defend(/*at=*/700., /*useBeam=*/false);
    Require(shot.sawRound && shot.defenderHit,
            "intercept: gunfire flies through a missile rather than stopping it");

    fs.Shutdown();
}

// The capacitor, through the one thing that draws on it so far: while it is
// burning a hull exceeds the speed its own engine could otherwise reach, it
// only burns while the engine is lit, and what it spends is what it has to
// wait for.
void TestCapacitor()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }

    Game game(fs);
    EntitySpawner& spawner = game.GetEntitySpawner();

    const UpgradeCatalog& catalog = game.GetUpgradeCatalog();
    const UpgradeDef* def = catalog.FindKind(UpgradeKind::Capacitor);
    Require(def != nullptr, "capacitor: the pool has a capacitor upgrade");

    // Empty space, no wells: whatever speed this ship reaches is its engine's
    // doing and nothing else's.
    flecs::entity ship = spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{0., 0.}, TeamId::Blue);
    const double hullMaxSpeed = game.GetPhysicsSystem().GetBody(ship.get<PhysicsRef>()).body->GetMaxSpeed();

    const auto fly = [&](bool thrust, bool boost, int ticks) {
        for (int tick = 0; tick < ticks && ship.is_alive(); ++tick) {
            InputCommand cmd;
            cmd.tick = game.GetStep();
            cmd.flags.thrustForward = thrust;
            cmd.flags.boost = boost;
            ship.get_mut<InputQueue>().Push(cmd);
            game.Update();
        }
    };
    const auto flyWith = [&](bool boost, int ticks) { fly(/*thrust=*/true, boost, ticks); };
    const auto speed = [&] { return ship.get<Transform>().vel.length(); };

    // No bank aboard: the button does nothing at all however good the drive is,
    // and the hull's own cap holds.
    flyWith(/*boost=*/true, 900);
    Require(!ship.get<Controls>().boosting, "capacitor: a ship with no bank never burns");
    Require(speed() <= hullMaxSpeed * 1.02,
            "capacitor: an unbanked hull is still held to its own top speed");

    FitFree(catalog, *def, 1, ship.get_mut<ShipLoadout>());
    const ShipStats stats = catalog.ResolveStats(ship.get<ShipLoadout>().levels);
    Require(stats.capacitorCharge > 0.f && stats.capacitorRefillPerTick > 0.f,
            "capacitor: the fitted bank resolves to real charge and a real refill");
    Require(stats.boostDrainPerTick > 0.f, "capacitor: and the drive draws on it");

    // Rounded up: a bank that divides into a fraction of a tick's drain still
    // has that fraction to burn, and the last sliver of it is a real tick.
    const auto burnTicks =
            static_cast<int>(std::ceil(stats.capacitorCharge / stats.boostDrainPerTick));
    const auto refillTicks =
            static_cast<int>(std::ceil(stats.capacitorCharge / stats.capacitorRefillPerTick));
    const auto spent = [&] { return ship.get<Controls>().capacitorSpent; };

    // Burning: past the cap the engine alone could reach, with no gravity to
    // credit it to.
    double peak = 0.;
    for (int tick = 0; tick < burnTicks && ship.is_alive(); ++tick) {
        flyWith(/*boost=*/true, 1);
        peak = std::max(peak, speed());
    }
    Require(peak > hullMaxSpeed, "capacitor: a burn carries the hull past its own top speed");
    Require(peak <= hullMaxSpeed * static_cast<double>(stats.boostMaxSpeedScale) * 1.02,
            "capacitor: and no further than the drive's own ceiling");

    // Spent: the loop above drew the bank dry, so the next tick on the button
    // grants nothing. Emptiness is read before that tick rather than after --
    // a tick that grants no burn is a tick the bank spends refilling, so by
    // then it is already a sliver off empty. Exactly one tick, not a handful:
    // with the trigger held an empty bank refills to the engage floor and
    // lights again all by itself (see below), which is the rule working rather
    // than a state to assert on.
    Require(spent() >= stats.capacitorCharge, "capacitor: a burn empties the bank");
    flyWith(/*boost=*/true, 1);
    Require(!ship.get<Controls>().boosting, "capacitor: ...and ends when the charge does");

    // And it will not re-light on the first drops back into it. A draw has to
    // be worth starting (CAPACITOR_ENGAGE_SHARE): without that floor a dry
    // injector lights again on one tick of charge, and the overburn degenerates
    // into a stutter that is on more often than off. A twentieth of the refill
    // is far too little to clear the floor -- asked for with the trigger
    // released, so what is being tested is the start and not a burn already
    // running.
    fly(/*thrust=*/true, /*boost=*/false, refillTicks / 20);
    const float dribble = spent();
    Require(dribble > 0.f && dribble < stats.capacitorCharge,
            "capacitor: a spent bank starts refilling, and is nowhere near full yet");
    flyWith(/*boost=*/true, 1);
    Require(!ship.get<Controls>().boosting,
            "capacitor: ...and that little back in it will not light the injector");

    // ...and comes back as it refills.
    for (int tick = 0; tick < refillTicks + burnTicks + 5 && !ship.get<Controls>().boosting; ++tick) {
        flyWith(/*boost=*/true, 1);
    }
    Require(ship.get<Controls>().boosting,
            "capacitor: another burn is available once it has refilled");

    // Coasting spends nothing: the injector feeds the engine, so asking for the
    // overburn with the throttle shut is not a burn and does not cost one.
    fly(/*thrust=*/false, /*boost=*/false, refillTicks + 10);
    Require(spent() == 0.f, "capacitor: the bank refills to full");
    fly(/*thrust=*/false, /*boost=*/true, 60);
    Require(!ship.get<Controls>().boosting && spent() == 0.f,
            "capacitor: holding it while coasting burns nothing");

    // A tap costs a tap. Letting go leaves the rest of the bank where it was,
    // rather than committing the whole burn and the whole wait.
    const int tap = burnTicks / 4;
    Require(tap > 0, "capacitor: the fitted bank is deep enough to tap");
    flyWith(/*boost=*/true, tap);
    const float afterTap = spent();
    Require(afterTap > 0.f && afterTap <= stats.boostDrainPerTick * static_cast<float>(tap + 1),
            "capacitor: a tap spends only what it burned");
    // Long enough for the refill to put a measurable amount back (a full bank
    // takes the whole recharge, so a tick returns far less than a tick of burn
    // costs), and far short of the whole wait an empty one would.
    flyWith(/*boost=*/false, 12);
    Require(spent() < afterTap && spent() > 0.f,
            "capacitor: letting go stops the drain and starts the refill where it stood");

    // The drive raises the cruise the hull's own thruster can reach -- and the
    // burn's ceiling stays a multiple of the hull's number rather than of the
    // drive's, so one authored value still bounds how fast anything can travel.
    const UpgradeDef* engine = catalog.FindKind(UpgradeKind::EngineTier);
    Require(engine != nullptr, "engine: the pool has a drive");
    {
        flecs::entity driven =
                spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{50000., 0.}, TeamId::Red);
        FitFree(catalog, *engine, 3, driven.get_mut<ShipLoadout>());

        const ShipStats stats = catalog.ResolveStats(driven.get<ShipLoadout>().levels);
        const ShipControlsSystem::Motion cruise = ShipControlsSystem::MotionOf(
                *game.GetPhysicsSystem().GetBody(driven.get<PhysicsRef>()).body, stats,
                ShipControlsSystem::BoostEffect{});
        Require(cruise.maxSpeed > hullMaxSpeed,
                "engine: a fitted drive raises the speed the thruster alone can reach");
        Require(cruise.thrust > 0., "engine: ...and still pushes");

        for (int tick = 0; tick < 900 && driven.is_alive(); ++tick) {
            InputCommand cmd;
            cmd.tick = game.GetStep();
            cmd.flags.thrustForward = true;
            driven.get_mut<InputQueue>().Push(cmd);
            game.Update();
        }
        Require(driven.get<Transform>().vel.length() > hullMaxSpeed * 1.02,
                "engine: a driven hull really does fly faster than a stock one's cap");

        // The same hull with a bank to burn as well: the ceiling is the higher
        // of the two, and the drive is what decides it here.
        FitFree(catalog, *def, 1, driven.get_mut<ShipLoadout>());
        const ShipStats both = catalog.ResolveStats(driven.get<ShipLoadout>().levels);
        const ShipControlsSystem::Motion burning = ShipControlsSystem::MotionOf(
                *game.GetPhysicsSystem().GetBody(driven.get<PhysicsRef>()).body, both,
                ShipControlsSystem::BoostEffectOf(/*boosting=*/true, both));
        Require(burning.maxSpeed
                        <= std::max(cruise.maxSpeed,
                                    hullMaxSpeed * static_cast<double>(both.boostMaxSpeedScale))
                                   + 1e-9,
                "engine: a burn's ceiling is a multiple of the hull's own speed, not the drive's");
        Require(burning.thrust > cruise.thrust,
                "engine: ...while the burn's thrust still stacks on the drive's");
    }

    fs.Shutdown();
}

void TestTakeoff()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }

    // A standing start off the deck: set down first (the same gentle upright
    // drop TestLandingAndClaiming uses), then hold full throttle with the nose
    // already radial and climb.
    {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();
        flecs::entity planet = spawner.SpawnOrbitingPlanet("models/planets/simple"_id,
                                                          Vector2d{0., 0.}, 1e-9, 800., 1.0, 0.0);
        const Vector2d center = planet.get<Transform>().pos;
        const double radius = planet.get<Planet>().radius * planet.get<Transform>().scale.x();

        flecs::entity ship = spawner.SpawnPlayer("models/ships/fighter-1"_id,
                                                 center + Vector2d{0., radius + 15.});
        cpBody* body = game.GetPhysicsSystem().GetBody(ship.get<PhysicsRef>()).cp.body.get();
        cpBodySetAngle(body, CP_PI); // legs (local +Y) down, so the nose is radial
        cpBodySetVelocity(body, cpv(0., -8.));

        for (int tick = 0; tick < 240 && ship.is_alive(); ++tick) game.Update();
        Require(ship.is_alive() && ship.get<LandingState>().landed,
                "takeoff: the ship is standing on the planet (setup check)");

        const double restRadius = (ship.get<Transform>().pos - center).length();
        for (int tick = 0; tick < 240 && ship.is_alive(); ++tick) {
            ControlFlags flags{};
            flags.thrustForward = true;
            ship.get_mut<InputQueue>().Push(InputCommand{game.GetStep(), flags});
            game.Update();
        }
        Require(ship.is_alive(), "takeoff: the ship survives its own launch");
        Require((ship.get<Transform>().pos - center).length() - restRadius > 100.,
                "takeoff: full throttle off the surface actually climbs");
    }

    // The AI's own launch, end to end: a pilot parked on a planet with its
    // business elsewhere departs, and gets clear rather than settling into a
    // hover or flapping in the hysteresis band.
    {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();
        flecs::entity planet = spawner.SpawnOrbitingPlanet("models/planets/simple"_id,
                                                           Vector2d{0., 0.}, 1e-9, 800., 1.0, 0.0);
        const Vector2d center = planet.get<Transform>().pos;
        const double radius = planet.get<Planet>().radius * planet.get<Transform>().scale.x();

        flecs::entity ship = spawner.SpawnAIShip("models/ships/fighter-1"_id,
                                                 center + Vector2d{0., radius + 13.},
                                                 game.GetAIPresets().Default(), Vector2d{}, CP_PI);
        // Far enough that no part of this is the pilot merely drifting: the
        // enemy is its objective, and the objective is not on this rock.
        spawner.SpawnPlayer("models/ships/fighter-1"_id, center + Vector2d{40000., 0.});

        bool sawDepart = false;
        double best = 0.;
        for (int tick = 0; tick < 900 && ship.is_alive(); ++tick) {
            game.Update();
            if (!ship.is_alive()) break;
            if (ship.get<AIPilot>().behavior == AIBehavior::Depart) sawDepart = true;
            best = std::max(best, (ship.get<Transform>().pos - center).length() - radius);
        }
        Require(ship.is_alive(), "takeoff: the AI survives its own launch");
        Require(sawDepart, "takeoff: a pilot with business elsewhere departs the body it sits on");
        Require(best > 400., "takeoff: the departure climb gets clear of the body");
        Require(ship.get<AIPilot>().behavior != AIBehavior::Depart,
                "takeoff: departure ends once clear instead of latching");
    }

    // The other half of the same rule, and the reason a claim is reachable at
    // all: a leader ordered onto a planet flies the whole braked descent from
    // a standing start well outside it and sets down gently enough to keep the
    // hull -- the well-avoidance reflex must not fight the landing it was
    // ordered to make, and the descent must be solved against the gravity
    // waiting at the bottom rather than the gravity it starts in.
    {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();
        flecs::entity planet = spawner.SpawnOrbitingPlanet("models/planets/simple"_id,
                                                           Vector2d{0., 0.}, 1e-9, 800., 1.0, 0.0);
        const Vector2d center = planet.get<Transform>().pos;
        flecs::entity leader = spawner.SpawnAILeader("models/ships/fighter-1"_id,
                                                     center + Vector2d{0., 2500.}, TeamId::Red,
                                                     game.GetAIPresets().Default());
        const float hp = leader.get<Damageable>().hp;

        bool claimed = false;
        for (int tick = 0; tick < 2400 && leader.is_alive() && !claimed; ++tick) {
            game.Update();
            claimed = planet.get<Team>().id == TeamId::Red;
        }
        Require(leader.is_alive(), "takeoff: the descending leader survives its own landing");
        Require(claimed, "takeoff: a leader ordered onto a planet actually lands and claims it");
        Require(leader.get<Damageable>().hp >= hp,
                "takeoff: the descent costs the hull nothing");
    }

    // The same descent onto a planet that is actually travelling its orbit --
    // the only kind the sector generator makes. centerMass puts the pad at
    // ~80 units/s, on a par with a fighter's own cruise, and the orbit is wide
    // enough that over a descent it is effectively a straight line at that
    // speed. Everything the pilot wants at the bottom is expressed in that
    // moving frame, so an attitude taken from a world-space velocity is an
    // attitude taken from the pad's direction of travel rather than from up.
    // The approach is radial to the *orbit* (the pad's +X pole, its motion
    // being +Y) so the two are perpendicular and the difference shows.
    {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();
        flecs::entity planet = spawner.SpawnOrbitingPlanet("models/planets/simple"_id,
                                                           Vector2d{0., 0.}, 1828571., 20000., 1.0, 0.0);

        flecs::entity leader = spawner.SpawnAILeader("models/ships/fighter-1"_id,
                                                     planet.get<Transform>().pos + Vector2d{2500., 0.},
                                                     TeamId::Red, game.GetAIPresets().Default());
        const float hp = leader.get<Damageable>().hp;

        game.Update(); // OrbitSystem writes the pad's velocity on the first tick
        Require(planet.get<Transform>().vel.length() > 60.,
                "takeoff: the orbiting planet actually moves (setup check)");

        bool claimed = false;
        int ticks = 0;
        for (ticks = 0; ticks < 3600 && leader.is_alive() && !claimed; ++ticks) {
            game.Update();
            claimed = planet.get<Team>().id == TeamId::Red;
        }
        Require(leader.is_alive(), "takeoff: the leader survives a landing on a moving planet");
        Require(claimed, "takeoff: a leader ordered onto a moving planet lands and claims it");
        // The budget is what the bug cost: taking the resting attitude off the
        // pad's world velocity rather than off up left the pilot flying the
        // whole time it sat there, and roughly doubled this.
        Require(ticks < 1800, "takeoff: setting down on a moving planet is not slower than the "
                              "descent itself -- a parked pilot stops flying");
        Require(leader.get<Damageable>().hp >= hp,
                "takeoff: the moving-planet descent costs the hull nothing");
    }

    // A pilot parked on a planet with an enemy loitering just overhead -- close
    // enough that the departure rule counts its business as being "here". It
    // still has to get airborne: nothing is winnable from the ground.
    {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();
        flecs::entity planet = spawner.SpawnOrbitingPlanet("models/planets/simple"_id,
                                                           Vector2d{0., 0.}, 1828571., 20000., 1.0, 0.0);
        const Vector2d centre = planet.get<Transform>().pos;
        const double radius = planet.get<Planet>().radius * planet.get<Transform>().scale.x();

        flecs::entity ship = spawner.SpawnAIShip("models/ships/fighter-1"_id,
                                                 centre + Vector2d{radius + 13., 0.},
                                                 game.GetAIPresets().Default(), Vector2d{},
                                                 -3.14159265358979323846 / 2.);
        spawner.SpawnPlayer("models/ships/fighter-1"_id, centre + Vector2d{0., radius + 120.});

        int clearAt = -1;
        for (int tick = 0; tick < 1200 && ship.is_alive() && clearAt < 0; ++tick) {
            game.Update();
            if (!ship.is_alive()) break;
            if ((ship.get<Transform>().pos - centre).length() - radius > 300.) clearAt = tick;
        }
        Require(ship.is_alive(), "takeoff: the pilot survives having an enemy overhead");
        Require(clearAt >= 0, "takeoff: a parked pilot with an enemy overhead gets off the "
                              "surface instead of fighting from where it stands");
    }

    // Both halves of "either lift off or stay": a leader down on the rock it
    // was ordered onto holds the pad for as long as the claim needs, and then
    // leaves once there is nothing keeping it there.
    {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();
        flecs::entity planet = spawner.SpawnOrbitingPlanet("models/planets/simple"_id,
                                                           Vector2d{0., 0.}, 1828571., 20000., 1.0, 0.0);
        flecs::entity leader = spawner.SpawnAILeader("models/ships/fighter-1"_id,
                                                     planet.get<Transform>().pos + Vector2d{2500., 0.},
                                                     TeamId::Red, game.GetAIPresets().Default());

        int downAt = -1;
        for (int tick = 0; tick < 3600 && leader.is_alive() && downAt < 0; ++tick) {
            game.Update();
            if (leader.is_alive() && leader.get<AIPilot>().behavior == AIBehavior::Landed) downAt = tick;
        }
        Require(downAt >= 0, "landed: a leader ordered onto a planet reaches the parked state");

        // Holding is measured against the pad, not the world: the planet is
        // orbiting, so a ship that sat perfectly still in world space would
        // have slid off it.
        const Vector2d touchdown = leader.get<Transform>().pos - planet.get<Transform>().pos;
        int held = 0;
        int observed = 0;
        double wander = 0.;
        bool claimed = false;
        for (int tick = 0; tick < 1800 && leader.is_alive() && !claimed; ++tick) {
            game.Update();
            if (!leader.is_alive()) break;
            claimed = planet.get<Team>().id == TeamId::Red;
            ++observed;
            if (leader.get<AIPilot>().behavior == AIBehavior::Landed) ++held;
            const Vector2d r = leader.get<Transform>().pos - planet.get<Transform>().pos;
            wander = std::max(wander, (r - touchdown).length());
        }
        Require(claimed, "landed: a parked leader holds the pad long enough to claim");
        Require(held >= observed - 2, "landed: a parked leader stays parked instead of flapping "
                                      "between the pad and the airborne reflexes");
        Require(wander < 40., "landed: a parked leader holds its spot on the pad");

        // And the other half -- the claim is done, so there is no longer any
        // business here.
        const double radius = planet.get<Planet>().radius * planet.get<Transform>().scale.x();
        bool left = false;
        for (int tick = 0; tick < 1800 && leader.is_alive() && !left; ++tick) {
            game.Update();
            if (!leader.is_alive()) break;
            left = (leader.get<Transform>().pos - planet.get<Transform>().pos).length() - radius > 300.;
        }
        Require(left, "landed: a leader with nothing left to do on the rock lifts off again");
    }

    // A body that is not the heaviest in the sector is still a body. The
    // danger reflex tested the predicted path against the single heaviest
    // source, so once sectors had suns, no planet was ever evaluated at all
    // and the only thing keeping a pilot off one was the departure rule.
    {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();

        // Unambiguously the heaviest thing in the sector, and far enough away
        // that its own pull is not what the pilot is reacting to.
        flecs::entity sun = spawner.SpawnStar("models/planets/simple"_id, Vector2d{0., 0.});
        sun.get_mut<GravitySource>().multiplier = 100.;

        // centerMass ~0 leaves it parked, so the course below stays a course.
        flecs::entity planet = spawner.SpawnOrbitingPlanet("models/planets/simple"_id,
                                                           Vector2d{0., 0.}, 1e-9, 60000., 1.0, 0.0);
        const Vector2d centre = planet.get<Transform>().pos;
        const double radius = planet.get<Planet>().radius * planet.get<Transform>().scale.x();

        // The pursuit runs straight through the planet: guidance has no
        // opinion about bodies in the way, so avoiding one is the reflex's
        // job and nothing else's.
        flecs::entity ship = spawner.SpawnAIShip("models/ships/fighter-1"_id,
                                                 centre + Vector2d{0., 1200.},
                                                 game.GetAIPresets().Default());
        spawner.SpawnPlayer("models/ships/fighter-1"_id, centre + Vector2d{0., -1200.});

        bool evaded = false;
        double closest = std::numeric_limits<double>::max();
        for (int tick = 0; tick < 900 && ship.is_alive(); ++tick) {
            game.Update();
            if (!ship.is_alive()) break;
            if (ship.get<AIPilot>().behavior == AIBehavior::Evade) evaded = true;
            closest = std::min(closest, (ship.get<Transform>().pos - centre).length() - radius);
        }
        Require(evaded, "evade: a planet that is not the sector's heaviest body still trips the "
                        "danger reflex");
        Require(ship.is_alive(), "evade: a pilot aimed at a planet survives the encounter");
        Require(closest > 0., "evade: it never reaches the surface");
    }

    // The strategy layer's side of the same rule: a rock nobody could lift
    // off again is not a claim worth ordering, however close and unowned.
    const auto ordersALanding = [&](double gravityMultiplier) {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();
        flecs::entity planet = spawner.SpawnOrbitingPlanet("models/planets/simple"_id,
                                                           Vector2d{0., 0.}, 1e-9, 800., 1.0, 0.0);
        planet.get_mut<GravitySource>().multiplier *= gravityMultiplier;
        const Vector2d center = planet.get<Transform>().pos;

        flecs::entity leader = spawner.SpawnAILeader("models/ships/fighter-1"_id,
                                                     center + Vector2d{0., 1200.}, TeamId::Red,
                                                     game.GetAIPresets().Default());
        bool landOrdered = false;
        for (int tick = 0; tick < 30 && leader.is_alive() && !landOrdered; ++tick) {
            game.Update();
            if (leader.is_alive()) {
                landOrdered = leader.get<AIPilot>().order.kind == AIOrderKind::Land;
            }
        }
        return landOrdered;
    };

    Require(ordersALanding(1.0), "takeoff: a leader still claims a planet it can lift off from");
    Require(!ordersALanding(20.0),
            "takeoff: no leader is sent to land on a body its hull cannot out-thrust");

    fs.Shutdown();
}

// Coming home: the hull is only ever given back on a faction's own developed
// planet, and having somewhere to go is what stops a hurt pilot orbiting out
// the round. Also the reachability rule that made that orbit possible -- a
// distant enemy is a trip, not a shrug.
void TestRepairAndReachability()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }

    // Same descent as TestLandingAndClaiming, on a planet that either carries
    // this team's complex or is a bare rock. Returns the hull gained while
    // standing on it.
    const auto hullGainedWhileLanded = [&](bool developed) {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();

        flecs::entity planet = spawner.SpawnOrbitingPlanet("models/planets/simple"_id,
                                                           Vector2d{0., 0.}, 1e-9, 800., 1.0, 0.0);
        if (developed) BuildStartingComplex(spawner, planet, TeamId::Blue);

        const float planetRadius = planet.get<Planet>().radius
                * static_cast<float>(planet.get<Transform>().scale.x());
        flecs::entity ship = spawner.SpawnPlayer("models/ships/fighter-1"_id,
                                                 Vector2d{800., planetRadius + 15.});
        cpBody* shipBody = game.GetPhysicsSystem().GetBody(ship.get<PhysicsRef>()).cp.body.get();
        cpBodySetAngle(shipBody, CP_PI);
        cpBodySetVelocity(shipBody, cpv(0., -8.));

        Damageable& hull = ship.get_mut<Damageable>();
        hull.hp = hull.maxHp * 0.4f;
        const float hpBefore = hull.hp;

        for (int tick = 0; tick < 900 && ship.is_alive(); ++tick) {
            game.Update();
        }
        Require(ship.is_alive(), "repair: the descending ship survives touchdown (setup check)");
        Require(ship.get<LandingState>().landed, "repair: the ship is standing on the planet (setup check)");
        return ship.get<Damageable>().hp - hpBefore;
    };

    Require(hullGainedWhileLanded(/*developed=*/true) > 1.f,
            "repair: a hurt ship standing on its faction's developed planet gets hull back");
    Require(hullGainedWhileLanded(/*developed=*/false) == 0.f,
            "repair: a bare rock repairs nothing, however long you sit on it");

    // Field plating mends the hull under it wherever the ship is, at a rate its
    // tier sets -- the leak's counterweight, and the one repair that does not
    // want a planet. Flown nowhere near one, so home ground can't explain it.
    {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();
        const UpgradeCatalog& catalog = game.GetUpgradeCatalog();

        const UpgradeDef* plating = catalog.FindKind(UpgradeKind::Shield, ShieldType::Plating);
        const UpgradeDef* bubble = catalog.FindKind(UpgradeKind::Shield, ShieldType::Bubble);
        Require(plating != nullptr && bubble != nullptr, "repair: the pool has both emitters");

        float hullMaxHp = 0.f;
        const auto hullGainedOver = [&](const UpgradeDef* def, std::uint8_t rank, int ticks) {
            flecs::entity ship = spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{0., 0.});
            if (def) FitFree(catalog, *def, rank, ship.get_mut<ShipLoadout>());
            Damageable& hull = ship.get_mut<Damageable>();
            hullMaxHp = hull.maxHp;
            hull.hp = hull.maxHp * 0.4f;
            const float before = hull.hp;
            for (int tick = 0; tick < ticks; ++tick) game.Update();
            const float gained = ship.get<Damageable>().hp - before;
            ship.destruct();
            return gained;
        };

        const float second = static_cast<float>(1.0 / Game::PHYSICS_DELTA);
        Require(hullGainedOver(nullptr, 0, static_cast<int>(second)) == 0.f,
                "repair: an unshielded hull in open space mends nothing");
        Require(hullGainedOver(bubble, bubble->maxLevel, static_cast<int>(second)) == 0.f,
                "repair: a bubble covers the hull, it does not mend it");

        float mendedBefore = 0.f;
        for (std::uint8_t rank = 1; rank <= plating->maxLevel; ++rank) {
            const float mended = hullGainedOver(plating, rank, static_cast<int>(second));
            Require(mended > mendedBefore, "repair: each plating tier mends the hull faster");
            mendedBefore = mended;
        }

        // The authored headline: top plating walks a full hull back in 30s, so
        // the 60% this hull is missing takes 18 -- worth flying out with, not
        // worth standing still for.
        const float over18s = hullGainedOver(plating, plating->maxLevel, static_cast<int>(18.f * second));
        const float missing = hullMaxHp * 0.6f;
        Require(std::abs(over18s - missing) < 1.f,
                "repair: top plating mends a full hull in the authored 30 seconds");
        Require(hullGainedOver(plating, plating->maxLevel, static_cast<int>(40.f * second)) <= missing,
                "repair: and stops at full rather than climbing past it");
    }

    // Reachability: an enemy well past the old engage-range cutoff used to
    // leave a pilot circling the nearest body instead. Nothing else is in
    // this scene, so Orbit here would be a pilot with nowhere to be.
    {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();
        spawner.SpawnOrbitingPlanet("models/planets/simple"_id, Vector2d{0., 0.}, 1e-9, 800., 1.0, 0.0);

        flecs::entity ship = spawner.SpawnAIShip("models/ships/fighter-1"_id, Vector2d{400., 0.},
                                                 game.GetAIPresets().Default());
        ship.get_mut<AIPilot>().personality.dangerLookaheadSteps = 0;
        flecs::entity enemy = spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{12000., 0.});

        const double rangeBefore = (enemy.get<Transform>().pos - ship.get<Transform>().pos).length();
        for (int tick = 0; tick < 900; ++tick) {
            game.Update();
        }
        const double rangeAfter = (enemy.get<Transform>().pos - ship.get<Transform>().pos).length();

        Require(ship.get<AIPilot>().behavior == AIBehavior::Intercept,
                "reachability: an enemy past the old engage range is still flown to");
        Require(rangeAfter < rangeBefore - 100.0,
                "reachability: and the pilot actually closes on it");
    }

    fs.Shutdown();
}

// Phase 5 tactics (docs/ai-ships.md): weapon discipline, jinking under fire,
// breaking off when hurt, and one leader per objective.
//
// Every pilot here is dialled to isolate the rule under test -- a huge
// engage/fire range so the tactical pick is never about distance, a zero
// danger lookahead so gravity-well evasion (which runs every tick and
// overrides everything) can't quietly explain a result, and an explicit
// flee threshold so hull never enters into it except where it is the point.
// An upgrade an AI never spends is an upgrade it may as well not have
// collected: a pilot handed a rack launches from it, and one handed the
// overburn burns with it.
void TestAIUsesItsUpgrades()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }

    // Missiles: a loaded rack, an enemy inside its envelope, nothing in the
    // way. The pilot has to choose to launch -- nothing here presses it.
    {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();

        flecs::entity shooter = spawner.SpawnAIShip("models/ships/fighter-1"_id, Vector2d{-500., 0.},
                                                    game.GetAIPresets().Default());
        shooter.get_mut<ShipLoadout>().levels.missileTier = 1;
        SetMissileBay(shooter.get_mut<ShipLoadout>(), 0, true); // no bay, no launcher
        shooter.get_mut<ShipLoadout>().missileAmmo = 4;
        spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{500., 0.}, TeamId::Blue);

        bool launched = false;
        for (int tick = 0; tick < 900 && !launched; ++tick) {
            game.Update();
            game.GetRegistry().each([&](flecs::entity, const Missile&) { launched = true; });
        }
        Require(launched, "ai upgrades: a pilot with a loaded rack actually launches from it");
        Require(shooter.get<ShipLoadout>().missileAmmo < 10,
                "ai upgrades: and the launch comes off its own ammo");
    }

    // The overburn, on the condition it exists for: a pilot whose target is
    // behind it at cruising speed has to kill all of that speed before it can
    // do anything at all -- the same correction an arrival at a planet is,
    // and far past what the engine alone delivers in reasonable time.
    {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();

        flecs::entity pilot = spawner.SpawnAIShip("models/ships/fighter-1"_id, Vector2d{0., 0.},
                                                  game.GetAIPresets().Default(),
                                                  /*velocity=*/Vector2d{1500., 0.});
        spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{-3000., 0.}, TeamId::Blue);

        const UpgradeDef* bank = game.GetUpgradeCatalog().FindKind(UpgradeKind::Capacitor);
        Require(bank != nullptr, "ai upgrades: the pool has a capacitor upgrade (setup check)");
        FitFree(game.GetUpgradeCatalog(), *bank, 1, pilot.get_mut<ShipLoadout>());

        bool burned = false;
        for (int tick = 0; tick < 600 && pilot.is_alive() && !burned; ++tick) {
            game.Update();
            burned = pilot.is_alive() && pilot.get<Controls>().boosting;
        }
        Require(burned, "ai upgrades: a pilot facing a correction it cannot brake spends a burn");

        // And an identical pilot without the upgrade simply never does.
        Game bare(fs);
        flecs::entity unfitted =
                bare.GetEntitySpawner().SpawnAIShip("models/ships/fighter-1"_id, Vector2d{0., 0.},
                                                    bare.GetAIPresets().Default(),
                                                    /*velocity=*/Vector2d{1500., 0.});
        bare.GetEntitySpawner().SpawnPlayer("models/ships/fighter-1"_id, Vector2d{-3000., 0.},
                                            TeamId::Blue);
        bool bareBurned = false;
        for (int tick = 0; tick < 600 && unfitted.is_alive() && !bareBurned; ++tick) {
            bare.Update();
            bareBurned = unfitted.is_alive() && unfitted.get<Controls>().boosting;
        }
        Require(!bareBurned, "ai upgrades: one that never collected it burns nothing");
    }

    fs.Shutdown();
}

void TestAITactics()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }

    const auto configure = [](flecs::entity ship, double fleeHealthFraction, double jinkSpeed) {
        AIPersonality& p = ship.get_mut<AIPilot>().personality;
        p.engageRange = 1e6;
        p.fireRange = 1e6;
        p.fireTolerance = 1.0;
        p.aimJitter = 0.0;
        p.reactionJitter = 0.0;
        p.fireInterval = 1;
        p.decisionInterval = 1;
        p.dangerLookaheadSteps = 0;
        p.fleeHealthFraction = fleeHealthFraction;
        p.jinkSpeed = jinkSpeed;
    };

    // Weapon discipline: the same standoff twice, once with a planet sitting
    // on the firing solution and once without.
    const auto shotsOverTicks = [&](bool blocker) {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();
        if (blocker) {
            spawner.SpawnOrbitingPlanet("models/planets/simple"_id, Vector2d{0., 0.}, 1e-9, 1e-6, 1.0, 0.0);
        }

        flecs::entity shooter = spawner.SpawnAIShip("models/ships/fighter-1"_id, Vector2d{-600., 0.},
                                                    game.GetAIPresets().Default());
        configure(shooter, /*fleeHealthFraction=*/0.0, /*jinkSpeed=*/0.0);
        spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{600., 0.});

        int shots = 0;
        for (int tick = 0; tick < 150; ++tick) {
            game.Update();
            game.GetRegistry().each([&](flecs::entity, const Bullet&) { ++shots; });
        }
        return shots;
    };

    Require(shotsOverTicks(/*blocker=*/false) > 0,
            "ai tactics: a pilot with a clear line of sight opens fire (setup check)");
    Require(shotsOverTicks(/*blocker=*/true) == 0,
            "ai tactics: a pilot holds fire while a planet sits on the firing solution");

    // Jinking: a stationary target on the +X axis means a straight closing
    // run has no lateral velocity at all, so any Y motion is the weave.
    {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();
        // Facing the target already (ship forward is local -Y, so rot = pi/2
        // is heading +X): the swing onto course is itself lateral motion, and
        // this test is about what happens after that.
        flecs::entity ship = spawner.SpawnAIShip("models/ships/fighter-1"_id, Vector2d{0., 0.},
                                                 game.GetAIPresets().Default(), Vector2d{}, 3.14159265358979323846 / 2.);
        configure(ship, /*fleeHealthFraction=*/0.0, /*jinkSpeed=*/40.0);
        spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{1500., 0.});

        double straightLateral = 0.0;
        for (int tick = 0; tick < 120; ++tick) {
            game.Update();
            if (tick >= 40) {
                straightLateral = std::max(straightLateral, std::abs(ship.get<Transform>().vel.y()));
            }
        }

        // The hit itself, minus the bullet: AIPilotSystem reads "under fire"
        // off a hull drop between ticks.
        ship.get_mut<Damageable>().hp -= 5.f;

        double jinkedLateral = 0.0;
        for (int tick = 0; tick < 90; ++tick) {
            game.Update();
            jinkedLateral = std::max(jinkedLateral, std::abs(ship.get<Transform>().vel.y()));
        }

        std::printf("JINKDBG straight=%.3f jinked=%.3f\n", straightLateral, jinkedLateral);
        Require(straightLateral < 5.0, "ai tactics: an unmolested closing run flies straight");
        // Well under the commanded jinkSpeed: a reversal every jinkPeriod ticks
        // is not long enough to actually reach the lateral velocity asked
        // for, which is what keeps the weave a weave rather than a detour.
        Require(jinkedLateral > 8.0, "ai tactics: taking a hit makes the closing run weave");
    }

    // Breaking off: below the flee threshold a pilot opens the range instead
    // of closing it. Same scene at full hull is the control.
    const auto rangeChange = [&](float hullFraction) {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();
        flecs::entity ship = spawner.SpawnAIShip("models/ships/fighter-1"_id, Vector2d{0., 0.},
                                                 game.GetAIPresets().Default());
        configure(ship, /*fleeHealthFraction=*/0.3, /*jinkSpeed=*/0.0);
        ship.get_mut<AIPilot>().personality.fleeRange = 700.0;

        Damageable& hull = ship.get_mut<Damageable>();
        hull.hp = hull.maxHp * hullFraction;

        flecs::entity enemy = spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{300., 0.});
        // This block measures where the pilot goes, not how hard it shoots,
        // and the healthy control closes to point-blank with its gun on the
        // target: without the armour the run ends early with nothing left to
        // measure the range against.
        Damageable& targetHull = enemy.get_mut<Damageable>();
        targetHull.maxHp = 1e6f;
        targetHull.hp = targetHull.maxHp;

        const double before = (enemy.get<Transform>().pos - ship.get<Transform>().pos).length();
        for (int tick = 0; tick < 240; ++tick) game.Update();
        const double after = (enemy.get<Transform>().pos - ship.get<Transform>().pos).length();

        Require(ship.is_alive(), "ai tactics: the pilot survives the run (setup check)");
        return std::make_pair(after - before, ship.get<AIPilot>().behavior);
    };

    const auto hurt = rangeChange(0.15f);
    const auto healthy = rangeChange(1.0f);
    Require(hurt.second == AIBehavior::Flee, "ai tactics: a hurt pilot with a threat close breaks off");
    Require(hurt.first > 100.0, "ai tactics: breaking off actually opens the range");
    Require(healthy.second == AIBehavior::Intercept,
            "ai tactics: a healthy pilot in the same spot still closes (setup check)");

    // Crowding: two leaders side by side, a near planet and a far one. Both
    // score the near one highest on its own merits; the second to decide has
    // to see it as taken.
    {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();
        flecs::entity near = spawner.SpawnOrbitingPlanet("models/planets/simple"_id, Vector2d{0., 0.},
                                                         1e-9, 1000., 1.0, 0.0);
        flecs::entity far = spawner.SpawnOrbitingPlanet("models/planets/simple"_id, Vector2d{0., 0.},
                                                        1e-9, 3000., 1.0, 0.5);

        flecs::entity first = spawner.SpawnAILeader("models/ships/fighter-1"_id, Vector2d{0., 0.},
                                                    TeamId::Red, game.GetAIPresets().Default());
        flecs::entity second = spawner.SpawnAILeader("models/ships/fighter-1"_id, Vector2d{0., 50.},
                                                     TeamId::Red, game.GetAIPresets().Default());
        game.Update();

        Require(first.get<AIStrategy>().goal == AIGoal::ClaimPlanet
                        && second.get<AIStrategy>().goal == AIGoal::ClaimPlanet,
                "ai tactics: both leaders want a planet with nothing else on offer (setup check)");
        Require(first.get<AIStrategy>().subject == near,
                "ai tactics: the first leader to decide takes the nearer planet");
        Require(second.get<AIStrategy>().subject == far,
                "ai tactics: the second leader goes elsewhere rather than doubling up");
    }

    // Closing speed: maxSpeed is a dogfighting cap, and a pilot that holds it
    // for a whole approach never arrives. Well outside the merge range it
    // should be flying a transit.
    {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();
        flecs::entity ship = spawner.SpawnAIShip("models/ships/fighter-1"_id, Vector2d{0., 0.},
                                                 game.GetAIPresets().Default());
        configure(ship, /*fleeHealthFraction=*/0.0, /*jinkSpeed=*/0.0);
        spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{5000., 0.});

        const double cruiseCap = ship.get<AIPilot>().guidance.maxSpeed;
        double peak = 0.0;
        for (int tick = 0; tick < 600; ++tick) {
            game.Update();
            peak = std::max(peak, ship.get<Transform>().vel.length());
        }
        Require(peak > cruiseCap * 1.8,
                "ai tactics: a long approach is flown at transit speed, not at the dogfight cap");
    }

    // Wing orders: fodder has no AIStrategy of its own and takes the team
    // leader's objective, with a claim handed on as cover rather than as a
    // second ship aiming at the same rock.
    {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();
        flecs::entity planet = spawner.SpawnOrbitingPlanet("models/planets/simple"_id, Vector2d{0., 0.},
                                                           1e-9, 1500., 1.0, 0.0);
        flecs::entity leader = spawner.SpawnAILeader("models/ships/fighter-1"_id, Vector2d{0., 0.},
                                                     TeamId::Red, game.GetAIPresets().Default());
        flecs::entity wingman = spawner.SpawnAIShip("models/ships/fighter-1"_id, Vector2d{100., 0.},
                                                    game.GetAIPresets().Default());
        game.Update();
        game.Update();

        Require(leader.get<AIPilot>().order.kind == AIOrderKind::Land
                        && leader.get<AIPilot>().order.subject == planet,
                "ai tactics: the leader takes the claim itself (setup check)");
        Require(wingman.get<AIPilot>().order.kind == AIOrderKind::Patrol
                        && wingman.get<AIPilot>().order.subject == planet,
                "ai tactics: the wing covers the leader's claim instead of doubling up on it");
    }

    // An order names its subject outright, and the pilot has to actually go
    // there. engageRange is the heuristic for picking a dogfight opponent out
    // of the air; letting it gate an ordered attack too leaves a leader
    // reading "InterceptFreighter" while it patrols a well thousands of units
    // from the freighter, which is what a live session showed.
    {
        Game game(fs);
        EntitySpawner& spawner = game.GetEntitySpawner();
        flecs::entity planet = spawner.SpawnOrbitingPlanet("models/planets/simple"_id,
                                                           Vector2d{14000., 0.}, 1e-9, 1., 1.0, 0.0);
        planet.set<Team>(Team{TeamId::Blue});
        flecs::entity freighter = spawner.SpawnFreighter("models/ships/freighter-0"_id,
                                                         Vector2d{9000., 0.}, TeamId::Blue, planet,
                                                         BuildOrder::Base);

        flecs::entity leader = spawner.SpawnAILeader("models/ships/fighter-1"_id, Vector2d{0., 0.},
                                                     TeamId::Red, game.GetAIPresets().Default());
        // Only the raid is on the table, so the goal under test is the one it
        // picks; the freighter starts well outside the default engageRange.
        leader.get_mut<AIStrategy>().weights = AIStrategyWeights{0., 0., 0., 1., 0.};
        AIPersonality& raider = leader.get_mut<AIPilot>().personality;
        raider.dangerLookaheadSteps = 0;
        raider.fleeHealthFraction = 0.0;

        const auto gap = [&] {
            return (freighter.get<Transform>().pos - leader.get<Transform>().pos).length();
        };
        const double before = gap();
        Require(before > raider.engageRange,
                "ai tactics: the raid starts outside engage range (setup check)");

        for (int tick = 0; tick < 600 && freighter.is_alive(); ++tick) game.Update();

        Require(leader.get<AIStrategy>().goal == AIGoal::InterceptFreighter,
                "ai tactics: the raid is the goal it holds (setup check)");
        Require(!freighter.is_alive() || gap() < before - 1000.0,
                "ai tactics: an ordered attack closes on its subject from outside engage range");
    }

    fs.Shutdown();
}

// A peer predicts and draws its own shots locally (ClientPrediction::Step),
// so the server must not also send it the authoritative copies -- otherwise
// the same shot draws twice, ~14 ticks apart (own ship renders ahead by
// INPUT_LEAD_TICKS, replicated entities behind by the interpolation delay).
// Everyone else's bullets must still come through.
void TestOwnBulletSuppression()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    Game game(fs);
    EntitySpawner& spawner = game.GetEntitySpawner();

    flecs::entity mine = spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{0., 0.});
    flecs::entity theirs = spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{5000., 0.});

    flecs::entity myBullet =
            spawner.SpawnBullet("models/bullets/bullet-0"_id, Vector2d{40., 0.}, Vector2d{100., 0.}, true);
    myBullet.emplace<Bullet>(3.0, TeamId::Blue, 10.f, mine.get<NetId>().value);

    flecs::entity theirBullet =
            spawner.SpawnBullet("models/bullets/bullet-0"_id, Vector2d{5040., 0.}, Vector2d{100., 0.}, true);
    theirBullet.emplace<Bullet>(3.0, TeamId::Red, 10.f, theirs.get<NetId>().value);

    const auto contains = [](const SnapshotData& s, std::uint32_t netId) {
        return std::any_of(s.entities.begin(), s.entities.end(),
                           [&](const EntityState& e) { return e.netId == netId; });
    };

    SnapshotData unfiltered;
    GatherSnapshot(game.GetRegistry(), game.GetEventQueue(), 0, 0, unfiltered);
    Require(contains(unfiltered, myBullet.get<NetId>().value)
                    && contains(unfiltered, theirBullet.get<NetId>().value),
            "bullet suppression: an unfiltered snapshot carries every bullet (setup check)");

    SnapshotData forMe;
    GatherSnapshot(game.GetRegistry(), game.GetEventQueue(), 0, 0, forMe, mine.get<NetId>().value);
    Require(!contains(forMe, myBullet.get<NetId>().value),
            "bullet suppression: my own bullet is omitted from my own snapshot");
    Require(contains(forMe, theirBullet.get<NetId>().value),
            "bullet suppression: another ship's bullet still reaches me");
    Require(contains(forMe, mine.get<NetId>().value),
            "bullet suppression: only bullets are filtered, my ship itself still replicates");

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

// A refitted, shot-up hull has to reach a client whole. GatherSnapshot reading
// a field and the wire carrying it is not enough -- ApplyEntityShipState has to
// put it back, and every field it forgets is a readout stuck at whatever the
// ship spawned with. That is exactly what shipped, twice: the own hull's copy
// of this mapping never learned its mounts, its bays or the cannon/missile
// tiers (a cannon could be bought and never appear), and it never learned `hp`
// (the hull bar sat at 100% until the ship died out from under it).
void TestLoadoutReplication()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    Game game(fs);
    EntitySpawner& spawner = game.GetEntitySpawner();

    flecs::entity server = spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{0., 0.});
    flecs::entity client = spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{5000., 0.});

    // Every field distinct from both the spawn default and its neighbours, so
    // a copy that drops one or crosses two over cannot pass.
    ShipLoadout& refit = server.get_mut<ShipLoadout>();
    refit.mounts[0] = MountArm::Heavy;
    refit.mounts[1] = MountArm::Light;
    // All three arms, so the range check the mount byte is read through cannot
    // quietly clamp the newest of them back to an empty hole.
    refit.mounts[2] = MountArm::Laser;
    SetMissileBay(refit, 1, true);
    refit.missileAmmo = 13;
    refit.cannonAmmo = 57;
    refit.levels.fireRate = 2;
    refit.levels.gunTier = 3;
    refit.levels.cannonTier = 2;
    refit.levels.missileTier = 3;
    refit.ammoBays[0] = AmmoPool::Missile;
    refit.ammoBays[1] = AmmoPool::Cannon;
    SyncAmmoStoreCounts(refit);
    refit.levels.engine = 3;
    refit.levels.capacitor = 2;
    refit.levels.laserTier = 1;
    refit.levels.shield = 3;
    refit.levels.shieldType = ShieldType::Plating;
    refit.shieldHp = 42.f;
    server.get_mut<ResearchAccess>().atLab = true;
    // Not part of the loadout, and the reason this hull is shot up: damage is
    // resolved server-side only, so the wire is the sole thing that ever moves
    // a client's hull bar.
    server.get_mut<Damageable>().hp = 37.f;

    SnapshotData gathered;
    GatherSnapshot(game.GetRegistry(), game.GetEventQueue(), game.GetStep(), 0, gathered);

    ByteWriter wire;
    SerializeSnapshot(gathered, wire);
    ByteReader reader(wire.Data(), wire.Size());
    SnapshotData parsed;
    Require(ReadSnapshot(reader, parsed), "loadout replication: the snapshot parses");

    const std::uint32_t netId = server.get<NetId>().value;
    const auto it = std::find_if(parsed.entities.begin(), parsed.entities.end(),
                                 [&](const EntityState& e) { return e.netId == netId; });
    Require(it != parsed.entities.end(), "loadout replication: the refitted hull is in the snapshot");

    ApplyEntityShipState(client, *it);

    Require(client.get<Damageable>().hp == server.get<Damageable>().hp,
            "loadout replication: hull damage, which nothing on a client predicts");

    const ShipLoadout& applied = client.get<ShipLoadout>();
    Require(applied.mounts == refit.mounts, "loadout replication: what each mount is armed with");
    Require(applied.missileBays == refit.missileBays, "loadout replication: which bays carry a launcher");
    Require(applied.missileAmmo == refit.missileAmmo && applied.cannonAmmo == refit.cannonAmmo,
            "loadout replication: rounds left in both magazines");
    Require(applied.levels.fireRate == refit.levels.fireRate, "loadout replication: the feed's rank");
    Require(applied.levels.gunTier == refit.levels.gunTier, "loadout replication: the light line's rank");
    Require(applied.levels.cannonTier == refit.levels.cannonTier, "loadout replication: the heavy line's rank");
    Require(applied.levels.missileTier == refit.levels.missileTier, "loadout replication: the bay's rank");
    Require(applied.ammoBays == refit.ammoBays,
            "loadout replication: which locker rides in which stowage bay");
    for (std::size_t i = 0; i < NUM_AMMO_POOLS; ++i) {
        Require(applied.levels.ammoStore[i] == refit.levels.ammoStore[i],
                "loadout replication: ...and the count of each that falls out of it");
    }
    Require(applied.levels.engine == refit.levels.engine, "loadout replication: the drive's rank");
    Require(applied.levels.capacitor == refit.levels.capacitor,
            "loadout replication: the bank's rank");
    Require(applied.levels.laserTier == refit.levels.laserTier,
            "loadout replication: the beam emitter's rank");
    Require(applied.levels.shield == refit.levels.shield
                    && applied.levels.shieldType == refit.levels.shieldType,
            "loadout replication: the emitter fitted and its rank");
    Require(applied.shieldHp == refit.shieldHp, "loadout replication: shield charge");
    Require(client.get<ResearchAccess>().atLab, "loadout replication: yard access");

    // The board resolves what it draws through the catalog, so a rank that
    // arrived is worth nothing if the stats it feeds don't come out the same.
    const UpgradeCatalog& catalog = game.GetUpgradeCatalog();
    const ShipStats expected = catalog.ResolveStats(refit.levels);
    const ShipStats got = catalog.ResolveStats(applied.levels);
    Require(got.cannon == expected.cannon && got.missile == expected.missile
                    && got.cannonCapacity == expected.cannonCapacity
                    && got.missileCapacity == expected.missileCapacity
                    && got.capacitorCharge == expected.capacitorCharge,
            "loadout replication: a client resolves the same weapons, magazines and bank");

    fs.Shutdown();
}

// docs/networking-plan.md 3.2-3.4: a NetServer/NetClient pair talking over a
// LoopbackTransport (no sockets -- proves the protocol/spawn/broadcast wiring
// itself, independent of whatever real transport Phase 3.1 eventually picks).
// Runs entirely inside RunSimulation()'s own Game, so it shares that Game's
// determinism gate rather than needing a second one.
void TestNetRoundtrip(Game& game)
{
    auto [serverTransport, clientTransport] = LoopbackTransport::CreatePair();
    NetServer server(game, *serverTransport);
    NetClient client(*clientTransport, "sim-test-client");
    // The client has no sector of its own; the seed reaches it in the welcome
    // and nowhere else, so the HUD can name the round it is playing.
    server.SetSectorSeed(0xC0FFEEu);

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
    Require(client.GetSectorSeed() == 0xC0FFEEu, "net: the welcome carries the served sector's seed");

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

    // Chat: the sender's own line comes back off the server attributed and
    // truncated by it, not echoed locally (see ChatSendPacket).
    client.SendChat(std::string(MAX_CHAT_TEXT + 20, 'x'));
    for (int i = 0; i < 3; ++i) {
        server.IngestInput(game.GetStep());
        game.Update();
        server.BroadcastSnapshot(game.GetStep());
        client.Update();
    }
    const std::vector<ChatMessagePacket> chat = client.TakeChatMessages();
    Require(chat.size() == 1, "chat: one line sent comes back exactly once");
    Require(chat[0].sender == "sim-test-client", "chat: the server attributes the line to its peer");
    Require(chat[0].text.size() == MAX_CHAT_TEXT, "chat: an overlong line is truncated on the wire");
    Require(client.TakeChatMessages().empty(), "chat: draining the inbox empties it");

    client.SendChat("");
    for (int i = 0; i < 3; ++i) {
        server.IngestInput(game.GetStep());
        game.Update();
        server.BroadcastSnapshot(game.GetStep());
        client.Update();
    }
    Require(client.TakeChatMessages().empty(), "chat: an empty line is never broadcast");
}

// A client whose stamped ticks land behind the server's must still fly. Its
// commands are late, not absent: dropping them latches the last-consumed
// flags, so the peer's ship stops answering the controls entirely while the
// player keeps flying a predicted copy that gets snapped back on the next
// reconciliation. Two ways that used to happen, both covered here -- the
// dead-man sweep advancing the *dedupe* watermark to the server's own tick
// (which then swallowed every command still in flight below it, silently and
// for as long as the peer stayed behind), and InputSystem discarding anything
// that missed its exact tick.
void TestLateInputStillFlies(Game& game)
{
    auto [serverTransport, clientTransport] = LoopbackTransport::CreatePair();
    NetServer server(game, *serverTransport);
    NetClient client(*clientTransport, "sim-test-late-client");

    for (int i = 0; i < 5; ++i) {
        server.IngestInput(game.GetStep());
        game.Update();
        server.BroadcastSnapshot(game.GetStep());
        client.Update();
    }
    Require(client.IsWelcomed(), "late input: client welcomed");

    const flecs::entity ship = game.GetEntitySpawner().EntityForNetId(client.GetYourShipNetId());
    Require(ship.is_alive(), "late input: server-side ship exists (test setup check)");

    // Well past NetServer::INPUT_TIMEOUT_TICKS, so the dead-man sweep fires
    // during the run rather than the run finishing inside its window.
    constexpr std::uint64_t LATE_BY = 5;
    for (int i = 0; i < 90; ++i) {
        server.IngestInput(game.GetStep());
        ControlFlags thrust{};
        thrust.thrustForward = true;
        client.SendInput(game.GetStep() - LATE_BY, thrust);
        game.Update();
        server.BroadcastSnapshot(game.GetStep());
        client.Update();
    }

    Require(ship.get<Controls>().actionFlags.thrustForward,
            "late input: a command past its stamped tick still reaches Controls");
    Require(ship.get<Transform>().vel.length() > 1.0,
            "late input: a peer stamping behind the server still actually moves");
}

// docs/networking-plan.md's known-gap fix: a peer whose ship dies must not
// become a permanent ghost. Own NetServer/NetClient pair (a second peer in
// `game`, independent of TestNetRoundtrip's) so killing this ship can't
// affect that test's assertions.
void TestPeerRespawn(Game& game)
{
    auto [serverTransport, clientTransport] = LoopbackTransport::CreatePair();
    NetServer server(game, *serverTransport);
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

// docs/gravity-well-mode-plan.md's Multiplayer wiring track: per-peer team
// assignment. LoopbackTransport is strictly one client/one server -- it
// can't host two peers on the same NetServer, so round-robin advancing
// across peers isn't provable here (it's a plain modulo counter; low risk).
// What this proves: an explicit request is honored, an unrequested peer
// auto-assigns to the roster's first team, SetPeerTeam reassigns live, and
// a respawn keeps the reassigned team rather than the original request.
void TestTeamAssignment(Game& game)
{
    auto [serverTransport, clientTransport] = LoopbackTransport::CreatePair();
    NetServer server(game, *serverTransport);
    NetClient client(*clientTransport, "sim-test-team-client");
    client.SetRequestedTeam(TeamId::Red);

    for (int i = 0; i < 5; ++i) {
        server.IngestInput(game.GetStep());
        game.Update();
        server.BroadcastSnapshot(game.GetStep());
        client.Update();
    }

    Require(client.IsWelcomed(), "team: client welcomed");
    Require(client.GetYourTeam() == TeamId::Red, "team: explicit requestedTeam is honored");

    const flecs::entity shipA = game.GetEntitySpawner().EntityForNetId(client.GetYourShipNetId());
    Require(shipA.get<Team>().id == TeamId::Red, "team: the server-side ship's Team component matches");

    // Explicit reassignment (server console command's underlying call).
    // PeerId isn't the ship's NetId -- LoopbackTransport always uses
    // SERVER_PEER (1) for its one connection, on both ends (see
    // LoopbackTransport::CreatePair); NetClient correctly has no reason to
    // expose its own PeerId (see transport.hpp), so this reuses that
    // transport-level constant directly instead.
    Require(server.SetPeerTeam(SERVER_PEER, TeamId::Cyan), "team: SetPeerTeam succeeds for a known peer");
    Require(shipA.get<Team>().id == TeamId::Cyan, "team: SetPeerTeam updates the live ship immediately");
    Require(!server.SetPeerTeam(9999, TeamId::Blue), "team: SetPeerTeam fails for an unknown peer");

    // A minimal Cyan complex: FactionSystem::TryRespawn (Phase 4) now
    // requires a site (friendly planet/High Port) and an affordable funder
    // (Base+Lab or High Port+Space Dock) before a fighter respawns at all --
    // without this, Cyan legitimately has nowhere to respawn (same as any
    // other faction that owns nothing), and the assertions below would never
    // fire. The Colony is part of that minimum too: a planet is only a
    // respawn site once it carries both a Base and a Colony, so a bare claim
    // (or a half-built complex) is not somewhere to come back to.
    flecs::entity cyanPlanet = game.GetEntitySpawner().SpawnOrbitingPlanet(
            "models/planets/simple"_id, Magnum::Vector2d{900., 900.}, 1e-9, 2000., 1.0, 0.0);
    cyanPlanet.set<Team>(Team{TeamId::Cyan});
    flecs::entity cyanBase = game.GetEntitySpawner().SpawnStructure(
            StructureType::Base, "models/structures/base"_id, cyanPlanet, TeamId::Cyan);
    game.GetEntitySpawner().SpawnStructure(StructureType::Lab, "models/structures/lab"_id, cyanPlanet, TeamId::Cyan);
    game.GetEntitySpawner().SpawnStructure(StructureType::Colony, "models/structures/colony"_id, cyanPlanet,
                                           TeamId::Cyan);
    cyanBase.get_mut<Structure>().finishedMaterials = 1000.f;

    // Kill A's ship and confirm the respawned one keeps the reassigned team
    // (not the original request, not the roster default).
    shipA.get_mut<Damageable>().hp = 0.f;
    for (int i = 0; i < 100; ++i) {
        server.IngestInput(game.GetStep());
        game.Update();
        server.BroadcastSnapshot(game.GetStep());
        client.Update();
    }
    const flecs::entity respawned = game.GetEntitySpawner().EntityForNetId(client.GetYourShipNetId());
    // Checked before the reads below rather than left to crash on them: a
    // respawn that never happened hands back a dead entity, and dereferencing
    // that segfaults instead of reporting which expectation broke.
    Require(respawned.is_alive(), "team: the respawned ship exists (test setup check)");
    Require(respawned != shipA, "team: the ship actually respawned (test setup check)");
    Require(respawned.get<Team>().id == TeamId::Cyan, "team: a respawn keeps the reassigned team, not the original");
    Require(client.GetYourTeam() == TeamId::Cyan, "team: the client's own re-welcome reflects the kept team too");
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

    NetServer server(game, serverTransport);
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

    // Not gravitaris-server's own default (17890): a dev server left running
    // on this machine would otherwise hold the port and fail this test for
    // reasons that have nothing to do with the code under test.
    constexpr std::uint16_t port = 17899;
    WebRtcServerTransport serverTransport(port);
    NetServer server(game, serverTransport);

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

    const AIPreset& preset = game.GetAIPresets().Default();
    game.GetEntitySpawner().SpawnAIShip("models/ships/fighter-1"_id, Magnum::Vector2d{200.0, -150.0}, preset);
    game.GetEntitySpawner().SpawnAIShip("models/ships/fighter-1"_id, Magnum::Vector2d{-200.0, 150.0}, preset);

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
    TestLateInputStillFlies(game);
    TestPeerRespawn(game);
    TestTeamAssignment(game);

    const RunResult result{game.ComputeStateChecksum(), eventHash, game.GetEventQueue().LatestSeq()};
    fs.Shutdown();
    return result;
}

// Keeping the target in the sights, which is the whole difference between a
// dogfight and a pirouette. A pilot already at its standoff range has nothing
// left to correct but station-keeping chatter, and a heading taken from that
// chatter points nowhere in particular -- so measure the heading itself rather
// than a kill, which a head-on merge would produce either way (there the
// velocity correction and the bearing to the target are the same vector, and
// the pilot lines up by accident).
void TestInterceptKeepsTargetInSights()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    Game game(fs);
    EntitySpawner& spawner = game.GetEntitySpawner();

    // Already inside firing range and past standoff, and crucially not on a
    // collision course: the target crosses the pilot's line of sight, so
    // "steer onto the velocity correction" and "point at the target" are two
    // genuinely different attitudes.
    flecs::entity ship = spawner.SpawnAIShip("models/ships/fighter-1"_id, Vector2d{0., 0.},
                                             game.GetAIPresets().Default());
    flecs::entity target = spawner.SpawnPlayer("models/ships/fighter-1"_id, Vector2d{150., 0.},
                                               TeamId::Blue, Vector2d{0., 70.});
    Damageable& targetHull = target.get_mut<Damageable>();
    targetHull.maxHp = 1e6f; // the aim is under test, not the damage
    targetHull.hp = targetHull.maxHp;

    int onTarget = 0;
    int measured = 0;
    for (int tick = 0; tick < 900; ++tick) {
        game.Update();
        if (ship.get<AIPilot>().behavior != AIBehavior::Intercept) continue;

        const Vector2d los = target.get<Transform>().pos - ship.get<Transform>().pos;
        if (los.length() > ship.get<AIPilot>().personality.fireRange) continue;

        const double heading = static_cast<double>(ship.get<Transform>().rot) - PI / 2.;
        double error = std::atan2(los.y(), los.x()) - heading;
        error = std::fmod(error + 3. * PI, 2. * PI) - PI;

        ++measured;
        if (std::abs(error) < 0.25) ++onTarget;
    }

    Require(measured > 300, "ai tactics: the pilot spends the run dogfighting in range (setup check)");
    Require(onTarget * 2 > measured,
            "ai tactics: a pilot holding standoff keeps its nose on the target most of the time, "
            "instead of flying the engagement broadside to it");
    fs.Shutdown();
}

// The single-player death/respawn path (Game::HandlePlayerRespawn), which is
// what a crash into a planet runs.
void TestPlayerRespawnAfterDeath()
{
    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "sim-test: filesystem init failed\n");
        std::exit(1);
    }
    Game game(fs);
    game.SetPlayerName("Crashy");
    game.Start();

    for (int life = 0; life < 3; ++life) {
        const std::optional<flecs::entity> before = game.GetPlayer();
        Require(before && before->is_alive(), "player respawn: there is a player to kill");
        const std::uint32_t pilotId = before->get<PilotRef>().pilotId;
        before->get_mut<Damageable>().hp = 0.f;

        for (int tick = 0; tick < 400; ++tick) {
            game.Update();
            const std::optional<flecs::entity> now = game.GetPlayer();
            if (now && *now != *before && now->is_alive()) break;
        }

        const std::optional<flecs::entity> after = game.GetPlayer();
        Require(after && after->is_alive() && *after != *before,
                "player respawn: a fresh hull turns up after the player dies");
        Require(after->get<Callsign>().name == "Crashy",
                "player respawn: the fresh hull flies under the same name");
        Require(after->get<PilotRef>().pilotId == pilotId,
                "player respawn: the fresh hull carries the same pilot identity");
    }

    fs.Shutdown();
}

} // namespace

int main()
{
    HasEnteredMain = true;

    TestByteStream();
    TestPredictedTickClock();
    TestInputLeadSizing();
    TestSnapshotInterpolation();
    TestOrbitReplication();
    TestClientPrediction();
    TestLandingAndClaiming();
    TestLandingChatterGrace();
    TestShipCollision();
    TestPlayerRespawnAfterDeath();
    TestStructures();
    TestFreighterEconomy();
    TestSelfDevelopment();
    TestPlanetsideStructureHits();
    TestUpgradeCatalog();
    TestHardpointMounts();
    TestPhysicsSlotStability();
    TestShields();
    TestHighPortDeck();
    TestHighPortApproach();
    TestResearch();
    TestResearchQueue();
    TestSunIsLethal();
    TestSunHeat();
    TestCheats();
    TestFriendlyFire();
    TestCapacitor();
    TestLasers();
    TestBeamDeflection();
    TestBeamAimsAtAPlace();
    TestBeamsInterceptMissiles();
    TestFactionDefeatAndWin();
    TestSectorGeneration();
    TestOwnBulletSuppression();
    TestLoadoutReplication();
    TestTakeoff();
    TestRepairAndReachability();
    TestAITactics();
    TestAIUsesItsUpgrades();
    TestInterceptKeepsTargetInSights();
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
