# Render Binding Registry

> ## ⚠ SUPERSEDED FOR BUFFER BINDINGS (corrected 2026-06-22 RENDER-CONTRACT-INDEX-1)
>
> **For UBO / SSBO base-binding occupancy, DO NOT TRUST this doc.** The
> authoritative source is now:
> - `docs/render-backend-seams/binding-slot-occupancy.md`
> - `docs/render-backend-seams/binding-slot-occupancy.json`
> - enforced by `scripts/check-binding-slots.py` (registered in `scripts/check-contracts.sh`)
>
> Those artifacts encode the **multiplexed-per-pass** model proven by
> GPU-BINDING-SLOTS-LOCKSTEP-1: a slot number is semantic ONLY inside a
> pass/pipeline (slot 0 = 7 buffers, slot 2 = 6+, slot 7 = 4 across
> mech/static-prop/cull/terrain/particle/gpu-driven). The flat-table model below
> can encode a FALSE "one slot = one owner" picture. The tables here are retained
> for historical / cross-reference value only — verify any buffer-binding claim
> against the occupancy doc before acting on it.
>
> See also `docs/render-backend-seams/render-contract-index-1.md`.

Inventory of runtime OpenGL binding-point conventions across the MC2 renderer.
Snapshot of HEAD `2b5024c9` on branch `claude/nifty-mendeleev`. Generated for
binding-conflict prevention; this is an **audit/lookup doc**, not an
authority/spec file.

## Intent

- Prevent silent binding-number collisions when adding new UBO/SSBO/image
  consumers or compute passes.
- Provide an audit trail: every claim cites a `file:line` so a reader can
  re-derive the assignment from the source.
- Surface known multi-binder slots (legitimate reuse) vs accidental clashes.

This doc is **NOT**:
- A feature spec for any subsystem (each subsystem owns its own design doc).
- A single source of truth at the C++ level — the named constants in
  `RenderCore/ViewUniforms.h`, `gpu_cull_*.h`, `gameos.hpp`, etc. are
  authoritative. This file mirrors them for cross-system visibility.

## How to use this doc

- **Adding a new binding:** in the same commit that introduces the new
  `glBindBufferBase` / `layout(binding=N)`, add a row to the matching table
  here.
- **Changing an existing binding:** update the row (number, owner, or cite)
  in the same commit. Stale rows defeat the purpose.
- **Reviewing a change:** if a PR touches `glBindBufferBase` or
  `layout(binding=N)` and does not update this doc, request the update.
- **GL namespace rule:** UBO bindings and SSBO bindings live in *separate*
  namespaces in GL 4.3+, so `UBO binding=3` and `SSBO binding=3` do not
  collide. Image-unit, texture-unit, and atomic-counter bindings are also
  independent namespaces. The tables below are partitioned accordingly.

---

## Runtime UBO bindings

