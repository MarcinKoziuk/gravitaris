#pragma once

#include <cstdint>
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

    // The mount family every weapon falls back to when the hull carries none
    // of its own (WeaponDef::hardpoint), and the last resort before the hull's
    // own center.
    static constexpr const char* GUN_HARDPOINT = "gun";

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
