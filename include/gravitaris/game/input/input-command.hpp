#pragma once

#include <cstdint>

#include <gravitaris/game/component/controls.hpp>

namespace Gravitaris {

// A tick-stamped control command — the unit of input the simulation consumes.
// Per ADR 0001, stepping is a pure function of (state, commands, dt); this is
// the "command". Keyboard input, AI pilots, network peers, and replays all
// ultimately produce these and push them onto an entity's InputQueue.
struct InputCommand {
    std::uint64_t tick = 0;
    ControlFlags flags{};
};

// Compact 1-byte packing of the action bits, for replay files and (later) the
// network wire format. Keep in sync with ControlFlags.
inline std::uint8_t PackControlFlags(const ControlFlags& f)
{
    return static_cast<std::uint8_t>(
        (f.thrustForward ? 0x01 : 0) |
        (f.rotateLeft    ? 0x02 : 0) |
        (f.rotateRight   ? 0x04 : 0) |
        (f.firePrimary   ? 0x08 : 0) |
        (f.fireSecondary ? 0x10 : 0));
}

inline ControlFlags UnpackControlFlags(std::uint8_t b)
{
    ControlFlags f{};
    f.thrustForward = (b & 0x01) != 0;
    f.rotateLeft    = (b & 0x02) != 0;
    f.rotateRight   = (b & 0x04) != 0;
    f.firePrimary   = (b & 0x08) != 0;
    f.fireSecondary = (b & 0x10) != 0;
    return f;
}

} // namespace Gravitaris
