#pragma once

#include <cstdint>
#include <functional>
#include <utility>

#include <flecs.h>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/fwd.hpp>

struct cpBody;

namespace Gravitaris {

struct Transform;
struct PhysicsBody;

class ShipControlsSystem {
public:
    // What the overburn is doing to a hull this tick: multipliers on its own
    // thrust and speed cap, both 1 when it is not burning (so a ship without
    // the upgrade needs no branch anywhere downstream).
    struct BoostEffect {
        double thrustScale = 1.;
        double maxSpeedScale = 1.;
    };

    // What ApplyMovement should push with this tick, hull and fittings and
    // overburn already folded together.
    struct Motion {
        double thrust = 0.;
        double maxSpeed = 0.;
    };

    // Nothing about the airframe is a constant here any more either: thrust
    // is the hull's own (Body::GetThrust), so guidance derives its available
    // acceleration as that force over the ship's live mass.

    // Nothing about a projectile is a constant here any more: what a ship
    // fires is a WeaponDef from data/upgrades.toml, picked by whatever the
    // ship has collected (UpgradeCatalog::ResolveStats).

private:
    flecs::world& m_registry;

    EntitySpawner& m_entitySpawner;

    PhysicsSystem& m_physicsSystem;

    GameEventQueue& m_eventQueue;

    const UpgradeCatalog& m_catalog;

public:
    explicit ShipControlsSystem(flecs::world& registry, EntitySpawner& entitySpawner,
                                PhysicsSystem& physicsSystem, GameEventQueue& eventQueue,
                                const UpgradeCatalog& catalog);

    ~ShipControlsSystem() = default;

    void Update(std::uint64_t step);

    // Rotation damping/turn + forward thrust only -- no weapons. Shared by
    // Update() (every ship, every tick) and client-side prediction
    // (docs/networking-plan.md Phase 5, own ship only): predicting a ship's
    // movement without also predicting weapon fire (which would need
    // client-assigned NetIds to reconcile against the server's -- Phase 6)
    // still needs the exact same force/torque the real sim applies.
    // `maxSpeed` caps what the thruster alone can reach (Body::GetMaxSpeed);
    // zero is uncapped. Gravity is applied elsewhere and is not capped, so a
    // slingshot still carries a ship past its engine's limit.
    static void ApplyMovement(cpBody* body, const ControlFlags& flags, double thrust, double maxSpeed);

    // Runs the overburn's timers for one tick and reports what it grants.
    // Both sides of the wire call this off the same input and the same
    // resolved stats -- the sim for every ship, ClientPrediction for the own
    // one -- so a boosted ship is predicted with the force it really got.
    // Call exactly once per ship per simulated tick: it is what counts the
    // burn and the cooldown down.
    static BoostEffect AdvanceBoost(Controls& controls, const ShipStats& stats);

    // The same grant without advancing anything, for replaying an already
    // -decided tick (ClientPrediction's reconciliation) where the timers
    // must not run a second time.
    [[nodiscard]] static BoostEffect BoostEffectOf(bool boosting, const ShipStats& stats);

    // What to hand ApplyMovement: the hull's own thrust and speed cap, the
    // fitted drive's scales, and the burn. One function because the sim and
    // client-side prediction must agree to the last bit -- and because the
    // engine/overburn interaction is a rule rather than a product (see the
    // implementation).
    [[nodiscard]] static Motion MotionOf(const Body& hull, const ShipStats& stats,
                                         const BoostEffect& boost);

    // The mount family every weapon falls back to when the hull carries none
    // of its own (WeaponDef::hardpoint), and the last resort before the hull's
    // own center.
    static constexpr const char* GUN_HARDPOINT = "gun";

    // How far apart a family's mounts are held, as a share of an even spread:
    // 1 puts them at 1/count of the cycle from each other, 0 fires them as one
    // volley. A ship setting eventually -- the pilot may well want the volley
    // -- which is why everything below takes it rather than reading it.
    static constexpr float FIRE_STAGGER = 1.f;

    // Which primary a ship fires this tick, and what it costs to fire it.
    // `weapon` is null on a hull with nothing armed that can shoot.
    struct Primary {
        const WeaponDef* weapon = nullptr;
        std::uint32_t cooldownTicks = 1;
        bool spendsAmmo = false;         // the cannon's magazine
        MountArm arm = MountArm::None;   // which mounts this fires from
    };

