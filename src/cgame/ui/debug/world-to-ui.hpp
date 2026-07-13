#pragma once

#include <imgui.h>

#include <Magnum/Math/Vector2.h>

#include <gravitaris/cgame/cgame.hpp>

namespace Gravitaris {

// World -> ImGui overlay coordinates: camera-centered, ppu = zoom in
// framebuffer pixels, world +Y up vs. ImGui +Y down, then framebuffer ->
// logical UI scale.
struct WorldToUi {
    Magnum::Vector2 camPos;
    float ppu;
    Magnum::Vector2 fbToUi;
    Magnum::Vector2 uiSize;

    WorldToUi(CGame& game, const Magnum::Vector2& uiSize)
            : camPos(game.GetCamera().GetPosition())
            , ppu(game.GetCamera().GetZoom())
            , fbToUi(uiSize / game.GetViewportSize())
            , uiSize(uiSize)
    {}

    ImVec2 operator()(const Magnum::Math::Vector2<double>& w) const
    {
        const float dx = static_cast<float>(w.x()) - camPos.x();
        const float dy = static_cast<float>(w.y()) - camPos.y();
        return ImVec2(uiSize.x() * 0.5f + dx * ppu * fbToUi.x(),
                      uiSize.y() * 0.5f - dy * ppu * fbToUi.y());
    }

    // World units -> UI pixels (for radii/lengths).
    [[nodiscard]] float Scale() const { return ppu * fbToUi.x(); }
};

} // namespace Gravitaris
