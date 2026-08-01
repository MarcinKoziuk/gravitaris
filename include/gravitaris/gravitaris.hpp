#pragma once

#define GRAVITARIS_NAME "Gravitaris"

#if defined(__GNUC__) || defined(__clang___)
#define GT_LIKELY(x) __builtin_expect(x, 1)
	#define GT_UNLIKELY(x) __builtin_expect(x, 0)
#else
    #define GT_LIKELY(x) (x)
    #define GT_UNLIKELY(x) (x)
#endif

#if defined(__GNUC__) || defined(__clang__)
    #define GT_UNUSED(x) __attribute__((unused)) x
#elif defined _MSC_VER
    #define GT_UNUSED(x) __pragma(warning(suppress:4100 4101)) x
#else
    #define GT_UNUSED x
#endif

namespace Gravitaris {
    extern bool HasEnteredMain;

    // M_PI is a POSIX/GNU <cmath> extension, not standard C++ -- MSVC doesn't
    // define it without _USE_MATH_DEFINES set before every <cmath> include.
    // A dozen files had grown their own copy of this constant instead.
    constexpr double PI = 3.14159265358979323846;
    constexpr float PI_F = 3.14159265358979323846f;
}
