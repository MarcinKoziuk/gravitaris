#include <algorithm>
#include <cmath>
#include <vector>

#include <Magnum/Mesh.h>
#include <Magnum/GL/Renderer.h>
#include <Magnum/Math/Matrix3.h>

#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/component/ship-loadout.hpp>
#include <gravitaris/game/resource/body.hpp>
#include <gravitaris/game/resource/common/resource-loader.hpp>
#include <gravitaris/game/upgrade/upgrade-catalog.hpp>

#include <gravitaris/cgame/resource/model.hpp>
#include <gravitaris/cgame/component/renderable.hpp>
#include <gravitaris/cgame/renderer/gl-safe-upload.hpp>
#include <gravitaris/cgame/hud/health-bar-renderer.hpp>

#include "cgame/renderer/detail/line2-batch.hpp"

namespace Gravitaris {

using namespace Magnum;

using Line2Batch::LineVertex;
using Line2Batch::InstanceData;
using Line2Batch::EmitFillTriangle;

namespace {

// The sidebar's own hull ramp (data/ui/hud.rml: #0cf -> #fc0 -> #f33), so a
// bar over a hull and the panel reporting one say the same thing in the same
// colours.
const Vector3 HULL_OK{0.f, 0.8f, 1.f};
const Vector3 HULL_WARN{1.f, 0.8f, 0.f};
const Vector3 HULL_CRITICAL{1.f, 0.2f, 0.2f};

// #6cf and #fc6: a bubble and a plated hull are different fittings and read
// as different colours, exactly as they do in the sidebar.
const Vector3 SHIELD_BUBBLE{0.4f, 0.8f, 1.f};
const Vector3 SHIELD_PLATING{1.f, 0.8f, 0.4f};

// How far past the view edge a hull may sit and still have its bars built.
// One bar width of slack, so nothing pops as it crosses the boundary.
constexpr float CULL_MARGIN_PX = 40.f;

// A bar is drawn as missing charge rather than as nothing at all only once it
// is genuinely off full; float noise on a freshly spawned hull is not damage.
constexpr float FULL_EPSILON = 1e-3f;

// How far above the hull's origin its bounding box reaches once the box is put
// in the pose the hull is actually in. A circle would be rotation-free but has
// to clear the corners of a wide, flat structure, which leaves its bars a whole
// hull-height of air above the roof.
float TopExtent(const Vector2& center, const Vector2& halfExtent, const Vector2& scale, float rot)
{
    const Vector2 offset = center * scale;
    const Vector2 half = halfExtent * scale;
    const float sinR = std::sin(rot);
    const float cosR = std::cos(rot);
    return offset.x() * sinR + offset.y() * cosR
           + std::abs(half.x() * sinR) + std::abs(half.y() * cosR);
}

void EmitRect(std::vector<LineVertex>& out, const Vector2& min, const Vector2& max,
              const Vector3& color)
{
    if (max.x() <= min.x() || max.y() <= min.y()) return;
    const Vector2 tl{min.x(), max.y()};
    const Vector2 br{max.x(), min.y()};
    EmitFillTriangle(out, min, br, max, color);
    EmitFillTriangle(out, min, max, tl, color);
}

// One bar: the spent part in a dim version of the fill's own hue, the charge
// left over it. Drawn as two rects rather than a track plus a fill so the
// boundary is exact at any fraction -- there is no border to eat the last few
// percent of a nearly-empty bar.
void EmitBar(std::vector<LineVertex>& out, const Vector2& origin, float width, float height,
             float fraction, const Vector3& color, float fillBrightness, float trackBrightness)
{
    const float clamped = std::clamp(fraction, 0.f, 1.f);
    const Vector2 top{origin.x() + width, origin.y() + height};
    EmitRect(out, origin, top, color * trackBrightness);
    EmitRect(out, origin, Vector2{origin.x() + width * clamped, top.y()}, color * fillBrightness);
}

} // namespace

HealthBarRenderer::HealthBarRenderer(IFilesystem& filesystem, ResourceLoader& resourceLoader,
                                     const UpgradeCatalog& upgradeCatalog)
        : m_resourceLoader(resourceLoader)
        , m_upgradeCatalog(upgradeCatalog)
        , m_shader(filesystem)
{
    // Single identity instance, uploaded once: every bar's colour rides the
    // vertex stream, so the instanced attributes exist only to satisfy the
    // shader (same arrangement the minimap draws under).
    const InstanceData identity{Matrix3{Math::IdentityInit}, Vector3{1.f, 1.f, 1.f}, 0.f};
    if (unsigned long ex = SafeUpload(m_instanceBuffer, &identity, sizeof(identity))) {
        LOG(error) << "[HealthBars] instance buffer upload raised exception 0x" << std::hex << ex;
    }

    m_mesh.setPrimitive(MeshPrimitive::Triangles)
          .addVertexBuffer(m_vertexBuffer, 0,
                           Line2Shader::PointA{},
                           Line2Shader::PointB{},
                           Line2Shader::PointC{},
                           Line2Shader::Param{},
                           Line2Shader::VertexColor{},
                           Line2Shader::TeamWeight{})
          .addVertexBufferInstanced(m_instanceBuffer, 1, 0,
                                    Line2Shader::InstanceTransform{},
                                    Line2Shader::InstanceTeamColor{},
                                    Line2Shader::InstanceFlash{})
          .setInstanceCount(1);
}

HealthBarRenderer::ModelBox HealthBarRenderer::BoxOf(id_t modelId)
{
    if (const auto it = m_boxByModel.find(modelId); it != m_boxByModel.end()) return it->second;

    ModelBox box;
    if (ResourcePtr<const Body> body = m_resourceLoader.Load<Body>(modelId)) {
        const Body::Bounds& bounds = body->GetLocalBounds();
        box.center = Vector2{bounds.Center()};
        box.halfExtent = Vector2{bounds.HalfExtent()};
    }
    m_boxByModel.emplace(modelId, box);
    return box;
}

Vector3 HealthBarRenderer::HullColor(float fraction) const
{
    if (fraction <= m_params.criticalFraction) return HULL_CRITICAL;
    if (fraction <= m_params.warnFraction) return HULL_WARN;
    return HULL_OK;
}

void HealthBarRenderer::Render(const SceneView& view, std::optional<flecs::entity> exclude,
                               const Vector2& cameraPos, float zoom,
                               const Vector2& designViewportSize, const Vector2& viewportSizePx)
{
    if (!m_params.enabled || m_params.maxBars <= 0) return;
    if (zoom <= 1e-6f || zoom < m_params.minZoom) return;

    // Design pixels per world unit. Sizes below are authored in design pixels
    // and divided by this, which is what keeps a bar the same size on screen
    // however far the camera has pulled back (see IndicatorRenderer).
    const float ppu = zoom;
    const Vector2 halfExtent = designViewportSize * 0.5f / ppu;
    const Vector2 cullExtent = halfExtent + Vector2{CULL_MARGIN_PX / ppu};

    m_candidates.clear();
    view.Each([&](flecs::entity entity, const Transform& transform, const Damageable& damageable) {
        if (!m_params.includeSelf && exclude && entity == *exclude) return;
        if (damageable.maxHp <= 0.f) return;

        const Renderable* renderable = entity.try_get<Renderable>();
        if (!renderable) return; // nothing drawn here to hang a bar over

        const Vector2 pos{static_cast<float>(transform.pos.x()), static_cast<float>(transform.pos.y())};
        const Vector2 fromCamera = pos - cameraPos;
        if (std::abs(fromCamera.x()) > cullExtent.x()) return;

        // The bars ride clear of the top of the hull, so the bottom edge has
        // to be tested against that rather than against the centre -- a High
        // Port just under it still has its bars in view.
        const ModelBox box = BoxOf(renderable->model.Id());
        const float topOffset = TopExtent(box.center, box.halfExtent,
                                          Vector2{static_cast<float>(transform.scale.x()),
                                                  static_cast<float>(transform.scale.y())},
                                          static_cast<float>(static_cast<double>(transform.rot)));
        if (fromCamera.y() > cullExtent.y() || fromCamera.y() < -(cullExtent.y() + topOffset)) return;

        const float hull = std::clamp(damageable.hp / damageable.maxHp, 0.f, 1.f);

        // Capacity is never replicated -- it resolves from the ship's own
        // upgrade levels, which are. Same derivation the sidebar's readout
        // uses, so a remote hull's bar and a spectated one's agree.
        float shield = -1.f;
        bool plating = false;
        if (const ShipLoadout* loadout = entity.try_get<ShipLoadout>()) {
            const float capacity = m_upgradeCatalog.ResolveStats(loadout->levels).shieldCapacity;
            if (capacity > 0.f && loadout->levels.shieldType != ShieldType::None) {
                shield = std::clamp(loadout->shieldHp / capacity, 0.f, 1.f);
                plating = IsPlated(*loadout);
            }
        }

        // Both bars appear together or not at all: a shield bar hanging alone
        // over an undamaged hull reads as a warning about nothing.
        const bool hurt = hull < 1.f - FULL_EPSILON || (shield >= 0.f && shield < 1.f - FULL_EPSILON);
        if (!hurt) return;

        m_candidates.push_back(Candidate{pos, topOffset, hull, shield, plating, fromCamera.dot()});
    });

    if (m_candidates.empty()) return;

    // Nearest-first, then cap: with a crowded field the closest hulls are the
    // ones worth the screen space (same rule the enemy arrows follow).
    const auto count = std::min<std::size_t>(m_candidates.size(),
                                             static_cast<std::size_t>(m_params.maxBars));
    const auto byDistance = [](const Candidate& a, const Candidate& b) {
        return a.distanceSq < b.distanceSq;
    };
    std::partial_sort(m_candidates.begin(), m_candidates.begin() + static_cast<std::ptrdiff_t>(count),
                      m_candidates.end(), byDistance);
    m_candidates.resize(count);

    const float width = m_params.widthPx / ppu;
    const float hullHeight = m_params.hullHeightPx / ppu;
    const float shieldHeight = m_params.shieldHeightPx / ppu;
    const float gap = m_params.gapPx / ppu;
    const float clearance = m_params.clearancePx / ppu;

    std::vector<LineVertex> vertices;
    vertices.reserve(m_candidates.size() * 4 * 6);

    for (const Candidate& candidate : m_candidates) {
        const float hullBrightness = candidate.hullFraction <= m_params.criticalFraction
                ? m_params.criticalBrightness
                : m_params.brightness;

        const float left = candidate.pos.x() - width * 0.5f;
        const float bottom = candidate.pos.y() + candidate.topOffset + clearance;

        EmitBar(vertices, Vector2{left, bottom}, width, hullHeight, candidate.hullFraction,
                HullColor(candidate.hullFraction), hullBrightness, m_params.trackBrightness);

        if (candidate.shieldFraction >= 0.f) {
            EmitBar(vertices, Vector2{left, bottom + hullHeight + gap}, width, shieldHeight,
                    candidate.shieldFraction,
                    candidate.plating ? SHIELD_PLATING : SHIELD_BUBBLE,
                    m_params.brightness, m_params.trackBrightness);
        }
    }

    if (vertices.empty()) return;

    if (unsigned long ex = SafeUpload(m_vertexBuffer, vertices.data(), vertices.size() * sizeof(LineVertex))) {
        LOG(error) << "[HealthBars] vertex buffer upload raised exception 0x" << std::hex << ex;
        return;
    }
    m_mesh.setCount(static_cast<Int>(vertices.size()));

    GL::Renderer::enable(GL::Renderer::Feature::Blending);
    GL::Renderer::setBlendFunction(GL::Renderer::BlendFunction::SourceAlpha,
                                   GL::Renderer::BlendFunction::OneMinusSourceAlpha);

    // The fill primitive takes its vertices straight through, so the stroke
    // width this shader resolves in pixel space never enters into it -- only
    // the projection matters here.
    m_shader.setViewportSize(viewportSizePx)
            .setViewProjection(Matrix3::projection(designViewportSize / ppu)
                               * Matrix3::translation(-cameraPos));

    m_shader.draw(m_mesh);
}

} // namespace Gravitaris
