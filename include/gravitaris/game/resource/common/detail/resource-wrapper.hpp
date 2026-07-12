#pragma once

#include <sigslot/signal.hpp>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {



template<typename T>
class ResourceWrapper {
private:
    sigslot::signal<id_t, T&> s_destroy;
    id_t m_id;
    T m_value;

    template <class U>
    friend class ResourcePtr;

public:
    explicit ResourceWrapper(id_t id, T&& value)
            : m_id(id)
            , m_value(std::forward<T>(value))
    {}

    template<typename... A>
    explicit ResourceWrapper(id_t id, A&&... args)
            : m_id(id)
            , m_value(std::forward<A>(args)...)
    {}

    ResourceWrapper(ResourceWrapper<T>&& o) noexcept
            : s_destroy(std::move(o.s_destroy))
            , m_id(std::move(o.m_id))
            , m_value(std::move(o.m_value))
    {}

    ~ResourceWrapper()
    {
        s_destroy(m_id, m_value);
    }

    [[nodiscard]] id_t Id() const
    {
        return m_id;
    }

    const T& operator*() const
    {
        return m_value;
    }

    T& operator*()
    {
        return m_value;
    }

    const T* operator->() const
    {
        return &m_value;
    }

    T* operator->()
    {
        return &m_value;
    }

    const T* Get() const
    {
        return &m_value;
    }

    T* Get()
    {
        return &m_value;
    }

    // Make noncopyable
    ResourceWrapper(const ResourceWrapper&) = delete;
    ResourceWrapper& operator=(const ResourceWrapper&) = delete;
};


} // namespace Gravitaris