#include <algorithm>
#include <cmath>
#include <vector>

#include <chipmunk/chipmunk.h>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/system/ship/ship-controls-system.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/pilot-account.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/missile.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/component/planet.hpp>
#include <gravitaris/game/component/ship-loadout.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/upgrade/upgrade-catalog.hpp>
#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/system/core/physics-system.hpp>
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/system/combat/damage-system.hpp>

namespace Gravitaris {

// What an intercepted missile leaves behind. Softer and shorter-lived than a
// hull's (see DeathSystem's HULL_SHRAPNEL): a round is a fuel tube and a
// warhead, not a magazine and a reactor, and burning one out of the air ought
// to be worth doing rather than trading one danger for another. Enough that a
// pilot who kills a missile in its own face still feels it.
static constexpr ShrapnelBurst MISSILE_SHRAPNEL{
        /*count=*/5,
        /*speedMin=*/90.0,
        /*speedMax=*/170.0,
        /*lifetimeSeconds=*/1.2,
        /*damage=*/3.f,
};

// Forgiveness radius around the swept segment, so a fast bullet's exact
// centerline doesn't have to intersect the target polygon precisely.
static constexpr double BULLET_QUERY_RADIUS = 2.0;

// The same for a beam, and tighter: a bullet's segment is where it travelled
// between two ticks and wants some slack, while a beam is a line the pilot is
// holding and should hit what it is actually drawn across.
static constexpr double BEAM_QUERY_RADIUS = 1.0;

// How often a burning beam raises a hit event. Every tick would spend the whole
// event ring on one held trigger; this is often enough to keep the flash alive
// and the sound going, and each one carries the damage of the whole interval so
// the numbers still add up.
static constexpr std::uint64_t BEAM_HIT_EVENT_TICKS = 6;

// Landing/ram damage below one hull point is discarded entirely.
static constexpr float MIN_LANDING_DAMAGE = 1.f;

// How far above a star's surface the fatal boundary sits, as a fraction of
// its radius. Slightly outside it, so a hull is gone at the moment it looks
// like it touched rather than after visibly sinking into the disc.
//
// This alone cannot be what kills a ship that comes to rest on a sun: the
// hull's own collision shape holds its centre a full ship-radius off the
// surface, which on a 320-unit star is further out than this 2% margin
// reaches, so a gentle arrival parks just outside it and sits there. What
// catches that is the heat below -- this stays as the floor for anything that
// actually reaches the disc, and for the tunnelling case position-testing was
// written for.
static constexpr double STAR_LETHAL_MARGIN = 1.02;

// A star's corona: hull points per second at the surface, falling off with
// the inverse square of distance out to STAR_HEAT_REACH radii, where it
// reaches exactly zero. Smooth to the boundary on purpose -- a hard edge
// would mean a ship parked a metre outside it cooks not at all while one a
// metre inside cooks steadily.
//
// The surface figure kills a stock 100hp fighter in under two seconds and a
// shielded one in about three: long enough to watch the shield go and try to
// pull out, far too short to stay.
static constexpr float STAR_SURFACE_HEAT = 60.f;
static constexpr double STAR_HEAT_REACH = 2.5;

static bool ShieldElementLive(flecs::entity ent, std::uint8_t element)
{
    const ShipLoadout* loadout = ent.try_get<ShipLoadout>();
    return loadout && ShieldElementLive(*loadout, element);
}

static float LeakRoll(std::uint64_t step, std::uint32_t seq, std::uint8_t element);
static float BeamFalloff(double alpha, const WeaponDef::Beam& beam);

DamageSystem::DamageSystem(flecs::world& registry, PhysicsSystem& physicsSystem, GameEventQueue& eventQueue,
                           EntitySpawner& entitySpawner, const UpgradeCatalog& catalog,
                           LagCompensation& lagCompensation)
        : m_registry(registry)
        , m_physicsSystem(physicsSystem)
        , m_eventQueue(eventQueue)
        , m_entitySpawner(entitySpawner)
        , m_catalog(catalog)
        , m_lagCompensation(lagCompensation)
{}

void DamageSystem::Update(std::uint64_t step)
{
    ResolveShipRams();
    ResolveStarContact();
    ResolveBeams(step);

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
        dmg->lastDamagePilotId = 0;

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

        HitSearch search{this, bulletEnt, bullet.team, m_friendlyFire, bullet.shooter};
        QueryFirstHit(space, transf.prevPos, transf.pos, BULLET_QUERY_RADIUS, search);

        if (!search.target.is_alive()) return;

        const Magnum::Vector2 hitPoint = search.point;
        const float toHull =
                AbsorbWithShield(step, search.target, bullet.damage, hitPoint, search.element);

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
            targetDmg.lastDamagePilotId = 0;
            // Zero means nothing fired it and nobody is credited: death
            // shrapnel (DeathSystem) and a structure's turrets both leave it
            // that way. Checked before the lookup, not after -- asking flecs
            // whether entity 0 is alive breaks a precondition it only verifies
            // in a debug build, which is what crashed a release build on the
            // tick after a ship came apart. The hit sweep above guards the same
            // field for the same reason.
            if (bullet.shooter != 0) {
                const flecs::entity shooter = m_registry.entity(bullet.shooter);
                if (shooter.is_alive()) {
                    if (const PilotRef* ref = shooter.try_get<PilotRef>()) {
                        targetDmg.lastDamagePilotId = ref->pilotId;
                    }
                }
            }

            m_eventQueue.Emit(GameEventType::Impact, search.target, hitPoint,
                              static_cast<std::uint32_t>(toHull * 10.f));
        }

