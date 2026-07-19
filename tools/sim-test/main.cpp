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

#include <gravitaris/game/fs/filesystem-physfs.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/id.hpp>
#include <gravitaris/game/net/byte-stream.hpp>
#include <gravitaris/game/net/snapshot.hpp>
#include <gravitaris/game/net/loopback-transport.hpp>
#include <gravitaris/game/net/net-server.hpp>
#include <gravitaris/game/net/net-client.hpp>
#include <gravitaris/game/net/webrtc-server-transport.hpp>
#include <gravitaris/game/net/webrtc-transport.hpp>
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
        client.SendInput(thrust);
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
        client.SendInput(thrust);
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
        client.SendInput(thrust);
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

    const RunResult result{game.ComputeStateChecksum(), eventHash, game.GetEventQueue().LatestSeq()};
    fs.Shutdown();
    return result;
}

} // namespace

int main()
{
    HasEnteredMain = true;

    TestByteStream();
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
