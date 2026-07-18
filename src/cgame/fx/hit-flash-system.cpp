#include <algorithm>

#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>

#include <gravitaris/cgame/component/hit-flash.hpp>
#include <gravitaris/cgame/fx/hit-flash-system.hpp>

namespace Gravitaris {

namespace {

// The flash previously decayed 1/8 per 60Hz tick inside DamageSystem; 7.5/s
// is that same rate, applied client-side with the rendered frame's dt.
constexpr float FLASH_DECAY_PER_SECOND = 7.5f;

} // namespace

HitFlashSystem::HitFlashSystem(flecs::world& registry, const GameEventQueue& eventQueue,
                               const EntitySpawner& entitySpawner)
        : m_registry(registry)
        , m_eventQueue(eventQueue)
        , m_entitySpawner(entitySpawner)
{}

void HitFlashSystem::Update(float dtSeconds)
{
    m_eventCursor = m_eventQueue.ConsumeSince(m_eventCursor, [&](const GameEvent& event) {
        if (event.type != GameEventType::Impact && event.type != GameEventType::LandingCrash) return;
        const flecs::entity entity = m_entitySpawner.EntityForNetId(event.sourceNetId);
        if (!entity.is_alive()) return; // e.g. the hit killed it this tick
        if (HitFlash* flash = entity.try_get_mut<HitFlash>()) {
            flash->amount = 1.f;
        }
    });

    m_registry.each([&](HitFlash& flash) {
        if (flash.amount > 0.f) {
            flash.amount = std::max(0.f, flash.amount - FLASH_DECAY_PER_SECOND * dtSeconds);
        }
    });
}

} // namespace Gravitaris
