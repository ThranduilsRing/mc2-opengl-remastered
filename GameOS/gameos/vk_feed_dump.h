// vk_feed_dump.h — VK-OFFLINE-PROOFS-1 FEED-GOLDEN HARNESS.
// -----------------------------------------------------------------------------
// Tiny env-gated, header-only appender. When MC2_VK_FEED_DUMP=<path> is set,
// each VK-island feed producer (terrain/prop/mech) appends ONE JSON line for
// its feed to that path (latched to fire once per process per island). To be
// run-to-run DETERMINISTIC, pair MC2_VK_FEED_DUMP with the EXISTING bookmark/
// sim-freeze capture machinery (MC2_VISUAL_BOOKMARK_CAPTURE +
// MC2_VISUAL_CAPTURE_FRAME, see gos_visual_capture.cpp) — readyToDump() below
// gates the actual write on mc2CaptureGaitFreezeActive() (the sim-frozen
// predicate the deterministic PNG sweep already relies on) whenever a bookmark
// is configured. WITHOUT a bookmark, the dump fires on the first successful
// feed with a loud one-time stderr warning that it is NOT guaranteed
// frame-identical across runs (mechs/camera/streaming may still be settling).
// No engine behavior changes when MC2_VK_FEED_DUMP is unset — this header is
// pure read + fprintf, no state mutation outside its own latch.
//
// File format: newline-delimited JSON, one object per island
//   {"island":"terrain","patchCount":N,"mapSide":S,"halfMap":H,"heightVersion":V,
//    "cementVersion":V,"lightingEnabled":0/1, ..., "patches":[{"qcX":..,"qcY":..,
//    "lodStep":..,"skirtDepth":..,"skirtEdgeMask":..}, ...]}
//   {"island":"prop","count":N,"objectDepthBias":F,"instances":[{"typeID":..,
//    "posX":..,"posY":..,"posZ":..}, ...]}
//   {"island":"mech","instanceCount":N,"boneCount":N,"runCount":N,"instances":[...]}
//
// tools/vk_feed_diff.py consumes this format.
#ifndef MC2_VK_FEED_DUMP_H
#define MC2_VK_FEED_DUMP_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Gates/gate_cold.h"

// sim-frozen-for-sweep predicate (VK-MECH-CAPTURE-DETERMINISM-1): declared in
// gos_visual_capture.h (single shared declaration, extern "C").
#include "gos_visual_capture.h"

namespace mc2vkfeeddump {

inline const char* dumpPath() {
    static const std::string p = mc2::gate::str<"MC2_VK_FEED_DUMP">();   // "" when unset or empty
    return p.empty() ? nullptr : p.c_str();
}

inline bool enabled() { return dumpPath() != nullptr && dumpPath()[0] != '\0'; }

// Determinism gate: when a bookmark/capture sweep is configured
// (MC2_VISUAL_BOOKMARK_CAPTURE or MC2_VISUAL_CAPTURE_FRAME), only dump once the
// sim is actually frozen for the sweep (mc2CaptureGaitFreezeActive(), defined in
// gos_visual_capture.cpp) -- this is the SAME freeze the deterministic bookmark
// PNG capture relies on, so a golden dump taken under a bookmark is bit-stable
// run-to-run. When NO bookmark is configured, this returns true immediately
// (fires on first successful feed) and prints ONE loud warning that the dump is
// NOT guaranteed deterministic across runs -- callers doing a real golden-vs-
// current comparison MUST pair MC2_VK_FEED_DUMP with a bookmark capture.
inline bool readyToDump() {
    const bool bookmarkConfigured =
        (std::getenv("MC2_VISUAL_BOOKMARK_CAPTURE") != nullptr) ||
        (std::getenv("MC2_VISUAL_CAPTURE_FRAME") != nullptr);
    if (!bookmarkConfigured) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                "[VK_FEED_DUMP] WARNING: MC2_VK_FEED_DUMP set without a bookmark "
                "capture (MC2_VISUAL_BOOKMARK_CAPTURE/MC2_VISUAL_CAPTURE_FRAME) -- "
                "dumping on first successful feed, NOT guaranteed frame-identical "
                "across runs. Pair with a bookmark for a real golden comparison.\n");
        }
        return true;
    }
    return mc2CaptureGaitFreezeActive() != 0;
}

// Appends to the dump file (creates on first open this process, truncating any
// stale file from a prior run so re-runs don't accumulate). One static flag per
// TU that includes this header + calls resetOnce() guards the truncate.
inline FILE* openAppend(bool truncateFirst) {
    const char* p = dumpPath();
    if (!p) return nullptr;
    FILE* f = std::fopen(p, truncateFirst ? "w" : "a");
    return f;
}

// Call once at process/dump start (e.g. from the terrain producer, which is
// the first island to fire each frame) to truncate any stale golden file.
inline void resetOnce() {
    if (!enabled()) return;
    static bool done = false;
    if (done) return;
    done = true;
    if (FILE* f = openAppend(/*truncateFirst=*/true)) std::fclose(f);
}

// Latches so each island only ever appends ONE record per process (the fixed
// deterministic bookmark frame is expected to be the only frame that matters;
// subsequent frames after the bookmark settle are not re-dumped).
inline bool& latch(const char* island) {
    static bool terrainLatch = false;
    static bool propLatch = false;
    static bool mechLatch = false;
    if (std::strcmp(island, "terrain") == 0) return terrainLatch;
    if (std::strcmp(island, "prop") == 0) return propLatch;
    return mechLatch;
}

} // namespace mc2vkfeeddump

#endif // MC2_VK_FEED_DUMP_H
