> **SUPERSEDED / RE-SCOPED (2026-06-09):** Stage 0 (§9) measured `MultiTransformShape` at ≤1.2ms
> peak. This plan activates the GPU color-submit path but **keeps `MultiTransformShape` running** —
> so it does NOT remove the measured cost. S2 is re-scoped into **S2a (measure the submit/renderLists
> path)** and **S2b (true `MultiTransformShape` elimination, only if justified)**. Do NOT implement
> the Stage 1/2 below as written. See **`docs/perf-attribution-plan.md`** for the revised R0–R3 plan
> (frame-time attribution first; present + renderLists are the prime suspects now that terrain is
> <100µs). The staged plan below is retained as reference for the GPU-VB dead-code path it documents.

# S2 Implementation Plan — TG Dynamic Shape GPU Transform Migration

**Date:** 2026-06-09  
**Branch:** `claude/nifty-mendeleev`  
**Author:** Design-only pass (no code touched)

---

## Executive Summary (10 lines)

The legacy TG render path CPU-bakes every dynamic mech/vehicle/turret/building vertex to
clip-space every frame inside `TG_Shape::MultiTransformShape` (`mclib/tgl.cpp:1634`), then
passes the pre-transformed `gos_VERTEX` array through `mcTextureManager->addTriangle` into the
`renderLists()` flush. A second, shadowed path (`bShadersDrawPathEnabled`, off by default) uses
a pre-built GPU `vb_`/`ib_` per `TG_TypeShape` and submits a `TG_RenderShape` record with an
`mvp_` matrix; this path already exists but is `false`-guarded and has known bugs (single-texture
FIXME, missing alpha support). The migration's first win is: make the `TG_RenderShape` path the
active default for mechs-only, behind `MC2_TG_GPU_XFORM=1`, while preserving the addTriangle
path for all other families and as the opt-out fallback. CPU transform, lighting bake, backface
cull, and alpha/window/spotlight special-cases are NOT touched in Stage 1 or 2. Instancing and
batching are explicitly deferred to Stage 4 and beyond. Every stage is reversible by unsetting
the env flag. The oracle gates (MECH_MATERIAL_GPU, MATERIAL_GPU, TEX_RESOLVE, RENDER_SNAPSHOT)
give agent-verifiable pass/fail on every stage; a pixel-diff at mc2_17 f600 catches visual regressions.

---

## What I Verified vs. Assumed

### Verified from source

- `drawOldWay = false` at `mclib/tgl.cpp:84` — the `gos_DrawTriangles` path is dead by default; all shapes flow through `mcTextureManager->addTriangle/addVertices/addRenderShape`.
- `bShadersDrawPathEnabled = false` at `mclib/tgl.cpp:97` — the GPU-VB/IB sub-path within `MultiTransformShape` is also dead by default. This is the path we are enabling.
- The `TG_TypeShape` already carries `vb_`, `ib_`, `vdecl_` (declared `mclib/tgl.h:565-567`). These are populated at load time (not verified where, see "assumed" below).
- CPU clip-space bake occurs in `TG_Shape::MultiTransformShape` at `mclib/tgl.cpp:1686-1729`, iterating `theShape->listOfTypeVertices[j].position` → `xformCoords.Multiply(pos, *shapeToClip)` → screen-space `gos_VERTEX listOfVertices[j]`.
- The `TG_RenderShape` path in `TG_Shape::Render` (`mclib/tgl.cpp:2751-2783`) submits the pre-built `theShape->vb_`/`ib_` with a `mvp_` (= `cur_shape2clip`, the object→clip matrix stored in `TG_Shape::cur_shape2clip`) and `mw_` (= `shapeToWorld`). The GPU VB stores local/object-space verts: confirmed from the `gos_tex_vertex_lighted.vert` shader (`shaders/gos_tex_vertex_lighted.vert:7-10`) which takes `layout(location=0) in vec3 pos` (object-space).
- **Two render loops in `TG_Shape::Render`:** (A) the `!drawOldWay` `addTriangle/addVertices` loop at lines 2607-2729 runs on pre-transformed `listOfVertices`; (B) the `bShadersDrawPathEnabled` + `ib_ && vb_` block at lines 2751-2783 also runs. Both run when `bShadersDrawPathEnabled=true`, causing double-submit — a known issue.
- Paint/team color is baked into the texture at load time via `mcTextureManager->loadTexture(..., paintInstance)` in `Mech3DAppearance::setPaintScheme` (`mech3d.cpp:1801`). It is not a per-draw uniform — it is a texture instance key. The `TG_RenderShape` path inherits this correctly because it uses the same `mcTextureNodeIndex`.
- Highlight (`aRGBHighlight`) is added to `listOfVertices[j].argb` during `MultiTransformShape` at `mclib/tgl.cpp:2240-2262`. It is baked into the CPU vertex. The GPU `vb_` does NOT carry the updated highlight value — it carries the static `aRGBLight` from load time.
- Lighting is also CPU-baked into `listOfTriangles[j].aRGBLight` per-tri during `MultiTransformShape`. The GPU-path re-gathers lights via `GatherLightsParameters` and passes a `light_data_buffer_index_` to the shader (`mclib/tgl.cpp:2777`); the shader does per-vertex lighting from object-space normals. So lighting is handled differently: CPU vs GPU path produce nominally equivalent output but via different mechanisms.
- `TG_RenderShape` path currently has a documented multi-texture bug (FIXME at tgl.cpp:2732, 2753) — only `listOfTypeTriangles[0].localTextureHandle` is used to pick the texture for the whole shape.
- Alpha/transparent shapes (`textureAlpha`, `alphaValue != 0xff`) are excluded from the `TG_RenderShape` block (`mclib/tgl.cpp:2733`). This exclusion must remain.
- Spotlight and window shapes are excluded from the `TG_RenderShape` block (`mclib/tgl.cpp:2733`). This must remain.
- Backface culling is CPU-side in `MultiTransformShape` (`mclib/tgl.cpp:2268-2278`, dot product of face normal with `backFacePoint`). Under the GPU-VB path this still runs — the VB/IB are only submitted if `listOfVisibleFaces` is populated. No change needed here.
- `TG_Shape` members `listOfVertices` (pool-allocated, per-frame), `listOfColors`, `listOfShadowTVertices`, `listOfTriangles`, `listOfVisibleFaces`, `listOfVisibleShadows` are all ephemeral frame-pool allocations reset per frame.
- `TG_TypeShape` members `listOfTypeVertices`, `listOfTypeTriangles` are persistent load-time data (original object-space).
- The current shader (`shaders/gos_tex_vertex_lighted.vert`) takes object-space `pos` and applies `wvp_` → clip-space inline. It does not use the pre-transformed `screen.x/y/z/rhw` layout that `gos_VERTEX` / `addTriangle` feeds through. These are two completely separate vertex paths in the GPU backend.

