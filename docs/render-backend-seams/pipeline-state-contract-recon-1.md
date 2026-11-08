<!-- doc-status: current | reviewed:2026-10-11 -->
# PIPELINE-STATE-CONTRACT-RECON-1

**Status:** RECON COMPLETE · read-only · doc-only commit to nifty.
**Worktree:** `…/worktrees/nifty-mendeleev` @ `claude/nifty-mendeleev` (base tip `073820b7`). All `file:line` re-grepped this session; lines drift on live-shared nifty.
**Question:** does MC2 have (or is it ready for) an explicit pipeline-state model — the GL analogue of a `VkGraphicsPipeline` baking `{SPIR-V stages, vertex input, blend, depth, cull, raster, render-pass compat}` into one immutable object?

## VERDICT: **DEFER**

Named blocker: **the runtime `#define` prefix-injection wall + the open-ended `gosMaterialVariation` define loop make the shader-variant set non-enumerable today.** A PSO key is `{shader-variant-id, vertex-layout-id, blend, depth, cull, raster, color-attachment-mask}`. MC2 already has clean, stable answers for **6 of the 7** axes (vertex layouts are fixed per shape type; FF state is small and mostly captured by `PipelineDesc`; render-pass compat is locked by RENDER-PASS-DAG-CONTRACT-1). The **one missing axis is `shader-variant-id`**: variants are materialized at runtime by string concatenation in `glsl_shader::makeShader`, and `gosMaterialVariation` injects an *arbitrary, data-driven* `#define` set per material. You cannot mint a stable PSO key over an unbounded variant space. Freezing a pipeline-state contract now would either (a) encode that unbounded space (a false/leaky model), or (b) freeze a transitional design before the prerequisite lands.

**Prerequisite (the real next arc, this recon RECOMMENDS it):** `SHADER-PERMUTATION-INVENTORY` / `SHADER-PIPELINE-OFFLINE-1` — close the material-define loop to a fixed macro enumeration, then bake offline SPIR-V variants keyed by define-set. The tooling for this is **90% built already** (see §5). Once the variant space is finite and named, the PSO-key contract becomes a GO.

**But ship the small bridge now (partial GO, §6):** a check-time `PipelineDesc`-completeness/occupancy checker. It does not require the `#define` wall to fall and converts the *known* pipeline quadrant (the 5 registered pipelines) into a checked artifact — same pattern as the rest of the manifest arc.

---

## 1. Existing pipeline surface (what's captured TODAY)

### `PipelineDesc` (`RenderCore/PipelineDesc.h:50-71`) — captures only the depth/blend/cull/program quadrant
| Field | Type | Line |
|---|---|---|
| `glProgramName` | `uint32_t` | 54 |
| `blend` | `BlendMode {Opaque,AlphaBlend,AlphaTest,Additive}` | 56 / enum :32 |
| `depthTestEnable` | `bool` | 57 |
| `depthWriteEnable` | `bool` | 58 |
| `depthFunc` | `DepthFunc {LessEqual,GreaterEqual,Always,Equal}` | 59 / enum :39 (GEQUAL = reverse-Z) |
| `cullMode` | `CullMode {None,Back,Front}` | 61 / enum :34 |
| `colorAttachments` | `ColorAttachmentMask {color0,color1,color2}` | 62 / struct :44 |
| `objectIdWriteEnabled` | `bool` (descriptive) | 66 |
| `ssboBindingsMask` | `uint32_t` (**metadata only, does NOT drive binding** — `PipelineRegistry.cpp:11`) | 70 |

`static_assert(sizeof(PipelineDesc) <= 20)` (:76). **NOT captured:** viewport/scissor, sampler state, **blend equation** (only func), **front-face winding**, color/depth write-mask granularity, MRT draw-buffer config, **vertex-input layout**, polygon offset.

