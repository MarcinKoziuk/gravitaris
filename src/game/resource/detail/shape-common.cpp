#include <cstring>

#include <Magnum/Magnum.h>
#include <Magnum/Math/Vector2.h>
#include <Magnum/Math/Matrix4.h>
#include <Magnum/Math/Color.h>

#include <gravitaris/game/logging.hpp>
#include <memory>

#include "shape-common.hpp"

namespace Gravitaris {

using Magnum::Vector3d;

static std::string SplitFilename(const std::string& path)
{
    std::size_t idx = path.find_last_of('/');
    if (idx != std::string::npos) {
        if (idx < path.size() - 1)
            return path.substr(idx + 1, path.size());
        else
            return "";
    }
    else {
        return path;
    }
}

static std::string GetModelConfigPath(const std::string& path)
{
    std::string dirname = SplitFilename(path);
    return path + "/" + dirname + ".yml";
}

static std::string GetModelDefaultSvgPath(const std::string& path)
{
    std::string dirname = SplitFilename(path);
    return path + "/" + dirname + ".svg";
}


std::unique_ptr<ShapeFiles> GetShapeFiles(IFilesystem& fs, const std::string& path)
{
    std::string cfgPath = GetModelConfigPath(path);

    std::unique_ptr<std::istream> istream = fs.OpenAsStream(cfgPath);
    if (istream != nullptr) {
        try {
            YAML::Node cfg = YAML::Load(*istream);
            std::string svgPath;

            if (cfg["model"]) {
                svgPath = path + "/" + cfg["model"].as<std::string>();
            }
            else {
                svgPath = GetModelDefaultSvgPath(path);
            }

            std::string svgInput;
            bool ok = fs.ReadString(svgPath, &svgInput);
            if (ok) {
                auto svgPtr = NSVGImageUniquePtr(nsvgParse(reinterpret_cast<char *>(&svgInput.front()), "px", 96));
                if (svgPtr != nullptr) {
                    return std::make_unique<ShapeFiles>(std::move(cfg), std::move(svgPtr));
                }
                else {
                    LOG(error) << "Parsing svg file " << svgPath << " failed";
                }
            }
            else {
                LOG(error) << "Could not read model svg " << svgPath;
            }
        }
        catch (...) {
            LOG(error) << "Parsing yml file " << cfgPath << " failed";
        }
    }
    else {
        LOG(error) << "Could not read model config " << cfgPath;
    }

    return nullptr;
}

Vector2d GetShapeCenter(const NSVGshape* shape)
{
    Vector2d center;
    NSVGpath* path = shape->paths;
    center.x() = (path->bounds[0] + path->bounds[2]) / 2.f;
    center.y() = (path->bounds[1] + path->bounds[3]) / 2.f;
    return center;
}

Matrix4d GetTransformMatrix(const ShapeFiles& mf)
{
    const YAML::Node& cfg = mf.cfg;
    const NSVGimage& svg = *mf.svg;

    double scale = 1.;
    Magnum::Radd rotation;
    Vector2d origin;

    if (cfg["scale"]) {
        scale = cfg["scale"].as<float>();
    }

    if (cfg["rotation"]) {
        const auto rotationDeg = cfg["rotation"].as<double>();
        rotation = Magnum::Degd(rotationDeg);
    }

    for (const NSVGshape* shape = svg.shapes; shape != nullptr; shape = shape->next) {
        const char* group = shape->groupLabel;
        if (std::strcmp(group, ORIGIN_GROUP_LABEL) == 0) {
            origin = GetShapeCenter(shape);
        }
    }

    Matrix4d transform = Matrix4d::translation(Vector3d(-origin.x() * scale, -origin.y() * scale, 0))
                       * Matrix4d::rotation(rotation, Vector3d(0.f, 0.f, 1.f))
                       * Matrix4d::scaling(Vector3d(scale, scale, 1.f));
    return transform;
}

Color3 Hex3ToNormalizedColor3(std::uint32_t hex)
{
    Color3 c;
    c.x() = static_cast<float>(hex & 0xFF) / 255.f;
    c.y() = static_cast<float>((hex >> 8) & 0xFF) / 255.f;
    c.z() = static_cast<float>((hex >> 16) & 0xFF) / 255.f;

    return c;
}

Color4 Hex3ToNormalizedColor4(std::uint32_t hex, float alpha)
{
    Color4 c;
    c.x() = static_cast<float>(hex & 0xFF) / 255.f;
    c.y() = static_cast<float>((hex >> 8) & 0xFF) / 255.f;
    c.z() = static_cast<float>((hex >> 16) & 0xFF) / 255.f;
    c.w() = alpha;

    return c;
}

} // namespace Gravitaris
