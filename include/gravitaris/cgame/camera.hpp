#pragma once

#include <algorithm>
#include <cmath>

#include <Magnum/Math/Vector2.h>

namespace Gravitaris {

using Magnum::Vector2;

// Holds the view state (world-space position + zoom) that every line
// renderer reads from when building its projection matrix. Kept as its own
// object rather than baked into a renderer so that input handling (scroll,
// drag-to-pan) and gameplay logic (follow the player) only ever need to
// touch this class; renderers just read GetPosition()/GetZoom() each frame.
class Camera {
private:
    Vector2 m_position{0.f, 0.f};
    float m_zoom = 1.f;

    static constexpr float MIN_ZOOM = 0.1f;
    static constexpr float MAX_ZOOM = 8.f;

public:
    [[nodiscard]] const Vector2& GetPosition() const { return m_position; }

    // Later: panning (drag) and following an entity both just call this.
    void SetPosition(const Vector2& position) { m_position = position; }

    [[nodiscard]] float GetZoom() const { return m_zoom; }

    void SetZoom(float zoom) { m_zoom = std::clamp(zoom, MIN_ZOOM, MAX_ZOOM); }

    // notches: mouse wheel/scroll offset (e.g. ScrollEvent::offset().y()).
    // Multiplicative so the zoom feels equally fast at any zoom level.
    void AddZoomNotches(float notches) { SetZoom(m_zoom * std::pow(1.15f, notches)); }
};

} // namespace Gravitaris
