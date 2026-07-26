# Tier-1 instrumentation env vars

> **STATUS: Verified current as of 2026-07-09 (nifty HEAD, build pipeline). Includes MC2_TERRAIN_LOD_CHUNK (default ON), MC2_SHADOW_CSM (default ON), asset-mod payload vars (HDRI_BC6H, BUILDING_PBR), diagnostic JSONL trace, and the Vulkan-prep / frame-graph-executor / render-backend-iface seam gates. Match against RendererFeatureRegistry.h for new feature gates. Bump this date whenever gates are added or edited below.**

## Adding a new MC2_* gate (checklist)

1. Register FIRST in `RenderCore/RendererFeatureRegistry.h` (`kFeatureTable` for a renderer feature gate, `kAuxEnvVars` for a trace/aux var).
2. Read it via `getenv`/`envFlag`/`selftestEnvFlag`/`envFlagDefaultOn` (the last is the default-ON kill-switch pattern — see `MC2_SMART_LOAD` below).
3. `sh scripts/check-env-registry.sh` must pass — unregistered `MC2_*` reads are CI-blocking (exit 1 on unknown vars; see header comment at the top of the script).
4. Add it to the `run_smoke.py` env passthrough allowlist (the `env_extra={...}` dict built around `scripts/run_smoke.py:1149`), or the harness spawns the game process without it ("Popen drops it and the gate-ON smoke does nothing" — see the surrounding comments in that file). `scripts/check-env-allowlist.sh` is an ADVISORY drift check for this allowlist (always exits 0 unless run with `--strict`) — it is not CI-blocking, so don't rely on it catching a miss.
5. Document it here in `docs/tier1_env_vars.md`: default value, polarity, one-line effect.
6. Prove gate-ON vs gate-OFF smoke: `py -3 scripts/run_smoke.py --tier tier1 --duration 30 --keep-logs`

## Live data-contract guards (MC2-VERIFY-LIVE-1)

- `MC2_VERIFY_MODE` — mode for the live `MC2_VERIFY` guard family (`docs/verify-primitive.md`). `log` (**default**): shadow-log `[VERIFY]` lines to stderr + crash-bundle ring, continue; `[VERIFY] mission-end fires=N` counter at Mission::destroy. `fatal`: STOP with message + crash bundle + hard terminate (exit 0xE0564631). `off`: exactly-legacy (silent, guarded degradations skipped). Smoke-allowlisted.

## Cursor / focus hands-off for automation (SMOKE-NO-CURSOR-STEAL-1)

- `MC2_NO_CURSOR_GRAB` — suppress **all** OS-cursor manipulation by the engine: the window-creation mouse grab (`SDL_SetWindowMouseGrab` + Win32 `ClipCursor` clamp to the client rect, re-asserted on every `FOCUS_GAINED`/resize/move), and every pointer warp through `gos_SetMousePosition` → `SDL_WarpMouseInWindow` (notably the tutorial `guiFrozen` path in `missiongui.cpp`, which recenters the pointer **every frame**). Also sets `SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN=1` so the game window does not steal keyboard focus on show.
  **Default: suppression is ON whenever `MC2_SMOKE_MODE` is set** — i.e. every agent-driven smoke / visual_verify / capture / screenshot harness gets it for free (they all set `MC2_SMOKE_MODE=1`; `scripts/smoke_lib/runner.py:83`). Interactive launches (neither var set) keep exactly the legacy behavior.
  `=1` forces suppression on even for an interactive launch; `=0` forces it **off** even under smoke (escape hatch, e.g. reproducing a grab-dependent bug headlessly). Evaluated once and cached (`gos_CursorControlSuppressed()`, `GameOS/gameos/gameos_input.cpp`).

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

## Feature gates (RendererFeatureRegistry.h kFeatureTable)

- `MC2_COLORMAP_KTX2` — BC7 KTX2 atlas. Default **ON**. Kill=`=0`. Bake: `py -3 scripts/bake_colormap_ktx2.py`.
- `MC2_COLORMAP_CPU_RETIRE` — free cpuColorMap after atlas upload. Default **ON**.
- `MC2_VIEW_UNIFORMS` — ViewUniforms UBO. Default **ON**. Kill=`=0`. Requires restart.
- `MC2_SHADOW_ENABLE=1` — dynamic shadow caster pass. Default **OFF**.
- `MC2_IMGUI=1` — ImGui overlay. Default **OFF**.
- `MC2_IMGUI_INSPECTOR=1` — inspector panel. Default **OFF**. Requires `MC2_IMGUI`.
- `MC2_DEBUG_RENDERER=1` — debug overlay. Requires `MC2_IMGUI_INSPECTOR`.
- `MC2_STATIC_PROP_REGISTRY=1` — GpuStaticPropRegistry. Default **ON**. Editor sets `=0`.
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
  **Terrain-residency probe routing (`mclib/txmmgr.cpp` ~3096-3150, fixed `claude/deferred-sweep-1` commit `235b88f3`):** the probe is gated on `mc2TerrainLodChunkEnabled()` and reads from whichever renderer is actually active — under the default chunk/GPU-indirect path it reuses the `g_loadRaceAtlasFirstFrame`/`g_loadRaceArmFirstFrame` latches (same state that drives the `[LOADRACE] atlas first-resident` / `[LOADRACE] gpu terrain first-arm` lines below), a cached-int read with no per-frame walk; under `MC2_TERRAIN_LOD_CHUNK=0` it falls back to the original `masterVertexNodes` walk. **Caveat found while verifying this fix:** in a *game* build (not `EditRel`/`MC2_IS_EDITOR`), `TerrainQuad::setupTextures()` — the function that would populate/mark `masterVertexNodes` terrain-node residency — is compile-gated out entirely (`mclib/quad.cpp` ~706-1337, commit `006800e5`, predates this fix). So `MC2_TERRAIN_LOD_CHUNK=0` has **no production renderer** in game builds (confirmed: `[BUCKET_CENSUS v1] ... legacy=0` at every frame) and the legacy branch of this probe will correctly report `terrain_resident=N` on *every* game-build run regardless of load state — a true negative reflecting the vestigial opt-out, not a bug in the probe. The legacy branch only has a live signal to read on an `MC2_IS_EDITOR` build exercising the passability-grid path.
- `MC2_OVERLAY_TEXTURE_TRACE=1` — overlay/decal resolve-fail counters + 1×1-magenta fallback-bind log (OMT-1): `[OVERLAY_TEXTURE v1] resolve_fail/summary`.
- `MC2_CLUSTER_DEPTH_PYRAMID=1` — **default OFF** (CLUSTER-DEPTH-PYRAMID-NATIVE-1). MC2-native GPU compute pass (`shaders/cluster_depth_pyramid.comp`, dispatched from `gosPostProcess::endScene` right after the HZB build) that reads the resolved sampleable scene depth (`sceneDepthTex_`) and writes a per-tile **(min,max)** depth image (`RG32F`: R=numeric min, G=numeric max), one texel per 32×32 tile. **Reversed-Z note:** MC2 is reversed-Z (near≈1, far≈0); the pass stores raw numeric extents and leaves near/far interpretation to a future consumer (nearest = MAX channel G, farthest = MIN channel R). **SUBSTRATE ONLY** — no lighting/decal/material consumer reads the image yet. **OFF = true no-op, byte-identical** (nothing allocated, nothing dispatched; one cached-bool branch). New `gpuSyncBarrier` edges `ComputeImageWrite→{TextureReadback,TextureSample}` (`GL_TEXTURE_UPDATE/FETCH_BARRIER_BIT`) registered in `gos_gpu_sync.cpp` — no raw `glMemoryBarrier`.
- `MC2_CLUSTER_DEPTH_PYRAMID_VERIFY=1` — **default OFF**, requires the master gate. One-shot CPU-vs-GPU parity check on a single fixed frame: reads scene depth back, recomputes per-tile min/max on CPU, compares against the GPU `RG32F` image readback (tolerance `1e-6`, observed delta 0 — same depth texture read by both sides). Logs `[CLUSTER_DEPTH_PYRAMID v1] PARITY PASS/FAIL tiles=… mismatches=… worst_delta=…`.
- `MC2_CLUSTER_DEPTH_PYRAMID_PLANT=1` — **NEGATIVE TEST**, default **OFF**, requires `_VERIFY=1`. Corrupts one CPU reference tile before the comparison so the checker SHOULD report FAIL — proves the parity check can actually fail. Never set in production.
- `MC2_LIGHTGRID_BUILD=1` — **default OFF** (MC2-LIGHTGRID-BUILD-NATIVE-1). MC2-native GPU compute pass (`shaders/lightgrid_build.comp`, dispatched from `gosPostProcess::endScene` right after the cluster depth pyramid) that builds a **per-tile light-bin grid**. Stage 0 derives a native `MC2LightCullSphere[]` cull-geometry buffer from `ObjectLights` (binding 20): center = `light_to_world` translation, radius = `light_falloff.y` (far), type = `light_dir.w`. **SPHERE ONLY** — POINT and SPOT both bin as spheres; MC2 has no SPOT cone half-angle so **cone culling is DEFERRED**. Stage 1 (one workgroup per tile) reconstructs each tile's world-space sub-frustum from the depth pyramid's (min,max) and tests each sphere; survivors are **LDS-staged then committed with ONE global `atomicAdd` reserve** (BT-shaped, not a per-candidate atomic storm) into a compact light-index pool + per-tile `(offset,count)` RG32UI header. **Z-bins: NZ=1** (single Z interval per tile — lean v1; froxel Z deferred). **Reversed-Z:** near = MAX (G), far = MIN (R). Requires `MC2_CLUSTER_DEPTH_PYRAMID=1` (no tile texture ⇒ pass skips). **INERT** — no shading consumer reads the grid, no visual change. **OFF = true no-op, byte-identical.** **16-light cap UNCHANGED.** Typed `gpuSyncBarrier` edges only (no raw `glMemoryBarrier`).
- `MC2_LIGHTGRID_VERIFY=1` — **default OFF**, requires the master gate. One-shot CPU-vs-GPU parity check. Prefers the first frame with `numLights>0` (waits ≤1200 dispatched frames) so the sphere/frustum cull runs against a real light; falls back to the empty case. Reads back the GPU-built sphere buffer + depth pyramid + grid header + index pool, recomputes the binning on CPU, compares **per-tile (count, membership SET)** (offsets are GPU-schedule-nondeterministic — sets are the stable surface). Logs `[LIGHTGRID_BUILD v1] PARITY PASS/FAIL tiles=… lights=… mismatches=…`. No float tolerance (exact integer comparison).
- `MC2_LIGHTGRID_PLANT=1` — **NEGATIVE TEST**, default **OFF**, requires `_VERIFY=1`. Corrupts one CPU reference tile's membership set before the comparison so the checker SHOULD report FAIL — proves the parity check can actually fail. Never set in production.
- `MC2_GPUBUF_RING=1` — **default OFF** (VULKAN-CONTRACT-MANIFEST-ARC, GPU-BUFFER-WRAPPER-TIER0-HUD-1). Routes the fixed-capacity gosMesh HUD/2D meshes (`gameos_graphics.cpp` `gosMesh::draw`/`drawIndexed`) through a 3-frame **persistent-coherent FENCED ring** (`GameOS/gameos/utils/gpu_ring_buffer.h`) instead of the per-batch `glBufferData` orphan. Pilot of the `GpuRingBuffer<N>` shape (persistent-coherent map, per-slot base offset, `glClientWaitSync`-on-reuse, `glFenceSync`-after-draw, KHR_debug label) on the smallest/safest live buffer. **OFF = legacy orphan path, byte-identical GL stream + output** (one cached-bool branch). **ON =** the `[GPUBUF v1]` `hud` orphan call count drops to ~0 (the ring memcpys into the coherent map; no per-batch `glBufferData`). Fence discipline reuses `gos_mech_batcher.cpp`'s pattern. `uploadBuffers()` (used by terrain via direct `getVB()/getIB()` NULL-offset draws) intentionally stays on the legacy path — its draws are external and would not honor a slot offset. Ring buffers are lazily created on first ON-use and drained+unmapped+deleted in `gosMesh::destroy` (no leak across mission/device reset).
- `MC2_GPUBUF_RING_FORCE_WRAP=1` — **NEGATIVE TEST**, default **OFF**, requires `MC2_GPUBUF_RING=1`. Deterministically advances the HUD ring twice (`beginFrame()` without an intervening `endFrame()`) so the one-`endFrame`-per-`beginFrame` / fence-per-frame invariant trips: logs `[GPUBUF v1] event=begin_without_end tag=… slot=…` (and `abort()`s under a debug build). Proves the safety guard is live rather than the happy path merely rendering. Never set in production.
- `MC2_GPU_DEBUG_NAMES=1` — **presence-gated**, default **OFF** (VULKAN-DEBUG-NAMES-1). Attaches `glObjectLabel` KHR_debug names to GPU buffers so RenderDoc / RGP / Vulkan-prep captures show human-readable names instead of anonymous handles. Helper `GameOS/gameos/utils/gpu_debug_labels.h::setGpuBufferDebugLabel`; proof site labels the ViewUniforms UBO (`view_uniforms_gl.cpp`). Read once into a static bool; also null-checks `glObjectLabel` (only wired under `MC2_GL_DEBUG`, so it may be absent — a missing extension must never crash). **OFF = one predicted-false branch, no GL call, byte-identical GL stream + output.** Diagnostic/tooling only.
- `MC2_GPUBUF_RESIDENCY=1` — default **OFF**. Once every ~300 frames lists the live HUD ring buffers: `[GPUBUF v1] residency tag=… kind=meshring frames=3 vbBytes=… ibBytes=… liveFences=…`. Diagnostic only.
- `MC2_GPUBUF_COUNTER=1` — **measurement-only**, default **OFF** (VULKAN-CONTRACT-MANIFEST-ARC slice 4, GPU-UPDATE-BUFFER-COUNTER-1). Counts per-frame orphan-on-write `glBufferData` re-spec (full re-spec, no fence/ring) that is otherwise INVISIBLE in the hitch trace (which only emits on >threshold frames). Independent gate that OR-enables the existing `MC2_GL_BufferData` accountant + a per-owner tally for these orphan sites, WITHOUT forcing the rest of `MC2_HITCH_TRACE` on. Emits every frame: `[GPUBUF v1] frame=N orphan_calls=C orphan_bytes=B by_owner=gos_UpdateBuffer:c/b,hud:c/b,light:c/b,sp_shadow:c/b`. Instrumented sites: `gos_UpdateBuffer`, the gosMesh/HUD `updateBuffer` 5-arg overload, the light SSBO create/grow/orphan path, and the static/dynamic-prop transient shadow SSBOs. Zero behavior change when unset (one predicted-false cached-bool check; byte-identical GL stream + output).
- `MC2_GPUBUF_LIGHT_GROWONCE=1` — **default OFF** (VULKAN-CONTRACT-MANIFEST-ARC, LIGHT-GROW-ONCE-SUBDATA-1). The **AMD-safe intermediate** for the light SSBO orphan churn — **NOT** the full `GpuStorageRing` (no ring, no N-buffer rotation). Keeps the single persistent light SSBO (same `s_lightDataSsbo` handle, slot-20 `glBindBufferBase`, std430 `LightsData` layout / shader contract — unchanged) but eliminates the per-frame FULL `glBufferData` orphan re-spec (1.85 MB/frame, the `LIGHTSSBO-ORPHAN-1` NVIDIA no-stall workaround). **ON =** per frame upload ONLY the live used bytes via `glBufferSubData` (routed through `MC2_GL_BufferSubData`, NOT the owner `glBufferData` macro); the GL buffer **grows ONLY when used bytes exceed current capacity** — rare, sized with **+128-record headroom** matching the CPU backing grow step (`txmmgr.cpp addLightDataStructure`), draining via `glFinish` before delete/recreate/rebind-to-slot-20 and logged **once per grow** (`[LIGHTSSBO v1] event=growonce_grow`, requires `MC2_LIGHTSSBO_TRACE`). The split (`_UploadSplit`) path is subsumed into the same full-live-bytes SubData when ON (the prefix/suffix optimization is moot without an orphan). **OFF = the existing `LIGHTSSBO-ORPHAN-1` orphan path runs completely UNCHANGED, byte-identical GL stream + output** (one cached-bool branch; kill switch). With `MC2_GPUBUF_COUNTER=1`, the `[GPUBUF v1]` **`light` owner orphan bytes drop to ~0** steady-state (only rare grow frames emit a `glBufferData`). WARNING: **NVIDIA is a HARD BLOCKER before default-on:** `glBufferSubData` into the single live buffer that is read all-frame by every lit draw (and cross-phase by the mech/static-prop batchers) has a cross-frame in-flight-write hazard — on NVIDIA SubData-into-in-flight STALLS the CPU (exactly the stall the orphan dodged); AMD tolerates it. Default-OFF until NVIDIA is in the verification loop; the fix is the full ring (out of scope here), NOT an N-buffer rotation.

