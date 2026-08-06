#if !defined(_WIN32)

#include "crash-handler.hpp"

namespace Gravitaris {

void InstallCrashHandler() {} // no POSIX equivalent written yet

} // namespace Gravitaris

#else

#include <windows.h>
#include <dbghelp.h>

#include <gravitaris/game/logging.hpp>

#include "crash-handler.hpp"

namespace Gravitaris {

static constexpr int MAX_FRAMES = 64;

static const char* ExceptionName(DWORD code);
static void WriteFrame(int index, DWORD64 address);

// Last-chance, not vectored: a first-chance handler would also catch the
// NVIDIA driver's benign AV inside glBufferData that SafeUpload already
// swallows (see gl-safe-upload.hpp), and drown the real fault in noise.
static LONG WINAPI HandleCrash(EXCEPTION_POINTERS* info)
{
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    LOG(error) << "*** CRASH: " << ExceptionName(code) << " (0x" << std::hex << code << std::dec
               << ") at 0x" << std::hex
               << reinterpret_cast<DWORD64>(info->ExceptionRecord->ExceptionAddress) << std::dec;

    // An access violation names what it touched, which is usually the whole
    // story: 0 is a null deref, a small value an offset off null, and a
    // recognisable poison pattern a freed block.
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
        const ULONG_PTR kind = info->ExceptionRecord->ExceptionInformation[0];
        LOG(error) << "*** " << (kind == 0 ? "read from" : kind == 1 ? "write to" : "execute at")
                   << " 0x" << std::hex << info->ExceptionRecord->ExceptionInformation[1] << std::dec;
    }

    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(process, nullptr, TRUE);

    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    for (int i = 0; i < MAX_FRAMES; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) break;
        WriteFrame(i, frame.AddrPC.Offset);
    }

    SymCleanup(process);
    return EXCEPTION_CONTINUE_SEARCH; // still crash, so the exit code stays honest
}

void InstallCrashHandler()
{
    SetUnhandledExceptionFilter(&HandleCrash);
}

static void WriteFrame(int index, DWORD64 address)
{
    // SYMBOL_INFO is a header plus an inline name, so the buffer has to carry
    // both; MaxNameLen counts the name part only.
    char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    DWORD64 displacement = 0;
    const char* name = SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)
                             ? symbol->Name : "??";

    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    if (SymGetLineFromAddr64(GetCurrentProcess(), address, &lineDisplacement, &line)) {
        LOG(error) << "***  #" << index << " " << name << "  " << line.FileName << ":"
                   << line.LineNumber;
    }
    else {
        LOG(error) << "***  #" << index << " " << name << " + 0x" << std::hex << displacement
                   << std::dec;
    }
}

static const char* ExceptionName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "access violation";
    case EXCEPTION_STACK_OVERFLOW:        return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "integer divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION:      return "privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR:         return "in-page error";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    default:                              return "exception";
    }
}

} // namespace Gravitaris

#endif
