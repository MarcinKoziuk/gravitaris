#include <cmath>
#include <cstdint>
#include <vector>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/util/splitmix.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/system/death-system.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

static constexpr double PI = 3.14159265358979323846;

static constexpr int FRAG_COUNT = 12;
static constexpr double FRAG_SPEED_MIN = 120.0;
static constexpr double FRAG_SPEED_MAX = 240.0;
static constexpr double FRAG_LIFETIME_SECONDS = 0.8;
static constexpr float FRAG_DAMAGE = 8.f;
static constexpr double FRAG_SPAWN_OFFSET = 6.0; // start just off-centre so frags don't share one point

DeathSystem::DeathSystem(flecs::world& registry, EntitySpawner& entitySpawner, GameEventQueue& eventQueue)
        : m_registry(registry)
        , m_entitySpawner(entitySpawner)
        , m_eventQueue(eventQueue)
{}

void DeathSystem::Update(std::uint64_t step)
{
    // Collect first, mutate after: exploding spawns entities and destruct
    // removes them, neither of which is safe mid-iteration.
    std::vector<flecs::entity> dead;
    m_registry.each([&](flecs::entity entity, Damageable& dmg) {
        if (dmg.hp <= 0.f) dead.push_back(entity);
    });

    for (flecs::entity ship : dead) {
        Explode(ship, step);
        ship.destruct();
    }
}

void DeathSystem::Explode(flecs::entity ship, std::uint64_t step)
{
    const Transform* transf = ship.try_get<Transform>();
    if (!transf) return;

    // Emitted before the frag loop (and before the caller destructs the ship)
    // so the event still resolves the ship's NetId.
    m_eventQueue.Emit(GameEventType::Explosion, ship,
                      Magnum::Vector2{static_cast<float>(transf->pos.x()),
                                      static_cast<float>(transf->pos.y())});

    // Deterministic per-(tick, entity) seed so record/replay stays identical.
    std::uint64_t rng = SplitMix64Seed(step, ship.id());

    for (int i = 0; i < FRAG_COUNT; ++i) {
        const double jitter = (SplitMix64NextUnit(rng) - 0.5) * (2.0 * PI / FRAG_COUNT);
        const double angle = (2.0 * PI * i) / FRAG_COUNT + jitter;
        const Vector2d dir{std::cos(angle), std::sin(angle)};

        const double speed = FRAG_SPEED_MIN + SplitMix64NextUnit(rng) * (FRAG_SPEED_MAX - FRAG_SPEED_MIN);
        const Vector2d pos = transf->pos + dir * FRAG_SPAWN_OFFSET;
        const Vector2d vel = transf->vel + dir * speed;

        flecs::entity frag = m_entitySpawner.SpawnBullet(
                "models/bullets/bullet-0"_id, pos, vel, /*sensor=*/true);
        frag.emplace<Bullet>(FRAG_LIFETIME_SECONDS, TeamId::None, FRAG_DAMAGE);
    }
}

} // namespace Gravitaris
