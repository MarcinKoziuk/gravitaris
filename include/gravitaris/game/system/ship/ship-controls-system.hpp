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
    // Forward thrust force (local -Y). Public so guidance can derive the
    // ship's available acceleration (force / mass).
    static constexpr double THRUST_FORCE = 140.0;

    // Ticks between shots while firePrimary is held (weapon cadence).
    // 7 (was 10) is ~50% more bullets/sec (8.6 vs 6).
    static constexpr std::uint32_t FIRE_COOLDOWN_TICKS = 7;

    static constexpr double BULLET_LIFETIME_SECONDS = 3.0;

    // Missiles (the Lab's first upgrade -- ResearchSystem): slower to leave
    // the rail than a bullet, but they fly far longer and hit much harder.
    // Unguided for now; guidance is the next step and will keep these.
    static constexpr std::uint32_t MISSILE_COOLDOWN_TICKS = 30;
    static constexpr double MISSILE_LIFETIME_SECONDS = 10.0;
    static constexpr double MISSILE_MUZZLE_SPEED = 140.0;
    static constexpr float MISSILE_DAMAGE = 45.f;

private:
    flecs::world& m_registry;

    EntitySpawner& m_entitySpawner;

    PhysicsSystem& m_physicsSystem;

    GameEventQueue& m_eventQueue;

public:
    explicit ShipControlsSystem(flecs::world& registry, EntitySpawner& entitySpawner,
                                PhysicsSystem& physicsSystem, GameEventQueue& eventQueue);

    ~ShipControlsSystem() = default;

    void Update(std::uint64_t step);

    // Rotation damping/turn + forward thrust only -- no weapons. Shared by
    // Update() (every ship, every tick) and client-side prediction
    // (docs/networking-plan.md Phase 5, own ship only): predicting a ship's
    // movement without also predicting weapon fire (which would need
    // client-assigned NetIds to reconcile against the server's -- Phase 6)
    // still needs the exact same force/torque the real sim applies.
    static void ApplyMovement(cpBody* body, const ControlFlags& flags);

    // Muzzle position/velocity for a bullet fired right now, from the ship's
    // first hardpoint. Shared by Update() and client-side prediction (Phase
    // 6) so the cosmetic bullet spawns exactly where the server's will.
    static std::pair<Magnum::Vector2d, Magnum::Vector2d> ComputeBulletSpawn(const Transform& transf,
                                                                             const PhysicsBody& phys);
};

} // namespace Gravitaris
