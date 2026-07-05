# SSBO Slot 14 / Slot 20 Split Plan — BINDING-SLOT-14-20-SPLIT-PLAN-1

---

## CORRECTION (source-verified, supersedes the original slot-20 analysis)

**The original §1/§2/§3/§4 slot-20 analysis below is WRONG on two counts and is
SUPERSEDED. Slot 20 is BENIGN — same category as slot 14 — not a "real Vulkan blocker."**
The slot-14 analysis was correct and stands. The reasoning trail is preserved below for
audit; do not act on the original slot-20 "20→21 move."

### Corrected verdict for slot 20

`SurfaceVertexBuf@20` (terrain-surface pipeline) and `LightsData@20` (lit pipelines:
mech / static-prop / building-pbr / gos_tex_vertex_lighted) live in **DISJOINT pipelines**.
No single descriptor-set layout names both at binding 20. This is per-pipeline-isolatable
under Vulkan descriptor layouts — exactly the slot-14 A/B/C situation. `check-vulkan-bindings.py`'s
`SSBO:20 roles=SurfaceVertexBuf,LightsData` is a **whole-corpus physical-slot aggregation**
(a known Vk-prep TODO, exit 0), benign, not a same-pipeline collision.

**No renumber is REQUIRED.** The behavior-changing follow-up (BINDING-SLOT-14-20-SPLIT-1)
is not needed for correctness. IF a physical relabel is ever wanted purely for Vulkan
descriptor hygiene, the free SSBO slots are **27–30** (NOT 21). This is now a low-priority
user-decision, not a blocker.

### ERROR 1 — "move SurfaceVertexBuf 20 → 21" would ALIAS SurfaceIndexBuf (terrain regression)

Binding 21 is **NOT free** in the terrain-surface pipeline. The terrain-surface draw binds
THREE SSBOs in one draw:

- `20 = SurfaceVertexBuf` — `shaders/gos_terrain_surface.vert:60`; C++ bind
  `GameOS/gameos/gameos_graphics.cpp:3923` `glBindBufferBase(...,20,s_surfaceVB)`
- `21 = SurfaceIndexBuf` — `shaders/gos_terrain_surface.vert:63`; C++ bind
  `GameOS/gameos/gameos_graphics.cpp:3955` `glBindBufferBase(...,21,s_surfaceIB)`
- `22 = SurfaceTileBuf` — `shaders/gos_terrain_surface.vert:78`; C++ bind
  `GameOS/gameos/gameos_graphics.cpp:3932` `glBindBufferBase(...,22,s_surfaceTB)`

All three unbind-to-0 symmetrically at `gameos_graphics.cpp:3974-3976`. The header comment
`gos_terrain_surface.vert:56-59` explicitly documents the 20/21/22 private high-range choice.
Moving `SurfaceVertexBuf` to 21 would collide with `SurfaceIndexBuf` in the SAME pipeline —
a real regression, the exact opposite of a fix.

### ERROR 2 — the claimed SAME-PIPELINE 20-A/20-B collision does not exist

Terrain does NOT include `lighting.hglsl`. `gos_terrain.frag:5-9,52,159` includes only
`shadow`, `noise`, `render_contract`, `terrain_depth_bias`, `edge_haze`, `terrain_mat_layers`,
`terrain_height_normal` — verified none of those transitively pull `lighting.hglsl` or declare
`binding = 20`. `LightsData` (`shaders/include/lighting.hglsl:63`, `binding =
LIGHT_DATA_SSBO_BINDING` = 20) is included ONLY by `mech.*`, `static_prop.*`, `building_pbr.*`,
`gos_tex_vertex_lighted.*` (and mirrored read-only in `lightgrid_build.comp`) — NOT terrain.
So `SurfaceVertexBuf@20` and `LightsData@20` never coexist in one pipeline's descriptor layout.
The original §2 claim "the terrain pass IS a lit pass ... includes lighting.hglsl" is false.

### SSBO occupancy (source-verified)

- terrain-surface pipeline: **20/21/22** (`gos_terrain_surface.vert:60/63/78`)
- LOD-chunk pipeline: **23/24/25/26** (`terrain_lod_chunk.vert:25/30/36`,
  `terrain_lod_chunk.frag:70/75`)
- `LightsData@20` in the disjoint lit pipelines (no overlap with terrain)
- globally-free relabel targets: **27, 28, 29, 30**

---

**Status: DOCUMENT-ONLY.** This slice changes NO runtime binding, no shader `layout`
number, no C++ constant, not even a comment marker. It reports the collisions, classifies
each occupancy BENIGN vs REAL-Vulkan-blocker, and proposes a post-split per-pipeline
descriptor binding map. The behavior-changing follow-up is **BINDING-SLOT-14-20-SPLIT-1**
(scoped in §4, explicitly NOT part of this doc).

