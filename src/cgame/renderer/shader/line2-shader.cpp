#include <Corrade/Containers/Reference.h>

#include <Magnum/Mesh.h>
#include <Magnum/Math/Matrix3.h>
#include <Magnum/GL/Context.h>
#include <Magnum/GL/Version.h>
#include <Magnum/GL/Shader.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/fs/ifilesystem.hpp>

#include <gravitaris/cgame/renderer/shader/line2-shader.hpp>

namespace Gravitaris {

using namespace Magnum;

Line2Shader::Line2Shader(IFilesystem& fileSystem)
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

    if (!fileSystem.ReadString("shaders/line2.v.glsl", &vertexSource)) {
        LOG(error) << "Could not read line2 vertex shader!";
    }

    if (!fileSystem.ReadString("shaders/line2.f.glsl", &fragmentSource)) {
        LOG(error) << "Could not read line2 fragment shader!";
    }

    vert.addSource(vertexSource);
    frag.addSource(fragmentSource);

    CORRADE_INTERNAL_ASSERT_OUTPUT(GL::Shader::compile({vert, frag}));

    attachShaders({vert, frag});

    bindAttributeLocation(Line2Shader::PointA::Location, "pointA");
    bindAttributeLocation(Line2Shader::PointB::Location, "pointB");
    bindAttributeLocation(Line2Shader::PointC::Location, "pointC");
    bindAttributeLocation(Line2Shader::Param::Location, "param");
    bindAttributeLocation(Line2Shader::VertexColor::Location, "color");
    bindAttributeLocation(Line2Shader::TeamWeight::Location, "teamWeight");
    bindAttributeLocation(Line2Shader::InstanceTransform::Location, "instanceTransform");
    bindAttributeLocation(Line2Shader::InstanceTeamColor::Location, "instanceTeamColor");

    link();

    u_width = uniformLocation("width");
    u_viewportSize = uniformLocation("viewportSize");
    u_viewProjection = uniformLocation("viewProjection");

    setWidth(1.f);
    setViewportSize(Vector2{1280.f, 720.f});
    setViewProjection(Matrix3{});
}

Line2Shader& Line2Shader::setWidth(Magnum::Float widthPixels)
{
    setUniform(u_width, widthPixels);
    return *this;
}

Line2Shader& Line2Shader::setViewportSize(const Vector2& size)
{
    setUniform(u_viewportSize, size);
    return *this;
}

Line2Shader& Line2Shader::setViewProjection(const Matrix3& matrix)
{
    setUniform(u_viewProjection, matrix);
    return *this;
}

} // namespace Gravitaris
