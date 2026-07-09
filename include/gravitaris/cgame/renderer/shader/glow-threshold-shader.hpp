#pragma once

#include <Magnum/Magnum.h>
#include <Magnum/GL/AbstractShaderProgram.h>
#include <Magnum/GL/Texture.h>
#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

using Magnum::Vector2;

// Bright-pass extraction + 2x box downsample; see glow-threshold.f.glsl.
// Draws a fullscreen triangle generated purely from gl_VertexID.
class GlowThresholdShader : public Magnum::GL::AbstractShaderProgram {
protected:
    Magnum::Int u_texelSize;
    Magnum::Int u_threshold;
    Magnum::Int u_imageUnit = 0;

public:
    explicit GlowThresholdShader(IFilesystem& fileSystem);

    ~GlowThresholdShader() override = default;

    GlowThresholdShader& setTexelSize(const Vector2& texelSize);
    GlowThresholdShader& setThreshold(Magnum::Float threshold);
    GlowThresholdShader& bindImage(Magnum::GL::Texture2D& texture);
};

} // namespace Gravitaris
