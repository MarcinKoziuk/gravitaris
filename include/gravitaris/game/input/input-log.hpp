#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <gravitaris/game/input/input-command.hpp>

namespace Gravitaris {

// An ordered stream of InputCommands for one controlled ship — the record/
// replay artifact. Commands are stored tick-relative (the first recorded tick
// is 0) so a replay can be re-based onto any starting tick.
//
// NOTE: replaying a log only reproduces the original run when fed into an
// identically-started sim on the same build/machine — Chipmunk runs in floats
// and ADR 0001 accepts cross-platform non-determinism. This is a debugging aid,
// not a canonical (e.g. e-sport) recording format.
class InputLog {
public:
    void Clear();

    void Append(const InputCommand& command);

    [[nodiscard]] bool Empty() const { return m_commands.empty(); }

    [[nodiscard]] std::size_t Size() const { return m_commands.size(); }

    [[nodiscard]] const std::vector<InputCommand>& Commands() const { return m_commands; }

    // Binary (de)serialization. Return false on I/O or format error.
    [[nodiscard]] bool Save(const std::string& path) const;

    [[nodiscard]] bool Load(const std::string& path);

private:
    std::vector<InputCommand> m_commands;
};

} // namespace Gravitaris
