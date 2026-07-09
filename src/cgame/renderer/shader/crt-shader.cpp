#include <Corrade/Containers/Reference.h>

#include <Magnum/GL/Context.h>
#include <Magnum/GL/Version.h>
#include <Magnum/GL/Shader.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/fs/ifilesystem.hpp>

#include <gravitaris/cgame/renderer/shader/crt-shader.hpp>

namespace Gravitaris {

using namespace Magnum;

CrtShader::CrtShader(IFilesystem& fileSystem)
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

    if (!fileSystem.ReadString("shaders/postprocess/crt.f.glsl", &fragmentSource)) {
        LOG(error) << "Could not read crt fragment shader!";
    }

    vert.addSource(vertexSource);
    frag.addSource(fragmentSource);

    CORRADE_INTERNAL_ASSERT_OUTPUT(GL::Shader::compile({vert, frag}));

    attachShaders({vert, frag});
    link();

    u_viewportSize = uniformLocation("viewportSize");
    u_scanlineStrength = uniformLocation("scanlineStrength");
    u_time = uniformLocation("time");
    setUniform(uniformLocation("image"), u_imageUnit);
}

CrtShader& CrtShader::setViewportSize(const Vector2& size)
{
    setUniform(u_viewportSize, size);
    return *this;
}

CrtShader& CrtShader::setScanlineStrength(Magnum::Float strength)
{
    setUniform(u_scanlineStrength, strength);
    return *this;
}

CrtShader& CrtShader::setTime(Magnum::Float time)
{
    setUniform(u_time, time);
    return *this;
}

CrtShader& CrtShader::bindImage(Magnum::GL::Texture2D& texture)
{
    texture.bind(u_imageUnit);
    return *this;
}

} // namespace Gravitaris