        spent.push_back(bulletEnt);
    });

    for (flecs::entity bulletEnt : spent) {
        bulletEnt.destruct();
    }
}

// The nearest DAMAGEABLE shape along the path, not simply the nearest shape:
// planetside structures are nested INSIDE their planet's own collision circle
// (see EntitySpawner::SpawnStructure), so the nearest shape to a shot aimed at
// a Base/Colony/Lab/Comm Center is always the planet -- which isn't damageable.
// Stopping at that one meant such a shot neither hurt the structure nor was
// consumed; it sailed on through. Only structures poking out past their
// planet's rim, or orbiting clear of it (the High Port), could ever be hit.
//
// A planet deliberately does NOT block the shot: it encloses the whole complex,
// so blocking would put every planetside structure back out of reach. Shots
// that meet only bare planet therefore still fly through it -- longstanding,
// and its own fix (structures would have to sit outside the planet's shape).
void DamageSystem::QueryFirstHit(cpSpace* space, const Magnum::Vector2d& from,
                                 const Magnum::Vector2d& to, double radius, HitSearch& search)
{
    const cpShapeFilter filter =
            cpShapeFilterNew(PhysicsSystem::BULLET_GROUP, CP_ALL_CATEGORIES, CP_ALL_CATEGORIES);

    cpSpaceSegmentQuery(space, cpv(from.x(), from.y()), cpv(to.x(), to.y()), radius, filter,
                        [](cpShape* shape, cpVect point, cpVect normal, cpFloat alpha, void* data) {
        auto* s = static_cast<HitSearch*>(data);
        const flecs::entity ent = s->self->m_physicsSystem.GetEntityForShape(shape);
        if (!ent.is_alive() || (s->ignore.is_alive() && ent == s->ignore)) return;
        // Nothing ever shoots itself: the round starts inside its own shooter's
        // hull, and a beam leaves from a mount buried in it, so this is the
        // sweep's first hit every time. A beam that has been deflected back at
        // its owner is the one exception, and it skips the team test below too --
        // a mirror does not check anybody's colours.
        const bool self = s->shooter != 0 && ent.id() == s->shooter;
        if (self && !s->selfIsTarget) return;

        const Team* entTeam = ent.try_get<Team>();
        // A shot meeting its own side passes through unless the round is being
        // fought with friendly fire on.
        if (!self && !s->friendlyFire && entTeam && entTeam->id == s->team) return;

        // A missile is not among these: it carries no Damageable (its airframe
        // is Missile::hp), so nothing in the game mistakes a round for a
        // target, and gunfire flies straight through one. Only a beam can stop
        // a missile, and it does it down a corridor of its own rather than on
        // this line -- see ResolveBeams.
        if (!ent.try_get<Damageable>()) return; // a planet: shots pass through it

        // A spent plate or a dropped bubble is ignored outright rather than
        // recorded as a hit that absorbs nothing, so the shot carries on to
        // whatever is behind it. That -- plus nearest-alpha-wins below -- is
        // the whole gap rule: a round threading the space between two plates
        // simply never meets a shield shape, and the hull polygon behind them
        // is what it lands on.
        const std::optional<std::uint8_t> element = s->self->ShieldElementFor(ent, shape);
        if (element && !ShieldElementLive(ent, *element)) return;

        if (!s->target.is_alive() || alpha < s->alpha) {
            s->target = ent;
            s->point = Magnum::Vector2{static_cast<float>(point.x), static_cast<float>(point.y)};
            s->alpha = alpha;
            s->normal = Magnum::Vector2d{normal.x, normal.y};
            s->element = element;
        }
    }, &search);
}

