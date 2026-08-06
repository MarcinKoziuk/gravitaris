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

static cpVect ThrustWithinSpeedLimit(cpBody* body, double thrust, double maxSpeed);

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
    if (controls.boostCooldown > 0) --controls.boostCooldown;

    if (controls.boostTicks > 0) {
        --controls.boostTicks;
    }
    // A fresh burn needs the upgrade, the button, and the wait behind it.
    // Thrust is not required: killing speed on the way into a planet is the
    // whole reason this exists, and the ship is pointed retrograde with the
    // engine lit for exactly that.
    else if (stats.boostTicks > 0 && controls.actionFlags.boost && controls.boostCooldown == 0) {
        controls.boostTicks = stats.boostTicks;
        // Started here rather than when the burn ends, so letting go early
        // buys nothing -- the wait is the same either way.
        controls.boostCooldown = static_cast<std::uint16_t>(stats.boostCooldownTicks + stats.boostTicks);
    }

    controls.boosting = controls.boostTicks > 0;
    return BoostEffectOf(controls.boosting, stats);
}

ShipControlsSystem::BoostEffect ShipControlsSystem::BoostEffectOf(bool boosting, const ShipStats& stats)
{
    if (!boosting) return BoostEffect{};
    return BoostEffect{static_cast<double>(stats.boostThrustScale),
                       static_cast<double>(stats.boostMaxSpeedScale)};
}

ShipControlsSystem::Primary ShipControlsSystem::PrimaryWeapon(const Controls& controls,
                                                             const ShipStats& stats,
                                                             const ShipLoadout* loadout)
{
    if (!loadout) return Primary{};

    const bool heavy = stats.cannon && loadout->cannonAmmo > 0
                    && MountsArmedWith(*loadout, MountArm::Heavy) > 0;
    const bool light = stats.gun && MountsArmedWith(*loadout, MountArm::Light) > 0;

    const Primary heavyPrimary{stats.cannon, stats.cannonCooldownTicks, true, MountArm::Heavy};
    const Primary lightPrimary{stats.gun, stats.fireCooldownTicks, false, MountArm::Light};

    if (controls.activeWeapon == ActiveWeapon::Cannon && heavy) return heavyPrimary;
    // Asked for the guns, or asked for heavy mounts that are dry, empty or
    // unfitted.
    if (light) return lightPrimary;
    if (heavy) return heavyPrimary;
    return Primary{};
}

void ShipControlsSystem::AdvancePrimary(Controls& controls, const ShipStats& stats,
                                        ShipLoadout* loadout, const Body& body,
                                        const ControlFlags& flags,
                                        const std::function<void(const WeaponDef&, unsigned)>& onShot)
{
    // One-shot: the swap happens on the press, not for every tick the key is
    // held down.
    if (flags.toggleWeapon) {
        controls.activeWeapon = controls.activeWeapon == ActiveWeapon::Cannon
                              ? ActiveWeapon::Gun : ActiveWeapon::Cannon;
    }

    const Primary primary = PrimaryWeapon(controls, stats, loadout);
    const unsigned mounts = MountsFor(body, WEAPON_HARDPOINT);

    // Only the mounts armed with the line that is firing, and phased among
    // themselves rather than by hull position: two heavy mounts either side of
    // an empty one should still interleave.
    std::array<unsigned, MAX_WEAPON_MOUNTS> firing{};
    unsigned count = 0;
    if (primary.weapon && loadout) {
        for (unsigned i = 0; i < mounts; ++i) {
            if (loadout->mounts[i] == primary.arm) firing[count++] = i;
        }
    }

    // Re-phased on a swap as well as on a fresh pull: the two lines have their
    // own cadences and their own mounts, so cooldowns left over from the other
    // one mean nothing here.
    const bool swapped = primary.weapon != controls.firingWeaponId;
    if (flags.firePrimary && (!controls.firePrimaryWasHeld || swapped)) {
        SeedMountPhases(controls.gunCooldown, count, primary.cooldownTicks, FIRE_STAGGER);
    }
    controls.firePrimaryWasHeld = flags.firePrimary;
    controls.firingWeaponId = primary.weapon;

    for (unsigned slot = 0; slot < count; ++slot) {
        if (controls.gunCooldown[slot] > 0) --controls.gunCooldown[slot];
        if (!flags.firePrimary || controls.gunCooldown[slot] != 0) continue;
        // A magazine emptied by an earlier mount this same tick stops the rest
        // of them: the fall back to the light guns waits for the next tick,
        // which is one frame nobody can see.
        if (primary.spendsAmmo && (!loadout || loadout->cannonAmmo == 0)) break;

        controls.gunCooldown[slot] = primary.cooldownTicks;
        if (primary.spendsAmmo && loadout) --loadout->cannonAmmo;
        onShot(*primary.weapon, firing[slot]);
    }
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
    const unsigned live = std::min<unsigned>(mounts, MAX_WEAPON_MOUNTS);
    for (unsigned i = 0; i < live; ++i) {
        const float share = static_cast<float>(i) / static_cast<float>(live);
        const auto delay = static_cast<std::uint32_t>(std::lround(share * stagger * periodTicks));

        // The tick that seeds these also runs the countdown, so a mount meant
        // to wait `delay` ticks is seeded one above it. Mount 0 is the
        // exception: it fires on the seeding tick itself, which is what makes
        // the trigger feel instant however many barrels follow it.
        cooldowns[i] = delay > 0 ? delay + 1 : 0;
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
        const ShipStats stats =
                m_catalog.ResolveStats(loadout ? loadout->levels : UpgradeLevels{});

        const BoostEffect boost = AdvanceBoost(scontrols, stats);
        ApplyMovement(body, scontrols.actionFlags, phys.body->GetThrust() * boost.thrustScale,
                      phys.body->GetMaxSpeed() * boost.maxSpeedScale);

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
        const unsigned missileMounts = stats.missile && phys.body
                                     ? MountsFor(*phys.body, stats.missile->hardpoint.c_str()) : 0;
        if (scontrols.actionFlags.fireMissile && !scontrols.fireMissileWasHeld) {
            SeedMountPhases(scontrols.missileCooldown, missileMounts, stats.missileCooldownTicks,
                            FIRE_STAGGER);
        }
        scontrols.fireMissileWasHeld = scontrols.actionFlags.fireMissile;

        for (unsigned mount = 0; mount < missileMounts; ++mount) {
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
