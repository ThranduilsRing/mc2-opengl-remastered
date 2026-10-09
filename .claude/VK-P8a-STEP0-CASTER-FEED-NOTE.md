# VK-P8a STEP 0 — CASTER GEOMETRY FEED DECISION

Worktree `A:/Games/mc2-vk-interop-bridge`, branch `claude/vk-interop-bridge-1`, HEAD `e107d642`.
Scope: P8a = **static-prop dynamic shadow casters only** (terrain + mechs deferred; see below).

## VERDICT: FEED IS TRACTABLE — NOT A WALL. Proceed to STEP 1.

The static-prop caster feed is the cleanest possible geometry region to port. It is already a
**CPU-resident POD instance vector + a single consolidated immutable GPU vertex/index buffer**, and
the shadow VS reads only `a_position` + `modelMatrix` + one uniform matrix. No new per-frame CPU→GPU
marshalling machinery has to be invented — the marshalling the recon feared (R1) **already exists** in
the GL path and produces exactly the buffer shape VK's instanced depth pipeline wants.

## The live feed (traced end-to-end)

Driver: `mclib/txmmgr.cpp` `MC_TextureManager::renderLists()`, dynamic shadow bracket
`gos_BeginDynamicShadowPass()` (txmmgr.cpp:2733) … `gos_EndDynamicShadowPass()` (txmmgr.cpp:2902).

Between the bracket, three caster sources draw (default-ON gate `MC2_SHADOW_DYNAMIC_PROP_CASTERS`):
1. **Static props** — `GpuStaticPropBatcher::instance().drawDynamicPropShadows(*dynShadowSet)`
   (txmmgr.cpp:2895 → body `gos_static_prop_batcher.cpp:8089`). **← P8a target.**
2. **Mechs** — `GpuMechBatcher::instance().flushShadow()` (txmmgr.cpp:2901). **Deferred to P12** (VK
   GPU skinning) per the build brief. Out of P8a scope.
3. **Tessellated terrain** — `gos_DrawShadowBatchTessellated` from `mclib/mapdata.cpp:1333/1349`,
   CPU-built `s_vbuf`/`s_ebuf` arrays through a TES. **Deferred to P8b** (needs the tessellation
   pipeline reproduced in VK — heavier). Out of P8a scope.

### The static-prop caster instance set — `dynShadowSet`
- Type: `const std::vector<GpuStaticPropInstance>*` (txmmgr.cpp:2821, built at 2758–2888).
- Built by `GpuStaticPropRegistry::getDynamicPropShadowInstances(s_dynPropInsts, includeBldg)`
  (txmmgr.cpp:2790), then lightbox-culled into `s_culledDynPropInsts` (txmmgr.cpp:2822–2888,
  gate `MC2_SHADOW_CASTER_LIGHTBOX_CULL` default-ON).
- **REGISTRY-STATIC**: rebuilt only when `GpuStaticPropRegistry::getRegistryGeneration()` changes
  (a prop spawns/despawns/immutable-field write) — dirty-only cache at txmmgr.cpp:2787. On a clean
  scene it is a stable CPU vector reused across frames. The shadow pass reads **only**
  `modelMatrix + typeID` from it (no lighting/color) — explicitly noted safe-to-cache
  (txmmgr.cpp:2764–2793).

### `GpuStaticPropInstance` (POD, 112 B, `alignas(16)`, static_assert'd — batcher.h:27)
```
float    modelMatrix[16];  // off 0   shape-to-world, row-major (uploaded GL_FALSE)
uint32_t typeID;           // off 64
uint32_t firstColorOffset; // off 68
uint32_t flags;            // off 72
uint32_t lightDataIndex;   // off 76
vec4     aRGBHighlight;    // off 80  (unused by shadow)
vec4     fogRGB;           // off 96  (unused by shadow)
```
The shadow VS only consumes `modelMatrix` (+ `typeID` used CPU-side to pick the geometry range).

### Geometry is GPU-resident and VK-bindable in shape
- **One shared immutable VBO + IBO for ALL prop types**: `s_sharedVbo`/`s_sharedIbo`, built once per
  map with `glBufferStorage(..., flags=0)` (fully immutable GPU-only) — `gos_static_prop_batcher.cpp:2454-2466`.
