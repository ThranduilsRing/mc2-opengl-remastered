// gos_visual_capture.h - Slice S9 v1: in-engine visual capture primitive +
// bookmark replay. Owner doc: docs/superpowers/strategy/
// visual-regression-lab-architecture.md (S9 = its S1 + S2 ONLY).
//
// Two env-gated, default-OFF features that ride the existing MC2_SMOKE_MODE
// harness (no new driver / socket / IPC):
//
//   1) Capture primitive  (MC2_VISUAL_CAPTURE_FRAME + MC2_VISUAL_CAPTURE_DIR)
//      At the requested frame, in smoke mode, write a deterministic PNG of the
//      finished scene FBO plus a JSON sidecar holding the capture tuple
//      (build identity, mission, frame, seed, preset, dims, gate-env set).
//
//   2) Bookmark replay    (MC2_VISUAL_BOOKMARK_CAPTURE=<bookmarks.json>)
//      Hard-teleport the camera to each bookmark (no input smoothing), settle
//      N frames, capture PNG + sidecar, advance, then restore prior camera.
//
// All entry points are no-ops (single cached int / pointer check, no
// allocation) when their env vars are unset. They MUST be called only from the
// existing post-render screenshot site in gameosmain.cpp; they never touch
// render passes, swap logic, or smoke gate verdicts.
#pragma once

namespace gos { namespace visual_capture {

// Called once per frame from the post-render screenshot site, just before
// swap, with the read framebuffer already bound to the finished scene FBO and
// its dimensions. `frame` is the canonical g_mc2FrameCounter value.
//
// Returns immediately (no allocation) when neither feature env is set.
// When MC2_VISUAL_CAPTURE_FRAME matches, writes one PNG + sidecar then latches.
// When MC2_VISUAL_BOOKMARK_CAPTURE is set, drives the teleport/settle/capture
// sweep across the bookmark file, capturing one PNG + sidecar per bookmark,
// and restores the camera to its pre-sweep pose when finished.
//
// sceneW/sceneH are the FBO dimensions to read. The caller is responsible for
// having bound GL_READ_FRAMEBUFFER + glReadBuffer; this routine does not alter
// any render or GL pass state beyond its own glReadPixels.
void onPostRenderFrame(unsigned int frame, int sceneW, int sceneH);

// Cheap default-OFF gate. Returns true only if MC2_VISUAL_CAPTURE_FRAME (with
// _DIR) or MC2_VISUAL_BOOKMARK_CAPTURE is set and the corresponding capture is
// not yet complete. Env is parsed once and cached; subsequent calls are a
// couple of int/pointer comparisons with no allocation. Callers use this to
// avoid resolving the scene FBO at all on the unset hot path.
bool active();

// VISUAL-CAPTURE-CLEAN-1: MC2_VISUAL_CAPTURE_CLEAN=1 (explicit, default OFF) makes
// GL and VK captures of one bookmark comparable: the HUD/UI batch flush and the
// movie-mode letterbox/fade quads are suppressed on BOTH backends. Without it GL
// reads the scene FBO (no HUD, but WITH in-scene letterbox bars) while VK reads
// the presented image (HUD included, letterbox not drawn). Env cached once.
bool cleanFrame();

// CAMERA-PARK-1 (ported from vk branch 4b69ef78): MC2_CAMERA_PARK=
// "x,y,rot,proj,alt[@frame]" hard-sets the live camera pose once at
// frame >= @frame (default 300) and pins it against scripted writers
// (freezeScriptedCameraForCapture) WITHOUT freezing the sim. Called once per
// frame from gameosmain.cpp. Default OFF = one static pointer check.
void cameraParkTick(unsigned int frame);

// VK-VISUAL-CAPTURE-1: Vulkan-backend readback seam. Under
// mc2::renderBackend()==Vulkan the GL scene FBO is not what is shown, so
// capture_to() does not glReadPixels; it queues a one-shot request here
// (sidecar written immediately, PNG deferred). gameosmain.cpp forces the VK
// present readback while vkCapturePending(), and engine_present_vk.cpp hands
// the presented RGBA8 rows (bottom-up, shared-image orientation, w*h*4 bytes)
// to vkCaptureDeliver() in the same frame, which flips them and writes the PNG.
bool vkCapturePending();
void vkCaptureDeliver(unsigned int w, unsigned int h, const unsigned char* rgbaBottomUp);

}}  // namespace gos::visual_capture
