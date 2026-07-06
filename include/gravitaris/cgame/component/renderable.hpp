#pragma once

#include <gravitaris/game/resource/common/resource-ptr.hpp>

#include <gravitaris/cgame/fwd.hpp>

namespace Gravitaris {

struct Renderable {
    ResourcePtr<const Model> model;
};

} // namespace Gravitaris
