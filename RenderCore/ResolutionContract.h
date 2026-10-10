#pragma once
//===========================================================================//
// RenderCore/ResolutionContract.h — META-FIX design D runtime tier.
//
// MC2_RES_ASSERT gated resolution/aspect contract audit (issues #45 / #49 /
// HUD-RES-CLAMP merge-drop class). Static sibling:
// scripts/check-res-aspect-contract.py; smoke sibling:
// tests/smoke/matrices/resolution.json. Contract text:
// docs/critical_inline_rules.md ("Resolution two-space contract").
//
// Modes (MC2_RES_ASSERT):
//   unset/"0"  -> off  (default; tier1 byte-identical — one static bool test)
//   "1"/"log"  -> log once per (site) signature: [RES_ASSERT v1] stdout line
//   "fatal"    -> log the line, then abort() (nonzero exit = red smoke/matrix)
//
// Chokepoints (all default-OFF, static-getenv latch):
//   (i)   mclib/camera.cpp Camera::setOrthogonal — projection aspect ==
//         drawable aspect unless MC2_CAMERA_ASPECT_NATIVE=0 or inside the
//         mech-preview FBO (g_mechPreviewRenderDepth > 0).
//   (ii)  GameOS/gameos/gameosmain.cpp draw_screen — HUD clamp invariant
//         (clamp enabled => logical canvas exactly 800x600), drawable
//         validity/freshness (3-frame grace), #49 windowed-size oracle.
//   (iii) GameOS/gameos/gameos_graphics.cpp flushHUDBatch — HUD canvas box
//         inside drawable + forward-remap∘gos_HudInverseMousePoint identity.
//
// Header-only, no engine includes — safe from mclib and GameOS TUs.
//===========================================================================//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()

namespace mc2res {

enum Mode { kOff = 0, kLog = 1, kFatal = 2 };

inline Mode mode() {
    static const Mode s_mode = []() -> Mode {
        const std::string v = mc2::gate::str<"MC2_RES_ASSERT">();   // "" when unset or empty
        if (v.empty() || v == "0") return kOff;
        if (v == "fatal") return kFatal;
        return kLog;   // "1" / "log" / anything else truthy
    }();
    return s_mode;
}

inline bool armed() { return mode() != kOff; }

// Log-once-per-site dedup (log mode). Bounded: at most 16 distinct sites
// (there are 6 today); once full, unseen sites still log (never silent).
inline bool firstHit(const char* site) {
    static const char* s_seen[16] = {};
    static int s_count = 0;
    for (int i = 0; i < s_count; ++i)
        if (::strcmp(s_seen[i], site) == 0)
            return false;
    if (s_count < 16)
        s_seen[s_count++] = site;
    return true;
}

// Contract violation. Emits one machine-parseable stdout marker line
//   [RES_ASSERT v1] event=violation site=<site> <detail>
// (once per site in log mode, every hit in fatal mode) and abort()s under
// fatal. `site` must be a string literal (stored for dedup).
inline void violation(const char* site, const char* detail) {
    const Mode m = mode();
    if (m == kOff) return;
    if (m == kFatal || firstHit(site)) {
        ::printf("[RES_ASSERT v1] event=violation site=%s %s\n", site, detail);
        ::fflush(stdout);
    }
    if (m == kFatal) {
        ::fprintf(stderr, "[RES_ASSERT v1] event=fatal site=%s %s\n", site, detail);
        ::fflush(stderr);
        ::abort();
    }
}

// Non-fatal advisory note (e.g. #49 oracle downgrade when the desktop is
// smaller than the requested window). Never aborts; once per site.
inline void note(const char* site, const char* detail) {
    if (mode() == kOff) return;
    if (firstHit(site)) {
        ::printf("[RES_ASSERT v1] event=note site=%s %s\n", site, detail);
        ::fflush(stdout);
    }
}

} // namespace mc2res
