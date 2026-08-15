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
#include <gravitaris/game/game.hpp>
#include <gravitaris/game/system/core/physics-system.hpp>
#include <gravitaris/game/system/ship/ship-controls-system.hpp>

namespace Gravitaris {

using Magnum::Vector2d;

static constexpr float BOX_HP = 30.f; // a couple of primary hits or one ram
static constexpr double HALF_PI = 1.5707963267948966;

// Least of the bank a draw can be started on. See AdvanceCapacitor -- it gates
// lighting the injector, never a burn already running.
static constexpr float CAPACITOR_ENGAGE_SHARE = 0.1f;

// Least of a burn a completed windup buys, however briefly the trigger was
// held. A windup cannot be called off, so the shot has to arrive: without this
// a tap would spend a full second of charge on a beam nobody ever saw.
static constexpr float BEAM_MIN_BURST_SECONDS = 0.35f;

static cpVect ThrustWithinSpeedLimit(cpBody* body, double thrust, double maxSpeed);
static unsigned PhaseSlotOf(MountArm arm);
static std::uint16_t SecondsToTicks(float seconds);

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

float ShipControlsSystem::LaserDrainPerTick(const ShipStats& stats, unsigned laserMounts)
{
    if (!stats.laser || laserMounts == 0) return 0.f;
    return stats.laser->beam.energyPerSecond * static_cast<float>(Game::PHYSICS_DELTA)
         * static_cast<float>(laserMounts);
}

std::uint16_t ShipControlsSystem::BeamWindupTicks(const WeaponDef& weapon)
{
    return SecondsToTicks(weapon.beam.windupSeconds);
}

ShipControlsSystem::PowerGrant ShipControlsSystem::AdvanceCapacitor(Controls& controls,
                                                                    const ShipStats& stats,
                                                                    unsigned laserMounts)
{
    const float bank = stats.capacitorCharge;
    if (bank <= 0.f) { // no capacitor fitted, or it has been pulled at a yard
        controls.capacitorSpent = 0.f;
        controls.boosting = false;
        controls.laserFiring = false;
        controls.laserWindup = 0;
        controls.laserBurnOwed = 0;
        return PowerGrant{};
    }
    if (controls.capacitorSpent > bank) controls.capacitorSpent = bank; // a rank came off mid-flight

    // A draw is worth starting or it is not on offer. Under
    // CAPACITOR_ENGAGE_SHARE of the bank the trigger does nothing at all, so a
    // capacitor that has only just begun refilling cannot be tapped for a tick
    // of thrust every few ticks -- which is what "boost" degenerated into once
    // the bank ran dry in a fight. Hysteresis, not a floor on the charge: the
    // test is on STARTING, and a draw already running goes to empty.
    float remaining = bank - controls.capacitorSpent;
    const auto engaged = [&](bool running, float cost) {
        if (cost <= 0.f) return false;
        const float floor = std::max(cost, bank * CAPACITOR_ENGAGE_SHARE);
        return running ? remaining > 0.f : remaining >= floor;
    };

    // The injector feeds the engine, so it only burns while the engine is lit:
    // asking for the overburn while coasting spends nothing, and letting go
    // stops the drain where it stands. Killing speed on the way into a planet
    // is still the main use of it -- braking is thrust, pointed retrograde.
    const bool burning = controls.actionFlags.boost && controls.actionFlags.thrustForward
                      && engaged(controls.boosting, stats.boostDrainPerTick);

    // One accumulated draw rather than a branch per consumer: the bank refills
    // only on a tick nothing at all reached into it. The beams are asked after
    // the burn and against what it left, so a hull doing both runs out of
    // shooting first -- see the header on why that order and not the other.
    float draw = burning ? stats.boostDrainPerTick : 0.f;
    remaining -= draw;

    const float laserDraw = LaserDrainPerTick(stats, laserMounts);
    const bool emitter = laserMounts > 0 && stats.laser && stats.laser->IsBeam();
    if (!emitter) { // an emitter pulled at a yard mid-charge owes nothing
        controls.laserWindup = 0;
        controls.laserBurnOwed = 0;
    }

    // The only thing the trigger can do is start one of these. From here the
    // two counters decide, and neither of them reads the button again: that is
    // what makes a beam a commitment rather than a tap.
    if (emitter && controls.actionFlags.fireLaser && !controls.laserFiring
            && controls.laserWindup == 0 && controls.laserBurnOwed == 0
            && engaged(false, laserDraw)) {
        controls.laserWindup = BeamWindupTicks(*stats.laser);
        controls.laserBurnOwed = SecondsToTicks(BEAM_MIN_BURST_SECONDS);
    }

    bool firing = false;
    if (controls.laserWindup > 0) {
        --controls.laserWindup;
        // Charging costs what burning costs, and takes whatever is there: a
        // bank that runs out on the way up does not stop the windup, it only
        // leaves nothing for it to light with when it arrives.
        const float charge = std::min(laserDraw, std::max(0.f, remaining));
        draw += charge;
        remaining -= charge;
    }
    else if (emitter && (controls.actionFlags.fireLaser || controls.laserBurnOwed > 0)) {
        // Not `engaged(controls.laserFiring, ...)`: a beam whose windup has
        // just finished is a draw already running, however little is left in
        // the bank. The engage floor gates the trigger, and the trigger was
        // answered a whole windup ago.
        firing = engaged(controls.laserFiring || controls.laserBurnOwed > 0, laserDraw);
        if (firing) {
            draw += laserDraw;
            remaining -= laserDraw;
            if (controls.laserBurnOwed > 0) --controls.laserBurnOwed;
        }
        else {
            controls.laserBurnOwed = 0; // the fizzle: a shot that never lit owes nothing
        }
    }

    if (draw > 0.f) {
        controls.capacitorSpent = std::min(bank, controls.capacitorSpent + draw);
    }
    else if (controls.capacitorSpent > 0.f) {
        controls.capacitorSpent = std::max(0.f, controls.capacitorSpent - stats.capacitorRefillPerTick);
    }

    controls.boosting = burning;
    controls.laserFiring = firing;
    return PowerGrant{BoostEffectOf(controls.boosting, stats), firing};
}

Vector2d ShipControlsSystem::ReflectHeading(const Vector2d& heading, const Vector2d& normal)
{
    const double lengthSq = normal.dot();
    if (lengthSq < 1e-12) return heading; // a surface with no direction cannot turn a beam

    const Vector2d unit = normal / std::sqrt(lengthSq);
    return heading - unit * (2. * Magnum::Math::dot(heading, unit));
}

double ShipControlsSystem::ClampAimToArc(double desired, double heading, double halfWidth)
{
    constexpr double PI = 3.141592653589793;
    constexpr double TURN = 6.283185307179586;
    if (halfWidth >= PI) return desired;

    double delta = std::fmod(desired - heading, TURN);
    if (delta > PI) delta -= TURN;
    if (delta < -PI) delta += TURN;

    return heading + std::clamp(delta, -halfWidth, halfWidth);
}

ShipControlsSystem::BeamOrigin ShipControlsSystem::ComputeBeamOrigin(const Transform& transf,
                                                                    const Body* hull,
                                                                    unsigned mount, std::uint16_t aim)
{
    const Body::Hardpoint* hp = hull ? hull->FindMount(WEAPON_HARDPOINT, mount) : nullptr;
    if (!hp && hull) hp = hull->FindMount(GUN_HARDPOINT, mount);

    Vector2d pos = transf.pos;
    if (hp) {
        const double s = std::sin(double(transf.rot));
        const double c = std::cos(double(transf.rot));
        pos += Vector2d(hp->pos.x() * c - hp->pos.y() * s,
                        hp->pos.x() * s + hp->pos.y() * c);
    }

    const double heading = double(transf.rot) - HALF_PI; // nose is local -Y
    const double halfWidth = hull ? hull->GetAimArcHalfWidth() : HALF_PI;

    return BeamOrigin{pos, ClampAimToArc(UnpackAim(aim), heading, halfWidth)};
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

// Rounded up, so an authored duration is never shortened by the tick rate: a
// windup asked for in seconds should not arrive early.
static std::uint16_t SecondsToTicks(float seconds)
{
    if (seconds <= 0.f) return 0;
    const double ticks = std::ceil(static_cast<double>(seconds) / Game::PHYSICS_DELTA);
    return static_cast<std::uint16_t>(std::min(ticks, 65535.0));
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

        // The beams are not paced or spawned here -- they are a state, not a
        // shot. All this tick owes them is the charge, and DamageSystem burns
        // whatever the bank agreed to light (Controls::laserFiring).
        const auto laserMounts =
                static_cast<unsigned>(loadout ? MountsArmedWith(*loadout, MountArm::Laser) : 0);
        const PowerGrant power = AdvanceCapacitor(scontrols, stats, laserMounts);
        const Motion motion = MotionOf(*phys.body, stats, power.boost);
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
            // this weapon's own guidance envelope. The airframe is what a beam
            // has to spend to bring the round down (Missile::hp).
            missile.emplace<Missile>(Missile{0u, round.id, 0u, round.hp});

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