- Per-type geometry is addressed by `s_packets[].baseVertex` / `.firstIndex` / `.indexCount`
  (batcher.cpp:8199-8221). Vertex stride = `kVertexStride` (position at loc 0, offset 0, vec3 float).
- Draw is `glDrawElementsInstancedBaseVertex(GL_TRIANGLES, indexCount, UINT, firstIndex*4, instCount,
  baseVertex)` per type run (batcher.cpp:8217), instances sorted by `typeID` and the SSBO range
  selected via a `u_instBase` uniform (whole buffer bound at binding 0, offset 0).

### Shadow VS math to reproduce EXACTLY in VK (shaders/shadow_static_prop.vert)
```
worldStuff = vec4(a_position,1) * modelMatrix;          // row-vector * M (Stuff col-major)
worldMC2   = vec3(-worldStuff.x, worldStuff.z, worldStuff.y);   // Stuff -> MC2 swizzle
gl_Position = lightSpaceMatrix * vec4(worldMC2, 1);
```
`lightSpaceMatrix = pp->getDynamicLightSpaceMatrix()` uploaded `GL_FALSE` (column-major direct),
batcher.cpp:8165. Forward-Z shadow: `glClearDepth(1.0)`, `GL_LESS` (gameos_graphics.cpp dynamic pass),
`[0,1]` z. Contact-acne bias via `glPolygonOffset(pp->shadowBiasFactor_, pp->shadowBiasUnits_)`
(batcher.cpp:8181) — **VK must replicate depthBias factor/units EXACTLY** (this is the STOP-watch).

## FEED DECISION for P8a (simplest working feed)

**Chosen: VK re-runs the static-prop caster draw from the existing CPU instance vector + the existing
GPU-resident shared VBO/IBO.** Concretely, when `MC2_VK_SHADOW_INTEROP` is ON:
- **Instances**: stage the same `*dynShadowSet` (`GpuStaticPropInstance` POD vector, already CPU-side,
  already culled/sorted-ready) into the VK instance ring the shadow island pipeline expects
  (`STORAGE_BUFFER_DYNAMIC`, set1). This is a straight `memcpy` of a POD vector after the GL fence —
  no new extraction machinery; the vector is already built by the GL path we tap.
- **Geometry**: two viable sub-options for the shared VBO/IBO —
  - **(A, preferred) VK imports the GL `s_sharedVbo`/`s_sharedIbo`** via the same external-memory path
    the shadow image uses, and binds them as VK vertex/index buffers (they are `glBufferStorage`
    immutable, GPU-only, single consolidated buffers — ideal for import). Zero re-upload.
  - **(B, fallback) VK owns a copy**: on map load, stage `s_stagingVbo`/`s_stagingIbo` (the CPU staging
    arrays that build the shared buffers, batcher.cpp:2457/2465) into VK-owned immutable buffers once.
    Per-map, not per-frame. Chosen only if GL-buffer import proves fiddly.
- **Per-type draw loop**: reproduce the `typeID`-run loop (sort instances by typeID, per run issue an
  instanced indexed draw with `baseVertex`/`firstIndex`/`indexCount` from `s_packets`). `u_instBase`
  becomes a push-constant / dynamic SSBO offset. P8a can start **opaque-only** (program
  `shadow_static_prop` = empty frag, depth-only) — skip the foliage alpha-test path
  (`shadow_static_prop_alpha`) initially to keep the first slice binary; widen in P8b.

**Rationale**: the recon's R1 ("brand-new per-frame CPU→GPU caster-instance marshalling") is already
solved by the engine — `dynShadowSet` IS the marshalled instance set, produced every frame (or cached).
We reuse it verbatim. The geometry is a single immutable GPU buffer, the cleanest possible import
target. The only genuinely new code is the VK-side draw loop + the instance-ring stage, both small.

