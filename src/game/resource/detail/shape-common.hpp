#pragma once

#include <memory>
#include <vector>

#include <toml++/toml.h>
#include <nanosvg/nanosvg.h>

#include <Magnum/Magnum.h>
#include <gravitaris/game/fs/ifilesystem.hpp>

namespace Gravitaris {

using Magnum::Vector2d;
using Magnum::Matrix4d;
using Magnum::Color3;
using Magnum::Color4;

// '@' layers are sim-only: the renderer skips them entirely.
static const char GROUP_LABEL_PREFIX = '@';
static const char ORIGIN_GROUP_LABEL[] = "@origin";
static const char BODY_GROUP_LABEL[] = "@body";
static const char HARDPOINTS_GROUP_LABEL[] = "@hardpoints";
// Refit slots, read by the client only: where the board's slot markers sit on
// a schematic. Each element's inkscape:label names it (see Shape::GetMarkers).
static const char SLOTS_GROUP_LABEL[] = "@slots";

// '+' layers are consumed by BOTH sides -- the sim builds collision from them
// and the renderer bakes them -- but they are drawn only while game state asks
// for it, and their authored stroke color is ignored (decided in code, see
// Shape::AddPaths). A ship carries at most one of these at a time today, chosen
// by ShipLoadout::levels.shieldType.
static const char FX_LABEL_PREFIX = '+';
static const char SHIELD_GROUP_LABEL[] = "+shield";
static const char PLATING_GROUP_LABEL[] = "+plating";

struct NSVGimage_deleter {
    void operator()(NSVGimage* p) { nsvgDelete(p); }
};

typedef std::unique_ptr<NSVGimage, NSVGimage_deleter> NSVGImageUniquePtr;

struct ShapeFiles {
    toml::table cfg;
    NSVGImageUniquePtr svg;

    ShapeFiles(toml::table cfg, NSVGImageUniquePtr svg)
            : cfg(std::move(cfg)), svg(std::move(svg))
    {}
};

std::unique_ptr<ShapeFiles> GetShapeFiles(IFilesystem& fs, const std::string& path);

Vector2d GetShapeCenter(const NSVGshape* shape);

struct CircleInfo {
    Vector2d center;
    double radius;
};

// True if this single path is (within tolerance) exactly the 4-arc cubic
// bezier construction nanosvg emits for <circle>/<ellipse> with equal
// radii (see nsvg__parseCircle in nanosvg.h) — i.e. a genuine circle, not an
// ellipse or an arbitrary hand-drawn closed curve that merely looks round.
// Deliberately per-path (unlike body.cpp's stricter shape-level IsCircle,
// which also requires the shape to have exactly one path) so it can be used
// while iterating individual paths in a possibly multi-path shape.
bool IsCircleGeometry(const NSVGpath* path, CircleInfo* outInfo = nullptr);

// One SVG path flattened to a transformed polyline, subdividing each cubic
// segment on its own. A closed path does not repeat its first point at the end.
std::vector<Vector2d> PathToPolyline(const NSVGpath* path, const Matrix4d& transform);

Matrix4d GetTransformMatrix(const ShapeFiles& mf);

Color3 Hex3ToNormalizedColor3(std::uint32_t hex);

Color4 Hex3ToNormalizedColor4(std::uint32_t hex, float alpha);

} // namespace Gravitaris