### Assumed / not verified

- Where `TG_TypeShape::vb_` and `ib_` are populated (presumably `ParseASEFile`/`LoadTGShapeFromASE` or a post-load `CreateGPUBuffers` call). This must be confirmed before Stage 2 to ensure all mech shapes have valid `vb_`/`ib_` on the code path we enable.
- The `gos_RenderShapeManager` flush path (how `TG_RenderShape` records are drawn by `txmmgr.cpp:renderLists()`). The plan notes that the `addRenderShape` call at `mclib/tgl.cpp:2779` and the legacy `addRenderShape` at line 2535 differ: line 2779 takes a `TG_RenderShape*` (full struct with vb/ib/mvp); line 2535 takes only `(nodeId, flags)` — the latter is the old path that uses `cur_shape2clip` stored in the `TG_Shape` instance. These two overloads in `txmmgr.h:505` and `txmmgr.h:1035` must be carefully distinguished.
- Whether the `vb_`/`ib_` currently populated on `TG_TypeShape` are correct (same vertex layout as `gos_tex_vertex_lighted.vert` expects). Must be verified at Stage 1 before enabling.
- Whether buildings, turrets, artillery, and vehicles flow through `TG_Shape::Render` with the same code path. From the search pattern all `->Render(true)` calls in `mech3d.cpp` go to `TG_MultiShape::Render` → `TG_Shape::Render`. Other game objects likely do the same. Stage 3 expansion must verify this.
- The `GatherLightsParameters` function body (not read) — assumed to mirror `TG_Shape::SetLightList` world-light iteration, filling `TG_HWLightsData`.
- The `bShadersDrawPathEnabled` block at lines 2504-2536 (old style, no `TG_RenderShape*`) vs the block at 2751-2783 (new style). The new block at 2751-2783 seems to be the correct one to use. The old block at 2504-2536 should remain a warning sign — it will double-submit if both are enabled simultaneously.

---

## 1. Current Render Path (Exact Call Chain)

### 1.1 Entry

`Mech3DAppearance::render()` (`mclib/mech3d.cpp:2378`)
→ `mechShape->Render(true)` — `mechShape` is `TG_MultiShape*`

### 1.2 `TG_MultiShape::Render` (`mclib/msl.cpp:1706`)

For each `listOfShapes[i]` (shape node in the hierarchy):
1. Optionally refreshes texture handles: `listOfShapes[i].node->myType->SetTextureHandle(j, ...)` (`msl.cpp:1718`).
2. Computes per-shape matrix: `shapeToClip.Multiply(listOfShapes[i].shapeToWorld, TG_Shape::s_worldToClip)` (`msl.cpp:1724`).
3. Calls `listOfShapes[i].node->Render(forceZ, isHudElement, alphaValue, isClamped, &shapeToClip, &shape2world)` (`msl.cpp:1727`).

Note: `s_worldToClip` is the combined world→clip matrix set once per frame by `TG_Shape::SetCameraMatrices` (`tgl.cpp:1549`).

### 1.3 `TG_Shape::Render` (`mclib/tgl.cpp:2561`)

Early-outs: no vertices, pools not allocated, not transformed this turn.

**Phase A — `MultiTransformShape` (already called before `Render`):**
The actual CPU transform happens in `TG_Shape::MultiTransformShape` (`tgl.cpp:1634`), which is called by `TG_MultiShape` BEFORE `Render` in the transform pass (see `msl.cpp` transform loop). It:
- Allocates frame-pool slices for `listOfVertices`, `listOfColors`, `listOfShadowTVertices`, `listOfTriangles`, `listOfVisibleFaces`, `listOfVisibleShadows` (`tgl.cpp:1665-1672`).
- For each vertex j: reads `theShape->listOfTypeVertices[j].position` (object-space), multiplies by `*shapeToClip` → `xformCoords` → perspective divide → stores to `listOfVertices[j].{x,y,z,rhw}` (`tgl.cpp:1686-1729`). **This is the CPU clip-space bake.**
- Per-vertex lighting: fills `listOfVertices[j].argb` with CPU-computed lit color, adds `aRGBHighlight` (`tgl.cpp:2240-2262`).
- Backface cull: builds `listOfVisibleFaces[]` (`tgl.cpp:2268-2278`).
- Per-face lighting baked into `listOfTriangles[j].aRGBLight` (`tgl.cpp:2280-2499`).

**Phase B — `TG_Shape::Render` submission (active path):**

