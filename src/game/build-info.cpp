#include <string>

#include <gravitaris/build-config.hpp>
#include <gravitaris/gravitaris.hpp>
#include <gravitaris/game/build-info.hpp>

namespace Gravitaris {

const std::string& BuildInfoString()
{
    static const std::string info =
            std::string{GRAVITARIS_NAME " " GRAVITARIS_BUILD_PLATFORM " "
                        GRAVITARIS_BUILD_TYPE " " GRAVITARIS_BUILD_GIT_HASH}
            + (GRAVITARIS_BUILD_GIT_DIRTY ? "*" : "")
            + " " GRAVITARIS_BUILD_TIMESTAMP;
    return info;
}

} // namespace Gravitaris
