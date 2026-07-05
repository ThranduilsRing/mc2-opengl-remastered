# Tier-1 instrumentation env vars

> **STATUS: Verified current as of 2026-07-01 (nifty HEAD, build pipeline). Includes MC2_TERRAIN_LOD_CHUNK (default ON), MC2_SHADOW_CSM (default ON), asset-mod payload vars (HDRI_BC6H, BUILDING_PBR), diagnostic JSONL trace, and the Vulkan-prep / frame-graph-executor / render-backend-iface seam gates. Match against RendererFeatureRegistry.h for new feature gates.**

## Live data-contract guards (MC2-VERIFY-LIVE-1)

- `MC2_VERIFY_MODE` — mode for the live `MC2_VERIFY` guard family (`docs/verify-primitive.md`). `log` (**default**): shadow-log `[VERIFY]` lines to stderr + crash-bundle ring, continue; `[VERIFY] mission-end fires=N` counter at Mission::destroy. `fatal`: STOP with message + crash bundle + hard terminate (exit 0xE0564631). `off`: exactly-legacy (silent, guarded degradations skipped). Smoke-allowlisted.

## Crash-soak harness (MC2_SOAK_AUTOWIN)

- `MC2_SOAK_AUTOWIN=1` — with a campaign booted via `MC2_BOOT_TO_BAY`, auto-launches each mission from logistics (no clicks), auto-wins it, lets the campaign auto-advance, and repeats until `campaign-complete`. Emits `[SOAK]` stdout markers (autowin/advance/launch/campaign-complete). Default OFF = byte-identical.
- `MC2_SOAK_WIN_AFTER_SEC=5.0` — scenario-time seconds before auto-win fires (default 5.0). Only consulted when `MC2_SOAK_AUTOWIN` is set.

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
- `MC2_GPUBUF_LIGHT_GROWONCE=1` — **default OFF** (VULKAN-CONTRACT-MANIFEST-ARC, LIGHT-GROW-ONCE-SUBDATA-1). The **AMD-safe intermediate** for the light SSBO orphan churn — **NOT** the full `GpuStorageRing` (no ring, no N-buffer rotation). Keeps the single persistent light SSBO (same `s_lightDataSsbo` handle, slot-20 `glBindBufferBase`, std430 `LightsData` layout / shader contract — unchanged) but eliminates the per-frame FULL `glBufferData` orphan re-spec (1.85 MB/frame, the `LIGHTSSBO-ORPHAN-1` NVIDIA no-stall workaround). **ON =** per frame upload ONLY the live used bytes via `glBufferSubData` (routed through `MC2_GL_BufferSubData`, NOT the owner `glBufferData` macro); the GL buffer **grows ONLY when used bytes exceed current capacity** — rare, sized with **+128-record headroom** matching the CPU backing grow step (`txmmgr.cpp addLightDataStructure`), draining via `glFinish` before delete/recreate/rebind-to-slot-20 and logged **once per grow** (`[LIGHTSSBO v1] event=growonce_grow`, requires `MC2_LIGHTSSBO_TRACE`). The split (`_UploadSplit`) path is subsumed into the same full-live-bytes SubData when ON (the prefix/suffix optimization is moot without an orphan). **OFF = the existing `LIGHTSSBO-ORPHAN-1` orphan path runs completely UNCHANGED, byte-identical GL stream + output** (one cached-bool branch; kill switch). With `MC2_GPUBUF_COUNTER=1`, the `[GPUBUF v1]` **`light` owner orphan bytes drop to ~0** steady-state (only rare grow frames emit a `glBufferData`). ⚠️ **NVIDIA is a HARD BLOCKER before default-on:** `glBufferSubData` into the single live buffer that is read all-frame by every lit draw (and cross-phase by the mech/static-prop batchers) has a cross-frame in-flight-write hazard — on NVIDIA SubData-into-in-flight STALLS the CPU (exactly the stall the orphan dodged); AMD tolerates it. Default-OFF until NVIDIA is in the verification loop; the fix is the full ring (out of scope here), NOT an N-buffer rotation.

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
- `MC2_VULKAN_EDGE_FOG_ISLAND=1` — Vulkan-native edge-fog island (`GameOS/gameos/vulkan_edge_fog_island.cpp`, VULKAN-CONTRACT-MANIFEST-ARC Layer 3): GL depth/color readback bridged into a Vulkan compute pass, pixel-parity-proven vs the GL path. Default **OFF**, presence-gated; falls back to GL if the probe device isn't viable. `MC2_VULKAN_ISLAND_FORCE_FALLBACK=1` forces the fallback for testing.
- `MC2_VULKAN_SWAPCHAIN_PRESENT=1` — VULKAN-CONTRACT-MANIFEST-ARC Layer 5: engine-owned Vulkan swapchain create/present/resize (`vulkan_backend_skeleton.cpp`, `vulkan_swapchain_present.cpp`), validation-clean core+sync per the L5 proof. Default **OFF**, presence-gated. `MC2_VULKAN_SWAPCHAIN_PRESENT_HIDDEN=1` runs it against a hidden window for headless/CI use.

