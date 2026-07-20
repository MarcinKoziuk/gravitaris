#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/input/input-command.hpp>
#include <gravitaris/game/net/snapshot.hpp>

namespace Gravitaris {

class ByteWriter;
class ByteReader;

// Protocol v1 (docs/networking-plan.md 3.2). All packets little-endian
// (ByteWriter/ByteReader's own format); first byte is always PacketType, so a
// receiver dispatches on one ReadU8() before parsing the rest.
enum class PacketType : std::uint8_t {
    ClientHello = 1,
    ServerWelcome = 2,
    ClientInput = 3,
    Snapshot = 4,
};

inline constexpr std::uint32_t PROTOCOL_VERSION = 2; // v2: +requestedTeam/yourTeam (per-peer team assignment)

// How many trailing commands ClientInput carries per send -- redundancy
// instead of reliability (quake3-style): as long as one of the last N sends
// lands, the server hasn't lost a tick. InputQueue's own dedupe-by-tick on
// the receiving end (InputSystem) makes resending already-applied commands
// harmless.
inline constexpr std::size_t CLIENT_INPUT_BACKUP = 8;

struct ClientHelloPacket {
    std::uint32_t protocolVersion = PROTOCOL_VERSION;
    std::string name;
    // TeamId::None here means "no preference, auto-assign" -- distinct from
    // its sim meaning (the ownerless/hostile-to-everyone team, see Team's
    // own doc comment), which never applies to a player's requested team.
    // No round-setup UI exists yet (docs/gravity-well-mode-plan.md's
    // Multiplayer wiring track) to ever send anything else, so every
    // client auto-assigns today.
    TeamId requestedTeam = TeamId::None;
};

struct ServerWelcomePacket {
    std::uint32_t clientId = 0;
    std::uint32_t yourShipNetId = 0;
    std::uint32_t tickRate = 60;
    TeamId yourTeam = TeamId::Blue;
};

// Deviation from the plan's sketch: adds lastAckedEventSeq alongside
// lastAckedSnapshotTick. The plan only specced the tick; the server also
// needs to know which GameEvents the client has already seen to pass the
// right `eventsSinceSeq` into GatherSnapshot, so this rides in the same
// packet rather than being inferred from the tick.
struct ClientInputPacket {
    std::uint64_t lastAckedSnapshotTick = 0;
    std::uint32_t lastAckedEventSeq = 0;
    std::vector<InputCommand> commands; // newest last; up to CLIENT_INPUT_BACKUP
};

// SnapshotPacket has no separate struct -- it's PacketType::Snapshot followed
// directly by SerializeSnapshot's own bytes (tick, entities, events); decode
// with ReadSnapshot after consuming the type byte.

void WriteClientHello(const ClientHelloPacket& packet, ByteWriter& out);
bool ReadClientHelloBody(ByteReader& in, ClientHelloPacket& out); // type byte already consumed

void WriteServerWelcome(const ServerWelcomePacket& packet, ByteWriter& out);
bool ReadServerWelcomeBody(ByteReader& in, ServerWelcomePacket& out);

void WriteClientInput(const ClientInputPacket& packet, ByteWriter& out);
bool ReadClientInputBody(ByteReader& in, ClientInputPacket& out);

// Combined convenience: PacketType::Snapshot + SerializeSnapshot(snapshot).
void WriteSnapshotPacket(const SnapshotData& snapshot, ByteWriter& out);

} // namespace Gravitaris
