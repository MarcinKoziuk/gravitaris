#include <algorithm>
#include <cmath>
#include <vector>

#include <chipmunk/chipmunk.h>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/ship-loadout.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/upgrade/upgrade-catalog.hpp>
#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/system/core/physics-system.hpp>
#include <gravitaris/game/system/combat/damage-system.hpp>

namespace Gravitaris {

// Forgiveness radius around the swept segment, so a fast bullet's exact
// centerline doesn't have to intersect the target polygon precisely.
static constexpr double BULLET_QUERY_RADIUS = 2.0;

// Landing/ram damage below one hull point is discarded entirely.
static constexpr float MIN_LANDING_DAMAGE = 1.f;

DamageSystem::DamageSystem(flecs::world& registry, PhysicsSystem& physicsSystem, GameEventQueue& eventQueue,
                           const UpgradeCatalog& catalog)
        : m_registry(registry)
        , m_physicsSystem(physicsSystem)
        , m_eventQueue(eventQueue)
        , m_catalog(catalog)
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

        const double safe = ev.upright ? m_landingParams.uprightThreshold
                                       : m_landingParams.tippedThreshold;
        const double over = ev.deltaV - safe;
        if (over <= 0.0) continue;

        const float multiplier = ev.upright ? 1.f : m_landingParams.tippedMultiplier;
        // The thresholds and the curve are global (and live-tunable from the
        // Physics debug tab); how badly THIS hull takes what comes out of
        // them is the model's own [landing] fragility.
        const float damage = static_cast<float>(over * m_landingParams.damagePerDeltaV) * multiplier
                             * dmg->landingFragility;

        // Sub-point scratches aren't worth an hp change, an event, or a line in
        // the log -- a barely-over-threshold touchdown should read as clean.
        if (damage < MIN_LANDING_DAMAGE) continue;

        LOG(info) << "[landing] deltaV " << ev.deltaV << (ev.upright ? " upright" : " tipped")
                  << " threshold " << safe << " over " << over
                  << " x perDeltaV " << m_landingParams.damagePerDeltaV
                  << " x mult " << multiplier << " x fragility " << dmg->landingFragility
                  << " = " << damage << " damage"
                  << "; hp " << dmg->hp << " -> " << (dmg->hp - damage);

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

        const cpShapeFilter filter =
                cpShapeFilterNew(PhysicsSystem::BULLET_GROUP, CP_ALL_CATEGORIES, CP_ALL_CATEGORIES);

        // The nearest DAMAGEABLE shape along the path, not simply the nearest
        // shape: planetside structures are nested INSIDE their planet's own
        // collision circle (see EntitySpawner::SpawnStructure), so the nearest
        // shape to a shot aimed at a Base/Colony/Lab/Comm Center is always the
        // planet -- which isn't damageable. Stopping at that one meant such a
        // shot neither hurt the structure nor was consumed; it sailed on
        // through. Only structures poking out past their planet's rim, or
        // orbiting clear of it (the High Port), could ever be hit.
        //
        // A planet deliberately does NOT block the shot: it encloses the whole
        // complex, so blocking would put every planetside structure back out
        // of reach. Shots that meet only bare planet therefore still fly
        // through it -- longstanding, and its own fix (structures would have
        // to sit outside the planet's shape).
        struct HitSearch {
            DamageSystem* self;
            flecs::entity bulletEnt;
            TeamId team;
            flecs::entity target;
            cpVect targetPoint{};
            cpFloat targetAlpha = 0.f;
        } search{this, bulletEnt, bullet.team};

        cpSpaceSegmentQuery(space, from, to, BULLET_QUERY_RADIUS, filter,
                            [](cpShape* shape, cpVect point, cpVect, cpFloat alpha, void* data) {
            auto* s = static_cast<HitSearch*>(data);
            const flecs::entity ent = s->self->m_physicsSystem.GetEntityForShape(shape);
            if (!ent.is_alive() || ent == s->bulletEnt) return;

            const Team* entTeam = ent.try_get<Team>();
            if (entTeam && entTeam->id == s->team) return; // no friendly fire

            if (!ent.try_get<Damageable>()) return; // a planet: shots pass through it
            if (!s->target.is_alive() || alpha < s->targetAlpha) {
                s->target = ent;
                s->targetPoint = point;
                s->targetAlpha = alpha;
            }
        }, &search);

        if (!search.target.is_alive()) return;

        const Magnum::Vector2 hitPoint{static_cast<float>(search.targetPoint.x),
                                       static_cast<float>(search.targetPoint.y)};
        const float toHull = AbsorbWithShield(search.target, bullet.damage, hitPoint);

        // A hit stopped entirely by a shield still consumes the round, but
        // emits no Impact -- the hull took nothing, and the hit flash reads
        // as hull damage.
        if (toHull > 0.f) {
            search.target.get_mut<Damageable>().hp -= toHull;

            m_eventQueue.Emit(GameEventType::Impact, search.target, hitPoint,
                              static_cast<std::uint32_t>(toHull * 10.f));
        }

        spent.push_back(bulletEnt);
    });

    for (flecs::entity bulletEnt : spent) {
        bulletEnt.destruct();
    }
}

float DamageSystem::AbsorbWithShield(flecs::entity target, float damage, const Magnum::Vector2& at)
{
    ShipLoadout* loadout = target.try_get_mut<ShipLoadout>();
    if (!loadout || loadout->shieldHp <= 0.f) return damage;

    const ShipStats stats = m_catalog.ResolveStats(loadout->levels);

    // Plating leaks a fixed share of every hit however much charge is left,
    // which is what keeps it from being strictly better than the bubble's
    // bigger but slower reservoir.
    const float absorbable = damage * stats.shieldAbsorbFraction;
    const float absorbed = std::min(loadout->shieldHp, absorbable);

    loadout->shieldHp -= absorbed;
    loadout->shieldRegenDelay = stats.shieldRegenDelayTicks;

    m_eventQueue.Emit(GameEventType::ShieldHit, target, at,
                      static_cast<std::uint32_t>(absorbed * 10.f));

    return damage - absorbed;
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
