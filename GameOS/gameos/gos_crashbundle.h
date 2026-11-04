// gos_crashbundle.h
// Crash-bundle + diagnostic-dialog layered on top of the existing MC2 SEH
// unhandled-exception filter. Collects a minidump, stack trace, profile.json,
// and the last ~64KB of instrumentation lines into
// <exe_dir>/crashes/crash_YYYY-MM-DD_HHMMSS/ then shows a modal dialog.
//
// Usage:
//   crashbundle_init();                 // once, near the top of main()
//   crashbundle_append("[TAG] foo");    // every instrumentation site
//   crashbundle_trigger_test_crash();   // dev hotkey: deliberate null deref
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Installs the SEH unhandled-exception filter and captures env-flag state.
// Safe to call once; subsequent calls are no-ops.
void crashbundle_init(void);

// Appends a single instrumentation line to the in-memory ring buffer.
// Called from normal program flow (not inside the crash handler).
// Thread-safe via InterlockedExchange-style update; trailing newline added.
void crashbundle_append(const char* line);

// Deliberate null-deref crash for smoke-testing the bundle path.
// Write-access-violation at 0x0; SEH filter will fire.
void crashbundle_trigger_test_crash(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <cstdio>
#include <cstring>
#include <format>
#include <string_view>
#include <utility>
#include "mc2fmt.h"

// Fatal-path bundle: appends `what` to the ring, then writes crash.txt, last_trace.txt and a minidump (stack = caller). The caller then aborts.
void crashbundle_fatal(std::string_view what);

// std::format line -> stdout + crash-bundle ring. Cap is the line buffer size (output truncated to Cap-1 bytes); prefix is
// prepended inside that cap (use "" for none).
template <size_t Cap, class... Args>
void crashbundle_printf(std::string_view prefix, std::format_string<Args...> fmt, Args&&... args)
{
    char buf[Cap];
    const size_t plen = prefix.size();
    std::memcpy(buf, prefix.data(), plen);
    mc2::fmt::to_buf_n(buf + plen, Cap - plen, fmt, std::forward<Args>(args)...);
    std::puts(buf);
    std::fflush(stdout);
    crashbundle_append(buf);
}
#endif
