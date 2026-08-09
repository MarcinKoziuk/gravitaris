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
#include <random>
#include <sstream>
#include <string>
#include <thread>

#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/debug/debug-spawn.hpp>
#include <gravitaris/game/event/death-report.hpp>
#include <gravitaris/game/fs/filesystem-physfs.hpp>
#include <gravitaris/game/notify/notifier.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/ai/ai-preset-library.hpp>
#include <gravitaris/game/id.hpp>
#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/net/net-server.hpp>
#include <gravitaris/game/net/webrtc-server-transport.hpp>
#include <gravitaris/game/scenario/sector-scenario.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/gravitaris.hpp>

using namespace Gravitaris;

namespace {

// Background stdin reader: std::cin has no non-blocking read, so the main
// tick loop can't just poll it directly without stalling on the syscall. A
// dedicated thread blocks on getline() and hands finished lines to the main
// loop through a mutexed queue, drained once per tick alongside
// NetServer::IngestInput. *Very* minimal by design (docs/networking-plan N5)
// -- no history/line-editing (replxx/isocline would add that later if ever
// wanted; skipped for now).
    // Claude: lets use isocline
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

static constexpr const char* TEAM_COLORS = "blue|red|green|yellow|magenta|cyan";
static constexpr const char* COMMAND_USAGE =
        "spawn [count] [preset] [color]|ai [color|fill] [preset]|list|team <peer-id> <color>|quit";

// Claude: can we use https://github.com/Neargye/magic_enum for this stuffie?
std::optional<TeamId> ParseTeam(const std::string& name)
{
    if (name == "blue") return TeamId::Blue;
    if (name == "red") return TeamId::Red;
    if (name == "violet") return TeamId::Violet;
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

    // Presets are data (data/ai-presets.toml), so these take whatever key the
    // file happens to hold rather than a hardcoded list.
    const auto findPreset = [&game](const std::string& verb, const std::string& key) -> const AIPreset* {
        const AIPreset* preset = game.GetAIPresets().FindByKey(key);
        if (!preset) {
            std::fprintf(stderr, "%s: unknown preset '%s'; known:", verb.c_str(), key.c_str());
            for (const AIPreset& known : game.GetAIPresets().All()) {
                std::fprintf(stderr, " %s", known.key.c_str());
            }
            std::fprintf(stderr, "\n");
        }
        return preset;
    };

    if (verb.empty()) {
        return;
    }
    else if (verb == "spawn") {
        int count = 1;
        std::string presetName = "balanced";
        std::string colorName = "red";
        iss >> count;
        iss >> presetName;
        iss >> colorName;

        const AIPreset* preset = findPreset(verb, presetName);
        if (!preset) {
            return;
        }
        const std::optional<TeamId> team = ParseTeam(colorName);
        if (!team) {
            std::fprintf(stderr, "spawn: unknown color '%s' (%s)\n", colorName.c_str(), TEAM_COLORS);
            return;
        }

        SpawnAIWave(game, count, *preset, *team);
        std::printf("spawned %d %s %s ship(s)\n", count, colorName.c_str(), presetName.c_str());
    }
    else if (verb == "ai") {
        std::string colorName;
        std::string presetName = "balanced";
        iss >> colorName;
        iss >> presetName;

        // Bare `ai` (or `ai fill`) takes every side nobody is flying, which is
        // what a host types once the lobby has stopped filling up.
        if (colorName.empty() || colorName == "fill") {
            const AIPreset* preset = findPreset(verb, presetName);
            if (!preset) {
                return;
            }
            const std::vector<TeamId> filled = game.FillEmptyTeamsWithAI(preset->id);
            if (filled.empty()) {
                std::printf("ai: every side is already taken\n");
                return;
            }
            std::printf("ai: %zu unfilled side(s) now field a %s leader\n", filled.size(),
                        presetName.c_str());
            return;
        }

        const std::optional<TeamId> team = ParseTeam(colorName);
        if (!team) {
            std::fprintf(stderr, "ai: unknown color '%s' (%s)\n", colorName.c_str(), TEAM_COLORS);
            return;
        }
        const AIPreset* preset = findPreset(verb, presetName);
        if (!preset) {
            return;
        }

        game.AddAIFaction(*team, preset->id);
        std::printf("%s now fields a %s AI leader\n", colorName.c_str(), presetName.c_str());
    }
    else if (verb == "list") {
        std::printf("peers: %zu\n", server.PeerCount());
    }
    else if (verb == "team") {
        std::uint32_t peer = 0;
        std::string colorName;
        iss >> peer >> colorName;
        const std::optional<TeamId> team = ParseTeam(colorName);
        if (!team) {
            std::fprintf(stderr, "team: unknown color '%s' (%s)\n", colorName.c_str(), TEAM_COLORS);
            return;
        }
        if (!server.SetPeerTeam(peer, *team)) {
            std::fprintf(stderr, "team: no such peer %u\n", peer);
            return;
        }
        std::printf("peer %u set to %s\n", peer, colorName.c_str());
    }
    else if (verb == "quit") {
        running = false;
    }
    else {
        std::fprintf(stderr, "unknown command '%s' (%s)\n", verb.c_str(), COMMAND_USAGE);
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

    // gravitaris-server [port] [bind-address] [--seed N] [--factions N]
    // [--stars N]; an empty bind address listens on every interface.
    std::uint16_t port = 17890;
    std::string bindAddress;
    SectorParams sectorParams;
    bool seedGiven = false;

    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> long { return (i + 1 < argc) ? std::atol(argv[++i]) : 0L; };

        if (arg == "--seed") {
            sectorParams.seed = static_cast<std::uint32_t>(value());
            seedGiven = true;
        }
        else if (arg == "--factions") sectorParams.factionCount = static_cast<int>(value());
        else if (arg == "--stars") sectorParams.stars = static_cast<int>(value());
        else if (positional++ == 0) port = static_cast<std::uint16_t>(std::atoi(arg.c_str()));
        else bindAddress = arg;
    }

    // Without --seed every round would be the same sector, since the default
    // is a plain 0. Picking one here is launch-time configuration, not the sim
    // reaching for entropy (ADR 0001 point 5) -- once chosen it is fixed, and
    // it alone determines the world. Tracked separately from the value because
    // 0 is a legal seed to ask for explicitly.
    if (!seedGiven) {
        std::random_device entropy;
        sectorParams.seed = entropy();
    }
    sectorParams.Sanitize();

    FilesystemPhysFS fs;
    if (!fs.Init()) {
        std::fprintf(stderr, "gravitaris-server: filesystem init failed\n");
        return 1;
    }

    // No local player: Game::Start() would spawn one nobody controls or
    // replicates meaningfully. BuildWorld alone gives the solar system
    // (planets, complexes) without it -- players arrive entirely through
    // NetServer's ClientHello handling.
    Game game(fs);
    // Logged because a generated sector is otherwise unreproducible: this is
    // the only record of what --seed to pass to get this round's map back.
    LOG(info) << "gravitaris-server: sector seed " << sectorParams.seed << " ("
              << sectorParams.factionCount << " factions, " << sectorParams.stars << " stars)";
    game.BuildWorld(sectorParams);

    // No leaders up front. An empty colour on a dedicated server is a slot
    // waiting for a player, and claiming it for the machine leaves whoever
    // joins flying alongside a bot they never asked for. The sides nobody
    // takes are filled on demand instead -- the `ai` console verb here, or
    // /ai in chat.

    WebRtcServerTransport transport(port, DefaultIceServers(), bindAddress);
    NetServer server(game, transport);
    server.SetAutoAssignRoster(game.GetRoster());
    server.SetSectorSeed(sectorParams.seed);

    // Kill feed, as a server notice on the existing chat channel: an unnamed
    // sender with no team is exactly what ChatMessagePacket already means by
    // one, so the feed needs no packet of its own and lands in every client's
    // log in the same order as everybody's chat.
    game.OnDeath().connect([&server](const DeathReport& report) {
        server.BroadcastNotice(FormatDeathMessage(report));
    });

    // On from the start on a dedicated server: nobody is sitting at its
    // console to type /notify, and the whole point is being told about a join
    // you weren't there for. Still a no-op without a webhook configured.
    game.SetNotifier(MakeWebhookNotifier(WebhookUrlFromEnvironment()));
    game.SetNotificationsEnabled(game.HasNotifier());
    game.Notify("Gravitaris server up on port " + std::to_string(port) + ".");

    LOG(info) << "gravitaris-server: listening on ws://" << (bindAddress.empty() ? "0.0.0.0" : bindAddress)
              << ":" << port;
    std::printf("commands: %s\n", COMMAND_USAGE);

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
