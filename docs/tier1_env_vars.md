# Tier-1 instrumentation env vars

> s68/P9: Retired GL sources are cited with Git object syntax (`8064c6930:path`) so the historical implementation remains inspectable.

> **STATUS: Verified current as of 2026-07-09 (nifty HEAD, build pipeline). Includes MC2_TERRAIN_LOD_CHUNK (default ON), options.cfg ShadowCSM (default ON), asset-mod payload vars (HDRI_BC6H, BUILDING_PBR), diagnostic JSONL trace, and the Vulkan-prep / frame-graph-executor / render-backend-iface seam gates. Match against RendererFeatureRegistry.h for new feature gates. Bump this date whenever gates are added or edited below.**

## Adding a new MC2_* gate (checklist)

1. Register FIRST in `RenderCore/RendererFeatureRegistry.h` (`kFeatureTable` for a renderer feature gate, `kAuxEnvVars` for a trace/aux var).
2. Read it via `getenv`/`envFlag`/`selftestEnvFlag`/`envFlagDefaultOn` (the last is the default-ON kill-switch pattern — see `MC2_SMART_LOAD` below).
3. `sh scripts/check-env-registry.sh` must pass — unregistered `MC2_*` reads are CI-blocking (exit 1 on unknown vars; see header comment at the top of the script).
4. Mark the gate `passthrough` in its `Gates/gate_table.cpp` row; `scripts/run_smoke.py` generates its passthrough list from those rows (the parent environment is inherited anyway, see below).
5. Document it here in `docs/tier1_env_vars.md`: default value, polarity, one-line effect.
6. Prove gate-ON vs gate-OFF smoke: `py -3 scripts/run_smoke.py --tier tier1 --duration 30 --keep-logs`

## Live data-contract guards (MC2-VERIFY-LIVE-1)

- `MC2_VERIFY_MODE` — mode for the live `MC2_VERIFY` guard family (`docs/verify-primitive.md`). `log` (**default**): shadow-log `[VERIFY]` lines to stderr + crash-bundle ring, continue; `[VERIFY] mission-end fires=N` counter at Mission::destroy. `fatal`: STOP with message + crash bundle + hard terminate (exit 0xE0564631). `off`: exactly-legacy (silent, guarded degradations skipped). Smoke-allowlisted.

## Cursor / focus hands-off for automation (SMOKE-NO-CURSOR-STEAL-1)

- `MC2_NO_CURSOR_GRAB` — suppress **all** OS-cursor manipulation by the engine: the window-creation mouse grab (`SDL_SetWindowMouseGrab` + Win32 `ClipCursor` clamp to the client rect, re-asserted on every `FOCUS_GAINED`/resize/move), and every pointer warp through `gos_SetMousePosition` → `SDL_WarpMouseInWindow` (notably the tutorial `guiFrozen` path in `missiongui.cpp`, which recenters the pointer **every frame**). Also sets `SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN=1` so the game window does not steal keyboard focus on show.
  **Default: suppression is ON whenever `MC2_SMOKE_MODE` is set** — i.e. every agent-driven smoke / visual_verify / capture / screenshot harness gets it for free (they all set `MC2_SMOKE_MODE=1`; `scripts/smoke_lib/runner.py:83`). Interactive launches (neither var set) keep exactly the legacy behavior.
  `=1` forces suppression on even for an interactive launch; `=0` forces it **off** even under smoke (escape hatch, e.g. reproducing a grab-dependent bug headlessly). Evaluated once and cached (`gos_CursorControlSuppressed()`, `GameOS/gameos/gameos_input.cpp`).
  **Scope, stated so it cannot drift again (META4-CLOSE-1):** this flag governs the **cursor and show-time activation only**. It does *not* remove the taskbar button or the Alt-Tab entry, does not make the window refuse keyboard focus, and does not suppress the crash dialog. Those are keyed on `gos_NoUserPresentRequested()` — `MC2_WINDOW_OFFSCREEN` or `MC2_SMOKE_MODE` — because there is no window a user could be looking at in those cases. Between `ffebf5e87` and `META4-CLOSE-1` the union was used, and an interactive `=1` launch really did lose all four; that is fixed, and this paragraph is the contract.

## Window never on the visible desktop (NOFOCUS-HARDEN-1)

- `MC2_WINDOW_OFFSCREEN` — create the SDL window **hidden at (-32000,-32000)** and keep it there, so an agent-launched engine window never occupies visible desktop area at *any* point in its lifetime.
  **Default: OFF** for `MC2_WINDOW_OFFSCREEN` — the engine-side polarity is opt-in, and unset means exactly-legacy interactive behaviour.
  Reader: `window_offscreen_requested()` in `GameOS/gameos/gos_render.cpp` (`e && e[0] && e[0] != '0'`, evaluated a single time and cached). `=1` enables; `=0` disables.
  **Why not the old poller.** The previous design created the window at `options.cfg` size in the middle of the desktop and relied on an external 250 ms Python poller (`smoke_lib/nofocus.park_window_offscreen`) to move it away *afterwards* — which guarantees a multi-second desktop-covering flash on every launch, measured live at 1719x995 on a 1707x960 desktop. Post-hoc repositioning is the wrong shape: the only way a window is never seen is for it never to be created anywhere visible. The poller is now a **backstop only**, for deployed exes built before this change (where the var is inert).
  **What it does:** position is correct at `SDL_CreateWindow` time (`SDL_WINDOW_HIDDEN` + explicit offscreen origin, so the `CreateWindowEx` HWND never lands on a visible pixel); re-asserted immediately before the single `SDL_ShowWindow`; re-asserted **synchronously** from `resize_window()` and `set_window_fullscreen()` — the path `prefs.applyPrefs()` → `gos_SetScreenMode` takes during boot, which is what used to undo a one-shot park; and **fullscreen is refused outright**, because a fullscreen window has no position that hides it.
  **No pixel effect.** The GL drawable keeps its full size, so `glReadPixels`/FBO readback — the golden visual-capture path — is unaffected, unlike minimizing (which makes SDL throttle and can zero the drawable). Golden capture resolution is preserved, so window *size* stops mattering.
  **Harness side:** `scripts/smoke_lib/nofocus.py` applies it via `OFFSCREEN_ENV` from `apply_env(offscreen=True)`, which is the parameter's default for agent launches. Three delivery paths do **not** go through `apply_env` and set it themselves, because a var the launcher never delivers is the same as no fix at all (NOFOCUS-FINISH-1): `smoke_lib/runner.FORCED_ENV` (the tier-1 smoke path — its own `STARTUPINFO` + `Popen`), `run_visual_capture.py` (env built by `mc2_env_pin.build_pinned_env`, so the var is also in `mc2_env_pin.HARNESS_CONTROL` or the deny-by-default scrub removes it), and `tools/tracy_capture_compare.py` (own `Popen`). `scripts/check-background-launch.py` RULE 2 and RULE 8 fail the build if any of them stops delivering it.
  **`show_window` is orthogonal.** An offscreen window is neither `SDL_WINDOW_MINIMIZED` nor `SDL_WINDOW_HIDDEN`, and `gameosmain.cpp`'s 10 ms background throttle keys on exactly those two flags — so a perf capture can be offscreen *and* unthrottled. `nofocus.spawn`/`run` therefore keep `offscreen` at `True` even when `show_window=True`; the old `offscreen = not show_window` coupling silently put perf-run and menu-oracle windows back on the user's desktop.
  > **CAVEAT — desktop-composite harnesses.** Any harness that screenshots the **desktop composite** (`pyautogui`/`mss`/`PIL.ImageGrab`) rather than reading the engine FBO gets *no pixels* from an offscreen window and would capture the user's wallpaper — a confident wrong visual verdict, which is worse than a visible window. Those call sites are enumerated in `check-background-launch.COMPOSITE_CAPTURE_FILES` (8 today) and must pass `offscreen=False`; RULE 6/RULE 7 keep the list honest in both directions. Declaring it is not being granted it: `nofocus.apply_env` raises `DesktopOccupancyRefused` unless the session is user-present (`MC2_HARNESS_ALLOW_FOREGROUND=1`), so an agent run aborts instead of blessing wallpaper. Converting a harness to the engine capture path (`MC2_VISUAL_CAPTURE` / `MC2_SCREENSHOT_PATH` on the Vulkan backend) retires its entry.

## Force all units visible for automated captures (FORCE-ALL-VISIBLE-1)

- `MC2_FORCE_ALL_VISIBLE` — mark **every** unit (mechs, ground vehicles, elementals/infantry, all teams) fully visible and sensed, bypassing sensor/contact gating so nothing in a capture is hidden, blip-only or faded out.
  **Why it exists:** agents repeatedly graded frames in which the unit under test was hidden or mid-fade, producing false negatives — a ghost-cone leak passed verification at four bookmark poses purely because no mover happened to be mid-fade in view, and vehicle-lighting lanes repeatedly captured scenes containing no visible vehicle at all.
  **Hiding is not one mechanism, it is three**, and this gate defeats all of them at their single source in `BattleMech::update` / `GroundVehicle::update` (`mc2_vis::forceVisibleOverride`, `mclib/vis_semantics.h`):
  1. **`alphaValue` faded to 0** — the *actual* hide for blip-only enemies (`code/mech.cpp`, `code/gvehicl.cpp` contact branches), honoured by ordinary leaves via `MC2_DRAWALPHA`. Note that `setVisibility(true,true)` is called on **every** contact branch, so the `visible` flag is **not** the real mechanism.
  2. **`sensorLevel`** — drives the blip-shape draw (`1..4`) and the `sensorLevel == 0 || sensorLevel >= 5` emission predicates. It is written only inside the `isOnGui` arm, so it can otherwise retain a stale value indefinitely. Forced to `5`: the only value satisfying both that predicate and the stricter `sensorLevel > 4` one (`mech3d.cpp` anubis pointLight), and it suppresses the blip shape in favour of the real mesh.
  3. the legacy **`visible`** flag — set for completeness.
  It additionally short-circuits the four `mc2_vis` predicates (`meshDrawn` / `knownToPlayer` / `emitsLight` / `selfLit`) and the `cone_glow.cpp` guards, so a forced-visible unit also emits and is lit.
  **Default: ON whenever `MC2_SMOKE_MODE` is set** — every capture harness gets it for free, since `run_smoke.py`, `run_visual_capture.py`, `capture_baseline.py`, `quick_shot.py`, `mech_shot.py`, `smoke_with_screenshots.py`, `renderdoc_capture.py` and `camera_sweep_smoke.py` all share `scripts/smoke_lib/runner.py`. Same shape as `MC2_NO_CURSOR_GRAB` above.
  `=0` forces it **off** even under smoke; unset + no smoke = normal interactive fog of war, unchanged. Engine-side default is OFF.
  > **CONSEQUENCE — read this before grading a capture.** A smoke capture is **no longer a fog-of-war-accurate frame**. Any test that specifically validates sensor, contact, or visibility behaviour **must** set `MC2_FORCE_ALL_VISIBLE=0`. This applies in particular to lane `VISIBILITY-SEMANTICS-1`, which owns the visibility/emission/self-lit predicate redesign and needs the OFF path for its own verification.

## Crash-soak harness (MC2_SOAK_AUTOWIN)

- `MC2_SOAK_AUTOWIN=1` — with a campaign booted via `MC2_BOOT_TO_BAY`, auto-launches each mission from logistics (no clicks), auto-wins it, lets the campaign auto-advance, and repeats until `campaign-complete`. Emits `[SOAK]` stdout markers (autowin/advance/launch/campaign-complete). Default OFF = byte-identical.
- `MC2_SOAK_WIN_AFTER_SEC=5.0` — scenario-time seconds before auto-win fires (default 5.0). Only consulted when `MC2_SOAK_AUTOWIN` is set.
- `MC2_SOAK_SALVAGE_TRADEMOVER=1` — ISSUE-42a repro fixture. With `MC2_SOAK_AUTOWIN` + `MC2_SOAK_KILL_ENEMY`, runs the REAL mid-mission salvage-craft flow (recover / setStatus(SHUTDOWN,force) / tradeMover / orderPowerUp — the exact missiongui.cpp VTOL sequence) on ONE disabled enemy mech at 80% of the win timer, so the Phase-3 `[SOAK] salvage-in-loadbay` check verifies `LogisticsData::setMissionCompleted`'s recovered-mech carry-over branch instead of the Phase-2 `addMechToInventory` shortcut. Emits `[SOAK] salvage-trademover name=… variant=… pilot=…`. Pair with `MC2_LOG_LOGISTICS_FLOW=1` + `MC2_DIAG_TAGS=LOGISTICS` for the per-unit cull/resolve/mech_lost JSONL trail. Default OFF = byte-identical.

## ABL compile safety

- `MC2_ABL_COMPILE_FATAL` — **default ON** fail-loud guard. If ABL preprocessing reports syntax errors, logs `[ABL_COMPILE_FATAL]` with file/error count and exits before partial bytecode can execute and corrupt `codeSegmentPtr`. Set exactly `=0` only to reproduce legacy execute-anyway behavior.
- `MC2_ABL_COMPILE_FATAL_REPRO=<absolute-path>` — negative-test hook. During ABL initialization, preprocesses the supplied malformed fixture; expected result with default settings is an attributable `[ABL_COMPILE_FATAL]` refusal before mission load.
- `MC2_ABL_SKIP_ERRORED_MODULES=1` — playability escape hatch: permits mission load but skips execution of every module with compile errors. Takes precedence over compile-fatal so bad third-party brains remain contained rather than crashing the VM.

## Feature gates (RendererFeatureRegistry.h kFeatureTable)

- `MC2_COLORMAP_KTX2` — BC7 KTX2 atlas. Default **ON**. Kill=`=0`. Bake: `py -3 scripts/bake_colormap_ktx2.py`.
- `MC2_COLORMAP_CPU_RETIRE` — free cpuColorMap after atlas upload. Default **ON**. Side effect: retires the CPU *dirt-displacement* half of `MapData::terrainElevation()` (its GPU counterpart, `gos_terrain.tese`, has had no reachable draw since `75b8ebef`). The *Phong* half of the same block is retired separately by `MC2_CPU_PHONG_RETIRE`.
- **options.cfg `CpuPhongRetire`** (this was the env gate `MC2_CPU_PHONG_RETIRE`, deleted 2026-10-12: the value is now the options.cfg key `CpuPhongRetire`, read once at startup; `=0`/`=1` in the text below means `FALSE`/`TRUE` (or the number); `MC2_OPTIONS_OVERRIDE="CpuPhongRetire=<value>"` sets it for one run without editing the file) — CPU-PHONG-RETIRE-1. Retires the CPU Phong-smoothing term in `MapData::terrainElevation()` (`mclib/mapdata.cpp`). Default **ON** (kill-switch; `=0` restores the exact legacy term). That term existed only to mirror the GPU TES `shaders/gos_terrain.tese:101-108`, which stopped being drawn at `75b8ebef` (2026-06-29) — the ground is now rasterized by `8064c6930:shaders/terrain_lod_chunk.vert`+`.frag`, VS+FS with no TCS/TES, which Phong-smooths nothing. ⚠ **USER-VISIBLE**: unset changes where every object sits on every map wherever the ground curves. Measured offline on the real tier1 `.pak` heightfields: mean |Δ| 0.58–1.35 wu, p99 3.8–6.4 wu, max 12.7–18.9 wu (1 wu = 0.2 m → up to ~3.8 m), 19–49 % of positions over 1 wu; exactly zero on planar ground (flat *and* uniform slope), zero-mean, so objects float on some cells and sink on others. ⚠ **Not smoked** — engine launches were hard-blocked in the authoring lane; needs a tier1 run + click-through before merge. Paired-site drift guarded by `scripts/check-cpu-gpu-cogating.py`.
- `MC2_OPTIONS_OVERRIDE=Key=Value,Key=Value` - test channel for the options.cfg render options (`Gates/render_options.h`; keys: PostCAS, PostCASSharpness 0..1, TonemapMode 0..3, ShadowCSM, ShadowBoundedNearFit, PropShadowReceive 0..1, ShorelineLegacySuppress, CpuPhongRetire, StaticPropAmbient, StaticPropIblSh, StaticPropIblSunRotate, StaticPropIblSunRotateFloor 0..2, StaticPropPbr, SunAxisFix, SunElevationFix, VfxSoftParticles, VfxLitParticles). Applied over the file by `CPrefs::applyPrefs()`, never written back by `CPrefs::save()`; booleans accept TRUE/FALSE/1/0. A garbage, non-finite or out-of-range value (in the file or here) is rejected with one `[OPTIONS v1] event=rejected` line and the default stays. The smoke matrices and capture drivers use it instead of editing the install options.cfg.
- `MC2_VIEW_UNIFORMS` — ViewUniforms UBO. Default **ON**. Kill=`=0`. Requires restart.
- `MC2_SHADOW_ENABLE=0` — dynamic shadow caster pass. Default **ON** (kill-switch). **Corrected 2026-07-30 (GATE-DEFAULT-TRUTH-1): this line said "Default OFF" and was wrong.** Both GL readers implement default-ON (`gos_static_prop_batcher.cpp:8717`, `gos_mech_batcher.cpp:1112` — `!(v && v[0]=='0')`), so `=1 vs unset` A/Bs were comparing ON against ON. ⚠ **The VK reader disagrees**: `vulkan_post_screenfx_native.cpp:760` is opt-in `== 1`, so a default *Vulkan* session takes no dynamic caster pass. Known parity defect, deliberately not flipped blind.
- `MC2_IMGUI=0` — ImGui overlay. Default **ON** (kill-switch; reader `GuiRuntime/GuiRuntime.cpp:88`).
- `MC2_IMGUI_INSPECTOR=1` — inspector panel. Default **OFF**. Requires `MC2_IMGUI`.
- `MC2_DEBUG_RENDERER=1` — debug overlay. Requires `MC2_IMGUI_INSPECTOR`.
- `MC2_HDRI_BC6H` — upload HDRI sky as BC6H_UFLOAT (requires `.ktx2` sidecar + `GL_ARB_texture_compression_bptc`). Default **ON** (absent = ON; set `=0` to force RGBA16F fallback). Sidecar cooked via `tools/cook_bc6h_hdri.py`.

## OpenGL-correctness campaign gates (2026-06-22, RENDER-BACKEND-SEAMS)

All default-OFF/inert unless noted. See `docs/render-backend-seams/opengl-correctness-ledger-1.md`.

- `MC2_RENDER_PASS_CONTRACT_TRACE=1` / `MC2_RENDER_PASS_CONTRACT_ASSERT=1` — pass-scope tracker (ENFORCEMENT-1): logs begin/end with stack depth / reports nesting+owner+missing-end violations. Non-fatal. Distinct from the aborting `MC2_RENDER_CONTRACT_ASSERT` and the CONTRACT-3 `MC2_RENDER_PASS_ORDER`.
- `MC2_WATER_THINRING_TRACE=1` — water thin-record ring fence trace (WATER-THINRING-FENCE-1): `WATER_THINRING: frame/slot/bytes/waited/wait_result/fence_created`.
- `MC2_R2B_TOUCH_PRESERVE` — **DEFAULT-ON** killswitch for the R2b static-natural tree cachedFrame_ stamp (NVIDIA tree-disappear fix). `=0` reproduces the bug (registry drops trees). A/B partner of the registry `stale_after_drawn` counter.
- `MC2_R2B_STATIC_NATURAL_TRACE=1` — `R2B_STATIC_NATURAL: skipped_full_update/touched_liveness/preserve`.
- `MC2_REGFLUSH_DIAG_TRACE=1` — static-prop registry flush summary incl. `stale_after_drawn` (the regression-guard counter; a persistent nonzero for an on-screen typeID = a cachedFrame_-stamp-skip regression).
- `MC2_STATIC_STALE_DROP_FATAL=1` — opt-in `abort()` on a registry stale-frame drop of an already-drawn range (CI/bisection teeth for the black-tree-bug class).
- `MC2_LOAD_RACE_TRACE=1` — mission-load residency-race one-shot diagnostics (default OFF). Logs `[LOADRACE] getAppearance NULL:` on each NULL appearance return (capped 50), `[LOADRACE] mech degrade #N:` per Mech3DAppearance instance that hit the no-base-shape degrade branch (name/apprNum, capped 50), `[LOADRACE] terrain first fully-resident frame=N` once, and a single machine-parseable `LOADRACE verdict: frame=… mech_degrade=… appr_null=… terrain_resident=Y/N terrain_first_resident_frame=… degraded=YES/no` line at `MC2_LOAD_RACE_VERDICT_FRAME` (defaults to `MC2_SCREENSHOT_AT_FRAME`). Zero output gate-off.
  **Terrain-residency probe routing (`mclib/vk_scene_renderer_passes.cpp`, fixed `claude/deferred-sweep-1` commit `235b88f3`):** the probe is gated on `mc2TerrainLodChunkEnabled()` and reads from whichever renderer is actually active — under the default chunk/GPU-indirect path it reuses the `g_loadRaceAtlasFirstFrame`/`g_loadRaceArmFirstFrame` latches (same state that drives the `[LOADRACE] atlas first-resident` / `[LOADRACE] gpu terrain first-arm` lines below), a cached-int read with no per-frame walk; under `MC2_TERRAIN_LOD_CHUNK=0` it falls back to the original `masterVertexNodes` walk. **Caveat found while verifying this fix:** in a *game* build (not `EditRel`/`MC2_IS_EDITOR`), `TerrainQuad::setupTextures()` — the function that would populate/mark `masterVertexNodes` terrain-node residency — is compile-gated out entirely (`mclib/quad.cpp` ~706-1337, commit `006800e5`, predates this fix). So `MC2_TERRAIN_LOD_CHUNK=0` has **no production renderer** in game builds (confirmed: `[BUCKET_CENSUS v1] ... legacy=0` at every frame) and the legacy branch of this probe will correctly report `terrain_resident=N` on *every* game-build run regardless of load state — a true negative reflecting the vestigial opt-out, not a bug in the probe. The legacy branch only has a live signal to read on an `MC2_IS_EDITOR` build exercising the passability-grid path.
- `MC2_OVERLAY_TEXTURE_TRACE=1` — overlay/decal resolve-fail counters + 1×1-magenta fallback-bind log (OMT-1): `[OVERLAY_TEXTURE v1] resolve_fail/summary`.
- `MC2_GPUBUF_RING=1` — **default OFF** (VULKAN-CONTRACT-MANIFEST-ARC, GPU-BUFFER-WRAPPER-TIER0-HUD-1). Routes the fixed-capacity gosMesh HUD/2D meshes (`gameos_graphics.cpp` `gosMesh::draw`/`drawIndexed`) through a 3-frame **persistent-coherent FENCED ring** (`8064c6930:GameOS/gameos/utils/gpu_ring_buffer.h`) instead of the per-batch `glBufferData` orphan. Pilot of the `GpuRingBuffer<N>` shape (persistent-coherent map, per-slot base offset, `glClientWaitSync`-on-reuse, `glFenceSync`-after-draw, KHR_debug label) on the smallest/safest live buffer. **OFF = legacy orphan path, byte-identical GL stream + output** (one cached-bool branch). **ON =** the `[GPUBUF v1]` `hud` orphan call count drops to ~0 (the ring memcpys into the coherent map; no per-batch `glBufferData`). Fence discipline reuses `gos_mech_batcher.cpp`'s pattern. `uploadBuffers()` (used by terrain via direct `getVB()/getIB()` NULL-offset draws) intentionally stays on the legacy path — its draws are external and would not honor a slot offset. Ring buffers are lazily created on first ON-use and drained+unmapped+deleted in `gosMesh::destroy` (no leak across mission/device reset).
- `MC2_GPUBUF_RING_FORCE_WRAP=1` — **NEGATIVE TEST**, default **OFF**, requires `MC2_GPUBUF_RING=1`. Deterministically advances the HUD ring twice (`beginFrame()` without an intervening `endFrame()`) so the one-`endFrame`-per-`beginFrame` / fence-per-frame invariant trips: logs `[GPUBUF v1] event=begin_without_end tag=… slot=…` (and `abort()`s under a debug build). Proves the safety guard is live rather than the happy path merely rendering. Never set in production.
- `MC2_GPU_DEBUG_NAMES=1` — **presence-gated**, default **OFF** (VULKAN-DEBUG-NAMES-1). Attaches `glObjectLabel` KHR_debug names to GPU buffers so RenderDoc / RGP / Vulkan-prep captures show human-readable names instead of anonymous handles. Helper `GameOS/gameos/utils/gpu_debug_labels.h::setGpuBufferDebugLabel`; proof site labels the ViewUniforms UBO (`view_uniforms_gl.cpp`). Read once into a static bool; also null-checks `glObjectLabel` (only wired under `MC2_GL_DEBUG`, so it may be absent — a missing extension must never crash). **OFF = one predicted-false branch, no GL call, byte-identical GL stream + output.** Diagnostic/tooling only.
- `MC2_GPUBUF_RESIDENCY=1` — default **OFF**. Once every ~300 frames lists the live HUD ring buffers: `[GPUBUF v1] residency tag=… kind=meshring frames=3 vbBytes=… ibBytes=… liveFences=…`. Diagnostic only.
- `MC2_GPUBUF_COUNTER=1` — **measurement-only**, default **OFF** (VULKAN-CONTRACT-MANIFEST-ARC slice 4, GPU-UPDATE-BUFFER-COUNTER-1). Counts per-frame orphan-on-write `glBufferData` re-spec (full re-spec, no fence/ring) that is otherwise INVISIBLE in the hitch trace (which only emits on >threshold frames). Independent gate that OR-enables the existing `MC2_GL_BufferData` accountant + a per-owner tally for these orphan sites, WITHOUT forcing the rest of `MC2_HITCH_TRACE` on. Emits every frame: `[GPUBUF v1] frame=N orphan_calls=C orphan_bytes=B by_owner=gos_UpdateBuffer:c/b,hud:c/b,light:c/b,sp_shadow:c/b`. Instrumented sites: `gos_UpdateBuffer`, the gosMesh/HUD `updateBuffer` 5-arg overload, the light SSBO create/grow/orphan path, and the static/dynamic-prop transient shadow SSBOs. Zero behavior change when unset (one predicted-false cached-bool check; byte-identical GL stream + output).
- `MC2_GPUBUF_LIGHT_GROWONCE=1` — **default OFF** (VULKAN-CONTRACT-MANIFEST-ARC, LIGHT-GROW-ONCE-SUBDATA-1). The **AMD-safe intermediate** for the light SSBO orphan churn — **NOT** the full `GpuStorageRing` (no ring, no N-buffer rotation). Keeps the single persistent light SSBO (same `s_lightDataSsbo` handle, slot-20 `glBindBufferBase`, std430 `LightsData` layout / shader contract — unchanged) but eliminates the per-frame FULL `glBufferData` orphan re-spec (1.85 MB/frame, the `LIGHTSSBO-ORPHAN-1` NVIDIA no-stall workaround). **ON =** per frame upload ONLY the live used bytes via `glBufferSubData` (routed through `MC2_GL_BufferSubData`, NOT the owner `glBufferData` macro); the GL buffer **grows ONLY when used bytes exceed current capacity** — rare, sized with **+128-record headroom** matching the CPU backing grow step (`txmmgr.cpp addLightDataStructure`), draining via `glFinish` before delete/recreate/rebind-to-slot-20 and logged **once per grow** (`[LIGHTSSBO v1] event=growonce_grow`, requires `MC2_LIGHTSSBO_TRACE`). The split (`_UploadSplit`) path is subsumed into the same full-live-bytes SubData when ON (the prefix/suffix optimization is moot without an orphan). **OFF = the existing `LIGHTSSBO-ORPHAN-1` orphan path runs completely UNCHANGED, byte-identical GL stream + output** (one cached-bool branch; kill switch). With `MC2_GPUBUF_COUNTER=1`, the `[GPUBUF v1]` **`light` owner orphan bytes drop to ~0** steady-state (only rare grow frames emit a `glBufferData`). WARNING: **NVIDIA is a HARD BLOCKER before default-on:** `glBufferSubData` into the single live buffer that is read all-frame by every lit draw (and cross-phase by the mech/static-prop batchers) has a cross-frame in-flight-write hazard — on NVIDIA SubData-into-in-flight STALLS the CPU (exactly the stall the orphan dodged); AMD tolerates it. Default-OFF until NVIDIA is in the verification loop; the fix is the full ring (out of scope here), NOT an N-buffer rotation.


**REMOVED 2026-06-22 (DEAD-POST-FX-CLEANUP-1 — features deleted as wrong-for-RTS):** `MC2_HDR_POST`, `MC2_BLOOM`, `MC2_TONEMAP_ACES` (now inert; RendererFeatureRegistry entries retained index-only, annotated `[REMOVED]`). God rays + bloom hotkeys (RAlt+6 / RAlt+F1) and the `bloomThreshold`/`bloomIntensity` profile keys are also gone.

## Vulkan-prep seam (VULKAN-CONTRACT-MANIFEST-ARC)

Vulkan gates. Readers in `GameOS/gameos/vulkan_*.cpp`; `MC2_VULKAN_SPV_DIR` is read by every VK island loader.

- `MC2_VULKAN_SPV_DIR=<path>` — override the compiled-`.spv` directory the Vulkan islands and the present object load from. Default = `shaders/vulkan`.
- `MC2_VULKAN_VALIDATION=<preset>` — enable the `VK_LAYER_KHRONOS_validation` layer + debug-utils messenger in the Vulkan context and present object, selecting a validation **preset** (VULKAN-VALIDATION-PRESETS-1). Default **OFF**. A Vulkan smoke run with it set grades the layer's messages (`[VK_VALIDATION v1]`, error = red bucket `vk_validation_error`): see `docs/vk-validation.md`. Presets:
  - unset / `0` / `off` — validation OFF (unchanged legacy behavior).
  - `1` / `core` / present-but-empty — core validation only (backward-compatible with the historical bare `=1`).
  - `sync` — core + `VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`.
  - `gpu-assisted` — core + `VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT` (+ reserve-binding-slot). May need extra plumbing/extensions; fails soft if the layer can't honor it.
  - `best-practices` — core + `VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT`. May emit best-practice **warnings** (logged as `validation-warning:`, non-failing) — probe still passes.
  - `debug-printf` — core + `VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT`. May need extra plumbing; fails soft.
  Extra features chain into `VkInstanceCreateInfo.pNext` via `VkValidationFeaturesEXT`. Unknown value → falls back to `core` + warns. Resolved preset name is logged at startup. Only ERROR-severity validation messages fail a probe; WARNING-severity ones are visible but non-failing.
### The VK gate model (VK-P14 GATE-CONSOLIDATION-1)

The VK shared-GBuffer scene arc's gates are organized into four classes. **The default flip (done 2026-10-17, lane 2 session 51) toggled exactly ONE name: `MC2_VK_SHARED_GBUFFER`.**

