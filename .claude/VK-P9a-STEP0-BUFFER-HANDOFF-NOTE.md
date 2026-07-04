# VK-P9a STEP 0 — GL→VK water-buffer hand-off decision

Worktree `A:/Games/mc2-vk-interop-bridge`, branch `claude/vk-interop-bridge-1` @ `19cf4fb8+`.
Resolves the recon's "biggest unknown" (VK-P9-WATER-RECON.md §7, R2) BEFORE building the full P9a pipeline.

## The question

The GL water path packs its per-frame geometry feed into 4 GPU buffers each frame
(WATER-REGISTRY-0, `83da3973`):

| Buffer | Global | id | binding | contents | lifetime |
|---|---|---|---|---|---|
| Recipe | `g_recipeBuffer` | 44 | 5 | `WaterRecipe[]` (64 B/rec): per-quad world XY, elevations, terrainTypes, waterBits, flags | **mission-static** (uploaded once, `EnsureRecipeBufferUploaded`) |
| Thin ring | `g_thinBuffer` | 43 | 6 | `WaterThinRecord[]` (48 B/rec): recipeIdx + per-frame lightRGB/fogRGB/pzValid | **per-frame, compute-packed, ring-3, fenced** |
| Bucket header | `g_waterBucketHeaderSsbo` | 45 | (compute 6) | 16 B visibleCount | per-frame |
| Indirect cmd | `g_waterIndirectCmdBuffer` | 46 | DRAW_INDIRECT | 2× `DrawArraysIndirectCommand` (32 B), `.count` patched by `gpu_driven_cmd_patch.comp` | per-frame |

For VK to draw the water mesh it must read the SAME data. How does VK get it?

## Option (a) — GL↔VK external buffer sharing — REJECTED (not viable)

`GL_EXT_memory_object` buffer import / `VK_KHR_external_memory` **requires the buffer's
backing memory to be allocated as external/imported at CREATION time.** These 4 buffers
are plain `glGenBuffers` + `glBufferData`/`MC2_GL_BufferData` (verified:
`gos_terrain_water_stream.cpp:532,538,760,1497,1441`). You cannot retroactively re-back an
already-allocated GL buffer's storage with imported external memory — this is the exact
same class as the FBO-0 problem the roadmap flagged (P5). The interop import helpers in the
tree (`gl_interop_view.cpp`: `glCreateMemoryObjectsEXT`/`glImportMemoryWin32*`) are all the
**VK-produces → GL-imports** direction (the output seam), not GL-buffer → VK-import. **No
GL-owned-buffer → VK-import path exists or is retrofittable here.** Expected per recon; confirmed.

## Option (b) — VK-side copy / re-pack — CHOSEN

The proven SSAO island already establishes the pattern for **every GL→VK input**: there is
NO zero-copy import of GL-owned resources. The island CPU-snapshots each GL input
(`glBindTexture(sceneColorTex_)` → scratch → `memcpy` into a VMA staging buffer via
`vmaMapMemory`, `vulkan_ssao_island.cpp:1297,1394-1403`) and only the **VK-produced output**
(`VkImageResource`) is imported into GL zero-copy (`glInteropViewCreate(...GL_RGBA16F)`,
`:707`). So the P9a rule is the island rule: **GL inputs are CPU-bridged into VK; the VK
color output is zero-copy-shared back to GL.**

**Chosen hand-off — two tiers, both parity-safe:**

1. **Recipe (mission-static): copy once → VK buffer at mission load.** `WaterRecipe[]` never
   changes after `Build()`. Read it from the CPU-side `WaterStream::GetRecipes()` /
   `GetRecipeCount()` (already public) and upload to a VK device-local buffer once. No
   per-frame cost, no race.

