#include <Corrade/Containers/Reference.h>

#include <Magnum/GL/Context.h>
#include <Magnum/GL/Version.h>
#include <Magnum/GL/Shader.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/fs/ifilesystem.hpp>

#include <gravitaris/cgame/renderer/shader/starfield-shader.hpp>

namespace Gravitaris {

using namespace Magnum;

StarfieldShader::StarfieldShader(IFilesystem& fileSystem)
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

    if (!fileSystem.ReadString("shaders/starfield.v.glsl", &vertexSource)) {
        LOG(error) << "Could not read starfield vertex shader!";
    }

    if (!fileSystem.ReadString("shaders/starfield.f.glsl", &fragmentSource)) {
        LOG(error) << "Could not read starfield fragment shader!";
    }

    vert.addSource(vertexSource);
    frag.addSource(fragmentSource);

    CORRADE_INTERNAL_ASSERT_OUTPUT(GL::Shader::compile({vert, frag}));

    attachShaders({vert, frag});

    bindAttributeLocation(StarfieldShader::Center::Location, "center");
    bindAttributeLocation(StarfieldShader::Corner::Location, "corner");
    bindAttributeLocation(StarfieldShader::Params::Location, "params");
    bindAttributeLocation(StarfieldShader::StarColor::Location, "starColor");

    link();

    u_viewportSize = uniformLocation("viewportSize");
    u_viewProjection = uniformLocation("viewProjection");

    setViewportSize(Vector2{1280.f, 720.f});
    setViewProjection(Matrix3{});
}

StarfieldShader& StarfieldShader::setViewportSize(const Vector2& size)
{
    setUniform(u_viewportSize, size);
    return *this;
}

StarfieldShader& StarfieldShader::setViewProjection(const Matrix3& matrix)
{
    setUniform(u_viewProjection, matrix);
    return *this;
}

} // namespace Gravitaris
