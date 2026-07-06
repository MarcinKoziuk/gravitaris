#pragma once

#include <Magnum/Magnum.h>
#include <Magnum/GL/AbstractShaderProgram.h>
#include <Magnum/GL/Texture.h>
#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

using Magnum::Vector2;

// One pass of a separable Gaussian blur; see glow-blur.f.glsl. Draws a
// fullscreen triangle generated purely from gl_VertexID — no mesh needed.
class GlowBlurShader : public Magnum::GL::AbstractShaderProgram {
protected:
    Magnum::Int u_direction;
    Magnum::Int u_imageUnit = 0;

public:
    explicit GlowBlurShader(IFilesystem& fileSystem);

    ~GlowBlurShader() override = default;

    GlowBlurShader& setDirection(const Vector2& direction);
    GlowBlurShader& bindImage(Magnum::GL::Texture2D& texture);
};

} // namespace Gravitaris
