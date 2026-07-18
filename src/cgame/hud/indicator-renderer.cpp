#include <algorithm>
#include <cmath>
#include <vector>

#include <Magnum/Math/Color.h>
#include <Magnum/Math/Matrix3.h>
#include <Magnum/Math/Functions.h>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/component/transform.hpp>
#include <gravitaris/game/component/damageable.hpp>
#include <gravitaris/game/resource/common/resource-loader.hpp>

#include <gravitaris/cgame/resource/model.hpp>
#include <gravitaris/cgame/renderer/model-renderer2.hpp>
#include <gravitaris/cgame/team-color.hpp>
#include <gravitaris/cgame/hud/indicator-renderer.hpp>

namespace Gravitaris {

using Magnum::Matrix3;

IndicatorRenderer::IndicatorRenderer(flecs::world& registry, ResourceLoader& resourceLoader,
                                     ModelRenderer2& modelRenderer2)
        : m_registry(registry)
        , m_modelRenderer2(modelRenderer2)
{
    // Loading it is what bakes it into m_modelRenderer2 (via OnCreate<Model>);
    // the ResourcePtr member then keeps it baked.
    m_arrowModel = resourceLoader.Load<Model>("models/ui/arrow-1"_id);
}

void IndicatorRenderer::Update(std::optional<flecs::entity> player, const Magnum::Vector2& cameraPos, float zoom,
                               const Magnum::Vector2& viewportSize, float pixelScale)
{
    if (!m_params.enabled || !m_arrowModel) return;
    if (!player) return;
    const Transform* playerTransf = player->try_get<Transform>();
    if (!playerTransf) return;

    const Team* playerTeamComp = player->try_get<Team>();
    const TeamId playerTeam = playerTeamComp ? playerTeamComp->id : TeamId::Blue;

    const Magnum::Vector2 playerPos{static_cast<float>(playerTransf->pos.x()),
                                    static_cast<float>(playerTransf->pos.y())};
    if (zoom <= 0.f) return;

    // The renderers map world->screen at `zoom` px per world unit, camera-
    // centered (see ModelRenderer2::ViewProjection), so screen offsets are just
    // world offsets from the camera scaled by zoom.
    const Magnum::Vector2 halfExtentPx = viewportSize * 0.5f;
    const float ringWorld = m_params.ringRadiusPx * pixelScale / zoom;
    const float arrowWorld = m_params.arrowSizePx * pixelScale / zoom;

    struct Candidate {
        Magnum::Vector2 pos;
        Magnum::Vector3 color;
        float distance;
    };
    std::vector<Candidate> enemies;

    // Enemy = damageable ship on a real opposing team -- same notion
    // CameraDirector's SelectFramedEnemy uses.
    m_registry.each([&](flecs::entity, const Transform& t, const Team& team, const Damageable&) {
        if (team.id == playerTeam || team.id == TeamId::None) return;
        const Magnum::Vector2 pos{static_cast<float>(t.pos.x()), static_cast<float>(t.pos.y())};
        const float dist = (pos - playerPos).length();
        if (dist > m_params.enemyRange) return;
        enemies.push_back({pos, Magnum::Vector3{TeamColor(team.id)}, dist});
    });

    // Nearest-first, then cap: with a crowded field the closest threats are the
    // ones worth the screen space.
    const auto byDistance = [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; };
    std::sort(enemies.begin(), enemies.end(), byDistance);
    enemies.resize(std::min<std::size_t>(enemies.size(), m_params.maxEnemies));

    const auto submit = [&](const Candidate& c, float range) {
        const Magnum::Vector2 fromCamera = c.pos - cameraPos;

        // How far outside the view the target is, in px past the (inset) edge.
        // Ramping the arrow in over fadeBandPx instead of switching it on at the
        // boundary keeps a target that's drifting across the edge from popping.
        const Magnum::Vector2 screenPx = fromCamera * zoom;
        const Magnum::Vector2 inset = halfExtentPx - Magnum::Vector2{m_params.edgeMarginPx * pixelScale};
        const float past = std::max(std::abs(screenPx.x()) - std::max(inset.x(), 1.f),
                                    std::abs(screenPx.y()) - std::max(inset.y(), 1.f));
        if (past <= 0.f) return; // comfortably on screen: the target speaks for itself
        const float edgeFade = std::clamp(past / std::max(m_params.fadeBandPx * pixelScale, 1.f), 0.f, 1.f);

        // Near targets read loud, distant ones stay legible but recede; also
        // fades an arrow out as its target leaves range, so nothing blinks off.
        const float nearness = std::clamp(1.f - c.distance / std::max(range, 1.f), 0.f, 1.f);
        const float strength = edgeFade * (m_params.minStrength
                                           + (1.f - m_params.minStrength) * nearness);
        if (strength <= 0.01f) return;

        // Ring center and pointing direction are player-relative, not camera-
        // relative: enemy framing can offset the camera from the player, and
        // the arrows should read as "which way from my ship", staying anchored
        // on the player's screen position rather than the viewport's.
        const Magnum::Vector2 fromPlayer = c.pos - playerPos;
        const float len = fromPlayer.length();
        if (len < 1e-3f) return;
        const Magnum::Vector2 dir = fromPlayer / len;

        // rot 0 points the glyph along -Y (ship convention, see arrow-1.svg), so
        // adding a quarter turn to the direction's angle aims it outward.
        const float rot = std::atan2(dir.y(), dir.x()) + Magnum::Constants::piHalf();

        // Width only fades in/out at the screen edge (edgeFade), never shrinks
        // with distance; height additionally stretches as the target closes
        // in, so proximity reads as "taller", not "bigger" -- the local X/Y
        // scaling is pre-rotation, so this is the arrow's own width/height
        // regardless of which way it's pointing.
        const float widthScale = arrowWorld * edgeFade;
        const float heightNearness = std::clamp(nearness * m_params.heightRampFactor, 0.f, 1.f);
        const float heightScale = widthScale * (1.f + (m_params.maxHeightFactor - 1.f) * heightNearness);

        const Matrix3 transform = Matrix3::translation(playerPos + dir * ringWorld)
                                * Matrix3::rotation(Magnum::Rad(rot))
                                * Matrix3::scaling({widthScale, heightScale});

        // No alpha in the line shader: on the black backdrop, scaling the color
        // toward black is the fade.
        m_modelRenderer2.SubmitOverlay(m_arrowModel.Id(), transform, c.color * strength);
    };

    for (const Candidate& c : enemies) submit(c, m_params.enemyRange);
}

} // namespace Gravitaris
