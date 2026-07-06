#pragma once

#include <RmlUi/Core/FileInterface.h>

#include <gravitaris/game/fs/ifilesystem.hpp>

namespace Gravitaris {

class FileInterface : public Rml::FileInterface {
private:
    IFilesystem& m_filesystem;

    Rml::FileHandle m_handleSequence;

    std::unordered_map<Rml::FileHandle, std::unique_ptr<std::istream>> m_handles;

    std::istream* TryGetStreamFromHandle(Rml::FileHandle handle);

public:
    FileInterface(IFilesystem& filesystem);

    ~FileInterface() override = default;

    Rml::FileHandle Open(const Rml::String& path) override;

    void Close(Rml::FileHandle handle) override;

    size_t Read(void* buffer, size_t size, Rml::FileHandle handle) override;

    bool Seek(Rml::FileHandle handle, long offset, int origin) override;

    size_t Tell(Rml::FileHandle handle) override;

    size_t Length(Rml::FileHandle handle) override;

    bool LoadFile(const Rml::String& path, Rml::String& out_data) override;

};

} // namespace Gravitaris
