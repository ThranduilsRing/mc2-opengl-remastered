# Lane 2 session 36 cleanup review (2026-10-16)

Range `3582651b6..b951f2ca5` on `claude/lane2-s3` (worktree `A:/Games/mc2-lane2`). Read-only review: no builds, no launches.

## Verdict

**PASS with should-fix.** The step-2 deletions are clean and the scripted-edit incident left no trace in `gos_dev_shell.cpp`. One real gap in the CancelUnlessSubmitted coverage (shadow island), plus leftover readers and gate rows for deleted producers.

## Must-fix

None.

## Should-fix

1. **Shadow island: one early return is not guarded.** `vulkan_shadow_island_live.cpp`: `begin_frame()` (~:1057) calls `vk_render_pass_timer::Begin(Pass_ShadowDyn)`, but the guard is only created in `mc2_vk_shadow_island_submit_frame()` (~:1413), *after* `if (s->passCount == 0) return;`. When every pass declines, a bracket stays open and is never cancelled. The same happens when a recording is dropped by the next `begin_frame`'s `vkResetCommandBuffer` without ever reaching submit. Fix: call `Cancel(Pass_ShadowDyn)` on the `passCount == 0` return, or create the guard before it. The other five islands (color-prop, mech, terrain, vfx, water) and the four native post passes (fog, SSAO, screen-shadow-static, screenfx) cover every non-submit return between Begin and submit. Note that only six islands open a timer bracket, not seven. The water guard is created before `vkBeginCommandBuffer`/`Begin`, so a failed begin cancels a bracket that was never opened. That is harmless as long as `ring.cancel` treats a pass that is not open as a no-op; check this.
2. **The `[GL_ERROR v1]` reader is now dead.** Nothing emits that tag any more (only `data/defs/text/en_us/engine.fit` and `resource_aliases.fit` still carry the format strings). Even so, `scripts/smoke_lib/logparse.py:186,274` counts it, `scripts/smoke_lib/gates.py:72` buckets it, and `tests/smoke/test_gates.py`, `test_logparse.py` and `test_runner.py` assert on it. The smoke gate can never fire again. Delete it together with the two fit strings.
3. **Gates kept alive for deleted code.** `MC2_GL_ERROR_DRAIN_SILENT` is still a gate row (`Gates/gate_table.cpp:333`) and is still read by `gameosmain_init.cpp:160` (the INSTR banner) and `gos_crashbundle.cpp:259,682`. `MC2_SCREENSHOT_SOURCE` (`gate_table.cpp:799`, `flags.auto.yaml`) has no code reader at all. Retire both rows, which also helps gate amnesty.
4. **Screenshot callers still on the old path.** `scripts/mechbay_preview_harness.py:78` still sets `MC2_SCREENSHOT_SOURCE=backbuffer`. `scripts/check-background-launch.py:198,855` still describes and recommends it. `scripts/mech_shot.py:110` comments on "writeTGA stores the GL framebuffer". Update these to the VK-present semantics, and note that a GL run now produces no TGA.
5. **A stuck screenshot request blocks every later one.** `gos::screenshot::g_req` is cleared only by `deliver()`. If the VK present never reaches the verify readback (present failure, fallback, or a run that ends first), the request stays pending and every later request is rejected. Clearing or expiring it from the dev-shell/validate paths, or at mission load, would be enough.

## Nits

- On a GL run the dev-shell screenshot logs two lines: `[SCREENSHOT v1] request rejected` from `request()`, then `[DEV_SHELL] screenshot: failed ...`. The brief said one line. The dev-shell message also claims "no other capture pending", but `request()` checks only its own pending slot, not `visual_capture`.
- Stale `// drainGLErrors (Tier-1 instr §4)` include comments remain in `gameos_graphics_material_internal.h:61`, `gos_postprocess_internal.h:16`, `mclib/txmmgr_internal.h:79` and `gos_postprocess_executor.cpp:160`. The `gos_screenshot.h` include comments in `gameosmain.cpp:134` and `gameosmain_internal.h:41` still say "backbuffer->TGA". In `gos_visual_capture.cpp:49` the include is unused, according to its own comment.
- `gos_screenshot.h`'s WHY line says "the one frame both backends of the renderer end in". That is inaccurate now that a GL run rejects the request.
- `.claude/VK-PARITY-GAPS.md` line 1 is still headed "session 35". It still lists the step-2 prerequisites (VK timestamp source) and "row 18 if the owner rules port", although later text in the same line closes row 18 and section 17 records step 2(a) as done.
- `data/defs/debug/source_diagnostics.fit:949` still carries the `"gl_errors": [` key, which validate no longer writes.

## Checked-OK

- (1) `gos_dev_shell.cpp`: the net diff is only the intended changes. The `s_pendingShotBackbuffer` and `source=backbuffer` handling are gone, the framegraph JSON lost its GL-state fields, `scheduleScreenshot` lost its `backbuffer` argument, and `capturePendingScreenshot` is now a `request()` call plus `onShotDone`. Comparing the function lists before and after shows no loss and no reordering: 553 lines before, 529 after, and the only signature deltas are the expected ones.
- (2) Deleted fields and keys (`gl_ati_*`, `gl_nvx_*`, `adapter-minus-gl-free`, `fbo`/`viewport*`/`depthTest`/`depthMask`/`blend`/`cull` in frame pass stats, `QueryActive`, `RecordPassBegin`, `drainGLErrors`, `writeTGA`): there are no remaining code, Python or test readers. `vram.py`, `test_vram.py`, `test_perf_baseline_vram.py`, `EditorInspector.cpp`, `debug_state_dump.cpp` and `mc2_cmd.py` were updated in the same commits. `gameos_graphics_shadow.cpp` `.depthMask` and `gos_terrain_patch_stream.cpp` `.depthTest` are unrelated local structs. The remaining gaps are should-fix items 2 to 4.
- (3) There is one owner, `gos::screenshot`, with one `Request` slot. The pixels are the present path's own readback mapping (`verifyReadbackContent`), which is unmapped after `deliver()`; the BGR conversion buffer is a local vector. The visual-capture delivery line is untouched; screenshot is a sibling call after it. `frameSwapWindow` forces the verify readback while `pending()`. A GL run is rejected with a log line and no crash, and the oracle's `s_ssDone` prevents a retry every frame. `g_req` is namespace-scope static-duration state (a POD-like struct with a `std::string`), which is acceptable as the single owner. It is not function-local static.
- (4) The guard is placed right after Begin in color-prop, mech, terrain, vfx, fog, SSAO, both screenfx passes, and shadow (submit side); water is placed before the record scope. `submitted()` follows each successful `vkQueueSubmit`. The vfx `groupsDrawn == 0` return now relies on the guard. The only gap is should-fix item 1.
- (5) The new comments state WHAT and WHY. There are no journal comments in the code diffs, and `VK-ISLAND-TRACY-1`-style tags are pre-existing.
- (6) Split self-consistency: at `806488128` there are no callers of `drainGLErrors`, `QueryActive`, `RecordPassBegin` or the deleted fields (grep at that sha shows only unrelated hits). `2dbb4965b` adds `SubmitGuard` and `CancelUnlessSubmitted` and uses them only in the same commit. At `0c31da555` every `scheduleScreenshot`, `validateWriteResults` and `onFrameTick` caller matches the new signatures, and `writeTGA` has no remaining callers. Each commit is buildable in principle.
- (7) Inventory section 17, row 18 in VK-PARITY-GAPS ("CLOSED 2026-10-16 owner ruling") and the lane2-state ruling line all agree. The only drift is the line-1 header nit.