## Residual risks carried into STEP 1 (none are walls)
- **R2 (compare-mode sampling over imported D32): RESOLVED.** The sampler probe
  (`tools/vk_shadow_sampler_probe`, HEAD `e107d642`) proved hardware `sampler2DShadow`
  `GL_COMPARE_REF_TO_TEXTURE`/`GL_LEQUAL` over imported D32 works, NEAREST + LINEAR PCF, 0 VK
  validation errors, 0 GL errors. VERDICT in `P8-shadow-sampler-probe-result.log`: HARDWARE-COMPARE VIABLE.
- **Bias parity (the STOP-watch)**: VK must set depthBias to the GL polygonOffset factor/units exactly,
  and match forward-Z `[0,1]` + `GL_LESS`. Any acne/peter-pan divergence → surface numbers, no silent floor.
- **Format**: GL dynamic shadow tex today is `GL_DEPTH_COMPONENT24`; the shared VK image is D32 /
  `GL_DEPTH_COMPONENT32F` on BOTH sides. GL consumer samples the 32F shared texture in place of
  `getDynamicShadowTexture()`. `sampler2DShadow` compare is filtering-mode, format-agnostic (proven).
- **vk2gl back-channel** (`glVkToGlSem`/`glInteropWaitFromVk`): first live geometry-region user. Validate
  signal/wait ordering under `MC2_VULKAN_VALIDATION=sync`.
- **CSM**: single-map only for P8a (the GL dynamic map is single unless CSM gate on). 3-cascade array = P8b.

## Deferred (explicitly OUT of P8a)
- **Terrain casters** (tessellated, mapdata.cpp) → P8b: needs VK tessellation pipeline.
- **Mech casters** (GpuMechBatcher::flushShadow) → P12: needs VK GPU skinning.
- **Foliage alpha-test prop shadows** → P8b widen (start opaque-only).

---
## POST-BUILD CORRECTIONS + PARITY OUTCOME (appended after implementation)

### Geometry-feed correction (STEP 0 assumption refined during build)
STEP 0 assumed the CPU staging vectors (`s_stagingVbo`/`s_stagingIbo`) persist after finalize.
They do NOT — `gos_static_prop_batcher.cpp:~2532` does `s_stagingVbo.clear(); shrink_to_fit()`
immediately after the immutable GL VBO/IBO are created (`glBufferStorage`). So the geometry lives
ONLY in the GL buffers post-finalize. The feed accessor (`mc2_shadow_island_get_geometry_feed`) was
changed to hand out the **GL buffer handles + captured byte sizes** (sizes snapshotted at finalize
into `s_sharedVboBytes`/`s_sharedIboBytes` BEFORE the clear); the island pulls the bytes ONCE per map
via `glGetBufferSubData` into VK-owned buffers. This is a one-time map-load copy, NOT the per-frame
shadow-map surface, so it does not touch the zero-copy shadow-map invariant (forbidden counters stay 0).
This is P8a feed option (B) realized via GL-buffer readback rather than CPU-staging memcpy.

### Build bug found + fixed (the crash)
First gate-ON run crashed (STATUS_STACK_BUFFER_OVERRUN, 0xC0000409) inside `vkCreateGraphicsPipelines`.
Root cause: the VK VS `shadow_prop.vert` declared the instance SSBO at `set=1 binding=0` while the C++
descriptor/pipeline layout used a SINGLE set (`set=0` binding0 UBO + binding1 SSBO) — a shader/layout
interface mismatch the AMD driver hard-crashed on. Fixed the shader to `set=0 binding=1`. After the fix
the gate-ON run is fully clean.

### Engagement (PASS — all criteria met, mc2_01, MC2_RENDER_BACKEND=vulkan MC2_VK_SHADOW_INTEROP=1
### MC2_SMOKE_FIXED_TIMESTEP=1, 25–30s foreground)
`vulkan_available=1 used_vulkan=3455..6293 used_interop=3455..6293 interop_active=1 vk2gl_sem=1`
`validation_errors=0 forbidden_copyImgToBuf=0 forbidden_getTexImage=0 forbidden_readPixels=0`
`inst_records=180..182 type_runs=45..47 geom_verts=23628 draws=49 fallback_reason="" vk_draws=1`
`draw_us≈1300–1470 stage_inst_us≈1.3`. Smoke PASSES 3456+ frames, no crash. The VK shadow island
produces the D32 caster-depth map every frame; GL binds it via getDynamicShadowTexture() and samples
through the UNCHANGED hardware calcShadow path. First-use of the vk2gl back-channel on a geometry
region CONFIRMED live (vk2gl_sem=1).

