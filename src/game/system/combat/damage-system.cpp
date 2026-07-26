#include <algorithm>
#include <cmath>
#include <vector>

#include <chipmunk/chipmunk.h>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/system/core/physics-system.hpp>
#include <gravitaris/game/system/combat/damage-system.hpp>

namespace Gravitaris {

// Forgiveness radius around the swept segment, so a fast bullet's exact
// centerline doesn't have to intersect the target polygon precisely.
static constexpr double BULLET_QUERY_RADIUS = 2.0;

// Landing/ram damage tuning. deltaV (impact speed) below the threshold does
// no damage; above it, damage scales linearly. Uprightness matters far more
// than speed: an upright landing shrugs off a hard touchdown almost entirely,
// while a tipped-over one starts hurting at a much lower speed and per-unit
// harder on top of that.
static constexpr double UPRIGHT_SAFE_DELTAV = 30.0;
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
    ResolveShipRams();

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

// networking-plan Phase 9's destroy rule. Nothing here destroys an entity
// itself -- it drives hp to zero and lets DeathSystem (next in the tick
// order) explode it through the one existing death path.
void DamageSystem::ResolveShipRams()
{
    const PhysicsSystem::ShipContactParams& params = m_physicsSystem.GetShipContactParams();

    for (const ShipRamEvent& ev : m_physicsSystem.DrainShipRams()) {
        flecs::entity a(m_registry, ev.a);
        flecs::entity b(m_registry, ev.b);
        if (!a.is_alive() || !b.is_alive()) continue;

        Damageable* dmgA = a.try_get_mut<Damageable>();
        Damageable* dmgB = b.try_get_mut<Damageable>();
        if (!dmgA || !dmgB) continue;
        // Already dead from something earlier this tick -- a ram shouldn't
        // resurrect it as the "survivor".
        if (dmgA->hp <= 0.f || dmgB->hp <= 0.f) continue;

        const Team* teamA = a.try_get<Team>();
        const Team* teamB = b.try_get<Team>();
        if (teamA && teamB && teamA->id == teamB->id) continue; // friendlies only ever overlap

        if (!std::isfinite(ev.massA) || !std::isfinite(ev.massB)) continue;

        const Magnum::Vector2 contact{static_cast<float>(ev.contact.x()), static_cast<float>(ev.contact.y())};
        const auto kill = [&](flecs::entity ship, Damageable& dmg) {
            dmg.hp = 0.f;
            m_eventQueue.Emit(GameEventType::Impact, ship, contact, 0);
        };

        // The lighter ship's momentum bounds what the pair can exchange, so
        // that -- not the heavier one's -- is what decides a mutual kill.
        const double momentum = std::min(ev.massA, ev.massB) * ev.closingSpeed;
        if (momentum >= params.bothDieMomentum) {
            kill(a, *dmgA);
            kill(b, *dmgB);
            continue;
        }

        // Toughness uses *current* hp, so a damaged ship loses a ram it
        // would have won at full health.
        const double toughA = ev.massA * static_cast<double>(dmgA->hp);
        const double toughB = ev.massB * static_cast<double>(dmgB->hp);

        // Evenly matched (two identical ships at equal health is the common
        // case, not a corner one) has no weaker party to pick, and picking
        // by entity id would decide a head-on ram on spawn order. Both die.
        static constexpr double TOUGHNESS_EPSILON = 1e-6;
        if (std::fabs(toughA - toughB) <= TOUGHNESS_EPSILON * std::max(toughA, toughB)) {
            kill(a, *dmgA);
            kill(b, *dmgB);
            continue;
        }

        const bool aWins = toughA > toughB;
        flecs::entity survivor = aWins ? a : b;
        Damageable& survivorDmg = aWins ? *dmgA : *dmgB;
        const double loserMass = aWins ? ev.massB : ev.massA;

        kill(aWins ? b : a, aWins ? *dmgB : *dmgA);

        const auto damage =
                static_cast<float>(loserMass * ev.closingSpeed * params.survivorDamageScale);
        survivorDmg.hp -= damage;
        // May itself be lethal -- DeathSystem's hp <= 0 scan handles that for
        // free, so a hard enough ram kills both without a special case here.
        m_eventQueue.Emit(GameEventType::Impact, survivor, contact, static_cast<std::uint32_t>(damage * 10.f));
    }
}

} // namespace Gravitaris
