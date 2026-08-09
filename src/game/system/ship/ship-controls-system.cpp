#include <algorithm>
#include <array>
#include <cmath>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/util/chipmunk-safe.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/bullet.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/missile.hpp>
#include <gravitaris/game/component/ship-loadout.hpp>
#include <gravitaris/game/event/game-event.hpp>
#include <gravitaris/game/spawner/entity-spawner.hpp>
#include <gravitaris/game/upgrade/upgrade-catalog.hpp>
#include <gravitaris/game/system/core/physics-system.hpp>
#include <gravitaris/game/system/ship/ship-controls-system.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

static constexpr float BOX_HP = 30.f; // a couple of primary hits or one ram
static constexpr double HALF_PI = 1.5707963267948966;

// Least of the tank a burn can be started on. See AdvanceBoost -- it gates
// lighting the injector, never a burn already running.
static constexpr float BOOST_ENGAGE_SHARE = 0.1f;

static cpVect ThrustWithinSpeedLimit(cpBody* body, double thrust, double maxSpeed);
static unsigned PhaseSlotOf(MountArm arm);

ShipControlsSystem::ShipControlsSystem(flecs::world& registry, EntitySpawner& entitySpawner,
                                       PhysicsSystem& physicsSystem, GameEventQueue& eventQueue,
                                       const UpgradeCatalog& catalog)
        : m_registry(registry)
        , m_entitySpawner(entitySpawner)
        , m_physicsSystem(physicsSystem)
        , m_eventQueue(eventQueue)
        , m_catalog(catalog)
{}

static inline void
cpBodyApplyTorque(cpBody *body, cpFloat torque)
{
    cpVect c = cpBodyGetCenterOfGravity(body);
    cpBodyApplyImpulseAtLocalPoint(body, cpv(0.0, torque), cpv(1.0 + c.x, c.y));
    cpBodyApplyImpulseAtLocalPoint(body, cpv(0.0, -torque), cpv(-1.0 + c.x, c.y));
}

std::pair<Vector2d, Vector2d> ShipControlsSystem::ComputeBulletSpawn(const Transform& transf,
                                                                    const PhysicsBody& phys,
                                                                    double muzzleSpeed,
                                                                    const char* hardpoint, unsigned mount)
{
    const Body::Hardpoint* hp = phys.body->FindMount(hardpoint, mount);
    if (!hp) hp = phys.body->FindMount(GUN_HARDPOINT, mount);

    Vector2d pos = transf.pos;
    if (hp) {
        const double s = std::sin(double(transf.rot));
        const double c = std::cos(double(transf.rot));
        pos += Vector2d(hp->pos.x() * c - hp->pos.y() * s,
                        hp->pos.x() * s + hp->pos.y() * c);
    }

    const double heading = double(transf.rot) - HALF_PI; // nose is local -Y
    const Vector2d vel = Vector2d{std::cos(heading), std::sin(heading)} * muzzleSpeed + transf.vel;

    return std::make_pair(pos, vel);
}

ShipControlsSystem::BoostEffect ShipControlsSystem::AdvanceBoost(Controls& controls, const ShipStats& stats)
{
    const std::uint16_t tank = stats.boostTicks;
    if (tank == 0) { // no injector fitted, or it has been pulled at a yard
        controls.boostSpent = 0;
        controls.boostRefill = 0;
        controls.boosting = false;
        return BoostEffect{};
    }
    if (controls.boostSpent > tank) controls.boostSpent = tank; // a rank came off mid-flight

    // A burn is worth starting or it is not on offer. Under BOOST_ENGAGE_SHARE
    // of the tank the trigger does nothing at all, so an injector that has only
    // just begun refilling cannot be tapped for a tick of thrust every few
    // ticks -- which is what "boost" degenerated into once the tank ran dry in
    // a fight. Hysteresis, not a floor on the fuel: the test is on STARTING,
    // and a burn already lit runs to empty.
    const std::uint16_t remaining = static_cast<std::uint16_t>(tank - controls.boostSpent);
    const auto engageFloor = std::max<std::uint16_t>(
            1, static_cast<std::uint16_t>(static_cast<float>(tank) * BOOST_ENGAGE_SHARE));
    const bool fuelled = controls.boosting ? remaining > 0 : remaining >= engageFloor;

    // The injector feeds the engine, so it only burns while the engine is lit:
    // asking for the overburn while coasting spends nothing, and letting go
    // stops the drain where it stands. Killing speed on the way into a planet
    // is still the main use of it -- braking is thrust, pointed retrograde.
    const bool burning = controls.actionFlags.boost && controls.actionFlags.thrustForward
                      && fuelled;
    if (burning) {
        ++controls.boostSpent;
        controls.boostRefill = 0;
    }
    else if (controls.boostSpent > 0 && stats.boostCooldownTicks > 0) {
        // A full tank takes the whole cooldown to come back, so the wait after
        // a burst is proportional to the burst -- and a hull that never
        // emptied it is never held to the full one.
        controls.boostRefill = static_cast<std::uint16_t>(controls.boostRefill + tank);
        while (controls.boostRefill >= stats.boostCooldownTicks && controls.boostSpent > 0) {
            controls.boostRefill =
                    static_cast<std::uint16_t>(controls.boostRefill - stats.boostCooldownTicks);
            --controls.boostSpent;
        }
        if (controls.boostSpent == 0) controls.boostRefill = 0;
    }

    controls.boosting = burning;
    return BoostEffectOf(controls.boosting, stats);
}