float DamageSystem::AbsorbWithShield(std::uint64_t step, flecs::entity target, float damage,
                                     const Magnum::Vector2& at, std::optional<std::uint8_t> element)
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

    // Plating stops most rounds whole and lets the rest through, however much
    // charge is left -- which is what keeps it from being strictly better than
    // the bubble's bigger but slower reservoir. Deeper plating leaks less of
    // each round that does get through, not fewer of them.
    const bool leaks = stats.shieldLeakChance > 0.f
            && LeakRoll(step, m_leakSeq++, *element) < stats.shieldLeakChance;
    const float absorbable = leaks ? damage * (1.f - stats.shieldLeakFraction) : damage;
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

void DamageSystem::ResolveStarContact()
{
    struct Star {
        Magnum::Vector2d pos;
        double radius = 0.;
        double lethalRadiusSq = 0.;
        double reachSq = 0.;
    };
    std::vector<Star> stars;
    m_registry.each([&](const Transform& transf, const Planet& planet) {
        if (!planet.star) return;
        const double radius = static_cast<double>(planet.radius) * transf.scale.x();
        const double lethal = radius * STAR_LETHAL_MARGIN;
        const double reach = radius * STAR_HEAT_REACH;
        stars.push_back(Star{transf.pos, radius, lethal * lethal, reach * reach});
    });
    if (stars.empty()) return;

    // Normalizes the inverse-square falloff to reach exactly zero at the edge
    // of the corona, so nothing has to special-case the boundary.
    const double edgeTerm = 1.0 / (STAR_HEAT_REACH * STAR_HEAT_REACH);
    const double heatScale = 1.0 / (1.0 - edgeTerm);

    // Position, not contact: a hull touching the disc is already gone, and
    // waiting for Chipmunk to resolve a collision against it would let a fast
    // enough approach tunnel straight through the star instead.
    m_registry.each([&](flecs::entity entity, const Transform& transf, Damageable& dmg) {
        if (dmg.hp <= 0.f || entity.has<Planet>()) return;

        // Every test below is a positive comparison, and a NaN fails all of
        // them: it passes the reach check without being culled, then the
        // lethal check without being killed, and lands on std::max(0.f, NaN),
        // which returns 0.f. So a hull that arrives here with a non-finite
        // position is destroyed and the star gets the blame, wherever the NaN
        // actually came from. Leave it alone and let it be visibly wrong.
        if (!std::isfinite(transf.pos.x()) || !std::isfinite(transf.pos.y())) return;

        for (const Star& star : stars) {
            const double distSq = (star.pos - transf.pos).dot();
            if (distSq > star.reachSq) continue;

            const Magnum::Vector2 at{static_cast<float>(transf.pos.x()),
                                     static_cast<float>(transf.pos.y())};

            if (distSq <= star.lethalRadiusSq) {
                dmg.hp = 0.f;
                // lastDamageTeam is left as it stands: whoever last put a
                // round into this hull gets the line, exactly as a crash
                // credits the pursuer that drove it into the ground.
                dmg.lastDamageCause = DamageCause::Star;
                m_eventQueue.Emit(GameEventType::Impact, entity, at, 0);
                return;
            }

            const double falloff = star.radius * star.radius / distSq;
            const float heat = STAR_SURFACE_HEAT
                    * static_cast<float>((falloff - edgeTerm) * heatScale * Game::PHYSICS_DELTA);
            if (heat <= 0.f) continue;

            // Shields first, and only while they last -- a bubble bought a
            // few more seconds in the corona is the point of carrying one.
            const float toHull = AbsorbHeatWithShield(entity, heat);
            if (toHull <= 0.f) return;

            dmg.hp = std::max(0.f, dmg.hp - toHull);
            dmg.lastDamageCause = DamageCause::Star;
            return;
        }
    });
}