| Binding | Name                 | Owner subsystem            | Type   | C++ bind site                                                        | Shader consumer(s)                                                                 | Notes |
|--------:|----------------------|----------------------------|--------|----------------------------------------------------------------------|------------------------------------------------------------------------------------|-------|
| 0       | `LIGHT_DATA_ATTACHMENT_SLOT` (legacy UBO slot — superseded) | gameos lighting             | UBO (legacy) | `GameOS/include/gameos.hpp:2823`                                  | superseded — see SSBO binding 20 below                                              | `LIGHTSSBO v1` migrated LightsData off UBO 0 to SSBO 20 (`GameOS/include/gameos.hpp:2824`). UBO 0 not actively bound by current code paths. |
| 1       | `mesh_data`          | gameos fixed-function lane | UBO    | (no explicit `glBindBufferBase`; bound implicitly via `gos_BindBufferBase` material plumbing) | `shaders/gos_tex_vertex_lighted.vert:12`                                            | std140 block `{ vec4 ambient; vec4 diffuse; }`. SCENE_DATA_ATTACHMENT_SLOT (`gameos.hpp:2828`) maps here. |
| 2       | `CullUBO` (frustum + cull params) | GPU compute cull           | UBO    | `GameOS/gameos/gpu_cull_compute.cpp:440`, `gpu_cull_compute.cpp:897` | `shaders/gpu_cull.comp:169`                                                         | Owned by `CULL_UBO_BINDING = 2u` (`gpu_cull_compute.cpp:49`). Compute-only. Does NOT collide with SSBO binding 2 (separate namespace). |
| 3       | `ViewUniformsBlock`  | RenderCore engine view     | UBO    | `GameOS/gameos/view_uniforms_gl.cpp:28`, `view_uniforms_gl.cpp:38`, `view_uniforms_gl.cpp:45` (per-frame ring) | `shaders/include/view_uniforms.hglsl:23`, `shaders/fixtures/view_uniforms_contract.frag:13` (fixture probes as SSBO) | Named `RenderCore::kViewUniformsBinding = 3` (`RenderCore/ViewUniforms.h:38`). 144 B std140; F1-3 series default-ON. Shader sites must `#include <include/view_uniforms.hglsl>` and define `MC2_USE_VIEW_UNIFORMS`. |

---

## Runtime SSBO bindings

Bindings 0..7 are heavily multiplexed — most slots are reused across
*disjoint* render passes (terrain compute vs static-prop draw vs mech draw vs
water stream). Reuse is safe as long as the slot is rebound or unbound at the
right point in the frame. See "Known multi-binder slots" below.

