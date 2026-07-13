#pragma once

#include <deque>

#include <gravitaris/game/input/input-command.hpp>

namespace Gravitaris {

// Commands awaiting consumption; InputSystem drains the entry matching the
// current sim tick. The buffering exists for commands arriving ahead of the
// sim (network) and for replays.
struct InputQueue {
    std::deque<InputCommand> pending;
};

} // namespace Gravitaris