Splitting is supervised. Under GL a binding-base slot is per-pass, so the multiplexing here
is legal today (see `binding-slot-occupancy.md`). The Vulkan concern is that a descriptor-set
layout pins a fixed `(set, binding)` per pipeline; a slot carrying DISTINCT live resources in
the SAME or overlapping pipeline layout is a descriptor collision that must be split before
descriptor assignment.

Resource-ID authority cross-referenced below is `RenderCore/RenderResourceRegistry.h`
(`RenderResourceId` enum). There is no `RenderCore/vulkan_contract.h` in the tree; the recon's
"vulkan_contract.h resource IDs" resolves to that registry enum.

---

## 1. Collision report (code-confirmed)

### Slot 14 — SSBO binding base 14

Named constant: `gpu_cull::READBACK_SSBO_BINDING = 14u`
(`GameOS/gameos/gpu_cull_readback.h:18`). Single source of truth; injected into
`gpu_cull.comp` via `#define` built in `gpu_cull_compute.cpp:284`.

| # | Resource | Pass / pipeline | Bind site | Lifecycle |
|---|---|---|---|---|
| 14-A | GPU-cull async readback ring (`ReadbackBuf`) | `gpu_cull.comp` compute dispatch (C2 readback permutation, `GPU_CULL_C2_READBACK`) | bind: `gpu_cull_compute.cpp:1017` via `glBindBufferRange` (ranged, per-slot); GLSL `shaders/gpu_cull.comp:146` `binding = READBACK_SSBO_BINDING`; unbind-to-0: `gpu_cull_compute.cpp:1366` | Per-dispatch. Bound ranged before dispatch, unbound to base-0 in the symmetric teardown block (`:1360-1366`). Ring buffer itself is persistent; the *binding* is per-frame. |
| 14-B | Particle billboard SSBO (`Particles`, `s_ssbo`) | particle bridge billboard draw; GLSL `shaders/particle_billboard.vert:26` `binding = 14` | bind: `gos_particle_bridge.cpp:1181` `glBindBufferBase(...,14,s_ssbo)`; unbind-to-0: `:1305` (D-01) | Per-flush. `s_ssbo` grown lazily (`:382-395`), rebound per flush group, unbound to 0 at flush end. |
| 14-C | Tube/ribbon position SSBO (`RibbonPos`, `s_tubePosSsbo`) | particle bridge tube-ribbon draw; GLSL `shaders/tube_ribbon.vert:19` `binding = 14` | bind: `gos_particle_bridge.cpp:578` and `:833` `glBindBufferBase(...,14,s_tubePosSsbo)`; unbind-to-0: `:616` and `:877` | Per-flush. Same slot as 14-B but a DIFFERENT buffer; each ribbon batch binds then unbinds to 0 symmetrically. |

All three consumers bind-then-unbind-to-base-0 symmetrically. Verified: no consumer observes
a stale slot-14 binding left by another (the D-01 unbind at `:1305` and the cull teardown at
`:1366` both zero the slot).

### Slot 20 — SSBO binding base 20

Named constant: `LIGHT_DATA_SSBO_BINDING = 20` — defined in **both**
`GameOS/include/gameos.hpp:2888` (C++) and `shaders/include/lighting.hglsl:15` (GLSL, true
lockstep pair). Also referenced as the literal/local const `kObjectLightsBinding = 20`
(`gos_lightgrid_build.cpp:33`).

| # | Resource | Pass / pipeline | Bind site | Lifecycle |
|---|---|---|---|---|
| 20-A | ObjectLights / `LightsData` SSBO (`s_lightDataSsbo`, `RenderResourceId::LightDataSsbo`=34) | ALL lit shaders — terrain, mech, static-prop — via `shaders/include/lighting.hglsl:63` `binding = LIGHT_DATA_SSBO_BINDING`; also consumed read-only by lightgrid-build stage 0 (`lightgrid_build.comp:71` `binding = 20` `LG_LightsData`) | bind: `gameos_graphics.cpp:8841/8872/8888/8907/8947/9001` `glBindBufferBase(...,LIGHT_DATA_SSBO_BINDING,s_lightDataSsbo)`; block binding wired at `:9035` | Per-mission (persistent). Bound at upload/mission start, stays bound for the frame; grow → handle recreated → re-registered + rebound. |
| 20-B | Terrain surface vertex buffer (`SurfaceVertexBuf`) | `shaders/gos_terrain_surface.vert:60` `layout(std430, binding = 20) readonly buffer SurfaceVertexBuf` | GLSL-declared; bound by the terrain-surface draw path's own VB setup | Per-draw (terrain surface pass). A DISTINCT buffer from 20-A. |

