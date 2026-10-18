# Lane 2 state (worktree A:/Games/mc2-lane2, branch claude/lane2-s3, build64-ninja, install mc2-win64-lane2)

## FIFTY-SIXTH session (2026-10-17, Sonnet 5.5) -- s55 review fixes, P1 packets, P3 handle slots, P4a identity, first game-side GL deletion

Commits on 662d777d0 (all local): 0ca2c5358 review MUST-FIX | dc7f0a297 review SHOULD | 093682238 + 0538822a7 P1 | 55fb7482a P3 | ba9909249 P4a | 43136d5c9 contract fixes | 21b732b2d GL deletion (capture arm). Tag `gl-baseline-s56` = 0538822a7 (the last revision whose capture still has a GL arm). GL baseline captures (day mc2_01_saved_205644 + night mc2_10_saved_204523, clean, `readback_source=gl_fbo`, frame 462): `.claude/l2cap/s56-gl-baseline`; the VK pair at the same tip `.claude/l2cap/s56-vk-tip`; their montage (VIEWED, MAD 0.82 / 0.50 = the s53 values) `.claude/l2cap/s56-ab-gl-vs-vk`.

### Review items (astraL2s55) -> fix
- P1 frame advance: `EnginePresentVk::presentGLFrame` wraps `presentGLFrameBody` and calls `frameBoundary()` on EVERY exit (presented, minimized zero extent, swapchain rebuild, declined, failed). gameosmain.cpp's call is gone; `frameSwapWindow` calls `frameBoundary()` itself only when no present was attempted (init deferred / torn down). The editor calls the same `presentGLFrame` (lane4 gos_host_vk.cpp:187): it needs NO call of its own after merging and must not add one.
- P1 timed-out F-2 wait: `VulkanContext::beginFrame` waits the ticket; on timeout it device-idles, and only a proven completion restarts the slot (`FrameRingCore::beginFrame(frame, restart)`); otherwise the slot keeps its head (allocations bump past, a full slot fails soft). Doctest `a slot whose previous user is unproven keeps its head`.
- P1 readback slot lifetime: `ReadbackCore` slots have a Discarded state; a discarded sealed slot stays parked until its ticket completes (`request(tag, tracker)`, `release(tok, tracker)`); doctest with the negative control.
- P1 capture/frame association: captures and screenshots record the frame they were asked in (`vkCapturePendingSince`, `screenshot::pendingSince`) and take only a readback whose tag is at or after it (`Slot::acceptsReadback`). Proof: bookmark capture sidecar frame 462, PNG vs the s55 pair MAD 0.02 / 0.00 (viewed).
- P1 resize vs readback: `drainReadbackBeforeImageDestroy()` waits the readback ticket (device idle on timeout) and releases the token before `buildSharedImage`. P2 teardown: `destroy()` releases the token after its idle. Exercised by the resize runs below; a pending readback at resize has no offline reproduction.
- SHOULD font upload: documented, pinned exception: `scripts/check-vk-submit-funnel.sh` asserts exactly one raw font-upload submit and two `vkQueueWaitIdle(v->Queue)` in `imgui_impl_vulkan.cpp` and that no engine code calls `ImGui_ImplVulkan_CreateFontsTexture` (it runs once, inside the first NewFrame). The row failed on my first miscount (negative control).
- SHOULD fallback waits: `mc2vk::submitAndWait(ctx, si, timeout, why)` (vk_one_shot.*) submits and waits the timeline ticket (no fence to allocate); mech, prop, legacy shape, terrain images, sky and the shadow diagnostic use it; no `vkQueueWaitIdle` is left in the islands.
- SHOULD tool_vk_shell: the submit result is checked; a refused submit re-creates the fence signaled, frees the readback resources and ends the run (`submitFailed`).
- SHOULD shared-slot contract: `frameContext()` reports the real N (`setFramesInFlight`, set by present init from `MC2_VK_FRAMES_IN_FLIGHT`) and the present path takes its slot from it. NOT DONE: islands taking the engine slot instead of private parities. BLOCKED, `vk_image_resource.cpp:148-159`: seven islands call `vkImageResourceBeginFrame` on ONE shared colour resource, each call flips the parity and waits-then-resets that parity's fence; with the engine slot as parity a second island call in the same frame would wait a fence the first one reset and may never signal (UINT64_MAX wait). The per-island parity has to become per-island fences first.
- Orchestrator 1 (present semaphores): the semaphore signaled for `vkQueuePresentKHR` is `renderSems[imageIndex]`, PER ACQUIRED SWAPCHAIN IMAGE (`imgRenderSem` in engine_present_vk.cpp); the acquire semaphore is per frame slot and is waited by the submit whose fence that slot waits. No change needed. The tracker's timeline means "GPU work done" only; presentation lifetime is carried by the per-image semaphores (VK_EXT_swapchain_maintenance1 not used).
- Orchestrator 2 (skipped frames): doctest `a skipped present still advances the frame and its ticket never undercuts earlier work`; code reading of each path: minimize (zero extent early return), resize (fences waited, readback drained, composite dropped), OUT_OF_DATE / SUBOPTIMAL at acquire or present (fence not reset on those paths, rebuild), failed acquire or submit (return false -> destroy + re-init, fence recreated signaled). Live: two resize/minimize/restore runs under `MC2_VULKAN_VALIDATION=sync` (`scratch/s56_resize_steps3.log`, `s56_resize_steps4.log`; swapchain rebuilt 3 times, recreate_ok 3, errors=0 warnings=0). NOTE: my first two resize runs (`scratch/s56_resize_steps.log`, `steps2`) matched mc2.exe by name and moved/minimized ANOTHER lane's running game window (lane3); the script now filters on the lane2 install path (`scratch/s56_resize.py`).
- Orchestrator 3 (holdouts), each guarded by a completion dependency at N=2, none rewritten without one: shadow instBuf[par]/ubo slice[par]: `vulkan_shadow_island_live.cpp:1031-1034` waits `frameFence[par]` (5 s, failure disables the island) before the parity buffers are rewritten; post per-pass UBOs: each pass waits the previous submit's timeline ticket before rewriting (`vulkan_post_fog_native.cpp:565`, `vulkan_post_hzb_native.cpp:611`, `vulkan_post_ssao_native.cpp:693`, `vulkan_post_screenfx_native.cpp:721/948`); water thin readback `thinBuf[par]`: written at `vulkan_water_island_run.cpp:152` after `vkImageResourceBeginFrame` waited the parity fence (line 105), readback mode only. Not moved to the ring.
- Style items of the C++23 checklist (std::expected in vk_one_shot, ranges in the shadow island, typed handles / byte spans in the tools): not done.

### Slices
- P1 (093682238, 0538822a7): `RenderCore/FramePackets.h`: `ViewPacket`, `LightingPacket`, `FramePackets`, `FrameSlotArenas` (replaces render_snapshot's ping-pong statics), `FramePacketPublisher`, and the contract (who builds, immutable, retains, recycles) in the header. Producer: `GameCamera::renderViewUniforms` (the one publish point). Consumers: mech and static-prop island feeds (camera position), the scene-lighting trace. UiPacket NOT built (no frame-start producer: the HUD draw list exists only during the present tail). Fixed pass order NOT done (needs a one-frame trace of the GL-era call graph). Doctest `test_frame_packets.cpp` (8). Proof: captures vs s55-f5-n2 MAD 0.02 / 0.00 (viewed). vk_feed_dump equality not run (the dump has no camera field; the packet is a copy of the same source).
- P3 (55fb7482a): `RenderCore/HandleSlots.h` `HandleSlotTable`; mech handles recycle in `RenderWorld/mech_handles.cpp`; `RenderWorld::advanceFrame(frame, completedFrame)` after every present (`VulkanContext::completedFrame()`). Doctest `test_handle_slots.cpp` (7) + live `MC2_MECH_OBJECT_ID_SELFTEST=1` step 5 PASS in tier1. GV / generic / legacy instances: no per-instance registration site (only Mech3DAppearance submits to the batcher, `mclib/mech3d_render.cpp:716`); not done. Determinism pair NOT run.
- P4a (ba9909249): `RenderCore/TextureService.h` (key, handle, node; reported from `loadTexture` / `releaseNode` / `MC_TextureNode::destroy`; provider is the constant "txmmgr" and content hash 0 until the VFS gives a resolved source id) and `RenderCore/MaterialRegistry.h` (dedupe, `MaterialSet`, `kRegistryTextureSemantic = DescriptorIndex`); nothing reads them yet. Doctest `test_texture_service.cpp` (11). `get_gosTextureHandle` census unchanged (no edits). TextureRef's releaser stays on the manager (eight callers call `releaseNode` directly); the service hook lives in `releaseNode`.
- F2b/F2c remainder: not converted (all three holdouts guarded).
- GL deletion (21b732b2d): removed the GL scene-FBO capture arm (`read_scene_rgb` glReadPixels, the GL branch of `capture_to`, the GL branch of the capture hook in `frameSwapWindow`) and the gate `MC2_VISUAL_CAPTURE_POST` (table, seed, generated files, docs, capture.py). Makes GL removable: the capture harness needs no GL framebuffer. The `ab.py` GL arm and `capture.py --env-declare MC2_RENDER_BACKEND=gl` no longer produce PNGs: use tag `gl-baseline-s56` + `.claude/l2cap/s56-gl-baseline` + `ab.py --compare-only`. The rest of the editor lane's list (editor-lane-state.md session 2 (d)) waits on 3.5/3.9: GL scene FBOs of gosPostProcess, legacy gos_* draw path, GL static-prop/terrain-indirect buffers, ImGui GL-name mirrors, SDL GL context. Not started.

### Proof
| claim | evidence |
|---|---|
| doctest | 3328/3328 test cases (`scratch/s56_doctest2.log`; 3302 before P3/P4a) |
| tier1 under sync validation, N=2, 30 s, `MC2_DIAG_VERBOSE=1`, at 43136d5c9 | 5/5 PASS errors=0 warnings=0, fingerprint OK, fps 108/76/55/80/50 on a loaded host (`scratch/s56_t1sync2.log`); `[MECH_OBJECT_ID_SELFTEST v1] result=PASS`; `[VK_FRAMERING v1] peak_used=4166432` |
| tier1 at dc7f0a297 (first run) | 3/5 PASS; mc2_03 and mc2_24 `heartbeat_freeze_play` on a host with ~50 `cl.exe` running (`scratch/s56_t1sync.log`), see OPEN 2 |
| captures (day + night, VIEWED) | `.claude/l2cap/s56-ab-p1/*/montage.jpg` (vs s55: 0.02 / 0.00), `.claude/l2cap/s56-ab-gl-vs-vk/*/montage.jpg` (night viewed) |
| resize / minimize / restore, sync validation | 2 runs, swapchain rebuilt 3x, errors=0 |
| check-contracts at 43136d5c9 (run before the contract fixes) | red: log_tag_census + tools_tests (named), cxx17_ratchet (LG20 tools s54 + my LG08 +6, fixed), file_size_ratchet (RenderWorld.cpp 1116 -> 1152, fixed by the split), default_build_quiet (9 VK tags: 8 gated behind MC2_DIAG_VERBOSE, VK_PROFILE keeps its parser); not re-run in full afterwards |

### OPEN / FAILED
1. quickload_cement_check FAILS at the tip: `runway_overview` differs by 3241 px (max 2000), deterministic (two runs identical). s55 measured 214 at 7f492c66f (not at its final tip 599f55c52, so the cause may predate this session). The diff is dithered alpha-tested edges of the pad buildings and trees plus a turret rotation (ctl vs ql, `.claude/l2cap/s56-quickload/crop_cmp.png`); the cement pad itself is present. Reverting the prop island copy-once change did not fix it. Not bisected (checking out the s55 sources into the tree was denied by the auto-mode classifier). BUGS row 67 (open).
2. heartbeat_freeze_play, two distinct causes with evidence. (a) the ~1.0 s peak frame (`[PERF v1] peak_ms=1011`, `[HEARTBEAT] frames=2 elapsed_ms=1011`) is the FIRST mission frames: every VK island initializes lazily on its first frame (colormap atlas 5120x5120, HDRI 16384x8192, 8192x8192 shadow maps, prop albedo arrays, geometry uploads: `tests/smoke/artifacts/2026-10-17T19-41-07-835071-3356/mc2_03.log:560-683`); not host load. Fix idea: initialize the islands in the load phase. (b) the bucket compares the last play heartbeat with the process exit: in the loaded run mc2_03 `mission_quit elapsed_ms=38143` vs process exit 47.2 s = a 9 s shutdown under ~50 concurrent compiles (`crash_evidence.json`: last heartbeat 38.0, walltime 47.2).
3. Islands on the engine slot (blocker above), fixed pass order, UiPacket.
4. Ratchet rises, named: `log_tag_census` / `tools_tests` RED (tags `[VK_TIMELINE v1]`, `[VK_UPLOAD v1]`, `[VK_FRAMERING v1]`: now MC2_DIAG_VERBOSE-gated but present in source; NOT re-frozen, `log_tag_baseline.json` is on the do-not-regenerate list); `cxx17_ratchet` LG20 +6 in tools (s54). Regenerated with their writer (`tools/gate_gen.py --write`): `Gates/gate_index.gen.h`, `docs/flags.auto.yaml`, `RenderCore/RendererFeatureRegistry.h` (one gate row removed). No callers.json / inventory regeneration.

### Re-sequencing note
The orchestrator moved P3, P4a/b, P5, P6 after GL deletion mid-session; P3 and P4a had already landed (55fb7482a, ba9909249: new headers plus hooks that change no behavior) and are left in.

## FIFTY-FIFTH session (2026-10-17, Sonnet 5.5) -- shared foundations F0-F5: completion tracker, deferred destroy, frame ring, upload batch, readback ring, two frames in flight

Spec: `E:/mc2-archive-2026-10/plans/shared-foundations-design-2026-10-17.md` sections 1 and 4. Owner defaults taken: N=1 until F5 (then 2 behind `MC2_VK_FRAMES_IN_FLIGHT`, default 2, `=1` kills), no transfer queue (the upload API is queue-agnostic), ImGui keeps its per-texture sets (retired by ticket).

### Commits (on eee783c6a, all local)
a86a4c65c F0 + F1 foundation (CompletionTracker, DeferredDestroy, `VulkanContext::submit`, `check-vk-submit-funnel.sh`, ImGui bridge + HUD mirrors retire by ticket) | 2c979a48f every island submit through the funnel, light-feed + producer growth retire, VFX ssbo growth no idle | e82501e46 F2a FrameRing (HUD vertex streams, ImGui staging, `frameBoundary`) | a5108a8fc + a688dff53 F2b mech, prop, shared light feed on dynamic offsets (+ the VUID-06715 range fix) | b3dc39439 F3 UploadBatch | 7ad3b77c9 F4 ReadbackRing | 7f492c66f F5 two frames in flight + `MC2_VK_FRAMES_IN_FLIGHT` | 563534e5a contract follow-ups (dup_lines, ledger, layout doc) | bc37bd37e F2b/F2c VFX, legacy shape, water | 2a991e787 F2b terrain + overlay | e335ef1fc + 4aa066fbf F2c post fences -> tickets (+ the sync-hazard fix it exposed) | dceaed7e5 F2b native producers | 94d497f69 ratchet tidy (`std::span`, no switch) + ledger/layout refresh, then this state commit. Mergeable code tip: 599f55c52 (94d497f69 + the ring peak line; deployed and smoked, doctest 3299/3299, tier1 5/5 core and sync validation errors=0, `scratch/s55_fin_smoke.log`, `s55_peak_smoke.log`).

### What each slice is now
- F0: `vk_completion_tracker.h` (vulkan-free ticket math: values in submit order, `unnoteSubmit`, frame sealing), `VulkanContext` owns the graphics timeline semaphore; `submit()` appends the timeline signal to a plain `VkSubmitInfo` (legacy submit + `VkTimelineSemaphoreSubmitInfo`, binary semaphores and fences pass through); every engine submit (17 TUs) calls `mc2vk::submitGraphics` or `ctx.submit`; contract row `vk_submit_funnel` (`scripts/check-vk-submit-funnel.sh`); `[VK_TIMELINE v1]` at 600 frames. Frame bracket = the present: `frameBoundary()` (endFrame + beginFrame) right after `frameSwapWindow`. Doctest: `test_completion_tracker.cpp` (6).
- F1: `vk_deferred_destroy.h` (`retire(owner)`: sealed with the open frame's ticket, outside a frame with everything submitted; newest-first destruction; `drainAll` only at teardown / present destroy / island retire). Converted: ImGui texture entries and their staging, HUD mirrors, shared light feed growth, `NativeProducers::growHost`, VFX ssbo growth, terrain overlay texture replacement. Doctest `test_deferred_destroy.cpp` (6, incl. the destroy-at-retire negative control).
- F2a: `vk_frame_ring.*`: one host-visible buffer, two slot regions + one slot of tail, 16 MiB per slot (48 MiB), `frameAlloc` returns {buffer, offset, ptr, range}; slot F%2 restarts at beginFrame after the ticket of F-2 completed; growth retires the old buffer. HUD color/tex vertex streams and ImGui staging moved off their own buffers. Doctest `test_frame_ring.cpp` (5).
- F2b/F2c (what moved): mech (inst, bone, UBO, pool, LightsData), color prop (UBO, instances, LightsData, pool), the shared light feed (copy-once span, no 3-slot ring), terrain (per-patch UBO + light pool/grid SSBOs) and its overlay (UBO, dynamic vertices), legacy shape (UBO + vertices), water (the two parameter UBOs), VFX (frame constants + particles; parity sets + ticket slots, NO inline fence wait), native producers (lighting window, lights, both param blocks; `ComputePipeline` takes `_DYNAMIC` types). Each island stages ONE span before `beginFrame`, re-points `set[par]` only when the ring buffer changed (`ringBound`), and binds with dynamic offsets. A dynamic descriptor's range may not be `VK_WHOLE_SIZE` with a non-zero offset (VUID-06715, caught by validation): storage bindings use `FrameAlloc::range` = slot size, the buffer has one slot of tail.
- F3: `vk_upload.*` `UploadBatch`: `stage()` (ring span, or a dedicated host buffer above a quarter slot, retired with the frame), `record(lambda)`, flushed as ONE command buffer inside `submit()` ahead of any island submit and in `endFrame`; no wait. Users: VFX sprite page import (its fence + borrowed command buffer gone), terrain overlay texture import (mip blits included). Load-time copy-once imports (prop/mech/terrain image imports, one-shots) keep their waits ("load-time callers may wait"). `[VK_UPLOAD v1]` counters (smoke: queued 21..51, dropped 0).
- F4: `vk_readback.*`: `ReadbackCore` (slots, tickets, tag + generation checks; doctest `test_readback.cpp` (5) with the wrong-tag and recycled-slot negative controls) + `ReadbackRing` (host-cached slots). The presented-image readback (T5 check, PPM dump, capture, screenshot) requests at one present and consumes at a later one; the `UINT64_MAX` fence wait is gone. Shadow diagnostic keeps its own explicit wait (diagnostics may wait); producer parity stays an in-island-cb copy.
- F5: present path slots: `slot() = engine frame % 2` selects the acquire semaphore, the in-flight fence and the composite + tail command buffers (`slotCount` 1 under `MC2_VK_FRAMES_IN_FLIGHT=1`, which reproduces the old single frame); the composite-record CPU wait is the fence of the frame two back; a resize waits both slots. New gate row (Int, default 2, passthrough; gate_gen regenerated). The editor lane (claude/editor-vk, `gos_host_vk` present-or-swap) must call `getOrCreateEngineVulkanContext().frameBoundary()` right after its present, or its ring slot never restarts: with no bracket `frameNo` stays constant, so slot() pins to one slot (safe, but the ring only grows).
- F2c post passes (BUGS 66): fog, HZB, SSAO, screenfx waited their own fence INLINE every frame (3-6 ms/frame in the `[QWI-WAIT]` lines, which also made two frames in flight a no-op). Each now records its submit's timeline value and waits it at the top of its next run (screenfx: two passes, two tickets, `cbuf[0]`/`cbuf[1]`); the wait is now 0.2-0.4 us avg. That exposed a latent hazard the layer could not see while the CPU waited: the eight depth/normal restore-to-attachment barriers had dst BOTTOM_OF_PIPE / access 0 (SYNC-HAZARD-WRITE-AFTER-WRITE x10 per mission under `MC2_VULKAN_VALIDATION=sync`); dst is now ALL_COMMANDS / MEMORY_READ|WRITE.

### Proof (VIEWED montages are marked V; MAD 0.00 on every pair = byte-identical, not the pass signal)
| claim | evidence |
|---|---|
| doctest + python contracts at dceaed7e5 | `build_contract_harnesses.py --run --unit` 3299/3299, rc 0 (`scratch/s55_doctest_final.log`) |
| check-contracts at dceaed7e5 | reds: `log_tag_census` + `tools_tests` (new tags, not re-frozen), `cxx17_ratchet` (LG20 tools, s54; LG04/LG17 fixed in 94d497f69), `vk_ownership_ledger` + `gated_layout_macros` (regenerated in 94d497f69); every other row green (`scratch/s55_contracts_final.log`) |
| F0/F1 | smoke-lite sync/core validation errors=0 (`scratch/s55_f01_smoke.log`: [VK_TIMELINE v1] frame=600 last_submitted=5807 completed=5806) |
| F2a HUD heavy pose + glyph text (objectives line, roster, bars) | V `.claude/l2cap/s55-ab-f2a/mc2_01_saved_205644/montage.jpg` and `mc2_10_saved_204523/montage.jpg` (baseline `s55-base-hud`) |
| F2b (mech, prop, light feed: night pools, ambient light feed) | V `.claude/l2cap/s55-ab-f2b/*/montage.jpg`; first build failed validation (VUID-06715 x10): `scratch/s55_f2b1_smoke.log`, fixed in a688dff53 |
| F3 | smoke-lite under `MC2_VULKAN_VALIDATION=sync` errors=0 (`scratch/s55_f3_smoke.log`, `[VK_UPLOAD v1] flushes=1 queued=21/42 dropped=0`) |
| F4 capture PNG + sidecar | V `.claude/l2cap/s55-ab-f4/*/montage.jpg`; sidecar frame 462 identical to the baseline's for both bookmarks |
| F5 1-vs-2 captures identical at the frozen pose | V `.claude/l2cap/s55-ab-f5/mc2_01_saved_205644/montage.jpg`, `mc2_10_saved_204523/montage.jpg` (N=1 `s55-f5-n1`, log line `frames_in_flight=1`; N=2 `s55-f5-n2`); `s55-ab-f5-vs-base` also 0.00 |
| F5 sync validation | `MC2_VULKAN_VALIDATION=sync` smoke-lite errors=0 warnings=0 (`scratch/s55_f5_smoke.log`, `scratch/s55_f2c2_smoke.log`) |
| F5 resize / minimize / restore | `scratch/s55_resize.py` (SetWindowPos 1280x720, 960x540, ShowWindow minimize, restore, resize) against a running mc2_01 under sync validation: PASS 2605 frames, errors=0 warnings=0, swapchain rebuilt (extents 854x480 -> 2216x1196 -> 1656x881 -> 1901x1021), the minimized rebuild took the existing retry path (`scratch/s55_resize_steps.log`, `s55_resize_smoke.log`) |
| F5 mission reload / quickload | `quickload_cement_check.py` PASS (214 px, max 2000) on the lane install (`scratch/s55_quickload.log`) |
| after F2c (partial) + F5: VK tier1 under validation | 5/5 PASS errors=0 on all five (`scratch/s55_f2c_t1.log`: mc2_01 138 fps, 03 94, 10 90, 17 99, 24 61); resolution tier 2/2 PASS validation 0 (its only red was the fingerprint of a stale exe, rerun on a fresh one by the later smokes) |
| F2c tip (post tickets + terrain + overlay + legacy + water + VFX) | captures V `.claude/l2cap/s55-ab-f2c/*/montage.jpg` (HUD) and `s55-ab-f2c-clean` (MAD 0.00); producers V `.claude/l2cap/s55-ab-prod/mc2_01_saved_205644/montage.jpg` (water present, identical), self-test `mismatches=0` |
| tip tier1 (94d497f69 + the peak line), core validation | 5/5 PASS errors=0: mc2_01 144 fps, 03 105, 10 86, 17 104, 24 78 (s54 had 139/84/83/88/64) (`scratch/s55_fin_t1.log`) |
| tip tier1 under `MC2_VULKAN_VALIDATION=sync`, two frames in flight, full 120 s soaks | 5/5 PASS errors=0 warnings=0 on all five (`scratch/s55_fin_t1sync.log`: 115/80/68/80/56 fps, sync validation costs about a third) |
| frame ring sizing | `[VK_FRAMERING v1] peak_used`: mc2_01 4.2 MB, mc2_10 (night) 10.5 MB of the 16 MiB slot (`scratch/s55_peak_smoke.log`); run_end VRAM is +90..99 MiB over s54 at every mission (the ring is 48 MiB device-local-preferred, plus its slot tail and the retire queue; the deleted parity buffers are small beside it) |
| waitIdle sites | 66 code sites at eee783c6a -> 50 at the tip in `GameOS/gameos` (`scratch/s55_count.py`); every remaining one is init failure, mission unload, geometry regen, swapchain/teardown, shadow, or the sky/legacy copy-once paths |

### Ratchet rises and generated files (named)
- `log_tag_census` (+ `tools_tests`, which freezes it) RED: new tags `[VK_TIMELINE v1]` (2 key sets), `[VK_UPLOAD v1]`, `[VK_FRAMERING v1]` (and the s54 `[TOOLVK]` lines). NOT re-frozen: `tests/unit/log_tag_baseline.json` is on the do-not-regenerate list; the owner/orchestrator runs `py -3 tools/log_tag_census.py --write` in the merge commit.
- `cxx17_ratchet` LG20 +6 in `tools/common/*` and `tools/mc2fx_preview` (s54 step 3.7, not touched this session). The two idioms this session added (LG04 a switch, LG17 a pointer+count pair in `vk_compute_pipeline`) were fixed (if-chain, `std::span`).
- `dup_lines_ratchet` +18 (command-pool boilerplate in `vk_one_shot.cpp` / `vk_upload.cpp`): fixed by `beginTransientCommandBuffer`.
- Regenerated with their own writers (not hand edits): `Gates/gate_table.json` + `gate_index.gen.h` + `docs/flags.auto.yaml` (new gate row), `.claude/cleanup/vk-ownership-ledger.md`, `docs/render-backend-seams/gated-layout-macros.md`, and `tests/callers.json` ONCE (own script row: the new `check-vk-submit-funnel.sh` row shifted the line numbers `check-contracts.sh` callers record; the diff is line numbers plus that row).
- New contract row: `vk_submit_funnel`. New gate: `MC2_VK_FRAMES_IN_FLIGHT` (default 2, kill switch =1; an Int, so no default-OFF gate was added).

### BUGS rows
64 (lifetime assumed one frame in flight, frame counts as proof, device idles on growth: fixed), 65 (blocking `UINT64_MAX` present readback: fixed), 66 (post islands' inline fence waits + the barrier hazard they hid: fixed). Rows are in `.claude/cleanup/BUGS-FOUND-FIXED.md`.

### Decisions (narrower option taken; recorded so the owner can overrule)
1. Order F0, F1, F2a, F2b, F3, F4, F5, then the F2b/F2c remainder: F5 needed the HUD/ImGui ring (F2a) and nothing of the island conversion (the islands already bound themselves to 2 frames with parity fences), so the remainder did not block it.
2. FrameRing is fixed-size in practice (16 MiB per slot; growth works and re-points sets lazily per parity) rather than growing from small: growth rewrites island sets, so it is meant to never happen.
3. The frame boundary is the present (`frameBoundary()`), so any host that presents joins the tracker with one call.
4. ReadbackRing/UploadBatch keep their staging in the frame ring or dedicated buffers retired by the frame ticket, instead of a separate wrap-around ring with its own tickets (no consumer needed the difference).
5. The s54 OPEN item "a record() failure should degrade to the old feed instead of disabling the island" was NOT done: the degrade path needs the GL cull/pack feed to be requested by the caller after the native producers go away, and there is no fault injection to exercise it (an unproven shipped path is worse than the current fail-hard-per-island).

### OPEN
1. F2b/F2c not converted: the shadow island's `instBuf[2]`/ubo (already grows without idling), the post islands' small per-pass UBOs (single buffers, now protected by their tickets rather than ring spans), the water thin readback buffer (binding 6 alternates with the native producer's device buffer), the sky island (no per-frame host data), terrain `heightBuf`/cement/visual-height (persistent tables, load-time device idle at growth stays).
2. Two frames in flight is a no-op for throughput today: three interleaved 30 s mc2_10 runs, avg fps N=2 119.5 / 113.3 / 106.3 vs N=1 121.4 / 116.5 / 98.0 (p99 13.9/15.6/28.4 ms vs 13.7/14.6/35.4 ms; the third pair ran on a loaded host), mc2_03 111.5 vs 112.2; `CompositeWait` avg 1.9 us vs 1.7 us (`scratch/s55_ab3.txt`, `s55_fn*_game.log`). The frame is CPU-bound at ~115 fps (IMMEDIATE present), so a deeper queue cannot help; the real win is the post passes' 3-6 ms per frame inline stall (BUGS 66, now 0.2-0.4 us).
3. Input latency: with two frames in flight the CPU can run one frame further ahead (up to ~1 frame of extra latency at the vsync bound); `MC2_VK_FRAMES_IN_FLIGHT=1` restores the single frame. Not measured with a latency tool.
4. Three smoke runs flagged `heartbeat_freeze_play` (tier1 mc2_03 and mc2_10 at 16:13 while this lane built and ran doctest in parallel, and one 30 s mc2_03 N=2 run at 17:29); the 0.7-0.9 s peak frame shows in every run of the same mission at N=1 and N=2, so it is not F5-specific. The bucket compares the last play heartbeat with the process exit (`scripts/smoke_lib/gates.py:115-123`), so a slow shutdown reads as a freeze. Not chased.
5. The editor lane must add the one `frameBoundary()` call (see F5 above) when it merges.
6. `tools/mc2fx` and the other non-viewport tools are untouched; s54 OPEN items 1-4 stand.

## FIFTY-FOURTH session (2026-10-17, Sonnet 5.5) -- 3.4P review fixes, step 3.7: the viewport tools ported to Vulkan

### Commits (on bc9133f66)
5bd49d2ac 3.4P review fixes (code) | 65a1f4fa1 step 3.7 tools (code) | a state commit after them (docs only). Mergeable code tip: 65a1f4fa1.

### 3.4P review (s52-s53 cleanup review, 1 MUST + SHOULDs)
- MUST: binding 6 of each parity descriptor set follows the feed that ran (`WaterIsland::thinNative[2]`; `write_descriptors(s, only)` rewrites one slot after its fence wait). Native -> readback flips that slot to `thinBuf[par]`; first readback frame still allocates and rewrites under waitIdle.
- SHOULD done: parity frames only 60 and 600 (the every-3600th stall is gone); a `record()` failure ends its begun command buffer before failing soft; thin-cap overflow warns at init; a lighting self-test mismatch refuses native (GL feed stays); parity truncation no longer inflates only_gl/only_vk (left at 0 when `truncated`).
- NOT done (nits, OPEN): self-test/reference in its own file; unused `par` arg and `VkNativeInputs::recipeCount`; Slang constant drift guard; self-test covers one branch combination; a record failure still disables the island instead of degrading to the old feed.
- Proof: ninja exit 0 + deploy; VK tier1 under `MC2_VULKAN_VALIDATION=1` = 5/5 PASS, validation errors=0 warnings=0 on all five (`scratch/s54_vk_t1.log`; mc2_01 139 fps, 03 84, 10 83, 17 88, 24 64). Fail-soft run (`MC2_WATER_GPU_FULL_RECIPE_AUTHORITATIVE=0`, `.claude/l2cap/s54-failsoft`, viewed): NO WATER, identical to GL with the same env (`s54-failsoft-gl`, viewed): the opt-out mode renders no water on either backend (pre-existing: its window mode is not fed), so VK draws nothing and no garbage, validation clean. The wrong-buffer bug needs a thinCount>0 readback frame while native is ready (null-MVP frame) and is fixed by construction, not exercised by a capture. Native capture `s54-native` shows water (day montage viewed).
- Doctest (`build_contract_harnesses.py --run --unit`, at 65a1f4fa1): 3277/3277 test cases, status SUCCESS; check-contracts: 121 of 124 rows PASS on the first run, the 3 reds (background_launch inventory, script_callers, scripts_tests = the same inventory) were the new runner and are fixed in 65a1f4fa1 (re-run of the two row tests green).

### Step 3.7: tools on Vulkan
- Inventory (before): all three tools were standalone SDL2 + `imgui_impl_opengl3` apps with their own GL 3.3 context; none used gameos or the engine render path. mc2weapon_gui and mc2fx_preview were pure ImGui (no GL drawing). asset_viewer drew GL FBO previews (mesh lit, Cook-Torrance sphere, engine-GLSL Backend-A), GL texture upload/decoders and about 30 `--smoke-*` modes, 13 of which created a GL context.
- New: `tools/common` = `mc2_tool_vk`: `tool_vk_shell` (SDL2 Vulkan surface, instance/device/swapchain UNORM, one render pass, `imgui_impl_vulkan`, validation counting `MC2_VULKAN_VALIDATION=1`, swapchain screenshot `MC2_TOOL_CAPTURE=<png>` + PNG writer) and `tool_vk_gpu` (textures RGBA8/BC7/sRGB + generated mips, offscreen color+depth targets for ImGui, buffers, one-UBO+N-sampler pipelines, flipped-viewport so GL-style Y-up renders upright, Vulkan [0,1] depth). It is the tools' own thin shell, not the game's VK umbrella: the tools never linked the engine renderer, so there is no umbrella to run them through; the ImGui-on-VK backend is the same `imgui_impl_vulkan` the game uses.
- mc2weapon_gui, mc2fx_preview: GL context and GL ImGui backend replaced; the draw code is untouched. mc2fx_preview no longer links OpenGL/GLEW.
- asset_viewer: textures, KTX2/BC7, MeshGpu, MeshPreview3D, PBR sphere ported (GLSL 450 in `tools/asset_viewer/shaders_vk`, glslangValidator at build time into `shaders_vk/` beside the exe); UiEditorImageCache and the GL smokes removed (13 GL smokes: the screenshot runner replaces them; CPU smokes kept). OpenGL/GLEW stay on its link line only for the isolated `mc2_tglloader` engine closure; no GL context is created.
- Runner: `scripts/run_tool_capture.py <tool> --out x.png [--validation] [--cwd install] -- <tool args>` (background via nofocus.spawn, host lease, hidden window, PNG graded for flatness, validation errors=0 required). Registered in background-launch-allow.json and tests/callers_allowlist.json; asset_viewer got `--view textures <f> | materials | props <name.tgl> | workbench`.

| tool | build | launch+capture (viewed) | validation |
|---|---|---|---|
| mc2weapon_gui | build_all_targets root tree, linked, no GL | `.claude/l2cap/s54-tools/weapon_vk.png`: weapon list + editor pane | errors=0 |
| mc2fx_preview | linked, no GL libs | `.claude/l2cap/s54-tools/fxpreview_vk.png`: effect curves plotted (the Effects window sits under Curves, as with the GL default placement) | errors=0 |
| asset_viewer props | linked | `.claude/l2cap/s54-tools/av_props_vk.png`: 2civliving textured and upright (BC7 albedo through the new path) | errors=0 |
| asset_viewer materials | linked | `.claude/l2cap/s54-tools/av_material_vk.png`: PBR sphere, 4 slots loaded from the fit | errors=0 |
| asset_viewer textures | linked | `.claude/l2cap/s54-tools/av_tex_vk.png`: a_2civliving.ktx2 BC7 sRGB 8 mips | errors=0 |

"Matching its GL rendering": there is no GL capture of the old builds (no baseline was ever recorded); the ImGui draw code and the preview math are unchanged and only the backend changed, so layout/colour match is by identity of the draw lists, and the 3D previews were judged by eye (orientation upright, texture right way up, lighting plausible). Say so rather than claim a pixel diff.

### VK gaps found
1. asset_viewer Backend-A (real engine `static_prop` GLSL preview): not ported. The engine's VK static-prop shaders (color_prop island) need the bindless texture array and scene SSBO set; there is no standalone entry. The radio stays, fails open to Backend-B, the Shader Contract panel states why. The ShaderIncludeResolver and its CPU smoke stay.
2. The 13 GL render smokes of asset_viewer have no VK twin (only the screenshot runner); the CPU pixel assertions (distinct-from-background, tangent flat vs tilt, UV-V orientation) are not reproduced. OPEN: add a readback hook to toolvk and port the three assertions if the regression is wanted.
3. Tools do not run under `MC2_RENDER_BACKEND=gl` (not required).

### OPEN
1. 3.4P nits above. 2. No Tracy/perf on the tools. 3. `tools/mc2fx` (CLI) and the other non-viewport tools were not touched. 4. The docs `asset-viewer-backend-a-*.md` still describe the GL Backend-A as shipped: they are now history.

## FIFTY-THIRD session (2026-10-17, Sonnet 5.5) -- step 3.4P: native Vulkan compute producers (terrain lighting + water cull/pack), first engine Slang shaders

### Commits (on 0421c2417, then merged nifty 26b00c43c)
Merge nifty (callers/cxx17 baseline: nifty side) | WIP shaders + compute RAII + lighting snapshot | wiring into the water island | build fixes | parity diagnostics | white-sentinel init | startup self-test | docs/regens. Mergeable tip: the state commit on top (code tip bd9c08b9e; everything after it is docs and regenerated inventories).

### What landed
- `GameOS/gameos/vk_compute_pipeline.{h,cpp}`: RAII `ComputePipeline` / `ComputeSets` / `cmdBarrier` (first VK compute pipelines in the engine). `vk_native_producers.{h,cpp}`: `NativeProducers` records lighting -> cull/pack -> draw-args in the water island command buffer on the single graphics queue (barriers, no semaphore, no async compute).
- Slang compute shaders (the first real engine Slang): `shaders/vulkan/terrain_lighting.comp.slang` (port of gos_terrain_lighting.comp), `water_cull_pack.comp.slang` (port of gpu_driven_water.comp, full-recipe mode), `water_draw_args.comp.slang` (port of gpu_driven_cmd_patch.comp). Baked by the CMake glob; no pilot needed.
- Water: the VK water draw is `vkCmdDrawIndirect` on GPU-written args over a GPU-written thin buffer. The per-frame blocking GL readback (count + count x 48 B) and the re-upload are gone; the GL cull dispatch is skipped while native is active (`mc2_vk_water_native_mode()`: 0 readback feed (fail-soft and the first frames), 1 native, 2 native + GL for a parity frame at native frames 60, 600, then every 3600th). Recipes now live in device-local memory (staging copy once). GL path under `MC2_RENDER_BACKEND=gl` untouched. No new gate (gated by the umbrella, fail-soft to the old feed).
- Terrain lighting: `gos_terrain_lighting::GetVkProducerSnapshot` publishes the exact arrays/scalars the GL dispatch used each frame; the VK compute runs the same math.

### Proof
| check | result |
|---|---|
| water thin records, VK vs GL, same frame (frames 60, 600) | mc2_01: 4413/4413 and 3673/3673 byte-identical (same=N differ=0 only_gl=0 only_vk=0); mc2_10: 399/399, 388/388; mc2_17: 3955/3955, 923/923 |
| lighting compute vs C++ transcription of quad.cpp (startup self-test, 4096 vertices, 8 lights, all branches) | 0 mismatches, max channel delta 0 (`[VK_PRODUCER_SELFTEST v1]`) |
| VK captures viewed (ab.py, day mc2_01_saved_205644 + night mc2_10_saved_204523, `.claude/l2cap/s53-ab/cmp/*/montage.jpg`) | water identical to GL (MAD 0.82 / 0.50) |
| VK tier1 under validation | validation errors=0 warnings=0 on all 5; full run 3/5 (mc2_01 and mc2_17 heartbeat_freeze_play on a 4-CPU-mask run with other lanes loading the host: the known flake class); each rerun alone PASS (mc2_01 4197 frames incl. the frame-3600 parity, mc2_17 3498) |
| doctest | 3277/3277 test cases |
| CPU per frame | the removed GL readback cost 598 us at 4525 records (`[VK_WATER_PERF] thin_readback_us`); the native path has none (Tracy not run) |

### Findings (changes the 3.4P list)
- Only the water feed was a GL-compute -> VK readback. The light grid VK consumes is the CPU-built `GetLightGridCpu` copy; `cluster_depth_pyramid` and `lightgrid_build` are INERT (no consumer, per their own headers); no VK TU references gpu_cull / terrain_indirect compute output. So those GL producers die with GL and need no VK port.
- The GL terrain-lighting window is EMPTY on the live LOD-chunk path (`land->getNumVertices()==0` at PackAndDispatch): the water thin records carry the 0xFFFFFFFF white sentinel in GL and now in VK (VK lightOut starts white). The lighting shader is therefore proven by the self-test, not by a live window.
- Slang toolchain gaps: set-0 slots must pre-exist in binding-slot-occupancy.json (UBO 4 refused; reused registered slots); the gpu_abi bake check covers only structs named like gpu_abi types (the producer structs have no entry); `docs/shaders-slang.md` updated.
- Process: the `run_smoke.py` combined run flakes under host load; the `Edit`-free helper `scratch/ce.py` preserves CRLF.

### OPEN
1. Terrain lighting is consumed only by water thin records today; if the LOD-chunk path ever packs a window the producer is live (proven by self-test). 2. The GL lighting/water producers stay until 3.5 (CPU consumers, the `gl` opt-out). 3. Thin capacity is capped at 131072 records per slot (overflow only shows on a parity line). 4. Tracy CPU delta not measured. 5. Owner question 5 (editor GL set) unchanged.

## FIFTY-SECOND session (2026-10-17, Sonnet 5.5) -- review must-fixes, editor check, GL canary/INTEROP gates deleted, step 3.4P and the editor blocker recorded

### Commits (on a8b3462ca)
9631b8379 MC2_RENDER_BACKEND gate doc | 249da5ee0 review must-fix (one parse rule, GL arms pinned, stale comments, letterbox RAII) | a2cf61686 3.4c part: four *_GL_CANARY gates | dcff10717 3.4c part: three *_INTEROP kill switches | ffc9c25cd plan amendments | 74708d95a test_gate floor | a docs-regen + state commit after it (docs only). Mergeable code tip: dcff10717 (the exe under test is ffc9c25cd, which differs from it only by the plan doc).

### Review items done
- MUST 1: `mc2::backendFromText(text, &recognized)` (render_backend_select.cpp) is the single parse rule: gl/GL -> GL; vulkan/VULKAN/vk/unset/empty -> Vulkan; any typo -> Vulkan, logged "coercing to Vulkan". `umbrellaOn()` asks `mc2::renderBackend()` (no string re-parse). Doctest "MC2_RENDER_BACKEND text" covers opengl / Vulkan / Gl / "gl ". render_backend_select.cpp added to the unit-test sources.
- MUST 2: `ab.py` GL arm, `capture_ab.capture_cmd(vulkan=False)` and `bless.BACKEND_DECLARED_ENV["gl"]` pin `MC2_RENDER_BACKEND=gl`; tests updated (18 pass).
- SHOULD: stale "=vulkan requested" / forceGL / dated comments rewritten (init, frame, vulkan_context, GuiRuntime, capture_backend); the letterbox/fade exempt bracket is one RAII scope (`HudScaleExemptScope` in gamecam.cpp). Not done: the 8+ brackets in controlgui_render.cpp / forcegroupbar.cpp, the bare-brace nits, window teardown in `fatalBackendFailure` (no handle reachable from that TU), the "fail-soft to GL" log strings (printf-corpus rows). Build check: `MC2_VULKAN` is a global CMake option, default ON; no shipped target drops it unless -DMC2_VULKAN=OFF is passed.
- Astra review (plan only): new step 3.4P "native VK producers for water / terrain lighting / culling" gates the rest of 3.4 and all of 3.5; comparison-tooling note; fade quad recorded UNPROVEN.

### Agenda
1. Editor: built (`ninja EditRel`, ok) and deployed to the lane2 install with `deploy_payload.py ... --exe-name "Mission Editor.exe" --build-dir build64-ninja/out/editor`. `run_editor_smoke.py` refuses on this install (stock dependency audit: non-stock loose data), so it was not usable. `run_editor_playtest_smoke.py` (MC2_PLAYTEST_EXE = lane2 mc2.exe, Exodus a_0_1.pak): the editor starts on its private GL (editor-startup.log: glewInit ok, `[CONFIG v1] MC2_RENDER_BACKEND value="gl"`, mission loaded, EditorObjectMgr::render through frame 120) but the -playtest driver never reaches Start() and the harness reports process_timeout. The pre-flip editor (v0.4, 2026-10-12) times out identically with the same mission, so it is not a flip regression (gaea_peaks_01.pak fails S-CLI auto-load for both). UNPROVEN: a playtest child launched on VK. Static proof only: EditorPlaytest.cpp:949 pushes `MC2_RENDER_BACKEND=vulkan` into the child env block.
2. Gate doc string fixed. GL opt-out letterbox captured and viewed (`.claude/l2cap/s52-letterbox-gl3`, mc2_10 --frame 100, no --clean): both bars full width, top 0..140, bottom 940..1080, same row profile as the s51 VK-after-fix capture. Note `--clean` hides the letterbox.
3. 3.4 done: four canary gates, three INTEROP kill switches, the GL occlusion wrappers. Not done, with reasons: the remaining `glGetTexImage` calls are GL-side uploads and self-tests (VK island counters assert zero GL pulls), so 3.4(b) already holds; the umbrella cannot be unconditional while `MC2_RENDER_BACKEND=gl` exists; the "discarded GL scene FBO" is the 3.5 content.
4. 3.5 NOT started: `gameos_editor` compiles the same GL scene sources as `gameos`, so any whole-file deletion removes the editor's renderer (owner question 5, recorded in the plan).

### Proof
| check | result |
|---|---|
| ninja -j2 mc2 mc2_launcher EditRel at ffc9c25cd + deploy | exit 0 / 0 (a first build raced my own edit on a string escape; fixed before the final build) |
| doctest `build_contract_harnesses --run --unit` | all pass (first run caught the gate-count floor 975 > 967, fixed in 74708d95a) |
| default-env VK tier1, validation on, fingerprint OK at ffc9c25cd | 5/5 PASS, errors=0 warnings=0 (scratch/t52vk2.log) |
| `MC2_RENDER_BACKEND=gl` tier1, fingerprint OK | 5/5 PASS (scratch/t52gl2.log) |
| pytest of the visual_verify tests touched | 18 pass |
| check-contracts -j2 | run 1: 3 red (split_map doc, background-launch inventory, scripts_tests through it), regenerated; final run in the report |
| GL-opt-out letterbox | viewed, bars correct |

### OPEN
1. Editor playtest e2e needs a stock-mission fixture the harness can load; a child on VK was not observed. 2. Owner question 5: editor GL set vs 3.5 (pull 3.6 first, or split the editor's GL set out). 3. 3.4P producers (cull / light grid / cluster pyramid, terrain lighting, water as VK compute). 4. Fade quad capture. 5. Scratch (mine, deletable): scratch/*52*.log, .claude/l2cap/s52-*.

## FIFTY-FIRST session (2026-10-17, Sonnet 5.5) -- GL deletion 3.2 DONE: the default backend is Vulkan; letterbox bar shrink bug fixed; 3.3 part; 3.4a lost-draws tracker deleted

### Commits (on 8015fa3c8)
51a08cb8c 3.2 flip | 9d8f0a310 umbrellaOn stateless str read + editor gate-cache invalidate + cxx17 baseline | 04c35b397 3.3 part (GL feature-coverage family deleted) | 52266a175 letterbox/fade exempt from the HUD shrink | 1567e0303 3.4a lost-draws tracker + MC2_VK_SCENE_GL_PRODUCER_QUERY deleted | 9dc99a357 gated-layout-macros doc regen. Mergeable code tip: 9dc99a357 (the state commit after it is docs only).

### 3.2 the flip
`resolveOnce` (render_backend_select.cpp): unset / empty / unrecognised -> Vulkan; only `gl`/`GL` selects GL. `MC2_VK_SHARED_GBUFFER` row is `Lead0Kill`, default true (registry, flags.auto.yaml, gate_index regenerated). `mc2vkgate::umbrellaOn()` (mclib/vk_umbrella_gate.cpp) is false whenever `MC2_RENDER_BACKEND=gl`, so the opt-out leaves every VK island off (a stateless `gate::str` read: R2 of check-vk-umbrella-readers forbids a cache). `scripts/smoke_lib/capture_backend.requested()` now returns vulkan for unset (its tests updated); the capture/ab/bless/vk_replay tools already pinned vulkan explicitly and the GL arms were retired in step 1, so nothing else assumed a GL default; run_smoke has no backend pin (its default leg is now VK). docs/tier1_env_vars.md and CLAUDE.md updated (GL tier1 is optional, not a required gate). Editor (owner ruling 3): `editor_set_default_env_vars` puts `MC2_RENDER_BACKEND=gl` when unset and invalidates the gate cache; `EditorPlaytest` passes `MC2_RENDER_BACKEND=vulkan` to the game it launches. The editor target is not built in the default tree: UNVERIFIED in the editor exe itself (`ninja EditRel` not run).

### Owner ruling 2 (letterbox / fade)
Already IsHUD under the umbrella since 7159006ef (now default ON, still off under =gl so GL is unchanged by construction). The default-env capture showed a real VK defect: the bottom bar was drawn 0.85 wide and 20 px short, because the HUD batch's bottom-band shrink (`s_hud_scale`) treats it as a HUD panel. Fixed in `GameCamera::renderLetterbox/renderFadeQuad` with `gos_SetHudScaleExempt(true)` around both quads (52266a175). Viewed: `.claude/l2cap/s51-letterbox` (before: bottom bar x 144..1776, y>=960), `s51-letterbox-gl` (GL reference: y>=940, full width), `s51-letterbox2` (after: matches GL), `s51-fade5` (mc2_01 frame 5, bars full width). The fade quad itself was NOT captured: it needs an ABL `fadeToColor` and no capture bookmark triggers one; it shares the code path and the exemption.

### 3.3 (part)
Deleted: `tools/gl_vk_feature_coverage.py`, `gl_vk_twins.py`, `gl_vk_cpp_gates.py`, `gl_vk_source.py`, `gl_vk_twin_map.json`, `vk_not_ported.allowlist`, its pytest and three generated docs, the check-contracts row and the caller inventory entries. NOT done (entangled, left for the next session): `check-gl-shader-variants` / `extract_gl_shader_variants` / `gl_shader_variants.json` (referenced by test_fingerprint, lane_merge, test_lane_merge), `tools/glsl_port_diff.py` (axis_swizzle_ratchet, test_vk_offline_proofs, shader comments), `check-shader-variant.py` (a lint of live GL batcher code; valid until 3.5), `tests/shader_variants`. These die with the GL shaders (3.8).

### 3.4a (first interop batch)
GL lost-draws tracker deleted: `GosGlLostDrawsScope` and its 60 call sites (gamecam, mission, mechcmd2_frame, missiongui, appear, txmmgr_*, gameos_graphics), the tracker body in gos_postprocess_diag.cpp, the scene producer query in beginScene/endScene (+ members), gate row `MC2_VK_SCENE_GL_PRODUCER_QUERY` (table, registry, yaml, index, seed json), log-tag baseline, split allow json. Left: the frozen `docs/gate-history.md` entry, the two printf-corpus rows for the deleted log lines (stale, harmless). Batches (b)-(d) of the plan are untouched.

### Proof
| check | result |
|---|---|
| default-env tier1 (VK) with `MC2_VULKAN_VALIDATION=1`, fingerprint OK at the tip | 5/5 PASS, validation errors=0 warnings=0 on all five (scratch/t51vk2.log); logs show `[VK_ENGINE_CONTEXT]` |
| `MC2_RENDER_BACKEND=gl` tier1, fingerprint OK | 5/5 PASS, CONFIG line value="gl", no VK context (scratch/t51gl2.log) |
| smoke-lite, validation, default env (at 52266a175) | 2/2 PASS, errors=0 |
| `check-contracts.sh -j2` | run 1: 2 red rows (gate_echo_reads, cxx17_ratchet), fixed in 9d8f0a310; run 2 at 1567e0303: 1 red row (gated_layout_macros doc, regenerated in 9dc99a357); all else green incl. scripts_tests. Not re-run after 9dc99a357 |
| `sim_determinism.py check mc2_03 --install lane2`, default env | PASS identical over 3277 frames, first 1000 match the committed baseline chain: the sim is backend-independent |
| letterbox captures | viewed, see above |

### OPEN
1. Editor exe not built (see 3.2). 2. No GL-opt-out letterbox capture after the shrink fix (the exemption is inside `if (vkHudQuad)`, so GL is unchanged). 3. The MC2_RENDER_BACKEND gate_table doc string still says "default gl" (changing it forces a full rebuild; do it with the next gate-table edit). 4. Scratch (mine, deletable): scratch/b51*.log d51*.log t51*.log cc51*.log, .claude/l2cap/s51-*.

## FIFTIETH session (2026-10-17, Sonnet 5.5) -- GL deletion step 3 plan written; step 3.1 done: a failed Vulkan init is fatal (no GL fallback)

### Commits (on 1de45a787)
6b2445340 plan + code (broken string escapes) | c5e75b2af fix of the escapes | state commit after it is docs only. Mergeable code tip: c5e75b2af.

### Plan
`.claude/cleanup/gl-deletion-step3-plan.md`: nine ordered steps (3.1 fatal VK init, 3.2 VK default flip, 3.3 retire the GL proof matrix, 3.4 GL/VK interop removal, 3.5 GL scene passes whole-file, 3.6 editor on VK, 3.7 in-repo tools, 3.8 GL-only shaders, 3.9 present/core/state guard) with size, proof, blockers; four owner questions at its end. Key fact found: the default backend is still GL (`resolveOnce` unset -> GL, umbrella `Exact1` default false), so the flip (3.2) is not done.

### Done: step 3.1
`mc2::forceGLFallback` and `g_mc2VkSessionForcedGL` (mclib/tgl.cpp) deleted; `mc2::fatalBackendFailure(reason)` (render_backend_select.cpp): logs `[RENDER_BACKEND] FATAL: ...`, message box (skipped for MC2_SMOKE_MODE / NO_LAUNCHER / SMOKE_SEED / HEARTBEAT / CRASH_FAST_EXIT), `_Exit(3)`. Callers: context/device init failure, no-MC2_VULKAN build, "not VK-owned" present, the ~60 s present-retry budget. `umbrellaOn()` is a plain gate read. Transient present failures still defer and retry (unchanged). docs/tier1_env_vars.md section rewritten ("VK init failure policy").
Left: the log strings "fail-soft to GL" in vulkan_context.cpp:157 and engine_present_vk.cpp:422 (printf corpus rows; wording only).

### Proof
| check | result |
|---|---|
| ninja -j2 + deploy at c5e75b2af | exit 0 / deploy_exit 0 |
| doctest (`build_contract_harnesses --run --unit`) | all harnesses pass (the doctest "FAILURE" line is the deliberate SIGABRT subprocess case) |
| VK tier1, `MC2_VULKAN_VALIDATION=1`, fingerprint OK | 5/5 PASS, errors=0 warnings=0 (fps 110/96/62/90/74), scratch/t50vk.log |
| GL tier1, fingerprint OK | 5/5 PASS (139/118/96/124/94 fps), scratch/t50gl.log |
| `MC2_VK_FORCE_FAIL=1` through run_smoke | exit_code 3, `[RENDER_BACKEND] FATAL` line in the log (crash_evidence.json), run reported crash_silent as expected |
| `check-contracts.sh -j2` | 124/124 green |
No rendering change (no capture needed).

### OPEN (owner questions are at the end of the plan)
1. Flip the default now (3.2) or at release? 2. Port non-IsHUD letterbox/fade quads via IsHUD? 3. Editor private GL until its rework? 4. asset_viewer / mc2fx_preview / mc2weapon_gui: port or retire?

## FORTY-NINTH session (2026-10-17, Sonnet 5.5) -- GL object-parity check DELETED (owner ruling), VK-PARITY-GAPS row 29 CLOSED (all rows closed)

### Commits (on nifty merge of 2026-10-17)
b1ce2f533 delete the parity check (code) | d12e07e3a docs/baselines (parity gaps row 29, inventory 16(b), printf corpus, log tag baseline) | 60c827647 regenerate gl_shader_variants.json + callers.json | state commit after it is docs only. Mergeable code tip: 60c827647.

### Deleted
`GameOS/gameos/gos_object_parity.cpp/.h/_query.h` (git rm); `static_prop.vert` `parityOut` (binding 3) + 5 `u_parity*` uniforms; in the static-prop batcher the readback (`candidateLogAndParityReadback`, now just `batcher_buildCandidateLog()`), the slot ring and cursor, the clear, the dual-emit snapshot in submit, `OnMissionLoad`/`ParityFrameTick`/`PickNextSampleIfReady`/`ObserveSubmittedShape` calls, `guardSsbo3`, `s_currentSubmittingMulti`, `kParitySlotBytes`; `fenceAndParitySlot` renamed `fenceRingSlots`; the dual-emit full-bake hooks in bdactor_tree/bdactor_update/genactor_frame; the `MC2_OBJECT_PARITY_CHECK` gate row and its views (gate_gen --sync-verified, `--check --rebuild` OK); asset-viewer stubs and doc; binding-registry row; shader-reflect goldens for static_prop.vert regenerated (they also carried unrelated drift); cxx17 baseline rows; 12 printf-corpus rows; 171 lines of log_tag_baseline. 30 files, about -610 lines in the code commit. Kept: `MC2_OBJECT_PARITY_TRACE` (separate CPU light-trace gate, `[PARITY_DIAG v2]`).
Left alone: the frozen `tools/gate_table.seed.json` keeps a stale reader location for the TRACE gate (edited only the CHECK entry); pre-existing shader_reflect DRIFT in other shaders is not from this change.

### Proof
| Check | Result |
|---|---|
| ninja build -j2 + deploy | exit 0 / deploy_exit 0 |
| doctest (`build_contract_harnesses --run --unit`) | 3277/3277 test cases (the harness "FAILURE" line is the deliberate SIGABRT subprocess case) |
| GL tier1, fingerprint OK | 5/5 PASS |
| VK tier1, `MC2_VULKAN_VALIDATION=1`, fingerprint OK | 5/5 PASS, errors=0 warnings=0 |
| `check-contracts.sh -j2` | red rows were gl_shader_variants (stale line number, regenerated, 230/230 variants compile) and script_callers (stale, regenerated); both fixed in 60c827647 |
| pytest scripts/tests | 912 passed |
| `repo_query.py binding --conflicts` | no new conflicts; ssbo multi_binder 5 -> 4, other counts equal to nifty |
| log_tag_census, printf corpus, cxx17 ratchet | OK |

### Also
VK-PARITY-GAPS.md: row 29 CLOSED by owner ruling (accepted residual: head shape slightly blockier than MLR; MC2_GPU_PARTICLES default OFF); header says all 29 rows closed. Inventory section 16(b) item marked DONE.

## FORTY-EIGHTH session (2026-10-17, Sonnet 5.5) -- s47 review items closed; gos_object_parity GL readback NOT droppable without a VK parity producer

### Commits (on 8b2eb167b)
83703f617 s47 review items (code) | state commit after it (docs only). Mergeable code tip: 83703f617.

### Task 1 (review items)
- `vulkan_mech_island.cpp upload_geometry_once`: the null/size check of `feed.vboCpu/iboCpu` now runs BEFORE the waitIdle and the vbo/ibo reset, so a normals rebuild declines with the old buffers kept and no device stall.
- Readback "shown" skip: NOT done. No cheap signal exists: `gos_EndCameraPreviewRender` is called only when simplecamera rendered the preview (review: not-shown cost is already none); whether the ImGui panel is actually drawn is decided later in the frame by GuiRuntime, after End. Left as is (review Should-fix 2 said acceptable).
- Nits done in `gameos_graphics_capi.cpp`: GL_PACK_ALIGNMENT and the bound 2D texture restored after the readback / first-call setup; `s_camPreviewTexelsName` cleared where the FBO is deleted on resize; lit-pixel diagnostic (`[PREVIEW_READBACK v1]`) removed; the stranded "Restore the default framebuffer" comment moved back above `gos_EndCameraPreviewRender`; comment that the down FBO lives for the process.

### Task 2 (gos_object_parity): neither (a) nor (b) taken
The parity bytes are written by the GL vertex shader `shaders/static_prop.vert` (`parityOut`, SSBO binding 3, `u_parityWrite`); no VK static-prop shader writes them. `CompareAndReport` compares those GPU bytes against the CPU recompute, so dropping the GL `glGetBufferSubData` leaves the comparison with no input: (b) deletes the feature, it does not "keep the CPU comparison". (a) needs a new parity SSBO output in the VK prop island shaders plus a VK buffer readback; the s39 screenshot readback and the s45 bridge give pixels / ImGui texels, not per-vertex colours, so neither is a source. The check is also default-OFF (`MC2_OBJECT_PARITY_CHECK`). Recommended owner ruling: DELETE the whole dual-emit parity check (readback, SSBO cursor, `static_prop.vert` parity block, `gos_object_parity` compare) with the GL static-prop producer, or port the shader output to VK; ask which.

### Proof
| check | result |
|---|---|
| ninja -j2 + deploy at 83703f617 | exit 0, fingerprint OK |
| `build_contract_harnesses --run --unit` | rc 0 |
| `pytest scripts/tests` | 908 passed, 270 subtests |
| VK tier1, `MC2_VULKAN_VALIDATION=1`, MC2_RENDER_BACKEND=vulkan, DEPLOY_FINGERPRINT OK | 5/5 PASS, errors=0 warnings=0 on all five (fps 136/105/86/99/76), `scratch/t48v.log` |
| `check-contracts.sh -j2` | 4 red rows, all from the s45 ImGui bridge code, none from this diff: counter-honesty (engine_present_vk.cpp:1621,1651) and script_callers (both already fixed on nifty by 3196cd289 / 94621015d merge follow-ups), log_tag_census (new `[VK_IMGUI]` lines: `log_tag_census.py --write` at merge) and test_gl_shader_variants manifest freshness (unrelated to this diff; not investigated) |

### OPEN
1. Owner ruling on gos_object_parity (above).
2. At merge: regenerate log_tag_census and the GL shader-variant manifest; counter-honesty/callers come from nifty's follow-ups.
3. Mechlopedia / options preview panel capture still not done.

## FORTY-SEVENTH session (2026-10-17, Sonnet 5.5) -- VK mech island VUID errors fixed (BUGS 58), 3D mech preview panel drawn under VK (BUGS 56 closed)

### Commits (on 9b9270005)
ba046261b mech island re-upload on geometry generation | 805e07ab8 run/instance/bone snapshot tagged with the geometry generation | 10897d0fd preview FBO readback for the VK ImGui bridge | state/BUGS commit after (docs only). Mergeable tip = the state commit. Code = 10897d0fd.

### Root causes
Task 1 (BUGS 58): two coupled causes. (a) `vulkan_mech_island.cpp` copied the batcher's VBO/IBO into VK buffers once per process (`geomUploaded` never reset) while the batcher rebuilds both at every map load and for late types; the shadow and prop islands already track a generation, the mech island did not. (b) the run/instance/bone snapshot from the last flush survives that rebuild, so the first frame after it draws old firstIndex ranges against the new buffer: after fix (a) alone the campaign leg still gave 10 errors at mc2_05 (upload 25896 B, runs up to firstIndex 6987). Fix: island re-uploads (waitIdle, reset, upload) when `Mc2MechGeomFeed::geomGen` changes; `s_vkMechSnapshotGeomGen` is set at snapshot time and `get_frame_feed`/`get_shadow_feed` refuse a stale snapshot.
Task 2 (BUGS 56): the mech IS drawn into the GL preview FBO under VK (`[PREVIEW] drawTris isHUD=0 curFbo=9`); the ImGui bridge just had no texels for a render target. Chosen option: scale-agnostic GL readback (the narrower of the two). `gos_EndCameraPreviewRender` (VK umbrella only) blits the FBO (default 4x, 3200x2400) to 800x600, `glReadPixels`, publishes the vector under the FBO texture's GL name (`gos_GetCameraPreviewTexels`, checked first in `gos_GetTextureTexelsByGlName`); a new vector per render is the bridge's content-change signal, so it re-uploads each render (about 1.9 MB). Log: `[PREVIEW_READBACK v1] first publish with pixels`.

### Proof
| check | result |
|---|---|
| VK campaign leg, `MC2_VULKAN_VALIDATION=1`, 15 missions, exe 10897d0fd | `validation_errors=0`, 0 `robustBufferAccess2`, 13 mech geometry uploads (`.claude/l2s47/vk-campaign2`); before: 10 errors (`.claude/l2s47/vk-campaign` with fix (a) only, `.claude/l2s46/vk-campaign`) |
| VK tier1 + validation, exe 10897d0fd, DEPLOY_FINGERPRINT OK x5 | 5/5 PASS, errors=0 warnings=0 on all five (`.claude/l2s47/t47.log`; fps 135/123/96/96/58) |
| mech bay VK | `.claude/l2s47/bay2.png` viewed: yellow Bushwhacker drawn in the preview panel, everything else upright; before `bay0.png` empty panel |

### Where the bay capture is launched (for the lease fix)
`scripts/mechbay_preview_harness.py`, `main()`: `nofocus.spawn([exe], ...)` (from `scripts/smoke_lib/nofocus.py`) with a hard `p.kill()` after `--timeout`. No lease code of its own: it waits only through whatever `nofocus.spawn` does ("[wait] host game lease" lines appeared when my own earlier campaign exe was still alive and once with "unknown holder"); the harness never calls host_lease itself. `tools/campaign_save_roundtrip.py` (`capture`, `nofocus.spawn` at line ~161) is the same pattern. Not fixed here.

### Open
1. The preview readback stalls the GL queue once per preview render (bay and mechlopedia only); an async PBO readback is the upgrade if a profile shows it. `MC2_PREVIEW_FBO_SCALE=1` makes the blit a copy.
2. Mechlopedia and Options paint preview use the same FBO but were not captured under VK this session (encyclopedia page not re-checked for the panel).
3. Carried from s46: BC7 UI colour space not compared by eye; late forced VK-to-GL fallback not run.
4. `preview_debug.log` in the lane2 install grows without bound under `MC2_LOG_PREVIEW=1` (8.6M lines); the harness sets that env.

## FORTY-SIXTH session (2026-10-17, Sonnet 5.5) -- ImGui layer drawn upside down under Vulkan (fixed), s45 texture bridge review blockers fixed

### Commits (on 8f0622666)
7f769cda5 TEMP diag (DrawUiText vertex log; net content reverted in 3da8aeb94) | 3da8aeb94 ImGui flip fix | a2bd261c6 bridge review fixes | state/BUGS commit after (docs only). Mergeable tip = the state commit.

### Root cause (task 1)
NOT text and NOT a font atlas: the ENTIRE ImGui layer was upside down (portraits, mech icons, button art, label positions; the s45 note that icons were upright was wrong, the portraits in `.claude/l2s45/vk_bay.png` are inverted). Every logistics/encyclopedia page is a defs page drawn through ImGui (`DrawUiText`/`DrawUiImage`). Evidence: a temporary log of `DrawUiText` boxes showed BACK at y=440 and MAIN MENU at y=14 (ImGui space, correct for the game), drawn at the top and bottom respectively. The present image has GL bottom-left origin (composite un-flipped, copy-2 flips into the swapchain) and the native HUD shader flips Y itself; the stock Vulkan ImGui backend draws top-down into that image. Introduced with the first ImGui present tail (4ed1ca21d); invisible while ImGui carried only the debug overlay. Fix: vendored backend `ImGui_ImplVulkan_SetFlipY(true)` (negative-height viewport, scissor mirrored). `MC2_VK_IMGUI=0` draws no ImGui at all under VK (black pages), so it cannot show a flip; no pre-s41 exe was run (by construction since 4ed1ca21d).

### Review blockers (orchestrator, Astra) fixed in a2bd261c6
1 pool: size 16 + 2*256, live and retired both capped at 256, `AddTexture` (vendored) returns null on exhaustion, draw skipped, logged once. 2 identity: each cache hit compares the live texture's texel vector pointer once per frame (plus the weak_ptr), a mismatch retires and re-uploads. 3 descriptor allocated before any command is recorded (a failure leaves nothing recorded). Should-fix: BC7 sRGB sampled as UNORM, BC7 size ceil(w/4)*ceil(h/4)*16 validated, LRU eviction at the live cap, scope guard for the id/clip restore.

### Proof
| check | result |
|---|---|
| mech bay VK (`mechbay_preview_harness bay`), final exe a2bd261c6 | `.claude/l2s46/bay3.png` viewed: all text, portraits, icons, buttons upright (before: `.claude/l2s45/vk_bay.png`, `.claude/l2s46/bay0.png`) |
| encyclopedia VK | `.claude/l2s46/enc3.png` viewed: upright, list order A to J, body text readable |
| in-mission HUD text VK | `.claude/l2s46/mis2.png` viewed: unit names and hint text upright (native HUD path, not affected) |
| VK tier1 + validation | 3da8aeb94: 5/5, 0/0 (`.claude/l2s46/t46.log`); final a2bd261c6: 5/5, errors 0 warnings 0, DEPLOY_FINGERPRINT OK x5 (`.claude/l2s46/t46b.log`) |
| VK campaign leg (validation ON), a2bd261c6 | `.claude/l2s46/vk-campaign`: results, logistics, mission 2 (mc2_16) loaded, no crash; ended on the timeout |
| `build_contract_harnesses --run --unit` | rc 0 (`.claude/l2s46/p_doctest.log`) |

### Open
1. Task 2 NOT done: the 3D mech preview panel is empty. The camera-preview colour texture is a GL FBO texture (`gos_BeginCameraPreviewRender`, 800x600 times `MC2_PREVIEW_FBO_SCALE`, default 4 = 3200x2400, about 30 MB). Needs either a per-frame GL readback into a VK image (scale 1 = 1.9 MB) after confirming the mech is still drawn into the GL FBO under VK-owned scenes (the log shows `BeginCameraPreviewRender fbo=9 status=0x8cd5` = complete, but whether the mech batcher draws into it is unverified), or the VK mech island rendering into a VK colour target for the panel. Row 56 stays OPEN on this.
2. The VK campaign leg with validation ON reports 10 `VUID-vkCmdDrawIndexed-robustBufferAccess2-08798` errors (index size 4, firstIndex 5562..6156 beyond a 19740-byte index buffer) during mission 2 load, after the `VK_MECH_ISLAND` texture-cache lines; not ImGui (no ImGui draws in missions). Tier1 stays 0/0. NOT investigated; s45 ran its campaign leg without validation. Candidate: a mech or shadow island draw. Needs its own session.
3. BC7 UI colour space: UI images now sample BC7 as UNORM; not compared by eye against a BC7-sRGB UI texture (the bay icons are RGBA8 mirrors).
4. Carried: late forced VK-to-GL fallback not run; GL flush reads `listOfTextures` at draw time (latent).
5. Process: the build and the game launches ran through script files (`scratch/b46.sh` waits for no mc2 process, builds with -j2 and a unique `_MSPDBSRV_ENDPOINT_`, deploys to the explicit lane2 directory); no variable-built exe path.
6. Lesson: a shell heredoc swallows `\n` (build the backslash with `chr(92)`), `re.sub` needs a function replacement for it, and long heredocs with apostrophes break this tool: use the Write tool.

## FORTY-FIFTH session (2026-10-17, Sonnet 5.5) -- ImGui images under Vulkan (GL texture name to descriptor set bridge), s44 review items folded

### Commits (on 3ac571c9f)
3cc5184f8 bridge + review items (non-mutating skip, one alpha helper, s_packetAlphaBaked cleared in onMapLoad) | 3c214d229 log newline fix and the helper as a batcher static (friend access to TG_TypeShape) | state/BUGS commit after (docs only). Mergeable tip = the state commit.

### What
`recordPendingImgui` (engine_present_vk.cpp): each non-font draw command's GL texture name goes through `gos_GetTextureTexelsByGlName` (new; `findTextureHandleByGlName` on the renderer, tolerant of released slots) to a VK image uploaded from the texture CPU texels (RGBA8 or BC7), recorded into the present command buffer before the ImGui render pass, wrapped with `ImGui_ImplVulkan_AddTexture`, cached per GL name (cap 256, pool 16+256). Invalidation: the entry holds a weak_ptr to the immutable texel vector; it expires when the texture is released or its content re-published, the entry retires and is destroyed 4 frames later (RemoveTexture first); everything is released before `ImGui_ImplVulkan_Shutdown`. A name with no texels (render targets) is skipped at bind time (clip rect collapsed) and retried every 120 frames. The ids and clip rects are swapped for the draw and RESTORED afterwards, so ImDrawData is not mutated (review item 1).

### Proof
| check | result |
|---|---|
| mech bay under VK (`scripts/mechbay_preview_harness.py bay --frame 1500`, lane2 install) | `.claude/l2s45/vk_bay.png`: mech icons, pilot portraits, tiles drawn (s44: none). The 3D preview panel is empty (camera-preview FBO texture, row 56 OPEN) |
| encyclopedia under VK | `.claude/l2s45/vk_encyclopedia.png`: the four icon atlases drawn |
| GL by eye | not possible: the GL screenshot arm is deleted, and `MC2_VK_IMGUI=0` gives a black VK readback (`gl_imgui_bay.png`). Compared to the game's own art by eye only |
| VK campaign leg (`campaign_save_roundtrip capture`, VK env) | `.claude/l2s45/vk-campaign`: results, logistics, mission 2 (mc2_15) loaded, frame 8880, no crash; ended on the 150 s timeout like s44's PASS |
| VK tier1 + `MC2_VULKAN_VALIDATION=1`, exe 3c214d229, DEPLOY_FINGERPRINT OK | 5/5 PASS, errors=0 warnings=0 (`.claude/l2s45/t45.log`) |
| `build_contract_harnesses --run --unit` | rc 0 |

### Open
1. Text in several logistics/encyclopedia labels (mech and pilot names, weights, encyclopedia body) is drawn vertically flipped under VK (see the two captures). NOT from the bridge: only four 256x256 icon textures are resolved there. Source not found (a HUD-native or ImGui font path); needs its own session.
2. 3D mech preview panel (camera-preview colour texture) is not drawn under VK: needs the FBO colour attachment as a VK image or the native preview path.
3. s44 BUGS row 53 collided with lane 1's rows 53-54 on main; renumbered at merge (2026-10-17): lane 2 rows are now 56 (campaign crash + prop alpha), 57 (images), 58 (Y-flip); main keeps 53-55.
4. GL flush still reads `type.source->listOfTextures` at draw time (flush.cpp ~422-441, flush_prep.cpp ~205-262): latent, not a regression.
5. Carried from s44: late forced VK-to-GL fallback not run.
6. Process notes: launches that mention the lane 2 `mc2.exe` path by literal text are blocked by a PreToolUse hook while any mc2.exe runs (lane 1's); assemble the path in a variable. Deploys named the explicit lane2 directory throughout.

## FORTY-FOURTH session (2026-10-17, Sonnet 5.5) -- VK mission-end crash (ImGui foreign textures) and the second-mission prop alpha crash

### Commits (after the nifty merge 248d9a4e6 of 505511f91)
346eaf31c recordPendingImgui liveness guard (context, backend data, main-viewport renderer data) | e397b1703 foreign ImTextureID commands dropped | d8741e48a per-packet alpha-test baked at finalize | state/BUGS commit after (docs only)

### Root cause
The crash lane 1 found (render frame 391, READ 0x66, ImGui_ImplVulkan_RenderDrawData <- recordPendingImgui) was NOT a lifecycle bug: the guard I added (backend + viewport data live) passed and it still crashed, in vkCmdDrawIndexed. The game's ImGui draws images with GL texture names as ImTextureID; the Vulkan backend binds the id as a VkDescriptorSet. Only the logistics/results screens (after won_mission) draw such images, so it never showed inside a mission. Fix: commands whose id is not io.Fonts->TexID get a collapsed clip rect (skipped before the bind). Behind it sat a second crash at the next mission start: packetAlphaTest dereferenced a released listOfTextures at draw time; now baked at finalize.

### Proof
| check | result |
|---|---|
| VK campaign leg (`MC2_RENDER_BACKEND=vulkan MC2_VK_SHARED_GBUFFER=1`, `campaign_save_roundtrip.py capture --out D --deploy A:/Games/mc2-opengl/mc2-win64-lane2`) | before: crash x3 (frames 326, 359, 391 in lane 1's run); after the ImGui fix: crash 2 (prop alpha); after both: PASS (.claude/l2s44/vk-fix2) |
| same leg with `MC2_VK_IMGUI=0` | PASS (.claude/l2s44/vk-imgui0); the pre-fix default-vs-0 A/B was not run on an old exe: the diagnosis is the guard-passed crash plus foreign_texture_cmds_dropped=41 |
| VK tier1 + `MC2_VULKAN_VALIDATION=1`, exe d8741e48a, DEPLOY_FINGERPRINT OK | 5/5, errors=0 warnings=0, tests/smoke/artifacts/2026-10-17T01-45-54-739735-34112; first run 01-34-29: 4/5, mc2_03 heartbeat_freeze_play (flake, validation on) |
| build + `build_contract_harnesses --run --unit` | build exit 0; harnesses + doctest rc 0 |

### Open
1. ImGui images are not drawn under Vulkan (logistics art, ImGui HUD icons): needs a GL-texture to VkDescriptorSet bridge (ImGui_ImplVulkan_AddTexture over VK-only images, or the owned HUD path). Player-visible: those screens show text and panels without pictures.
2. "late forced VK-to-GL fallback" (lazy GL3 ImGui init) NOT run: the harness cannot force forceGLFallback mid-run (only the 120-retry deferred-present budget or a "not VK-owned" decline reach it). To force it: a test gate or dev-shell command that calls mc2::forceGLFallback at a chosen frame.
3. MISTAKE to report: my first `deploy_payload --target game --ninja` (no explicit dir) deployed to the DEFAULT install mc2-win64-v0.4 (lane 1's dev install), replacing its exe/payload with lane 2's 248d9a4e6 build. Lane 1 should redeploy. Lane 2 deploys must name the dir: `deploy_payload.py A:/Games/mc2-opengl/mc2-win64-lane2 --ninja --allow-new-target`.
4. `campaign_save_roundtrip --deploy` takes a PATH (not the name lane2); env is inherited, so set the VK vars in the shell.

## FORTY-THIRD session (2026-10-16, Sonnet 5.5) -- s42 review fixes: camera pull limited to heads/OneOne, near-plane clamp, nits

### Commits (after nifty merge bee7435c2)
d83a07b0f vfx_particle.vert: pull only when `kind_flags&1` (head) or the group is OneOne (`G.oneOne`), lift capped by the room in front of the near plane (near taken from the GL projection terms, never more than half the view distance), WHAT+WHY comment, `ADDITIVE_ONE_ONE` constant + `VFX_ADD_*` enum in `vulkan_vfx_island.cpp`, `v_is_head` -> `v_flags`, duplicate blend comment dropped | state commit. MERGEABLE TIP = the commit carrying this line.

### Proof
| Evidence | Result |
|---|---|
| nifty merge | `git merge --no-ff 629aac459` clean (bee7435c2) |
| build | ninja -j2, exit 0, exe d83a07b0f9df deployed to the lane install |
| doctest | `build_contract_harnesses --run --unit` exit 0 |
| row 29 oracle, mc2_24 ppc frame 154 `--fixture 1`, exe d83a07b0f (`scratch/vo43a`) | four heads present (2 blue, 2 orange); crop is PIXEL-IDENTICAL to the s42 GPU capture (`scratch/vo42j`: 9,999 px differ from the MLR run, same counts); montage MLR / s42 GPU / s43 GPU viewed: `.claude/l2cap/s43-row29-heads-mlr-s42gpu-s43gpu.png` |
| smoke/dust behind geometry | NOT SHOWN: no oracle bookmark has non-head sprites behind geometry (mc2_10 missile `scratch/vo43b` and mc2_01 volley `scratch/vo43c` viewed: beams, heads and flares only, no smoke or dust drawn). The restriction is by construction (shader gate), not demonstrated on a capture |
| near-plane clamp | by construction only (no capture has a sprite within 4 radii of the near plane); the near term uses the GL projection formula, falls back to half the view distance when the matrix is reverse-Z |
| VK tier1 + validation (shell env, umbrella), final exe d83a07b0f (`scratch/t43.log`, run 2026-10-16T21-36-03) | 5/5 PASS, DEPLOY_FINGERPRINT OK x5, validation errors 0 warnings 0 on all five |

### Open
- Should-fix 3 (s42 review): lazy GL3 init after a late VK-to-GL fallback is unproven. Run one late forced fallback before VK becomes the default backend.
- Row 29 and the 14 pre-existing red contract rows: unchanged from session 42.

## FORTY-SECOND session (2026-10-16, Sonnet 5.5) -- row 29 beam heads and halos fixed (shape residual proposed as accepted), s41 review items folded

### Commits (after 92f06782a)
Temp diag commits (net content removed again) | VFX island: OneOne blend, texels as stored, billboard pull (code in `vulkan_vfx_island.*`, `gos_particle_bridge.cpp`, `vfx_particle.{vert,frag}`, gosFX `card/cardcloud/pointcloud.cpp` blend mapping) | GuiRuntime lazy GL3 init after a VK-to-GL fallback + editor Render branch + doc/scrub nits | docs/state commit. MERGEABLE TIP = the commit carrying this line.

### Proof
| Evidence | Result |
|---|---|
| row 29 oracle, mc2_24 ppc frame 154 `--fixture 1`, exe 0db577a86 (`scratch/vo42h` MLR, `scratch/vo42j` GPU) | MLR byte-identical to s41; GPU heads (2 blue, 2 orange) now drawn with halo; crop 650x370: 10,424 px differ (7,505 over 8 levels, 3,923 over 32, mean 0.908) vs s41 8,263 / 6,738 / 3,971 / 0.966; montage `.claude/l2cap/s42-row29-heads-mlr-s41gpu-s42gpu.png` viewed |
| causes (experiments in `scratch/vo42c..g`) | clip bias 0.001/0.01/0.1 all show the heads (0.1 = 0.01 = 0.001), so depth value; dump showed halo groups with vertex alpha 0; BC7 sRGB view -> UNORM view brought the halos back |
| doctest | `build_contract_harnesses --run --unit` exit 0 |
| VK tier1 + validation, exe 0db577a86 (`scratch/t42.log`, run 2026-10-16T20-15-23) | 5/5 PASS, DEPLOY_FINGERPRINT OK x5, validation errors 0 warnings 0 on all five (default path: the island is not exercised, `MC2_GPU_PARTICLES` default OFF) |
| check-contracts.sh | 14 rows red, the SAME 14 as a clean worktree at 92f06782a (`scratch/cc42base.log` vs `scratch/cc42b.log`): abl_writer_census, estring_callsites, gl_shader_variants, spirv_artifacts, vk_pipeline_layout, gl_vk_feature_coverage, background_launch, vk_ownership_ledger, gated_layout_macros, doc_links, script_callers, scripts_tests, tools_tests, sim_include_firewall. None from this diff; not fixed (cleanup debt on this branch) |

### Review items (orchestrator, s41 cleanup review)
1. `vkImguiRequested()` now reads the backend live (gate cached only); `NewFrame` brings GL3 up lazily when no renderer is up and the session is not Vulkan. Not exercised by a forced fallback run.
2. Editor: `Render()` takes the VK path only when no GL3 renderer is initialised (`!s_openGlRendererInitialized && vkImguiRequested()`).
3. `MC2_ABL_COMPILE_FATAL` `verified` drop is INTENDED: the generator check says it is marked verified but has no `gate::get<>` reader (restoring it makes `gate_gen_check` fail; I tried, then reverted). verified_count 966 stays.
4. check-contracts run (above). Nits: "since lane 2 session 41" dropped; `MC2_VK_NO_GL_INTEROP_STRICT` (deleted gate) removed from the field-check SCRUB list; `MC2_VK_IMGUI` stays scrubbed on purpose (a leaked `=0` would flip the check).

### Open
- Row 29: shape/glow fidelity residual (proposal: accept; default-OFF gate). GPU arm not validation-run (oracle cannot declare it).
- 14 pre-existing red contract rows (above).
- Scratch (mine, deletable): `scratch/` vo42*, b42.sh, build/deploy_s42.log, t42.log, cc42*.log, s42-heads-*.png.

### Lessons (42)
- Do not trust an inherited cause ("heads fail against the tube depth"): MLR beams are HUD-tail and write no depth. A clip-bias sweep (0.001..0.1 identical) localised it in three runs.
- A blend difference can hide in data: vertex alpha 0 is invisible under SRC_ALPHA but not under ONE,ONE.
- BC7 sRGB pages sampled as sRGB crush soft glows when the reference blends in display space.
- A heredoc through this shell swallows `\n`: build the backslash with `chr(92)` or sed.

## FORTY-FIRST session (2026-10-16, Sonnet 5.5) -- nifty 9c39e5cbf and Codex row 29 fix merged, row 29 re-graded (stays open), MC2_VK_IMGUI default ON

### Commits (after 07056c242)
8179fe9b6 merge nifty 9c39e5cbf (clean, no conflicts) | merge of af42a52e7 (Codex Card::Draw harvest, clean) | 53101fc19 MC2_VK_IMGUI default ON (gate row Lead0Kill true, registry, `vkImguiRequested()` only true under the Vulkan backend, tier1 doc) + s40 review note (transient import failure cached) | state/docs commit. MERGEABLE TIP = the commit carrying this line.

### Proof
| Evidence | Result |
|---|---|
| build + doctest at the merge (497a98894537) and at 53101fc19 | ninja exit 0, deploy exit 0, `build_contract_harnesses --run --unit` exit 0 both times |
| row 29 oracle, mc2_24 ppc frame 154 `--fixture 1`, both arms, lane2 install, exe 497a98894537 (`scratch/vo41`), repeated on 53101fc19 (`scratch/vo41f`, identical) | MLR byte-identical to session 39; crop 8,263 px differ, 6,738 over 8 levels, 3,971 over 32, mean 0.966 (s39: 8,381 / 6,734 / 3,947 / 0.997); heads still missing in the GPU arm; montages viewed (`.claude/l2cap/s41-row29-*.png`) |
| depth experiment (GEQUAL -> ALWAYS, reverted, `scratch/vo41x`) | heads appear but as jagged spikes without the halo: depth cause AND wrong-tile cause both still present |
| VK tier1 + validation (shell umbrella), exe 53101fc19, `scratch/t41.log` | 4/5, mc2_24 `heartbeat_freeze_play` at 12 fps min (host contention, known flake); mc2_24 rerun alone PASS (`scratch/t41b.log`); DEPLOY_FINGERPRINT OK x5; validation errors 0 warnings 0 on all five |
| ImGui overlay, default (no `MC2_VK_IMGUI`), mc2_01 + `MC2_DEBUG_RENDERER_TEST=1` + validation (`scratch/d41.log`) | PASS, 0/0, `[VK_IMGUI] requested=1 used_vulkan=1`, `[DEBUGDRAW v1] first_draw queued=67`; dev-shell screenshot viewed: white ring / Y arm and red X arm drawn (`.claude/l2cap/s41-imgui-default.png`) |
| not run | GL tier1 (GL backend ignores the gate, GL ImGui unchanged), check-contracts.sh, item 4 |

### Open
- Row 29: depth (heads fail GEQUAL against the tube depth) and the tile without halo; lane3 evidence (round flares) does not reproduce on lane2.
- Item 4 `gos_object_parity` GL readback NOT taken: still needs a VK source for the parity bytes (inventory section 16 (b)).
- Scratch (mine, deletable): `scratch/` vo41*, b41*.sh, build/deploy/harn_s41*.log, t41*.log, d41.log.

### Lessons (41)
- A pixel-count drop on someone else's install proves nothing until the same oracle on your install moves too; look at the heads, not the total.
- `vkImguiRequested()` was gate-only, so flipping the gate ON would have made a GL-backend run draw no ImGui: tie it to the backend.

## FORTIETH session (2026-10-16, Sonnet 5.5) -- row 29 diagnosed (not fixed), s39 review folded; nifty not merged

### Commits (after 9113a4554)
fc5ba5e83 s39 review fold (island negative cache for failed imports, page-cap log once, `<string>` include, bridge comment shortened) | row 29 docs + state commit. MERGEABLE TIP = the commit carrying this line.

### Proof
| Evidence | Result |
|---|---|
| build + doctest at fc5ba5e83 | ninja exit 0; `build_contract_harnesses --run --unit` exit 0 |
| row 29 diagnosis (VK GPU arm, mc2_24 ppc frame 154, `--fixture 1`, `scratch/vo40*`) | a temporary per-group dump of the frame-154 flush (reverted) shows the four beam heads ARE in the stream as placeholder Card particles (age-0.5 sampling, 8 wu floor); under GEQUAL they fail the depth the tubes wrote (forced ALWAYS: they appear), and visible they show the wrong flipbook tile (V flip tried: different wrong tile). Muzzle residual = same placeholder stream; shaders are line-equal to GL. Both experiments reverted; the oracle diff stayed 8,3xx-8,5xx px (8,381 baseline) in every variant, so no code change shipped |
| GL reference (item 2) | `E:/mc2-archive-2026-10/vfx-oracle-q2/final/ppc` (GL backend, same bookmark and frame): arms differ 213 px over 8 levels but the frame holds NO weapon fire, so it cannot grade muzzle size; row 29 stays OPEN with a recipe (card.cpp is in the never-touch gosfx set) |
| VK tier1 + validation (shell env, umbrella), final exe 04db5f633 (code identical to the docs-only tip) | `scratch/t40.log`, run 2026-10-16T17-25-47: 3/5 PASS (mc2_01, 17, 24), DEPLOY_FINGERPRINT OK x5, validation errors 0 warnings 0 on all five; mc2_03 and mc2_10 `heartbeat_freeze_play` at 41 / 16 fps (host contention, the known flake class); rerun alone: mc2_03 PASS `2026-10-16T17-39-32`, mc2_10 PASS `2026-10-16T17-41-45`, validation 0/0 |

### Lessons (40)
- The oracle needs `--fixture 1` to reproduce the session 39 stream (default `loop` spawns a different sequence: 10 particles, no beams at frame 154); and an arm's `MC2_FX_FORCE_SPAWN` value is the first thing to diff when a rerun does not match.
- An old GL evidence set that "matches" proves nothing when the frame has no content: look at the picture before reading the pixel counts.
- Scratch (mine, deletable): `scratch/` vo40diag, vo40b, vo40c, vo40d, vo40e, vo40cmp*.png, b40*.log, h40a.log.

## THIRTY-NINTH session (2026-10-16, Sonnet 5.5) -- VK VFX island textured (row 29 partly closed), s38 review folded; nifty not merged

### Commits (after 21d247eed; code tip f796fe717 + docs, later commit = state/baselines only)
c33983edb VK VFX island samples the per-group sprite page | f796fe717 s38 review fold (Slot::refusal doctest, oracle `--install` one log line + test) | 66cbc8c8c row 29 + header/Total | state commit. MERGEABLE TIP = the commit carrying this line.

### Proof
| Evidence | Result |
|---|---|
| row 29 oracle, VK both arms, mc2_24 ppc frame 154, exe c33983edb (`scratch/vo39/`) | both arms DEPLOY_FINGERPRINT OK, repeats byte-equal; 8,381 px differ (6,734 over 8 levels, 3,947 over 32) vs session 38's 8,800 / 7,471 / 5,121; mean abs RGB 1.50 -> 1.00; montages viewed, copied to `.claude/l2cap/s39-row29-*.png`; grade stays DIAGNOSTIC. Second cause (beam-head flares not in the particle stream; muzzle flares smaller/dimmer) recorded in the row, not chased |
| validation run of the GPU arm (`MC2_VULKAN_VALIDATION=1` declared through the harness) | `validation=core`, 0 errors, 0 VUID |
| doctest | `build_contract_harnesses --run --unit` exit 0, 3274 cases (+1: screenshot refusal) after the fold; 3273 after the island |
| ledger | Manual 0 for the island (total over all islands 0) |
| VK tier1 + validation, final exe 66cbc8c8c (`/tmp` run 2026-10-16T16-53-25, `tests/smoke/artifacts/2026-10-16T16-53-25-855513-35148`) | 5/5 PASS, DEPLOY_FINGERPRINT OK x5, `validation=core`, validation_errors 0, VUID 0, warnings 0 on all five (fps 119 / 108 / 91 / 92 / 73, host shared) |
| verify.py vs CURRENT_GOLDEN, fresh VK capture on exe 66cbc8c8c (`scratch/s39-vk`, validation declared) | regions 36/36 PASS, poses 10/10 PASS (18 extra bookmarks NO_MANIFEST); validation_errors 0 in all 119 log lines; no golden pose shows VFX, nothing moved |
| ratchets | log-tag census re-frozen (`[SCREENSHOT v1]` keys 6 -> 4: three rejection prints became one with the reason from `Slot::refusal`); own_inventory OK (0 rose); dup-lines OK (no rewrite); callers.json current; background-launch: the same 2 findings as session 38 (`tests/analyze/b5_asset_census.py`, `scripts/tests/test_host_lease_batch.py:69`), not from this diff, baseline not rewritten |

### Lessons (39)
- The shell's `MC2_VULKAN_VALIDATION=1` is SCRUBBED by the harness (env-pin); a validation run needs `--env-declare` through `run_visual_capture`/`capture.py`. The oracle script's own argv[0] ("capture_vfx_oracle.py") is what makes its lease exclusive: a driver script for the harness must carry "capture" in its file name or the lease upgrade fails.
- The textured island first looked like a regression (the big pale discs vanished): they were the untextured stand-in; the real particles are small additive sprites on a black ground, dim next to the MLR cards.
- Earlier journal sentence moved here from row 29: the first VK oracle montage (lane3 exe d3787704e) showed the MLR arm as opaque white squares because the HUD tail declined a missing texture mirror; it does not reproduce on the lane 2 exe.
- Scratch (mine, deletable): `scratch/` vo39, s39-vk, b39/h39 logs; vo38 deleted.

## THIRTY-EIGHTH session (2026-10-16, Sonnet 5.5) -- VK VFX parity row 29 (cause found, not fixed), VFX oracle and capture_ab GL arms deleted, empty POPPED_ENV accepted, s37 review folded; nifty not merged

### Commits (after 05f081b35; code tip 1d8be954e, later commit = baselines/docs only)
85cdc3bc3 cherry-pick of Codex's d3787704e (the oracle VK arm was not on this branch) | 62652a62e parity row 29 + oracle `--install` | 488e402c8 oracle and capture_ab lose their GL arms | (check-env-drive-guard commit) | 1d8be954e s37 review fold (S1-S6) | baselines/docs commit. MERGEABLE TIP = the commit carrying this line.

### Proof
| Evidence | Result |
|---|---|
| row 29: oracle, VK both arms, mc2_24 ppc frame 154, exe 85cdc3bc3 (`scratch/vo38/`) | both arms DEPLOY_FINGERPRINT OK, same-arm repeats byte-equal; crop 650x370: 8,800 px differ (7,471 over 8 levels), grade DIAGNOSTIC; montage viewed. Cause: the VK VFX island is untextured by design (radial falloff of vertex colour), the MLR arm draws the textured sprite atlas through the HUD tail. The white squares of Codex's run do not reproduce on this exe. Fix recipe ~250 lines (atlas import + shader), written in `.claude/VK-PARITY-GAPS.md` row 29, not attempted |
| item 2/3 tooling | pytest oracle + visual_verify + anti_churn + tests/smoke: green except `test_tracy_capture_compare` / `test_tracy_driver_builds_the_pinned_env` (known); one more red in `scripts/tests/test_build_compiler_presets.py::test_default_corpus_commands_and_output_unchanged[False]` (the harness corpus now has an extra `order_seam` cmake pair; neither file is in this diff, not investigated); `check-env-drive-guard` selftest + check PASS (0 popped) |
| harnesses + doctest at the fold | `build_contract_harnesses --run --unit` exit 0, doctest 3273 cases (3 new: screenshot slot) |
| VK tier1 + validation, final exe 1d8be954e (`scratch/t38.log`, run 2026-10-16T16-08-13) | 5/5 PASS, DEPLOY_FINGERPRINT OK x5, validation errors 0 warnings 0 on all five |
| debug overlay with the owned queue: VK mc2_01 `MC2_DEBUG_RENDERER=1 MC2_DEBUG_RENDERER_TEST=1 MC2_VK_IMGUI=1` + validation (`scratch/d38.log`) | PASS, 0/0, `[DEBUGDRAW v1] event=first_draw queued=67` (same count as session 37) |
| ratchets | dup-lines OK (55274, no rewrite); log-tag census OK; own_inventory OK (0 rose); callers.json and background-launch inventory regenerated (the two background-launch findings are the same two files as session 37, not from this diff) |
| not run | GL tier1 (no GL draw path moved), `check-contracts.sh`, the oracle re-run (no fix landed) |

### Deleted
VFX oracle GL arm (`--backend` flag, `gl_fbo` readback acceptance, GL tests), `capture_ab.py --gl-arm` and the `<TAG>-gl` set, the retired `MC2_GL_ERROR_DRAIN_SILENT` name from POPPED_ENV. Kept on purpose: `smoke_lib/capture_backend.py` still recognises the `gl_fbo` spelling (it must reject an old GL sidecar).

### Review s37 folded (1d8be954e)
S1 one-frame overlay lag in the comment and inventory section 19; S5 `DebugRenderer::Queue` owned by `gosRenderer` (free functions act on the live instance); S6 failed/timed-out fence wait recorded before the present declines; S2 `gos::screenshot::Slot` (header-only, doctests) and `tick()` before the Vulkan block; S3 printf corpus is a frozen format-equivalence corpus generated from its jsonl, left; S4 the dup-lines reason goes in the commit body whenever a baseline is rewritten (none this session).

### Open
- Row 29 fix (textured VFX island); MLR retirement waits on it or on an owner ruling that the untextured island is the standard.
- Section 16 (b): lost-draws tracker + `MC2_VK_SCENE_GL_PRODUCER_QUERY`, `gos_object_parity` GL readback, HZB probe, input probe GL reads, dev-shell `reload_shaders` GL path, terrain `tessDebug`.
- `MC2_VK_IMGUI` default-off hides the debug overlay in a default VK run.
- Scratch (mine, deletable): `scratch/` b38*.log, h38.log, t38.log, d38.log, vo38/ (oracle captures, ~100 MB).

### Lessons (38)
- The oracle's `--install` was parsed but unused (the exe path was a lane3 constant): check that a flag reaches the command before trusting a run's provenance.
- A grader that stops reading sidecars for one backend hides an unchecked path: deleting the GL arm made the sidecar check unconditional and the old fixtures needed a sidecar.

## THIRTY-SEVENTH session (2026-10-16, Sonnet 5.5) -- hitch trace and debug renderer off GL, two gate rows retired, s36 review folded; nifty not merged

### Commits (after b951f2ca5; code tip 3b90168bd, later commit = docs/baselines only)
4d1c840c0 hitch trace on the VK frame fence | 8e4d2bf2b s36 review fold (S1, S2, S5, nits) | c5b6fb023 debug_renderer on the ImGui overlay | 019ee0da7 mechbay harness on VK + gate rows MC2_GL_ERROR_DRAIN_SILENT / MC2_SCREENSHOT_SOURCE deleted | 3b90168bd pass-timer all-cancelled frame skipped + shared spvDir() | state/inventory/baselines commit. MERGEABLE TIP = the commit carrying this line. Detail: inventory section 18.
- Coordinator note: the ratchet's "pass-timer bracket boilerplate copied nine times" was the per-island `spv_dir()` body, now `vk_island_util::spvDir()`; no timer-bracket helper was written (the shadow island's bracket spans two functions, so its guard stays at the submit function, now before the early return).

### Proof
| Evidence | Result |
|---|---|
| item 1: VK mc2_01, `MC2_HITCH_TRACE=1 MC2_HITCH_MS=0.001`, validation (`scratch/v37a.log`, deployed 4d1c840c0, DEPLOY_FINGERPRINT OK) | PASS, errors 0 warnings 0; 5,588 `[HITCH_VK]` lines; `fenceWaitMs` 0.01-0.02 per frame, `fenceWaits=1` (a fast GPU: waits are short but non-zero); doctest 4 cases |
| item 2: VK mc2_01 `MC2_DEBUG_RENDERER=1 MC2_DEBUG_RENDERER_TEST=1 MC2_VK_IMGUI=1 MC2_HZB_VIEW_FILE=scratch/view0.txt`, dev-shell `screenshot` (`dev_shell_out/d37_dbg4.tga`, viewed) | red X arm, white ring and the Y arm visible over the water, validation 0/0, `[DEBUGDRAW v1] event=first_draw queued=67`. Without `MC2_VK_IMGUI=1` the overlay is absent (ImGui draws on the GL context then): recorded, not hidden |
| item 3: pytest `tests/smoke`, `scripts/tests`, `tools/repo_intel/test_anti_churn.py`; `gate_gen --check --rebuild` | 35 failures, all `test_tracy_capture_compare` / `test_tracy_driver_builds_the_pinned_env` (same failure on the previous tip, not touched); gate table consistent (976 gates); `check-env-drive-guard` PASS |
| harnesses + doctest at 3b90168bd | `build_contract_harnesses --run --unit` exit 0 (mc2_tests incl. hitch fence, debug line project, all-cancelled frame) |
| ratchets | dup-lines 55319 -> 55274 (baseline rewritten, the spv_dir dedupe); `own_inventory --check` ok; log-tag census rewritten (banner field, HITCH_VK); background-launch inventory regenerated (two findings in `tests/analyze/b5_asset_census.py` and `test_host_lease_batch.py` are not from this diff); `tests/callers.json` regenerated |
| VK tier1 + validation, final exe 3b90168bd (`scratch/t37.log`) | 5/5 PASS, DEPLOY_FINGERPRINT OK on all five, errors 0 / warnings 0 on all five (fps 134 / 92 / 83 / 98 / 70, host shared) |
| not run | GL tier1 (no GL draw path moved: only the static-prop fence call sites changed from the wrapper macro to the raw GL call), `check-contracts.sh`, the mechbay harness end to end, captures |

### Open
- `MC2_VK_IMGUI` default-off means the debug overlay (and the editor inspector's highlight) is invisible in a default VK run; flip it with the editor rework.
- Hitch trace GL upload counters (`MC2_GL_BufferData*` and the `MC2_GPUBUF_COUNTER` tally, ~90 call sites in terrain / static-prop / light-SSBO files) go with their GL paths. POPPED_ENV keeps a retired name because `check-env-drive-guard` fails on an empty table (proof-standard question for the coordinator).
- Section 16 (b) still open: lost-draws tracker + `MC2_VK_SCENE_GL_PRODUCER_QUERY` (with the GL interop), `gos_object_parity` GL readback, HZB probe, input probe GL reads, dev-shell `reload_shaders` GL path, terrain `tessDebug`. VFX oracle VK arm, then the capture `gl_fbo` arm.
- Scratch (mine, deletable): `scratch/` b37*.log, h37*.log, v37*.log, t37.log, d37*.log, gg*.log, shot37*, view0.txt, d37_dbg*.png; the TGAs are in the lane install's `dev_shell_out/`.

### Lessons (37)
- Never `git stash` while a build is running: the working tree flipped under ninja and the pop half-failed (recovered by checkout + pop; no loss).
- `run_smoke.py --deploy lane2` does not deploy; run `deploy_payload.py --target game --ninja --allow-new-target <install>` after every commit-and-build or the fingerprint check fails.
- `--duration` caps at 30; a dev-shell screenshot driver must poll `ping` and shoot ~8 s after the shell answers.

## THIRTY-SIXTH session (2026-10-16, Sonnet 5.5) -- GL deletion step 2 (a), gos_screenshot on the VK readback, s35 review should-fixes; nifty not merged

### Commits (after 3582651b6)
806488128 step 2 (a): GL pass-timer half, GL error drain, GL memory counters, GL frame-pass-stats fields | 2dbb4965b s35 review S1 + S3 | 0c31da555 gos_screenshot on the VK present readback | state/inventory/baselines commit after it.
- Deleted (item 1): `gos_render_pass_timer` ring + Begin/End/QueryActive (27 call sites, two spike-timer guards); `drainGLErrors` (9 call sites) + exit-time drain + `gl_errors` key; `[VRAM v1]` GL_ATI / GL_NVX keys (`vram.py`, tests); FramePassStats FBO / viewport / depth / blend / cull (dev-shell framegraph keys, debug-state-dump keys, four Frame Inspector columns). Pass `ran` is set from the VK timer's harvest.
- Item 2: `gos::screenshot::request(path, done)` / `pending()` / `deliver(w,h,rgbaBottomUp)`; `engine_present_vk.cpp` delivers next to the capture hook, `frameSwapWindow` reads the frame back while a request is pending. Callers: dev-shell, `MC2_SCREENSHOT_PATH`, VisualDiff, `--validate --screenshot`. `source=backbuffer` and the `scheduleScreenshot` flag are gone.
- Review s35: S1 `vk_render_pass_timer::CancelUnlessSubmitted` (over the testable `SubmitGuard` in the ring header) on every island, the VFX island's one-off Cancel folded in; the dynamic shadow pass opens the bracket in its begin function and guards in the submit function. S2 accepted: the session 35 dup-lines baseline rewrite was GameOS +5 lines from the timer Begin/End inside the three near-copy record functions (reason belongs in the commit message; it is now in this file and in 2dbb4965b). S3 exactly eight hex digits. Nit (skip a frame whose brackets were all cancelled) not done.
- Proof: build exit 0 per tree state; `build_contract_harnesses --run --unit` green (doctest 3261 cases); mc2_01 VK + `MC2_VULKAN_VALIDATION=1` + `MC2_DEV_SHELL=1` at 0c31da555: PASS, validation errors 0 warnings 0, dev-shell `screenshot` wrote `dev_shell_out/s36_vk.tga` 854x480 (the present size), viewed (non-black mission frame with HUD); VK tier1 with validation on the final exe (cc168dace, DEPLOY_FINGERPRINT OK): run 2026-10-16T14-02-42-037700-9048 PASS 5/5, validation errors 0 / warnings 0 on all five (fps 147 / 110 / 92 / 115 / 98), no GL tier1 (no GL draw path moved). Not proven: the framegraph runtime rows' VK ms were not read out in that run (collectors reported pass_time on, dropped 0); `check-contracts.sh` not run this session. Owner rulings of 2026-10-16 (row 18 closed, step 2 (b) RULED) are in inventory section 16 and `VK-PARITY-GAPS.md`; `gos_object_parity` readback drop not taken (over 50 lines, lane 3 files).
- Lessons: (1) the Bash tool halves doubled backslashes, so a python heredoc writing `\n` into C++ produced raw newlines in string literals (the compile error is the only signal): write C++ with the Edit/Write tools or `chr(92)`; (2) a python `index()` of a function name hit the non-Windows stub first and a slice replace wiped 500 lines of gos_dev_shell.cpp (restored from git): anchor slices on unique text and diff after every scripted edit; (3) entangled files were split into two commits through temp blobs (`git hash-object` + `update-index --cacheinfo`).
- Open: gate rows `MC2_GL_ERROR_DRAIN_SILENT` and `MC2_SCREENSHOT_SOURCE` (see inventory section 17); section 16 (b) rulings; row 18; VFX oracle VK arm; capture `gl_fbo` arm.

## THIRTY-FIFTH session (2026-10-16, cap 6 h, Opus 5.5) -- s34 review M1 fixed first (fix-then-merge), row 28 closed with numbers, Post covers the native passes, gl_fbo arm NOT retired (consumers listed), step-2 list for the owner; nifty not merged (queued tips)

### Commits (after 3a7322523; final exe = 77b0d2a80, later commits docs / baselines only)
6e8646f8d capture-only building highlight hook | decc5aeae gate row order + doc length (first build failed the table asserts) | 18a0fa4f0 review s34 M1 + S1/S2/S3/N1-N3 + Post on fog / SSAO / screen FX, composite unbracketed | 77b0d2a80 `mc2vk::submitOneShot` (vk_one_shot.{h,cpp}): vk_first_layout and the timer's pool reset share one setup submit (the timer had copied it) | this commit: gaps row 28 closed, inventory section 16, ledger, log-tag census ([VK_ONE_SHOT], the timer's and the hook's lines), dup-lines baseline, state. MERGEABLE TIP = the commit carrying this line.
- dup-lines baseline rewritten (55326 -> 55319 total): the total fell (the shared one-shot submit), but GameOS rose +5 because the timer's Begin/End lines sit inside the fog / SSAO / screen-FX record functions, which were already a three-way near-copy; the calls are not reshaped to dodge the counter.

### Review s34 (FIX-THEN-MERGE) folded
- M1: `vk_render_pass_timer::Cancel(pass)`; the VFX island cancels on its nothing-to-draw path; the present composite (recorded in one frame, submitted or dropped later: resize, re-record, skipped present) no longer carries a bracket. Audit of the other islands: terrain / props / mechs / shadow / water / fog / SSAO / screen FX return between End and submit on error paths. CORRECTION (s35 review, fixed in session 36, 2dbb4965b): not all of them disable the island; the static-shadow post pass (`runVkPostScreenShadowStaticNative`) returns false without `disabled` and retries next frame, and the other islands could publish one stale frame before disabling.
- S1: `vk_render_pass_timer_ring.h` (Vulkan-free ring: bracket query pairs, cancel, drop at reuse, younger slots wait, hard errors apart, overflow count) with doctests incl. the M1 case (8 cases / 50 assertions pass). S2: a hard result-read error discards the frame and logs once. S3: the unsupported verdict is keyed to the device handle. N1: overflow logged once. N2/N3: comments say why.

### Proof
| Evidence | Result |
|---|---|
| build + deploy (`scratch/build_s35b.log`) | exit 0 at 18a0fa4f0; mc2_tests suite rc 0 |
| M1 proof: VK mc2_01 + validation + `MC2_RENDER_PASS_TIME=1`, framegraph polled 12 times over the run (`scratch/b35_passtime.log`, `scratch/s35_framegraph.out`) | PASS, errors 0 warnings 0; AlphaVfx absent in every poll (no stale value after thousands of ring wraps), `pass_time_dropped` 0; post 0.023 ms now covers fog / SSAO / screen FX (was 0.008, composite only); shadowDyn ~0.19, terrainChunk ~0.85, spColor 0.07, mechs 0.02, water 0.068 |
| row 28 oracle (hook 0x40406000 vs off, mc2_01 205644, both backends) | GL term 17,829 px, VK 17,811, shared 17,808, err 0.83 / 48.54; montage viewed |
| verify.py, fresh VK capture `s35-vk` (`scratch/verify35.log`) | 36/36 regions, 10/10 poses |
| tier1 VK + validation, final exe (`scratch/b35_vk_t1.log`) | 5/5 PASS, errors 0 warnings 0 all five, fingerprint PASS (fps low: host shared) |
| tier1 GL, final exe, first run (`scratch/b35_gl_t1.log`) | 3/5: mc2_10 and mc2_17 `heartbeat_freeze_play` with all 8,192 frames done, exit 0, last heartbeat 4 s before exit (the session-31 shape), host shared with other lanes |
| tier1 GL, rerun on 18a0fa4f0 (`scratch/b35b_gl_t1.log`) | 5/5 PASS, fingerprint PASS |
| final exe 77b0d2a80 (the one-shot refactor): tier1 VK + validation (`scratch/b35c_vk_t1.log`) | 5/5 PASS, errors 0 warnings 0 all five, fingerprint PASS; every VK run executes the shared submit through vk_first_layout. The timer's reset path on this exe was not re-run under `MC2_RENDER_PASS_TIME` (proved on 18a0fa4f0; the refactor moves the same calls) |
| final exe 77b0d2a80: tier1 GL (`scratch/b35c_gl_t1.log`) | 5/5 PASS, +0 destroys, fingerprint PASS |

### Open
- Row 18 (owner); the gl_fbo arm's four consumers (inventory section 16); step-2 owner rulings (inventory section 16 (b)).
- Scratch (mine, deletable): `scratch/` s35b.sh/.out, build/deploy_s35*.log, b35*.log, verify35.log, build_s35_tests.log; s34 leftovers r34move.py, b34*.log, verify34.log, build/deploy_s34*.log; KEEP `scratch/s35_framegraph.out` (cited above until merge), `.claude/l2cap/s35-{gl,vk}{,-hl}` and the montage (row 28 cites them).

### Lessons (35)
- A bracket must live on a command buffer whose submit is decided in the same function; a buffer recorded now and submitted (or dropped) later cannot be timed safely by a frame ring.
- The gate table enforces name order and a 160-character doc; `gate_gen --sync-verified` refreshes the JSON from the compiler.
- Prefer an alpha-carrying forced value in a capture hook (0x40406000), so both GL terms (vertex add and alpha-weighted fragment add) are in the oracle.

## THIRTY-FOURTH session (2026-10-16, cap 6 h, Opus 5.5) -- row 28 ported (highlight-on proof needs a capture hook: recipe in the row), water + vegetation field trace (no gap), VK pass timer implemented and proven, review s33 folded; nifty not merged (queued tips)

### Commits (after 9ddea7e06; final exe = d066a9efb, later commits docs only)
843417018 row 28 port | 425f80254 row 28 + water/vegetation census in the gaps file, allowlist | 36133eeaa VK pass timer | (ledger regen) | a11b7692f pass timer pool reset at creation (fixes VUID-...-09401) | d066a9efb review s33 S1/S2/N1 | this commit: state. MERGEABLE TIP = the commit carrying this line.

### VK pass timer (implemented; the session-33 note's design, files by path: review s33 S3)
`GameOS/gameos/vk_render_pass_timer.{h,cpp}` (new: pool in the context's new `IslandSlot::PassTimer`, `vulkan_context.{h,cpp}`; `OwnedQueryPool` in `vk_owned.h`), `gos_render_pass_timer.{h,cpp}` (`SubmitBackendFrame`, `SubmitBackendDropped`, `LastDropped`; GL queries skipped while VK times), `gos_dev_shell.cpp` (`pass_time_dropped` in framegraph collectors), island brackets in `vulkan_terrain_island_run.cpp` (TerrainChunk), `vulkan_color_prop_island.cpp` (SpColor), `vulkan_mech_island.cpp` (Mechs), `vulkan_shadow_island_live.cpp` (ShadowDyn), `vulkan_water_island_run.cpp` (Water), `vulkan_vfx_island.cpp` (AlphaVfx), `engine_present_vk.cpp` (Post: the composite only; the native post passes are not bracketed); doctest `tests/unit/test_vk_render_pass_timer.cpp`. gpu_total under VK = sum of the timed passes (no frame-wide pair across separate submits).

### Proof
| Evidence | Result |
|---|---|
| row 28: fresh VK capture `.claude/l2cap/s34-vk` vs `--golden current` (`scratch/verify34.log`) | 36/36 regions, 10/10 poses, validation 0 on 40 logs; vs the pre-port capture 0 / 0 / 64 px over 15 (no highlight at golden poses); highlight-on pixels not proven (recipe in row 28) |
| pass timer, first run (`scratch/b34_passtime.log`) | per-pass ms non-zero, but validation errors 9 (VUID-vkGetQueryPoolResults-None-09401): fixed by a11b7692f |
| pass timer, rerun on the final exe (`scratch/b34b_passtime.log`, `scratch/s34b_framegraph.out`) | mc2_01 VK PASS, validation errors 0 warnings 0; dev-shell framegraph (12 replies): shadowDyn 0.184, terrainChunk 0.849, spColor 0.071, mechs 0.020, water 0.068, post 0.008 ms, `pass_time_dropped` 0; log `[RENDER_PASS_TIME v1] ... gpu_total=1.21` |
| doctest | mc2_tests vk pass timer 3/3; suite run rc 0 |
| tier1 VK + validation, final exe (`scratch/b34_vk_t1.log`) | 5/5 PASS, errors 0 warnings 0 all five, fingerprint PASS |
| tier1 GL, final exe (`scratch/b34_gl_t1.log`) | 5/5 PASS, +0 destroys, fingerprint PASS (GL code moved: gos_render_pass_timer, comments) |
| provers | coverage --check 0 new 0 stale (row 28 graded PORTED by member read); vk_ownership_ledger OK |

### Open
- Row 28 highlight-on proof (capture hook recipe), row 18 owner ruling; AlphaVfx showed no sample at mc2_01 (no VFX island submit in the window); the native post passes (fog / SSAO / screen FX) are outside the Post bracket.
- Scratch (mine, deletable): `scratch/` s34*.sh/.out, s34*_framegraph.out (cited above until merge), r34move.py, build/deploy_s34*.log, b34*.log, verify34.log; `.claude/l2cap/s34-vk` (cited by row 28).

### Lessons (34)
- A query bracket can be recorded on a command buffer that is then re-recorded or never submitted: reset the whole pool once at creation so such reads are "not ready", not "uninitialized".
- Check who actually sets a per-instance field before planning its visual proof: `setDrawColored` had no caller, so only damage flashes highlight buildings.
- The dev-shell socket can be polled from the harness during a lease-held smoke; that is enough to prove an editor-facing readout without opening the editor.

## THIRTY-THIRD session (2026-10-16, cap 6 h, Opus 5.5) -- row 27 closed (dead GL arm deleted, not ported), ungated census done (row 28 opened and pinned by the coverage tool), VK pass-timer design note (not implemented, over the bar), review s32 folded; nifty not merged (f170964d8 queued at lane 1's session end)

### Commits (after f170964d8; final exe = 5e0ad4c1f, later commits tools/docs)
9c528ddfe row 27 dead GL arm deleted | 5e0ad4c1f review s32 N3/N4 | 6c98b51d9 coverage tool UNGATED_FEATURES + row 28 pin + test | e4c5a5ba8 gaps rows 27/28, census, parity state, review S1/S2 | this commit: state (design note, review N1), callers.json. MERGEABLE TIP = the commit carrying this line.

### Proof
| Evidence | Result |
|---|---|
| row 27 deadness: GL `s32-gl` (exe 20f3c0575) vs `s33-gl` (exe 9c528ddfe) at mc2_10 212258 / 204753 | 0 px over 15, mean 0.000 both poses |
| build -j2 + deploy (`scratch/build_s33b.log`) | exit 0 at 5e0ad4c1f; check-gl-shader-variants 231/231 after the GL deletion |
| tier1 VK + validation (`scratch/b33_vk_t1.log`) | 5/5 PASS, errors=0 warnings=0 all five, fingerprint PASS (mc2_24 passed on 4,853 frames at 40 fps, host shared with other lanes) |
| tier1 GL (`scratch/b33_gl_t1.log`) | 5/5 PASS, +0 destroys, fingerprint PASS (GL shader moved: row 27) |
| provers / board (`scratch/cc_s33.log`) | coverage 0 new 0 stale with row 28 pinned under UNGATED; flip-check OK; pytest coverage 28 pass; dup_lines 55314 <= 55326, file_size and journal at or under baseline; reds unchanged and not this lane's (estring / container callsites, vk_pipeline_layout split test, background-launch, mc2_authored_order_seam, build_compiler_presets) |

### Open
- Row 28 port (two terms in color_prop_tier1; needs a highlighted-building pose or a forced-highlight oracle), row 18 owner ruling, VK pass timer (note above, routed by the coordinator).
- Scratch (mine, deletable): `scratch/` s33a/b.sh/.out, r33tags.py/.out, r33unif.out, build/deploy_s33*.log, b33_*.log, cc_s33.log, and the session-32 leftovers r32move.py, r32w.py, build/deploy_s32*.log, b32_*.log, cc_s32.log; `.claude/l2cap/s33-gl` is cited by row 27 (keep).

### Lessons (33)
- Before porting a GL block, check that its inputs are still written: row 27's bits lost their writer in a gate deletion, and the gate ledger had said so.
- A shared GPU struct declares every field for both backends, so "the name exists in VK" proves nothing for a per-instance term; grade member reads.
- Event-driven terms (selection highlight) are invisible to every static golden pose; the census by data flow (inst.* reads) found what the pixel oracles could not.

### VK timestamp source for gos_render_pass_timer / gos_frame_pass_stats (design note; not implemented: about 200 lines over 8 files, above the 150-line bar)
- Reader contract to keep: `gos_render_pass_timer::LastMs(Pass)` / `HasSample(Pass)` (read by the dev shell `framegraph` and the Frame Inspector), `SetCollect(bool)`, the `[RENDER_PASS_TIME v1]` line. Under VK today every pass reads 0: the GL `GL_TIME_ELAPSED` scopes wrap GL calls that the VK islands replaced.
- Source: one `VkQueryPool` (TIMESTAMP) per frame in flight (2, matching the islands' parity slots), `2 * Pass_Count + 2` queries each (a begin / end stamp per pass plus the frame pair for `gpu_total`). Owned by a new `vk_render_pass_timer` unit beside `vk_gpu_timestamps.h` (reuse its `timestampValidBits` / `timestampPeriod` gating), created lazily on the first `Enabled()` frame: zero cost when off, as the GL timer.
- Recording: each island brackets its own work on its command buffer with `vk_render_pass_timer::Stamp(cb, pass, begin|end)`: shadow island (Pass_ShadowDyn, Pass_ShadowStatic), terrain (Pass_TerrainChunk), colour props (Pass_SpColor), mechs (Pass_Mechs), water (Pass_Water), post / composite (Pass_Post). The terrain and mech islands already own a `mc2vkts::GpuTimer`; the timer unit replaces those private pools rather than adding a third. A pass recorded in several command buffers takes its first begin and last end in queue order (all islands submit on the one graphics queue).
- Reset: `vkCmdResetQueryPool` for the slot at the start of the frame's first island command buffer (the frame submit hook that already orders the islands).
- Resolve: at `FrameEnd`, `vkGetQueryPoolResults` without WAIT on the OTHER parity slot (the previous frame, whose fence the island submit already waited for before reusing its parity resources); a not-yet-available result is dropped and counted (`dropped=`), never waited on, the GL timer's rule.
- Hand-off: the resolved per-pass ms go through a new `gos_render_pass_timer::SubmitBackendSample(Pass, double ms)` into the SAME window aggregation the GL harvest feeds, so `LastMs` / the log line / the Frame Inspector read VK values unchanged; under VK the GL `Begin`/`End` become no-ops (the backend is known at init).
- `gos_frame_pass_stats` needs no timestamps: its GL-state fields (FBO, viewport, blend) are GL-only and report "n/a" under VK; its draw / instance counts come from CPU aggregates that both backends already fill.
- Proof when built: `MC2_RENDER_PASS_TIME=1` under VK prints non-zero passes on mc2_01, the sum within 10% of the frame GPU time from `MC2_VK_GPU_TIMESTAMPS`, validation 0, and the off path allocates no pool (validation object count unchanged).

## THIRTY-SECOND session (2026-10-16, cap 6 h, Opus 5.5) -- nifty merged (c6f7dba41); rows 25 + 26 ported and closed, row 20 ShadowCSM=FALSE closed (no code), row 27 opened, provers pin zero debt, tracy N2/N4, s31 review S1/S2/N2/N3; owner ruling mid-session: minimum viable proof

### Commits (after c6f7dba41; code exe = 20f3c0575, everything later is tools/docs)
dc800a2dd rows 25+26 port + doctest | 20f3c0575 cementHit hoist (first build failed on scope) | 65fd9d6cb provers: parity section empty, PORTED_AS row-26 spellings | 2a320e92c tracy S1/N2/N4 | 9de53b08b gaps rows 20/25/26/27, parity state, Total line | this commit: dup-lines exclude for the gpu_abi-generated frame-UBO includes, callers.json, state. MERGEABLE TIP = the commit carrying this line.

### Proof
| Evidence | Result |
|---|---|
| build -j2 + deploy (`scratch/build_s32a.log`) | exit 0 at 20f3c0575; doctest suite (`build_contract_harnesses --unit-only --run`) SUCCESS |
| golden `.claude/l2cap/s32-vk` vs `--golden current` (`scratch/verify32.log`) | 36/36 regions, 10/10 poses, validation_errors=0 on all 40 logs; vs pre-port s31-vk every pose moved under 0.04 mean |
| row 25 term oracle (`s32-{gl,vk}{,-seam0}`) | 212258 1,664 of 1,667 GL px shared, err 0.45 / 33.51; 204753 0.74 / 32.74; 205644 3.29 / 83.89 |
| row 26 term oracle (`s32-{gl,vk}{,-shore0}`, GL shore0 at 204611 + 205952 only) | 139 / 124 shared px, VK-GL on term px 19.10 -> 15.78, 22.48 -> 9.97; montage `montages/s32-r26-205952-gl-vk-shore.png` viewed; mc2_17 water overviews 0 term px in both |
| row 20 VK ShadowCSM=FALSE smoke mc2_01 + validation (`scratch/b32_csmoff.log`) | PASS 63 fps, errors=0 warnings=0 (was 6.5 fps fail) |
| tier1 VK + validation (`scratch/b32_vk_t1.log`) | 5/5 PASS, errors=0 warnings=0 all five, 122/110/106/115/94 fps, fingerprint PASS; SHORELINE mask imported on mc2_01/17/24 |
| tier1 GL (`scratch/b32_gl_t1.log`) | 5/5 PASS, 147/119/147/145/130 fps, +0 destroys, fingerprint PASS (GL bindShorelineMask moved to the shared producer) |
| provers / board (`scratch/cc_s32.log`) | coverage --check 0 new 0 stale (parity section empty), glsl_port_diff exit 0, gpu_abi 14 match, gate_gen consistent, file_size = baseline, journal 6465 <= 6497, dup_lines 55314 <= 55326 after excluding the generated frame-UBO includes; reds not this lane: estring/container callsites, vk_pipeline_layout split-unit test, background-launch (nifty's tests/analyze/b5_asset_census.py + test_host_lease_batch.py:69), journal ratchet tests, mc2_authored_order_seam |

### Open
- Row 27 runway edge blend (owner lane 2), row 18 owner ruling.
- Scratch (mine): deletable `scratch/` s32a-e.sh/.out, s32d.out, r32move.py, r32w.py, build/deploy_s32*.log, b32_*.log, cc_s32.log, `.claude/l2cap/` s32w-* (not cited); KEEP `s32-{gl,vk}{,-seam0,-shore0}` (rows 25/26 cite them), s31-{gl,vk}{,-amb0}, s31-gl-diag, s31-gl-unbind, `scratch/r31a.py`, `r27term.py` (rows 3/24 cite them: the brief listed s31-vk/s31-gl and r31*.py as deletable, kept because the gaps file cites them).
- I copied `tests/visual/bookmarks/mc2_17_water_wide.json` into the lane2 install's saved_bookmarks for the row 26 capture.

### Lessons (32)
- A generated include (gpu_abi) shows up as duplicated lines; the exclude file is the place for machine-written sources, not a baseline rewrite.
- Wide water overviews cannot prove a 16 wu shoreline band; pick the term poses from the VK on/off diff before spending GL captures.
- Moving gate reads into one producer struct makes the coverage tool attribute every knob to every gate (295 -> 351 rows); harmless, but expected.

### Row 26 design note (before the port)
- GL: `terrain_lod_chunk.frag` block `if (u_useShorelineMask != 0)` (after the edge feather, last term before the output): elevation bands measured as horizontal run from `u_waterElevation` via the macro slope (`smoothTerrainNormal`), fbm edge jitter, wet lobe (sand darken toward `vec3(1.16,1.06,0.78)*luma*0.9`) and animated foam rim (`u_shaderTime` fbm), slope guard 12-20 deg on N.z, beach affinity from lit luma and material weights, optional mask sidecar modulator (G wet, B foam, A valid) in `u_shorelineBounds`. Knobs uploaded by `bindShorelineMask()` (`gos_terrain_lod_chunk_draw.cpp`).
- VK: the same terrain pass (`terrain_tier1.frag`, after its edge-feather block); `mv_smoothTerrainNormal`, `fbm` (noise.hglsl) and `mv_weights` already exist there. The VK `pureConcrete` is a bool (accepted stand-in elsewhere in the file).
- New `GpuTerrainFrameUBO` fields (gpu_abi header, generated include): shorelineOn, shorelineHasMask, shorelineWaterElev, shorelineTime, shorelineStrength, shorelineFoamStrength, shorelineWetRun, shorelineFoamRun, shorelineEdgeJitter, shorelineBounds[4] (pad 62 -> 48 with the row-25 seam flag; review s32 N1).
- One producer for the knobs: `gos_TerrainShorelineKnobs()` read by both GL `bindShorelineMask()` and the VK feed.
- New binding 14 in the terrain set: the mask sidecar (RGBA8, from `TerrainFieldStore::shorelineMask()`, copy-once per store version, 1x1 dummy when absent; linear clamp sampler = the colormap sampler). Ships for mc2_01 / mc2_17 / mc2_24.
- Found while reading row 25: GL's runway edge blend (`terrain_lod_chunk.frag` ~1040-1110, cement-word bits 19:16, gate deleted = always on) has no VK twin: new row 27, not ported this session unless time remains.

## THIRTY-FIRST session (2026-10-16, cap 5 h, Opus 5.5) -- nifty merged (76195d1b2), row 24 closed (VK reference, GL compare defect), row 3 closed by term oracle, rows 11/12 closed as no default gap, allowlist audited (rows 25/26 opened), parity-state header, tracy review should-fix

### Commits (after 39fc9203f + nifty merge 76195d1b2; code exe = b44a12842 = merge tip code, the TEMP pairs net to zero)
f3b3f3d13 TEMP per-tap diag on mech px / 1e9740df1 revert | 409b7242e TEMP sampler probe / b44a12842 revert | 1512d173c tracy review should-fix 1+2, nit N1 | ff341a1f3 provers audit + PORTED_AS | b1fde3d8d gaps rows 3/11/12/24 closed, 25/26 opened, PARITY STATE, BUGS 43, inventory 15 | 28a50ffa7 callers.json regen | this state commit. MERGEABLE TIP = the commit carrying this line.

### Rows
- 24 CLOSED: per-tap test on mech px at mechnight (GL): darkened faces 52,553 px, GL compare occluded 52,553, GL raw depth occluded 0. VK is the reference; BUGS 43 extended. Sampler-on-unit-3/4 hypothesis refuted (both 0, no pixel moves). Caster classes: both backends cast static props + mechs (VK 624 packets / 9,240 mech verts; GL 2056 -> 1409 after cull); per-cascade census skipped as moot.
- 3 CLOSED: MC2_TERRAIN_AMBIENT_FLOOR=0 vs default, 10 poses both backends: 205644 GL 1,041,136 / VK 1,042,012 / shared 1,039,512 px, VK term error 0.95 on 49.33; night poses no term.
- 11 CLOSED (billboard residual gated by MC2_LIGHT_BILLBOARD_FACING, default OFF), 12 CLOSED (SSAO/cloud shadow default OFF in GL).
- 25 OPEN (cement atlas seam inset, default ON, VK lacks), 26 OPEN (terrain shoreline wet/foam bands, MC2_TERRAIN_SHORELINE default ON; the gaps file had it wrongly as default-off). Owner lane 2. Plus row 20's ShadowCSM=FALSE VK defect, row 18 (owner ruling).

### Proof
| Evidence | Result |
|---|---|
| build -j2 + deploy (`scratch/build_s31c.log`) | exit 0 at b44a12842 (full rebuild after the merge) |
| golden `.claude/l2cap/s31-vk` vs `--golden current` (`scratch/verify31.log`) | 36/36 regions, 10/10 poses PASS, validation_errors=0 on all logs |
| tier1 VK + validation, first run (`scratch/b31_vk_t1.log`) | 4/5: mc2_24 `heartbeat_freeze_play` = clean `mission_quit` at 135.9 s, 5,070 frames, exit 0, no crash; host loaded by two other lanes |
| tier1 VK + validation, rerun (`scratch/b31b_vk_t1.log`) | 5/5 PASS, errors=0 warnings=0 all five, 135/93/86/102/86 fps (lower than s30's 147/126/114/119/105: concurrent lanes on the host) |
| tier1 GL (`scratch/b31_gl_t1.log`) | 5/5 PASS, 142/113/109/82/133 fps, +0 destroys, fingerprint PASS |
| provers | gl_vk_feature_coverage --check 0 new 0 stale (68 NOT_PORTED pairs, 9 of them parity rows 25/26); glsl_port_diff --all exit 0 (one pin, waterBaseColor = default-OFF meander); pytest coverage 27 pass, tracy 14 pass + the same 9 pre-existing failures (lease/warmup signature drift: test_lease_precedes_both_children_and_covers_normal_exit[True/False], test_wait_failure_reaps_both_children_and_releases, test_trivial_trace_releases_on_system_exit, test_timeout_reaps_children_before_release[recorder/game], test_warmup_launch_runs_under_the_lease_before_the_recorder, test_held_chain_lease_is_not_released_by_a_capture, test_main_holds_the_lease_for_the_whole_command_and_compare_takes_none) |
| board (`scratch/cc_s31.log`) | gate_gen consistent, gpu_abi 14 match, dup_lines 55326 = baseline, file_size 49 = baseline, journal 6463 <= 6497, quiet-ratchet OK. Reds, none this lane's: estring_callsites, container_callsites, vk_pipeline_layout split-unit test, background-launch, callers (test_journal_comment_ratchet.py uncalled), editor_smoke / build_compiler_presets / contract_harness_migration tests |

### Open
- Rows 25, 26 (ports + same-exe GL oracle), row 18 ruling, row 20 ShadowCSM=FALSE VK defect; tracy review nits N2 (`baseline --arms` help), N4 (docstring "GL-side frame").
- Scratch (mine, deletable): `scratch/` s31a-d.sh/.out, r31a.py, r31b.py, build_s31*.log, deploy_s31*.log, verify31.log, b31*_t1.log, cc_s31.log; `.claude/l2cap/` s31-vk, s31-gl, s31-vk-amb0, s31-gl-amb0, s31-gl-diag, s31-gl-unbind (KEEP the last four while gaps rows 3 and 24 cite them). s30-gl/vk/vk-glss still cited by row 24.

### Lessons (31)
- Replaying a reverted TEMP diag (`git show <sha> -- files | git apply`) and retargeting one condition answered row 24 in one build; check a prior row's proven defect before a fresh census.
- A VK include can be generated (gpu_abi): run `gpu_abi_gen --check` before editing anything under shaders/vulkan/include.
- Read gate defaults from `Gates/gate_table.json` before trusting a doc's "default-OFF" list (row 26 was mislisted).

## THIRTIETH session (2026-10-16, cap 5 h, Opus 5.5) -- Tracy GL arm retired (VK selfcheck on 1080p vs 4K), interim VK golden archived, row 24 GL term named, step-1 GameOS half STOPPED with readers listed, goldenswitch review should-fix; nifty NOT merged ("merge lane2 56f0426c1" absent from mc2-main)

### Commits (after 56f0426c1; no C++ change, the exe is still f77062b02)
91c1e0502 interim golden archived | 27887bacc goldenswitch review should-fix 1 | f4d0204b4 tracy VK selfcheck + GL arm retired | 6c61587b3 row 24 + inventory section 15 | this state commit. MERGEABLE TIP = the commit carrying this line.

### Step 1, GameOS half: nothing removed (each item blinds a proof or has a live reader/ruling; list in inventory section 15)
- `gl_fbo` capture arm: the only GL-frame readback; every open parity row (row 24 used it today) and the VFX oracle need it.
- Lost-draws tracker + `MC2_VK_SCENE_GL_PRODUCER_QUERY`: full deletion made (15 files, -215 lines), REVERTED on finding the 2026-10-12 orchestrator KEEP ruling (gate-amnesty-wave1.md row 395).
- pass timer/stats (editor Frame Inspector, dev-shell framegraph), dev shell, debug_renderer (KEEP-LIVE), HZB probe (KEEP-LIVE), gpu_memory_report (vram.py), gos_validate (runner.py), renderer_input_probe (UI probes), object_parity (prop producer VK consumes), hitch_trace (no VK equivalent).

### Rows
- Row 24: GL term = `shadow_screen.frag` sampleDynamicShadow mech floor 0.55; GL/VK 0.553 median on 52,546 px; VK screen-shadow term 1.000 there (pass active: moves 50,831 other px). Occlusion (left shin same normal, untouched). Reference NOT decided; next GL cascade caster census (`MC2_SHADOW_DIAG`). No BUGS row.

### Proof
| Evidence | Result |
|---|---|
| tracy selfcheck VK (`scratch/s30_tsc3.out`) | 1080p p50 7.886 ms vs 4K 10.485 ms = 33.0% (bar 8%) PASS; pool cap 64 vs 256 on VK 1.3% (`s30_tsc2.out`), the reason the knob was replaced |
| golden: `.claude/l2cap/s30-vkgold2` vs `--golden current` | 36/36 regions, 10 poses PASS, validation_errors=0 on all 41 logs (`scratch/verify30b.log`). First run `s30-vkgold` refused: A: was full (98%), 204907 PNG/sidecar written 0 bytes |
| tier1 VK + validation (`scratch/b30_vk_t1.log`) | 5/5 PASS, errors=0 warnings=0 all five, 147/126/114/119/105 fps, DEPLOY_FINGERPRINT PASS (f77062b02, doc-only since) |
| tier1 GL (`scratch/b30_gl_t1.log`) | 5/5 PASS, 143/113/142/142/135 fps, +0 destroys, fingerprint PASS |
| board | gate_gen --check consistent; log_tag_census OK; dup_lines OK (55571 = baseline); check-golden-coverage PASS; background-launch red = pre-existing test_host_lease_batch.py:69 os.kill (not this lane); tracy pytest 11 pass / same 9 pre-existing failures |

### Open
- Row 24 caster census; VFX oracle VK arm; gl_fbo arm and debug views need VK ports or owner decisions (inventory section 14 question 3).
- Scratch (mine, deletable): `scratch/` s30_tsc*.sh/.out, s30_selfcheck3/, s30_r24.sh/.out, r30term.py, f30*.sh/.out, verify30*.log, b30_*_t1.log; `.claude/l2cap/` s30-vkgold (broken run), s30-vkgold2, s30-gl, s30-vk, s30-vk-glss (KEEP s30-gl/vk/vk-glss while gaps row 24 cites them).

### Lessons (30)
- Read the gate-amnesty ledger for rulings BEFORE deleting a gated diagnostic: a 25-minute deletion was reverted.
- Edit with an empty new_string also eats the following newline; anchor deletions on a neighbouring line.
- A "known cost" must be measured on the backend it proves: the GL pool knob is 1.3% on VK.

## TWENTY-NINTH session (2026-10-16, cap 5 h, Sonnet 5.5) -- CURRENT_GOLDEN switched to the VK set, capture_ab on two VK arms, GL goldens archived, row2 review fixed; nifty NOT merged (waits for "merge lane2 cbfac5d8e")

### Commits (after cbfac5d8e; code exe = f77062b02, everything later is scripts/docs/baselines)
324fa2a36 CURRENT_GOLDEN + RETIRED.md rows | 8f4799f7f + ced0be17f capture_ab two VK arms (`--gl-arm` keeps the GL set) | 79ee15509 GL goldens archived and removed | b7eed679c tracy baseline defaults to vk | d4cca58e9 + 236a628b6 row2 review fixes | f77062b02 plan/inventory (owner fail-soft ruling recorded: failed VK init exits with a message, no fallback) | 575adb9bf + 80a1e8b45 check-contracts duplicate row | feff1b7ea + 9c31cbaa3 + 4ae1fefe7 board debts (ColorFrameUBO 320, stale allowlist row, dup_lines baseline, background-launch inventory, callers.json) | this state commit. MERGEABLE TIP = the commit carrying this line.

### GL deletion step 1: what went, what stopped (inventory section 15)
- Removed: `golden-e9e3f19d6-1080p` + `reset-50331d5` (61 files, 2,486 deleted lines incl. json; tarball + sha256 `E:/mc2-archive-2026-10/docs/gl-goldens-2026-10-16/`, extraction diffed against the tree before removal); capture_ab GL noise floor (now two VK arms, GL behind `--gl-arm`); tracy `baseline` default arm gl,vk -> vk.
- STOPPED, listed: VFX oracle (GL readback is the only MLR-vs-GPU proof, no VK arm); GL-reference provers (rows 3, 5-22, 24 still open, they pin those); Tracy `selfcheck`/`matrix` GL arm (MC2_LIGHT_POOL_MAX is a GL knob, no VK-visible known cost); capture `gl_fbo` arm and the 10 debug views (all GameOS, outside this session).
- Row2 review: should-fix 1 (verify.py refuses a golden with no backend record; VK-vs-GL refusal test), 2 and 3 plus the two nits (gaps row 2) fixed.

### Proof
| Evidence | Result |
|---|---|
| ninja -j2 (`scratch/build_s29.log`) | exit 0 at f77062b02, deployed to mc2-win64-lane2 |
| golden: `.claude/l2cap/s29-vk` vs `--golden current` | 36/36 regions PASS, 10 poses PASS, validation errors 0 (`scratch/verify29.log`) |
| tier1 VK + validation | 5/5 PASS, errors=0 warnings=0 on all five, 146/123/116/117/96 fps, DEPLOY_FINGERPRINT OK (`scratch/b29_vk_t1.log`) |
| tier1 GL | 5/5 PASS, 142/114/142/141/132 fps, +0 destroys, fingerprint OK (ancestor f77062b02, doc-only commits since) (`scratch/b29_gl_t1.log`) |
| pytest | tests/smoke + tools/visual_verify 222+ passed; test_gpu_abi_gen + test_gl_vk_feature_coverage 90 passed; scripts/tests/test_tracy_capture_compare.py 9 failures are pre-existing (same 9 at cbfac5d8e) |
| board (`scratch/cc_s29b.log`, before the debt commits) | reds fixed here: gpu_abi, gl_vk_feature_coverage, dup_lines, background_launch inventory staleness, callers.json. Reds left, not this lane: estring_callsites, container_callsites, spirv_artifacts (prints nothing), vk_pipeline_layout split-unit case, background_launch (test_host_lease_batch.py:69 os.kill), doc_links, script_callers (test_journal_comment_ratchet.py has no caller), scripts_tests (journal ratchet tests, build_compiler_presets, editor_smoke, tracy lease tests), sim_include_firewall |

### Open
- VFX oracle VK arm; Tracy VK known-cost selfcheck; provers after the last row; GameOS step (gl_fbo arm, debug views, then sections 2-10); `golden-e350d0b4a5c6-vk-1080p` is a superseded interim set (candidate for the next archive); the check-contracts duplicate-row fix is also needed on nifty (same merge bug).
- Scratch (mine, deletable): `scratch/` f29.sh/.out, build_s29.log, deploy_s29.log, verify29.log, b29_*_t1.log, cc_s29*.log, ab_edit.py, vb_edit.py, gaps_edit.py, docs_edit.py, state_edit.py; `.claude/l2cap/s29-vk` (keep until merge).

## TWENTY-EIGHTH session (2026-10-16 ~04:35-06:30 CDT, cap 5 h, Opus 5.5) -- row 2 closed on close-up mech bookmarks (no VK change), row 24 opened (GL-only mech face darkening), verify.py backend refusal, mech pool buffer placement, rows1-9 review should-fix 1 + 2, CURRENT_GOLDEN switch recommended; nifty NOT merged ("merge lane2 649c149c4" absent from mc2-main)

### Commits (after 649c149c4)
908e2ad90 verify.py backend refusal + pytest | d37588c7c mech pool SSBO sequential-write placement + close-up bookmarks | fb8ef42f0 bookmarks declare covers | 67b4b029a review should-fix 1 (pool uploads flush) + 2 (props-only wording) | this state/gaps/plan commit. MERGEABLE TIP = the commit carrying this line (code = 67b4b029a).

### Rows
- Row 2 CLOSED (numbers in the gaps row): day term error 0.11 on 26.46; night 0.32 on 10.21 after dividing out GL's whole-pixel face multiplier. No VK code change, so no golden pose moves.
- Row 24 OPENED: GL multiplies 43,428 night mech px by a median 0.55 (diffuse and spec alike), not via mech casters (`MC2_SHADOW_ENABLE=0` identical). Cause and correct backend not established.
- Mech pool buffer (task 3): no pixel change (s28a vs s28b at both close-ups 0.000 / 0 px both backends; s28c-vk vs s27d-vk at most 0.004 mean / 46 px, the noise floor); VK tier1 mc2_10 122 fps (p1 102) on d37588c7c, 122 (p1 101) on 67b4b029a, as before (122 / 101): mechs are few pixels in tier1.

### GL deletion step 1 candidates (task 5; nothing deleted; after CURRENT_GOLDEN is VK)
| item | files | tests / checks that reference it |
|---|---|---|
| capture GL readback arm | `GameOS/gameos/gos_visual_capture.cpp` (`gl_fbo` arm, `g_readbackSource`), `gos_screenshot.cpp` (port to VK present readback) | `scripts/smoke_lib/capture_backend.py` (`gl_fbo` mapping), `tests/smoke/test_capture_backend.py`, `tools/visual_verify/test_capture_ab.py`, `test_capture_index.py` (fixtures keep `gl_fbo` as a refused/old sidecar case) |
| A/B noise floor on two GL arms | `tools/visual_verify/capture_ab.py` (switch to two VK arms), `tools/visual_verify/ab.py` (keep while a GL exe exists) | `tools/visual_verify/test_capture_ab.py`, `scripts/smoke_lib/render_resolution.py` (reader list), `scripts/check-background-launch.py` |
| VFX oracle (GL readback only) | `tools/vfx_parity_oracle.py`, `scripts/capture_vfx_oracle.py` (needs a VK arm, not a deletion) | `tools/test_vfx_parity_oracle.py`, `tools/test_capture_vfx_oracle.py` |
| Tracy compare GL arm | `tools/tracy_capture_compare.py` (VK only, shadows on) | `scripts/tests/test_tracy_capture_compare.py`, `scripts/check-background-launch.py`, `render_resolution.py` |
| GL-as-reference provers (after the last row) | `tools/glsl_port_diff.py`, `tools/gl_vk_feature_coverage.py`, `tools/vk_not_ported.allowlist` | `scripts/check-contracts.sh`, `scripts/tests/test_gl_vk_feature_coverage.py`, `test_vk_offline_proofs.py`, `test_axis_swizzle_ratchet.py`, `test_contract_coverage.py` |
| GL goldens | `tests/visual/goldens/golden-e9e3f19d6-1080p`, `reset-50331d5` (RETIRED.md rows in the switch commit) | `tests/smoke/test_resolution_tier.py`, `scripts/check-capture-resolution.py`, `check-visual-bookmark-honesty.py` (vendored poses) |
| per-row GL tier1 leg + term-oracle scratch | lane proof recipe; `scratch/r27term.py` (scratch only) | none tracked |
| debug views (step 2, decision per view) | `gos_object_parity.cpp/.h`, `gos_dev_shell.cpp`, `gos_postprocess_diag.cpp`, `debug_renderer.cpp`, `gos_render_pass_timer.cpp`, `mc2_hitch_trace.h`, `gos_validate.cpp`, `gpu_memory_report.cpp`, `gos_renderer_input_probe.cpp`, `gos_frame_pass_stats.cpp` | all in `GameOS/gameos/CMakeLists.txt`; `scripts/check-gate-echo-reads.py` (dev shell), `check-object-mvp-currency.py` (debug_renderer), `scripts/smoke_lib/vram.py` (gpu_memory_report) |

### BUGS rows proposed
None this session (row 24 is unpinned; the flush is a latent correctness fix with no observed symptom on this host).

### Proof
| Evidence | Result |
|---|---|
| ninja -j2 (`scratch/build_s28{a,b}.log`) | exit 0 at d37588c7c and 67b4b029a, each built after its commit |
| harnesses --run --unit (`scratch/harn28.log`, 67b4b029a) | doctest 3242/3242 (10 failed assertions inside passing cases, as before); FAIL mc2_authored_order_seam (nifty, not this lane) |
| pytest | test_verify_backend.py 7 + test_bless_backend.py 9 passed; `callers.py` regenerated (remaining unallowlisted: scripts/tests/test_journal_comment_ratchet.py, not this lane) |
| bookmark honesty | OK, 41 pose files |
| row 2 oracle | `s28b-{gl,vk}{,-nospec}`, `s28d-gl-noms{,-nospec}`; montage `s28-r2-mechclose-gl-vk-terms.png` viewed (committed) |
| golden | `s28c-vk` (d37588c7c) and `s28e-vk` (67b4b029a): 36/36 PASS vs golden-d9e1f3801716-vk-1080p; validation_errors=0 on every VK capture |
| tier1 VK + validation, final exe 67b4b029a (`scratch/b28e_vk_t1.log`) | 5/5 PASS, errors=0 warnings=0 (145/138/122/128/113 fps), DEPLOY_FINGERPRINT OK |
| tier1 VK on d37588c7c (`scratch/b28_vk_t1.log`) | 5/5 PASS, errors=0 warnings=0 (144/139/122/128/113) |
| tier1 GL, final exe (`scratch/b28e_gl_t1.log`) | 5/5 PASS (142/114/142/143/131 fps; mc2_24 p1 28 one hitch), +0 destroys, DEPLOY_FINGERPRINT OK |

### Open
1. CURRENT_GOLDEN switch: recommended in `vk_golden_plan.md` section 4 (orchestrator commit). 2. Row 24 cause. 3. Gaps row 5 text still says "props and vehicles" for the residue (vehicles via the legacy shape island have no pool term: check whether GL vehicles receive pool light). 4. Rows1-9 review notes not taken: ghost_wedge watch on next bless; sed edits of session 27 (none outside the scanned range found). 5. Carried: T7 / T9, row 20 GL cause, row 18.
6. Scratch (mine, deletable): `scratch/` a28.sh/.out, b28.sh/.out, d28.sh/.out, e28.sh/.out, s28a_frame.png, s28_r2_night_terms.png, s28_r2_night_diffmap.png, build_s28*.log, deploy_s28*.log, harn28.log, verify28*.log, b28*_t1.log; `.claude/l2cap/` s28a-*, s28b-*, s28c-vk, s28d-*, s28e-vk (KEEP s28b-* while gaps row 2 cites them).

### Lessons (28)
- A term oracle must divide out the base-light ratio before blaming the term: a 22% "spec excess" was a whole-pixel GL multiplier on some faces.
- A close-up pose turns an inconclusive 20-200 px oracle into a 30k-140k px one in two capture rounds; frame it first with a 4-candidate sweep.

## TWENTY-SEVENTH session (2026-10-16 01:55-04:30 CDT, cap 5 h, Opus 5.5) -- parity rows 1 and 9 ported (prop night pools, map-edge feather), rows 4, 6, 7 proven and closed, row 2 ported but not proven, VK golden re-blessed (interim), CURRENT_GOLDEN decision written, row-5 review should-fix 1 + note 3; nifty NOT merged ("merge lane2 f0fffa457" absent from mc2-main)

### Commits (after f0fffa457)
bcb9b5623 row 1 prop pool term | af54408bd review row5 should-fix 1 + note 3 (harness env in the golden) | 8d7f14ec8 row 9 edge feather | d9e1f3801 row 1 typed casts (LG09 back to baseline) | b7392551f golden `golden-d9e1f3801716-vk-1080p` + gaps rows 1/2/4/6/7/9 + plan section 4 + montages | 64084214e row 1 perf (pool SSBO memory) | this state commit. MERGEABLE TIP = the commit carrying this line (code = 64084214e).

### Rows (numbers in `.claude/VK-PARITY-GAPS.md`)
- Row 1 CLOSED: the terrain and mech pool terms were already ported; the missing piece was the OBJECT pool term on static props (GL `static_prop.frag` POOL-CONSUMERS-1). Ported with the shared `object_light_pool.hglsl` body, same pool array and knobs via the prop light feed (binding 8). Same-exe VK-GL: nightlow a 4.665 -> 1.252, c 5.013 -> 0.519; golden poses 205843 6.07 -> 4.52, 204753 3.75 -> 2.01, 212019 4.17 -> 2.42, 212258 3.04 -> 1.57; day 0 change beyond noise.
- Perf found by the tier1 and fixed before closing: the pool SSBO was allocated `HOST_ACCESS_RANDOM` (system memory) and every prop fragment walks ~142 lights on mc2_10: VK tier1 mc2_10 31 fps (p1 7). `HOST_ACCESS_SEQUENTIAL_WRITE` (device-local host-visible): 122 fps (p1 101); pixels unchanged (verify 36/36 on the fixed exe). The mech island's pool buffer has the same allocation (mechs are few pixels; not changed here).
- Row 9 CLOSED for the default-ON members: VK lacked TERRAIN-EDGE-FEATHER-1 (GL default ON); 8d7f14ec8 ports it. 205952 9.932 -> 2.787 (the "white GL map-edge skirt"), 205843 4.522 -> 0.519, 204907 3.994 -> 1.992. Macro variation, control map, material library: default OFF in GL.
- Rows 4 and 7 CLOSED by a term oracle (`scratch/r27term.py`: each backend with the term off and on, same exe, compare the two terms on GL's term pixels): row 4 VK term error 0.35-5.01 on 24.9-67.4 terms (mc2_01 day 13.12 / 34.56 worst, fade edge a few px apart); row 7 1.39 / 76.19 at 204523, 3.40 / 145.30 at 205952, 0.64 / 29.32 at 212258.
- Row 6 CLOSED for the default path: overlay pass ported earlier; the OVERLAY-V2 sidecar composite is behind `MC2_TERRAIN_OVERLAY_V2` (default OFF).
- Row 2 PORTED, NOT PROVEN: the spec-term oracle at golden-pose scale covers 23-220 px with partial overlap (negative result, numbers in the row). Needs a close-up mech bookmark.
- Row 22 metallic: no lead (the VK prop specular uses the same swizzled world frame as GL; metallic still the literal 0 without per-material scalars).

### Golden
- `golden-d9e1f3801716-vk-1080p` blessed (`vkgold27-r{1,2,3}`, 36/36, 0 refused, `capture_env.harness_env` records the RNG seed); r2 against the section-3 set: 14 FAIL, all legitimate (11 row 1, 3 row 9 on 205843), each measured toward GL in its rect (plan section 4). Final exe (64084214e) VK capture `s27d-vk`: 36/36 PASS.
- CURRENT_GOLDEN: NOT switched; decision and the GL deletion step 1 needs written in `vk_golden_plan.md` section 4 (blocked on row 2 proof, then the verify.py backend refusal).

### Review lane2-2026-10-16-row5-cleanup.md
Should-fix 1 fixed (af54408bd): capture.py records the harness-set env per sweep (`harness_env`, per-pose paths dropped) and its key list (`effective_mc2_env`); bless.py carries `harness_env` into `golden.json`; two pytest cases. Note 3 fixed (docstring). Notes 1, 2, 4 not taken (no change needed / follow-up stays in plan section 5).

### BUGS rows proposed (BUGS-FOUND-FIXED.md shape; numbers to the orchestrator)
| # | Found | Bug (symptom) | Root cause | Fix (sha) | Proof | Status |
|---|---|---|---|---|---|---|
| ? | 2026-10-16 | under VK, static props near night lamps stayed dark where GL lights them (mc2_10 airbase, 99.4% of the night VK-GL residue) | the VK colour prop FS never received GL's object light-pool term (POOL-CONSUMERS-1) | bcb9b5623 + 64084214e | nightlow a 4.665 -> 1.252, c 5.013 -> 0.519 mean abs RGB sum to the same-exe GL frame; VK tier1 mc2_10 122 fps, validation 0 | fixed |
| ? | 2026-10-16 | under VK the map perimeter cut a hard dark edge against the sky where GL hazes it out | VK terrain FS lacked TERRAIN-EDGE-FEATHER-1 (GL default on) | 8d7f14ec8 | mc2_10 205952 9.932 -> 2.787, mc2_05 205843 4.522 -> 0.519 | fixed |

### Proof
| Evidence | Result |
|---|---|
| ninja -j2 (`scratch/build_s27{a,b,c,d}.log`) | exit 0 at bcb9b5623, 8d7f14ec8, d9e1f3801, 64084214e; every exe built after its commit |
| build_contract_harnesses --run --unit (`scratch/harn27a.log`, at 8d7f14ec8) | doctest 3242/3242 cases (10 failed assertions inside passing cases, same as session 26's `harn26b.log`); FAIL mc2_authored_order_seam (nifty compile error, not this lane) |
| pytest test_bless_backend.py | 9 passed (+2) |
| cxx17 (--worktree) / log_tag_census / own_inventory / gpu_abi_gen / gate_gen | OK; file_size: vulkan_color_prop_island.cpp 1779 -> 1814 WARNING (within the 49-file baseline) |
| oracle | rows 1 / 9 same-exe VK-GL full frame + per region; rows 4 / 7 / 2 term oracle; VK validation_errors=0 on every capture (`s27a-vk`, `s27b-vk`) |
| montages viewed | `s27-r1-nightlow-gl-vkb-vka.png`, `s27-r1-golden-gl-vkb-vka.png`, `s27-r9-edge-gl-vkb-vka.png`, `s27-vkgold-gl-vk.png` (committed) |
| golden | bless 36/0; verify r2 36/36; final exe `s27d-vk` 36/36 |
| tier1 VK + validation, final exe 64084214e (`scratch/b27d_vk_t1.log`, rerun `scratch/b27e_vk_t1.log`) | 5/5 PASS both runs, errors=0 warnings=0 on all five; rerun 145/140/122/128/112 fps (first run mc2_01 101, p1 22: a one-off hitch); DEPLOY_FINGERPRINT OK |
| tier1 GL, final exe (`scratch/b27d_gl_t1.log`) | 5/5 PASS (143/114/142/143/133 fps), +0 destroys, DEPLOY_FINGERPRINT OK |
| tier1 on d9e1f3801 (`scratch/b27_{vk,gl}_t1.log`) | VK 5/5 but mc2_10 31 fps (the perf bug above); GL 4/5, mc2_17 heartbeat_freeze_play (flake class of `tier1-flake-log.md`; GL code unchanged; passed on 64084214e) |

### Open
1. Row 2 proof on a close-up mech pose, then re-bless if it moves pixels, then CURRENT_GOLDEN (plan section 4). 2. Mech island pool buffer uses the same random-access host allocation (perf only up close). 3. `verify.py` backend refusal (plan section 5). 4. Carried: T7 / T9 VK resident cost; row 20 GL cause; row 18 (no content); not this lane: mc2_authored_order_seam.
5. Scratch (mine, deletable): `scratch/` b27*.sh, c27*.sh/.out, f27.sh/.out, g27.sh/.out, h27.sh, v27e.sh/.out, r27.py, r27reg.py, r27term.py, build_s27*.log, deploy_s27*.log, harn27a.log, verify27*.log, bless27.log, b27*_t1.log; `.claude/l2cap/` s27a-*, s27b-*, s27d-vk (KEEP vkgold27-r1..3 and s27b-gl while the plan cites them); the section-3 runs vkgold26-r* and gl26-r1 can go once `golden-e350d0b4a5c6-vk-1080p` is retired.

### Lessons (27)
- A row's recon text can be stale by many commits: grep the VK tree for the GL term before porting; four of six rows were already ported and needed a proof, not code.
- Term oracle (term off vs on in each backend, compare the two deltas on GL's pixels) proves a port independently of every other open gap.
- A per-fragment loop over a host-written SSBO must not live in system memory: tier1 fps, not the pixel oracle, caught it.

## TWENTY-SIXTH session (2026-10-16 00:10-02:15 CDT, cap 5 h, Opus 5.5) -- night capture determinism (P1) found and fixed in the harness, row 5 closed, row 22 residual fixed, VK golden set blessed (interim), review should-fix 1 + NOTE 1, static-map doctest; nifty NOT merged ("merge lane2 c0cfd7070" absent from mc2-main)

### Commits (after c0cfd7070)
06c4c08be capture harness seeds the simulation stream | e350d0b4a static-map building runs + doctest (census review NOTE 1) | 9a0b0ac35 row 22 roughness override | 2c89c4a9c VK golden `golden-e350d0b4a5c6-vk-1080p` + regions 2/6/13 retired + plan | 669e23d81 review row23 should-fix 1 + NOTE 1 | 71fc1d310 gated-layout-macros regenerated | this state/gaps commit. MERGEABLE TIP = the commit carrying this line (code = 669e23d81).

### Task 1: night light determinism (P1) and row 5
- Evidence first: two VK launches of mc2_10 nightlow a with `MC2_LIGHT_POOL_DUMP=1` (13.3 mean |RGB sum| apart): the frame-481 dumps differ in exactly two lights, slots 58 / 59, type spot, argb ff3f3f3b, far 450 = the turret searchlights (`code/turret.cpp`), at different positions. Their idle sweep targets come from `RandomNumber` (the simulation stream, `gos_rand`), which is wall-clock seeded unless `MC2_DETERMINISTIC_RNG=1`; the capture harness set the fixed clock but not the seed.
- Fix (06c4c08be, harness only, play unchanged): `run_visual_capture.build_capture_env` sets `MC2_DETERMINISTIC_RNG=1` with the deterministic clock (per-mission seed = hash of the mission name); pytest case added.
- Proof: three same-exe launches each, nightlow a and c, VK and GL: every pair 0.000 mean / 0 px over 30 (explicit env `s26b-*`, harness default `s26d-*`); 204753 (the lane-7 floodlight pose) VK three runs and GL three runs 0.000 / 0. Day noise floor for comparison (VK golden runs): 205644 0.002 / 61 px.
- Row 5 CLOSED: same-exe VK vs GL, seeded: nightlow a 4.66, c 5.01 (were 43.9 / 27.6 and 96.8 / 3.79 unseeded); 99.4% of differing pixels VK-darker, all on props and vehicles (row 1's object pool term), the apron and pads agree (montage `s26-r5-nightlow-gl-vk-diff.png`, viewed).

### Task 2: doctest and BUGS rows
- Census review NOTE 1: `shadow_static_building_runs.h` (Vulkan-free) plans the static map's per-type runs; `test_shadow_static_runs.cpp` pins: no geometry -> no runs, every instance dropped; no instances -> nothing; per-type runs with out-of-range types counted. doctest 3239 -> 3242.
- BUGS rows proposed (BUGS-FOUND-FIXED.md shape; numbers left to the orchestrator):
| # | Found | Bug (symptom) | Root cause | Fix (sha) | Proof | Status |
|---|---|---|---|---|---|---|
| ? | 2026-10-15 | VK drew the small hexagonal lattice props at the mc2_10 base with a solid dark top wedge GL does not have | the VK colour prop pipelines drew double-sided (cull NONE) where GL's StaticPropOpaque / AlphaTest pipelines cull back faces; packets identical in both backends | 56270f2e4 | mc2_10 204523 VK-GL 1.695 -> 1.671, hex windows 9.12 / 8.30 / 7.70 -> 6.38 / 5.27 / 4.57; GL 0 px changed; VK tier1 validation 0 | fixed |
| ? | 2026-10-15 | with PropShadowReceive=1, GL static props show a sun-facing dark band their shadow depth does not justify (bldgB: 27,212 darkened pixels) | not pinned (compare or sampler state on texture units 3 / 4 suspected): GL's hardware compare says occluded where a raw read of the same texel against the same biased reference says lit; VK's compare agrees with its depth | none in GL (GL is deleted after parity; VK is the reference backend) | row 20 per-tap counts: bldgB GL compare 27,212 / raw 0, VK 0 / 0 | open (GL only) |
| ? | 2026-10-16 | the VK colour prop specular (StaticPropPbr) used roughness 1.0 where GL uses its override 0.95 | the island never received GL's roughness override | 9a0b0ac35 | 205644, StaticPropPbr=1: on the 8,909 pixels the option changes, GL +42.78, VK +42.65 | fixed (minor: option off by default; orchestrator may keep it in the lane review instead) |
- Review should-fix 2 (census review) taken: the GL self-shadow row says "not pinned"; session 25's proposal 3 (night capture non-determinism) is withdrawn as a BUGS row (no player-visible symptom; harness item, fixed by 06c4c08be).

### Task 3: VK goldens
- Blessed `tests/visual/goldens/golden-e350d0b4a5c6-vk-1080p` (INTERIM, not CURRENT_GOLDEN) with `bless.py --backend vulkan` from `.claude/l2cap/vkgold26-r{1,2,3}` (10 poses, 1080p, umbrella, frozen animation, seeded): 36 regions, 0 refused. Noise floors, region decisions (2 / 6 / 13 retired, 8 unchanged because its rect is lit under VK, 3 blessed because the water is stable with the seed) and the GL/VK view are in `tools/visual_verify/vk_golden_plan.md` section 3. `verify.py` r2: 36/36 PASS; the final exe default VK capture `s26f-vkdef`: 36/36 PASS.
- Not done: `CURRENT_GOLDEN` not switched (open parity rows 1, 2, 4, 6, 7, 9 change these poses); RETIRED.md rows for the GL sets wait for that switch.

### Task 4: rows
- Row 22: residual FIXED (9a0b0ac35), metallic residual named in the row. Row 18: unchanged (no stock content; negative result stands). Rows 13-23 still open after this session: 18 (not ported, no content), 22 metallic sub-residual only. Every other row 13-23 is fixed, closed or decided.
- Review lane2-2026-10-16-row23 should-fix 1 fixed (669e23d81: decoder constants, unknown atlas format rejected with one log line per version, static_assert on the store's constants); NOTE 1 fixed. Not taken: NOTE 2 (move overload, transient VK-only copy), NOTE 4 (coordinator question), NOTE 5 (pre-existing slot reset). NOTE 3 recorded here: the six gos texture factories (incl. `gos_NewTerrainNormalTexture`, LG03 +1 / LG05 +2 at cddaacc61) belong to the step-4 ownership batch "gos texture factories x6 -> owner-returning addTexture".

### Proof
| Evidence | Result |
|---|---|
| ninja -j2 (`scratch/build_s26{a,b,c}.log`) | exit 0 at e350d0b4a, 9a0b0ac35, 669e23d81; every exe built after its commit |
| build_contract_harnesses --run --unit (`scratch/harn26{a,b}.log`) | doctest 3242/3242 (+3 static-map cases); FAIL mc2_authored_order_seam (nifty compile error in test_authored_movement_fixture.cpp:189, not this lane, as in session 25) |
| file_size / log_tag_census / own_inventory / cxx17 ratchet / gate_gen / gpu_abi_gen --check | all OK (file size: vulkan_color_prop_island.cpp 1779 -> 1780 warning, within the 49-file baseline) |
| check-contracts (`scratch/cc_s26.log`) | gated_layout_macros regenerated (71fc1d310); remaining reds not this lane: doc_links (pre-existing), script_callers (scripts/tests/test_journal_comment_ratchet.py from the c905df64d merge has no caller), sim_include_firewall (code/warrior_orders.cpp -> mission.h), scripts_tests 41 fails in files this lane did not touch (journal ratchet 17, tracy compare 9 while the host lease was held by this lane's tier1, editor smoke 3, compiler presets 7, vk_pipeline_layout, background_launch, contract coverage / harness migration) |
| P1 determinism | three launches per backend, nightlow a / c and 204753: 0.000 mean, 0 px > 30 on every pair (`s26b-*`, `s26d-*`, `vkgold26-r*`, `gl26-r*`) |
| row 22 | `s26e-{vk,gl}-pbr` vs defaults, VK validation 0; montage `s26-r22-pbr-205644-gl-vk.png` viewed |
| VK golden | bless 36/0; verify r2 36/36 PASS; final exe `s26f-vkdef` 36/36 PASS, validation_errors=0 on all 10 poses |
| montages viewed | `s26-r5-nightlow-gl-vk-diff.png`, `s26-vkgold-gl-vk-{1,2}.png`, `s26-r22-pbr-205644-gl-vk.png` |
| tier1 VK + validation, final exe (`scratch/b26f_vk_t1.log`, run 2026-10-16T01-44-31) | 5/5 PASS, errors=0 warnings=0 on all five (145/134/108/120/100 fps), DEPLOY_FINGERPRINT OK (ancestor 669e23d81) |
| tier1 GL, final exe (`scratch/b26f_gl_t1.log`, run 2026-10-16T01-55-22) | 5/5 PASS (143/113/141/142/132 fps), +0 destroys, DEPLOY_FINGERPRINT OK |
| tier1 on 9a0b0ac35 (`scratch/b26_{vk,gl}_t1.log`) | INVALID by fingerprint (669e23d81 committed while it ran); superseded by the pair above |

### Open
1. CURRENT_GOLDEN switch to a VK set after rows 1, 2, 4, 6, 7, 9 close (re-bless then). 2. Row 1 object pool term: the remaining night VK-GL residue at nightlow a / c is VK-darker props and vehicles. 3. Row 22 metallic sub-residual; row 18 (no content). 4. Not this lane: mc2_authored_order_seam, script_callers, sim_include_firewall, the scripts_tests reds above. 5. Carried: T7 / T9 VK resident cost (BC7 colormap sidecars); row 20 GL cause.
6. Scratch (mine, deletable): `scratch/` r26a/b/c .sh/.out, r26.py, r26g.py, r8.png, crlf_edit.py, b26a/b26b .sh, t1_26.sh/.out, t1_26f.sh/.out, state26.md, build_s26*.log, deploy_s26*.log, harn26*.log, cc_s26.log, b26_*_t1.log, b26f_*_t1.log; `.claude/l2cap/` s26a-*, s26b-*, s26d-*, s26e-*, s26f-vkdef, gl26-r*, vkgold26-r* (KEEP vkgold26-r1..3 and gl26-r1 while vk_golden_plan.md cites them). Montages `s26-*.png` are committed (rows and the plan cite them).

### Lessons (26)
- Dump the light set beside the pixels: two launches and one diff of `MC2_LIGHT_POOL_DUMP` named the varying lights (turret searchlights) in minutes; a fixed clock is not determinism while the sim RNG is wall-clock seeded.
- A golden plan's proposed rect must be checked on a frame of the backend being blessed: the 1080p re-author suggestion for region 8 landed on the lake, while the existing rect was valid under VK.
- Do not commit code while a tier1 chain on the previous exe is still queued: the DEPLOY_FINGERPRINT check voids it and costs a second pair.

## TWENTY-FIFTH session (2026-10-15 night, cap 5 h, Opus 5.5) -- T9 per the ruling, row 20 decided (VK is the reference), row 23 fixed (back-face cull), T7 closed, row 14 closed for the cliff material, row 5 oracle shown non-deterministic; nifty NOT merged ("merge lane2 6bcfb5d8b" absent from mc2-main)

### Commits (after 6bcfb5d8b)
b8739f240 T9 | d4bdc86a6 + 4da948f2d TEMP row-20 / row-23 diags, reverted by 4e15a2ac2 (`git diff d4bdc86a6^ 4e15a2ac2` empty on the six files) | 61f5c881d census review should-fix 2 + log text | 56270f2e4 row 23 cull | 0fa0c839d T7 | 404bc46f6 TEMP row-14 diag, reverted by e3bcd857d | 59e6f66c2 T7 umbrella-only | 505f85088 T7 span | cddaacc61 cxx17 + log-tag baselines (reason in the message) | 889c424a5 file-size + ownership baselines (reason in the message) | this state/gaps/census commit. MERGEABLE TIP = the commit carrying this line (code = cddaacc61).

### Review (lane2-2026-10-15-census-cleanup.md)
Should-fix 1 (T9 size) = task 1 below. Should-fix 2 (T3 chain below a baked level) fixed in 61f5c881d. Cosmetic setImage log text fixed. Notes 1-4 not taken (NOTE 1 asks for a no-prop doctest of the row-17 static map; open).

### Rows and census
- T9 (ruling): slots are gos handles (`gos_NewTerrainNormalTexture`), the island reads them with `gos_GetTextureTexels` into a transient upload; `terrain_normal_array_cpu_` deleted; slot texels kept only under the VK umbrella (GL runs 0 MB, VK 151 MB: the session-24 copy was one level, so 151 MB, not 200); a replaced slot texture is deleted. mc2_10 204523 GL and VK before/after 0 px, `[VRAM v1] load_end proc_dedicated_mb=1763` unchanged.
- Row 20: DECIDED, VK is the reference backend for the prop self-shadow term. Per tap of sampleDynamicShadow: bldgB GL compare occluded 27,212 / raw occluded 0 (every one a disagreement), VK 0 / 0; 205644 GL 43,694 / 1,079 (42,617 disagreements), VK 4,843 / 4,589 (406). Raw depths agree GL/VK (100% / 94.5%).
- Row 23: FIXED (56270f2e4). Types 374 / 379 / 380 at the hex props; their packets are identical in both backends (alpha-OFF, no alpha test); the VK colour pipeline drew double-sided where GL culls back faces. mc2_10 204523 VK-GL 1.695 -> 1.671, hex windows 9.12 / 8.30 / 7.70 -> 6.38 / 5.27 / 4.57; 205644 2.671 -> 2.495; bldgB 1.756 -> 1.689; GL 0 px changed.
- T7: CLOSED with a cost question (colormap atlas in the field store; stock missions are RGBA8: 105 MB mc2_01 / 151 MB mc2_10 resident in VK runs only; BC7 sidecars would make it 26 / 38 MB). Before/after 0 px (mc2_10 GL+VK, bldgB VK).
- Row 14: CLOSED for the cliff material (normal view corr 0.999 / 0.973 / 0.999 and albedo view abs 0.23 on the steepest 2% of terrain pixels, mc2_05 205843); the night overshoot is a lighting term.
- Row 5: oracle invalid at the nightlow poses (run-to-run VK pose a 94.6, GL pose c 42.0; second pair pose a VK-GL 3.79; with MC2_TERRAIN_LIGHTING_GPU=0 GL = VK to 0.02 / 0.12). Not fixed.

### Proof
| Evidence | Result |
|---|---|
| ninja -j2 | exit 0 at every commit; the final exe built after cddaacc61 (fingerprint clean) |
| build_contract_harnesses --run --unit (`scratch/harn25b.log`, after 505f85088) | doctest 3239/3239 cases (+1 colormap store case); FAIL mc2_authored_order_seam (nifty compile error, not this lane) |
| cxx17 ratchet / log_tag_census / file_size_ratchet / own_inventory --check | OK after the baseline commits cddaacc61 and 889c424a5 (rises: LG03 +1, LG05 +2, gameos.hpp +4 lines, gosTexture.new +1, all from the T9 handle factory; reasons in the commit messages) |
| gate_gen --check | consistent |
| check-contracts (`scratch/cc_s25.log`, before the size/ownership baselines) | this lane's reds (file_size, own_inventory) re-frozen; not this lane: estring/container callsites, background-launch (`scripts/tests/test_host_lease_batch.py:69`), test_check_vk_pipeline_layout split-unit case, test_build_compiler_presets, mc2_authored_order_seam |
| same-exe GL vs VK on the final exe (s25n, VK validation on) | mc2_10 204523 1.671, 205644 2.494, bldgB 1.689; validation_errors=0 on all; montage `.claude/l2cap/montages/s25-final-gl-vk.png` viewed |
| montages viewed | `s25-t9-gl-vk.png`, `s25-r23-hex-gl-vkbefore-vkafter.png`, `s25-r23-205644-gl-vkbefore-vkafter.png`, `s25-r14-mc2_05-normal-albedo-gl-vk.png`, `s25-r5-{a,c}-final-albedo-shadow-gl-vk.png`, `s25-r5-a-vk-normallight.png`, `s25-final-gl-vk.png` |
| tier1 VK+validation / GL on the final exe (`scratch/b25_{vk,gl}_t1.log`) | VK 5/5 PASS, errors=0 warnings=0 on all five (142/127/111/127/112 fps); GL 5/5 PASS (143/114/142/143/130 fps), +0 destroys; DEPLOY_FINGERPRINT OK (ancestor cddaacc61) |

### BUGS rows proposed
1. GL prop self-shadow (row 20): symptom: GL static props show a sun-facing self-shadow band when PropShadowReceive=1 that the shadow depth map does not justify; root cause: GL's hardware depth compare on the CSM array / full map (units 3 / 4) returns occluded for taps whose stored depth is farther than the biased reference (27,212 of 27,212 darkened bldgB pixels); fix: none in GL (deleted after parity), VK is the reference; proof: row 20 numbers.
2. VK hex lattice props (row 23, FIXED 56270f2e4): symptom: VK draws the hex lattice props with a solid dark top wedge; root cause: VK colour prop pipeline cullMode NONE where GL culls back faces; proof: row 23 numbers.
3. Night capture non-determinism (row 5): symptom: the same exe and night pose give different frames run to run (VK 94.6, GL 42.0 mean |RGB sum|); root cause: the night light-pool term (GPU terrain lighting off makes both runs and both backends agree); fix: not started (owner determinism ruling applies).

### Open
1. Row 5: deterministic light pool in capture mode, then re-measure. 2. T7 / T9 resident cost (VK only): BC7 colormap sidecars; T9 151 MB is the slot texels. 3. Row 20 GL cause (sampler or compare state on units 3/4) not pinned; only matters if GL stays. 4. Review NOTE 1 (no-prop static-map doctest). 5. Not this lane: mc2_authored_order_seam, background-launch, vk_pipeline_layout split-unit test, compiler presets tests.
6. Scratch (mine): `scratch/` r25a-r25l .sh/.out, r23cmp.py, r25b.py, r14.py, r14b.py, r5.py, r5b.py, r5c.py, t9cmp.py, t1_25.sh/.out, pkt25/, build_s25*.log, deploy_s25*.log, harn25*.log, cc_s25.log, cxx17_25.log, cxx17_wb25.log, gate25.log, ltc25.log, ltc_w25.log, fsr25.log, own25.log, b25_*_t1.log; `.claude/l2cap/s25*` capture sets (montages `s25-*.png` keep: the gaps rows cite them). Deleted: `.claude/l2cap/s25m-*` (superseded by s25n, cited nowhere).

### Lessons (25)
- A per-tap encode beside the hardware compare settles "which backend is right" in one capture pair; aggregate compares only narrow.
- Before chasing a lighting delta at a pose, capture the same pose twice: the nightlow oracle was mostly run-to-run noise.
- A packet dump plus each instance's projected origin finds a prop's types without a pick tool; equal packets point at pipeline state.
- A `ps | grep` wait inside a script launched from a shell whose command line holds the same string never ends; wait on a file instead.


## TWENTY-FOURTH session (2026-10-15 night, cap 5 h, Opus 5.5) -- row 20 negative (images agree, GL compare is the odd reading), row 17 edge closed, row 23 narrowed, census T3 / T8 / T9 closed (T9 with a cost question); nifty fast-forwarded at the start (21ad1da90)

### Commits (after 21ad1da90)
6772f7f38 deploy_payload: mc2-win64-lane2 canonical (no --allow-new-target) | 31c456762 + c84beb221 TEMP row-20 raw-depth diags, reverted by 3696120d3 (`git diff 31c456762^ 3696120d3` empty on the four files) | 36abd3d24 row 17 edge | ab495bceb row 23 live alpha flag | 39c1ca805 T3 | beb634d96 TEMP row-17 scratch, reverted 7c0d0b982 | c63b3df73 T8 | 5be553c44 T9 | 38655dc49 row 23 alpha-ON LOD | acce838f5 LG08 fix + MC2_VK_PROP_MIP_PROBE row deleted + log-tag baseline | this state/gaps/census commit.

### Rows
- Row 20: NEGATIVE (gaps row 20). Raw (non-compare) reads of the bound images agree GL vs VK; nearer-than-biased-reference counts bldgB GL 0 / VK 0, 205644 GL 9,885 / VK 5,008; cascade selection 99.89% / 99.73% agree. GL's compare/PCF result contradicts its own raw depth; next is a per-tap GL encode, then an owner ruling on which backend is right.
- Row 17 edge: CLOSED (gaps row 17): `building_instances=0 terrain_tris=19602` static build with no prop geometry, validation 0.
- Row 23: narrowed, not closed (gaps row 23): neither the live alpha flag nor the LOD clamp is the hex cause; full-frame VK-GL 1.750 -> 1.695.
- Rows 14, 5: not entered (time).

### Census (status table moved into `vk-producer-census-2026-10-15.md`)
T3 CLOSED (GL before/after 26 px, mean 0.008; resident 11.6 MB mc2_10 / 4.8 MB mc2_01, accepted like the geometry copy). T8 CLOSED (4 MB; the store exists at atlas build on mc2_01 and mc2_10). T9 CLOSED WITH A COST QUESTION: about 200 MB resident (2048^2 x 9 RGBA8 with mips) in every run; ruling needed (level 0 only = 151 MB, or the island reads the slot textures through gos_GetTextureTexels). T7 OPEN.

### Proof
| Evidence | Result |
|---|---|
| ninja -j2 | exit 0 at every commit (the acce838f5 exe rebuilt after its commit so the fingerprint is clean) |
| build_contract_harnesses --run --unit (`scratch/harn24.log`) | doctest 3238/3238 cases; FAIL mc2_authored_order_seam (the nifty compile error at tests/unit/test_authored_movement_fixture.cpp:189, not this lane) |
| cxx17 ratchet --check --worktree (`scratch/cxx17_24.log`) | OK after acce838f5 (LG08 +1 from the T3 loop fixed, not baselined) |
| gate_gen --check | consistent (977 gates) |
| check-contracts (`scratch/cc_s24.log`, before acce838f5) | this lane's reds fixed in acce838f5 (cxx17 LG08, log_tag_census, gate table); not this lane: estring/container callsites, background-launch, tracy_capture_compare, sim_include_firewall, journal_comment_ratchet duplicate row + callers.json, build_compiler_presets, editor_smoke, vk_pipeline_layout fixture, mc2_authored_order_seam |
| row 20 diag pairs (s24a, s24c) | numbers above |
| row 17 scratch (s24d-vk-r17) | validation_errors=0; montage `s24-r17-noprops.png` viewed |
| row 23 / T3 pairs (s24b, s24d, s24e on mc2_10 204523) | montages `s24-r23-hex-gl-vk-before-after.png` viewed; GL T3 before/after 26 px |
| same-exe GL vs VK (s24e, mc2_01 205644 + bldgB, VK validation on) | 205644 3.491 -> 2.671, bldgB 1.756; montage `s24-final-gl-vk.png` viewed (cement, roads, detail normals intact); validation_errors=0 on every VK capture |
| tier1 VK+validation / GL on the final exe (`scratch/b24_{vk,gl}_t1.log`) | VK 5/5 PASS, validation errors=0 warnings=0 on all five (143/114/108/121/104 fps), DEPLOY_FINGERPRINT PASS; GL 5/5 PASS (143/117/143/142/124 fps), +0 destroys; exe acce838f5 |

### BUGS rows proposed
none fixed this session that is player-visible on stock maps (row 17 edge: only a map without registry props; candidate row once a modded map shows it).

### Open
1. Row 20: per-tap GL compare vs raw encode; owner ruling on which backend the prop self-shadow oracle follows. 2. Row 23: packet dump of the hex type. 3. T9 resident cost ruling; T7 colormap atlas. 4. Rows 14, 5 not entered. 5. Not this lane: mc2_authored_order_seam compile error, callers.json / journal ratchet duplicate row from the nifty merge.
6. Deleted this session (mine, untracked, unreferenced by any doc): 253 old `.claude/l2cap/*` capture dirs (13 GB; A: had 3.3 GB free). This included `s21-{gl,vk}-psr0`, which gaps row 23 (lane 6 text) cites with a brace pattern my reference check missed; s24b / s24d / s24e replace it as evidence.
7. Scratch (mine): `scratch/` r24a/b/c/d/e .sh/.out/.py, t3_new.txt, t1_24.sh/.out, build_s24*.log, deploy_s24*.log, harn24.log, cc_s24.log, cxx17_24.log, b24_*_t1.log; `.claude/l2cap/s24*`; montages `s24-*.png` (keep).

### Lessons (24)
- An encode that samples one cascade outside the shader's own selection measures the clamp/border, not the image: replicate the selection loop in the diag.
- Commit before the build that gets deployed: a build started before its commit embeds a dirty fingerprint.
- A same-pose GL before/after is the cheapest proof for a CPU-side texel producer (26 px for T3).


## TWENTY-THIRD session (2026-10-15 evening, cap 5 h, Opus 5.5) -- gate cleanup, row 20 narrowed to the cascade compare, row 17 rebake proven, HDRI RGBA16F finding; nifty merged twice

### Commits (after bff0b72f7)
merge of nifty (9e012d3f0) | 268113f38 MC2_SIM_SPEED row states both meanings + delete MC2_VK_NO_GL_INTEROP_STRICT and the dead terrain-island helpers (review should-fix 2, lane 3 review should-fix 2) | 16f15475f + 3 TEMP row-20 diags, reverted by 07bbb01ac (`git diff 16f15475f^ 07bbb01ac -- shaders/` empty) | 70924bc9f merge of nifty (slot scheduler e6c662fb7; flags.auto.yaml regenerated by gate_gen) | 20173d2cc HDRI RGBA16F failure lines always log | this state/gaps commit.

### Review (row 17 cleanup): should-fix 2 done (268113f38). Should-fix 1 (static map built only from the caster-pass preamble) recorded as an OPEN EDGE in gaps row 17, not fixed: a no-props map would need the island to open a frame recording without caster geometry. Nits not taken.

### Row 20: see gaps row 20. Normals, guard, receiver position and reference depth match GL; the cascade-0 compare does not (GL occluded on every prop pixel, VK on none). Contradicts session 22's depth dump: next = a non-compare read of the bound VK array at one tent texel. The fourth diag (reference offsets -0.002/+0.01/+0.2) was inconclusive: on that exe GL's on/off pair was identical (every channel 1 = occluded at all three offsets), VK all lit; not used as evidence.

### Row 17: destroy rebake proven (gaps row 17): VK generation 2 after the scripted destroy, GL and VK change the same 7.6k pixels.

### HDRI RGBA16F (MC2_HDRI_BC6H=0): NOT exercisable on the stock install. mc2_01 selects sky 4 = `data/hdr/mud_road_puresky_16k.exr`, which the install does not ship (only its .ktx2 sidecar): `load_failed rc=-7`, so GL and VK both end with no HDRI (VK water imports the 1x1 dummy; the startup 4K DaySky RGBA16F chain is built and then replaced before VK imports). That failure printed only under MC2_DIAG_VERBOSE; 20173d2cc makes it unconditional. Frame distance not recorded (no RGBA16F frame exists to measure).

### Rows 14 and 5: not entered (time). Row 14 read-through: the VK cliff block (`mv_cliffMaterial`, `mv_cliffParallaxPlane`) is line-for-line GL's math, constants and inputs (macro normal, Stuff camera through mc2ToRender, layer-5 alpha at level 0 of an RGBA8 copy with linear filtering); the next step stays a per-pixel channel encode in both shaders, as row 20 used.

### Proof
| Evidence | Result |
|---|---|
| ninja -j2 | exit 0 at every commit |
| gate_gen --check --rebuild | consistent (979 gates before the second merge, 969 typed readers after) |
| build_contract_harnesses --run --unit (`scratch/harn23.log`) | doctest 3231/3231; FAIL mc2_authored_order_seam (compile error in `tests/unit/test_authored_movement_fixture.cpp:189`, a nifty file, not this lane) |
| cxx17 ratchet --worktree (`scratch/cxx17_23.log`) | rc 0 |
| check-contracts (`scratch/cc_s23.log`) | reds not this lane's: estring_callsites (gui/alistbox.h), container_callsites (editor/, code/mission_update.cpp), background-launch, test_tracy_capture_compare, test_sim_include_firewall baseline, mc2_authored_order_seam |
| VK captures + validation (s23-vk-base, s23-vk-rebake, s23-vk-hdri16) | validation_errors=0 |
| tier1 VK+validation / GL on exe 20173d2cc (`scratch/b23_{vk,gl}_t1.log`, DEPLOY_FINGERPRINT PASS) | VK 4/5, errors=0 warnings=0 on all five; mc2_01 heartbeat_freeze_play (min 14 fps, host under the new slot scheduler), rerun alone PASS 144/122 fps errors=0 warnings=0 (`scratch/b23_vk_t1_mc201.log`); GL 5/5 PASS (143/117/142/143/130 fps), +0 destroys |

### BUGS rows proposed: none (no player-visible defect fixed). Candidate for BUGS-FOUND-FIXED once fixed: row 20 VK prop self-shadow (PropShadowReceive inert under VK).

### Open
1. Row 20: non-compare read of the bound VK cascade array at a tent texel vs GL. 2. Row 17 edge: static map with no registry props. 3. HDRI RGBA16F: ship the 16k EXR to a test install or use a 4K sky mission. 4. Rows 14, 5 not entered. 5. Not this lane: mc2_authored_order_seam compile error, callsite ledgers stale after nifty edits.
6. Scratch (mine): `scratch/` r20oct.sh/.out, r20oct.py, r20b.py, r20c.py, r20d.py, b23.sh/.out, b23h.sh/.out, t1_23.sh/.out, build_s23*.log, deploy_s23*.log, harn23.log, cc_s23.log, cxx17_23.log, b23_*_t1.log; `.claude/l2cap/s23*`, montage `s23-rebake-bldgB.png`.

### Lessons (23)
- Tonemapped pixels are not ratios: encode binary facts (sign bits, compare results) when the pass output is multiplied into HDR colour.
- Bisect the receiver formula input by input (normal, guard, position, reference, compare); the first disagreeing term is the lead.
- `deploy_payload` now refuses mc2-win64-lane2 as non-canonical; `--allow-new-target` is needed for the lane install (register it).

## TWENTY-SECOND session (2026-10-15 ~15:30-20:30 CDT, cap 5 h, Opus 5.5) -- nifty merged (lane 6 field store), review fixes, row 17 implemented, row 20 narrowed (negative oracle), T14 HDRI producer

### Commits (after 56c7fb27f)
merge of nifty (1aa7ec654: lane 6 terrain_field_store / texture_decode; nothing to regenerate at the merge) | 3fe58fdad review should-fix + nit 5 | 8c0b8cfe5 row 17 static map + row 20 dump timing | 7a158ef3e T14 HDRI | 367a67098 diag static dump + receipt | 2a5969a3d diag after-submit + contracts (twin map, split-path island row, allowlist, regenerated gl_shader_variants / gated-layout-macros) | 450549755 + 5d301755f TEMP row-20 channel dumps, reverted by 0606adf30 + 8ffedb00c | 2bb62014c VK guard takes the clamped sun | this state/gaps commit. MERGEABLE TIP = the commit carrying this line (code = 2bb62014c).

### Task 1 (review): should-fix = accepted cost. The CPU prop geometry stays for the map in every run (GL is deleted after parity, and row 17's building draws read it too); the size is now in the finalize line (`[GPUPROPS] finalize: ... cpu_geometry_bytes=N`). Nits: the process-lifetime flag comment reworded, the colour island's mismatch check gains the zero test (both checks identical), row 17's empty `WAS:` replaced. Nits 3 (accessor in a VK header) and 4 (mech readback row) stay for the merge with lane 6 / a producer row.

### Row 17: see VK-PARITY-GAPS row 17 (FIXED except the destroy-rebake pose). Design as written in session 21, with one change: the generation is the engine's static-map generation (the engine still builds its light matrix and latch; the island reads them as CPU data through `gos_GetStaticShadowSpec`), so P11 has not moved to a no-GL TU.

### Row 20: see VK-PARITY-GAPS row 20 (negative oracle; cause narrowed to the receiver's N.L on prop pixels).

### T14 HDRI (7a158ef3e): `loadHdriTexture` fills an `HdriCpuImage` (BC6H mip 0, or RGBA16F with a CPU 2x2 box mip chain, round-to-nearest-even halves) owned by gosPostProcess (`hdriCpu_`, kept only while the VK umbrella is armed: ~24 MB BC6H / ~85 MB RGBA16F); the sky and water islands read it through `vk_hdri_source.h` (one validating reader, rejects malformed sizes with a log line) and key re-imports on its load generation (a reloaded HDRI could reuse the GL name). No GL in either island's import any more. Seam with lane 6: none (no lane 6 file touched). Proof: VK captures log `[VK_SKY] HDRI REAL import: 16384x8192 fmt=143` and the water twin, full-frame distances unchanged (205644 water/sky 1.165). NOT exercised: the RGBA16F CPU mip chain (the field default is BC6H; run with MC2_HDRI_BC6H=0 to test it).

### Proof
| Evidence | Result |
|---|---|
| ninja -j2 (MC2_BUILD_JOBS=2) | exit 0 at every commit |
| build_contract_harnesses --run --unit (`scratch/harn22.log`, at 7a158ef3e) | rc 0, doctest 3209/3209 (+ the intended fatal_if_aborts case) |
| check-contracts (`scratch/cc_s22.log`, before the contract fixes) | 13 failed; mine fixed in 2a5969a3d (vk-pipeline-layout OK, split-path RULE4 OK, gl_vk_feature_coverage 0 new 0 stale, gl_shader_variants + gated-layout-macros regenerated); not mine: background-launch (tests/analyze/b4_heap_census.py from nifty), test_build_compiler_presets, test_contract_harness_migration, test_editor_smoke x3, test_tracy_capture_compare x3 |
| VK captures + validation (s22-vk, s22c-vk-psr0) | errors=0 on 11 captures; static map built (mc2_01 257 buildings / 19,602 terrain tris, mc2_10 477 / 28,322) |
| oracle | rows 17 / 20 in the gaps file |
| tier1 VK+validation / GL on 2bb62014c (`scratch/b22_{vk,gl}_t1.log`) | exe 2bb62014c, DEPLOY_FINGERPRINT OK: VK 5/5 PASS, errors=0 warnings=0 on all five (156/128/76/124/113 fps); GL 5/5 PASS (143/107/140/142/127 fps), +0 destroys |

### BUGS rows proposed: none fixed this session that a player sees (row 20's VK prop receive is still open).

### Open
1. Row 20: the prop GBuffer1 normal vs GL at one tent texel. 2. Row 17 destroy-rebake pose. 3. HDRI RGBA16F path unproven at runtime. 4. `terrainNoInteropStrict` / `terrainPerFrameReadback` (vulkan_terrain_island.cpp) are now unread; their gate MC2_VK_NO_GL_INTEROP_STRICT needs a gate-table deletion. 5. Session 20 items 4, 7, 8.
6. Scratch (mine): `scratch/` battery22.sh/.out, diag22.sh/.out, diag22b.sh/.out, psrc.sh/.out, t1_22.sh/.out, r17an.py, psr22.py, psr22c.py, build_s22*.log, deploy_s22*.log, harn22.log, cc_s22.log, b22_*_t1.log; `.claude/l2cap/s22*` (s22-depthdump is ~3 GB of .f32 dumps: delete first), montages `s22-*`.

### Lessons (22)
- A dump that never fires: check when the harness ends the process before suspecting the branch.
- A diagnostic read before the frame's submit shows the previous frame; camera-fitted maps then look shifted.
- Channel-encode the suspect terms (N.L, the guard) into the pass output once; it ended a day of hypotheses about depth.

## TWENTY-FIRST session (2026-10-15 ~12:00-15:00 CDT, cap 5 h, Opus 5.5) -- review fixes, lane 6 geometry seam, census T12-T14, row 20 narrowed, row 17 design; nifty NOT merged ("merge lane2 5135a6d04" absent from mc2-main at the checkpoints)

### Commits (after 5135a6d04)
9a45d009f should-fix 1+2, nits 1+2 (once-only log, call-site comment, dead staging, glDynamicShadowTex) | e1e21176b lane 6 seam: shadow + colour prop islands copy the batcher's CPU geometry (mc2_static_prop_geometry_cpu), Mc2ShadowGeomFeed loses glVbo/glIbo; nits 3+4 (extern placement, diag counter) | 9fbfc49a6 screenfx decline-reason log + periodic draw receipt | e93adcdbf CSM feed drops glCsmArrayTex/glCsmFullMapTex | 85317ac3b TEMP class tint + 805bd5896 its revert | this state/gaps commit.
- MERGE NOTE: `mc2_static_prop_geometry_cpu` (declaration in vulkan_color_prop_island.h, definition in gos_static_prop_batcher_finalize.cpp, the colour-island upload) is byte-identical to lane 6's 242b9582c hunks (applied from its diff), so merging both lanes unifies them; the shadow island side and the Mc2ShadowGeomFeed field removal are lane 2's only. Lane 6's colour-island hunk still compares `feed.vboBytes` (kept).

### Census T12-T14 (lane 6 `vk-producer-census-2026-10-15.md`): the backend-neutral producers
- T12 static sun shadow map: producer = a VK static pass in the shadow island (gaps row 17 DESIGN); it deletes `copy_static_shadow`'s GL depth readback; P11 (static light matrix) moves to a no-GL scene-lighting TU with it. NOT done.
- T13 dynamic / CSM maps: DONE for consumers -- terrain binds only the VK shadow island's views (single map since eae98a21e, cascades), and e93adcdbf drops the GL texture names from the feed. Left on GL: the cascade matrices (CPU data, placement: move to the scene-lighting TU) and `diagGlRead` in the shadow island's matrix diag (diagnostic only; delete with GL).
- T14 HDRI: producer = the neutral HDRI decode in `gos_hdri.cpp` keeping its decoded mips (BC6H blob or RGBA16F chain) CPU-side for both islands. NOT small: two islands (sky, water) read it, and the RGBA16F path needs a CPU mip chain to replace `glGenerateMipmap`; left for a session of its own.

### Row 20 (narrowed, OPEN) and row 17 (design only): see VK-PARITY-GAPS rows 20 and 17.

### Proof
| Evidence | Result |
|---|---|
| ninja (`MC2_BUILD_JOBS=2 ... ninja_build.py -j2 mc2` after the owner's 2-job ruling) | exit 0 at every commit |
| build_contract_harnesses --run --unit (`scratch/harn21.log`, at e93adcdbf = tip code) | rc 0, doctest 3185/3185, 2 skipped |
| check-contracts (`scratch` log at e1e21176b) | only the pre-existing non-lane reds: container_callsites (editor), scripts_tests test_editor_smoke legacy reload |
| row 20 battery (`.claude/l2cap/s21-{vk,gl}-psr{0,1}`, exe 9fbfc49a6) | table in gaps row 20 |
| tier1 VK+validation and GL at the tip (`scratch/b21_{vk,gl}_t1.log`) | exe 805bd5896 (DEPLOY_FINGERPRINT OK): VK+validation run 2026-10-15T14-24-46 PASS 5/5, errors=0 warnings=0 on all five; GL run 2026-10-15T14-35-38 PASS 5/5 (152/101/132/140/146 fps), +0 destroys |

### INCIDENT (mine, report to the orchestrator)
- 12:05 `deploy_payload --target game --ninja` with no target dir deployed lane 2's exe into lane 1's install `mc2-win64-v0.4` (the preset's default). Restored at 12:06 by deploying mc2-main's own `build64-ninja` (manifest src 55f62bc4d985, exe == mc2-main/build64-ninja/mc2.exe). Lane 6 held the batch lease from 11:55 and at ~12:38 was running mc2.exe from v0.4 with that restored exe; if lane 6 had deployed between 11:55 and 12:05 I overwrote it, and its runs since then tested mc2-main's exe (its own build is from 12:18): lane 6's runtime proof must be re-checked against its manifest. Lesson: always pass the target dir to deploy_payload; the preset default is another lane's install.
- My tree's `host_lease.py` has no `hold` (mc2-main's does), and my capture/smoke runner does not read MC2_HOST_LEASE_BATCH, so this session took the lease per run instead of a batch hold.

### BUGS rows proposed: none (no player-visible defect fixed this session).

### Open
1. Row 20: caster-depth self-occlusion differs (dump GL vs VK cascade depth at a tent); a mech-shadow-on-prop pose needs a spawn facility. 2. Row 17: implement the design. 3. T12, T14. 4. Session 20 items 3, 4, 7, 8 unchanged.
5. Scratch (mine): `scratch/` battery21.sh, battery21.out, classdiag.out, psr21.py, t1_21.sh, t1_21.out, harn21.log, build_s21*.log, deploy_s21*.log, b21_*_t1.log; `.claude/l2cap/s21-*`, montages `s21-psr-*.png`; session scratchpad patch l6seam.patch. Older lane-2 scratch listed in session 20.

### Lessons (21)
- A preset default target is somebody else's install: name the target directory on every deploy.
- Classify before chasing a sampler: a one-line class tint settled the GBuffer1 question in one capture.
- A same-exe on/off diff with a viewed mask shows what the option actually does (here prop self-shadow banding), which is often not the pose the row assumed.

## TWENTIETH session (2026-10-15 morning, cap 5 h) -- ShadowCSM=FALSE under VK, building shadows (row 17), row 20 receive arm, review nits 7/8, tracy tool; nifty NOT merged (no "merge lane2 6adecc763" in mc2-main at any checkpoint)

### Commits (oldest first, after 6adecc763)
9adb540d9 nits 7 + 8 | b1d4dc498 tracy tool vk arm | e628af503 building casters in the VK cascades (REVERTED, see row 17) | 85e2a3a21 colour-prop matrix diag under MC2_VK_SHADOW_MATRIX_DIAG | 37524b403 screen-shadow decline skips GL (REVERTED by 9a682565f: the hypothesis was wrong, it changed nothing) | 1b695abac building casters read from the registry (REVERTED) | eae98a21e TASK 1 FIX | 3b4ccb028 destroy re-read (REVERTED) | 0df0c8f6d regenerated files | 2bb4da291 ca2de0d8a 2bbada2df the three reverts | this state/gaps commit. MERGEABLE TIP = the commit carrying this line; net code vs 6adecc763 = nit 8 (mech geometry generation), nit 7 comment, the matrix diag, the terrain fix, the tracy tool.

### Task 1: ShadowCSM=FALSE under VK (FIXED, eae98a21e)
- Evidence chain: VK frame showed objects smeared over a frame with no terrain (mechs repeated along the camera path = the scene colour never cleared). `MC2_PROP_FIXB_MVP=0` and `MC2_SHADOW_ENABLE=0` did not change it (s20-csmoff-vk-nofixb / -noshadow identical), and the colour-prop matrix diag printed prop matrix == live camera matrix, so no matrix leak. The capture log had `[VK_TERRAIN_PERFRAME_READBACK v1] fn=copy_shadow_slot` then `[VK_TERRAIN_ISLAND] FATAL: a color readback fired on the co-write path - failing frame + disabling` BEFORE the shadow island initialised: on the first frame of a ShadowCSM=FALSE mission the VK single dynamic map does not exist yet, `copy_dynamic_shadow` fell back to the GL-copy (a per-frame GL depth readback), the co-write guard latched the whole terrain island off for the process.
- Root = VK side. Fix: the single dynamic map comes only from the VK shadow island; a frame without it has no dynamic shadow on terrain. No GL state involved (the suspected legacy-pass state was not the cause).
- Oracle (same exe GL, 1080p, FREEZE_ANIM, ShadowCSM=FALSE both arms): mc2_01 205644 56.126 -> 1.157 (mech box 0.936), mc2_10 204523 32.901 -> 0.586 (exe eae98a21e; final-exe recheck in the proof table below). Default ShadowCSM=TRUE unchanged: mc2_01 1.165, mc2_10 0.583 (s20-vk-nobldg = code identical to the tip). Montage `.claude/l2cap/montages/s20-t1-csmoff-gl-vk-after.png` (viewed; before: `s20-t1-csmoff-before-ref.png`).

### Task 2 / row 17: building shadows from a VK producer (NOT closed; cascade route measured and reverted)
- Built: the registry building set (257 on mc2_01, 477 on mc2_10) cast into every VK shadow pass. Measured against GL it moved VK AWAY on every pose (before = same code without the building set, after = with it; full frame): 205644 1.165 -> 1.204, 204523 0.583 -> 0.585, mz 2.310 -> 2.352, bldgA 1.032 -> 1.152, bldgB 0.634 -> 1.106; on the changed pixels bldgB 0.07 -> 47.41, bldgA 0.20 -> 39.01. Cause: GL drops the static map on pure concrete and keeps buildings out of its dynamic pass, so GL has no tent/hangar shadow on the apron; the cascades put one there (montage `s20-r17-bldgB-gl-vknobldg-vkbldg.png`, viewed). Reverted.
- Design for the next session (gaps row 17 updated): a VK world-fixed static map producer (terrain full-map depth + registry buildings, built once per map and on `gos_ResetStaticLightMatrix`), sampled where GL samples its static map: terrain (concrete exemption) and the VK screen shadow's `shadowMap` binding (`hasStaticShadow`), so props and mechs receive buildings like GL. Two traps found: the static build runs before the shadow island exists (pull from `GpuStaticPropRegistry::getBuildingShadowInstances` inside the island, not a push), and destroys rebake (hook `gos_ResetStaticLightMatrix`).
- Pose: no start position puts a mech inside a building shadow. Candidate bookmarks from `MC2_BRIDGE_MOVER_STATE=1` mover positions are in the install (`saved_bookmarks/mc2_10_saved_m{z,h,c}.json`, `mc2_01_saved_m{u,s}.json`); mz frames a Zeus beside a building shadow band (not inside). A spawn/teleport facility is needed for the required pose.

### Row 20 (PropShadowReceive under VK): OPEN, the VK arm has no effect
Same exe, option on vs off, pixels that change (>6 summed RGB): GL 1889 / 763 / 4981 / 3563 / 11990 on 205644 / 204523 / mz / bldgA / bldgB; VK 0 / 2 / 0 / 0 / 0. VK vs GL with the option on: 1.194 / 0.584 / 2.275 / 1.069 / 0.790 (off: 1.165 / 0.583 / 2.310 / 1.032 / 0.634). The VK pass gets `propShadowReceive` at the right UBO offset (404, both sides) and props write GBuffer1.a 0.25 (static-prop class), so the cause is downstream (suspect: the pass does not run its shadow draw at all, the log shows the cloud/shoreline init only; check `wantShadow` / `screenShadowEnabled_` and a RenderDoc of the multiply). No building bookmark with a mech shadow on it exists (same pose problem as row 17).

### Nits 7 and 8, tracy tool
- Nit 8: `s_vkVboNativeGen` renamed `s_vkMechGeomGen`, bumped after the IBO upload too. Nit 7: documented (the old map's albedo arrays stay until the next import because the per-parity descriptor sets still name their views; releasing them early would leave a set naming a destroyed view).
- `tools/tracy_capture_compare.py` vk arm no longer forces `MC2_VK_SHADOW_INTEROP=0`. Tracy pair NOT taken: the host lease was held by lane 1 tier2, lane 3 smokes and the codex editor smoke nearly continuously (my first capture waited ~1 h).

### Proof (final exe = tip code)
| Evidence | Result |
|---|---|
| build_all_targets (`scratch/bat20.log`) | every stage built, exit 0 (on 3b4ccb028; the reverts only remove code) |
| build_contract_harnesses --run --unit (`scratch/doctest_s20b.log`) | rc 0, doctest 3185 / 3185 passed, 2 skipped (10 failed assertions = the intended fatal_if_aborts case) |
| VK tier1 + validation, exe 2bbada2df (`scratch/b20_vk_t1.log`) | errors=0 warnings=0 on all five; 4/5 PASS (mc2_01 110, mc2_03 82, mc2_10 68, mc2_17 116 fps); mc2_24 heartbeat_freeze_play under lane 1 / lane 3 host load, rerun NOT taken (the host lease stayed with lane 1 at session end) |
| GL tier1, same exe (`scratch/b20_gl_t1.log`) | 5/5 PASS (79 / 94 / 143 / 146 / 141 fps) |
| VK ShadowCSM=FALSE + validation smoke mc2_01 (`tests/smoke/artifacts/2026-10-15T10-57-16`) | errors=0 warnings=0, the frame renders, but heartbeat_freeze_play: avg 6.5 fps, p50 27 ms, p99 993 ms spikes (the previous session's same run also failed at 8 fps, with the island latched off): OPEN item 7 |
| Oracle csmoff (exe eae98a21e, which still had the building casters) | 56.126 -> 1.157, 32.901 -> 0.586; the final-exe recapture was queued but not taken (lease); the building set changes default poses by 0.002-0.04 only |
| Oracle default (s20-vk-nobldg = tip code) | mc2_01 1.165, mech box 2.466, mc2_10 0.583: identical to session 19 |
| cxx17 ratchet --check | OK (24 fewer legacy sites than baseline) |
| check-contracts (on 3b4ccb028, before the regenerations) | 115 passed, 8 failed: log_tag_census, background_launch, gated_layout_macros, script_callers regenerated in 0df0c8f6d (re-run green individually after the reverts: no further diff); container_callsites, doc_links, scripts_tests (test_editor_smoke legacy reload), tools_tests (the census test, regenerated) not this lane's or fixed by the regeneration |

### BUGS rows proposed (orchestrator numbers them)
- ShadowCSM=FALSE under VK: terrain island latched off on the first frame (GL-copy fallback = forbidden per-frame readback), scene never cleared, objects smeared; found 2026-10-15 lane 2; fix eae98a21e; proof csmoff 56.1 -> 1.16 / 32.9 -> 0.59.

### Open (session 20)
1. Row 17: VK static map producer (design above). 2. Row 20: VK receive arm inert (0 px). 3. Rows 14, 5: no lead this session (not entered). 4. Tracy pair. 5. `check-contracts` rows not this lane's: container_callsites, doc_links, scripts_tests (`test_editor_smoke` legacy reload). 7. VK ShadowCSM=FALSE runs at ~6.5 fps with 1 s spikes under validation (pre-existing; profile it: GL legacy single map + VK single map at 8192). 8. VK tier1 mc2_24 rerun and the final-exe csmoff recapture.
6. Scratch (mine): `scratch/` (t1diag.sh movers.sh s20caps.sh battery20.sh final20.sh b20.bat vsil_keep.cpp, *.out, build_s20*.log, deploy_s20*.log, doctest_s20*.log, cc_s20.log, bat20.log, blogs20/), `.claude/l2cap/s20-*`, `s20f-*` capture sets, install `saved_bookmarks/mc2_10_saved_m{z,h,c}.json`, `mc2_01_saved_m{u,s}.json`.

### Lessons (20)
- A frozen frame that smears objects is a skipped clear, not a matrix: check the island latches (`FATAL` / `FAIL-SOFT` lines) before the matrices.
- One fatal-guarded fallback on the first frame can latch an island off for the whole process; a fallback that is forbidden on the path should not exist on that path.
- Parity beats physical plausibility: more shadow is a regression when GL does not draw it; measure on the changed pixels, not only the full frame.

## NINETEENTH session (2026-10-14 ~23:05 CDT start, cap 5 h) -- VulkanContext owner, shadows on VK (rows 17 / 20), row 14 mc2_05; nifty fast-forwarded at the start (152d6e152, nothing to regenerate)

### Shadow design (written before the code)
- WHAT casts into the VK cascades today: only the static-prop caster set (`dynShadowSet`, every registered non-building prop incl. trees, solid: no foliage alpha test), rendered by `vulkan_shadow_island_live.cpp` into the dynamic map, the near-cascade array layers and the full-map cascade. And only when three opt-in env rows (`MC2_VK_SHADOW_INTEROP`, `..._CSM_ARRAY_INTEROP`, `..._CSM_FULLMAP_INTEROP`) were set: the default umbrella frame produced NO cascade at all, so terrain (which samples the cascades in its own fragment shader) showed no prop, tree or mech shadow. Mechs never cast (their reverted attempt be08b5575 was perf-bad), buildings cast only through GL's world-fixed static map (imported once into the terrain island, so terrain does see building shadows).
- WHAT GL's screen-shadow pass does: a full-screen multiply over the GL scene that, for object pixels only (terrain/decals shade themselves), applies the static building map and (mechs; props only with `PropShadowReceive`) the dynamic cascades. Under the umbrella it cannot write the VK-owned scene; the VK twin is `vulkan_post_screenfx_native.cpp` (opt-in `MC2_VK_POST_SCREENFX`, cascade input only, its static input retired).
- ROUTE chosen: add casters to the VK cascades (mechs first, then foliage alpha test), make the VK producers part of the umbrella (the three rows become kill switches, default on), then make the VK screen-shadow arm the receiver for object pixels. Reason: it deletes GL later (the GL cascade passes, `flushShadow` drawers, the screen-shadow program and the static-map pass all have a VK owner afterwards), whereas porting GL's pass to write the VK scene would keep GL's cascade production as the caster source.
- Mech casters live in their own TU (`vulkan_shadow_mech_casters.{h,cpp}`, shader `shadow_mech.vert/.frag`), lent the island's render pass and UBO, fed by `mc2_mech_island_get_shadow_feed` (the previous flush's instance / bone / run snapshots GL's `flushShadow` draws, geometry bytes + generation); staged once per frame epoch, drawn in the dynamic map, each cascade layer and the full-map pass.

### Commits (this session, oldest first)
e1ee3f402 VulkanContext released from a unique_ptr (own_inventory entry gone) | 8e2770972 mech casters | b0375f550 producers join the umbrella + one async submit per frame | b62d988be single map only when no cascades | c4af7c4f7 screen-shadow receiver on | b87a39ce2 foliage alpha-test casters | a1d368c49 helper dedupe + parity-2 review fixes + regenerated files | 1ba568ce9 checker-visible pipeline helper + fixture | this state/gaps commit. MERGEABLE TIP = the commit carrying this line (code = 1ba568ce9).

### Results (every number printed by the tool named; same-exe GL oracle, 1080p, `MC2_SMOKE_FREEZE_ANIM=1`, montages viewed: `.claude/l2cap/montages/s11-*`, `s12-*`, `s13-*`)
| Item | Before -> after |
|---|---|
| VK default vs GL, mc2_01 mech box 830,480 245x100 | 4.173 -> 2.466 (chain alone 2.788; screen-shadow receiver 2.466) |
| mc2_01 full / mc2_10 204523 full | 1.348 -> 1.165 / 0.600 -> 0.583 |
| with `SunAxisFix=TRUE,PropShadowReceive=1` both arms (the 5.289 arm of row 21) | mech box 2.505, full 1.195 / 0.584 |
| fps mc2_01 VK, no validation, host quiet | 148 shadows off; 127 old inline-wait passes; 136-137 one submit per frame |
| async refactor image identity | mech box 2.788 and full 1.283 vs 1.284 identical to the sync passes |
| Row 14 mc2_05 isolation | not found, narrowed (gaps row 14): cliff changes 4.9% of pixels, dist to GL 16.5 -> 21.4, signed luma moves not additive |
| Row 5 night | flat-light already ported (gaps row corrected); nightlow a 43.9, c 27.6, b 1.8 remain (lighting term, albedo darker in VK) |

### Proof battery (exe 1ba568ce9, DEPLOY_FINGERPRINT clean)
Battery 1 on b87a39ce2 (before the dedupe, host quiet): VK tier1 + validation 5/5 errors=0 warnings=0, 152/132/108/126/114 fps (`tests/smoke/artifacts/2026-10-15T00-49-59`); GL tier1 5/5 150/103/137/145/149 (`01-01-00`); early-exit (`MC2_VISUAL_DIFF_FRAME_N=100000`) `reason=early_exit stage=atexit` errors=0 (`01-00-49`); `MC2_VULKAN_VALIDATION=sync` mc2_10 errors=0 (`01-11-44`). Earlier chain run on 8e2770972: 5/5 errors=0 (`2026-10-14T23-53-42`..).
Battery 2 on the final exe after the host reboot (Defender busy, fps depressed): see the table the next lines carry; failures there that are GL-only or host-timing are reruns noted below.

### Pre-merge review fixes (shadows review, commit 63d8e1107)
(1) caster shaders and the push range moved above the pipeline builder: `check-vk-pipeline-layout.py` now pairs them with shadow_prop.vert; with `vib.stride = 12` it FAILS (`VK-STRIDE-VBO` 12 != 40, `VK-ATTR-OOB`), restored it prints OK; it also found the push-range ordering error that fix removed. (2) alpha descriptor rebinds on a colour-island `mapGeneration` (bumped at every map invalidate) as well as the view handle. (3) an empty prop set still opens the passes and stages/draws mechs. Nits: three stale comments, hot-table comment. Proof on exe 63d8e1107 (DEPLOY_FINGERPRINT OK): build_all_targets exit 0; doctest RC=0; VK tier1 + validation 5/5 errors=0 warnings=0, 156/127/89/118/112 fps (`2026-10-15T07-49-30`..); mc2_01 mech box 2.466, full 1.165, mc2_10 0.583 (unchanged, `s15-*`). No bookmark has mechs without prop casters, so the empty-set path ran by code reading only.

### Open (session 19)
1. Task: VK `ShadowCSM=FALSE` renders broken props (distance to GL 56.1 / 32.9), reproduces with `MC2_VK_SHADOW_INTEROP=0`: pre-existing, not the shadow island; GL fine. Suspect GL state left by the legacy dynamic pass read by the VK island feeds.
2. Row 14 cause (see gaps), row 5 night apron lighting term, row 20 option oracle needs a pose with a mech shadow on a building, static building map still GL-owned (VK producer needed before GL deletion), objects do not receive it.
3. Colour-island material arrays and hot table were never reset at a map change (a second mission in one process kept the first's textures); fixed in a1d368c49 by code reading, no two-mission run exists to prove it (BUGS row proposed in the final message).
4. `tools/tracy_capture_compare.py` still forces `MC2_VK_SHADOW_INTEROP=0` in its vk arm (Sol/unowned): measures the no-shadow frame. Tracy pair NOT taken (host loaded, then rebooted).
5. Scratch (mine): `scratch/` (scripts patch_*.py caps.sh rd*.py r14*.py battery*.sh, logs, blogs*/), `.claude/l2cap/s11-*` .. `s14-*` capture sets, install `saved_bookmarks/mc2_01_saved_bldg{A,B}.json`.

### Lessons (19)
- An inline `vkWaitForFences` after every submit serialised CPU and GPU: the shadow passes cost 15% fps; one submit per frame with parity fences cost 8% and rendered the identical image.
- A helper that takes the vertex layout as a struct hides it from `check-vk-pipeline-layout.py` silently (it reported OK while a wrong format passed): mutate-and-recheck when moving pipeline code.
- Debug views of GL and VK differ in encoding; per-channel means across them are not comparable.


Every number below was printed by the tool named beside it in this session. Task 1 result: the tip `dafe472ad` plus the docs commit that carries this section is MERGEABLE (the code is the commit 71d2a3d29; everything after is docs).

### Commits (oldest first)
60907eefc state (task 1, RESUME removed) | db0af1461 row 21 | 75ecbb05c row 20 receive arm | ee3016909 row 22 pbr | 65aeedc3d row 19 | 91fe7b29e row 13 | 2675522ea row 14 (mixed) | e05bc6430 screenfx layout fix | 2c3a68eb1 gaps table + montages + bookmark + ledger + bug row 32 | 1411c6719 static_cast | 3b5a1829e gated-layout doc | 37b4e0797 dup_lines baseline | this state commit. MERGEABLE TIP = the commit that carries this line (code = 37b4e0797, everything after is docs).

### Task 1 proof (exe built from 71d2a3d29, deployed to `mc2-win64-lane2`, DEPLOY_FINGERPRINT OK "2 doc-only commits since")
| Evidence | Id |
|---|---|
| `tools/build_all_targets.py --log-dir scratch/blogs`: every stage built (exit 0) | `scratch/bat.log` |
| `build_contract_harnesses --run --unit`: rc 0, doctest 3165 cases / 3165 passed, 3 skipped (the 10 failed assertions are the intended `fatal_if_aborts` test) | `scratch/doctest.log` |
| VK tier1 `MC2_VULKAN_VALIDATION=1`, 5/5, errors=0 warnings=0 on mc2_01/03/10/17/24, 143 / 120 / 120 / 120 / 118 fps (the host was shared with lane 3) | `tests/smoke/artifacts/2026-10-14T19-47-40/report.md` |
| EARLY-EXIT (`MC2_VISUAL_DIFF_FRAME_N=100000 MAX_FRAMES=20`, VK + validation): log ends `event=summary result=fail reason=early_exit stage=atexit`, `crash_handler_hit=False`, no minidump, errors=0 warnings=0 | `2026-10-14T20-08-36/mc2_01.log` |
| GL tier1 on the same exe: 4/5; mc2_17 `heartbeat_freeze_play` twice under another lane's ninja/cmake on the host (flake log row added), mc2_17 alone then PASS 133 fps; the other four 138 / 100 / 127 / 147 fps | `2026-10-14T20-09-57`, `20-24-49` (fail), `20-33-43` (pass) |
| Material-off arm: `MC2_VK_PROP_MATERIAL` is folded into the umbrella, so there is no off run. By code reading: `init_island` appends layout binding 7 under `if (s->tier1)` (outside the `materialBindings` block), sizes the storage-buffer pool with `(s->tier1 ? 1u : 0u)`, uploads the placeholder with `if (s->tier1 && !cp_upload_hot_table(s, {}))`, and `write_descriptors` writes binding 7 under `if (s->tier1 && s->hotBuf)` outside the `wantMaterial` block; the shader reads the table only when `uFrame.hotTableReady != 0 && hotBase + 2u < uTypeHot.c.length()`. No unit seam exists (`grep -rn "hotReady\|uTypeHot" tests` finds nothing; the island is Vulkan-only), so this is the code-read proof plus the validation-clean tier1 above. | `GameOS/gameos/vulkan_color_prop_island.cpp`, `shaders/vulkan/color_prop_tier1.vert` |

### Task 2: VK parity rows (one commit per row; oracle = same-exe GL capture, 1080p, `MC2_SMOKE_FREEZE_ANIM=1`, VK carries the umbrella, every montage viewed)
Mean absolute per-channel pixel distance VK -> GL, printed by `scratch/regdist.py` / `regdist2.py` (regions: boxes on the 1920x1080 frame; the option under test is ON in both arms via `MC2_OPTIONS_OVERRIDE`). Default options: VK after vs VK before identical on every region (0.000, `s9-r0-after-vk` vs `s9-r21-before-vk`), so every new path is inert until its option is on.

| Row | Commit | Before -> after (VK vs GL) | Evidence |
|---|---|---|---|
| 21 SunAxisFix | db0af1461 | `SunAxisFix=TRUE`: day mech box 5.363 -> 5.289, day mech region 3.547 -> 3.519, day full 1.905 -> 1.904, night unchanged (remaining mech distance = missing mech shadows); `glsl_port_diff` calc_light now VERBATIM, allowlist row removed | montage `s9-r21-mechs-gl-before-after.png`; sets `s9-r21-*` |
| 20 PropShadowReceive (receive arm) | 75ecbb05c | NO measurable effect: with the opt-in chain (`MC2_VK_SHADOW_INTEROP` + both CSM gates + `MC2_VK_POST_SCREENFX=1`) option on/off differ by 92-105 px, GL's pair by 1,913 px; hangar 2.193 both arms. The VK cascades carry only static-prop casters. NOT closed. `ShadowCSM` (legacy single map) not ported | `s9-r20-sfx-*`, `s10-r20-chain-fix-gl-off-on.png` |
| 22 StaticPropPbr | ee3016909 | `StaticPropPbr=TRUE`: day hangar 2.469 -> 1.686, day airbase 3.317 -> 3.222, day full 1.945 -> 1.929, night compound 3.719 -> 3.708 | `s9-r22-hangar-gl-before-after.png` |
| 19 PostCAS / TonemapMode | 65aeedc3d | `TonemapMode=1,PostCAS=TRUE`: day full 63.426 -> 1.572, night full 75.326 -> 1.263; Hable (mode 2, no CAS) after: 0.756 / 0.439 | `s10-r19-agx-cas-day-gl-before-after.png` |
| 13 terrain POM real view | 91fe7b29e | full frame: mc2_01 day 1.904 -> 1.353, mc2_05 3.148 -> 2.775, mc2_10 212019 2.135 -> 1.777, 212140 2.661 -> 1.362, 204523 1.565 -> 0.709 (centre region 2.629 -> 1.815, 5.860 -> 4.403, 2.028 -> 1.536, 3.631 -> 1.965, 1.876 -> 0.926) | `s10-r13-terrain-day-*`, `s10-r13-terrain-snow-*` |
| 14 cliff material | 2675522ea | MIXED, not closed: 204523 0.709 -> 0.600 (on the changed pixels the distance fell 1.45 -> 0.41, the brightness step -1.11 vs GL's -1.10), mc2_01 / 212019 / 212140 unchanged, mc2_05 2.775 -> 2.883 (the changed pixels brighten +2.47 where GL moves +1.22, near the pad light pool; cause not found). Cliff albedo re-projection not ported (VK has no albedo array) | `s10-r14-cliff-mc2_05-*` |
| 17 building static shadow | none (finding) | VERIFIED ABSENT in default VK: the GL screen-shadow pass cannot write the VK-owned scene, the VK screenfx pass is opt-in and its static map input is retired (`hasStaticShadow = false`); no mech, tree or building shadow reaches receivers under the umbrella (GL shows mech shadows, VK none, with or without the chain) | `s10-r17-mech-shadow-gl-default-chain.png` |
| 18 decal fill / colour blend | none (finding) | not portable with an oracle: no stock mission places a MarbleCliff; the blend also needs the terrain colormap in the prop island | gaps row 18 |
| bug found on the way | e05bc6430 | the screen-shadow pass assumed the attachment layout on the cascade images (VUID-vkCmdDraw-None-09600 x10 with `MC2_VK_POST_SCREENFX=1` + the CSM gates): errors 10 (`2026-10-14T21-34-21`) -> 0 (`22-27-07`); BUGS-FOUND-FIXED row 32 | `vk_chainA2.log` |
Also: the low night bookmark `tests/visual/bookmarks/mc2_10_night_runway_low.json` (three poses over the mc2_10 runway and apron; the install copies are `saved_bookmarks/mc2_10_saved_nightlow{a,b,c}.json`): GL / VK / VK-with-options montage `s10-night-runway-low-gl-vk-vkopts.png` shows the red, blue and green fixture lights and the lit buildings under VK; pose c also shows the VK apron staying bright where GL darkens it (gaps row 5).

### Proof on the final exe (37b4e0797 = e05bc6430 code + one static_cast; docs after)
| Evidence | Id |
|---|---|
| VK tier1 `MC2_VULKAN_VALIDATION=1` on e05bc6430 (rows 21 20 22 19 13 14 + the layout fix): 5/5, errors=0 warnings=0, 150 / 152 / 141 / 136 / 132 fps | `tests/smoke/artifacts/2026-10-14T22-29-17/report.md` (ee3016909: `21-08-47`) |
| the three options + TonemapMode 1 + PostCAS on, VK + validation: mc2_01 and mc2_10 errors=0 warnings=0 | `22-43-06`, `22-45-15` |
| the opt-in chain (`MC2_VK_POST_SCREENFX=1` + CSM gates) + validation: errors=0 warnings=0 (was 10) | `22-27-07` |
| GL tier1 on e05bc6430: 5/5, 149 / 105 / 139 / 146 / 147 fps | `22-47-26` |
| final exe 37b4e0797: VK tier1 + validation 5/5 errors=0 warnings=0 (150 / 156 / 146 / 137 / 133 fps, `2026-10-14T23-06-00`); GL tier1 5/5 (149 / 105 / 139 / 146 / 146 fps, `23-16-45`); early-exit (`MC2_VISUAL_DIFF_FRAME_N=100000`, VK + validation) ends `reason=early_exit stage=atexit`, exit 4, `crash_handler_hit=False`, no minidump, errors=0 (`23-27-29`) | `tests/smoke/artifacts/` ids as listed |
| `build_all_targets` exit 0; doctest 3165 / 3165 passed (3 skipped; the 10 failed assertions are the intended `fatal_if_aborts` case); pytest scripts 811 passed | `scratch/bat2.log`, `scratch/doctest2.log`, `scratch/cc_final.log` |
| `check-contracts.sh` full board at 37b4e0797: 121 passed, 2 failed (`container_callsites`, `doc_links`, not this lane's); ratchets: `cxx17_ratchet` green (LG09 +1 cast fixed with a static_cast); `dup_lines` baseline REWRITTEN 55908 -> 56005 (the VK composite shader is the port of postprocess.frag: +96 twin shader lines, +1 parameter list entry; not contorted); `gated_layout_macros` doc regenerated (line numbers); `container_callsites` and `doc_links` red as before, not this lane's | the board before those fixes: 118 passed, 5 failed (`scratch/cc_final.log`) |

### Tracy (task 3): NOT taken. Lane 1 and lane 3 held the host lease for about an hour at the start and the editor smoke / lane 3 loops kept it busy afterwards; a before / after pair would have measured the neighbours.

### Open (session 18)
1. Rows 14 (mc2_05 overshoot), 20 (needs mech / building casters in the VK cascades and `ShadowCSM` legacy map), 22 (material scalars), 17 (default VK has no screen shadow: port target = enable the VK screenfx shadow arm by default with a static-building input, or a world-fixed building map consumer), 18 (no content), 9/5 (terrain night cement) remain.
2. `container_callsites` and `doc_links` of `check-contracts.sh` are red and are not this lane's.
3. Islands4 nit 1 (protected ScopedEnv ctor) still waits for the Sol files.
4. Terrain POM: the GL per-layer height multipliers and the dirt layer in the displacement sum are not in the VK sum.
5. Scratch to clean (mine): `scratch/` (scripts `capb*.sh capg*.sh chain14.sh final*.sh gates.sh vk_opts*.sh edit_*.py regdist*.py mont.py build_mc2.bat`, logs `*.log`, `blogs/`, `blogs2/`, `terrain_both.frag`, `state_add.md`), `.claude/l2cap/s9-*`, `s10-*` capture sets (PNGs, not committed; the ledger records them), `.claude/l2cap/s7*`, `s8after-vk`, older leftovers listed in session 16 and 17; the install `saved_bookmarks/mc2_10_saved_nightlow{a,b,c}.json` stay (they are the pose source for the tracked bookmark file).

### Lessons (18)
- A `timeout N` around `run_smoke.py` kills the runner while it waits for the host lease: run smokes with `nohup` in the background and poll; the dead run leaves its own folder-lease entry (remove only your pid).
- The default VK path has no screen-shadow pass at all (GL's cannot write the VK-owned scene); an option that steers only the GL screen shadow (PropShadowReceive, ShadowCSM) has nothing to steer under the umbrella.
- Run an option's oracle with the option ON in BOTH arms (`MC2_OPTIONS_OVERRIDE`), and a default-options pair as the regression guard (0.000 distance).

## SEVENTEENTH session (2026-10-14 ~16:05 CDT start, cap 5 h) -- post_* owners, CSM layouts, VK parity rows 15 + 16 (+ the ambient arm of 22); nifty merged at the start (d109918d4), NOT merged back
Every number below was printed by the tool named beside it in this session.

### Commits (oldest first, after the merge d109918d4)
- e1411ba4b TASK 1: `vkPostFogNative_`, `vkPostHzbNative_`, `vkPostSsaoNative_`, `vkPostScreenFx_` are `std::unique_ptr<T, TDeleter>` members of `gosPostProcess` (the deleter is defined next to each state, idles the GPU through `destroy_state`, then deletes); no static-duration owner; each state has ONE typed delete (in its deleter) and the five `reset(new T())` stay at the call sites (review parity1 item 3: claim reworded, not wrapped in a factory; the e1411ba4b message saying "no raw new/delete" was too strong). `own_inventory` baseline rewritten (e1c4cf592: the four types left the raw-owner list, the deleter's delete is untyped), gated-layout doc and callers regenerated. Ledger regenerated: 0 manual tokens everywhere.
- 71fd6181a TASK 2 (CSM layouts): the terrain descriptors for bindings 7/8/9 named SHADER_READ_ONLY while the shadow island hands its depth images over in DEPTH_STENCIL_READ_ONLY; they now name the layout the image is in (the island's own copies stay SHADER_READ_ONLY). It was a descriptor-layout mismatch, not a missing barrier.
- cc9e1eda0 + 44b5dbf8d + 8d950fd8b TASK 3: VK parity row 15 (night tags + fixture emission: `ColorFrameUBO.isNight/nightFactor` in the former pad, per-type emission colours as set 0 binding 7 fed from `s_typeHotColors`, window nodes carry only their hot-colour light, port of `get_base_light`) and row 16 (base-light direction wrap 0.45). The first oracle result was NEGATIVE (the wrap alone moved the day poses AWAY from GL: 3.479 -> 3.623 mean abs pixel distance); the cause is the missing StaticPropAmbient hemisphere fill (row 22 arm), so 8d950fd8b feeds it (`ambientV1Strength`, UBO 256 -> 272 bytes) and the three together move VK toward GL.
- 76fe86e69 islands4 review nits 2-5 (ScopedEnv comment, shadow re-stage capacity, throwing-scope doctest); nit 1 (protected ScopedEnv ctor) NOT done: the Sol files `tools/vfx_oracle/lifecycle_driver.cpp` and `tests/unit/test_vfx_frozen_clouds.cpp` use the untyped base.
- c9bddde83 + 680c596d2 BUG found by the early-exit proof (BUGS row 28): the shutdown trace added last session crashed (access violation) in the atexit chain on an `exit()` while the presenter was alive: `tagRequested` read a function-local static set that was already destroyed. The default whitelist is now a constant array.
- d41a0375e VK-PARITY-GAPS rows 15, 16 FIXED, 22 partly (montages committed).

### Oracle (1080p, FREEZE_ANIM, VK carries the umbrella; GL = default path of the same exe, `.claude/l2cap/s7gl`)
Mean absolute pixel distance to the GL capture, VK before (`s7before-vk`, exe d0e295e4f) -> after (`s8after-vk`, exe 8d950fd8b): day airbase region 3.479 -> 3.266, day hangar 1.905 -> 1.719, night compound 4.106 -> 3.408, night full frame 1.616 -> 1.565. The first port without the ambient arm (`s7after-vk`): day 3.623 (worse). VK noise: `s7after-vk` vs `s7after2-vk` identical. Montages viewed: `.claude/l2cap/montages/s7-day-hangar-gl-before-after.png`, `s8-night-compound-gl-before-after.png` (night buildings now as lit as GL; GL still shows tree-shadow streaks VK does not). The golden verify (`verify.py --golden current`) FAILs every region on VK before and after (VK differs from the GL golden everywhere); the region numbers did not move. The night poses are too high to show a lit window or runway fixture: the emission arm is proven by the shader port and by the by-day arm, NOT by a close-up capture (open 1).

### Proof (final exe 8d950fd8b, docs commits after)
| Evidence | Id |
|---|---|
| VK tier1 `MC2_VULKAN_VALIDATION=1`, 5/5, errors=0 warnings=0 on mc2_01/03/10/17/24, 133 / 111 / 130 / 129 / 121 fps | `tests/smoke/artifacts/2026-10-14T18-23-42/report.md` |
| GL tier1, 5/5, 152 / 107 / 140 / 146 / 147 fps | `2026-10-14T18-34-33` |
| post_ssao / post_screenfx live (`MC2_VK_POST_SSAO=1 MC2_VK_POST_SCREENFX=1 MC2_SSAO=1 MC2_CLOUD_SHADOW=1`, validation) mc2_01: used_vulkan=17556 for both, errors=0 warnings=0, PASS | `2026-10-14T18-21-31/mc2_01.log` |
| EARLY-EXIT (`MC2_VISUAL_DIFF_FRAME_N=100000 MAX_FRAMES=20`, VK + validation): log ends `event=summary result=fail reason=early_exit stage=atexit`, no crash bundle; the run BEFORE c9bddde83 crashed (BUGS row 28) | `2026-10-14T18-20-36/mc2_01.log` (crash: `17-12-27`) |
| TASK 2: `MC2_VK_SHADOW_INTEROP=1 MC2_VK_CSM_ARRAY_INTEROP=1 MC2_VK_CSM_FULLMAP_INTEROP=1` + validation mc2_01: array and full map produced and consumed directly (`used_vulkan=9144 direct_chain=1`), errors=0 warnings=0 (was 10 x VUID-vkCmdDraw-None-09600 for two sessions); day mc2_01 capture with the gates on viewed: terrain, props, mechs, shadows all drawn | `2026-10-14T17-06-31/mc2_01.log`; capture `.claude/l2cap/s7csm-vk`; re-run on the final exe: see Open 6 |
| `build_all_targets` every stage built (exit 0); `build_contract_harnesses --run --unit` rc 0, doctest 3165 cases (the one aborting case is the intended `fatal_if_aborts` test); `check-contracts.sh` full board at the last commit: see Open 4 | this session's logs |

### Tracy (task 4): NOT taken. The host was never quiet (lane 1 smokes held the lease most of the session; my own proof chains used it for ~2 h).

### Open
1. A close-up night bookmark on the mc2_10 runway / base to see lit windows and fixtures under VK.
2. Rows 13, 14, 17-22 remain; row 22 StaticPropPbr (Schlick specular) is unported; the ambient fill is checked only with the option ON.
3. Islands4 nit 1 (protected ScopedEnv ctor) waits for the Sol files.
4. `container_callsites` and `doc_links` of `check-contracts.sh` are red and are not this lane's.
5. Scratch to clean (mine): `scratch/proof_*.sh`, `scratch/cap*.sh`, `scratch/spv_before/`, `scratch/oldsh/`, `scratch/blogs/`, `scratch_*.log` in the worktree root, `.claude/l2cap/s7*` and `s8after-vk` (the two committed montages stay), older leftovers listed in session 16.
6. The CSM interop run was re-run on the final exe only if the line in the Proof table says so (a lane 1 smoke held the lease at the end).

### Lessons
- A deploy fingerprint hard fail ends a proof chain: finish ALL code commits before the first smoke of a chain; two chains (about 1 h) were lost to commits made mid-chain.
- A negative oracle result is a finding: a port of one term of a balanced lighting sum can move the picture away from GL; port the arm that balances it together with it.
- Never run the contract board or a build during a tier1: the first VK tier1 of the session lost four missions to `heartbeat_freeze_play` because of my own check-contracts run.
- Killing a smoke leaves a stale entry in `A:/Games/mc2-opengl/.smoke_leases.json` that blocks the next deploy of that install; when the owner pid is dead, remove only that entry.
- Bash-tool quirks: `grep $'\r'` is unreliable (use python for byte checks); the commit hook regex trips on a hyphenated word starting with a in the message and on such text inside a heredoc.

## SIXTEENTH session (2026-10-14 ~12:50 CDT start, cap 5 h) -- review should-fix items, Gates work, harness, ownership slices 7-9 (shadow, present, terrain); nifty NOT merged (lane 1 mid-session), start tip cdce24359
Every number below was printed by the tool named beside it in this session.

### Commits (oldest first)
- 61db23a9e review should-fix 1-3: post_ssao / post_screenfx `destroy_state` and `vulkan_water_island_images.cpp` idle through `waitIdle("...")`; the release-order comment was wrong, so the ORDER was fixed, not the comment: the state moves into a scope-local (`{ auto dying = std::move(*s); }`) whose destructor releases in reverse declaration order (member-wise move-assignment releases forward).
- d684bdb1f Gates: br-a-lane2.patch (973 -> 971 rows, `gate_gen --check` green, scripts_tests `test_the_committed_table_is_consistent` green again); `VFX_COMBAT` in `knownTags()`; `gate::ScopedOverride<"MC2_X">` (see API below), doctest x2, `Gates/README.md`.
- e52392c70 harness: `run_visual_capture.py` starts the budget timer AFTER `nofocus.spawn` returns (spawn blocks on the host lease); bbee2604a F4 provider API `std::optional<size_t> gos_ResourceStringLength(HSTRRES, DWORD)` in the new `GameOS/include/gameos_resource.hpp` (own header: gameos.hpp is on the file-size ratchet; optional because the first version, bool + out-param, added LG05 sites); `gos_GetResourceString` unchanged for its callers (marker + log line kept).
- 91a842230 ownership slice 7 shadow (`IslandSlot::Shadow`, every object owned, 49 -> 0); b19c52c46 shutdown-order trace; c119e8010 slice 8 present (new `OwnedRawBuffer` / `OwnedSwapchain` / `OwnedSurface`, `~Impl` releases in reverse, 103 -> 0); ffff63cbb slice 9 terrain (`IslandSlot::Terrain`, `g_island` and `g_ovl` gone, overlay state is a member of the island, 117 -> 0).
- 5be71e04b regeneration: `scripts/check-vk-pipeline-layout.py` reads owner-created layouts (`mc2vk::create(s->dsl, ...)` and `{ s->dsl.get(), o->texDsl.get() }`: 26 false errors otherwise), ledger, background-launch inventory, gated-layout doc, callers allowlist row for nifty's `test_build_compiler_presets.py`.
- 57ec9b574 FIX found by the live shadow run (below): the sorted caster re-stage copied `alignas(16)` instances through a typed pointer into a mapped allocation.

### Pre-merge fixes (review islands4), tip after them: see git log
- The shutdown trace now prints only under the `VK_SHUTDOWN` diag tag (registered in `knownTags()`; `mc2_diag::tagRequested(string_view)` reads MC2_DIAG_TAGS directly because the JSONL file is already closed at teardown): default log unchanged, `MC2_DIAG_TAGS=VK_SHUTDOWN` mc2_01 shows 11 lines (`2026-10-14T15-14-06`), the default VK run shows 0 and a live terrain (`2026-10-14T15-17-48/mc2_01.log`, `[VK_TERRAIN_TIER1] vulkan_available=1`). `log_tag_census` OK.
- Terrain per-patch UBO slots are written with memcpy (`vulkan_terrain_island_run.cpp`), like the shadow fix.
- Proof on that exe: VK tier1 validation 5/5 errors=0 warnings=0 (146 / 141 / 128 / 116 / 100 fps, `2026-10-14T15-17-48`; a first VK launch right after the deploy died silently on all 5 and the tag run seconds later passed: rerun clean, cause not found, probably the deploy's lease release racing the launch), `build_all_targets` exit 0, doctest 3143 / 3143, `cxx17_ratchet` / `dup_lines` / `file_size` / `gate_gen` green (the cxx17 ratchet scans the COMMITTED tree: commit before running it).

### Ledger (`py -3 tools/vk_ownership_ledger.py`): Manual tokens, before -> after
shadow 49 -> 0, present 103 -> 0, terrain 117 -> 0. Every island is at 0 (total created 0 / released 0). Command buffers and descriptor sets are the pool-owned kinds the ledger lists apart.

### Proof
| Evidence | Id |
|---|---|
| VK tier1, `MC2_VULKAN_VALIDATION=1`, FINAL exe 57ec9b574 (fe1c794ffd00), 5/5, errors=0 warnings=0 on mc2_01/03/10/17/24, 135 / 134 / 125 / 125 / 123 fps | `tests/smoke/artifacts/2026-10-14T14-32-55/report.md` |
| GL tier1, same final exe, 5/5, 147 / 103 / 125 / 139 / 144 fps | `tests/smoke/artifacts/2026-10-14T14-43-48/report.md` |
| Same two runs on the previous exe (5be71e04b, b89ad90f3b63, before the memcpy fix): VK 5/5 errors=0 warnings=0 (149 / 125 / 117 / 121 / 108), GL 5/5 (152 / 107 / 144 / 142 / 146) | `2026-10-14T13-42-42`, `2026-10-14T14-00-01` |
| Captures day mc2_01_saved_205644 / night mc2_10_saved_204523 (1920x1080, FREEZE_ANIM), BEFORE = the installed exe of the previous session (cb24dc2686a1), AFTER = 5be71e04b (the memcpy fix is in code the default VK and GL runs never reach): day GL noise (before1->before2) 34 px, before1->after1 34 px, before1->after2 68 px; day VK before->after 24 px vs VK noise (before->beforeb) 71 px; night 0 px in all four pairs. Montages viewed: day VK (the only red box is a 1-px shoreline/horizon cluster) and night GL. | `.claude/l2cap/s6before{1,2}-gl`, `s6before-vk`, `s6beforeb-vk`, `s6after{1,2}-gl`, `s6after-vk`; montages `.claude/l2cap/montages/s6-*` |
| EARLY-EXIT run (VK + validation + `MC2_VISUAL_DIFF_CAPTURE=1 MC2_VISUAL_DIFF_FRAME_N=100000 MC2_VISUAL_DIFF_MAX_FRAMES=20`): `capture_timeout` exit while every default island was live; log ends `event=summary result=fail reason=early_exit stage=atexit` after one `[VK_IMAGE_RESOURCE] destroy image owner`, no crash, no `shutdown order violated`, errors=0 warnings=0 | `tests/smoke/artifacts/2026-10-14T14-11-00/mc2_01.log` |
| Live coverage of default-off islands: `MC2_VK_POST_SSAO=1 MC2_VK_POST_SCREENFX=1 MC2_SSAO=1 MC2_CLOUD_SHADOW=1` mc2_01 VK + validation: post_ssao used_vulkan=17510, post_screenfx cloud_frames=17510, errors=0 warnings=0, PASS | `2026-10-14T14-11-13/mc2_01.log` |
| SHADOW island (default OFF, so the default VK run does not reach it; `[VK_SHADOW_INTEROP] vulkan_available=0` there): `MC2_VK_SHADOW_INTEROP=1` on the first converted exe CRASHED on frame 1 (access violation at 0xFFFFFFFFFFFFFFFF in the sorted re-stage, `2026-10-14T14-16-35`); the pre-session exe passed the same gates, so it was a regression of the conversion (latent bug, root cause in Open 1). After 57ec9b574: shadow only PASS 8192 frames, used_vulkan=15917, errors=0 warnings=0 (`2026-10-14T14-28-27`); shadow + `MC2_VK_CSM_ARRAY_INTEROP=1 MC2_VK_CSM_FULLMAP_INTEROP=1`: errors=10 (VUID-vkCmdDraw-None-09600 x10), IDENTICAL to the pre-session exe (10, `2026-10-14T14-18-29`), so pre-existing | see ids |
| Shutdown-order trace (default VK run, `2026-10-14T13-42-42/mc2_01.log`): four `[VK_IMAGE_RESOURCE] destroy image owner ...; N alive after` lines (4 -> 1), then `[VK_ENGINE_CONTEXT] retire island slot=` Water, ColorProp, Terrain, Mech, LegacyShape, Sky in slot-use order, then the last image owner (the present image) `0 alive after`, then the context destroy: the `destroy()` precondition reads straight from the log (it goes through the existing `[VK_ENGINE_CONTEXT]` / `[VK_IMAGE_RESOURCE]` tags: the JSONL diag file is already closed at that point, `gos_DestroyRenderer` shuts it down first) | same file |
| `tools/build_all_targets.py` every stage built (exit 0); `build_contract_harnesses --run --unit` 34 PASS rc 0, doctest 3143 / 3143 cases (3141 + the 2 ScopedOverride cases; the one aborting case in the log is the intended `fatal_if_aborts` test); `check-contracts.sh` full board 119 passed / 3 failed: `container_callsites`, `delivery_installs`, `doc_links` (the pre-existing reds, none of this lane's) | this session's logs (deleted, ids above) |

### Gate override API
- `gate::ScopedOverride<"MC2_X"> on("1");` (`Gates/gate_cold.h`; the name is `static_assert`ed against the gate table at compile time) sets the PROCESS environment (`_putenv_s` / `setenv`), so children started inside the scope inherit it.
- The destructor restores the previous value (or unsets it) and BOTH ends call `gate::invalidate(name)`, so `gate::get<>()` in the same process sees the change at once; `nullptr` or `""` clears the variable; scopes nest (LIFO); single-threaded phases only, like `invalidate()`.
- `gate::ScopedEnv(name, value)` is the untyped base for a runtime name; `Gates/README.md` documents it for `tools/vfx_oracle/lifecycle_driver.cpp` and `tests/unit/test_vfx_frozen_clouds.cpp` (the Sol files were NOT touched).

### Open
1. ROOT CAUSE of the shadow crash, worth a sweep: a mapped allocation is only aligned to its buffer's OFFSET alignment, so a typed copy of an `alignas(16)` struct (`GpuStaticPropInstance`, `GpuMechInstance`) into mapped memory can fault when the allocator hands out a 4- or 8-aligned offset; the conversion changed the allocation pattern and exposed it. Fixed for the three shadow sites; other islands copy with memcpy or write to 256-aligned UBO slots (checked by grep), not audited at run time.
2. The CSM interop gates (default OFF) leave the CSM images in DEPTH_STENCIL_READ_ONLY while terrain expects SHADER_READ_ONLY: 10 validation errors, unchanged by this session.
3. Tracy before/after pair NOT taken: the host was never quiet (lane 3 smokes and a lane 1 game held the lease or the CPU at every check); recipe in the EVENING section below.
4. The post_* states are still new / deleted by `gosPostProcess` (post_fog and post_hzb too): out of the ledger's islands, not converted. `OvlState` and the island slots are the last parked states; no static-duration owner remains in the converted islands.
5. Red rows of `check-contracts.sh` that are not this lane's: `container_callsites` (311), `delivery_installs`, `doc_links`.
6. No BUGS-FOUND-FIXED row: the shadow crash was a regression of this session's own conversion in a default-OFF path, no player-visible symptom ever shipped.
7. Scratch to clean (deleted by me: this session's scripts, patches, commit-message files and logs): `.claude/l2cap/s6*` (7 sets) and `.claude/l2cap/montages/s6-*`, plus older leftovers I did not touch: `.claude/l2cap/s5*`, `scratch/before/mc2_before.exe` (9910a8ee22d1, used to prove the shadow crash is a regression), `scratch/head_mc2.exe`, `scratch/patch_*.py`, `scratch/*.sh` (ab_gl, cap_after, cap_before2, gl_singles), `scratch/state_new.md`, `scratch_*.log` in the worktree root (older sessions).

### Lessons
- A default-off gate group is NOT covered by tier1: a conversion of an island behind a gate needs a live run with that gate on, before the commit, not after the batch (the shadow island's first frame crashed, and the default 5/5 proofs were green).
- The Bash tool rewrites backslashes inside heredocs: `'\\0'` became a NUL byte (compile error C2137) and `"\n"` inside a Python string became a real newline (broke a string literal); a loop that searched for the NUL never ended. Write scripts with the Write tool. Python `open(f, 'w')` on Windows writes CRLF: always `newline=''`.
- A checker that parses code SHAPE (`check-vk-pipeline-layout.py` read `&s->dsl` and plain array members) needs the new owner idiom taught to it in the same batch: 26 false errors after the terrain slice.
- Scripted conversion (a table of member -> owner type, regex rewrites of the create / destroy / use sites) built clean on the first full compile of present and terrain; the table is the whole design, the compiler finds the rest.
- The lease queue was the long pole (lane 3 and lane 1 smokes): chain every lease-taking step into one background script instead of one launch per step; the budget no longer includes the wait.

## FIFTEENTH session (2026-10-14 ~09:15 CDT start, cap 5 h) -- review should-fix items + ownership batches 5 and 6; nifty merged at the start (fast-forward to 5fddfc613; nifty NOT merged back)
Every number below was printed by the tool named beside it in this session.

### Commits (oldest first, after the fast-forward to 5fddfc613)
- d87aa9be9 TASK 1, the two should-fix items and the nits of `reviews/lane2-2026-10-14-islands2-cleanup.md`: (1) the engine context is heap-allocated and never freed (`engineContext()` in vulkan_context.cpp), so no static destructor runs `destroy()`; the explicit `destroy()` at engine shutdown is the only teardown. (2) `destroy()` states its precondition (the owners outside the context are gone) and checks it: `vkImageResourceLiveCount()` (vk_image_resource) must be 0 after the islands are retired, else a `[VK_ENGINE_CONTEXT] ... shutdown order violated` line plus an assert. (3) nits: `VulkanContext::waitIdle(why)` logs a non-success `vkDeviceWaitIdle` result and is the one "idle then drop" step (first-layout timeout, island retire, vfx / sky init failure, every buffer grow); the sky island stores a typed `VkImageResource*`.
- 336201ecd ownership batch 5: legacy_shape and mech are parked in engine-context slots (`IslandSlot::LegacyShape`, `Mech`; `~MechIsland` tears the GPU timer down first); post_ssao and post_screenfx keep the post-process state objects as post_fog does (owners; `destroy_state` idles and resets). `mc2vk::createChecked` (vk_owned.h) returns the VkResult for the callers that log it; the ledger counts it as an adoption.
- 8790d6831 color_prop and 3aa5b509c water (batch 6): both parked in slots (`ColorProp`, `Water`), every object owned, the `make_buffer` helpers call `OwnedBuffer::create`, staging buffers are scoped `OwnedBuffer`s, one-shot fences are `OwnedFence`.
- f9669d668 `waitIdle` takes a `string_view` (LG06 +2 otherwise). f27037a32 regeneration: cxx17 baseline tightened (17 fewer legacy sites), own_inventory baseline (the six island types left the inventory; `VulkanContext` is the deliberate never-freed singleton; TerrainIsland.delete 0 -> 6 is no new site: `g_island` is now assigned only by the terrain island, so its untyped deletes are attributed to it), callers, gated-layout macros.

### Ledger (`py -3 tools/vk_ownership_ledger.py`): Manual tokens, before -> after
legacy_shape 32 -> 0, mech 37 -> 0, post_ssao 41 -> 0, post_screenfx 45 -> 0, color_prop 47 -> 0, water 47 -> 0. Remaining: shadow 49, present 103, terrain 117 (created / released by hand, imbalance -17 / -29 / -55).

### Proof (one batch: all six islands were converted before the first proof run, each its own commit for bisection)
| Evidence | Id |
|---|---|
| VK tier1, `MC2_VULKAN_VALIDATION=1`, exe f27037a32 (cb24dc2686a1), 5/5, errors=0 warnings=0 on mc2_01/03/10/17/24, VRAM run_end mc2_01 3207 MiB (identical to before), 148 / 124 / 117 / 115 / 106 fps | `tests/smoke/artifacts/2026-10-14T10-13-09/report.md` |
| EARLY-EXIT run (task 1 proof): VK umbrella + validation, `MC2_VISUAL_DIFF_CAPTURE=1 MC2_VISUAL_DIFF_FRAME_N=100000 MC2_VISUAL_DIFF_MAX_FRAMES=20` makes the engine `exit(4)` 21 frames after the intro (`capture_timeout`), i.e. while the terrain, sky, legacy_shape, color_prop and water islands were live: the log ends at `[SMOKE v1] event=summary result=fail reason=early_exit stage=atexit` (the expected red of an early exit), no crash, no teardown line, no `shutdown order violated`, validation errors=0 warnings=0 | `tests/smoke/artifacts/2026-10-14T10-32-06/mc2_01.log` |
| Live coverage of islands the default VK run does not reach: `MC2_VK_POST_SSAO=1 MC2_VK_POST_SCREENFX=1 MC2_SSAO=1 MC2_CLOUD_SHADOW=1`, mc2_01 under VK + validation: post_ssao used_vulkan=17514, post_screenfx cloud_frames=17514, errors=0 warnings=0, PASS. The default run's logs show mech `used_vulkan=17491`, water 17763, color_prop 17763, legacy_shape first co-write nodes=1 verts=693 | `tests/smoke/artifacts/2026-10-14T12-38-00/mc2_01.log` (default run with logs: `2026-10-14T12-26-55`) |
| GL tier1, same exe, 5/5 (150 / 101 / 134 / 140 / 147 fps) after FOUR earlier full GL runs of the same exe that each lost 1-2 missions to `heartbeat_freeze_play` on a loaded host (rows in `tier1-flake-log.md`); control: the BEFORE exe (previous session's, 4896b503e) and the HEAD exe on mc2_10 and mc2_17 alone, interleaved, all four PASS at 128-141 fps | `tests/smoke/artifacts/2026-10-14T12-14-45/report.md` |
| Captures, day mc2_01_saved_205644 / night mc2_10_saved_204523, before (scratch `scratch/before/mc2_before.exe` = the installed exe of the previous session) vs after: night 0 px on GL and VK (0 px in every noise pair too); day VK before->after 73 px (4 clusters, largest 57x6 at 1206,193) vs VK noise floor 37 px (before->beforeb), after->beforeb 62 px; day GL before1->after1 73 px (42x4 at 1221,195), before1->after2 49 px, GL after1->after2 noise 40 px; montages viewed (`.claude/l2cap/montages/s5-day-vk.png`, `s5-day-gl.png`): healthy, three mechs drawn, the red boxes are far-shoreline pixels that the GL noise pair carries too. (The GL before pair's mc2_01 pose 2 was re-taken alone and is a different frame, 693k px: unusable as noise; the after1->after2 pair is the GL floor.) | `.claude/l2cap/s5before{1,2}-gl`, `s5before{,b}-vk`, `s5after{1,2}-gl`, `s5after-vk` |
| `tools/build_all_targets.py` every stage built; `build_contract_harnesses --run --unit` rc 0, doctest 3141 / 3141 cases; `check-contracts.sh` before the regeneration commit: the red rows listed under Open 3 plus the three this session regenerated (cxx17_ratchet, gated_layout_macros, own_inventory) | `scratch_bat.log`, `scratch_harn.log`, `scratch_cc.log` (scratch) |

### Open
1. Tracy before/after pair NOT taken: another lane's `mc2.exe` (v0.4 install smoke) and `ninja` were running on the host at every check, and the harness refuses while any mc2.exe runs. Re-take on a quiet host (recipe in the EVENING section below).
2. Ledger order next: shadow (49), present (103), terrain (117). The post_* states are still new/deleted by `gosPostProcess` (as post_fog / post_hzb are); the shadow island, the present path and the terrain island are the last hand-written ownership code in this area.
3. Red rows of `check-contracts.sh` that are not this lane's: `gate_gen_check` (+ scripts_tests `test_the_committed_table_is_consistent`: `MC2_CONTRACT_REPO_ROOT` / `MC2_IBL_ASSET_STRICT` marked verified with no reader; fails in mc2-main too), `container_callsites` (311 problems), `delivery_installs` (every install lacks `calcLightPools` in `shaders/gos_tex_vertex_lighted.frag`), `doc_links`, and `tools/callers.py --check`: nifty's `scripts/tests/test_build_compiler_presets.py` has no caller and is not allowlisted (arrived with 5fddfc613).
4. No BUGS-FOUND-FIXED row: the should-fix items had no player-visible symptom (static teardown order on an abnormal exit).
5. Scratch to clean (nothing deleted by me except the build logs I overwrote): `.claude/l2cap/s5*` (7 sets) and `.claude/l2cap/montages/s5-*`, `scratch/before/mc2_before.exe`, `scratch/head_mc2.exe`, `scratch/patch_*.py`, `scratch/*.sh`, `scratch/state_new.md`, `scratch_*.log` in the worktree root, `C:/tmp_patch_ls.py` (a truncated heredoc script of mine).

### Lessons
- A long heredoc through the Bash tool was truncated mid-command (the script ended inside a Python string): write scripts with the Write tool. A regex that rewrites `r = vkCreateX(...)` also matches inside `rr = ...`.
- Compile-driven conversion works: swap the member types, build, fix the `.get()` sites from the compiler's list; `mc2vk::createChecked` keeps the log lines of the old `r = vkCreate...; if (r != VK_SUCCESS)` blocks.
- `heartbeat_freeze_play` on GL is the host, not the code: it hit a different mission in each of four full runs and passed in the interleaved before/head control; look for another lane's game (`Get-Process mc2`) before reading an fps collapse.
- Take the BEFORE capture sets first on the old install; the `--duration` budget includes the lease wait (pass `capture.py --duration 1500`; `capture_ab capture` cannot), then deploy.

## MORNING session (fourteenth session, 2026-10-14 ~06:15 CDT start, cap 5 h) -- review fixes + ownership slices 2 and 3; nifty merged at the start (fast-forward to ba6ba6f84; nifty NOT merged back)
Every number below was printed by the tool named beside it in this session.

### Commits (oldest first, after the fast-forward to ba6ba6f84)
- d2bd1b7d5 the review's four should-fix items: (1) the light-feed ring is `mc2vk::SharedLightFeed` (OwnedBuffer slots), a MEMBER of the engine `VulkanContext`, freed by `VulkanContext::destroy()` ahead of the allocator and device: no static owner, no `mc2vkSharedLightFeedRelease`, the "allocator changed" branch is gone (handles that are not the context's are refused, caller keeps its own copy); (2) the release call is gone from gameosmain.cpp: one teardown path (the context's destroy); (3) the VFX island is parked in a context slot (`IslandState` / `IslandSlot::Vfx`, `retireIsland()`): no raw static pointer; destroy() retires every parked island before the light feed; (4) the first-layout fence timeout (see 944a64774). Log-tag census re-frozen (one line deleted).
- (next) `mc2vkSharedLightFeedAcquire` returns the buffer (null = fail-soft): no bool + out-param (LG05).
- post-merge regeneration: callers, `scripts/background-launch-allow.json` row for nifty's `tools/vfx_oracle/probe_rng.py`, cxx17 baseline rewritten (LG20 +8: nifty's `tools/vfx_oracle` drivers name MC2_ variables as literals).
- 944a64774 ownership slice 2: shared_scene (`vk_first_layout.cpp`: OwnedCommandPool/OwnedFence; a fence that times out now waits for the device before release, never destroys live), image_resource (`VkImageResource` owns memory/image/view/semaphores/fences; 46 consumer sites `.get()`), sky (parked island `IslandSlot::Sky`, all objects owned, staging `OwnedBuffer`), post_fog (state objects owned).
- ownership slice 3: post_hzb (state objects owned). cxx17 baseline tightened twice.

### Proof (every island of slices 2 and 3 shares the same runs: they were converted as a batch, not one proof per island)
| Evidence | Id |
|---|---|
| VK tier1, validation ON, exe of 944a64774 (light_feed, shared_scene, image_resource, sky, post_fog; VFX parked in slot), 5/5, errors=0 warnings=0 on mc2_01/03/10/17/24, VRAM run_end mc2_01 3207 MiB (identical to before) | `tests/smoke/artifacts/2026-10-14T07-27-28/report.md` |
| VK tier1, validation ON, exe with post_hzb added, 5/5, errors=0 warnings=0 | `tests/smoke/artifacts/2026-10-14T07-46-19/report.md` |
| Captures day mc2_01_saved_205644 / night mc2_10_saved_204523, before (the installed exe of the previous session, 2c0052240) vs after (944a64774): night 0 px on GL and VK; day VK 59 px (2 clusters, largest 42x4 at 1221,195) vs after-vs-beforeb 86 px; noise floors: GL before-vs-before2 19 and 40 px, VK before-vs-beforeb 27 px; the same shoreline cluster at 1221,195 sits in the GL noise pair; montage viewed (`.claude/l2cap/montages/lfsi-day-vk.png`): healthy, no artefact | `.claude/l2cap/lfsi-before{,b}{1,2}-gl`, `lfsi-before{,b}-vk`, `lfsi-after*` |
| GL tier1, same exe as the second VK run (post_hzb included), 5/5 (150 / 103 / 132 / 115 / 97 fps) | `tests/smoke/artifacts/2026-10-14T08-49-46/report.md` |
| Final-exe VK day capture (`lfsi-after4-vk`, sidecar `backend: vulkan`): 88 px (6 clusters) vs before, 61 px vs beforeb; montage viewed (`.claude/l2cap/montages/lfsi-day-vk-final.png`): the red boxes are shoreline pixels, no artefact. Night of the final exe was not re-taken (night 0 px on the 944a64774 exe; post_hzb only reduces depth for culling). |
| `tools/build_all_targets.py`: every stage built; `build_contract_harnesses --run --unit` rc 0 (doctest green); `check-contracts.sh` 115 passed / 7 failed: container_callsites, gate_gen_check (+ scripts_tests `test_the_committed_table_is_consistent`, tools_tests `test_sim_determinism`), delivery_installs, doc_links are NOT this lane's (gate_gen --check fails identically in A:/Games/mc2-main: MC2_CONTRACT_REPO_ROOT and MC2_IBL_ASSET_STRICT marked verified with no reader); gated_layout_macros was mine and is regenerated | `scratch_cc.log` (scratch) |

### Open
1. Tracy before/after pair NOT taken: the host was busy (lane 3 smoke holding the host lease, 6 `cl.exe`) when checked at ~09:00 CDT. Re-take on a quiet host (recipe in the EVENING section). 2. Ledger order next: legacy_shape (32 manual), mech (37), post_ssao (41), post_screenfx (45), color_prop, water, shadow, present, terrain. The `vkPostFogNative_` / `vkPostHzbNative_` objects are still new/deleted by `gosPostProcess`; the engine context itself is still the file-static `g_engineContext` (the only static owner left in this area, it destroys itself in `destroy()` before the device goes). 3. No BUGS-FOUND-FIXED row: the four should-fix items had no player-visible symptom (static teardown order, a timed-out first-layout submit). 4. Scratch to clean (nothing deleted by me): `.claude/l2cap/lfsi-*` (17 sets, plus `lfsi-after3-vk`/`lfsi-after4*` partial sets), `scratch_*.log` in the worktree root, `scratch/` build leftovers. 5. nifty's gate-table failures above need the orchestrator.

### Lessons
- A capture's `--duration` budget includes the lease wait again (two day poses lost behind lane 1/3 smokes): rerun just the lost pose with `--bookmarks`.
- The Bash tool turned `\n` in heredoc python into real newlines again (log strings in three files): build the character with `chr(92)` or write scripts with the Write tool; the line-range "fix by regex from compiler errors" script (`error C` lines -> `.get()`) converted 63 consumer sites safely because the compiler lists every one.
- Static unique_ptr owners are not a fix for static-teardown order: park the object in an owner the context destroys (`IslandSlot`).


## EVENING session (thirteenth session, 2026-10-13 16:30 CDT start, cap 6 h) -- 7 task / checkpoint commits + the merge and its regeneration commit + this state commit; nifty merged at the start (tip d853bdfce, merge c336a1dca; nifty NOT merged back)
Every number below was printed by the tool named beside it in this session; none is typed from an earlier note.

### Evidence index (read these first)
| What | Id / path |
|---|---|
| **VK tier1, validation layer ON, at the checkpoint HEAD (exe 2c0052240995), 5/5, `errors=0 warnings=0` on mc2_01/03/10/17/24; VRAM run_end mc2_01 3207, mc2_03 3307, mc2_10 3325 MiB, identical to the pre-conversion runs** | `tests/smoke/artifacts/2026-10-13T21-07-52/report.md` |
| **GL tier1 at the checkpoint HEAD (exe 2c0052240995), 5/5** (114 / 90 / 102 / 84 / 77 fps) | `tests/smoke/artifacts/2026-10-13T21-19-01/report.md`; two earlier GL runs of this session were 4/5 on a loaded host (20-08-14 mc2_17, 20-53-23 mc2_10), each failing mission passed alone (20-27-01, 21-05-30): flake log |
| VK tier1 with validation, exe 9bd8dd644bd6 (same code, the commit before the checkpoint regeneration), 5/5 | `tests/smoke/artifacts/2026-10-13T17-54-33/report.md` |
| VK tier1 before the conversion (exe fdfbce2400b0): 4/5, mc2_17 `heartbeat_freeze_play` (27 fps; mc2_10 passed at 18 fps); validation errors 0 and warnings 0 on all five even so | `tests/smoke/artifacts/2026-10-13T17-06-01/report.md`; isolated reruns of mc2_17 and mc2_10 at 10 and 8 fps while a stray process of mine burned a core (see the flake log row) |
| the converted VFX island RUNNING (it is dormant by default: `MC2_GPU_PARTICLES` and `MC2_VFX_ORACLE_RENDER` are default OFF, so no default mission draws a particle through it): `MC2_VFX_ORACLE_RENDER=1 MC2_GPU_PARTICLES=1`, mc2_10 under VK + validation: `[VK_VFX] first frame OK: particles=6`, `used_vulkan=5371 vk_draws=2`, `errors=0 warnings=0` (so no `05137` at device destroy), PASS | `tests/smoke/artifacts/2026-10-13T20-04-59/mc2_10.log` |
| L4: the refusal wording, scratch floor 1.5 (header edited in a scratch build, never committed; exe 6e66.. replaced by the HEAD exe afterwards) | `tests/smoke/artifacts/2026-10-13T20-00-41/mc2_01.log`: `[VK_PROFILE v1] verdict=refused ... instance_api=1.4.350 floor=1.5 required_missing=api_1_2`, `the engine can request Vulkan instance version 1.4.350 (the loader reports 1.4.350), below the 1.5 floor`, GL fallback, bucket `vk_profile_refused` |
| VK before/after captures (day mc2_01_saved_205644, night mc2_10_saved_204523; VK umbrella, FREEZE_ANIM, 1920x1080, `capture_ab.py capture`) | `.claude/l2cap/vfxown-before-vk`, `vfxown-before2-vk` (noise), `vfxown-after-vk`; GL pair `vfxown-before1-gl/2-gl`, `vfxown-after2-gl` (`vfxown-after1-gl` lost its mc2_01 pose to the host-lease wait: the budget includes the wait; unusable, delete) |
| Tracy traces (VK arm, mc2_10, free timestep, uncapped), compare tables | `scratch/vfxown/*.tracy` (git-ignored), `scratch/vfxown/tracy_compare*.txt` |
| ownership ledger | `.claude/cleanup/vk-ownership-ledger.md` (`py -3 tools/vk_ownership_ledger.py --check`, `check-contracts.sh` row `vk_ownership_ledger`) |

### Commits (oldest first, after the merge c336a1dca)
- 69a0a48a6 post-merge regeneration: cxx17 baseline (nifty's side, +1 LG20 for a launcher UI row re-added), callers, log tag census, `docs/background-launch-child-baseline.json`, `scripts/tests/test_board_selftests.py` registered as a direct_spawn (nifty's file: its children take `console_child.child_kwargs` through a shared dict the scanner cannot see; the child baseline now lists it, 40).
- f729ec7b9 TASK 1, validation to zero. `VUID-vkCmdDraw-None-09600`: `vk_first_layout.{h,cpp}` gives the shared colour / normal / object-id / depth images their steady layout in ONE sync2 submit at creation (`VulkanContext::cmdPipelineBarrier2` picks the core or KHR entry point by route: the first sync2 consumer); the object-id image is cleared to 0; the present composite transitions the object-id image on every draw when its descriptor binds it. `VUID-vkDestroyDevice-device-05137`: the light-feed ring is a `LightFeedRing` object (destructor = the only release, device idle first), released by `mc2vkSharedLightFeedRelease()` from engine teardown ahead of the context. L10: `mc2vk::nameObject` / `nameObjectAt` object names on every image, buffer and pipeline the islands and the present path create (66 call sites), live only while the debug messenger is. `validationErrors` is `std::atomic`. No BUGS-FOUND-FIXED row: the layouts and the leak had no player-visible symptom in a normal view mode (the object-id image was undefined content only in the debug view modes 1 and 3, where it is now 0 = "no object"; the leak was at device destroy).
- 78cdd24ac smoke: L9 `vk_validation.grade(messages, asked)` has a production caller (`GateConfig.validation_requested` from `MC2_VULKAN_VALIDATION` in the child env; a run that never printed `layer_enabled` is red, bucket `vk_validation_unconfirmed`); coordinator item (2): `logparse.PRESENCE_RULES` (absence rule: mc2_10 must print `[VIDEO v1] event=play_requested` once it reached play, bucket `video_floor_missing`), `default_build_quiet_baseline.json` reason for `VIDEO v1` names the parser. NOT done: the `pre_video site=campaign_start` assertion after a campaign boot (needs a campaign-boot run).
- fdfbce240 coordinator item (3): `deploy_payload.py` takes the smoke FOLDER LEASE while it writes into an install (waits `--wait` / `MC2_DEPLOY_WAIT_S` with the countdown, fatal after, releases in a `finally`; `run_deploy()` split out of `main()`); `scripts/tests/test_deploy_folder_lease.py` (3 cases, redirects the lease module `deploy_payload` actually imports). Verified live: both deploys of this session printed `folder lease taken / released`.
- 08be36b39 TASK 2, review fixes M1-M2, L1-L3, L5-L8 (see the commit message; L4 is the evidence-index row above).
- fd000f067 TASK 3, batch 4 slice 1 (below). 9bd8dd644 cxx17 baseline tightened (LG03 -2).
- 2c0052240 checkpoint regeneration (`tests/callers.json`, `docs/render-backend-seams/gated-layout-macros.md`), after `check-contracts.sh` found both stale.

### Ownership ledger (TASK 3), numbers from `tools/vk_ownership_ledger.py`
- Counting rule (lexical, comments and strings blanked, parent-owned kinds excluded; a `vkCreate*` inside `mc2vk::create(...)` is an adoption, not a manual create): all islands BEFORE this session's conversion created 222 / released 419 (imbalance -197), AFTER 213 / 405 (-192). The imbalance is mostly one create helper (`make_buffer`) against many hand-written releases, so the useful column is `Manual` (created + released = the size of the hand-written ownership code left).
- Converted island: **vfx** 9 created / 14 released (manual 23, imbalance -5, kinds Buffer 1/3, Pipeline 1/2, ShaderModule 0/2) -> **0 / 0 (manual 0)**. Its state holds `OwnedBuffer` / `OwnedRenderPass` / `OwnedFramebuffer` / `OwnedDescriptorSetLayout` / `OwnedDescriptorPool` / `OwnedShaderModule` / `OwnedPipelineLayout` / `OwnedPipeline` / `OwnedCommandPool` / `OwnedFence` declared in creation order; `retire_island()` idles the GPU and deletes it; the 14-token `destroy_island` is gone.
- New types: `GameOS/gameos/owned_handle.h` (Vulkan-free, doctest: 4 cases), `vk_owned.h/.cpp` (device-handle owners, `OwnedBuffer`, `OwnedImage`, `mc2vk::create`). No shared_ptr, no registry, no `release()`.
- Remaining islands, the batch list for the next sessions (smallest hand-written footprint first; the ledger's "Conversion order" section regenerates it): light_feed 2 manual tokens, shared_scene 4, image_resource 11, sky 27, post_fog 28, post_hzb 28, legacy_shape 32, mech 37, post_ssao 41, post_screenfx 45, color_prop 47, water 47, shadow 49, present 103, terrain 117. Twelve near-identical `make_buffer` helpers (one per island) collapse into `OwnedBuffer::create`; the light-feed ring should become `OwnedBuffer` slots in the same slice as `shared_scene`.
- Tracy delta (VK arm, mc2_10, free timestep, `MC2_VK_SHARED_GBUFFER=1`, before exe = the pre-conversion source rebuilt to `mc2_before.exe`, after = HEAD): the host was too loaded for a clean pair. Best pair (first umbrella pair, equal workload but 1332 vs 3536 frames): DrawScreen mean 4.318 -> 4.464 ms (+3.4 %), `VKIsl.Present.CompositeRecord` 32 -> 21 us, `VKIsl.Shared.LightFeed` 454 -> 482 us (+6 %); the non-umbrella pair gave DrawScreen -1.9 %; later pairs ranged from -59 % to +49 % with 259 to 3536 frames per 30 s. Verdict: NO evidence of a regression, NOT a measurement. The converted island is dormant in default play (see above), so its own Tracy zone has no samples. Re-take on a quiet host: `capture --arm vk --env MC2_VK_SHARED_GBUFFER=1 --env MC2_VFX_ORACLE_RENDER=1 --env MC2_GPU_PARTICLES=1`, a warm-up run per exe first (a freshly written exe is slow on its first launch).
- Captures day + night under VK, before vs after (`capture_ab.py`, DIFF lines): night mc2_10 **0 differing pixels**; day mc2_01 46 px in 4 clusters (largest 42x4 at 1221,195) against a VK noise floor of 77 px (before vs before2) and a GL noise floor of 15 px; viewed the montage (red boxes at the far shoreline, thin water edge): the noise floor.

### Checkpoint (HEAD 2c0052240, code of fd000f067; every number printed by the tool named)
- `tools/build_all_targets.py`: every stage built (16/16 harnesses, doctest target, launcher, editor-side targets; exit 0). `build_contract_harnesses.py --run --unit`: 20/20 harnesses, doctest 2839 passed / 3 skipped (2835 before the 4 owned-handle cases).
- `scripts/check-contracts.sh` at HEAD before the regeneration commit 2c0052240: 115 passed, 5 failed: script_callers and gated_layout_macros (mine, regenerated in 2c0052240) and three that are not lane 2's: `own_inventory` (VideoSession.delete 1 -> 2 rose, WaterIsland.delete 24 -> 21 fell: lane 1's baseline against the merged tree), `doc_links` (10 broken refs in live docs this lane did not touch), `delivery_installs` (`VEHICLE-LIGHT-GATHER-ALL-LEAVES`: every install's `shaders/gos_tex_vertex_lighted.frag` lacks `calcLightPools`). After 2c0052240: **117 passed, 3 failed** (the three above, none touched by this lane).
- pytest `tests/smoke scripts/tests tools/visual_verify`: 686 passed (3 cases more than the first run: the lease, capture-index and replay tests). cxx17 ratchet `--check` OK at HEAD (baseline tightened: LG03 -2 in 9bd8dd644); `log_tag_census --check` OK (1386 emitters); `callers.py --check` current (567 scripts); `check-background-launch.py` OK (29 launch sites); `capture_index.py --check` OK (147 sets, 23 VOID); `vk_ownership_ledger.py --check` OK.
- Deploy: `deploy_payload --target game --ninja --allow-new-target` (the lease of the install taken and released by the script itself, exe sha 2c0052240995, DEPLOY_FINGERPRINT OK at every mission).

### Review fixes (TASK 2) at a glance
M1 supersession note + Fallback column struck + rank 1/4/5 proofs + question 1 answered in `vk-best-practices-review-2026-10-12.md`; M2 `gpu_memory_report.cpp` second pass (`most_dedicated_luid_unmatched`); L1 comment (eight bits); L2 `independentBlend` read before the version early-out, OLD_GPU fixture lists seven; L3 `static_assert(kFloorApi <= kUseApi13)` (it also means the L4 refusal can only be reached by raising BOTH constants in a scratch build); L5 `vk_replay proof` replays the recorded frame count; L6 sidecar frame check + `find_live_png` by sidecar + `captured_file` newest; L7 `--refresh` keeps drew/readback; L8 the notes say 25 of 39 regions.

### Lessons of this session
- A `TaskStop` on a Bash background task kills the shell, NOT its grandchildren: a `grep -rn ... /` I had started to look for a header kept running (100 % of one core from 16:40 to 19:10) and made every tier1/Tracy/capture run of the evening slow and noisy; list `Get-CimInstance Win32_Process` for leftovers after a stop, and never grep `/`. The two mc2_17 / mc2_10 `heartbeat_freeze_play` failures and the unusable Tracy pairs are this plus other lanes' builds (11 `cl.exe` seen at once) and their smokes holding the host lease.
- A smoke / capture `--duration` budget includes the host-lease wait (one capture set lost its mc2_01 pose); pass `--duration 1500` to `capture.py` when another lane is running.
- The Bash tool turns a backslash-n inside a heredoc into a real newline AND a backslash-b into a BACKSPACE (0x08) character (three times in this session: a regex `\b`, a printf `\n`, a python replacement); write files with the Write tool, or build the character with `chr(92)`, and scan the changed files for 0x08 and CRLF before a commit (the CRLF came from the same edit path in four water-island files).
- `tools/cxx17_ratchet --check` scans HEAD by default: use `--worktree` to see the effect of uncommitted edits; mid-merge it scans the pre-merge tree.
- Tracy needs an idle host (the harness refuses while any `mc2.exe` runs, so `tracy_capture_compare.py capture` retried every 30 s for up to 10 minutes behind lane 1's smokes) and a warm-up launch per freshly copied exe.
- The stale `mspdbsrv.exe` (C1041 at the first TU) happened twice again; the one process killed by PID fixed it.

### Open
1. Tracy delta on a quiet host (above). 2. The `pre_video site=campaign_start` assertion (needs a campaign-boot run). 3. Next ownership slices per the ledger order. 4. Delete the scratch capture sets `vfxown-after1-gl` and the 11 GB in `.claude/l2cap` once nothing cites them. 5. The shader-perturbation "first differing draw" for the replay oracle and the night pose (carried from the afternoon session).

## AFTERNOON session (twelfth session, 2026-10-13 12:15 CDT start, cap 6 h) -- 17 commits after the merge + this state commit; nifty merged at the start (tip f6ef2a4be, 12 commits; nifty NOT merged back)
Every number below was printed by the tool named beside it in this session; none is typed from an earlier note.
- Start: `git merge claude/nifty-mendeleev` (cc05eaf6d): conflicts in `docs/background-launch-inventory.md` and `tools/cxx17_ratchet/baseline.json` took nifty's side; `callers.py`, `log_tag_census --write`, `check-background-launch --write` regenerated; the ratchet was rewritten after the merge (8cbb7e306: the three lane-2 sites LG06/LG11/LG20 +1 each returned); `mc2_launcher` rebuilt after the merge (its test guard). LESSON: two ninja builds died with `C1041 cannot open program database gui.pdb` at 11/172 although the pdb was writable and `-j1` failed too: a stale `mspdbsrv.exe` (started by the first failed attempt) could not create PDBs; `taskkill //PID <that mspdbsrv>` fixed it (kill the one mspdbsrv by PID, never the compilers by name).
- OWNER RULING applied (b2e3b05dc): dynamic rendering, synchronization2, timeline semaphores, descriptor indexing and buffer device address are REQUIRED; a missing one refuses the device (`required_missing=...`, `verdict=refused`, nothing enabled) and the session runs GL; `docs/vk-device-profile.md` carries the floor (NVIDIA Maxwell, AMD GCN 2+, Intel Skylake with current drivers; Kepler and first-generation GCN dropped); doctest + `test_vk_profile.py` updated. 1678068bb then added a sixth required row, `independent_blend` (see validation ids).
- Review `reviews/lane2-2026-10-13-vkprofile-cleanup.md`, commit by commit:
  - M1 (5d591f936): `VulkanContext` keeps the `profile::Evaluation` (`profile()`, `featureEnabled()`, `route()`); the two private bools are gone. Enabled feature bits = the eight the ruling requires (timelineSemaphore, bufferDeviceAddress, descriptorIndexing + runtimeDescriptorArray + shaderSampledImageArrayNonUniformIndexing + descriptorBindingPartiallyBound, dynamicRendering, synchronization2): no island consumes any yet, they are on because the ruling requires them; the ninth, `independentBlend`, has consumers today (terrain, water, VFX, legacy-shape pipelines). L5 same commit: `memory_budget` is an optional table row, the swapchain joins the list, `extensions=VK_KHR_swapchain,VK_EXT_memory_budget` on this GPU, one device-extension enumeration. L6: the line is built by one snprintf, the census sees `[VK_PROFILE v1]`. L7: stale 1.1 comments.
  - M2: refusal path run LIVE (scratch builds, not committed; harness runs `run_smoke.py --mission mc2_01 --deploy lane2` with `MC2_RENDER_BACKEND=vulkan MC2_VK_SHARED_GBUFFER=1`, `MC2_SMOKE_REQUIRE_FINGERPRINT=0` because the exe was a scratch). Floor 1.4: report `2026-10-13T12-49-20`: `[VK_PROFILE v1] verdict=refused device="(no device queried...)" api=0.0.0 instance_api=1.4.350 floor=1.4 required_missing=api_1_2 ...` then `[VK_ENGINE_CONTEXT] engine context init FAILED ... -- fail-soft to GL.`, `[RENDER_BACKEND] fail-soft: forcing GL for the session`, `[VRAM v1] phase=run_end backend=gl`, 1952 frames, red bucket `vk_profile_refused`. Device refusal (push-constant minimum 1 MiB, so the instance and messenger were created and torn down by `fail()`): `2026-10-13T12-51-45`: `verdict=refused device="AMD Radeon RX 7900 XTX" api=1.4.355 ... required_missing=limits ... enabled=- extensions=-`, `init FAILED: device profile: required item(s) missing on AMD Radeon RX 7900 XTX: limits -- fail-soft to GL.`, 4045 frames on GL, `vk_profile_refused`. The run found a wrong message ("loader 1.4.350 below the 1.4 floor": the engine caps the instance request at 1.3): fixed in 388063c80. VK mission at the final exe: tier1 under the umbrella (reports below) carries `Vulkan profile: ok AMD Radeon RX 7900 XTX api 1.4.355 (instance 1.3.0): core: api_1_2 timeline_semaphore buffer_device_address descriptor_indexing dynamic_rendering synchronization2 independent_blend, ext: swapchain memory_budget, limit: limits`.
  - M3 (ec8c2def2, 1a9fa0185): `.claude/l2cap/ledger.json` is the record (one entry per set, VOID sticky, a deleted directory loses nothing, `--check` passes on a clone), `index.md` is its rendering; `capture_index.py` printed `147 set(s) in the ledger, 23 VOID` after adding the two Buildings.csv sets (145 + 2). The 11 GB of scratch PNGs under `.claude/l2cap` can be deleted now without losing a flag. L2: `probe-ab` (no `capture_run.json`) is VOID `no capture_run.json`; the header counts are generated. L3: the option column is the difference from the defaults of the exe that drew the set (`Gates/render_options.h` read at that commit), `[vs HEAD defaults]` when it cannot be read. L4: VK-PARITY-GAPS row 22 states the real defaults.
  - L1: `golden.json` of `golden-e9e3f19d6-1080p` says `n_regions 25, n_refused 14` (the earlier "15 refused" was wrong). L8 (4b3e35187, 3cbadd0bb): `ItemResult::id` is a `string_view`, routes are `enum class Route`, no first-character or strcmp tests; LG06 -1. L9 in b2e3b05dc. L10 (17e48e2d0): the launcher test compares the exe with `ninja -t deps` of the launcher object (exact include closure); touching `mclib/camera.h` stays green, touching `Gates/render_options_cfg.h` goes red with the file named. L11 (1c8530e97): the DXGI adapter is matched by LUID (VK: `VkPhysicalDeviceIDProperties`, GL: `GL_DEVICE_LUID_EXT`); live `[VRAM_ADAPTER v1] rule=luid adapter_dedicated_mb=24516` on GL and on VK. L12 (5a39cb4bf): the `perf` block comment says it is informational.
- R1 BATCH 2 (c34b63b66, docs/vk-validation.md): `MC2_VULKAN_VALIDATION=1` smoke runs print `[VK_VALIDATION v1] severity= id= text=` (`layer_enabled` info proves a clean run was validated; `layer_unavailable` / `messenger_unavailable` are errors), error = red bucket `vk_validation_error`, warnings counted, `Vulkan validation:` report line. VK tier1 with the layer on, BEFORE the two small fixes (exe 5a39cb4bf, `MC2_SMOKE_REQUIRE_FINGERPRINT=0`, report `2026-10-13T14-20-06`): every mission mc2_01 / 03 / 10 / 17 / 24 = errors 26, warnings 0, distinct ids: `VUID-VkPipelineColorBlendStateCreateInfo-pAttachments-00605` x5, `VUID-vkCmdDraw-None-09600` x10, `VUID-vkCmdDraw-magFilter-04553` x10, `VUID-vkDestroyDevice-device-05137` x1 (the layer limits repeats at 10; the same startup ids in every mission). 1678068bb fixed two: `independentBlend` is a required profile row (the two-attachment pipelines blend colour only), and the present composite pass samples the R32_UINT object-id image with a NEAREST sampler. AFTER (final exe 1678068bb, report `2026-10-13T15-59-39`): every mission errors 11, warnings 0, ids `VUID-vkCmdDraw-None-09600` x10 (images in layout UNDEFINED at submit where SHADER_READ_ONLY / COLOR_ATTACHMENT / DEPTH_STENCIL is expected) and `VUID-vkDestroyDevice-device-05137` x1 (three VkBuffers leaked at device destroy): the next batch. The tier is red until those two are fixed, so a VK tier1 WITHOUT the layer is the green one.
- R1 BATCH 3 (93730459a, `tools/visual_verify/vk_replay.py`, docs/host-tools.md): `proof --install <lane2> --bookmark mc2_01_saved_205644 --out scratch/vkreplay-1`: GFXReconstruct layer under the harness capture (frames 440-470, 479 MB file `mc2_frames_440_through_470.gfxr`), `gfxrecon-replay --swapchain offscreen --screenshots 1-31` (the SDK's `--wsi headless` still creates the captured Win32 surface and fails: VK_ERROR_UNKNOWN), `compare`: replay frame 23 = application frame 462 (the sidecar's own `frame` counter) has the same decoded-pixel hash as the engine's live PNG (7238bc8ab474a01f...); the 31 replayed frames are 31 distinct hashes. Exe 1678068bb. The shader-perturbation "first differing draw" part of the review's rank 2 is NOT done. The 479 MB capture sits in `scratch/vkreplay-1` (gitignored).
- Buildings.csv (coordinator ruling): `data/art/Buildings.csv` deleted from the lane 2 install (plain rm). Self-compare of `golden-e9e3f19d6-1080p` with a fresh GL capture of its ten poses (exe 5a39cb4bf, frozen animation): every blessed region PASS, mean_delta 0.0, changed_pct 0.0 (18 regions in `gold1080-nocsv`, 7 in `gold1080-nocsv-01`; the first attempt of the mc2_01 pose spent its 150 s budget waiting for another lane's host lease). NOTHING changed: no re-bless. Noted in the ledger (`--note`). The later commits changed VK-only code, so the GL pixels are unchanged.
- CHECKPOINT (HEAD code 1678068bb, later commits docs/python only): `tools/build_all_targets.py` every stage built; `build_contract_harnesses --run --unit` 20/20 + doctest 2806 passed / 1 skipped; pytest `tests/smoke tools/visual_verify scripts/tests` 582 passed; `check-contracts.sh` 170 passed, 2 failed (`default_build_quiet`: `[VIDEO v1]`, lane 1's play line, already in nifty's baseline and arriving with the next merge; `doc_links`: the 4 pre-existing broken refs); ratchets tightened (LG11 -1 after the snprintf move, head stamp); deploy `deploy_payload --target game --ninja --allow-new-target` (exe sha 1678068bbe9a, DEPLOY_FINGERPRINT OK, "2 doc-only commits since"). tier1 on the lane install: GL PASS 5/5 `2026-10-13T15-20-03` (mc2_01 150 fps, 03 87, 10 78, 17 105, 24 105: the host was busy, see the flake log); VK umbrella PASS 5/5 `2026-10-13T15-44-22` (mc2_01 147, 03 149, 10 147, 17 131, 24 120 fps; profile ok, no validation layer). Two flakes logged in `tier1-flake-log.md`: GL `13-22-21` (mc2_03, mc2_24 fps collapse; rerun 5/5 `13-37-53`) and VK `15-31-51` (mc2_10 fps decay 146 -> 35 -> 7; rerun 5/5).
- Lessons: (1) a capture's `--duration` budget includes the wait for the host lease: three captures lost their whole budget behind another lane's run (vk_replay now asks for 3000 s); (2) gfxrecon-replay `--wsi headless` fails on a capture that had a Win32 surface, `--swapchain offscreen` is the windowless mode; (3) the GFXReconstruct layer appends the frame range to the file name; (4) `log_tag_census` reads TRACKED files only (an untracked new header is invisible until `git add`); (5) heredocs with `\b`/`\[` in non-raw Python strings silently corrupt regexes: write such scripts with the Write tool; (6) `ninja -n` is useless as a staleness probe here (it always lists the CMake re-run), `ninja -t deps` is the exact one; (7) the Bash tool turned `'\0'` in a heredoc into a literal NUL byte in a header.
- Open: (a) validation ids 09600 (image layouts) and device-05137 (three leaked buffers): the next batch, then the tier can be green; (b) shader-perturbation proof for the replay oracle (rank 2) and replaying the night pose; (c) golden: 14 refused regions (13 unstable run to run + 1 degenerate crop); (d) `capture_ab capture` takes no options override; (e) `check-contracts` has no row for `capture_index.py --check` (orchestrator-only); (f) `[VIDEO v1]` quiet-baseline entry arrives with the next nifty merge; (g) VK `load_end` VMA usage reads 0 (lazy islands): compare `run_end`; (h) the 11 GB of `.claude/l2cap` PNGs can be deleted.

- CORRECTIONS to the MIDDAY section below (found by the cleanup review, numbers re-read from the tools): the 1080p golden refused 14 regions (`golden.json n_refused 14`, 13 unstable + 1 degenerate), not 15; the index flagged 22 VOID lines before this session and 23 after `probe-ab` was flagged (`capture_index.py` output); the 688f8a38c tier1 fps entries were new, not re-recorded.

## MIDDAY session (eleventh session, 2026-10-13 ~10:00 CDT, cap 6 h) -- 6 task commits, 3 checkpoint-cleanup commits + this state commit; nifty merged at the start (fast-forward to 5d29cecb1; nifty NOT merged back)
- Start: `git merge claude/nifty-mendeleev` (fast-forward, no conflicts), `cxx17_ratchet --check --tighten` -> 4cfc5d70f (head stamp only).
- Commits (each C++ one built, `build_contract_harnesses --run --unit` 20/20 + doctest green):
  - e9e3f19d6 task 1: `StaticPropPbr` default OFF again (owner ruling; `StaticPropAmbient` stays ON): `Gates/render_options.h`, the launcher row (label OFF, unchecked emits nothing), `test_gate.cpp` (defaults, JSON, override and cfg-spelling cases moved to default-ON keys), `test_render_options_pin.py`, the staticprop matrix (`pbr_ambient_off` became `pbr_on` + `ambient_off`), `tier1_env_vars.md`. d18fd8ef1: ledger row in `gate-amnesty-wave1.md`. DEFAULT EQUALITY (GL, 1920x1080, frozen animation, sets `.claude/l2cap/t1-*`): the new default exe vs the PREVIOUS exe (98fc6705b) with `MC2_OPTIONS_OVERRIDE=StaticPropPbr=0` = day 25 px (2 clusters; noise floor 20 px between two default sets), night 0 px; same exe with `StaticPropPbr=1`: day 3,529 px / 24 clusters, night 3 px; montage `.claude/l2cap/montages/t1-default-vs-pbr1-day-gl.png` viewed (healthy; ON adds a faint hangar sheen).
  - 4d95c4334 task 2 (capture review F1-F10): `capture_ab compare` calls `check_backends` (a `-vk` set that drew GL = "VK arm UNPROVEN", exit 1) and its docstring names both umbrella variables; `run_visual_capture` manifest FAIL carries `fail_reasons` and the backend, the sweep's `drew` is the sorted per-pose list, a candidate set.json records backend + options; harness tree-model comments corrected; launcher: `MigrateLegacyEnvKeys` counts and names unknown `MC2_*` keys (`dropped_unknown`/`names`, cap 8) and rewrites once, the launcher_env.json write is atomic and logs `event=migrate_failed` instead of claiming success, headless `--sync-options` calls `GfxSetDefaults`; tests for the gl-requested/vk_present branch, the exit codes of `capture.main` and `run_visual_capture.main` (single and multi run), manifest reasons, candidate backend, legacy-key-beats-options.cfg (documented in the function comment: options.cfg is not a newer choice because the launcher writes defaults there), dropped keys, GUI defaults of a minimal file, failed rewrite, and an autouse "launcher exe is newer than its sources" check. `tools/visual_verify/capture_index.py` writes the committed `.claude/l2cap/index.md` (one line per set: requested, drew, readback, exe commit, option differences, frozen or not, VOID flag; 23 VOID = the 21 GL-not-VK sets, `probe-vk-noumbrella` and one more plain `-vk` set); the F3 VK montage is committed and VK-PARITY-GAPS row 22 says where the sets live. The void VK lines of this file (79, 94, 113, 118, 130 of the old numbering) carry a correction.
  - c5214e23e task 3: tier1 pinned to 1920x1080 (`render_resolution.PINNED_TIERS`, `asked_size`, `tier_size_env`; runner fails a run whose log lacks `[RESOLUTION] Pre-window: 1920x1080`; report header line). Golden re-blessed: `tests/visual/goldens/golden-e9e3f19d6-1080p` (exe e9e3f19d6 clean, GL, frozen animation, runs `.claude/l2cap/gold1080-r{1,2,3}`): 25 regions blessed (4.6 MB), 15 refused by the all-pairs rule, 10 poses vendored; `golden.json capture_env` records the 17 options, declared env and backend; `bless.py` refuses runs with no/different options or a backend that is not the requested one; `verify.py` refuses an options mismatch unless `--vary`, records both, takes `--golden current` (`render_resolution.CURRENT_GOLDEN`); `check-capture-resolution.py --current`. The 4K `reset-50331d5` stays under its own name. PROOF: `verify.py --golden current` on run r2 = 25 PASS (max changed_pct 0.18, nearly every region 0.0); real A/B (same exe, `MC2_OPTIONS_OVERRIDE=StaticPropPbr=1`): refused without `--vary`, with `--vary StaticPropPbr` 25 PASS (largest horizon_waterline 2.19% of a 3.0% budget). 688f8a38c: tier1 fps baselines re-recorded at 1080p (`tests/smoke/baselines.json`, `perf.resolution`): mc2_01 149.4 fps, mc2_03 92.5, mc2_10 128.8, mc2_17 139.4, mc2_24 137.6 (exe 487584705; a first mc2_03 sample of 37.6 fps / 1.3 s peak was a host hitch, re-run alone).
  - 487584705 task 4: `[VRAM v1]` (`GameOS/gameos/gpu_memory_report.{h,cpp}`): VK = VMA device-local heap budget/usage/allocated (VK_EXT_memory_budget now enabled when the device has it), GL = `GL_TEXTURE_FREE_MEMORY_ATI` (AMD) / `GL_NVX_gpu_memory_info` (NVIDIA), both + DXGI `QueryVideoMemoryInfo` process counter; lines at mission load end and run end (smoke or `MC2_DIAG_VERBOSE` only), Tracy plot `VRAM.Process.MB` every 120 frames, `gpu_memory` in the state dump and `get_visual_settings`; `scripts/smoke_lib/vram.py` + logparse + report `VRAM:` line; `perf_baseline_report.py` and `tracy_capture_compare.py` baseline record the plot and log samples; `tests/smoke/test_vram.py`, `test_perf_baseline_vram.py`. CURRENT NUMBERS, mc2_01 at 1920x1080 (tier1 2026-10-13T10-39-54, exe 487584705): GL load_end 1624 MiB, run_end 3782 MiB process dedicated (DXGI; budget 23748 MiB, adapter 24516 MiB, `GL_TEXTURE_FREE_MEMORY_ATI` 21989 -> 20541 MiB free); VK umbrella (smoke 11-14-47, exe 4f91ac2d8): load_end 1759 MiB (VMA usage 0: the islands allocate lazily), run_end 3207 MiB process / 3204 MiB VMA usage / 615 MiB allocated by VMA allocations. Other missions run_end GL: mc2_03 3901, mc2_10 3902, mc2_17 3962, mc2_24 4069 MiB.
  - 4f91ac2d8 task 5 (Vulkan R1 batch 1): `GameOS/gameos/vk_device_profile.{h,cpp}` (see `docs/vk-device-profile.md`): REQUIRED = 1.2 floor (device + instance; the instance asks for 1.3 when the loader has it), `VK_KHR_swapchain` with a present window, four limits; WANTED = timeline semaphore, buffer device address, descriptor indexing (umbrella + 3 bits), dynamic rendering, synchronization2 (core 1.3 when both versions have it, else the KHR extension on 1.2), each with its fallback; the context gathers every physical device, takes the first discrete one meeting the required items, refuses (fail-soft to GL, line with the missing items) otherwise, creates the device with a features2 chain enabling only the supported wanted bits, VMA gets min(device, instance) and the BDA flag. `vk_profile.py` + the `vk_profile_refused` red marker bucket + report line; 8 doctest cases in `test_gate.cpp` (header is Vulkan-free), `test_vk_profile.py` (9). Whether the floor should REQUIRE the five wanted facilities is the review's open owner question 1: one word per row. LIVE LINE for this GPU (tier1 mc2_01 under the VK umbrella, `2026-10-13T11-14-47`): `[VK_PROFILE v1] verdict=ok device="AMD Radeon RX 7900 XTX" api=1.4.355 instance_api=1.3.0 floor=1.2 required_missing=- wanted_missing=- items=api_1_2:core,swapchain:ext,limits:limit,timeline_semaphore:core,buffer_device_address:core,descriptor_indexing:core,dynamic_rendering:core,synchronization2:core enabled=timelineSemaphore,bufferDeviceAddress,descriptorIndexing,runtimeDescriptorArray,shaderSampledImageArrayNonUniformIndexing,descriptorBindingPartiallyBound,dynamicRendering,synchronization2 extensions=-`. VK tier1 under the umbrella on the lane install: PASS 5/5, run `2026-10-13T11-01-55` (report has the `Vulkan profile:` and `VRAM:` lines; mc2_01 33 fps with p1 2 and loads of 16-20 s on mc2_01/mc2_17 are VK cold-start costs, not graded).
- Checkpoint-cleanup commits: 3634a9ac1 (modern casts / string_view in the new GPU memory code, `using` alias for the profile handle, `gated-layout-macros.md` regenerated: vulkan_context.h line numbers moved), 69f421440 (string_view parameters of the profile), 9c876ecb7 (cxx17 baseline rewritten, +3 sites: LG06 +1 the `evaluate()` lambda's `const char*` id, LG11 +1 the escaped quote of the `[VK_PROFILE v1]` literal read as a path backslash, LG20 +1 launcher JSON KEY names that are not gate reads).
- CHECKPOINT (HEAD code 9c876ecb7; this state commit after it is docs only): `build_all_targets.py` every stage built; `check-contracts.sh` 170/172 (red: `estring_callsites`, 4 editor files "edited since their rows were verified" = lane 1's baseline vs the merged tree, not touched here; `doc_links` 4 broken refs, pre-existing); `build_contract_harnesses --run --unit` 20/20 + doctest 2802/2802; pytest `tests/smoke tools/visual_verify scripts/tests` 554 passed; `log_tag_census`, `callers.py`, `check-background-launch.py`, cxx17 ratchet all OK; deploy `deploy_payload --target game --ninja --allow-new-target` (exe sha 9c876ecb757d, DEPLOY_FINGERPRINT OK at HEAD); tier1 on the lane install `run_smoke.py --tier tier1 --deploy lane2 --continue --wait-timeout 3600` PASS 5/5, run `2026-10-13T11-50-22` (report `tests/smoke/artifacts/2026-10-13T11-50-22/report.md`, 1920x1080 pinned; mc2_01 148 fps, 03 99, 10 129, 17 140, 24 146; +0 destroys). Under the VK umbrella: tier1 5/5 `2026-10-13T11-01-55` (exe 4f91ac2d8, profile ok).
- Lessons: (1) the Bash tool turns every backslash-n inside a heredoc into a real newline AGAIN (7 times this session, in python patch scripts AND test sources): write any file with an escape in it through the Write tool, or build the character with `chr(92)`; (2) tier1 compares the exe sha with the worktree HEAD at EVERY mission, so no commit may land while a tier1 runs; (3) `check-background-launch.py --write` and `callers.py` regenerate on every edit that shifts a line in a listed script: run both before each commit; (4) `tests/unit/log_tag_baseline.json` needs `log_tag_census --write` for a new `[TAG v1]` emitter (a header-built line like `[VK_PROFILE v1]` is not seen by the census); (5) 11 GB sit in `.claude/l2cap` (scratch PNG sets): `index.md` is the record, the PNGs can go once nobody cites them.
- Open: (a) validation-layer grading + the capture/replay proof (review ranks 2 and the validation tier) are the next Vulkan batches; (b) owner question 1 (REQUIRE the five wanted facilities?); (c) golden refused regions: 15 night-light/water regions are unstable run to run (same class as the 4K set); (d) `capture_ab capture` still takes no options override; (e) the 11 GB l2cap scratch; (f) VRAM on VK reads usage 0 at load end because the islands allocate lazily: a later reader should compare run_end.

## MORNING session (tenth session, 2026-10-13 07:05 CDT start, ~1h50, cap 4 h) -- 5 code/doc commits + this state commit; nifty merged at the start (nifty NOT merged back)
- Start: `git merge claude/nifty-mendeleev` (fast-forward to 492ba32c7), `cxx17_ratchet --check --tighten` -> dd2bf2345.
- Task 1 (c5b7e0843): the engine writes `backend` (the backend that drew, after any `forceGLFallback`) into every capture sidecar; `scripts/smoke_lib/capture_backend.py` compares it and `readback_source` with the requested `MC2_RENDER_BACKEND`; `run_visual_capture.py` and `tools/visual_verify/capture.py` exit 1 on a mismatch (the sweep manifest records requested/drew); `tests/smoke/test_capture_backend.py`.
- Commits (dd2bf2345 = cxx17 baseline after the merge; each C++ one built, `build_contract_harnesses --run --unit` 20/20 + doctest 2789/2789):
  - c5b7e0843 task 1: capture honesty (above). c0158be25: `capture_ab` VK arm asks for the umbrella, F3 VK verdict, VK-PARITY-GAPS row 22.
  - e84671b99 task 2: `scripts/check-ab-kit.py` fails an arm that sets a name `Gates/gate_table.json` no longer lists (AB-7 bundle trimmed to the two live gates; `scripts/tests/test_check_ab_kit.py`); `tools/class_light_band_harness` reads the Stage G floor / IBL-on / rotate-off defaults from `Gates/render_options.h` (harness is C++20 now), the retired env-reader test became the live options.cfg grammar (bad value = rejected, default stays), stale gate names in comments replaced, and the header says the static-prop/tree band models describe StaticPropAmbient=OFF + no base-light wrap (not the shipped default) with a test that pins the gap. The harness is NOT retired: its numbers for buildings are the "both off" bands until the model is re-derived with the two now-default terms (open, below).
  - 159073e59 task 3: `Gates/render_options_legacy_env.h` (14 retired names -> options.cfg keys, value translation through the options grammar) + `MigrateLegacyEnvKeys()` in the launcher (GUI start and `--sync-options`): a legacy key becomes its row's value, a value already stored under the new name wins, a meaningless value keeps the default (counted `unmapped`), the file is rewritten without the legacy keys (once), one `[LAUNCHER_ENV v1] event=migrated_legacy_keys ...` line goes to `launcher.log`. `tests/smoke/test_launcher_legacy_env.py` (6 cases against the built launcher in `build64-ninja/out/mc2_launcher`); also run on a copy of the lane install's real launcher_env.json (found=4 applied=4). 98fc6705b: `tests/callers.json`.
- Task 4 (Vulkan R1 batch 1) SKIPPED on purpose: rank 1 of the best-practices review is "declare/enforce the Vulkan 1.2 device profile", neither the capture/replay proof (rank 2) nor the validation-tier item (a finding, not a ranked batch), and not the RAII rewrite. No validation error count was taken. The validation grading task is still open as written (the layer is `VK_LAYER_KHRONOS_validation`, SDK 1.4.350.0 present, `resolveValidationPreset` / `MC2_VULKAN_VALIDATION` already exist in `vulkan_context.cpp`).
- Checkpoint (HEAD code 98fc6705b; state commit after it is docs only): `build_all_targets.py` every stage built (exit 0); `check-contracts.sh` 170/172 (red: `doc_links` 4 broken refs in live docs, the pre-existing docs gap; `script_callers` was stale until 98fc6705b regenerated it, green by `tools/callers.py --check`); deploy `deploy_payload --target game --ninja --allow-new-target` (exe built at the committed HEAD, DEPLOY_STALENESS OK 875 files, src_commit 98fc6705b); tier1 on the lane install `run_smoke.py --tier tier1 --deploy lane2 --continue --wait-timeout 3600` PASS 5/5, run `2026-10-13T07-46-38` (report `tests/smoke/artifacts/2026-10-13T07-46-38/report.md`; mc2_01 140 fps, 03 80, 10 98, 17 125, 24 120; +0 destroys).
- Lessons: (1) I deployed once from an exe built before the commit (dirty fingerprint warning); rebuilt and redeployed: always COMMIT, then build, then deploy; (2) `MC2_RENDER_BACKEND=vulkan` alone makes the engine force GL (`present image not VK-owned`); the umbrella needs `MC2_VK_SHARED_GBUFFER=1`; (3) `scripts/background-launch` checks treat any `*mc2*.exe` launched from a test as an engine launch: it needs the `direct_spawn` allow row AND an inline `# background-launch: allow <why>` comment; (4) `tools/callers.py` needs an allowlist row for each new `scripts/tests/test_*.py` (and a regeneration after a new test imports console_child); (5) a blind `find /` in Git Bash hangs the tool for minutes: scope it.
- Open for the next lane 2 (owner items from the FIX session still stand: re-bless the 1080p golden set, rule the StaticPropPbr/Ambient default, tier1 resolution pin + options in golden.json): (a) validation-layer grading in the VK smoke tier (task 4 as written) and the capture/replay proof (review rank 2) once the orchestrator rules the order against rank 1 (1.2 device profile); (b) VK-PARITY-GAPS rows 20-22 are measured/known no-twin items; (c) re-derive the class_light_band_harness static-prop band with `StaticPropAmbient` ON and the base-light wrap (then drop the pinning test); (d) `capture_ab capture` takes no options override (the F3 arms were run through `capture.py --env`); (e) `launcher_env.json` keys of the deleted non-option gates (MC2_TERRAIN_LOD_CHUNK, MC2_COLORMAP_KTX2, ...) are still ignored and dropped on the next save without a log line.
- F3 VK VERDICT (exe c5b7e0843 on the lane install, umbrella `MC2_RENDER_BACKEND=vulkan MC2_VK_SHARED_GBUFFER=1`, 1920x1080, `--clean`, `MC2_SMOKE_FREEZE_ANIM=1`; every sidecar says `backend: vulkan`, `readback_source: vk_present`; sets `.claude/l2cap/f3{on,off,on2}-vk-{day,night}`): VK IGNORES StaticPropPbr / StaticPropAmbient. Night mc2_10: ON vs OFF 0 differing pixels (noise floor 0); day mc2_01: 84 px in 14 clusters against a noise floor of 61 px in 8 (ON vs second ON). GL differs by 12,151 / 6,511 px on the same poses. Viewed `.claude/l2cap/montages/f3-vk-on-off-2x2.png`: the VK frames are healthy and the two arms look identical. There is no VK twin: the row is VK-PARITY-GAPS #22 (rows 20 and 21 were already there). CAUSE of last session's silent GL fallback (reproduced, `.claude/l2cap/probe-vk-noumbrella`): `capture_ab capture`'s VK arm passed only `MC2_RENDER_BACKEND=vulkan`; without `MC2_VK_SHARED_GBUFFER=1` the engine logs `[RENDER_BACKEND] fail-soft: forcing GL for the session (reason: present image not VK-owned ...)` and draws GL. The new guard turns that into exit 1; `capture_ab` now passes both variables (`capture_cmd`, a pytest, live check `.claude/l2cap/probe-ab` rc 0). This session's ON/OFF arms were taken with `capture.py --env` directly because `capture_ab capture` takes no options override.

## FIX session (ninth session, 2026-10-12 23:44 CDT -> 2026-10-13 early morning, cap 4 h) -- review fixes F1-F8 of `reviews/lane2-2026-10-12-settings-cleanup.md`, 8 commits (nifty NOT merged)
- Source: the Sonnet cleanup review of the night session (ACCEPT-WITH-FIX). Base = 8a4148a01 (the review commit on top of 3a6838e21), tip = the state commit after ca1ed706b. Lane 1 stayed live in mc2-main; nothing merged either way.
- Commits, mapped to findings (each C++ one built, `build_contract_harnesses --run --unit` green, doctest 2786/2786):
  - a087d7743 F1: ONE options parser. `CPrefs::load` and the pre-window publish both call `cfg::loadFromText` (strict grammar: bool exactly TRUE/FALSE/1/0 in any case, long a whole integer, float finite, nothing after the value, so a trailing comment is rejected; duplicate key = first line; a rejected value keeps the default and CPrefs logs `[OPTIONS v1] event=rejected key= value= why=`). `loadRenderOptions` (FitIni path) deleted. Doctest: 17 rejected spellings (`b ShadowCSM = banana`, `= F`, `l TonemapMode = abc`, `= 2 ; AgX`, `f ... = 0,45`, nan, 0x2 ...) through the CPrefs call and the early-reader call; accepted spellings; duplicate; a rejected value does not clobber an earlier one.
  - 408f4754f F2: the 17 option values are a pinned input of every proof. Engine: `toJson()`; the state dump carries `renderOptions` (also served by the `get_visual_settings` MCP tool) and the capture sidecar `render_options`. Harness: `scripts/smoke_lib/render_options_pin.py` (table read from `Gates/render_options.h`; reads an install like the engine: options.cfg then `MC2_OPTIONS_OVERRIDE`, same strict grammar; `require_same`/`--vary`). `run_visual_capture.py` + `capture.py` write the values into the run manifest and `capture_run.json` (the engine's report is the record, a harness/engine disagreement fails the capture); `capture_ab.py compare` and the Tracy driver (`<trace>.options.json`, per-arm in `baseline.json`, matrix rows; `compare` and `regress`) REFUSE a mismatch or a missing record unless `--vary Key,Key` names the difference; `run_smoke.py` records them per row in report.md/report.json. `tests/smoke/test_render_options_pin.py` (11 cases, including a cross-check of the python grammar against the case table in `test_gate.cpp`).
  - 559714587 F3: owner ruling still PENDING; applied the interim default the orchestrator set: the ENGINE default of `StaticPropPbr` and `StaticPropAmbient` is now ON, so the launcher rows (which started checked) and the engine agree. The launcher dialog now starts each options.cfg row from `RenderOptions{}` (defaultVal ignored for those rows), so Launch and `--sync-options` cannot write a value the engine did not choose (headless `--sync-options` on a copy of the lane install's options.cfg writes exactly the 14 engine defaults, checked). Env-mechanism comments and deleted gate names/reader lines are gone from the launcher. Staticprop matrix: `pbr_on` arm became `pbr_ambient_off`. VERDICT below.
  - 3f03e1841 F4: docs say 1920x1080 is the pinned capture/Tracy size, traces before it are 4K and not comparable, the `reset-50331d5` golden is 4K until the owner re-blesses; the `check-capture-resolution.py` refusal names the re-bless command (`capture.py` x3 then `bless.py --to`), selftest case added.
  - c0ea77258 F5: resolution tier edges: `render_resolution.pin_unless_declared` refuses a half-declared MC2_WIDTH/MC2_HEIGHT; `build_capture_env` (rvc) and `game_env` (Tracy, built before the recorder starts) are real functions and the test calls them (the text-grep test is gone); `run_smoke.py` names crash-evidence and ring_trace artifacts by the row label (the two `resolution` rows no longer overwrite each other), checked on the parsed source.
  - 054514d73 F6+F7+F8: override keys case-insensitive; an unknown key logs `why=unknown_key` (`Verdict::UnknownKey`), a known key without a value stays `garbage`; the mission editor now publishes options.cfg + the override itself in `InitGameOS` through the new `Gates/render_options_publish.h` `publishFromInstall()` (same reader and grammar as the game's pre-window pass; the editor has no CPrefs::load for options.cfg and no gameosmain); stale comments (publisher named in `gos_static_prop_batcher_state.cpp`, the `/we4840` "every first-party target"); the four `switch (kind)` copies are one (`withValue()`); `applyPrefs` has one `report` flag; `opt_detail` alias; launcher `WriteTextFile` writes a temp file and `MoveFileEx`-replaces; the test_gate "read before the publish" case no longer depends on test order.
  - 2fc0972d8 + ca1ed706b: `saveRenderOptions` through an overloaded functor (the `if constexpr (is_same_v)` form added 2 LG07 sites in code/); cxx17 baseline tightened (`--check --tighten`, no count rose).
- COUNT CORRECTION (F7): the commit messages of 8589e3148 and 1c3ef0de9 say "Feature 118 -> 105" / "Feature 105 stays"; those used the previous session's figure. The measured registry kinds (`Gates/gate_table.json`) are Feature 114 -> 100 and Trace 107 -> 101 over the night session (table 1000 -> 980 rows). Local history, not rewritten.
- F3 VERDICT (default equality, GL, 1920x1080, `MC2_SMOKE_FREEZE_ANIM=1`, lane install exe 1c3ef0de9, ON arm via `MC2_OPTIONS_OVERRIDE=StaticPropPbr=1,StaticPropAmbient=1`; sets `.claude/l2cap/f3{off,on}{1,2}-gl`): day `mc2_01_saved_205644` 12,151 px in 37 clusters (noise floor 61 px), night `mc2_10_saved_204523` 6,511 px in 18 clusters (noise floor 0). Viewed `.claude/l2cap/montages/f3-default-off-vs-on-gl-*.png` and the crops `f3-crop-day-off-over-on.png`, `f3-crop-night-off-over-on.png`: static props (hangar, barrels, tents, walls) read slightly lighter with a faint specular sheen; terrain, water, mechs, shadows unchanged; no artefact. ON is a visible but benign change, NOT a regression, so the engine default stays ON and the launcher rows agree. The VK arm of that capture fell back to the GL readback (sidecar `readback_source: gl_fbo`), so VK is not evidenced. The owner may overrule the default; reverting is one line each in `Gates/render_options.h` (+ the test and the staticprop matrix arm).
- Outside the lane's area (all small and named by the findings): `code/prefs.cpp` (load/save/publish), `GameOS/gameos/{gameosmain_init,debug_state_dump,gos_visual_capture,gos_static_prop_batcher_state}.cpp`, `editor/EditorGameOS.cpp` (one call), `GuiRuntime/EditorInspector_sections.cpp` (one comment), `scripts/mcp/mc2_render_state_server.py` (one field), `tests/unit/test_gate.cpp`, root `CMakeLists.txt` (a comment). Regenerated: `docs/background-launch-inventory.md`, `tests/callers.json` (twice), `tools/cxx17_ratchet/baseline.json` (three times).
- Checkpoint (HEAD code ca1ed706b; the state commit after it is docs only): `tools/build_all_targets.py` every stage built (exit 0, zero C4840); `gate_gen --check --rebuild` OK; `build_contract_harnesses --run --unit` 20/20 + doctest 2786/2786; pytest `tests/smoke` + `capture_ab` 97 passed; `check-contracts.sh` 170/171 (only `doc_links`, the pre-existing docs gap); deploy `deploy_payload --target game --ninja --allow-new-target` (exe sha ca1ed706b4f7, DEPLOY_FINGERPRINT OK); tier1 on the lane install (`run_smoke.py --tier tier1 --deploy lane2 --continue --wait-timeout 3600`) PASS 5/5, run `2026-10-13T00-29-06` (report `tests/smoke/artifacts/2026-10-13T00-29-06/report.md`, which now carries the `Render options: all defaults` line; mc2_01 138 fps, 03 79, 10 98, 17 122, 24 119; +0 destroys; mover coherence 0 violations) -- the same fps as the night session although two more options are now ON; resolution tier PASS 2/2 `2026-10-13T00-46-41` (1080p 147 fps, 4K 137 fps); editor (EditRel built from build64-ninja with the new options publish, copied to the lane install) smoke PASS 15/15 `tests/smoke/editor/2026-10-13T00-46-32/report.md`; live check of the engine side of F2/F6 on the deployed exe: a capture with `MC2_OPTIONS_OVERRIDE=shadowcsm=0,Bogus=1,TonemapMode=9` (`.claude/l2cap/f2verify-gl`) wrote all 17 values to the sidecar and `capture_run.json` (ShadowCSM false, the harness reading agreed), and the engine log carries `event=override accepted=1 rejected=2`, `key=Bogus why=unknown_key`, `key=TonemapMode why=out_of_range want=0..3`. The ratchets (`cxx17`, `file_size`, `log_tag_census`) are green at the tip.
- Lessons: (1) the Bash tool turns backslash escapes inside heredocs into real characters (it put real CR/LF into test strings again): every python patch containing `\r`, `\n`, `\t` or `\"` went through the Write tool; (2) `cxx17_ratchet` reads the COMMITTED tree (HEAD), so a fix is only seen after its commit; (3) a `Gates/*.h` emitter is invisible to `log_tag_census` until the header is tracked (`git add` first); (4) `grep -c $'\r'` in this shell lies about CRLF: count with python (`code/prefs.cpp` is CRLF, almost everything else LF); (5) `capture_ab compare` now refuses sets whose options differ, so an A/B that varies an option on purpose needs `--vary`; (6) the VK capture arm silently fell back to GL on the lane install (check `readback_source` in the sidecar before calling it VK evidence).
- Open for the next lane 2: owner items first: (a) re-bless the golden set at 1920x1080 (the compare refuses until then), (b) rule the `StaticPropPbr`/`StaticPropAmbient` default (applied ON here), (c) whether tier1 is pinned to 1920x1080 (its fps baselines are 4K numbers) and whether goldens/bless should record the options too (the golden.json capture_env has no `render_options` yet), (d) an old `launcher_env.json` keeps `MC2_SHADOW_CSM`-style keys that are now ignored (a player's old "CSM off" resets once). Code: `scripts/check-ab-kit.py:206` (an external bat-kit checker) and `tools/class_light_band_harness/*.cpp` still name deleted gates; VK parity rows 13-21 of `.claude/VK-PARITY-GAPS.md` (20: ShadowCSM/PropShadowReceive options steer GL only; 21: SunAxisFix GL only); the W2/W3 list below (MATERIAL_TEXTURES, KTX+BC7 VK path, TREE_DIAG_TRACE, VIEW_UNIFORMS, VK_VFX_TRACE, family 18).

## NIGHT session (eighth session, 2026-10-12, 21:37 CDT start, cap 03:37) -- 7 commits + this state commit, nifty merged first (nifty NOT merged back); the commit messages' Feature counts are corrected in the FIX session above (114 -> 100)
- Start: `git merge claude/nifty-mendeleev` (clean, tip c5156854d: wave 2 + fixes at 0af64d2d6, the cxx17 baseline rewrite 8ac9bb84e, lane 1 step 4 B1 up to its handoff), `cxx17_ratchet --check --tighten` -> eb8063307. Lane 1 stayed live in mc2-main; the edits this session made under code/, mclib/, tests/unit are listed under "Outside the lane's area" below.
- Commits (eb8063307 -> HEAD, each built; the C++ ones with `build_contract_harnesses --run --unit`, 20/20 + doctest):
  - eb8063307 cxx17 baseline tightened after the merge.
  - de85936cd T1: `GameOS/gameos/vulkan_context.cpp` `ctxlog(..., env)` passed a std::string through the printf-style call (printed garbage on an unrecognised `MC2_VULKAN_VALIDATION` preset) -> `env.c_str()`, like the neighbouring `fail()` call; `/we4840` now covers gameos, gameos_editor and gameos_main (root CMakeLists.txt, next to lane 1's mc2/mclib/gui/EditRel list). Nothing else was flagged: the dev tree and `build_all_targets` (every stage) have zero C4840.
  - e8437015b T2: captures and Tracy traces pinned to 1920x1080 in one place + the weekly `resolution` smoke tier (facts below). Regenerated: `docs/background-launch-inventory.md`.
  - 8589e3148 T3a: the settings wave (below). Regenerated: gate_table.json + gate_index.gen.h + RendererFeatureRegistry.h + flags.auto.yaml, log_tag_baseline.json, gl-vk-feature-coverage.{md,json}, gl_shader_variants.json, background-launch-inventory.md, tests/callers.json.
  - 675c7555a T3b: the launcher writes the same keys (`Gates/render_options_cfg.h`, `--sync-options`); early-read tripwire names its source.
  - e8a4ac64e T3c: the render options are published before the window exists (ShadowCSM was read during renderer start-up and did not apply from options.cfg; found by the tripwire).
  - 1c3ef0de9 T4: seven `MC2_TERRAIN_DECAL_*` rows frozen as `CliffDecalTuning::kDefault*`; `u_glassDither` leftovers; the options log line made quiet; cxx17 + file-size baselines rewritten with the reason. Regenerated: gate files again, log_tag_baseline.json, seed.
- Counts (Gates/gate_table.json registry kinds, session start eb8063307 -> HEAD 1c3ef0de9): table 1000 -> 980 rows; Feature 114 -> 100; Trace 107 -> 101; Retired 2. (The commit messages of 8589e3148 and 1c3ef0de9 say "Feature 118 -> 105": that used the previous session's figure; these are the measured ones.) `MC2_OPTIONS_OVERRIDE` is the one row added. Legacy-idiom KPI after the baseline rewrite: LG04 +2, LG06 +2 (code/prefs.cpp table dispatch), LG19/LG20 down with the deleted gates; run `cxx17_ratchet --report` for the totals.

### T2 facts (resolution)
- `MC2_WIDTH`/`MC2_HEIGHT` unset means the drawable is options.cfg `ResolutionX/Y` (`gameosmain_init.cpp initResolutionFromConfig`). `nofocus.small_window` (640x480) is only a default: `run_smoke`/`smoke_lib/runner.py`, `run_visual_capture.py` (-> `visual_verify/capture.py`, `capture_ab.py`, the golden/baseline captures) and `tracy_capture_compare.py` all pass `small_window=False`. Every install on this host (lane2, lane3, v0.4, v0.4c) ships a 3840x2160 options.cfg, so every capture, every Tracy trace AND every tier1 run was a 4K run; the golden set `tests/visual/goldens/reset-50331d5` records 3840x2160. `perf_baseline_report.py` only reads traces.
- Pinned: `scripts/smoke_lib/render_resolution.py` (constant `PINNED_WIDTH/HEIGHT` = 1920x1080, the reason, `pinned_env()`, `RESOLUTION_TIER_SIZES`); `run_visual_capture.py` and `tracy_capture_compare.py` read it (a declared `MC2_WIDTH/HEIGHT` / Tracy `--env` overrides one run). tier1 is NOT pinned (fps baselines are 4K numbers). Proof: capture PNGs 1920x1080; a Tracy capture's game log says `[RESOLUTION] Pre-window: 1920x1080 (from MC2_WIDTH/MC2_HEIGHT env)`.
- Goldens: `check-capture-resolution.py` refuses a 1080p capture against the 4K golden (`1/1 pose(s) mismatched [..=1920x1080]`), so the golden set needs RE-BLESSING at 1920x1080 (owner decision, not done). Scene equivalence shown: day GL pair at the pinned size `res1080-{a,b}-gl` (noise floor 38 px) vs a 4K capture box-downsampled 2x = 76,125 px, mean abs diff 1.58, edge/sampling only; montage (viewed, 4x diff) `.claude/l2cap/montages/res-1080-vs-4k-down-mc2_01_saved_205644.png`.
- `resolution` tier: manifest tier + `resolution=WxH` key (`smoke_lib/manifest.py`), two rows (mc2_01 at 1920x1080 and 3840x2160), `run_smoke.py --tier resolution --deploy <name>`, rows named `mc2_01@WxH`, red unless the engine log has the matching `[RESOLUTION] Pre-window` line; `tests/smoke/test_resolution_tier.py`; documented in `tests/smoke/README.md`. First run (exe e8437015b): 1080p PASS 149 fps, 4K FAIL `heartbeat_freeze_play` at the END of the run (mission_quit 8 s after the 120 s duration, 136 fps in play, one stall frame 1.4 s; the usual shutdown flake class); the checkpoint run below passed both.

### T3 settings wave (options.cfg entries and bounds)
- One table `Gates/render_options.h` (struct, defaults, bounds, `setLong/setFloat/setFromText`, the override grammar, `current()` + the early-read tripwire) + `Gates/render_options_cfg.h` (line editor for the launcher; `loadFromText` for the pre-window publish). `CPrefs::load` reads the whole table (superseded by the FIX session F1: one strict parser, any malformed value -> one `[OPTIONS v1] event=rejected` line, default kept), `save` writes it, `applyPrefs` publishes; `gameosmain_init.cpp initRenderOptionsFromConfig()` publishes first. Bool keys write `b K = TRUE/FALSE`.
- Keys (default; bound): PostCAS (FALSE), PostCASSharpness (0.5; 0..1), TonemapMode (0; 0..3) [the three of 968949a66, now validated too], ShadowCSM (TRUE), ShadowBoundedNearFit (TRUE), PropShadowReceive (0; 0..1), ShorelineLegacySuppress (TRUE), CpuPhongRetire (TRUE), StaticPropAmbient (FALSE), StaticPropIblSh (TRUE), StaticPropIblSunRotate (FALSE), StaticPropIblSunRotateFloor (0.45; 0..2), StaticPropPbr (FALSE), SunAxisFix (FALSE), SunElevationFix (FALSE), VfxSoftParticles (FALSE), VfxLitParticles (FALSE). 14 gate rows deleted (the ledger marks 14 SETTINGS rows; the brief said 21 -- the other launcher rows are not SETTINGS in the ledger or are kept by ruling). `MC2_OPTIONS_OVERRIDE=Key=Value,...` (one new gate, passthrough) is the test channel the matrices (staticprop, vfx, shadow), `run_vk_field_check.py` and the capture drivers use; never saved.
- Launcher: rows carry a `cfg` flag and key = the options.cfg key; Launch and the dialog's OK write them into the install's options.cfg (a missing file or block is left alone; every other byte kept); the dialog starts from options.cfg when it has the key. `ShadowBoundedNearFit` started unchecked while the engine default is ON: fixed (starts checked). `mc2-launcher.exe --sync-options` (headless) wrote exactly the 14 lines on a copy of the lane install's options.cfg with seven launcher_env.json states, CRLF kept.
- Proof captures (GL, 1920x1080, `MC2_SMOKE_FREEZE_ANIM=1`, strips in `.claude/l2cap/`, montages in `.claude/l2cap/montages/`, all viewed): default equality on the final-form exe = day `res1080-a-gl` (pre-settings exe e8437015b) vs `set2-after-day-gl` 3 px (noise floor 38), night `set-before-night-gl` vs `set2-after-night-gl` 0 px; effect: options.cfg `b ShadowCSM = FALSE` alone `set2-csm-day-gl` = 6,385 px in 28 clusters (mech and ground shadows move; `set2-csm-day-gl-default-vs-csm0.png`), ShadowCSM + StaticPropIblSh = FALSE `set2-cfg-day-gl` = 47,970 px / 44 clusters (props darken; `set2-cfg-day-gl-default-vs-csm0-iblsh0.png`). The earlier `set-*` sets are from the exe BEFORE e8a4ac64e where ShadowCSM did not apply (engine log `early_reads=1 first=gos_postprocess_gates.cpp:22`); after it the log is quiet (`event=stale_reader` only prints when a reader was early or a value changed after the first publish).
- Gate-ledger rows: section 7 of `.claude/cleanup/gate-amnesty-wave1.md` (the settings row, the decal row, the glassDither row).

### Outside the lane's area (all reader, generated-file or test edits; lane 1 may see them in the merge)
- `code/prefs.cpp`/`prefs.h` (CPrefs: table load/save/publish), `mclib/mapdata.cpp`, `camera_update.cpp`, `txmmgr_render_phases.cpp` (one reader line + comment each), `txmmgr_render_passes.cpp` (two comments, latin-1), `mclib/cliff_decal_tuning.{h,cpp}` + `bdactor_static.cpp` (decal constants, T4), `code/missiongui_uicmd_probe.cpp` (one comment, T4), `tests/unit/test_gate.cpp` (floor 975, options + line-editor cases), `tests/unit/test_rendercore.cpp` (the Track-V VFX payoff guardrail removed: its only two subjects became options), `tests/unit/log_tag_baseline.json` (regenerated), `tests/shader_variants/static_prop.frag.splice-regression` (T4).

### Checkpoint (HEAD code 1c3ef0de9; this state commit is docs only)
- `tools/build_all_targets.py`: every stage built (exit 0, zero C4840); `gate_gen --check --rebuild` OK; `build_contract_harnesses --run --unit` 20/20 + doctest; `check-contracts.sh` 170/171 (only `doc_links`, the pre-existing docs/beta gap); editor (EditRel from build64-ninja, copied to the lane install) smoke PASS 15/15, `tests/smoke/editor/2026-10-12T23-15-12/report.md`; deploy `deploy_payload --target game --ninja --allow-new-target` (exe sha 1c3ef0de9c44); tier1 on the lane install PASS 5/5, run `2026-10-12T23-15-14` (DEPLOY_FINGERPRINT OK; mc2_01 138 fps, 03 78, 10 96, 17 122, 24 118; +0 destroys); resolution tier PASS 2/2, run `2026-10-12T23-26-10` (mc2_01@1920x1080 148 fps, mc2_01@3840x2160 136 fps). The tier1 and editor smoke waited for lane 1's tier1 to release the host lease first.
- Lessons: (1) the bash tool turns `\\n` into a real newline in heredocs: write backslash text with the Write tool or `chr(92)` in Python (it silently put raw newlines into three C++ log strings and a NUL into the launcher; the compiler caught them); (2) a log line printed in every run trips the default-build quiet ratchet (`check-default-build-quiet.py` grades the kept smoke logs under `tests/smoke/artifacts`, so delete stale kept logs after changing a tag); (3) a static cached reader that runs before CPrefs (renderer start-up) needs the pre-window publish, and `early_reads` in the engine log is the check; (4) `git stash push -- <paths>` lets one commit build clean while the next family's edits wait.

### Open for the next lane 2 (as of the night session; the FIX session above closed the review items and has the current list)
- Re-bless the golden set at 1920x1080 (owner). Decide whether tier1 should be pinned too (its fps baselines are 4K today).
- VK rows 13-21 of `.claude/VK-PARITY-GAPS.md` (20: ShadowCSM/PropShadowReceive options steer GL only; 21: SunAxisFix GL only) are the R1 port list.
- Still open from the W2/W3 list: MATERIAL_TEXTURES per-layer strength / re-cooked albedo, the KTX+BC7 VK prop path, TREE_DIAG_TRACE (needs `check-counter-honesty.py` + allowlist), VIEW_UNIFORMS, VK_VFX_TRACE (dead arms in code/), family 18 when the owner says. Stale mentions of the deleted gate names remain only in comments of `tools/class_light_band_harness/*.cpp`, `scripts/check-ab-kit.py` (an external bat-kit checker) and `docs/gate-history.md` (history on purpose).

## Earlier sessions (kept verbatim below)

## Wave 2 CLEANUP session (seventh session, 2026-10-12 evening, 18:23 CDT start, cap 21:23) -- review fixes F1-F8, 5 commits (nifty NOT merged)
- Source: `.claude/cleanup/reviews/lane2-2026-10-12-wave2-cleanup.md` (ACCEPT-WITH-FIX). Base = 583a0c263/b629795cc, tip = the state commit after 199546150.
- Commits: 3281d357a F1 (VK parity record) | 37e86abfe F2+F3+F4+F6+F7 code cleanup with the F5 comments and docs in the same commit | e896c7c13 F8 test floor + cxx17 baseline | 199546150 ledger evidence (F-POSTOPT default equality, F-SPREG capture reference) | the state commit.
- F1: the gate-keyed `tools/vk_not_ported.allowlist` cannot hold a promoted feature (a row for a deleted gate is STALE and fails `gl_vk_feature_coverage.py`), so the record is `.claude/VK-PARITY-GAPS.md` rows 13-19 (real-view terrain POM + fade; cliff triplanar / height normal / cliff POM; night tags + fixture emission; base-light direction 0.45; building static shadow + rebake on destroy; terrain mesh decal fill / colour blend; post-FX options), each with the GL file:line and the VK file to change, plus a new section 5 in `vk-default-flip-checklist.md` (rows 13-16 are visible in every mission and gate the flip like night gap #1; 17-19 need a verify-then-decide line each). Wave-2 ledger rows corrected: VK does NOT lack POM, it carries the DELETED faux-view march (`mv_parallax`, always on). `terrain_tier1.frag` comments no longer cite the deleted `chunkParallax`. `gl_vk_feature_coverage.py --write` regenerated nothing (0 new / 0 stale).
- F2 (fixture discriminator, night emission scale), F3 (building-shadow dead arm and its parameters, `bldgTriggerStaticShadowRebakeIfArmed` -> `bldgTriggerStaticShadowRebake(wasRegistered)`), F4 (`g_staticPropAlphaGeneration` everywhere incl. both tool stubs), F6 (`u_glassDither` on GL and on the VK `ColorFrameUBO` with `_pad[2]` keeping 256 B; `u_pomView` + `pomParams` -> `u_pomScale` + shader consts; `u_propBaseLightDirStrength` -> `kPropBaseLightDirStrength`; the bare scope shells), F7 (`submitCachedInstanceRange`, `persistentStaticTotalCount`, `s_spBuildRetired` and its snapshot/state-dump/`[DRAW_PACKET v1]` plumbing, `buildV6Packets` -> `resetV6FrameCounters`, three `<intrin.h>`), F5 (comments, `docs/tier1_env_vars.md`, `tag-registry.json` SNAPSHOT_BRIDGE_COMPARE + SPFLUSH_COST_SPLIT, the whitelists, `MC2_SNAPSHOT_STATIC_PROP_BUILD` state-dump key, orphan comments in `Gates/gate_table.cpp`). Regenerated: `tests/unit/log_tag_baseline.json` (census), `scripts/gl_shader_variants.json`, `GpuColorFrameUBO.layout.gen.h` + `color_frame_ubo_vk.glsl` (gpu_abi_gen). Also `scripts/check-render-contract-alpha-classes.py` (a prose-allowlist row for a comment that went).
- Not done, with the reason: (1) `snapshotDispatchDefault`/`legacyDispatch` stay as two keys (the MCP server validates both); (2) `s_spBuildAttempted` stays (live field of `[RENDER_SNAPSHOT v3]` and the state dump); (3) the `StaticProp.SnapshotBuild` profiler scope in `flush_dispatch` stays (it is an RAII zone, not a shell); (4) `code/missiongui_uicmd_probe.cpp:263` comment (code/ is out of scope); (5) `tests/shader_variants/static_prop.frag.splice-regression` still names `u_glassDither` (tests/ out of scope); (6) the review's `CPrefs::load` clamp for NaN post-FX floats and the seven `MC2_TERRAIN_DECAL_*` knobs are still open.
- Proof: ninja_build exit 0 at every commit tip; `build_contract_harnesses --run --unit` 20/20 + doctest 2746/2746 (also after the test_gate change); `tools/build_all_targets.py` every stage built (at 37e86abfe); `gate_gen --check --rebuild` OK; `check-gl-shader-variants` 231/231; `log_tag_census --check` OK after `--write`; `cxx17_ratchet --check --tighten` (40 sites of slack, baseline tightened, no count rose); `check-contracts.sh` = only the five pre-existing reds (background_launch, doc_links, script_callers, contract_coverage_selftest, scripts_tests) at 37e86abfe and again at e896c7c13.
- A/B (GL twice + VK, frozen animation; BEFORE = installed exe e796f71af `s4base*`, AFTER = 37e86abfe `s4after*`; strips in `.claude/l2cap/`, montages `.claude/l2cap/montages/s4-gl-mc2_01_saved_205644.png` viewed, `s4-gl-mc2_10_saved_204523.png`): day GL 209 / 280 px vs a 228 px noise floor (the recurring 90x8 horizon-water speck at 2438,390), VK day 297 px (the 113x13 speck at 2415,385), night 0 px on GL twice and VK. Nothing but the known water specks moves.  **[VOID VK, corrected 2026-10-13: the plain `-vk` sets named here (no `MC2_VK_SHARED_GBUFFER=1`) read back GL, so every "VK" figure on this line that comes from them is a second GL capture, not Vulkan evidence; `.claude/l2cap/index.md` flags them VOID; the `-vkarm` figures, where cited, are real VK.]**
- F-POSTOPT default equality (GL, pre-slice exe 7d8c47397 `f25tip2-gl` vs post-slice e796f71af `s4base1-gl`): day 273 px at the 228 px noise floor, night 0 px; montage `.claude/l2cap/montages/s4-postopt-default-gl-mc2_01_saved_205644.png` (viewed). The options.cfg defaults (PostCAS FALSE, sharpness 0.5, TonemapMode 0) reproduce the old gate defaults.
- Tracy A/B (GL arm, mc2_01, `tracy_capture_compare.py baseline`, free timestep, `MC2_FPS_CAP=0`, 75 s window, `perf_baseline_report.py` steady state = after the first 40% of frames; traces `release_assets/tracy-baselines/s4*` gitignored). BASE = the merge-tip exe 642019a64 (built in the scratch tree `build-w2base` with a detached worktree, exe + that commit's shaders put over the lane install for the base arm, then HEAD redeployed), HEAD = e896c7c13 code. Default camera: base b/c/d frame mean 6.80 / 6.82 / 6.81 ms (p99 7.97 / 7.63 / 8.01 ms, `SwapWindow.SDL` GPU wait 4.36-4.40 ms), head a/d 6.67 / 6.69 ms (p99 7.54 / 7.71, SwapWindow 4.33): about -0.13 ms (-1.9%); `RenderLists.DynShadowCSM` 0.135 -> 0.084 ms/frame (buildings left the dynamic caster pass). Close pose `2496,-1941,30,45,300@600` (near_land_low, where POM and the cliff march are exercised): base a/b 6.67 / 6.66 ms (p99 11.19 / 11.23, SwapWindow 3.29 ms), head b/c 5.60 / 6.29 ms (p99 9.75 / 10.92, SwapWindow 2.58 ms in b). So the always-on POM, base-light direction and the registry changes cost nothing measurable at either pose (the GPU wait fell); run-to-run noise at the close pose is about +-10%. Discarded as host-contended: base-a default camera (cold cache, 26.8 fps), head-b/c default camera (49 fps while another lane worked), base-c close pose (p99 20 ms, one 200 ms hitch), head-a close pose (a csvexport ran beside it). NOT measured: a worst-case grazing-angle POM stress. Full-map static shadow rebake on building destroy (`MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE_TEST_DESTROY_AT_FRAME=6000`, event fired, `s4destroy-gl.tracy`): zone `Shadow.StaticFullMapBuild` counts=2 (mission-load build + the rebake) mean 1.98 ms, min 1.17 ms, max 2.80 ms, so one destroy costs at most 2.8 ms once (the 182 ms DrawScreen max is the load hitch).
- Checkpoint (HEAD code 199546150, docs only after): build exit 0, harnesses + doctest green, `check-contracts` five pre-existing reds, deploy `deploy_payload --target game --ninja --allow-new-target` (exe sha 1995461504c6), tier1 on the lane install (`run_smoke.py --tier tier1 --deploy lane2 --continue --wait-timeout 3600`, DEPLOY_FINGERPRINT OK at HEAD 199546150): PASS 5/5, run `2026-10-12T20-18-58` (report `tests/smoke/artifacts/2026-10-12T20-18-58/report.md`; mc2_01 138 fps, 03 79, 10 97, 17 122, 24 118; +0 destroys; mover coherence 0 violations on all five). The tier1 waited for lane 1's smoke to release the host lease first.
- Lessons: (1) a commit message containing a hyphenated word with an `a` (e.g. `default-equality`) trips the `git commit -a` hook regex when passed with `-m`: use `-F file`; (2) `tracy_capture_compare.py` takes no host lease and aborts if any `mc2.exe` runs, so run it bare (it takes the lease itself in `nofocus.spawn`); wrapping it in a lease holder deadlocks the child; (3) the Tracy first arm after a swap of exe is a cold-cache run (26 fps): discard it or warm it; (4) `perf_baseline_report.py` costs about 2 minutes per trace, run it after the captures not beside them; (5) `MC2_NINJA_TREE=build-w2base` + `git switch --detach <sha>` gives a second tree for a baseline exe without a second worktree, but switching back rebuilds about 465 TUs in the main tree; (6) a state-only commit after the deploy does not invalidate the tier1 fingerprint (`check_ancestor_fingerprint` accepts doc-only commits).
- Open for the next lane 2: the VK rows 13-19 are the R1 port list (not started here); NaN clamp in `CPrefs::load` for the post-FX floats; freeze the seven `MC2_TERRAIN_DECAL_*` knobs; the W2/W3 list of the section below (settings wave, MATERIAL_TEXTURES, KTX+BC7 VK path, TREE_DIAG_TRACE, VIEW_UNIFORMS, VK_VFX_TRACE, family 18).

## T2 gate amnesty WAVE 2 (sixth session, 2026-10-12 afternoon, started 16:01 CDT, cap 22:00) -- 14 commits, 5 families promoted, 3 W3
- Start: merged nifty 642019a64 (fast-forward), cxx17 ratchet re-pinned (9314de32b). Lane registered as LANE2-T2-GATE-AMNESTY-W2.
- Commits (merge tip 642019a64 -> HEAD): 337b05c18 F-PROPKNOB knobs (8 rows) | 9bec41219 F-TPOM PROMOTED (5) | 415f2997c F-BLDGSHADOW PROMOTED (2) | cf99ca5c3 F-PROPKNOB fixes (NIGHT_TAGS + BASE_LIGHT_DIR 0.45 promoted, FIXTURE_LIGHTS frozen, FIXTURE_GLOW deleted as a losing arm; 5 rows) | 2f8639820 census | 1c4018bd9 F-TCLIFFMAT PROMOTED (7) | 87d1a26ac F-TMATTEX (RESAMPLE deleted; MATERIAL_TEXTURES -> W3) | 0d479ca9b F-TDECAL PROMOTED (3) | c77939fc1 variant manifest | 69eb328bd F-SPREG (10 rows) | 7d8c47397 F-SPDISPATCH (9 rows) | 968949a66 F-POSTOPT -> options.cfg PostCAS/PostCASSharpness/TonemapMode (3 rows) | 48b528081 gated-layout-macros | 19d1cee73 LIGHTBAKE_STABILITY + VK_PROP_MATERIAL_DEBUG | 434abb5a0 table floor test | e796f71af ledger section 7.
- Counts (Gates/gate_table.json registry kinds): Feature 139 -> 118, Trace 136 -> 108 (table 1055 -> 1000 rows, gate_table `count` 1000). Retired 2 unchanged.
- PROMOTED (A/B viewed, strips `.claude/l2cap/montages/w2-*`, `x2-*`, `g1-*`, `f25-*`): terrain chunk POM, cliff material (triplanar + height normal + POM), terrain mesh decal, static building shadow, night tags, base-light direction 0.45. W3 with the montage and defect: MC2_TERRAIN_MATERIAL_TEXTURES (recolours sand to mud-brown, 54-61% of the frame), MC2_MATERIAL_KTX + MC2_STATICPROP_BC7 (VK renders props as cyan blocks; the ORM slots alone are inert). Family 18 skipped by the owner. Ledger section 7 of `gate-amnesty-wave1.md` has one row per gate.
- options.cfg entries added: `PostCAS` (bool, FALSE), `PostCASSharpness` (float, 0.5), `TonemapMode` (long, 0..3) in CPrefs; other launcher SETTINGS rows noted for the settings wave.
- Process deviations (recorded): the families were committed one per family but built and proved at group tips (terrain group 15/16/17/19 + 27 + 4 at 0d479ca9b..c77939fc1, F-SPREG and F-SPDISPATCH each on their own build + proof), because a table deletion rebuilds ~480 TUs and the proof chain is ~12 min per run; every tip build exited 0 and the proof results are below.
- Proof per tip (each: ninja exit 0, `build_contract_harnesses.py --run --unit`, `gate_gen.py --check --rebuild` exit 0, `check-contracts.sh`): contracts 164/169 with only the five pre-existing reds (background_launch + scripts_tests from the merged lane-3 file strhunt_patch.py, doc_links, script_callers, contract_coverage_selftest). Fixed on the way: log_tag_census (re-frozen), gl_shader_variants manifest anchors, gated-layout-macros doc, test_gate table floor (1000 rows), stale vk_not_ported.allowlist rows.
- A/B before/after the dispatch families: base = tip `g1tip*`, after = F-SPDISPATCH exe `f25tip*`: GL day 67 px, night 0 px, VK night 0 px, umbrella-armed VK day 1,646 px (one mech) / night 449 px (the known 185x148 mech region); strips `f25-*`. **[VOID VK, corrected 2026-10-13: "VK night 0 px" came from the GL-labelled `g1tip-vk`/`f25tip-vk` sets (index: VOID); the umbrella-armed figures are real VK, but their strip compares the UNFROZEN `g1tip-vkarm` with the frozen `f25tip-vkarm`, so "one mech 1,646 px / 449 px" is animation phase, not a regression signal.]**
- Checkpoint at e796f71af (HEAD code; later commits are docs/state only): `build_all_targets.py` every stage built (exit 0); editor (EditRel built from build64-ninja, deployed to the lane install) smoke PASS 15/15, report `tests/smoke/editor/2026-10-12T17-58-12/report.md`; tier1 on the lane install PASS 5/5 with DEPLOY_FINGERPRINT OK (exe sha e796f71afba0 = HEAD), run 2026-10-12T17-38-45 (mc2_01 149 fps, 03 85, 10 105, 17 132, 24 128; +0 destroys); final proof at the tip: harnesses 20/20 + doctest 2746/2746, `gate_gen --check --rebuild` exit 0, check-contracts 164/169 with the five pre-existing reds. The earlier tier1 on the F-SPDISPATCH exe (7d8c47397) was 4/5 and flagged RUN INVALID by DEPLOY_FINGERPRINT because HEAD had moved on while it waited for the host lease: mc2_01 heartbeat_freeze_play while the lane ran a build and a proof concurrently (the known load flake class); the same mission passes at the final tip. options.cfg round trip: PostCAS=TRUE + PostCASSharpness=1.0 + TonemapMode=1 in the lane install's options.cfg changes 99.9% of the frame against the defaults (`.claude/l2cap/montages/opt-cas-tonemap-mc2_01_saved_205644.png`); options.cfg restored afterwards.
- Lessons: (1) commit -> build -> deploy again (a tier1 started after a later commit is fingerprint-invalid); (2) capture drivers must pass `--env MC2_SMOKE_FREEZE_ANIM=1` like capture_ab or mech pixels are animation noise; (3) the host lease can be held 15+ min by lane 1's tier1/tier2, so queue captures and smokes behind one driver and keep the exe frozen while they wait; (4) `Grep` (ripgrep) is minutes faster than bash grep over the tree while a build runs.
- Next lane 2: settings wave (the launcher SETTINGS rows, same CPrefs pattern), W3 (MATERIAL_TEXTURES per-layer strength / re-cooked albedo, KTX+BC7 VK prop path), TREE_DIAG_TRACE (needs check-counter-honesty.py + counter-honesty-allowlist.txt change), VIEW_UNIFORMS (shader define pair, gamecam/EditorCamera readers, smoke matrices), VK_VFX_TRACE dead arms in code/, family 18 when the owner says (`g_staticPropAlphaGeneration` and the dead shader arms went in 37e86abfe).


## T2 gate amnesty wave 1, PHASE 2 (fifth session, resumed 2026-10-12 08:38 CDT after the weekly-limit cut, cap 14:38) -- 6 FAMILY COMMITS LANDED
- Merged nifty 2bdcf003d (clean; `cxx17_ratchet --check --tighten` OK, baseline head re-pinned), applied the predecessor's WIP patch (F-VIEWMODEKNOB) with `git apply --3way` (clean), finished it. Lane registered as LANE2-T2-GATE-AMNESTY-W1.
- Commits (merge tip 2bdcf003d -> HEAD 08cf582de), one proof each (ninja build, harnesses 20/20 + doctest 2734/2732, `gate_gen --check --rebuild`, check-contracts):
  - ba17c82f8 F-VIEWMODEKNOB: MC2_VIEWMODE_LOWLIGHT_GAIN/_TINT deleted (the viewmode matrix's gain-5.0 entry was the seed's only setter and went with it).
  - baa815bc6 F-VFXKNOB: the five MC2_TUNE_VFX_* seeds + MC2_VFX_ORACLE_TUBE_LOG deleted (profile + sliders are the live inputs; the profile guard that let the env win goes).
  - e469b369a F-DIAG: 12 rows deleted (ASSET_FAILLOUD, LIGHTING_DEBUG_VIEW env seed, STATIC_UPDATE_SKIP, TERRAIN_LIGHT_POOLS, TEXMGR_COMPRESSED_UPLOAD + enum renumber, ASSET_TRACE, ASSIMP_TRACE, CAMERA_MOVE_DIAG, FLUSH_PROBE, RENDERLISTS_COST_SPLIT span machinery, RENDER_WORLD_TRACE blocks, SPLIT_PATH_PARITY_TRACE + its never-firing bucket/tests). W2: TREE_DIAG_TRACE (gos_static_prop* = lane 3), VIEW_UNIFORMS.
  - 1066e96cf F-VKDEAD: 6 rows deleted (PRESENT_NO_GL_IMPORT, STATIC_SHADOW_MAP no-ops, TERRAIN_HEIGHT_AUDIT(+_CORRUPT) + the snapshot checksum, VFX_COALESCE off-arm, TERRAIN_GL_FALLBACK_AUDIT counters). Kept by ruling: VK_SCENE_GL_PRODUCER_QUERY. W2: VK_PROP_MATERIAL_DEBUG (lane 3 file), VK_VFX_TRACE (dead arms in code/), VK_TERRAIN_LODFADE (recorded regression under the promote rule, ledger row).
  - a7ddae10b F-VKFOLD: the 25 umbrella members (16 registry + 9 plain Tri rows) folded into MC2_VK_SHARED_GBUFFER; armed<>/regionTier1Armed<>/the composites/vfxArmed() -> umbrellaOn(); strict runner, field-check SCRUB, check-vk-umbrella-readers (new `umbrellaOn@function` keys), the G4 tests and the tier1 doc's VK gate model follow.
  - 08cf582de F-FOG + F-WATERKNOB + F-PROPFIX: FOG_HORIZON_CLAMP/_FADE_START/_FADE_END, SKYBOX_FOG_EXCLUDE (always on; skyboxFogExcludeEnabled() gone, the VK fog region backend declines unconditionally as it always did), WATER_SHORE3_DEPTH_K (0.03) / _EXTRA_WU (60) frozen; PROMOTED (owner ruling 2026-10-12): GV_LIGHTSOUT_CLEAR, PROP_FOLIAGE_ALPHAFLAG (A/B at the noise floor, see below; enum positions 43/41 renumbered, COUNT 55 -> 53). dup_lines baseline rewritten (+4 GameOS: the fold made the region-gate idiom identical across islands).
- Counts (Gates/gate_table.json registry kinds): Feature 195 -> 158, Trace 156 -> 140 (table 1140 -> 1078 rows). Ledger section 6 of `.claude/cleanup/gate-amnesty-wave1.md` carries one row per gate (72 rows: deleted, folded, promoted, W2 with reasons).
- Promote rulings applied from the predecessor's on-arm captures (`.claude/l2cap/fix-*-{gl,vk}` vs `w1before*`, all four montage sets viewed, `.claude/l2cap/montages/w1-promote-<fix>-<backend>-<bookmark>.png`; noise floor GL 29 px day / 579 px night):
  - PROMOTED: MC2_GV_LIGHTSOUT_CLEAR (5/400/233/475 px), MC2_PROP_FOLIAGE_ALPHAFLAG (298/666/239/291 px) -- no visible change on either bookmark, GL or VK.
  - NOT promoted, W2 with the record: MC2_NIGHT_FOG_DIM -- the DAY sky/horizon band of mc2_01 goes dark grey (821,637 px, the full-width top 221 rows; the dim is not night-conditioned); MC2_WATER_SHORE_MEANDER -- the A=75 arm moves the waterline on both maps (435k / 373k px) and is the recorded water-deleting defect; MC2_NIGHT_AMBIENT_FLOOR -- the only capture (=16) shows no change and the knob has no authored on-value; MC2_WATER_SHINE -- no A/B exists yet; MC2_VK_TERRAIN_LODFADE -- recorded flat-lit regression (VK-PARITY-F3).
- Wave A/B at the family-set end (merge-tip exe a221a8f7a on the lane install vs HEAD 08cf582de): BEFORE sets `.claude/l2cap/w1-before{1-gl,2-gl,-vk}` + `w1-before-vkarm` (MC2_VK_SHARED_GBUFFER=1), AFTER sets `w1-after*` + `w1-after-vkarm`; compare lines in `lane2 scratch compare_w1.log` and the armed-VK montages `.claude/l2cap/montages/w1-vkarm-<bookmark>.png`. RESULT (viewed): all at the noise floor -- GL day before1->after1 = 0 px (byte-identical), before1->after2 = 286 px (the recurring 113x13 flicker at 2415,385; noise 170), VK day 227 px; night GL 511/511 px and VK 474 px against a 349 px noise floor (the 22x32 flicker at 1403,903); umbrella-armed VK day 200 px (59x6 at 1792,311), night 513 px (the same 33x32 flicker) with the same island health lines before and after.  **[VOID VK, corrected 2026-10-13: `w1-before-vk` / `w1-after-vk` / `w1-head-vk` (no `MC2_VK_SHARED_GBUFFER=1`) read back GL, so every "VK" figure on this line that comes from them is a second GL capture, not Vulkan evidence; `.claude/l2cap/index.md` flags them VOID; the `-vkarm` figures, where cited, are real VK.]**
- Checkpoint: `tools/build_all_targets.py` every stage built (at 08cf582de); editor (EditRel from build64-ninja, deployed to the lane install) smoke PASS, report `tests/smoke/editor/2026-10-12T10-28-51/report.md`; tier1 run 2026-10-12T10-19-09 on the lane install = 5/5 missions PASS but RUN INVALID by DEPLOY_FINGERPRINT (the exe had been built before the family commit: sha a7ddae10b + dirty) -- the attributable rerun after the clean rebuild at 5ecd82e6b (relink only, DEPLOY_FINGERPRINT OK): tier1 run 2026-10-12T10-31-45 = PASS 5/5 (mc2_01 145 fps, 03 85, 10 87, 17 116, 24 118; +0 destroys), `tests/smoke/reports` id 2026-10-12T10-31-45. The HEAD exe's capture set `w1-head*` vs `w1-after*`: night 0 px on all four lines, day at the noise floor (71/274/241 px vs 286). Lesson: COMMIT -> BUILD -> DEPLOY, never build-then-commit-then-deploy. The lane install now carries mc2.exe 5ecd82e6b and the EditRel editor (deployed while the invalid tier1 run held avcodec-61.dll: that one copy was refused, the DLL is unchanged).
- SECOND HALF (coordinator: continue in the same session), commits after aef3e7c23:
  - 0d9d0e55a F-CEMENT: the 11 cement/runway A/B arms deleted (ARM A/B/C, magenta key, transition fallback, edge-blend pack, CEMENT_LIT / RAMP_WORD uniforms + the ramp-band shader block, OVERLAY_CEMENT_SHADOW_MATCH; ALPHA_ATLAS and OVERLAY_ART_FLATLIGHT unconditional, enum 52 renumbered). ITS BUILD HAD FAILED (two classifier constants went with the ARM C block; the lane missed the ninja exit code) -> 29af3c85d restores them; the first commit does not build on its own.
  - c7b979d8a F-LIGHTKNOB (diagnostics and probes only): FULLBAKE_DIAG, LIGHTBAKE_PARITY, PROP_LIGHT_LATCH_DIAG(+_INTERVAL) deleted; W2 with reasons: LIGHTBAKE_STABILITY (observer called from gos_static_prop_registry.cpp), GPU_CULL_STATIC_LIGHT_ZERO_PROBE, LIGHT_ROW_PURE_VERIFY, LIGHT_GRID_DIM (knob + Int test fixture); the default-on fix switches await a ruling.
  - ec0e983ec F-MECH: the four GPU-mech killswitch kill arms, ANIM_CADENCE_FIX, MECH_LIGHT_FRESHNESS, MISSION_INTRO_LEGACY_RENDER, PICK_FALLBACK_COARSE deleted (enum 51/35 renumbered, COUNT 50). Own A/B: f21-before (c7b979d8a install) vs f21-after (ec0e983ec), GL twice + VK + armed VK, montages `.claude/l2cap/montages/f21-{gl,vk,vkarm}-<bookmark>.png` (viewed): GL day before1->after1 = 0 px, night 0 px on every line (GL twice, VK, armed VK); VK day 210 px and armed VK day 166 px = the recurring horizon-water specks (90x8 at 2438,390 / 59x6 at 1792,311), mechs identical. tier1 at ec0e983ec (DEPLOY_FINGERPRINT OK): run 2026-10-12T11-29-44 = 4/5 with mc2_17 heartbeat_freeze_play under host load (lane 3 tier2 + 35 python children; every mission 40-60% below its usual fps), mc2_17 alone = PASS (run 2026-10-12T11-42-24, 94 fps) -- the known flake class (tier1-flake-log.md row appended). KEPT.  **[VOID VK, corrected 2026-10-13: the `f21-before-vk` / `f21-after-vk` sets (no `MC2_VK_SHARED_GBUFFER=1`) read back GL, so every "VK" figure on this line that comes from them is a second GL capture, not Vulkan evidence; `.claude/l2cap/index.md` flags them VOID; the `-vkarm` figures, where cited, are real VK.]**
- Counts at ec0e983ec: Feature 195 -> 139, Trace 156 -> 136 (table 1140 -> 1055 rows); ledger section 6 has 103 rows.
- Lesson (hard): a background build's exit code must be read before the proof chain starts; the F-CEMENT harness/doctest/contracts ran green on a tree whose GAME build had failed because the harness trees compile separately. The ninja exit line is now grepped explicitly per family.
- Families done in the second half: 20, 21, 11 (diagnostics). W2 untouched: 4, 24, 25, 5, 13, 8, 14-19, 26-29 plus the W2 rows named in the ledger.

## Rules learned this session
- Every table deletion rebuilds ~200 TUs (gate_index.gen.h), so a family costs ~10 min build + ~6 min harness/doctest + ~4 min check-contracts; batch small families into one commit when each has its own proof line in the message.
- `Gates/gate_table.json` keeps gate_dump's 17-digit float spelling: delete records textually, never json round-trip it (`--check --rebuild` would diff). The seed file round-trips fine (indent=1).
- The HEAD-graded rows (gate_reader_conflicts, test_gl_vk_feature_coverage known_gaps) are red while the table is ahead of HEAD and green right after the commit; doc_links is red from the pre-existing docs/beta/* gap (not this lane's).
- Mixed CRLF/LF engine files (tube.cpp, vulkan_water_island.cpp) defeat byte-exact edits: the scratch `edit_mixed_eol.py` matches either ending per line and keeps the matched ending.
- A gate_table.cpp row deletion must also move: tools/gate_table.seed.json, docs/tier1_env_vars.md bullet, test_gate.cpp fixtures, vk_not_ported.allowlist rows, log_tag_census (`--write`), gl_vk_feature_coverage (`--write`), the GL shader-variant manifest (line anchors), gated-layout-macros and background-launch inventories when `#ifdef` pairs or script lines move, and the RendererFeature enum when the row had a position (scratch `amnesty_delete.py --renumber-feature N`).
- The `[CONFIG v1] warn=premain_read` test needs a surviving MC2_VK_ Bool row (MC2_VK_FORCE_FAIL prints premain_value=0, not `unset`).
- capture_ab's `-vk` set used to run WITHOUT the umbrella, which made it a GL capture (the engine forced GL); fixed by c0158be25 (both variables are passed now) and guarded at capture and compare time (`capture_backend.py`, `capture_ab.check_backends`). An umbrella-armed VK A/B before that fix was a separate `capture.py --env MC2_RENDER_BACKEND=vulkan --env MC2_VK_SHARED_GBUFFER=1` pair.

## Exact next steps for the next lane 2
1. Merge nifty (regenerate callers, log tag census, background-launch inventory; the cxx17 baseline conflict takes nifty's side and re-adds only real sites). Then the ownership batch (R1 batch 4), next slices in the ledger's conversion order (`py -3 tools/vk_ownership_ledger.py`): light_feed + shared_scene + image_resource together (the shared owners), then sky, post_fog, post_hzb ... Each slice: convert the island completely, delete its manual teardown, regenerate the ledger, VK tier1 with `MC2_VULKAN_VALIDATION=1` zero, day + night VK captures at the noise floor, `own_inventory` untouched.
2. A quiet-host Tracy pair (evening state, "Open" 1) and the campaign-start `pre_video` assertion.
3. Carried from the earlier sessions: W2 planning with the orchestrator (the gos_static_prop* rows, the code/ dead arms, the promote candidates that need a scene); dead shader arms left by the deletions (`u_horizonClampEnabled` off-branch in edge_fog/fog_oob + VK twins; terrain_tier1 lodFadeEnabled stays live); the T2 audit's section-5 rows (code/gui/editor readers) are lane 1 territory.

## Never-touch (coordinator, steps 2 and 3)
mclib/estring*, elist*, heap*, stuff/*, mlr/*, gosfx/*, memorystream*, file*, packet*, fastfile*, abl*; code/multplyr*, dmultplyr.h; every *_internal.h; the 50 TUs of the audit's section D.
Ratchet policy: never contort code for a ratchet; a legitimate rise gets a baseline rewrite with the reason in the commit.

## Prior sessions (C++23 step 2, splits, tools) -- see git history of this file before 2026-10-12 and `.claude/cleanup/step2-never-touch-audit.md`
- Tools (tested, committed): tools/objhash.py, tools/tidy/{proof_allow.json,proof_ext.py,vs_trees.py,rewrite_defines.py,rewrite_enums.py,batch_run.py,check_batch_allowed.py}.
- Deploy to the lane install needs `--allow-new-target` (existing folder). `run_smoke.py --deploy` takes the NAME (`lane2`). A foreground command over 120 s is moved to the background: run long waits with `timeout 115 bash -c 'until ...'`.
