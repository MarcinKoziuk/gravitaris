#pragma once

#include <unordered_map>

#include <entt/signal/sigh.hpp>

#include <gravitaris/game/id.hpp>
#include <gravitaris/game/fs/ifilesystem.hpp>
#include <gravitaris/game/resource/common/iresource.hpp>
#include <gravitaris/game/resource/common/resource-ptr.hpp>

#include "detail/typed-resource-id.inl"

namespace Gravitaris {

class ResourceLoader {
private:
    template<IsIResource T>
    struct Signals {
        entt::sigh<void(const T&, id_t)> create;
        entt::sigh<void(const T&, id_t)> destroy;
    };

    IFilesystem& m_filesystem;
    std::unordered_map<TypedResourceId, WeakResourcePtr<const IResource>, TypedResourceId::Hash> m_resourcePtrs;
    std::unordered_map<std::type_index, Signals<const IResource>> m_signals;

    template<IsIResource T>
    Signals<const T>& SignalsFor(std::type_index typeIndex);

    template<IsIResource T>
    void HandleResourceDestroyed(id_t id, const T& instance);

public:
    explicit ResourceLoader(IFilesystem& filesystem) : m_filesystem(filesystem) {}

    template<IsIResource T>
    [[nodiscard]] ResourcePtr<const T> Get(id_t id);

    template<IsIResource T>
    [[nodiscard]] ResourcePtr<const T> Load(id_t id);

    template <IsIResource T>
    entt::sink<entt::sigh<void(const T&, id_t)>> OnCreate();

    template <IsIResource T>
    entt::sink<entt::sigh<void(const T&, id_t)>> OnDestroy();
};

} // namespace Gravitaris

#include "detail/resource-loader.inl"
