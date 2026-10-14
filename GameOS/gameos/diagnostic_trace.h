#pragma once
#include <cstdint>
#include <string_view>

// Diagnostic event JSONL writer.
//
// Controlled by:
//   MC2_DIAGNOSTIC_TRACE_FILE  -- output path (default: debug_state/diagnostic_trace.jsonl)
//   MC2_DIAG_TAGS              -- comma-sep tag whitelist, or "*" (all), or "none" (disable)
//                                 default: high-value whitelist (GPU_CULL,LIGHTBAKE_PROOF,etc.)
//
// All writes are mutex-protected (thread-safe).
// File is opened in append mode at init(). Rotated to .prev.jsonl if >10MB.
// Line-buffered: each event is flushed immediately for crash-survival.
// Crash handlers should call flush() then write to stderr as usual.

namespace mc2_diag {

// Call once at engine startup with the session_id and pid from debug_state_dump.
// Parses env vars, opens file, rotates if needed.
// Safe to call multiple times (idempotent after first call).
void init(const char* sessionId, int pid);

// Write one diagnostic event. Returns false if JSONL disabled or tag not in whitelist.
// tag: registered tag name (e.g. "GPU_CULL", "ANIM_GATE")
// version: schema version integer for this tag's data block
// frame: current frame index (pass 0 if not available)
// dataJson: raw JSON object string, e.g. "{\"count\":5,\"ms\":1.2}"
//           Must be a valid JSON object (starts with {, ends with }).
bool writeEvent(const char* tag, int version, uint64_t frame, const char* dataJson);

// Flush pending writes. Safe to call from crash/signal handler (no allocation).
void flush();

// Flush and close the file. Call during orderly engine shutdown after the last
// diagnostic events are emitted. Safe to call if init() was never called or
// if output is disabled.
void shutdown();

// Is JSONL output enabled and initialized?
bool enabled();

// Was the tag asked for in MC2_DIAG_TAGS? Needs no init() and survives shutdown(), for lines printed during engine teardown.
bool tagRequested(std::string_view tag);

// Is the given tag in the current whitelist?
bool tagEnabled(const char* tag);

} // namespace mc2_diag
