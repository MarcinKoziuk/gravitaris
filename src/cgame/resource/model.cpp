#include <numeric>
#include <gravitaris/cgame/resource/model.hpp>
#include <gravitaris/cgame/team-color.hpp>

namespace Gravitaris {

using Magnum::Vector2;

ResourcePtr<const Model> Model::placeholder = Model::MakePlaceholder();

static std::size_t ModelGroupSize(const Model::Group& group)
{
    return sizeof(group)
        + PodContainerSize(group.vertexBuffer)
        + PodContainerSize(group.lineStrips);
}

std::size_t Model::CalculateSize() const
{
    return sizeof(*this)
        + std::accumulate(
            m_groups.begin(),
            m_groups.end(),
            std::size_t(0L),
            [](std::size_t v, std::pair<id_t, Group> e) {
                return v + ModelGroupSize(e.second);
            }
    );
}

ResourcePtr<const Model> Model::Placeholder()
{
    return Model::placeholder;
}

ResourcePtr<const Model> Model::Create(id_t id, LoadingContext& context)
{
    auto shape = context.resourceLoader.Load<Shape>(id);
    return Create(id, *shape);
}

ResourcePtr<const Model> Model::Create(id_t id, const Shape& shape)
{
    auto lineMesh = MakeResourcePtr<Model>(id);
    lineMesh->InitFromShape(shape);
    return lineMesh;
}

void Model::InitFromShape(const Shape& shape)
{
    std::unordered_map<id_t, std::size_t> offsets;

    for (const Shape::Path& path : shape.GetPaths()) {
        const id_t tag = path.group;

        const std::size_t offset = offsets[tag];

        Group& modelGroup = m_groups[tag];
        modelGroup.tag = tag;

        for (const Vector2d& point : path.points) {
            modelGroup.vertexBuffer.emplace_back(Vector2{static_cast<float>(point.x()), static_cast<float>(point.y())});
        }
        if (path.closed && !path.points.empty()) {
            const auto& first = path.points.front();
            modelGroup.vertexBuffer.emplace_back(Vector2{static_cast<float>(first.x()), static_cast<float>(first.y())});
        }

        const std::size_t newOffset = modelGroup.vertexBuffer.size();

        std::optional<CircleHint> circleHint;
        if (path.circle) {
            circleHint = CircleHint{
                Vector2{static_cast<float>(path.circle->center.x()), static_cast<float>(path.circle->center.y())},
                static_cast<float>(path.circle->radius)};
        }

        modelGroup.lineStrips.emplace_back(VertexLineStrip{
            .color  = path.style.color.rgb(),
            .offset = offset,
            .count  = newOffset - offset,
            .circle = circleHint,
            .teamColor = path.style.color.rgb() == TEAM_COLOR_PLACEHOLDER});
        offsets[tag] = newOffset;
    }

    for (auto& [tag, group] : m_groups) {
        group.vertexBuffer.shrink_to_fit();
    }
}

ResourcePtr<const Model> Model::MakePlaceholder()
{
    return Model::Create("model-placeholder"_id, *Shape::Placeholder());
}

} // namespace Gravitaris
