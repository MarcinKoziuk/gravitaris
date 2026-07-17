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

std::uint64_t RunSimulation()
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

    for (int i = 0; i < TICKS; ++i) {
        game.Update();
    }

    const std::uint64_t checksum = game.ComputeStateChecksum();
    fs.Shutdown();
    return checksum;
}

} // namespace

int main()
{
    HasEnteredMain = true;

    const std::uint64_t a = RunSimulation();
    const std::uint64_t b = RunSimulation();

    std::printf("sim-test: run 1 checksum = 0x%016llx\n", static_cast<unsigned long long>(a));
    std::printf("sim-test: run 2 checksum = 0x%016llx\n", static_cast<unsigned long long>(b));

    if (a != b) {
        std::fprintf(stderr, "sim-test: MISMATCH -- sim is not deterministic across runs\n");
        return 1;
    }

    std::printf("sim-test: OK, deterministic across %d ticks\n", TICKS);
    return 0;
}
