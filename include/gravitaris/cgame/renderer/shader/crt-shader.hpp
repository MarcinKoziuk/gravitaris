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
    Magnum::Int u_lineWidthPx;
    Magnum::Int u_periodPx;
    Magnum::Int u_flickerRate;
    Magnum::Int u_flickerAmplitude;
    Magnum::Int u_scanJitterRate;
    Magnum::Int u_scanJitterAmplitude;
    Magnum::Int u_phaseJitterPx;
    Magnum::Int u_imageUnit = 0;

public:
    explicit CrtShader(IFilesystem& fileSystem);

    ~CrtShader() override = default;

    CrtShader& setViewportSize(const Vector2& size);
    CrtShader& setScanlineStrength(Magnum::Float strength);
    // Scanline geometry at the 1080p reference (auto-scaled by window height
    // in the shader): dark-line thickness and line+gap period, in pixels.
    CrtShader& setLineWidthPx(Magnum::Float px);
    CrtShader& setPeriodPx(Magnum::Float px);
    // Temporal instability (phosphor flicker / unstable beam current / raster
    // breathing); see the comment above the matching uniforms in crt.f.glsl.
    CrtShader& setFlickerRate(Magnum::Float rate);
    CrtShader& setFlickerAmplitude(Magnum::Float amplitude);
    CrtShader& setScanJitterRate(Magnum::Float rate);
    CrtShader& setScanJitterAmplitude(Magnum::Float amplitude);
    CrtShader& setPhaseJitterPx(Magnum::Float px);
    // Seconds, from any monotonically-increasing small-magnitude clock — see
    // the caution in GlowPostProcess::EndSceneAndComposite.
    CrtShader& setTime(Magnum::Float time);
    CrtShader& bindImage(Magnum::GL::Texture2D& texture);
};

} // namespace Gravitaris
