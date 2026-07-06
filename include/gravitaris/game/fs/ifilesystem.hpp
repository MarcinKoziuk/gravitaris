#pragma once

#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include <cstdint>

#include <gravitaris/game/id.hpp>

namespace Gravitaris {

class IFilesystem {
public:
    virtual bool Init() = 0;

    virtual void Shutdown() = 0;

    virtual std::size_t GetLength(id_t id) = 0;

    virtual std::size_t GetLength(const std::string& path) = 0;

    virtual bool ReadBytes(id_t id, std::vector<std::uint8_t>* destVector) = 0;

    virtual bool ReadBytes(const std::string& path, std::vector<std::uint8_t>* destVector) = 0;

    virtual bool ReadString(id_t id, std::string* destString) = 0;

    virtual bool ReadString(const std::string& path, std::string* destString) = 0;

    // may return a nullptr unique_ptr as well
    virtual std::unique_ptr<std::istream> OpenAsStream(id_t id) = 0;

    virtual std::unique_ptr<std::istream> OpenAsStream(const std::string& path) = 0;

    virtual bool GetPathForId(id_t id, std::string& dest) = 0;

    virtual ~IFilesystem() = default;
};

} // namespace Gravitaris
