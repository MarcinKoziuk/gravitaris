// Headless determinism harness (ADR 0001's "cheap test that keeps this
// honest"): links game/ only, never cgame/GL/Audio -- if this target fails
// to link, something pulled a rendering/window/audio dependency into the
// sim, violating ADR 0001 constraint 1.
//
// Runs a short scripted fight twice from a fresh Game/filesystem each time
// and compares Game::ComputeStateChecksum() at the end. A mismatch means the
// sim depends on something outside (state, commands, dt) -- wall-clock,
// unseeded RNG, iteration-order-dependent hashing, etc.

#include <cstdio>
#include <cstdlib>
#include <cstdint>

#include <gravitaris/game/fs/filesystem-physfs.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/id.hpp>
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

    const RunResult result{game.ComputeStateChecksum(), eventHash, game.GetEventQueue().LatestSeq()};
    fs.Shutdown();
    return result;
}

} // namespace

int main()
{
    HasEnteredMain = true;

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

    std::printf("sim-test: OK, deterministic across %d ticks\n", TICKS);
    return 0;
}