// Radiation bathes the whole hull rather than striking one face of it, so it
// drains the shield as a pool: the bubble directly, or every live plate at
// once in proportion to what each still holds. Returns what got through.
//
// Deliberately silent -- no per-tick ShieldHit/PlatingHit. This runs every
// tick for every ship in a corona, and one event per ship per tick is a wire
// full of noise describing a condition the client can already see from where
// the ship is and what its bars are doing.
float DamageSystem::AbsorbHeatWithShield(flecs::entity target, float damage)
{
    ShipLoadout* loadout = target.try_get_mut<ShipLoadout>();
    if (!loadout || loadout->shieldHp <= 0.f) return damage;

    const ShipStats stats = m_catalog.ResolveStats(loadout->levels);
    const float absorbed = std::min(loadout->shieldHp, damage);

    if (IsPlated(*loadout)) {
        const float pool = loadout->shieldHp;
        for (std::uint8_t i = 0; i < loadout->plateCount; ++i) {
            if (loadout->plates[i] <= 0.f) continue;
            loadout->plates[i] = std::max(0.f, loadout->plates[i] - absorbed * loadout->plates[i] / pool);
            loadout->plateRegenDelay[i] = stats.shieldRegenDelayTicks;
        }
    }
    else {
        loadout->shieldRegenDelay = stats.shieldRegenDelayTicks;
    }

    // ShieldSystem re-sums this from the plates next tick; keeping it honest
    // now matters for anything else resolving damage in this one.
    loadout->shieldHp = std::max(0.f, loadout->shieldHp - absorbed);
    return damage - absorbed;
}