    // Every line the trigger works this tick, in firing order. Two when the
    // pilot has asked for both and the hull can serve both, otherwise the one
    // that answers.
    struct PrimarySet {
        std::array<Primary, MAX_PRIMARY_LINES> lines;
        unsigned count = 0;
    };

    // The pilot's choice, honoured where it can be: the heavy mounts when
    // there are any with rounds for them, and the light ones otherwise. A line
    // nothing is armed with is not a choice, however well the rank is owned --
    // fitting and mounting are separate now.
    //
    // Falling back is deliberately not a state change: reloading should put
    // the heavy mounts back in the pilot's hands without a second key press.
    // That is why Both is not simply "no choice at all": a hull set to Both
    // whose magazine runs dry keeps shooting with the guns and picks the
    // cannon back up on its own.
    [[nodiscard]] static PrimarySet PrimaryWeapons(const Controls& controls, const ShipStats& stats,
                                                   const ShipLoadout* loadout);

    // How many mounts a weapon actually fires from, following the same
    // fallback ComputeBulletSpawn does: a hull carrying none of the weapon's
    // own family shoots it out of the gun mounts, and a hull with neither
    // fires one round from its centre. The two must agree, or a weapon would
    // be counted across mounts it never leaves from. Never more than
    // MAX_WEAPON_MOUNTS: this is a loop bound over a fixed-width array.
    [[nodiscard]] static unsigned MountsFor(const Body& body, const char* hardpoint);

    // The family every weapon mount belongs to: one hull position per index,
    // armed by the loadout rather than claimed by a weapon.
    static constexpr const char* WEAPON_HARDPOINT = "weapon";

    // Runs one tick of a ship's primary fire and reports what it fired, so the
    // sim and client-side prediction cannot pace differently. `onShot` is
    // handed the mount index each time a round leaves; everything the two
    // sides do differently with that round (a real bullet, a cosmetic one)
    // stays in the callback.
    static void AdvancePrimary(Controls& controls, const ShipStats& stats, ShipLoadout* loadout,
                               const Body& body, const ControlFlags& flags,
                               const std::function<void(const WeaponDef&, unsigned)>& onShot);

    // Deals each mount its opening wait, in ticks, so the first trigger pull
    // spreads them across one cycle instead of firing them together. Struck
    // from the fraction (i/count) at the moment it is needed rather than kept
    // as a tick count, so a cycle that changes length -- an autoloader fitted
    // between bursts -- re-phases correctly instead of drifting toward a
    // volley. `mounts` is clamped to the array; a hull with more mounts than
    // MAX_WEAPON_MOUNTS leaves the surplus silent.
    static void SeedMountPhases(std::array<std::uint32_t, MAX_WEAPON_MOUNTS>& cooldowns,
                                unsigned mounts, std::uint32_t periodTicks, float stagger);

    // The same deal, but to the hull mounts named rather than to a contiguous
    // run: the mounts one line is armed on are not neighbours, and with both
    // lines live they share this array. `mounts` holds `count` hull indices.
    static void SeedPhasesAt(std::array<std::uint32_t, MAX_WEAPON_MOUNTS>& cooldowns,
                             const std::array<unsigned, MAX_WEAPON_MOUNTS>& mounts, unsigned count,
                             std::uint32_t periodTicks, float stagger);

    // Muzzle position/velocity for a projectile leaving `mount`-th mount of the
    // `hardpoint` family at `muzzleSpeed` right now, falling back to the gun
    // mounts and then to the hull's center. Shared by Update() and client-side
    // prediction (Phase 6) so the cosmetic bullet spawns exactly where the
    // server's will -- which means both sides must resolve the same muzzle
    // speed and the same mount for the shooter's gun tier, not just call this
    // the same way.
    static std::pair<Magnum::Vector2d, Magnum::Vector2d> ComputeBulletSpawn(const Transform& transf,
                                                                             const PhysicsBody& phys,
                                                                             double muzzleSpeed,
                                                                             const char* hardpoint = GUN_HARDPOINT,
                                                                             unsigned mount = 0);
};

} // namespace Gravitaris
