#pragma once

#include <vector>

#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/Vector2.h>
#include <Magnum/Math/Vector3.h>
#include <Magnum/Math/Vector4.h>

namespace Gravitaris {

// CPU-side vertex batching for Line2Shader, shared by the panel renderers that
// rebuild their geometry every frame (minimap, compass) rather than baking it
// into a Model the way the world renderers do. The layouts here are the
// shader's contract -- keep them in step with Line2Shader's attributes.
namespace Line2Batch {

struct LineVertex {
    Magnum::Vector2 pointA;
    Magnum::Vector2 pointB;
    Magnum::Vector2 pointC;
    Magnum::Vector4 param; // xyz weights, w = type (0 segment, 2 ring, 4 disc fill)
    Magnum::Vector3 color;
    float teamWeight;
};

// One identity instance is all a panel draws: colors ride the vertex stream
// instead, so the instanced attributes exist only to satisfy the shader.
struct InstanceData {
    Magnum::Matrix3 transform;
    Magnum::Vector3 teamColor;
    float flash;
};

constexpr float PRIM_SEGMENT = 0.f;
constexpr float PRIM_RING = 2.f;
constexpr float PRIM_FILL = 3.f;
constexpr float PRIM_DISC = 4.f;

constexpr Magnum::Vector2 CIRCLE_QUAD[] = {
        {-1.f, -1.f}, {1.f, -1.f}, {1.f, 1.f},
        {-1.f, -1.f}, {1.f, 1.f},  {-1.f, 1.f},
};

constexpr Magnum::Vector2 SEGMENT_WEIGHTS[] = {
        {0.f, -0.5f}, {1.f, -0.5f}, {1.f, 0.5f},
        {0.f, -0.5f}, {1.f, 0.5f},  {0.f, 0.5f},
};

inline void EmitBillboard(std::vector<LineVertex>& out, const Magnum::Vector2& center, float radius,
                          const Magnum::Vector3& color, float prim)
{
    const Magnum::Vector2 radiusCarrier{radius, 0.f};
    for (const Magnum::Vector2& corner : CIRCLE_QUAD) {
        out.push_back(LineVertex{center, radiusCarrier, Magnum::Vector2{},
                                 Magnum::Vector4{corner.x(), corner.y(), 0.f, prim}, color, 0.f});
    }
}

inline void EmitSegment(std::vector<LineVertex>& out, const Magnum::Vector2& a, const Magnum::Vector2& b,
                        const Magnum::Vector3& color)
{
    for (const Magnum::Vector2& w : SEGMENT_WEIGHTS) {
        out.push_back(LineVertex{a, b, Magnum::Vector2{},
                                 Magnum::Vector4{w.x(), w.y(), 0.f, PRIM_SEGMENT}, color, 0.f});
    }
}

// Solid triangle: the fill primitive takes vertex positions straight through
// with no expansion, so a filled shape is just its own corners.
inline void EmitFillTriangle(std::vector<LineVertex>& out, const Magnum::Vector2& a, const Magnum::Vector2& b,
                             const Magnum::Vector2& c, const Magnum::Vector3& color)
{
    for (const Magnum::Vector2& corner : {a, b, c}) {
        out.push_back(LineVertex{corner, Magnum::Vector2{}, Magnum::Vector2{},
                                 Magnum::Vector4{0.f, 0.f, 0.f, PRIM_FILL}, color, 0.f});
    }
}

} // namespace Line2Batch

} // namespace Gravitaris