void DamageSystem::ResolveBeams(std::uint64_t step)
{
    // Rounds burned down this tick, destroyed after the walk for the same
    // reason a spent bullet is: nothing may be destructed mid-iteration.
    std::vector<flecs::entity> intercepted;

    // Every round in flight that a beam could stop, gathered UP FRONT: a flecs
    // query run inside another query's callback silently iterates nothing (see
    // CLAUDE.md), so looking for missiles per beam would quietly intercept
    // none of them.
    struct InFlight {
        flecs::entity entity;
        Magnum::Vector2d pos;
        TeamId team = TeamId::None;
    };
    std::vector<InFlight> rounds;
    m_registry.each([&](flecs::entity ent, const Missile& round, const Bullet& fired,
                        const Transform& transf) {
        // The side that fired a round is on its Bullet: a missile carries no
        // Team of its own.
        if (round.hp > 0.f) rounds.push_back(InFlight{ent, transf.pos, fired.team});
    });

    // Who is burning, gathered before any of it is resolved. Not a style
    // preference: resolving one shooter's beam means holding the whole world
    // back to the tick that shooter was looking at (LagCompensation), and that
    // walks every hull -- a query inside another query's callback silently
    // iterates nothing here (see CLAUDE.md), so it would rewind precisely
    // nobody and every networked shot would quietly go back to missing.
    std::vector<flecs::entity> burning;
    m_registry.each([&](flecs::entity shooter, const Transform&, const Controls& controls,
                        const ShipLoadout&, const PhysicsRef&) {
        if (controls.laserFiring) burning.push_back(shooter);
    });

    for (flecs::entity shooter : burning) {
        if (!shooter.is_alive()) continue;

        const Controls& controls = shooter.get<Controls>();
        const ShipLoadout& loadout = shooter.get<ShipLoadout>();
        const ShipStats stats = m_catalog.ResolveStats(loadout.levels);
        if (!stats.laser || !stats.laser->IsBeam()) continue;

        const WeaponDef::Beam& beam = stats.laser->beam;
        const PhysicsBody& phys = m_physicsSystem.GetBody(shooter.get<PhysicsRef>());
        cpSpace* space = phys.cp.space.get();
        if (!space) continue;

        // Everyone but this shooter, put back where it saw them. Nothing at all
        // for a shot composed inside the sim, which carries no delay.
        const LagCompensation::Rewind rewind(
                m_lagCompensation, LagCompensation::ViewTickOf(step, controls.viewDelay), shooter);

        // Read AFTER the rewind: it moves Transforms as well as bodies, and
        // although it never moves the shooter, taking the reference first would
        // be a reference into a table this may have moved the entity out of.
        const Transform& transf = shooter.get<Transform>();
        const Team* shooterTeam = shooter.try_get<Team>();
        const PilotRef* pilot = shooter.try_get<PilotRef>();
        const float perTick = beam.damagePerSecond * static_cast<float>(Game::PHYSICS_DELTA);

        for (std::size_t mount = 0; mount < MAX_WEAPON_MOUNTS; ++mount) {
            if (loadout.mounts[mount] != MountArm::Laser) continue;

            const ShipControlsSystem::BeamOrigin start = ShipControlsSystem::ComputeBeamOrigin(
                    transf, phys.body.Get(), static_cast<unsigned>(mount), controls.actionFlags.aim);
            const TeamId team = shooterTeam ? shooterTeam->id : TeamId::Blue;

            // One pass per leg of the beam: it leaves the mount, and each time a
            // mirrored hull throws it back it carries on from there with less of
            // itself. `travelled` is measured along the WHOLE path, so the
            // falloff does not start over at a bounce -- what comes back off a
            // plate is the far, spent end of the same beam, not a fresh one.
            Magnum::Vector2d from = start.pos;
            Magnum::Vector2d heading{std::cos(start.angle), std::sin(start.angle)};
            double travelled = 0.;
            float strength = 1.f;
            flecs::entity deflector; // whatever bounced it last, so it cannot re-bounce

            for (unsigned leg = 0; leg <= ShipControlsSystem::MAX_BEAM_BOUNCES; ++leg) {
                const double reach = beam.range - travelled;
                if (reach <= 0. || strength <= 0.f) break;

                const Magnum::Vector2d far = from + heading * reach;
                HitSearch search{this, deflector, team, m_friendlyFire, shooter.id(), leg > 0};
                QueryFirstHit(space, from, far, BEAM_QUERY_RADIUS, search);

                // The interception corridor, measured rather than swept.
                // Chipmunk's segment query prunes by the raw line before it ever
                // applies the query radius, so a shape a few units off the beam
                // is invisible to one however generous the radius -- and a few
                // units off is exactly where a missile is.
                flecs::entity hitRound;
                double roundAlpha = 0.;
                Magnum::Vector2d roundPoint;
                for (const InFlight& live : rounds) {
                    if (!m_friendlyFire && live.team == team) continue;
                    // Another mount may have burned this one through already
                    // this tick; it is not destroyed until the walk ends.
                    const Missile* airframe = live.entity.try_get<Missile>();
                    if (!airframe || airframe->hp <= 0.f) continue;

                    const Magnum::Vector2d toRound = live.pos - from;
                    const double along = Magnum::Math::dot(toRound, heading);
                    if (along < 0. || along > reach) continue;
                    if ((toRound - heading * along).length() > BEAM_INTERCEPT_RADIUS) continue;

                    const double alpha = along / reach;
                    if (hitRound.is_alive() && alpha >= roundAlpha) continue;

                    hitRound = live.entity;
                    roundAlpha = alpha;
                    roundPoint = from + heading * along;
                }

                // A round in front of whatever hull the line found is what the
                // beam meets: no shield to spend, nothing to credit, and no hit
                // event until it comes apart -- what a pilot needs to see is the
                // interception, not the burn. It stops the leg either way, so a
                // missile shields whatever is behind it for as long as it lasts.
                if (hitRound.is_alive() && (!search.target.is_alive() || roundAlpha <= search.alpha)) {
                    const double at = (travelled + roundAlpha * reach) / beam.range;
                    const float burn = perTick * BeamFalloff(at, beam) * strength;
                    Missile& airframe = hitRound.get_mut<Missile>();
                    airframe.hp -= burn;
                    if (airframe.hp <= 0.f) {
                        intercepted.push_back(hitRound);
                        m_eventQueue.Emit(GameEventType::Explosion, hitRound,
                                          Magnum::Vector2{static_cast<float>(roundPoint.x()),
                                                          static_cast<float>(roundPoint.y())});
                    }
                    break;
                }
                if (!search.target.is_alive()) break; // out into empty space

                const double at = (travelled + search.alpha * reach) / beam.range;
                const float carried = perTick * BeamFalloff(at, beam) * strength;
                if (carried <= 0.f) break; // reached, but out where it carries nothing

                // A plate is a mirror: it takes its own share into the shield and
                // the rest leaves the hull as a live beam. Only a plate, and only
                // one that still has charge in the element that was hit -- a
                // spent plate never becomes a hit at all (see QueryFirstHit), so
                // reaching here with an element means there is something left to
                // reflect off.
                const float absorbShare = BeamAbsorbShare(search.target, search.element);
                const float absorbed = carried * absorbShare;

                if (absorbed > 0.f) {
                    const float toHull = AbsorbWithShield(step, search.target, absorbed,
                                                          search.point, search.element);
                    if (toHull > 0.f) {
                        Damageable& targetDmg = search.target.get_mut<Damageable>();
                        targetDmg.hp -= toHull;
                        targetDmg.lastDamageCause = DamageCause::Gunfire;
                        targetDmg.lastDamageTeam = search.target == shooter
                                ? TeamId::None
                                : (shooterTeam ? shooterTeam->id : TeamId::None);
                        targetDmg.lastDamagePilotId = pilot ? pilot->pilotId : 0;

                        // Not every tick. A held beam lands sixty times a second,
                        // and one event apiece would spend the whole 256-entry
                        // ring on a condition the wire already describes -- the
                        // beam itself replicates, so a client can see where it
                        // ends without being told. What these are still for is
                        // the flash and the noise of being hit.
                        if (step % BEAM_HIT_EVENT_TICKS == 0) {
                            m_eventQueue.Emit(GameEventType::Impact, search.target, search.point,
                                              static_cast<std::uint32_t>(
                                                      toHull * BEAM_HIT_EVENT_TICKS * 10.f));
                        }
                    }
                }

                if (absorbShare >= 1.f) break; // swallowed whole: a bubble, or bare hull

                // Off it goes, from where it landed. The deflector is excluded
                // from the next leg so the beam cannot meet the same plate again
                // at the point it just left -- two mirrored hulls still trade it
                // back and forth, which is what the bounce cap is for.
                strength *= 1.f - absorbShare;
                travelled += search.alpha * reach;
                from = Magnum::Vector2d{search.point};
                heading = ShipControlsSystem::ReflectHeading(heading, search.normal);
                deflector = search.target;
            }
        }
    }

    for (flecs::entity round : intercepted) {
        // Two beams can burn the same round through in one tick.
        if (!round.is_alive()) continue;

        // Thrown off the round itself rather than off the point on the beam
        // that killed it: the two are within BEAM_INTERCEPT_RADIUS of each
        // other, and it is the airframe that comes apart.
        if (const Transform* transf = round.try_get<Transform>()) {
            m_entitySpawner.SpawnShrapnel(transf->pos, transf->vel, step, round.id(),
                                          MISSILE_SHRAPNEL);
        }
        round.destruct();
    }
}