### `PipelineId` registry (`RenderCore/PipelineRegistry.h:25-33`)
`Invalid=0, StaticPropOpaque=1, StaticPropAlphaTest=2, MechOpaque=3, StaticPropDepth=4, Count_=5`. Rows in `PipelineRegistry.cpp:39-116`, `static_assert(row==Count_)` :118.

### Who routes through `PipelineDesc` (`applyPipeline` = the ONLY translation site, `GameOS/gameos/pipeline_binder.cpp:73`)
**Correction to the handoff premise** ("only StaticPropOpaque + MechOpaque"): **three** subsystems / four ids are live through `applyPipeline`:
- **Static prop batcher** `gos_static_prop_batcher.cpp`: `:5128` (`StaticPropDepth` prepass), `:5365`/`:6421` (`StaticPropOpaque` color). `bindProgram` wires Opaque/AlphaTest `:1206-1207`, coalesce `:1342-1343`, Depth `:1397`.
- **Mech batcher** `gos_mech_batcher.cpp:1992-1993` (`MechOpaque`), `bindProgram` :570.
- `StaticPropAlphaTest`'s desc **row exists** but the live color-pass `applyPipeline` calls fetch the **`StaticPropOpaque` desc** (same FF state; alpha split drives texture-array selection, not GL state — `PipelineRegistry.cpp:14-16`). So AlphaTest is effectively descriptive-only at runtime.

`pipelineDescRegistered=true` in `RenderPassContract.h` for only `StaticPropOpaque` (:169) and `MechOpaque` (:198).

**What `applyPipeline` sets** (`pipeline_binder.cpp:19-75`): program (skip if 0), `GL_DEPTH_TEST`, `glDepthMask`, `glDepthFunc`, blend (always `glBlendFunc` even when disabled — GLSTATE-BLEND-RESTORE-1, :38-41), cull. **Does NOT set:** MRT/draw-buffers, color-mask, viewport, SSBO binds, VAO, samplers, front-face.

### What BYPASSES `PipelineDesc` entirely
All other passes (`pipelineDescRegistered=false`): **Terrain, Shadow, VFX, Water, PostProcess, VegetationCards, TerrainDecal, TerrainOverlay, UI**. They set FF state ad-hoc via raw `gl*` literals or `applyRenderStates`.

## 2. Per-draw FF-state ownership (THREE assembly paths)

(A) `PipelineDesc`/`applyPipeline` (static-prop + mech only) · (B) `gosRenderer::applyRenderStates` legacy gos_State_* → GL bridge (terrain/HUD/MLR queue) · (C) raw `glEnable`/`gl*` literal blocks in `gameos_graphics.cpp`/`gos_postprocess.cpp` with manual save/restore.

| State | Where set | In PipelineDesc? |
|---|---|---|
| `GL_DEPTH_TEST` | applyPipeline `pipeline_binder.cpp:23-24`; applyRenderStates `gameos_graphics.cpp:5471-5475`; raw literals (many) | partial |
| `glDepthFunc` | applyPipeline :28-34; applyRenderStates :5484-5489 (GEQUAL reverse-Z); shadow GL_LESS :6155/6311/6400 | partial |
| `glDepthMask` | applyPipeline :25; applyRenderStates :5463-5467; raw (many) | partial |
| `glColorMask` | **raw only** :3798/4003/4375/… (static depth-prepass masks color via caller — `PipelineRegistry.cpp:108-110`) | **no** |
| `GL_BLEND`+`glBlendFunc` | applyPipeline :42-60; applyRenderStates :5493-5506 (5-way) | partial (func only) |
| `glBlendEquation` | **set nowhere** in scene path (0 hits) — default GL_FUNC_ADD assumed | **no** |
| `GL_CULL_FACE`+`glCullFace` | applyPipeline :63-75; applyRenderStates :5444-5453 (CW→BACK/CCW→FRONT) | partial |
| `glFrontFace` | **only** `GameOS/gameos/gameos_graphics_draw.cpp:497` (save/restore snapshot) | **no** |
| viewport/scissor | FBO/post code; no pipeline abstraction | **no** |
| MRT/draw-buffers | FBO setup only | **no** |
| texture+sampler binds | applyRenderStates :5544-5568 (units 0/1/2, `setSamplerParams` :5554) | **no** |

