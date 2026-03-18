/*++

Module Name:

    Trace.h

Abstract:

    This module contains the local type definitions for the
    AppSandboxVDD driver tracing.

Environment:

    Windows User-Mode Driver Framework 2

--*/

//
// Define the tracing flags.
//
// Tracing GUID - a5b0cafe-1234-4000-8000-000000000vdd
//

#define WPP_CONTROL_GUIDS                                              \
    WPP_DEFINE_CONTROL_GUID(                                           \
        AppSandboxVDDTraceGuid, (a5b0cafe,1234,4000,8000,0000000000dd),\
                                                                       \
        WPP_DEFINE_BIT(MYDRIVER_ALL_INFO)                              \
        WPP_DEFINE_BIT(TRACE_DRIVER)                                   \
        WPP_DEFINE_BIT(TRACE_DEVICE)                                   \
        WPP_DEFINE_BIT(TRACE_QUEUE)                                    \
        )

#define WPP_FLAG_LEVEL_LOGGER(flag, level)                             \
    WPP_LEVEL_LOGGER(flag)

#define WPP_FLAG_LEVEL_ENABLED(flag, level)                            \
    (WPP_LEVEL_ENABLED(flag) &&                                        \
     WPP_CONTROL(WPP_BIT_ ## flag).Level >= level)

#define WPP_LEVEL_FLAGS_LOGGER(lvl,flags)                              \
           WPP_LEVEL_LOGGER(flags)

#define WPP_LEVEL_FLAGS_ENABLED(lvl, flags)                            \
           (WPP_LEVEL_ENABLED(flags) && WPP_CONTROL(WPP_BIT_ ## flags).Level >= lvl)

//
// This comment block is scanned by the trace preprocessor to define our
// Trace function.
//
// begin_wpp config
// FUNC Trace{FLAG=MYDRIVER_ALL_INFO}(LEVEL, MSG, ...);
// FUNC TraceEvents(LEVEL, FLAGS, MSG, ...);
// end_wpp

//
// Driver specific #defines
//
#if UMDF_VERSION_MAJOR == 2 && UMDF_VERSION_MINOR == 0
    #define MYDRIVER_TRACING_ID      L"AppSandbox\\AppSandboxVDD V1.0"
#endif

/*
 * Simple debug trace helper (always available, independent of WPP).
 * Usage: VDD_TRACE("format string %d", value);
 */
#ifdef DBG
#define VDD_TRACE(fmt, ...) \
    do { \
        char _vdd_buf[512]; \
        _snprintf_s(_vdd_buf, sizeof(_vdd_buf), _TRUNCATE, \
                    "[AppSandboxVDD] " fmt "\n", ##__VA_ARGS__); \
        OutputDebugStringA(_vdd_buf); \
    } while (0)
#else
#define VDD_TRACE(fmt, ...) ((void)0)
#endif