The `!drawOldWay` branch (`drawOldWay=false` → this branch always taken):
- Iterates `listOfVisibleFaces` (`tgl.cpp:2607`).
- Assembles `gos_VERTEX gVertex[3]` from pre-transformed `listOfVertices[triType.Vertices[i]]` + per-face UV + per-face `tri.aRGBLight`.
- For spotlight: `mcTextureManager->addVertices(0xffffffff, gVertex, MC2_ISSPOTLGT)` (`tgl.cpp:2704`).
- For window: `mcTextureManager->addVertices(0xffffffff, gVertex, MC2_DRAWALPHA)` (`tgl.cpp:2708`).
- For opaque: `mcTextureManager->addVertices(texNodeIndex, gVertex, MC2_DRAWSOLID | addFlags)` (`tgl.cpp:2720`).
- For alpha: `mcTextureManager->addVertices(texNodeIndex, gVertex, MC2_DRAWALPHA | addFlags)` (`tgl.cpp:2717`).

**GPU-VB/IB sub-path (`bShadersDrawPathEnabled`, currently OFF):**

Lines 2751-2783: guarded by `!isSpotlight && !isWindow && !textureAlpha && alphaValue==0xff` AND `theShape->ib_ && theShape->vb_`.
- Stores `cur_shape2clip` (`tgl.cpp:2532`) and viewport.
- Fills `TG_RenderShape rs` with `vb_`, `ib_`, `vdecl_`, `mvp_` (= `cur_shape2clip` possibly forceZ-adjusted), `mw_` (= shapeToWorld), viewport, `light_data_buffer_index_`.
- Calls `mcTextureManager->addRenderShape(texNodeIndex, &rs, MC2_DRAWSOLID | addFlags)` (`tgl.cpp:2779`).

### 1.4 Final GL draw

`mcTextureManager->renderLists()` (called from `txmmgr.cpp`) issues the actual GL draw calls by iterating the sorted/bucketed triangle/shape lists accumulated by `addVertices`/`addRenderShape`. The TG shader for the GPU-VB path is `shaders/gos_tex_vertex_lighted.vert` / matching fragment.

### 1.5 Which path is active?

**Default build:** `drawOldWay=false`, `bShadersDrawPathEnabled=false`.
- The `addVertices` path (pre-transformed clip-space `gos_VERTEX`) is active.
- The GPU-VB `addRenderShape` path is NOT active.
- `drawOldWay` controls `gos_DrawTriangles` vs `addVertices` — `drawOldWay=false` → `addVertices` is used.

---

## 2. Data Model

### TG_TypeShape (shared, persistent, load-time)
- `listOfTypeVertices[numTypeVertices]` — original **object-space** positions + normals + `aRGBLight`
- `listOfTypeTriangles[numTypeTriangles]` — index triples, per-face UV, face normals, `localTextureHandle`
- `listOfTextures[numTextures]` — `{textureName, mcTextureNodeIndex, gosTextureHandle, textureAlpha}`
- `vb_`, `ib_`, `vdecl_` (`HGOSBUFFER`, `HGOSVERTEXDECLARATION`) — GPU buffers, populated at load time (**assumed**)
- `hotPinkRGB`, `hotYellowRGB`, `hotGreenRGB` — window/spotlight glow colors

### TG_Shape (instance, per-game-object)
- `myType` — pointer to shared `TG_TypeShape`
- `listOfVertices` — **frame-pool**, `gos_VERTEX[numVertices]`, pre-transformed clip-space — reset each frame
- `listOfColors` — frame-pool, per-vertex fog/spec scratch
- `listOfTriangles` — frame-pool, per-instance lit triangle records
- `listOfVisibleFaces` / `listOfVisibleShadows` — frame-pool, backface-culled index lists
- `cur_shape2clip`, `cur_viewport` — cached this-frame object→clip matrix + viewport (instance members, written by `MultiTransformShape`)
- `lightData_` (`TG_HWLightsData`) — per-instance light gather, used only by GPU-VB path
- `aRGBHighlight` — per-instance additive highlight color, baked into `listOfVertices[j].argb` during transform

### Frame pools
Pools are static members shared across all `TG_Shape` instances, reset once per frame:
- `vertexPool` → `gos_VERTEX*`
- `colorPool` → `TG_Vertex*`
- `shadowPool` → `TG_ShadowVertexTemp*`
- `trianglePool` → `TG_Triangle*`
- `facePool` → `DWORD*` (shared for visible faces + visible shadows)

### Are local-space verts available at render time?

Yes. `theShape->listOfTypeVertices[j].position` (on the `TG_TypeShape` / `myType`) is **always** available — it is the persistent load-time object-space data. The GPU `vb_` presumably stores the same data. The CPU-path only reads `listOfTypeVertices` during `MultiTransformShape` and writes out to the frame-pool `listOfVertices`. Both are accessible simultaneously.

### Do indices exist?

Yes. `TG_TypeShape::ib_` is a pre-built GPU index buffer (load-time). The CPU path uses `listOfVisibleFaces` (backface-culled DWORDs into `listOfTypeTriangles`) — this is a **filtered** subset, not a raw IB. The GPU `ib_` includes ALL triangles (pre-backface-cull) so backface culling must remain CPU-side or move to the shader (via face normal dot in the vertex shader, which is non-standard). Backface cull is currently CPU-side; the GPU-VB path does NOT subset the IB — it submits all triangles.

### Triangle ordering / sort step

For opaque shapes: `addVertices(..., MC2_DRAWSOLID)` — no sort, draw order irrelevant.  
For alpha shapes: `addVertices(..., MC2_DRAWALPHA)` — sorted internally by `renderLists`.  
The `TG_RenderShape` path (`addRenderShape`) is opaque-only (guarded by `!textureAlpha && alphaValue==0xff`). Sort is not a concern for Stage 2.

---

## 3. Migration Design — First Win (GPU Transform, No Instancing)

### Goals
A. Keep CPU cull/bounds (backface cull stays in `MultiTransformShape`).  
B. Keep existing material/texture resolution (mcTextureNodeIndex path unchanged).  
C. Stop CPU clip-space bake for shapes that go through the `TG_RenderShape` path.  
D. Upload local/object-space verts (already in `TG_TypeShape::vb_` — just need to confirm format).  
E. Send object→world→view→proj matrix to the shader (already done: `rs.mvp_` = `cur_shape2clip`, `rs.mw_` = shapeToWorld).  
F. Persistent or frame-cached IB: `TG_TypeShape::ib_` is already persistent load-time. Use it directly.

