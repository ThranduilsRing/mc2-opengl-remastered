# Tier-1 instrumentation env vars

> **STATUS: Verified current as of 2026-07-01 (nifty HEAD, build pipeline). Includes MC2_TERRAIN_LOD_CHUNK (default ON), MC2_SHADOW_CSM (default ON), asset-mod payload vars (HDRI_BC6H, BUILDING_PBR), diagnostic JSONL trace, and the Vulkan-prep / frame-graph-executor / render-backend-iface seam gates. Match against RendererFeatureRegistry.h for new feature gates.**

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
- `MC2_VK_INTEROP_BRIDGE=1` — **presence-gated `=1`, default OFF** (VK-INTEROP-BRIDGE-1). The zero-copy GL↔VK image interop SEAM, proven live on the EdgeFog **color** island (`GameOS/gameos/vulkan_edge_fog_island.cpp`; core `vk_image_resource.{h,cpp}` + adapter `gl_interop_view.{h,cpp}`). ON repoints the island's color render-pass attachment to an **exportable** `VkImageResource` (raw-allocated, `OPAQUE_WIN32` dedicated export) imported into GL zero-copy; GL renders scene color into the shared texture, hands it to Vulkan via a cross-API semaphore, Vulkan blends fog, hands it back — with the per-frame **COLOR** readback/upload gone (`glGetTexImage(RGBA)` / `vkCmdCopyImageToBuffer` / `glTexSubImage2D` for color), **hard-asserted ==0** (blocker #5; any color-copy firing → fail-soft + LOUD log). Requires `MC2_VULKAN_EDGE_FOG_ISLAND=1` (the per-pass island route). `useInterop` = gate ∧ interop-support ∧ LUID-ok; **fail-soft to the readback path** on any negative (LUID mismatch / missing external exts / import failure). **Gate OFF = byte-identical readback island with ZERO external probes** (blocker #6: the external-caps instance/device exts + LUID query are requested ONLY when the gate is ON). Depth still round-trips (color-only slice; `copy_depth_us` unchanged, but the color half is gone). Surfaced in the `[VK_EDGE_FOG]` health line: `interop_active`, `edgefog_used_interop`, `forbidden_copyBufToImg/copyImgToBuf/getTexImage/texSubImage` (all ==0 on the interop path), plus depth-side counters (EXPECTED). Companion infra gate `MC2_VK_INTEROP_SELFTEST=1` runs the standalone T1/T2 create/import/signal/wait self-check at island init.
- `MC2_VULKAN_POSTPROCESS_SUBGRAPH_INTEROP=1` — **presence-gated `=1`, default OFF** (VK-INTEROP-BRIDGE-1 **P2**). Puts the already-fused VK↔VK fog **subgraph** (edge_fog+fog_oob, `GameOS/gameos/vulkan_postprocess_subgraph.cpp`) **COLOR boundaries** on the zero-copy interop seam. The internal chain (2 VK draws / 1 render pass / 1 VK-owned intermediate, no GL between the draws) is **UNCHANGED**; only INPUT (scene color in) + OUTPUT (fog result out) move onto the seam. ON repoints the subgraph render-pass color attachment to an **exportable** `VkImageResource` imported into GL (`vk_image_resource.{h,cpp}` + `gl_interop_view.{h,cpp}`, post-P1.5 `vkToGlSem` VK→GL semaphore back-channel — GPU hand-off, no CPU stall); GL copies scene color into the shared tex, hands it to VK, VK blends both fog passes, GL copies the result back — with the per-frame **COLOR** readback/upload gone (`glGetTexImage(RGBA)` / `vkCmdCopyBufferToImage` / `vkCmdCopyImageToBuffer` / `glTexSubImage2D` for color), **hard-asserted ==0** (blocker #5; any color-copy firing → fail-soft + LOUD log + frame fail). Requires the subgraph to be selected (`MC2_VULKAN_POSTPROCESS_SUBGRAPH=1` or `MC2_POSTPROCESS_BACKEND=vk`). `useInterop` = gate ∧ resource-create-ok ∧ LUID-ok; **fail-soft to the subgraph readback path** on any negative. **Gate OFF = byte-identical readback subgraph with ZERO external probes** (blocker #6: the external-caps instance/device exts are requested ONLY when the gate is ON, and only inside `build_subgraph` which runs only when the subgraph itself is selected). **DEPTH still round-trips** (color-only slice; P5 scope). Surfaced in the `[VK_PP_SUBGRAPH_INTEROP]` health line: `interop_active`, `vk2gl_sem`, `used_interop`, `forbidden_copyBufToImg/copyImgToBuf/getTexImage/texSubImage` (all ==0 on the interop path). Golden-parity oracle = interop-ON vs the subgraph's OWN readback baseline (so fog_oob AMD-GLSL-vs-glslang noise cancels) — DEFERRED to a gated step.
- `MC2_VK_SSAO_INTEROP=1` — **presence-gated `=1`, default OFF** (VK-INTEROP-BRIDGE-1 **P3**). Puts the **SSAO island**'s (`GameOS/gameos/vulkan_ssao_island.cpp`) **COLOR I/O** on the zero-copy interop seam. Scope = COLOR only: the pass2 (apply) scene-color INPUT + blended-color OUTPUT move onto the shared exportable `VkImageResource`; **DEPTH + NORMAL inputs STAY readback** (P5 scope) and occlusion pass1 is VK-internal (unchanged). ON repoints the **pass2** (apply) render-pass color attachment to an **exportable** `VkImageResource` (raw-allocated `OPAQUE_WIN32` dedicated export, `vk_image_resource.{h,cpp}` + adapter `gl_interop_view.{h,cpp}`, post-P1.5 `vkToGlSem` VK→GL semaphore back-channel — GPU hand-off, no CPU stall) imported into GL; GL copies scene color into the shared tex, hands it to VK, VK runs occlusion pass1 + multiplicative apply pass2 over it, GL copies the AO-multiplied result back — with the per-frame **COLOR** readback/upload gone (`glGetTexImage(RGBA)` / `vkCmdCopyBufferToImage(color)` / `vkCmdCopyImageToBuffer(color)` / `glTexSubImage2D(color)`), **hard-asserted ==0** (blocker #5; any color-copy firing → fail-soft + LOUD log + frame fail). Requires the SSAO Vulkan backend selected (`MC2_SSAO=1` + `MC2_POSTPROCESS_BACKEND=vk`). `useInterop` = gate ∧ resource-create-ok ∧ LUID-ok; **fail-soft to the SSAO readback path** on any negative. **Gate OFF = byte-identical readback SSAO island with ZERO external probes** (blocker #6: the external-caps instance/device exts + LUID query are requested ONLY when the gate is ON, and only inside `build_island` which runs only when the SSAO island is selected). Layout-STABLE pass2 (finalLayout == initialLayout == `COLOR_ATTACHMENT_OPTIMAL`, P1.5) so sync-val is clean. Surfaced in the `[VK_SSAO_ISLAND_INTEROP]` health line + `vulkan_ssao_island` dump section: `interop_active`, `vk2gl_sem_active`, `ssao_used_interop`, `forbidden_copyBufToImg/copyImgToBuf/getTexImage/texSubImage` (all ==0 on the interop path), plus `depthnormal_*` counters (EXPECTED). Golden-parity oracle = interop-ON vs the SSAO island's OWN readback baseline (`--region PostprocessSSAO --backend-compare`; SSAO is parity-proven 5/5 — 4 byte-exact + `highangle_wide` within the ±1 LSB floor; reproduce the floor, don't improve it).
- `MC2_VK_WATER_INTEROP=1` — **presence-gated `=1`, default OFF** (VK-P9a). The **first Vulkan GEOMETRY region**: renders the armed water fast-path **base layer** in Vulkan (`GameOS/gameos/vulkan_water_island.cpp`) into an **exportable RGBA16F** `VkImageResource` that GL composites into MainColor, instead of the GL MDI water draw inside `gosRenderer::renderWaterFastPath` (mutually exclusive; this gate is the single arbiter). **Feed** (VK-P9a STEP 0 — VK-side copy/re-pack, NOT external buffer share): the **recipe** SSBO is copied **once at load**; per-frame **thin records** are **CPU-staged** from `WaterStream::g_thinStaging` (populated by the CPU pack `UploadAndBindThinRecords`, byte-identical to the GPU compute pack per `ComputeDispatchParity_Check`); **GL scene depth** is CPU-imported (`glGetTexImage(DEPTH)` → staging → depth image) as a **read-only depth attachment** so the shoreline depth-TEST is preserved (a depthless composite = the v0.3 staircase regression). No fenced GL buffer crosses the seam → the ring-3 thin fence (id 43) is irrelevant to VK; **R2 sync hazard dissolves by construction**. **Pipeline** (this tree's `PipelineRegistry` WaterArmed row): reverse-Z **GEQUAL**, depth-test on, **depth-write ON**, cull **None**, **MRT** color0 (MainColor) + color1 (GBuffer1), **alpha-blend** SRC_ALPHA/ONE_MINUS_SRC_ALPHA. The MVP water depth fudge `clip.z += 0.00025*clip.w` lives in the VS (`shaders/vulkan/water_geom.vert`), byte-copied from the GL water VS (WATER-MVP-ORACLE-1: `worldToClipGL` column_major std140, SAME 16 raw floats as the GL `GL_FALSE` upload, no transpose). **Reflection/HDRI OFF** (P9a). **UNLIKE** the postprocess-interop islands it **borrows the shared engine `VulkanContext`** (`getOrCreateEngineVulkanContext`, like `engine_present_vk.cpp`) instead of creating its own device — so **NO `volkLoadDevice` collision**, it is **NOT** coerced off by the R2 interlock, and it **REQUIRES `MC2_RENDER_BACKEND=vulkan`** (the shared device is born only then; **fail-soft to the GL water draw** when the context is absent/invalid). Color surface zero-copy; **forbidden COLOR-op counters hard-asserted ==0** (depth-IN copies EXPECTED, counted separately as `depthin_*`). Surfaced in the `[VK_WATER_INTEROP]` health line: `interop_active`, `vk2gl_sem`, `used_interop`, `thin_records`, `recipe_count`, `forbidden_*` (==0), `depthin_*` (EXPECTED). Golden-parity oracle = interop-ON vs the GL water baseline on a water-visible bookmark (`tests/visual/bookmarks/mc2_01_water.json`); watch shoreline z-fight / depth-fudge (the STOP-risk). **Gate OFF = byte-identical GL water draw.**
- `MC2_VK_SHADOW_INTEROP=1` — **presence-gated `=1`, default OFF** (VK-P8a). The **second Vulkan GEOMETRY region** and the **first VK-PRODUCES-DEPTH region** (water was GL-produces/VK-consumes; shadow reverses it): renders the **dynamic static-prop shadow-caster depth** in Vulkan (`GameOS/gameos/vulkan_shadow_island_live.cpp`) into an **exportable D32** `VkImageResource`. GL binds that shared D32 in place of `gosPostProcess::getDynamicShadowTexture()` and samples it via the **UNCHANGED** hardware `sampler2DShadow` `calcShadow()` path (`shaders/include/shadow.hglsl`) — lighting up the **vk2gl back-channel** (`glVkToGlSem` / `glInteropWaitFromVk`) for the first time on a geometry region: the VK submit **SIGNALS** vk2gl, GL **WAITS** it, then samples (the hardware-compare-over-imported-D32 path proven by `tools/vk_shadow_sampler_probe`). **Feed** (VK-P8a STEP 0, `.claude/VK-P8a-STEP0-CASTER-FEED-NOTE.md`): the CPU `GpuStaticPropInstance` vector (`dynShadowSet`, 112B each) the GL path already builds each frame is **memcpy'd** into a VK instance SSBO; the shared static-prop VBO/IBO are copied **ONCE per map** into VK-owned buffers (`mc2_shadow_island_get_geometry_feed`). **Pipeline**: depth-only, **LESS forward-Z** (NOT reverse-Z — matches the GL dynamic shadow map, `glClearDepth(1.0)`), cull **None**, `depthBias` set per-frame to `glPolygonOffset(shadowBiasFactor_, shadowBiasUnits_)` **EXACTLY** (the parity **STOP-watch** — bias divergence = acne/peter-panning). Reproduces `shaders/shadow_static_prop.vert` math bit-for-bit (`pos*M` row-vector → Stuff→MC2 swizzle → `lightSpaceMatrix*`). **BORROWS** the shared engine `VulkanContext` (`ensureEngineVulkanContextForInterop`) — no second device, no `volkLoadDevice`; **REQUIRES `MC2_RENDER_BACKEND=vulkan`** (**fail-soft to the GL shadow path** when the context is absent/invalid). Depth surface zero-copy; **forbidden DEPTH-readback counters hard-asserted ==0**. Surfaced in the `[VK_SHADOW_INTEROP]` health line + `vulkan_shadow_island` dump section: `interop_active`, `vk2gl_sem_active`, `used_interop`, `inst_records`, `type_runs`, `forbidden_*` (==0). Golden-parity oracle = interop-ON vs the GL shadow baseline on a shadow-visible deterministic bookmark (`MC2_SMOKE_FIXED_TIMESTEP=1`); the VK-produced shadow must match byte-exact or within a documented depth-precision floor (water ±1-LSB / SSAO `highangle_wide` precedent). **P8a scope** = opaque static props, single map (foliage-alpha + tessellated-terrain casters = P8b, bone-skinned mech casters = P12). **Gate OFF = byte-identical GL shadow path.**
- `MC2_VK_PROP_COLOR_INTEROP=1` — **presence-gated `=1`, default OFF** (VK-P11, Tier-0 seam probe). The **first VK-native COLOR geometry region** rendering **REAL static-prop triangle geometry** (the keystone the terrain/mech color regions inherit): renders the **visible static-prop color instances** in Vulkan (`GameOS/gameos/vulkan_color_prop_island.cpp`) into an **exportable RGBA16F** `VkImageResource`, with a **VK-OWNED (non-exported) D32** depth (cleared to reverse-Z far each frame) for correct **inter-prop** occlusion. GL **alpha-composites** (premultiplied-over, `ONE`/`ONE_MINUS_SRC_ALPHA`) the VK result over MainColor at the `GpuStaticPropBatcher::flush()` call site (`mclib/txmmgr.cpp` `runSpFlushUnit`) **instead of** the GL `flush()` prop color draws (mutually exclusive; this gate is the single arbiter). **Feed**: the shared static-prop VBO/IBO copied **ONCE per map** (reuses the shadow island's `mc2_shadow_island_get_geometry_feed`); per-frame the **visible color instances** the GL `flush()` would draw (`s_bucketsByType`, still populated because `flush()` is skipped) are flattened grouped by typeID (`mc2_color_prop_island_get_frame_feed`) and memcpy'd into a VK instance SSBO. **Pipeline**: **reverse-Z GEQUAL**, depth-test+write **ON**, cull **None**, premult-over color blend. VS (`shaders/vulkan/color_prop.vert`) reproduces `static_prop.vert` world→clip (`pos*M` row-vector → Stuff→MC2 swizzle → `u_worldToClipGL`=`gos_GetObjectFixBMVPMat4()` `GL_FALSE` → `clip.z += OBJECT_DEPTH_BIAS*clip.w`). Full 40B vertex stream mirrored (pos@0/normal@12/uv@24/id@32/aRGB@36). **Tier-0 FS** = flat/debug color (**no** texture-array/MaterialGpu/lighting). **BORROWS** the shared engine `VulkanContext` (`ensureEngineVulkanContextForInterop`) — no second device; **REQUIRES `MC2_RENDER_BACKEND=vulkan`** (**fail-soft to the GL `flush()`** when the context is absent/invalid → props still draw). Color surface zero-copy; **forbidden COLOR-readback counters hard-asserted ==0**. Surfaced in the `[VK_PROP_COLOR_INTEROP]` health line: `interop_active`, `vk2gl_sem`, `used_interop`, `inst_records`, `type_runs`, `forbidden_*` (==0). **TIER-0 scope**: props composite **OVER** the scene (prop-vs-terrain occlusion **APPROXIMATE**, **NOT** pixel parity — shared scene depth = Tier-1, a separate slice). **Gate OFF = byte-identical GL prop color path.**
- `MC2_VK_SHARED_SCENE_DEPTH=1` — **presence-gated `=1`, default OFF** (VK-TIER1 shared-depth substrate, step 2). Backs the engine **scene-depth** texture (`sceneDepthTex_`) with a **VK-BUILT exportable `D32_SFLOAT_S8_UINT`** `VkImageResource` (`GameOS/gameos/vulkan_shared_scene_depth.{h,cpp}`, reusing `vk_image_resource.{h,cpp}` + `gl_interop_view.{h,cpp}`) imported into GL as a **`GL_DEPTH24_STENCIL8`** texture (depth+stencil aspect usable for BOTH sampling AND FBO depth-stencil attachment), substituted at the **SINGLE** scene-FBO depth alloc+attach site (`GameOS/gameos/gos_postprocess.cpp` `createFBOs`, formerly `glTexStorage2D(GL_DEPTH24_STENCIL8)` + `glFramebufferTexture2D(GL_DEPTH_STENCIL_ATTACHMENT)`). **GL-ONLY slice**: GL still renders scene depth into it and the **42 depth-sample consumers** (SSAO / edge-fog / OOB-fog / HZB / DOF / god-rays / cluster-depth-pyramid / particle depth-copy / stencil `glTextureView`) sample it **UNCHANGED** — there is **NO VK depth consumer yet** (that is the P11 Tier-1 slice). Direction = **GL-produces / VK-imports** (inverse of the shadow island). The **P5-T0** (`main_d24s8`) + **depth-test** (`main_d32s8_depthtest`) spikes proved the D32S8-backed image imported under `GL_DEPTH24_STENCIL8` **samples the DEPTH aspect exactly** (0 val errors) and round-trips the `glCopyImageSubData` snapshot + stencil view, so the substitution is transparent. **BORROWS** the shared engine `VulkanContext` (`ensureEngineVulkanContextForInterop`) — no second device. **FAIL-SOFT**: if the shared VK context / interop is unavailable at scene-FBO creation (context not born — e.g. no `MC2_RENDER_BACKEND=vulkan`, LUID mismatch, missing external exts, GL import error), falls back to the **plain GL `GL_DEPTH24_STENCIL8` alloc** and logs the reason → **gate-ON with no interop degrades to the GL path, never crashes**. Rebuilt on **resize** (`destroyFBOs` destroys the VK image + GL view — NOT `glDeleteTextures`, which would double-free the interop tex — and `createFBOs` rebuilds). Surfaced in the `[VK_SHARED_SCENE_DEPTH]` log line (`active tex=… WxH` / `fallback reason=…`). **Gate OFF (or island not compiled) = byte-identical GL scene-depth path.**
- `MC2_VK_PROP_COLOR_TIER1=1` — **presence-gated `=1`, default OFF; REQUIRES `MC2_VK_SHARED_SCENE_DEPTH=1`** (VK-P11 **Tier-1 parity**). Flips the Tier-0 side-image+composite to **IN-PLACE co-write into the SHARED scene GBuffer**. (1) `sceneColorTex_` (MRT0) **and** `sceneNormalTex_` (MRT1) become **GL↔VK interop-shared exportable RGBA16F** images (`GameOS/gameos/vulkan_shared_scene_color.{h,cpp}`, sibling of the shared-depth substrate) at their **SINGLE** scene-FBO alloc+attach sites (`gos_postprocess.cpp createFBOs`), fail-soft to plain GL, resize-safe (owner destroy, not `glDeleteTextures` — double-free guard). (2) The VK prop island (`vulkan_color_prop_island.cpp`, Tier-1 mode) imports the shared color+normal as **LOAD** (preserve GL terrain) color attachments 0+1 and the shared **D32S8 depth as LOAD** depth attachment, depth-tests **GEQUAL reverse-Z** (depth-test+write on, matching GL `StaticPropOpaque`), and draws props **IN-PLACE** emitting **two** outputs — loc0 lit color (matches Tier-0 flat albedo) and loc1 `GBuffer1 = vec4(normalize(v_normal_StuffSpace)*0.5+0.5, 0.25)` **bit-adjacent** to the GL producer `static_prop.frag` (★normal forwarded **un-swapped** Stuff-space via row-vector `a_normal*mat3(M)`; `a=0.25` = static-prop no-self-shadow class). **NO clear** of either color attachment (respects the GL `clearGBuffer1()` that already ran) and **NO composite** — a winning prop fragment fully overwrites the terrain GBuffer pixel (no blend). `objectId` (MRT2) **DEFERRED** (later follow-on behind `MC2_OBJECT_ID_BUFFER`). **Mutually exclusive with** `MC2_VK_PROP_COLOR_INTEROP` (Tier-1 takes precedence; Tier-0 kept intact behind its own gate). **BORROWS** the shared engine `VulkanContext`; **FAIL-SOFT** to the GL `flush()` prop path if the shared GBuffer/context is unavailable → props still draw. Surfaced in the `[VK_PROP_COLOR_INTEROP]`/`[VK_SHARED_SCENE_COLOR]` log lines. **Gate OFF (or the require missing, or island not compiled) = byte-identical GL prop color path.**
- `MC2_VK_PROP_MATERIAL=1` — **presence-gated `=1`, default OFF; REQUIRES `MC2_VK_PROP_COLOR_TIER1=1`.** Replaces the VK prop Tier-1 flat DEBUG teal with **real textured VERTEX-BAKED-lit materials** (NOT PBR — GL props are vertex-baked by default). Copy-once imports **both** static-prop albedo `2D_ARRAY`s (`s_texArrayOff` binding 3 + `s_texArrayOn` binding 6; per-type group-relative layer tables bindings 2/5; RGBA8→`R8G8B8A8_UNORM` or BC7→`BC7_*_BLOCK` per real internalformat, full mips). Alpha-ON (class-1 alpha-MIXED) types route to `s_texArrayOn` with the GL `0.5` alpha-test discard (`static_prop.frag:236`). Frag = `albedo * vArgb`, `vArgb` = per-vertex `a_aRGBLight` base_light seed + stand-in ambient + sun·NdotL. `GBuffer1` unchanged (`a=0.25`). PROVEN mc2_01: teal gone, buildings textured, runway diff 1.22 mean/ch. **DEFERRED vs full parity:** ObjectLights ambient + per-type magic hot-color SSBO + per-actor sun/highlight/fog (stand-in constants → lighting is approximate model, not byte-parity), MaterialGpu roughness/metallic, BC7-bucketed arrays. **SEPARATE parked issue:** Tree/Generic POPULATION coverage (only Building population fed). FAIL-SOFT: array not ready / layer<0 → teal. **Gate OFF = byte-identical.**
- `MC2_VK_PROP_MATERIAL_DEBUG=1` — diagnostic only; one-shot `[GPUPROPS_HIST v2]` instance-weighted layer histogram (textured/alpha_on/no_primary). No render effect.
- `MC2_VK_TERRAIN_DETAIL=1` — **presence-gated `=1`, default OFF; REQUIRES `MC2_VK_TERRAIN_LIGHTING=1`.** Perturbs the VK terrain's flat-up normal before NdotL to close the flat-grass residual: macro-slope normal from the height SSBO (`smoothTerrainNormal`, always on) + per-material weighted detail-normal from `matNormalArray` (9-layer 2D_ARRAY, copy-once import, REPEAT) via the colormap classifier, composed exactly as GL (`pert = (baseN.xy/baseN.z*nfhStrength + dN.xy*detailStrength, 1)`). Suppressed on `pureConcrete`; shadow keeps flat-up N. PROVEN mc2_01: high-angle grass matches GL (signed +0.00); oblique/grazing residual remains (deferred POM/anti-tile/LOD-fade). FAIL-SOFT: detail array not ready → macro-slope only. **Gate OFF = byte-identical.**
- `MC2_VK_MECH_TIER1=1` — **presence-gated `=1`, default OFF; REQUIRES `MC2_VK_SHARED_SCENE_DEPTH=1`** (VK-P14 MECH TIER-1). The **first VK-native SKINNED region**: co-writes the GL mech batcher's **visible mech instances IN-PLACE** into the SHARED scene GBuffer (`GameOS/gameos/vulkan_mech_island.cpp`), reusing the prop Tier-1 shared-GBuffer plumbing (LOAD-preserve MRT0 color + MRT1 normal + shared D32S8 depth, **GEQUAL reverse-Z**, no composite). **Skinning** (inherited from the mech batcher): the shared mech VBO/IBO (`GpuMechVertex` 48B) copied **ONCE per map**; per-frame the **bone palette** (`GpuMechBone` 64B) + **instance SSBO** (`GpuMechInstance` 64B) are **memcpy'd** from a batcher snapshot; the VS (`shaders/vulkan/mech_tier1.vert`) does **linear-blend skinning** over the 4 bone weights (`mech.vert:156-180`; stock weights `(1,0,0,0)` collapse to rigid) then the **Stuff→MC2 axis swap** `worldMC2=vec3(-x,z,y)` (`mech.vert:178`). NO compute. `worldToClipGL` = `gos_GetMechFixBMVPMat4()` (GL_FALSE); `clip.z += OBJECT_DEPTH_BIAS*clip.w`. **SUB-SLICE 1 (this gate)** = skinning + **FLAT color** — the **P-conjugation / axis-swap silhouette proof** (A/B the mech shape vs GL BEFORE material/lighting). `GBuffer1 = vec4(normalize(N_GLworld)*0.5+0.5, 0.0)` bit-adjacent to `mech.frag`'s `rc_gbuffer1_screenShadowEligible` (a=0.0 = screen-shadow-eligible, no in-frag shadow — the GL screen pass does mech shadow). The batcher's `flush()` still **BUILDS the instance/bone SSBOs + feed** but **SKIPS its GL color draws** when the gate is armed; the island co-writes instead. **BORROWS** the shared engine `VulkanContext`; **FAIL-SOFT** to the GL mech flush if the shared GBuffer/context is unavailable (mechs still draw). Feed exported by `gos_mech_batcher.cpp` (`mc2_mech_island_get_geometry_feed` / `mc2_mech_island_get_frame_feed`). Surfaced in `[VK_MECH_ISLAND]`/`[VK_MECH_TIER1]` log lines: `interop_active`, `vk2gl_sem`, `inst_records`, `bone_records`, `runs`, `forbidden_*` (==0). **DEFERRED follow-ons:** `MC2_VK_MECH_MATERIAL` (albedo) + full `calc_light`/LightsData lighting (`MC2_VK_MECH_LIGHTING`, not yet built — needs the whole `ObjectLights` SSBO mirrored to VK). **Gate OFF (or the require missing, or island not compiled) = byte-identical GL mech color path.**
- `MC2_VK_MECH_MATERIAL=1` — **presence-gated `=1`, default OFF; REQUIRES `MC2_VK_MECH_TIER1=1`.** Copy-once imports the **FIRST run's slot-0 paint-scheme** single `GL_TEXTURE_2D` (via `glGetTexImage`→`R8G8B8A8_UNORM`, once per map — a stock actor's paint scheme is static across the mission) and samples it **un-lit** in the mech FS (single-material probe). **DEFERRED (flagged, not built):** the general case = a **per-frame slot-0 handle → VK-descriptor cache** (multiple paint schemes / per-actor per-packet 2D textures resolved live). Lighting is a **separate** follow-on — until `MC2_VK_MECH_LIGHTING` lands the albedo shows flat (NOT byte-parity with the `tex_color * v_litColor` GL mech). FAIL-SOFT: no slot-0 tex / import failure → flat silhouette fill. **Gate OFF = flat color (skinning proof only).**
- `MC2_VK_MECH_LIGHTING=1` — **presence-gated `=1`, default OFF; REQUIRES `MC2_VK_MECH_TIER1=1`** (visible only with `MC2_VK_MECH_MATERIAL=1`). **VK-MECH-MATERIAL-LIGHTING-1.** Ports the **GL DEFAULT mech shading** into the VK mech island: the VS (`shaders/vulkan/mech_tier1.vert`) runs **per-vertex `calc_light` Lambert** (verbatim port of `lighting.hglsl`'s 4-param kernel; `MC2_STATIC_PROP_LIGHTING` `numLights==0`→preserve-base branch, `kAmbientFloor=0.35`, `+aRGBHighlight*a` clamp — matching `mech.vert:204-235` with `u_lightingMode=1`), and the FS does `albedo * v_litColor + highlight`, then per-actor haze `mix(c, fogColor, inst.fogRGB.a)` (`mech.frag:189-393`). The live GL **LightsData SSBO** (binding 20) is **copied per-frame** into a VK SSBO (`glGetBufferSubData` — a buffer copy, not a color readback; `forbidden_*` stay 0) via `gos_LightDataSsbo_GetInterop`; `g_scene.fogColor.rgb` snapshotted at the scene-UBO upload site (`mc2GetSceneFogColorRGB`). **NOT** the PBR/StandardLit-GGX path — that needs `MC2_MECH_VIEWUNIFORMS`+`MC2_STANDARD_LIT_V1` (both default-OFF), so this matches the **default deploy** mech look. FAIL-SOFT: LightsData unavailable → flat-white lit (`v_litColor=1`). **Gate OFF = un-lit albedo (or flat, per material gate).**

## Top-level render backend selector (VK-P13-SELECTOR-1)

- `MC2_RENDER_BACKEND=gl|vulkan` — **the top-level GL-vs-Vulkan backend selector**, default `gl` (unset or unrecognized → `gl`, coercion logged). Read **once** and memoized by `mc2::renderBackend()` (`GameOS/gameos/render_backend_select.{h,cpp}`). **DISTINCT from `MC2_RENDER_BACKEND_IFACE`** (a per-region postprocess-interface toggle in `gos_postprocess.cpp`) — do not conflate. On `=gl`/unset the accessor adds only one memoized `getenv` and every code path is the GL path → **byte-identical GL behavior**. On `=vulkan` the engine additionally **births the standalone shared engine Vulkan device** (`mc2vk::VulkanContext` / `getOrCreateEngineVulkanContext()`, `GameOS/gameos/vulkan_context.{h,cpp}`) at engine init (after GLEW), owning `VkInstance`/`VkPhysicalDevice`/`VkDevice`/graphics queue/VMA + volk device dispatch — the durable vehicle P6/P8/P9/P14 all reuse (the device is **NOT** owned by `VulkanPresentObject`). **Present is still GL swap in P13** (the VK present intercept is the deferred T3 slice); `=vulkan` proves device-birth, not display. **Fail-soft:** any VK bring-up failure (no runtime / bad device / missing exts / validation-layer failure) logs a reason, leaves the device null, and calls `mc2::forceGLFallback()` so `renderBackend()` returns `gl` and every consumer takes the GL path — no crash. **R2 interlock:** when `=vulkan` is active the engine device is born via `volkLoadDevice`, which writes a **process-global** dispatch table; the postprocess-interop islands (`MC2_VK_INTEROP_BRIDGE` / `MC2_VULKAN_POSTPROCESS_SUBGRAPH_INTEROP` / `MC2_VK_SSAO_INTEROP`) also load their own device into that table, so if any of them is on it is **coerced OFF (logged)** before the engine device loads, preventing the dispatch collision. Device-birth proof surfaces in the `[VK_ENGINE_CONTEXT]` health line: `created`, `device_name`, `validation_enabled`, `validation_errors`, `fallback_reason`. GL-only build (no `MC2_VULKAN`) with `=vulkan` → fail-soft to GL.
- `MC2_VK_PRESENT_DIRECT_FBO=1` — **presence-gated `=1`, default OFF** (VK-P14b, copy-1 elimination). Only meaningful with `MC2_RENDER_BACKEND=vulkan` (and the VK present path up). When ON, `gosPostProcess::endScene()` redirects its **single** backbuffer bind (`GameOS/gameos/gos_postprocess.cpp`, the composite-output edge) away from FBO-0 to the FBO wrapping the **shared exportable present image** (`EnginePresentVk`'s `copyFbo`, RGBA8, COLOR_ATTACHMENT0-complete) via the new `EnginePresentVk::presentDirectFbo()` accessor — so the **whole composite + FORCE-43 pillarbox + HUD + ImGui render straight into the shared image**. `presentGLFrame` then **SKIPS copy-1** (the FBO-0→shared blit) entirely and does only interop-signal + copy-2 (VK blit → swapchain + present). The **Y-flip** that copy-1 performed is **moved into copy-2** in the SAME change (dst Y offsets swapped `(0,H)→(H,0)` only on the skip path) so orientation is unchanged (**lockstep** — the two sites agree via the per-frame `directFbo_` flag). **Format/sRGB:** the shared image is `GL_RGBA8` (linear UNORM) and the composite writes gamma-space bytes with `GL_FRAMEBUFFER_SRGB` disabled (LINEAR-COLOR-AUDIT verdict), identical to what copy-1's raw blit produced → **no color shift**. **Fail-soft:** `presentDirectFbo()` returns `0` (endScene falls back to the normal FBO-0 bind + copy-1) whenever the gate is OFF, the present path isn't valid, or the FBO doesn't exist → **no crash**. **Gate OFF (or `=gl`, or GL-only build) = byte-identical FBO-0 + copy-1 path.** Retires the removable half of the present tax (copy-2 stays until VK produces every pixel). Oracle = gate-ON vs gate-OFF present must be visually identical (`verifyReadbackContent()` reads the finished shared image either way + `present_frames`/`readback_changed` counters + same-camera A/B); watch for **upside-down** (Y-flip bug) or **color-shifted** (sRGB bug) frames.
- `MC2_VULKAN_VALIDATION=<preset>` (also documented under the probe seam above) selects the validation preset for the engine `VulkanContext` (`sync` = core + synchronization-validation, used for the `=vulkan` device-birth proof).

## Frame-graph executor + backend-iface seam (RENDER-FRAME-GRAPH ARC)

- `MC2_FRAMEGRAPH_EXECUTOR=1` — top-level frame-graph pass executor (`gos_postprocess.cpp` `executorEnabled()`, `mclib/render_contract.cpp`). Default **OFF**. Value must be `'1'`. Substrate — the reorderer below is NOT shipped; executor stays default-OFF.
- `MC2_FRAMEGRAPH_REORDER_SPMECH` — **REJECTED experiment, default OFF, presence-gated** (`mclib/txmmgr.cpp`). Would swap the StaticProp-flush ↔ Mech-flush order (the one legal adjacent swap the reorder oracle proved). PARITY-FAILED on depth-EQUAL ties (mc2_24 overlapping opaque fragments resolve order-dependently); candidate rejected, kept only as an A/B lever. Never set in production. `=0`/unset = today's order verbatim (byte-identical).
- `MC2_FRAMEGRAPH_DRYRUN=1` — frame-graph dry-run mode (`render_contract.cpp` `dryrunEnabled()`). Default **OFF**. Value must be `'1'`.
- `MC2_FRAMEGRAPH_AMBIENT_GUARD` — ambient-handoff contract guard (colorMask / depthFunc / depthWrite axes; `render_contract.cpp`). **DEFAULT ON**; only `=0` disables. Companion CI check: `scripts/check-ambient-guard.py` (reads the dump counter).
- `MC2_FRAMEGRAPH_AMBIENT_FATAL=1` — hard-`abort()` on an ambient-guard mismatch (CI/bisection teeth). Default **OFF**, presence-gated. Legacy alias `MC2_AMBIENT_ASSERT_FATAL` also honored.
- `MC2_RENDER_BACKEND_IFACE=1` — route the scene-FBO bind through `IRenderBackend` / `GLBackend` instead of direct `glBindFramebuffer` (RENDER-BACKEND-IFACE-FBO-1; `gos_postprocess.cpp`). Default **OFF** = direct GL, byte-identical. ON = same GL call via the backend seam (proves the seam routes; output identical). Value must be `'1'`.
- `MC2_RENDER_BACKEND_REGION_IFACE=1` — MASTER gate (RENDER-BACKEND-REGION-IFACE-1, Layer-6 ENTRY; `gos_postprocess.cpp`). Route the WHOLE PostprocessFog region (EdgeFog + OOB fog) through `IRenderBackend::runRegion()` with a backend-neutral context (`RenderResourceId` + params + dims, NO raw handles). Default **OFF** = the original direct-GL fog sites run, abstraction fully skipped, byte-identical. ON = one selectable region routed through the backend; BOTH original fog sites skipped (no double-apply). Value must be `'1'`.
- `MC2_POSTPROCESS_BACKEND=gl|vk` — sub-selector for the region iface (only meaningful with the master gate `MC2_RENDER_BACKEND_REGION_IFACE=1` ON). `gl` (default, ships) = the extracted GL fog wrapper (`runFogRegionGL`, `runEdgeFog`+`runFogOob` unchanged); byte-identical to the original path (`region_impl=GLInline`). `vk` (opt-in only) = **the TRUE Vulkan opt-in** (POSTPROCESS-VULKAN-BACKEND-OPT-IN): selecting `vk` RUNS the Vulkan subgraph directly — the backend selection IS the enable signal, so it does **NOT** require `MC2_VULKAN_POSTPROCESS_SUBGRAPH=1` (requires `MC2_VULKAN_ISLAND` build only). Fail-soft: on a genuine Vulkan device/init/render failure the region falls back to the GL wrapper (`region_impl=FallbackGL`), no crash, output == GL. On success `region_impl=VulkanSubgraph`, `vk_draws=2`. NO default flip — `gl` ships. The standalone `MC2_VULKAN_POSTPROCESS_SUBGRAPH=1` gate + the per-island gates remain independently runnable for their own testing (both route through the same gate-free readiness half). Surfaced in the health dump as `render_backend_region.backend_region_selected` = `gl|vk|fallback_gl`. **Same selector now also picks the PostprocessSSAO region** (VK-REGION-SSAO-1; `runSSAORegionThroughBackend`), surfaced separately as `render_backend_ssao_region.backend_region_selected`. SSAO only runs at all when `MC2_SSAO=1` (unchanged, still default-OFF) — golden-parity/backend-compare runs against `--region PostprocessSSAO` must additionally force `MC2_SSAO=1` or the region never enters the frame. **And the PostprocessHzb region** (VK-REGION-HZB-1; `runHzbRegionThroughBackend`), surfaced as `render_backend_hzb_region.backend_region_selected`; HZB only runs at all when `MC2_HZB_BUILD=1` (unchanged, still default-OFF) — `--region PostprocessHzb` compares force `MC2_HZB_BUILD=1`. Note the HZB pyramid feeds no visible pass yet (diagnostic substrate): the pixel compare proves no-regression + non-fallback; the math proof is the bit-tight `hzb_fixture` probe token.

## SPFLUSH cost-split decomposition (SPFLUSH-COST-SPLIT-1)

- `MC2_STATIC_PROP_FLUSH_COST_SPLIT=1` — RDTSC cost-split of `StaticPropRegistryFlush`. Default **OFF**. Emits `[SPFLUSH_COST_SPLIT v1] event=summary` every 10 frames with per-bucket ns averages: `submit_loop`, `inst_build`, `map_lookup`, `color_fill`, `actor_record`, `world_to_block`, `substrate_append`, `baseinstance_upload`, plus lifetime + window dirty counters (`invalidates`, `registrations`, `rebuilds`, `light_writes`). TSC calibrated once on first flush (~1ms spin). Zero behavior change.

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

## RenderWorld arc

- `MC2_OBJECT_ID_BUFFER=1` — R32_UINT MRT attachment-2 (M1.5)
- `MC2_RENDER_WORLD_TRACE=1` — verbose RenderWorld events
- `MC2_MECH_OBJECT_ID_SELFTEST=1` — M2.5 validator
- `MC2_MECH_PICK=1` — mech-pick consumer (M2.6). Requires `MC2_OBJECT_ID_BUFFER=1`.
- `MC2_STATIC_PROP_PICK=1` — static-prop pick (M1.6). Requires `MC2_OBJECT_ID_BUFFER=1`.
- `MC2_GAMEPLAY_PICK_SELFTEST=1` — M2-pre spine validator
- `MC2_GPU_PICK_HOVER=1` — GPU hover pick for mechs/dynamic actors (GPU_PICK_HOVER_DYNAMIC-1). Requires `MC2_OBJECT_ID_BUFFER=1`. Only mech kind uses GPU result; static props fall through to CPU.
- `MC2_GPU_PICK_HOVER_TRACE=1` — verbose hover-pick log (hit/miss/fallback per frame + session totals on exit).

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
- `MC2_TERRAIN_SHORELINE=1` — land-side wet/foam shoreline band (TERRAIN-SHORELINE-MASK-1), sampled from an offline-cooked `<mission>.beauty/shoreline_mask.png` sidecar (`tools/terrain_beautify/cook_shoreline.py`). Default **OFF**. Suppresses the legacy screen-space `runShoreline()` post pass when a mask is actually loaded.
- `MC2_TERRAIN_SHORELINE_FILE=<path>` — override the sidecar PNG path (default `data/missions/<stem>.beauty/shoreline_mask.png`).
- `MC2_TERRAIN_SHORELINE_STRENGTH=F` — wet/damp darken intensity multiplier, clamped [0,2]. Default 1.0.
- `MC2_TERRAIN_SHORELINE_FOAM=F` — foam rim intensity multiplier, clamped [0,2]. Default 1.0.

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
