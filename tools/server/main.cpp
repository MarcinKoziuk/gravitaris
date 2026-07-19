// Headless dedicated server (docs/networking-plan.md 3.5.2): FilesystemPhysFS
// + Game + NetServer + WebRtcServerTransport, wall-clock-paced 60Hz loop.
// Links game/ only -- no cgame/GL/Audio -- the same ADR 0001 constraint
// gravitaris-sim-test enforces, so this binary can run on a headless box.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <gravitaris/game/fs/filesystem-physfs.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/net/net-server.hpp>
#include <gravitaris/game/net/webrtc-server-transport.hpp>
#include <gravitaris/game/scenario/classic-scenario.hpp>
#include <gravitaris/gravitaris.hpp>

using namespace Gravitaris;

// See tools/sim-test/main.cpp's identical definition: guards id.cpp's
// hashed-string mutex, which must stay unlocked during static
// initialization -- this target has no client, so it owns the definition.
namespace Gravitaris {
bool HasEnteredMain = false;
}

int main(int argc, char** argv)
{
    HasEnteredMain = true;

    std::uint16_t port = 17890;
    if (argc > 1) port = static_cast<std::uint16_t>(std::atoi(argv[1]));

    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "gravitaris-server: filesystem init failed\n");
        return 1;
    }

    // No local player: Game::Start() would spawn one nobody controls or
    // replicates meaningfully. BuildClassicScenario alone gives the solar
    // system (planets, AI ships) without it -- players arrive entirely
    // through NetServer's ClientHello handling.
    Game game(fs);
    BuildClassicScenario(game.GetEntitySpawner());

    WebRtcServerTransport transport(port);
    NetServer server(game.GetRegistry(), game.GetEntitySpawner(), game.GetEventQueue(), transport);

    LOG(info) << "gravitaris-server: listening on ws://0.0.0.0:" << port;

    const auto tickDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(Game::PHYSICS_DELTA));
    auto nextTick = std::chrono::steady_clock::now();
    for (;;) {
        server.IngestInput(game.GetStep());
        game.Update();
        server.BroadcastSnapshot(game.GetStep());

        nextTick += tickDuration;
        std::this_thread::sleep_until(nextTick);
    }
}