- `MC2_TONEMAP_V2=0|1|2|3` — **default OFF (0)** (POST-FX-TONEMAP-V2, SCOUR-1 #3). Optional filmic curve on the final composited colour in `postprocess.frag::applyTonemap`, applied AFTER grade+CAS, before UI (visual-only; no geometry/depth/object-id effect). `0`/unset = OFF, byte-identical no-op branch. `1`/`agx` = AgX (Sobotka minimal), `2`/`hable` = Uncharted2 filmic, `3`/`reinhard` = Reinhard `c/(1+c)`. ⚠️ **Input is already-graded LDR** (`gradeColor` clamps to `[0,1]`), so `3` Reinhard globally halves brightness (no white point) — reject for RTS; `1`/`2` are the real A/B. Resolved once from env in `gos_postprocess.cpp` → `[TONEMAP v2] mode=N` log line. Allowlisted for smoke passthrough. Replaces the removed ACES path below. **VERDICT 2026-07-03 (eyes A/B, mc2_01): all three modes look WORSE than baseline OFF — the LDR-input curves crush/desaturate the RTS palette. Keep default OFF; not recommended. Retained for completeness, not deleted (unlike ACES) since it is a clean no-op gate.**

**REMOVED 2026-06-22 (DEAD-POST-FX-CLEANUP-1 — features deleted as wrong-for-RTS):** `MC2_HDR_POST`, `MC2_BLOOM`, `MC2_TONEMAP_ACES` (now inert; RendererFeatureRegistry entries retained index-only, annotated `[REMOVED]`). God rays + bloom hotkeys (RAlt+6 / RAlt+F1) and the `bloomThreshold`/`bloomIntensity` profile keys are also gone.

## Vulkan-prep seam (VULKAN-CONTRACT-MANIFEST-ARC)

Headless Vulkan probe / backend-skeleton exercise gates. All read via bare `getenv` presence (any non-null value activates), all default **OFF** = no Vulkan code runs. Readers in `GameOS/gameos/vulkan_backend_skeleton.cpp`.

- `MC2_VULKAN_PROBE=1` — run the one-shot headless Vulkan probe at startup (`mc2_vulkan_probe_if_env`): caps enumeration, SPIR-V shader-module load, fullscreen-triangle offscreen render, descriptor-set + sampled-image smoke. Default **OFF** = never runs (no instance/device created). Presence-gated (`getenv != null`).
- `MC2_VULKAN_SPV_DIR=<path>` — override the compiled-`.spv` directory the probe loads from. Default = `shaders/vulkan`. Only consulted when `MC2_VULKAN_PROBE` is set.
- `MC2_VULKAN_CACHE_DIR=<path>` — directory for the persistent `VkPipelineCache` blob the fullscreen-triangle probe seeds from / writes back (`triangle_pipeline.cache`), VULKAN-PIPELINE-CACHE-1. Default = `debug_state/vulkan_cache`. Fully fail-soft: a missing/unreadable file just means a cold pipeline build (`no prior cache`), and write/get failures are logged but never abort the probe. First run writes the cache; subsequent runs load it (visible as `loaded N bytes`). Only consulted when a triangle-probe path runs.
- `MC2_VULKAN_VALIDATION=<preset>` — enable the `VK_LAYER_KHRONOS_validation` layer + debug-utils messenger inside the descriptor/probe paths, selecting a validation **preset** (VULKAN-VALIDATION-PRESETS-1). Default **OFF**. Only meaningful when a probe path runs. Presets:
  - unset / `0` / `off` — validation OFF (unchanged legacy behavior).
  - `1` / `core` / present-but-empty — core validation only (backward-compatible with the historical bare `=1`).
  - `sync` — core + `VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`.
  - `gpu-assisted` — core + `VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT` (+ reserve-binding-slot). May need extra plumbing/extensions; fails soft if the layer can't honor it.
  - `best-practices` — core + `VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT`. May emit best-practice **warnings** (logged as `validation-warning:`, non-failing) — probe still passes.
  - `debug-printf` — core + `VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT`. May need extra plumbing; fails soft.
  Extra features chain into `VkInstanceCreateInfo.pNext` via `VkValidationFeaturesEXT`. Unknown value → falls back to `core` + warns. Resolved preset name is logged at startup. Only ERROR-severity validation messages fail a probe; WARNING-severity ones are visible but non-failing.
### The VK gate model (VK-P14 GATE-CONSOLIDATION-1)

The VK shared-GBuffer scene arc's gates are organized into four classes. **The default flip will toggle exactly ONE name: `MC2_VK_SHARED_GBUFFER`.**

- **Umbrella** — `MC2_VK_SHARED_GBUFFER=1` arms the whole shared-GBuffer scene path at once (see the entry below). Default OFF.
- **Sub-gates (umbrella members)** — the scene-region + region-detail gates: `MC2_VK_SHARED_SCENE_DEPTH`, `MC2_VK_PROP_COLOR_TIER1`, `MC2_VK_PROP_MATERIAL`, `MC2_VK_TERRAIN_TIER1`, `MC2_VK_TERRAIN_LIGHTING`, `MC2_VK_TERRAIN_DETAIL`, `MC2_VK_TERRAIN_LODFADE`, `MC2_VK_TERRAIN_DYNSHADOW`, `MC2_VK_MECH_TIER1`, `MC2_VK_MECH_MATERIAL`, `MC2_VK_MECH_LIGHTING`. When the umbrella is ON each **defaults ON**; an **explicit** sub-gate env is a debug **override** that always wins (`=1` force-on even if umbrella off, `=0` force-off even if umbrella on). Resolved by `mc2vkgate::subGate()` in `GameOS/gameos/vk_shared_gbuffer_gate.h`. Region preconditions still apply (e.g. the Tier-1 regions require shared depth; terrain detail/dynshadow/lodfade require terrain lighting; lodfade further requires terrain detail).
- **Direct-chain gates (umbrella members as of VK-UMBRELLA-DIRECT-CHAIN-DEFAULT-1)** — the no-GL-interop strict-path gates still LIVE: `MC2_VK_PROP_DIRECT_CHAIN`, `MC2_VK_WATER_DIRECT_CHAIN`, `MC2_VK_POST_COMPOSITE_DIRECT_CHAIN`. When the umbrella is ON each **defaults ON** — i.e. **umbrella-ON MEANS the strict/direct-chain no-GL-interop path** (each producer + post pass keeps the shared color/depth/normal chain Vulkan-owned and skips its GL↔VK semaphore handoff); an explicit per-gate `=0` override now only forces that member's term of the post-upstream predicate OFF (the prop/water hybrid arms were **DELETED in VK-RUNG8-ITEM5-SHARED-SCENE-DELETE-1**, so `=0` no longer selects a hybrid path — it is inert beyond a one-shot deprecation note; the island stays unconditionally native). Resolved by `mc2vkgate::directChainGate()` in `GameOS/gameos/vk_shared_gbuffer_gate.h` (same precedence as `subGate`). **RETIRED to parseable no-ops (item-4 hybrid-arm deletions; explicit set prints a deprecation note):** `MC2_VK_MECH_DIRECT_CHAIN`, `MC2_VK_SHADOW_DIRECT_CHAIN`, `MC2_VK_POST_FOG_DIRECT_CHAIN`, `MC2_VK_POST_SSAO_DIRECT_CHAIN`, `MC2_VK_POST_SCREENFX_DIRECT_CHAIN`, `MC2_VK_POST_HZB_DIRECT_CHAIN`, and `MC2_VK_TERRAIN_NATIVE_PREFIX_CLEAR` (VK-RUNG8-ITEM4-TERRAIN-DELETE-1 — the armed terrain Tier-1 island is now unconditionally the native first owner of shared scene color0/normal/depth: CLEAR loadOp, no GL↔VK semaphore handoff; the GL frame-entry clears are skipped via `mc2vkgate::terrainNativeFirstOwnerOn()`). `MC2_VK_OBJECTID_DIRECT_CHAIN` is **NOT** a default member (view-mode-only canary; stays explicit `=1`). Each direct-chain member still has its own upstream precondition (e.g. post passes require the full terrain→prop→mech→water chain armed).
- **Safety gates (INDEPENDENT — not umbrella members)** — `MC2_VK_TERRAIN_HEIGHT_AUDIT` (verification-only, default-OFF) and the structural island fail-soft (no env; always-on GL fallback). A verification/robustness gate stays orthogonal to the feature so it can be toggled during a soak without disarming the path. **Boot-level native-or-decline (VK-UMBRELLA-DIRECT-CHAIN-DEFAULT-1 item-2):** the terrain first-owner island now distinguishes a PERMANENT init failure (spv/pipeline/resource creation — declines the island for the whole session with ONE receipt line, GL owns terrain from frame 0) from a TRANSIENT readiness gate (VK context / shared GBuffer not born yet — keeps the item-9 fail-hard-with-retry). No per-frame re-init churn on a permanent failure.

**Byte-identical rule:** umbrella unset/≠`1` AND no sub-gate forced ⇒ every member OFF ⇒ pure GL, byte-identical to today (near-oracle `near_land_off` self-diff 0.000).

- `MC2_VK_SHARED_GBUFFER=1` — **presence-gated `=1`, default OFF** (VK-P14 GATE-CONSOLIDATION-1). **THE UMBRELLA** for the shared-GBuffer scene path. ON sets every sub-gate above **and the 10 direct-chain gates** to default-ON (explicit sub-gate/direct-chain env still overrides per the model) — so umbrella-ON is the strict/direct-chain no-GL-interop path. **The eventual default flip toggles exactly this one name.** Requires `MC2_RENDER_BACKEND=vulkan` (the shared engine `VulkanContext`); fail-soft to GL when absent. Does NOT arm the safety gates or the retired Tier-0 path. **Gate OFF (or any sub-gate forced `=0`) is byte-identical to the pre-arc GL path.**
- `MC2_VK_MECH_FEED_NATIVE` — **DEPRECATED / no-op (VK-RUNG8 item-9).** The VK mech island now **always** consumes the game's CPU byte sources (copy-once geometry from `s_vkVboNativeCopy` + `s_stagingIbo`; per-frame LightsData from `mcTextureManager->lightData_`); the GL `glGetBufferSubData` readback fallback was **deleted**. The env name is still parsed so old command lines don't hit unknown-var behavior — explicitly setting it just prints a `[VK_MECH_FEED] ... deprecated` note. Fail-hard replacement: if the CPU geometry source is transiently null (a normals rebuild raced the accessor) the island declines the frame and retries; if LightsData is unavailable the mech renders flat-lit that frame. No per-frame GL rescue.
- `MC2_VK_PRESENT_SKIP_GL_HANDOFF` / `MC2_VK_PRESENT_NO_GL_IMPORT` — **DEPRECATED / no-op (VK-RUNG8 item-8).** The present interop tail (copy-1 GL blit + GL→VK semaphore handoff + the exportable present image + its GL view) was deleted; the present image is **unconditionally** a VK-only allocation, so the "skip GL handoff" and "no GL import" behaviors are now structural. The names are still parsed (explicit `=1` prints a one-shot `[VK_PRESENT] ... deprecated` note). **VK-RUNG8 item-10:** the stale requirement that BOTH be `=1` before `mc2VkSharedSceneDirectResourceEnabled()` (shared-scene VK-only arm) returned true was **removed** — it was dead resolver code that would wrongly decline the whole shared scene to pure GL under the umbrella unless a user set two retired gates. The shared scene now arms on the post/composite direct-chain stack alone (default-ON under the umbrella).
- `MC2_VK_STATIC_SHADOW_MAP` — **DEPRECATED / no-op (VK-RUNG8 item-6).** The VK-backed static-shadow import substrate (an exportable `D32_SFLOAT` `VkImageResource` imported into GL as `GL_DEPTH_COMPONENT32F`, plus the whole `vulkan_shared_static_shadow` TU) was **deleted**. The static sun shadow map is now always the plain GL `GL_DEPTH_COMPONENT24` allocation; a non-CSM **static-only** screen-shadow config **declines to the GL screen-shadow pass** (CSM screen-shadow is unaffected — it samples the direct VK CSM array/full-map views). Explicit set prints a one-shot `[VK_SHARED_STATIC_SHADOW] ... deprecated` note. **This was the last live caller of the interop family**, so items 1/2/3 could then delete it entirely.
- **VK-RUNG8 items 1/2/3 (interop family deletion — Metal-gate opened):** `gl_interop_view.{cpp,h}` (GL-boundary zero-copy import adapter) was **deleted** whole (item 1); the **exportable IMAGE half** of `vk_image_resource.{cpp,h}` (`VkExternalMemoryImageCreateInfo` / `VkExportMemoryAllocateInfo` / `vkGetMemoryWin32HandleKHR` / `memWin32` / export-sanity queries) was **deleted** — only the NON-exportable VK-only core survives (item 2); and the `wantInterop` plumbing in `vulkan_context.cpp` (the `VK_KHR_external_memory[_win32]` / `VK_KHR_external_semaphore[_win32]` device+instance extension family) was **removed**, so `vkCreateDevice` on MoltenVK no longer fails on an unsupported extension (item 3 — **the one hard Metal-gate blocker**). `ensureEngineVulkanContext()` was renamed `ensureEngineVulkanContext()`. Device extensions now enabled = **surface + swapchain only** (`VK_KHR_surface` + `VK_KHR_win32_surface` + `VK_KHR_swapchain`, all Metal-portable via a platform branch). `MC2_VK_NO_GL_INTEROP_STRICT` survives as a live guard on the terrain per-frame GL depth-readback fallback; `MC2_VK_NO_GL_INTEROP_RESOURCE_STRICT` is now **permanently satisfied by construction** (no exportable resource can be created; no code reader remains) and stays as a checker provenance receipt.
- `MC2_VK_POST_FOG_NATIVE=1` — **envTriState OPT-IN, default OFF** (rung 8.1, VK full-ownership ladder — `.claude/VK-RUNG8-LADDER-1.md`; unset -> OFF regardless of `MC2_VK_SHARED_GBUFFER`, explicit `0` forces OFF — same new-rung pattern as `MC2_VK_TERRAIN_SKIRTS`/`MC2_VK_WATER_TIER1`). Renders BOTH postprocess fog passes (edge fog + OOB fog) **natively on the shared engine VulkanContext** via `vulkan_post_fog_native.cpp`: samples the SHARED scene depth directly and alpha-blends into the SHARED scene color0 IN PLACE — **zero copies, zero CPU readbacks, no private VkDevice** (kills the old fog subgraph's per-frame `glGetTexImage(GL_DEPTH_COMPONENT)` readback + staging upload + 2x `glCopyImageSubData` color round-trips). Reuses the parity-proven `edge_fog`/`fog_oob` SPIR-V verbatim (UBO PODs offline-checked by `tools/check_ubo_layout.py` labels `postfog_edge`/`postfog_oob`). HIGHEST precedence at the fog site (native > subgraph > per-pass island > GL); runs only when BOTH GL fog passes would run AND the shared-GBuffer substrate is live; any not-ready condition falls through to the unchanged chain. Receipts: `vk_post_fog_native` section in the debug-state dump (`postfog_used_vulkan`). **Gate OFF = byte-identical GL fog chain.**
- `MC2_VK_POST_SSAO=1` — **envTriState OPT-IN, default OFF** (rung 8.2, VK full-ownership ladder — `.claude/VK-RUNG8-LADDER-1.md`; unset -> OFF regardless of `MC2_VK_SHARED_GBUFFER`, explicit `0` forces OFF — same new-rung pattern as `MC2_VK_POST_FOG_NATIVE`). Renders SSAO (gen + apply) **natively on the shared engine VulkanContext** via `vulkan_post_ssao_native.cpp`: the gen pass samples the SHARED scene depth AND the SHARED scene normal (GBuffer1) directly into a **VK-OWNED half-res R16_SFLOAT occlusion image** (device-local, non-shared — the ladder's one intermediate), the apply pass 4-tap-box-upsamples it (LINEAR, GL parity) and **multiplies (DST_COLOR/ZERO) into the SHARED scene color0 IN PLACE** — **zero copies, zero CPU readbacks, no private VkDevice** (retires the old SSAO island's per-frame `glGetTexImage` depth+normal+color readback pattern when gated on). Reuses the parity-proven `ssao`/`ssao_apply` SPIR-V verbatim (R16F format parity, LINEAR apply-sampler parity, column-major matrix parity all inherited; UBO PODs offline-checked by `tools/check_ubo_layout.py` labels `ssaonat_occ`/`ssaonat_apply`). HIGHEST precedence at the SSAO site (native > region-iface > GL); runs only when the GL `runSSAO()` would run AND `MC2_SSAO_DEBUG` is off (the debug grayscale-overwrite is an unported diagnostic — falls through to GL) AND the shared-GBuffer substrate is live; any not-ready condition falls through to the unchanged chain. Receipts: `vk_post_ssao_native` section in the debug-state dump (`postssao_used_vulkan`). **Gate OFF = byte-identical GL SSAO chain.**
- `MC2_VK_POST_SCREENFX=1` - **envTriState OPT-IN, default OFF** (rung 8.3, VK full-ownership ladder; unset -> OFF regardless of `MC2_VK_SHARED_GBUFFER`, explicit `0` forces OFF). Renders the **cloud-shadow + shoreline-foam** postprocess passes natively on the shared engine VulkanContext via `vulkan_post_screenfx_native.cpp`: cloud-shadow samples shared scene depth and multiplies into shared scene color0 in place; shoreline samples shared scene normal and brightens foam in place. Zero copies, zero CPU readbacks, no private VkDevice. Per-pass bisect sub-gates `MC2_VK_POST_SCREENFX_CLOUD` / `MC2_VK_POST_SCREENFX_SHORELINE` can force individual passes back to GL. **Screen-shadow is still not part of this gate**; the CSM shadow producers now have direct VK consume gates (`MC2_VK_CSM_ARRAY_INTEROP` / `MC2_VK_CSM_FULLMAP_INTEROP`), so a future native screen-shadow slice no longer needs to add CSM `glGetTexImage` readbacks. Receipts: `vk_post_screenfx_native` debug-state section (`postscreenfx_used_vulkan`, `cloud_frames`, `shoreline_frames`). **Gate OFF = byte-identical GL passes.**
- `MC2_VK_MECH_FEED_AUDIT=1` — **verification-only safety gate, default OFF** (NOT an umbrella member; same class as `MC2_VK_TERRAIN_HEIGHT_AUDIT`). With the native feed armed, ALSO reads the GL buffers back and **byte-compares** them against the CPU sources on every native copy (geometry once, LightsData per frame, min-prefix compare), logging `[VK_MECH_ISLAND] FEED-AUDIT ...` lines with running mismatch tallies. Nonzero mismatch = native feed diverges from what GL rendered with = feed bug.
- `MC2_VK_GPU_TIMESTAMPS=1` — **verification-only diagnostic, default OFF** (NOT an umbrella member; same class as `MC2_VK_TERRAIN_HEIGHT_AUDIT` / `MC2_VK_MECH_FEED_AUDIT`). Adds `vkCmdWriteTimestamp` GPU timestamp brackets (3 stamps: pass-start / draws-start / pass-end) around the **terrain** and **mech** island render passes so the coarse CPU `draw_us` (which spans record + submit + `vkWaitForFences`) can be split into real **GPU pass execution time** vs **CPU submit/fence-wait**, and within the GPU pass into **setup/clear/load** vs **draws**. VK 1.1 core, **no device feature required** (only the queue-family `timestampValidBits` + `limits.timestampPeriod` gate it — MoltenVK-clean); no pipeline-statistics query (that would need a device feature the shared engine context does not enable). Header-only helper `GameOS/gameos/vk_gpu_timestamps.h`. Emits a `[VK_TERRAIN_ISLAND]`/`[VK_MECH_TIER1] GPU_TIMESTAMPS samples=… gpu_pass_us=… gpu_setup_us=… gpu_draw_us=… (cpu_draw_us=… incl. record+submit+fence-wait)` line at island teardown (EMA over the run). **Gate OFF (or timestamps unsupported) = zero query commands recorded, byte-identical render path.**
- `MC2_VK_SHADOW_INTEROP=1` — **presence-gated `=1`, default OFF** (VK-P8a). The **second Vulkan GEOMETRY region** and the **first VK-PRODUCES-DEPTH region** (water was GL-produces/VK-consumes; shadow reverses it): renders the **dynamic static-prop shadow-caster depth** in Vulkan (`GameOS/gameos/vulkan_shadow_island_live.cpp`) into an **exportable D32** `VkImageResource`. GL binds that shared D32 in place of `gosPostProcess::getDynamicShadowTexture()` and samples it via the **UNCHANGED** hardware `sampler2DShadow` `calcShadow()` path (`shaders/include/shadow.hglsl`) — lighting up the **vk2gl back-channel** (`glVkToGlSem` / `glInteropWaitFromVk`) for the first time on a geometry region: the VK submit **SIGNALS** vk2gl, GL **WAITS** it, then samples (the hardware-compare-over-imported-D32 path proven by `tools/vk_shadow_sampler_probe`). **Feed** (VK-P8a STEP 0, `.claude/VK-P8a-STEP0-CASTER-FEED-NOTE.md`): the CPU `GpuStaticPropInstance` vector (`dynShadowSet`, 112B each) the GL path already builds each frame is **memcpy'd** into a VK instance SSBO; the shared static-prop VBO/IBO are copied **ONCE per map** into VK-owned buffers (`mc2_shadow_island_get_geometry_feed`). **Pipeline**: depth-only, **LESS forward-Z** (NOT reverse-Z — matches the GL dynamic shadow map, `glClearDepth(1.0)`), cull **None**, `depthBias` set per-frame to `glPolygonOffset(shadowBiasFactor_, shadowBiasUnits_)` **EXACTLY** (the parity **STOP-watch** — bias divergence = acne/peter-panning). Reproduces `shaders/shadow_static_prop.vert` math bit-for-bit (`pos*M` row-vector → Stuff→MC2 swizzle → `lightSpaceMatrix*`). **BORROWS** the shared engine `VulkanContext` (`ensureEngineVulkanContext`) — no second device, no `volkLoadDevice`; **REQUIRES `MC2_RENDER_BACKEND=vulkan`** (**fail-soft to the GL shadow path** when the context is absent/invalid). Depth surface zero-copy; **forbidden DEPTH-readback counters hard-asserted ==0**. Surfaced in the `[VK_SHADOW_INTEROP]` health line + `vulkan_shadow_island` dump section: `interop_active`, `vk2gl_sem_active`, `used_interop`, `inst_records`, `type_runs`, `forbidden_*` (==0). Golden-parity oracle = interop-ON vs the GL shadow baseline on a shadow-visible deterministic bookmark (`MC2_SMOKE_FIXED_TIMESTEP=1`); the VK-produced shadow must match byte-exact or within a documented depth-precision floor (water ±1-LSB / SSAO `highangle_wide` precedent). **P8a scope** = opaque static props, single map (foliage-alpha + tessellated-terrain casters = P8b, bone-skinned mech casters = P12). **Gate OFF = byte-identical GL shadow path.**
- `MC2_VK_SHARED_SCENE_DEPTH=1` — **presence-gated `=1`, default OFF** (VK-TIER1 shared-depth substrate, step 2). Backs the engine **scene-depth** texture (`sceneDepthTex_`) with a **VK-BUILT exportable `D32_SFLOAT_S8_UINT`** `VkImageResource` (`GameOS/gameos/vulkan_shared_scene_depth.{h,cpp}`, reusing `vk_image_resource.{h,cpp}` + `gl_interop_view.{h,cpp}`) imported into GL as a **`GL_DEPTH24_STENCIL8`** texture (depth+stencil aspect usable for BOTH sampling AND FBO depth-stencil attachment), substituted at the **SINGLE** scene-FBO depth alloc+attach site (`GameOS/gameos/gos_postprocess.cpp` `createFBOs`, formerly `glTexStorage2D(GL_DEPTH24_STENCIL8)` + `glFramebufferTexture2D(GL_DEPTH_STENCIL_ATTACHMENT)`). **GL-ONLY slice**: GL still renders scene depth into it and the **42 depth-sample consumers** (SSAO / edge-fog / OOB-fog / HZB / DOF / god-rays / cluster-depth-pyramid / particle depth-copy / stencil `glTextureView`) sample it **UNCHANGED** — there is **NO VK depth consumer yet** (that is the P11 Tier-1 slice). Direction = **GL-produces / VK-imports** (inverse of the shadow island). The **P5-T0** (`main_d24s8`) + **depth-test** (`main_d32s8_depthtest`) spikes proved the D32S8-backed image imported under `GL_DEPTH24_STENCIL8` **samples the DEPTH aspect exactly** (0 val errors) and round-trips the `glCopyImageSubData` snapshot + stencil view, so the substitution is transparent. **BORROWS** the shared engine `VulkanContext` (`ensureEngineVulkanContext`) — no second device. **FAIL-SOFT**: if the shared VK context / interop is unavailable at scene-FBO creation (context not born — e.g. no `MC2_RENDER_BACKEND=vulkan`, LUID mismatch, missing external exts, GL import error), falls back to the **plain GL `GL_DEPTH24_STENCIL8` alloc** and logs the reason → **gate-ON with no interop degrades to the GL path, never crashes**. Rebuilt on **resize** (`destroyFBOs` destroys the VK image + GL view — NOT `glDeleteTextures`, which would double-free the interop tex — and `createFBOs` rebuilds). Surfaced in the `[VK_SHARED_SCENE_DEPTH]` log line (`active tex=… WxH` / `fallback reason=…`). **Gate OFF (or island not compiled) = byte-identical GL scene-depth path.**
- `MC2_VK_PROP_MATERIAL=1` — **presence-gated `=1`, default OFF; REQUIRES `MC2_VK_PROP_COLOR_TIER1=1`.** Replaces the VK prop Tier-1 flat DEBUG teal with **real textured VERTEX-BAKED-lit materials** (NOT PBR — GL props are vertex-baked by default). Copy-once imports **both** static-prop albedo `2D_ARRAY`s (`s_texArrayOff` binding 3 + `s_texArrayOn` binding 6; per-type group-relative layer tables bindings 2/5; RGBA8→`R8G8B8A8_UNORM` or BC7→`BC7_*_BLOCK` per real internalformat, full mips). Alpha-ON (class-1 alpha-MIXED) types route to `s_texArrayOn` with the GL `0.5` alpha-test discard (`static_prop.frag:236`). Frag = `albedo * vArgb`, `vArgb` = per-vertex `a_aRGBLight` base_light seed + stand-in ambient + sun·NdotL. `GBuffer1` unchanged (`a=0.25`). PROVEN mc2_01: teal gone, buildings textured, runway diff 1.22 mean/ch. **DEFERRED vs full parity:** ObjectLights ambient + per-type magic hot-color SSBO + per-actor sun/highlight/fog (stand-in constants → lighting is approximate model, not byte-parity), MaterialGpu roughness/metallic, BC7-bucketed arrays. **SEPARATE parked issue:** Tree/Generic POPULATION coverage (only Building population fed). FAIL-SOFT: array not ready / layer<0 → teal. **Gate OFF = byte-identical.**
- `MC2_VK_PROP_MATERIAL_DEBUG=1` — diagnostic only; one-shot `[GPUPROPS_HIST v2]` instance-weighted layer histogram (textured/alpha_on/no_primary). No render effect.
- `MC2_VK_TERRAIN_DETAIL=1` — **presence-gated `=1`, default OFF; REQUIRES `MC2_VK_TERRAIN_LIGHTING=1`.** Perturbs the VK terrain's flat-up normal before NdotL to close the flat-grass residual: macro-slope normal from the height SSBO (`smoothTerrainNormal`, always on) + per-material weighted detail-normal from `matNormalArray` (9-layer 2D_ARRAY, copy-once import, REPEAT) via the colormap classifier, composed exactly as GL (`pert = (baseN.xy/baseN.z*nfhStrength + dN.xy*detailStrength, 1)`). Suppressed on `pureConcrete`; shadow keeps flat-up N. PROVEN mc2_01: high-angle grass matches GL (signed +0.00); oblique/grazing residual remains (deferred POM/anti-tile/LOD-fade). FAIL-SOFT: detail array not ready → macro-slope only. **Gate OFF = byte-identical.**
- `MC2_VK_TERRAIN_LODFADE=1` — **presence-gated `=1`, default OFF; REQUIRES `MC2_VK_TERRAIN_DETAIL=1`.** Ports GL's camera-distance `lodNear` fade (`gos_terrain.frag:322-328,616-736`: `LOD_NEAR=4000`/`LOD_NEAR_FADE=4500` smoothstep) into the VK terrain frag, closing the VK-LOD0-vs-GL-band color seam at the fade boundary. Feeds `gos_GetTerrainCameraPos()` (Stuff/MLR space) into the existing UBO pad tail each frame (no slot growth — `TerrainFrameUBO` stays 512B); the frag recomputes `camDist`/`lodNear` verbatim and multiplies the POM offset **and** the whole detail-normal contribution (`dN * lodNear`) by it, matching GL's `pomOffset *= lodNear` / `detailMul` exactly. FAIL-SOFT: gate off → `lodNear` stays `1.0` always. **Gate OFF = byte-identical to the pre-fade VK-TERRAIN-DETAIL frame.**
- `MC2_VK_TERRAIN_CSM=1` - **presence-gated `=1`, default OFF; requires the `MC2_VK_TERRAIN_DYNSHADOW` chain (lighting) AND CSM active (`MC2_SHADOW_CSM=1`, launcher default).** Ports GL's `calcDynamicShadow` near-cascade cascade-select verbatim into `terrain_tier1.frag` and feeds the near-cascade array (`sampler2DArrayShadow`, binding 8), separate lower-res full-map catch-all cascade (binding 9), cascade matrices, texel-world values, and `csmDepthSpan` into the 1024B terrain UBO. **VK-RUNG8 item-7: the CSM copy-mode was DELETED — the CSM cascades are now DIRECT-VIEW-ONLY.** The VK shadow island re-renders the near-cascade array + full-map catch-all directly into VK-owned D32 depth targets (`MC2_VK_CSM_ARRAY_INTEROP=1` + `MC2_VK_CSM_FULLMAP_INTEROP=1`, under `MC2_VK_SHADOW_INTEROP=1`) and terrain binds their `VkImageView`s at bindings 8/9. The former per-frame CSM `glGetTexImage` + `glCopyImageSubData` GL scratch array + staging + `vkCmdCopyBufferToImage` import (plus its `MC2_VK_CSM_COPYMODE` / `MC2_VK_CSM_BISECT` recon gates, now deprecated no-ops) is gone. Fail-soft to the legacy single-map `MC2_VK_TERRAIN_DYNSHADOW` path when CSM is not built this frame, or during the ~1-2 warmup frames before the shadow island's first CSM product is ready (`csmEnabled=0`; never a hole, never stale — flags reset every frame). **Gate OFF = byte-identical** (single-map dynshadow path unchanged).
- `MC2_VK_TERRAIN_CSM_FULLMAP_EXPERIMENTAL=1` - **parked escape hatch, default OFF.** The OLD "biggest-safe-subset" (feeds only the coarse full-map cascade through the legacy binding-7 single-map path). Confirmed NET-NEGATIVE at normal play distance (camera-swimming blob wash, `a50c6033`) - kept only as a fallback if the near-cascade-array path (`MC2_VK_TERRAIN_CSM` above) regresses. Requires `MC2_VK_TERRAIN_CSM=1` AND this flag; does not feed `csmEnabled` (writes the legacy `dynShadowsEnabled` path instead), so the two paths cannot both be armed - `csmEnabled` takes priority in the frag when both somehow race (mutually exclusive C++ population).
- `MC2_VK_MECH_TIER1=1` — **presence-gated `=1`, default OFF; REQUIRES `MC2_VK_SHARED_SCENE_DEPTH=1`** (VK-P14 MECH TIER-1). The **first VK-native SKINNED region**: co-writes the GL mech batcher's **visible mech instances IN-PLACE** into the SHARED scene GBuffer (`GameOS/gameos/vulkan_mech_island.cpp`), reusing the prop Tier-1 shared-GBuffer plumbing (LOAD-preserve MRT0 color + MRT1 normal + shared D32S8 depth, **GEQUAL reverse-Z**, no composite). **Skinning** (inherited from the mech batcher): the shared mech VBO/IBO (`GpuMechVertex` 48B) copied **ONCE per map**; per-frame the **bone palette** (`GpuMechBone` 64B) + **instance SSBO** (`GpuMechInstance` 64B) are **memcpy'd** from a batcher snapshot; the VS (`shaders/vulkan/mech_tier1.vert`) does **linear-blend skinning** over the 4 bone weights (`mech.vert:156-180`; stock weights `(1,0,0,0)` collapse to rigid) then the **Stuff→MC2 axis swap** `worldMC2=vec3(-x,z,y)` (`mech.vert:178`). NO compute. `worldToClipGL` = `gos_GetMechFixBMVPMat4()` (GL_FALSE); `clip.z += OBJECT_DEPTH_BIAS*clip.w`. **SUB-SLICE 1 (this gate)** = skinning + **FLAT color** — the **P-conjugation / axis-swap silhouette proof** (A/B the mech shape vs GL BEFORE material/lighting). `GBuffer1 = vec4(normalize(N_GLworld)*0.5+0.5, 0.0)` bit-adjacent to `mech.frag`'s `rc_gbuffer1_screenShadowEligible` (a=0.0 = screen-shadow-eligible, no in-frag shadow — the GL screen pass does mech shadow). The batcher's `flush()` still **BUILDS the instance/bone SSBOs + feed** but **SKIPS its GL color draws** when the gate is armed; the island co-writes instead. **BORROWS** the shared engine `VulkanContext`; **FAIL-SOFT** to the GL mech flush if the shared GBuffer/context is unavailable (mechs still draw). Feed exported by `gos_mech_batcher.cpp` (`mc2_mech_island_get_geometry_feed` / `mc2_mech_island_get_frame_feed`). Surfaced in `[VK_MECH_ISLAND]`/`[VK_MECH_TIER1]` log lines: `interop_active`, `vk2gl_sem`, `inst_records`, `bone_records`, `runs`, `forbidden_*` (==0). **DEFERRED follow-ons:** `MC2_VK_MECH_MATERIAL` (albedo) + full `calc_light`/LightsData lighting (`MC2_VK_MECH_LIGHTING`, not yet built — needs the whole `ObjectLights` SSBO mirrored to VK). **Gate OFF (or the require missing, or island not compiled) = byte-identical GL mech color path.**
- `MC2_VK_MECH_MATERIAL=1` — **presence-gated `=1`, default OFF; REQUIRES `MC2_VK_MECH_TIER1=1`.** Copy-once imports the **FIRST run's slot-0 paint-scheme** single `GL_TEXTURE_2D` (via `glGetTexImage`→`R8G8B8A8_UNORM`, once per map — a stock actor's paint scheme is static across the mission) and samples it **un-lit** in the mech FS (single-material probe). **DEFERRED (flagged, not built):** the general case = a **per-frame slot-0 handle → VK-descriptor cache** (multiple paint schemes / per-actor per-packet 2D textures resolved live). Lighting is a **separate** follow-on — until `MC2_VK_MECH_LIGHTING` lands the albedo shows flat (NOT byte-parity with the `tex_color * v_litColor` GL mech). FAIL-SOFT: no slot-0 tex / import failure → flat silhouette fill. **Gate OFF = flat color (skinning proof only).**
- `MC2_VK_MECH_LIGHTING=1` — **presence-gated `=1`, default OFF; REQUIRES `MC2_VK_MECH_TIER1=1`** (visible only with `MC2_VK_MECH_MATERIAL=1`). **VK-MECH-MATERIAL-LIGHTING-1.** Ports the **GL DEFAULT mech shading** into the VK mech island: the VS (`shaders/vulkan/mech_tier1.vert`) runs **per-vertex `calc_light` Lambert** (verbatim port of `lighting.hglsl`'s 4-param kernel; `MC2_STATIC_PROP_LIGHTING` `numLights==0`→preserve-base branch, `kAmbientFloor=0.35`, `+aRGBHighlight*a` clamp — matching `mech.vert:204-235` with `u_lightingMode=1`), and the FS does `albedo * v_litColor + highlight`, then per-actor haze `mix(c, fogColor, inst.fogRGB.a)` (`mech.frag:189-393`). The live GL **LightsData SSBO** (binding 20) is **copied per-frame** into a VK SSBO (`glGetBufferSubData` — a buffer copy, not a color readback; `forbidden_*` stay 0) via `gos_LightDataSsbo_GetInterop`; `g_scene.fogColor.rgb` snapshotted at the scene-UBO upload site (`mc2GetSceneFogColorRGB`). **NOT** the PBR/StandardLit-GGX path — that needs `MC2_MECH_VIEWUNIFORMS`+`MC2_STANDARD_LIT_V1` (both default-OFF), so this matches the **default deploy** mech look. FAIL-SOFT: LightsData unavailable → flat-white lit (`v_litColor=1`). **Gate OFF = un-lit albedo (or flat, per material gate).**

## Top-level render backend selector (VK-P13-SELECTOR-1)

- `MC2_RENDER_BACKEND=gl|vulkan` � **the top-level GL-vs-Vulkan backend selector**, default `gl`. On `=vulkan`, the engine creates the shared `mc2vk::VulkanContext` used by native geometry, native postprocess, and present-tail paths; the retired private-device postprocess islands are no longer part of the dispatch model. Fail-soft context creation still falls back to GL for the session. Device-birth proof surfaces in `[VK_ENGINE_CONTEXT]`.
- `MC2_VK_PRESENT_DIRECT_FBO=1` — **presence-gated `=1`, default OFF** (VK-P14b, copy-1 elimination). Only meaningful with `MC2_RENDER_BACKEND=vulkan` (and the VK present path up). When ON, `gosPostProcess::endScene()` redirects its **single** backbuffer bind (`GameOS/gameos/gos_postprocess.cpp`, the composite-output edge) away from FBO-0 to the FBO wrapping the **shared exportable present image** (`EnginePresentVk`'s `copyFbo`, RGBA8, COLOR_ATTACHMENT0-complete) via the new `EnginePresentVk::presentDirectFbo()` accessor — so the **whole composite + FORCE-43 pillarbox + HUD + ImGui render straight into the shared image**. `presentGLFrame` then **SKIPS copy-1** (the FBO-0→shared blit) entirely and does only interop-signal + copy-2 (VK blit → swapchain + present). The **Y-flip** that copy-1 performed is **moved into copy-2** in the SAME change (dst Y offsets swapped `(0,H)→(H,0)` only on the skip path) so orientation is unchanged (**lockstep** — the two sites agree via the per-frame `directFbo_` flag). **Format/sRGB:** the shared image is `GL_RGBA8` (linear UNORM) and the composite writes gamma-space bytes with `GL_FRAMEBUFFER_SRGB` disabled (LINEAR-COLOR-AUDIT verdict), identical to what copy-1's raw blit produced → **no color shift**. **Fail-soft:** `presentDirectFbo()` returns `0` (endScene falls back to the normal FBO-0 bind + copy-1) whenever the gate is OFF, the present path isn't valid, or the FBO doesn't exist → **no crash**. **Gate OFF (or `=gl`, or GL-only build) = byte-identical FBO-0 + copy-1 path.** Retires the removable half of the present tax (copy-2 stays until VK produces every pixel). Oracle = gate-ON vs gate-OFF present must be visually identical (`verifyReadbackContent()` reads the finished shared image either way + `present_frames`/`readback_changed` counters + same-camera A/B); watch for **upside-down** (Y-flip bug) or **color-shifted** (sRGB bug) frames.
- `MC2_VULKAN_VALIDATION=<preset>` (also documented under the probe seam above) selects the validation preset for the engine `VulkanContext` (`sync` = core + synchronization-validation, used for the `=vulkan` device-birth proof).

### VK present-pixel capture oracle (VK-FIELD-ORACLE-1 / VFX-5/8, all Trace-kind, default OFF)

The OS/PrintWindow grab of a VK swapchain returns only the clear colour, and the GL scene-FBO screenshot is empty under shared-GBuffer — so these are the ONLY true-backbuffer capture path under `MC2_VK_SHARED_GBUFFER`. All require `MC2_VK_PRESENT_VERIFY=1` (the light-cadence readback tick).

- `MC2_VK_PRESENT_DUMP_PPM=<path>` — dumps the FULL composited present image (`sharedImg`, the RGBA8 backbuffer VK blits to the swapchain) to a PPM (P6) on each verify tick. Headless, non-disruptive. `EnginePresentVk::verifyReadbackContent`.
- `MC2_VK_PRESENT_DUMP_FRAME=N` — static frame pin for the PPM dump: capture once at the first verify tick with `g_mc2FrameCounter >= N`. gameosmain extends the verify-tick window to `[N, N+600)` so a pin past the initial 600-frame verify stretch still fires (an unpinnable pin silently reported "PPM never wrote" on a HEALTHY present path — the 2026-07-12 grey-regression false clue; fixed `8c56eafb`).
- `MC2_VK_PRESENT_DUMP_ON_FIRE=1` — DYNAMIC fire-event pin (VFX-5). With `MC2_FX_FORCE_SPAWN=loop` the fixture's FIRST volley arms the PPM capture ~0.5s (30 frames) after itself (`g_mc2FxFireCaptureFrame`), so the capture always lands mid-volley — no static-pin roulette against a mission's intro/scenario clock. Ignored when `MC2_VK_PRESENT_DUMP_FRAME` is set (static pin wins).
- `MC2_VISUAL_CAPTURE_ON_FIRE=1` — GL twin of the above (VFX-8). The engine visual capture (`MC2_VISUAL_CAPTURE_FRAME/_DIR`, smoke mode) triggers at the same fixture-armed fire frame instead of the static pin, so GL-vs-VK weapon-class pairs capture the same fire-relative instant on both backends.
- **Capture recipe gotchas:** `MC2_LOG=1` or all game stdout/stderr goes to NUL; PPM dump path must be ABSOLUTE (mission load chdirs); never combine `MC2_SMOKE_MODE` with the PPM oracle (VK present never runs under it, and standalone smoke mode needs `--mission` double-dash). For the full HUD headless, `MC2_SCREENSHOT_SOURCE=backbuffer` reads FBO 0 (with HUD + ImGui) instead of the pre-HUD scene FBO.

### VK fail-soft semantics (VK-FAILSOFT-DISARM-1/2, 2026-07-14)

Post-item-8 there is NO per-layer GL fallback: if the `MC2_VK_SHARED_GBUFFER` content family stays armed while present runs GL, every VK-owned layer VANISHES (torn grey scene with only residual GL-drawn VFX/decals). Two rules now hold:
- **`forceGLFallback` disarms the umbrella family** (`g_mc2VkSessionForcedGL`, read per-call by `mc2vkgate::umbrellaOn()`), so a genuine session fail-soft renders the COMPLETE GL scene, not a half-disarmed torn one. All umbrella gate reads are per-call (not memoized) so a mid-run fail-soft propagates.
- **Present-level failures NEVER force GL** — a zero surface extent (window minimized) or `vkCreateSwapchainKHR` `NATIVE_WINDOW_IN_USE` (transient surface contention) DEFERS present init and retries from the frame loop (~2×/s); the session stays VK. Mid-run present failure tears the present stack down and re-enters the same deferred-retry path. While deferred the frame loop does NOT GL-swap (a GL SwapBuffers on the shared HWND holds the WSI binding and starves the VK swapchain retry — the deferral deadlock). Only the structural "present image not VK-owned" decline still forces GL, plus a ~60s budget of non-minimized failed retries.
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
- `MC2_FRAMEGRAPH_REORDER_SPMECH` — **REJECTED experiment, default OFF, presence-gated** (`mclib/txmmgr.cpp`). Would swap the StaticProp-flush ↔ Mech-flush order (the one legal adjacent swap the reorder oracle proved). PARITY-FAILED on depth-EQUAL ties (mc2_24 overlapping opaque fragments resolve order-dependently); candidate rejected, kept only as an A/B lever. Never set in production. `=0`/unset = today's order verbatim (byte-identical).
- `MC2_FRAMEGRAPH_DRYRUN=1` — frame-graph dry-run mode (`render_contract.cpp` `dryrunEnabled()`). Default **OFF**. Value must be `'1'`.
- `MC2_FRAMEGRAPH_AMBIENT_GUARD` — ambient-handoff contract guard (colorMask / depthFunc / depthWrite axes; `render_contract.cpp`). **DEFAULT ON**; only `=0` disables. Companion CI check: `scripts/check-ambient-guard.py` (reads the dump counter).
- `MC2_FRAMEGRAPH_AMBIENT_FATAL=1` — hard-`abort()` on an ambient-guard mismatch (CI/bisection teeth). Default **OFF**, presence-gated. Legacy alias `MC2_AMBIENT_ASSERT_FATAL` also honored.
- `MC2_RENDER_BACKEND_IFACE=1` — route the scene-FBO bind through `IRenderBackend` / `GLBackend` instead of direct `glBindFramebuffer` (RENDER-BACKEND-IFACE-FBO-1; `gos_postprocess.cpp`). Default **OFF** = direct GL, byte-identical. ON = same GL call via the backend seam (proves the seam routes; output identical). Value must be `'1'`.
- `MC2_RENDER_BACKEND_REGION_IFACE=1` — MASTER gate (RENDER-BACKEND-REGION-IFACE-1, Layer-6 ENTRY; `gos_postprocess.cpp`). Route the WHOLE PostprocessFog region (EdgeFog + OOB fog) through `IRenderBackend::runRegion()` with a backend-neutral context (`RenderResourceId` + params + dims, NO raw handles). Default **OFF** = the original direct-GL fog sites run, abstraction fully skipped, byte-identical. ON = one selectable region routed through the backend; BOTH original fog sites skipped (no double-apply). Value must be `'1'`.
- `MC2_POSTPROCESS_BACKEND=gl|vk` — sub-selector for the region iface (only meaningful with the master gate `MC2_RENDER_BACKEND_REGION_IFACE=1` ON). `gl` (default, ships) = the extracted GL fog wrapper (`runFogRegionGL`, `runEdgeFog`+`runFogOob` unchanged); byte-identical to the original path (`region_impl=GLInline`). `vk` (opt-in only) = **the TRUE Vulkan opt-in** (POSTPROCESS-VULKAN-BACKEND-OPT-IN): selecting `vk` runs the shared-GBuffer native fog path (`runVkPostFogNative`) directly. It requires `MC2_VULKAN_ISLAND`, `MC2_RENDER_BACKEND=vulkan`, and a live shared-GBuffer substrate. Fail-soft: on a genuine Vulkan device/init/render failure the region falls back to the GL wrapper (`region_impl=FallbackGL`), no crash, output == GL. On success `region_impl=VulkanSubgraph`, `vk_draws=2`. NO default flip — `gl` ships. The standalone subgraph gate is retired; the per-island fog gates remain independently runnable for fallback/oracle testing. Surfaced in the health dump as `render_backend_region.backend_region_selected` = `gl|vk|fallback_gl`. **Same selector now also picks the PostprocessSSAO region** (VK-REGION-SSAO-1; `runSSAORegionThroughBackend`), surfaced separately as `render_backend_ssao_region.backend_region_selected`. SSAO only runs at all when `MC2_SSAO=1` (unchanged, still default-OFF) — golden-parity/backend-compare runs against `--region PostprocessSSAO` must additionally force `MC2_SSAO=1` or the region never enters the frame. **And the PostprocessHzb region** (VK-REGION-HZB-1; `runHzbRegionThroughBackend`), surfaced as `render_backend_hzb_region.backend_region_selected`; HZB only runs at all when `MC2_HZB_BUILD=1` (unchanged, still default-OFF) — `--region PostprocessHzb` compares force `MC2_HZB_BUILD=1`. Note the HZB pyramid feeds no visible pass yet (diagnostic substrate): the pixel compare proves no-regression + non-fallback; the math proof is the bit-tight `hzb_fixture` probe token.

- `MC2_VK_POST_HZB=1` - rung-8.4 native HZB gate. envTriState opt-in, default-OFF. Highest precedence at the HZB site when `MC2_HZB_BUILD=1`: samples the shared scene depth directly on the shared engine VulkanContext, builds the reverse-Z pyramid natively with one separate R32F image per ceil-ladder level, and falls back to the unchanged GL `runHzbReduce()` body if not ready. `MC2_HZB_PROBE=1` intentionally forces the native gate to decline so the probe keeps reading the fresh GL pyramid instead of stale GL textures. Zero-copy normal path: no `glGetTexImage` depth ingress, no `glTexSubImage2D` egress.
- `MC2_VK_POST_COMPOSITE=1` - rung-8.5b native composite runtime slice. envTriState opt-in, default-OFF. With `MC2_RENDER_BACKEND=vulkan`, `MC2_VK_SHARED_GBUFFER=1`, and `MC2_VK_PRESENT_DIRECT_FBO=1`, Visual/ObjectIdDebug/Thermal/LowLight modes sample shared scene color0 on the shared engine VulkanContext and write the postprocess composite directly into EnginePresentVk's shared present image. The native path includes the GL FXAA algorithm and tunables (`MC2_FXAA`, `MC2_FXAA_SUBPIX`, `MC2_FXAA_EDGE_THRESHOLD`, `MC2_FXAA_EDGE_THRESHOLD_MIN`), FORCE-43/pillarbox via a VK black clear and narrowed composite viewport, LowLight/NightVision (`MC2_VIEWMODE_FRAMEWORK=1 MC2_VIEW_MODE=5`), and ObjectIdDebug/Thermal via the shared R32UI object-id slot (`MC2_OBJECT_ID_BUFFER=1`). `MC2_SHADOW_DEBUG_MODE=dynamic` uses the native CSM shadow-debug overlay when the shadow island exposes a same-frame CSM array view; static/non-ready debug sources fall back to the unchanged GL overlay. Proof line: `[VK_PRESENT] composite_vk_frames=N shadow_debug_vk_frames=N` with `copy1_blit_frames=0`, `copy1_skip_frames=N`, `fallback_reason=""`. Gate OFF remains byte-identical GL composite.
- `MC2_VK_PRESENT_NO_GL_IMPORT=1` - rung-8.7 present no-import proof gate. Default-OFF. With `MC2_RENDER_BACKEND=vulkan`, `MC2_VK_PRESENT_DIRECT_FBO=1`, `MC2_VK_POST_COMPOSITE=1`, native HUD/ImGui enabled, and `MC2_VK_PRESENT_NATIVE_STRICT=1`, `EnginePresentVk` allocates the present image as a VK-only RGBA8 target instead of an exportable GL memory-object/FBO. `gosPostProcess::endScene()` arms a VK-only direct-present sentinel, native composite writes the image, and `presentGLFrame()` skips copy-1 and the GL->VK handoff. Proof fields: `[VK_PRESENT] composite_vk_frames=N`, `copy1_skip_frames=N`, `gl_handoff_skip_frames=N`, `hud_vk_consumed_frames=N`, `imgui_vk_frames=N`, `fallback_reason=""`, plus the one-time `[VK_PRESENT] MC2_VK_PRESENT_NO_GL_IMPORT=1` gate log. This removes the present-specific GL import; shared scene color/depth ownership remains a separate resource-strict blocker.
- `MC2_VK_PRESENT_SKIP_GL_HANDOFF=1` - rung-8.7 present handoff proof gate. Default-OFF. With `MC2_RENDER_BACKEND=vulkan`, `MC2_VK_PRESENT_DIRECT_FBO=1`, and `MC2_VK_POST_COMPOSITE=1`, `EnginePresentVk::presentGLFrame()` can consume a present image already written by Vulkan without issuing or waiting a GL->VK interop semaphore. Proof field: `[VK_PRESENT] gl_handoff_skip_frames=N`. This is intentionally explicit because any remaining GL writer to the shared present image still requires the normal handoff.
- `MC2_VK_PRESENT_TAIL_HOOK=1` - rung-8.6 final-tail seam proof. Default-OFF. When set, `EnginePresentVk::presentGLFrame()` records a behavior-neutral command-buffer dependency after the final GL->VK handoff and before the swapchain blit. `[VK_PRESENT] tail_hook_frames` proves the exact native HUD/ImGui insertion point executes while the shared present image is Vulkan-owned. Gate OFF is byte-identical present behavior.
- `MC2_VK_HUD=1` - rung-8.6f native HUD queue gate. Default-OFF. `gosRenderer::flushHUDBatch()` deep-copies the already-scaled ordered GameOS HUD batch into `EnginePresentVk` for present-tail native replay; GL replay remains active until the Vulkan renderer consumes every call kind. Proof fields: `[VK_PRESENT] hud_vk_queued_frames`, `hud_vk_consumed_frames`, `hud_vk_draws_last`.
- `MC2_VK_HUD_COLOR_PROOF=1` - rung-8.6h/8.6i native HUD proof renderer. Default-OFF. With `MC2_VK_HUD=1` and `MC2_VK_PRESENT_TAIL_HOOK=1`, `EnginePresentVk` renders untextured HUD line/tri records plus mirrored textured quad/text records into the shared present image before native ImGui. GL HUD replay remains active in this proof slice. Proof fields: `[VK_PRESENT] hud_vk_color_frames`, `hud_vk_color_draws_last`, `hud_vk_textured_frames`, `hud_vk_textured_draws_last`, `hud_vk_texture_uploads`.
- `MC2_VK_HUD_SKIP_GL=1` - rung-8.6 native HUD ownership gate. Default-OFF. With `MC2_VK_HUD=1`, `MC2_VK_HUD_COLOR_PROOF=1`, and a live present tail, `gosRenderer::flushHUDBatch()` skips legacy GL HUD replay only when every queued HUD draw is representable by the native ordered renderer. Proof line: `[VK_HUD_SKIP v1] reason="native_full_coverage"`.
- `MC2_VK_IMGUI=1` - rung-8.6 native ImGui gate. With `MC2_RENDER_BACKEND=vulkan` and `MC2_VK_PRESENT_TAIL_HOOK=1`, `GuiRuntime` builds ImGui draw data without the OpenGL3 renderer backend and `EnginePresentVk` renders it through `ImGui_ImplVulkan` inside the final present command buffer, after GL->VK handoff and before the swapchain blit. Proof lines: `[VK_IMGUI] requested=1 used_vulkan=1 fallback_reason=""` plus `[VK_PRESENT] imgui_vk_ready=1 imgui_vk_frames=N`. If the Vulkan renderer is not ready, it logs `used_vulkan=0` and does not render ImGui through GL in that run. Gate OFF is byte-identical GL ImGui.

## SPFLUSH cost-split decomposition (SPFLUSH-COST-SPLIT-1)

- `MC2_STATIC_PROP_FLUSH_COST_SPLIT=1` — RDTSC cost-split of `StaticPropRegistryFlush`. Default **OFF**. Emits `[SPFLUSH_COST_SPLIT v1] event=summary` every 10 frames with per-bucket ns averages: `submit_loop`, `inst_build`, `map_lookup`, `color_fill`, `actor_record`, `world_to_block`, `substrate_append`, `baseinstance_upload`, plus lifetime + window dirty counters (`invalidates`, `registrations`, `rebuilds`, `light_writes`). TSC calibrated once on first flush (~1ms spin). Zero behavior change.

## Mission interface cost-split (MISSION-INTERFACE-PERF-1)

- `MC2_IFACE_COST_SPLIT=1` — chrono cost-split of `MissionInterfaceManager::update()` (the `GameLogic.Mission.Interface` Tracy zone). Default **OFF**, presence-gated. Emits one `[IFACE_PERF v1] event=window` stdout line every 900 frames + `event=mission_end` partial flush at mission teardown, with per-phase `{avg_us,max_us}`: `invProj`/`LOS`/`controlGui`/`updateTarget`/`postTarget`/`drawBars`/`rollovers` plus ControlGui sub-phases (`cg.pauseWnd`, `cg.btnHover`, `cg.rosterScan`, `cg.moverState`, `cg.tacMap`, `cg.infoWnd`, `cg.vehicleTab`, `cg.fgBar`) and `invProj_walks`/`invProj_cacheHits`. Superset of the older `MC2_MIF_SPLIT` (either env enables both; `MC2_MIF_SPLIT` keeps its shutdown-only `[MIF_SPLIT v1]` line). `code/missiongui.cpp` + `code/controlgui.cpp`. Zero behavior change.
- `MC2_PICK_FALLBACK_COARSE=1` — coarse-to-fine closest-vertex fallback in `Camera::inverseProject` (`mclib/camera.cpp`). Default **OFF**, presence-gated. The "off map" fallback is the PRODUCTION ground picker whenever the terrain quadList is empty (LOD-chunk terrain path: `numTiles=0`); legacy code brute-force-projects ALL `realVerticesMapSide`² map vertices per cache-miss frame (~890µs/frame on mc2_24 = 85-95% of `Mission.Interface`). ON = stride-8 coarse pass + full-res ±9 refine around the coarse winner — same nearest-projected-vertex answer in practice at ~1/60th cost. With `MC2_PICK_CAP_TRACE=1` also set, every 32nd walk re-runs the brute force and logs a `[PICK_FALLBACK] parity=ok|DIFF` stderr line (and keeps the brute answer on divergence).

## TRACKV CPU perf kill-switches (2026-06-03)

- `MC2_STATIC_PROP_LIVE_BUILDER=1` — DrawPacket v8 kill-switch. Default **OFF** = snapshot is sole static-prop draw-packet owner (live builder + per-flush compare retired). `=1` restores the v3-flip dual build + compare path (regression bisect / A-B).
- `MC2_STATIC_PROP_SNAPSHOT_BRIDGE_COMPARE` — default **OFF**. STATICPROP-SNAPSHOT-BRIDGE-COMPARE-1 gate.
  - unset/0: no-op (no behavior change).
  - 1: builds an independent registry oracle each frame and compares field-by-field with the produced rows (legacy propBuf on dirty frames, the memcpy'd arena span on clean frames). Emits `[SNAPSHOT_BRIDGE_COMPARE v1] path=DIRTY|CLEAN ...` per frame with: regGen, cullVer, rowCount, oracleCount, rowCountMismatch, immutableMismatch, hasCullMismatch.
- `MC2_STATIC_PROP_SNAPSHOT_FILL_DIRTYONLY` — default **ON** (flipped after Tracy proof). STATICPROP-SNAPSHOT-FILL-DIRTYONLY-1 gate.
  - unset/1: on clean `registryGeneration` AND `cullRecordVersion` (vs cached), skip Fill + WriteLoop and `memcpy` the cached rows into the snapshot arena (`Extract.SP.CleanCopy` zone); per-prop diagnostic counters restored from cache. Dirty frames run the legacy path and refresh the cache. First ~10 frames after mission load rebuild while cullVer settles — win is steady-state. **Tracy (user, mc2-win64-water):** `ExtractRenderSnapshot` median 1.68ms→36.7µs (−97%); `Extract.SP.Fill` 1.15ms + `WriteLoop` 159µs → gone, replaced by `CleanCopy` ~15µs.
  - 0: legacy full rebuild every frame (`fillStaticPropSlots` + per-prop WriteLoop) — kill-switch.
  - Combine with `MC2_STATIC_PROP_SNAPSHOT_BRIDGE_COMPARE=1` to prove cached rows still match a live registry rebuild (`path=CLEAN immutableMismatch=0`). Note: compare ON forces the oracle's own Fill, so Tracy under compare will NOT show the Fill drop — measure with compare OFF.
- `MC2_THIN_CANARY=1` — re-enable Probe-6 thin-record canary readback (per-frame ~80KB `glGetBufferSubData` ×2 + CPU recipeIdx/flags compare). Default **OFF**. Diagnostic only; gated 2026-06-03 (was ungated per-frame stall).
- `MC2_STATIC_PROP_LEGACY_DISPATCH=1` — DrawPacket v7 kill-switch: revert to legacy `glMultiDrawElementsIndirect`. Default **OFF**.

## Shadow tuning

- `MC2_SHADOW_BOUNDED_NEAR_FIT=1` — cap fit radius for crisp near shadows. Default **OFF**.
- `MC2_SHADOW_BOUNDED_NEAR_RADIUS=2500` — bounded near-fit radius in wu. Default 2500.
- `MC2_SHADOW_FRUSTUM_DIAG=1` — per-frame coverage probe. Read-only.
- `MC2_STATIC_PROP_BUILDING_SHADOW=1` — replay all buildings into world-fixed static shadow map. Default **OFF**. `=2` adds trace.
- `MC2_SHADOW_DYNAMIC_PROP_CASTERS` — feed dynamic caster pass from registry. Default **ON**. Kill=`=0`.
- `MC2_SHADOW_ROBUST_BASIS` — light-basis singularity guard + AABB corner-scarcity fallback (SHADOW-ROBUST-BASIS-1). Default **ON**. `=0` restores legacy (byte-identical for normal suns; can go singular).
- `MC2_SHADOW_STATIC_BANDFIX` — STATIC-SHADOW-BANDING-1: raises the static-shadow LOD tap floor in `gos_terrain.frag` (16/8/4 → 16/12/8) so shadow.hglsl's gradient-adaptive PCF doesn't undersample its Poisson disk at the wide radius used on flat terrain (root cause of the reported grass/rolling-terrain shadow banding — NOT a map-resolution or depth-precision issue). Default **ON** (2026-07-18, field bug: shadow banding persisted in shipped default config). Kill=`=0` (byte-identical legacy).
- `MC2_SHADOW_STATIC_BANDFIX_V2` — STATIC-SHADOW-BANDING-2: round-1 (taps) refuted as sufficient by field check (taps=16 still banded on grass/dirt slopes). Mechanism A measured: static map world-per-texel (~4.6 wu on mc2_01, 4096² ortho) / tan(slope) predicts the observed ~25–90 wu diagonal contour bands on 3–10° slopes. v2 feeds `calcShadow`/`calcDynamicShadow` bias the real height-derived slope normal (`computeTerrainNormalFromHeight`) instead of a fake flat-up normal — slope-scaled bias finally tracks actual tilt. Lighting normal untouched. Pair with v1. Default **ON** (2026-07-18, field bug: banding in/around mech shadows on terrain; A/B verified mc2_01 f1800, high-pass stripe amplitude 3.67→1.12). Kill=`=0` (byte-identical legacy).

### Dynamic CSM (reworked 2026-06-16 `8d36b37d`/`b8f764b6`/`13e96cc9`/`ead760df`)

- `MC2_SHADOW_CSM` — master cascaded-shadow-map toggle. Default **ON** (2026-06-18, `8ff13a36`); opt out with `=0`.
- `MC2_SHADOW_MAP_SIZE` — per-cascade shadow texture size. Default **8192**. 3×8192² ≈ 805 MB VRAM; `=4096` ≈ 201 MB (still sharp at R0=512).
- `MC2_SHADOW_CSM_R0` — near-cascade fit radius (WU). Default **512** (→ 0.25 WU/texel at 4096).
- `MC2_SHADOW_CSM_R1` — mid-cascade fit radius (WU). Default **4096**.
- `MC2_SHADOW_CSM_COUNT` — cascade count. Default **3** (R0 near / R1 mid / full-map far).
- `MC2_SHADOW_CSM_SOFTNESS` — PCF softness. Default **0.9**.
- `MC2_SHADOW_OBJ_NORMAL_BIAS` — object self-shadow normal-offset bias (kills residual acne). Default **2.0**.
- `MC2_SHADOW_MECH_SOFT` — mech self-shadow softness (wider PCF penumbra + terminator smoothstep so flat low-poly facets fade instead of flip + raised floor). Default **1.0**, clamp [0,4]. Object self-shadow is now per-type via GBuffer1.a mask: terrain skip / static-prop NO self-shadow / mech soft (`3253d582`).
- `MC2_SHADOW_GUARD_CLAMPED_SUN` — SHADOW-SUNCLAMP-1: makes the `shadow_screen.frag` back-face guard (`if (NdotL < 0.05) return 1.0;`) dot against the **same altitude-clamped sun the shadow matrices are built from**. `gameos_graphics.cpp:10197` feeds `-gos_GetTerrainLightDir()` into `buildStaticLightMatrix`, which clamps it via `mc2ClampShadowSunAltitude` (15° minimum); `runScreenShadow` previously uploaded the byte-identical expression **unclamped**. On a mission authoring a near-horizontal sun the guard therefore became a purely *azimuthal* test and discarded shadows the shadow map genuinely contains, splitting the world into a shadowed and an unshadowed half — the mc2_10 night overview asymmetry. Default **ON**; `=0` reverts to the legacy raw-sun guard. Byte-identical wherever `sin(LightDirPitch) >= sin(15°) = 0.2588`, so **only mc2_10** (`LightDirPitch=180` → `0.0`) changes; mc2_01 (50°), mc2_03 (46°), mc2_24 (75°) are untouched. Scene brightness is unaffected — the shading light dir / terrain N·L is a separate uniform and is deliberately not touched.

- `MC2_TERRAIN_SHADOW_GUARD_CLAMPED_SUN` — TERRAIN-SHADOW-GUARD-1: the **terrain-side twin** of `MC2_SHADOW_GUARD_CLAMPED_SUN`, and the one that actually moves world pixels. `shaders/include/shadow.hglsl`'s back-face guard (`if (NdotL < 0.05) return 1.0;`) and its SHADOW-ALT-SCALE-1 `sunAltSin = clamp(lightDir.z, 0, 1)` consumed the **raw authored** `terrainLightDir`, while the shadow matrices were built from a sun clamped to `MC2_SHADOW_MIN_SUN_ALT` (15° default). Under `MC2_SHADOW_STATIC_BANDFIX_V2` (default ON) `shadow.hglsl` resets `normal = vec3(0,0,1)` immediately before the guard, so on terrain the test reduces to `terrainLightDir.z < 0.05`. mc2_10 authors `LightDirPitch = 180` → `sin(180°) = 0` → **every** terrain / overlay / decal / LOD-chunk pixel early-outs fully lit while props and mechs keep their shadows via the unguarded static path — which is exactly why the mech-pixel-only `MC2_SHADOW_GUARD_CLAMPED_SUN` fix captured byte-identical. Implemented as a **compile-time shader define** (`MC2_SHADOW_MIN_SUN_SIN`) emitted by `mc2TerrainShadowSunDefine()` at every terrain-family program prefix (main terrain variations, thin, surface, mask-solid, overlay + decal, LOD-chunk base + tess); `shadow.hglsl` defaults it to `0.0`, so programs that do not inject it (props, mechs) compile byte-identical. Default **ON**; `=0` emits no define and reverts to the raw-sun guard. Byte-identical wherever `sin(LightDirPitch) >= sin(15°) = 0.2588` — only **mc2_10** changes; mc2_01 (50°), mc2_03 (46°), mc2_24 (75°) are untouched. Scene brightness is unaffected: the shading light dir (`gos_terrain.frag:820` diffuse N·L) is deliberately not routed through the clamp.- `MC2_SHADOW_MIN_SUN_ALT` — SHADOW-SUN-ALT-CLAMP-1 tunable: minimum **effective sun altitude in degrees** used by the shadow projection matrices (`mc2ClampShadowSunAltitude`, `gos_postprocess.cpp`). Default **15**; `0` or negative disables the clamp. Azimuth is preserved. Applied at both the static full-map matrix and the dynamic cascade matrices. Missions authoring the sun at or below the horizon (mc2_10 `LightDirPitch=180` → `lightDirection.z = 0`) otherwise smear absurdly elongated caster silhouettes across up-facing slopes (the "black streak fans"). Suns already above the minimum are byte-identical. Since SHADOW-SUNCLAMP-1 this clamp also governs the screen-shadow back-face guard. Resolved once at process start.
- `MC2_IMPOSTOR_DIST` — Static-prop / tree impostor LOD switch distance in world units (`mclib/bdactor.cpp`, `TreeAppearance::selectActiveLOD`). Default **800**. A hysteresis deadband is applied around it (`_near = dist*0.92`, `_far = dist*1.08`), so at the default the LOD flip band is **[736, 864]**. Lowering it pulls impostor cards nearer the camera (cheaper, more visible pop); raising it keeps full-geometry LODs further out. Resolved once at process start.
- `MC2_PROP_LOD_SHADOW_CAST` — SHADOW-DECISIONS-1: re-enable shadow **casting** for far/impostor static-prop LODs (`lod >= 1`). Default **OFF**; `=1` enables. OFF keeps `setRecipeNoShadow(regIdx, true)` in `TreeAppearance::registerStatic` (`mclib/bdactor.cpp`), i.e. the shipped caster set, byte-identical. The exclusion's original rationale ("shadow depth pass has no alpha discard" → rectangular blob shadows) has been false since `b8f764b6`; the bound program is `shadow_static_prop_alpha[_atlas]`, both default-ON, both alpha-discarding. Only affects `getDynamicPropShadowInstances` (the full-registry replay) — visible LOD1 types already cast via `flushShadow`. ON grows the caster set at distance; candidate cure for missing distant-tree shadows.
- `MC2_SSAO` — SSAO-GTAO-LITE-MVP-1 (Track V): half-res screen-space ambient-occlusion pass (`gosPostProcess::runSSAO`) reusing scene depth + GBuffer1 normals, composited multiplicatively into scene color before tonemap. Default **OFF**; `=1` enables. OFF skips `runSSAO` entirely → byte-identical. Cost is **full-screen**, not proportional to prop pixel coverage. Tunables `aoRadius` / `aoStrength` / `aoBias`; `MC2_SSAO_DEBUG=1` overwrites with raw AO grayscale.
- `MC2_PROP_FOLIAGE_ALPHAFLAG` — FOLIAGE-ALPHAFLAG-1: makes the per-instance flags **bit 2** emitted by the static-prop *recipe* paths (`BldgAppearance`/`TreeAppearance::registerStatic`, `mclib/bdactor.cpp`) carry its documented `kFlagIsAlphaTest` meaning, sourced from `TG_TypeShape::alphaTestOn` via the new `TG_Shape::GetIsAlphaTest()` seam. Both sites still built the word with `if (child->GetIsSpotlight()) flags |= (1u << 2)` — the meaning bit 2 carried *before* T3.2 repurposed it — and since spotlight children are `continue`-skipped ~10 lines above both sites, **bit 2 was never set on the recipe path at all**, silently killing two shader features there: the alpha-test *exclusion* in the `MC2_PROP_BASE_LIGHT_DIR` wrap term (`static_prop.vert:800`) and two-sided `abs(NdotL)` foliage lighting (`static_prop.vert:731`). Default **OFF** → recipe path emits the same zero bit as the shipped build → byte-identical. **The gate sits at the emission site, not in the shader, on purpose**: `submitMultiShape` already sets bit 2 correctly from the same source (`gos_static_prop_batcher.cpp:5575`), so two-sided foliage lighting is *already live* on that path — gating the shader's `abs()` branch would have **changed** shipped pixels, whereas gating here makes ON *converge* the two paths. Does **not** touch the `a=0.1875` alpha-test shadow class, which rides the independent per-*packet* `materialFlags` channel (`gos_static_prop_batcher.cpp:2894`) — no double-classification, no shoreline movement.
- `MC2_PROP_BASE_LIGHT_DIR` — PROP-BASE-LIGHT-DIR-1: gives the static-prop baked per-vertex `base_light` seed a **directional (hemispherical-wrap) orientation term**, so faces pointing away from the mission key light read darker than faces pointing toward it. **Value is a float strength `0..1`, not a bool** — the strength *is* the enable, so `0.0` (default, and the value used when unset) is OFF. Clamped to `[0,1]`. Default **OFF** → `static_prop.vert`'s `u_propBaseLightDirStrength > 0.0` guard skips the block → `seed == base_light` → byte-identical. Suggested ON value `0.45`.
  - **Why this and not a shadow gate.** The user's defect ("this building face should be in shadow as the light is from behind") is not a shadow bug. Prop lighting is `base_light + sunColor*ndl + fill`, and `shadow_screen.frag` modulates the **sun term only** — but a face with the light behind it already has `ndl == 0`, so a shadow term has nothing to attenuate. That is why `MC2_PROP_SHADOW_RECEIVE` measures ≤0.05 % of pixels by day and 0.000 % at night (`.claude/SHADOW-DECISIONS-1-STATUS.md` §2). The wall is bright because `base_light` is a baked per-leaf constant with **no orientation term**.
  - **Mechanism.** `factor = mix(1 - strength, 1, 0.5 + 0.5*dot(N, u_terrainSunMC2))`, multiplied into the seed before the FAR-BACKDROP-LIT mix. `factor ≤ 1.0` always, equality only for a face pointing at the sun ⇒ **it redistributes and can never brighten**, so it composes safely with `MC2_STATIC_PROP_IBL_NIGHT_SCALE` and does not worsen the over-bright-at-night complaint. Multiplying the existing per-vertex seed preserves authored variation (this is not tone-flattening).
  - **No double-count with `MC2_STATIC_PROP_IBL_SH`.** That probe is an *additive* ambient irradiance summand; this is a *multiplicative* modulation of a different summand. The IBL contribution is never scaled by this gate.
  - **Alpha-test instances are excluded** (foliage/fence cards are two-sided — the `ndl` branch already uses `abs()` for that reason — so a signed wrap would darken an arbitrary side). Implemented in the vertex shader rather than the CPU bake because far/impostor LODs register via `TransformMultiShape_HierarchyOnly` and never run the per-vertex light bake, so a bake change could not reach them. Visual-only.
- `MC2_PROP_SHADOW_RECEIVE` — PROP-SHADOW-RECEIVE-1: static props RECEIVE the dynamic CSM cascade (building self-shadow + prop-on-prop) in `shadow_screen.frag`. Default **OFF**; `=1` enables. OFF = legacy static-map-only reception for the a=0.25 prop class (pixel-identical). Shares the CSM PCF ledger + `MC2_SHADOW_MECH_SOFT` terminator; floor 0.4 min-combined with the static map (no double-darken).
- `MC2_SHADOW_PROP_ALPHA` — tree-foliage shadow alpha-test (`shadow_static_prop.frag`). Default **ON**. Covers BOTH the legacy per-texture path and (since SHADOW-PROP-ALPHA-ATLAS-1) the coalesce/BC7-atlas path.
- `MC2_SHADOW_PROP_ALPHA_ATLAS` — SHADOW-PROP-ALPHA-ATLAS-1 kill switch: routes the prop-shadow alpha-test through the SAME coalesce group/bucket `GL_TEXTURE_2D_ARRAY` the color pass samples (program `shadow_static_prop_alpha_atlas`, per-packet `u_texLayer`/`u_uvScale`) whenever coalesce is armed. Default **ON**; `=0` reverts to legacy per-texture binds (pre-slice behavior: atlas trees whose legacy gos texture is dead cast solid square shadows). Inert when `MC2_SHADOW_PROP_ALPHA=0` or coalesce is disarmed.
- `MC2_CLOUD_SHADOW` — cloud-shadow pass. Default **ON**.
- **DEBUG-only:** `MC2_SHADER_PATH_TINT` (shader-path tint); `MC2_TERRAIN_DEBUG_MODE` / `MC2_TERRAIN_LOD_CHUNK_DIAG` = **30/31** = dynamic-shadow viz.

## GPU cull readback diagnostics

- `MC2_GPU_CULL_READBACK_TRACE=1` — gate all `[GPU_CULL v1]` diagnostic prints (fallback_n2, fallback_conservative, readback_ok, lifecycle_snapshot, motion_tolerance). Default **OFF**. Without this, zero stdout/fflush on hot path. `readback_stale_reset` (10-frame miss → slot abandon) remains unconditional as it signals a real error.

## Always-on / safety

- `MC2_TGL_POOL_TRACE=1` — TGL pool NULL trace
- `MC2_DESTROY_TRACE=1` — per-destruction lifecycle snapshot
- `MC2_GL_ERROR_DRAIN_SILENT=1` — suppress first-error prints
- `MC2_HEARTBEAT=1` — stderr heartbeat per second
- `MC2_REVERSE_Z_TRACE=1` — reverse-Z lifecycle prints
- `MC2_GL_DEBUG_FATAL=1` — abort on GL_DEBUG_SEVERITY_HIGH
- `MC2_XFORM_PARITY_FATAL=1` — abort when F1-3C clip-space parity probe fails (ViewUniforms.worldToClipGL vs legacy terrain MVP, max_diff>1e-5). Default **OFF** = log-only. Host counterpart: `tests/unit/test_xform_convention.cpp` (XFORM-CONVENTION-HARNESS-1).
- `MC2_ANIM_CADENCE_FIX` — advance mech gait at most once per render frame; prevents a second same-frame gait step when `Mover::getLOSPosition()` (mover.cpp:3528) re-invokes `appearance->update()` for a weapon-node LOS refresh (`mclib/mech3d.cpp`). **Default ON** (user-confirmed fix, mc2_17 Catapult/Bushwacker); `=0` disables to A/B the double-step.
- `MC2_STATIC_UPDATE_SKIP` — skip the per-frame static building/tree update walk when nothing is dirty (`gameosmain.cpp`, `code/terrobj.cpp`, `code/bldng.cpp`). **Default ON**; `=0` restores the legacy always-walk behavior.

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

- `MC2_SPLIT_PATH_PARITY_TRACE=1` — design C runtime tier (`GameOS/gameos/gameos_graphics.cpp` `gosRenderer::init`), default **OFF** = static-getenv latch, byte-identical. Emits machine-parseable `[SPLIT_PATH_PARITY] site=terrain_material_load pair=tess_vs_modern_shadow tess=<0|1> shadow=<0|1> match=<0|1>` at terrain-material load; `match` asserts the ISSUE-34 decouple invariant (modern shadow path stays active whether tess is on, kill-switched via `MC2_TERRAIN_TESS=0`, or TCS/TES compile-failed). Any `match=0` line is a RED smoke verdict via the `split_path_parity` marker-bucket row (`scripts/smoke_lib/logparse.py`). Nightly pairing: `run_smoke --mission mc2_01 --mission mc2_24` with `MC2_TERRAIN_TESS=0` + this gate. Static sibling: `scripts/check-split-path-contracts.py` (see `docs/render-backend-seams/split-path-pairs.md`).
- `MC2_LOG_FILE_RESOLVE` — design E tier 3 (`mclib/file.cpp`, parsed at `InitModSearchPaths`), default **OFF** (`0`). `=1`: level-1 stdout tags (`[mod]`/`[mod-miss]`) + shutdown summary `[file-resolve] opens=N misses=M` (opens = successful `File::open` resolutions; misses = full-ladder misses incl. `fileExists` probes). `=2`: level 1 plus a JSONL resolve trace (layer ∈ `mod:<id>`/`base-loose`/`base-strip`/`fastfile`/`cd`/`MISS`) to `MC2_RESOLVE_TRACE_FILE`; `fileExists()` misses also emit MISS records (probe API — the CD-presence check probes absent files by design; the benign-probe baseline of `scripts/check-file-resolve-miss.py` absorbs them). Promoted from the check-env-registry allowlist to `kAuxEnvVars` in META-FIX-BATCH-1.
- `MC2_RESOLVE_TRACE_FILE=<path>` — output path for the level-2 JSONL resolve trace (opened for append at `InitModSearchPaths`; relative paths resolve against the game CWD = exe dir). Only consulted when `MC2_LOG_FILE_RESOLVE>=2`. Consumed by the nightly `scripts/check-file-resolve-miss.py` MISS audit (fails on any MISS key absent from `scripts/file-resolve-miss-baseline.txt`).

## META-FIX recurrence-proofing gates (batch 2, 2026-07-16)

- `MC2_FIXTURE_ORDER_LIVENESS=1` — design F behavioral fixture (`code/mission.cpp` soak namespace; requires `MC2_SOAK_AUTOWIN`), default **OFF** = byte-identical (static-getenv latch). Phases (fractions of `MC2_SOAK_WIN_AFTER_SEC`): 0.2 — COMMANDER-origin unaimed `ATTACK_TO_DESTROY` per armed home mech at the nearest live enemy (PLAYER origin never installs the tac order — the trap that burned the first aimed-fire fixture run); 0.5 — COMMANDER-origin `orderMoveToPoint` per home mech, 300 m toward map center (replaces the attack order → clean 0.4·win displacement window; attack-first is deliberate — the reverse order self-sabotages the move bound when targets spawn in range: measured 1/11 stalled standing-and-firing on stock mc2_24). `[ORDER_FIXTURE] kind=attack|move wid=… tgt=… t=…` per issued order; 0.9 — one self-asserting `ORDER_LIVENESS_SUMMARY issued_move=N moved=N refused=N stalled=N verdict=PASS|FAIL` line (moved = net displacement ≥ 25 m from move-issue; refused = classified `LastMoveCalcErr`; dead/disabled exempt). NOTE for smoke-tier runs: set `MC2_SOAK_WIN_AFTER_SEC` slightly ABOVE `--duration` (e.g. 31 vs 30) — an autowin that ends the mission before the smoke window closes reports `reason=early_exit` and fails the run for an unrelated reason. When this var is set in the **runner** env, `run_smoke.py` hard-fails a missing summary or `verdict=FAIL` (VEG-SMOKE-FLOOR activation pattern — non-fixture runs untouched; absence-is-failure catches the fixture dying silently). Consumed by `scripts/check-order-liveness.py` (nightly, needs a deployed exe). Static sibling: `scripts/check-order-gate-coverage.py`; triage: `docs/ai-order-triage.md`.
- `MC2_RES_ASSERT` — design D runtime tier (`RenderCore/ResolutionContract.h`), default **OFF** (unset/`0`) = one static bool test, tier1 byte-identical. `=1`/`log`: one `[RES_ASSERT v1] event=violation site=…` stdout line per violation signature. `=fatal`: log then `abort()` (nonzero exit = red run — the machine oracle for the resolution smoke matrix). Chokepoints: `camera_aspect` (`mclib/camera.cpp` `Camera::setOrthogonal` — projection aspect must equal drawable aspect unless `MC2_CAMERA_ASPECT_NATIVE=0` or inside the mech-preview FBO; issue #45 class), `hud_clamp`/`drawable_stale`/`drawable_nonpositive`/`windowed_size` (`gameosmain.cpp` `draw_screen` per-frame: clamp ⇒ 800x600 logical canvas, cached drawable matches a fresh probe with 3-frame grace, `MC2_WINDOWED`+`MC2_WIDTH`/`MC2_HEIGHT` boots keep the requested drawable — the #49 oracle; desktop-smaller downgrades to `event=note`), `hud_canvas_bounds`/`hud_roundtrip` (`gameos_graphics.cpp` `flushHUDBatch` — canvas box ⊆ drawable; forward remap (hud-scale shrink∘canvas) composed with `gos_HudInverseMousePoint` is identity within 0.5 px). Static sibling: `scripts/check-res-aspect-contract.py`; smoke sibling: `tests/smoke/matrices/resolution.json` (nightly). Passthrough-only companions added to the run_smoke allowlist: `MC2_WIDTH`, `MC2_HEIGHT`, `MC2_WINDOWED`, `MC2_FORCE_43`, `MC2_CAMERA_ASPECT_NATIVE` (all pre-registered/allowlisted in check-env-registry).

## META-FIX recurrence-proofing gates (batch 3, 2026-07-16)

- `MC2_ASSET_TRACE=1` — design B PART 0 companion (`mclib/msl.cpp` loader chokepoint), default **OFF**. Adds `[ASSET_LOAD v1] event=file_missing loader=… reason=… path=…` lines for MISSING files at the three shape-load entry points. Default OFF because missing OPTIONAL files (arms/shadows/damage shapes/anims) are NORMAL stock degradation — always-on `file_missing` would red-bucket healthy runs. `parse_fail` (file present, content bad: bad ASE header/material block, unknown `.tgl` nodeType, bad `.tgl` version, importer reject) and `required_missing` (required base slots via `mclib/tgl_load_guard`) log ALWAYS regardless of this gate; only those (+`appr_type_fail`) count toward the `asset_load_fail` red bucket. Rate-limited per unique (event,path), cap 64.
- `MC2_ASSET_FAILLOUD` — design B PART 1 (`mclib/tgl_load_guard.cpp`), **default ON** killswitch (`MC2_TERRAIN_TESS` shape: unset=ON, `=0` restores the exact legacy fail-soft). On a REQUIRED base-shape load failure (rc!=0 or `GetNumShapes()==0` at the ~17 required slots in `mech3d`/`bdactor`/`gvactor`/`genactor`, incl. the `[Import]` branches and `loadBuildingImportSourceWithSidecarAxis`), populates a VISIBLE magenta placeholder box carrying a REAL in-memory 4x4 texture through `mcTextureManager` (valid handles — the GPU batchers cull `layerForPacket=-1` packets, so `numTextures=0` would be invisible on the modern path). The always-on `[ASSET_LOAD v1] event=required_missing` line, `MC2_VERIFY` fire, and `AppearanceType::baseShapeLoadFailed` flag are independent of this gate. Stock byte-identical (branch is cold — no stock base shape fails). Mech instance-repaint clobber guarded via `baseShapeLoadFailed` early-return in `resetPaintScheme`; non-mech legacy-path instance rebinds may degrade the box to untextured-but-visible (stock NULLTXM case, crash-free) — documented residual.
- `MC2_ICON_FAILLOUD=1` — design B PART 3 (`code/mechicon.cpp` `iconTileChecked` at the three CPU icon-tile blits), default **OFF**. `=1` substitutes a solid MAGENTA tile when an icon index resolves outside the source atlas (last-pixel arithmetic identical to the per-pixel bounds guards). Default keeps the legacy wrap/blank semantics — load-bearing for MC2X high-index mod content — while the `[ASSET_LOAD v1] event=icon_oob` line stays always-on (rate-limited). UV-path sibling: `icon_atlas_cell.h` `cellForIndexChecked` (logisticsmechicon/mechlistbox) returns the legacy-computed cell UNCHANGED on OOB. Row bound harness-validated (retail 256x256 / MC2X 512 / MCO 1024/40, `tools/icon_atlas_harness`) before the `icon_oob` smoke bucket arms red.
- `MC2_APPEAR_AUDIT=1` — design B PART 4 (`code/mission.cpp` `Mission::start` beside `mission_phase_mark("mission_ready")`), default **OFF**, OBSERVATIONAL-FIRST. One O(N) pass at mission_ready: every existing registered object either has a non-NULL appearance whose base shape loaded, or emits `[APPEAR_AUDIT v1] event=missing reason=appearance_null|base_shape_failed handle= class= part=` (bounded 32 lines) + `summary scanned= missing=`. POSITION-FREE (off-map-staging carve-out, INVIS-VEHICLES-FIX-1: staged movers have valid appearances; no positional/visibility term — pinned by `check-tgl-load-callers.sh`). The `appear_audit` smoke bucket ships ADVISORY-DISARMED; arming it red (and force-setting this gate for tier runs) requires a full 24-map `failed=0` baseline — batch 3 ships the tier1-missions baseline, the 24-map run is a documented follow-up.
- `MC2_MOVER_COHERENCE_AUDIT=1` — design A runtime tier (`code/mission.cpp` `auditMoverCoherence`, ~1 Hz), default **OFF** = one static bool test, byte-identical. Read-only audit that the five per-mover registries (isOnGUI render gate, force-bar roster, selection, tacmap blip, commandability) agree for **home-commander SP** movers: I1 live onGUI mover holds a force-bar icon (I1b bar-full = ADVISORY — `ForceGroupBar::addMech` silently drops the 17th mover in release); I2 off-GUI mover has no icon and no PLAYER-origin tac order (SELF/COMMANDER origin = ADVISORY I2b: auto-powerup and brain orders are legitimate; the #50 invisible-but-commandable signature violates this row); I3 disabled-not-destroyed keeps `getAppearance()!=NULL` (#42b-ADJACENT teardown class — the HISTORICAL #42b guard-shape bug is static rule S4's job); I4 every icon unit is ObjectManager-registered (pointer-identity scan — deliberately not `getWatchID`, which would ASSIGN an id inside a read-only audit); I5 selected ⇒ onGUI. Emits deduped `[MOVER_COHERENCE] VIOLATION|ADVISORY rule=… part=… name=… detail=…` stdout lines (mirrored to the diag sink) + a process-exit `summary checked= violations= advisories= audits=` line. **POSITION-FREE by contract**: off-map staged movers (onGUI=1 at an authored staging position waiting on a mission flag — mc2_10's convoy, INVIS-VEHICLES-FIX-1 census evidence) are a LEGITIMATE mover state; the audit keys on no positional term and static rule S6 (`scripts/check-mover-registry-coherence.sh`) pins the audit region position-free. `run_smoke.py` force-sets `=1` for `--tier` runs (read-only; shipping default stays OFF) and red-flags any VIOLATION via the `mover_coherence` marker bucket (`allow_mover_coherence` manifest override); ADVISORY lines surface as details only. Nightly add-ons for rows tier1 cannot reach: mc2_08 (LINKUP-PENDING, #50) + `MC2_SOAK_SALVAGE_TRADEMOVER` (trade row). Static sibling: `scripts/check-mover-registry-coherence.sh` (S1 write allowlists, S2 pairing windows, S3 read-gate pins, S4 #42b guard shape — SOLE defense, S5 link-up gate shape, S6 staging carve-out).

## RenderWorld arc

- `MC2_OBJECT_ID_BUFFER=1` — R32_UINT MRT attachment-2 (M1.5)
- `MC2_RENDER_WORLD_TRACE=1` — verbose RenderWorld events
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
- `MC2_BRAIN_DISPATCH=1` — TechScript special-dispatch master gate (`code/mission.cpp`, `code/brain_special_dispatch.cpp`). Default **OFF**; requires `MC2_BRAIN_RUNTIME=1` + `MC2_BRAIN_RUNTIME_APPLY=1`.
- `MC2_DYNAMIC_DECALS` — dynamic decal ring (`mclib/dynamic_decal_ring.cpp`); `spawn()`/`gatherToDecalBatch()` are no-ops when off. Default **OFF**; any non-`0` value enables.
- `MC2_MOVE_RECON` — pathfinding cost instrumentation (`mclib/move_recon.cpp`). Default **OFF**, zero behavior change when unset. `MC2_MOVE_CHUNK_SHADOW` / `MC2_MOVE_PATH_CACHE_SHADOW` are alternate enables for the same instrumentation.
- `MC2_UNIT_PROFILE_DATA` — UNIT-PROFILE-ARC data-driven unit+equipment definition source (`code/unitprofile.cpp`). Default **LEGACY** (unset/`0` = legacy path); any non-`0` value switches to the generated `UnitProfileData` source.
- `MC2_ACTIVE_MOD=<folder>` — active mod overlay id (`mclib/file.cpp`, `mclib/fx_def_registry.cpp`, `mclib/anim_override_registry.cpp`, `mclib/model_override_registry.cpp`). Unset/empty = stock base-game mode. When set, mounts `<folder>`'s content (and any `MC2_MOD_DEPS` add-on layers) over the stock campaign for mod-cache/asset-override resolution.
- `MC2_FX_DEFS=1` — FX-DEFS-SIDECAR-1: overlay `data/effects/defs/<Name>.fxdef.json` (+ active mod's own defs) onto the loaded effect `SpecLibrary` (`code/mechcmd2.cpp`). Default **OFF**; parsing/diagnostics are always free, but the spec-mutating apply only runs when set. OFF = byte-identical to stock.
- `MC2_EQUIPMENT_ADD` — EQUIP-ADD-FILL-1 (ADD-ladder S2): mod equipment defs with an explicit `LegacyMasterId` on an UNDEFINED compbas ordinal materialize as live MasterComponent + mechlab rows at load (`code/equipment_registry_engine.cpp`). Default **OFF** (unset/`0`); requires `MC2_UNIT_PROFILE_DATA=1` and an active mod (`MC2_ACTIVE_MOD`/`MC2_MOD_DEPS`). Gate-OFF is zero-I/O byte-identical.

## EditorBridge

- `MC2_EDITOR_MODE` — activates `EditorBridge` API. Default `0`. Combine with `MC2_OBJECT_ID_BUFFER=1` for GPU pick.

## Render-contract assert

- `MC2_RENDER_CONTRACT_ASSERT=1` — runtime GL draw-buffer/depth state vs declared expectation. Phase 2 (`137dc70`).

## Pre-commit invariant scripts

- Order liveness (nightly, needs a deployed exe): `py -3 scripts/check-order-liveness.py --exe <lane>/mc2.exe` — joins `check-combat-soak.py` in the nightly tier; triage playbook `docs/ai-order-triage.md`.
- Resolution matrix (nightly): `py -3 scripts/run_smoke_matrix.py resolution` — 5 aspect/windowed entries, `MC2_RES_ASSERT=fatal` machine oracle.
- Object lifecycle: `sh scripts/check-destroy-invariant.sh`
- UI icon atlas: `sh scripts/check-asset-scale-callers.sh`
- Shader ABI: `cmake --build build64 --target shader_schema`
- RenderWorld firewall: `sh scripts/check-include-firewall.sh`
- No raw GL from game: `sh scripts/check-no-raw-gl-from-game.sh`
- VFX no objectId: `sh scripts/check-vfx-no-objectid.sh`

## Material / static prop gates
- `MC2_SKIP_STATIC_TREES` — pure static-natural update skip (R2b, ~4977→~145 terrain-object updates on dense maps). **Default ON** (opt-out: only `=0` disables; gameplay-critical gates/turrets/special-buildings are excluded from the skip set and still tick every frame).
- `MC2_STATIC_PROXY_RECON` — StaticSceneProxy-arc recon instrumentation (`mclib/bdactor.cpp`); counts `touchSerialCommit` calls for building/tree appearances ahead of the per-frame static-prop-walk meta-fix. Default **OFF**, presence-gated.

- `MC2_MATERIAL_KTX=1` — KTX2 sidecar loader for static-prop tex array. Default **OFF**.
- `MC2_MATERIAL_GPU` — MaterialGpu table upload + SSBO bind. Default **ON**.
- `MC2_MATERIAL_GPU_SAMPLE` — route albedo through MaterialGpu in frag. Default **ON**.
- `MC2_STATIC_PROP_AMBIENT_V1=1` — hemisphere ambient fill in `static_prop.vert`. Default **OFF**.
- `MC2_STATIC_PROP_IBL_SH` — SH-L2 IBL ambient. Default **ON**. Kill=`=0`.
- `MC2_STATIC_PROP_IBL_SH_STRENGTH=N` — SH-IBL strength override (0..3). ⚠ Setting this **disables** the per-mission `data/visual_tuning.json` `staticPropIblStrength` override (defaults 0.55; mc2_01/mc2_10/mc2_24 use 0.6), so the env is *not* interchangeable with leaving it unset. The bare code fallback is 0.5.
- `MC2_STATIC_PROP_IBL_NIGHT_SCALE=F` — **IBL-NIGHT-SCALE-1. DEFAULT-ON, floor `0.25`.** Night-scales the SH-IBL probe: `strength *= 1 - (1 - F) * nightFactor`. The probe is projected from a **day sky** (`DaySkyHDRI063B_4K.exr`), there is one SH set for every mission and every time of day, and nothing scaled it by night — so since the 2026-05-27 default-ON flip a day sky lit every night mission at full strength (mc2_10 night: static props +44%..+83%, one building face hard-clipping to 255 on 2.49% of its pixels; stock MC2 had no IBL at all). `F` is the night floor: `1.0` → multiplier is exactly 1.0 at every `nightFactor` → **byte-identical to the pre-slice build** (this is the killswitch); `0.0` → hard `(1 - nightFactor)`, which is equivalent to the blunt `MC2_STATIC_PROP_IBL_SH=0` and drives walls/turrets 29–46% darker (near-black). Day is byte-identical **by arithmetic**, not by measurement: `nightFactor` is exactly `0.0f` in daylight, so the multiplier is exactly `1.0f`.
- `MC2_STATIC_PROP_DEBUG_MATERIAL=N` — frag debug view (0=Final,1=Albedo,2=MatIdx,3=Normal,4=TexLayer,5=Rough,6=Metal).
- `MC2_STATIC_PROP_PBR_V1=1` — per-frag Schlick-Fresnel specular. Default **OFF**.
- `MC2_FAR_BACKDROP_LIT=1` — night-shade substitution for far/impostor static-prop LODs in `static_prop.vert`: fades the stale day/black per-vertex `base_light` toward the mission night object-light ambient across the LOD impostor distance (fixes fullbright impostors + black far geometry at night). Default **OFF** (`u_farBackdropLit=0` -> shader branch skipped -> byte-identical). Tunables (only meaningful when `=1`): `MC2_FAR_BACKDROP_LIT_NEAR` (clip.w where fade begins, default 700), `MC2_FAR_BACKDROP_LIT_FAR` (fade completes, default 960), `MC2_FAR_BACKDROP_LIT_STRENGTH` (max substitution, default 1.0; >1 exaggerates), `MC2_FAR_BACKDROP_LIT_TINT` (`r,g,b` debug/exaggeration additive tint, e.g. `1,0,0`).

- `MC2_LIGHT_CONE_NODES=0` — **LIGHT-CONE-MODELS-1. DEFAULT-ON** (`mclib/cone_glow.cpp:118`, `return !(v && v[0]=='0')`). Places building/guard-tower spotlight ground pools using the fixture's authored cone **node** geometry (apex + aim + length read off the shape) rather than the older heuristic that centred each pool on the light node's own origin. Unset or any value other than `0` (**default**) = node-derived pool centres; `0` = restores the pre-LIGHT-CONE-MODELS-1 heuristic/origin-based centres (batch-6 behaviour). Single reader: `cone_glow.cpp` `nodes_enabled()`; consumed at `mclib/bdactor.cpp:3398`. **Documented retroactively by NIGHT-TAGS-GREEN-1** — it shipped default-ON but unregistered and undocumented, which is precisely how `CONE-SPILL-COUPLING-1` (`c1f98b45`) later rode it into the default render ungated (see `MC2_CONE_SPILL_COUPLING` below). Resolved once at process start.
- `MC2_CONE_SPILL_COUPLING=1` — **CONE-SPILL-GRADE-1.** Re-enables CONE-SPILL-COUPLING-1 (`c1f98b45`), which originally shipped **ungated** as a default-behaviour change riding the default-ON `MC2_LIGHT_CONE_NODES`, and is now **default-OFF on measured evidence**. `1` = building/guard-tower spotlight ground pools are placed at the true beam/terrain intersection found by marching the ray (`mc2ConeGroundHit`, `mclib/bdactor.cpp`), the `200.0f` spill clamp is removed, and a beam that never meets ground within its own length (or leaves the map) emits **no** pool. Unset/`0` (**default**) = the pre-`c1f98b45` placement (`apex + aim*spill`, `200.0f` clamp, `z` snapped to `getTerrainElevation`, no miss rejection). **Why default-OFF:** `MC2_LIGHT_CONE_DIAG=1` over all 9 distinct building spotlight fixtures on `mc2_10` measured `hit=0` (pool **suppressed**) for **7 of 9**, `hit=1` for 2 with `horizErr` only **7.0 world units**, and `clamped=1` for **0** — the stock fixtures are authored near-horizontal (`aim.z=-0.089`, `coneLen=167.8` → 14.9 units of drop) from an apex ~17 units above terrain, and 3 point straight up (beacons), so requiring a real intersection within the authored length deletes almost every pool. The authored cone length is a visual extent, not a physical reach. Net effect at default-ON was 7 of 9 night ground pools deleted for an imperceptible reposition of the other 2 — a regression against the complaint it was written for. Pairs with `MC2_LIGHT_CONE_DIAG=1`, which prints the OLD and NEW pool positions plus `horizErr` in the same frame.
- `MC2_STATIC_PROP_FIXTURE_GLOW=1` — **FIXTURE-GLOW-1. DEFAULT-OFF.** Gives the runway/taxiway edge lights and beacons a **bounded, distance-attenuated additive glow** so they read as *light sources* at play distance, which is the user-reported defect (*"the actual lights are not on at ALL"*) that four previous lanes did not close.

  **Why a new mechanism was required.** `MC2_STATIC_PROP_FIXTURE_LIGHTS` scopes the fixtures correctly but *cannot* brighten them, for two independent reasons, both measured: (1) `get_base_light()` **clamps internally at 1.0**, so that gate at scales `1`, `4` and `8` renders **byte-identical frames** — there is no headroom left; and (2) whatever survives the clamp is then **multiplied by the albedo** at `shaders/static_prop.frag` `vec4 c = tex_color * vec4(litRgb, ...)`, and a fixture's lens texel is dark. **A light source is not expressible as a multiplier on a dark texture.** This gate therefore adds a *separate additive term* in the **fragment** shader, **after the albedo modulate and after fog** — which is precisely how it escapes both the `1.0` clamp and the multiply.

  **Scoping and colour** come for free from `BEACON-FIXTURE-ID-1`: the per-instance fixture flag already rides the per-type SSBO padding lane (`hotPinkRGB.w`), so there is no struct growth, no new binding and no lockstep risk. The glow **tint is the fixture's own night emission delta**, which is exactly `vec3(0)` on the fixture's non-emissive post/base geometry — so the glow lands on the **lens only**, and white `runwaylight` / red / green / blue beacons each get their correct colour with no extra plumbing.

  **★ Why this cannot reproduce `RUNWAY-MARKER-LIGHTS-1`.** That regression (*"atrociously hideous"* overview-altitude dashes, the same failure that defaulted `MC2_LIGHT_BILLBOARD_FACING` OFF) came from an **unbounded minimum-screen-size clamp**, `k = max(1.0, kLbMinNdc/sil)`, which pinned every marker to a constant screen size. This term does the opposite at both ends: it contains **no screen-size clamp, no billboard, and no geometry or size term at all** — the fixture keeps shrinking with distance exactly as it does today — and its magnitude is multiplied by `1 - smoothstep(2600, 4600, clip.w)`, which is **arithmetically zero at and beyond view depth 4600**. Overview altitude is ~6000, i.e. **1400 units past identically zero**. Overview safety is a property of the formula, not of a capture. The near/far window is **compile-time and deliberately not env-tunable**, so the bound cannot be configured away. The term is also multiplied by the night factor, so it is **identically 0.0 in daylight**.

  **Default is OFF, on measured evidence.** The mechanism is verified and bounded, but at strength `1.0` the visible result on the only fixtures that could be observed on screen — `mc2_10`'s green beacons — is a modest *pale-ing* of an already-green cone, not "a light came on". The fixture lens already renders at channel max, so the only headroom left in an LDR target is the **other** channels: any added glow necessarily desaturates toward white. That is physically what a hot light core does, but it is an aesthetic call, and this user already rejected one fixture-lighting attempt as *"atrociously hideous"*. The red and blue fixture types were **never observed glowing on screen**. Shipping ON would repeat the predecessor's outcome — a safe default-ON change that does not close the complaint.

  Unset/`0` (**default**) = **byte-identical** to the pre-`FIXTURE-GLOW-1` render; numeric = strength, clamped `[0,8]`. `1` is the calibrated baseline; `3`–`4` is the "clearly a light" range. Per-map fixture inventory from the `[GPUPROPS] fixture-light type:` receipt: `mc2_10` = 2 blue + 2 red + 1 green, `mc2_01` = 2 red, `mc2_03`/`mc2_24` = **none** (gate is a mathematical no-op there). Note the stock data contains **no white fixture** — `runwaylight.ini` declares `0x00ff0000`, i.e. **red**, identical to `redbeaconlight`.

- `MC2_STATIC_PROP_FIXTURE_LIGHTS=0` — **BEACON-FIXTURE-ID-1. DEFAULT-ON.** Turns the runway/taxiway edge lights and beacons ON at night, scoped by **fixture identity**, not vertex colour. Vertex colour cannot scope this: **738 of 2947** stock `.ase` carry the Hot Green tag and only **8** are fixtures — the tag's stock name is literally *"Building Base Lights"* (`shaders/include/lighting.hglsl:118`), which the fixtures merely borrow. That is why `MC2_STATIC_PROP_NIGHT_TAGS` (which relights ~730 buildings to buy 8 fixtures) stays default-OFF.

  **Discriminator** (`isEmissiveFixtureType()`, `gos_static_prop_batcher.cpp`): a light fixture declares the *same saturated primary* for all three `Hot*RGB` slots; buildings declare *independent desaturated* greys/olives (`0x00707070`, `0x00557238`, `0x006AA6A6`). Validated against **all 773** stock `data/tgl/*.ini` → exactly **4** match (`runwaylight`, `red`/`green`/`bluebeaconlight`, backing the ~24 `.ase` base/x/dam/damx/l1 variants), **zero buildings**. Both halves of the test are load-bearing: *saturated primary* alone leaks **503/773**, because `mclib/bdactor.cpp:813-819` defaults an undeclared slot to the magic tag itself (`HotGreenRGB` → `0xff00ff00`); requiring all three **equal** rejects every one of those, since the three defaults differ from each other.

  Carried as `1.0`/`0.0` in the existing per-type SSBO `vec4` padding lane (`hotPinkRGB.w`) — no struct growth, no new binding, no lockstep risk. Unset/`1` (**default**) = fixtures emit at stock; `0` = byte-identical to the pre-fix render; numeric `>1` = verify-by-exaggeration, clamped `[0,16]`. Non-fixture props take scale `0.0` → `base_light == base_day` → **building windows are provably untouched**. Introduces **no** screen-size clamp and **no** billboard: the emission rides real fixture geometry and shrinks/fades with distance, which is what avoids the `RUNWAY-MARKER-LIGHTS-1` overview-altitude streak regression (the same failure that defaulted `MC2_LIGHT_BILLBOARD_FACING` OFF). Numeric receipt at load: `[GPUPROPS] fixture-lights: N of M types classified`.
- `MC2_STATIC_PROP_NIGHT_TAGS=1` — **EDGE-LIGHT-EMISSION-1.** Restores the stock hot-colour NIGHT vertex tags on the GPU static-prop path. Runway/taxiway edge lights and beacons (`data/tgl/{runway,red,green,blue}beaconlight.ase`) paint their post base pure green = `0xff00ff00` = *"Hot Green — Building Base Lights — ONLY at NIGHT"*, and each fixture `.ini` declares its emission colour in a `//Night Lights` block (runway + red beacon `0x00ff0000`, green `0x0000ff00`, blue `0x000000ff`). The legacy CPU path honours these via `eye->getIsNight()`/`getNightFactor()` (`mclib/tgl.cpp:1901`); this path had them hardcoded `false, 0.0`, so every night-only branch took its daytime arm and **the lights never came on**. `0`/unset (**default**) uploads `u_isNight=0`, `u_nightFactor=0`, `u_nightEmissiveScale=0` → byte-identical to the pre-fix render. `1` = stock emission restored. `2` = verify-by-exaggeration. Tunable: `MC2_STATIC_PROP_NIGHT_TAGS_SCALE` (scales the night **emission delta** only — `0.0` = pre-fix, `1.0` = exact stock, default `1.0`, or `4.0` under mode `2`). **Blast radius:** not scoped to runway lights — every static prop carrying a night tag relights, including building windows and building base lights. Equally stock-correct, but broad, hence default-OFF pending click-through.
- `MC2_NIGHT_FOG_DIM=1` — night-dim the horizon "cloud bank" fog color. BACKDROP-BLIT-ATTRIB-1 RenderDoc pixel-history on the pre-composite HDR scene FBO (231) pinned the mc2_10 night "white horizon band" to the EDGE-FOG pass (`edge_fog.frag`, eid 7158) painting a hardcoded bright-grey `u_fogColor` (`{0.80,0.81,0.83}`) never dimmed for night (the HDRI sky it sits against measures ~0.27 in the same FBO). When `=1`, `runEdgeFog`/executor scale `edgeFogColor_` and the sibling OOB fog color by `MC2_NIGHT_FOG_DIM_SCALE` (default **0.35**, tuned to the measured night-sky value; `0.0`=black fog exaggeration lower-bound) before upload. Default **OFF** -> unscaled constant uploaded -> byte-identical. Cinematic letterbox bars are a different path and untouched.
- `MC2_LEGACY_LIT_MC2_FRAME=1` — VEHICLE-NIGHT-LIT-1: feed `calc_light` MC2-frame position + normal in `gos_tex_vertex_lighted.vert` (the legacy `mcTextureManager`/`ShapeRenderer` lit program that draws **ground vehicles**). `GatherLightsParameters` writes MC2-frame `light_to_world`/`light_dir`, and the other two `calc_light` consumers already feed MC2-frame inputs (`mech.vert` -> `worldMC2`; `static_prop.vert` -> MC2 `worldPos` + `STATIC-PROP-NORMAL-FRAME-FIX` normal swap). This shader was the last one still passing Stuff-frame `WorldPos`/`Normal`, so `length(vertex_world_pos - light_pos)` mixed frames -> `GetFalloff` rejected every POINT/SPOT/INFINITEWITHFALLOFF light and the INFINITE sun `NdotL` was ~90° off. Vehicles kept only `base_light` + AMBIENT and rendered near-black at night while mechs/buildings/fences/terrain in the **same** light pool were lit — including trucks parked inside a guard-tower cone. Default **OFF** (`u_legacyLitMc2Frame=0` -> legacy feed -> byte-identical). Blast radius: the program is shared by other legacy lit shapes, so `=1` changes their lighting too (day included) — hence the gate.
- `MC2_GV_LIGHTSOUT_CLEAR=1` — VEHICLE-NIGHT-LIT-2: restore the missing `else` branch in `GVAppearance::update` (`mclib/gvactor.cpp`) that clears `gvShape->SetLightsOut(false)` when a ground vehicle is no longer `OBJECT_STATUS_DESTROYED`/`OBJECT_STATUS_DISABLED`. Legacy code sets the flag but never clears it, so it is a **one-way latch** — unlike the mech path (`mech3d.cpp Mech3DAppearance::update`, which sets both arms). Once latched, that vehicle's shape can never be lit again, including after the actor slot is recycled. Default **OFF** -> legacy latch preserved -> byte-identical. Latent-correctness fix; **not** the cause of the reported all-vehicles-dark-at-night defect (the reported trucks are intact and never disabled).
- `MC2_STATIC_LIGHT_UPLOAD_SPLIT` — upload immutable static light prefix `[0..S)` once/dirty-only + dynamic suffix per frame (vs whole LightsData SSBO every frame). Default **ON**. Kill=`=0` (legacy whole-buffer, bit-identical). Requires `MC2_LIGHTBAKE`.
- `MC2_LIGHTBRIDGE_STABLE_SKIP=1` — skip per-frame `EmitBakedGpuLightData` + `getCachedGpuLightIndex` + `staticReg` pointer-chain for stable baked static props (LIGHTBRIDGE-STABLE-SKIP-WIRE-1). Default **OFF**. Requires `MC2_LIGHTBAKE=1` (implicit when bake map populated). Gate: `stableLightSkipArmed` + `stableLightSkipEligible` + valid `lightDataIndex` + generation match. Expected: `serial_commit_us` ~593µs → ~50–80µs on stable frames. Diagnostics: `MC2_LIGHTBRIDGE_STABLE_SKIP_DIAG=1` (prints `LIGHTBRIDGE_STABLE_SKIP:` every 300 frames; independent of skip gate).
- `MC2_STATIC_PROP_FLUSH_CACHED_BLOB=1` — STATICPROP-REGISTRY-FLUSH-CACHED-BLOB-2A: replace per-leaf registry-flush rebuild with cached immutable instance/actor-record blobs bulk-appended into the existing rings. Keeps per-frame range walk + staleness skip. Default **OFF** pending Tracy proof.
- `MC2_STATIC_PROP_FLUSH_CACHED_BLOB_COMPARE=1` — diagnostic compare: with the cached path active, also build the legacy temp instance+record per leaf and compare hash/count; logs mismatches. Default **OFF**; requires `MC2_STATIC_PROP_FLUSH_CACHED_BLOB=1`.
- `MC2_LIGHTBAKE_STABILITY=1` — trace: per-instance `lightDataIndex` permanence/stability proof (`[LIGHTBAKE-PROOF v1] event=first/UNSTABLE`). Diagnostic, capped 32. Default **OFF**.
- `MC2_LIGHTBAKE_PARITY=1` — trace: baked permanent record == gathered transient record byte/hash (`[LIGHTBAKE-PROOF v1] event=parity match=1`). Diagnostic, capped 32. Default **OFF**.
- `MC2_TERRAIN_LIGHT_POOLS=0` — killswitch, default **ON** (NIGHT-LIGHT-POOLS-1). Master gate for the per-fragment terrain light-pool loop on the LOD-chunk terrain path (`terrain_lod_chunk.frag`, SSBO binding 28). `=0` uploads `u_numPoolLights=0` so the loop is never entered → byte-identical. Also inert when `MC2_TERRAIN_LIGHTING_GPU=0` (no producer). **Allowlisted for smoke passthrough as of PERF-HARNESS-TRUTH-1** — before that a harness could not drive it, so the OFF arm of every light-pool A/B silently ran the ON path.
- `MC2_LIGHT_POOL_MAX=N` — tune, default **256** (LIGHT-POOL-CAP-1). Caps how many lights the producer packs into the pool. Was 64 before `5213d257`. Because the shader loops per fragment, this is a **fragment-bound** cost: raising it is measurable only against a visible, unthrottled window (see `docs/tracy-harness.md`).
- `MC2_LIGHT_GRID=0` — killswitch, default **ON** (LIGHT-CLUSTER-PERF-1, `93c22e39`). World-space XY light grid that replaces the linear per-fragment pool loop with a per-cell index list, cutting worst-case iterations at the busy mc2_10 night pose from ~200 to ≤20. `=0` uploads `u_lightGridDim=0` and the shader takes the original linear loop. `MC2_LIGHT_GRID_DIM=N` tunes cells/axis (default 64). Inert below 8 packed lights and whenever `MC2_TERRAIN_LIGHT_POOLS=0`. Exact, not approximate — each cell list is a conservative superset of the true contributors.
- `MC2_LIGHT_REALISM=1` — night lighting realism reshape, default **OFF** (LIGHT-REALISM-1). Widens each pool light's ground falloff (`MC2_LIGHT_REALISM_POOL_SPILL`, default `2.2`, user-approved variant B) with a soft core (`MC2_LIGHT_REALISM_POOL_CORE`, default `0.0`) and softens the spotlight cone billboard (`MC2_LIGHT_REALISM_CONE_OPACITY`, default `0.40`, user-approved variant B). Covers cone transparency, beacon ground discs and pool continuity. OFF = every path skipped = byte-identical. **Cost (PERF-HARNESS-TRUTH-1, mc2_10 busy night pose, 3840×2160):** with `MC2_LIGHT_GRID=1` (the default) it is **free** — p50 `6.405 ms` ON vs `6.454 ms` OFF, within run-to-run noise — because the grid holds the per-fragment loop to `avg_cell 1.70` even though the reshape **triples** grid stamping (`1962 → 6945`, `peak_cell 17 → 33`). With `MC2_LIGHT_GRID=0` the same gate costs **+31%** (`8.732 ms`). The grid is what makes it affordable; do not enable this gate with the grid killswitched.
- `MC2_LIGHTING_DEBUG_VIEW=<mode>` — diagnostic render replacement, default **OFF** (DEBUG-VIEW-SWEEP-1). Modes: `off|albedo|normal|sun|ambient|shadow|final|overbright|lightcount|lightindex`; an unknown value safely falls back to the normal render. Applies across terrain, static props and mechs. Resolved **once per process** (no setenv path); the `debugview N` command channel overrides it at runtime via `gos_SetGlobalDebugView()`. **Registered + documented + allowlisted by PERF-HARNESS-TRUTH-1** — it previously silently no-opped through `run_smoke.py`, so a lighting investigation could set it, see an ordinary lit frame, and wrongly conclude the debug channel showed nothing.
- `MC2_STATIC_PROP_FLUSH_CACHED_BLOB=1` — cached-blob registry flush (bulk submit + per-recipe cached cull records). Default **OFF** (pending soak/default-flip). `_COMPARE=1` adds the FNV cached-vs-legacy oracle.
- `MC2_STATIC_PROP_COLORS_FILL=1` — restore the per-static-instance Colors SSBO zero-fill. Default **OFF = skip** (no production shader reads colors; ~80ns/leaf saved). Kill-switch only.
- `MC2_STATIC_PROP_PERSISTENT_BUCKETS` — (2b Stage 2) persistent static instance store; skip per-frame static re-push when registry generation clean. Default **ON** (Tracy: flush 312µs→68µs). Kill=`=0`. `_COMPARE=1` adds the FNV store-consistency oracle. `MC2_BUCKET_ORDER_TRACE=1` = Task-0 static/dynamic bucket probe.

## Mech PBR lighting (StandardLit GGX path)

- `MC2_STANDARD_LIT_V1` — Cook-Torrance GGX PBR on mechs. Default **ON**. Kill=`=0`.
- `MC2_MECH_SURFACE_MATERIAL` — surface material profile for mechs. Default **unset** = passthrough (no surface material). Explicit values:
  - `=metal061b` — Metal061B normal+ORM as substrate + PaintedMetal003 paint layer (debug/exposed-metal look; strong PBR)
  - `=painted_subtle` — ORM-only (flat normal, no perturbation) using Metal061B roughness/metalness. Soft defaults: metallic=0.03, roughness 0.65–0.90, wear=0.0, tile_scale=2.0. For "mech catches light better" not "mech looks like raw metal".
- `MC2_MECH_IBL_SH` — SH-L2 HDRI ambient on mechs (shared coefficients with static props). Default **ON**. Kill=`=0`.
- `MC2_MECH_BACK_FILL` — cool-sky fill light for shadow hemisphere (`max(-NdotL,0) * strength`). Default **2.0**. Set `=0` to disable.
- `MC2_PBR_METALLIC_INFLUENCE` — metallic weight multiplier (profile-aware default: see MC2_MECH_SURFACE_MATERIAL).
- `MC2_PBR_ROUGHNESS_MIN` — minimum roughness clamp (profile-aware default: see MC2_MECH_SURFACE_MATERIAL).
- `MC2_PBR_ROUGHNESS_MAX` — maximum roughness clamp (profile-aware default: see MC2_MECH_SURFACE_MATERIAL).
- `MC2_PBR_WEAR_STRENGTH` — wear factor modulation (profile-aware default: see MC2_MECH_SURFACE_MATERIAL).
- `MC2_PBR_TILE_SCALE` — UV tile scale for PBR detail textures (profile-aware default: see MC2_MECH_SURFACE_MATERIAL). ENV VAR overrides always win.
- `MC2_PBR_TRIPLANAR` — world-space triplanar sampling (no UV seams). Default **OFF**. Set `=1` to enable.
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

## Road PBR (ROAD-PBR-ABANDON-1 — RETIRED)

- `MC2_ROAD_PBR` — **[RETIRED 2026-07-18]** setting this var has no effect.
  USER RULING 2026-07-18: the PBR/normal-layer road path is abandoned —
  the stock legacy 64Overlays road tiles are unconditional
  (`gos_TerrainRoadMaterialReady` always returns false; the opt-in env
  read was removed). History: introduced by ROAD-PBR-ASPHALT-1/GRAVEL-1
  (`d7c0ff2d`), demoted to opt-in by ROAD-PBR-OPTIN-1 (`63bb0832`,
  USER RULING 2026-07-05: PBR asphalt look rejected), retired for good
  by ROAD-PBR-ABANDON-1.

## ENV-DROP registration sweep (2026-07-07)

Gates found in source but missing from `scripts/check-env-registry.sh` (now allowlisted, batch 8). All default-OFF unless noted.

- `MC2_GAMEOS_UI_DEFS=0` — force the legacy GameOS FIT UI path. Default **ON** (data/defs is the replacement path, not an opt-in). `gui/UiDefs.cpp`.
- `MC2_EDITOR_UI_DEFS=0` — force the legacy pure-MFC editor shell. Default **ON** (editor shell overlay is data/defs-first).
- `MC2_UI_DEFS_LOADSCREEN=1` / `MC2_UI_DEFS_RESULTS=1` — opt the loadscreen pages / mission-results page back into data/defs UI (both pinned to legacy by default until authored).
- `MC2_UI_DEFS_IGNORE_PACKAGE_VERSION=1` — bypass the UI defs package version check (modder iteration escape hatch).
- `MC2_UI_DEFS_TRACE=1` — UI defs resolve/replacement trace (`gui/UiDefs.cpp`, `GuiRuntime/GuiRuntime.cpp`).
- `MC2_FORCE_43=1` — forced 4:3 letterbox composite box (launcher "Force 4:3" toggle; unset = full surface, byte-identical).
- `MC2_HUD_SCALE=<0.2..1.0>` — HUD scale env seed; the ImGui slider wins after startup.
- `MC2_PREVIEW_FBO_SCALE=<1..8>` — camera-preview FBO supersample factor (default **4** = 3200x2400).
- `MC2_PREVIEW_NOCULL=1` — panel-preview force-disable `GL_CULL_FACE` (FBO winding-order debug).
- `MC2_PNG_MAGENTA_KEY=0` — killswitch for PNG magenta→alpha keying at decode. Default **ON**.
- `MC2_LEGACY_LOAD_BANNER=1` — restore the retired world-floating blue load banner overlay.
- `MC2_SCREENSHOT_SOURCE=backbuffer` — `MC2_SCREENSHOT_AT_FRAME` capture reads FBO 0 backbuffer instead of sceneFBO (needed for logistics/menu screens; requires non-minimized window).
- `MC2_SHOT_RESULTS=1` — RESULTS-CAPTURE-1: deterministic AAR screenshots of results screens after 30 settled frames (pairs with `MC2_DEV_SHELL=1`).
- `MC2_SOAK_STOP_AT_BAY=1` — soak harness parks at the mech bay instead of auto-advancing (requires `MC2_SOAK_AUTOWIN`).
- `MC2_DRAW_PACKET_V3=1` — static-prop DrawPacket v3 candidate→`RenderCore::DrawPacket[]` ABI promotion (no dispatch; legacy flush unchanged).
- `MC2_DRAW_PACKET_COALESCE_V5=1` — DrawPacket v5 per-draw-call substitutive dispatch (requires ARB_base_instance). `_TRACE` variant logs.
- `MC2_DRAW_PACKET_STATIC_PROP_V6_TRACE=1` — v7 canonical packet+meta dispatch trace (the v6 opt-in gate itself is retired; kill-switch is `MC2_STATIC_PROP_LEGACY_DISPATCH=1`).
- `MC2_STATIC_C1B_DRAW=1` — re-enable the (architecturally buggy) C1b GPU indirect instance count for the per-type legacy static-prop draw loop.
- `MC2_REGFLUSH_GUARD2=1` — static-registry persistent-vanish discriminator: per-typeID stale buckets + consecutive-stale streaks (streak ≥16 frames = real drop, not transient cull). Per-range map cost when on.
- `MC2_M2D_PZ_PARITY=1` — quad `pz` clip-path vs legacy-formula parity probe (demoted-not-deleted after green).
- `MC2_3DVIEW_DIAG=1` — one-shot 3D-view composite diagnostics (`gameos_graphics.cpp`).
- `MC2_TERRAIN_DECAL_TRACE=1` — CLIFF_WALL mesh-decal placement trace (`mclib/bdactor.cpp`).
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

- `MC2_LIGHT_BILLBOARD_TWOSIDED=0` — killswitch, default **ON**. Restores legacy backface culling of beacon/runway edge-light glow cards.

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

The cure, in `shaders/static_prop.vert` under per-instance flag **bit 4**
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

- `MC2_TERRAIN_DEBUG_MODE=N` — terrain frag debug-mode (0=off, 1=DepthCmp, 2=RawColormap, 3=BlurredColormap, 4=MatWeights R=rock/G=grass/B=dirt, 5=NormalLighting, 6=ShadowFactor, 7=CloudShadow, 8=CementDiag, 9=ThinRecordDiag, 10=HeightNormal, 11=HemiAdditive, -1=TessAliveProbe).
- `MC2_TERRAIN_NORMALS_FROM_HEIGHT=1` — derive normal from R32F height tex. Default **OFF**.
- `MC2_TERRAIN_HEIGHT_RESAMPLE_FACTOR=N` — CPU bilinear resample (1/2/4). Default 1.
- `MC2_TERRAIN_LIGHTING_V1=1` — hemisphere ambient fill on terrain. Default **OFF**.
- `MC2_TERRAIN_LIGHTING_V2=1` — shadow-aware V1 modulation. Default **OFF**.
- `MC2_TERRAIN_CLIFF_SHADOW_FLOOR=<float>` — CLIFF SHADOW FLOOR: lifts shadow-side steep terrain cliff faces off near-black by applying `shadow = max(shadow, floor * smoothstep(0.85,0.55,|normal.z|))` in `terrain_lod_chunk.frag` (LodChunk path, mc2_01) and `gos_terrain.frag` (legacy). ImGui-tunable 0..0.6 (Graphics Options → "Cliff Shadow Floor"). Default/unset/`0` = **byte-identical** (no-op). Only steep faces affected; flat terrain untouched.
- `MC2_TERRAIN_DECAL=1` — TERRAIN-DECAL-SLICE-0A: terrain mesh-decal system. When ON, cliff-wall decoration types (currently the `MarbleCliff` appearance) are submitted to the GPU static-prop batcher with an explicit **CLIFF_WALL face-frame** mat4 (up=world vertical, facing=outward cliff normal from `getTerrainNormal` 4-corner avg, tangent=contour) + small outward offset, instead of the yaw-only building recipe (`mclib/bdactor.cpp` `BldgAppearance::registerStatic`). Uniform scale only. Default **OFF** = byte-identical. `MC2_TERRAIN_DECAL_TRACE=1` prints a one-line `[TERRAIN_DECAL v1] CLIFF_WALL …` frame trace. Mesh generated by `tools/gen_cliff_wall_glb.py`; placed on mc2_01 via `tools/place_cliffwall_mc2_01.py` (deploy-side).
- `MC2_TERRAIN_MATERIAL_TEXTURES=1` — TERRAIN-MATERIAL-TEXTURES-1: per-layer PBR albedo on the LOD-chunk terrain. Loads `data/terrain_layers/<channel>_albedo.ktx2` (rock/grass/dirt/concrete/snow/cliff, BC7 **sRGB**, mip-complete) into a 6-layer `GL_TEXTURE_2D_ARRAY` (`GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM` — the sampler does the sRGB decode) and composes it world-space by matWeights/snow with the material-lib per-layer tiling; the colormap stays the MACRO tint (2× multiply). Steep slopes take the CLIFF layer triplanar (cliffs read as real rock). Default **OFF** = byte-identical (`u_useMatAlbedo=0`, tint path verbatim). Fail-soft: bad/missing layer files log `[TERRAIN_MAT_TEX]` and keep the legacy look. JSON (`terrain_materials.json`): `matAlbedoStrength`, `textureRoot`, `layers.<channel>.albedo` path overrides (TinyJson nested-layers extension). VRAM ≈ 32 MiB.
- `MC2_TERRAIN_MATERIAL_TEXTURES_STRENGTH=F` — albedo mix strength override, clamped [0,1] (0 = legacy tinted colormap, 1 = full textured composite). Wins over JSON `matAlbedoStrength`; default chain env > JSON > **0.7**. Only consumed when the gate is on.
- `MC2_TERRAIN_SHORELINE=1` — land-side wet/foam shoreline band (TERRAIN-SHORELINE-V3), placed by **elevation** in the frag (`v_worldPos.z` vs `u_waterElevation` — the same source the water fast path uses) so the band hugs the RENDERED waterline by construction, at any LOD/slope. No sidecar is required. Default **ON** since SHORELINE-SMOOTHING-REVISIT-1 (deferred-sweep); `=0` killswitch restores the legacy screen-space `runShoreline()` glow (which traces the tile-quantized water edge and amplifies the staircase at night). Suppresses the legacy screen-space `runShoreline()` post pass whenever this gate is on.
- `MC2_TERRAIN_SHORELINE_FILE=<path>` — override the OPTIONAL mask sidecar path (default `data/missions/<stem>.beauty/shoreline_mask.png`), offline-cooked by `tools/terrain_beautify/cook_shoreline.py`. When present, its G/B channels MODULATE the elevation bands (wide-beach falloff / basin exclusion); when absent, the gate alone still produces full elevation bands.
- `MC2_TERRAIN_SHORELINE_STRENGTH=F` — wet/damp darken intensity multiplier, clamped [0,2]. Default 1.0.
- `MC2_TERRAIN_SHORELINE_FOAM=F` — foam rim intensity multiplier, clamped [0,2]. Default 1.0.
- `MC2_TERRAIN_SHORELINE_WET_RUN=F` — V3 wet-lobe width as a **horizontal** world-unit run inland from the drawn waterline (the frag converts vertical rise → horizontal run via the macro slope). Default 16.0 (~4.8 m). **UNIT CHANGE** vs the pre-horizontal-fix knob: values are horizontal runs, not vertical heights.
- `MC2_TERRAIN_SHORELINE_FOAM_RUN=F` — V3 foam-rim width, horizontal world-unit run from the waterline. Default 5.0 (~1.5 m). Same unit change as `_WET_RUN`.
- `MC2_TERRAIN_SHORELINE_WET_HEIGHT=F` / `MC2_TERRAIN_SHORELINE_FOAM_HEIGHT=F` — **legacy aliases** for `_WET_RUN` / `_FOAM_RUN` (consulted only when the `_RUN` name is unset). NOTE: since the horizontal-distance fix these values are interpreted as horizontal runs, not vertical heights — an old `_WET_HEIGHT=3.0` now means a 3 wu (~0.9 m) run, far narrower than the 16 wu default.
- `MC2_TERRAIN_SHORELINE_EDGE_JITTER=F` — V4-STYLE static world-XY fbm jitter amplitude (horizontal wu) added to the band's distance-from-waterline, so the wet/foam lobes stop tracing the mesh waterline's straight diamond segments (the "zigzag" read). Placement-stable (no time term, camera-independent) and leak-proof (the strictly-above-water clamp uses the un-jittered distance — bands are ALWAYS zero at/below the drawn water plane as of V4). Clamped [0,32]; `0` = exact V3 contour. Default 4.0 (~±3 wu wander).
- `MC2_WATER_SHORE_FEATHER=F` — SHORELINE-STAIRCASE-1 (MASTER-FAIL #1). The **water-body outline** (not the land-side wet band above) snaps to the terrain tile grid — ~140 px axis-aligned steps with dead-straight risers at top-down altitudes — because the water FS shore alpha ramp (`shoreBlend = max(alphaDepth, 1)`) is only ~`alphaDepth` world-units wide, i.e. a 1-2 px transition that reads as a hard tile step. This gate sets a **floor (world units) on the effective `alphaDepth`** used by BOTH the FS shore smoothstep (fed through the water bridge — no `gameos_graphics.cpp` change) AND the beach-ring `shoreExt` geometry (which is exactly `alphaDepth*0.5`, the band's land-side extent, so the two stay matched). A wider band dissolves the staircase into a soft gradient that **hugs the rendered waterline by construction** (the band is driven per-fragment by `WaterThickness = waterElevation − interpolated terrain z`, the same principle as V3), so it auto-widens horizontally on flat shores (where the staircase is worst) and stays tight on cliffs. No sidecar required. Default **unset/≤0 = OFF = byte-identical** (`EffAlphaDepth` is the identity). Applies to both the CPU thin-record and GPU-driven water build paths (predicates kept in lockstep). Recommended live value ≈ **20 wu** (taste range 16-28); at 20 the critical mc2_10 top-down inlet's block-staircase structural signature falls below the shore-staircase sentinel's struct threshold while the shore still reads defined, and the flat mc2_01 shore softens without over-washing. The band's *screen* width scales with shore flatness (a fixed wu floor is wider in screen space on flat shores, tighter on cliffs), so very flat maps read softer at the same value. Larger values (e.g. 80) exaggerate for mechanism verification (full dissolve).
- `MC2_WATER_SHORE_MEANDER=A` / `MC2_WATER_SHORE_MEANDER_FREQ=F` — SHORELINE-GEOMETRY-1 (master-FAIL #1 re-open: the user rejected the feather above as "still a literal stair step"). **The silhouette fix.** Diagnosis: the water FS discard contour is the iso-line of a *bilinearly interpolated* `WaterThickness`, so it is already sub-tile — but the stock heightfield is **terraced** (long runs of shore vertices share one elevation), which degenerates that iso-line onto the tile lattice: each boundary tile contributes one straight segment and those segments chain into the axis-aligned / 45-degree risers the user annotated. Widening the alpha band (`MC2_WATER_SHORE_FEATHER`) blurs the risers but cannot bend them — it is an alpha fix for a geometry-shaped defect. This gate instead perturbs the **field**: it adds a static, zero-mean, world-space 2-octave fBm displacement of amplitude `A` (world units) to the thickness that drives the shore term and the `discard`, so the contour meanders through tile interiors. Horizontal contour displacement is `A / |grad(thickness)|`, i.e. **self-scaling** — large across the flat terraces that produce the worst steps, small on cliffs where the coast is already organic. The noise is **time-invariant** (a swimming coastline would be worse than a stepped one) and keyed on continuous `WorldPos.xy` (seam-free, camera-independent). The tile-admission `shoreExt` widens by `A` in lockstep across all four predicate mirrors (`WaterStream::EffShoreExt` is now the single source of truth for the CPU stream build, both `mclib/terrain.cpp` narrow-append branches, and the `u_shoreExt` fed to `gpu_driven_water.comp`), so water geometry exists wherever the meander can push the contour landward — without that, landward excursions clip back to the tile boundary and the step reappears. Only the shore/alpha term is perturbed; the depth **tint** still uses the true `WaterThickness`, so shallow/deep colour stays keyed to real terrain and only the coastline SHAPE changes. `F` is the spatial frequency in cycles/wu (default `1/384` ≈ one dominant cycle per 3 tiles — long enough to bend a whole riser rather than serrate it). Default `A` unset/≤0 = **OFF = byte-identical** (FS skips the term, `EffShoreExt` collapses to the stock expression). GL path only (the Vulkan water island reads `alphaDepth` via its own push constant and is unaffected).
- `MC2_TERRAIN_VISUAL_HEIGHT=1` — loads the 4x-res VISUAL heightfield bake into an SSBO (binding 26) for corner-pinned interior displacement (`mclib/terrain.cpp`). Default **OFF** = no load, no SSBO, byte-identical; also enabled implicitly by `MC2_TERRAIN_VISUAL_DISPLACE=1`. `MC2_TERRAIN_VISUAL_HEIGHT_FILE=<path>` overrides the bake file path.
- `MC2_TERRAIN_LOD_GEOMORPH=0` — TERRAIN-LOD-GEOMORPH-1 killswitch. When the bake shipped `visual_height_mips.r32` (max-preserving per-stride mips appended to binding 26), coarse-band interior verts sample their OWN stride's max mip (silhouette keeps peaks) and geomorph-lerp toward the parent band near the demotion threshold. Rides `MC2_TERRAIN_VISUAL_DISPLACE` (displace OFF or mips absent = inactive, byte-identical); `=0` disables the mips+morph even when the sidecar is present.
- `MC2_TERRAIN_LOD_MORPH_START=F` — geomorph ramp start as a fraction of the LOD band (default 0.6; morph runs over the outer 40% of the band). Clamped [0, 0.95].
- `MC2_TERRAIN_LOD_CHUNK_DIAG=41` — LOD-band tint debug view (LOD0..5 = green/cyan/blue/yellow/orange/red). `=42` — geomorph morph-factor heat (black=own band -> white=parent surface). Exact-value escapes like `=40` (shadow tier).

## Water gates

- `MC2_WATER_SKYTINT=1` — camera-independent sky tint on MDI water. Default **OFF**.
- `MC2_WATER_REFLECTION=1` — SH-L2 sky reflection (camera-dependent). Default **OFF**.
- `MC2_WATER_REFLECTION_RT=1` — mirrored terrain fill into reflection RT. Default **OFF**.
- `MC2_WATER_DEBUG_MODE=N` — MDI water frag debug (0=Final,1=Tint,2=Alpha,3=Normal,4=Depth,5=Shore,6=Lighting,7=ReflSH,8=ReflRT,9=ReflBlend).

## VFX gates

- `MC2_VFX_DEBUG_MODE=N` — GPU particle frag debug (0=Final,1=Albedo,2=Alpha,3=Kind,4=Overdraw).
- `MC2_VFX_AGE_SAMPLE=1` — sample curves at real m_age. Default **OFF**.
- `MC2_VFX_ORACLE_RENDER=1` — Phase 1 originals-restoration (CardCloud+ShardCloud oracle). Default **OFF**.
- `MC2_VFX_GPU_SIM_CARDCLOUD=1` — CardCloud GPU sim SSBO substrate. Default **OFF**.
- `MC2_VFX_SOFT_PARTICLES=1` — depth-fade soft particles. Default **OFF**.
- `MC2_VFX_LIT_PARTICLES=1` — ambient+sun*0.5 fill on particles. Default **OFF**.
- `MC2_VK_VFX_COALESCE=0` — VK-VFX-COALESCE-1: merges consecutive same-render-state HUD
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
- `MC2_CEMENT_TONE_MATCH=1` — ⛔ **USER-REJECTED — DO NOT RECOMMEND, DO NOT PROMOTE.** The user graded the BEFORE/AFTER and their verdict was *"after is WAY worse"*. Keep default **OFF**; it is retained only as a historical lever. The paragraph below documents what it *attempts*, **not** a cure that works — the patchwork it targets is **not** cured by it. (The same rejection applies to `MC2_RUNWAY_TONE_MATCH`, which is additionally census-proven inert on stock maps.) **CEMENT-ATLAS-TONE-MATCH-1** (MC2-03-CEMENT-PINK-1). *Attempted* to cure the concrete **patchwork** on airbase aprons/runways (mc2_24, mc2_03): the cement catalog packs concrete art from multiple authored families whose base tone differs materially (measured on mc2_24: `(134,130,120)` warm tan vs `(100,100,100)` neutral grey), and cement fragments are **flat-lit by design** (`terrain_lod_chunk.frag`: `normalLight = mix(normalLight, 1.0, pureConcrete)`), so no shared lighting term unifies them and the raw per-cell tone difference shows at full contrast with straight tile-boundary seams. `BuildCementCatalogAtlas` (`gos_terrain_indirect.cpp`) normalises each cell's **concrete-band** colour — the `[P50,P90]` luminance window, which excludes dark asphalt/markings and highlights — to the component-wise **median** across cells, per channel (brightness *and* tint), gains clamped to `[0.70,1.40]`. Per-cell detail (slab lines, cracks, markings) untouched. Default **OFF** = atlas buffer byte-identical. Once per mission load, zero per-frame cost. Log: `[CEMENT_TONE_MATCH v1] event=applied cells=… adjusted=… ref_rgb=…`.
- `MC2_CEMENT_LIT=0..1` — **RUNWAY-APRON-TONE-2** (user report, mc2_10 @ bookmark 204937). Share of the terrain lighting term that CEMENT fragments obey; default `0.0` = stock fully flat-lit slab. **Measured root cause:** at that pose the apron draws from the flat-lit cement catalog atlas at luminance 145–171, while the runway under the parked aircraft is *not cement-classified at all* — `MC2_TERRAIN_LOD_CHUNK_DIAG=32768` shows it uncoloured (baked colormap only) — and so draws through the full terrain lighting at luminance 65 (bare terrain: 54). Two sources, one lighting-exempt, is why a single runway changes tone along its length. The `[CEMENT_CELL_TONE v1]` census shows `dark_cells=0` on both mc2_10 and mc2_24, i.e. there is **no dark asphalt family** and no atlas-side tone fix can reach it. At `1.0` cement obeys the same light as the terrain it sits on. Default `0.0` = byte-identical. **ON arm not yet captured** — see `.claude/RUNWAY-APRON-TONE-2-STATUS.md`.
- `MC2_RUNWAY_TONE_MATCH=1` — ⛔ **USER-REJECTED — DO NOT RECOMMEND, DO NOT PROMOTE.** Rejected outright alongside `MC2_CEMENT_TONE_MATCH` (*"after is WAY worse"*), **and** census-proven inert on stock maps (`dark_cells=0` on both mc2_10 and mc2_24 — there is no dark asphalt family for it to correct). Keep default **OFF**. **RUNWAY-APRON-TONE-2** wide-gain absolute-target cement atlas normalisation. Re-measures each cell's concrete band *after* `MC2_CEMENT_TONE_MATCH` and pulls it onto a cross-map target with a wide `[0.35,4.0]` gain (the existing pass clamps to `[0.70,1.40]`, which caps an asphalt-dominant cell at +40%). Highlights pass a soft knee (linear to 190, asymptotic to 255) so lane markings survive a >2x gain instead of clipping flat. Tunables: `MC2_RUNWAY_TONE_STRENGTH` (0..1, default 1.0) and `MC2_RUNWAY_TONE_TARGET="r,g,b"` (unset = the map's own upper-cluster band median). **Not recommended on stock maps** — the census reports `dark_cells=0`, so there is nothing to correct; retained for third-party/mod maps that do pack a dark family. Default OFF = atlas byte-identical. The `[CEMENT_CELL_TONE v1]` per-cell census prints regardless of the gate.
- `MC2_CEMENT_TRANSITION_FALLBACK=1` — **CEMENT-TRANSITION-FALLBACK-1** (MC2-03-CEMENT-PINK-1). Cures **missing road swaths** (user report, mc2_24). `PopulateRecipeCementWords` bakes a cement word for a **transition** tile (`isCement && isAlpha`) only when at least one corner `terrainType` resolves to a **solid** cement catalog layer *and* `deriveMaskIdFromCorners() >= 0`. A road tile running across open ground satisfies neither, bakes **no word**, and falls back to the bare baked colormap — but the paving/road **art lives in the cement catalog** (verify with `MC2_TERRAIN_LOD_CHUNK_DIAG=262144`), so the road runs, drops out, and resumes. ON: a dropped transition tile adopts the cement layer of an orthogonally adjacent tile that *did* bake a word (as a transition word with the all-corners mask; the shader re-derives its own coverage via CEMENT-HARD-EDGE-1, so outline shape is unaffected). Default **OFF** = no word changes. Census `[CEMENT_TRANSITION_DROP v1] event=census transition_baked=… dropped_no_layer=… dropped_no_mask=…` prints regardless of the gate.
- `MC2_CEMENT_ATLAS_MAGENTA_KEY=1` — **CEMENT-ATLAS-MAGENTA-KEY-1** (MC2-03-CEMENT-PINK-1). Solid cement catalog cells reach the atlas via `TerrainTextures::textureFromMemory` (`gos_Texture_Solid`), which — unlike the keyed transition path (`textureFromMemoryAlpha` / TERRAIN-ROAD-COLORKEY-TRANSPARENCY-1) — never honours MC2's legacy `0xFF00FF` colourkey. Magenta left in a solid cell is packed into the **gutterless** atlas and sampled `GL_LINEAR`, which can show as thin pink lines/patches along road and pad edges. ON zeroes matching texels (same chroma test as `terrtxm.cpp`: `r>150 && b>150 && r>g+40 && b>g+40`). Default **OFF**. The census line `[CEMENT_ATLAS_MAGENTA v1] event=census mission=… magenta_cells=… magenta_texels=… keyed=…` prints **regardless** of the gate, so a map can be judged without enabling it.
- `MC2_PAVED_OVERLAY_CLASS=0|1|2` — **PAVED-OVERLAY-CLASS-1** (user report on five maps: *"all of that concrete should be the same color. same for the roads"*; one runway changes tone along its own length; a *"weird coloured edge"* where pavement meets pavement). **Root cause:** roads and runways are `&Overlays` ids (`PAVED_ROAD=1`, `RUNWAY=7`, …) painted over an **arbitrary** base terrain type, while `isCement()` tests a flag on the **base slot** (`terrtxm.h`, types 10/13–20). The two channels are **disjoint**, so overlay-only pavement is never classified, never baked, renders as **lit** baked colormap beside a **lighting-exempt** cement apron, and increments **no drop counter** — it is invisible to the `[CEMENT_TRANSITION_DROP v1]` census. Measured by `[CEMENT_PAVEMENT_GAP v1]`: mc2_01 **29%**, mc2_03 **81%**, mc2_05 **41%**, mc2_10 **58%**, mc2_24 **34%** of paved-overlay tiles carry no cement word; at the user's mc2_10 `@204937` pose the runway is **100% unclassified**. Every pre-existing lever (`MC2_CEMENT_LIT`, `MC2_CEMENT_TONE_MATCH`, `MC2_RUNWAY_TONE_MATCH`, `MC2_RUNWAY_EDGE_BLEND`) lives **inside** the `isCement && isAlpha` guard or is multiplied by `pureConcrete` — which is why none of them can move these tiles, and why ROAD-OVERLAY-LIGHT-1 measured the runway luma **bit-identical** across all four `MC2_CEMENT_LIT` strengths. This gate adds an `IsPavedOverlayType()` predicate **outside** that guard. `1` = **lighting mode**: the tile gets the same lighting exemption cement receives (`cementWord` bit 22, baked **without** the bit-31 valid bit so every other cement reader ignores it by construction) but **keeps its own baked colormap albedo** — this is what cures a runway changing tone along its own length, since that variation *is* the per-tile lighting the neighbouring flat-lit cement never receives. `2` = **albedo mode** (A/B arm): additionally routes the tile into the solid cement atlas so it samples the apron's texture; fuller convergence but replaces the baked road art, and both prior albedo-swapping attempts were user-rejected as *"WAY worse"*. **A residual albedo gap in mode 1 is expected** — the baked colormap and the cement atlas are different source textures. Default **`0` = OFF = not one word written = byte-identical stock**. Prints `[PAVED_OVERLAY_CLASS v1] event=applied mission=… mode=… flat_baked=… albedo_baked=…` when on; with `MC2_TERRAIN_LOD_CHUNK_DIAG=32768` these tiles read **white** when classified and **black** when not.
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
- `MC2_BRIDGE_MOVER_STATE` — emit one `[MOVER v2] id=… name=… team=… pos=…` line per live
  mover (`code/mission.cpp:516`), plus `[MOVER v2 begin] t=… count=…`, at a period set by
  `MC2_BRIDGE_MOVER_PERIOD_SEC` (default 2.0s, floor 0.25s). **DEFAULT OFF**;
  diagnostics-only, no gameplay or render effect. Added to the smoke passthrough
  allowlist by DETERMINISM-RATCHET-2 (same ENV_DROP gap). Note `scripts/run_visual_capture.py`
  inherits the full environment (`os.environ.copy()`), so it was never subject to the drop.
- `MC2_TERRAIN_TESS` — TCS/TES terrain tessellation material. **DEFAULT ON**; `=0` skips
  the `gos_terrain` material load (ISSUE-34 kill-switch — same engine state as a TCS/TES
  compile failure, for headless A/B of the no-tess config; console prints
  `[TESS] MC2_TERRAIN_TESS=0 …`). Shadow builds (`txmmgr.cpp`) and blob-shadow
  suppression / GPU transform skips (`mech3d.cpp`, `gvactor.cpp`) are keyed on
  `gos_IsModernShadowPathActive()` (shadow_terrain material loaded), NOT on tess, so
  CSM/static shadows stay on and legacy pz-gated blob shadows stay off when tess is
  disabled — near-plane clipping stays uniform.
- Hemisphere ambient on the chunk path is env-gated like legacy: `MC2_TERRAIN_LIGHTING_V1`
  / `MC2_TERRAIN_LIGHTING_V2` (default OFF).

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

## Diagnostic JSONL trace (diagnostic_trace.cpp — 2026-06-17)

Structured per-event JSONL output replacing env-gated stderr prints.
Survives normal exits and most crashes better than stderr.
Crash handlers still write to stderr as the last-resort path.

- `MC2_DIAGNOSTIC_TRACE_FILE=<path>` — JSONL output file. Default: `debug_state/diagnostic_trace.jsonl`. Set to `off` to disable.
- `MC2_DIAG_TAGS=<list>` — comma-separated tag whitelist. Controls which tags write events.
  - Unset: default high-value whitelist (`GPU_CULL,LIGHTBAKE_PROOF,ANIM_GATE,SPFLUSH_COST_SPLIT,CONFIG,BUILD,DEVICE,SHADER_COMPILE`).
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
| `SPFLUSH_COST_SPLIT` | Static prop flush perf | `MC2_STATIC_PROP_FLUSH_COST_SPLIT=1` stderr |
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

**Static prop flush perf triage** — 10-frame perf window with per-bucket ns breakdown:
```powershell
$env:MC2_STATIC_PROP_FLUSH_COST_SPLIT="1"
$env:MC2_DIAG_TAGS="CONFIG,BUILD,DEVICE,SPFLUSH_COST_SPLIT"
```
`get_diagnostic_events("SPFLUSH_COST_SPLIT", 10)` — summary events with `frame`, `window_frames`, and all ns buckets.

**Light bake stability triage** — bake index permanence/parity proof:
```powershell
$env:MC2_LIGHTBAKE_STABILITY="1"
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
  **Registry status:** carried in the `scripts/check-env-registry.sh` `ALLOWLIST`
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
  omissions are (`MC2_TERRAIN_CLIFF_*` ×9, `MC2_TERRAIN_POM*` ×5,
  `MC2_TERRAIN_DECAL_*` ×8, `MC2_GPU_MECH_*` ×4, `MC2_POST_CAS*`,
  `MC2_SHADER_SPIRV*`, `MC2_FOG_HORIZON_*` ×3). `docs/flags.auto.yaml` is the
  generated full inventory (1154 names) and is the right reconciliation target
  for the long tail.
- **8 documented-but-dead** gates: `MC2_DRAWALPHA`, `MC2_GL_B`,
  `MC2_HUD_IMGUI_INFOWINDOW`, `MC2_IS_EDITOR`, `MC2_ROAD_PBR`,
  `MC2_STATIC_PROP_LIGHTING`, `MC2_VERIFY`, `MC2_VULKAN_ISLAND`.
- **5 ghost gates** — in the registry tables with zero code reader:
  `MC2_DRAW_PACKET_STATIC_PROP_V6`, `MC2_HUD_IMGUI_PAUSE`,
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
`scripts/check-env-allowlist.sh:13-19` documents this and is advisory for exactly
that reason.

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
