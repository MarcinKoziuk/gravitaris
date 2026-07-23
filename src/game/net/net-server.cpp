#include <optional>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/input-queue.hpp>
#include <gravitaris/game/net/byte-stream.hpp>
#include <gravitaris/game/net/protocol.hpp>
#include <gravitaris/game/net/snapshot.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/gwell/faction-system.hpp>
#include <gravitaris/game/net/net-server.hpp>

namespace Gravitaris {

NetServer::NetServer(flecs::world& registry, EntitySpawner& entitySpawner, const GameEventQueue& eventQueue,
                     FactionSystem& factionSystem, INetTransport& transport)
        : m_registry(registry)
        , m_entitySpawner(entitySpawner)
        , m_eventQueue(eventQueue)
        , m_factionSystem(factionSystem)
        , m_transport(transport)
{}

void NetServer::IngestInput(std::uint64_t currentTick)
{
    for (NetEvent& event : m_transport.Poll()) {
        switch (event.type) {
            case NetEventType::Connected:
                m_peers.emplace(event.peer, PeerState{});
                LOG(info) << "net: peer " << event.peer << " connected (" << m_peers.size() << " total)";
                break;
            case NetEventType::Disconnected:
                HandleDisconnect(event.peer);
                break;
            case NetEventType::Packet:
                HandlePacket(event.peer, event.data.data(), event.data.size(), currentTick);
                break;
        }
    }

    // Dead-man sweep (see INPUT_TIMEOUT_TICKS): a peer that stops landing
    // fresh commands gets one synthetic idle command, so InputSystem's
    // repeat-last-command semantics settle on "hands off the controls"
    // instead of replaying its last held flags forever.
    for (auto& [peer, state] : m_peers) {
        if (!state.welcomed || !state.ship.is_alive() || state.idleInjected) continue;
        if (currentTick <= state.lastQueuedInputTick + INPUT_TIMEOUT_TICKS) continue;

        InputCommand idle;
        idle.tick = currentTick;
        state.ship.get_mut<InputQueue>().Push(idle);
        state.lastQueuedInputTick = currentTick;
        state.idleInjected = true;
        LOG(info) << "net: peer " << peer << " input timed out (" << INPUT_TIMEOUT_TICKS
                  << " ticks silent, " << state.staleInputCount << " stale so far), zeroing controls";
    }

    HandleRespawns();
}

void NetServer::HandleRespawns()
{
    for (auto& [peer, state] : m_peers) {
        if (!state.welcomed || state.ship.is_alive()) {
            state.respawnTimer = -1;
            continue;
        }
        if (state.respawnTimer < 0) {
            state.respawnTimer = RESPAWN_DELAY_TICKS;
            continue;
        }
        if (--state.respawnTimer > 0) continue;

        // Timer expired: keep retrying every subsequent tick (respawnTimer
        // just keeps decrementing below zero, so this branch runs again
        // next tick too) until FactionSystem::TryRespawn succeeds -- a site
        // exists but nothing can fund the fighter yet is a transient wait,
        // not a failure -- or permanently doesn't (no friendly planet/High
        // Port left at all -- that faction is out; this peer just stays a
        // ghost, no ship, no fresh ServerWelcome, until the round ends).
        const std::optional<Vector2d> pos = m_factionSystem.TryRespawn(state.team);
        if (!pos) continue;

        const id_t playerModel = "models/ships/fighter-1"_id;
        state.ship = m_entitySpawner.SpawnPlayer(playerModel, *pos, state.team);

        ServerWelcomePacket welcome;
        welcome.clientId = peer;
        welcome.yourShipNetId = state.ship.get<NetId>().value;
        welcome.tickRate = 60;
        welcome.yourTeam = state.team;

        ByteWriter writer;
        WriteServerWelcome(welcome, writer);
        m_transport.Send(peer, 0, writer.Data(), writer.Size(), true);
        LOG(info) << "net: peer " << peer << " respawned, ship NetId " << welcome.yourShipNetId;
    }
}

void NetServer::HandlePacket(PeerId peer, const std::uint8_t* data, std::size_t size, std::uint64_t currentTick)
{
    ByteReader reader(data, size);
    const auto type = static_cast<PacketType>(reader.ReadU8());

    switch (type) {
        case PacketType::ClientHello: {
            ClientHelloPacket hello;
            if (!ReadClientHelloBody(reader, hello)) return;
            if (hello.protocolVersion != PROTOCOL_VERSION) return; // silently drop, no version-mismatch UX yet

            auto it = m_peers.find(peer);
            if (it == m_peers.end()) return; // Packet before Connected shouldn't happen; be defensive
            if (it->second.welcomed) return; // duplicate hello (resent, lost welcome) -- ignore

            // TeamId::None means "no preference" here (see requestedTeam's
            // own doc comment); auto-assign round-robins a small default
            // roster so two players with no setup UI to pick a side still
            // get a versus match instead of stacking on Blue.
            if (hello.requestedTeam != TeamId::None) {
                it->second.team = hello.requestedTeam;
            } else {
                it->second.team = AUTO_ASSIGN_ROSTER[m_nextAutoAssign % (sizeof(AUTO_ASSIGN_ROSTER)
                                                                        / sizeof(AUTO_ASSIGN_ROSTER[0]))];
                ++m_nextAutoAssign;
            }

            // Placeholder spawn point/model until Phase 3 grows real player
            // -slot selection; matches Game::Start()'s single local player.
            // Offset by however many peers already hold a slot (this one
            // included, since Connected already inserted it) so players
            // don't stack on top of each other.
            const id_t playerModel = "models/ships/fighter-1"_id;
            const double spawnOffset = 200. * static_cast<double>(m_peers.size() - 1);
            const flecs::entity ship =
                    m_entitySpawner.SpawnPlayer(playerModel, Vector2d{spawnOffset, 0.}, it->second.team);
            it->second.ship = ship;
            it->second.welcomed = true;
            it->second.lastQueuedInputTick = currentTick; // dead-man baseline: "joined now", not tick 0

            ServerWelcomePacket welcome;
            welcome.clientId = peer;
            welcome.yourShipNetId = ship.get<NetId>().value;
            welcome.tickRate = 60;
            welcome.yourTeam = it->second.team;

            ByteWriter writer;
            WriteServerWelcome(welcome, writer);
            m_transport.Send(peer, 0, writer.Data(), writer.Size(), true);
            LOG(info) << "net: peer " << peer << " welcomed, ship NetId " << welcome.yourShipNetId
                      << " at (" << spawnOffset << ", 0), team " << static_cast<int>(it->second.team);
            break;
        }
        case PacketType::ClientInput: {
            ClientInputPacket input;
            if (!ReadClientInputBody(reader, input)) return;

            const auto it = m_peers.find(peer);
            if (it == m_peers.end() || !it->second.welcomed || !it->second.ship.is_alive()) return;

            InputQueue& queue = it->second.ship.get_mut<InputQueue>();
            for (const InputCommand& cmd : input.commands) {
                if (cmd.tick <= it->second.lastQueuedInputTick) continue; // already queued, part of the resend window
                if (cmd.tick < currentTick) {
                    // Diagnostic (2026-07-19): this command is already stale
                    // by the time it's queued -- InputSystem will drop it on
                    // sight (tick < step) and repeat the last-consumed flags
                    // instead, which is a candidate cause for the "own ship
                    // teleports" reconciliation symptom (see CGame::
                    // ReconcileOwnShipIfNeeded's matching log). Lateness in
                    // ticks roughly indicates whether INPUT_LEAD_TICKS is
                    // enough slack for this connection's real RTT/jitter.
                    ++it->second.staleInputCount;
                    LOG(trace) << "net: peer " << peer << " input for tick " << cmd.tick
                              << " arrived " << (currentTick - cmd.tick) << " ticks late (currentTick "
                              << currentTick << "), dropped by InputSystem -- " << it->second.staleInputCount
                              << " stale so far";
                }
                queue.Push(cmd);
                it->second.lastQueuedInputTick = cmd.tick;
                it->second.idleInjected = false; // fresh input: stall episode (if any) is over
            }
            break;
        }
        case PacketType::Ping: {
            // Stateless echo, no game-state lookup needed -- works even
            // before Welcome (measuring RTT doesn't need a ship yet) and
            // unreliable (0, matching ClientInput/Snapshot) so it reflects
            // the same channel's real loss/latency characteristics rather
            // than the reliable channel's.
            PingPacket ping;
            if (!ReadPingBody(reader, ping)) return;

            PongPacket pong;
            pong.seq = ping.seq;
            ByteWriter writer;
            WritePong(pong, writer);
            m_transport.Send(peer, 0, writer.Data(), writer.Size(), false);
            break;
        }
        case PacketType::ServerWelcome:
        case PacketType::Snapshot:
        case PacketType::Pong:
            break; // server never receives these
    }
}

void NetServer::HandleDisconnect(PeerId peer)
{
    const auto it = m_peers.find(peer);
    if (it == m_peers.end()) return;
    if (it->second.ship.is_alive()) it->second.ship.destruct();
    m_peers.erase(it);
}

bool NetServer::SetPeerTeam(PeerId peer, TeamId team)
{
    const auto it = m_peers.find(peer);
    if (it == m_peers.end()) return false;

    it->second.team = team;
    if (it->second.ship.is_alive()) {
        it->second.ship.set<Team>(Team{team});
    }
    return true;
}

void NetServer::BroadcastSnapshot(std::uint64_t currentTick)
{
    for (auto& [peer, state] : m_peers) {
        if (!state.welcomed) continue;

        // Normally a peer predicts and draws its own shots locally, so it
        // must not also receive the authoritative copies (see GatherSnapshot's
        // suppressBulletsOwnedBy). On the no-client-prediction branch the
        // client draws NO cosmetic own-bullets (it predicts nothing), so it
        // must receive its own authoritative bullets like everyone else's or
        // its shots would be invisible -- suppress nothing.
        // TODO: once prediction is a per-client runtime choice rather than a
        // build-wide one, key this off what the client actually does.
        const std::uint32_t ownShipNetId = 0u;

        SnapshotData snapshot;
        GatherSnapshot(m_registry, m_eventQueue, currentTick, state.lastSentEventSeq, snapshot, ownShipNetId);
        if (!snapshot.events.empty()) {
            state.lastSentEventSeq = snapshot.events.back().seq;
        }

        ByteWriter writer;
        WriteSnapshotPacket(snapshot, writer);
        m_transport.Send(peer, 0, writer.Data(), writer.Size(), false);
    }
}

} // namespace Gravitaris
