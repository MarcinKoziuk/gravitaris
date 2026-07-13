#pragma once

#include <vector>
#include <unordered_map>

#include <flecs.h>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/transform.hpp>

#include <gravitaris/game/util/chipmunk-safe.hpp>

namespace Gravitaris {

// Chipmunk state for one entity. Owned by PhysicsSystem, not stored in the
// ECS: cpSpace holds raw pointers into this, so it must never be relocated
// by archetype moves, and it can be bulk-freed per space on level unload
// (see docs/adr/0002-physics-ownership.md).
struct PhysicsBody {
    id_t spaceId{};
    ResourcePtr<Body> body;
    std::uint32_t generation = 0;

    struct {
        std::shared_ptr<cpSpace> space;
        cpBodyUniquePtr body;
        std::vector<cpShapeUniquePtr> shapes;
    } cp;

    [[nodiscard]] bool IsAlive() const { return cp.body != nullptr; }
};

class PhysicsSystem {
private:
    flecs::world& m_registry;

    std::unordered_map<id_t, std::shared_ptr<cpSpace>> m_spaces;

    // Slot storage + free-list; PhysicsRef indexes into m_bodies. Declared
    // after m_spaces so slots (whose deleters may touch their space) are
    // destroyed first.
    std::vector<PhysicsBody> m_bodies;
    std::vector<std::uint32_t> m_freeList;

    flecs::observer m_bodyAddedObserver;
    flecs::observer m_bodyRemovedObserver;

    void InitSpace(id_t spaceId);

    void InitBody(PhysicsBody& slot, const Transform& transf);

    std::uint32_t Allocate();

    void ApplyGravity(id_t spaceId);

    void HandleBodyAdded(flecs::entity ent, const RigidBodyDesc& desc);

    void HandleBodyRemoved(const PhysicsRef& ref);

public:
    explicit PhysicsSystem(flecs::world& registry);

    ~PhysicsSystem();

    [[nodiscard]] PhysicsBody& GetBody(const PhysicsRef& ref);

    // Arena-style level teardown: frees every body/shape in the space and
    // the space itself in one pass, skipping the per-object cpSpaceRemove*
    // the deleters do (pointless when the whole space dies). Stale
    // PhysicsRefs on still-live entities become no-ops via the generation.
    void UnloadSpace(id_t spaceId);

    void Simulate(double dt);

    void Update();
};

} // namespace Gravitaris
