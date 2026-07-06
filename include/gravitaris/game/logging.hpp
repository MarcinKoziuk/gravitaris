#pragma once

#include <cstddef>
#include <iostream>
#include <sstream>

namespace Gravitaris::Log {

enum Severity {
    trace,
    debug,
    info,
    notice,
    warning,
    error,
    assert,
    critical,
    fatal
};

static const char* SEVERITY_TEXT_REPR[] = {
    "TRACE",
    "DEBUG",
    "INFO",
    "NOTICE",
    "WARNING",
    "ERROR",
    "ASSERT",
    "CRITICAL",
    "FATAL"
};

class SimpleLogStream {
private:
    Severity severity;
    std::stringstream ss;

public:
    SimpleLogStream(Severity severity, const char* file, const char* func, int line);

    SimpleLogStream(Severity severity, const std::string_view tag);

    ~SimpleLogStream();

    [[nodiscard]]
    bool IsWarningOrHigher() const;

    [[nodiscard]]
    std::ostream& PhysicalStream() const;

    template<class T>
    SimpleLogStream& operator<<(const T &x)
    {
        ss << x;
        return *this;
    }
};

} // namespace Gravitaris::Log


/*
 * Logs a message.
 * Usage: LOG(<severity>) << "<text to output>";
 */
#define LOG(severity) Gravitaris::Log::SimpleLogStream(Gravitaris::Log::severity, __FILE__, __func__, __LINE__)