### What the new shader already does (from `gos_tex_vertex_lighted.vert`)
- Takes `layout(location=0) in vec3 pos` (object-space), `layout(location=1) in vec3 normal`, `layout(location=2) in vec4 aRGBLight`, `layout(location=3) in vec2 texcoord`.
- Applies `wvp_` to compute clip position.
- Computes `WorldPos` from `world_` matrix.
- Does GPU lighting via `calc_light` using the light SSBO index.

**This shader already expects object-space input and a matrix. The data model is already there.**

### What does NOT need to change in the shader for Stage 2
Nothing. The shader is already correct for the GPU-transform path. The only issue is that `ENABLE_VERTEX_LIGHTING` must be set, and `wvp_` must be bound to the `mvp_` from the `TG_RenderShape` struct.

### What remains legacy after Stage 2

- The entire `addTriangle` / `addVertices` loop (lines 2607-2729 in `Render`) — unchanged, still active for all non-mech shapes and for the `MC2_TG_GPU_XFORM=0` fallback.
- `MultiTransformShape` — still runs for ALL shapes regardless of flag (still needed for backface cull, `listOfVisibleFaces`, the highlight/lighting bake feeding the CPU path, and for the IB-less fallback).
- `bShadersDrawPathEnabled` — this old flag and its code block (lines 2504-2536) is NOT used. The Stage 2 path uses the lower block (lines 2751-2783) only.
- Alpha/window/spotlight shapes — always use the `addVertices` path.
- Buildings, vehicles, turrets, artillery — still use the `addTriangle` path until Stage 3.
- The R2b static-tree skip in `objmgr.cpp` — NOT touched.
- `clipSpaceFrustumAdmit` / M2 visibility — NOT touched.
- Static prop registry — NOT touched.

---

## 4. Risk List

| Risk | Why at risk | Oracle catch |
|------|-------------|--------------|
| **Paint / team color** | Paint is texture-instance-keyed, not a per-draw uniform. Under the GPU path the same `mcTextureNodeIndex` is used (verified). Risk: if the `vb_` was built before paint was applied to the texture, it may carry stale `aRGBLight` base colors. | `MECH_MATERIAL_GPU mismatches == 0` in smoke log |
| **Highlight (`aRGBHighlight`)** | CPU path bakes highlight into `listOfVertices[j].argb` during `MultiTransformShape`. GPU `vb_` carries static load-time `aRGBLight` — NO highlight. Mech selection / flash highlight will be silently lost. | User-visual item 1 (selection highlight); pixel diff at f600 (post-selection state) |
| **Alpha / blending** | GPU-VB path is excluded for `textureAlpha` shapes. Risk: any shape where `textureAlpha=true` but `ib_` exists will continue to use CPU path — correct. Risk: if the exclusion check at tgl.cpp:2733 is wrong for mixed-texture models (multi-texture FIXME), some alpha tris may be omitted entirely. | `MATERIAL_GPU mismatches == 0`; pixel diff |
| **MLR / GOSFX clipping** | GOSFX effects (smoke, wake, FX) go through `MLRClipper` / gosFX path, NOT through `TG_Shape::Render`. They are completely separate. No risk from S2. | `MC2_FX_COUNT_LOG` counts unchanged; pixel diff at water/wake frame |
| **Fog / water / wake** | Fog color (`fogRGB`) is set per-frame via `gos_SetRenderState(gos_State_Fog, fogRGB)` in `TG_Shape::Render`. The GPU shader does not currently use fog state. Shapes in fog zones will render without fog under the GPU path. | Pixel diff at f8000 (water/terrain scene) |
| **Old shader pre-transformed-vert assumption** | The `addTriangle`/`addVertices` path feeds `gos_VERTEX` with pre-transformed screen-space `x/y/z/rhw` into a different draw bucket (CPU-path pipeline in GameOS). These are separate pipelines; enabling the GPU path only adds an `addRenderShape` record — it does not modify the CPU path. Double-submit risk if both run for the same shape. | `RENDER_SNAPSHOT count_mismatch == 0` |
| **Multi-texture shapes** | `ib_` and `vb_` represent the full mesh; the `localTextureHandle` from `listOfTypeTriangles[0]` is used for the whole shape. Multi-texture models will draw with the first texture only. Known FIXME. | `MATERIAL_GPU mismatches`; pixel diff |
| **All dynamic families share `TG_Shape::Render`** | The same `Render` function is called by mechs, vehicles, turrets, artillery, buildings, and any `TG_MultiShape`. Enabling the GPU-VB path by a global flag touches all of them. Stage 2 must be **mech-only** gated; a family-type filter is needed before Stage 3 expansion. | `RENDER_SNAPSHOT` + pixel diff |
| **`vb_`/`ib_` may not be populated for all shapes** | If a shape was loaded without populating `vb_`/`ib_` (e.g., load path that skips GPU buffer creation), `theShape->ib_ && theShape->vb_` guard at tgl.cpp:2751 will correctly skip the GPU path and fall back to CPU. Not a regression, but means no GPU-path for those shapes. | Not a crash risk; just silent no-op |

---

## 5. Oracle Plan

### Baseline capture (do before any code change)

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev

# Counter oracle — tier1 baseline
py -3 scripts/run_smoke.py --tier tier1 --duration 30 --keep-logs
# Expected: all parity counters 0, PASS on all 5 missions

# FX counter oracle — mc2_17 @ 150s
MC2_FX_COUNT_LOG=1 py -3 scripts/run_smoke.py --mission mc2_17 --duration 150 --keep-logs
# Expected: lArmSmoke~958, wake~349 (match oracle-dynamic-pipeline-gate.md baseline)

