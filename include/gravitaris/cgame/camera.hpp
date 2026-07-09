#pragma once

#include <algorithm>
#include <cmath>

#include <Magnum/Math/Vector2.h>

namespace Gravitaris {

using Magnum::Vector2;

class Camera {
private:
    Vector2 m_position{0.f, 0.f};
    float m_zoom = 1.f;

    static constexpr float MIN_ZOOM = 0.1f;
    static constexpr float MAX_ZOOM = 8.f;

public:
    [[nodiscard]] const Vector2& GetPosition() const { return m_position; }

    void SetPosition(const Vector2& position) { m_position = position; }

    [[nodiscard]] float GetZoom() const { return m_zoom; }

    void SetZoom(float zoom) { m_zoom = std::clamp(zoom, MIN_ZOOM, MAX_ZOOM); }

    void AddZoomNotches(float notches) { SetZoom(m_zoom * std::pow(1.15f, notches)); }
};

} // namespace Gravitaris