ShipControlsSystem::BoostEffect ShipControlsSystem::BoostEffectOf(bool boosting, const ShipStats& stats)
{
    if (!boosting) return BoostEffect{};
    return BoostEffect{static_cast<double>(stats.boostThrustScale),
                       static_cast<double>(stats.boostMaxSpeedScale)};
}

ShipControlsSystem::Motion ShipControlsSystem::MotionOf(const Body& hull, const ShipStats& stats,
                                                        const BoostEffect& boost)
{
    const double thrust = hull.GetThrust() * stats.thrustScale * boost.thrustScale;

    // The drive raises cruise; the overburn's ceiling is a multiple of the
    // hull's OWN number and never moves. So a hull whose engines already
    // out-run a burn gets the thrust from one and no extra speed -- which is
    // what keeps one authored number bounding how fast anything can travel.
    const double cruise = hull.GetMaxSpeed() * stats.maxSpeedScale;
    const double maxSpeed = std::max(cruise, hull.GetMaxSpeed() * boost.maxSpeedScale);

    return Motion{thrust, maxSpeed};
}

ShipControlsSystem::PrimarySet ShipControlsSystem::PrimaryWeapons(const Controls& controls,
                                                                 const ShipStats& stats,
                                                                 const ShipLoadout* loadout)
{
    PrimarySet set;
    if (!loadout) return set;

    const bool heavy = stats.cannon && loadout->cannonAmmo > 0
                    && MountsArmedWith(*loadout, MountArm::Heavy) > 0;
    const bool light = stats.gun && MountsArmedWith(*loadout, MountArm::Light) > 0;

    const Primary heavyPrimary{stats.cannon, stats.cannonCooldownTicks, true, MountArm::Heavy};
    const Primary lightPrimary{stats.gun, stats.fireCooldownTicks, false, MountArm::Light};

    // Both lines at once, each on its own mounts at its own cadence. A hull
    // carrying only one of them needs no special case here -- the other
    // contributes nothing, exactly as it does under a pilot's own choice.
    if (controls.activeWeapon == ActiveWeapon::Both) {
        if (heavy) set.lines[set.count++] = heavyPrimary;
        if (light) set.lines[set.count++] = lightPrimary;
        return set;
    }

    if (controls.activeWeapon == ActiveWeapon::Cannon && heavy) {
        set.lines[set.count++] = heavyPrimary;
        return set;
    }
    // Asked for the guns, or asked for heavy mounts that are dry, empty or
    // unfitted.
    if (light) set.lines[set.count++] = lightPrimary;
    else if (heavy) set.lines[set.count++] = heavyPrimary;
    return set;
}

