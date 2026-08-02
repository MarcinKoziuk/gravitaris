#include <algorithm>
#include <cmath>
#include <vector>

#include <chipmunk/chipmunk.h>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/missile.hpp>
#include <gravitaris/game/component/planet.hpp>
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

// How far above a star's surface the fatal boundary sits, as a fraction of
// its radius. Slightly outside it, so a hull is gone at the moment it looks
// like it touched rather than after visibly sinking into the disc.
static constexpr double STAR_LETHAL_MARGIN = 1.02;

static bool ShieldElementLive(flecs::entity ent, std::uint8_t element);

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
    ResolveStarContact();

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
        dmg->lastDamageCause = DamageCause::Crash;
        dmg->lastDamageTeam = TeamId::None;

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
            std::optional<std::uint8_t> targetElement;
        } search{this, bulletEnt, bullet.team};

        cpSpaceSegmentQuery(space, from, to, BULLET_QUERY_RADIUS, filter,
                            [](cpShape* shape, cpVect point, cpVect, cpFloat alpha, void* data) {
            auto* s = static_cast<HitSearch*>(data);
            const flecs::entity ent = s->self->m_physicsSystem.GetEntityForShape(shape);
            if (!ent.is_alive() || ent == s->bulletEnt) return;

            const Team* entTeam = ent.try_get<Team>();
            if (entTeam && entTeam->id == s->team) return; // no friendly fire

            if (!ent.try_get<Damageable>()) return; // a planet: shots pass through it

            // A spent plate or a dropped bubble is ignored outright rather
            // than recorded as a hit that absorbs nothing, so the shot carries
            // on to whatever is behind it. That -- plus nearest-alpha-wins
            // below -- is the whole gap rule: a round threading the space
            // between two plates simply never meets a shield shape, and the
            // hull polygon behind them is what it lands on.
            const std::optional<std::uint8_t> element = s->self->ShieldElementFor(ent, shape);
            if (element && !ShieldElementLive(ent, *element)) return;

            if (!s->target.is_alive() || alpha < s->targetAlpha) {
                s->target = ent;
                s->targetPoint = point;
                s->targetAlpha = alpha;
                s->targetElement = element;
            }
        }, &search);

        if (!search.target.is_alive()) return;

        const Magnum::Vector2 hitPoint{static_cast<float>(search.targetPoint.x),
                                       static_cast<float>(search.targetPoint.y)};
        const float toHull = AbsorbWithShield(search.target, bullet.damage, hitPoint, search.targetElement);

        // A hit stopped entirely by a shield still consumes the round, but
        // emits no Impact -- the hull took nothing, and the hit flash reads
        // as hull damage.
        if (toHull > 0.f) {
            Damageable& targetDmg = search.target.get_mut<Damageable>();
            targetDmg.hp -= toHull;
            // Shrapnel is the ownerless team, so it credits nobody -- which is
            // also what tells a death by debris from one by gunfire.
            targetDmg.lastDamageCause = bullet.team == TeamId::None ? DamageCause::Debris
                                        : bulletEnt.has<Missile>() ? DamageCause::Missile
                                                                   : DamageCause::Gunfire;
            targetDmg.lastDamageTeam = bullet.team;

            m_eventQueue.Emit(GameEventType::Impact, search.target, hitPoint,
                              static_cast<std::uint32_t>(toHull * 10.f));
        }

        spent.push_back(bulletEnt);
    });

    for (flecs::entity bulletEnt : spent) {
        bulletEnt.destruct();
    }
}

float DamageSystem::AbsorbWithShield(flecs::entity target, float damage, const Magnum::Vector2& at,
                                     std::optional<std::uint8_t> element)
{
    ShipLoadout* loadout = target.try_get_mut<ShipLoadout>();
    if (!loadout || loadout->shieldHp <= 0.f) return damage;

    // No shield shape was struck. On a hull that authors shield geometry that
    // means the round found a real gap and the hull eats it; on one that
    // doesn't (structures, and any model not yet drawn a '+shield' layer) the
    // emitter falls back to the pooled behaviour it had before the geometry
    // existed, so collecting a shield is never a dead pickup.
    if (!element) {
        if (HasShieldGeometry(target)) return damage;
        element = PhysicsBody::SHIELD_BUBBLE;
    }

    const ShipStats stats = m_catalog.ResolveStats(loadout->levels);

    // Plating leaks a fixed share of every hit however much charge is left,
    // which is what keeps it from being strictly better than the bubble's
    // bigger but slower reservoir.
    const float absorbable = damage * stats.shieldAbsorbFraction;
    float absorbed;

    if (*element == PhysicsBody::SHIELD_BUBBLE) {
        absorbed = std::min(loadout->shieldHp, absorbable);
        loadout->shieldHp -= absorbed;
        loadout->shieldRegenDelay = stats.shieldRegenDelayTicks;

        m_eventQueue.Emit(GameEventType::ShieldHit, target, at,
                          static_cast<std::uint32_t>(absorbed * 10.f));
    }
    else {
        // Only the struck plate pays, and only it goes quiet -- the far side
        // of the ship keeps its charge and keeps regenerating.
        float& plate = loadout->plates[*element];
        absorbed = std::min(plate, absorbable);
        plate -= absorbed;
        loadout->plateRegenDelay[*element] = stats.shieldRegenDelayTicks;
        // ShieldSystem re-sums this next tick; keeping it honest now matters
        // for a second round landing in the same one.
        loadout->shieldHp = std::max(0.f, loadout->shieldHp - absorbed);

        m_eventQueue.Emit(GameEventType::PlatingHit, target, at,
                          PackPlatingHit(*element, static_cast<std::uint32_t>(absorbed * 10.f)));
    }

    return damage - absorbed;
}

