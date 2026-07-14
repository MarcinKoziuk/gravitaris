#pragma once

#include <array>
#include <cstddef>
#include <type_traits>

#include <gravitaris/game/input/input-command.hpp>

namespace Gravitaris {

// Fixed-capacity ring of commands awaiting consumption; InputSystem drains the
// entry matching the current sim tick. The buffering exists for commands
// arriving ahead of the sim (network) and for replays. In single-player it
// holds exactly the current tick's command.
//
// Trivially copyable so flecs relocates it by memcpy and it serializes
// directly. When full, Push overwrites the oldest entry (a producer that far
// ahead of the sim has already lost those ticks to the CAPACITY-tick window).
struct InputQueue {
    static constexpr std::size_t CAPACITY = 64; // quake3 CMD_BACKUP

    std::array<InputCommand, CAPACITY> commands{};
    std::size_t head = 0;  // index of the oldest entry
    std::size_t count = 0;

    [[nodiscard]] bool Empty() const { return count == 0; }

    [[nodiscard]] bool Full() const { return count == CAPACITY; }

    [[nodiscard]] const InputCommand& Front() const { return commands[head]; }

    void PopFront()
    {
        head = (head + 1) % CAPACITY;
        --count;
    }

    void Push(const InputCommand& command)
    {
        if (Full()) {
            PopFront();
        }
        commands[(head + count) % CAPACITY] = command;
        ++count;
    }
};

static_assert(std::is_trivially_copyable_v<InputQueue>);

} // namespace Gravitaris
