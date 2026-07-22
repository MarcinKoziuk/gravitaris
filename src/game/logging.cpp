#include <iostream>
#include <algorithm>
#include <ctime>

#include <gravitaris/game/logging.hpp>

// Claude: concatenate namespaces for me (not just here)
namespace Gravitaris {
namespace Log {

static std::string formatFileString(std::string file)
{
    std::string source_path = GRAVITARIS_COMPILETIME_SOURCE_PATH;

#ifdef _WIN32
    // temp fix for cmake's source dir representation
    std::replace(source_path.begin(), source_path.end(), '/', '\\');
#endif

    std::size_t index = file.find(source_path);
    if (index != std::string::npos) {
        return file.substr(index + source_path.length() + 1, file.length());
    } else {
        return file;
    }
}

static std::string makeDateString()
{
    std::time_t rawtime;
    std::tm timeinfo;
    char buffer[80];

    std::time(&rawtime);

    // don't use std::localtime; NOT thread-safe!!
#ifdef _WIN32
    localtime_s(&timeinfo, &rawtime);
#else
    localtime_r(&rawtime, &timeinfo);
#endif

    std::strftime(buffer, 80, "%Y-%m-%d %H:%M:%S", &timeinfo);
    return std::string(buffer);
}

SimpleLogStream::SimpleLogStream(Severity severity, const char* file, const char* func, int line)
        : severity(severity)
{
    ss << makeDateString() << ' ' << SEVERITY_TEXT_REPR[severity] << " [" << formatFileString(file) << ":" << line << "]: ";
}

SimpleLogStream::SimpleLogStream(Severity severity, const std::string_view tag)
        : severity(severity)
{
    ss << makeDateString() << ' ' << SEVERITY_TEXT_REPR[severity] << " [" << tag << "]: ";
}

SimpleLogStream::~SimpleLogStream()
{
    PhysicalStream() << ss.str() << "\n" << std::flush;
}

bool SimpleLogStream::IsWarningOrHigher() const
{
    return severity >= warning;
}

std::ostream& SimpleLogStream::PhysicalStream() const
{
    return IsWarningOrHigher() ? std::cerr : std::cout;
}

} // namespace Log
} // namespace Gravitaris