# Pixel oracle — mc2_17 @ frame 600
MC2_SCREENSHOT_AT_FRAME=600 MC2_SCREENSHOT_PATH=A:/tmp/oracle_s2_before_f600.tga ^
  py -3 scripts/run_smoke.py --mission mc2_17 --duration 15 --keep-logs

# Optional second capture at frame 8000 (water/wake scene)
MC2_SCREENSHOT_AT_FRAME=8000 MC2_SCREENSHOT_PATH=A:/tmp/oracle_s2_before_f8000.tga ^
  py -3 scripts/run_smoke.py --mission mc2_17 --duration 160 --keep-logs
```

### After each stage — verification

```bash
# Counter gate (all stages)
py -3 scripts/run_smoke.py --tier tier1 --duration 30 --keep-logs
# PASS criterion: MECH_MATERIAL_GPU mismatches=0, MATERIAL_GPU mismatches=0,
#                 TEX_RESOLVE mismatches=0 oob=0, RENDER_SNAPSHOT all 0,
#                 Δdestroys=0, result=PASS on all 5

# FX gate (Stage 2+)
MC2_FX_COUNT_LOG=1 py -3 scripts/run_smoke.py --mission mc2_17 --duration 150 --keep-logs
# PASS criterion: lArmSmoke in [900,1020], wake in [300,400] (±10% tolerance on 150s run)

# Pixel diff — f600 (Stage 2+)
MC2_SCREENSHOT_AT_FRAME=600 MC2_SCREENSHOT_PATH=A:/tmp/oracle_s2_after_f600.tga ^
  py -3 scripts/run_smoke.py --mission mc2_17 --duration 15 --keep-logs
