// gos_visual_diff.h - Stage 2.E natural-mission-camera capture harness.
//
// The engine waits for gos_terrain_indirect::WasEverFrameSolidArmed() to
// go true (intro pan complete; the engine's own gate per terrain.cpp -- a
// sticky-once cousin of IsFrameSolidArmed() needed because the per-frame
// arm is cleared by gosRenderer::endFrame() before this hook fires), counts
// frames from there, captures the pre-HUD framebuffer at
// MC2_VISUAL_DIFF_FRAME_N, and exits. No camera teleport, no pose
// authoring, no goal-clearing, no camera APIs. MC2_VISUAL_DIFF_FRAME_N is
// "frames AFTER intro-pan-complete"; intro-pan wall-clock jitter is
// upstream of the latch and cancels across runs.
//
// Per-mission frameN values live in the Python harness (Phase 1 Step 1.7),
// not in the engine.
#pragma once

namespace VisualDiff {

// True iff MC2_VISUAL_DIFF_CAPTURE is set in the environment.
// Reads env once on first call; cached.
bool isCaptureEnabled();

// Per-frame hook. Wired into the gameosmain.cpp render loop at the pre-HUD
// seam (between pp->endScene() and projectz_overlay_render). When
// isCaptureEnabled() is false, returns immediately.
//
// When enabled:
//   - Owns its own localFrame counter (independent of SmokeMode::g_frameCount)
//   - Snapshots its own "intro complete frame" on the first tick after
//     gos_terrain_indirect::WasEverFrameSolidArmed() returns true
//   - At framesSinceStart == MC2_VISUAL_DIFF_FRAME_N (default 90):
//     requests a TGA of the presented frame via gos::screenshot::request to MC2_VISUAL_DIFF_OUT
//   - At framesSinceStart > MC2_VISUAL_DIFF_MAX_FRAMES (default frameN+60):
//     logs capture_timeout and exits process with code 4
void onFrameTick();

// Mission-load reset hook (currently uncalled from external code; reserved
// for future engine-side wiring if in-process restart support is needed).
void onMissionLoad();

}  // namespace VisualDiff
