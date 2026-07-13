#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/input-queue.hpp>
#include <gravitaris/game/system/input-system.hpp>

namespace Gravitaris {

InputSystem::InputSystem(flecs::world& registry)
        : m_registry(registry)
{}

void InputSystem::Update(std::uint64_t step)
{
    m_registry.each([&](InputQueue& queue, Controls& controls) {
        while (!queue.pending.empty() && queue.pending.front().tick < step) {
            queue.pending.pop_front();
        }

        if (!queue.pending.empty() && queue.pending.front().tick == step) {
            controls.actionFlags = queue.pending.front().flags;
            queue.pending.pop_front();
        }
        // No command for this tick: Controls keeps its previous value
        // (repeat-last-command, quake3-style).
    });
}

} // namespace Gravitaris