- **Umbrella** — `MC2_VK_SHARED_GBUFFER` arms the whole shared-GBuffer scene path at once (see the entry below). Default ON (kill switch `=0`).
- **Members (folded, gate amnesty wave 1)** — the former per-member Tri rows (the scene regions and region details `MC2_VK_SHARED_SCENE_DEPTH`, `MC2_VK_PROP_COLOR_TIER1`, `MC2_VK_PROP_MATERIAL`, `MC2_VK_TERRAIN_TIER1`, `MC2_VK_TERRAIN_LIGHTING`, `MC2_VK_TERRAIN_DETAIL`, `MC2_VK_TERRAIN_DYNSHADOW`, `MC2_VK_MECH_TIER1`, `MC2_VK_MECH_MATERIAL`, `MC2_VK_MECH_LIGHTING`, the direct-chain gates `MC2_VK_PROP_DIRECT_CHAIN`, `MC2_VK_WATER_DIRECT_CHAIN`, `MC2_VK_POST_COMPOSITE_DIRECT_CHAIN`, and the complete-content gates `MC2_VK_HUD`, `MC2_VK_HUD_COLOR_PROOF`, `MC2_VK_HUD_SKIP_GL`, `MC2_VK_POST_COMPOSITE`, `MC2_VK_POST_FOG_NATIVE`, `MC2_VK_PRESENT_DIRECT_FBO`, `MC2_VK_PRESENT_TAIL_HOOK`, `MC2_VK_TERRAIN_ALL_LODS`, `MC2_VK_TERRAIN_SKIRTS`, `MC2_VK_TERRAIN_SKIRTS`, `MC2_VK_WATER_TIER1`, `MC2_VK_VFX`) no longer exist: their only non-umbrella value was the per-member `=0`/`=1` debug override, ruled expendable (2026-10-09 #3). Every island asks `mc2vkgate::umbrellaOn()` (`mclib/vk_umbrella_gate.cpp`, which supplies the session fail-soft veto); umbrella-ON MEANS the strict/direct-chain no-GL-interop path (each producer + post pass keeps the shared color/depth/normal chain Vulkan-owned and skips its GL↔VK semaphore handoff, the armed terrain Tier-1 island is the native first owner of shared scene color0/normal/depth with a CLEAR loadOp). Region preconditions still apply (terrain detail/dynshadow require terrain lighting; skirts require all LODs). `MC2_VK_TERRAIN_LODFADE` and `MC2_VK_OBJECTID_DIRECT_CHAIN` are explicit opt-ins (`=1`), not folded.
- **Safety gates (INDEPENDENT — not umbrella members)** — the verification-only gate (`MC2_VK_GPU_TIMESTAMPS`, default-OFF) and the structural island fail-soft (no env; always-on GL fallback). A verification/robustness gate stays orthogonal to the feature so it can be toggled during a soak without disarming the path. **Boot-level native-or-decline (VK-UMBRELLA-DIRECT-CHAIN-DEFAULT-1 item-2):** the terrain first-owner island now distinguishes a PERMANENT init failure (spv/pipeline/resource creation — declines the island for the whole session with ONE receipt line, GL owns terrain from frame 0) from a TRANSIENT readiness gate (VK context / shared GBuffer not born yet — keeps the item-9 fail-hard-with-retry). No per-frame re-init churn on a permanent failure.

**Byte-identical rule:** umbrella unset/≠`1` ⇒ the whole family OFF ⇒ pure GL, byte-identical to today (near-oracle `near_land_off` self-diff 0.000).

- `MC2_VK_SHARED_GBUFFER` — **default ON since 2026-10-17 (kill switch `=0`)** (VK-P14 GATE-CONSOLIDATION-1; was presence-gated `=1`, default OFF). **THE UMBRELLA** for the shared-GBuffer scene path. ON arms every island, the direct chain, the native post/composite, the present tail and the HUD at once (the former members are folded into this one name) — so umbrella-ON is the strict/direct-chain no-GL-interop path. **The default flip toggled exactly this one name.** Requires `MC2_RENDER_BACKEND=vulkan` (the shared engine `VulkanContext`), which is now the default. Does NOT arm the safety gates or the retired Tier-0 path. **`MC2_RENDER_BACKEND=gl` (or the umbrella forced `=0`) runs the GL path.**
- `MC2_VK_MECH_FEED_NATIVE` — **DEPRECATED / no-op (VK-RUNG8 item-9).** The VK mech island now **always** consumes the game's CPU byte sources (copy-once geometry from `s_vkVboNativeCopy` + `s_stagingIbo`; per-frame LightsData from `mcTextureManager->lightData_`); the GL `glGetBufferSubData` readback fallback was **deleted**. The env name is still parsed so old command lines don't hit unknown-var behavior — explicitly setting it just prints a `[VK_MECH_FEED] ... deprecated` note. Fail-hard replacement: if the CPU geometry source is transiently null (a normals rebuild raced the accessor) the island declines the frame and retries; if LightsData is unavailable the mech renders flat-lit that frame. No per-frame GL rescue.
- `MC2_VK_PRESENT_SKIP_GL_HANDOFF` — **DEPRECATED / no-op (VK-RUNG8 item-8).** The present interop tail (copy-1 GL blit + GL→VK semaphore handoff + the exportable present image + its GL view) was deleted; the present image is **unconditionally** a VK-only allocation, so the "skip GL handoff" and "no GL import" behaviors are now structural. The names are still parsed (explicit `=1` prints a one-shot `[VK_PRESENT] ... deprecated` note). **VK-RUNG8 item-10:** the stale requirement that BOTH be `=1` before `mc2VkSharedSceneDirectResourceEnabled()` (shared-scene VK-only arm) returned true was **removed** — it was dead resolver code that would wrongly decline the whole shared scene to pure GL under the umbrella unless a user set two retired gates. The shared scene now arms on the post/composite direct-chain stack alone (default-ON under the umbrella).
- **VK-RUNG8 items 1/2/3 (interop family deletion — Metal-gate opened):** `gl_interop_view.{cpp,h}` (GL-boundary zero-copy import adapter) was **deleted** whole (item 1); the **exportable IMAGE half** of `vk_image_resource.{cpp,h}` (`VkExternalMemoryImageCreateInfo` / `VkExportMemoryAllocateInfo` / `vkGetMemoryWin32HandleKHR` / `memWin32` / export-sanity queries) was **deleted** — only the NON-exportable VK-only core survives (item 2); and the `wantInterop` plumbing in `vulkan_context.cpp` (the `VK_KHR_external_memory[_win32]` / `VK_KHR_external_semaphore[_win32]` device+instance extension family) was **removed**, so `vkCreateDevice` on MoltenVK no longer fails on an unsupported extension (item 3 — **the one hard Metal-gate blocker**). `ensureEngineVulkanContext()` was renamed `ensureEngineVulkanContext()`. Device extensions now enabled = **surface + swapchain only** (`VK_KHR_surface` + `VK_KHR_win32_surface` + `VK_KHR_swapchain`, all Metal-portable via a platform branch). `MC2_VK_NO_GL_INTEROP_STRICT` survives as a live guard on the terrain per-frame GL depth-readback fallback; `MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT` is now **permanently satisfied by construction** (no exportable resource can be created; no code reader remains) and stays as a checker provenance receipt.
- `MC2_VK_POST_SCREENFX` - **kill switch, ON under the VK umbrella since 2026-10-15** (rung 8.3, VK full-ownership ladder; `=0` turns the VK passes off; with the umbrella off the GL passes run untouched). Renders the **cloud-shadow + shoreline-foam** postprocess passes natively on the shared engine VulkanContext via `vulkan_post_screenfx_native.cpp`: cloud-shadow samples shared scene depth and multiplies into shared scene color0 in place; shoreline samples shared scene normal and brightens foam in place. Zero copies, zero CPU readbacks, no private VkDevice. Per-pass bisect sub-gates `MC2_VK_POST_SCREENFX_CLOUD` / `MC2_VK_POST_SCREENFX_SHORELINE` can force individual passes back to GL. The **screen-shadow** pass (`MC2_VK_POST_SCREENFX_SHADOW`) is part of this gate: it samples the cascades the VK shadow island produces and shades the object pixels (mech bodies in the shadows of the casters). Receipts: `vk_post_screenfx_native` debug-state section (`postscreenfx_used_vulkan`, `cloud_frames`, `shoreline_frames`). **`=0`: the VK passes are off.**
- `MC2_VK_GPU_TIMESTAMPS=1` — **verification-only diagnostic, default OFF** (NOT an umbrella member). Adds `vkCmdWriteTimestamp` GPU timestamp brackets (3 stamps: pass-start / draws-start / pass-end) around the **terrain** and **mech** island render passes so the coarse CPU `draw_us` (which spans record + submit + `vkWaitForFences`) can be split into real **GPU pass execution time** vs **CPU submit/fence-wait**, and within the GPU pass into **setup/clear/load** vs **draws**. VK 1.1 core, **no device feature required** (only the queue-family `timestampValidBits` + `limits.timestampPeriod` gate it — MoltenVK-clean); no pipeline-statistics query (that would need a device feature the shared engine context does not enable). Header-only helper `GameOS/gameos/vk_gpu_timestamps.h`. Emits a `[VK_TERRAIN_ISLAND]`/`[VK_MECH_TIER1] GPU_TIMESTAMPS samples=… gpu_pass_us=… gpu_setup_us=… gpu_draw_us=… (cpu_draw_us=… incl. record+submit+fence-wait)` line at island teardown (EMA over the run). **Gate OFF (or timestamps unsupported) = zero query commands recorded, byte-identical render path.** **VK-GPU-ZONES:** the same gate now also arms `GameOS/gameos/vk_gpu_profile.cpp`, which brackets EVERY render pass and compute dispatch group (shadow static/dyn, terrain, overlay, mech, props, legacy, water, sky, vfx, post fog/shadow-resolve/cloud-shore/HZB build+cull, actor cull, terrain cull compute, present ImGui/HUD/composite) and logs `[VK_GPU_PASSES v1] frame=N n=120 span_ms= sum_ms= unattributed_ms= | <VKIsl.Pass>=ms(xcalls)...` every 120 frames (span = first stamp to last stamp of the frame, sum = the scopes added, unattributed = the gap). The same scopes feed Tracy GPU zones (one Tracy Vulkan context on the graphics queue, collected once per frame; recorded only while a profiler is connected) and VK_EXT_debug_utils command-buffer labels (instance extension now enabled whenever the loader offers it, so RGP/RenderDoc/Nsight show named passes and the existing object names).
- `MC2_VK_SHADOW_INTEROP` — **kill switch, ON under the VK umbrella since 2026-10-15** (`=0` turns every VK shadow producer off; `MC2_VK_CSM_ARRAY_INTEROP` / `MC2_VK_CSM_FULLMAP_INTEROP` are the same kind of switch for the cascade array and the full-map cascade). Since the same date the island casts the mechs too (`vulkan_shadow_mech_casters.cpp`), records the frame's passes into one command buffer and submits it once with no CPU wait (`mc2_vk_shadow_island_submit_frame`), and the single dynamic map is recorded only when a frame has no cascades; the historical text below describes the first GL-consuming version (VK-P8a). The **second Vulkan GEOMETRY region** and the **first VK-PRODUCES-DEPTH region** (water was GL-produces/VK-consumes; shadow reverses it): renders the **dynamic static-prop shadow-caster depth** in Vulkan (`GameOS/gameos/vulkan_shadow_island_live.cpp`) into an **exportable D32** `VkImageResource`. GL binds that shared D32 in place of `gosPostProcess::getDynamicShadowTexture()` and samples it via the **UNCHANGED** hardware `sampler2DShadow` `calcShadow()` path (`shaders/include/shadow.hglsl`) — lighting up the **vk2gl back-channel** (`glVkToGlSem` / `glInteropWaitFromVk`) for the first time on a geometry region: the VK submit **SIGNALS** vk2gl, GL **WAITS** it, then samples (the hardware-compare-over-imported-D32 path proven by the since-deleted `tools/vk_shadow_sampler_probe`; recover from tag `archive/dead-infra-2026-10-09`). **Feed** (VK-P8a STEP 0, `.claude/VK-P8a-STEP0-CASTER-FEED-NOTE.md`): the CPU `GpuStaticPropInstance` vector (`dynShadowSet`, 112B each) the GL path already builds each frame is **memcpy'd** into a VK instance SSBO; the shared static-prop VBO/IBO are copied **ONCE per map** into VK-owned buffers (`mc2_shadow_island_get_geometry_feed`). **Pipeline**: depth-only, **LESS forward-Z** (NOT reverse-Z — matches the GL dynamic shadow map, `glClearDepth(1.0)`), cull **None**, `depthBias` set per-frame to `glPolygonOffset(shadowBiasFactor_, shadowBiasUnits_)` **EXACTLY** (the parity **STOP-watch** — bias divergence = acne/peter-panning). Reproduces `shadow_static_prop.vert` (former GL source, removed in s63) math bit-for-bit (`pos*M` row-vector → Stuff→MC2 swizzle → `lightSpaceMatrix*`). **BORROWS** the shared engine `VulkanContext` (`ensureEngineVulkanContext`) — no second device, no `volkLoadDevice`; **REQUIRES `MC2_RENDER_BACKEND=vulkan`** (**fail-soft to the GL shadow path** when the context is absent/invalid). Depth surface zero-copy; **forbidden DEPTH-readback counters hard-asserted ==0**. Surfaced in the `[VK_SHADOW_INTEROP]` health line + `vulkan_shadow_island` dump section: `interop_active`, `vk2gl_sem_active`, `used_interop`, `inst_records`, `type_runs`, `forbidden_*` (==0). Golden-parity oracle = interop-ON vs the GL shadow baseline on a shadow-visible deterministic bookmark (`MC2_SMOKE_FIXED_TIMESTEP=1`); the VK-produced shadow must match byte-exact or within a documented depth-precision floor (water ±1-LSB / SSAO `highangle_wide` precedent). **P8a scope** = opaque static props, single map (foliage-alpha + tessellated-terrain casters = P8b, bone-skinned mech casters = P12). **Gate OFF = byte-identical GL shadow path.**
- `MC2_VK_TERRAIN_LODFADE=1` — **presence-gated `=1`, default OFF; REQUIRES `MC2_VK_TERRAIN_DETAIL=1`.** Ports GL's camera-distance `lodNear` fade (`gos_terrain.frag:322-328,616-736`: `LOD_NEAR=4000`/`LOD_NEAR_FADE=4500` smoothstep) into the VK terrain frag, closing the VK-LOD0-vs-GL-band color seam at the fade boundary. Feeds `gos_GetTerrainCameraPos()` (Stuff/MLR space) into the existing UBO pad tail each frame (no slot growth — `TerrainFrameUBO` stays 512B); the frag recomputes `camDist`/`lodNear` verbatim and multiplies the POM offset **and** the whole detail-normal contribution (`dN * lodNear`) by it, matching GL's `pomOffset *= lodNear` / `detailMul` exactly. FAIL-SOFT: gate off → `lodNear` stays `1.0` always. **Gate OFF = byte-identical to the pre-fade VK-TERRAIN-DETAIL frame.**
- `MC2_VK_TERRAIN_CSM_DEBUG=N` — **debug viz, default 0 (off).** One-shot terrain-frag debug view (1 normalLight, 2 sun dir, 3 static shadow, 4 dynamic shadow, 5 combined shadow, 6 albedo, 7 sun N.L, 8 normal) that bypasses the composite; CSM-BLACKOUT-RECON-1. Read in `vulkan_terrain_island.cpp` `fill_frame_ubo` (shared by the terrain and VK-PARITY-OVERLAY-1 overlay passes). **Unset = byte-identical.**
  - **VK-PARITY-F3 (2026-10-08): now genuinely explicit-only even under `MC2_VK_SHARED_GBUFFER=1`** (the umbrella used to arm it via `subGate`, contradicting this text). The GL chunk terrain (the shipped renderer) has no distance detail fade, so the armed fade zeroed VK detail-normal relief beyond ~4000wu and left bare ground ~15% darker than GL at night/RTS zoom. Reader: `gos_terrain_lod_chunk.cpp` `lodFadeOn = gate::get<"MC2_VK_TERRAIN_LODFADE">()` (an Exact1 row).
- `MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN=<float>` — **default `0.78`**; multiplies detected-snow terrain albedo (`snowBrightnessDampen` in `terrain_lod_chunk.frag`). Single reader `gos_TerrainSnowBrightnessDampen()` in `gos_terrain_lod_chunk.cpp`, shared by the GL uniform upload and the VK terrain feed (VK-PARITY-F3). Pre-existing knob; listed here because the reader moved.
- `MC2_VK_TERRAIN_CSM_FULLMAP_EXPERIMENTAL=1` - **parked escape hatch, default OFF.** The OLD "biggest-safe-subset" (feeds only the coarse full-map cascade through the legacy binding-7 single-map path). Confirmed NET-NEGATIVE at normal play distance (camera-swimming blob wash, `a50c6033`) - kept only as a fallback if the near-cascade-array path (`MC2_VK_TERRAIN_CSM` above) regresses. Requires `MC2_VK_TERRAIN_CSM=1` AND this flag; does not feed `csmEnabled` (writes the legacy `dynShadowsEnabled` path instead), so the two paths cannot both be armed - `csmEnabled` takes priority in the frag when both somehow race (mutually exclusive C++ population).
- **VK-PARITY-F1 legacy-shape island (no env var of its own).** `GameOS/gameos/vulkan_legacy_shape_island.cpp` owns the legacy `mcTextureManager::renderLists` CPU-lit `gos_VERTEX` loops (ground vehicles, blob shadows, non-terrain alpha nodes, additive effect/headlight-cone nodes) under the VK umbrella; it is armed exactly when the mech island is (`MC2_VK_MECH_TIER1` + `MC2_VK_SHARED_SCENE_DEPTH`). Gate off = those loops draw through GL byte-identically. Without it vehicles are invisible under VK.

## Top-level render backend selector (VK-P13-SELECTOR-1)

- `MC2_RENDER_BACKEND` (legacy; read only to report an ignored value) � **the top-level GL-vs-Vulkan backend selector**, default `vulkan` since 2026-10-17 (unset, empty and unrecognised values all mean Vulkan; `gl`/`GL` is ignored: the opt-out was removed, a `[RENDER_BACKEND]` line says so; the MFC editor is Vulkan too and keeps only a private GL context for the host window). With Vulkan, the engine creates the shared `mc2vk::VulkanContext` used by native geometry, native postprocess, and present-tail paths; the retired private-device postprocess islands are no longer part of the dispatch model. A failed context creation is fatal (see "VK init failure policy"). Device-birth proof surfaces in `[VK_ENGINE_CONTEXT]`.
- `MC2_VULKAN_VALIDATION=<preset>` (also documented under the probe seam above) selects the validation preset for the engine `VulkanContext` (`sync` = core + synchronization-validation, used for the `=vulkan` device-birth proof).

### VK present-pixel capture oracle (VK-FIELD-ORACLE-1 / VFX-5/8, all Trace-kind, default OFF)

The OS/PrintWindow grab of a VK swapchain returns only the clear colour, and the GL scene-FBO screenshot is empty under shared-GBuffer — so these are the ONLY true-backbuffer capture path under `MC2_VK_SHARED_GBUFFER`. All require `MC2_VK_PRESENT_VERIFY=1` (the light-cadence readback tick).

- `MC2_VK_PRESENT_DUMP_PPM=<path>` — dumps the FULL composited present image (`sharedImg`, the RGBA8 backbuffer VK blits to the swapchain) to a PPM (P6) on each verify tick. Headless, non-disruptive. `EnginePresentVk::verifyReadbackContent`.
- `MC2_VK_PRESENT_DUMP_FRAME=N` — static frame pin for the PPM dump: capture once at the first verify tick with `g_mc2FrameCounter >= N`. gameosmain extends the verify-tick window to `[N, N+600)` so a pin past the initial 600-frame verify stretch still fires (an unpinnable pin silently reported "PPM never wrote" on a HEALTHY present path — the 2026-07-12 grey-regression false clue; fixed `8c56eafb`).
- `MC2_VK_PRESENT_DUMP_ON_FIRE=1` — DYNAMIC fire-event pin (VFX-5). With `MC2_FX_FORCE_SPAWN=loop` the fixture's FIRST volley arms the PPM capture ~0.5s (30 frames) after itself (`g_mc2FxFireCaptureFrame`), so the capture always lands mid-volley — no static-pin roulette against a mission's intro/scenario clock. Ignored when `MC2_VK_PRESENT_DUMP_FRAME` is set (static pin wins).
- `MC2_VISUAL_CAPTURE_ON_FIRE=1` — GL twin of the above (VFX-8). The engine visual capture (`MC2_VISUAL_CAPTURE_FRAME/_DIR`, smoke mode) triggers at the same fixture-armed fire frame instead of the static pin, so GL-vs-VK weapon-class pairs capture the same fire-relative instant on both backends.
- **Capture recipe gotchas:** `MC2_LOG=1` or all game stdout/stderr goes to NUL; PPM dump path must be ABSOLUTE (mission load chdirs); never combine `MC2_SMOKE_MODE` with the PPM oracle (VK present never runs under it, and standalone smoke mode needs `--mission` double-dash). For the full HUD headless, `MC2_SCREENSHOT_PATH` (Vulkan backend) reads back the presented frame, HUD included.

### VK init failure policy (owner ruling 2026-10-16; supersedes VK-FAILSOFT-DISARM-1/2)

There is no GL fallback. With the Vulkan backend (the default), a Vulkan context or device that cannot come up, a build without `MC2_VULKAN`, a present path that is not VK-owned, or a present init that keeps failing on a visible drawable for ~60 s calls `mc2::fatalBackendFailure`: it logs `[RENDER_BACKEND] FATAL: ...`, shows a message box (skipped for automated runs: `MC2_SMOKE_MODE`, `MC2_NO_LAUNCHER`, `MC2_SMOKE_SEED`, `MC2_HEARTBEAT`, `MC2_CRASH_FAST_EXIT`) and exits with code 3. `MC2_VK_FORCE_FAIL=1` exercises it. `mc2vkgate::umbrellaOn()` is a read of `MC2_VK_SHARED_GBUFFER` that is false under `MC2_RENDER_BACKEND=gl`.
Present-level transient failures (zero surface extent while minimized, `NATIVE_WINDOW_IN_USE`) still DEFER present init and retry from the frame loop; the session stays VK. Mid-run present failure tears the present stack down and re-enters the deferred-retry path. While deferred the frame loop does NOT GL-swap (a GL SwapBuffers on the shared HWND holds the WSI binding and starves the VK swapchain retry).

<!-- MERGE-WAVE-1 2026-07-22: MC2_VULKAN_EDGE_FOG_ISLAND / MC2_VULKAN_SWAPCHAIN_PRESENT docs removed
     (old interop islands retired by the VK branch); MC2_HUD_IMGUI_PAUSE / MC2_HUD_IMGUI_INFOWINDOW
     removed (superseded by ui-phase1 MC2_UI_IMGUI_PAUSE / MC2_UI_IMGUI_INFO). -->
## In-mission HUD modernization (IN-MISSION-HUD-IMGUI-SCOPE-1)

- `MC2_HUD_READABILITY_V1=1` — slice A, data-first HUD readability. Substitutes the next-size-up retail D3F font atlas at the single font-resolve choke point (`gui/afont.cpp` `aFont::loadFont`) for the worst-offender in-mission HUD fonts (help / objectives / info-window name+components / vehicle-tab costs / pause window / hotkey overlay), and brightens the disabled vehicle-tab label color (`code/controlgui.cpp` `renderVehicleTab`, stock `0xff7f7f7f`). Text code always renders at scale 1.0 — size lives in the atlas — so no layout/hit-test change. Default **OFF** = byte-identical.
- `MC2_HUD_IMGUI_TABS=1` — slice C, data-driven command-bar tab registry (`code/hudtabregistry.{h,cpp}` + wiring in `code/controlgui.cpp`). Replaces the hardcoded 3-tab model (enum-consecutive `TACMAP_TAB..VEHICLE_TAB` + hand-written cycle table) with an ordered registry: render/update dispatch, help text, isOverTacMap, click mutual-exclusion, and TAB-hotkey cycle all route through it; the stock 3 tabs keep their legacy ControlButton chrome and PRESSED-bit sync. Presence-gated; pin-to-legacy shared with the slice-B helper. Default **OFF** = every legacy branch runs unchanged (registry registered but never consulted).
- `MC2_HUD_TABS_TEST=1` — slice C witness tab (requires `MC2_HUD_IMGUI_TABS=1` to be visible). Registers a 4th "test" tab with no legacy button: ImGui-drawn tab-strip chrome (bottom-band shrink + canvas transform via `HudImGui::hudShrinkPoint`/`hudToDisplay`) and a trivial ImGui body (build stamp + frame counter) over the tac-area rect. The modder's worked example — left in deliberately, double-gated. Default **OFF**.
- `MC2_HUD_SIM_SPEED=1` — SIM-SPEED-BUTTON-1 (requires `MC2_HUD_IMGUI_TABS=1` to be visible). Registers a "speed" tab (no legacy button, ImGui chrome + body like the test tab) with 1x/2x/4x buttons driving the runtime sim-speed multiplier behind `MC2_SIM_SPEED` (`simSpeedSetMultiplier`, `code/mission.cpp` — the SAME mechanism, no second path). Guards: buttons disabled in multiplayer; multiplier resets to the env seed at every `Mission::init`; pause freezes `scenarioTime` regardless. Clicks log `[SIM-SPEED-BUTTON v1]` + `[SIM-SPEED v1]` to stderr; dev-shell `sim_speed [--value V]` drives the same setter. Default **OFF**.

## Frame-graph executor + backend-iface seam (RENDER-FRAME-GRAPH ARC)

- `MC2_FRAMEGRAPH_EXECUTOR=1` — top-level frame-graph pass executor (`gos_postprocess.cpp` `executorEnabled()`, `mclib/render_contract.cpp`). Default **OFF**. Value must be `'1'`. Substrate — the reorderer below is NOT shipped; executor stays default-OFF.
- `MC2_FRAMEGRAPH_REORDER_SPMECH` — **REJECTED experiment, default OFF, presence-gated** (`mclib/vk_scene_renderer_passes.cpp`). Would swap the StaticProp-flush ↔ Mech-flush order (the one legal adjacent swap the reorder oracle proved). PARITY-FAILED on depth-EQUAL ties (mc2_24 overlapping opaque fragments resolve order-dependently); candidate rejected, kept only as an A/B lever. Never set in production. `=0`/unset = today's order verbatim (byte-identical).
- `MC2_FRAMEGRAPH_DRYRUN=1` — frame-graph dry-run mode (`render_contract.cpp` `dryrunEnabled()`). Default **OFF**. Value must be `'1'`.
- `MC2_FRAMEGRAPH_AMBIENT_GUARD` — ambient-handoff contract guard (colorMask / depthFunc / depthWrite axes; `render_contract.cpp`). **DEFAULT ON**; only `=0` disables. The dump counter is read by `get_render_health` / `validate_state` (mc2-render-state MCP); the standalone check-ambient-guard.py was retired 2026-10-12, cull wave 1.
- `MC2_FRAMEGRAPH_AMBIENT_FATAL=1` — hard-`abort()` on an ambient-guard mismatch (CI/bisection teeth). Default **OFF**, presence-gated. Legacy alias `MC2_AMBIENT_ASSERT_FATAL` also honored.

- `MC2_HZB_BUILD` - default ON. Same-frame native Vulkan depth pyramid and render-only static-prop occlusion. Exact MIN footprints, per-object LOD; `=0` keeps every color instance. The GL reducer/probe and separate native opt-in gate are retired. See `docs/hzb-visibility-mvp.md`.
- `MC2_VK_PRESENT_SKIP_GL_HANDOFF=1` - rung-8.7 present handoff proof gate. Default-OFF. With `MC2_RENDER_BACKEND=vulkan`, `MC2_VK_PRESENT_DIRECT_FBO=1`, and `MC2_VK_POST_COMPOSITE=1`, `EnginePresentVk::presentGLFrame()` can consume a present image already written by Vulkan without issuing or waiting a GL->VK interop semaphore. Proof field: `[VK_PRESENT] gl_handoff_skip_frames=N`. This is intentionally explicit because any remaining GL writer to the shared present image still requires the normal handoff.

## SPFLUSH cost-split decomposition (SPFLUSH-COST-SPLIT-1)


## Mission interface cost-split (MISSION-INTERFACE-PERF-1)

- `MC2_IFACE_COST_SPLIT=1` — chrono cost-split of `MissionInterfaceManager::update()` (the `GameLogic.Mission.Interface` Tracy zone). Default **OFF**, presence-gated. Emits one `[IFACE_PERF v1] event=window` stdout line every 900 frames + `event=mission_end` partial flush at mission teardown, with per-phase `{avg_us,max_us}`: `invProj`/`LOS`/`controlGui`/`updateTarget`/`postTarget`/`drawBars`/`rollovers` plus ControlGui sub-phases (`cg.pauseWnd`, `cg.btnHover`, `cg.rosterScan`, `cg.moverState`, `cg.tacMap`, `cg.infoWnd`, `cg.vehicleTab`, `cg.fgBar`) and `invProj_walks`/`invProj_cacheHits`. Superset of the older `MC2_MIF_SPLIT` (either env enables both; `MC2_MIF_SPLIT` keeps its shutdown-only `[MIF_SPLIT v1]` line). `code/missiongui.cpp` + `code/controlgui.cpp`. Zero behavior change.

## TRACKV CPU perf kill-switches (2026-06-03)

- `MC2_STATIC_PROP_SNAPSHOT_FILL_DIRTYONLY` — default **ON** (flipped after Tracy proof). STATICPROP-SNAPSHOT-FILL-DIRTYONLY-1 gate.
  - unset/1: on clean `registryGeneration` AND `cullRecordVersion` (vs cached), skip Fill + WriteLoop and `memcpy` the cached rows into the snapshot arena (`Extract.SP.CleanCopy` zone); per-prop diagnostic counters restored from cache. Dirty frames run the legacy path and refresh the cache. First ~10 frames after mission load rebuild while cullVer settles — win is steady-state. **Tracy (user, mc2-win64-water):** `ExtractRenderSnapshot` median 1.68ms→36.7µs (−97%); `Extract.SP.Fill` 1.15ms + `WriteLoop` 159µs → gone, replaced by `CleanCopy` ~15µs.
  - 0: legacy full rebuild every frame (`fillStaticPropSlots` + per-prop WriteLoop) — kill-switch.
- `MC2_THIN_CANARY=1` — re-enable Probe-6 thin-record canary readback (per-frame ~80KB `glGetBufferSubData` ×2 + CPU recipeIdx/flags compare). Default **OFF**. Diagnostic only; gated 2026-06-03 (was ungated per-frame stall).

## Shadow tuning

- **options.cfg `ShadowBoundedNearFit`** (this was the env gate `MC2_SHADOW_BOUNDED_NEAR_FIT`, deleted 2026-10-12: the value is now the options.cfg key `ShadowBoundedNearFit`, read once at startup; `=0`/`=1` in the text below means `FALSE`/`TRUE` (or the number); `MC2_OPTIONS_OVERRIDE="ShadowBoundedNearFit=<value>"` sets it for one run without editing the file) — cap the dynamic sun-shadow fit radius (`MC2_SHADOW_BOUNDED_NEAR_RADIUS`, default 2500wu) for crisp near shadows. Default **ON** (opt out with `=0`). **Corrected 2026-07-30 (GATE-DEFAULT-TRUTH-1): this line said "Default OFF" and was wrong**, as did the registry entry and the reader's own adjacent comment — while `gos_postprocess.cpp:6749-6752` does `return !(v && v[0] == '0')`. Every default launch caps sun-shadow coverage to a 2500wu camera-centered radius. Whether that is the *desired* default is a separate visual question needing a capture pass; this is only the documentation catching up to the binary.
- `MC2_SHADOW_BOUNDED_NEAR_RADIUS=2500` — bounded near-fit radius in wu. Default 2500.
- `MC2_SHADOW_FRUSTUM_DIAG=1` — per-frame coverage probe. Read-only.
- `MC2_STATIC_PROP_BUILDING_SHADOW_REBAKE_TEST_DESTROY_AT_FRAME=<N>` — deterministic repro hook (test-only): at frame N, inflict lethal damage on the first live building via the real `Building::setDamage` API to drive the rebake trigger above for before/after capture proofs. Unset = no-op.
- `MC2_SHADOW_DYNAMIC_PROP_CASTERS` — feed dynamic caster pass from registry. Default **ON**. Kill=`=0`.
- `MC2_SHADOW_ROBUST_BASIS` — light-basis singularity guard + AABB corner-scarcity fallback (SHADOW-ROBUST-BASIS-1). Default **ON**. `=0` restores legacy (byte-identical for normal suns; can go singular).
- `MC2_SHADOW_STATIC_BANDFIX` — STATIC-SHADOW-BANDING-1: raises the static-shadow LOD tap floor in `gos_terrain.frag` (16/8/4 → 16/12/8) so shadow.hglsl's gradient-adaptive PCF doesn't undersample its Poisson disk at the wide radius used on flat terrain (root cause of the reported grass/rolling-terrain shadow banding — NOT a map-resolution or depth-precision issue). Default **ON** (2026-07-18, field bug: shadow banding persisted in shipped default config). Kill=`=0` (byte-identical legacy).
- `MC2_SHADOW_STATIC_BANDFIX_V2` — STATIC-SHADOW-BANDING-2: round-1 (taps) refuted as sufficient by field check (taps=16 still banded on grass/dirt slopes). Mechanism A measured: static map world-per-texel (~4.6 wu on mc2_01, 4096² ortho) / tan(slope) predicts the observed ~25–90 wu diagonal contour bands on 3–10° slopes. v2 feeds `calcShadow`/`calcDynamicShadow` bias the real height-derived slope normal (`computeTerrainNormalFromHeight`) instead of a fake flat-up normal — slope-scaled bias finally tracks actual tilt. Lighting normal untouched. Pair with v1. Default **ON** (2026-07-18, field bug: banding in/around mech shadows on terrain; A/B verified mc2_01 f1800, high-pass stripe amplitude 3.67→1.12). Kill=`=0` (byte-identical legacy).

### Dynamic CSM (reworked 2026-06-16 `8d36b37d`/`b8f764b6`/`13e96cc9`/`ead760df`)

- **options.cfg `ShadowCSM`** (this was the env gate `MC2_SHADOW_CSM`, deleted 2026-10-12: the value is now the options.cfg key `ShadowCSM`, read once at startup; `=0`/`=1` in the text below means `FALSE`/`TRUE` (or the number); `MC2_OPTIONS_OVERRIDE="ShadowCSM=<value>"` sets it for one run without editing the file) — master cascaded-shadow-map toggle. Default **ON** (2026-06-18, `8ff13a36`); opt out with `=0`.
- `MC2_SHADOW_MAP_SIZE` — per-cascade shadow texture size. Default **8192**. 3×8192² ≈ 805 MB VRAM; `=4096` ≈ 201 MB (still sharp at R0=512).
- `MC2_SHADOW_CSM_R0` — near-cascade fit radius (WU). Default **512** (→ 0.25 WU/texel at 4096).
- `MC2_SHADOW_CSM_R1` — mid-cascade fit radius (WU). Default **4096**.
- `MC2_SHADOW_CSM_COUNT` — cascade count. Default **3** (R0 near / R1 mid / full-map far).
- `MC2_SHADOW_CSM_SOFTNESS` — PCF softness. Default **0.9**.
- `MC2_SHADOW_OBJ_NORMAL_BIAS` — object self-shadow normal-offset bias (kills residual acne). Default **2.0**.
- `MC2_SHADOW_MECH_SOFT` — mech self-shadow softness (wider PCF penumbra + terminator smoothstep so flat low-poly facets fade instead of flip + raised floor). Default **1.0**, clamp [0,4]. Object self-shadow is now per-type via GBuffer1.a mask: terrain skip / static-prop NO self-shadow / mech soft (`3253d582`).
- `MC2_SHADOW_GUARD_CLAMPED_SUN` — SHADOW-SUNCLAMP-1: makes the `shadow_screen.frag` back-face guard (`if (NdotL < 0.05) return 1.0;`) dot against the **same altitude-clamped sun the shadow matrices are built from**. `gameos_graphics.cpp:10197` feeds `-gos_GetTerrainLightDir()` into `buildStaticLightMatrix`, which clamps it via `mc2ClampShadowSunAltitude` (15° minimum); `runScreenShadow` previously uploaded the byte-identical expression **unclamped**. On a mission authoring a near-horizontal sun the guard therefore became a purely *azimuthal* test and discarded shadows the shadow map genuinely contains, splitting the world into a shadowed and an unshadowed half — the mc2_10 night overview asymmetry. Default **ON**; `=0` reverts to the legacy raw-sun guard. Byte-identical wherever `sin(LightDirPitch) >= sin(15°) = 0.2588`, so **only mc2_10** (`LightDirPitch=180` → `0.0`) changes; mc2_01 (50°), mc2_03 (46°), mc2_24 (75°) are untouched. Scene brightness is unaffected — the shading light dir / terrain N·L is a separate uniform and is deliberately not touched.

- `MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN` — TERRAIN-SHADOW-GUARD-1: the **terrain-side twin** of `MC2_SHADOW_GUARD_CLAMPED_SUN`, and the one that actually moves world pixels. `shaders/include/shadow.hglsl`'s back-face guard (`if (NdotL < 0.05) return 1.0;`) and its SHADOW-ALT-SCALE-1 `sunAltSin = clamp(lightDir.z, 0, 1)` consumed the **raw authored** `terrainLightDir`, while the shadow matrices were built from a sun clamped to `MC2_SHADOW_MIN_SUN_ALT` (15° default). Under `MC2_SHADOW_STATIC_BANDFIX_V2` (default ON) `shadow.hglsl` resets `normal = vec3(0,0,1)` immediately before the guard, so on terrain the test reduces to `terrainLightDir.z < 0.05`. mc2_10 authors `LightDirPitch = 180` → `sin(180°) = 0` → **every** terrain / overlay / decal / LOD-chunk pixel early-outs fully lit while props and mechs keep their shadows via the unguarded static path — which is exactly why the mech-pixel-only `MC2_SHADOW_GUARD_CLAMPED_SUN` fix captured byte-identical. Implemented as a **compile-time shader define** (`MC2_SHADOW_MIN_SUN_SIN`) emitted by `mc2TerrainShadowSunDefine()` at every terrain-family program prefix (main terrain variations, thin, surface, mask-solid, overlay + decal, LOD-chunk base + tess); `shadow.hglsl` defaults it to `0.0`, so programs that do not inject it (props, mechs) compile byte-identical. Default **ON**; `=0` emits no define and reverts to the raw-sun guard. Byte-identical wherever `sin(LightDirPitch) >= sin(15°) = 0.2588`; mc2_01 (50°), mc2_03 (46°), mc2_24 (75°) are untouched. **CORRECTED 2026-07-30 (LIGHTING-INCAPABLE-BATCH-3): this sentence used to end "only **mc2_10** changes", which contradicts the rule stated in the same sentence and materially understates the blast radius of a DEFAULT-ON gate.** Applying the rule to the authored pitches gives **five** changed missions, not one: mc2_10 (180° → `z = 0`), **mc2_19** (0° → `z = 0`), **mc2_23** (9° → `z = 0.155`), and **mc2_05 / mc2_21** (270° → `z = −0.707`, below the horizon, so the raw guard returned fully-lit there too). **Two different mechanisms, and conflating them is how the original claim went wrong (refined 2026-07-30 by TREE-CONSOLIDATE-1):** on **mc2_10 / mc2_19 / mc2_05 / mc2_21** the guard itself early-outed — `MC2_SHADOW_STATIC_BANDFIX_V2` pins `normal` to `+Z`, so `NdotL = max(z, 0) = 0 < 0.05` — and terrain had **no shadow term at all**; those four go from fully-lit to actually sampling the shadow map, which is where this gate does its most work. **mc2_23 is not in that group:** its `z = 0.155` is already ≥ `0.05`, so its guard never early-outed. It changes only because the clamp *also* feeds SHADOW-ALT-SCALE-1's `sunAltSin` (`0.155 → 0.259`, moving `shadowFadeGains`' `smoothstep(0.21, 0.57)` off its lower edge) and the `NdotL`-derived depth bias. Either way the gate is emphatically *not* a no-op on the night maps. Note also that the threshold in **authored-pitch** terms is **15.54°**, not 15°, because `mclib/camera.cpp:2324-2329` builds `lightDirection` with unit-length `xy` and normalizes *after*, so `z = sin(p)/√(1+sin²p)`; nothing shipping sits in that gap (next-lowest is mc2_14 at 20°, `z = 0.324`). Note the direction: on mc2_05/mc2_21 it makes terrain take shadows from an artificial 15° sun on a mission whose authored sun points straight down, which is a change worth grading rather than assuming. Scene brightness is unaffected: the shading light dir (`gos_terrain.frag:820` diffuse N·L) is deliberately not routed through the clamp.- `MC2_SHADOW_MIN_SUN_ALT` — SHADOW-SUN-ALT-CLAMP-1 tunable: minimum **effective sun altitude in degrees** used by the shadow projection matrices (`mc2ClampShadowSunAltitude`, `gos_postprocess.cpp`). Default **15**; `0` or negative disables the clamp. Azimuth is preserved. Applied at both the static full-map matrix and the dynamic cascade matrices. Missions authoring the sun at or below the horizon (mc2_10 `LightDirPitch=180` → `lightDirection.z = 0`) otherwise smear absurdly elongated caster silhouettes across up-facing slopes (the "black streak fans"). Suns already above the minimum are byte-identical. Since SHADOW-SUNCLAMP-1 this clamp also governs the screen-shadow back-face guard. Resolved once at process start.
- `MC2_VISUAL_TUNING_FILE` — path to an alternate visual-tuning JSON, consumed by the profile reader (`visual_tuning_profile.cpp:301`). Overrides the built-in profile values for every registered tuning key (`aoRadius`, `aoStrength`, `aoBias`, and the rest), which makes it the **zero-rebuild grading path** for look tuning: set the file, relaunch, confirm the `[VisualTuning] applied N keys` line. Not a renderer gate — a tuning-input path; unset = built-in defaults. Documented and smoke-allowlisted by SSAO-BLUR-1 after it was found to have a live reader but **no registry entry, no docs entry, and no `run_smoke.py` passthrough** — meaning any smoke-driven tuning sweep silently graded the built-in defaults instead of the supplied file.
- `MC2_LIGHT_POS_COLUMN_FIX` — **DEFAULT-ON as of VEHICLE-LIGHTING-PATH-1** (`=0` reverts). Its prior "1/255, sub-perceptual" measurement **no longer characterises this gate**: that was taken at sun-dominated fixture poses, where the only meaningful contributor is the INFINITE sun — which has no position term and is structurally immune to this bug. It measured the one condition under which the fix cannot matter. It is load-bearing now that vehicles carry a populated light row, since vehicles are ground-level movers sitting close to exactly the classes this breaks (POINT explosions/bolts, SPOT turret lamps and guard-tower cones). Needs re-measurement at vehicle-adjacent night poses. LIGHT-POS-COLUMN-1: delivers the object-path light's **world position** to the GPU. `Stuff::AffineMatrix4D` is 3 columns × 4 rows indexed `entries[(column<<2)+row]` (`mclib/stuff/affinematrix.hpp:141`), so `BuildTranslation` puts the world translation at `entries[3]/[7]/[11]`, and `BuildRotation` only rewrites rows 0..2 (`affinematrix.cpp:162-194`) so the producers (`bdactor.cpp:3435`/`:3673`, `carnage.cpp:742`, `turret.cpp:917`, `weaponbolt.cpp:1451`) really do leave a correct position there. `GatherLightsParameters` (`mclib/vk_object_light_gather.cpp`) memcpys those 12 floats — landing translation at GPU floats `[3]/[7]/[11]`, i.e. `m[0].w`/`m[1].w`/`m[2].w` under column-major mat4 — then **explicitly zeroes floats `[12][13][14]`**, which is exactly what every consumer decodes as the light origin (`shaders/include/lighting.hglsl:246,256,274`; `shaders/vulkan/mech_tier1.vert:110,117,124`; `shaders/lightgrid_build.comp:112`). **Net effect in the shipped build: every object-path POINT / SPOT / INFINITEWITHFALLOFF light is evaluated at the world origin**, so its falloff tracks distance-from-origin instead of distance-from-lamp. **Fixed at the upload, not in the shaders**, because `GatherLightsParameters` is the single chokepoint feeding all seven read sites across GLSL, SPIR-V and compute (one edit, no SPIR-V rebake) — and because it *cannot* double-correct: the shipped upload is the constant `(0,0,0)`, which carries no information for any consumer to be compensating with. Unaffected: `lightDir` is gathered separately via `GetLocalForwardInWorld` and was already correct; AMBIENT uploads Identity and returns before reading position; INFINITE has no position term. The CPU mirror (`mclib/msl.cpp:1792`) reads POINT position from the *different* `TG_Light::direction` field, so this gate moves the GPU path **toward** the CPU path. Default **OFF** → zeroing branch taken → upload byte-identical to the shipped build.
- `MC2_LIGHT_ROW_PURE` — **DEFAULT-OFF, killswitch inverted** (`=1` opts IN; unset/`0` is the legacy packing loop, byte-for-byte, so the shipped build is unchanged). **Why OFF:** `tools/object_light_row_harness` proves the *algorithm* byte-identical but cannot cover the engine-coupled adapter (`flattenLight()`: `TG_Light` + `Stuff::LinearMatrix4D` -> `LightInput`), because the harness links no engine; with game launches blocked, in-engine byte-identity is **unproven**. **Flip criterion:** one run with `MC2_LIGHT_ROW_PURE=1 MC2_LIGHT_ROW_PURE_VERIFY=1` reporting `checked>0 mismatched=0` across tier1. LIGHT-ROW-PURE-1 / ARCH-LIGHTING slice 1: object light-**row production** is delegated to one pure function, `object_light_row::BuildObjectLightRow` (`mclib/object_light_row.h`). The shipped row is built from an EXPLICIT query — light list plus a DECLARED per-class policy (sun-slot recolour / ambient-slot recolour / sun-disabled) — instead of being a by-product of the per-class mutate-the-shared-light-list, gather, restore dance. **Scope is row production only**: no lighting formula, ambient floor, clamp or sun-direction change, and terrain/water/VFX/UI lighting are untouched, so the ratified look is unchanged by construction. Byte-identity is the acceptance bar, proven offline by `tools/object_light_row_harness` (memcmp of policy-built vs mutation-built rows across every class policy, light counts 0..42, every single-light hole, and both env switches), live by `MC2_LIGHT_ROW_PURE_VERIFY`, and structurally by `=0` being an exact revert. **What it does NOT do:** it does not remove the per-class mutations, so it does not by itself close PROP-BAKE-ORDER-VS-TIME-1 — a reader census found `TG_Shape::MultiTransformShape`, the CPU per-vertex lighting kernel (`mclib/tgl.cpp:2189-2192`), reads the same `TG_Light` objects inside `TreeAppearance`'s sun-off window (`mclib/bdactor.cpp:7139`) and is live in this build, so deleting the mutations would change baked vertex colours while every row stayed byte-identical.
- `MC2_LIGHT_ROW_PURE_VERIFY=1` — LIGHT-ROW-PURE-1 diagnostic, **pure observation**, default OFF. Runs both row producers on the same frame and memcmps the 3600-byte rows: the legacy loop reading the globally mutated list against the pure function reading the same list plus the declared policy. Equal rows mean a class's declaration still matches what it does; a mismatch means a published policy has drifted from its mutation. Emits `[LIGHT_ROW_PURE] MISMATCH` (first 8) plus a checked/mismatched summary every 5000 gathers. Never changes which row is used.
- `MC2_LIGHT_ROW_CENSUS=1` — LIGHT-POS-COMBAT-1 diagnostic, **pure observation**, default OFF. The pre-existing `MC2_OBJECT_PARITY_TRACE` light dump latches after the FIRST `GatherLightsParameters` call of the session, so it can only ever sample a static opening frame — it structurally cannot observe the transient combat lights (`carnage.cpp` explosions and `weaponbolt.cpp` bolts = POINT; `turret.cpp` turret lamps = SPOT with falloff 50/450) that `MC2_LIGHT_POS_COLUMN_FIX` actually acts on. This census keeps a running per-light-type tally for the whole session and records BOTH the source translation (row `[3]/[7]/[11]`, where `BuildTranslation` leaves it) and the uploaded position (row `[12]/[13]/[14]`, what every shader decodes as the light origin). Emits `[LIGHT_ROW_CENSUS v1] FIRST …` once the first time each positional type (INFINITEWITHFALLOFF/POINT/SPOT) is ever gathered, plus a `DUMP` of all seen types every 5000 calls. Each line carries `fix=0|1` so the receipt is self-identifying under either gate state. Never modifies an uploaded float.
- **options.cfg `PropShadowReceive`** (this was the env gate `MC2_PROP_SHADOW_RECEIVE`, deleted 2026-10-12: the value is now the options.cfg key `PropShadowReceive`, read once at startup; `=0`/`=1` in the text below means `FALSE`/`TRUE` (or the number); `MC2_OPTIONS_OVERRIDE="PropShadowReceive=<value>"` sets it for one run without editing the file) — PROP-SHADOW-RECEIVE-1: static props RECEIVE the dynamic CSM cascade (building self-shadow + prop-on-prop) in `shadow_screen.frag`. Default **OFF**; `=1` enables. OFF = legacy static-map-only reception for the a=0.25 prop class (pixel-identical). Shares the CSM PCF ledger + `MC2_SHADOW_MECH_SOFT` terminator; floor 0.4 min-combined with the static map (no double-darken).
- `MC2_SHADOW_PROP_ALPHA` — tree-foliage shadow alpha-test (`shadow_static_prop.frag`). Default **ON**. Covers BOTH the legacy per-texture path and (since SHADOW-PROP-ALPHA-ATLAS-1) the coalesce/BC7-atlas path.
- `MC2_SHADOW_PROP_ALPHA_ATLAS` — SHADOW-PROP-ALPHA-ATLAS-1 kill switch: routes the prop-shadow alpha-test through the SAME coalesce group/bucket `GL_TEXTURE_2D_ARRAY` the color pass samples (program `shadow_static_prop_alpha_atlas`, per-packet `u_texLayer`/`u_uvScale`) whenever coalesce is armed. Default **ON**; `=0` reverts to legacy per-texture binds (pre-slice behavior: atlas trees whose legacy gos texture is dead cast solid square shadows). Inert when `MC2_SHADOW_PROP_ALPHA=0` or coalesce is disarmed.
- `MC2_CLOUD_SHADOW` — cloud-shadow pass. Default **OFF** (opt in with `=1`). Reader: `GameOS/gameos/gos_postprocess.cpp:502` early-returns false when unset. Documented as **ON** until ENV-DROP-CLOSE-1 (2026-07-30).
- **DEBUG-only:** `MC2_SHADER_PATH_TINT` (shader-path tint); `MC2_TERRAIN_DEBUG_MODE` / `MC2_TERRAIN_LOD_CHUNK_DIAG` = **30/31** = dynamic-shadow viz.

## GPU cull readback diagnostics

- `MC2_GPU_CULL_READBACK_TRACE=1` — gate all `[GPU_CULL v1]` diagnostic prints (fallback_n2, fallback_conservative, readback_ok, lifecycle_snapshot, motion_tolerance). Default **OFF**. Without this, zero stdout/fflush on hot path. `readback_stale_reset` (10-frame miss → slot abandon) remains unconditional as it signals a real error.

## Always-on / safety

- `MC2_TGL_POOL_TRACE=1` — TGL pool NULL trace
- `MC2_DESTROY_TRACE=1` — per-destruction lifecycle snapshot
- `MC2_HEARTBEAT=1` — stderr heartbeat per second
- `MC2_REVERSE_Z_TRACE=1` — reverse-Z lifecycle prints
- `MC2_XFORM_PARITY_FATAL=1` — abort when F1-3C clip-space parity probe fails (ViewUniforms.worldToClipGL vs legacy terrain MVP, max_diff>1e-5). Default **OFF** = log-only. Host counterpart: `tests/unit/test_xform_convention.cpp` (XFORM-CONVENTION-HARNESS-1).

## Load-phase timing (LOAD-PHASE-FACTS-1)

- Always-on, no gate — cheap wall-clock phase marks recorded during `Mission::init()`/`Mission::start()` (`code/mission.cpp`, `GameOS/gameos/gameosmain.cpp`). Emits per-phase `[MISSION] t=…s phase=…` lines plus one consolidated `[LOAD_PHASES v1] total=…ms phase1=name:ms …` line at `mission_ready`. Phases: `setup_tgl_ready` (TGL pool init), `terrain_ready` (FST/pak parse + terrain load/prime), `actor_spawn_ready` (mover/vehicle/part object init), `texture_prewarm_ready` (terrain-object load + static-prop residency prewarm), `gpu_finalize_ready` (geometry finalize + indirect-buffer build + light-bake prewarm), `mission_ready`.

## Load-time / logistics gates

- `MC2_SMART_LOAD` — **default ON** (SMART-LOAD-DEFAULT-ON-1). Defers mech-bay data (components/pilots/variants CSV + `Object2.pak` parse + first availability pass) until the first mech-bay accessor touches `LogisticsData`, instead of eagerly loading it as part of campaign init. Kill-switch: `MC2_SMART_LOAD=0` restores legacy eager `LogisticsData::init()` (byte-identical to pre-flip behavior). Only affects the interactive splash->logistics->mech-bay flow (`Logistics::initializeLogData()`, `code/logistics.cpp`); smoke/tier1's `justStartMission` direct-mission-start path never touches `LogisticsData`, so this gate has no effect on smoke `mission_ready_ms`.
- `MC2_SMART_LOAD_TRACE=1` — `[SMART_LOAD]` phase-A/phase-B trace prints (`code/logisticsdata.cpp`).
- `MC2_LOG_LOGISTICS=1` — `[LOGISTICS]` inventory/availability diagnostics (`code/logisticsdata.cpp`), incl. the ISSUE-8 silent-failure trace when `appendAvailability` cannot open a purchase file (`appendAvailability: open FAILED '<path>' -- purchase file skipped`). Default OFF.
- `MC2_LINKUP_TRACE=1` — ISSUE-50 link-up diagnostics (`code/mech.cpp BattleMech::update`): for every off-GUI mech, periodically (~1/s) prints `[LINKUP] part=… name=… status=… onGUI=0 los=… pos=…`, and `[LINKUP] linkup-complete part=… name=…` when the link-up fires. Default OFF = no output, no behavior change.

## Combat / AI gameplay gates + probes (GitHub-issue batch 2026-07-16)

- `MC2_CALLED_SHOT_FIX=1` — ISSUE-9 called-shot effectiveness (gameplay gate, default **OFF** = stock). S1 (`code/mover.cpp Mover::calcAttackChance`): floors the aimed-shot chance modifiers at Head 0.30 / Arm 0.45 / Leg 0.50 (stock gamesys.fit values 0.08/0.15/0.20 leave a head call at the 5% clamp for most engagements; data value wins if a mod sets it higher). Sharpshooter +20 and the 5–95 clamp are unchanged. S2 (`code/weaponbolt.cpp deliverAimedAreaHit`, single-player only): a successful AIMED area-effect (LRM) hit delivers its located damage to the called location; the splash explosion still plays but is cosmetic (0 damage). Stock behavior replaces located damage with a randomly-located explosion ("Hit or MISS, its the same!").
- `MC2_AIMED_FIRE_FIXTURE=1` — ISSUE-9 headless called-shot probe (`code/warrior.cpp MechWarrior::updateActions`, default OFF = byte-identical). Every home-team mech without an active combat order is issued an aimed-HEAD attack at the nearest live enemy mech via `orderAttackObject` with COMMANDER origin (the only origin that installs the tac order — and the aim location only flows from `curTacOrder`); re-issues as targets die (2 s global throttle). Emits `[AIMED_FIRE v1] event=order wid=… tgt_wid=… dist=… res=…`. Pair with `MC2_DIAG_TAGS=…,COMBAT`: the COMBAT `hit` events now carry `"loc"` (mech body index, 0 = head) so the hit-location distribution can be A/B'd with `MC2_CALLED_SHOT_FIX` on/off.
- `MC2_FIRE_DECISION_TRACE=1` — ISSUE-17 fire-refusal instrumentation (default **OFF** = byte-identical; requires `MC2_DIAG_TAGS=…,COMBAT`). NO behavior change. Two event families in the COMBAT JSONL stream:
  - `nofire` (`code/warrior.cpp combatDecisionTree`): emitted once per combat update (0.25 s cadence) when a combat order is active but no weapon fired. Fields: `wid`, `tgt_wid`, `ord` (tac-order code), `result` (`weaponsStatusResult`), per-gate weapon tallies `rd`/`am`/`rn`/`lk`/`ch`/`hot` (not-ready / out-of-ammo / out-of-range / not-locked / no-chance / too-hot), `los` (direct lineOfSight probe — splits LOS from fire-arc within `lk`), `dist`.
  - `firegate` (`code/mech.cpp fireWeapon`): emitted at each silent non-zero early return — `site` ∈ `status`/`body`/`ready`/`terrain`/`dead_target`/`no_target`/`range`/`shots`/`node_recycle` — i.e. the decision tree said FIRE but the weapon silently skipped.
- `MC2_BRAIN_FREEZE_TRACE=1` — per-tick decision-state stderr trace at the end of `MechWarrior::runBrain` (`code/warrior.cpp`), default **OFF** = byte-identical. One `[BRAIN_FREEZE_TRACE]` line per warrior per brain tick: `wid`, `brainState`, `curTacOrder.code`, `movePath.numSteps`, `moveState`, `hasBrain`, plus the ISSUE-10 stall-classification fields: `orderState` (0=GENERAL/1=PLAYER/2=ALARM — pinned at 2 = escape-tile alarm loop), `mainGoal`/`goalWID` (goal-plan divert target: turret/gate/wall capture that never completes), `genOrder` (`tacOrder[ORDERSTATE_GENERAL].code` — brain/FSM emitted an order vs ABL stall), `pos=(x,y)` (position delta over ticks = path-but-not-moving class) and `lastMoveErr` (`LastMoveCalcErr` — no-path class). High volume: one line per warrior per tick; use short runs.

## META-FIX recurrence-proofing gates (batch 1, 2026-07-16)

- `MC2_LOG_FILE_RESOLVE` — design E tier 3 (`mclib/file.cpp`, parsed at `InitModSearchPaths`), default **OFF** (`0`). `=1`: level-1 stdout tags (`[mod]`/`[mod-miss]`) + shutdown summary `[file-resolve] opens=N misses=M` (opens = successful `File::open` resolutions; misses = full-ladder misses incl. `fileExists` probes). `=2`: level 1 plus a JSONL resolve trace (layer ∈ `mod:<id>`/`base-loose`/`base-strip`/`fastfile`/`cd`/`MISS`) to `MC2_RESOLVE_TRACE_FILE`; `fileExists()` misses also emit MISS records (probe API — the CD-presence check probes absent files by design; the nightly MISS audit check-file-resolve-miss.py and its baseline were retired 2026-10-12, cull wave 1). Promoted from the check-env-registry allowlist to `kAuxEnvVars` in META-FIX-BATCH-1.
- `MC2_RESOLVE_TRACE_FILE=<path>` — output path for the level-2 JSONL resolve trace (opened for append at `InitModSearchPaths`; relative paths resolve against the game CWD = exe dir). Only consulted when `MC2_LOG_FILE_RESOLVE>=2`. Its nightly consumer check-file-resolve-miss.py was retired 2026-10-12, cull wave 1; the trace is read by hand.

## META-FIX recurrence-proofing gates (batch 2, 2026-07-16)

- `MC2_FIXTURE_ORDER_LIVENESS=1` — design F behavioral fixture (`code/mission.cpp` soak namespace; requires `MC2_SOAK_AUTOWIN`), default **OFF** = byte-identical (static-getenv latch). Phases (fractions of `MC2_SOAK_WIN_AFTER_SEC`): 0.2 — COMMANDER-origin unaimed `ATTACK_TO_DESTROY` per armed home mech at the nearest live enemy (PLAYER origin never installs the tac order — the trap that burned the first aimed-fire fixture run); 0.5 — COMMANDER-origin `orderMoveToPoint` per home mech, 300 m toward map center (replaces the attack order → clean 0.4·win displacement window; attack-first is deliberate — the reverse order self-sabotages the move bound when targets spawn in range: measured 1/11 stalled standing-and-firing on stock mc2_24). `[ORDER_FIXTURE] kind=attack|move wid=… tgt=… t=…` per issued order; 0.9 — one self-asserting `ORDER_LIVENESS_SUMMARY issued_move=N moved=N refused=N stalled=N verdict=PASS|FAIL` line (moved = net displacement ≥ 25 m from move-issue; refused = classified `LastMoveCalcErr`; dead/disabled exempt). NOTE for smoke-tier runs: set `MC2_SOAK_WIN_AFTER_SEC` slightly ABOVE `--duration` (e.g. 31 vs 30) — an autowin that ends the mission before the smoke window closes reports `reason=early_exit` and fails the run for an unrelated reason. When this var is set in the **runner** env, `run_smoke.py` hard-fails a missing summary or `verdict=FAIL` (VEG-SMOKE-FLOOR activation pattern — non-fixture runs untouched; absence-is-failure catches the fixture dying silently). Its nightly consumer check-order-liveness.py and the triage playbook ai-order-triage.md were retired 2026-10-12, cull wave 1. Static sibling: `scripts/check-order-gate-coverage.py`.
- `MC2_RES_ASSERT` — design D runtime tier (`RenderCore/ResolutionContract.h`), default **OFF** (unset/`0`) = one static bool test, tier1 byte-identical. `=1`/`log`: one `[RES_ASSERT v1] event=violation site=…` stdout line per violation signature. `=fatal`: log then `abort()` (nonzero exit = red run — the machine oracle for the resolution smoke matrix). Chokepoints: `camera_aspect` (`mclib/camera.cpp` `Camera::setOrthogonal` — projection aspect must equal drawable aspect unless `MC2_CAMERA_ASPECT_NATIVE=0` or inside the mech-preview FBO; issue #45 class), `hud_clamp`/`drawable_stale`/`drawable_nonpositive`/`windowed_size` (`gameosmain.cpp` `draw_screen` per-frame: clamp ⇒ 800x600 logical canvas, cached drawable matches a fresh probe with 3-frame grace, `MC2_WINDOWED`+`MC2_WIDTH`/`MC2_HEIGHT` boots keep the requested drawable — the #49 oracle; desktop-smaller downgrades to `event=note`), `hud_canvas_bounds`/`hud_roundtrip` (`gameos_graphics.cpp` `flushHUDBatch` — canvas box ⊆ drawable; forward remap (hud-scale shrink∘canvas) composed with `gos_HudInverseMousePoint` is identity within 0.5 px). Static sibling: `scripts/check-res-aspect-contract.py`; smoke sibling: `tests/smoke/matrices/resolution.json` (nightly). Passthrough-only companions added to the run_smoke allowlist: `MC2_WIDTH`, `MC2_HEIGHT`, `MC2_WINDOWED`, `MC2_FORCE_43`, `MC2_CAMERA_ASPECT_NATIVE` (all pre-registered/allowlisted in check-env-registry).

## META-FIX recurrence-proofing gates (batch 3, 2026-07-16)

- `MC2_ICON_FAILLOUD=1` — design B PART 3 (`code/mechicon.cpp` `iconTileChecked` at the three CPU icon-tile blits), default **OFF**. `=1` substitutes a solid MAGENTA tile when an icon index resolves outside the source atlas (last-pixel arithmetic identical to the per-pixel bounds guards). Default keeps the legacy wrap/blank semantics — load-bearing for MC2X high-index mod content — while the `[ASSET_LOAD v1] event=icon_oob` line stays always-on (rate-limited). UV-path sibling: `icon_atlas_cell.h` `cellForIndexChecked` (logisticsmechicon/mechlistbox) returns the legacy-computed cell UNCHANGED on OOB. Row bound harness-validated (retail 256x256 / MC2X 512 / MCO 1024/40, `tools/icon_atlas_harness`) before the `icon_oob` smoke bucket arms red.
- `MC2_APPEAR_AUDIT=1` — design B PART 4 (`code/mission.cpp` `Mission::start` beside `mission_phase_mark("mission_ready")`), default **OFF**, OBSERVATIONAL-FIRST. One O(N) pass at mission_ready: every existing registered object either has a non-NULL appearance whose base shape loaded, or emits `[APPEAR_AUDIT v1] event=missing reason=appearance_null|base_shape_failed handle= class= part=` (bounded 32 lines) + `summary scanned= missing=`. POSITION-FREE (off-map-staging carve-out, INVIS-VEHICLES-FIX-1: staged movers have valid appearances; no positional/visibility term — pinned by `check-tgl-load-callers.sh`). The `appear_audit` smoke bucket ships ADVISORY-DISARMED; arming it red (and force-setting this gate for tier runs) requires a full 24-map `failed=0` baseline — batch 3 ships the tier1-missions baseline, the 24-map run is a documented follow-up.
- `MC2_MOVER_COHERENCE_AUDIT=1` — design A runtime tier (`code/mission.cpp` `auditMoverCoherence`, ~1 Hz), default **OFF** = one static bool test, byte-identical. Read-only audit that the five per-mover registries (isOnGUI render gate, force-bar roster, selection, tacmap blip, commandability) agree for **home-commander SP** movers: I1 live onGUI mover holds a force-bar icon (I1b bar-full = ADVISORY — `ForceGroupBar::addMech` silently drops the 17th mover in release); I2 off-GUI mover has no icon and no PLAYER-origin tac order (SELF/COMMANDER origin = ADVISORY I2b: auto-powerup and brain orders are legitimate; the #50 invisible-but-commandable signature violates this row); I3 disabled-not-destroyed keeps `getAppearance()!=NULL` (#42b-ADJACENT teardown class — the HISTORICAL #42b guard-shape bug is static rule S4's job); I4 every icon unit is ObjectManager-registered (pointer-identity scan — deliberately not `getWatchID`, which would ASSIGN an id inside a read-only audit); I5 selected ⇒ onGUI. Emits deduped `[MOVER_COHERENCE] VIOLATION|ADVISORY rule=… part=… name=… detail=…` stdout lines (mirrored to the diag sink) + a process-exit `summary checked= violations= advisories= audits=` line. **POSITION-FREE by contract**: off-map staged movers (onGUI=1 at an authored staging position waiting on a mission flag — mc2_10's convoy, INVIS-VEHICLES-FIX-1 census evidence) are a LEGITIMATE mover state; the audit keys on no positional term and static rule S6 (`scripts/check-mover-registry-coherence.sh`) pins the audit region position-free. `run_smoke.py` force-sets `=1` for `--tier` runs (read-only; shipping default stays OFF) and red-flags any VIOLATION via the `mover_coherence` marker bucket (`allow_mover_coherence` manifest override); ADVISORY lines surface as details only. Nightly add-ons for rows tier1 cannot reach: mc2_08 (LINKUP-PENDING, #50) + `MC2_SOAK_SALVAGE_TRADEMOVER` (trade row). Static sibling: `scripts/check-mover-registry-coherence.sh` (S1 write allowlists, S2 pairing windows, S3 read-gate pins, S4 #42b guard shape — SOLE defense, S5 link-up gate shape, S6 staging carve-out).

## RenderWorld arc

- `MC2_OBJECT_ID_BUFFER=1` — R32_UINT MRT attachment-2 (M1.5)
- `MC2_MECH_OBJECT_ID_SELFTEST=1` — M2.5 validator
- `MC2_MECH_PICK=1` — mech-pick consumer (M2.6). Requires `MC2_OBJECT_ID_BUFFER=1`.
- `MC2_STATIC_PROP_PICK=1` — static-prop pick (M1.6). Requires `MC2_OBJECT_ID_BUFFER=1`.
- `MC2_GAMEPLAY_PICK_SELFTEST=1` — M2-pre spine validator
- `MC2_GPU_PICK_HOVER=1` — GPU hover pick for mechs/dynamic actors (GPU_PICK_HOVER_DYNAMIC-1). Requires `MC2_OBJECT_ID_BUFFER=1`. Only mech kind uses GPU result; static props fall through to CPU.
- `MC2_GPU_PICK_HOVER_TRACE=1` — verbose hover-pick log (hit/miss/fallback per frame + session totals on exit).

## Mech import, TechBrain, and misc arc gates

- `MC2_ASSIMP_MECH_IMPORT=1` — BT2018 mech import 1A: route mech geometry through the Assimp GLTF/FBX importer (`mclib/mech3d.cpp`) instead of the legacy MSL loader. Default **OFF**.
- `MC2_BRAIN_RUNTIME=1` — TechBrain declarative-brain runtime master gate (`code/warrior.cpp`). Default **OFF**. Companion `MC2_BRAIN_RUNTIME_APPLY` gates whether resolved intents are actually applied.
- `MC2_BRAIN_RUNTIME_APPLY=1` — TechBrain runtime apply gate. Default **OFF**; requires `MC2_BRAIN_RUNTIME=1`.
- `MC2_BRAIN_MISSIONFIT_OPORD=1` — Enhanced Brain OPORD precedence over editor-authored routes (`code/`). Default **OFF** by owner ruling 2026-10-14 (the one named exception to the no-default-off-gates rule): enabling it changes stock AI behavior and needs its own proof.
- `MC2_BRAIN_DISPATCH=1` — TechScript special-dispatch master gate (`code/mission.cpp`, `code/brain_special_dispatch.cpp`). Default **OFF**; requires `MC2_BRAIN_RUNTIME=1` + `MC2_BRAIN_RUNTIME_APPLY=1`.
- `MC2_MOVE_RECON` — pathfinding cost instrumentation (`mclib/move_recon.cpp`). Default **OFF**, zero behavior change when unset. `MC2_MOVE_CHUNK_SHADOW` / `MC2_MOVE_PATH_CACHE_SHADOW` are alternate enables for the same instrumentation.
- `MC2_UNIT_PROFILE_DATA` — UNIT-PROFILE-ARC data-driven unit+equipment definition source (`code/unitprofile.cpp`). Default **LEGACY** (unset/`0` = legacy path); any non-`0` value switches to the generated `UnitProfileData` source.
- `MC2_ACTIVE_MOD=<folder>` — active mod overlay id (`mclib/file.cpp`, `mclib/fx_def_registry.cpp`, `mclib/anim_override_registry.cpp`, `mclib/model_override_registry.cpp`). Unset/empty = stock base-game mode. When set, mounts `<folder>`'s content (and any `MC2_MOD_DEPS` add-on layers) over the stock campaign for mod-cache/asset-override resolution.
- `MC2_FX_DEFS=1` — FX-DEFS-SIDECAR-1: overlay `data/effects/defs/<Name>.fxdef.json` (+ active mod's own defs) onto the loaded effect `SpecLibrary` (`code/mechcmd2.cpp`). Default **OFF**; parsing/diagnostics are always free, but the spec-mutating apply only runs when set. OFF = byte-identical to stock.
- `MC2_EQUIPMENT_ADD` — EQUIP-ADD-FILL-1 (ADD-ladder S2): mod equipment defs with an explicit `LegacyMasterId` on an UNDEFINED compbas ordinal materialize as live MasterComponent + mechlab rows at load (`code/equipment_registry_engine.cpp`). Default **OFF** (unset/`0`); requires `MC2_UNIT_PROFILE_DATA=1` and an active mod (`MC2_ACTIVE_MOD`/`MC2_MOD_DEPS`). Gate-OFF is zero-I/O byte-identical.

## EditorBridge

- `MC2_EDITOR_MODE` — activates `EditorBridge` API. Default `0`. Combine with `MC2_OBJECT_ID_BUFFER=1` for GPU pick.
- `MC2_EDITOR_TRACE=1` — Mission Editor launch/diagnostic trace: writes `editor-startup.log` and logs brain `.abl` opens plus imported patrol waypoints to stderr. Forced on by the editor shell and by `run-editor.bat`.

## Render-contract assert

- `MC2_RENDER_CONTRACT_ASSERT=1` — runtime GL draw-buffer/depth state vs declared expectation. Phase 2 (`137dc70`).

## Pre-commit invariant scripts

- Resolution matrix (nightly): `py -3 scripts/run_smoke_matrix.py resolution` — 5 aspect/windowed entries, `MC2_RES_ASSERT=fatal` machine oracle.
- Object lifecycle: `sh scripts/check-destroy-invariant.sh`
- UI icon atlas: `sh scripts/check-asset-scale-callers.sh`
- Shader ABI: `cmake --build build64 --target shader_schema`
- RenderWorld firewall: `sh scripts/check-include-firewall.sh`
- No raw GL from game: `sh scripts/check-no-raw-gl-from-game.sh`
- VFX no objectId: `sh scripts/check-vfx-no-objectid.sh`

## Material / static prop gates
- `MC2_SKIP_STATIC_TREES` — pure static-natural update skip (R2b, ~4977→~145 terrain-object updates on dense maps). **Default ON** (opt-out: only `=0` disables; gameplay-critical gates/turrets/special-buildings are excluded from the skip set and still tick every frame).
- `MC2_TREE_DIAG_TRACE` — **TREE-DIAG-REGISTER-1. Default OFF — and the OFF token is UNSET, not `0`.** Diagnostic (2026-05-05) for the "black billboard square" static-prop bug: prints up to 8 lines from each of `GpuStaticPropBatcher::submit()` (dynamic register/replay) and `submitCachedInstance()` (static recipe replay), with every `GpuStaticPropInstance` field formatted identically so a side-by-side grep diff pinpoints which baked field is stale on the static replay. ⚠ The reader is `GameOS/gameos/gos_static_prop_batcher.cpp:115` `(getenv("MC2_TREE_DIAG_TRACE") != nullptr)` — **presence-only, so `MC2_TREE_DIAG_TRACE=0` ENABLES the trace.** Same trap as `MC2_BRIDGE_MOVER_STATE`; do not describe it as an `=0` killswitch. Observation only (no render/gameplay/sim state changes), so ON vs OFF is pixel-identical. **Registered retroactively:** it was engine-read but absent from both the `run_smoke.py` passthrough allowlist and this file, so a smoke run asked to turn it ON had the var dropped at `Popen` and emitted no `[TREE_DIAG]` lines — which reads as "the bug did not reproduce" rather than "the trace never ran".

- `MC2_MATERIAL_KTX=1` — KTX2 sidecar loader for static-prop tex array. Default **OFF**.
- `MC2_MATERIAL_GPU` — MaterialGpu table upload + SSBO bind. Default **ON**.
- `MC2_MATERIAL_GPU_SAMPLE` — route albedo through MaterialGpu in frag. Default **ON**.
- **options.cfg `StaticPropAmbient`** (this was the env gate `MC2_STATIC_PROP_AMBIENT_V1`, deleted 2026-10-12: the value is now the options.cfg key `StaticPropAmbient`, read once at startup; `=0`/`=1` in the text below means `FALSE`/`TRUE` (or the number); `MC2_OPTIONS_OVERRIDE="StaticPropAmbient=<value>"` sets it for one run without editing the file) — hemisphere ambient fill in `static_prop.vert`. Default **ON** (flipped 2026-10-13, after a default-equality capture; `=0` restores the old look).
- **options.cfg `StaticPropIblSh`** (this was the env gate `MC2_STATIC_PROP_IBL_SH`, deleted 2026-10-12: the value is now the options.cfg key `StaticPropIblSh`, read once at startup; `=0`/`=1` in the text below means `FALSE`/`TRUE` (or the number); `MC2_OPTIONS_OVERRIDE="StaticPropIblSh=<value>"` sets it for one run without editing the file) — SH-L2 IBL ambient. Default **ON**. Kill=`=0`.
- **options.cfg `StaticPropIblSunRotate`** (this was the env gate `MC2_STATIC_PROP_IBL_SUN_ROTATE`, deleted 2026-10-12: the value is now the options.cfg key `StaticPropIblSunRotate`, read once at startup; `=0`/`=1` in the text below means `FALSE`/`TRUE` (or the number); `MC2_OPTIONS_OVERRIDE="StaticPropIblSunRotate=<value>"` sets it for one run without editing the file) — **IBL-ROTATION-LAND-1. DEFAULT OFF.** Rotates the SH-IBL ambient probe about world-up so its bright lobe lands on the **mission sun**. Today it never is: there is one coefficient set for the whole game (`kIblShSetCount == 1`; `kMissionShMap[]` holds a single `{nullptr,nullptr}` placeholder, so its *count is 1, not 0* — functionally empty, not literally), its bright axis is a constant +0.344° Stuff azimuth, the **median mission misses it by 106°** (worst 176.7° on mc2_14), and on **17 of 24 missions the probe is brighter on the wall facing *away* from the sun**. The swing being mis-aimed is 21.05:1 across the sphere — the often-quoted 7.71:1 is only the +X/−X *pair*. This is an oversight, not a design choice: the **visible skybox drawn from the same HDRI is already sun-synced** (`gos_postprocess.cpp:5201-5218`, `skyYaw = hdriBakedSunAz_ − sunAzGL`); the probe derived from that sky is the one consumer that skips the step. Implemented **CPU-side at the upload site** (`gos_static_prop_batcher.cpp::iblShCoeffsForUpload`) — zero per-vertex cost, `8064c6930:shaders/static_prop.vert` untouched — because rotation about the basis pole is closed form (m=0 `[0]/[1]/[6]` invariant; m=1 pairs `([3],[2])`,`([4],[5])` at *t*; m=2 pair `([8],[7])` at *2t*) and rotating the coefficients is provably identical to counter-rotating the normal (1e-12). OFF returns the same pointer the pre-slice build uploaded ⇒ byte-identical. **Grade on mc2_17** (mean move 0.181, 54.6 % of wall azimuths unclamped) **or mc2_24** (0.231) — **not mc2_10 or mc2_22**, which are ambient-192 *and* sun-192 saturation traps where a full sun-align rotation moves one byte (0.0037 / 0.0123). mc2_05 moves most (0.358) but its sun is below the horizon. Ship it with its floor, below. Contract: `tools/class_light_band_harness` Stage F+G. **NOT VISUALLY VERIFIED** — engine launches are blocked in this lane.
- **options.cfg `StaticPropIblSunRotateFloor`** (this was the env gate `MC2_STATIC_PROP_IBL_SUN_ROTATE_FLOOR`, deleted 2026-10-12: the value is now the options.cfg key `StaticPropIblSunRotateFloor`, read once at startup; `=0`/`=1` in the text below means `FALSE`/`TRUE` (or the number); `MC2_OPTIONS_OVERRIDE="StaticPropIblSunRotateFloor=<value>"` sets it for one run without editing the file) — companion floor for the gate above, in the probe's **irradiance** units. **Default `0.45`**; `=0` gives the bare rotation; junk/negative/NaN → 0.0; clamped to 2.0. Inert unless `MC2_STATIC_PROP_IBL_SUN_ROTATE` is on. A rotation is an *isometry*, so it cannot invent a darker probe value — but once the probe is aimed correctly its dark lobe **coincides** with the `NdotL == 0` wall instead of sitting apart from it. ⚠ **Corrects an inherited figure:** the "shadow-side walls darken hard, maxDown −0.705/−0.704/−0.692" warning is real *as a per-azimuth delta*, but a delta is not a new minimum — on the darkest-wall statistic the bare rotation moves those four missions by −0.012 *or not at all* (0.3076→0.2953 on mc2_12/18; 0.2953→0.2953 on mc2_13/17). The worst darkest-wall regression is **mc2_14 at −0.248**, not one of the four named. (Also: the four are all ambient 31/255, but only mc2_12/18 are near 106° error — mc2_13 is −75.3°, mc2_17 −51.6°.) The floor earns its place for the honest reason: those four sit at **0.295 in the build that ships today**, below terrain's own hard floor of `mix(0.35,1.20,0.02) = 0.367` — a wall darker than the ground it stands on. **Shape:** one *uniform* lift on `L00` (the only isotropic coefficient), so the aim is untouched (bright axis still hits the sun to 1e-9 at every floor value), it can only lift, it stays uniform across R/G/B so the blue-dominant dark end (min 0.232 R / 0.360 G / 0.477 B) is not neutralised, and — the sphere minimum being rotation-invariant — it is searched **once per coefficient table**, numerically rather than baked, so regenerating `IblShCoeffs.h` cannot make it stale. **Why 0.45:** solve `31/255 + F·0.55 ≥ 0.367` ⇒ `F ≥ 0.4462`, giving a guaranteed **0.3691 ≥ 0.367** for every normal (walls over-deliver at 0.4149). **Cost, stated not hidden:** +0.218 irradiance on the bright end too — +4.5 % against a 4.896 maximum, almost all inside the clamp.
- **options.cfg `StaticPropPbr`** (this was the env gate `MC2_STATIC_PROP_PBR_V1`, deleted 2026-10-12: the value is now the options.cfg key `StaticPropPbr`, read once at startup; `=0`/`=1` in the text below means `FALSE`/`TRUE` (or the number); `MC2_OPTIONS_OVERRIDE="StaticPropPbr=<value>"` sets it for one run without editing the file) — per-frag Schlick-Fresnel specular. Default **OFF** (owner ruling 2026-10-13: most props carry no PBR data and the fallback sheen reads as a metallic glint that clusters in one corner; it returns with the material re-authoring arc; `StaticPropPbr=TRUE` turns it on).

- `MC2_LIGHT_CONE_NODES=0` — **LIGHT-CONE-MODELS-1. DEFAULT-ON** (`mclib/cone_glow.cpp:118`, `return !(v && v[0]=='0')`). Places building/guard-tower spotlight ground pools using the fixture's authored cone **node** geometry (apex + aim + length read off the shape) rather than the older heuristic that centred each pool on the light node's own origin. Unset or any value other than `0` (**default**) = node-derived pool centres; `0` = restores the pre-LIGHT-CONE-MODELS-1 heuristic/origin-based centres (batch-6 behaviour). Single reader: `cone_glow.cpp` `nodes_enabled()`; consumed at `mclib/bdactor.cpp:3398`. **Documented retroactively by NIGHT-TAGS-GREEN-1** — it shipped default-ON but unregistered and undocumented, which is precisely how `CONE-SPILL-COUPLING-1` (`c1f98b45`) later rode it into the default render ungated (see `MC2_CONE_SPILL_COUPLING` below). Resolved once at process start.
- `MC2_SPOTLIGHT_DIRECTION_FIX=1` — **SPOTLIGHT-DIRECTION-1.** Fixes two coordinate-space defects at the building/guard-tower spotlight registration site (`mclib/bdactor.cpp`). **(a)** `TG_Light::direction` was never set, so it kept the `init()` default `(0,0,0)`. Despite the name, `direction` is a **POSITION** for POINT/SPOT lights — `mclib/tgl.h:213` says *"This is the light spot center for POINT and SPOT"*, and `mclib/msl.cpp` reads it as `lightPos = light->direction; shapePosition -= lightPos;`, i.e. it **subtracts** it from a world position to derive the per-vertex light vector. Left at `(0,0,0)` every CPU-baked vertex is lit as though the lamp sat at the **world origin** instead of on its own building. **(b)** `SetLightToWorld()` was built from the **raw** MC2-space `childPos` rather than the swizzled Stuff-space position every other consumer expects. `1` → both fields use the MC2→Stuff position swizzle `(-x, z, y)`, matching the sibling terrain-light site in the same file. Unset/`0` (**default**) → historical behaviour, byte-identical. **Not latent:** measured on the deployed build with the temporary `[FULLBAKE_DIAG v1]` counters — `mc2_10` (night) consumed **4,310,486** active POINT/SPOT lights in the CPU bake over a 15 s run, **3,995,686 (92.7%)** of them with `direction` still `(0,0,0)`; `mc2_01`/`mc2_03`/`mc2_24` measured **0**, because these lights are night-gated (`eye->isNight`). Default-**OFF** because the correction changes default night appearance on `mc2_10`. Single reader: `mclib/cone_glow.cpp` `spot_direction_fix_enabled()`. Resolved once at process start.
- `MC2_CONE_SPILL_COUPLING=1` — **CONE-SPILL-GRADE-1.** Re-enables CONE-SPILL-COUPLING-1 (`c1f98b45`), which originally shipped **ungated** as a default-behaviour change riding the default-ON `MC2_LIGHT_CONE_NODES`, and is now **default-OFF on measured evidence**. `1` = building/guard-tower spotlight ground pools are placed at the true beam/terrain intersection found by marching the ray (`mc2ConeGroundHit`, `mclib/bdactor.cpp`), the `200.0f` spill clamp is removed, and a beam that never meets ground within its own length (or leaves the map) emits **no** pool. Unset/`0` (**default**) = the pre-`c1f98b45` placement (`apex + aim*spill`, `200.0f` clamp, `z` snapped to `getTerrainElevation`, no miss rejection). **Why default-OFF:** `MC2_LIGHT_CONE_DIAG=1` over all 9 distinct building spotlight fixtures on `mc2_10` measured `hit=0` (pool **suppressed**) for **7 of 9**, `hit=1` for 2 with `horizErr` only **7.0 world units**, and `clamped=1` for **0** — the stock fixtures are authored near-horizontal (`aim.z=-0.089`, `coneLen=167.8` → 14.9 units of drop) from an apex ~17 units above terrain, and 3 point straight up (beacons), so requiring a real intersection within the authored length deletes almost every pool. The authored cone length is a visual extent, not a physical reach. Net effect at default-ON was 7 of 9 night ground pools deleted for an imperceptible reposition of the other 2 — a regression against the complaint it was written for. Pairs with `MC2_LIGHT_CONE_DIAG=1`, which prints the OLD and NEW pool positions plus `horizErr` in the same frame.
- `MC2_CONE_POOL_POLICY=0` — **CONE-SPILL-POLICY-1. DEFAULT-ON (`=1`).** Pool-placement policy for building/beacon spotlight ground pools on the **default** path (i.e. while `MC2_CONE_SPILL_COUPLING` stays OFF). Policy `1` (**default**): **(a)** a beam aimed **up** (`aim.z > 0.15`; MC2 world is z-up — the `mc2_23` beacons aim exactly `(0,0,1)`) emits **no** ground pool: it lights the sky, and the legacy unconditional snap-z placement planted a pool at the beacon's own base (the user-reported "cones do not intersect the ground and don't need a spill"); **(b)** a downward/near-horizontal beam places its pool at the **true beam/terrain intersection** (`mc2ConeGroundHit`) searched over an **extended reach** of `MC2_CONE_POOL_REACH × coneLen` — the authored cone length is a *visual* extent, not a physical reach (requiring a hit within it deleted 7 of 9 `mc2_10` pools, the CONE-SPILL-GRADE-1 regression) — and a miss within the extended reach suppresses the pool. `0` = legacy unconditional far-end + snap-z placement with the 200u clamp (killswitch, exact pre-slice behaviour). Constants validated offline by predict_cone_pools.py (retired 2026-10-12, cull wave 1; mc2_10 non-up fixtures all KEPT, mc2_23 straight-up beacons 0 pools). Single reader: `mclib/cone_glow.cpp` `pool_policy()`; consumed in `mclib/bdactor.cpp`. Resolved once at process start.
- `MC2_CONE_POOL_REACH=K` — **CONE-SPILL-POLICY-1** reach multiplier for the extended intersection search (reach = `K × coneLen × MC2_LIGHT_CONE_SPILL`). Default **4**, clamped `[1,16]`. Only read when `MC2_CONE_POOL_POLICY=1`. `4` keeps every non-upward `mc2_10` tower fixture (their near-horizontal beams drop ~15 u over the authored length from ~17 u apexes, needing ~1.2–4× the authored length to meet ground) while still rejecting beams that genuinely land nowhere. Tuned against predict_cone_pools.py (retired 2026-10-12, cull wave 1).

  - **Mechanism.** On HDRI missions the skybox colour draw is followed by a stencil-only tag pass (`8064c6930:shaders/hdri_skybox_stencil_tag.frag`) marking true-sky pixels `stencil=1`; `runEdgeFog`/`runFogOob` sample that stencil through a `GL_STENCIL_INDEX` `glTextureView` of `sceneDepthTex_` and feather-exclude those pixels, so the OOB cloud bank stops painting over the sky (the user-reported "can't see the skybox").
  - **Inert on non-HDRI missions.** `renderHdriSkybox()` early-returns before the tag pass, nothing is tagged, the frags' stencil branch is a no-op, and `fog_oob.frag`'s `worldDir.z < -0.22` band fallback is retained — byte-identical to legacy there.
  - **Registered and documented only as of BACKEND-DEFAULT-PARITY-1.** Before that it was in no registry table; the only thing accounting for it was check-env-registry.sh (retired in G8; the gate table is the registry)'s ALLOWLIST, whose comment still read "(default OFF)" long after the flip, as did `edge_fog.frag` (former GL source, removed in s63) and `fog_oob.frag` (former GL source, removed in s63). Being unregistered also kept it out of the reader-conflict rule of check-gate-default-agreement.py (retired 2026-10-12, cull wave 1), which iterated the registry — so its **two contradicting readers** (`gos_postprocess.cpp:578` default ON vs `vulkan_post_fog_native.cpp:589` default OFF) went unreported for the life of the gate, and on an unset environment the VK-native fog path rendered sky pixels the GL path excludes. There is now exactly **one** reader: `skyboxFogExcludeEnabled()`, exported as `gos_SkyboxFogExcludeEnabled()` for the Vulkan fog TU's parity guard.
  - **VK effect of the correction.** The parity guard now fires on an unset environment as designed, so `runVkPostFogNative()` declines and the unchanged GL fog chain runs. ⚠ **Corrected by GATE-PARITY-CLOSE-1:** this line previously described `MC2_VK_POST_FOG_NATIVE` as "itself opt-in, default OFF", copying a stale header comment in `vulkan_post_fog_native.cpp`. It is **not** opt-in — `vkPostFogNativeEnabled()` resolves it with `mc2vkgate::armed<>()`, which falls through to the umbrella, so **unset + `MC2_VK_SHARED_GBUFFER=1` arms it**. The guard is therefore load-bearing under the umbrella, not only under an explicit debug flag. It is still OFF on a default launch, because the umbrella itself is presence-gated `=1` and reads OFF once the session fail-softs to GL. VK-native fog is reachable only alongside `MC2_SKYBOX_FOG_EXCLUDE=0`. GL frames are unaffected either way. Porting the stencil plane to VK (`glGetTexImage(GL_STENCIL_INDEX)` → `VK_FORMAT_R8_UINT` → `usampler2D`) is the standing follow-up, `.claude/VULKAN-NEXT-SLICES.md` LM4.
- `MC2_LEGACY_LIT_MC2_FRAME` — **DEFAULT-ON as of VEHICLE-LIGHTING-PATH-1** (`=0` reverts). **This is a correctness requirement of the `gvactor.cpp` `CacheGpuLightData()` call, not an optional extra — do not "clean up" as inert.** It previously measured as a no-op for a structural reason: until VEHICLE-LIGHTING-PATH-1 vehicles never populated `lightData_`, so `calc_light` took its `numLights==0` early-return and returned `vec3(1,1,1)` regardless of frame. Correcting the *frame* of a row holding *zero lights* cannot do anything — it was unreachable, not wrong. The shared-program blast radius noted below is now self-limiting: shapes that do not gather still return before the frame is read. VEHICLE-NIGHT-LIT-1: feed `calc_light` MC2-frame position + normal in `gos_tex_vertex_lighted.vert` (the legacy `mcTextureManager`/`ShapeRenderer` lit program that draws **ground vehicles**). `GatherLightsParameters` writes MC2-frame `light_to_world`/`light_dir`, and the other two `calc_light` consumers already feed MC2-frame inputs (`mech.vert` -> `worldMC2`; `static_prop.vert` -> MC2 `worldPos` + `STATIC-PROP-NORMAL-FRAME-FIX` normal swap). This shader was the last one still passing Stuff-frame `WorldPos`/`Normal`, so `length(vertex_world_pos - light_pos)` mixed frames -> `GetFalloff` rejected every POINT/SPOT/INFINITEWITHFALLOFF light and the INFINITE sun `NdotL` was ~90° off. Vehicles kept only `base_light` + AMBIENT and rendered near-black at night while mechs/buildings/fences/terrain in the **same** light pool were lit — including trucks parked inside a guard-tower cone. Default **OFF** (`u_legacyLitMc2Frame=0` -> legacy feed -> byte-identical). Blast radius: the program is shared by other legacy lit shapes, so `=1` changes their lighting too (day included) — hence the gate.
- `MC2_STATIC_LIGHT_UPLOAD_SPLIT` — upload immutable static light prefix `[0..S)` once/dirty-only + dynamic suffix per frame (vs whole LightsData SSBO every frame). Default **ON**. Kill=`=0` (legacy whole-buffer, bit-identical). Requires `MC2_LIGHTBAKE`.
- `MC2_LIGHTBRIDGE_STABLE_SKIP=1` — skip per-frame `EmitBakedGpuLightData` + `getCachedGpuLightIndex` + `staticReg` pointer-chain for stable baked static props (LIGHTBRIDGE-STABLE-SKIP-WIRE-1). Default **OFF**. Requires `MC2_LIGHTBAKE=1` (implicit when bake map populated). Gate: `stableLightSkipArmed` + `stableLightSkipEligible` + valid `lightDataIndex` + generation match. Expected: `serial_commit_us` ~593µs → ~50–80µs on stable frames. Diagnostics: `MC2_LIGHTBRIDGE_STABLE_SKIP_DIAG=1` (prints `LIGHTBRIDGE_STABLE_SKIP:` every 300 frames; independent of skip gate).
- `MC2_LIGHT_POOL_MAX=N` — tune, default **256** (LIGHT-POOL-CAP-1). Caps how many lights the producer packs into the pool. Was 64 before `5213d257`. Because the shader loops per fragment, this is a **fragment-bound** cost: raising it is measurable only against a visible, unthrottled window (see `docs/tracy-harness.md`).
- `MC2_LIGHT_GRID=0` — killswitch, default **ON** (LIGHT-CLUSTER-PERF-1, `93c22e39`). World-space XY light grid that replaces the linear per-fragment pool loop with a per-cell index list, cutting worst-case iterations at the busy mc2_10 night pose from ~200 to ≤20. `=0` uploads `u_lightGridDim=0` and the shader takes the original linear loop. `MC2_LIGHT_GRID_DIM=N` tunes cells/axis (default 64). Inert below 8 packed lights and whenever `MC2_TERRAIN_LIGHT_POOLS=0`. Exact, not approximate — each cell list is a conservative superset of the true contributors.
- `MC2_LIGHT_REALISM=0` — night lighting realism reshape, **DEFAULT ON since CONE-SPILL-POLICY-1 (2026-08-01)**; `=0` reverts to the legacy hard-edged pools/cones (LIGHT-REALISM-1). The shipped shape is user-approved **variant B** and measured **free** with the default light grid (see cost note below). Widens each pool light's ground falloff (`MC2_LIGHT_REALISM_POOL_SPILL`, default `2.2`, user-approved variant B) with a soft core (`MC2_LIGHT_REALISM_POOL_CORE`, default `0.0`) and softens the spotlight cone billboard (`MC2_LIGHT_REALISM_CONE_OPACITY`, default `0.40`, user-approved variant B). Covers cone transparency, beacon ground discs and pool continuity. OFF = every path skipped = byte-identical. **Cost (PERF-HARNESS-TRUTH-1, mc2_10 busy night pose, 3840×2160):** with `MC2_LIGHT_GRID=1` (the default) it is **free** — p50 `6.405 ms` ON vs `6.454 ms` OFF, within run-to-run noise — because the grid holds the per-fragment loop to `avg_cell 1.70` even though the reshape **triples** grid stamping (`1962 → 6945`, `peak_cell 17 → 33`). With `MC2_LIGHT_GRID=0` the same gate costs **+31%** (`8.732 ms`). The grid is what makes it affordable; do not enable this gate with the grid killswitched.

## Mech PBR lighting (StandardLit GGX path)

- `MC2_MECH_SURFACE_MATERIAL` — surface material profile for mechs. Default **unset** = passthrough (no surface material). Explicit values:
  - `=metal061b` — Metal061B normal+ORM as substrate + PaintedMetal003 paint layer (debug/exposed-metal look; strong PBR)
  - `=painted_subtle` — ORM-only (flat normal, no perturbation) using Metal061B roughness/metalness. Soft defaults: metallic=0.03, roughness 0.65–0.90, wear=0.0, tile_scale=2.0. For "mech catches light better" not "mech looks like raw metal".
- `MC2_MECH_AMBIENT_V1` — hemisphere ambient fill (legacy Blinn path only; no effect when StandardLit ON). Default **ON**. Kill=`=0`.
- `MC2_MECH_SPECULAR_V1` — Blinn specular sheen (legacy Blinn path only). Default **ON**. Kill=`=0`.
- `MC2_MECH_VIEWUNIFORMS` — mech ViewUniforms block binding (required for StandardLit + specular). Default **ON**. Kill=`=0`.

## Building animation gate (BLDG-TYPE-ANIM-GATE-FIX-1)

- `MC2_BLDG_TYPE_ANIM_STATIC_ELIGIBLE` — Default **ON**. When enabled, buildings with animation TYPE data but `bdAnimationState==-1` are eligible for the static fast path (`touch()` instead of `TransformMultiShape`). Set `=0` to restore legacy behaviour (`bldgTypeHasAnimations` disqualifies the whole type). Gate logic and env-var read in `mclib/bdactor.cpp`; `[ANIM_GATE v1]` summary lines emitted by `g_staticUpdateEmitSummary()` in `code/terrobj.cpp` every 600 frames.

## Debug state dump

- `MC2_DEBUG_STATE_DUMP=1` — JSON render-state snapshot every 300 frames to `debug_state/latest_render_state.json`. Default **OFF**.
- `MC2_DEBUG_STATE_DUMP_DIR=<path>` — override output dir.
- `MC2_DEBUG_STATE_DUMP_HISTORY=1` — rolling 8-slot history ring. Requires `MC2_DEBUG_STATE_DUMP=1`.

## Dev command socket (DEV-SHELL-1)

- `MC2_DEV_SHELL=1` — localhost dev command socket 127.0.0.1:9877 (write half of the debug-state bridge). Default **OFF** (stock byte-identical). Commands: ping / reload_shaders / screenshot / last_screenshot / framegraph / texture_refresh / get_gate / set_gate / quit. Client: `tools/dev_shell/mc2_cmd.py`. Commands execute on the main thread at the frame-loop poll point.
- `framegraph` command — JSON dump of the pass graph: contract table passes+declared reads/writes, derived producer→consumer edges, last-frame runtime rows (GL state, draw counts, GPU ms). `--collect true` flips the gos_frame_pass_stats + gos_render_pass_timer in-process collect flags live (no env restart).
- `ui_reload` command — re-reads the ACTIVE front-end screen's loose `.fit` from `data/art/` and re-inits it in place (LogisticsScreen::init self-clears). Layout edits (positions/rects/buttons/text defs) apply live; changed `.tga` pixels reload via `texture_refresh`. Single-player grid + main menu; registered from MissionBegin via `gos_dev_shell::registerCommand` (the generic upper-layer extension seam).
- `texture_refresh` command — `--name <substring>` re-reads the loose source file from disk for every txmmgr node whose path contains the substring (case-insensitive), replaces the system-RAM copy (same read/LZ path as loadTexture), and destroys the resident GL texture so the next use re-uploads fresh pixels. Node indices stay stable. Reply `{refreshed, skipped_no_file}`; FST-only textures are skipped — drop a loose copy (loose overrides FST) and refresh again. Caveat: a `.ktx2` BC7 sidecar next to the `.tga` still wins at re-upload (`MC2_TEXMGR_COMPRESSED_UPLOAD`).
- `MC2_DEV_SHELL_PORT=<n>` — port override.

## Front-end direct boot (pre-existing capture harness; pairs with dev shell)

- `MC2_BOOT_TO_BAY=<campaign fit basename>` (e.g. `campaign`) — auto-start that campaign from the main menu, no clicks. Note: campaign mission 1 skips logistics (goes straight to mission); pair with `MC2_BOOT_TO_MISSION`.
- `MC2_BOOT_TO_MISSION=<mission FileName>` (e.g. `mc2_03`) — jump to that mission's logistics stage.
- `MC2_BOOT_TO_SCREEN=purchase|bay|loadout|launch` — which logistics grid cell to land on (default bay).
- Verified combo for GUI iteration: `MC2_DEV_SHELL=1 MC2_NO_LAUNCHER=1 MC2_BOOT_TO_BAY=campaign MC2_BOOT_TO_MISSION=mc2_03 MC2_BOOT_TO_SCREEN=bay` then `mc2_cmd.py screenshot --source backbuffer`.
## UI aspect anchor (UI-ASPECT-ANCHOR-1)

- `MC2_UI_ASPECT_ANCHOR=0` — killswitch for the 16:9 UI canvas. Default **ON**
  (ui-phase1 branch): front-end frames (MainMenu/MissionBegin render assert
  `gos_SetUiCanvasActive`, auto-cleared at flushHUDBatch) draw the whole UI —
  legacy HUD-batch layer AND defs/ImGui pages — into a centered 16:9 canvas.
  Wider displays: black flanks. Narrower: letterbox, scaled down. Exactly 16:9:
  identity. Mouse normalize (`gos_GetMouseInfo`) is canvas-relative on those
  frames only; mission frames never assert, so world pick is untouched. `=0`
  restores full-window stretch everywhere.
  In-mission: `gos_SetHudCanvasActive` (mission.cpp, beside the HUD-shrink
  toggle) remaps non-scaleExempt HUD chrome into the same canvas — wider
  displays show extra terrain in the flanks; cursor, modal dialogs and world
  pick stay full-surface. HUD hit-tests go through getMouseHudX/Y
  (gos_HudInverseMousePoint inverts canvas + shrink). ImGui HUD port seam:
  gos_ComputeHudCanvasBox (see gameos.hpp note).

## Free window resize (FREE-RESIZE-1) + native camera aspect

- `MC2_WINDOW_RESIZABLE=0` — kill the resizable window. Default **ON** for ANY
  windowed boot (ISSUE-49 residual fix: `MC2_WINDOWED` env var OR options.cfg
  `FullScreen=FALSE` — `create_window` now receives the engine's windowed boot
  state): the SDL window is user-resizable; SIZE_CHANGED refreshes
  `Environment.drawableWidth/Height` + ImGui display size, and every consumer
  (postprocess FBOs, UI canvas box, mouse normalize, camera aspect) re-derives
  from drawable per frame — drag to any size and shape, everything adapts.
  Boot-time proof line under `MC2_RES_DIAG`: `[RES_DIAG] window flags:
  resizable=… windowed_env=… windowed_hint=…`.
- `MC2_CAMERA_ASPECT_NATIVE=0` — restore the legacy stretched 4:3 frustum.
  Default **ON**: the in-mission perspective projection uses the drawable
  aspect (true proportions at any window shape); panel previews keep their
  fixed 4:3 FBO ratio via the preview-scope guard.

## Gameplay camera smoothing (CAMERA-SMOOTHING-1)

- `MC2_CAMERA_SMOOTH=<ms>` — time constant of the frame-rate-independent
  exponential approach the gameplay camera uses for keyboard/edge scroll,
  rotation, and wheel zoom (`mclib/camera.cpp`). Default **100** (unset);
  `0` = legacy verbatim (hard per-frame scroll steps, 11.25-degree rotation
  snaps, fixed 20/s zoom lerp). Clamped [0, 2000]. Input accumulates a GOAL
  pose (goal clamped with `setPosition`'s map-bound rule; altitude clamps
  unchanged on the desired value); `Camera::update()` eases the actual pose
  toward it with `1-exp(-dt/tau)`. Hard-set paths (bookmark apply, scripted
  swoop/intro, target tracking) never arm smoothing, and any externally
  caused pose change drops the goal — captures stay deterministic.
- `MC2_CAMERA_SMOOTH_TRACE=1` — append per-frame pose deltas to
  `cam_smooth_trace.csv` (working dir) whenever the camera moves.
- `MC2_CAM_SMOOTH_TEST=1` — synthetic input driver: frames 120-240 feed
  `moveRight` at 2000 wu/s plus one 11.25-degree `rotateLeft` snap at frame
  150, then input stops. Pair with the trace to prove ramp-in/tail-out.

## ENV-DROP registration sweep (2026-07-07)

Gates found in source but missing from check-env-registry.sh (retired in G8; the gate table is the registry) (now allowlisted, batch 8). All default-OFF unless noted.

- `MC2_GAMEOS_UI_DEFS=0` — force the legacy GameOS FIT UI path. Default **ON** (data/defs is the replacement path, not an opt-in). `gui/UiDefs.cpp`.
- `MC2_EDITOR_UI_DEFS=0` — force the legacy pure-MFC editor shell. Default **ON** (editor shell overlay is data/defs-first).
- `MC2_UI_DEFS_LOADSCREEN=1` / `MC2_UI_DEFS_RESULTS=1` — opt the loadscreen pages / mission-results page back into data/defs UI (both pinned to legacy by default until authored).
- `MC2_UI_DEFS_IGNORE_PACKAGE_VERSION=1` — bypass the UI defs package version check (modder iteration escape hatch).
- `MC2_UI_DEFS_TRACE=1` — UI defs resolve/replacement trace (`gui/UiDefs.cpp`, `GuiRuntime/GuiRuntime.cpp`).
- `MC2_DISABLE_GOSFX=1` — disable the original gosFX/MLR particle leaves (mlr_gate). Default **OFF** (FX enabled); only `=1` disables, and the Graphics Options window shows the state.
- `MC2_STATIC_PROP_TRACE=1` — static-prop registry trace and the one-shot registered-types dump. Default **OFF**; set, non-empty and not `0` enables.
- `MC2_FORCE_43=1` — forced 4:3 letterbox composite box (launcher "Force 4:3" toggle; unset = full surface, byte-identical).
- `MC2_HUD_SCALE=<0.2..1.0>` — HUD scale env seed; the ImGui slider wins after startup.
- `MC2_PREVIEW_FBO_SCALE=<1..8>` — camera-preview FBO supersample factor (default **4** = 3200x2400).
- `MC2_PREVIEW_NOCULL=1` — panel-preview force-disable `GL_CULL_FACE` (FBO winding-order debug).
- `MC2_PNG_MAGENTA_KEY=0` — killswitch for PNG magenta→alpha keying at decode. Default **ON**.
- `MC2_LEGACY_LOAD_BANNER=1` — restore the retired world-floating blue load banner overlay.
- `MC2_SHOT_RESULTS=1` — RESULTS-CAPTURE-1: deterministic AAR screenshots of results screens after 30 settled frames (pairs with `MC2_DEV_SHELL=1`).
- `MC2_SOAK_STOP_AT_BAY=1` — soak harness parks at the mech bay instead of auto-advancing (requires `MC2_SOAK_AUTOWIN`).
- `MC2_DRAW_PACKET_V3=1` — static-prop DrawPacket v3 candidate→`RenderCore::DrawPacket[]` ABI promotion (no dispatch; legacy flush unchanged).
- `MC2_STATIC_C1B_DRAW=1` — re-enable the (architecturally buggy) C1b GPU indirect instance count for the per-type legacy static-prop draw loop.
- `MC2_REGFLUSH_GUARD2=1` — static-registry persistent-vanish discriminator: per-typeID stale buckets + consecutive-stale streaks (streak ≥16 frames = real drop, not transient cull). Per-range map cost when on.
- `MC2_M2D_PZ_PARITY=1` — quad `pz` clip-path vs legacy-formula parity probe (demoted-not-deleted after green).
- `MC2_3DVIEW_DIAG=1` — one-shot 3D-view composite diagnostics (`gameos_graphics.cpp`).
- `MC2_BOOKMARK_SAVE_DIR=<dir>` — output dir for the in-game "Save Camera Bookmark" inspector button (default `saved_bookmarks/` under the working dir); files are bookmark-schema JSON directly consumable by `MC2_VISUAL_BOOKMARK_CAPTURE`. `MC2_BOOKMARK_SAVE_TEST=1` auto-fires one save ~120 frames after mission load (round-trip proof hook).
- `MC2_WATER_S6_TRACE=1` / `MC2_WATER_S6_COST=1` — water S6 armed-skip ownership transitions / setupTextures QPC cost summary every 600 frames.

## Terrain fast-path drop log

- `MC2_FASTPATH_DROP_LOG=1` — log terrain fast-path -> legacy setupTextures transitions (transition-only, `[FASTPATH_DROP]`). Default **OFF**.

## Terrain gates

### MC2_TERRAIN_NORMAL_ARRAY

**Default:** off

Switches terrain normal-map sampling from 5 individual `sampler2D` uniforms
(units 5–8, 12) to a single `sampler2DArray` on unit 5. Packs all 5
per-material normal maps (rock/grass/dirt/concrete/snow) into one `GL_RGBA8`
array texture, rebuilt once on texture slot change (lazy, at first draw).
Copies source mip levels from each individual texture rather than regenerating
them, preserving authored displacement-alpha behavior.

**Ceiling removed:** per-material texture-unit ceiling at the sampler-binding
layer. The classifier/blending ceiling (vec4 matWeights, 5 named layers,
separate snow logic) is a separate future task.

**When to enable:** when adding more than 8 terrain materials, or as a
long-term cleanup of the individual-sampler path. After a soak period, delete
the `#else` branches (see post-migration section in the plan).

**Safety:** `buildTerrainNormalArray()` saves/restores all GL pixel-store, PBO,
active texture unit, and texture binding state via `GlPixelStoreGuard`. Safe to
call from inside a draw-bind path.

### MC2_RUNWAY_EDGE_BLEND

**Default:** off (killswitch)

RUNWAY-EDGE-BLEND-1 (MASTER-FAIL #6). Cures the hard full-tile stairstep cliff
where a **solid** cement pad/runway tile abuts **bare terrain** with no authored
transition art (the MISSING-RUNWAY / airbase-perimeter class). Indirect terrain
path only (`gos_terrain.frag`); the LOD-chunk path (`terrain_lod_chunk.frag`)
already marching-squares-shapes its transition tiles.

> **Revision history — read this before citing the mechanism.**
> **REV-1 (erosion) was USER-REJECTED** and is no longer what ships. Rev-1 ramped
> cement *out toward terrain **inside*** the solid pad tile over its outer half
> tile, which dissolved the apron's outer edge and read as a blurry dirty rim.
> User verdict on the mc2_01 day apron A/B: *"gate on is worse. the answer is
> usually MORE concrete, not less."* **REV-2 (dilation) is what ships** —
> `8e59de64`, reach widened 0.5 → 1.0 tile by `3c6aad71`. Same gate, same
> default-OFF killswitch, same CPU mask bake.

Mechanism (**rev-2, shipped**): `PopulateRecipeCementWords()`
(`gos_terrain_indirect.cpp`) runs a second pass at mission load that packs a
4-neighbour "open border" mask into `cementWord` bits 19:16 for solid cement
boundary tiles — an edge is "open" only when the neighbour carries **no** valid
cement word (neighbours with any valid word, solid OR transition, count as
cement, so authored transition edges are left to the legacy blend).

The shader work then runs on the **bare-terrain** tile, not the cement tile:
solid cement tiles keep **crisp full replacement out to their own boundary (no
erosion)**. If an orthogonal neighbour is a solid cement tile whose baked mask
flags this side open, that neighbour's cement is blended over the terrain at
**full strength at the shared edge**, falling to zero **one tile** out. Alpha is
exactly `1.0` at the seam, so it stays continuous with the crisp cement tile.
**Net effect: the concrete footprint GROWS with a soft outer margin, instead of
shrinking.**

Path note: the dilation lives in `terrain_lod_chunk.frag` (the **LIVE**
renderer). `gos_terrain.frag` (indirect path) had the rev-1 erosion **removed**
and is plain full replacement as in stock — it is per-quad off a thin record
with no neighbour access, so it cannot host the dilation.

**Off = byte-identical stock:** with the gate off the second pass never runs, no
mask bits are written, so no neighbour is ever flagged open and the dilate block
is a no-op. All cost is at mission load; zero per-frame.

**Verify:** runway/airbase poses (day + night). `[RUNWAY_EDGE_BLEND]
event=edge_mask_baked feathered_solid_tiles=N` on stdout confirms the bake armed.

### MC2_LIGHT_BILLBOARD_TWOSIDED

- `MC2_LIGHT_BILLBOARD_TWOSIDED=1` — opt-in, default **OFF** (reader `mclib/tgl.cpp:107`, whose own comment says "default OFF (AUDIT-TRUTH-1)"; documented as a default-**ON** killswitch until ENV-DROP-CLOSE-1, 2026-07-30). Restores legacy backface culling of beacon/runway edge-light glow cards.

**Default:** on (killswitch)

EDGE-LIGHT-BILLBOARD-CULL-1 (MASTER-FAIL #8). Beacon and runway edge-light glow
cards (`data/tgl/runwaylight.tgl` + `red/green/bluebeaconlight.tgl`, all sharing
the `Lightsnflags.tga` atlas, node names `*beaconlight*`) are authored as a single
flat quad with **no** billboarding. As the RTS camera orbits toward top-down, the
quad's face normal flips away from the camera and the legacy per-triangle backface
cull (`TG_Shape::Render`, `mclib/tgl.cpp`) discards it — the light visibly pops out
by view angle. This gate makes shapes flagged `isLightBillboard` (node name contains
`beaconlight`) **two-sided**, exactly like the existing `isSpotlight` exemption, so
the glow stays lit from every orbit angle.

The flag is set once at shape load from the node name, so only beacon/runway lights
are affected — the flags and the barrels/streetlight props that also draw from the
`Lightsnflags` atlas keep stock one-sided culling.

**Scope limit (measured):** this gate only reaches cards that take the legacy CPU
`TG_Shape::Render` path. At runtime the airbase light population renders through
the GPU static-prop batcher instead, so this gate alone is near-inert. See
`MC2_LIGHT_BILLBOARD_FACING` below for the GPU-path cure.

### MC2_LIGHT_BILLBOARD_FACING

- `MC2_LIGHT_BILLBOARD_FACING` unset or `=0` — **DEFAULT: OFF.** Stock
  fixed-orientation light fixtures on the GPU static-prop batcher path. No
  instance carries the flag bit, so the shader block never runs and the frame is
  byte-identical to stock. **This is the clean way to disable the modern
  marker-light treatment entirely.**
- `MC2_LIGHT_BILLBOARD_FACING=1` — opt-in modern treatment: camera-facing
  fixtures with a **capped** minimum screen size.
- `MC2_LIGHT_BILLBOARD_FACING=2` — verification only: renders the flagged cards at
  4x scale so the affected population is unmistakable in a capture.

**Default:** off

> **RUNWAY-MARKER-LIGHTS-1 (default flipped ON → OFF).** This gate shipped
> default-ON in `f412423e` and the user verdict on the result was that the
> runway marker lights were "atrociously hideous" — stark white streaks and
> dashed slashes scattered across the dark terrain at night, brightest exactly
> at map-overview altitude where they should have faded out.
>
> Cause: the shader's minimum-screen-size clamp was `max(1.0, kLbMinNdc/sil)`
> with **no upper bound**. Because `sil` falls off as 1/distance, `k` grew
> without limit and pinned every fixture to a constant ~4-7px hard-edged bright
> triangle at *every* camera distance. Several hundred regularly-spaced runway
> and taxiway fixtures, all refusing to shrink, read as bright dashes over the
> dark valley. A secondary error compounded it: `kLbRefRadius` was set to the
> authored *cap* radius (0.8) when the rigid billboard actually presents the
> local XY cross-section (silhouette radius ~1.2), over-inflating by a further
> ~1.5x.
>
> Fix: the clamp is now `clamp(kLbMinNdc/sil, 1.0, kLbMaxScale)` with
> `kLbMaxScale = 2.5` and the corrected reference radius, so a fixture is still
> guaranteed to survive rasterisation at playable RTS ranges but resumes normal
> 1/distance shrink beyond that and goes sub-pixel at overview altitude on its
> own. The gate default is additionally OFF so stock is what ships.

LIGHT-BILLBOARD-GPU-1 (MASTER-FAIL #8, the comprehensive fix).

**The predecessor lane's diagnosis was wrong on both counts.** These props are
not flat quads, and they are not being backface-culled. Reading
`data/tgl/runwaylight.ase`, each is a 6-vertex / 7-face **truncated triangular
prism** about 2.85 units tall whose only emissive element is a small
**horizontal top-cap triangle**. And they render through the GPU static-prop
batcher, not the legacy `TG_Shape::Render` loop that `MC2_LIGHT_BILLBOARD_TWOSIDED`
patched — which is why that gate measured near-null.

The real mechanism, reproduced stock from a fixed camera **position** with only
the pitch varied (mc2_10 night airbase, bookmarks 204753 vs 204907): the cap is
horizontal, so its projected area falls off with view angle, and the whole
fixture already subtends only 1-2 pixels at RTS ranges. At grazing angles the
cap drops below one pixel and is never rasterised — the entire runway light
population disappears at once. This is a **sub-pixel geometry** problem, so the
originally-deferred "two-sided GPU draw bucket" would not have fixed it either.

The cure, in `8064c6930:shaders/static_prop.vert` under per-instance flag **bit 4**
(bit 5 = the exaggeration debug bit), is two parts:

1. **Rigid camera-facing billboard.** The fixture's LOCAL vertex coordinates are
   re-emitted in an eye-facing basis `(R1, U1, toCam)`, rotating the whole prop
   about its own origin so the cap points at the viewer. Both frames are
   right-handed, so shape, scale and triangle winding are preserved and the cap
   additionally can never be back-face culled. Using the per-VERTEX normal here
   instead rotates each vertex differently and tears the prism apart — that was
   measured (the lights vanished entirely), hence the rigid per-instance form.
2. **Minimum screen size.** Even facing the eye the cap is sub-pixel at range,
   so the fixture is uniformly scaled about its origin until its silhouette
   spans at least `kLbMinNdc` (0.0012 NDC) — the clamp is `max(1.0, ...)`, so it
   is inactive at close range and the geometry there is untouched.

Both static-prop instance flag-emission sites emit the bit (the batcher's
`submitMultiShape()` and the persistent-static recipe path in `mclib/bdactor.cpp`)
— they must stay in lockstep or only half the population is fixed. With the gate
at 0 no instance carries the bit, the shader block never executes, and the render
is byte-identical.

Population covered: `runwaylight` + `red/green/bluebeaconlight` and their
damage/LOD variants (node names `*beaconlight*`, 4-6 vertex glow cards). An audit
of every `*light*` node name in the prop library confirms the remaining
candidates are **solid geometry, not cards** — `streetlight` (12 vert / 14 face
pole) and the `Light` lamp head inside `*spotlightcontrol` (10 vert / 16 face) —
so they are correctly excluded, as are the flags and barrels.

**Off (`=0`) = byte-identical stock:** the two-sided OR-term is skipped and edge-light
cards backface-cull as before.

**Verify:** night runway/airbase pose, rotate the camera through top-down — the edge
lights should remain visible at every angle instead of blinking out.

## Terrain debug / visual

- `MC2_TERRAIN_DEBUG_MODE=N` — terrain frag debug-mode (0=off, 1=DepthCmp, 2=RawColormap, 3=BlurredColormap, 4=MatWeights R=rock/G=grass/B=dirt, 5=NormalLighting, 6=ShadowFactor, 7=CloudShadow, 8=CementDiag, 9=ThinRecordDiag, 10=HeightNormal, -1=TessAliveProbe).
- `MC2_TERRAIN_CLIFF_SHADOW_FLOOR=<float>` — CLIFF SHADOW FLOOR: lifts shadow-side steep terrain cliff faces off near-black by applying `shadow = max(shadow, floor * smoothstep(0.85,0.55,|normal.z|))` in `terrain_lod_chunk.frag` (LodChunk path, mc2_01) and `gos_terrain.frag` (legacy). ImGui-tunable 0..0.6 (Graphics Options → "Cliff Shadow Floor"). Default/unset/`0` = **byte-identical** (no-op). Only steep faces affected; flat terrain untouched.
- `MC2_TERRAIN_AMBIENT_FLOOR=<float>` — **TERRAIN-AMBIENT-FLOOR-2**, **default `0.35` = ON**. Terrain-only, albedo-scaled ambient floor in `terrain_lod_chunk.frag`.
  - **Full revert is this ONE gate**: `MC2_TERRAIN_AMBIENT_FLOOR=0` uploads `0.0`, the frag block is skipped, output is **byte-identical**. There is no second gate. (The predecessor slice shipped two — an ambient floor *and* a diffuse wrap — and documented a one-gate revert that left the primary change running.)
  - **Defect**: on mc2_03 in broad daylight the terrain outside the concrete pads reads near-black while the pads are correctly lit. Not a regression; long-standing.
  - **Mechanism**: terrain has **no additive term at the runtime lighting stage**, so it is exactly `baseColor * normalLight * shadow` — a pure product with nothing bounding it from below — while static props on the same ground get an additive ambient via SH-IBL. Two ~1.7× factors (concrete albedo × the ratified concrete lighting exemption) then multiply that unbounded product into the ~3× pad:ground ratio. **Precision, because the loose version of this claim is wrong**: `baseColor` is not raw albedo — the colormap is pre-burned with the mission light (`terrtxm2.cpp burnInShadows` → `eye->getLightRed(lightFactor)`), so terrain albedo *does* carry baked mission lighting. That is a *multiplicative* factor inside `baseColor`, not a floor, and cannot stop `normalLight` (floor `mix(0.35,1.20,0.02)` = **0.367**) from crushing the product. An "ambient debug view reads 0.0000 over terrain" measurement proves only that `hemiContrib` is zero, which is true by construction when V1 is off.
  - **Term**: `baseColor * ambCol * floor * mix(0.55,1.0,shadow) * dayGate * (1 - flatLitShare)`. Multiplying `baseColor` is the rendering equation's actual ambient form (irradiance × albedo): dark ground stays proportionally dark, so the map keeps its albedo variation instead of washing to grey. The `N.z` hemisphere shaping preserves relief (flat-to-away lighting ratio compresses only ~2.3× → ~2.0×). Absolute lift on typical terrain ≈ **0.044** luma, under a third of the ~0.157 that `MC2_TERRAIN_LIGHTING_V1` would add at strength 1.0 — and V1's term is albedo-*independent*, which is why turning it on instead was rejected (it inflates pads +21–26% and flattens albedo).
  - **Flat-lit surfaces exempt on BOTH paths.** Mask is `flatLitShare = max(cementLightBlend, pavedOverlayFlat) * (1 - u_cementLit)` — literally the fraction of the fragment's lighting that was replaced by the flat `1.0`, built at the lighting site so the ambient's edge *is* the flat-lighting edge. Masking on `pureConcrete` alone lifted user-ratified flat-lit **pavement**: PAVED-OVERLAY-CLASS-1 runway/road tiles are flat-lit by a separate branch and carry `pureConcrete == 0` by construction.
    - **CORRECTED 2026-07-30 (LIGHTING-INCAPABLE-BATCH-3).** This bullet used to end *"…and `PavedOverlayClassMode()` **returns 1 when unset** (the comment above it saying '0 = OFF (default)' is stale — read the reader), so it is live at stock."* **That is false, and it was false when written** — it is the third live copy of the same wrong claim (the others were in `terrain_lod_chunk.frag` and `RendererFeatureRegistry.h`; all three are now fixed). The reader opens `if (!v || !v[0]) return 0;`: CONCRETE-DELIVER-2 reverted the default to `0`. So bit 22 is **never baked at stock**, `pavedOverlayFlat` is false on every default-launch fragment, and the pavement half of this mask is reachable **only** under `MC2_PAVED_OVERLAY_CLASS=1`. There is **no pixel difference** at stock either way (both arms of the branch reduce to the same expression) — the design is still right, the stated reason was not.
  - **Daylight-only, and the gate is proven offline.** `dayGate` comes from `GameOS/gameos/utils/terrain_daylight.h`, keyed on the **authored** sun pitch (`Camera::dayLightPitch`) `min` the engine runtime night state. `tools/terrain_daylight_harness` (12 tests, no launch, no capture) pins it to **exactly 0.000** on every night mission in the shipping data: mc2_10 (pitch 180), **mc2_05 and mc2_21 (pitch 270)**, mc2_19 (pitch 0), mc2_23 (pitch 9) — all byte-identical. Full 1.000 on mc2_03/46°, mc2_01/50°, mc2_24/75°.
  - **Why not the obvious signals.** Mission flat light *cannot reach zero* (`camera.cpp:2028-2036` pins the night path to fixed night values by design) — that is why the predecessor lifted mc2_10's ground **+59.1%**. The **runtime** sun altitude / `isNight` / `nightFactor` are also unusable: `camera.cpp:2033` folds a below-horizon authored sun into a mirrored **moon** pitch and `Camera::update()` re-derives the flags from it, so on mc2_05 the runtime signals report a *noon sun overhead* on a mission the engine itself just classified as night.
  - Bounded `0..2`; junk/NaN clamps to 0. Trace: `MC2_TERRAIN_AMBIENT_FLOOR_TRACE=1` prints one line with the resolved floor, the uploaded `dayGate`, the raw pre-floor `dayGate`, the night floor, and the three runtime inputs.
  - ⚠️ **KNOWN LIMITATION — this fix cannot reach the five maps it is most needed on.** The `dayGate` above is **exactly 0.0000** on **mc2_05, mc2_10, mc2_19, mc2_21 and mc2_23**, which are precisely the five **lowest-multiplier** (0.3670–0.4814) and **highest-floor-pin** (14.3–100% of fragments at `diffuse == 0.02`) maps in the game. A default-ON cure for "terrain is too dark" is therefore switched off on exactly the five maps where terrain is darkest, and those frames are **byte-identical to before it landed**. It is live and worth +22.9% to +31.6% on the other 19. This is not a bug in the gate — a daylight gate *should* be 0 at night — it is a scope limit, and `MC2_TERRAIN_AMBIENT_FLOOR_NIGHT` below is the lever that lifts it.
  - **NOT VISUALLY VERIFIED.** Engine launches were hard-blocked in the implementing lane. The confirming capture matrix is: (1) **mc2_03** `apron_wide` / `apron_lightfacing` / `apron_lightaway` / `apron_low` × {`MC2_TERRAIN_AMBIENT_FLOOR=0`, unset} — expect pad Δ **0.00%** in every pose and ground **+10–25%**; (2) **mc2_10, mc2_05, mc2_21, mc2_19, mc2_23** × 2 poses × {`=0`, unset} — expect **0.00% changed pixels / identical sha256**, which is the gate proof at the pixel level; (3) **mc2_05 and mc2_01** wide poses unset vs the pre-change build — mc2_05 must be identical (user-praised look), mc2_01 must not wash out; (4) `MC2_TERRAIN_AMBIENT_FLOOR_TRACE=1` on mc2_05 — expect `dayGate=0.0000` next to `runtime_isNight=0 runtime_nightFactor=0.000 runtime_lightDirZ=+0.707`, which is the direct evidence that the runtime signals are the ones that lie; (5) mc2_03 with `MC2_PAVED_OVERLAY_CLASS=1` explicit — pavement Δ **0.00%**.
- `MC2_TERRAIN_AMBIENT_FLOOR_NIGHT=0.0..1.0` — **TERRAIN-AMBIENT-FLOOR-NIGHT-1**, **default `0.0` = OFF = byte-identical.** A **floor under** `MC2_TERRAIN_AMBIENT_FLOOR`'s daylight gate: the uploaded gate becomes `max(dayGate, floor)`. At `0.0` that is `max(dayGate, 0) == dayGate`, so every mission is unchanged — that is the complete revert, and there is no second gate.
  - **Why:** see the ⚠️ limitation above. The shipped default-ON ambient floor is zeroed on the five darkest maps. This is the only lever that lets it reach them.
  - **Shape — a floor, not a night branch.** Monotone, so there is no step at the dusk-band edge (a mission at `dayGate = 0.2` with floor `0.3` lands on `0.3`, not on a jump from `0.001`); and it composes with `MC2_TERRAIN_AMBIENT_FLOOR` rather than duplicating it — strength stays one number, this is the fraction of it night maps are allowed.
  - ⛔ **Default OFF, and it must stay OFF until the user grades it.** This is a taste call, not a regression fix. The predecessor slice (TERRAIN-AMBIENT-FLOOR-1) was rejected at merge review for lifting mc2_10's ground **+59.1%**, and the user has explicitly **praised how mc2_05 currently looks**.
  - **Flip criterion — where an A/B actually shows something** (saturation checked first, so this is not a recipe pointing at a mission where nothing can move): the ambient term is **additive**, so the `normalLight` floor pin does not block it, and terrain's headroom to clip on these maps is **9.07× (mc2_10) to 20.83× (mc2_21)** — nothing saturates. Grade **mc2_05** and **mc2_10** from a ground-level pose carrying **both bare terrain and a concrete pad** in frame, `0` vs `0.30`. Expect bare terrain up and pads **Δ 0.00%** (`flatLitShare` exempts them) — i.e. the pad:ground ratio closing from the *terrain* side only. A pad delta above zero means the exemption mask broke and is a stop.
  - Bounded `0..1`; junk/NaN clamps to **0** (fail-dark — a visual floor that fails bright is exactly the predecessor's rejected regression). Confirm it armed with `MC2_TERRAIN_AMBIENT_FLOOR_TRACE=1`: on mc2_05 expect `raw_dayGate=0.0000 night_floor=0.3000 dayGate=0.3000`.
  - **NOT VISUALLY VERIFIED.** Engine launches were hard-blocked in the implementing lane.
- `MC2_TERRAIN_AMBIENT_FLOOR_TRACE=1` — TERRAIN-AMBIENT-FLOOR-2 trace: one line at the first terrain-chunk uniform upload — resolved floor, uploaded `dayGate`, the raw pre-floor `dayGate` and the `MC2_TERRAIN_AMBIENT_FLOOR_NIGHT` floor, raw env strings, and the runtime `isNight` / `nightFactor` / `terrainLightDir.z`. Prints the runtime signals deliberately *alongside* the uploaded gate, because those are the values that mislead on below-horizon missions. Default **OFF**; output only, no pixels change.
- `MC2_TERRAIN_MATERIAL_TEXTURES=1` — TERRAIN-MATERIAL-TEXTURES-1: per-layer PBR albedo on the LOD-chunk terrain. Loads `data/terrain_layers/<channel>_albedo.ktx2` (rock/grass/dirt/concrete/snow/cliff, BC7 **sRGB**, mip-complete) into a 6-layer `GL_TEXTURE_2D_ARRAY` (`GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM` — the sampler does the sRGB decode) and composes it world-space by matWeights/snow with the material-lib per-layer tiling; the colormap stays the MACRO tint (2× multiply). Steep slopes take the CLIFF layer triplanar (cliffs read as real rock). Default **OFF** = byte-identical (`u_useMatAlbedo=0`, tint path verbatim). Fail-soft: bad/missing layer files log `[TERRAIN_MAT_TEX]` and keep the legacy look. JSON (`terrain_materials.json`): `matAlbedoStrength`, `textureRoot`, `layers.<channel>.albedo` path overrides (TinyJson nested-layers extension). VRAM ≈ 32 MiB.
- `MC2_TERRAIN_MATERIAL_TEXTURES_STRENGTH=F` — albedo mix strength override, clamped [0,1] (0 = legacy tinted colormap, 1 = full textured composite). Wins over JSON `matAlbedoStrength`; default chain env > JSON > **0.7**. Only consumed when the gate is on.
- `MC2_TERRAIN_SHORELINE=1` — land-side wet/foam shoreline band (TERRAIN-SHORELINE-V3), placed by **elevation** in the frag (`v_worldPos.z` vs `u_waterElevation` — the same source the water fast path uses) so the band hugs the RENDERED waterline by construction, at any LOD/slope. No sidecar is required. Default **ON** since SHORELINE-SMOOTHING-REVISIT-1 (deferred-sweep); `=0` turns the terrain-side band off. **CORRECTED by SHORELINE-GLOW-1** — this row previously claimed two things that were not true. (a) It said the gate "suppresses the legacy screen-space `runShoreline()` post pass whenever this gate is on". It did not: the suppression's only input, `gos_TerrainLodChunk_IsShorelineMaskActive()`, was a third reader of this variable that kept the pre-flip default-OFF idiom, so on a stock launch the V3 band and the legacy pass both ran. All readers now share one accessor. *(Updated by LEGACY-SHORELINE-MISAIM-1: the suppression is no longer opt-in **and no longer keyed on this variable at all** — it is `MC2_SHORELINE_LEGACY_SUPPRESS` alone, default ON. Setting `MC2_TERRAIN_SHORELINE=0` therefore no longer brings the legacy pass back.)* (b) It described the legacy pass as tracing "the tile-quantized water edge". It cannot reach water at all — see `MC2_SHORELINE_LEGACY_SUPPRESS` for the shader-by-shader reason.
- **options.cfg `ShorelineLegacySuppress`** (this was the env gate `MC2_SHORELINE_LEGACY_SUPPRESS`, deleted 2026-10-12: the value is now the options.cfg key `ShorelineLegacySuppress`, read once at startup; `=0`/`=1` in the text below means `FALSE`/`TRUE` (or the number); `MC2_OPTIONS_OVERRIDE="ShorelineLegacySuppress=<value>"` sets it for one run without editing the file) — **DEFAULT FLIPPED OFF → ON by LEGACY-SHORELINE-MISAIM-1 (2026-07-31).** The legacy screen-space `runShoreline()` post pass is now **suppressed on a default launch**; `=0` is the only revert token and restores it exactly as it shipped. This row previously read "`=1` … Default **OFF**, so stock is byte-identical to the shipped build" — that is no longer true and the sentence is replaced rather than left standing. **Why the flip, against SHORELINE-GLOW-1's own explicit "kept opt-in" note:** the brightness being removed is *animated* — `shoreline.frag` scrolls its fbm by `time` and pulses it with `sin(time*2.0)` — so the shipped behaviour is a shimmer crawling along the silhouette of a **static building**. That is not a brightness judgement call; no grading pass would have kept it. Holding a mis-aimed pass on by default while waiting for a capture that this lane cannot take is the bargain that produced the incapable-fix list this flip comes from. **The pass is NOT deleted, and this gate is not a no-op:** `setShorelineSuppressedByTerrainMask()` is still called only from the LOD-chunk terrain draw, so under `MC2_TERRAIN_LOD_CHUNK=0` the setter never runs and the pass runs **unsuppressed**. **⚠ CORRECTED by SHORELINE-DISPOSITION-1 (2026-07-31):** this sentence used to end "…the legacy terrain shader draws, its continuous `materialAlpha` *does* land in the 0.15–0.35 band, and the pass still runs and still foams water — the one configuration it was written for." **That was false, and it was the stated reason the pass was kept rather than deleted.** `gos_terrain.frag`'s two `GBuffer1` writes (`:1103`, `:1156`) sit inside `#ifdef MRT_ENABLED`; `#define MRT_ENABLED` is injected from **exactly one** C++ site (`gameos_graphics.cpp:495`, `getMaterialVariation()` → `gosRenderMaterial::load()`), so those lines are preprocessed **out** of the thin / surface / mask_solid programs built from the same `.frag`; and the one program that does receive the macro, `terrain_material_`, is created and destroyed but **never `apply()`'d** — its draw was removed by TERRAIN-DEADCODE-DELETE-1 (`75b8ebefe`). **There is therefore NO configuration in which the 0.15–0.35 classifier matches a water pixel.** Under `MC2_TERRAIN_LOD_CHUNK=0` the pass does not foam water; it applies the prop-silhouette rim unsuppressed. `=0` restores that rim, not shoreline foam. Pinned by Census 4 of `scripts/check-render-contract-gbuffer1.sh`. **Disposition: the pass should be deleted** — see that Census 4 block for the reasoning and `gos_TerrainShorelineLegacySuppressOn()` for the retirement note. **Also changed:** the suppression no longer ANDs with `MC2_TERRAIN_SHORELINE`; that coupling meant turning the V3 band off silently restored the prop rim. **Flip-back A/B:** any mission with buildings or fences near terrain (mc2_01, mc2_03) with a **moving** camera — the artifact is temporal and a single still frame understates it. **What the legacy pass actually does, since every prior note about it was wrong:** `shaders/shoreline.frag:22` picks its pixels with `GBuffer1.a > 0.15 && < 0.35`, and on the production path nothing water-like is in that band — the MDI water frag (`gos_terrain_water_mdi.frag`) and the non-MDI water fast-path frag (`gos_tex_vertex.frag`) both write alpha **0.0**, `terrain_lod_chunk.frag` writes alpha **1.0**, and the MRT1 clear is **0.0**. A census of every fragment shader writing attachment 1 leaves only `static_prop.frag` (0.25 solid prop, 0.1875 alpha-test lattice) inside the band, plus `gos_terrain.frag`'s continuous `materialAlpha` — which belongs to the legacy terrain shader and **never reaches a framebuffer in any configuration** (SHORELINE-DISPOSITION-1: this clause used to read "is not drawing while `MC2_TERRAIN_LOD_CHUNK` is default-ON", which wrongly implied it draws when that gate is off — it does not). So the pass applies an animated fbm foam brighten (up to 1.4× multiplicative) to **static-prop pixels within ~6 screen texels of terrain** — a shimmering rim where buildings and fences meet the ground. Suppressing it removes that rim and changes no water pixel. *(Original SHORELINE-GLOW-1 rationale, preserved so the reversal can be judged against it: "Kept opt-in because it removes brightness from prop edges while a dark-props report is open, and no capture has graded it." Both facts are still true; see the flip argument above for why they no longer carry.)*
- `MC2_TERRAIN_SHORELINE_FILE=<path>` — override the OPTIONAL mask sidecar path (default `data/missions/<stem>.beauty/shoreline_mask.png`), offline-cooked by `tools/terrain_beautify/cook_shoreline.py`. When present, its G/B channels MODULATE the elevation bands (wide-beach falloff / basin exclusion); when absent, the gate alone still produces full elevation bands.
- `MC2_TERRAIN_SHORELINE_STRENGTH=F` — wet/damp darken intensity multiplier, clamped [0,2]. Default 1.0.
- `MC2_TERRAIN_SHORELINE_FOAM=F` — foam rim intensity multiplier, clamped [0,2]. Default 1.0.
- `MC2_TERRAIN_SHORELINE_WET_RUN=F` — V3 wet-lobe width as a **horizontal** world-unit run inland from the drawn waterline (the frag converts vertical rise → horizontal run via the macro slope). Default 16.0 (~4.8 m). **UNIT CHANGE** vs the pre-horizontal-fix knob: values are horizontal runs, not vertical heights.
- `MC2_TERRAIN_SHORELINE_FOAM_RUN=F` — V3 foam-rim width, horizontal world-unit run from the waterline. Default 5.0 (~1.5 m). Same unit change as `_WET_RUN`.
- `MC2_TERRAIN_SHORELINE_WET_HEIGHT=F` / `MC2_TERRAIN_SHORELINE_FOAM_HEIGHT=F` — **legacy aliases** for `_WET_RUN` / `_FOAM_RUN` (consulted only when the `_RUN` name is unset). NOTE: since the horizontal-distance fix these values are interpreted as horizontal runs, not vertical heights — an old `_WET_HEIGHT=3.0` now means a 3 wu (~0.9 m) run, far narrower than the 16 wu default.
- `MC2_TERRAIN_SHORELINE_EDGE_JITTER=F` — V4-STYLE static world-XY fbm jitter amplitude (horizontal wu) added to the band's distance-from-waterline, so the wet/foam lobes stop tracing the mesh waterline's straight diamond segments (the "zigzag" read). Placement-stable (no time term, camera-independent) and leak-proof (the strictly-above-water clamp uses the un-jittered distance — bands are ALWAYS zero at/below the drawn water plane as of V4). Clamped [0,32]; `0` = exact V3 contour. Default 4.0 (~±3 wu wander).
- `MC2_WATER_SHORE_FEATHER=F` — SHORELINE-STAIRCASE-1 (MASTER-FAIL #1). The **water-body outline** (not the land-side wet band above) snaps to the terrain tile grid — ~140 px axis-aligned steps with dead-straight risers at top-down altitudes — because the water FS shore alpha ramp (`shoreBlend = max(alphaDepth, 1)`) is only ~`alphaDepth` world-units wide, i.e. a 1-2 px transition that reads as a hard tile step. This gate sets a **floor (world units) on the effective `alphaDepth`** used by BOTH the FS shore smoothstep (fed through the water bridge — no `gameos_graphics.cpp` change) AND the beach-ring `shoreExt` geometry (which is exactly `alphaDepth*0.5`, the band's land-side extent, so the two stay matched). A wider band dissolves the staircase into a soft gradient that **hugs the rendered waterline by construction** (the band is driven per-fragment by `WaterThickness = waterElevation − interpolated terrain z`, the same principle as V3), so it auto-widens horizontally on flat shores (where the staircase is worst) and stays tight on cliffs. No sidecar required. Default **unset/≤0 = OFF = byte-identical** (`EffAlphaDepth` is the identity). Applies to both the CPU thin-record and GPU-driven water build paths (predicates kept in lockstep). Recommended live value ≈ **20 wu** (taste range 16-28); at 20 the critical mc2_10 top-down inlet's block-staircase structural signature falls below the shore-staircase sentinel's struct threshold while the shore still reads defined, and the flat mc2_01 shore softens without over-washing. The band's *screen* width scales with shore flatness (a fixed wu floor is wider in screen space on flat shores, tighter on cliffs), so very flat maps read softer at the same value. Larger values (e.g. 80) exaggerate for mechanism verification (full dissolve).
- `MC2_WATER_SHORE_MEANDER=A` / `MC2_WATER_SHORE_MEANDER_FREQ=F` — SHORELINE-GEOMETRY-1 (master-FAIL #1 re-open: the user rejected the feather above as "still a literal stair step"). **The silhouette fix.** Diagnosis: the water FS discard contour is the iso-line of a *bilinearly interpolated* `WaterThickness`, so it is already sub-tile — but the stock heightfield is **terraced** (long runs of shore vertices share one elevation), which degenerates that iso-line onto the tile lattice: each boundary tile contributes one straight segment and those segments chain into the axis-aligned / 45-degree risers the user annotated. Widening the alpha band (`MC2_WATER_SHORE_FEATHER`) blurs the risers but cannot bend them — it is an alpha fix for a geometry-shaped defect. This gate instead perturbs the **field**: it adds a static, zero-mean, world-space 2-octave fBm displacement of amplitude `A` (world units) to the thickness that drives the shore term and the `discard`, so the contour meanders through tile interiors. Horizontal contour displacement is `A / |grad(thickness)|`, i.e. **self-scaling** — large across the flat terraces that produce the worst steps, small on cliffs where the coast is already organic. The noise is **time-invariant** (a swimming coastline would be worse than a stepped one) and keyed on continuous `WorldPos.xy` (seam-free, camera-independent). The tile-admission `shoreExt` widens by `A` in lockstep across all four predicate mirrors (`WaterStream::EffShoreExt` is now the single source of truth for the CPU stream build, both `mclib/terrain.cpp` narrow-append branches, and the `u_shoreExt` fed to `gpu_driven_water.comp`), so water geometry exists wherever the meander can push the contour landward — without that, landward excursions clip back to the tile boundary and the step reappears. Only the shore/alpha term is perturbed; the depth **tint** still uses the true `WaterThickness`, so shallow/deep colour stays keyed to real terrain and only the coastline SHAPE changes. `F` is the spatial frequency in cycles/wu (default `1/384` ≈ one dominant cycle per 3 tiles — long enough to bend a whole riser rather than serrate it). **DEFAULT REVERTED TO 0 = OFF (WATER-MEANDER-CLAMP-1, 2026-07-26).** The AUDIT-TRUTH-1 default of 75 **deletes water**, not just on the flat-shelf map: on `mc2_17` wide pose `shelf_east_wide` it removes 11.2 percentage points of frame water (42.95% -> 31.78%), one-way (11.23% lost vs 0.05% gained) -- the perturbation is zero-mean but `if (shore <= 0.0) discard;` is not, so landward excursions destroy fragments while seaward ones add nothing. It also deletes an inlet on `mc2_10`, the very map the 75 approval was granted on; that went unseen because no mc2_10 bookmark framed water until `tests/visual/bookmarks/mc2_10_water.json` was authored. Cause: `A` is a THICKNESS with fBm gain 2.40, so 75 is a +/-180 wu excursion -- wider than the entire water depth range of either map (mc2_17 120 wu, mc2_10 80 wu). Two rescues were measured and rejected: a horizontal-distance clamp cuts the loss monotonically but is self-defeating (it scales by `|grad(thickness)|`, and the staircase it must cure lives where `|grad|` is small, so it suppresses the cure exactly where it is needed, and it facets per-primitive); and lowering `A` is non-monotonic (at `A=15` mc2_10 grows a blocky staircase neither `A=0` nor `A=75` has, because `EffShoreExt` widens tile admission BY `A`, so a small `A` clips landward excursions back to tile edges). Set `MC2_WATER_SHORE_MEANDER=75` to restore the previous behaviour exactly. Default `A` unset/≤0 = **OFF = byte-identical** (FS skips the term, `EffShoreExt` collapses to the stock expression). GL path only (the Vulkan water island reads `alphaDepth` via its own push constant and is unaffected).
- `MC2_WATER_SHORE_MASK=0` — WATER-SHORE-MASK-1. Default-**ON** killswitch. When a mission carries a cooked `data/missions/<stem>.beauty/shoreline_mask.png` sidecar, the water FS (`gos_terrain_water_mdi.frag` `u_hasShoreMask`) feathers the water-side shore band along the mask's smoothed contour distance, and the terrain FS (`terrain_lod_chunk.frag` `u_hasShorelineMask`) uses the same sidecar to modulate the V3 land-side band. Both uniforms are multiplicative-identity when the sidecar is absent. **DATA STARVATION, stated here because the failure is 100% silent at runtime — the loader `fopen()`s, misses, logs one line and carries on:** only **mc2_17 and mc2_24** have a cooked mask in the git-tracked corpus (`tests/terrain/beautify/`, see `docs/beauty-sidecar-inventory.txt`), so on the other 22 shipping missions this default-ON gate is an exact no-op *by missing data, not by code*. It is therefore not a knob to reach for when a shoreline looks wrong on any other map — `MC2_WATER_SHORE_FEATHER` and the V3 `MC2_TERRAIN_SHORELINE*` family need no sidecar and are the levers that exist everywhere. **A SEPARATE, LOUDER PROBLEM FOUND WHILE VERIFYING THE ABOVE (2026-07-31), recorded here because there is no gate that would have caught it:** the `0.4` deploy lane (`A:/Games/mc2-opengl/mc2-win64-v0.4`) carries a **`mc2_01.beauty/shoreline_mask.png`** (470 KB, dated 2026-07-02) that exists in **no git-tracked cook** — `git log --all -- tests/terrain/beautify/mc2_01/shoreline_mask.png` returns nothing — alongside `overlay_v2.png`, `control_map.png` and `visual_height_mips.r32`, all equally untracked. The install shipped 2026-07-30 (`A:/Games/MC2-OpenGL-CURRENT-2026-07-30`) has **none of those four**: its `mc2_01.beauty/` holds only `visual_damp.r32` and `visual_height_4x.r32`. So mc2_01 has *lost* four beauty sidecars relative to the 0.4 install, and the loss is unreproducible because nothing in the repo can re-cook them. `scripts/check-beauty-sidecars.py` cannot see this: it checks cooked→installed in one direction only, so an installed-file-with-no-cook and an install-to-install regression are both outside its contract (and the 2026-07-30 tree is not in its `LANES` list at all). If mc2_01 water or ground looks different from the 0.4 build, this is the first thing to check, and it is a DATA question, not a lighting one. **What would unblock it:** `py -3 tools/terrain_beautify/cook_shoreline.py` per mission (it needs `[Water].Elevation` from the mission `.fit` plus a height source — the hi-res `visual_height_4x.r32` when present, else the coarse pak grid), then `py -3 scripts/check-beauty-sidecars.py --install` to reach the deploy trees. Cooking is an authoring decision that changes what those maps look like on a default launch, so it wants a graded pass per map rather than a bulk cook. Install coverage for what *is* cooked is enforced by `scripts/check-beauty-sidecars.py`.
- `MC2_WATER_SHORE_MODE=N` / `MC2_WATER_SHORE_FEATHER_WU=K` — WATER-SHORE-EDGE-1. Selects how the water FS (`gos_terrain_water_mdi.frag`) computes the shore-edge alpha term, resolved ONCE engine-wide by `gos_WaterShoreMode()` (loader in `mclib/terrain.cpp` and draw path in `gameos_graphics.cpp` cannot disagree). `0` = legacy per-vertex `WaterThickness` ramp only (its iso-line degenerates onto the 128wu tile lattice — at the mc2_17 `shelf_east_wide` pose this is a huge soft fade, on terraced maps a staircase); `1` = the WATER-SHORE-MASK-1 cooked-mask feather (contour still steps ~8wu — the residual staircase the recon graded); `3` = **default** (SHORE-FIELD-1, 2026-10-18) — edge AND deep/shallow depth tint from one smooth field: the cooked `shore_field.r16f` sidecar (`tools/terrain_beautify/cook_shore_field.py`: 16x cubic upsample, threshold at the water level, open/close, signed distance field smoothed inside a coastal band, baked as a float16 pseudo-height) else `visual_height_4x.r32`, Catmull-Rom sampled; the water plane is flat and depth-lifted (`MC2_WATER_SHORE3_DEPTH_K`); no sidecar -> falls back to 2/1/0. `2` = per-fragment signed height: the hi-res `visual_height_4x.r32` sidecar is uploaded as an R32F GL_LINEAR texture (`gos_TerrainLodChunk_UploadWaterShoreHeight`) and the FS feathers on `shoreDist = waterElevation - h(WorldPos.xy)` with `shore = smoothstep(-K, K*0.5, shoreDist)`, `K` = `MC2_WATER_SHORE_FEATHER_WU` (default **7** wu), `min()`'d with the depth term so it can only soften the edge, never extend water onto land. **Data-driven fallback**: no height sidecar → mode-1 behaviour; no mask either → mode 0 — missing data always degrades to shipped pixels (loader logs `[WATER_SHORE_EDGE v1]`). Back-compat: `MC2_WATER_SHORE_MASK=0` with `MC2_WATER_SHORE_MODE` unset still means mode 0. The mask sidecar load is now decoupled from `MC2_TERRAIN_SHORELINE` (previously terrain-band OFF silently killed the water feather too).
- `MC2_WATER_SHINE=F` — WATER-SUN-SPEC-2. Blinn-Phong **sun** specular glint on the water plane. Value is an **intensity**, not a boolean: unset or `0` → 0.0 = exact no-op (default **OFF**); any other value is `atof()`'d and used verbatim (a non-numeric truthy string falls back to 0.25). Recommended **0.2–0.5**; `1.0` is deliberately exaggerated and is the value to use when confirming the mechanism works at all. **This gate did nothing at all from `c50e99a6` until 2026-07-31.** WATER-SUN-SPEC-1 shipped its C++ half, its env var and this knob while its shader half was reverted in session review: `gameos_graphics.cpp` uploaded `terrainLightDir` and `u_waterSpecIntensity` to the MDI water program every frame, and **neither uniform had ever been declared in any shader in this repo's history** (`git log -S"u_waterSpecIntensity" --all -- 'shaders/*'` returns zero commits). `glGetUniformLocation` returned −1 for both and the upload helper skips `loc < 0` silently, so headroom was exactly **1.000×** at every value on every mission — and, the part that mattered more than the dead knob, **the water fragment shader had no sun direction at all**, which is why "the sun does not glint off the water" could not be fixed by any lighting tuning. SPEC-2 declares and consumes both uniforms in `gos_terrain_water_mdi.frag` (former GL source, removed in s63), placing the term after the reflection mix and before fog (a highlight is emitted light: it must not be diluted by the sky-tint/reflection `mix()`es, but it must be fogged). It is **not** the existing `GLINT_GAIN` shimmer — that is camera- *and* sun-independent, keyed on the fbm crest alone; this one is the half-vector between the sun and the eye against the wave normal, so it moves when you orbit and vanishes when the sun is behind you. **Arithmetic precondition, so this is not mistaken for a second dead gate:** the term is multiplied by `terrainLightDir.z` and skipped when that is `<= 0`, so it is **exactly 0.0** on every mission whose sun is at or below the horizon — including mc2_05/10/19/21 (LightDirPitch 270/180/0/270). Those are not A/B candidates. **Grade it on** a sun-above-horizon mission with water in frame and the camera pitched *down* toward the water so the half-vector can reach the wave normals — mc2_17 (flat water shelf, bookmark `shelf_east_wide`) and mc2_01 have the most water in frame. A top-down pose shows almost nothing: the lobe is `pow(N·H, 96)` and the sun is not at the camera.

- `MC2_TERRAIN_VISUAL_HEIGHT=1` — loads the 4x-res VISUAL heightfield bake into an SSBO (binding 26) for corner-pinned interior displacement (`mclib/terrain.cpp`). Default **OFF** = no load, no SSBO, byte-identical; also enabled implicitly by `MC2_TERRAIN_VISUAL_DISPLACE=1`. `MC2_TERRAIN_VISUAL_HEIGHT_FILE=<path>` overrides the bake file path.
- `MC2_TERRAIN_LOD_GEOMORPH=0` — TERRAIN-LOD-GEOMORPH-1 killswitch. When the bake shipped `visual_height_mips.r32` (max-preserving per-stride mips appended to binding 26), coarse-band interior verts sample their OWN stride's max mip (silhouette keeps peaks) and geomorph-lerp toward the parent band near the demotion threshold. Rides `MC2_TERRAIN_VISUAL_DISPLACE` (displace OFF or mips absent = inactive, byte-identical); `=0` disables the mips+morph even when the sidecar is present.
- `MC2_TERRAIN_LOD_MORPH_START=F` — geomorph ramp start as a fraction of the LOD band (default 0.6; morph runs over the outer 40% of the band). Clamped [0, 0.95].
- `MC2_TERRAIN_LOD_CHUNK_DIAG=41` — LOD-band tint debug view (LOD0..5 = green/cyan/blue/yellow/orange/red). `=42` — geomorph morph-factor heat (black=own band -> white=parent surface). Exact-value escapes like `=40` (shadow tier).

## Water gates

- `MC2_WATER_SKYTINT=1` — camera-independent sky tint on MDI water. Default **OFF**.
- `MC2_WATER_REFLECTION=0` — SH-L2 sky reflection (camera-dependent). Default **ON**; `=0` is the kill.
  - Both readers say so: `gos_GetWaterReflectionGate()` returns `(v && v[0]=='0') ? 0 : 1`, and `gos_GetWaterReflStrength()` resolves an unset value to **1.5**, not 0 (`GameOS/gameos/gameos_graphics.cpp:2885,2898`, WATER-REFL-DEFAULT-ON).
  - This bullet asserted the opposite for the whole life of that flip. BACKEND-DEFAULT-PARITY-1 found it: the gate-default checker attributed a written claim only when *every* reader of the var was classifiable, and the strength resolver is an idiom it cannot read — so one unreadable sibling made the entire variable unauditable. The rule now compares against the classified readers when they agree.
- `MC2_WATER_REFLECTION_RT=1` — mirrored terrain fill into reflection RT. Default **OFF**.
- `MC2_WATER_DEBUG_MODE=N` — MDI water frag debug (0=Final,1=Tint,2=Alpha,3=Normal,4=Depth,5=Shore,6=Lighting,7=ReflSH,8=ReflRT,9=ReflBlend).

## VFX gates

- `MC2_VFX_DEBUG_MODE=N` — GPU particle frag debug (0=Final,1=Albedo,2=Alpha,3=Kind,4=Overdraw).
- `MC2_VFX_AGE_SAMPLE=1` — sample curves at real m_age. Default **OFF**.
- `MC2_VFX_ORACLE_RENDER=1` — Phase 1 originals-restoration (CardCloud+ShardCloud oracle). Default **OFF**.
- `MC2_VFX_GPU_SIM_CARDCLOUD=1` — CardCloud GPU sim SSBO substrate. Default **OFF**.
- **options.cfg `VfxSoftParticles`** (this was the env gate `MC2_VFX_SOFT_PARTICLES`, deleted 2026-10-12: the value is now the options.cfg key `VfxSoftParticles`, read once at startup; `=0`/`=1` in the text below means `FALSE`/`TRUE` (or the number); `MC2_OPTIONS_OVERRIDE="VfxSoftParticles=<value>"` sets it for one run without editing the file) — depth-fade soft particles. Default **OFF**.
- **options.cfg `VfxLitParticles`** (this was the env gate `MC2_VFX_LIT_PARTICLES`, deleted 2026-10-12: the value is now the options.cfg key `VfxLitParticles`, read once at startup; `=0`/`=1` in the text below means `FALSE`/`TRUE` (or the number); `MC2_OPTIONS_OVERRIDE="VfxLitParticles=<value>"` sets it for one run without editing the file) — ambient+sun*0.5 fill on particles. Default **OFF**.
  tri-batch draws (`gosRenderer::drawTris` IsHUD branch) into one `hudBatch_` entry instead
  of one per triangle (gosFX weapon VFX emit one `gos_DrawTriangles` call per triangle via
  `MLRSortByOrder`, `mclib/mlr/mlrsortbyorder.cpp:589-609`; measured peak 251-349 entries/frame
  under forced multi-mech fire vs a ~14-entry quiet floor). Order-preserving (merges only
  ADJACENT same-state records); guards the bottom-band HUD-shrink decision per triangle.
  Independent of `MC2_VK_VFX` — benefits both the VK native HUD tail and GL replay. **DEFAULT
  ON** (verified: draw-count peak 349->109, `[VK_HUD_AUDIT v1]` vertices= bit-identical at
  matching samples, zero declines/fallbacks, fire-capture matches banked VFX-9 reference);
  `=0` restores the legacy per-triangle path.

## Terrain LOD chunk

- `MC2_TERRAIN_LOD_CHUNK` — chunked heightfield LOD renderer. **DEFAULT ON** (cutover
  2026-06-09 `a7b090be`); set `=0` to opt out (legacy tessellated path, e.g. editor).
  Single-source gate `mc2TerrainLodChunkEnabled()` (terrain.h). Replaces
  makeLists/geometry()/TerrainQuad::draw() + suppresses legacy DrawIndirect.
- `MC2_TERRAIN_LOD_CHUNK_FORCE_LOD=k` — force every chunk block to LOD k (diag).
- `MC2_TERRAIN_LOD_CHUNK_NO_SKIRTS` / `_NO_APRON` / `_NO_STITCH` — disable skirt seal /
  1-ring draw apron / vertex edge-stitch (A/B).
- `MC2_TERRAIN_LOD_CHUNK_SKIRT_MAX=px` — cap skirt depth (default 256).
- `MC2_TERRAIN_LOD_CHUNK_CEMENT_MAXLOD=k` — relax the cement-block LOD0 clamp (default 0).
  - ⚠️ **This gate does not close the runway/apron gap, and it can only ever DARKEN. Recorded 2026-07-30 (LIGHTING-INCAPABLE-BATCH-3); do not re-propose it as the runway fix.**
    - **(a) It cannot reach the runway at stock.** The blend is `pureConcrete`, which is `0` on unclassified runway/road tiles by construction (58% of mc2_10's paved-overlay tiles, 81% of mc2_03's, **100% of the runway at the user's mc2_10 `@204937` pose**). The originating lane captured that runway **bit-identical across all four strengths** (66.51 L at `t = 0, 0.35, 0.6, 1.0`). The one branch that does reach those tiles needs `MC2_PAVED_OVERLAY_CLASS=1`, which is **default `0`**.
    - **(b) On the cement it *does* reach, it is monotonically darkening.** It can only move the multiplier between `normalLight` and `1.0`, and pads/aprons are flat (`N = +Z`), so `normalLight = mix(0.35, 1.20, L.z)`. `mclib/camera.cpp` builds `lightDirection` with **unit-length xy and normalizes after** (the default arm — `MC2_SUN_ELEVATION_FIX` is default-OFF), capping `L.z` at `1/√2`. So flat-ground `normalLight ≤ 0.35 + 0.85/√2 = 0.95104 < 1.0` for **every possible authored sun pitch**, and raising `MC2_CEMENT_LIT` can only lower a flat cement fragment.
    - Consequently the lane's advertised **"24% gap closure"** was achieved **entirely by dimming the apron** (90.43 → 84.63) while the runway did not move at all. Keep default `0.0`. It stays because it is the right knob once classification coverage is fixed — it is not a brightener and not a runway fix.

  The `[CEMENT_CONCRETE v1] event=census` line (family size, family luminance span, per-cell marking fraction) prints **regardless of either gate**, so a map can be judged without enabling anything.


- `MC2_CEMENT_ATLAS_DUMP=<path-prefix>` — diagnostic. Writes the assembled cement catalog atlas as `<prefix>.rgba` (raw BGRA8, `atlasPixelSide²`) plus `<prefix>.csv` (per-cell slot / is_alpha / geometry), so the **art** can be inspected directly instead of inferred from thresholds. This is the instrument that found the baked road tileset. Unset = no dump.

- `MC2_OVERLAY_CEMENT_ADOPT=0|1` — **OVERLAY-CEMENT-ADOPT-1** (CONCRETE-DELIVER-1), **default `1` = ON**. The delivered cure for the concrete patchwork. CEMENT-PATCHY-CAUSE-1 proved the defect is a **two-shader seam** and then explicitly left the cure open as a design call; the user has since made it: *"the transition tiles should just use the same concrete texture"*, with mc2_01 as the runway reference. **The terrain path wins on a cement tile.** Four terms disagreed on one physical surface, not the one that was gated. Chunk: `base = cementAtlas * u_missionFlatLight`, `normalLight` forced `1.0` on concrete, `shadow -> dynamicShadow` only, and **no distance fog anywhere in the shader**. Overlay: a *different* texture times a hardcoded `vec3(0.82,0.80,0.76)` (~18–20% darkening), `shadow = staticShadow * dynamicShadow`, and an **in-shader distance fog** toward `(0.58,0.65,0.75)` clamped at `0.70`. **The missed term is the fog**, and the prior lane's own data already showed it: its seam step **grows with camera distance** (`runway_ground` −4.95 L, `runway_mid` −16.93 L, `runway_overview` −14.81 L) and fog is the only distance-dependent term in the difference set; and its RenderDoc pixels are **blue-shifted on the overlay side only** (overlay eid 3678 `[0.301,0.3083,0.3623]`, B>R; terrain eid 408 `[0.1515,0.1836,0.1495]`, B lowest) — the overlay's own tint is *warm* and cannot make a blue-dominant pixel, while the fog colour is blue-dominant. The overlay is **double-fogged**: its own in-shader fog plus the shared screen-space `edge_fog`/`fog_oob` both layers receive. **Scoping** (keyed off the cement word already bound at binding 25, no new binding): bit 31 set + **bit 24 clear** = genuine concrete transition → adopt the cement **atlas** albedo (same UV math + flat light, **including the CEMENT-ATLAS-SEAM-1 half-texel inset** — OVERLAY-CEMENT-SEAM-PARITY-1 added the inset the original landing omitted while claiming "byte-for-byte the same UV math"; both shaders now read `MC2_CEMENT_ATLAS_SEAM_FIX` with the same default so they cannot drift), drop the warm tint, drop static shadow, skip the in-shader fog. Bit 31 set + **bit 24 set** = **authored art on a cement base → keep its albedo**, reconcile **lighting only**. **Bit 24 is set by `TileIsRoadArt()`, not by an id test.** It was widened twice: OVERLAY-CEMENT-ARTGUARD-1 went from the 9 hard-paved `&Overlays` ids to all 17 (`DIRT_ROAD`, `ROUGH`, `TWO_LANE_DIRT_ROAD`, the dirt/damaged crossings, `OBRIDGE`, `DAMAGED_BRIDGE` were being overpainted); CORRECTION-FIRST-MERGE-1 then went from "any `&Overlays` id" to `TileIsRoadArt()`, which **also** covers cement-**alpha** road art carrying no `&Overlays` id at all. That last class is mc2_10's roads and airbase runway — authored as alpha transition art over snow, admitted here with a valid cement word by the CEMENT-ALPHA-LAND-1 rescue, and measured being repainted **+46.8 L over 23.0% of the runway frame**, i.e. the albedo swap the user rejected twice as *"WAY worse"*. Bit 31 clear → **left entirely alone**; ambiguity always resolves to do-nothing, and an unready oracle protects the tile. Alpha always comes from the decal texture, so the authored marching-squares **shape** is untouched — only the colour inside it changes. Uploaded from `uploadOverlayUniforms_`, the single chokepoint all binders route through, so **both** overlay bind sites are covered and `MC2_TERRAIN_INDIRECT_OVERLAY=0` cannot silently lose it. `MC2_OVERLAY_CEMENT_ADOPT=0` writes no bit 24, uploads 0, and (OVERLAY-CEMENT-REVERT-PURITY-1) **no longer touches GL texture unit 3** → **byte-identical stock**. Prints `[OVERLAY_CEMENT_ADOPT v1] event=authored_art_marked … paved_subset=… overlay_id_subset=… art_saved_vs_paved=… art_saved_vs_overlay_id=…` under `MC2_TERRAIN_INDIRECT_TRACE`, so each widening's yield is measurable in one headless run. **Known interaction:** this *widens* the separate mc2_03 "terrain outside the pads is near-black" contrast (concrete becomes more uniformly flat-lit while surrounding terrain keeps `mix(0.35,1.20,diffuse)` with `diffuse` clamped at 0.02). That is a terrain ambient-floor issue, tracked separately and deliberately not folded in here.
- `MC2_PAVED_OVERLAY_CLASS=0|1|2` — **default `0` = OFF.** CONCRETE-DELIVER-1 promoted this to default `1`; **CONCRETE-DELIVER-2 reverted that.** The promotion re-used the ROAD-OVERLAY-LIGHT-1 attribution (*"closing the paved-classification gap cures the weird coloured edge"*) that **CEMENT-EDGE-CURVE-1 §4 had already measured and retracted in writing**: at the user's own night poses mode 1 measures **max delta 20–29/255 and 9.2–21.7% hard-step pixels** — *"It ADDS hard-edged outlines rather than removing them"*, and *"This should not propagate into another status doc."* The mechanism agrees with the retraction: mode 1 is **lighting-only**, the tile keeps its dark baked colormap, and CEMENT-EDGE-CURVE-1 §3 measured its ceiling at **L≈24 vs concrete L≈60–87** — it structurally cannot deliver *"runways the color they are on top of concrete like in mc2_01"*, because that gap is **albedo-side**. The originating lane's own stated precondition (PAVED-OVERLAY-CLASS-1-STATUS.md: a user click-through on mc2_10 `saved_204937` is *"the single decision that gates everything else"*, plus extended pose coverage *"before any default-ON proposal"*) was never met. Evaluate with `MC2_PAVED_OVERLAY_CLASS=1`; nothing in OVERLAY-CEMENT-ADOPT-1 depends on it. Original entry: **PAVED-OVERLAY-CLASS-1** (user report on five maps: *"all of that concrete should be the same color. same for the roads"*; one runway changes tone along its own length; a *"weird coloured edge"* where pavement meets pavement). **Root cause:** roads and runways are `&Overlays` ids (`PAVED_ROAD=1`, `RUNWAY=7`, …) painted over an **arbitrary** base terrain type, while `isCement()` tests a flag on the **base slot** (`terrtxm.h`, types 10/13–20). The two channels are **disjoint**, so overlay-only pavement is never classified, never baked, renders as **lit** baked colormap beside a **lighting-exempt** cement apron, and increments **no drop counter** — it is invisible to the `[CEMENT_TRANSITION_DROP v1]` census. Measured by `[CEMENT_PAVEMENT_GAP v1]`: mc2_01 **29%**, mc2_03 **81%**, mc2_05 **41%**, mc2_10 **58%**, mc2_24 **34%** of paved-overlay tiles carry no cement word; at the user's mc2_10 `@204937` pose the runway is **100% unclassified**. Every pre-existing lever (`MC2_CEMENT_LIT`, `MC2_CEMENT_TONE_MATCH`, `MC2_RUNWAY_TONE_MATCH`, `MC2_RUNWAY_EDGE_BLEND`) lives **inside** the `isCement && isAlpha` guard or is multiplied by `pureConcrete` — which is why none of them can move these tiles, and why ROAD-OVERLAY-LIGHT-1 measured the runway luma **bit-identical** across all four `MC2_CEMENT_LIT` strengths. This gate adds an `IsPavedOverlayType()` predicate **outside** that guard. `1` = **lighting mode**: the tile gets the same lighting exemption cement receives (`cementWord` bit 22, baked **without** the bit-31 valid bit so every other cement reader ignores it by construction) but **keeps its own baked colormap albedo** — this is what cures a runway changing tone along its own length, since that variation *is* the per-tile lighting the neighbouring flat-lit cement never receives. `2` = **albedo mode** (A/B arm): additionally routes the tile into the solid cement atlas so it samples the apron's texture; fuller convergence but replaces the baked road art, and both prior albedo-swapping attempts were user-rejected as *"WAY worse"*. **A residual albedo gap in mode 1 is expected** — the baked colormap and the cement atlas are different source textures. Default **`0` = OFF = not one word written = byte-identical stock**. Prints `[PAVED_OVERLAY_CLASS v1] event=applied mission=… mode=… flat_baked=… albedo_baked=…` when on; with `MC2_TERRAIN_LOD_CHUNK_DIAG=32768` these tiles read **white** when classified and **black** when not.
- `MC2_TERRAIN_LOD_CHUNK_DIAG` additions (diagnostic, zero-cost off): `32768` = cement-class viz (**black** baked colormap only, **red** SOLID cement-atlas replacement, **green** TRANSITION cement-atlas, **blue** diag-connect, **yellow** runway-edge dilate, **magenta** authored overlay sidecar); `65536` = cement bypass (skip every cement/overlay override so the raw baked colormap shows through — the A/B oracle for "is the cement override what wipes baked road art?"); `131072` = cement atlas **layer-index hash** (adjacent tiles pulling different catalog cells become obvious); `262144` = **raw (unlit) cement atlas texel**; `524288` = **CPU-side pavement-gap census** (AUDIT-TRUTH-1) — enables the per-tile `getOverlay` walk in `PopulateRecipeCementWords` and prints `[CEMENT_PAVEMENT_GAP v1] event=census mission=… paved_overlay=… paved_no_cement=… any_overlay=… cement_total=…`, the pavement denominator the `[CEMENT_TRANSITION_DROP v1]` census cannot see (paved `&Overlays` tiles laid over a non-cement base are never cement-classified, so they are absent from the drop denominator entirely). **Unset = the walk and the print are both skipped**, byte-identical to the pre-`a25037cf` render. Originally shipped ungated; gated by AUDIT-TRUTH-1 because it ran on every mission load in a default build.
- `MC2_TERRAIN_LOD_CHUNK_DIAG=<bitmask>` — frag viz (zero-cost off): 1 no-GBuffer1,
  4 no-lighting, 8 no-shadow, 16 flat-normal, 32 no-detail, 64 rock-sample,
  128 terrainType, 256 raw-colormap, 512 bypass-tint. (bit 2 dead — depth now in vert.)
- `MC2_TERRAIN_NORMAL_ARRAY` — material normal sampler2DArray path. Now **default ON**
  (opt-out `=0`); the chunk path needs it built (mip-completeness fixed 2026-06-09).
- `MC2_LETTERBOX_OPAQUE` — cinematic letterbox bars drawn opaque black; only their
  EXTENT animates. **DEFAULT ON** killswitch (`MC2_TERRAIN_TESS` shape); `=0` restores
  the legacy alpha ramp verbatim. Read once at `code/gamecam.cpp:841` (getenv-seeded
  static, cannot drift mid-run). Legacy behaviour ramped `letterBoxAlpha` with the bar
  extent, so during the ~5s swoop-in the bars were translucent: measured on mc2_10 night
  at capture frame 100, extent 0.0986 of 0.15 max => ramp 65.7% => `letterBoxAlpha=168`,
  i.e. 34% transparent. It read as a broken TOP bar and a fine bottom bar only because
  of what sits behind each — bright horizon/sky above, dark near-field terrain below.
  The separate `fadeAlpha` fade-to-black quad is a real fade and is untouched.
  Visual-only; byte-identical at its default.
  **Registered/documented/allowlisted by DETERMINISM-RATCHET-2**, which found it read by
  code but missing from all three: a smoke run setting `=0` was silently dropped by
  `run_smoke.py` and ran the `=1` path, so the verification was void and said nothing.
- `MC2_GPU_CULL_STATIC_LIGHT_ZERO_PROBE` — static-prop light-bake observation gate,
  **DEFAULT OFF**. Drives **two** diagnostics off one env var: `[STATIC_LIGHT_ZERO v1]`
  (`GameOS/gameos/gos_static_prop_registry.cpp:1644`) counts, every ~300 registry
  flushes, alive prop recipes vs those whose permanent light slot has `numLights_=0`
  (never baked) — with `MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE` also set `zeroLightSlots`
  should reach 0 after mission load; and `[BAKEORD v1]` (`mclib/bdactor.cpp:3203`) emits
  one line per real bake (MISS path only) carrying bake ordinal, pre/post-invalidate
  timing and `sunActive` at bake time — the order-vs-time discriminator for the
  tree-window light-bake hypothesis. Read-only observation: emits log lines, never
  alters a light slot, a bake decision, or any render state.
  **GOTCHA:** the `[BAKEORD v1]` arm additionally routes through `MC2_DIAGF`, so it emits
  **nothing** unless `MC2_DIAG_VERBOSE=1` is also set — a run reporting "zero `[BAKEORD v1]`
  lines" proves nothing on its own.
  **Registered/documented by GATE-HYGIENE-CLOSE-1**, which found it registered *nowhere*:
  it passed `check-env-registry.sh` only via that script's legacy `ALLOWLIST` escape hatch
  and had no tier1 entry, so it was invisible to every gate inventory.
  `mclib/bdactor.cpp`'s own comment asserted the gate was "already-registered" — it was not.
  It is **not** in `run_smoke.py`'s passthrough allowlist, and that is **harmless**:
  `smoke_lib/runner.py` builds the child env with `os.environ.copy()`, so ambient `MC2_*`
  vars reach `mc2.exe` and the allowlist drops nothing (HARNESS-HYGIENE-3). Contrast
  `MC2_LETTERBOX_OPAQUE` above, whose "allowlisted by DETERMINISM-RATCHET-2" note predates
  that correction.
- `MC2_BRIDGE_MOVER_STATE` — emit one `[MOVER v2] id=… name=… team=… pos=…` line per live
  mover (`code/mission.cpp:516`), plus `[MOVER v2 begin] t=… count=…`, at a period set by
  `MC2_BRIDGE_MOVER_PERIOD_SEC` (default 2.0s, floor 0.25s). **DEFAULT OFF**;
  diagnostics-only, no gameplay or render effect. Added to the smoke passthrough
  allowlist by DETERMINISM-RATCHET-2 (same ENV_DROP gap). Note `scripts/run_visual_capture.py`
  inherits the full environment (`os.environ.copy()`), so it was never subject to the drop.
- `MC2_SIM_IDENTITY` — SIM-IDENTITY-HASH-1 world-state hash. **DEFAULT ON**; `=0`/`off`/
  `false`/`no` turns it off. Reader is `code/sim_identity.cpp:80` (`envOnOff`, so unlike
  `MC2_BRIDGE_MOVER_STATE` above it does *not* enable on `=0`). ON emits a `sim_identity`
  block — sorted-by-partId digest over every live mover, no cap — into each visual-capture
  sidecar; sim_identity.py (retired 2026-10-12, cull wave 1) was the refuse-to-grade gate that read it
  (the retired golden_compare.py exited **7** = arms were not showing the same world; tools/sim_determinism.py is the sim-identity proof now). Costs a
  few hundred getter calls once per capture; the per-frame path is the opt-in below.
  Default ON on purpose: a refuse-to-grade gate whose input defaults OFF returns UNGRADED
  for everything and becomes wallpaper.
- `MC2_SIM_IDENTITY_TRACE` — one `[SIMID v1] simframe=… t=… n=… graded=… visual=… strict=…`
  line per SIM frame to stdout (`code/sim_identity.cpp:85`, called from
  `code/mission.cpp` Mission::update). **DEFAULT OFF**; diagnostics-only. Fixed arity and
  width, so `diff run_a.log run_b.log | head -1` names the FIRST divergent sim frame with
  no tooling — the successor to the ~58k-line per-mover dump above.
- `MC2_SIM_IDENTITY_TRACE_DETAIL` — with the trace on, additionally emit one
  `[SIMID v1 mover]` line per mover per sim frame (`code/sim_identity.cpp:91`).
  **DEFAULT OFF**; very verbose, intended for a narrow frame window once the trace has
  named the frame. All three were added to the smoke passthrough allowlist by
  VERIFY-THE-UNVERIFIED-1; they shipped without it, so a smoke run setting
  `MC2_SIM_IDENTITY_TRACE=1` had it dropped by Popen and got no trace at all.
- `MC2_TERRAIN_TESS` — TCS/TES terrain tessellation material. **DEFAULT ON**; `=0` skips
  the `gos_terrain` material load (ISSUE-34 kill-switch — same engine state as a TCS/TES
  compile failure, for headless A/B of the no-tess config; console prints
  `[TESS] MC2_TERRAIN_TESS=0 …`). Shadow builds (`txmmgr.cpp`) and blob-shadow
  suppression / GPU transform skips (`mech3d.cpp`, `gvactor.cpp`) are keyed on
  `gos_IsModernShadowPathActive()` (shadow_terrain material loaded), NOT on tess, so
  CSM/static shadows stay on and legacy pz-gated blob shadows stay off when tess is
  disabled — near-plane clipping stays uniform.

## Deterministic capture clock

- `MC2_SMOKE_FIXED_TIMESTEP=1` — deterministic capture clock (S9D sim + S9E render-shader + CAPTURE-DETERMINISM-1 `frameLength` motion pin). NOTE: engaging this gate does NOT guarantee reproducible frames — the sidecar's `deterministic` flag only reports that the gate was set. Measured residual on mc2_10: up to 27% of pixels between identical launches. See docs/visual-verify.md "Capture nondeterminism".
  Opt-in, default **OFF** (byte-identical retail/normal smoke). Pins BOTH the sim clock
  (S9D: scenarioTime/frameRate advance a fixed 1/30s per frame) AND every render-shader
  `time` uniform (S9E: water/terrain/overlay/decal/screen-shadow/godray/shoreline, all in
  seconds, driven off the shared S9D fixed sim-frame counter via `SmokeMode::fixedClockSeconds()`),
  so frame N renders identically every run. Makes sim speed fps-proportional → it is a
  CAPTURE/DETERMINISM knob, **NOT** for full-duration regression smokes.
- `MC2_FPS_CAP=<int>` — frame-rate limiter, read once at startup
  (`GameOS/gameos/gameosmain.cpp`). `0` = **uncapped** (render-bound). Unset = the
  build's default limiter. This is the hygienic setting for perf measurement: the
  Tracy baseline/regress harness (`tools/tracy_capture_compare.py`, see
  [tracy-harness.md](tracy-harness.md)) sets `MC2_FPS_CAP=0` **and** runs
  free-timestep (i.e. does NOT set `MC2_SMOKE_FIXED_TIMESTEP`), because fixed
  timestep decouples frame count from wall-clock render cost. Smoke-allowlisted in
  `scripts/run_smoke.py`; a diagnostic knob, not a feature gate.

## Sim clock speed + trace (SIM-CLOCK-SPEED-1 — 2026-07-06)

- `MC2_SIM_SPEED=<float>` — sim-speed multiplier SEED, default **1.0**, clamped `[0.25, 4.0]`.
  Applied at both drivers of gameplay time: `scenarioTime` advance (`code/mission.cpp`,
  wall-clock-driven mission timer) and `frameLength` (`code/mechcmd2.cpp`, drives
  mover/object/GOSFX motion+animation). The two are kept in lockstep so mission timers
  and on-screen motion speed up/slow down together; single source
  `simSpeedGetMultiplier()`/`simSpeedSetMultiplier()` (`code/mission.cpp`, declared in
  `mission.h`). SIM-SPEED-BUTTON-1 made the value runtime-settable: the HUD "speed" tab
  (`MC2_HUD_SIM_SPEED`) and dev-shell `sim_speed [--value V]` drive the same setter; the
  value resets to this env seed at every `Mission::init`. NOT applied on the
  `MC2_SMOKE_FIXED_TIMESTEP` branch (smoke determinism keeps 1x). Unset env, never set =
  no-op (`*1.0`). Allowlisted in `scripts/run_smoke.py` (Popen drops non-allowlisted env).
- `MC2_SIM_CLOCK_TRACE=1` — once-per-second stderr diagnostic line comparing
  wall-elapsed vs scenarioTime-elapsed vs frame count vs live `frameLength`:
  `[SIM-CLOCK-TRACE] wall=Xs sim=Ys ratio(sim/wall)=Z frames=N frameLength=F`.
  Opt-in, zero cost when unset (single cached `getenv` check). Was added to
  root-cause a field report of sim running ~2x wall time; investigation found
  no reproduction (ratio measured 1.000 across field/minimal env configs at
  40-80+ fps — no floor-clamp on `frameLength` exists in the timing chain, only
  a max-clamp at `0.25f` protecting against stall/debugger-pause teleport).
  Kept permanently gated for future field diagnosis. Allowlisted in
  `scripts/run_smoke.py`.

## Gameplay command channel (UI-COMMAND-CHANNEL-1 / GAMEPLAY-DRIVER-1/2)

- `MC2_UI_COMMAND_FILE=<path>` — default **unset** = byte-identical. Points at a plain
  text file that `MissionInterfaceManager::processUiCommandChannel()` (`code/missiongui.cpp`)
  polls once per frame from `MissionInterfaceManager::update()`. Each line is one verb;
  the file is read-then-truncated (empty = all consumed) and every consumed line is
  appended to `<path>.ack` as `[f=<frame>] <cmd> -> ok|unknown|blocked (reason)`. The
  `[f=]` prefix is the render frame counter (`g_mc2FrameCounter`) — gate on it instead of
  wall-clock guesses. Readback verbs append extra indented lines after their ack line.
  **The channel is mission-only**: nothing outside the mission loop polls it, so the
  first ack doubles as a "mission is live" probe and the AAR/logistics/mechbay screens
  are unreachable from it. Commands are consumed during the intro swoop AND while paused
  (`Mission::update` calls `missionInterface->update()` regardless of pause state).
  Allowlisted in `scripts/run_smoke.py` (Popen drops non-allowlisted env).
  Verbs:
  - UI: `toggle_pause`, `press pause.<BTN>`, `press dialog.YES|NO`, `debugview <0..6>`,
    `screenshot <name> [backbuffer]` (needs `MC2_DEV_SHELL=1`).
  - World (all route through the same dispatchers the mouse/keyboard use, so game rules
    apply): `select <partId|all|lance|class <NAME>|name <substr>>`,
    `order move <x> <y> [run|walk]`, `order attack <targetPartId>`,
    `order stop|hold_fire|powerup|powerdown|withdraw`,
    `camera <x> <y> [rotDeg] [zoomAlt]`, `camera_follow <partId|off>`,
    `time_scale <f>`.
  - Readback (multi-line ack payload): `status` (SELECTED units only),
    `list [all|player|enemy|team <N>|class <NAME>]` (every mover incl. enemies — the only
    way to obtain a target partId), `unit <partId>` (deep readback **including which draw
    path the object took**: appearance type + number, `inView`, static recipe index,
    `screenPos`), `mission_status`, `light_state`.
  - **Renderer-input readback (RENDERER-INPUT-READBACK-1).** Every verb above this
    line reads GAME state — position, hp, gesture, cull verdict. These five read what
    the RENDERER was handed, which is the half that did not exist. Four of five open
    user complaints are wrong-renderer-INPUT defects, and without these the only
    available grade was whole-frame mean luma — the instrument class that produced the
    retracted "+14.8 luma" result. All five are read-only and carry **no extra env
    gate**; see the runbook in [`docs/gameplay-driver.md`](gameplay-driver.md) for the
    exact command and the exact assert each one unlocks.
    - `light_row <partId>` — the per-object light row, produced by the **same**
      `GatherLightsParameters()` the draw path calls (`mclib/vk_object_light_gather.cpp`), fed
      from the **same** list that object's appearance class passes to `SetLightList`.
      Prints `feed=worldLights|NULL`, then per light: type, the **uploaded** origin
      (`lightToWorld[12..14]`, the only slots any shader reads as `m[3].xyz`) beside
      the **source** origin (`[3]/[7]/[11]`, where `BuildTranslation` leaves it), plus
      `atOrigin=1` when those disagree — i.e. that light is evaluated at the world
      origin wherever it actually sits. Then the **baked** row still being replayed
      for a registered static prop, read back through
      `MC_TextureManager::copyLightSlot`. `feed=NULL` is real, not an error:
      `GenericAppearance::SetLightList(NULL, 0)` (`mclib/genactor.cpp:1238`) hands the
      gather nothing, so that whole class gathers zero lights on this path.
    - `draw_path <partId>` — `recipe_replay` | `dynamic_submit` | `legacy_cpu`, plus
      `recipeDrift` = distance between the recipe's **baked** model matrix and the
      object's live position. The batcher replays the baked matrix, never the live
      transform, so a non-zero drift means every pixel for that object is placed from
      an old pose.
    - `shadow_feed <partId>` — membership in the two prop shadow feeds
      (`getBuildingShadowInstances`, `getDynamicPropShadowInstances`), the pose each
      one **submitted**, `shadowDrift` against the live pose, and `staticBldgPass:
      drew=` from `gos_GetStaticBuildingShadowCounts` (zero until
      the static building shadow has drawn). Membership in a feed that never
      drew is not a shadow, which is why the draw receipt is printed alongside.
    - `ground <partId>` / `ground <x> <y>` — terrain elevation, type, normal and
      terrain light under a point; for an object also `objAboveGround`, and for a
      static prop `recipeAboveGround` (baked render pose vs terrain) and `recipeVsObj`.
    - `pixel <x> <y> [z]` / `pixel screen <px> <py>` — one texel of the scene colour
      attachment, at a world point projected through `Camera::projectForScreenXY` or at
      a raw FBO texel. **`srcFrame` is CURRENT-1**: the channel is polled from
      `MissionInterfaceManager::update()`, before this frame renders, so the FBO holds
      the previous frame — a `screenshot` issued on the same line is an end-of-frame
      capture and is one frame AHEAD. Both the Y-down screen coordinate and the Y-up
      FBO texel are printed so a disagreement with a TGA is diagnosable.
  - Lighting: `time_of_day <0..1>` (0.25 noon / 0.5 sunset / 0.75 midnight — full
    sun + ambient + sky + fog recompute via `Camera::setDaylightFraction`) and
    `sun_pitch <deg> [yaw]` (light DIRECTION only, colors unchanged). These make the
    day AND night verdict rule enforceable on ANY mission instead of only on ones that
    ship as night. Verified on mc2_10 from one frozen pose: `time_of_day 0.25` →
    `isNight=0 lightPitch=90`, `0.75` → `isNight=1 lightPitch=270`, with the rendered
    frame measurably darker (mean luma 67.3 → 57.2 on the standard baseline pose;
    2.9x on a close-in lance pose).
    - **Assert on `isNight` / `lightPitch`, NOT on `nightFactor`.** `nightFactor` is
      not purely a function of the sun: `Weather::update` (`code/weather.cpp`) raises
      it to `rainLightLevel / 4` and takes the MAX while it is raining, so a rainy
      DAY reports `nightFactor ≈ 0.8` with `isNight=0`.
    - Engine note behind these verbs: `Camera::update()` re-derives
      `isNight`/`nightFactor`/`lightDirection` from `lightPitch` EVERY frame and
      assumes it is the sun, but `updateDaylight()`'s night branch replaces
      `lightPitch` with the MOON's pitch (`180 - |180 - pitch|`). A midnight moon at
      zenith therefore left `lightPitch == 90`, which `update()` read back one frame
      later as high noon — so the night verdict was silently undone.
      `setDaylightFraction` restores the true sun angle after the colour recompute.
  - `end_mission win|loss|draw` — forces `Mission::forceTerminate`, i.e. the same three
    fields the authored win/loss paths set, so the normal results flow runs.
  - Notes: `time_scale` routes through `simSpeedSetMultiplier()` (see
    **Sim clock speed** above) so `scenarioTime` and `frameLength` stay in lockstep —
    it is clamped `[0.25, 4.0]` and is a **no-op under `MC2_SMOKE_FIXED_TIMESTEP=1`**,
    which pins both clocks to 1x. `camera_follow` is mutually exclusive with
    `MC2_CAMERA_PARK` (the park path deliberately clears `lookTargetObject`).
    **For an A/B capture use `MC2_CAMERA_PARK`, not the `camera` verb**: `camera`
    calls `eye->setPosition(pos, TRUE)` — the SWOOPY goal-seek — and the in-mission
    scripted camera keeps writing the pose afterwards, so two arms end up framing
    different scenes (measured: an unsettled pair reported midnight BRIGHTER than
    noon). `MC2_CAMERA_PARK` does a one-shot hard set plus
    `freezeScriptedCameraForCapture()`, so every arm shares one frozen pose.
- `MC2_TEST_SPAWN=1` — default **unset** = the two fixture verbs ack `blocked`; zero
  behavior change. Arms `spawn <clone|vehicle|infantry|objNum> <x> <y> [team] [variant]`
  (runs `mission->addMover`, the same path reinforcements take; `vehicle` clones an
  existing `GROUNDVEHICLE` so a mech and a vehicle can be posed side by side under
  identical light for a lighting A/B) and `teleport <partId> <x> <y>`
  (`Mover::setTeleportPosition`). Allowlisted in `scripts/run_smoke.py`.
  **KNOWN DEFECT — do not trust a bare `ok`**: a spawned BattleMech (`spawn clone`)
  comes up with a broken status rating. Measured 2026-07-30 on mc2_10: `getStatusRating()`
  returned `-nan(ind)` on three consecutive runs and `1.000` on a fourth, i.e. it is
  uninitialised, not merely zero. (The 2026-07-23 capture recorded the same units at
  `hp=0.000` ignoring group move orders while their authored peers moved ~380 world
  units.) `spawn vehicle` is healthy in the same runs (`hp=1.000`). Spawned units ARE
  added to the GUI roster — `select all` counts them — and they render, so they are
  usable as *lighting / render* subjects for an A/B; do not treat a spawned mech as a
  functional gameplay actor. The spawn ack reports the unit's actual `hp` and appends
  `WARNING:bad hp, spawned unit may be inert`; the test is written `!(hp > 0)` rather
  than `hp <= 0` precisely because every comparison against NaN is false and the `<= 0`
  form let the NaN case ack a clean `ok`. Use `unit <partId>` for the per-unit detail.
  `MC2_TEST_SPAWN=1` **also** arms `animate <partId> <gestureId|damaged|normal>`
  (ANIMATE-BUILDING-1) — see below.
- `MC2_FREEZE_MOVERS_AT=<frame>` — default **unset** (reader parks at `-1`, latch never
  fires, `mc2FreezeMoversActive()` returns 0 and `Mission::update`'s predicate is
  unchanged) = **OFF**. Set it, or send `freeze_movers on <frame>` on the command
  channel, and every mover is pinned the first frame `g_mc2FrameCounter >= <frame>`.
  Allowlisted in `scripts/run_smoke.py`.
  **Frozen movers still draw.** This does not add a second freeze mechanism: it widens
  the predicate on the pause branch `Mission::update` already had, which calls
  `ObjectManager::updateAppearancesOnly()` — `recalcBounds` + `setObjectParameters` +
  `updateGeometry` still run, `Appearance::update(false)` skips the gait, and
  `setMoverParameters` is deliberately never called, so **torso twist and turret aim
  hold their last value**. That is the whole point: a turret barrel resting at a
  different aim angle between two arms is what produced the retracted "+14.8 luma
  brighter" result.
  **Arm by FRAME, not by wall clock.** `freeze_movers on` with no frame freezes
  whenever the driver's write happens to land, which is reproducible within one
  process and nothing more. `freeze_movers on <frame>` is what makes a pose
  reproducible across a **rebuild** — but only together with
  `MC2_SMOKE_FIXED_TIMESTEP=1`, without which `frameLength` stays wall-clock and
  "frame N" is a different simulated instant in each build.
  Also freezes the wall-clock accumulators that survive an ordinary pause and would
  otherwise keep a "frozen" pose drifting: mech `idleTime` (which **flips the gesture**
  2→13 at `idleMAX`, so two arms can differ by a whole animation), mech and vehicle
  damage-flash blink, and vehicle `sensorSpin` (the mech path was frozen by
  VK-MECH-CAPTURE-DETERMINISM-1; the vehicle path never was). Every one of those guards
  tests **this** gate only and never the visual-capture predicate, so existing capture
  goldens render exactly as before.
  Readback: `mission_status` reports `frozen=` and `frozenAt=`; `freeze_movers status`
  reports `active/armedAt/engagedAt`.
- `animate <partId> <gestureId|damaged|normal>` (verb, gated by `MC2_TEST_SPAWN=1`) —
  forces ONE building off the GPU static-prop batch and onto the per-frame animated
  path. `code/terrobj.cpp`'s own comment notes its `animStartInvalidated` counter "may
  be zero in smoke missions if no registered-static building animates" — that is the
  whole defect class (the black-quad and sunk-turret reports) never being exercised,
  because a passive fly-through never makes a building animate.
  Two **different** routes off the static batch, both exposed on purpose:
  `<gestureId>` calls `setGesture()`, which explicitly calls
  `invalidateStaticRegistration()` on the idle→animated edge; `damaged` calls
  `setObjStatus()`, which swaps in `bldgDmgShape` and does **not** invalidate — the
  stale `staticReg.shape != bldgShape` is dropped a frame *later* from `render()`, and
  that one-frame asymmetry is itself a black-quad candidate.
  Enumerate targets with the **ungated** `list buildings`, which prints each building's
  `animGestures` (which gesture slots actually have animation data), `staticReg`,
  `recipe`, `staticNow` and `eligible`. Requesting a gesture with no animation data is
  acked `blocked` rather than silently doing nothing — `setGesture` would call
  `ClearAnimation()` and leave the object on the static path, which looks like success.
  The ack reports `animStartInvalidated` before→after, so a scenario can assert the
  object really left the static batch.
- `set_light <phase|0..1>` / `set_light alt <deg> [yaw]` (verb, ungated) — one named
  front door for the sun (`dawn`/`morning`/`day`/`noon`/`afternoon`/`dusk`/`sunset`/
  `night`/`midnight`), delegating to the same `Camera::setDaylightFraction` /
  `Camera::setSunPitch` that `time_of_day` and `sun_pitch` use. The standing
  day-AND-night verdict rule was previously enforceable only on whichever missions
  happen to author the sun you need; this makes it expressible in a scenario file on
  any mission. `alt` moves direction only, leaving ambient/sky/fog untouched, which is
  how you isolate a terrain-lighting response from a colour change.
  **Only `day`/`noon` and `night`/`midnight` are guaranteed opposite.** `isNight` comes
  from sun *altitude* vs `NIGHT_LIGHT_PITCH = 10°` (`mclib/camera.cpp:149`), and the
  phase table maps `pitch = fraction*360`, so `dawn` (0°), `dusk` and `sunset` (180°)
  all put the sun on the horizon at altitude 0 and therefore ack `isNight=1`. Asserting
  `is_night: false` after `set_light dusk` will fail — assert `light_pitch` for those.
  Only the day↔night pair has been measured live; the rest is threshold arithmetic.

## Diagnostic JSONL trace (diagnostic_trace.cpp — 2026-06-17)

Structured per-event JSONL output replacing env-gated stderr prints.
Survives normal exits and most crashes better than stderr.
Crash handlers still write to stderr as the last-resort path.

- `MC2_DIAGNOSTIC_TRACE_FILE=<path>` — JSONL output file. Default: `debug_state/diagnostic_trace.jsonl`. Set to `off` to disable.
- `MC2_DIAG_TAGS=<list>` — comma-separated tag whitelist. Controls which tags write events.
  - Unset: default high-value whitelist (`GPU_CULL,LIGHTBAKE_PROOF,ANIM_GATE,CONFIG,BUILD,DEVICE,SHADER_COMPILE`).
  - `*` — all registered tags.
  - `none` — disable JSONL output entirely.
  - Unknown tag names in list → startup warning to stderr, tag ignored.

### Per-event format

```json
{"tag":"GPU_CULL","v":1,"session_id":"12345-1718616000000","pid":12345,"tid":6789,"frame":1234,"ts_ms":16234,"written_epoch":1718616016.234,"data":{...}}
```

### Registered tags

| Tag | Source | Replaces |
|---|---|---|
| `GPU_CULL` | GPU cull readback | `MC2_GPU_CULL_READBACK_TRACE=1` stderr |
| `LIGHTBAKE_PROOF` | Light bake parity | `MC2_LIGHTBAKE_PARITY` stderr |
| `ANIM_GATE` | Animation eligibility summary | `MC2_BLDG_TYPE_ANIM_STATIC_ELIGIBLE` stderr |
| `TerrainLOD_prod` | Terrain chunk production telemetry | `[TerrainLOD prod]` stderr |
| `CONFIG` | Feature gate snapshot at startup | (new) |
| `BUILD` | Build identity at startup | (new) |
| `DEVICE` | GPU/driver info at startup | (new) |
| `SHADER_COMPILE` | Shader compile results | (new) |

### MCP query

Sessions query via `get_diagnostic_events(tag, last_n)` instead of reading log files:
```
get_diagnostic_events("GPU_CULL", 20)     → last 20 GPU_CULL events
get_diagnostic_events("*", 50)            → last 50 events of any tag
```
Unknown tag → error with known tag list. Known tag with no events → empty list.

### Rotation

At engine startup, if `diagnostic_trace.jsonl` exceeds 10 MB, it is renamed to `diagnostic_trace.prev.jsonl` and a fresh file is started. Previous run's trace is preserved in `.prev.jsonl`.

### Flush policy

Each event is flushed immediately (`fflush`) for crash-survival on most platforms. `mc2_diag::flush()` may be called from crash handlers (no allocation). Full crash survival (hardware fault, SIGKILL, kernel panic) is not guaranteed without `fsync`/`FlushFileBuffers`, which is too expensive per-event.

### Diagnostic profiles

**Standard smoke** (canonical `MC2_DIAG_TAGS=CONFIG,BUILD,DEVICE`):
```powershell
$env:MC2_DEBUG_STATE_DUMP="1"
$env:MC2_DIAGNOSTIC_TRACE_FILE="debug_state/diagnostic_trace.jsonl"
$env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE"
```

**Shader triage** — add `SHADER_COMPILE` to diagnose boot shader failures or missing vegetation card compiles:
```powershell
$env:MC2_DEBUG_STATE_DUMP="1"
$env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE,SHADER_COMPILE"
```
Then: `get_diagnostic_events("SHADER_COMPILE", 20)` — each event includes `stage`, `path`, `result`, and `info_log` on failure.

**GPU cull triage** — startup health of substrate/compute/readback init:
```powershell
$env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE,GPU_CULL"
```
`get_diagnostic_events("GPU_CULL", 20)` — substrate_init, gl_probe, c1b_cull_ok/fail, compute_selftest, readback_init, readback_selftest per session.

**Light bake stability triage** — bake index permanence/parity proof:
```powershell
$env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE,LIGHTBAKE_PROOF"
```
`get_diagnostic_events("LIGHTBAKE_PROOF", 20)` — enabled/first/coverage/UNSTABLE events.

**Fail-open:** Shader compile failures emit `SHADER_COMPILE compile_fail` with `info_log` and do not abort optional shader paths where the existing loader already continues gracefully (e.g. vegetation, optional overlays). Core shaders that call `STOP()` on failure remain fatal — fail-open applies only to paths the loader was already skipping.

## Visibility semantics + light-cone diagnostics (VERIFY-INTEGRITY-3 backfill)

Both gates below were live in shipped builds while being **absent from this
file**. `MC2_VIS_SEMANTICS` is worse than a plain omission: it is **default ON**
and it changes what a capture can even show, so an undocumented default-ON
visibility gate is exactly the shape of defect that lets a lane "verify" a fix
against frames the subject was never in. Documented here as part of the
verification-integrity sweep.

- `MC2_VIS_SEMANTICS` — **default ON** (`env_default_on`, `mclib/vis_semantics.cpp:37`).
  The single gate for the centralised mover-visibility predicate family in
  `mclib/vis_semantics.{h,cpp}` (`powered()`, `friendly()`, and the mover
  model/cone-emission predicates). It **supersedes** `MC2_LIGHT_MOVER_VIS_GATE`
  and `MC2_LIGHT_CONE_VIS_GATE` rather than ANDing with them — GHOST-CONE-VERIFY-4
  parked both of those default-OFF (`89f0ed4f`), so consulting them would leave
  the semantics dead on arrival.
  `=0` does **not** mean "stock": it inlines `mover_model_shown()`'s ARMED
  predicate (`legacyVisible && (sensorLevel==0 || sensorLevel>=5)`), which is
  **stricter** than the parked `mover_model_shown()` (that returns true
  unconditionally). So `=0` hides *more* than stock, not less — do not reach for
  it as an "off" baseline.
  Related: `MC2_FORCE_ALL_VISIBLE` (default OFF) in the section above, which is
  the correct lever for making sure a capture actually contains its subject.
  **Registry status:** carried in the check-env-registry.sh (retired in G8; the gate table is the registry) `ALLOWLIST`
  array, not in `RendererFeatureRegistry.h`'s `kFeatureTable`/`kAuxEnvVars`.
  Smoke-allowlisted (`scripts/run_smoke.py:1415`).

- `MC2_LIGHT_CONE_DIAG` — default OFF. Diagnostic trace for the vehicle/mech
  headlight light-cone path; read at `mclib/bdactor.cpp:3415`, `:3446` and
  `mclib/cone_glow.cpp:304`. Emits per-cone placement/curl census lines used by
  the ghost-cone and cone-spill lanes. No rendering effect when OFF.
  **Registry status:** `check-env-registry.sh` `ALLOWLIST` array, not the
  feature table. Smoke-allowlisted (`scripts/run_smoke.py:1422`).

### Known gate-hygiene debt (measured 2026-07-25, VERIFY-INTEGRITY-3)

A four-way sweep (registry / this file / `run_smoke.py` passthrough / code
readers) found, and these numbers are the baseline for the next lane to burn
down — do not re-derive them by hand:

- **121 of 297** registry-table gates are undocumented here. This file is a
  *Tier-1 subset* by design, so that is not all debt, but the feature-grade
  omissions are (`MC2_TERRAIN_CLIFF_*` ×9, `MC2_TERRAIN_DECAL_*` ×8, `MC2_GPU_MECH_*` ×4,
  `MC2_SHADER_SPIRV*`, `MC2_FOG_HORIZON_*` ×3). `docs/flags.auto.yaml` is the
  generated full inventory (1154 names) and is the right reconciliation target
  for the long tail.
- **6 documented-but-dead** gates: `MC2_GL_B`, `MC2_HUD_IMGUI_INFOWINDOW`,
  `MC2_IS_EDITOR`, `MC2_STATIC_PROP_LIGHTING`, `MC2_VERIFY`,
  `MC2_VULKAN_ISLAND`.
- **4 ghost gates** — in the registry tables with zero code reader:
  `MC2_HUD_IMGUI_PAUSE`,
  `MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT`, `MC2_VULKAN_EDGE_FOG_ISLAND`,
  `MC2_VULKAN_POSTPROCESS_SUBGRAPH`.
- **101 registered+read gates missing from the `run_smoke.py` passthrough
  allowlist.** See the correction below on what that does and does not mean.

### Correction: what a missing allowlist entry actually costs

Step 4 of the checklist at the top of this file says an unallowlisted gate means
"Popen drops it and the gate-ON smoke does nothing". **That is no longer true of
the current code** and believing it will send you chasing the wrong bug:
`scripts/smoke_lib/runner.py:83` does `env = os.environ.copy()` before
`env.update(cfg.env_extra)` at `:119`, so the parent environment **is** inherited
and a gate set in your shell **does** reach `mc2.exe` even when unallowlisted.
The old hand-kept allowlist and its advisory checker are gone for the same reason.

The two real costs of a missing entry are:
1. the harness itself cannot **drive** the gate (only the operator's shell can),
   so any A/B that relies on `run_smoke.py` setting it runs both arms identically; and
2. `run_smoke.py:2588-2605` (`SMOKE-GATE-GUARD-1`) prints a loud and, in this
   case, **false** `[ENV-DROP] WARNING ... gate inert` for it.

Keep adding gates to the allowlist — but diagnose an A/B that "showed no
difference" by checking whether the *harness* set the gate, not by assuming the
value was stripped.

---

## `MC2_DIAG_VERBOSE` — the umbrella gate for developer chatter

*(QUIET-DEFAULT-BUILD-1. Appended at end-of-file: `EDGE-LIGHT-EMISSION-1` and
`FORCE-ALL-VISIBLE-1` also claim this file, so this section deliberately does
not touch theirs.)*

**Default OFF.** Set `MC2_DIAG_VERBOSE=1` to restore the engine's internal
`[TAG vN]` diagnostic chatter verbatim.

### What it is for

VERIFY-INTEGRITY-3 measured **83 distinct diagnostic tags printing in a DEFAULT
build** — output a real user sees on a normal launch with no `MC2_*` env set.
Nine repeated every 600 frames forever during play; four announced features
that were switched **OFF**. QUIET-DEFAULT-BUILD-1 routed ~380 call sites across
46 files behind this one gate. A measured default-env `mc2_10` run went from
**79 distinct tags / 439 diagnostic lines to 9 tags / 44 lines**.

### Why one gate and not one per tag

These lines share a single purpose — "show me the engine's internal chatter" —
so they share a single switch. Seventy registry entries nobody would ever set
is not a facility. Anything needing **selective** or **structured** control
keeps using the per-subsystem facilities: `MC2_TERRAIN_LOD_CHUNK_DIAG` bits for
terrain, `MC2_DIAG_TAGS` for the structured JSONL trace.

### What it does NOT gate

Ten tags still print unconditionally, on purpose, because the **verification
harness parses them** — silencing these would blind the smoke gate, which is a
worse failure than being noisy:

`BUILD_FINGERPRINT` `INSTR` `PERF` `TIMING` `TGL_POOL` `ASSET_SCALE`
`ASSET_LOAD` `RENDER_SNAPSHOT` `QUADSETUP_SKIP` `SMOKE`

Each is recorded in `tests/contract/default_build_quiet_baseline.json` with a
`keep` reason naming the harness code that reads it. Moving any of them behind
a gate is a **harness-contract change** and belongs to its own slice.

### Implementation

`GameOS/include/mc2_diag_verbose.h` — header-only, so no CMake change and no
link-order risk (GameOS, mclib, code, RenderWorld and GameAdapters are separate
link units, but `GameOS/include` is on all of their include paths). Four
macros, each a **one-token** replacement for the print call it wraps:

| macro | replaces |
|---|---|
| `MC2_DIAGF` | `printf("[TAG v1] …")` |
| `MC2_DIAGFE` | `fprintf(stderr, "[TAG v1] …")` |
| `MC2_DIAG_PUTS` / `MC2_DIAG_FPUTS` | `puts`/`fputs` sink of an assembled line |

Read once per translation unit (getenv-seeded static), so it cannot drift
mid-run. Default-OFF changes **no** rendering or gameplay behaviour — the only
effect is whether the lines are written.

**Do not wrap a variadic macro's body in these.** Nesting `##__VA_ARGS__` inside
another variadic macro does not expand under MSVC's traditional preprocessor
(this broke `COMPUTE_TRACE` in `gpu_cull_compute.cpp`). Gate such a macro's
*condition* instead.

### The ratchet

`scripts/check-default-build-quiet.py`, wired into `check-contracts.sh` as
`default_build_quiet`. It is a **runtime** check because "is this printf gated?"
is not answerable lexically. It grades the newest smoke run taken with **no
output-affecting `MC2_*` env** (decided from the run manifest's `env_gates`),
looks only at the per-mission stdout logs, and fails on any tag not in the
baseline. It SKIPs (exit 0) when no clean run exists.

- `MC2_VEHICLE_LIT_PATH` — **VEHICLE-LIGHTING-PATH-1, default ON** (`=0` restores the shipped unlit-vehicle output). Puts GROUND VEHICLES on the same lighting path as every other object class. **The bug:** every other class populates its per-object light row via `CacheGpuLightData()` before drawing — `mech3d.cpp:5614`, `bdactor.cpp:3144/:3181/:4821` (buildings), `:6566` (trees), `genactor.cpp:1256` (generic props). `GVAppearance` called **neither** `CacheGpuLightData()` **nor** `submitMultiShape()`, yet renders unconditionally through the legacy `TG_Shape::Render` path, which consumes `lightData_` at `tgl.cpp:3508` with no gather and no eligibility guard. Buildings reach that same legacy `Render` only as an `if (!submittedToGpu)` fallback (`bdactor.cpp:2313`) they normally never take — so vehicles were the only class actually exercising an ungathered row. **Consequence chain, every link verified against committed blobs:** (1) `TG_HWLightsData()` zero-inits `numLights_` (`tgl.h:339`); (2) `addLightDataStructure` is a pure dedup/append with **no back-fill** (`txmmgr.cpp:1929-1988`), so the all-zero row is stored verbatim; (3) `calc_light` takes its `numLights.x==0` early-return, and because `MC2_STATIC_PROP_LIGHTING` is **not** defined for this shader (only `mech.vert:15` / `static_prop.vert:20` define it, and nothing injects it at compile time) it returns `vec3(1,1,1)`, discarding even the per-vertex `base_light` (`lighting.hglsl:232-236`); (4) `ENABLE_VERTEX_LIGHTING` is `1` globally (`lighting.hglsl:8`); (5) `gos_tex_vertex_lighted.frag:60` computes `c.xyz = c.xyz * lighting`. **Net effect: vehicles rendered at RAW ALBEDO x 1.0, fully unlit, day and night.** **Why this looked like an albedo problem and is not:** mechs and buildings receive `ambient + sun*ndl`, which exceeds 1.0 in daylight, so a vehicle texture at the 49th percentile of the corpus (`trucksprop.tga` masked luminance 85.9 vs median 86.2) still looks dark parked beside a mech. The albedo theory was measured and correctly retracted in `fc43338a`; this reconciles that measurement with the symptom — **do not reopen it**. **Why not route vehicles through `GpuStaticPropBatcher`:** `static_prop.vert:713` deliberately omits point/spot ("negligible for buildings"), so that would match buildings but still not mechs; and vehicles are movers, so a static-transform batcher would collide with in-flight static-prop work. Instead this routes them into the same full 6-type `calc_light` that `mech.vert:219` uses — a strict superset of the building path. Ships with `MC2_LEGACY_LIT_MC2_FRAME` and `MC2_LIGHT_POS_COLUMN_FIX` promoted to default-ON (both become load-bearing only once this is on), plus the night light-pool term added to `gos_tex_vertex_lighted.frag` — the only object shader that lacked it — uploaded via the new `gos_UploadObjectLightPools` seam. Visual-only; no geometry/objectId/UI/gameplay change.

- **options.cfg `SunElevationFix`** (this was the env gate `MC2_SUN_ELEVATION_FIX`, deleted 2026-10-12: the value is now the options.cfg key `SunElevationFix`, read once at startup; `=0`/`=1` in the text below means `FALSE`/`TRUE` (or the number); `MC2_OPTIONS_OVERRIDE="SunElevationFix=<value>"` sets it for one run without editing the file) — **SUN-ELEVATION-FIX-1, default OFF** (`=1` opts in; unset/`0` is the legacy three lines bit-for-bit). `Camera::lightDirection` (`camera.cpp:2244-2248`) builds a vector whose **horizontal part is already unit length** and whose `z` is `sin(pitch)`, and only *then* normalizes the whole thing — so its elevation is **`atan(sin(pitch))`, not `pitch`**. Every mission with a non-zero authored pitch gets a sun *lower* than its `.fit` asked for: **mc2_24 authors 75° and receives 44.0°**, mc2_01 50° → 37.5°, mc2_03 46° → 35.7°. Verified across all 24 shipping missions offline (`terrain_sun_elevation_is_atan_sin_pitch_not_pitch`; per-mission table via `--test print_stage_d_delta`). **Separate defect and separate gate from `MC2_SUN_AXIS_FIX`:** that one is about the *frame* of the object-light row and touches only mech/vehicle diffuse; this is about the *elevation* of the terrain sun and touches nearly everything else. They compose; neither implies the other. **Why default OFF — the blast radius is the world, not the terrain:** `lightDirection` is the single source for `gos_GetTerrainLightDir`, and through it for the terrain NdotL, the static-prop sun (`u_terrainSunMC2`), the **shadow projection matrix** (`gos_postprocess.cpp:455` `buildStaticLightMatrix`, `:3214`, `:5208`, `:5502`, `:5688`), SSAO/screen-FX, **and** — via `intensity = terrainNormal · lightDirection` handed to `Camera::getLightRed` (`mapdata.cpp:1898`) — the **sun colour byte of every actor class**. It moves sun direction and shadow direction together, on every mission at once. Too large to enable on arithmetic alone with launches blocked. **Correction (this row's first draft claimed otherwise):** it does **not** move the pre-burned terrain colormap. `burnInShadows` (`terrtxm2.cpp:609`) reads `eye->lightDirection` only under an `if (eye)` guard (`:816`), and its call chain runs inside `land->init()` at `mission.cpp:3710` — *before* `eye = new GameCamera` at `:5108`, with `eye` nulled on teardown at `:5541`. At burn time `eye` is NULL, so the bake takes the `(0,0,1)` straight-up fallback and the mission sun never reaches it; it is also one-shot and disk-cached (`if (!burnedIn)` then `saveTGAFile`). The colormap is unaffected by this gate in either position. **Degenerate-pitch guard:** for `cos(pitch) < 0` the spherical form flips the *azimuth* 180° against every shipped build, so those missions return the legacy vector **verbatim** — among the 24 that is **mc2_10 (pitch 180) alone**, a regular check mission, which is therefore bit-identical with this gate on. `cos(pitch) == 0` (mc2_05/mc2_21, pitch 270) is *not* degenerate — the form collapses to straight down `(0,0,−1)`, no azimuth to flip — though their delivered elevation moves −45° → −90° (below-horizon night either way). **The epsilon is load-bearing:** `cos(270°)` evaluates to `−1.8e-16`, not `0`, so a bare `< 0.0` guard silently drops mc2_05/mc2_21 onto the legacy branch; the harness caught that on the first run and now pins it. **What ON unifies:** the corrected terrain sun becomes *exactly* `−swap(ObjectLights.light_dir)` — the vector mech specular already uses and the one `MC2_SUN_AXIS_FIX` gives mech/vehicle diffuse — so terrain, props, mech diffuse, mech specular and vehicles land on **one** sun vector by construction on all 23 non-degenerate missions. Neither gate alone achieves that. **Flip criterion:** a graded day-mission capture pair (mc2_24 biggest move) *plus* a look at shadow direction and the burned colormap, since those change too. Visual-only; no geometry/objectId/UI/gameplay change.

- **options.cfg `SunAxisFix`** (this was the env gate `MC2_SUN_AXIS_FIX`, deleted 2026-10-12: the value is now the options.cfg key `SunAxisFix`, read once at startup; `=0`/`=1` in the text below means `FALSE`/`TRUE` (or the number); `MC2_OPTIONS_OVERRIDE="SunAxisFix=<value>"` sets it for one run without editing the file) — **SUN-AXIS-FIX-1, default OFF** (`=1` opts in; unset/`0` is the shipped expression bit-for-bit). Applies the Stuff→MC2 axis swap to `ObjectLights.light_dir` inside `calc_light`'s two **directional** branches, so mech and ground-vehicle **diffuse** stop dotting an MC2-frame normal against a Stuff-frame sun. **The derivation, from the code that runs:** the row's `light_dir` is `GetLocalForwardInWorld()` on the sun's light-to-world (`object_light_row.h:123`,`:265-267` ← `txmmgr.cpp:2621`), built as `BuildRotation(EulerAngles(lightPitch, lightYaw+135, 0))` (`camera.cpp:509/:1860/:2110/:4252`). `LinearMatrix4D::BuildRotation` forwards to `AffineMatrix4D::BuildRotation` (`linearmatrix.hpp:85-86` → `affinematrix.cpp:162-194`) — *not* `Matrix4D::BuildRotation` in `stuff/matrix.cpp`, which an inherited note mis-cited; the bodies are arithmetically identical for `EulerAngles`, so the result stands but the citation did not. With `roll == 0`, row 2 is `(cosP·sinY, −sinP, cosP·cosY)`, and `GetLocalForwardInWorld` returns exactly that row (`FORWARD_AXIS == Z_Axis == 2`, `APPLY_FORWARD_SIGN` identity — `vector3d.hpp:55`,`:73`). **The cancellation that hid it:** `−swap(row) == (cosP·cos a, cosP·sin a, sinP)` with `a = lightYaw+45` — the true spherical toward-sun vector. The +90° between the object light's `yaw+135` and the terrain sun's `yaw+45` (`camera.cpp:2244`) is *exactly* cancelled by the axis swap, verified identically on all 24 shipping missions offline. **So the consumer that swaps is right and the ones that do not are wrong.** `mech.vert:279` swaps before using `light_dir` for **specular** and is correct — this gate does not touch it. The diffuse branches did not swap: with the mech clamp removed and a face pointed straight at the sun, `ndl == 0.0` on **14 of 24 missions** — anti-correlated on the majority, not "90° off", and the `mech.vert:223` clamp is what hid it. **Scope:** `TG_LIGHT_INFINITE` and `TG_LIGHT_INFINITEWITHFALLOFF` only (stock missions author the sun as the latter). POINT/SPOT derive direction from `to_light`, a position difference, and never read `light_dir.xyz` — the SPOT branch's own comment claims otherwise and the code beneath it disagrees, so no cone axis can move. **Implementation:** `calc_light` gained a 5th parameter; the 4-arg arity survives as a forwarder hard-coding `0`, so shaders that did not opt in are byte-identical *by construction* (`building_pbr.vert` untouched). Opted in: `mech.vert` (diffuse only) and `gos_tex_vertex_lighted.vert` **only on its `u_legacyLitMc2Frame=1` branch** — so `MC2_LEGACY_LIT_MC2_FRAME=0` reverts that shader completely. **Where it is visible:** on missions whose authored ambient alone clears the clamp (`0.35 + A ≥ 1`) the multiplier is pinned at 1.0 and this gate changes *nothing* — that is **mc2_10** (AmbientRed 192, a regular check mission: **zero delta measured**) and mc2_22. Elsewhere 32–97% of the normal sphere moves, up to 0.53 of full scale. **Why default OFF:** launches are blocked in this lane so no human has seen it; the change is large; and it is **not monotone** — measured over 4096 normals/mission it darkens about as many as it brightens (mc2_13: 47.7% brighter / 48.0% darker), so it cannot be sold as "brighter mechs". **Flip criterion:** one side-by-side on mc2_03 or mc2_13 with `=1` versus unset. Evidence: `tools/class_light_band_harness` Stage B/D, 42 tests, no launch. Visual-only; no geometry/objectId/UI/gameplay change.

- `MC2_VEHICLE_AMBIENT_FLOOR` — **VEHICLE-AMBIENT-FLOOR-1, default ON** (`=0` restores no-floor/no-clamp, byte-identical to the pre-slice build). Completes `MC2_VEHICLE_LIT_PATH` by giving the legacy lit vehicle path the **two terms `mech.vert` has and it never did**: the `kAmbientFloor = 0.35` seed passed into `calc_light` (`mech.vert:204`,`:218-222`) and the `clamp(...,0,1)` on the result (`mech.vert:223`). **Why the previous slice was necessary but not sufficient:** it correctly fixed *"vehicles carry no light row"* (that is why they drew at raw albedo), but it did not fix the sentence the user actually wrote — *"the truck is DARK, while the mech next to it is lit"* — because the two classes still evaluate **different expressions off the same row**. Vehicle: `M = base_light + A + S*ndl`, unclamped. Mech: `M = clamp(0.35 + A + S*ndl, 0, 1)`. **What `base_light` actually is — measured, and *not* the "always zero" it looks like from `tgl.cpp:1093` alone:** that line only *seeds* `aRGBLight` to `0xff000000`; the ASE loader overwrites it from `*MESH_VERTCOL` whenever `*MESH_NUMCVFACES` is present (`tgl.cpp:1319-1448`), which every stock vehicle ASE has, and the binary `.tgl` loader reads the `TG_TypeVertex` array verbatim (`tgl.cpp:641`) so the baked value ships. Counted over all 8 stock ground-vehicle `.tgl` binaries: `0xff000000` (0.0000) on 56–110 verts/file, `0xff010101` (0.0039) on 3–6, and `0xff646257` (0.3922) — the only value above the 0.35 floor — on **at most 2**. The bright `0xffece7cc` (0.9255) palette entry is referenced by no CFACE and reaches zero vertices. So the floor raises 97%+ of vehicle vertices and is inert on at most two. Here `A = AmbientRed/255` (the `TG_LIGHT_AMBIENT` slot, `camera.cpp:516`+`:2418`) and `S = min(LightRed*I + AmbientRed, 255)/255` (`camera.cpp:4392-4408`), the INFINITE sun slot written per vehicle at `gvactor.cpp:2611-2620`, with `I = terrainNormal · lightDirection` and `lightDirection.z = sin(pitch)/sqrt(1+sin² pitch)` (`camera.cpp:2244-2248`, `mapdata.cpp:1892-1900`). **On the map in the report:** mc2_10 authors `AmbientRed 192`, `LightDirPitch 180` → `A = S = 0.7529`, so the vehicle sat at **0.753 on shadow faces against the mech's 1.0** — the symptom surviving the fix meant to cure it. On mc2_03 (`AmbientRed 31`, `LightRed 242`, pitch 46 → `A = 0.1216`, `S = 0.6745`) the vehicle **peak** was 0.796, below 1.0 at every normal, so every vehicle pixel darkened. **With this ON the vehicle expression is arithmetically identical to the mech expression** for the same row, on every map. **`max()` not a bare `vec3(0.35)`:** `mech.vert` has no per-vertex seed to lose; this shader does (`get_base_light` still implements the hot-colour lit-window/lamp conventions), so the floor is `max(base_light, vec3(0.35))` — it can only raise the seed. **The clamp ships with the floor, not separately:** without it a vehicle on a bright-ambient map overshoots the mech beside it (mc2_10 lit faces reach 1.506 vs the mech's 1.0, and the object target is RGBA16F so the excess is not free). One gate, one revert. **Self-limiting blast radius:** the program is shared with other legacy lit shapes, but a shape that never gathers still has `numLights == 0`, so `calc_light` returns `vec3(1,1,1)` *before* reading the seed (`lighting.hglsl:232-236`) and `clamp(vec3(1),0,1) == vec3(1)` — non-gathering shapes are bit-identical with this ON. **Night lamps survive the clamp:** the light-pool term is added in `gos_tex_vertex_lighted.frag` *after* the vertex stage and is not clamped, exactly as in `mech.frag`. **Falsification arm:** on a high-ambient map the floor pushes the vertex multiplier into the clamp and vehicle shading goes flat (mc2_10: `0.35+0.7529 = 1.1029 → 1.0` at every normal). That is parity, not a defect — `mech.vert` computes the same 1.0 there and always has. The arm that would make this gate *wrong* is a vertex whose `base_light` already exceeds 0.35 **and** whose lit multiplier already exceeded 1.0, so the floor is inert and only the clamp fires (net darkening). **That arm is live on stock content, not hypothetical** — but bounded, and measured rather than waved away: at most 2 vertices per stock vehicle sit above the floor, all at 0.392, so on the worst map (mc2_10) those go `1.145 → 1.0` (a 1.15x cap) while the other 97%+ go `0.753 → 1.0` (a 1.33x lift); every vertex then lands at 1.0, uniform and equal to the mech. Imported/BT2018 shapes with richer authored VERTCOL data can exceed that bound and are the first population to check if anything looks dimmer. Visual-only; no geometry/objectId/UI/gameplay change.



  **Why it only detects — the correction of `db221dcc2`, which *declined* the GPU submit here.** That decline was justified on the ground that `gpuMechSubmitted` stays false and the CPU MLR path draws the mech via `mechShape->Render(true)`. **That is false in the only configuration where the decline can fire**, and the enclosing gate of the submit block is the proof: it requires `g_useGpuMechs && !previewContext`, which are exactly the conditions under which `updateGeometry` (`mech3d.cpp:4567-4576`) transforms `mechShape` with a **fast-transform** variant — preview forces the full `TransformMultiShape`, and `!g_useGpuMechs` falls to the full transform in the `else` arm. In the default configuration (`g_useGpuMechs` and `g_useGpuMechLeafSkip` are both `envFlagDefaultOn`, `gos_mech_batcher.cpp:134`/`:214`, modern shadow path active) that variant is `_HierarchyOnly`, which delegates to `TransformMultiShape_BuildRecipe` (`msl.cpp:2003`) and hits `if (s_buildRecipeOnly) continue;` at `msl.cpp:1910` **before** the per-leaf dispatch. `TG_Shape::MultiTransformShape` therefore never runs, `lastTurnTransformed` is never advanced (its only writers are `tgl.cpp:1911`/`:1972`/`:2984`, all inside the skipped per-leaf dispatchers, plus `Touch()` at `:4352`, called on building and tree shapes only — never on `mechShape`), and `TG_Shape::Render` returns at the staleness guard `tgl.cpp:3190` **without drawing**. The engine documents this twice in-tree: `mech3d.cpp:4560-4563` ("the GPU fast-transform variants leave `listOfVertices` stale, so `TG_Shape::Render` early-returns and the preview goes blank") and `mech3d.cpp:3117-3124` — the `MECH-CULL-CPU-FIX` comment, which records it as an **observed runtime bug**: "we skipped the GPU submit and fell back to the CPU `mechShape->Render()` path, **which does not actually draw** while GPU mech batching owns rendering → the mech vanished for that frame → center-screen flicker." Declining reintroduced a bug this file had already fixed once.

  **And the decline was worst in its own target case.** It cannot fire in steady state: `Mission::update()` calls `clearArrays()`→`resetLightData()` (`code/mission.cpp:1146`), then `CacheGpuLightData()` stamps `cachedFrame_ = g_mc2FrameCounter` (`mech3d.cpp:5793`, ungated by cull), then `draw_screen()` runs the check — and `g_mc2FrameCounter` is incremented **only after** `draw_screen` (`GameOS/gameos/gameosmain.cpp:2207`). Reset, stamp and check share **one counter epoch**, so the stamp always matches. The stamp can only go stale when `Mech3DAppearance::update()` does not run at render cadence — the Alt+D debug-draw latch (`gameosmain.cpp:1816`) or the Ctrl+R `doTransformMath` toggle (`code/mechcmd2.cpp:2295`) — but those skip `Mission::update()` itself, so `resetLightData()` does not run either, the table is **not** rebased, and the "stale" index still names its own correct row. Pre-fix those frames rendered **correctly**; the decline made the mech **vanish** on them. The decline was therefore **inert in steady state and strictly harmful whenever it fired** — no reachable case improved.

  **Why the range check clamps rather than declines:** an out-of-range `light[]` read is undefined, so the check must stay — but slot 0 is the pre-slice substitute and a defined in-range row (the static/baked prefix under LIGHTBAKE). An invisible mech is a worse outcome than a mistinted one for the actors the player aims at and is shot by. The check now also applies on the stale path, which is where an out-of-range index is most likely. **Fail-loud:** `[LIGHTROW_FRESHNESS v1] event=mech_stale_row_detected` emits once per distinct `mechType` with the uncapped running total inline, plus an `event=mech_row_out_of_range` one-shot; the totals ride the **always-on** per-mission `[MECHBATCHER v1] event=stale_light_row_summary` line (`GpuMechBatcher::onMapUnload`), so they are visible on a supervised run **without** the env-gated mech-batcher stats trace. Previously they were readable only from their own once-per-mech-type line, which made a count that started or grew later unobservable. `GpuMechFallbackReason::StaleLightRow` is retained for numbering stability but is **no longer recorded** — nothing falls back, so recording a fallback would be false. **Why default-ON:** what ships default-ON is the detector, the bounds clamp and the per-mission report — none of which change what is drawn relative to the pre-slice read, except by replacing an undefined out-of-range read with a defined one. **Honest limit on the evidence:** engine launches are hard-blocked in this lane; the draw-path chain above is a source trace, not an observed frame, and the one **inferred** link is that `gos_IsModernShadowPathActive()` (`gameos_graphics.cpp:10133`) is true in a normal session — it is a runtime `shadow_terrain`-material-loaded predicate, not an env default. `stale=0` on the per-mission line means the hazard is **inert**, the predicted steady-state result; non-zero means some actor's `update()` is not running at render cadence (candidate: the `withdrawing` early return at `code/mech.cpp:6117`) and the repair is to **re-stamp the row at `update()` time**, not to decline at render time. `oob` **must read zero**. **Not graded visually** — engine launches are blocked in this lane.
- `MC2_VK_SHADOW_MATRIX_DIAG=1` — VK shadow producer/consumer evidence (Trace, default OFF = zero cost, `GameOS/gameos/vulkan_shadow_island_live.cpp`). At shadow-pass tick 600/1200 logs `[VK_SHADOW_MATRIX v1]` matrices (GL dyn + CSM, VK island received, VK terrain UBO) and at tick 600 dumps `vk_shadowdiag_<dyn|csm0|full>_<gl|vk>.png` to the cwd. No behavior change.
- `MC2_CONFIG_DUMP` — **Typed gate registry (`Gates/`), default `default`.** Scope of the `[CONFIG v1]` dump the game and the editor print at startup: `default` lists the gates the environment sets plus a `summary` line, `all` lists every row (about 1100 lines), `none` prints the summary only. Read in `Gates/gate.cpp`.
- `MC2_CONFIG_STRICT` — **Typed gate registry (`Gates/`), default OFF; `scripts/run_smoke.py` sets it to `1`.** A bad value (a Bool that is neither a known spelling nor empty, an unparsable number, an unknown enum token) or an unregistered `MC2_*` variable in the environment ends the process at startup with exit code 78 after printing `[CONFIG v1] warn=...` lines, so a typo in a smoke harness fails loudly instead of silently doing nothing. Read in `Gates/gate.cpp`.
