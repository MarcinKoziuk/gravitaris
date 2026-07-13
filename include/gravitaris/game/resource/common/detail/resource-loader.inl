#pragma once

#include <gravitaris/game/logging.hpp>

namespace Gravitaris {

template<IsIResource T>
ResourcePtr<const T> ResourceLoader::Get(const id_t id)
{
    const auto tId = TypedResourceId::Of<T>(id);
    auto ptrIter = m_resourcePtrs.find(tId);
    if (ptrIter != m_resourcePtrs.end() && !ptrIter->second.Expired()) {
        WeakResourcePtr<const IResource>& abstractPtr = ptrIter->second;
        ResourcePtr<const T> ptr = abstractPtr.Lock().As<T>();
        return ptr;
    } else {
        return T::Placeholder();
    }
}

template<IsIResource T>
ResourcePtr<const T> ResourceLoader::Load(const id_t id)
{
    const auto tId = TypedResourceId::Of<T>(id);
    if (m_resourcePtrs.count(tId) && !m_resourcePtrs.find(tId)->second.Expired()) {
        return Get<T>(id);
    } else {
        auto context = IResource::LoadingContext{*this, m_filesystem};
        ResourcePtr<const T> ptr = T::Create(id, context);
        if (ptr) {
            ResourcePtr<const IResource> abstractPtr = ptr.template As<const IResource>();
            auto weakPtr = WeakResourcePtr<const IResource>(abstractPtr);
            // insert_or_assign, NOT try_emplace: on reload after the previous
            // instance died, the expired cache entry is still present and
            // try_emplace keeps it -- every later Load then creates another
            // duplicate live instance of the same id, and the first duplicate
            // to die erases the renderers' shared per-id caches out from
            // under the survivors.
            m_resourcePtrs.insert_or_assign(tId, weakPtr);

            SignalsFor<T>(std::type_index(typeid(const T))).create(*ptr, id);

            ptr.OnDestroy().connect(&ResourceLoader::HandleResourceDestroyed<T>, this);

            return ResourcePtr<T>(std::move(ptr));
        } else {
            return ResourcePtr<T>(std::move(T::Placeholder())); // don't add it to resourcePtrs!
        }
    }
}

template<IsIResource T>
void ResourceLoader::HandleResourceDestroyed(id_t id, const T& instance)
{
    SignalsFor<const T>(std::type_index(typeid(const T))).destroy(instance, id);
}

template<IsIResource T>
ResourceLoader::Signals<const T>& ResourceLoader::SignalsFor(std::type_index typeIndex)
{
    m_signals.try_emplace(typeIndex, Signals<const IResource>{});
    ResourceLoader::Signals<const IResource>& signals = m_signals[typeIndex];
    auto* signalsPtr = reinterpret_cast<ResourceLoader::Signals<const T>*>(&signals);
    return *signalsPtr;
}

template <IsIResource T>
sigslot::signal<const T&, id_t>& ResourceLoader::OnCreate()
{
    return SignalsFor<const T>(std::type_index(typeid(const T))).create;
}

template <IsIResource T>
sigslot::signal<const T&, id_t>& ResourceLoader::OnDestroy()
{
    return SignalsFor<const T>(std::type_index(typeid(const T))).destroy;
}


} // namespace Gravitaris