## Frame-graph executor + backend-iface seam (RENDER-FRAME-GRAPH ARC)

- `MC2_FRAMEGRAPH_EXECUTOR=1` — top-level frame-graph pass executor (`gos_postprocess.cpp` `executorEnabled()`, `mclib/render_contract.cpp`). Default **OFF**. Value must be `'1'`. Substrate — the reorderer below is NOT shipped; executor stays default-OFF.
- `MC2_FRAMEGRAPH_REORDER_SPMECH` — **REJECTED experiment, default OFF, presence-gated** (`mclib/txmmgr.cpp`). Would swap the StaticProp-flush ↔ Mech-flush order (the one legal adjacent swap the reorder oracle proved). PARITY-FAILED on depth-EQUAL ties (mc2_24 overlapping opaque fragments resolve order-dependently); candidate rejected, kept only as an A/B lever. Never set in production. `=0`/unset = today's order verbatim (byte-identical).
- `MC2_FRAMEGRAPH_DRYRUN=1` — frame-graph dry-run mode (`render_contract.cpp` `dryrunEnabled()`). Default **OFF**. Value must be `'1'`.
- `MC2_FRAMEGRAPH_AMBIENT_GUARD` — ambient-handoff contract guard (colorMask / depthFunc / depthWrite axes; `render_contract.cpp`). **DEFAULT ON**; only `=0` disables. Companion CI check: `scripts/check-ambient-guard.py` (reads the dump counter).
- `MC2_FRAMEGRAPH_AMBIENT_FATAL=1` — hard-`abort()` on an ambient-guard mismatch (CI/bisection teeth). Default **OFF**, presence-gated. Legacy alias `MC2_AMBIENT_ASSERT_FATAL` also honored.
- `MC2_RENDER_BACKEND_IFACE=1` — route the scene-FBO bind through `IRenderBackend` / `GLBackend` instead of direct `glBindFramebuffer` (RENDER-BACKEND-IFACE-FBO-1; `gos_postprocess.cpp`). Default **OFF** = direct GL, byte-identical. ON = same GL call via the backend seam (proves the seam routes; output identical). Value must be `'1'`.
- `MC2_RENDER_BACKEND_REGION_IFACE=1` — MASTER gate (RENDER-BACKEND-REGION-IFACE-1, Layer-6 ENTRY; `gos_postprocess.cpp`). Route the WHOLE PostprocessFog region (EdgeFog + OOB fog) through `IRenderBackend::runRegion()` with a backend-neutral context (`RenderResourceId` + params + dims, NO raw handles). Default **OFF** = the original direct-GL fog sites run, abstraction fully skipped, byte-identical. ON = one selectable region routed through the backend; BOTH original fog sites skipped (no double-apply). Value must be `'1'`.
- `MC2_POSTPROCESS_BACKEND=gl|vk` — sub-selector for the region iface (only meaningful with the master gate `MC2_RENDER_BACKEND_REGION_IFACE=1` ON). `gl` (default, ships) = the extracted GL fog wrapper (`runFogRegionGL`, `runEdgeFog`+`runFogOob` unchanged); byte-identical to the original path (`region_impl=GLInline`). `vk` (opt-in only) = **the TRUE Vulkan opt-in** (POSTPROCESS-VULKAN-BACKEND-OPT-IN): selecting `vk` RUNS the Vulkan subgraph directly — the backend selection IS the enable signal, so it does **NOT** require `MC2_VULKAN_POSTPROCESS_SUBGRAPH=1` (requires `MC2_VULKAN_ISLAND` build only). Fail-soft: on a genuine Vulkan device/init/render failure the region falls back to the GL wrapper (`region_impl=FallbackGL`), no crash, output == GL. On success `region_impl=VulkanSubgraph`, `vk_draws=2`. NO default flip — `gl` ships. The standalone `MC2_VULKAN_POSTPROCESS_SUBGRAPH=1` gate + the per-island gates remain independently runnable for their own testing (both route through the same gate-free readiness half). Surfaced in the health dump as `render_backend_region.backend_region_selected` = `gl|vk|fallback_gl`.

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

