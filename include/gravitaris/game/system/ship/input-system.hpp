#pragma once

#include <cstdint>

#include <flecs.h>

namespace Gravitaris {

// Moves the current tick's command from each entity's InputQueue into its
// Controls. Runs before ShipControlsSystem in the tick.
class InputSystem {
private:
    flecs::world& m_registry;

public:
    explicit InputSystem(flecs::world& registry);

    ~InputSystem() = default;

    void Update(std::uint64_t step);
};

} // namespace Gravitaris