std::optional<std::uint8_t> DamageSystem::ShieldElementFor(flecs::entity ent, const cpShape* shape)
{
    const PhysicsRef* ref = ent.try_get<PhysicsRef>();
    if (!ref) return std::nullopt;
    return m_physicsSystem.GetBody(*ref).ShieldElementOf(shape);
}

bool DamageSystem::HasShieldGeometry(flecs::entity ent)
{
    const PhysicsRef* ref = ent.try_get<PhysicsRef>();
    return ref && !m_physicsSystem.GetBody(*ref).shieldShapes.empty();
}

// A shield element stops a shot only while it is the emitter the ship is
// actually carrying and still has charge to spend against it.
static bool ShieldElementLive(flecs::entity ent, std::uint8_t element)
{
    const ShipLoadout* loadout = ent.try_get<ShipLoadout>();
    if (!loadout) return false;

    if (element == PhysicsBody::SHIELD_BUBBLE) {
        return loadout->levels.shieldType == ShieldType::Bubble && loadout->shieldHp > 0.f;
    }

    return IsPlated(*loadout) && element < loadout->plateCount && loadout->plates[element] > 0.f;
}

void DamageSystem::ResolveStarContact()
{
    struct Star {
        Magnum::Vector2d pos;
        double lethalRadiusSq = 0.;
    };
    std::vector<Star> stars;
    m_registry.each([&](const Transform& transf, const Planet& planet) {
        if (!planet.star) return;
        const double radius = static_cast<double>(planet.radius) * transf.scale.x() * STAR_LETHAL_MARGIN;
        stars.push_back(Star{transf.pos, radius * radius});
    });
    if (stars.empty()) return;

    // Position, not contact: a hull touching the disc is already gone, and
    // waiting for Chipmunk to resolve a collision against it would let a fast
    // enough approach tunnel straight through the star instead.
    m_registry.each([&](flecs::entity entity, const Transform& transf, Damageable& dmg) {
        if (dmg.hp <= 0.f || entity.has<Planet>()) return;

        for (const Star& star : stars) {
            if ((star.pos - transf.pos).dot() > star.lethalRadiusSq) continue;

            dmg.hp = 0.f;
            // lastDamageTeam is left as it stands: whoever last put a round
            // into this hull gets the line, exactly as a crash credits the
            // pursuer that drove it into the ground.
            dmg.lastDamageCause = DamageCause::Star;
            m_eventQueue.Emit(GameEventType::Impact, entity,
                              Magnum::Vector2{static_cast<float>(transf.pos.x()),
                                              static_cast<float>(transf.pos.y())},
                              0);
            return;
        }
    });
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
        const auto credit = [](Damageable& dmg, const Team* other) {
            dmg.lastDamageCause = DamageCause::Ram;
            dmg.lastDamageTeam = other ? other->id : TeamId::None;
        };
        const auto kill = [&](flecs::entity ship, Damageable& dmg, const Team* other) {
            dmg.hp = 0.f;
            credit(dmg, other);
            m_eventQueue.Emit(GameEventType::Impact, ship, contact, 0);
        };

        // The lighter ship's momentum bounds what the pair can exchange, so
        // that -- not the heavier one's -- is what decides a mutual kill.
        const double momentum = std::min(ev.massA, ev.massB) * ev.closingSpeed;
        if (momentum >= params.bothDieMomentum) {
            kill(a, *dmgA, teamB);
            kill(b, *dmgB, teamA);
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
            kill(a, *dmgA, teamB);
            kill(b, *dmgB, teamA);
            continue;
        }

        const bool aWins = toughA > toughB;
        flecs::entity survivor = aWins ? a : b;
        Damageable& survivorDmg = aWins ? *dmgA : *dmgB;
        const double loserMass = aWins ? ev.massB : ev.massA;

        kill(aWins ? b : a, aWins ? *dmgB : *dmgA, aWins ? teamA : teamB);

        const auto damage =
                static_cast<float>(loserMass * ev.closingSpeed * params.survivorDamageScale);
        survivorDmg.hp -= damage;
        credit(survivorDmg, aWins ? teamB : teamA);
        // May itself be lethal -- DeathSystem's hp <= 0 scan handles that for
        // free, so a hard enough ram kills both without a special case here.
        m_eventQueue.Emit(GameEventType::Impact, survivor, contact, static_cast<std::uint32_t>(damage * 10.f));
    }
}

} // namespace Gravitaris
