#pragma once

#include <cstdint>

#include <gravitaris/game/component/controls.hpp>

namespace Gravitaris {

// The unit of input the sim consumes (ADR 0001: stepping is a pure function
// of state + commands). Keyboard, AI pilots, network peers and replays all
// produce these.
struct InputCommand {
    std::uint64_t tick = 0;
    ControlFlags flags{};
    // One tech-tree purchase this tick commits, if any (TechPick::IsSet).
    TechPick techPick;
    // How many ticks behind this command's own tick the pilot was seeing
    // everybody else when they composed it -- their interpolation delay, plus
    // however far behind the server they are running. Zero for anything
    // composed inside the sim itself (single player, AI, replays), which is why
    // it is a delay rather than a tick: a number that means "not applicable"
    // and "no delay" at once needs no special case anywhere downstream.
    //
    // The server resolves hitscan against the world this far back, so a shot
    // that looked dead on lands (LagCompensation).
    std::uint16_t viewDelay = 0;
};

// The bits alone, for replay files and the wire. Two bytes since the laser's
// trigger: the eight below filled the first one exactly. `aim` is not in here
// -- it is a whole word of its own and every caller writes it beside this.
// Keep in sync with ControlFlags.
inline std::uint16_t PackControlFlags(const ControlFlags& f)
{
    return static_cast<std::uint16_t>(
        (f.thrustForward ? 0x001 : 0) |
        (f.rotateLeft    ? 0x002 : 0) |
        (f.rotateRight   ? 0x004 : 0) |
        (f.firePrimary   ? 0x008 : 0) |
        (f.fireSecondary ? 0x010 : 0) |
        (f.fireMissile   ? 0x020 : 0) |
        (f.boost         ? 0x040 : 0) |
        (f.toggleWeapon  ? 0x080 : 0) |
        (f.fireLaser     ? 0x100 : 0));
}

inline ControlFlags UnpackControlFlags(std::uint16_t b)
{
    ControlFlags f{};
    f.thrustForward = (b & 0x001) != 0;
    f.rotateLeft    = (b & 0x002) != 0;
    f.rotateRight   = (b & 0x004) != 0;
    f.firePrimary   = (b & 0x008) != 0;
    f.fireSecondary = (b & 0x010) != 0;
    f.fireMissile   = (b & 0x020) != 0;
    f.toggleWeapon  = (b & 0x080) != 0;
    f.boost         = (b & 0x040) != 0;
    f.fireLaser     = (b & 0x100) != 0;
    return f;
}

} // namespace Gravitaris
