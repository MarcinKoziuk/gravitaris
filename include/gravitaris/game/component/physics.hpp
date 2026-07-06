#pragma once

#include <vector>

#include <entt/entity/entity.hpp>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/resource/body.hpp>
#include <gravitaris/game/util/chipmunk-safe.hpp>

namespace Gravitaris {

struct Physics {
    id_t spaceId{};
    ResourcePtr<Body> body;

    // TODO delete
    struct {
        entt::id_type entity_id;
    } cpUserData{};

    struct {
        std::shared_ptr<cpSpace> space;
        cpBodyUniquePtr body;
        std::vector<cpShapeUniquePtr> shapes;
    } cp;

    Physics() = default;

    Physics(id_t spaceId, const ResourcePtr<Body>& body)
            : spaceId(spaceId)
            , body(body)
    {}
};

} // namespace Gravitaris