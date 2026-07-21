// Headless dedicated server (docs/networking-plan.md 3.5.2): FilesystemPhysFS
// + Game + NetServer + WebRtcServerTransport, wall-clock-paced 60Hz loop.
// Links game/ only -- no cgame/GL/Audio -- the same ADR 0001 constraint
// gravitaris-sim-test enforces, so this binary can run on a headless box.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/fs/filesystem-physfs.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/gnc/ai-personality-presets.hpp>
#include <gravitaris/game/id.hpp>
#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/net/net-server.hpp>
#include <gravitaris/game/net/webrtc-server-transport.hpp>
#include <gravitaris/game/scenario/classic-scenario.hpp>
#include <gravitaris/game/scenario/starting-complex.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/gravitaris.hpp>

using namespace Gravitaris;

namespace {

// M_PI is a POSIX/GNU <cmath> extension, not standard C++ -- MSVC doesn't
// define it without _USE_MATH_DEFINES set before every <cmath> include.
constexpr double PI = 3.14159265358979323846;

// Background stdin reader: std::cin has no non-blocking read, so the main
// tick loop can't just poll it directly without stalling on the syscall. A
// dedicated thread blocks on getline() and hands finished lines to the main
// loop through a mutexed queue, drained once per tick alongside
// NetServer::IngestInput. *Very* minimal by design (docs/networking-plan N5)
// -- no history/line-editing (replxx/isocline would add that later if ever
// wanted; skipped for now).
class StdinCommandQueue {
public:
    StdinCommandQueue()
        : m_thread([this] { ReadLoop(); })
    {
        m_thread.detach();
    }

    // Pops all lines queued since the last call, oldest first.
    std::deque<std::string> Drain()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::deque<std::string> lines = std::move(m_lines);
        m_lines.clear();
        return lines;
    }

private:
    void ReadLoop()
    {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lines.push_back(std::move(line));
        }
    }

    std::thread m_thread;
    std::mutex m_mutex;
    std::deque<std::string> m_lines;
};

std::optional<AIPersonalityPreset> ParsePreset(const std::string& name)
{
    if (name == "balanced") return AIPersonalityPreset::Balanced;
    if (name == "aggressive") return AIPersonalityPreset::Aggressive;
    if (name == "cautious") return AIPersonalityPreset::Cautious;
    if (name == "sniper") return AIPersonalityPreset::Sniper;
    if (name == "reckless") return AIPersonalityPreset::Reckless;
    return std::nullopt;
}

std::optional<TeamId> ParseTeam(const std::string& name)
{
    if (name == "blue") return TeamId::Blue;
    if (name == "red") return TeamId::Red;
    if (name == "green") return TeamId::Green;
    if (name == "yellow") return TeamId::Yellow;
    if (name == "magenta") return TeamId::Magenta;
    if (name == "cyan") return TeamId::Cyan;
    return std::nullopt;
}

// `spawn [count] [preset]`, `list`, `team <peer-id> <color>`, `quit` -- see
// docs/networking-plan N5 and docs/gravity-well-mode-plan.md's Multiplayer
// wiring track (explicit team control until a round-setup UI exists).
void HandleCommand(const std::string& line, Game& game, NetServer& server, bool& running)
{
    std::istringstream iss(line);
    std::string verb;
    iss >> verb;

    if (verb.empty()) {
        return;
    } else if (verb == "spawn") {
        int count = 1;
        std::string presetName = "balanced";
        iss >> count;
        iss >> presetName;
        const auto preset = ParsePreset(presetName);
        if (!preset) {
            std::fprintf(stderr, "spawn: unknown preset '%s' (balanced|aggressive|cautious|sniper|reckless)\n",
                         presetName.c_str());
            return;
        }
        count = std::clamp(count, 1, 100);
        const id_t shipModel = "models/ships/fighter-1"_id;
        for (int i = 0; i < count; ++i) {
            const double angle = (2. * PI * static_cast<double>(i)) / static_cast<double>(count);
            const Vector2d pos{600. * std::cos(angle), 600. * std::sin(angle)};
            game.GetEntitySpawner().SpawnAIShip(shipModel, pos, *preset);
        }
        std::printf("spawned %d %s ship(s)\n", count, presetName.c_str());
    } else if (verb == "list") {
        std::printf("peers: %zu\n", server.PeerCount());
    } else if (verb == "team") {
        std::uint32_t peer = 0;
        std::string colorName;
        iss >> peer >> colorName;
        const auto team = ParseTeam(colorName);
        if (!team) {
            std::fprintf(stderr, "team: unknown color '%s' (blue|red|green|yellow|magenta|cyan)\n",
                         colorName.c_str());
            return;
        }
        if (!server.SetPeerTeam(peer, *team)) {
            std::fprintf(stderr, "team: no such peer %u\n", peer);
            return;
        }
        std::printf("peer %u set to %s\n", peer, colorName.c_str());
    } else if (verb == "quit") {
        running = false;
    } else {
        std::fprintf(stderr, "unknown command '%s' (spawn [count] [preset]|list|team <peer-id> <color>|quit)\n",
                     verb.c_str());
    }
}

} // namespace

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
    const flecs::entity homePlanet = BuildClassicScenario(game.GetEntitySpawner());
    // Single, shared starting complex for now (docs/gravity-well-mode-plan.md
    // Phase 2) -- per-faction starting planets are Phase 6's job.
    BuildStartingComplex(game.GetEntitySpawner(), homePlanet, TeamId::Blue);

    WebRtcServerTransport transport(port);
    NetServer server(game.GetRegistry(), game.GetEntitySpawner(), game.GetEventQueue(), game.GetFactionSystem(),
                     transport);

    LOG(info) << "gravitaris-server: listening on ws://0.0.0.0:" << port;
    std::printf("commands: spawn [count] [preset]|list|team <peer-id> <color>|quit\n");

    StdinCommandQueue stdinQueue;
    bool running = true;

    const auto tickDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(Game::PHYSICS_DELTA));
    auto nextTick = std::chrono::steady_clock::now();
    while (running) {
        for (const std::string& line : stdinQueue.Drain()) {
            HandleCommand(line, game, server, running);
        }

        server.IngestInput(game.GetStep());
        game.Update();
        server.BroadcastSnapshot(game.GetStep());

        nextTick += tickDuration;
        std::this_thread::sleep_until(nextTick);
    }
}
