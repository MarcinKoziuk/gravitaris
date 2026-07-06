#pragma once

#include <chrono>

#include <RmlUi/Core/SystemInterface.h>

namespace Gravitaris {

class SystemInterface : public Rml::SystemInterface {
private:
    std::chrono::high_resolution_clock::time_point m_startTime;

public:
    SystemInterface();

    ~SystemInterface() override = default;

    double GetElapsedTime() override;

    virtual bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;

};

} // namespace Gravitaris