### Dynamic CSM (reworked 2026-06-16 `8d36b37d`/`b8f764b6`/`13e96cc9`/`ead760df`)

- `MC2_SHADOW_CSM` — master cascaded-shadow-map toggle. Default **ON** (2026-06-18, `8ff13a36`); opt out with `=0`.
- `MC2_SHADOW_MAP_SIZE` — per-cascade shadow texture size. Default **8192**. 3×8192² ≈ 805 MB VRAM; `=4096` ≈ 201 MB (still sharp at R0=512).
- `MC2_SHADOW_CSM_R0` — near-cascade fit radius (WU). Default **512** (→ 0.25 WU/texel at 4096).
- `MC2_SHADOW_CSM_R1` — mid-cascade fit radius (WU). Default **4096**.
- `MC2_SHADOW_CSM_COUNT` — cascade count. Default **3** (R0 near / R1 mid / full-map far).
- `MC2_SHADOW_CSM_SOFTNESS` — PCF softness. Default **0.9**.
- `MC2_SHADOW_OBJ_NORMAL_BIAS` — object self-shadow normal-offset bias (kills residual acne). Default **2.0**.
- `MC2_SHADOW_MECH_SOFT` — mech self-shadow softness (wider PCF penumbra + terminator smoothstep so flat low-poly facets fade instead of flip + raised floor). Default **1.0**, clamp [0,4]. Object self-shadow is now per-type via GBuffer1.a mask: terrain skip / static-prop NO self-shadow / mech soft (`3253d582`).
- `MC2_PROP_SHADOW_RECEIVE` — PROP-SHADOW-RECEIVE-1: static props RECEIVE the dynamic CSM cascade (building self-shadow + prop-on-prop) in `shadow_screen.frag`. Default **OFF**; `=1` enables. OFF = legacy static-map-only reception for the a=0.25 prop class (pixel-identical). Shares the CSM PCF ledger + `MC2_SHADOW_MECH_SOFT` terminator; floor 0.4 min-combined with the static map (no double-darken).
- `MC2_SHADOW_PROP_ALPHA` — tree-foliage shadow alpha-test (`shadow_static_prop.frag`, legacy texture path only). Default **ON**.
- `MC2_CLOUD_SHADOW` — cloud-shadow pass. Default **ON**.
- **DEBUG-only:** `MC2_SHADER_PATH_TINT` (shader-path tint); `MC2_TERRAIN_DEBUG_MODE` / `MC2_TERRAIN_LOD_CHUNK_DIAG` = **30/31** = dynamic-shadow viz.
- **RETIRED:** `MC2_SHADOW_CSM_FULLMAP_LAST`, `MC2_SHADOW_CSM_NEAR_MAX` (folded into the fixed-cascade rework).

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
- `MC2_SMART_LOAD=1` — SMART-LOAD startup/mission-load perf path (`code/logisticsdata.cpp`). Default **OFF**. `MC2_SMART_LOAD_TRACE=1` logs the load-time breakdown.
- `MC2_UNIT_PROFILE_DATA` — UNIT-PROFILE-ARC data-driven unit+equipment definition source (`code/unitprofile.cpp`). Default **LEGACY** (unset/`0` = legacy path); any non-`0` value switches to the generated `UnitProfileData` source.

## EditorBridge

- `MC2_EDITOR_MODE` — activates `EditorBridge` API. Default `0`. Combine with `MC2_OBJECT_ID_BUFFER=1` for GPU pick.

## Render-contract assert

- `MC2_RENDER_CONTRACT_ASSERT=1` — runtime GL draw-buffer/depth state vs declared expectation. Phase 2 (`137dc70`).

