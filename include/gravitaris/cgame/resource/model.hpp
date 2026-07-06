#pragma once

#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>

#include <Magnum/Magnum.h>

#include <gravitaris/gravitaris.hpp>
#include <gravitaris/game/id.hpp>
#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/resource/common/iresource.hpp>
#include <gravitaris/game/resource/common/resource-ptr.hpp>

#include <gravitaris/cgame/resource/shape.hpp>

struct NSVGshape;

namespace Gravitaris {

using Magnum::Vector2;
using Magnum::Color3;

class Model : public IResource {
public:
    // Present only when this strip is an exact circle (see
    // Shape::Path::circle). Renderers that don't know about circles simply
    // ignore this and draw the polyline in `offset`/`count` as always;
    // ModelRenderer2 uses it to draw an analytically perfect circle instead
    // and skips the (still-present, redundant-for-it) polyline.
    struct CircleHint {
        Vector2 center;
        float radius;
    };

    struct VertexLineStrip {
        Color3 color;
        std::size_t offset;
        std::size_t count;
        std::optional<CircleHint> circle;
    };

    struct Group {
        id_t tag{};
        std::vector<Vector2> vertexBuffer;
        std::vector<VertexLineStrip> lineStrips;
    };

private:
    std::unordered_map<id_t, Group> m_groups;

    static ResourcePtr<const Model> placeholder;

    void InitFromShape(const Shape& shape);

public:
    ~Model() override = default;

    [[nodiscard]] std::size_t CalculateSize() const override;

    [[nodiscard]] const char* GetResourceName() const override
    { return "simple-line-segment"; }

    [[nodiscard]] const std::unordered_map<id_t, Group>& GetModelGroups() const
    { return m_groups; }

    static ResourcePtr<const Model> Placeholder();

    static ResourcePtr<const Model> MakePlaceholder();

    static ResourcePtr<const Model> Create(id_t id, LoadingContext& context);

    static ResourcePtr<const Model> Create(id_t id, const Shape& shape);
};

} // namespace Gravitaris
