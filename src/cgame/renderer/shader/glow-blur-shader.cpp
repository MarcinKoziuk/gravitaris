#include <Corrade/Containers/Reference.h>

#include <Magnum/GL/Context.h>
#include <Magnum/GL/Version.h>
#include <Magnum/GL/Shader.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/fs/ifilesystem.hpp>

#include <gravitaris/cgame/renderer/shader/glow-blur-shader.hpp>

namespace Gravitaris {

using namespace Magnum;

GlowBlurShader::GlowBlurShader(IFilesystem& fileSystem)
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

    if (!fileSystem.ReadString("shaders/glow-blur.f.glsl", &fragmentSource)) {
        LOG(error) << "Could not read glow blur fragment shader!";
    }

    vert.addSource(vertexSource);
    frag.addSource(fragmentSource);

    CORRADE_INTERNAL_ASSERT_OUTPUT(GL::Shader::compile({vert, frag}));

    attachShaders({vert, frag});
    link();

    u_direction = uniformLocation("direction");
    setUniform(uniformLocation("image"), u_imageUnit);
}

GlowBlurShader& GlowBlurShader::setDirection(const Vector2& direction)
{
    setUniform(u_direction, direction);
    return *this;
}

GlowBlurShader& GlowBlurShader::bindImage(Magnum::GL::Texture2D& texture)
{
    texture.bind(u_imageUnit);
    return *this;
}

} // namespace Gravitaris
