#pragma once

#if defined(_WIN32)
#include <windows.h> // SEH only (EXCEPTION_EXECUTE_HANDLER, GetExceptionCode)
#endif

#include <Corrade/Containers/ArrayView.h>

#include <Magnum/GL/Buffer.h>

namespace Gravitaris {

// glBufferData occasionally raises a first-chance SEH exception in the
// NVIDIA driver on this machine (root cause unknown); catch it so it doesn't
// kill the process. Upload still succeeds either way. Returns 0 on success,
// or the SEH exception code on failure (caller decides how to log/skip).
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
