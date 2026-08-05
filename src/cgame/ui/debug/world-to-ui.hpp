#pragma once

#include <imgui.h>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/cgame/cgame.hpp>

namespace Gravitaris {

// World -> ImGui overlay coordinates: camera-centered on the scene viewport,
// ppu = framebuffer pixels per world unit, world +Y up vs. ImGui +Y down, then
// framebuffer -> UI scale. The scene viewport is inset horizontally by the HUD
// sidebar, so the overlay's centre is the viewport's centre rather than the
// window's; vertically the two still coincide.
struct WorldToUi {
    Magnum::Vector2 camPos;
    float ppu;
    Magnum::Vector2 fbToUi;
    Magnum::Vector2 uiCenter;

    WorldToUi(CGame& game, const Magnum::Vector2& uiSize)
            : camPos(game.GetCamera().GetPosition())
            , ppu(game.GetCamera().GetZoom() * game.GetContentScale())
            , fbToUi(Magnum::Vector2{1.f / game.GetContentScale()})
            , uiCenter((game.GetViewportOrigin().x() + game.GetViewportSize().x() * 0.5f) * fbToUi.x(),
                       uiSize.y() * 0.5f)
    {}

    ImVec2 operator()(const Magnum::Math::Vector2<double>& w) const
    {
        const float dx = static_cast<float>(w.x()) - camPos.x();
        const float dy = static_cast<float>(w.y()) - camPos.y();
        return ImVec2(uiCenter.x() + dx * ppu * fbToUi.x(),
                      uiCenter.y() - dy * ppu * fbToUi.y());
    }

    // World units -> UI pixels (for radii/lengths).
    [[nodiscard]] float Scale() const { return ppu * fbToUi.x(); }
};

} // namespace Gravitaris
