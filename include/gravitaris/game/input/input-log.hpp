#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <gravitaris/game/input/input-command.hpp>

namespace Gravitaris {

// Recorded command stream for one controlled ship. Ticks are stored relative
// to the first recorded tick so a replay can re-base onto any starting tick.
//
// Replays only reproduce a run on the same build/machine (float
// non-determinism, ADR 0001) -- a debugging aid, not a canonical format.
class InputLog {
public:
    void Clear();

    void Append(const InputCommand& command);

    [[nodiscard]] bool Empty() const { return m_commands.empty(); }

    [[nodiscard]] std::size_t Size() const { return m_commands.size(); }

    [[nodiscard]] const std::vector<InputCommand>& Commands() const { return m_commands; }

    [[nodiscard]] bool Save(const std::string& path) const;

    [[nodiscard]] bool Load(const std::string& path);

private:
    std::vector<InputCommand> m_commands;
};

} // namespace Gravitaris
