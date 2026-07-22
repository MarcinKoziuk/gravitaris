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
        while (!queue.Empty() && queue.Front().tick < step) {
            queue.PopFront();
        }

        if (!queue.Empty() && queue.Front().tick == step) {
            controls.actionFlags = queue.Front().flags;
            queue.PopFront();
        }
        // No command for this tick: Controls keeps its previous value
        // (repeat-last-command, quake3-style).
    });
}

} // namespace Gravitaris
