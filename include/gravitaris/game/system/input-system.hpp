#pragma once

#include <cstdint>

#include <flecs.h>

namespace Gravitaris {

// Moves the current tick's command from each entity's InputQueue into its
// Controls, so ShipControlsSystem (and anything downstream) reads a single
// resolved control state without knowing where the command came from. Runs
// before ShipControlsSystem in the tick (slice-components step 1).
class InputSystem {
private:
    flecs::world& m_registry;

public:
    explicit InputSystem(flecs::world& registry);

    ~InputSystem() = default;

    void Update(std::uint64_t step);
};

} // namespace Gravitaris
