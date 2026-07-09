#include <Corrade/Containers/Reference.h>

#include <Magnum/GL/Context.h>
#include <Magnum/GL/Version.h>
#include <Magnum/GL/Shader.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/fs/ifilesystem.hpp>

#include <gravitaris/cgame/renderer/shader/glow-threshold-shader.hpp>

namespace Gravitaris {

using namespace Magnum;

GlowThresholdShader::GlowThresholdShader(IFilesystem& fileSystem)
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

    if (!fileSystem.ReadString("shaders/postprocess/glow-fullscreen.v.glsl", &vertexSource)) {
        LOG(error) << "Could not read glow fullscreen vertex shader!";
    }

    if (!fileSystem.ReadString("shaders/postprocess/glow-threshold.f.glsl", &fragmentSource)) {
        LOG(error) << "Could not read glow threshold fragment shader!";
    }

    vert.addSource(vertexSource);
    frag.addSource(fragmentSource);

    CORRADE_INTERNAL_ASSERT_OUTPUT(GL::Shader::compile({vert, frag}));

    attachShaders({vert, frag});
    link();

    u_texelSize = uniformLocation("texelSize");
    u_threshold = uniformLocation("threshold");
    setUniform(uniformLocation("image"), u_imageUnit);
}

GlowThresholdShader& GlowThresholdShader::setTexelSize(const Vector2& texelSize)
{
    setUniform(u_texelSize, texelSize);
    return *this;
}

GlowThresholdShader& GlowThresholdShader::setThreshold(Magnum::Float threshold)
{
    setUniform(u_threshold, threshold);
    return *this;
}

GlowThresholdShader& GlowThresholdShader::bindImage(Magnum::GL::Texture2D& texture)
{
    texture.bind(u_imageUnit);
    return *this;
}

} // namespace Gravitaris
