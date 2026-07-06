#pragma once

#include <vector>

#include <Magnum/Magnum.h>

namespace Gravitaris {

using Magnum::Vector2d;
using Magnum::Matrix4d;

std::vector<Vector2d> CasteljauRaw(const std::vector<Vector2d>& curve);

std::vector<Vector2d> Casteljau(const std::vector<Vector2d>& curve, const Matrix4d& transform, bool closed);

} // namespace Gravitaris
