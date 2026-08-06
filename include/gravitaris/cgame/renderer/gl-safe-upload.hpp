#pragma once

#if defined(_WIN32)
#include <windows.h> // SEH only (EXCEPTION_EXECUTE_HANDLER, GetExceptionCode)
#endif

#include <Corrade/Containers/ArrayView.h>

#include <Magnum/GL/Buffer.h>

namespace Gravitaris {

// Catches an SEH exception raised inside glBufferData so it doesn't kill the
// process. Returns 0 on success, or the SEH exception code on failure (caller
// decides how to log/skip).
//
// The fault this was written for turned out to be ours: a caller handing the
// driver eight times the byte count it meant to (docs/client-startup-crash.md),
// fixed at its source. Nothing is known to still need this, and it is worth
// deleting once a few sessions have passed without it firing -- a swallowed
// access violation leaves the driver in a state nobody has reasoned about.
//
// `data` must stay a `const void*` here: ArrayView<const void>'s typed
// constructor would multiply `bytes` by sizeof(T) again.
inline unsigned long SafeUpload(Magnum::GL::Buffer& buf, const void* data, std::size_t bytes)
{
#if defined(_WIN32)
    __try {
        buf.setData(Corrade::Containers::ArrayView<const void>{data, bytes});
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
#else
    buf.setData(Corrade::Containers::ArrayView<const void>{data, bytes});
    return 0;
#endif
}

} // namespace Gravitaris
