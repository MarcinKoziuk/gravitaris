#pragma once

// Extracted and adapted from Turbo Badger's TBID
// (C) 2011-2014, Emil Segerås

#include <cstddef>
#include <cstdint>

#ifndef NDEBUG
#include <string>
#endif

namespace Gravitaris {

typedef std::uint32_t id_t;

// FNV constants
static constexpr id_t basis = 2166136261U;
static constexpr id_t prime = 16777619U;

// compile-time hash helper function
static constexpr id_t IDC_one(char c, const char* remain, id_t value)
{
    return c == 0 ? value : IDC_one(remain[0], remain + 1, (value ^ c) * prime);
}

// compile-time hash
static constexpr id_t IDC(const char* str)
{
    return (str && *str) ? IDC_one(str[0], str + 1, basis) : 0;
}

#ifndef NDEBUG

// Debug (non compile-time constant) version of ID, which checks for
// hash collisions.
class IDD {
private:
    id_t id;
public:
    explicit IDD(id_t id = 0) : id(id) {}
    IDD(const IDD &id) : id(id) {}
    explicit IDD(const char *string) : id(0L) { Set(string); }

    void Set(const char *string);

    explicit(false) operator id_t () const { return id; } // NOLINT(*-explicit-constructor)
    IDD& operator= (const IDD &new_id) = default;

    static std::string GetString(id_t id);
};

[[nodiscard]] static id_t ID(const char* str)
{
    return IDD(str);
}

[[nodiscard]] static id_t operator"" _id(const char *str, std::size_t) noexcept {
    return ID(str);
}

#else

[[nodiscard]] static constexpr id_t ID(const char* str)
{
	return IDC(str);
}

[[nodiscard]] constexpr static id_t operator"" _id(const char *str, std::size_t) noexcept {
    return ID(str);
}

#endif // NDEBUG

} // namespace Gravitaris
