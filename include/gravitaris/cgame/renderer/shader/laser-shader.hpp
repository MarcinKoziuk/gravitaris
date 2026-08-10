#pragma once

#include <Magnum/Magnum.h>
#include <Magnum/GL/AbstractShaderProgram.h>
#include <Magnum/GL/Attribute.h>
#include <Magnum/GL/Mesh.h>
#include <Magnum/Math/Color.h>
#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Flat triangles with a colour per vertex, alpha included -- which is the one
// thing Line2Shader cannot do (its vertex colour is a vec3, and its primitives
// expand around a centreline rather than being handed corners). A beam is a
// wedge whose alpha runs out across its length, so the gradient IS the
// geometry here.
class LaserShader : public Magnum::GL::AbstractShaderProgram {
private:
    Magnum::Int u_transformationProjectionMatrix = 0;

public:
    typedef Magnum::GL::Attribute<0, Magnum::Vector2> Position;
    typedef Magnum::GL::Attribute<1, Magnum::Color4> Color;

    explicit LaserShader(IFilesystem& filesystem);

    LaserShader& setTransformationProjectionMatrix(const Magnum::Matrix3& matrix);
};

} // namespace Gravitaris