python -c "
from PIL import Image, ImageChops
import numpy as np
a = np.array(Image.open('A:/tmp/oracle_s2_before_f600.tga'))
b = np.array(Image.open('A:/tmp/oracle_s2_after_f600.tga'))
diff = np.abs(a.astype(int) - b.astype(int))
print(f'max diff={diff.max()} mean={diff.mean():.4f} nonzero_pct={100*np.count_nonzero(diff)/diff.size:.2f}%')
"
# PASS criterion: max diff < 5 (TGA round-trip tolerance); if > 10 investigate
```

### Explicit non-regression scope

The following are **explicitly NOT changed** by S2:
- R2b static-tree skip in `objmgr.cpp`
- Static prop registry / `gos_static_prop_batcher`
- M2 GPU-cull / `clipSpaceFrustumAdmit`
- Terrain pipeline (`MC2_TERRAIN_LOD_CHUNK`, `gos_terrain_indirect`)
- Shadow lane / water pipeline

---

## 6. Staged Patch Plan

### Hard Guardrail

**Do NOT start by making everything instanced. First win = move dynamic TG shapes from CPU
clip-space bake to GPU transform while PRESERVING visual output. Instancing/batching comes AFTER
visual parity (Stage 4+). First patch (Stage 2) must be reversible + opt-in behind `MC2_TG_GPU_XFORM`,
NOT a renderer replacement.**

---

### Stage 0 — Instrumentation Only (no behavior change)

**Goal:** Measure the CPU-transform hotspot. Prove that `MultiTransformShape` is a meaningful cost
on large maps (1K) and on mc2_17 mech combat. Zero behavior change.

**Files touched:**
- `mclib/tgl.cpp` — add Tracy zone in `TG_Shape::MultiTransformShape` body (between pool alloc and return)
- `mclib/tgl.cpp` — add a counter of total vertices transformed per frame (env `MC2_TG_XFORM_STATS`, default OFF)
- `mclib/msl.cpp` — add Tracy zone on `TG_MultiShape::Render` loop body (one zone per shape)

**Tracy zone names:**
- `TGShape::MultiTransformShape` — wraps the vertex loop `for (long j=0;j<numVertices;j++)`
- `TGMultiShape::Render` — wraps the per-shape loop body in `TG_MultiShape::Render`

**Counter (env `MC2_TG_XFORM_STATS=1`):**
```
[TG_XFORM_STATS v1] frames=N total_verts_transformed=M shapes=K avg_verts_per_shape=F
```
Emit via `printf` at atexit (mirror `MC2_FX_COUNT_LOG` pattern).

**Env flag:** `MC2_TG_XFORM_STATS` (default OFF, zero behavior change when unset)

**Oracle checks:** tier1 5/5 PASS with and without `MC2_TG_XFORM_STATS=1`. FPS unchanged.

**Rollback:** remove the Tracy zone + counter, or just leave (zero cost when Tracy disabled + env unset).

---

### Stage 1 — Shader Accepts Matrix Path, Old Path Remains Default

**Goal:** Confirm that `TG_TypeShape::vb_`/`ib_`/`vdecl_` are populated correctly for mechs,
and that the `TG_RenderShape` submit path in `addRenderShape(nodeId, rs, flags)` dispatches
correctly through `renderLists()`. No shape switches to the new path yet.

**Files touched:**
- `mclib/tgl.cpp` — add a one-time debug log at the first shape that has `vb_ && ib_`, printing the `numTypeVertices`, `vb_` handle, `ib_` handle, and `vdecl_` handle. Gate on `MC2_TG_GPU_XFORM_DEBUG=1`.
- `mclib/tgl.cpp` — confirm the `cur_shape2clip` store at line 2532 is reached when `bShadersDrawPathEnabled=true` by adding a once-per-mech printf guarded on `MC2_TG_GPU_XFORM_DEBUG`. DO NOT enable `bShadersDrawPathEnabled` globally — it will double-submit.
- Optionally: add a static assertion that `sizeof(TG_HWTypeVertex)` (the GPU VB element type) matches the stride in `vdecl_`.

**Env flag:** `MC2_TG_GPU_XFORM_DEBUG` (default OFF)

**Oracle checks:** tier1 5/5 PASS with and without `MC2_TG_GPU_XFORM_DEBUG=1`. Debug log confirms `vb_ != 0` and `ib_ != 0` on at least one mech shape in mc2_01.

**Rollback:** unset env flag; no production code path changed.

---

### Stage 2 — Mechs-Only GPU Transform Behind `MC2_TG_GPU_XFORM`

**Goal:** Enable the `TG_RenderShape` GPU-VB path for mechs only. Suppress the duplicate `addTriangle` loop for the same shapes. Visual parity with CPU path for opaque mech geometry (no alpha, no spotlight, no highlight — those are flagged as known regressions to be addressed later).

**Files touched:**

1. **`mclib/tgl.h`** — add to `TG_Shape` class:
   ```cpp
   static bool s_gpuXformEnabled; // MC2_TG_GPU_XFORM env gate
   ```
   Add to `TG_TypeShape` class (or to `TG_MultiShape`):
   ```cpp
   bool isMechShape_; // true if this shape is part of a mech MultiShape
   ```
   Alternatively, filter at the `TG_MultiShape::Render` call site in `mech3d.cpp` by passing a `bool isMech` parameter to `TG_Shape::Render`.

2. **`mclib/tgl.cpp`** — read `MC2_TG_GPU_XFORM` env at startup (alongside other env reads), set `TG_Shape::s_gpuXformEnabled`.

3. **`mclib/tgl.cpp:TG_Shape::Render`** — around the `for (long j=0;j<numVisibleFaces;j++)` loop at line 2607 and the `bShadersDrawPathEnabled` block at line 2504:
   - Preserve the existing loop structure.
   - **New logic:** If `s_gpuXformEnabled && isMechShape && theShape->ib_ && theShape->vb_ && !isSpotlight && !isWindow && !textureAlpha && alphaValue==0xff`:
     - SKIP the `addTriangle`/`addVertices` loop (CPU path) for opaque faces of this shape.
     - Execute the `TG_RenderShape` block (lines 2751-2783 with the `addRenderShape(nodeId, &rs, flags)` overload).
   - Alpha, spotlight, window faces: always use CPU path.
   - `MC2_TG_GPU_XFORM=0` or `vb_==0` or `ib_==0`: always use CPU path.

4. **`mclib/mech3d.cpp:Mech3DAppearance::render()`** — before calling `mechShape->Render(true)` at line 2378, call a new setter on `mechShape` to mark it as a mech shape (if the `isMechShape_` approach is used):
   ```cpp
   mechShape->SetIsMechShape(true); // new method; no-op when s_gpuXformEnabled=false
   ```
   Or pass a flag through `TG_MultiShape::Render` → `TG_Shape::Render` via the existing parameter chain.

**Multi-texture FIXME:** The `TG_RenderShape` submit at line 2779 uses only `listOfTypeTriangles[0].localTextureHandle`. This is a pre-existing limitation. Document it in the plan; do NOT fix it in Stage 2 (separate issue, tracked under Stage 4).

**Highlight regression:** `aRGBHighlight` is baked into the CPU `listOfVertices[j].argb` but NOT into the GPU `vb_` (which stores static `aRGBLight`). Under `MC2_TG_GPU_XFORM=1`, mech selection highlights will be invisible. **This must be documented as a known gap; user-visual item 1 will FAIL at Stage 2.** A follow-up patch must either: (a) update the GPU VB each frame with highlight-baked data, or (b) pass `aRGBHighlight` as a uniform to the shader and blend it in the fragment shader.

**Env flag:** `MC2_TG_GPU_XFORM=1` enables; default OFF.

**Oracle checks:**
```bash
# Counter gate
MC2_TG_GPU_XFORM=1 py -3 scripts/run_smoke.py --tier tier1 --duration 30 --keep-logs
# PASS: MECH_MATERIAL_GPU mismatches=0, MATERIAL_GPU mismatches=0,
#       TEX_RESOLVE 0/0, RENDER_SNAPSHOT all 0, Δdestroys=0

# FX gate (effects must still draw from the CPU path)
MC2_TG_GPU_XFORM=1 MC2_FX_COUNT_LOG=1 \
  py -3 scripts/run_smoke.py --mission mc2_17 --duration 150 --keep-logs
# PASS: lArmSmoke in [900,1020], wake in [300,400]

# Pixel diff (flag-on vs baseline)
MC2_TG_GPU_XFORM=1 MC2_SCREENSHOT_AT_FRAME=600 \
  MC2_SCREENSHOT_PATH=A:/tmp/oracle_s2_on_f600.tga \
  py -3 scripts/run_smoke.py --mission mc2_17 --duration 15 --keep-logs
# diff against oracle_s2_before_f600.tga
# NOTE: some visible diff EXPECTED due to highlight regression + GPU-vs-CPU lighting
# differences. Document the diff magnitude; user must visually confirm mechs are
# correctly painted + lit (no black mechs, no texture artifacts).
```

**Rollback:** `MC2_TG_GPU_XFORM=0` (default) restores CPU path exactly.

---

### Stage 3 — Expand to Dynamic Movers (Vehicles, Turrets, Artillery)

**Prerequisite:** Stage 2 passes all counter oracles AND user visual confirmation of mech parity.

**Goal:** Enable GPU transform for all `TG_MultiShape` families that go through `TG_Shape::Render` with valid `vb_`/`ib_`. Remove the mech-only filter.

**Files touched:**
- `mclib/tgl.cpp` — remove the `isMechShape_` gate (or widen it to all shapes with `vb_&&ib_`).
- `mclib/mech3d.cpp` — remove the `SetIsMechShape(true)` call if no longer needed.
- Verify other `TG_MultiShape::Render` call sites (vehicle, turret, building appearances) are reached and their shapes have valid GPU buffers.

**Env flag:** `MC2_TG_GPU_XFORM=1` (same flag, wider coverage)

**Oracle checks:** Same as Stage 2 plus a second pixel capture at a mission with vehicles/turrets (mc2_03 or mc2_10 have varied enemy types):
```bash
MC2_TG_GPU_XFORM=1 MC2_SCREENSHOT_AT_FRAME=600 \
  MC2_SCREENSHOT_PATH=A:/tmp/oracle_s3_mc2_10_f600.tga \
  py -3 scripts/run_smoke.py --mission mc2_10 --duration 15 --keep-logs
