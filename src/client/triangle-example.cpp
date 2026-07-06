#include <Magnum/Mesh.h>

#include "triangle-example.hpp"

namespace Gravitaris {

using namespace Math::Literals;

struct TriangleVertex {
    Vector2 position;
    Color3 color;
};

static const TriangleVertex vertices[]{
        {{-0.5f, -0.5f}, 0xff0000_rgbf},    /* Left vertex, red color */
        {{ 0.5f, -0.5f}, 0x00ff00_rgbf},    /* Right vertex, green color */
        {{ 0.0f,  0.5f}, 0x0000ff_rgbf}     /* Top vertex, blue color */
};

TriangleExample::TriangleExample()
{
    m_mesh.setPrimitive(Magnum::MeshPrimitive::Triangles)
        .setCount(static_cast<Int>(Containers::arraySize(vertices)))
        .addVertexBuffer(GL::Buffer{vertices}, 0,
                           Shaders::VertexColor2D::Position{},
                           Shaders::VertexColor2D::Color3{});
}

void TriangleExample::Draw()
{
    m_shader.draw(m_mesh);
}

} // namespace Gravitaris
