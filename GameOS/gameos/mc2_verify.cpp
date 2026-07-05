// mc2_verify.cpp — MC2-VERIFY-LIVE-1 implementation.
// See GameOS/include/mc2_verify.h for the contract and docs/verify-primitive.md
// for the adoption protocol.

#include "mc2_verify.h"

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

// NOTE: this project defines LINUX_BUILD even in the Windows SDL build, so OS
// facilities are gated on _WIN32 (platform truth), not LINUX_BUILD (port flavor).
#ifdef _WIN32
#include <windows.h>
#endif

#include "gos_crashbundle.h" // crashbundle_append (extern "C", ring buffer)

// gameos.hpp declares this (game-specific STOP callback). Declared locally so
// this TU stays independent of the monolithic gameos.hpp.
extern int __cdecl InternalFunctionStop(const char* fmt, ...);

namespace {

enum Mode { MODE_OFF = 0, MODE_LOG = 1, MODE_FATAL = 2 };

// Cap on full "[VERIFY]" lines emitted per process (log mode) so a hot-loop
// fire cannot flood stderr/the ring; counting continues past the cap.
const long kLogLineCap = 64;

int resolve_mode()
{
    const char* v = std::getenv("MC2_VERIFY_MODE");
    if (!v || !*v)
        return MODE_LOG;
#ifdef _MSC_VER
    if (_stricmp(v, "off") == 0)   return MODE_OFF;
    if (_stricmp(v, "fatal") == 0) return MODE_FATAL;
    if (_stricmp(v, "log") == 0)   return MODE_LOG;
#else
    if (strcasecmp(v, "off") == 0)   return MODE_OFF;
    if (strcasecmp(v, "fatal") == 0) return MODE_FATAL;
    if (strcasecmp(v, "log") == 0)   return MODE_LOG;
#endif
    std::fprintf(stderr,
                 "[VERIFY] unknown MC2_VERIFY_MODE='%s' -- defaulting to log\n", v);
    return MODE_LOG;
}

int mode()
{
    // C++11 magic static: resolved once, thread-safe.
    static const int m = resolve_mode();
    return m;
}

// Fire counters. volatile long + Interlocked on Windows; plain elsewhere.
volatile long g_firesTotal   = 0;
volatile long g_firesMission = 0;

long bump(volatile long* p)
{
#ifdef _WIN32
    return InterlockedIncrement(p);
#else
    return ++(*p);
#endif
}

const char* basename_of(const char* file)
{
    const char* base = file;
    for (const char* p = file; *p; ++p)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    return base;
}

void emit_line(const char* line)
{
    std::fprintf(stderr, "%s\n", line);
    std::fflush(stderr);
#ifdef _WIN32
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
#endif
    crashbundle_append(line);
}

} // namespace

namespace mc2verify {

bool Fail(const char* file, int line, const char* cond, const char* fmt, ...)
{
    const int m = mode();
    if (m == MODE_OFF)
        return true; // exactly-legacy: behave as if the check passed

    const long total = bump(&g_firesTotal);
    bump(&g_firesMission);

    char msg[512];
    msg[0] = '\0';
    if (fmt && *fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        msg[sizeof(msg) - 1] = '\0';
    }

    char lineBuf[1024];
    snprintf(lineBuf, sizeof(lineBuf), "[VERIFY]%s %s:%d (%s) %s",
              (m == MODE_FATAL) ? " FATAL" : "",
              basename_of(file ? file : "?"), line,
              cond ? cond : "?", msg);
    lineBuf[sizeof(lineBuf) - 1] = '\0';

    if (m == MODE_FATAL || total <= kLogLineCap) {
        emit_line(lineBuf);
        if (m != MODE_FATAL && total == kLogLineCap) {
            char cap[128];
            snprintf(cap, sizeof(cap),
                      "[VERIFY] line cap (%ld) reached -- further fires "
                      "counted but not printed", kLogLineCap);
            cap[sizeof(cap) - 1] = '\0';
            emit_line(cap);
        }
    }

    if (m == MODE_FATAL) {
        // Route through the STOP callback (gameos.hpp contract), then make the
        // stop REAL: a non-continuable custom exception so the crash-bundle
        // SEH filter (gos_crashbundle.cpp) captures minidump + stack + the
        // instrumentation ring (which now ends with this [VERIFY] line), then
        // terminates (fast-exit under harness). If any wrapper swallows the
        // exception (editor SafeRunGameOSLogic SEH), terminate anyway —
        // fatal mode must never resume into the code the check guards.
        InternalFunctionStop("%s", lineBuf);
#ifdef _WIN32
        RaiseException(0xE0564631 /* 'VF1' */, EXCEPTION_NONCONTINUABLE, 0, NULL);
        TerminateProcess(GetCurrentProcess(), 0xE0564631);
#else
        std::abort();
#endif
    }
    return false;
}

void MissionSummary(const char* label)
{
    const int m = mode();
    if (m == MODE_OFF)
        return; // silent: off mode emits nothing

    long missionFires;
    long totalFires;
#ifdef _WIN32
    missionFires = InterlockedExchange(&g_firesMission, 0);
    totalFires = g_firesTotal;
#else
    missionFires = g_firesMission;
    g_firesMission = 0;
    totalFires = g_firesTotal;
#endif

    char lineBuf[512];
    snprintf(lineBuf, sizeof(lineBuf),
              "[VERIFY] mission-end fires=%ld total=%ld mode=%s label=%s",
              missionFires, totalFires,
              (m == MODE_FATAL) ? "fatal" : "log",
              label ? label : "(null)");
    lineBuf[sizeof(lineBuf) - 1] = '\0';
    emit_line(lineBuf);
}

} // namespace mc2verify
