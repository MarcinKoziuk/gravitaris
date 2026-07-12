#include <gravitaris/cgame/resource/model.hpp>
#include <gravitaris/cgame/component/renderable.hpp>

#include <gravitaris/cgame/spawner/centity-spawner.hpp>

namespace Gravitaris {

CEntitySpawner::CEntitySpawner(flecs::world& registry, ResourceLoader& resourceLoader)
    : EntitySpawner(registry, resourceLoader)
{}

void CEntitySpawner::AddRenderable(flecs::entity entity, id_t modelId)
{
    ResourcePtr<const Model> model = m_resourceLoader.Load<Model>(modelId);
    entity.emplace<Renderable>(model);
}

} // namespace Gravitaris
