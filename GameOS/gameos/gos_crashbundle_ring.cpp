// Crash-bundle in-memory trace ring: the one piece of gos_crashbundle.cpp that the sim and the logging facade call during normal
// play (crashbundle_append). Split out so it builds into the small mc2_core_diag library (no dbghelp, no crashpad, no renderer).
// Linux has no in-process bundle writer (crashpad owns the fatal path), so the append is a no-op there.
#include "mc2_czstring.h"
#include "gos_crashbundle.h"

#ifndef _WIN32

extern "C" void crashbundle_append(mc2::czstring) {}

#else

#include <algorithm>
#include <cstring>
#include <windows.h>

namespace {
const size_t kRingBytes = 64 * 1024;
char g_ring[kRingBytes];
volatile LONG64 g_ring_cursor = 0;  // total bytes written, monotonic
volatile LONG   g_ring_lock   = 0;  // spinlock, 0=free 1=held
}  // namespace

// Appends a single instrumentation line to the ring (normal program flow, not the crash handler).
extern "C" void crashbundle_append(mc2::czstring line)
{
    if (!line) return;
    size_t n = strlen(line);
    if (n == 0) return;
    // Trim to avoid pathological lines blowing out the ring in one write.
    n = (std::min)(n, kRingBytes / 2);

    // Spinlock - short critical sections only.
    while (InterlockedCompareExchange(&g_ring_lock, 1, 0) != 0) {
        YieldProcessor();
    }
    LONG64 cur = g_ring_cursor;
    for (size_t i = 0; i < n; ++i) {
        g_ring[(cur + i) % kRingBytes] = line[i];
    }
    // ensure newline terminator
    g_ring[(cur + n) % kRingBytes] = '\n';
    g_ring_cursor = cur + (LONG64)n + 1;
    InterlockedExchange(&g_ring_lock, 0);
}

// Crash path: writes the wrap-safe linear view of the ring to an open file handle (WriteFile only, no allocation).
void crashbundle_ring_write(void* fileHandle)
{
    HANDLE h = static_cast<HANDLE>(fileHandle);
    LONG64 cur = g_ring_cursor;
    DWORD wrote = 0;
    if (cur < (LONG64)kRingBytes) {
        WriteFile(h, g_ring, static_cast<DWORD>(cur), &wrote, nullptr);
    } else {
        // wrapped: oldest byte is at cur % kRingBytes
        DWORD start = static_cast<DWORD>((cur % kRingBytes));
        WriteFile(h, g_ring + start, static_cast<DWORD>((kRingBytes - start)), &wrote, nullptr);
        WriteFile(h, g_ring, start, &wrote, nullptr);
    }
}

#endif
