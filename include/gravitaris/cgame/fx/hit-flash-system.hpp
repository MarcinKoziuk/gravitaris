#pragma once

#include <cstdint>

#include <flecs.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Client-side hit-flash: sets HitFlash.amount = 1 on entities named by
// Impact/LandingCrash events (resolved via the NetId registry), then decays
// every entity's flash with the rendered frame's dt. A GameEventQueue
// consumer with its own cursor, the same shape AudioSystem uses -- HitFlash
// itself is client-only presentation state (see its own header), so this
// system, not the sim, owns decaying it.
class HitFlashSystem {
    flecs::world& m_registry;
    const GameEventQueue& m_eventQueue;
    const EntitySpawner& m_entitySpawner;

    std::uint32_t m_eventCursor = 0;

public:
    HitFlashSystem(flecs::world& registry, const GameEventQueue& eventQueue, const EntitySpawner& entitySpawner);

    void Update(float dtSeconds);

    // Decays every HitFlash in `world` by the rendered frame's dt. Public
    // and static so a world with no matching GameEventQueue/NetId registry
    // of its own (the net-client mirror world, whose HitFlash amounts are
    // set directly by CGame from replicated events instead) can still decay
    // them the same way Update() does for m_registry.
    static void Decay(flecs::world& world, float dtSeconds);
};

} // namespace Gravitaris