Closest thing to a full FF capture = the save/restore snapshot struct `GameOS/gameos/gameos_graphics_draw.cpp:491-497` (`depthTest/depthFunc/depthMask/colorMask/cullEnabled/cullFace/frontFace`). No single object captures program+FF+VAO+SSBO+textures+viewport together.

## 3. Vertex-input layouts (the VAO half of a PSO) — STABLE, finite, per-shape-type

| Shape type | File:line | Layout (loc: size×type) |
|---|---|---|
| Static prop (shared VAO) | `GameOS/gameos/gos_static_prop_batcher_program.cpp:382-395` | stride 40B: 0=pos3 1=norm3 2=uv2 3=uint(I) 4=uint(I) |
| Mech (`GpuMechVertex` 48B) | `GameOS/gameos/gos_mech_batcher_geometry.cpp:208-214` | 0=pos3 1=norm3 2=uv2 3=bone4ub(I) 4=wt4ub-norm 5=tan2short-norm 6=rgbLight uint(I) |
| Terrain patch stream | `gos_terrain_patch_stream.cpp:1054/1060` | worldPos3, worldNorm3 (name-resolved) |
| Terrain lod/legacy | `gameos_graphics.cpp:6254-6256`, `:7058-7063` | extra attrs + worldPos/Norm |
| Vegetation cards (instanced) | `gos_vegetation.cpp:176-215` | per-vtx 0=pos3 1=uv2; per-inst 2..6 |
| VFX mesh | `gos_vfx_mesh_bridge.cpp:174-177` | 0=pos3 1=uv2 |
| HUD/2D sprite | `gameos_graphics.cpp:503-519` | pos4 + color4ub + spec/fog4ub + uv2 (optional, loc-resolved) |
| Water/scene quad | `GameOS/gameos/gameos_graphics_bridge.cpp:870-874`,`:5229-5232`,`:9823-9826` | 0=pos3 1=uv2 2=uint(I)/float 3=color4ub |
| Shadow/depth quad | `GameOS/gameos/gameos_graphics_draw.cpp:773` | 0=pos3, stride 36 |

Particle/VFX-trail + GPU-driven indirect terrain solid = **no `glVertexAttribPointer`** (SSBO/`gl_VertexID`-driven). → vertex-input is a clean finite ID space; **not a blocker.**

## 4. The shader-variant axis = THE blocker (RED wall)

**Injection point:** `glsl_shader::makeShader` (`GameOS/gameos/utils/shader_builder.cpp:638`, :446): `strings[] = { prefix?:"" , source }` → `glShaderSource`/`glCompileShader` at runtime. Files carry **no `#version`**; the prefix supplies `"#version 430\n"` + all `#define`s. `reload()` (:533/:971) re-injects the stored `prefix_` — hot-reload **depends on** runtime recompile. **No `glShaderBinary`/SPIR-V load path, no variant cache keyed by define-set exists.** (`makeProgram`/`makeProgram2` decls `shader_builder.h:134-135`.) Compute shaders have a parallel hand-rolled injector (`gpu_cull_compute.cpp:287-295`, etc.).

### Macro classification
(a)=compile-time VARIANT→distinct SPIR-V · (b)=spec-constant candidate · (c)=runtime uniform · (d)=dead

