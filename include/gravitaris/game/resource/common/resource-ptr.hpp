#pragma once

#include <cassert>
#include <memory>
#include <string>
#include <type_traits>

#include <sigslot/signal.hpp>

#include <gravitaris/game/id.hpp>

#include "detail/resource-wrapper.hpp"

namespace Gravitaris {

template <typename T>
class WeakResourcePtr;

template <typename T>
class ResourcePtr {
private:
    typedef ResourceWrapper<typename std::remove_const<T>::type> WrappedType;

    std::shared_ptr<WrappedType> m_ref;

    template <class U>
    friend class ResourcePtr;

    friend class WeakResourcePtr<T>;
public:
    ResourcePtr()
            : m_ref(nullptr)
    {}

    ResourcePtr(std::nullptr_t)  // NOLINT(*-explicit-constructor)
            : m_ref(nullptr)
    {}

    explicit ResourcePtr(std::shared_ptr<WrappedType>&& ref)
            : m_ref(std::move(ref))
    {}

    template <typename U>
    ResourcePtr(const ResourcePtr<U>& other) // NOLINT(*-explicit-constructor)
            : m_ref(other.m_ref)
    {
        static_assert(std::is_same<const U, T>::value || std::is_same<const U, const T>::value,
                      "Can only convert ResourcePtr<T> to ResourcePtr<const T>");
    }

    ResourcePtr(const ResourcePtr<T>& other)
            : m_ref(other.m_ref)
    {}

    explicit operator bool() const
    {
        return bool(m_ref);
    }

    T& operator*()
    {
        assert(m_ref != nullptr && "Tried to dereference uninitialized ResourcePtr!");
        return **m_ref;
    }

    const T& operator*() const
    {
        assert(m_ref != nullptr && "Tried to dereference uninitialized ResourcePtr!");
        return **m_ref;
    }

    T* operator->()
    {
        assert(m_ref != nullptr && "Tried to dereference uninitialized ResourcePtr!");
        return m_ref->operator->();
    }

    const T* operator->() const
    {
        assert(m_ref != nullptr && "Tried to dereference uninitialized ResourcePtr!");
        return m_ref->operator->();
    }

    T* Get()
    {
        assert(m_ref != nullptr && "Tried to dereference uninitialized ResourcePtr!");
        return m_ref->Get();
    }

    const T* Get() const
    {
        assert(m_ref != nullptr && "Tried to dereference uninitialized ResourcePtr!");
        return m_ref->Get();
    }

    [[nodiscard]] id_t Id() const
    {
        assert(m_ref != nullptr && "Tried to retrieve the Id of an uninitialized ResourcePtr!");
        return m_ref->Id();
    }

    [[nodiscard]] auto& OnDestroy() const
    {
        assert(m_ref != nullptr && "Cannot connect uninitialized ResourcePtr!");
        return m_ref->s_destroy;
    }

    template<typename U>
    [[nodiscard]] ResourcePtr<U> As() const
    {
        typedef ResourceWrapper<typename std::remove_const<U>::type> UWrappedType;
        //static_assert(std::is_same<U, IResource>::value, "Can only downcast IResource");
        auto voidPtr = std::static_pointer_cast<void>(m_ref);
        auto typedPtr = std::static_pointer_cast<UWrappedType>(voidPtr);

        ResourcePtr<U> da = ResourcePtr<U>(std::move(typedPtr));
        return std::move(da);
    }
};

template<typename T>
ResourcePtr<T> MakeResourcePtr(id_t id, T&& v)
{
    std::shared_ptr<ResourceWrapper<T>> ptr = std::make_shared<ResourceWrapper<T>>(id, std::forward<T>(v));
    return ResourcePtr<T>(std::move(ptr));
}

template<typename T, typename... A>
ResourcePtr<T> MakeResourcePtr(id_t id, A&&... args)
{
    std::shared_ptr<ResourceWrapper<T>> ptr = std::make_shared<ResourceWrapper<T>>(id, std::forward<A>(args)...);
    return ResourcePtr<T>(std::move(ptr));
}

template <typename T>
class WeakResourcePtr {
private:
    typedef ResourceWrapper<typename std::remove_const<T>::type> WrappedType;

    std::weak_ptr<WrappedType> m_ref;

public:
    explicit WeakResourcePtr(const std::weak_ptr<WrappedType>& ref)
            : m_ref(std::move(ref))
    {}

    explicit WeakResourcePtr(ResourcePtr<T>& ptr)
            : m_ref(std::weak_ptr(ptr.m_ref))
    {}

    [[nodiscard]] ResourcePtr<T> Lock() const
    {
        std::shared_ptr<WrappedType> ref = m_ref.lock();
        return ResourcePtr<T>(std::move(ref));
    }

    [[nodiscard]] bool Expired() const
    {
        return m_ref.expired();
    }

};


} // namespace Gravitaris