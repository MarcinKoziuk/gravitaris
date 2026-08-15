#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/input-queue.hpp>
#include <gravitaris/game/system/ship/input-system.hpp>

namespace Gravitaris {

InputSystem::InputSystem(flecs::world& registry)
        : m_registry(registry)
{}

void InputSystem::Update(std::uint64_t step)
{
    m_registry.each([&](InputQueue& queue, Controls& controls) {
        // One-shots are cleared every tick; only the held flags below carry
        // over when no command arrives.
        controls.techPick = {};

        // Everything up to and including this tick is consumable, newest
        // wins. A command that missed its stamped tick is applied late rather
        // than discarded: discarding it latches the last-consumed flags
        // instead, so a networked client whose stamps run even slightly
        // behind the server loses *every* command and its ship stops
        // responding entirely. A pick from a superseded command still
        // commits -- a purchase must not be lost to a catch-up burst.
        while (!queue.Empty() && queue.Front().tick <= step) {
            controls.actionFlags = queue.Front().flags;
            // Carried with the flags rather than beside them: the shot this
            // tick resolves is the one this command asked for, so it has to be
            // resolved against the world THAT command was composed against.
            controls.viewDelay = queue.Front().viewDelay;
            if (queue.Front().techPick.IsSet()) controls.techPick = queue.Front().techPick;
            queue.PopFront();
        }
        // No command for this tick: Controls keeps its previous value
        // (repeat-last-command, quake3-style).
    });
}

} // namespace Gravitaris