### Golden-parity (BLOCKED — capture-oracle degeneracy on the ON side, NOT a shadow divergence)
`run_golden_parity.py mc2_01 MC2_VK_SHADOW_INTEROP --gate-value 1 --fixed-timestep` result = FAIL, but
the failure mode is a CAPTURE-HARNESS artifact, not a shadow-shading difference:
- OFF side (gate off): 11 bookmark PNGs, all **DISTINCT** shas (healthy per-pose capture).
- ON side (gate on): 11 bookmark PNGs, **ALL BYTE-IDENTICAL** (single sha 35b51b11 across every
  bookmark). Every bookmark captured the SAME frame — the bookmark camera poses did not retarget
  between captures on the ON run. The single ON frame is a fully-rendered scene (60996 unique colors,
  not blank), but it is the same pose 11×.
- The ON run's shadow health was perfect throughout the capture (used_interop=6293, validation=0,
  forbidden=0) — so the island itself is healthy; the degeneracy is in the visual-capture camera-
  application path under the gate (likely the per-frame `vkWaitForFences` stall or the interop
  acquire/wait interacting with the capture harness's bookmark-pose driver / frame pacing).
- Because all ON captures are one identical pose, the pixel oracle cannot yield a real shadow parity
  number. This is the STOP condition per the brief (no silent floor). NOT a bias/acne divergence —
  those were never reachable because the oracle didn't produce comparable per-pose frames.

### NEXT to unblock parity (coordinator call)
Options: (a) diagnose why the bookmark camera driver doesn't retarget on the ON run (frame-pacing /
fence-stall interaction with run_visual_capture) and fix, then re-run; (b) add a dedicated shadow-
visible bookmark + a lighter-weight single-pose readback oracle that doesn't depend on the multi-pose
fly; (c) drop the per-frame fence WAW stall to a lighter sync so the capture pacing matches OFF. The
scope caveat still stands: the VK map is static-prop casters only (mech = P12, terrain = P8b), so the
fair oracle is a prop-only shadow scene.

---
## RE-WIRE + SCENE-PIXEL PARITY (2026-07-04, MC2_VK_SHADOW_INTEROP=1 ALONE — PASS)

### The re-wire (the missing producer call, restored)
The WIP branch had everything EXCEPT the per-frame producer call (txmmgr.cpp was reverted
clean). Restored ONLY the producer-side integration in `mclib/txmmgr.cpp`:
- Guarded include `vulkan_shadow_island_live.h` (after the batcher include, `#ifdef MC2_VULKAN_ISLAND`).
- Per-frame producer call inside the dynamic-shadow bracket, RIGHT AFTER the GL caster draw
  (`drawDynamicPropShadows(*dynShadowSet)`, ~txmmgr.cpp:2909): when `mc2_vk_shadow_island_enabled()`,
  call `mc2_vk_shadow_island_run(size, lightSpace16, dynShadowSet->data(), size, biasFactor, biasUnits)`
  on the SAME `dynShadowSet`, then `ppVk->setDynamicShadowInteropTexture(vkTex)` (0 on fail-soft).
- A stale-tex guard at the top of the bracket (`gos_BeginDynamicShadowPass`): clear the interop
  tex to 0 each frame so a declined/flushShadow-branch frame never leaves a stale VK tex bound.
- All inputs from `gosPostProcess` accessors (getDynamicShadowMapSize / getDynamicLightSpaceMatrix /
  getShadowBiasFactor / getShadowBiasUnits — all already present on the WIP branch).

### Proof invocation (per the brief — GATE ALONE, not MC2_RENDER_BACKEND=vulkan)
`MC2_VK_SHADOW_INTEROP=1 MC2_SMOKE_FIXED_TIMESTEP=1` on the on-demand shared-device path
(`ensureEngineVulkanContextForInterop()`, wantInterop=true/wantPresent=false), GL present.
Foreground `run_visual_capture.py mc2_01` (11 bookmarks), build64_island, deploy mc2-win64-v0.4c.
★This path does NOT trigger the prior capture-oracle degeneracy — all 11 bookmarks retarget
and produce DISTINCT deterministic shas on the ON side (the `MC2_RENDER_BACKEND=vulkan`
minimized-fail-soft degeneracy that blocked the earlier attempt is GONE).

