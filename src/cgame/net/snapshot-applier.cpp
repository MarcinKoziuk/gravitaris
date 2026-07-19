#include <algorithm>
#include <vector>

#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/team.hpp>
#include <gravitaris/game/component/controls.hpp>
#include <gravitaris/game/component/net-id.hpp>
#include <gravitaris/game/component/gravity-source.hpp>
#include <gravitaris/game/input/input-command.hpp>
#include <gravitaris/game/resource/common/resource-loader.hpp>

#include <gravitaris/cgame/resource/model.hpp>
#include <gravitaris/cgame/component/renderable.hpp>
#include <gravitaris/cgame/component/hit-flash.hpp>
#include <gravitaris/cgame/net/snapshot-applier.hpp>

namespace Gravitaris {

SnapshotApplier::SnapshotApplier(flecs::world& world, ResourceLoader& resourceLoader)
        : m_world(world)
        , m_resourceLoader(resourceLoader)
{}

void SnapshotApplier::Apply(const SnapshotData& snapshot)
{
    // Destroy entities absent from this snapshot first (full snapshots: not
    // being in one means the entity is gone). snapshot.entities is sorted by
    // netId, so a binary search per live entity suffices. Collect-then-mutate,
    // as everywhere else that destroys during iteration.
    std::vector<std::uint32_t> presentIds;
    presentIds.reserve(snapshot.entities.size());
    for (const EntityState& state : snapshot.entities) presentIds.push_back(state.netId);

    std::vector<std::uint32_t> gone;
    for (const auto& [netId, entity] : m_byNetId) {
        if (!std::binary_search(presentIds.begin(), presentIds.end(), netId) || !entity.is_alive()) {
            gone.push_back(netId);
        }
    }
    for (const std::uint32_t netId : gone) {
        const auto it = m_byNetId.find(netId);
        if (it->second.is_alive()) it->second.destruct();
        m_byNetId.erase(it);
    }

    for (const EntityState& state : snapshot.entities) {
        const auto it = m_byNetId.find(state.netId);
        flecs::entity entity = (it != m_byNetId.end()) ? it->second : flecs::entity{};

        if (!entity.is_alive()) {
            entity = m_world.entity();
            entity.emplace<NetId>(state.netId);
            entity.emplace<Transform>(Vector2d{state.pos}, Radd{static_cast<double>(state.rot)},
                                      Vector2d{state.scale}, Vector2d{state.vel});
            if (state.teamId != TeamId::None) {
                entity.emplace<Team>(state.teamId);
            }
            if (state.type == NetEntityType::Ship) {
                entity.emplace<Controls>();
            }
            if (state.type == NetEntityType::Planet) {
                entity.emplace<GravitySource>(GravitySource{state.gravityMass, state.gravityMultiplier});
            }
            entity.emplace<Renderable>(m_resourceLoader.Load<Model>(state.modelId));
            entity.emplace<HitFlash>();
            m_byNetId[state.netId] = entity;
        }

        Transform& t = entity.get_mut<Transform>();
        t.prevPos = t.pos;
        t.pos = Vector2d{state.pos};
        t.rot = Radd{static_cast<double>(state.rot)};
        t.scale = Vector2d{state.scale};
        t.vel = Vector2d{state.vel};
        t.angVel = static_cast<double>(state.angVel);

        if (Controls* controls = entity.try_get_mut<Controls>()) {
            controls->actionFlags = UnpackControlFlags(state.controlsFlags);
        }
        if (GravitySource* source = entity.try_get_mut<GravitySource>()) {
            source->mass = state.gravityMass;
            source->multiplier = state.gravityMultiplier;
        }
    }
}

} // namespace Gravitaris
