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
