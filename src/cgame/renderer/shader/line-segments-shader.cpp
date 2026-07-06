#include <Corrade/Containers/Reference.h>

#include <Magnum/Mesh.h>
#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/Color.h>
#include <Magnum/GL/Context.h>
#include <Magnum/GL/Extensions.h>
#include <Magnum/GL/Shader.h>
#include <Magnum/Shaders/Generic.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/fs/ifilesystem.hpp>

#include <gravitaris/cgame/renderer/shader/line-segments-shader.hpp>

namespace Gravitaris {

using namespace Magnum;



LineSegmentsShader::LineSegmentsShader(IFilesystem& fileSystem)
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

    if (!fileSystem.ReadString("shaders/line-segments.v.glsl", &vertexSource)) {
        LOG(error) << "Could not read vertex shader!";
    }

    if (!fileSystem.ReadString("shaders/lines.f.glsl", &fragmentSource)) {
        LOG(error) << "Could not read fragment shader!";
    }


    vert.addSource(vertexSource);
    frag.addSource(fragmentSource);

    CORRADE_INTERNAL_ASSERT_OUTPUT(GL::Shader::compile({vert, frag}));

    attachShaders({vert, frag});

    bindAttributeLocation(LineSegmentsShader::Position::Location, "position");
    bindAttributeLocation(LineSegmentsShader::InstancePointA::Location, "instancePointA");
    bindAttributeLocation(LineSegmentsShader::InstancePointB::Location, "instancePointB");


    link();

    u_width = uniformLocation("width");
    u_color = uniformLocation("color");
    u_transformationProjectionMatrix = uniformLocation("transformationProjectionMatrix");

    setWidth(1.f);
    setTransformationProjectionMatrix(Matrix3{});

}

LineSegmentsShader& LineSegmentsShader::setWidth(Magnum::Float width)
{
    setUniform(u_width, width);
    return *this;
}

LineSegmentsShader& LineSegmentsShader::setColor(const Magnum::Color3& color)
{
    setUniform(u_color, color.toSrgb());
    return *this;
}

LineSegmentsShader& LineSegmentsShader::setTransformationProjectionMatrix(const Matrix3& matrix)
{
    setUniform(u_transformationProjectionMatrix, matrix);
    return *this;
}
} // Gravitaris
