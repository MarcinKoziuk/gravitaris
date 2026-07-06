#include <Magnum/Math/Color.h>
#include <Magnum/GL/Mesh.h>
#include <Magnum/Shaders/VertexColor.h>

namespace Gravitaris {

using namespace Magnum;

class TriangleExample {
public:
    explicit TriangleExample();

    void Draw();
private:
    GL::Mesh m_mesh;
    Shaders::VertexColor2D m_shader;
};

} // namespace Gravitaris
