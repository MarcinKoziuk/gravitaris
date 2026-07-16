#pragma once

#include <algorithm>

#include <Magnum/Math/Vector2.h>

namespace Gravitaris {

using Magnum::Vector2;

class Camera {
private:
    Vector2 m_position{0.f, 0.f};
    float m_zoom = 1.f;

public:
    static constexpr float MIN_ZOOM = 0.1f;
    static constexpr float MAX_ZOOM = 8.f;

    [[nodiscard]] const Vector2& GetPosition() const { return m_position; }

    void SetPosition(const Vector2& position) { m_position = position; }

    [[nodiscard]] float GetZoom() const { return m_zoom; }

    void SetZoom(float zoom) { m_zoom = std::clamp(zoom, MIN_ZOOM, MAX_ZOOM); }

    // Move the camera the minimum amount to keep `target` inside a centered
    // dead-zone rectangle of half-size `deadZoneHalf` (world units). The
    // target roams freely inside the zone; the camera only follows once it
    // reaches an edge.
    void FollowWithDeadZone(const Vector2& target, const Vector2& deadZoneHalf)
    {
        const Vector2 delta = target - m_position;
        Vector2 move{0.f, 0.f};
        for (std::size_t axis = 0; axis < 2; ++axis) {
            if (delta[axis] > deadZoneHalf[axis]) move[axis] = delta[axis] - deadZoneHalf[axis];
            else if (delta[axis] < -deadZoneHalf[axis]) move[axis] = delta[axis] + deadZoneHalf[axis];
        }
        m_position += move;
    }
};

} // namespace Gravitaris