| Binding | Name                                  | Owner subsystem               | Type | C++ bind site                                                                                                  | Shader consumer(s)                                                                                                                                                                                                                                          | Notes |
|--------:|---------------------------------------|-------------------------------|------|----------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|-------|
| 0       | `Instances` (static-prop)             | static-prop batcher           | SSBO | restored at `GameOS/gameos/gos_static_prop_batcher.cpp:4904`, `5140`                                            | `shaders/static_prop.vert:55`, `shaders/shadow_static_prop.vert:20`                                                                                                                                                                                          | Multi-binder slot. |
| 0       | `InstanceBuffer` (mech)               | mech batcher                  | SSBO | restored at `GameOS/gameos/gos_mech_batcher.cpp:631`, `1535`                                                    | `shaders/mech.vert:46`, `shaders/shadow_mech.vert:11`                                                                                                                                                                                                        | Multi-binder slot — mech draw pass. |
| 0       | `QuadRecordBuf` / `Recipes`           | terrain GPU-driven            | SSBO | `gos_terrain_indirect.cpp:2901`, `gos_terrain_patch_stream.cpp:1315/1335`, `gos_terrain_water_stream.cpp:1352`  | `shaders/gos_terrain.tesc:34`, `shaders/gpu_driven_terrain_solid.comp:139`, `shaders/gpu_driven_water.comp:121`, `shaders/gpu_driven_cmd_patch.comp:57`                                                                                                       | Multi-binder slot — terrain compute/draw. |
| 1       | `Colors` (static-prop per-color)      | static-prop batcher           | SSBO | restored at `gos_static_prop_batcher.cpp:4905`                                                                  | `shaders/static_prop.vert:56`                                                                                                                                                                                                                                | Multi-binder slot. |
| 1       | `BoneBuffer` (mech bones)             | mech batcher                  | SSBO | restored at `gos_mech_batcher.cpp:632`, `1536`                                                                  | `shaders/mech.vert:52`, `shaders/shadow_mech.vert:12`                                                                                                                                                                                                        | Multi-binder slot — mech draw pass. |
| 1       | `RecipeBuf` / `Lighting` / `Cmds`     | terrain                       | SSBO | `gos_terrain_water_stream.cpp:1615`, `gos_terrain_patch_stream.cpp:1434`, `gos_terrain_indirect.cpp:2902`, `gameos_graphics.cpp:3096` | `shaders/gos_terrain_thin.vert:32`, `shaders/gpu_driven_terrain_solid.comp:140`, `shaders/gpu_driven_water.comp:122`, `shaders/gos_terrain_lighting.comp:43`, `shaders/gpu_driven_cmd_patch.comp:58`                                                            | Multi-binder slot. |
| 2       | `PerType` (static-prop per-type)      | static-prop batcher           | SSBO | `gos_static_prop_batcher.cpp:3499`, restored at `:4906`                                                         | `shaders/static_prop.vert:57`                                                                                                                                                                                                                                | Multi-binder slot. Owner `TL_OUTPUT_BINDING=2` (`gos_terrain_lighting.cpp:115`) for terrain-lighting compute. |
| 2       | `ThinRecord` / `LightingBuf` / `Window` | terrain                     | SSBO | `gos_terrain_patch_stream.cpp:1433`, `gameos_graphics.cpp:3391/3417/3542/3568/3231`, `gos_terrain_water_stream.cpp:1354/1643`, `gos_terrain_indirect.cpp:2903/3292` | `shaders/gos_terrain_thin.vert:23`, `shaders/gos_terrain.frag:101`, `shaders/gos_terrain_mask_water.vert:41`, `shaders/gos_terrain_mask_solid.vert:33`, `shaders/gpu_driven_terrain_solid.comp:141`, `shaders/gpu_driven_water.comp:123`, `shaders/gos_terrain_lighting.comp:44` | Multi-binder slot. |
| 2       | `mech material table`                 | mech batcher                  | SSBO | `gos_mech_batcher.cpp:1347`, restored at `:1537`                                                                | (mech material lane)                                                                                                                                                                                                                                         | Multi-binder slot — mech draw pass. |
| 3       | `ParityOut` (static-prop parity probe)| static-prop batcher           | SSBO | restored at `gos_static_prop_batcher.cpp:4907`                                                                  | `shaders/static_prop.vert:67`                                                                                                                                                                                                                                | Multi-binder slot. Default-off probe (`u_parityWrite=0`). Does NOT collide with `UBO binding=3` ViewUniforms — separate namespaces. |
| 3       | `Thin` (terrain solid/water)          | terrain GPU-driven            | SSBO | `gos_terrain_water_stream.cpp:1376/1385/1394/1408/1419/1600/1644`                                              | `shaders/gpu_driven_terrain_solid.comp:152`, `shaders/gpu_driven_water.comp:124`                                                                                                                                                                              | Multi-binder slot. Unbind-to-0 used as fence between disjoint passes. |
| 4       | `PerDrawData` (static-prop per-draw)  | static-prop batcher           | SSBO | `gos_static_prop_batcher.cpp:3851`, restored at `:4656`                                                         | `shaders/static_prop.frag:48`                                                                                                                                                                                                                                |  |
| 5       | `MaterialTable` (static-prop GPU material) | static-prop batcher       | SSBO | `gos_static_prop_batcher.cpp:3858`, restored at `:4908`                                                         | `shaders/static_prop.frag:60`, `shaders/fixtures/material_gpu_contract.frag:14`                                                                                                                                                                              | Multi-binder slot. |
| 5       | `WaterRecipeBuf`                      | terrain water                 | SSBO | `gameos_graphics.cpp:2483/2545/2560/2641/3540/3566`                                                             | `shaders/gos_terrain_water_fast.vert:31`, `shaders/gos_terrain_water_fast_mdi.vert:28`, `shaders/gos_terrain_mask_water.vert:33`                                                                                                                              | Multi-binder slot. |
| 6       | `WaterThinBuf` / bucket-header        | terrain water / terrain solid | SSBO | `gameos_graphics.cpp:2546/2642`, `gos_terrain_water_stream.cpp:1363`, `gos_terrain_indirect.cpp:2911/3299`      | `shaders/gos_terrain_water_fast.vert:49`, `shaders/gos_terrain_water_fast_mdi.vert:38`, `shaders/gpu_driven_terrain_solid.comp:153`, `shaders/gpu_driven_water.comp:125`                                                                                       |  |
| 7       | `PerCmdBuf` / `Canary` / `CmdToBucket`| terrain / cull-patch          | SSBO | `gameos_graphics.cpp:2485/2547/2641`, `gos_terrain_indirect.cpp:2912`, `gpu_cull_compute.cpp:1049/1083`         | `shaders/gos_terrain_water_fast_mdi.vert:55`, `shaders/gpu_driven_terrain_solid.comp:160`, `shaders/gpu_cull_patch.comp:76`                                                                                                                                  |  |
| 8       | `SUBSTRATE_SSBO_BINDING` / `RecordsBuf` / `CmdBuf` | gpu_cull substrate / cull / terrain solid | SSBO | `gpu_cull_substrate.cpp:24` (constant), `gpu_cull_compute.cpp:885/1121`, `gos_terrain_indirect.cpp:2920/3296` | `shaders/gpu_cull.comp:51`, `shaders/gpu_cull_block_rollup.comp:44`, `shaders/gpu_driven_terrain_solid.comp:166`                                                                                                                                              | First slot "clearly clear of all existing paths" (`gpu_cull_substrate.cpp:22-24` comment). Read by `substrate_getInstanceSsboBindingPoint()`. |
| 9       | `DEBUG_SSBO_BINDING` / `VisibleIds` / `SolidWin` | gpu_cull / terrain solid | SSBO | `gpu_cull_compute.cpp:43` (constant), `:969/:1160`, `gos_terrain_indirect.cpp:2926`                            | `shaders/gpu_cull.comp:73`, `shaders/gpu_cull.comp:131` (DebugOut variant)                                                                                                                                                                                    | Two declarations at binding=9 in `gpu_cull.comp` (VisibleIds + DebugOut) — pre-existing pattern, single shader does not bind both simultaneously. |
| 10      | `BUCKET_COUNTS_BINDING` / `BucketCounts` | gpu_cull                  | SSBO | `gpu_cull_compute.cpp:44` (constant), `:970/:1006`                                                              | `shaders/gpu_cull.comp:81`, `shaders/gpu_cull_patch.comp:46`                                                                                                                                                                                                  |  |
| 11      | `BUCKET_CAPS_BINDING` / `INDIRECT_CMD_BINDING` | gpu_cull                 | SSBO | `gpu_cull_compute.cpp:45/48` (constants), `:971/:1010`                                                          | `shaders/gpu_cull.comp:91`, `shaders/gpu_cull_patch.comp:54`                                                                                                                                                                                                  | Same slot reused across two compute shaders — `BucketCaps` (read-only) vs `IndirectCmds` (write). Explicit comment in source: "same slot OK — different shader". |
| 12      | `ACTOR_VIS_BINDING` / `ActorVis`      | gpu_cull                      | SSBO | `gpu_cull_compute.cpp:46` (constant), `:972/:1122`                                                              | `shaders/gpu_cull.comp:97`, `shaders/gpu_cull_block_rollup.comp:51`                                                                                                                                                                                          |  |
| 13      | `BLOCK_VIS_BINDING` / `BlockVis`      | gpu_cull                      | SSBO | `gpu_cull_compute.cpp:47` (constant), `:978/:1123`                                                              | `shaders/gpu_cull.comp:124`, `shaders/gpu_cull_block_rollup.comp:63`                                                                                                                                                                                          |  |
| 14      | `READBACK_SSBO_BINDING` / `Particles` | gpu_cull readback / particle bridge | SSBO | `gpu_cull_readback.h:18` (constant), `gpu_cull_compute.cpp:918`, `gos_particle_bridge.cpp:310` (`/*binding=*/14`) | `shaders/particle_billboard.vert:26`                                                                                                                                                                                                                          | **Multi-binder conflict candidate.** Cull readback ring (compute writes) and particle SSBO (vertex reads) both use 14. They run in disjoint passes but share the slot without an explicit rebind/unbind contract documented. See "Known issues" below. |
| 15      | `PermutationBuf`                      | gpu_cull patch                | SSBO | `gpu_cull_compute.cpp:1037/:1085`                                                                               | `shaders/gpu_cull_patch.comp:69`                                                                                                                                                                                                                              |  |
| 16      | `BASE_INSTANCE_SSBO_BINDING` / `BaseInstanceByCmd` | static-prop / cull patch | SSBO | `gos_static_prop_batcher.cpp:210` (constant), `:5699`                                                            | `shaders/gpu_cull_patch.comp:35`                                                                                                                                                                                                                              |  |
| 17      | `SolidMaskBuf`                        | terrain solid mask            | SSBO | `gameos_graphics.cpp:3388/3414`                                                                                 | `shaders/gos_terrain_mask_solid.vert:16`                                                                                                                                                                                                                      |  |
| 18      | `WaterMaskBuf`                        | terrain water mask            | SSBO | `gameos_graphics.cpp:3539/3565`                                                                                 | `shaders/gos_terrain_mask_water.vert:30`                                                                                                                                                                                                                      |  |
| 19      | `RecipeBuf` (mask-solid recipe)       | terrain solid mask            | SSBO | `gameos_graphics.cpp:3389/3415`                                                                                 | `shaders/gos_terrain_mask_solid.vert:25`                                                                                                                                                                                                                      |  |
| 20      | `LIGHT_DATA_SSBO_BINDING` / `SurfaceVertexBuf` | gameos lighting / terrain surface | SSBO | `GameOS/include/gameos.hpp:2827` (constant), `gameos_graphics.cpp:6966/6993/7020`, `:2835/:2886`               | `shaders/gos_terrain_surface.vert:60`                                                                                                                                                                                                                         | **Multi-binder conflict candidate.** Per `gameos.hpp:2826` comment, "SSBO bindings 0-19 are allocated elsewhere", but `SurfaceVertexBuf` also uses 20 (`gameos_graphics.cpp:2835`). Terrain surface and lighting SSBO use disjoint passes — but the comment is now stale. See "Known issues". |
| 21      | `SurfaceIndexBuf`                     | terrain surface               | SSBO | `gameos_graphics.cpp:2867/2887`                                                                                 | `shaders/gos_terrain_surface.vert:63`                                                                                                                                                                                                                         |  |
| 22      | `SurfaceTileBuf`                      | terrain surface               | SSBO | `gameos_graphics.cpp:2844/2888`                                                                                 | `shaders/gos_terrain_surface.vert:78`                                                                                                                                                                                                                         |  |
| 23      | `TERRAIN_HEIGHT_SSBO_BINDING` / `TerrainHeightBuf` | terrain LOD chunk             | SSBO | `GameOS/gameos/gos_terrain_lod_chunk.cpp`                                                                       | `shaders/terrain_lod_chunk.vert:layout(binding=23)`, `shaders/terrain_lod_chunk.frag:58` (`TerrainHeightBufFrag` — same slot, same buffer; frag reads it for smooth per-pixel normals via central difference) | Added Phase 3. Full map float[] upload at load; dirty row-wise patch on edit. |
| 24      | `TERRAIN_TYPE_SSBO_BINDING` / `TerrainTypeBuf`     | terrain LOD chunk             | SSBO | `GameOS/gameos/gos_terrain_lod_chunk.h:17` (constant), `gos_terrain_lod_chunk.cpp:605`, `:992`                  | `shaders/terrain_lod_chunk.vert:30`                                                                                                                                                                                                                           | Step 5b. Per-vertex `float terrainType` — 0.0 = terrain, values >0 = concrete. Passed as `v_terrainType` to frag for cement/colormap blend (smoothstep). Bound conditionally (skipped if never uploaded). |
| 25      | `TERRAIN_CEMENT_SSBO_BINDING` / `TerrainCementBufFrag` | terrain LOD chunk         | SSBO | `GameOS/gameos/gos_terrain_lod_chunk.h:18` (constant), `gos_terrain_lod_chunk.cpp:608`, `:1005`                 | `shaders/terrain_lod_chunk.frag:63`                                                                                                                                                                                                                           | Step 5c. Per-tile `uint cementWord` (packed: valid bit + layer index). Read by world tile in the frag (LOD-independent) to select cement atlas UVs for runways and roads. Bound conditionally (skipped if never uploaded). |