void ShipControlsSystem::AdvancePrimary(Controls& controls, const ShipStats& stats,
                                        ShipLoadout* loadout, const Body& body,
                                        const ControlFlags& flags,
                                        const std::function<void(const WeaponDef&, unsigned)>& onShot)
{
    // One-shot: the swap happens on the press, not for every tick the key is
    // held down.
    if (flags.toggleWeapon) controls.activeWeapon = NextWeapon(controls.activeWeapon);

    const PrimarySet primaries = PrimaryWeapons(controls, stats, loadout);
    const unsigned mounts = MountsFor(body, WEAPON_HARDPOINT);

    // Only the mounts armed with each line, and phased among themselves rather
    // than by hull position: two heavy mounts either side of an empty one
    // should still interleave. Gathered up front because the three passes
    // below have to happen in order across *all* the lines -- seed, then count
    // down, then fire -- and not line by line.
    std::array<std::array<unsigned, MAX_WEAPON_MOUNTS>, MAX_PRIMARY_LINES> firing{};
    std::array<unsigned, MAX_PRIMARY_LINES> armed{};
    std::array<const void*, MAX_PRIMARY_LINES> weapons{};
    for (unsigned line = 0; line < primaries.count && loadout; ++line) {
        const Primary& primary = primaries.lines[line];
        const unsigned phase = PhaseSlotOf(primary.arm);
        weapons[phase] = primary.weapon;
        for (unsigned i = 0; i < mounts; ++i) {
            if (loadout->mounts[i] == primary.arm) firing[phase][armed[phase]++] = i;
        }
    }

    // Re-phased on a swap as well as on a fresh pull: the lines have their own
    // cadences and their own mounts, so cooldowns left over from another one
    // mean nothing here.
    for (unsigned line = 0; line < primaries.count; ++line) {
        const Primary& primary = primaries.lines[line];
        const unsigned phase = PhaseSlotOf(primary.arm);
        const bool swapped = primary.weapon != controls.firingWeaponIds[phase];
        if (flags.firePrimary && (!controls.firePrimaryWasHeld || swapped)) {
            SeedPhasesAt(controls.gunCooldown, firing[phase], armed[phase],
                         primary.cooldownTicks, FIRE_STAGGER);
        }
    }
    controls.firingWeaponIds = weapons;
    controls.firePrimaryWasHeld = flags.firePrimary;

    // Every mount counts down once a tick, armed or not -- one walk over the
    // array rather than one per line, so a mount both lines somehow named
    // could not be stepped twice.
    for (std::uint32_t& cooldown : controls.gunCooldown) {
        if (cooldown > 0) --cooldown;
    }

    for (unsigned line = 0; line < primaries.count; ++line) {
        const Primary& primary = primaries.lines[line];
        const unsigned phase = PhaseSlotOf(primary.arm);
        for (unsigned slot = 0; slot < armed[phase]; ++slot) {
            const unsigned mount = firing[phase][slot];
            if (!flags.firePrimary || controls.gunCooldown[mount] != 0) continue;
            // A magazine emptied by an earlier mount this same tick stops the
            // rest of them: the fall back to the light guns waits for the next
            // tick, which is one frame nobody can see.
            if (primary.spendsAmmo && (!loadout || loadout->cannonAmmo == 0)) break;

            controls.gunCooldown[mount] = primary.cooldownTicks;
            if (primary.spendsAmmo && loadout) --loadout->cannonAmmo;
            onShot(*primary.weapon, mount);
        }
    }
}

// Which phase slot a line keeps its cadence in. Keyed by the line rather than
// by this tick's firing order, so a magazine running dry -- which drops the
// cannon out of the set -- does not shunt the guns into its slot and re-phase
// barrels that never changed.
static unsigned PhaseSlotOf(MountArm arm)
{
    return arm == MountArm::Heavy ? 0u : 1u;
}

unsigned ShipControlsSystem::MountsFor(const Body& body, const char* hardpoint)
{
    unsigned mounts = body.CountMounts(hardpoint);
    if (mounts == 0) mounts = body.CountMounts(GUN_HARDPOINT);
    if (mounts == 0) mounts = 1; // none authored at all: one round out of the hull's centre

    // Clamped here rather than at each caller: this is the loop bound for the
    // per-mount cooldowns as well as their count, and those are a fixed-width
    // array.
    return std::min<unsigned>(mounts, MAX_WEAPON_MOUNTS);
}

void ShipControlsSystem::SeedMountPhases(std::array<std::uint32_t, MAX_WEAPON_MOUNTS>& cooldowns,
                                         unsigned mounts, std::uint32_t periodTicks, float stagger)
{
    std::array<unsigned, MAX_WEAPON_MOUNTS> run{};
    const unsigned live = std::min<unsigned>(mounts, MAX_WEAPON_MOUNTS);
    for (unsigned i = 0; i < live; ++i) run[i] = i;

    SeedPhasesAt(cooldowns, run, live, periodTicks, stagger);
}

void ShipControlsSystem::SeedPhasesAt(std::array<std::uint32_t, MAX_WEAPON_MOUNTS>& cooldowns,
                                      const std::array<unsigned, MAX_WEAPON_MOUNTS>& mounts,
                                      unsigned count, std::uint32_t periodTicks, float stagger)
{
    const unsigned live = std::min<unsigned>(count, MAX_WEAPON_MOUNTS);
    for (unsigned i = 0; i < live; ++i) {
        if (mounts[i] >= MAX_WEAPON_MOUNTS) continue;

        const float share = static_cast<float>(i) / static_cast<float>(live);
        const auto delay = static_cast<std::uint32_t>(std::lround(share * stagger * periodTicks));

        // The tick that seeds these also runs the countdown, so a mount meant
        // to wait `delay` ticks is seeded one above it. The first is the
        // exception: it fires on the seeding tick itself, which is what makes
        // the trigger feel instant however many barrels follow it.
        cooldowns[mounts[i]] = delay > 0 ? delay + 1 : 0;
    }
}

