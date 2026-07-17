# Known issues (current)

To report a new issue: [GitHub Issues](https://github.com/ThranduilsRing/mc2-opengl-remastered/issues)

---

## Smoke harness: intermittent `crash_silent` / `heartbeat_freeze` — HOW TO HANDLE

A `--tier tier1` BATCH run intermittently fails ONE mission with `crash_silent`
(0 frames, nonzero exit) or `heartbeat_freeze_play`; the failing mission VARIES
run-to-run and **passes on an isolated `--mission <id>` re-run**. This is
**ENVIRONMENT-SUSPECTED, not a confirmed regression** — do NOT treat a single
isolated-passing flake as a code failure, and do NOT add a settle-delay or engine
change on a hypothesis.

**The way to handle it (in order):**
1. Re-run the failing mission ALONE. If it passes, the batch result stands as
   environmental — record it and move on.
2. To classify, read the auto-captured evidence:
   `tests/smoke/artifacts/<ts>/<mission>.crash_evidence.json`
   (written by `SMOKE-CRASH-SILENT-EVIDENCE-1`, `scripts/smoke_lib/crash_evidence.py`).
   It has the real exit code, whether a Windows display-driver TDR (event 4101) /
   AppError (1000) / WER (1001) fired, minidump presence, the concurrent mc2.exe
   list, GPU/driver, and the stdout+heartbeat tail.
3. Only after the evidence points somewhere, run the A/B/C concurrency matrix
   (solo 3× / with soak / with another direct mc2.exe). A settle-delay
   (`SMOKE-INTER-MISSION-SETTLE-1`) is justified ONLY if the SOLO case flakes.

Proven: it is NOT an ordinary caught C++ crash (that buckets `crash_no_summary`),
NOT STARTUP-INIT-ASYNC-1 (tier1 bypasses MissionSelectionScreen), NOT a
`--kill-existing` race (never use that flag). GPU TDR is plausible, **not proven**.
Full verdict: `docs/testing/smoke-first-launch-crash-silent-recon-1.md`.

---

## Editor: generated-map path divergence (architectural debt, no active bug)

- **Generated maps use a divergent terrain bring-up (`EditorData::initTerrainFromTGA` ~1561-1731) instead of the proven stock `.pak` load path (`initTerrainFromPCV`/`Terrain::load`).** Greybeard ruled META-FIX = converge them; DEFERRED as justified debt. **Blocker:** no standalone generated-`.pak` writer exists — `tools/terrain_gen/pak_exporter.py` only `patch_pak()`s Packet 0 of a same-grid template (`--template-pak`); no tacmap/MOVE writer; shipping design is deliberately `.elev.r32`+burnin → `setVertexHeight` direct apply (terrain_gen.py:285). Completing it = full PacketFile writer = a dedicated terrain-export session, not an editor session. **NO active bug:** the hardened `EditorDebugOverlay` Terrain Probe (`MC2_TERRAIN_PROBE=1` headless, or Debug Overlays panel) proved the direct-apply path correct — `file==vert==mesh` (68-399), `water=0`, recipe+colormap ready. The earlier "71% underwater" was an `if(water)` miscount of `PostcompVertex.water` dither-marker bits (0x40/0x80); renderer uses `water & 1` (quad.cpp:1008). `eye->reset()` is NOT a terrain fix (`EditorCamera::reset()` only deletes sky+compass). Full record: `memory/debt_generated_map_pak_convergence.md`.

## Player-visible issues

Known bugs that affect anyone running the game. Full detail in the beta doc:
[docs/beta/known_issues.md](beta/known_issues.md)

- ~~**Shadow stutter** when the camera jumps more than ~500 units in one move.~~ **FIXED, stale entry** (fix predates this line — see Shadows section below).
- **Shadow banding** shifts slightly with camera rotation due to view-dependent tessellation. *Status:* STATIC-SHADOW-BANDING-1 (`9c31e2bf`, PCF undersampling fix, shipped) and STATIC-SHADOW-BANDING-2 (`5b0e488e`, slope-normal bias + normal-offset receiver, gate `MC2_SHADOW_STATIC_BANDFIX_V2` default-OFF, awaiting user field check) address this.
- **Water shoreline z-fight** visible when zooming in or changing elevation (not on pan).
- **First-launch black terrain** (intermittent) — relaunch the mission; second load is normal.
- **Options menu writes bad resolution** to `options.cfg` on 4K displays. The render base is now force-clamped to 800×600 (`[HUD-RES-CLAMP v1]`, `gos_SetScreenMode` + `gameosmain` startup) so the HUD stays correct regardless — the legacy 2D HUD only lays out at the tuned widths {640,800,1024,1280,1600,1920}; a non-tuned value broke pause/loading/comms-video/bottom-bar. Whole frame renders at 800×600 + FULLSCREEN_DESKTOP upscale (scene therefore soft; sharp-native-scene + 800-HUD lane split deferred to the imgui UI). Memory: `hud_scene_resolution_separation`. `MC2_RES_DIAG=1` dumps the split.
- ~~**Water "HDR clipped" at odd desktop resolutions**~~ **ROOT-CAUSED + FIXED (deferred item 8, `claude/deferred-sweep-1`).** The reflection-FBO is NOT fixed-size — it's `drawableWidth/4 x drawableHeight/4` (480×270 was just the 1920×1080 case). The real bug: `shaders/gos_terrain_water_mdi.frag`'s `dFdx(nz)` aspect compensation (`WATER-ASPECT-CORRECT-1`) assumed the perspective frustum is ALWAYS 4:3-locked while the viewport is native-res — true before `UI-ASPECT-ANCHOR-1` (`mclib/camera.cpp`), which now builds the frustum at the real drawable aspect BY DEFAULT. The shader kept unconditionally "correcting" a stretch that no longer exists by default, injecting an aspect-proportional artificial distortion into the wave-reflection normal that grows the further the window aspect departs from 4:3 — exactly the odd-resolution signature. Fixed by passing the actual effective frustum aspect to the shader (`u_waterProjAspect`, mirrors `g_cameraProjAspectNative`) instead of hardcoding 4/3; the correction is now a no-op under the default native-aspect path and only activates under the legacy `MC2_CAMERA_ASPECT_NATIVE=0` frustum, where it's still needed. Confirmed via the shader's own `u_waterDebugMode=10` (Aniso) probe that `dFdx(WorldPos)`/`dFdy(WorldPos)` anisotropy is already resolution-independent (mid-gray ~103.6-103.7 at both 1920×1080 and 3072×1500) — geometry is not stretched, so the shader-side compensation had nothing left to correct. Native-vs-legacy A/B with `MC2_WATER_REFLECTION_RT=1` (debug mode 9) at 3072×1500 shows the fix changes the sampled reflection content, confirming real effect. Caveat: this dev deploy's overcast-sky HDRI fails to load (`exr_load_failed`, pre-existing/unrelated — `data/hdr/mud_road_puresky_16k.exr` is an external-pack asset per `docs/testing/ibl_hdri_external_pack.txt`, not installed here) so the water reflection fell back to the smooth SH-L2 sky term in this session's captures, which produced a byte-identical before/after at the default (non-RT) resolution matrix I ran — the fix's effect is real (confirmed above) but its magnitude on the ORIGINAL dramatic "dark/blown/noisy" report likely depends on a high-frequency HDRI sun-disk reflection being active, which this environment lacks. Commit `337c2c1e`; tier1 5/5 PASS post-fix.
- **GPU gosFX FX render badly → REVERTED to CPU/MLR (default-off, 2026-06-15 `4ad31a9e`).** The GPU oracle FX paths (`MC2_VFX_ORACLE_TUBE` / `MC2_GPU_PARTICLES` / `MC2_VFX_ORACLE_RENDER`) rendered tube trails as flat grey sheets / wrong animations; all three flipped back to **default-OFF** so gosFX uses the known-good legacy CPU/MLR path. Opt back into a GPU path with its env var `=1` (the breakage returns; the energy-beam visual upgrade was never done). The PPC "little ball" uAdditive discard fix (`ac9f3315`) is still in the shader but off the default path.

---

## Developer / Internal

Extracted from CLAUDE.md 2026-05-24. Issues here are KNOWN — don't re-discover
them. Add new findings as new bullets; remove fixed ones outright (don't append
"FIXED" — delete the bullet).

## GL state / render-state cache (META-FIX DEBT)

- **No mechanical GL-state save/restore contract for GPU-direct passes (GREYBEARD: GlStateGuard meta-fix, deferred 2026-06-01).** GPU-direct passes (particle bridge, water fast path, mech/static-prop batchers, terrain bridges, post-process, shadow direct draws) set blend/cull/depth via RAW GL that bypasses the gos render-state cache (`gos_SetRenderState`/`applyRenderStates`, single `stateCacheValid_` bit). Each must manually (a) save/restore touched slots and (b) call `gos_InvalidateRenderStateCache()` — enforced only by comment + discipline. The invalidation call-site list at `gameos_graphics.cpp:1565` has grown to ~14 entries = the footprint of the gap. Failures this class produced: explosion cards backface-culled (sub-pass `copySceneDepthForParticles` re-enabled GL_CULL_FACE mid-flush), water vanished (inherited SimpleCamera `SetBackFaceOn`), stale-cached blend on legacy MLR draws after a raw-GL pass. **Meta-fix:** a `GlStateGuard` RAII scope (snapshot slots + set on ctor; restore + `gos_InvalidateRenderStateCache()` on dtor) wrapping every GPU-direct pass; sub-passes get an inner guard. Collapses the 14-item manual list to zero, makes the bug class impossible by construction. No-behavior-change refactor; touches all ~14 sites in one coordinated slice. Interim patches landed 2026-06-01 (particle-bridge cull re-assert + bridge/water cache-invalidate) — substitutive for the visible flicker; the guard refactor remains the upstream fix.

## Shadows

- **Shadow re-render stutter when camera moves >500 units — FIXED, stale entry (`e9b9aea7`, 2026-05-16; not previously reconciled here).** The "design ready" static world-fixed shadow map this entry pointed at shipped as `e9b9aea7` ("static full-map shadow feed + Phase-1 camera-accum retire"), which predates this known_issues.md line (added 2026-05-24/25) but was never cross-referenced back to it. The retired code was the exact mechanism this entry describes: a camera-move-gated (~100u, later documented as ~500u in `docs/architecture.md`) re-bake of the static shadow FBO. Post-fix, the static terrain shadow builds exactly ONCE per mission (`gos_StaticLightMatrixBuilt()` latch, `mclib/txmmgr.cpp` ~2692) and never touches camera position again — a >500-unit jump structurally cannot trigger a re-render. Re-verified empirically 2026-07-17 (`DEFERRED-SWEEP-1`, no code change needed): a hard ~990-unit teleport onto a `shadow_cascade` mc2_24 view (`tests/visual/bookmarks/mc2_24_shadow_jump500.json`) captures byte-identical at `MC2_VISUAL_SETTLE=1` vs `2/30/60` (`.claude/DEFERRED-1-STATUS.md`). Dynamic CSM cascades rebuild fully every frame regardless of camera motion, so they carry no jump-triggered cost either.
- **Shadow banding shifts with camera rotation** (view-dependent terrain geometry). *Status:* addressed by STATIC-SHADOW-BANDING-1 (`9c31e2bf`, shipped) + STATIC-SHADOW-BANDING-2 (`5b0e488e`, gate `MC2_SHADOW_STATIC_BANDFIX_V2` default-OFF, awaiting user field check).
- **Dynamic prop shadows: no light-box cull yet** — with MC2_SHADOW_DYNAMIC_PROP_CASTERS=1 the dynamic pass draws ALL registered non-building props every frame (visibility-independent, depth-only) regardless of the light box. Cheap now; **HZB is the planned long-term fix**. Interim: CPU light-box AABB cull (must cull vs the LIGHT box, not the camera frustum). See `memory/shadow_dynamic_projection_and_caster_feed_fixed.md`.
- **Static building shadow goes stale on destroy** — MC2_STATIC_PROP_BUILDING_SHADOW bakes the world-fixed building shadow map once per mission; destroyed buildings keep their shadow until reload.
- **Dynamic CSM (`MC2_SHADOW_CSM`) is now default-ON** (2026-06-18, `8ff13a36`; was default-OFF). Reworked 2026-06-16 (`8d36b37d`): camera-target focus, 3 fixed cascades (R0=512 near / R1=4096 mid / full-map far), mech caster fix. Object self-shadow normal-offset bias kills residual acne (`13e96cc9`, `MC2_SHADOW_OBJ_NORMAL_BIAS=2.0`). Overlay/decal programs now also receive CSM shadows (`ead760df`). Opt out with `MC2_SHADOW_CSM=0`. Live runtime state queryable via the `"shadow"` block in the render state dump (`9d0556c0`).
- **Tree-foliage shadow alpha-test covers the LEGACY texture path only** (`b8f764b6`, `MC2_SHADOW_PROP_ALPHA`, `shadow_static_prop.frag`). Coalesce / BC7-atlas trees still cast solid square shadows — the alpha-test is not yet wired through the atlas sampling path. TODO: extend alpha-test to the coalesce/atlas path. (Supersedes the old SHADOW-FOLIAGE-ALPHA-DISCARD-1 deferral for the legacy path.)
- **Shadow VRAM cost.** 3 cascades × 8192² (`MC2_SHADOW_MAP_SIZE` default 8192) ≈ **805 MB**. Set `MC2_SHADOW_MAP_SIZE=4096` for ≈ **201 MB** (with R0=512 → 0.25 WU/texel, still sharp). Future option (not implemented): per-cascade resolution — keep cascades 0/1 at 8192 and put the full-map far cascade in a separate 4096 texture, saving VRAM without softening the near cascades.

## Water / terrain rendering

- **Cement pad `mat6_normal.tga` saga — RESOLVED (deferred item 3, claude/deferred-sweep-1).** History: the CEMENT-DETAIL-COLOR experiment sampled `matNormalArray[MAT_LAYER_PAINTED_CONC]` (= `data/textures/mat6_normal.tga`, at the time a true normal map) as *color* → dark blue slab; on 2026-06-14 the on-disk `.tga` was literally swapped to a concrete ALBEDO (backup kept as `mat6_normal.tga.orig_normal`) to compensate; `ead760df` (2026-06-16) then restored the cement diffuse atlas as the pad's albedo source in `gos_terrain.frag` + `terrain_lod_chunk.frag` (permanent, later absorbed by CEMENT-HARD-EDGE-1 — no "stopgap" code remains to retire). That left the RESIDUAL bug: layer 6 is still sampled AS A NORMAL for cement relief (`terrain_lod_chunk.frag` ~1031, `gos_terrain.frag` ~720), and every install deployed after 2026-06-14 (v0.4 AND rc1 — the old "fixed in rc1, not 0.4" claim was stale, both carried the same albedo-content file) decoded albedo-as-normal on pads. **Fix:** the true normal map is now repo-tracked at `data/textures/mat6_normal.tga` and ships via `deploy_payload.py` `GAME_DATA_PAYLOAD`, so any deploy mechanically restores correct pad relief. Cement albedo stays atlas-sourced.

- **LANDMINE — a shader program calling `calcDynamicShadow` MUST be compiled with the same `MC2_SHADOW_CSM` / `MC2_SHADOW_CSM_MAX` defines the engine binds for.** The overlay/decal programs lacked these defines → they fell into the legacy single-shadow branch → the CSM cascade uniform binds were dropped (`glGetUniformLocation` == -1) → no dynamic shadow on cement/decals (static shadows still worked because they take a different code path). Fixed `ead760df` by passing the CSM defines into those programs. Any new program that calls `calcDynamicShadow` must inherit the same define set.

- **Water shoreline z-fight on zoom/elevation-change (NOT pan); water sits slightly low** (pre-existing). Interim fast-path fixes shipped 2026-05-17. Full state: `memory/water_fastpath_interim_fixes_and_residuals.md`.
- **Terrain transparency / flicker during fast pan** (pre-existing ring-buffer coherency race, NOT a regression). When the camera pans quickly, brief transparent patches appear on the terrain. Root cause documented at `GameOS/gameos/gos_terrain_indirect.cpp` ~line 2660-2668: the thin-record ring-buffer fence wait can expire, producing a RAW hazard where the CPU overwrites a slot the GPU may still be reading. Effect is masked when the camera is stationary (thin-record content is frame-to-frame identical under no-motion). To confirm this is NOT caused by tile-retirement (COLORMAP-TILES-RETIRE-1): launch with `MC2_SETUPTEXTURES_LEGACY_FORCE=1` and pan at the same speed — the artifact reproduces identically with the 400-tile path active. Diagnostic gate: `MC2_RING_TRACE=1` writes `ring_trace.log` with per-frame fence wait timings and MVP fingerprints.

## Terrain LOD chunk renderer (NOW DEFAULT-ON, `MC2_TERRAIN_LOD_CHUNK=0` opts out)

- **DEFAULT-ON since 2026-06-09 (`a7b090be`).** The chunk path is now the default
  terrain renderer (single-source gate `mc2TerrainLodChunkEnabled()`); full tier1
  5/5 PASS on the no-flag chunk path. Set `MC2_TERRAIN_LOD_CHUNK=0` for the legacy
  tessellated path. **EDITOR:** now on the chunk path and rendering correctly (fixed
  2026-06-10) — the editor frame loop was building the chunk draw commands
  (`land->render()`) but never submitting them: `Terrain::flushDrawCommands()` lived
  only in `code/gamecam.cpp` (grep `Terrain::flushDrawCommands` — line drifts), so under the default-on chunk path (which suppresses
  the legacy per-quad draw) terrain rendered BLACK (only overlays drew). Fixed by
  mirroring the game's flush in `EditorCamera::render` (after shadow/compass, before
  `renderLists()`). If terrain misbehaves, `MC2_TERRAIN_LOD_CHUNK=0` still falls back
  to legacy.