```

**Rollback:** `MC2_TG_GPU_XFORM=0`

---

### Stage 4 — Persistent IB and Fix Known Gaps (Multi-Texture, Highlight, Batching)

**Prerequisite:** Stage 3 passes all oracles.

**Goals (in order of priority):**
1. Fix highlight regression: pass `aRGBHighlight` as a per-shape uniform (`vec4 highlight_add_`) to the shader; blend it in `gos_tex_vertex_lighted.frag`.
2. Fix multi-texture: split `TG_RenderShape` submission per texture group (or store per-face texture assignments in the GPU draw; the simplest fix is one `addRenderShape` call per unique `localTextureHandle` group). This requires knowing the texture-to-face mapping — `listOfTypeTriangles` provides it.
3. Enable alpha shapes: the exclusion at tgl.cpp:2733 can be lifted if the GPU-path is configured to submit to the alpha bucket (`MC2_DRAWALPHA`) with correct sort key.
4. Persistent IB: `TG_TypeShape::ib_` is already persistent. Verify it is never invalidated between frames (it should be since it's load-time). The CPU frame-pool `listOfVisibleFaces` is still needed for backface cull input — this does not change.

**Files touched:**
- `shaders/gos_tex_vertex_lighted.vert` — add `uniform vec4 highlight_add_`, apply in output color.
- `shaders/gos_tex_vertex_lighted.frag` — receive and apply highlight in final color.
- `mclib/tgl.h:TG_RenderShape` — add `float highlight_[4]` field.
- `mclib/tgl.cpp:TG_Shape::Render` — populate `rs.highlight_` from `aRGBHighlight`.
- `mclib/txmmgr.cpp:renderLists()` — bind `highlight_add_` uniform before GPU-VB draw.

**Env flag:** `MC2_TG_GPU_XFORM=1` (same); `MC2_TG_GPU_XFORM_ALPHA=1` to opt-in alpha shapes to GPU path.

**Oracle checks:** User visual item 1 (selection highlight) must now PASS.

**Rollback:** `MC2_TG_GPU_XFORM=0`

---

### Stage 5 — Fold in S4 Material SSBO

**Prerequisite:** Stage 4 passes all oracles including visual items 1-4.

**Goal:** Replace per-draw texture binds with the material SSBO prepared by the S4 material arc. This stage is defined by the S4 material SSBO design (out of scope for this plan). The `TG_RenderShape` struct gains a `material_index_` field replacing the `mcTextureNodeIndex` lookup.

**Files touched:** TBD by S4 material arc plan.

**Env flag:** New flag from S4 arc (e.g., `MC2_MATERIAL_SSBO=1`), orthogonal to `MC2_TG_GPU_XFORM`.

**Oracle checks:** All existing counters + `MECH_MATERIAL_GPU mismatches=0` is the load-bearing check for S4.

---

## 7. Summary Table

| Stage | Env Flag | Files | Key Change | Oracle |
|-------|----------|-------|------------|--------|
| 0 | `MC2_TG_XFORM_STATS` | `tgl.cpp`, `msl.cpp` | Tracy zones + vertex counter | tier1 5/5 PASS |
| 1 | `MC2_TG_GPU_XFORM_DEBUG` | `tgl.cpp` | Debug log: vb/ib populated? | tier1 5/5 PASS + log shows vb != 0 |
| 2 | `MC2_TG_GPU_XFORM` | `tgl.cpp`, `tgl.h`, `mech3d.cpp` | Mechs → GPU VB path; CPU path skipped for opaque | tier1 5/5 + pixel diff + FX counts |
| 3 | `MC2_TG_GPU_XFORM` | `tgl.cpp` | All dynamic families → GPU VB path | Same + mc2_10 pixel diff |
| 4 | `MC2_TG_GPU_XFORM[_ALPHA]` | `tgl.cpp`, `tgl.h`, shaders | Highlight uniform, multi-texture fix, alpha opt-in | All oracles + user visual items 1-4 |
| 5 | `MC2_MATERIAL_SSBO` | `tgl.cpp`, `txmmgr.cpp`, shaders | S4 material SSBO integration | `MECH_MATERIAL_GPU mismatches=0` |

---

## 8. Architecture Review (Opus, 2026-06-09) — APPROVED with 2 blocking conditions

Design is concrete and executable. The key reframing — the GPU-VB/`TG_RenderShape` path already
exists as `bShadersDrawPathEnabled`-guarded dead code, so S2 = enable+de-risk, not rewrite — is
correct and materially lowers risk/effort. Staging, env-gating, reversibility, and per-stage oracle
gates are all sound. The highlight-regression catch (aRGBHighlight baked into CPU vertex, absent
from static `vb_`) is exactly the kind of latent trap the oracle exists to catch. Approved to execute
**Stage 0 immediately**, but TWO conditions gate Stage 2:

### BLOCKER 1 — Stage 0 is a GO/NO-GO measurement gate, not just instrumentation.
Do NOT assume `MultiTransformShape` is the hotspot. **Strong project prior: the analogous terrain
premise ("slimReduce is the O(n²) cost") INVERTED when measured (`MC2_SLIM_COST_SPLIT`) — it was 0
cycles under the flag.** Stage 0 must answer, with Tracy + the vertex counter, on BOTH mc2_17 mech
combat AND a 1K map: is per-frame CPU clip-space bake actually a top-N CPU cost (e.g. >1ms / >5% frame)?
- If YES → proceed to Stage 1.
- If NO → S2 drops in priority exactly as S1 did; stop and re-rank. Do not migrate a non-hotspot.
This must be an explicit gate with a recorded number, not a formality.

### BLOCKER 2 — Resolve the lighting-parity question before claiming "visual parity."
The plan notes (correctly) that the CPU path bakes per-vertex/per-face lighting into `argb`, while the
GPU path re-computes lighting via `calc_light` + the light SSBO. These are "nominally equivalent via
different mechanisms" — meaning the f600 pixel diff WILL show non-zero delta even with a *correct*
migration (the plan already admits "some visible diff EXPECTED"). That breaks the core gate: you can't
distinguish a real regression from the CPU↔GPU lighting math difference. Decide UP FRONT:
- (a) Quantify the expected lighting delta on a static frame (mech lit identically both paths) and set a
  tolerance band the pixel diff must fall within; OR
- (b) Declare the GPU path the new reference and RE-BASELINE the oracle screenshots after Stage 2,
  with a one-time human confirmation that the GPU lighting looks correct (no black/over-bright mechs).
Until this is decided, "preserve visual output" is unfalsifiable. Recommend (a) if the delta is small,
(b) if GPU lighting is intentionally better — but make it an explicit, recorded decision in Stage 2.

### Secondary notes (non-blocking)
- **Shadows:** `MultiTransformShape` keeps running for ALL shapes (backface cull + shadow verts + CPU
  fallback); the GPU path only replaces the COLOR submit. Verify in Stage 2 that the shadow pass
  (`renderShadows`, `listOfShadowTVertices`) is unaffected — confirm shadows still present in the f600
  diff. Likely fine, but make it an explicit check.
- **Double-submit:** the old `bShadersDrawPathEnabled` block (lines 2504-2536) AND the new one
  (2751-2783) must never both fire for one shape. Stage 2 should route through exactly one, and ideally
  delete/neuter the old block to remove the foot-gun.
- **Stage 2 is NOT cutover-ready** (ships highlight regression behind the flag). Flag stays default-OFF
  through Stage 3; Stage 4 (highlight uniform) is the earliest default-on candidate. Plan already says
  this — keep it loud.
- Mech-only gating: pick ONE mechanism (recommend threading an `isMech` bool through the existing
  `TG_MultiShape::Render → TG_Shape::Render` param chain — less state than an `isMechShape_` member).

**Verdict:** Execute Stage 0 now as a measured GO/NO-GO. Resolve Blocker 2 before writing Stage 2 code.
No implementation beyond Stage 0 instrumentation until both blockers clear.

---

## 9. STAGE 0 RESULTS — Measured 2026-06-09 → **NO-GO for S2 as a perf win**

Measured `TG_Shape::MultiTransformShape` (the CPU clip-space vertex bake) via the pre-existing
`MC2_OBJECT_RECON_TRACY` per-frame `shape={ns,calls}` accumulator (it already instruments this exact
function; my added `MC2_TG_XFORM_STATS` counter did not emit — minor bug, OBJECT_RECON supersedes it).
All runs: tier1 5/5 PASS with the flag on, Δdestroys=0 → instrumentation is behavior-neutral.

Per-frame `MultiTransformShape` cost (peak across the run):

| Mission | peak/frame | typical | peak shape calls | fps | % of ~14ms frame |
|---------|-----------|---------|------------------|-----|------------------|
| 1kbasicmap (1K map) | **0.018 ms** | 0.016 ms | 2 | **18 (55ms/frame)** | **0.03%** |
| mc2_17 (mech combat) | 0.159 ms | ~0.03 ms | 184 | 74 | ~1% |
| mc2_10 (densest objects) | 1.19 ms | ~0.5 ms | 262 | 69 | 3.5–8% (peak only) |
| mc2_03 | 0.34 ms | ~0.2 ms | 205 | 71 | ~2.4% |
| mc2_01 / mc2_24 | ~0.002 ms | 0.002 ms | 2 | 82 / 72 | ~0% |

### Conclusion (Blocker 1 → NO-GO)
- **The CPU clip-space bake is NOT a dominant hotspot.** Worst case is ~1.2ms in a *peak* frame on the
  single most object-dense normal map (mc2_10, 262 buildings+mechs); typically ~0.5ms; negligible
  (<0.16ms) everywhere else.
- **The 1K map disproves the original premise outright:** at 18fps / 55ms-per-frame it spends 0.018ms
  (2 dynamic shapes) in MultiTransformShape. The 1K slowness is terrain-bound, not TG-dynamic-bound.
  (The terrain per-vertex cost was a *different* system — slimReduce — already addressed by the
  TerrainLOD chunk arc.)
- Best realistic S2 saving: ~0.5–1ms on the densest object map, ~0 elsewhere. Against the migration's
  real risks (highlight regression, CPU↔GPU lighting parity, multi-texture FIXME, all-families blast
  radius), **the payoff does not justify the work.**

### Recommendation
1. **Do NOT proceed to Stage 1/2.** S2 drops in priority (same outcome as S1).
2. If perf is the goal, the measured hotspot is **elsewhere** — the 1K map's 55ms/frame is terrain/
   present-bound. Profile terrain + SwapWindow/present (a ~45ms present stall is noted at
   gameosmain.cpp:1544) before any dynamic-object render rework.
3. Keep Stage 0 instrumentation as a cheap diagnostic OR revert it (OBJECT_RECON already covers the
   measurement) — disposition TBD by user.
4. The GPU-VB/`TG_RenderShape` dead-code path remains available if a *correctness* (not perf) reason to
   activate it appears later (e.g. unifying the draw path), but that is not S2's current justification.
