#include <gravitaris/cgame/resource/model.hpp>
#include <gravitaris/cgame/component/renderable.hpp>

#include <gravitaris/cgame/spawner/centity-spawner.hpp>

namespace Gravitaris {

CEntitySpawner::CEntitySpawner(entt::registry& registry, ResourceLoader& resourceLoader)
    : EntitySpawner(registry, resourceLoader)
{}

void CEntitySpawner::AddRenderable(entt::entity entity, id_t modelId)
{
    ResourcePtr<const Model> model = m_resourceLoader.Load<Model>(modelId);
    m_registry.emplace<Renderable>(entity, model);
}

} // namespace Gravitaris
