# Lane 2 session 37 cleanup review (b951f2ca5..05f081b35)

## Verdict
ACCEPT with should-fixes. No must-fix. Read-only review: no build, no launch, diffs and grep only.

## Must-fix
None.

## Should-fix
- S1 debug lines lag one frame, undocumented. `GuiRuntime::NewFrame()` runs at the top of the frame (gameosmain_frame.cpp:28), before GameLogic and render. `flushWorldPrims` (gamecam.cpp:1036) freezes the segments mid-frame, so they draw at the next frame's NewFrame with the matrix frozen with them. The matrix is consistent with the segments (frozen together), so there is no skew, but the overlay shows the previous frame's scene. Acceptable; say so in the `drawFrozenPrimsToOverlay` comment and inventory section 18.
- S2 `screenshot::tick()` has no doctest and sits inside the Vulkan present block (gameosmain_frame.cpp:724). A run whose present branch is never entered (fallback) never ticks, so the request is never expired there. Add a small test of the 300-frame expiry (one line, request cleared, done(false) called) and tick from a path every presented frame reaches.
- S3 `tests/unit/printf_corpus.jsonl` and `printf_corpus_gen.inc` still carry the `[INSTR v1]` banner with `gl_error_print`. Confirm they are regenerated or intentionally frozen.
- S4 Commit 05f081b35's body is only the subject. The dup-lines rewrite reason (55319 -> 55274, the nine-copy block being spv_dir) is in lane2-state and the inventory, not in the commit body. Amend the convention for next time.
- S5 Static-duration owners in debug_renderer.cpp: file-scope `std::vector s_verts, s_frozen`. They existed before (s_verts) and are never freed by a shutdown path; low risk, but "no static-duration owner" is not met.
- S6 `fenceWait` early return: a failed or timed-out vkWaitForFences (5 s) returns false without a sample, so the worst hitch is not recorded. Consider recording it before the return.

## Nits
- engine.fit / resource_aliases.fit still hold the `"MC2_GL_ERROR_DRAIN_SILENT"` format text (generated tables; the lane notes say so for gl_errors, not for this name).
- Debug lines are 1 px, no depth test; stated in the header.

## Checked-OK
1. Fence call sites: the old macros only bumped counters around glFenceSync/glClientWaitSync (the value-returning ones returned the call result). Each site now calls the same function with the same arguments (diff compared line by line: 8 sites, args identical). No error check lost. No remaining reader of MC2_GL_* sync macros or the glWaitSync/glFence counters (git grep clean outside archived .claude scratch files).
2. spvDir(): one definition (vk_island_util.h). All 13 deleted bodies (present, color_prop, legacy_shape, mech, fog, hzb, screenfx, ssao, shadow, sky, terrain, vfx, water) were byte-identical except one comment (terrain). No island behaves differently. terrain/water keep an internal-header declaration of spv_dir() that now forwards.
3. Hitch trace: RecordFenceWait is called after the real vkWaitForFences returns success, with the measured microseconds / 1000. Rejects negative or non-finite samples with a log line. Per-frame accum is reset with the struct. Doctest asserts accumulation, rejection and the exact `[HITCH_VK] ...` string.
4. debug_renderer: frozen together with `gos_GetTerrainMVPMat4()` of that frame; ImGui foreground draw list is the correct top layer; projection is doctested (near cut, non-finite). First-draw and all-culled logs once each. Behavior notes in the lane text (overlay only with MC2_VK_IMGUI=1, no depth test) match the code and header.
5. Screenshot expiry: 300 frames, one `[SCREENSHOT v1]` line, g_req reset before the callback, done(false).
6. Gate cascade: no reader of MC2_SCREENSHOT_SOURCE left; MC2_GL_ERROR_DRAIN_SILENT remains only in runner.py POPPED_ENV (deliberate, documented as a coordinator decision), test_anti_churn.py (asserts it is popped / absent) and generated .fit text.
7. Comments are WHAT+WHY; no journal comments seen in the touched hunks.
8. Inventory section 18 and lane2-state agree on commits, thirteen copies, 55319 -> 55274, and the coordinator note. The "Open" line in the s36 section of lane2-state is historical and now superseded by section 18.
