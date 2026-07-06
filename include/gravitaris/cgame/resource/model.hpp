#pragma once

#include <vector>
#include <unordered_map>
#include <memory>

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
    struct VertexLineStrip {
        Color3 color;
        std::size_t offset;
        std::size_t count;
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
