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
using Magnum::Vector3;
using Magnum::Color3;

class LineMiterShader : public Magnum::GL::AbstractShaderProgram {
protected:
    Magnum::Int u_width;
    Magnum::Int u_color;
    Magnum::Int u_transformationProjectionMatrix;

public:
    typedef Magnum::GL::Attribute<0, Vector3> Position;
    typedef Magnum::GL::Attribute<1, Vector2> InstancePointA;
    typedef Magnum::GL::Attribute<2, Vector2> InstancePointB;
    typedef Magnum::GL::Attribute<3, Vector2> InstancePointC;


    explicit LineMiterShader(IFilesystem& fileSystem);

    ~LineMiterShader() override = default;

    LineMiterShader& setWidth(Magnum::Float width);
    LineMiterShader& setColor(const Color3& color);
    LineMiterShader& setTransformationProjectionMatrix(const Matrix3& matrix);
};

} // namespace Gravitaris
