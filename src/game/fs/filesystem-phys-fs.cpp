#include <cstdlib>
#include <cassert>
#include <limits>

#include <physfs.h>
#include <memory>
#include <physfs/physfs.hpp>

#include <gravitaris/gravitaris.hpp>
#include <gravitaris/game/logging.hpp>
#include <gravitaris/game/fs/filesystem-physfs.hpp>

// not thread safe, just a safeguard
static int instanceCount = 0;

static Gravitaris::FilesystemPhysFS* instance = nullptr;

namespace Gravitaris {

FilesystemPhysFS::FilesystemPhysFS()
        : m_isInitialized(false)
{
    instanceCount++;
    if (instanceCount != 1) {
        LOG(fatal) << "Only a single FilesystemPhysFS can exist at a time!";
        std::abort();
    }
    ::instance = this;
}

FilesystemPhysFS::~FilesystemPhysFS()
{
    instanceCount--;
}

FilesystemPhysFS& FilesystemPhysFS::instance()
{
    return *::instance;
}

bool FilesystemPhysFS::Init()
{
    int result = PHYSFS_init(GRAVITARIS_NAME);
    if (!result) {
        LOG(error) << "PhysFS initialization failed";
        return false;
    }

    int mounts = 0;
    mounts += (PHYSFS_mount("../data", "/", true) != 0);
    mounts += (PHYSFS_mount("./data", "/", true) != 0);
    mounts += (PHYSFS_mount("/mnt/host/GravitarisNG/data", "/", true) != 0);
    mounts += (PHYSFS_mount(R"(C:\Users\marcin\Projects\GravitarisNG\data)", "/", true) != 0);
    mounts += (PHYSFS_mount("/Users/marcin/projects/gravitaris/data", "/", true) != 0);

    if (!mounts) {
        LOG(error) << "Mounting of data directory failed";
        return false;
    }
    else {
        LOG(info) << "Number of PhysFS mounts: " << mounts;
    }

    LOG(trace) << "Currently mounted search paths are:";

    const std::vector<std::string> paths = PhysFS::getSearchPath();
    for (const std::string& path : paths) {
        LOG(trace) << path;
    }

    ReloadIdPaths();

    m_isInitialized = true;
    return true;
}

void FilesystemPhysFS::Shutdown()
{
    int result = PHYSFS_deinit();
    if (!result) {
        LOG(error) << "Failed to cleanly shut down PhysFS";
    }
}

#define ID_PATH_CHECK(return_good, return_bad) \
	std::string path; \
	bool exists = GetPathForId(id, path); \
	if (exists) \
		return return_good; \
	else { \
		LOG(error) << "no path with id_t " << id << " found"; \
		return return_bad; \
	}

std::size_t FilesystemPhysFS::GetLength(id_t id)
{
    ID_PATH_CHECK(GetLength(path), 0L)
}

std::size_t FilesystemPhysFS::GetLength(const std::string& path)
{
    assert(m_isInitialized);

    PHYSFS_File* file = PHYSFS_openRead(path.c_str());
    if (file) {
        return PHYSFS_fileLength(file);
    }
    else {
        return 0L;
    }
}

bool FilesystemPhysFS::ReadBytes(id_t id, std::vector<std::uint8_t>* destVector)
{
    ID_PATH_CHECK(ReadBytes(path, destVector), false)
}

bool FilesystemPhysFS::ReadBytes(const std::string& path, std::vector<std::uint8_t>* destVector)
{
    assert(m_isInitialized);

    PHYSFS_File* file = PHYSFS_openRead(path.c_str());
    if (file) {
        std::size_t length = PHYSFS_fileLength(file);
        destVector->reserve(length);
        destVector->resize(length);

        assert(length < std::numeric_limits<PHYSFS_uint32>::max());

        PHYSFS_read(file, &(*destVector)[0], 1, static_cast<PHYSFS_uint32>(length));
        PHYSFS_close(file);

        return true;
    }
    else {
        LOG(error) << "Could not open: " << path;
        return false;
    }
}

bool FilesystemPhysFS::ReadString(id_t id, std::string* destString)
{
    ID_PATH_CHECK(ReadString(path, destString), false)
}

bool FilesystemPhysFS::ReadString(const std::string& path, std::string* destString)
{
    assert(m_isInitialized);

    PHYSFS_File* file = PHYSFS_openRead(path.c_str());
    if (file) {
        PHYSFS_sint64 length = PHYSFS_fileLength(file);
        destString->reserve(length);
        destString->resize(length);

        assert(length < std::numeric_limits<PHYSFS_uint32>::max());

        // TODO: check 0 termination and optimize reserve() if needed
        PHYSFS_read(file, &(*destString)[0], 1, static_cast<PHYSFS_uint32>(length));
        PHYSFS_close(file);

        return true;
    }
    else {
        LOG(error) << "Could not open: " << path;
        return false;
    }
}

std::unique_ptr<std::istream> FilesystemPhysFS::OpenAsStream(id_t id)
{
    ID_PATH_CHECK(OpenAsStream(path), nullptr)
}

std::unique_ptr<std::istream> FilesystemPhysFS::OpenAsStream(const std::string& path)
{
    PHYSFS_File* file = PHYSFS_openRead(path.c_str());
    if (file) {
        return std::make_unique<PhysFS::ifstream>(file);
    }
    else {
        LOG(error) << "Could not open: " << path;
        return nullptr;
    }
}

void FilesystemPhysFS::PathEnumCallback(void* self_, const char* root_, const char* filename)
{
    auto* self = reinterpret_cast<FilesystemPhysFS*>(self_);

    std::string root(root_);
    std::string path = root.empty() ? filename : root + "/" + filename;
    const std::uint32_t id = ID(path.c_str());

    /*if (!self->m_idPaths.count(id_t) == 0) {
		LOG(error) << "ID hash collision!";
        LOG(error) << path.c_str() << "; " << id_t;
		LOG(error) << self->m_idPaths[id_t] << "; " << id_t;
		assert(false && "ID hash collision!");
	}

    assert(self->m_idPaths.count(id_t) == 0 && "ID hash collision!");*/
    self->m_idPaths[id] = path;

    PHYSFS_enumerateFilesCallback(path.c_str(), &PathEnumCallback, self);
}

void FilesystemPhysFS::ReloadIdPaths()
{
    m_idPaths.clear();

    PHYSFS_enumerateFilesCallback("", &PathEnumCallback, this);

    LOG(trace) << "Enumerated " << m_idPaths.size() << " files in search path";
}

bool FilesystemPhysFS::GetPathForId(id_t id, std::string& dest)
{
    if (GT_LIKELY(m_idPaths.count(id))) {
        dest = m_idPaths.at(id);
        return true;
    }
    else {
        // Check again, maybe something has changed in the data directory (development)
        LOG(trace) << "Could NOT find path for id_t " << id << "; re-enumerating paths...";
        ReloadIdPaths();

        if (m_idPaths.count(id)) {
            dest = m_idPaths.at(id);
            return true;
        }
        else {
            return false;
        }
    }
}

} // namespace Gravitaris
