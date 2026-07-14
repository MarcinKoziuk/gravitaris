#pragma once

#include <Magnum/Magnum.h>
#include <Magnum/GL/Mesh.h>
#include <Magnum/GL/AbstractShaderProgram.h>
#include <Magnum/Math/Matrix3.h>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

using Magnum::Matrix3;
using Magnum::Vector2;
using Magnum::Vector2i;
using Magnum::Vector3;
using Magnum::Vector4;

// Shader for the baked (cached) line renderer. Line expansion happens per
// vertex from adjacency baked at load time; per-entity transforms are supplied
// as instanced attributes so N ships of the same model are one instanced draw.
class Line2Shader : public Magnum::GL::AbstractShaderProgram {
protected:
    Magnum::Int u_width;
    Magnum::Int u_viewportSize;
    Magnum::Int u_viewProjection;

public:
    // Per-vertex (static, baked once per model). teamWeight is 1 for strokes
    // authored in the team-color placeholder, 0 otherwise.
    typedef Magnum::GL::Attribute<0, Vector2> PointA;
    typedef Magnum::GL::Attribute<1, Vector2> PointB;
    typedef Magnum::GL::Attribute<2, Vector2> PointC;
    typedef Magnum::GL::Attribute<3, Vector4> Param;
    typedef Magnum::GL::Attribute<4, Vector3> VertexColor;
    typedef Magnum::GL::Attribute<5, Magnum::Float> TeamWeight;

    // Per-instance (dynamic, one entry per drawn entity). A Matrix3 attribute
    // occupies locations 6, 7 and 8; team color and hit-flash follow.
    typedef Magnum::GL::Attribute<6, Matrix3> InstanceTransform;
    typedef Magnum::GL::Attribute<9, Vector3> InstanceTeamColor;
    typedef Magnum::GL::Attribute<10, Magnum::Float> InstanceFlash;

    explicit Line2Shader(IFilesystem& fileSystem);

    ~Line2Shader() override = default;

    Line2Shader& setWidth(Magnum::Float widthPixels);
    Line2Shader& setViewportSize(const Vector2& size);
    Line2Shader& setViewProjection(const Matrix3& matrix);
};

} // namespace Gravitaris