### Island is LIVE (per-frame [VK_SHADOW_ISLAND] log, ON run)
`depth interop seam ACTIVE (vk2gl_sem=1); island initialized 8192x8192 (borrowed shared device,
D32 depth-only LESS forward-Z, depthBias dynamic); caster geometry uploaded once vbo=945120 B
ibo=94512 B stride=40 packets=226 types=219; first VK shadow map produced instCount=811
typeRuns=48 tex=262 (GL samples the VK-produced D32 via calcShadow).` validation_errors=0,
0 forbidden DEPTH-readback, 0 GL errors, no fallback/disable.

### SCENE-PIXEL ORACLE (the P8a scar) — PASS
- Scene NOT black: 11 DISTINCT deterministic bookmark shas on the ON side (fully-rendered scene,
  terrain/props/sky present) — the mandatory not-black + not-degenerate check.
- OFF-vs-ON SCENE parity (fixed timestep): **10/11 bookmarks BYTE-IDENTICAL**, including both
  shadow-bearing views — `ridge_lowangle` (shadow_cascade) c836122619637700 and `highangle_wide`
  (static_prop_pbr, shadow-RECEIVING prop pixels) 9f068e5c2063d5cf. Byte-exact = zero shadow
  divergence (no bias/acne/peter-pan floor needed; better than the water ±1-LSB precedent).
- CONSUMPTION PROVEN (throwaway gate `MC2_VK_SHADOW_INTEROP_PROVE (gate since removed)`, since removed): with the GL
  caster draw SKIPPED so GL's own dynamic map = cleared depth=1.0 (no shadows), the two
  shadow-bearing bookmarks STILL rendered byte-identical to the GL baseline — the shadows come
  ENTIRELY from the VK-produced D32. GL provably CONSUMES tex 262 at the three forward bind sites
  (gameos_graphics.cpp:6968/9526/10261, all `getDynamicShadowTexture()` → returns the interop tex).

### The one non-matching bookmark (`cross_map_view`) = HARNESS FLAKINESS, NOT a shadow divergence
`cross_map_view` yields FOUR different shas across FOUR runs, THREE of which are pure gate-OFF
runs my code cannot touch (off=518c…, ship_off=676c…, off2=94e2…). It is inherently
non-deterministic run-to-run on this wide pose (bookmark tags `invviewproj_transpose_repro` +
edge_fog/oob_fog/water) regardless of the VK gate. Classified: capture-harness non-determinism
on one bookmark, isolated from the shadow path. The 10 stable bookmarks (incl. both shadow views)
are the fair oracle and all pass byte-exact.

### Gate-OFF byte-identical — HOLDS
All additions are `#ifdef MC2_VULKAN_ISLAND` + `if (mc2_vk_shadow_island_enabled())`; with
`MC2_VK_SHADOW_INTEROP` unset, `enabled()`==0 → zero new GL work, interop tex stays 0 →
`getDynamicShadowTexture()` returns the GL tex. OFF runs are healthy/deterministic on the 10
stable bookmarks; the OFF path is structurally untouched.

### Producer-direction sync rule — VERIFIED HOLDS
`mc2_vk_shadow_island_run`: NO GL→VK acquire (VK never reads a GL-written depth; the harmful
mid-frame glSignalSemaphoreEXT that blacked the scene is absent). VK produces D32 → signals vk2gl
→ `glInteropWaitFromVk` before GL samples → per-slot WAW covered by the frame fence. Scene is
NOT black (11 distinct rendered bookmarks) — the blackout regression did not recur.

### VERDICT: PASS. Scope = static-prop casters only (mech P12, terrain P8b deferred, as designed).
Ready to merge WIP→arc. The throwaway MC2_VK_SHADOW_INTEROP_PROVE consumption-proof gate was
removed from the shipped diff; the final txmmgr.cpp diff is the producer call + stale-tex clear only.
