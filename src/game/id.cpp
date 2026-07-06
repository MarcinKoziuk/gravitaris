#include <cassert>
#include <mutex>
#include <unordered_map>

#include <gravitaris/gravitaris.hpp>
#include <gravitaris/game/id.hpp>

// Extracted and adapted from Turbo Badger's TBID
// (C) 2011-2014, Emil Segerås

#ifndef NDEBUG

namespace Gravitaris {

static std::mutex g_hashed_strings_m;

// Hash table for checking if we get any collisions (same hash value for IDs created
// from different strings)
std::unordered_map<id_t, std::string>& GetHashedStrings()
{
    static std::unordered_map<id_t, std::string> g_hashed_strings;
    return g_hashed_strings;
}

void IDD::Set(const char *string)
{
    id = IDC(string);

    if (Gravitaris::HasEnteredMain) g_hashed_strings_m.lock();

    std::unordered_map<id_t, std::string>& hashedStrings = GetHashedStrings();
    if (hashedStrings.count(id)) {
        assert(hashedStrings[id] == string);
    } else {
        hashedStrings[id] = string;
    }

    if (Gravitaris::HasEnteredMain) g_hashed_strings_m.unlock();
}

std::string IDD::GetString(id_t id)
{
    std::string ret;

    if (Gravitaris::HasEnteredMain) g_hashed_strings_m.lock();

    std::unordered_map<id_t, std::string>& hashedStrings = GetHashedStrings();
    ret = hashedStrings[id];

    if (Gravitaris::HasEnteredMain) g_hashed_strings_m.unlock();
    return ret;
}

} // namespace Gravitaris

#endif // NDEBUG