**Important nuance for slot 20:** `lightgrid_build.comp` *reads* slot 20 (20-A) read-only, but
`gos_lightgrid_build.cpp` never binds slot 20 itself — its own dispatch bind set uses LOCAL
slots 0/1/2 (`kSphereSsboSlot`/`kSphereCountSlot`/`kIndexPoolSlot`/`kCursorSlot`,
`:46-52`, bound at `:473-474`, `:500-502`). It relies on the engine's mission-persistent
slot-20 bind (`gameos_graphics.cpp`). So the lightgrid pipeline consumes 20-A but adds no
independent slot-20 occupant.

---

## 2. Classification — BENIGN vs REAL Vulkan blocker

| Occupancy pair | Verdict | Why |
|---|---|---|
| **14-A vs 14-B** (readback ring ↔ particle billboard) | **BENIGN** | Different pipelines (cull compute vs particle graphics). No pipeline's descriptor-set layout names both at binding 14. Symmetric bind/unbind-to-0 means no cross-pipeline aliasing under GL, and under Vulkan each pipeline gets its own layout — the readback pipeline binds `ReadbackBuf` at its own `(set,binding)`, the particle pipeline binds `Particles` at its own. Distinct pipelines → isolatable → not a descriptor collision. |
| **14-B vs 14-C** (particle billboard `Particles` ↔ tube-ribbon `RibbonPos`) | **BENIGN** | Two DIFFERENT buffers, but two DIFFERENT graphics pipelines (`particle_billboard.vert` vs `tube_ribbon.vert`) — never in one descriptor-set layout at the same time. Each Vulkan pipeline names its own buffer at its own binding. Symmetric per-flush unbind confirms no live overlap. |
| **20-A vs 20-B** (`LightsData` ↔ terrain `SurfaceVertexBuf`) | ~~**REAL Vulkan blocker**~~ **SUPERSEDED → BENIGN** (see top CORRECTION: terrain does NOT include lighting.hglsl; disjoint pipelines) | The terrain pass IS a lit pass: `gos_terrain_surface.vert` declares `SurfaceVertexBuf` at `binding = 20` **and** the terrain fragment/lighting path includes `lighting.hglsl`, which declares `LightsData` at `binding = LIGHT_DATA_SSBO_BINDING` = 20. Two DISTINCT live buffers claim binding 20 within the SAME terrain pipeline's descriptor-set layout. A single Vulkan `VkDescriptorSetLayout` cannot bind two different buffers to the same `(set,binding)`. This is a hard collision that must be split before terrain descriptor assignment. |
| 20-A across mech / static-prop / lightgrid-build | **BENIGN** | Those pipelines carry only ONE slot-20 occupant (`LightsData`). Same logical resource, per-pipeline isolatable. Lightgrid-build binds its working set on local slots 0/1/2, not 20. |

**~~Summary: exactly ONE real Vulkan blocker — 20-A vs 20-B inside the terrain-surface
pipeline.~~ SUPERSEDED — see top CORRECTION. There is ZERO real Vulkan blocker: 20-A/20-B
are in DISJOINT pipelines (terrain never includes lighting.hglsl), and binding 21 is
already SurfaceIndexBuf so the proposed 20→21 move is itself a regression.** All slot-14
shares and all slot-20 shares are benign (distinct pipelines, per-layout isolatable,
symmetric bind/unbind).

This matches — and refines — the `check-vulkan-bindings.py` VULKAN-COLLISION entries: the
slot-14 entry is a *reported-debt* item that this analysis reclassifies BENIGN (no single
pipeline aliases it); the slot-20 entry is the genuine blocker, localized to the terrain
pipeline.

---

## 3. Proposed post-split Vulkan binding map — SUPERSEDED (see top CORRECTION)

> **SUPERSEDED.** The "MOVE SurfaceVertexBuf → (0,21)" row below is WRONG: binding 21 is
> already `SurfaceIndexBuf` in the same pipeline. No move is required (slot 20 is benign).
> If a relabel is ever wanted, use a free slot (27–30), not 21. Retained for audit only.


Convention: `set 0` = per-frame/global resources; `set 1` = per-material/per-pass resources.
`LightsData` is a frame-global lit input → stays on a stable global binding across all lit
pipelines. Only the genuine collider (terrain `SurfaceVertexBuf`) moves.

Minimal-change principle: leave slot 14 and the `LightsData` slot-20 numbers alone (benign);
relocate ONLY terrain `SurfaceVertexBuf` off 20.