## Pre-commit invariant scripts

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
- `MC2_STATIC_PROP_DEBUG_MATERIAL=N` — frag debug view (0=Final,1=Albedo,2=MatIdx,3=Normal,4=TexLayer,5=Rough,6=Metal).
- `MC2_STATIC_PROP_PBR_V1=1` — per-frag Schlick-Fresnel specular. Default **OFF**.
- `MC2_STATIC_LIGHT_UPLOAD_SPLIT` — upload immutable static light prefix `[0..S)` once/dirty-only + dynamic suffix per frame (vs whole LightsData SSBO every frame). Default **ON**. Kill=`=0` (legacy whole-buffer, bit-identical). Requires `MC2_LIGHTBAKE`.
- `MC2_LIGHTBRIDGE_STABLE_SKIP=1` — skip per-frame `EmitBakedGpuLightData` + `getCachedGpuLightIndex` + `staticReg` pointer-chain for stable baked static props (LIGHTBRIDGE-STABLE-SKIP-WIRE-1). Default **OFF**. Requires `MC2_LIGHTBAKE=1` (implicit when bake map populated). Gate: `stableLightSkipArmed` + `stableLightSkipEligible` + valid `lightDataIndex` + generation match. Expected: `serial_commit_us` ~593µs → ~50–80µs on stable frames. Diagnostics: `MC2_LIGHTBRIDGE_STABLE_SKIP_DIAG=1` (prints `LIGHTBRIDGE_STABLE_SKIP:` every 300 frames; independent of skip gate).
- `MC2_STATIC_PROP_FLUSH_CACHED_BLOB=1` — STATICPROP-REGISTRY-FLUSH-CACHED-BLOB-2A: replace per-leaf registry-flush rebuild with cached immutable instance/actor-record blobs bulk-appended into the existing rings. Keeps per-frame range walk + staleness skip. Default **OFF** pending Tracy proof.
- `MC2_STATIC_PROP_FLUSH_CACHED_BLOB_COMPARE=1` — diagnostic compare: with the cached path active, also build the legacy temp instance+record per leaf and compare hash/count; logs mismatches. Default **OFF**; requires `MC2_STATIC_PROP_FLUSH_CACHED_BLOB=1`.
- `MC2_LIGHTBAKE_STABILITY=1` — trace: per-instance `lightDataIndex` permanence/stability proof (`[LIGHTBAKE-PROOF v1] event=first/UNSTABLE`). Diagnostic, capped 32. Default **OFF**.
- `MC2_LIGHTBAKE_PARITY=1` — trace: baked permanent record == gathered transient record byte/hash (`[LIGHTBAKE-PROOF v1] event=parity match=1`). Diagnostic, capped 32. Default **OFF**.
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

- `MC2_WINDOW_RESIZABLE=0` — kill the resizable window. Default **ON** with
  `MC2_WINDOWED`: the SDL window is user-resizable; SIZE_CHANGED refreshes
  `Environment.drawableWidth/Height` + ImGui display size, and every consumer
  (postprocess FBOs, UI canvas box, mouse normalize, camera aspect) re-derives
  from drawable per frame — drag to any size and shape, everything adapts.
- `MC2_CAMERA_ASPECT_NATIVE=0` — restore the legacy stretched 4:3 frustum.
  Default **ON**: the in-mission perspective projection uses the drawable
  aspect (true proportions at any window shape); panel previews keep their
  fixed 4:3 FBO ratio via the preview-scope guard.

## Road PBR fail-soft (ROAD-PBR-FAILSOFT-1)

- `MC2_ROAD_PBR=1` — opt IN to the high-res PBR asphalt/gravel road
  materials (also requires `data/textures/asphalt_albedo.tga` /
  `gravel_albedo.tga` to be present; the bake checks
  `gos_TerrainRoadMaterialReady(matId)` per material). Default **OFF**
  (USER RULING 2026-07-05: PBR asphalt look rejected) — every install
  renders the original legacy 64Overlays road tiles. History: the PBR
  slice originally baked the matId unconditionally, so installs without
  the hand-placed TGAs sampled texture 0 -> black/invisible roads.

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
- `MC2_TERRAIN_SHORELINE=1` — land-side wet/foam shoreline band (TERRAIN-SHORELINE-V3), placed by **elevation** in the frag (`v_worldPos.z` vs `u_waterElevation` — the same source the water fast path uses) so the band hugs the RENDERED waterline by construction, at any LOD/slope. No sidecar is required. Default **OFF**. Suppresses the legacy screen-space `runShoreline()` post pass whenever this gate is on.
- `MC2_TERRAIN_SHORELINE_FILE=<path>` — override the OPTIONAL mask sidecar path (default `data/missions/<stem>.beauty/shoreline_mask.png`), offline-cooked by `tools/terrain_beautify/cook_shoreline.py`. When present, its G/B channels MODULATE the elevation bands (wide-beach falloff / basin exclusion); when absent, the gate alone still produces full elevation bands.
- `MC2_TERRAIN_SHORELINE_STRENGTH=F` — wet/damp darken intensity multiplier, clamped [0,2]. Default 1.0.
- `MC2_TERRAIN_SHORELINE_FOAM=F` — foam rim intensity multiplier, clamped [0,2]. Default 1.0.
- `MC2_TERRAIN_SHORELINE_WET_RUN=F` — V3 wet-lobe width as a **horizontal** world-unit run inland from the drawn waterline (the frag converts vertical rise → horizontal run via the macro slope). Default 16.0 (~4.8 m). **UNIT CHANGE** vs the pre-horizontal-fix knob: values are horizontal runs, not vertical heights.
- `MC2_TERRAIN_SHORELINE_FOAM_RUN=F` — V3 foam-rim width, horizontal world-unit run from the waterline. Default 5.0 (~1.5 m). Same unit change as `_WET_RUN`.
- `MC2_TERRAIN_SHORELINE_WET_HEIGHT=F` / `MC2_TERRAIN_SHORELINE_FOAM_HEIGHT=F` — **legacy aliases** for `_WET_RUN` / `_FOAM_RUN` (consulted only when the `_RUN` name is unset). NOTE: since the horizontal-distance fix these values are interpreted as horizontal runs, not vertical heights — an old `_WET_HEIGHT=3.0` now means a 3 wu (~0.9 m) run, far narrower than the 16 wu default.
- `MC2_TERRAIN_SHORELINE_EDGE_JITTER=F` — V4-STYLE static world-XY fbm jitter amplitude (horizontal wu) added to the band's distance-from-waterline, so the wet/foam lobes stop tracing the mesh waterline's straight diamond segments (the "zigzag" read). Placement-stable (no time term, camera-independent) and leak-proof (the strictly-above-water clamp uses the un-jittered distance — bands are ALWAYS zero at/below the drawn water plane as of V4). Clamped [0,32]; `0` = exact V3 contour. Default 4.0 (~±3 wu wander).
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

