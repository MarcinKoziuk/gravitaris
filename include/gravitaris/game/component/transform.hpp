#pragma once

#include <Magnum/Magnum.h>
#include <Magnum/Math/Vector2.h>
#include <Magnum/Math/Angle.h>

namespace Gravitaris {

using Magnum::Vector2d;
using Magnum::Radd;

// TODO refactor to Matrix?
struct Transform {
    Vector2d pos;
    Vector2d prevPos;
    Radd rot;
    Vector2d scale;
    Vector2d vel;

    Transform()
            : scale(1., 1.)
            , rot(0.)
    {}

    Transform(Vector2d pos, Radd rot, Vector2d scale, Vector2d vel)
            : pos(pos)
            , scale(scale)
            , rot(rot)
            , vel(vel)
    {}

    Transform(Vector2d pos, Radd rot, Vector2d scale)
            : Transform(pos, rot, scale, Vector2d{})
    {}

    Transform(Vector2d pos, Radd rot)
            : Transform(pos, rot, Vector2d{1., 1.})
    {}

    explicit Transform(Vector2d pos)
            : Transform(pos, Radd{0.})
    {}
};

} // namespace Gravitaris