---

## Image / storage-image bindings

No `glBindImageTexture` call sites found in the codebase
(grep of `glBindImageTexture\s*\(` returns only the declaration in
`3rdparty/include/SDL2/SDL_opengl_glext.h:2453`). Compute shaders write only
via SSBO, not image-store.

---

## Texture sampler units

> **⚠ STALE / DO-NOT-TRUST (corrected 2026-06-22 RENDER-CONTRACT-INDEX-1).**
> The table below is self-admittedly non-exhaustive — notably the terrain
> `matNormal0..matNormal4` samplers are **not** enumerated (see the note after
> the table), and several units carry "varies" placeholders. Do not treat it as
> a complete sampler-unit map. Authoritative sampler occupancy is being built as
> `docs/render-backend-seams/sampler-unit-occupancy.{md,json}` under
> SHADER-SAMPLER-BINDING-MANIFEST-1; use that once it lands. This table is
> retained for historical value only.

Samplers are wired implicitly through `glActiveTexture(GL_TEXTUREn)` +
`glUniform1i(sampler_uniform_loc, n)`. The codebase does **not** use
`layout(binding = N) uniform sampler2D` declarations
(grep of `layout\s*\(\s*binding\s*=\s*\d+\s*\)\s*uniform\s+sampler` returns
zero matches). The table below records the active units observed.