## Terrain LOD chunk

- `MC2_TERRAIN_LOD_CHUNK` — chunked heightfield LOD renderer. **DEFAULT ON** (cutover
  2026-06-09 `a7b090be`); set `=0` to opt out (legacy tessellated path, e.g. editor).
  Single-source gate `mc2TerrainLodChunkEnabled()` (terrain.h). Replaces
  makeLists/geometry()/TerrainQuad::draw() + suppresses legacy DrawIndirect.
- `MC2_TERRAIN_LOD_CHUNK_BRIDGE_OBJBLOCK=1` — bridge ObjBlockInfo.active from
  blockMeta.inFrustum each frame. Default **OFF**. Phase 2 only; removed Phase 4. Runtime getenv().
- `MC2_TERRAIN_LOD_CHUNK_FORCE_LOD=k` — force every chunk block to LOD k (diag).
- `MC2_TERRAIN_LOD_CHUNK_NO_SKIRTS` / `_NO_APRON` / `_NO_STITCH` — disable skirt seal /
  1-ring draw apron / vertex edge-stitch (A/B).
- `MC2_TERRAIN_LOD_CHUNK_SKIRT_MAX=px` — cap skirt depth (default 256).
- `MC2_TERRAIN_LOD_CHUNK_CEMENT_MAXLOD=k` — relax the cement-block LOD0 clamp (default 0).
- `MC2_TERRAIN_LOD_CHUNK_DIAG=<bitmask>` — frag viz (zero-cost off): 1 no-GBuffer1,
  4 no-lighting, 8 no-shadow, 16 flat-normal, 32 no-detail, 64 rock-sample,
  128 terrainType, 256 raw-colormap, 512 bypass-tint. (bit 2 dead — depth now in vert.)
- `MC2_TERRAIN_NORMAL_ARRAY` — material normal sampler2DArray path. Now **default ON**
  (opt-out `=0`); the chunk path needs it built (mip-completeness fixed 2026-06-09).
- Hemisphere ambient on the chunk path is env-gated like legacy: `MC2_TERRAIN_LIGHTING_V1`
  / `MC2_TERRAIN_LIGHTING_V2` (default OFF).

## Deterministic capture clock

- `MC2_SMOKE_FIXED_TIMESTEP=1` — deterministic capture clock (S9D sim + S9E render-shader).
  Opt-in, default **OFF** (byte-identical retail/normal smoke). Pins BOTH the sim clock
  (S9D: scenarioTime/frameRate advance a fixed 1/30s per frame) AND every render-shader
  `time` uniform (S9E: water/terrain/overlay/decal/screen-shadow/godray/shoreline, all in
  seconds, driven off the shared S9D fixed sim-frame counter via `SmokeMode::fixedClockSeconds()`),
  so frame N renders identically every run. Makes sim speed fps-proportional → it is a
  CAPTURE/DETERMINISM knob, **NOT** for full-duration regression smokes.

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
| `TERRAIN_ACTIVE_AB` | A/B false-negative counts | `MC2_TERRAIN_ACTIVE_AB` stderr |
| `TERRAIN_SOLID_AB` | Solid window A/B counts | `MC2_TERRAIN_SOLID_AB` stderr |
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
