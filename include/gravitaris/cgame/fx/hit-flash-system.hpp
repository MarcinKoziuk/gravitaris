#pragma once

#include <cstdint>

#include <flecs.h>

#include <Magnum/Math/Vector2.h>

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

    // Lights `entity`'s ShieldFlash for a hit at `worldPos`, recording the
    // bearing in the ship's own frame and which plate took it
    // (ShieldFlash::BUBBLE if the bubble did). Public and static for the same
    // reason Decay is: the net-client mirror world's shield hits arrive
    // through RemoteEventApplier rather than through a local GameEventQueue.
    static void ApplyShieldHit(flecs::entity entity, const Magnum::Vector2& worldPos, std::int8_t plate);

    // The plate a ShieldHit/PlatingHit event names, unpacked from its param.
    [[nodiscard]] static std::int8_t PlateOf(const GameEvent& event);
};

} // namespace Gravitaris
