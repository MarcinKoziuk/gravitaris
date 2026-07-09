#include <cmath>

#include <Magnum/Math/Vector2.h>
#include <Magnum/Math/Vector4.h>
#include <Magnum/Math/Matrix4.h>

#include "casteljau.hpp"

namespace Gravitaris {

using Magnum::Vector4d;

static bool IsFlat(const std::vector<Vector2d>& curve)
{
    const double tol = 1000.0;

    double ax = 3.*curve[1][0] - 2.*curve[0][0] - curve[3][0]; ax *= ax;
    double ay = 3.*curve[1][1] - 2.*curve[0][1] - curve[3][1]; ay *= ay;
    double bx = 3.*curve[2][0] - curve[0][0] - 2.*curve[3][0]; bx *= bx;
    double by = 3.*curve[2][1] - curve[0][1] - 2.*curve[3][1]; by *= by;

    return std::max(ax, bx) + std::max(ay, by) <= tol;
}

static Vector2d Midpoint(const Vector2d& p, const Vector2d& q)
{
    return {(p.x() + q.x()) / 2, (p.y() + q.y()) / 2};
}

static std::vector<Vector2d> Midpoints(const std::vector<Vector2d>& curve)
{
    std::vector<Vector2d> res;
    for (std::size_t i = 0; i < curve.size() - 1; i++) {
        res.push_back(Midpoint(curve[i], curve[i + 1]));
    }

    return res;
}

static std::pair<std::vector<Vector2d>, std::vector<Vector2d>> Subdivide(const std::vector<Vector2d>& curve)
{
    std::vector<Vector2d> midpoints1 = Midpoints(curve);
    std::vector<Vector2d> midpoints2 = Midpoints(midpoints1);
    std::vector<Vector2d> midpoints3 = Midpoints(midpoints2);

    std::vector<Vector2d> piece1 { curve[0], midpoints1[0], midpoints2[0], midpoints3[0] };
    std::vector<Vector2d> piece2 { midpoints3[0], midpoints2[1], midpoints1[2], curve[3] };

    return std::make_pair(piece1, piece2);
}

std::vector<Vector2d> CasteljauRaw(const std::vector<Vector2d>& curve)
{
    std::vector<Vector2d> res;
    if (IsFlat(curve)) {
        return curve;
    } else {
        auto pieces = Subdivide(curve);
        auto v1 = CasteljauRaw(pieces.first);
        auto v2 = CasteljauRaw(pieces.second);
        res.insert(res.end(), v1.begin(), v1.end());
        res.insert(res.end(), v2.begin(), v2.end());
    }

    return res;
}

std::vector<Vector2d> Casteljau(const std::vector<Vector2d>& curve, const Matrix4d& transform, bool closed)
{
    std::vector<Vector2d> pre = CasteljauRaw(curve);
    std::vector<Vector2d> post;

    for (const Vector2d& pt : pre) {
        // Copy, NOT a reference: Vector4::xy() returns a Vector2& aliasing the
        // multiplied temporary's storage. Binding a reference here does not
        // extend that temporary's lifetime (lifetime extension doesn't reach
        // through the xy() call), so `tpt` would dangle — benign at -O0 but
        // reads garbage under -O2, collapsing every path to <3 points.
        const Vector2d tpt = (transform * Vector4d(pt.x(), pt.y(), 1., 1.)).xy();

        if (post.empty() || post.back() != tpt) {
            post.push_back(tpt);
        }
    }

    if (closed && !post.empty()) {
        if (post.front() == post.back()) {
            post.pop_back();
        }
    }

    return post;
}

} // namespace Gravitaris
