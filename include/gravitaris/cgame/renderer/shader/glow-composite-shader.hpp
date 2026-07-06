#pragma once

#include <Magnum/Magnum.h>
#include <Magnum/GL/AbstractShaderProgram.h>
#include <Magnum/GL/Texture.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

// Additively composites a blurred glow texture on top of the sharp scene.
// Draws a fullscreen triangle generated purely from gl_VertexID.
class GlowCompositeShader : public Magnum::GL::AbstractShaderProgram {
protected:
    Magnum::Int u_intensity;
    Magnum::Int u_sceneUnit = 0;
    Magnum::Int u_glowUnit = 1;

public:
    explicit GlowCompositeShader(IFilesystem& fileSystem);

    ~GlowCompositeShader() override = default;

    GlowCompositeShader& setIntensity(Magnum::Float intensity);
    GlowCompositeShader& bindScene(Magnum::GL::Texture2D& texture);
    GlowCompositeShader& bindGlow(Magnum::GL::Texture2D& texture);
};

} // namespace Gravitaris