| Unit | Purpose                                            | C++ activate site (representative)                                                       | Shader uniform name(s) |
|-----:|----------------------------------------------------|------------------------------------------------------------------------------------------|------------------------|
| 0    | Primary albedo / colormap / scene color            | `GameOS/gameos/gameos_graphics.cpp:2007` and many; `gos_postprocess.cpp:677`             | `tex1`, `sceneTex`, `u_tex`, `uAtlas`, `image`, `u_hdri` (skybox via FBO sampler) |
| 1    | Secondary detail / bloom; (chunk-program-scoped, own uniform namespace) Terrain LOD chunk authored cement/pad/runway overlay sidecar (`MC2_TERRAIN_OVERLAY_V2`, default OFF) | `gameos_graphics.cpp:2501/2535`; `gos_postprocess.cpp:679/736`; `GameOS/gameos/gos_terrain_lod_chunk.cpp` (`TERRAIN_OVERLAY_SIDECAR_TEXUNIT`) | `tex2`, `bloomTex`, `matNormal*` (terrain frag uses non-explicit-binding samplers); `u_overlaySidecar` (`shaders/terrain_lod_chunk.frag`) — TERRAIN-OVERLAY-V2-PARITY-1 |
| 2    | Tertiary detail / reflection atlas (water)         | `gos_postprocess.cpp:681`                                                                 | `tex3`, `reflTex` |
| 3    | Shadow map (sometimes diffuse)                     | `gameos_graphics.cpp:3055/3223/3365/3428`; `gos_postprocess.cpp:683`                      | (varies) |
| 5    | Mine sprite array; HDRI lookup                     | `gameos_graphics.cpp:3624/3665/3687`                                                       | `mineSpriteArray` (`shaders/gos_terrain_mine_static.frag:16` — "bound at unit 5") |
| 7    | Post-process input variant                         | `gameos_graphics.cpp:4890`                                                                 | (varies) |
| 9    | Shadow / scene auxiliary                           | `gameos_graphics.cpp:4810/5164/5278/5402`, `:7076`, `:7500`                                | `shadowMap` (`shaders/include/shadow.hglsl:4`) — typically unit 9 |
| 10   | Dynamic shadow / scene normal                      | `gameos_graphics.cpp:5028/5174/5289/5413`, `:7092`, `:7512`                                | `dynamicShadowMap` (`shaders/include/shadow.hglsl:87`), `sceneNormalTex` |
| 12   | Terrain LOD chunk authored control map (RGBA rock/grass/dirt/concrete weights, override-only, `MC2_TERRAIN_CONTROLMAP`) | `GameOS/gameos/gos_terrain_lod_chunk.cpp` (`TERRAIN_CONTROLMAP_TEXUNIT`, `gos_terrain_lod_chunk.h`) | `u_controlMap` (`shaders/terrain_lod_chunk.frag`) — TERRAIN-CONTROLMAP-SAMPLE-1 |