void ShipControlsSystem::ApplyMovement(cpBody* body, const ControlFlags& flags, double thrust, double maxSpeed)
{
    cpFloat ang = cpBodyGetAngularVelocity(body);
    const cpFloat maxAng = 15.0;

    cpBodyApplyTorque(body, -ang * 4);

    if (flags.rotateLeft && ang < maxAng) {
        cpBodyApplyTorque(body, 20.0);
    }
    if (flags.rotateRight && ang > -maxAng) {
        cpBodyApplyTorque(body, -20.0);
    }
    if (flags.thrustForward) {
        cpBodyApplyForceAtLocalPoint(body, ThrustWithinSpeedLimit(body, thrust, maxSpeed), cpv(0, 0));
    }
}

// The hull's thrust, with any part of it that would push the ship past its
// own speed limit taken out. Only the engine is capped: gravity is applied
// separately (PhysicsSystem::ApplyGravity) and stays free to throw a ship
// well past this, so a slingshot still beats what the thruster alone can do.
//
// Removing a component leaves a force that can point off the thruster's
// axis, which a real rocket couldn't manage. That is the deliberate trade
// for still being able to turn at top speed -- scaling the magnitude down
// instead would take away heading authority exactly when it is most wanted.
static cpVect ThrustWithinSpeedLimit(cpBody* body, double thrust, double maxSpeed)
{
    const cpVect local = cpv(0, -thrust);
    if (maxSpeed <= 0.0) return local; // uncapped hull

    const cpVect vel = cpBodyGetVelocity(body);
    const cpFloat speed = cpvlength(vel);
    if (speed < maxSpeed) return local;

    const cpVect heading = cpBodyGetRotation(body);
    const cpVect world = cpvrotate(local, heading);
    const cpVect travel = cpvmult(vel, 1.0 / speed);

    const cpFloat along = cpvdot(world, travel);
    if (along <= 0.0) return local; // pointed away from travel: braking, always allowed

    return cpvunrotate(cpvsub(world, cpvmult(travel, along)), heading);
}