float DamageSystem::BeamAbsorbShare(flecs::entity ent, std::optional<std::uint8_t> element)
{
    const ShipLoadout* loadout = ent.try_get<ShipLoadout>();
    if (!loadout) return 1.f;

    if (element) {
        // A spent plate is no mirror; QueryFirstHit already declines to stop a
        // beam on one, so this is belt and braces rather than the live path.
        if (!ShieldElementLive(*loadout, *element)) return 1.f;
    }
    else {
        // No element means the beam landed on the hull polygon, and what that
        // means depends on whether the hull has plates drawn on it at all. With
        // plates, this is the gap rule -- the beam threaded between them and the
        // field never touched it. With none, the emitter falls back to one pooled
        // field wrapping the whole hull (see ShipLoadout::plateCount), so a hull
        // hit IS a field hit and the mirror is the field.
        if (loadout->plateCount > 0 || loadout->shieldHp <= 0.f) return 1.f;
    }

    return m_catalog.ResolveStats(loadout->levels).laserAbsorb;
}

// What a beam still carries at `alpha` of its reach: everything inside
// falloffStart, then straight down to nothing at the far end. This -- not the
// damage figure -- is the weapon: a pilot who wants a laser's numbers has to
// fly into knife range for them.
static float BeamFalloff(double alpha, const WeaponDef::Beam& beam)
{
    const auto travelled = static_cast<float>(std::clamp(alpha, 0.0, 1.0));
    if (travelled <= beam.falloffStart) return 1.f;
    if (beam.falloffStart >= 1.f) return 1.f;

    return 1.f - (travelled - beam.falloffStart) / (1.f - beam.falloffStart);
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
        // Friendlies only ever overlap -- unless the round says otherwise.
        if (!m_friendlyFire && teamA && teamB && teamA->id == teamB->id) continue;

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

// 0..1 from sim state alone (ADR 0001: no std::rand), so a replay and a second
// peer leak on exactly the same rounds. FNV-1a over the tick, the hit's own
// ordinal and the element it landed on, then 24 bits of that as a fraction.
static float LeakRoll(std::uint64_t step, std::uint32_t seq, std::uint8_t element)
{
    std::uint32_t h = 2166136261u;
    const auto mix = [&h](std::uint32_t v) {
        for (int byte = 0; byte < 4; ++byte) {
            h = (h ^ ((v >> (byte * 8)) & 0xffu)) * 16777619u;
        }
    };
    mix(static_cast<std::uint32_t>(step));
    mix(static_cast<std::uint32_t>(step >> 32));
    mix(seq);
    mix(element);
    return static_cast<float>(h & 0xffffffu) / 16777216.f;
}

} // namespace Gravitaris
