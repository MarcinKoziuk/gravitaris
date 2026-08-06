#include <SDL_clipboard.h>
#include <SDL_stdinc.h>

#include <gravitaris/game/logging.hpp>

#include "system-interface.hpp"

namespace Gravitaris {

using namespace std::chrono;

SystemInterface::SystemInterface()
{
    m_startTime = high_resolution_clock::now();
}

double SystemInterface::GetElapsedTime()
{
    auto current_time = high_resolution_clock::now();
    return duration<double>(current_time - m_startTime).count();
}

bool SystemInterface::LogMessage(Rml::Log::Type type, const Rml::String& message)
{
    const char* tag = "RmlUi";
    switch (type) {
        case Rml::Log::LT_ALWAYS:
            Gravitaris::Log::SimpleLogStream(Gravitaris::Log::notice, tag) << message;
            return true;
        case Rml::Log::LT_ERROR:
            Gravitaris::Log::SimpleLogStream(Gravitaris::Log::error, tag) << message;
            return true;
        case Rml::Log::LT_ASSERT:
            Gravitaris::Log::SimpleLogStream(Gravitaris::Log::assert, tag) << message;
            return true;
        case Rml::Log::LT_WARNING:
            Gravitaris::Log::SimpleLogStream(Gravitaris::Log::warning, tag) << message;
            return true;
        case Rml::Log::LT_INFO:
            Gravitaris::Log::SimpleLogStream(Gravitaris::Log::info, tag) << message;
            return true;
        case Rml::Log::LT_DEBUG:
            Gravitaris::Log::SimpleLogStream(Gravitaris::Log::debug, tag) << message;
            return true;
        case Rml::Log::LT_MAX:
            Gravitaris::Log::SimpleLogStream(Gravitaris::Log::trace, tag) << message;
            return true;
        default:
            return false;
    }
}

#if defined(__EMSCRIPTEN__)
// Emscripten's SDL shim declares the clipboard API but implements none of it,
// and the browser has no synchronous clipboard read to build one from. Copy
// and paste stay inside the page.
static Rml::String s_clipboard;

void SystemInterface::SetClipboardText(const Rml::String& text)
{
    s_clipboard = text;
}

void SystemInterface::GetClipboardText(Rml::String& text)
{
    text = s_clipboard;
}
#else
void SystemInterface::SetClipboardText(const Rml::String& text)
{
    SDL_SetClipboardText(text.c_str());
}

void SystemInterface::GetClipboardText(Rml::String& text)
{
    char* owned = SDL_GetClipboardText();
    if (!owned) return;

    text = owned;
    SDL_free(owned);
}
#endif

} // namespace Gravitaris