2. **Thin records (per-frame): copy the CPU staging array into a VK ring buffer each frame,
   AFTER GL's fence.** The thin records are packed by GL compute (`gpu_driven_water.comp`)
   into `g_thinBuffer`, OR on the legacy path from the CPU staging vector `g_thinStaging`.
   Rather than fight GL→VK buffer visibility for a compute-written GL SSBO, **P9a re-packs
   from the CPU side**: the same `g_thinStaging` array (or a CPU pack from recipes) is the
   source of truth. This eliminates the sync hazard entirely — there is no GL-GPU→VK-GPU
   buffer read across the seam; the copy is CPU-staged (`memcpy` → `vmaMapMemory`), exactly
   like the island's inputs. The indirect draw count is just the thin-record count (an int),
   so VK issues a plain `vkCmdDraw`/instanced draw — **no shared indirect-cmd buffer needed
   for P9a** (the GL `g_waterIndirectCmdBuffer` is a GL-only artifact).

   **Sync resolution (R2):** because the copy is CPU-staged, the GL ring-3 fence
   (`WATER-THINRING-FENCE-1`, id 43) is irrelevant to VK — VK never touches the GL buffer.
   The CPU array is stable by the time the water pass runs (renderLists has drained,
   `simplecamera.cpp:255-261`). No GL→VK GPU hazard exists to close. **The biggest unknown
   dissolves: we do not hand a fenced GL GPU buffer to a VK draw at all.**

### Why re-pack from CPU, not port the GL compute (P9a scope)

The recon's P9a explicitly keeps the GL compute as a producer and defers porting it (§2,
§7 P9c). The CPU thin-staging array is already built every frame on the parity/legacy path
and is the exact same data the compute writes (proven byte-identical by
`ComputeDispatchParity_Check`, `MC2_GPU_DRIVEN_PARITY`). Sourcing P9a from CPU staging is
the smallest working feed, is inherently hazard-free across the seam, and keeps the geometry
identical to GL. Porting the cull/pack compute into VK is P9c.

### Cost

An extra CPU→VK copy per frame for the thin records (small: hundreds–few-thousand × 48 B,
sub-100 KB typical) + a one-time recipe upload. This is the same "extra copy, not zero-copy"
shape as P13's FBO-0→shared-texture copy — cheap, parity-exact. The ONLY zero-copy surface
in P9a is the RGBA16F color output (already proven transport-transparent by
`tools/vk_interop_transport_test`, RGBA16F instantiation).

## STEP 0 addendum 2 — DEPTH/MRT PIPELINE SPEC (tree-authoritative, LOCKED)

Source of truth = THIS worktree's `RenderCore/PipelineRegistry.cpp:227-249` (WaterArmed row) +
`gameos_graphics.cpp:3424-3432` + `gameosmain.cpp` glClipControl. NOTE: an expert consult read the
ROOT `mc2-opengl-src` tree, which has DIVERGED from this bridge worktree on water depth-write — its
"depth-write OFF / GBuffer1 inert" answers are WRONG for this build. Bridge-tree truth:

- **depthTestEnable = true**, **depthFunc = GreaterEqual** (reverse-Z GEQUAL). VK:
  `depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL`, viewport minDepth 0 / maxDepth 1, clear depth 0.0.
- **depthWriteEnable = TRUE** (default armed; OOB-FOG-WATER-DEPTH-1; only `MC2_WATER_NO_DEPTH_WRITE`
  flips off). VK: `depthWriteEnable = VK_TRUE`. (Root tree = FALSE; bridge tree = TRUE. Use TRUE.)
- **cullMode = None**; **blend = AlphaBlend** SRC_ALPHA/ONE_MINUS_SRC_ALPHA; **polygonOffset = false**
  (`depthBiasEnable = VK_FALSE`). The ONLY depth bias is the in-VS `clip.z += WATER_DEPTH_FUDGE_FAST*
  clip.w` — byte-copied VS gets it free; do NOT re-derive C++-side.
- **colorAttachments = { true, true, false } → MRT**: water writes BOTH color0 (MainColor) AND
  color1 (GBuffer1 = `rc_gbuffer1_screenShadowEligible` = vec4(0.5,0.5,1.0,0.0)). For strict parity the
  VK pass should produce BOTH. P9a MAY start color0-only (GBuffer1 loses water screen-shadow
  eligibility — low-risk, low-contrast over lakes) but must reproduce GBuffer1 for full parity.
- **reverse-Z**: `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)` + `glClearDepth(0.0)` in gameosmain
  (game path only; EditRel is forward-Z — irrelevant here). VK is natively [0,1]; matches.

### THE HARD MUST-HAVE (else automatic STOP): scene-depth read-in

