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

    // Muzzle position/velocity for a projectile leaving the ship's first
    // hardpoint at `muzzleSpeed` right now. Shared by Update() and
    // client-side prediction (Phase 6) so the cosmetic bullet spawns exactly
    // where the server's will -- which means both sides must resolve the same
    // muzzle speed for the shooter's gun tier, not just call this the same way.
    static std::pair<Magnum::Vector2d, Magnum::Vector2d> ComputeBulletSpawn(const Transform& transf,
                                                                             const PhysicsBody& phys,
                                                                             double muzzleSpeed);
};

} // namespace Gravitaris
