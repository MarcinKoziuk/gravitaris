#include <cstdint>
#include <fstream>
#include <ostream>
#include <istream>

#include <gravitaris/game/input/input-log.hpp>

namespace Gravitaris {

namespace {

constexpr char          kMagic[4] = {'G', 'R', 'P', 'L'}; // "GRaVitaris rePLay"
constexpr std::uint32_t kVersion  = 1;

template <typename T>
void WritePod(std::ostream& os, const T& value)
{
    os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
bool ReadPod(std::istream& is, T& value)
{
    return static_cast<bool>(is.read(reinterpret_cast<char*>(&value), sizeof(T)));
}

} // namespace

void InputLog::Clear()
{
    m_commands.clear();
}

void InputLog::Append(const InputCommand& command)
{
    m_commands.push_back(command);
}

bool InputLog::Save(const std::string& path) const
{
    std::ofstream os(path, std::ios::binary);
    if (!os) return false;

    os.write(kMagic, sizeof(kMagic));
    WritePod(os, kVersion);
    WritePod(os, static_cast<std::uint64_t>(m_commands.size()));

    for (const InputCommand& cmd : m_commands) {
        WritePod(os, cmd.tick);
        WritePod(os, PackControlFlags(cmd.flags));
    }

    return static_cast<bool>(os);
}

bool InputLog::Load(const std::string& path)
{
    std::ifstream is(path, std::ios::binary);
    if (!is) return false;

    char magic[4];
    if (!is.read(magic, sizeof(magic))) return false;
    for (std::size_t i = 0; i < sizeof(magic); ++i) {
        if (magic[i] != kMagic[i]) return false;
    }

    std::uint32_t version = 0;
    if (!ReadPod(is, version) || version != kVersion) return false;

    std::uint64_t count = 0;
    if (!ReadPod(is, count)) return false;

    std::vector<InputCommand> loaded;
    loaded.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        InputCommand cmd;
        std::uint8_t packed = 0;
        if (!ReadPod(is, cmd.tick) || !ReadPod(is, packed)) return false;
        cmd.flags = UnpackControlFlags(packed);
        loaded.push_back(cmd);
    }

    m_commands = std::move(loaded);
    return true;
}

} // namespace Gravitaris
