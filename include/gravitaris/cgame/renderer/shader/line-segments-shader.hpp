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

class LineSegmentsShader : public Magnum::GL::AbstractShaderProgram {
protected:
    Magnum::Int u_width;
    Magnum::Int u_color;
    Magnum::Int u_transformationProjectionMatrix;

public:
    typedef Magnum::GL::Attribute<0, Vector2> Position;
    typedef Magnum::GL::Attribute<1, Vector2> InstancePointA;
    typedef Magnum::GL::Attribute<2, Vector2> InstancePointB;


    explicit LineSegmentsShader(IFilesystem& fileSystem);

    ~LineSegmentsShader() override = default;

    LineSegmentsShader& setWidth(Magnum::Float width);
    LineSegmentsShader& setColor(const Color3& color);
    LineSegmentsShader& setTransformationProjectionMatrix(const Matrix3& matrix);
};


} // namespace Gravitaris
