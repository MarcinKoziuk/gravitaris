#include <gravitaris/game/logging.hpp>

#include "file-interface.hpp"

namespace Gravitaris {

FileInterface::FileInterface(IFilesystem& filesystem)
    : m_filesystem(filesystem)
    , m_handleSequence(1UL)
{}

Rml::FileHandle FileInterface::Open(const Rml::String& path)
{
    auto handle = m_handleSequence++;
    m_handles.try_emplace(handle, m_filesystem.OpenAsStream(path));
    return handle;
}

void FileInterface::Close(Rml::FileHandle handle)
{
    m_handles.erase(handle);
}

size_t FileInterface::Read(void *buffer, size_t size, Rml::FileHandle handle)
{
    auto stream = TryGetStreamFromHandle(handle);
    if (!stream) return 0L;

    stream->read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
    return stream->gcount();
}

bool FileInterface::Seek(Rml::FileHandle handle, long offset, int origin)
{
    auto stream = TryGetStreamFromHandle(handle);
    if (!stream) return false;

    stream->seekg(offset, static_cast<std::ios_base::seekdir>(origin));
    return !stream->fail();
}

size_t FileInterface::Tell(Rml::FileHandle handle)
{
    auto stream = TryGetStreamFromHandle(handle);
    if (!stream) return 0L;

    return static_cast<size_t>(stream->tellg());
}

size_t FileInterface::Length(Rml::FileHandle handle)
{
    auto stream = TryGetStreamFromHandle(handle);
    if (!stream) return 0L;

    std::streampos currentPos = stream->tellg();
    stream->seekg(0, std::ios::end);
    size_t length = static_cast<size_t>(stream->tellg());
    stream->seekg(currentPos); // Restore the original position
    return length;
}

bool FileInterface::LoadFile(const Rml::String& path, Rml::String& out_data)
{
    return m_filesystem.ReadString(path, &out_data);
}

std::istream *FileInterface::TryGetStreamFromHandle(Rml::FileHandle handle)
{
    if (m_handles.contains(handle)) {
        return m_handles.at(handle).get();
    }

    LOG(error) << "Invalid or closed file handle: " << handle;

    return nullptr;
}

} // namespace Gravitaris
