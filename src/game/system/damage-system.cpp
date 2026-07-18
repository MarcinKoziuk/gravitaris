#include <algorithm>
#include <vector>

#include <chipmunk/chipmunk.h>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/system/physics-system.hpp>
#include <gravitaris/game/system/damage-system.hpp>

namespace Gravitaris {

// Forgiveness radius around the swept segment, so a fast bullet's exact
// centerline doesn't have to intersect the target polygon precisely.
static constexpr double BULLET_QUERY_RADIUS = 2.0;

// Landing/ram damage tuning. deltaV (impact speed) below the threshold does
// no damage; above it, damage scales linearly. Uprightness matters far more
// than speed: an upright landing shrugs off a hard touchdown almost entirely,
// while a tipped-over one starts hurting at a much lower speed and per-unit
// harder on top of that.
static constexpr double UPRIGHT_SAFE_DELTAV = 90.0;
static constexpr double TIPPED_SAFE_DELTAV = 12.0;
static constexpr double DAMAGE_PER_DELTAV = 0.6;
static constexpr float TIPPED_DAMAGE_MULTIPLIER = 3.0f;

DamageSystem::DamageSystem(flecs::world& registry, PhysicsSystem& physicsSystem, GameEventQueue& eventQueue)
        : m_registry(registry)
        , m_physicsSystem(physicsSystem)
        , m_eventQueue(eventQueue)
{}

void DamageSystem::Update()
{
    // Landing / ram damage from this step's hard contacts.
    for (const ImpactEvent& ev : m_physicsSystem.DrainImpacts()) {
        flecs::entity hitEntity(m_registry, ev.entity);
        if (!hitEntity.is_alive()) continue;

        Damageable* dmg = hitEntity.try_get_mut<Damageable>();
        if (!dmg) continue; // planets etc. aren't damageable

        const double safe = ev.upright ? UPRIGHT_SAFE_DELTAV : TIPPED_SAFE_DELTAV;
        const double over = ev.deltaV - safe;
        if (over <= 0.0) continue;

        float damage = static_cast<float>(over * DAMAGE_PER_DELTAV);
        if (!ev.upright) damage *= TIPPED_DAMAGE_MULTIPLIER;

        dmg->hp -= damage;

        m_eventQueue.Emit(GameEventType::LandingCrash, hitEntity,
                          Magnum::Vector2{static_cast<float>(ev.contact.x()),
                                          static_cast<float>(ev.contact.y())},
                          static_cast<std::uint32_t>(damage * 10.f));
    }

    // Bullets that scored a hit this tick; destroyed after the query loop so
    // we don't structurally mutate the world mid-iteration.
    std::vector<flecs::entity> spent;

    m_registry.each([&](flecs::entity bulletEnt, Bullet& bullet, Transform& transf, PhysicsRef& ref) {
        if (transf.pos == transf.prevPos) return;

        cpSpace* space = m_physicsSystem.GetBody(ref).cp.space.get();

        const cpVect from = cpv(transf.prevPos.x(), transf.prevPos.y());
        const cpVect to = cpv(transf.pos.x(), transf.pos.y());

        cpSegmentQueryInfo info;
        const cpShapeFilter filter =
                cpShapeFilterNew(PhysicsSystem::BULLET_GROUP, CP_ALL_CATEGORIES, CP_ALL_CATEGORIES);
        cpShape* hit = cpSpaceSegmentQueryFirst(space, from, to, BULLET_QUERY_RADIUS, filter, &info);
        if (!hit) return;

        flecs::entity hitEntity = m_physicsSystem.GetEntityForShape(hit);
        if (!hitEntity.is_alive() || hitEntity == bulletEnt) return;

        const Team* hitTeam = hitEntity.try_get<Team>();
        if (hitTeam && hitTeam->id == bullet.team) return; // no friendly fire

        Damageable* dmg = hitEntity.try_get_mut<Damageable>();
        if (!dmg) return;

        dmg->hp -= bullet.damage;

        m_eventQueue.Emit(GameEventType::Impact, hitEntity,
                          Magnum::Vector2{static_cast<float>(info.point.x),
                                          static_cast<float>(info.point.y)},
                          static_cast<std::uint32_t>(bullet.damage * 10.f));

        spent.push_back(bulletEnt);
    });

    for (flecs::entity bulletEnt : spent) {
        bulletEnt.destruct();
    }
}

} // namespace Gravitaris