- The chunk path reached parity with legacy (stitching/shadows/smooth-normal/
  material+colour/concrete+cement-atlas + all ImGui tunables). Full record + the
  3 load-bearing depth/MVP rules:
  `memory/HANDOFF_2026_06_09_terrain_lod_chunk_phase10_fidelity_cutover_prep.md`.
- **RESOLVED (do not re-discover) — three chunk-ONLY bugs were depth/MVP/early-Z,
  NOT a "matrix divergence":** water-recede-on-zoom (net depth must be -0.004, the
  legacy thin path double-applies the fudge), shore-water-dropout-on-pitch +
  decal-tearing-on-zoom (chunk must project with the baked dispatch MVP
  `IsFrameSolidArmed()?getDispatchMvp16():live` — frame N-1 — that water-cull/decals
  use, else a 1-frame offset), and the early-Z violation (bias PRE-DIVIDE in the
  vert, never frag `gl_FragDepth`). All fixed (`4da9cfb1`,`67e4f5e4`).
- Editor is now on the chunk path (see EDITOR note above, fixed 2026-06-10);
  interactive gameplay sanity (picking/gates/turrets) in the editor is still a
  worthwhile manual check.

## First-launch / startup

- **First-launch black terrain intermittency** — tier1 first mission occasionally renders black; second normal. Suspected: GPU/shader state dirty from previous mission teardown (or a driver-level cold-start race between the previous process's teardown and the next process's context creation — each tier1 mission is its own fresh process, so no in-process state carries over). **NOT REPRODUCED (deferred item 9, `claude/deferred-sweep-1`, 2026-07-17):** 23 fresh cold launches of mc2_01 (own-PID-discipline, no `--kill-existing`, real non-fixed clock so a genuine timing race isn't masked) across two independent detection methods — 15 via `scripts/run_visual_capture.py`'s glReadPixels FBO capture, 8 via the engine's own `MC2_SCREENSHOT_AT_FRAME`+`MC2_LOAD_RACE_TRACE` oracle (`GameOS/gameos/gameosmain.cpp`) — all 23 rendered normal terrain (mean RGB ~103-104, near-black pixel fraction ~0.04-0.05%, stable to within noise across every run). **The right ambush for a future session:** `MC2_LOAD_RACE_TRACE=1` + `MC2_SCREENSHOT_AT_FRAME=<N>` + `MC2_SCREENSHOT_PATH=<path>.tga` (native to `gameosmain.cpp`, works minimized, near-zero overhead — no bookmark harness needed) launched as a tight cold-launch loop; watch the `atlas first-resident`/`gpu terrain first-arm` frame numbers for a late or missing fire and the screenshot itself for ground truth. It's intermittent and this session's 23 attempts came up clean — more attempts, a slower/cold-disk machine state, or a different mission may be what it takes.
- **`MC2_LOAD_RACE_TRACE` terrain-residency false negative — FIXED (`claude/deferred-sweep-1`, commit `235b88f3`, 2026-07-16).** The item-9 hunt above found `terrain_resident`/`degraded` permanently `N`/`YES` on every run under the default GPU-indirect/chunk renderer, even on visually-normal terrain — the probe (`mclib/txmmgr.cpp` ~3096-3150) only ever walked the legacy per-quad path (`masterVertexNodes`), which the chunk renderer never populates. Fixed by gating the probe on `mc2TerrainLodChunkEnabled()`: chunk-path runs now reuse the same `g_loadRaceAtlasFirstFrame`/`g_loadRaceArmFirstFrame` latches that already drive the `[LOADRACE] atlas first-resident` / `[LOADRACE] gpu terrain first-arm` lines (two cached-int compares, no new walk); the legacy masterVertexNodes walk is unchanged and only runs when `MC2_TERRAIN_LOD_CHUNK=0`. Re-verified: 8/8 fresh cold launches now report `terrain_resident=Y degraded=no terrain_first_resident_frame=1`, with 3 spot-checked screenshots matching the item-9 healthy baseline (mean RGB ~104/101/87, near-black <0.07%). See `docs/tier1_env_vars.md`'s `MC2_LOAD_RACE_TRACE` entry for the legacy-path caveat surfaced while verifying this fix.

## Editor picking

- **Editor pick uses raw MFC client pixels vs GL viewport space (pre-existing, low-risk).** `EditorObjectMgr::getObjectAtScreenPosition` now forward-projects each object to screen via `Camera::projectForScreenXY` and matches by pixel distance (fixed the wrong-far-object pick; commit screen-space-pick). The click coords are raw MFC `CPoint` client pixels, while `projectForScreenXY` uses the `gos_GetViewport` GL viewport — same space the status bar uses, and they agree UNLESS the editor window is resized so GL viewport width != MFC client width (`EditorInterface.cpp:~4072` normalizes this for the gameplay-pick path but the status-bar/object-pick paths do not). Correct in the common no-resize case; if a resize-offset pick is ever reported, normalize the click coords the same way the gameplay pick does. Meta-fix history: `memory/editor_pick_screen_projection_metafix.md`.

## Editor smoke

- **MSBuild stale-object after a same-second edit (false smoke failure).** If a NEW `[ESMOKE v1]` field comes back missing / `None` from `run_editor_smoke.py` right after a `check-editor-build.sh --build-only` that printed PASS, the build likely SKIPPED recompiling a just-edited source: MSBuild's up-to-date check has ~1s timestamp granularity, so an edit landing in the same second as the prior object can be judged "up to date" and the deployed exe ships old code. **Diagnose before assuming a code/path bug:** confirm the field string is literally in the built exe — `powershell -NoProfile -Command "Select-String -Path 'build64/out/editor/RelWithDebInfo/Mission Editor.exe' -Pattern 'your_new_field' -SimpleMatch -Quiet"`. If ABSENT, force a recompile (touch the source: `(Get-Item editor/EditorMFC.cpp).LastWriteTime = Get-Date`) and rebuild + redeploy. Observed 2026-06-09 (validate_* fields). Memory: `memory/msbuild_stale_object_same_second_edit.md`.

- **Editor smoke launch flake (`0xC0000005`)** — `foliage_present` and `foliage_menu_commands` occasionally crash with `STATUS_ACCESS_VIOLATION` (`0xC0000005`, rc `3221225477`) ~1s into a **back-to-back full-suite** `run_editor_smoke.py` run, but PASS reliably **in isolation**. Suspected editor process teardown/startup contention or a deployed asset/runtime-state race (sibling of the first-launch black-terrain teardown intermittency above). **NOT introduced by the Scene Outliner / Inspector work** — those cases pass in the same runs. Policy (`run_case_with_retry`, visible quarantine): an `0xC0000005` failure is retried **once**; crash-then-pass → reported as `FLAKY_PASS` and the suite header is starred (`result=PASS* (.., N flaky-pass)`) so the run is never read as strictly clean; a double `0xC0000005` stays a hard FAIL. Only `0xC0000005` is retried. If it reappears outside foliage cases or as a double-fail, investigate teardown — do not raise the retry count. Memory: `memory/editor_smoke_foliage_launch_flake.md`.

## Options / config

- **Options menu writes bad ResolutionX/Y to options.cfg** (observed 4096x2160 on 4K). MITIGATED by `[HUD-RES-CLAMP v1]`: the render base is force-clamped to 800×600 at both resolution entry points (`gos_SetScreenMode`, `gameosmain` startup), so a bad ResolutionX/Y no longer reaches the HUD layout — HUD/video stay correct, scene upscales from 800×600. Diagnostics: `memory/options_cfg_resolution_drift.md`, `memory/hud_scene_resolution_separation.md`. `MC2_RES_DIAG=1`.

## Blocked slice work

- **Stage 0.5 §4 (renderVisible repoint) BLOCKED — empirically NO-GO 2026-05-20 EVENING.** Tentative ship `40a54b7` reverted as `dc2e8f6` (popping + black-textures + resurrected 2026-05-05 black-tree class). §2.5 sticky-bit (`91b6991`) shipped independently and is durable. §4 deferred to alpha-Stage 1 OR pivot to v4 (gate render on `blockVisBits[]` directly under sticky). Full: `memory/stage_0_5_section_4_blocked_on_readback_non_superset.md`.

## Default-on flags / state

- **drawPass-retirement decal static-bake (`MC2_TERRAIN_INDIRECT_OVERLAY`): DEFAULT-ON since Stage-6 flip `60f2ef8`** (only `=0` reverts). On default path both SOLID and OVERLAY are armed so per-quad `draw()` loop in `Terrain::render drawPass` is SKIPPED. Slice A+B WIRED & validated 2026-05-17. Remaining endpoint: user-driven substitutive non-COST_SPLIT Tracy proof + decal visual canary. Full state: `memory/drawpass_retirement_decal_bake_state_and_raster_sheet_trap.md`.

## RenderSnapshot ok gate

- ~~**mc2_10 `ok=0` due to `staticPropValidationFail` (`sp_fail=1`).**~~ **FIXED `7bdbd1fd`.** `invalidateStaticRegistration()` tombstoned the registry recipe but never called `retireRecord` on the matching `s_objectRecords` slot; the slot stayed `alive=true` with a tombstoned recipe, causing `sp_fail=1` from frame ~1706 onward. Fix: `retireRecord` call added. Requires re-validation of mc2_10 with `MC2_RENDER_SNAPSHOT_LOG=1` to confirm ok=1 throughout.

## RenderWorld arc residuals

- **MLR-rendered mechs do not write object IDs (M2.5 known gap).** M2.6 pickup will work only for GPU-batched mech pixels. Empirical tier1 data shows `mlr_mech_draws=0` across all 5 missions, so the gap is rare-in-practice. If tier1 ever shows `mlr_mech_draws>0`, M2.6 must preserve mover-first legacy fallback for those mechs and cannot claim full mech GPU-pick coverage. Counter: `[MECHBATCHER v1] event=mlr_mech_summary mlr_mech_draws=M` on per-mission summary.
- **gosFX dev-override broken under unified projection (F1 ship).** Running with `MC2_DISABLE_GOSFX=0` after F1 will render gosFX particles wrong: MLR's `mlrclipper.cpp:206-209,305,321,347` reads of `cameraToClip(2,2)` and `(3,2)` use stale MC2-pixel-homogeneous convention while the runtime uniforms drive shaders via the new GL convention (clip.w > 0 for in-front; polarity folded into `kAxisSwapMC2toGL` per addendum-rclipw-polarity.md). Default `MC2_DISABLE_GOSFX=1` (gate-dead MLR work-leaves) is unaffected. Runtime guard prints `[UNIFIED_PROJ v1] WARN:` stderr once per startup. Dev-override path re-enabled when MLR retirement Slices 1-5 ship.

## Asset / atlas pre-commit guards

- **Do NOT upscale the icon atlases.** `code/mechicon.cpp` hardcodes `unitIconX/Y` (32/38) and reads source-pixel offsets against `s_MechTextures->width` / `s_pilotTextures->width`. Oversized source TGAs (upscaler `*_4x_gpu/` or Magic's Unofficial Expansion overlay) scramble icon sub-rectangles. Keep all 9 icon atlases at FST-archive resolution + `mcl_pr_pilotskillicons.tga`. Pre-commit guard: `sh scripts/check-asset-scale-callers.sh`.
