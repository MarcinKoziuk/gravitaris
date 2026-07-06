#pragma once

#include <unordered_map>
#include <unordered_set>

#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

#include <Magnum/Magnum.h>
#include <Magnum/GL/Mesh.h>
#include <Magnum/GL/AbstractShaderProgram.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

using Magnum::Matrix3;
using Magnum::Vector2;
using Magnum::Color3;

class SimpleLineShader : public Magnum::GL::AbstractShaderProgram {
private:
    Magnum::Int u_transformationProjectionMatrix;
    Magnum::Int u_color;

public:
    typedef Magnum::GL::Attribute<0, Vector2> Position;

    explicit SimpleLineShader(IFilesystem& fileSystem);

    SimpleLineShader& setTransformationProjectionMatrix(const Matrix3& matrix);
    SimpleLineShader& setColor(const Color3& color);
};

} // namespace Gravitaris