Note: terrain `matNormal0..matNormal4` samplers (`shaders/gos_terrain.frag:44-48`)
are assigned units via runtime `glUniform1i` at program-link rebind time; the
exact assignments are not encoded with `layout(binding=)` and are not exhaustively
enumerated here. When wiring a new sampler, prefer explicit `glActiveTexture` +
`glUniform1i` over `layout(binding=)` for consistency with the existing codebase.

---

## Fixture-only / test bindings

These appear in `shaders/fixtures/*` and exist solely for the shader-reflection
contract probes (`tools/shader_reflect/`). They are **never** bound at runtime.

| Binding | Decl                                                                       | Notes |
|--------:|----------------------------------------------------------------------------|-------|
| UBO 3 (probed as SSBO) | `shaders/fixtures/view_uniforms_contract.frag:13` (decl `layout(std430, binding=3)`) | Reflection-only probe of the ViewUniforms layout. Not a runtime conflict — fixtures are never linked into a runtime program. |
| SSBO 5  | `shaders/fixtures/material_gpu_contract.frag:14`                          | Material-GPU contract probe. |

---

## Reserved / forbidden

No explicit reserved/forbidden binding numbers are documented in code as of
this snapshot. Known driver-specific oddity:
- AMD TES uniform / UBO propagation is unreliable — see memory note
  `amd_tes_uniform_propagation_unreliable` (referenced by the
  unified-projection F1 handoff). The workaround is to push the final
  transport over an SSBO; binding 23 (`TERRAIN_HEIGHT_SSBO_BINDING`) is
  now the live slot for that transport (terrain LOD chunk heightfield).