| Macro | Site | Class |
|---|---|---|
| `MRT_ENABLED` | `GameOS/gameos/gameos_graphics_material_internal.h:369` | (a) changes frag output decls |
| `TERRAIN_NORMAL_ARRAY` | :368/5028/5054/5095 | (a) sampler type / sample code |
| `MC2_SHADOW_CSM` | :375/5173; `gos_terrain_lod_chunk.cpp:356`; `gos_postprocess.cpp:516/528` | (a) shadow.hglsl body |
| `MC2_SHADOW_CSM_MAX %d` | same | **(b)** array-size param — cleanest spec-constant |
| material `#define <d>=1` (gosMaterialVariation loop) | `gameos_graphics.cpp:341-358` | **(a) OPEN-ENDED** — arbitrary per-material; the non-enumerable term |
| `MC2_OBJECT_ID_BUFFER` | `gos_static_prop_batcher.cpp:1141/1163`; `gos_mech_batcher.cpp:542` | (a) adds frag out loc=2 |
| `MC2_USE_VIEW_UNIFORMS` | static_prop :1155/1166; mech :549 | (a) swaps default-uniform for UBO binding=3 (descriptor change) |
| `MC2_COALESCE` | static_prop :1161 | (a) MDI vertex path + diff SSBO set |
| `MC2_STATICPROP_PBR_SLOTS` | static_prop :1173-1174 | (a) PBR slot code |
| `READBACK_SSBO_BINDING <n>` | `gpu_cull_compute.cpp:284` | (b) build-time binding constant (lockstep, not a variant axis) |
| `GPU_CULL_C2_READBACK` | :362/383 | (a) adds readback SSBO write |
| `GPU_CULL_C1B_INDIRECT` | :381 | (a) VisibleIds/indirect SSBOs |

No (c) runtime-uniform macros; no confirmed (d) dead (matrix harness anchors would WARN).

### Permutation explosion (multiplicative per family)
- **static_prop** = largest fixed source: `COALESCE × OBJECT_ID × USE_VIEW_UNIFORMS × PBR_SLOTS` → up to **2⁴=16/stage** (reflect.py enumerates 4 for COALESCE×OBJECTID alone; static_prop.frag has 18 in-file guards).
- **terrain** `MRT × NORMAL_ARRAY × CSM (× CSM_MAX)` → 8+ (gos_terrain.frag = 27 guards, most-guarded file).
- **mech** `OBJECT_ID × USE_VIEW_UNIFORMS` → 4/stage. **gpu_cull.comp** `C1B × C2` → 4.
- **`gosMaterialVariation`** = **unbounded / data-driven** — each distinct define set mints a uniquely-named program (`unique_name_suffix_` :356-357). **This is what makes static enumeration genuinely hard.**

(Audit reports ~94 `#ifdef` across 81 shaders; my grep: 117 `#if*` across 32 shader files — same order; live count via the matrix harness.)

## 5. SPIR-V reflector reuse — machinery 90% present, but no runtime consumer

`tools/shader_reflect/reflect.py`:
- **Compiles each variant to SPIR-V** (`glslangValidator --auto-map-locations --auto-map-bindings -V -R`, :329-339) reusing **engine-identical** include/define flattening (`shader_common.build_shader_source`, :312-314) → can reproduce runtime-injected source offline.
- **`spirv-cross --reflect`** (:365-385) → normalized contract (`:464-472`): UBOs (binding+members), SSBOs (binding+members w/ offset/stride/matrix_stride), frag `outputs` (loc+name+type), `stage`/`variant`/`defines`.
- Has `SHADER_VARIANTS` table (:63-119) enumerating the (a)-class macros + `REQUIRED_INVARIANTS` (:131-258) locking output locs / bindings (ViewUniforms=3, readback=14) / std430 offsets. `scripts/check_shader_interface.py` (family `schema`) rides these goldens for C++↔GLSL ABI lockstep.

**Can feed a PSO?** Yes for **stage interfaces + descriptor-set layout** (the `VkPipelineLayout` core). **Gaps:** no **vertex input attributes** extracted (only frag outputs normalized, :459); no push-constants; no pipeline render-state (blend/depth/cull is C++-side, §2); and **samplers are auto-mapped** (`--auto-map-bindings`) not read from real units — the audit's one true missing descriptor contract.

