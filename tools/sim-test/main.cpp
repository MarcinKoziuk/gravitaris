// Headless determinism harness (ADR 0001's "cheap test that keeps this
// honest"): links game/ only, never cgame/GL/Audio -- if this target fails
// to link, something pulled a rendering/window/audio dependency into the
// sim, violating ADR 0001 constraint 1.
//
// Runs a short scripted fight twice from a fresh Game/filesystem each time
// and compares Game::ComputeStateChecksum() at the end. A mismatch means the
// sim depends on something outside (state, commands, dt) -- wall-clock,
// unseeded RNG, iteration-order-dependent hashing, etc.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#include <gravitaris/game/fs/filesystem-physfs.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/id.hpp>
#include <gravitaris/game/net/byte-stream.hpp>
#include <gravitaris/game/net/snapshot.hpp>
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

    const RunResult result{game.ComputeStateChecksum(), eventHash, game.GetEventQueue().LatestSeq()};
    fs.Shutdown();
    return result;
}

} // namespace

int main()
{
    HasEnteredMain = true;

    TestByteStream();

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
