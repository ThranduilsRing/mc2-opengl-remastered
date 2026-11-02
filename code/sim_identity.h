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

// MC2_CAPTURE_FOCUS: position of the nth live player-team mech (capture/dev only).
// Returns 1 and fills xyz[3], or 0 when there is none.
int mc2CaptureFocusMech(int n, float* xyz);
// Walk variant: nth live mech on team selector sel (0 = player team, 1 = every other team,
// 2 = any team), in object-manager order. Fills xyz[3], *id (part id), *team and name (>= 64
// chars); *total gets the number of live mechs matching sel.
// *contact: 0 = player team, 1 = enemy at CONTACT_VISUAL, 2 = enemy fogged (not drawn by the
// stock gate), 3 = enemy fogged but drawn because ShowMovers is set. Returns 1 or 0.
int mc2CaptureFocusMechEx(int n, int sel, float* xyz, int* id, int* team, char* name, int* total, int* contact);
// MC2_CAPTURE_FOCUS walk only: set the stock ShowMovers switch (the -show movers equivalent) so
// fogged enemies are drawn. Tool-only; never called by interactive play or the smoke runner.
void mc2CaptureFocusSetShowMovers(int on);

#ifdef __cplusplus
}
#endif
