#include <algorithm>

#include <gravitaris/game/net/byte-stream.hpp>
#include <gravitaris/game/net/protocol.hpp>

namespace Gravitaris {

namespace {

// Length-prefixed (u16) UTF-8 bytes -- local to this file rather than added
// to ByteWriter/ByteReader, since Phase 2's byte-stream is otherwise
// string-free and only ClientHello's `name` needs this.
void WriteString(const std::string& s, ByteWriter& out)
{
    const std::uint16_t len = static_cast<std::uint16_t>(std::min<std::size_t>(s.size(), 0xFFFFu));
    out.WriteU16(len);
    for (std::uint16_t i = 0; i < len; ++i) out.WriteU8(static_cast<std::uint8_t>(s[i]));
}

bool ReadString(ByteReader& in, std::string& out)
{
    const std::uint16_t len = in.ReadU16();
    out.clear();
    out.reserve(len);
    for (std::uint16_t i = 0; i < len; ++i) out.push_back(static_cast<char>(in.ReadU8()));
    return in.Ok();
}

} // namespace

void WriteClientHello(const ClientHelloPacket& packet, ByteWriter& out)
{
    out.WriteU8(static_cast<std::uint8_t>(PacketType::ClientHello));
    out.WriteU32(packet.protocolVersion);
    WriteString(packet.name, out);
}

bool ReadClientHelloBody(ByteReader& in, ClientHelloPacket& out)
{
    out.protocolVersion = in.ReadU32();
    ReadString(in, out.name);
    return in.Ok();
}

void WriteServerWelcome(const ServerWelcomePacket& packet, ByteWriter& out)
{
    out.WriteU8(static_cast<std::uint8_t>(PacketType::ServerWelcome));
    out.WriteU32(packet.clientId);
    out.WriteU32(packet.yourShipNetId);
    out.WriteU32(packet.tickRate);
}

bool ReadServerWelcomeBody(ByteReader& in, ServerWelcomePacket& out)
{
    out.clientId = in.ReadU32();
    out.yourShipNetId = in.ReadU32();
    out.tickRate = in.ReadU32();
    return in.Ok();
}

void WriteClientInput(const ClientInputPacket& packet, ByteWriter& out)
{
    out.WriteU8(static_cast<std::uint8_t>(PacketType::ClientInput));
    out.WriteU64(packet.lastAckedSnapshotTick);
    out.WriteU32(packet.lastAckedEventSeq);

    const std::size_t count = std::min(packet.commands.size(), CLIENT_INPUT_BACKUP);
    out.WriteU8(static_cast<std::uint8_t>(count));
    // Newest-last in the struct; send oldest-of-the-window-first so a reader
    // can just apply them in wire order.
    for (std::size_t i = packet.commands.size() - count; i < packet.commands.size(); ++i) {
        const InputCommand& cmd = packet.commands[i];
        out.WriteU64(cmd.tick);
        out.WriteU8(PackControlFlags(cmd.flags));
    }
}

bool ReadClientInputBody(ByteReader& in, ClientInputPacket& out)
{
    out.lastAckedSnapshotTick = in.ReadU64();
    out.lastAckedEventSeq = in.ReadU32();

    const std::uint8_t count = in.ReadU8();
    out.commands.clear();
    out.commands.reserve(count);
    for (std::uint8_t i = 0; i < count; ++i) {
        InputCommand cmd;
        cmd.tick = in.ReadU64();
        cmd.flags = UnpackControlFlags(in.ReadU8());
        out.commands.push_back(cmd);
    }
    return in.Ok();
}

void WriteSnapshotPacket(const SnapshotData& snapshot, ByteWriter& out)
{
    out.WriteU8(static_cast<std::uint8_t>(PacketType::Snapshot));
    SerializeSnapshot(snapshot, out);
}

} // namespace Gravitaris
