#pragma once

#include <unordered_map>

#include <gravitaris/game/fs/ifilesystem.hpp>

namespace Gravitaris {

class FilesystemPhysFS : public IFilesystem {
private:
    bool m_isInitialized;
    std::unordered_map<id_t, std::string> m_idPaths;

    void ReloadIdPaths();
    static void PathEnumCallback(void* self_, const char* rootDir, const char* path);

public:
    FilesystemPhysFS();
    ~FilesystemPhysFS() override;

    static FilesystemPhysFS& instance();

    bool Init() override;

    void Shutdown() override;

    std::size_t GetLength(id_t id) override;

    std::size_t GetLength(const std::string& path) override;

    bool ReadBytes(id_t id, std::vector<std::uint8_t>* destVector) override;

    bool ReadBytes(const std::string& path, std::vector<std::uint8_t>* destVector) override;

    bool ReadString(id_t id, std::string* destString) override;

    bool ReadString(const std::string& path, std::string* destString) override;

    std::unique_ptr<std::istream> OpenAsStream(id_t id) override;

    std::unique_ptr<std::istream> OpenAsStream(const std::string& path) override;

    bool GetPathForId(id_t id, std::string& dest) override;
};

} // namespace Gravitaris
