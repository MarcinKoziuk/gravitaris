#pragma once

#include <Magnum/Magnum.h>
#include <Magnum/GL/AbstractShaderProgram.h>
#include <Magnum/GL/Texture.h>
#include <Magnum/Math/Vector2.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

using Magnum::Vector2;

// Simple CRT scanline present pass; see crt.f.glsl. Draws a fullscreen
// triangle generated purely from gl_VertexID.
class CrtShader : public Magnum::GL::AbstractShaderProgram {
protected:
    Magnum::Int u_viewportSize;
    Magnum::Int u_scanlineStrength;
    Magnum::Int u_time;
    Magnum::Int u_imageUnit = 0;

public:
    explicit CrtShader(IFilesystem& fileSystem);

    ~CrtShader() override = default;

    CrtShader& setViewportSize(const Vector2& size);
    CrtShader& setScanlineStrength(Magnum::Float strength);
    // Seconds, from any monotonically-increasing small-magnitude clock — see
    // the caution in GlowPostProcess::EndSceneAndComposite.
    CrtShader& setTime(Magnum::Float time);
    CrtShader& bindImage(Magnum::GL::Texture2D& texture);
};

} // namespace Gravitaris
