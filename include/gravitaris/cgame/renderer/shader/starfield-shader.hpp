#pragma once

#include <Magnum/Magnum.h>
#include <Magnum/GL/Mesh.h>
#include <Magnum/GL/AbstractShaderProgram.h>
#include <Magnum/Math/Matrix3.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

using Magnum::Matrix3;
using Magnum::Vector2;
using Magnum::Vector3;

// Background parallax starfield. Each star is a billboard quad expanded in the
// vertex shader from a center point plus a corner offset, sized in pixel space
// so quads stay a sensible on-screen size across zoom.
class StarfieldShader : public Magnum::GL::AbstractShaderProgram {
protected:
    Magnum::Int u_viewportSize;
    Magnum::Int u_viewProjection;

public:
    typedef Magnum::GL::Attribute<0, Vector2> Center;   // layer-space position
    typedef Magnum::GL::Attribute<1, Vector2> Corner;   // quad corner in [-1,1]^2
    typedef Magnum::GL::Attribute<2, Vector2> Params;   // x = size (px), y = brightness
    typedef Magnum::GL::Attribute<3, Vector3> StarColor;

    explicit StarfieldShader(IFilesystem& fileSystem);

    ~StarfieldShader() override = default;

    StarfieldShader& setViewportSize(const Vector2& size);
    StarfieldShader& setViewProjection(const Matrix3& matrix);
};

} // namespace Gravitaris
