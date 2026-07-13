#pragma once

#include <deque>

#include <gravitaris/game/input/input-command.hpp>

namespace Gravitaris {

// Per-entity queue of tick-stamped commands awaiting consumption. Both human
// input (client) and AI pilots push commands here; InputSystem drains the
// command whose tick matches the current sim tick each Update. In single-player
// the queue holds exactly the current tick's command, but the buffering is what
// makes networked play (commands arriving ahead of the sim) and replays work.
struct InputQueue {
    std::deque<InputCommand> pending;
};

} // namespace Gravitaris
