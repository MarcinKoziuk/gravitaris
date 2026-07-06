#pragma once

#include <memory>

#include <yaml-cpp/yaml.h>
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
    YAML::Node cfg;
    NSVGImageUniquePtr svg;

    ShapeFiles(const YAML::Node& cfg, NSVGImageUniquePtr svg)
            : cfg(cfg), svg(std::move(svg))
    {}
};

std::unique_ptr<ShapeFiles> GetShapeFiles(IFilesystem& fs, const std::string& path);

Vector2d GetShapeCenter(const NSVGshape* shape);

Matrix4d GetTransformMatrix(const ShapeFiles& mf);

Color3 Hex3ToNormalizedColor3(std::uint32_t hex);

Color4 Hex3ToNormalizedColor4(std::uint32_t hex, float alpha);

} // namespace Gravitaris