#include <Corrade/Containers/Reference.h>

#include <Magnum/GL/Context.h>
#include <Magnum/GL/Version.h>
#include <Magnum/GL/Shader.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/fs/ifilesystem.hpp>

#include <gravitaris/cgame/renderer/shader/glow-composite-shader.hpp>

namespace Gravitaris {

using namespace Magnum;

GlowCompositeShader::GlowCompositeShader(IFilesystem& fileSystem)
{
#ifndef MAGNUM_TARGET_GLES
    const GL::Version version = GL::Context::current().supportedVersion({GL::Version::GL320, GL::Version::GL310, GL::Version::GL300});
#else
    const GL::Version version = GL::Context::current().supportedVersion({GL::Version::GLES300});
#endif

    GL::Shader vert{version, GL::Shader::Type::Vertex};
    GL::Shader frag{version, GL::Shader::Type::Fragment};

    std::string vertexSource;
    std::string fragmentSource;

    if (!fileSystem.ReadString("shaders/glow-fullscreen.v.glsl", &vertexSource)) {
        LOG(error) << "Could not read glow fullscreen vertex shader!";
    }

    if (!fileSystem.ReadString("shaders/glow-composite.f.glsl", &fragmentSource)) {
        LOG(error) << "Could not read glow composite fragment shader!";
    }

    vert.addSource(vertexSource);
    frag.addSource(fragmentSource);

    CORRADE_INTERNAL_ASSERT_OUTPUT(GL::Shader::compile({vert, frag}));

    attachShaders({vert, frag});
    link();

    u_intensity = uniformLocation("intensity");
    setUniform(uniformLocation("sceneTex"), u_sceneUnit);
    setUniform(uniformLocation("glowTex"), u_glowUnit);
}

GlowCompositeShader& GlowCompositeShader::setIntensity(Magnum::Float intensity)
{
    setUniform(u_intensity, intensity);
    return *this;
}

GlowCompositeShader& GlowCompositeShader::bindScene(Magnum::GL::Texture2D& texture)
{
    texture.bind(u_sceneUnit);
    return *this;
}

GlowCompositeShader& GlowCompositeShader::bindGlow(Magnum::GL::Texture2D& texture)
{
    texture.bind(u_glowUnit);
    return *this;
}

} // namespace Gravitaris
