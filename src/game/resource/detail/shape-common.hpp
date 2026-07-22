#pragma once

#include <memory>

#include <toml++/toml.h>
#include <nanosvg/nanosvg.h>

#include <Magnum/Magnum.h>
#include <gravitaris/game/fs/ifilesystem.hpp>

namespace Gravitaris {

using Magnum::Vector2d;
using Magnum::Matrix4d;
using Magnum::Color3;
using Magnum::Color4;

static const char GROUP_LABEL_PREFIX = '@';
static const char ORIGIN_GROUP_LABEL[] = "@origin";
static const char BODY_GROUP_LABEL[] = "@body";
static const char HARDPOINTS_GROUP_LABEL[] = "@hardpoints";

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

Matrix4d GetTransformMatrix(const ShapeFiles& mf);

Color3 Hex3ToNormalizedColor3(std::uint32_t hex);

Color4 Hex3ToNormalizedColor4(std::uint32_t hex, float alpha);

} // namespace Gravitaris