---

## Known multi-binder slots (intentional reuse)

The following slot reuses are **safe** because the bound buffer is rebound or
unbound between disjoint passes. They are not bugs; documenting them so future
reviewers don't try to "clean them up":

- **SSBO 0–7**: reused across static-prop draw vs mech draw vs terrain
  GPU-driven compute/draw vs water stream. Each subsystem rebinds on entry;
  `gos_terrain_water_stream.cpp:1641-1644` and `gos_static_prop_batcher.cpp:4904-4908`
  show the explicit unbind/restore patterns.
- **SSBO 11**: `BUCKET_CAPS_BINDING` (read in `gpu_cull.comp`) vs
  `INDIRECT_CMD_BINDING` (write in `gpu_cull_patch.comp`). Explicit comment
  at `gpu_cull_compute.cpp:48`: *"same slot OK — different shader"*.
- **SSBO 9**: `VisibleIds` vs `DebugOut` in the same `gpu_cull.comp` (two
  `layout(std430, binding=9)` declarations at lines 73 and 131). Only one is
  active per dispatch path.

---

## Known issues / TBD

1. **SSBO 14 multi-binder needs documented contract.** Used by both the
   GPU-cull readback ring (`gpu_cull_readback.h:18` `READBACK_SSBO_BINDING = 14u`,
   compute writes in `gpu_cull_compute.cpp:918`) and the particle billboard
   buffer (`gos_particle_bridge.cpp:310` hard-codes `/*binding=*/14`, shader
   reads in `shaders/particle_billboard.vert:26`). These run in disjoint
   passes but the particle bridge does not currently reference the cull
   constant or explicitly restore/unbind on exit. Risk: enabling
   `MC2_GPU_CULL_READBACK=1` while particle SSBO is still bound, or
   particle path running between cull dispatch and CPU readback. Suggested
   action: replace the magic `14` literal in `gos_particle_bridge.cpp:310`
   with a named constant and add a rebind/unbind step at pass boundaries.

