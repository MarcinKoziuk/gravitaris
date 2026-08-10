#include <string>

#include <Corrade/Containers/Reference.h>

#include <Magnum/GL/Context.h>
#include <Magnum/GL/Version.h>
#include <Magnum/GL/Shader.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/fs/ifilesystem.hpp>

#include <gravitaris/cgame/renderer/shader/laser-shader.hpp>

namespace Gravitaris {

using namespace Magnum;

LaserShader::LaserShader(IFilesystem& filesystem)
{
#ifndef MAGNUM_TARGET_GLES
    const GL::Version version = GL::Context::current().supportedVersion(
            {GL::Version::GL320, GL::Version::GL310, GL::Version::GL300});
#else
    const GL::Version version = GL::Context::current().supportedVersion({GL::Version::GLES300});
#endif

    GL::Shader vert{version, GL::Shader::Type::Vertex};
    GL::Shader frag{version, GL::Shader::Type::Fragment};

    std::string vertexSource;
    std::string fragmentSource;
    if (!filesystem.ReadString("shaders/laser.v.glsl", &vertexSource)) {
        LOG(error) << "laser: could not read the vertex shader";
    }
    if (!filesystem.ReadString("shaders/laser.f.glsl", &fragmentSource)) {
        LOG(error) << "laser: could not read the fragment shader";
    }

    vert.addSource(vertexSource);
    frag.addSource(fragmentSource);

    CORRADE_INTERNAL_ASSERT_OUTPUT(GL::Shader::compile({vert, frag}));
    attachShaders({vert, frag});

    bindAttributeLocation(Position::Location, "position");
    bindAttributeLocation(Color::Location, "color");

    link();

    u_transformationProjectionMatrix = uniformLocation("transformationProjectionMatrix");
    setTransformationProjectionMatrix(Matrix3{});
}

LaserShader& LaserShader::setTransformationProjectionMatrix(const Matrix3& matrix)
{
    setUniform(u_transformationProjectionMatrix, matrix);
    return *this;
}

} // namespace Gravitaris