The depth-TEST against scene depth is LOAD-BEARING at the shoreline: without it, water fragments
draw over above-water land = the v0.3 tile-aligned shoreline STAIRCASE regression (documented at
gameos_graphics.cpp shoreline-fix comment). The VS pz-cull does NOT restrict geometry to submerged
tiles; per-pixel occlusion by higher terrain is done by the depth TEST. So **P9a MUST import GL scene
depth into the VK water pass as a read-only depth attachment** (LOAD, GEQUAL, write per above, no
export-back). Depthless color-only water = STOP. This is the biggest remaining build item beyond the
color seam — scene depth is GL_DEPTH24_STENCIL8 (P5: not externally shareable), so it is CPU-staged
IN like SSAO's depth input (glGetTexImage(DEPTH) → VMA staging → depth image), the import direction
(no D24S8 EXPORT blocker). Depth-write ON means water writes into this VK depth image; since we do NOT
export depth back, the write is local to the VK pass (used only for intra-water ordering + OOB-fog on
the GL side is moot because GL re-composites color only) — VERIFY whether losing the depth-write-back
to GL regresses OOB-fog-over-water (OOB-FOG-WATER-DEPTH-1). If it does, that's a P9a parity caveat to
surface. (Watch item, not yet a proven STOP.)

### Parity canary discipline
`[WATER_DEPTHPROBE v2]` FNV-hashes only the cull-feed MVP — BLIND to depth-fudge/render-VS/depth-state
bugs ("equal" can coexist with a wrong shoreline). Do NOT use it as the VK canary. Use pixel-diff
golden-parity + in-engine visual on a water-visible bookmark. Capped FPS = dead A/B metric.

## STEP 0 addendum — INTEGRATION SURFACE (discovered while grounding the build)

The recon framed the seam as "the SSAO island shape + a mesh draw." True for the *transport*,
but the **integration surface is materially different**, and this is the real size of the lift:

- All 4 proven VK regions (fog, SSAO, HZB, composite-present) run through
  `IRenderBackend::runRegion(RenderRegionId, ctx, out)` invoked **from `gos_postprocess.cpp`**.
  They are **post-process, full-screen, scene-buffer** regions with a GL fallback impl each
  (`gos_postprocess.cpp:2948,3109,3312,3519`). The island `.cpp` files expose the VK impl;
  `runRegion` picks GL-vs-VK by `MC2_POSTPROCESS_BACKEND`.
- **Water is NOT a post-process region.** It is a **mid-frame geometry pass** inside
  `gosRenderer::renderWaterFastPath` (`gameos_graphics.cpp:3181`), executed from
  `renderWater`/`simplecamera.cpp:261`, AFTER renderLists drains but BEFORE post-process. It
  draws a procedural SSBO-driven mesh (VS `gos_terrain_water_fast_mdi.vert` derives geometry
  from `gl_VertexID`/`gl_DrawIDARB` + recipe/thin SSBOs — no vertex buffer) via
  `glMultiDrawArraysIndirect`, alpha-blended into MainColor with reverse-Z GEQUAL + depth-write.

**Consequence:** a water VK region does NOT slot into the existing `runRegion` post-process
dispatcher unchanged. P9a needs EITHER (i) a new mid-frame region entry (a `renderWaterFastPath`
seam that calls a VK water island and composites its RGBA16F output into MainColor, parallel to
but separate from the postprocess `runRegion` path), or (ii) a standalone island invoked
directly from `renderWaterFastPath` (SSAO-island-style extern entry, not via runRegion). Option
(ii) is smaller and matches the recon's "standalone vulkan_water_island" language. This is a
genuine new integration seam in the geometry path, not a clone of an existing postprocess region.

The VK water pipeline itself must reproduce the procedural VS bit-exactly (recipe/thin SSBO
layout, the 6-verts-per-record expansion, `u_worldToClipGL * worldPos` with
`clip.z += WATER_DEPTH_FUDGE_FAST*clip.w`, WATER-MVP-ORACLE-1 column_major/GL_FALSE) AND the FS
(`gos_terrain_water_mdi.frag`). Both need SPIR-V variants. Reflection/HDRI OFF for P9a trims the
FS inputs to base/detail textures.

## Net STEP 0 verdict

- Buffer hand-off = **VK-side copy/re-pack (option b)**. Recipe copied once at load; thin
  records CPU-staged per frame; no shared GL buffer crosses the seam; no indirect-cmd buffer
  shared. **The R2 sync unknown is resolved by construction** (nothing fenced is handed off).
- This is NOT a wall. The feed is straightforward and parity-safe. Proceed to build P9a.
- Color output seam (the one zero-copy surface) is the proven SSAO RGBA16F seam; transport
  harness already green for RGBA16F.
