// gos_visual_diff.cpp - Stage 2.E natural-mission-camera capture harness.
//
// Strips away the camera-teleport approach (Steps 1.2-1.5 architecture) in
// favor of capturing the engine's natural mission camera at a chosen frame.
// Rationale: each mission's intro pan + command-position settle already
// produces a repeatable, useful vantage on its own. Trying to teleport the
// camera and clear goal-tracking state ran into intro-pan / mission-script
// camera-driver fights that the goal-clearing block alone couldn't address.
//
// The engine's job here is now minimal: latch on the first frame where
// gos_terrain_indirect::WasEverFrameSolidArmed() returns true (the
// sticky-once cousin of IsFrameSolidArmed(); see header for why we need
// the sticky variant), count frames after that, capture at frame N, exit.
// MC2_VISUAL_DIFF_FRAME_N is then frames-since-intro-complete, so small
// values (60-90) suffice -- the intro-pan wall-clock jitter is upstream of
// the latch and cancels across runs. Per-mission frameN tuning lives in the
// Python harness.
#include "gos_visual_diff.h"
#include "gos_screenshot.h"
#include "gos_smoke.h"
#include "gos_terrain_indirect.h"

#include <cstdio>
#include <cstdlib>
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "mc2fmt.h"

namespace VisualDiff {

namespace {

// Lazy-cached env reads.
bool s_captureRead = false;
bool s_capture     = false;

enum class Phase { Waiting, Done };

struct State {
    int   localFrame            = 0;
    int   introCompleteFrame    = -1;
    bool  introCompleteObserved = false;
    Phase phase                 = Phase::Waiting;
    bool  paramsResolved        = false;
    int   frameN                = 90;
    int   maxFrames             = 150;
};
State s_state;

// The presented frame was read back and written (or the write failed).
void onCaptureDone(const char* path, bool ok, int w, int h) {
    if (ok) {
        const std::string mission = mc2::gate::str<"MC2_VISUAL_DIFF_MISSION">();
        mc2::fmt::print_err("[VISUAL_DIFF v1] event=capture_done out={} w={:d} h={:d} mission={}\n", mc2::fmt::safe(path), mc2::fmt::as_i32(w), mc2::fmt::as_i32(h), mc2::fmt::safe(mc2::gate::isSet<"MC2_VISUAL_DIFF_MISSION">() ? mission.c_str() : "(unset)"));
    } else {
        mc2::fmt::print_err("[VISUAL_DIFF v1] event=capture_failed reason=write_tga out={}\n", mc2::fmt::safe(path));
    }
    fflush(stderr);
}

void doCapture() {
    const std::string outPathText = mc2::gate::str<"MC2_VISUAL_DIFF_OUT">();   // "" when unset or empty
    const char* outPath = outPathText.c_str();
    if (!outPath[0]) {
        mc2::fmt::print_err("[VISUAL_DIFF v1] event=capture_skipped reason=no_out_path\n");
        fflush(stderr);
        return;
    }
    if (!gos::screenshot::request(outPath, onCaptureDone)) {
        mc2::fmt::print_err("[VISUAL_DIFF v1] event=capture_failed reason=request_rejected out={}\n", mc2::fmt::safe(outPath));
        fflush(stderr);
    }
}

}  // anonymous namespace

bool isCaptureEnabled() {
    if (!s_captureRead) {
        s_capture = mc2::gate::get<"MC2_VISUAL_DIFF_CAPTURE">();
        s_captureRead = true;
    }
    return s_capture;
}

void onMissionLoad() {
    s_state = State{};
}

void onFrameTick() {
    if (!isCaptureEnabled()) return;
    if (s_state.phase == Phase::Done) return;

    s_state.localFrame++;

    // Resolve frameN + maxFrames from env on first tick. No JSON parsing,
    // no per-mission file lookup — Python harness sets these per launch.
    if (!s_state.paramsResolved) {
        s_state.frameN    = mc2::gate::get<"MC2_VISUAL_DIFF_FRAME_N">();   // row default 90
        s_state.maxFrames = mc2::gate::isNonEmpty<"MC2_VISUAL_DIFF_MAX_FRAMES">()
                              ? static_cast<int>(mc2::gate::get<"MC2_VISUAL_DIFF_MAX_FRAMES">()) : s_state.frameN + 60;
        s_state.paramsResolved = true;
        const std::string mission = mc2::gate::str<"MC2_VISUAL_DIFF_MISSION">();
        mc2::fmt::print_err("[VISUAL_DIFF v1] event=capture_armed mission={} frameN={:d} maxFrames={:d}\n", mc2::fmt::safe(mc2::gate::isSet<"MC2_VISUAL_DIFF_MISSION">() ? mission.c_str() : "(unset)"), mc2::fmt::as_i32(s_state.frameN), mc2::fmt::as_i32(s_state.maxFrames));
        fflush(stderr);
    }

    // Snapshot intro-complete frame on first tick after the engine's own
    // "intro pan complete" signal: WasEverFrameSolidArmed() is the sticky
    // cousin of IsFrameSolidArmed() and is set on the first frame that
    // ComputePreflight() arms. We must use the sticky variant here because
    // gosRenderer::endFrame() resets the per-frame s_frameSolidArmed flag
    // BEFORE the VisualDiff post-PP hook fires, so the per-frame getter
    // always reads false from here. Latching on the sticky makes frameN
    // deterministic: intro-pan wall-clock jitter is upstream of the latch
    // and cancels across runs.
    if (!s_state.introCompleteObserved) {
        if (gos_terrain_indirect::WasEverFrameSolidArmed()) {
            s_state.introCompleteObserved = true;
            s_state.introCompleteFrame    = s_state.localFrame;
            mc2::fmt::print_err("[VISUAL_DIFF v1] event=intro_pan_complete_observed local_frame={:d}\n", mc2::fmt::as_i32(s_state.localFrame));
            fflush(stderr);
        }
        return;
    }

    int framesSinceStart = s_state.localFrame - s_state.introCompleteFrame;

    // Timeout — exit cleanly with code 4 so the Python harness can distinguish.
    if (framesSinceStart > s_state.maxFrames) {
        mc2::fmt::print_err("[VISUAL_DIFF v1] event=capture_timeout frames_since_start={:d} max_frames={:d}\n", mc2::fmt::as_i32(framesSinceStart), mc2::fmt::as_i32(s_state.maxFrames));
        fflush(stderr);
        fflush(stdout);
        exit(4);
    }

    // Capture at frameN.
    if (framesSinceStart >= s_state.frameN) {
        doCapture();
        s_state.phase = Phase::Done;
    }
}

}  // namespace VisualDiff
