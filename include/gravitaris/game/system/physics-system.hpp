#pragma once

#include <vector>
#include <unordered_map>

#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>
#include <entt/entity/group.hpp>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/component/physics.hpp>
#include <gravitaris/game/component/transform.hpp>

#include <gravitaris/game/util/chipmunk-safe.hpp>

namespace Gravitaris {

class PhysicsSystem {
private:
    entt::registry& m_registry;

    std::unordered_map<id_t, std::shared_ptr<cpSpace>> m_spaces;

    void InitSpace(id_t spaceId);

    void InitBody(const entt::entity& ent, const Transform& transf, Physics& phys);

    void ApplyGravity(cpSpace* space, double dt);

    void HandlePhysicsAdded(const entt::entity& ent);

    void HandlePhysicsRemoved(const entt::entity& ent);
public:
    explicit PhysicsSystem(entt::registry& registry);

    ~PhysicsSystem();

    void Simulate(double dt);

    void Update();
};

} // namespace Gravitaris
