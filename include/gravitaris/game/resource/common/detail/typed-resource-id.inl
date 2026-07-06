#pragma once

#include <typeindex>

#include <gravitaris/game/id.hpp>

namespace Gravitaris {

struct TypedResourceId {
    std::type_index typeIndex;
    id_t resourceId;

    inline TypedResourceId(std::type_index type, id_t resourceId);
    template<typename T> static TypedResourceId Of(id_t);

    inline bool operator==(const TypedResourceId& other) const;
    inline int operator<(const TypedResourceId& other) const;

    struct Hash {
        inline std::size_t operator()(const TypedResourceId& k) const;
    };
};


template<typename T>
TypedResourceId TypedResourceId::Of(id_t resourceId)
{
    return { std::type_index(typeid(T)), resourceId };
}

TypedResourceId::TypedResourceId(std::type_index typeIndex, id_t resourceId)
        : typeIndex(typeIndex), resourceId(resourceId)
{}

bool TypedResourceId::operator==(const TypedResourceId& other) const
{
    return typeIndex == other.typeIndex
           && resourceId == other.resourceId;
}

int TypedResourceId::operator<(const TypedResourceId& other) const
{
    if (typeIndex < other.typeIndex) return 1;
    else if (typeIndex > other.typeIndex) return -1;
    else return 0;
}

std::size_t TypedResourceId::Hash::operator()(const TypedResourceId& k) const
{
    return std::hash<std::type_index>()(k.typeIndex)
           ^ std::hash<id_t>()(k.resourceId);
}

} // namespace Gravitaris
