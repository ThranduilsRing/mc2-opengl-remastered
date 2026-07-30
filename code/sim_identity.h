#pragma once

// SIM-IDENTITY-HASH-1 -- prototypes for the world-state hash collector.
//
// Deliberately dependency-free (no engine headers, no STL) so the GameOS
// translation unit gos_visual_capture.cpp can include it directly. The
// implementation in code/sim_identity.cpp is the one that pulls in objmgr.h /
// mover.h / appear.h. Same layering reason as the mc2VisualCaptureSetPaused
// shim: GameOS cannot see the game's quote-include paths.

#ifdef __cplusplus
extern "C" {
#endif

// Emit the "sim_identity" JSON object body -- key + value, NO enclosing braces
// and NO trailing comma -- into `buf` (at most cap-1 chars + NUL).
//
// Returns the number of chars written, or 0 when the hash is disabled, no
// ObjectManager exists, there are no live movers, or the buffer was too small.
// On ANY zero return `buf` is left as an empty string and the caller must emit
// nothing at all: an ABSENT block is honest ("no sim identity recorded"),
// whereas a truncated one would hash a partial population and read as valid.
int mc2SimIdentityWriteJson(char* buf, int cap,
                            double scenarioTimeNow, long simFrame);

// Emit one "[SIMID v1] ..." line per sim frame to stdout. No-op unless
// MC2_SIM_IDENTITY_TRACE is on (default OFF). Call once per sim frame from the
// same place scenarioTime advances.
void mc2SimIdentityTraceFrame(double scenarioTimeNow, long simFrame);

#ifdef __cplusplus
}
#endif