void ShipControlsSystem::Update(std::uint64_t step)
{
    m_registry.each([&](flecs::entity entity, Transform& transf, PhysicsRef& ref, Controls& scontrols) {
        PhysicsBody& phys = m_physicsSystem.GetBody(ref);
        cpBody* body = phys.cp.body.get();

        ShipLoadout* loadout = entity.try_get_mut<ShipLoadout>();
        // Something with a thruster and no loadout at all -- a freighter -- is
        // not a fighter with its drive pulled: it flies on the hull's own
        // numbers. Only a ship that carries a loadout is held to what it has
        // fitted, which is what makes "no engine, no acceleration" a rule about
        // fighters rather than about everything with an engine bell.
        const ShipStats stats = loadout ? m_catalog.ResolveStats(loadout->levels) : ShipStats{};

        const BoostEffect boost = AdvanceBoost(scontrols, stats);
        const Motion motion = MotionOf(*phys.body, stats, boost);
        ApplyMovement(body, scontrols.actionFlags, motion.thrust, motion.maxSpeed);

        // firePrimary is held, not one-shot; each armed mount's own cooldown
        // paces its own fire rate, so holding the button auto-fires every
        // barrel at its full cadence -- interleaved rather than as a volley.
        // The pacing itself is shared with client-side prediction, so the two
        // cannot drift; only what a round *is* differs between them.
        AdvancePrimary(scontrols, stats, loadout, *phys.body, scontrols.actionFlags,
                       [&](const WeaponDef& gun, unsigned mount) {
            const std::pair<Vector2d, Vector2d> ret =
                    ShipControlsSystem::ComputeBulletSpawn(transf, phys, gun.speed,
                                                           WEAPON_HARDPOINT, mount);

            const Team* shooterTeam = entity.try_get<Team>();
            const NetId* shooterNetId = entity.try_get<NetId>();
            // Fired along the barrel, so a round drawn as a streak
            // (models/bullets/bullet-heavy) lies down its own line of flight
            // rather than across it. A point-shaped round doesn't care.
            flecs::entity bulletEntity =
                    m_entitySpawner.SpawnBullet(gun.modelId, ret.first, ret.second, /*sensor=*/true,
                                                static_cast<double>(transf.rot));
            bulletEntity.emplace<Bullet>(gun.lifetimeSeconds,
                                         shooterTeam ? shooterTeam->id : TeamId::Blue,
                                         gun.damage,
                                         shooterNetId ? shooterNetId->value : 0u,
                                         entity.id());

            // param carries the weapon's id so a listener that never sees the
            // shooter's loadout -- the audio system on a remote client --
            // still knows which gun to play.
            m_eventQueue.Emit(GameEventType::BulletFired, entity,
                              Magnum::Vector2{static_cast<float>(ret.first.x()),
                                              static_cast<float>(ret.first.y())},
                              gun.id);
        });

        // Missiles: same held-button pacing as the primary, but each shot
        // spends a round off the rack the Lab's upgrade filled (ResearchSystem).
        // ownerNetId stays 0 deliberately, unlike a bullet's: a peer's own
        // bullets are omitted from its snapshots because it predicts them
        // locally, and missiles are not predicted -- suppressing them would
        // leave the shooter the only player who never sees their own missile.
        // Tubes run on the same rule as barrels: each on its own cadence,
        // phased apart, so a rack with two of them empties twice as fast as
        // one with a single tube.
        //
        // Only the bays the hull has actually fitted a launcher in: the rack
        // is the ship's, but a tube is a purchase per bay, and the empty ones
        // are holes in the airframe rather than launchers.
        const unsigned missileMounts = stats.missile && phys.body
                                     ? MountsFor(*phys.body, stats.missile->hardpoint.c_str()) : 0;
        std::array<unsigned, MAX_WEAPON_MOUNTS> tubes{};
        unsigned tubeCount = 0;
        for (unsigned bay = 0; bay < missileMounts && loadout; ++bay) {
            if (MissileBayFitted(*loadout, bay)) tubes[tubeCount++] = bay;
        }

        if (scontrols.actionFlags.fireMissile && !scontrols.fireMissileWasHeld) {
            SeedPhasesAt(scontrols.missileCooldown, tubes, tubeCount, stats.missileCooldownTicks,
                         FIRE_STAGGER);
        }
        scontrols.fireMissileWasHeld = scontrols.actionFlags.fireMissile;

        for (unsigned tube = 0; tube < tubeCount; ++tube) {
            const unsigned mount = tubes[tube];
            if (scontrols.missileCooldown[mount] > 0) {
                --scontrols.missileCooldown[mount];
            }
            if (!scontrols.actionFlags.fireMissile || scontrols.missileCooldown[mount] != 0) continue;
            if (!loadout || loadout->missileAmmo == 0) break;

            const WeaponDef& round = *stats.missile;
            scontrols.missileCooldown[mount] = stats.missileCooldownTicks;
            --loadout->missileAmmo;

            const auto [muzzlePos, vel] =
                    ShipControlsSystem::ComputeBulletSpawn(transf, phys, round.speed,
                                                           round.hardpoint.c_str(), mount);

            const Team* shooterTeam = entity.try_get<Team>();
            flecs::entity missile =
                    m_entitySpawner.SpawnBullet(round.modelId, muzzlePos, vel, /*sensor=*/true,
                                                static_cast<double>(transf.rot), Vector2d{1., 1.});
            missile.emplace<Bullet>(round.lifetimeSeconds,
                                    shooterTeam ? shooterTeam->id : TeamId::Blue, round.damage,
                                    /*ownerNetId=*/0u, entity.id());
            // MissileSystem locks a target on its first tick, and steers by
            // this weapon's own guidance envelope.
            missile.emplace<Missile>(Missile{0u, round.id});

            m_eventQueue.Emit(GameEventType::BulletFired, entity,
                              Magnum::Vector2{static_cast<float>(muzzlePos.x()),
                                              static_cast<float>(muzzlePos.y())},
                              round.id);
        }

        if (scontrols.actionFlags.fireSecondary) {
            scontrols.actionFlags.fireSecondary = false;
            std::pair<Vector2d, Vector2d> ret = ShipControlsSystem::ComputeBulletSpawn(
                    transf, phys, stats.gun ? stats.gun->speed : 0.);
            flecs::entity box = m_entitySpawner.SpawnBullet("models/doodads/box"_id, ret.first, transf.vel);
            box.emplace<Damageable>(Damageable{BOX_HP, BOX_HP});

            m_eventQueue.Emit(GameEventType::BulletFired, entity,
                              Magnum::Vector2{static_cast<float>(ret.first.x()),
                                              static_cast<float>(ret.first.y())});
        }
    });
}


} // namespace Gravitaris
