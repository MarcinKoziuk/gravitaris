#include <Corrade/Containers/Reference.h>

#include <Magnum/Mesh.h>
#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/Color.h>
#include <Magnum/GL/Context.h>
#include <Magnum/GL/Extensions.h>
#include <Magnum/GL/Shader.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/fs/ifilesystem.hpp>

#include <gravitaris/cgame/renderer/shader/simple-line-shader.hpp>

namespace Gravitaris {

using namespace Magnum;

SimpleLineShader::SimpleLineShader(IFilesystem& fileSystem)
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

    if (!fileSystem.ReadString("shaders/simple/lines.v.glsl", &vertexSource)) {
        LOG(error) << "Could not read vertex shader!";
    }

    if (!fileSystem.ReadString("shaders/simple/lines.f.glsl", &fragmentSource)) {
        LOG(error) << "Could not read fragment shader!";
    }

    vert.addSource(vertexSource);
    frag.addSource(fragmentSource);

    CORRADE_INTERNAL_ASSERT_OUTPUT(GL::Shader::compile({vert, frag}));

    attachShaders({vert, frag});

    bindAttributeLocation(SimpleLineShader::Position::Location, "position");

    link();

    u_transformationProjectionMatrix = uniformLocation("transformationProjectionMatrix");
    u_color = uniformLocation("color");

    setTransformationProjectionMatrix(Matrix3{});
}

SimpleLineShader& SimpleLineShader::setTransformationProjectionMatrix(const Matrix3& matrix)
{
    setUniform(u_transformationProjectionMatrix, matrix);
    return *this;
}

SimpleLineShader& SimpleLineShader::setColor(const Color3& color)
{
    setUniform(u_color, color.toSrgb());
    return *this;
}

} // Gravitaris
