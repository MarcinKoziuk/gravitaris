#pragma once

#include <vector>
#include <unordered_map>

#include <flecs.h>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/transform.hpp>

#include <gravitaris/game/util/chipmunk-safe.hpp>

namespace Gravitaris {

class PhysicsSystem {
private:
    flecs::world& m_registry;

    std::unordered_map<id_t, std::shared_ptr<cpSpace>> m_spaces;

    flecs::observer m_physicsAddedObserver;
    flecs::observer m_physicsRemovedObserver;

    void InitSpace(id_t spaceId);

    void InitBody(flecs::entity ent, const Transform& transf, Physics& phys);

    void ApplyGravity(id_t spaceId);

    void HandlePhysicsAdded(flecs::entity ent);

    void HandlePhysicsRemoved(flecs::entity ent);
public:
    explicit PhysicsSystem(flecs::world& registry);

    ~PhysicsSystem();

    void Simulate(double dt);

    void Update();
};

} // namespace Gravitaris