### Per-pipeline descriptor-set layouts (SSBO bindings shown)

| Pipeline | Buffer | (set, binding) post-split | Note |
|---|---|---|---|
| **cull compute** (`gpu_cull.comp`) | `ReadbackBuf` (14-A) | (0, 14) | unchanged; sole slot-14 occupant in this layout |
| **particle billboard** (`particle_billboard.vert`) | `Particles` (14-B) | (0, 14) | unchanged; sole occupant in this layout |
| **particle tube-ribbon** (`tube_ribbon.vert`) | `RibbonPos` (14-C) | (0, 14) | unchanged; sole occupant in this layout |
| **lightgrid-build** (`lightgrid_build.comp`) | `LG_LightsData` (20-A, read-only) | (0, 20) | unchanged; working set stays on local 0/1/2 |
| **mech** (`mech.*` + `lighting.hglsl`) | `LightsData` (20-A) | (0, 20) | unchanged |
| **static-prop** (`static_prop.*` + `lighting.hglsl`) | `LightsData` (20-A) | (0, 20) | unchanged |
| **terrain-surface** (`gos_terrain_surface.vert` + `lighting.hglsl`) | `LightsData` (20-A) | (0, 20) | unchanged — global lit input |
| **terrain-surface** (same pipeline) | `SurfaceVertexBuf` (20-B) | ~~**(0, 21) — MOVED off 20**~~ **WRONG — 21 is SurfaceIndexBuf; keep (0,20), no move needed** | ORIGINAL CLAIM FALSE: 21 is NOT free (it is `SurfaceIndexBuf`, `gos_terrain_surface.vert:63`); and 20-A/20-B are disjoint pipelines so there is no collision to resolve |

`LightsData` keeps `(0,20)` engine-wide so the shared `lighting.hglsl` include needs no
per-pipeline permutation. `SurfaceVertexBuf` is terrain-local and used by no other pipeline, so
relocating it to binding 21 is contained to two files (see §4). Verify 21 is unused in the
terrain-surface pipeline's SSBO namespace before committing the number (per
`binding-slot-occupancy.md`, slot 21 is not claimed on the terrain-surface path).

Cross-ref `RenderResourceRegistry.h`: `LightDataSsbo=34` (20-A). `SurfaceVertexBuf` (20-B) has
no registry id today; the split slice may add one for FrameGraph ownership tracking (optional,
not required for the descriptor fix).

---

## 4. Migration note — BINDING-SLOT-14-20-SPLIT-1 (NOT this slice) — SUPERSEDED (see top CORRECTION)

> **SUPERSEDED / NOT REQUIRED.** The edits below (moving `SurfaceVertexBuf` 20→21) are
> WRONG (21 = SurfaceIndexBuf) and unnecessary (slot 20 is benign, disjoint pipelines).
> No behavior-changing follow-up is needed for correctness. If Vulkan hygiene ever wants a
> physical relabel, target a free slot (27–30), not 21. Retained for audit only.


A future SUPERVISED behavior-changing slice would edit only the terrain `SurfaceVertexBuf`
occupant (the one real blocker); slot 14 and `LightsData@20` need no change.

**Edits (behavior-changing — deferred, out of scope here):**
1. `shaders/gos_terrain_surface.vert:60` — change `binding = 20` → `binding = 21` for
   `SurfaceVertexBuf`.
2. C++ terrain-surface VB bind site — change the `glBindBufferBase(..., 20, ...)` for the
   surface vertex buffer to `21` (locate the terrain-surface draw's SurfaceVertexBuf bind;
   it must move in lockstep with the shader `layout`).
3. Optionally introduce a named constant (`kTerrainSurfaceVbBinding = 21`) rather than a bare
   literal, and optionally register `SurfaceVertexBuf` in `RenderResourceRegistry.h`.
4. No change to `LIGHT_DATA_SSBO_BINDING`, `READBACK_SSBO_BINDING`, particle slot-14 binds,
   or any lightgrid/mech/static-prop binding.

**Verification for that future slice:**
- `scripts/check-binding-slots.py` — must stay exit 0 (C++↔GLSL lockstep; new terrain literal
  should not collide).
- `scripts/check-vulkan-bindings.py` — the slot-20 VULKAN-COLLISION entry should clear; slot-14
  remains a benign share.
- Golden parity + tier1 `mc2_01` (terrain-heavy) at minimum, plus `mc2_24` — terrain must render
  byte-identical (the surface VB is a pure relocation, no data change). This is a rendering
  path change, so a full-relink parity run is warranted.

**This slice (BINDING-SLOT-14-20-SPLIT-PLAN-1) makes none of the above edits.** It is the
analysis + map only.
