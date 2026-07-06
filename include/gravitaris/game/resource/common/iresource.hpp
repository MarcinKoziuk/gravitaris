#pragma once

#include <cstddef>
#include <numeric>
#include <concepts>
#include <iterator>

#include <gravitaris/game/fwd.hpp>

namespace Gravitaris {

class IResource {
public:
    IResource() = default;

    virtual ~IResource() = default;

    [[nodiscard]] virtual std::size_t CalculateSize() const = 0;

    [[nodiscard]] virtual const char* GetResourceName() const = 0;

    struct LoadingContext {
        ResourceLoader& resourceLoader;
        IFilesystem& filesystem;
    };

    // Make noncopyable
    IResource(const IResource&) = delete;
    IResource& operator=(const IResource&) = delete;
};

template <typename T>
concept IsIResource = std::is_base_of<IResource, T>::value;

template<typename T>
std::size_t PodContainerSize(const T& container)
{
    return sizeof(typename T::value_type) * container.size();
}

template<typename T>
std::size_t PodContainerContainerSize(const T& container)
{
    return std::accumulate(
            container.begin(),
            container.end(),
            std::size_t(0L),
            [](std::size_t v, const typename T::value_type& e) {
                return v + PodContainerSize(e);
            }
    );
}

template<typename T>
concept SizeAwareContainer = requires(T container) {
    typename T::value_type;
    requires std::input_iterator<typename T::const_iterator>;

    { CalculateSize(*std::begin(container)) } -> std::same_as<std::size_t>;
};

template<SizeAwareContainer T>
std::size_t SizeAwareContainerSize(const T& container)
{
    return std::accumulate(
            container.begin(),
            container.end(),
            std::size_t(0L),
            [](std::size_t v, const typename T::value_type& e) {
                return v + CalculateSize(e);
            }
    );
}

} // namespace Gravitaris