2. **SSBO 20 multi-binder vs stale comment.** `gameos.hpp:2826` says "SSBO
   bindings 0-19 are allocated elsewhere" — implying 20 is the first free
   slot for LightsData SSBO. But `SurfaceVertexBuf` also uses 20
   (`gameos_graphics.cpp:2835`, shader `shaders/gos_terrain_surface.vert:60`).
   The terrain-surface pass and the lighting SSBO bind are disjoint, so this
   is functionally safe, but the comment at `gameos.hpp:2826` is now stale.
   Suggested action: update the comment to acknowledge terrain-surface SSBOs
   at 20–22 and define the contract for when each path holds the slot.

3. **UBO 0 (legacy `LIGHT_DATA_ATTACHMENT_SLOT`)** — superseded by SSBO 20
   per `gameos.hpp:2824` comment, but the `#define LIGHT_DATA_ATTACHMENT_SLOT 0`
   constant remains at `gameos.hpp:2823`. Verify no path still binds to UBO 0
   under this name before removing.

---

## Cross-references

- `RenderCore/ViewUniforms.h` — `kViewUniformsBinding = 3` and authoritative
  struct layout.
- `GameOS/gameos/view_uniforms_gl.{h,cpp}` — GL implementation of the
  ViewUniforms UBO.
- `GameOS/gameos/gpu_cull_compute.cpp:43-49` — central block of named
  constants for the cull pipeline (`DEBUG_SSBO_BINDING`,
  `BUCKET_COUNTS_BINDING`, `BUCKET_CAPS_BINDING`, `ACTOR_VIS_BINDING`,
  `BLOCK_VIS_BINDING`, `INDIRECT_CMD_BINDING`, `CULL_UBO_BINDING`).
- `GameOS/gameos/gpu_cull_readback.h:18` — `READBACK_SSBO_BINDING = 14`.
- `GameOS/gameos/gpu_cull_substrate.{h,cpp}` —
  `SUBSTRATE_SSBO_BINDING = 8` and accessor
  `substrate_getInstanceSsboBindingPoint()`.
- `GameOS/gameos/gos_terrain_lighting.cpp:113-115` —
  `TL_VERTEX_INPUT_BINDING=0`, `TL_LIGHT_INPUT_BINDING=1`,
  `TL_OUTPUT_BINDING=2` (terrain-lighting compute).
- `GameOS/gameos/gos_static_prop_batcher.cpp:210` —
  `BASE_INSTANCE_SSBO_BINDING = 16`.
- `GameOS/include/gameos.hpp:2823,2827` — `LIGHT_DATA_ATTACHMENT_SLOT=0`
  (legacy UBO), `LIGHT_DATA_SSBO_BINDING=20`.
- `shaders/include/view_uniforms.hglsl` — shader-side ViewUniforms include.
- `shaders/include/lighting.hglsl` — shader-side LightsData/calc_light
  with the post-`LIGHTSSBO v1` SSBO layout.
- `tools/shader_reflect/` — reflection contract that diffs shader binding
  layouts against committed JSON goldens (`docs/observations/INDEX-SHADERS.md`
  for context).