**The two hard blockers:** (1) engine has **no consumer for precompiled SPIR-V** (`makeShader` only `glShaderSource`+`glCompileShader`s strings; hot-reload depends on it). (2) the **open-ended material-define loop** (`gameos_graphics.cpp:341-358`) means the variant set is **not statically enumerable** — must be closed first. Single code seam to change = `glsl_shader::makeShader` (`GameOS/gameos/utils/shader_builder.cpp:638`), exactly as `vulkan-readiness-audit-1.md` Dimension F (:43) + blocker #6 (:61) name it.

## 6. Proposed PSO-key schema (descriptive) + smallest checkable artifact

### PSO key (maps to `VkGraphicsPipelineCreateInfo`)
```
PsoKey = {
  shaderVariantId   // → pStages (SPIR-V modules)  ── BLOCKED (§4/§5)
  vertexLayoutId    // → pVertexInputState          ── READY (§3, finite)
  blend             // → pColorBlendState (need +equation, §2)
  depth{test,write,func} // → pDepthStencilState    ── READY (PipelineDesc)
  cull + frontFace  // → pRasterizationState (frontFace missing from desc, §2)
  colorAttachmentMask // → render-pass color attachments ── READY (PipelineDesc)
  renderPassCompat  // → renderPass/subpass          ── READY (RENDER-PASS-DAG-CONTRACT-1)
}
```
6 of 7 axes have stable answers TODAY. Only `shaderVariantId` is non-enumerable. Add to `PipelineDesc` before any PSO impl: `blendEquation`, `frontFace`, explicit MRT/draw-buffer mask (today implied by `colorAttachments`).

### Smallest checkable artifact (partial GO — does NOT need the #define wall to fall)
A check-time script `scripts/check-pipeline-desc.py` + occupancy doc `docs/render-backend-seams/pipeline-state-occupancy.{md,json}` that:
1. Parses `PipelineRegistry.cpp` rows → asserts every `PipelineId` (except Invalid) has a complete `PipelineDesc` (all FF fields explicitly set, no implicit defaults).
2. Asserts every live `applyPipeline` call site references a registered `PipelineId` (no ad-hoc desc construction escaping the registry).
3. WARNs on FF state that `applyPipeline` does NOT set but a pipeline implies (colorMask, frontFace, blendEquation) — documents the ad-hoc escape surface (§2) so it can't silently grow.
4. Registered in `scripts/check-contracts.sh`, same shape as `check-binding-slots.py` / sampler-occupancy.

This converts the *known* pipeline quadrant into a checked artifact now, and gives the future PSO arc a baseline to extend. It does NOT attempt to model the variant explosion.

## 7. Recommendation summary
- **DEFER** the full pipeline-state / PSO-key contract. Blocker = shader-variant non-enumerability (`#define` wall + open-ended `gosMaterialVariation`).
- **Prerequisite arc:** `SHADER-PERMUTATION-INVENTORY` → close material-define loop to a fixed enumeration → offline SPIR-V bake keyed by define-set (tooling 90% built: reflect.py + matrix harness). Single seam: `makeShader` `GameOS/gameos/utils/shader_builder.cpp:638`.
- **Partial GO now:** ship `check-pipeline-desc.py` + `pipeline-state-occupancy.{md,json}` (§6) — converts the 5-pipeline known quadrant into a checked contract without touching the wall.
- **Stale-comment flags (doc-only, no edits made):** `ssboBindingsMask` is metadata-only (does not drive binding) — `PipelineDesc.h:70` could say so; `StaticPropAlphaTest` desc row is descriptive-only at runtime (`PipelineRegistry.cpp:14-16`).

### Exclusions honored
No Vulkan code, no PSO/RenderDevice abstraction, no shader edits, no offline-compile migration (recommended as separate arc), no material/texture work, foreign WIP (`mech3d.cpp`, `txmmgr.h`, `golden-sets.json`) untouched.
