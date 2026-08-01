#include <algorithm>

#include <Magnum/Math/Complex.h>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>

#include <gravitaris/cgame/component/hit-flash.hpp>
#include <gravitaris/cgame/component/shield-flash.hpp>
#include <gravitaris/cgame/fx/hit-flash-system.hpp>

namespace Gravitaris {

namespace {

// The flash previously decayed 1/8 per 60Hz tick inside DamageSystem; 7.5/s
// is that same rate, applied client-side with the rendered frame's dt.
constexpr float FLASH_DECAY_PER_SECOND = 7.5f;

// Slower than the hull's: a shield strike is meant to read as the field
// absorbing and settling, not as the single-frame smack of taking a round.
constexpr float BUBBLE_FLASH_DECAY_PER_SECOND = 3.0f;

// A struck plate stays lit for about a second before it is back to reading as
// bare armour -- long enough to see *which* plate stopped the round, which is
// the whole point of drawing them separately.
constexpr float PLATE_FLASH_DECAY_PER_SECOND = 1.0f;

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
        const bool hull = event.type == GameEventType::Impact
                          || event.type == GameEventType::LandingCrash;
        const bool shield = event.type == GameEventType::ShieldHit
                            || event.type == GameEventType::PlatingHit;
        if (!hull && !shield) return;

        const flecs::entity entity = m_entitySpawner.EntityForNetId(event.sourceNetId);
        if (!entity.is_alive()) return; // e.g. the hit killed it this tick

        if (hull) {
            if (HitFlash* flash = entity.try_get_mut<HitFlash>()) flash->amount = 1.f;
        }
        else {
            ApplyShieldHit(entity, event.pos, PlateOf(event));
        }
    });

    Decay(m_registry, dtSeconds);
}

std::int8_t HitFlashSystem::PlateOf(const GameEvent& event)
{
    if (event.type != GameEventType::PlatingHit) return ShieldFlash::BUBBLE;
    return static_cast<std::int8_t>(PlatingHitPlate(event.param));
}

void HitFlashSystem::ApplyShieldHit(flecs::entity entity, const Magnum::Vector2& worldPos,
                                    std::int8_t plate)
{
    ShieldFlash* flash = entity.try_get_mut<ShieldFlash>();
    const Transform* transform = entity.try_get<Transform>();
    if (!flash || !transform) return;

    flash->amount = 1.f;
    flash->plate = plate;

    // Stored in the ship's own frame so it keeps pointing at the same part of
    // the shield as the ship turns, rather than sliding around it.
    const Magnum::Vector2 toHit = worldPos - Magnum::Vector2{static_cast<float>(transform->pos.x()),
                                                             static_cast<float>(transform->pos.y())};
    if (toHit.isZero()) return;

    const auto rot = static_cast<float>(static_cast<double>(transform->rot));
    flash->dir = Magnum::Complex::rotation(Magnum::Rad{-rot}).transformVector(toHit.normalized());
}

void HitFlashSystem::Decay(flecs::world& world, float dtSeconds)
{
    world.each([&](HitFlash& flash) {
        if (flash.amount > 0.f) {
            flash.amount = std::max(0.f, flash.amount - FLASH_DECAY_PER_SECOND * dtSeconds);
        }
    });

    world.each([&](ShieldFlash& flash) {
        if (flash.amount <= 0.f) return;
        const float rate = flash.plate == ShieldFlash::BUBBLE ? BUBBLE_FLASH_DECAY_PER_SECOND
                                                              : PLATE_FLASH_DECAY_PER_SECOND;
        flash.amount = std::max(0.f, flash.amount - rate * dtSeconds);
    });
}

} // namespace Gravitaris
