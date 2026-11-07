<!-- doc-status: current | reviewed:2026-10-11 -->
# MC2 OpenGL Render Contract

> s68/P9: Retired GL sources are cited with Git object syntax (`8064c6930:path`) so the historical implementation remains inspectable.

This document defines the authoritative rendering contracts for the modernized
OpenGL renderer. Its purpose is to stop accidental mixed-state paths from
spreading through the codebase and to make future rendering work easier to
reason about.

Use this file when changing:
- coordinate spaces
- submission formats
- culling and visibility decisions
- shader ownership of projection or depth
- overlay, water, terrain, object, and UI render paths

If a proposed change does not fit one of the contract buckets below, the change
is probably introducing a new bridge state and should be reconsidered.

## Main-view resolution ownership

`MC2_RENDER_SCALE` scales each scene axis with `max(1, lround(display * scale))`.
Shadow maps, light caches and bake targets have independent resolutions. They are
not main-view images. Picking and cursor projections use unjittered matrices.

| Pass / target | Resolution owner |
|---|---|
| Sky, terrain, props, legacy shapes, skinned mechs | shared scene color/depth, render |
| MRT1 normals, object IDs | shared scene, render |
| Water, particles, vegetation, impact decals | shared scene, render |
| Water reflection | HDRI texture / SH; no main-view reflection or refraction target |
| Screen shadow/contact/shore resolve | shared scene color/normal/depth, render |
| Edge and out-of-bounds fog | shared scene color/depth, render |
| HZB build | render at mip 0; subsequent mips halve, rounded up |
| Tone mapping / grading | render; encoded intermediate only when scaled |
| SMAA color copy, edges, weights, neighborhood; FXAA / CMAA2 | render |
| Scene reconstruction / CAS | display (composite), before UI |
| HUD, text, ImGui; final swapchain blit | display |

The UI order is enforced by `EnginePresentVk::presentFrame`: its single queue
submission executes `{compositeCbuf, cbuf}` in that order. The composite command
buffer finishes scene AA and, when scaled, reconstruction/CAS into `sharedImg`.
The tail command buffer then calls `recordPendingHudColorProof` (including
textured HUD/text), followed by `recordPendingImgui`, before the swapchain blit.
Both UI paths attach to the display-sized `sharedImg`; neither attaches to
`scaledComposite` or the shared scene images. This order also holds at scale 1.0,
where reconstruction is skipped. UI must never become input to scene AA or CAS.

Expected extents below are allocation rules, not captured validation evidence.
`[SCENE_TARGETS]`, `[RENDER_SIZES]` and `[SPATIAL_AA]` report actual runtime extents.

| Display | Scale | Scene / AA / HZB mip 0 / fog / screen resolve | Composite / HUD / ImGui |
|---|---|---|---|
| 1920 x 1080 | 1.0 | 1920 x 1080 | 1920 x 1080 |
| 1920 x 1080 | 0.67 | 1286 x 724 | 1920 x 1080 |
| 3840 x 2160 | 1.0 | 3840 x 2160 | 3840 x 2160 |
| 3840 x 2160 | 0.67 | 2573 x 1447 | 3840 x 2160 |

The 2026-10 temporal experiment was removed; the owner approved a new temporal
track on 2026-11-06 (`docs/plans/2026-11-06-temporal-aa.md`). Its camera
motion-vector target (`scene.motionVectors`, RG16F, render resolution, UV units,
previous minus current, unjittered) is recorded every frame at the start of the
composite; until the TAA resolve lands its only consumer is the Graphics Options
"Motion Vectors" debug inset. No reactive targets, history or jitter run until
the resolve consumes them. Fixed mission
exposure remains `gos_GetExposure()`; there is no automatic pre-exposure.

Material sampler creation uses `mc2vk::infra::materialSampler` to apply
`log2(MC2_RENDER_SCALE)` mip bias (zero at native resolution). UI, shadow,
depth and lookup samplers retain their existing bias. Explicit-LOD reads such
as the water HDRI reflection keep their authored LOD; the sampler bias affects
implicit derivative-based material reads.

### Upscale prerequisite validation (2026-10-30)

Proofs live in `E:/mc2-archive-2026-10/proofs/upscale-prereqs/`.
The compared binaries are baseline `c7ef9a666` and candidate `e843a30c4`.
The original baseline-versus-baseline runway captures differ by 3,704 pixels;
this is pre-existing settle/shadow-bake noise, not an upscale regression.
The close-out captures use trigger frame 900 and 120 settle frames, with
shadow-bake queue-drain messages checked before readback. Comparisons use the
baseline repeat noise as their tolerance and require visual inspection.

At 20 seconds, SIMID 03, 10 and 24 pass both repeat equality and their committed
baselines (`close-simid20-mc2_03`, `recovery-simid20-mc2_10`, `simid20-mc2_24`).
Explicit `MC2_RENDER_JITTER=0` plus `MC2_RENDER_SCALE=1.0` matches the default
night captures exactly in all three shoreline views; the day runway control
differs by 2,329 pixels, within the original baseline noise, and was viewed.
Fresh baseline repeats (`close-base-a-*`, `close-base-b-*`) have a maximum
day-view difference of 8,618 pixels. All nine explicit native/jitter-off views
versus baseline A are within that measured envelope (maximum 8,618); the runway
is exact. Baseline, default and explicit night captures are pixel-identical.
The first default day capture has timing outliers beyond that envelope (maximum
23,147 pixels versus baseline A). Its same-binary, same-settings repeat
(`close-default-repeat-mc2_01`) is within the envelope in all nine views
(maximum 8,618), has an exact runway match and differs from explicit native/
jitter-off by at most 2,230 pixels. No engine change was made between repeats.
All nine close-out captures have zero Vulkan validation errors. The viewed
cross-map, runway and shoreline comparisons show no visible scene regression.

## Modern spine vs legacy dispatch (corrected 2026-06-22 RENDER-CONTRACT-INDEX-1)

This document's Bucket A–D taxonomy is a **coordinate-space / submission-space**
contract. It is *orthogonal* to two other axes that describe the same passes:
- the **owner-lane spine** in `RenderCore/RenderPassContract.h` (5 `RenderPassId`
  lanes: StaticPropOpaque, Terrain, MechOpaque, Shadow, VFX), and
- the **callsite-tag taxonomy** in `mclib/render_contract.h` (`PassIdentity`,
  15 values).

A pass's bucket here (where its vertices live) is independent of whether it routes
the modern RenderCore spine (ViewUniforms b=3 + PipelineDesc + snapshot-authoritative)
or the legacy path. Which passes route the modern spine:
- **Modern spine:** `StaticPropOpaque` and `MechOpaque` — both `viewUniformsBound=true`,
  carry a PipelineDesc, and are snapshot-authoritative.
- **Legacy:** `Terrain`, `Shadow`, `VFX` rows in `RenderPassContract.h` are `false`
  on all three axes (no ViewUniforms, no PipelineDesc, not snapshot-authoritative).
  Water / overlay / decal / grass / UI / post-process are un-enumerated **orphans**
  (not even given a `RenderPassId` lane).

Crucially, the modern spine owns **identity / lifecycle / visibility-reporting only**,
**not dispatch**: even StaticPropOpaque and MechOpaque are still *flushed* by the
legacy driver `MC_TextureManager::renderLists()` (`mclib/txmmgr.cpp:2251-3679`;
static-prop flush `:3081`, mech flush `:3097`, shadow lanes `:2789-2812`).
`RenderPassContract.h` is explicitly descriptive ("NOT a scheduler... the imperative
frame loop continues to call each pass-owner's draw functions directly",
`RenderPassContract.h:12-15`).

The full per-pass routing table (owner subsystem, identity spine, dispatch driver,
and the three modern-spine axes for all ~13 passes) lives in
`docs/engine-standalone-seams.md` ("Modern-spine vs legacy pass routing"). See also
`docs/render-backend-seams/render-contract-index-1.md`.

> **Staleness note (corrected 2026-06-22 RENDER-CONTRACT-INDEX-1).** The staleness
> flagged by the 2026-05-14 audit is mainly **OMISSION** — this doc never covered the
> spine/dispatch dimension above — **not** wrong buckets. Do **NOT** rewrite the
> Bucket A–D model; its VPL-retirement content (A1 / D1) is current.

## Core Rules

### Rule 1: Every render path has one authoritative submission space

A path must be one of:
- **world-space authoritative**
- **projected-space authoritative**
- **screen-space authoritative**

Do not allow one path to submit vertices in one space while making correctness
decisions in another space unless the path is explicitly documented as a bridge
path scheduled for removal.

### Rule 2: Visibility ownership must be explicit

For each path, document who owns visibility and clipping:
- CPU world-space visibility
- CPU projected-space visibility
- GPU clip-space visibility
- mixed bridge path (temporary only)

### Rule 3: Shadow ownership must be explicit

For each path, document who is responsible for shadow correctness:
- forward shading in the path's own fragment shader
- post-process bridge path
- not shadowed by design

### Rule 4: Projected exceptions are valid

Not every `projectZ()` call is debt. Some paths are correctly projected by
design. These must remain explicit exceptions rather than being swept into
broad "GPU projection cleanup."

### Rule 5: Bridge code is allowed only with an exit plan

Temporary compatibility layers are acceptable when they unblock migration, but
they must be called out as bridge state in this document and in the design doc
that introduced them.

## Coordinate Space Vocabulary

### Raw MC2 world space

- `x = east`
- `y = north`
- `z = elevation`

This is the preferred authoritative world-space contract for terrain and future
GPU-native world geometry.

### Stuff / camera space

Used by older MC2/MLR math. See [architecture.md](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/docs/architecture.md).

### Projected space

The result of `Camera::projectZ()` or equivalent CPU projection. This includes
values such as:
- `px`, `py`, `pz`, `pw`
- `wx`, `wy`, `wz`, `ww` for some older projected paths

### Screen-space

Pixel-space UI or pretransformed vertex submission intended to map directly to
the final viewport.

## Contract Buckets

## Bucket A: World-Space Authoritative

These paths submit raw MC2 world-space positions and expect GPU projection and
GPU depth/shadow logic to be authoritative.

<!-- UPDATED 2026-05-15 by mc2-render-contract-synthesizer:
     action: UPDATE
     reason: VPL (VertexProjectLoop) retirement complete (Steps 1-9, plan
       2026-05-14-vertex-project-loop-retirement.md, HEAD 96642cc). The
       "still has CPU visibility debt" / projectZ()-as-producer status no
       longer holds: the slim reduction loop is now the sole CPU producer
       of both the cull cascade and the setInverseProject reductions, and
       terrain-quad projection authority is the GPU (clipPos/Fix-B). The
       old VPL body is DELETED.
     source notes: plan 2026-05-14-vertex-project-loop-retirement.md (v3.5
       trail), docs/superpowers/reviews/2026-05-15-step8-vpl-body-deletion-
       adversarial-review.md, docs/superpowers/reviews/2026-05-15-overlay-
       pz-v2-bit-identity-proof.md
     verification: mclib/terrain_init.cpp:848 ZoneScopedN slimReduce, :1544
       projectForTerrainAdmission, :1553-1559 cull cascade write,
       :1564 reduction gate, :1678 setInverseProject; zero
       vertexProjectLoop/VPParitySnap/s_vpFast/s_vpParity in mclib/code/
       GameOS/shaders (comments only)
-->
### A1. Terrain base (tessellated)

Status:
- active
- clean (CPU projected-depth debt RETIRED 2026-05-15 via VPL retirement)
- one decoupled slim CPU pass remains by design (cull cascade + min/max
  reduction; projection re-homed here, NOT a debt)

Primary files:
- [mclib/quad.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/quad.cpp)
- [mclib/terrain.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/terrain.cpp)
- [code/gamecam.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/code/gamecam.cpp)
- [GameOS/gameos/gameos_graphics.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/gameos_graphics.cpp)
- [shaders/gos_terrain.vert](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_terrain.vert)
- [shaders/gos_terrain.tesc](A:/Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\shaders\gos_terrain.tesc)
- [shaders/gos_terrain.tese](A:/Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\shaders\gos_terrain.tese)

Authoritative submission space:
- raw MC2 world space

Projection owner:
- GPU. The indirect terrain-quad path's sole projection authority is
  `clipPos[4]` in the thin record (Fix B): the producer writes per-corner
  clip-space positions and the thin VS reads them directly. The thin VS
  has NO `terrainMVP` uniform. Verified: `8064c6930:shaders/gos_terrain_thin.vert`
  Fix-B comment block (`vec4 clipPos[4]` at the record struct; "terrainMVP
  uniform REMOVED from the thin VS"). Fix A's per-slot MVP snapshot is
  DEMOTED behind `MC2_RING_TRACE=1` (default-off, inert: cached uniform
  loc is -1) - verified `g_envRingTrace` gate at
  `GameOS/gameos/gos_terrain_indirect_cement.cpp:692`, writers skipped at
  `:1651` when unset.

Shadow owner:
- forward terrain shading

CPU cull + reduction producer (post-VPL-retirement, by design - NOT debt):
- The slim reduction loop in [mclib/terrain.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/terrain.cpp)
  (`ZoneScopedN("Terrain::geometry slimReduce")` at `terrain.cpp:1466`)
  is the SOLE CPU producer of BOTH:
  (a) the cull cascade - `rv->clipInfo = clipR;` then, `if (rv->clipInfo)`,
      `setObjBlockActive` + `setObjVertexActive` at `terrain.cpp:1553-1559`,
      written BEFORE the reduction-admission gate `if (!clipR || !inViewR)
      continue;` at `mclib/terrain_init.cpp:946`;
  (b) the `leastZ/mostZ/leastW/mostW/leastWY/mostWY` reductions at
      `mclib/terrain_init.cpp:949-969`, which derive `yzRange`/`ywRange` at
      `terrain.cpp:1671-1675` and feed `eye->setInverseProject(mostZ,
      leastW,yzRange,ywRange)` at `mclib/terrain_init.cpp:1060`.
- The per-vertex projection is RE-HOMED here, not eliminated:
  `eye->projectForTerrainAdmission(vertex3D,sp)` at `mclib/terrain_init.cpp:926`.
  It cannot be derived from world-AABB bounds because perspective divide
  under the oblique cinematic camera does not preserve z/w ordering over
  the per-frame in-rect-visible set (camera model:
  `memory/camera_model_oblique_cinematic.md`).
- INVARIANT (catastrophic-axis, CRIT-1 of the Step 8 review): the cull
  write MUST be emitted on the `clipR` decision and BEFORE the
  `if (!clipR || !inViewR) continue;` reduction gate. `clipR` uses the
  identical formula to the deleted VPL `clipInfo` write
  (`eye->usePerspective && Environment.Renderer != 3 ? onScreenR :
  inViewR`, `mclib/terrain_init.cpp:928`). Placing the cull write AFTER the gate, or
  gating it on `inViewR`, makes the slim active-set a STRICT SUBSET of the
  loose 768u/384u-dilated legacy `{onScreen}` cull contract, and
  edge/off-rect objects and mechs VANISH (`memory/cull_gates_are_load_
  bearing.md`; mechs iterate last = canary). The loose `onScreen`
  contract is intentionally wider than strict `inView`; the cull must
  honor the loose set, the reduction may legitimately use the tighter set.

End state (REACHED 2026-05-15):
- world-space submission remains
- terrain-quad projection authority is the GPU (clipPos/Fix-B); no
  terrain correctness dependency on CPU `pz`
- the surviving CPU pass is the decoupled slim cull+reduction loop above
  (re-homed projection, by design)

### A2. Grass

Status:
- aligned with terrain path

Primary files:
- [GameOS/gameos/gos_postprocess.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/GameOS/gameos/gos_postprocess.cpp)
- [shaders/gos_grass.geom](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_grass.geom)
- [shaders/gos_grass.frag](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_grass.frag)

Authoritative submission space:
- terrain-derived world-space inputs

Projection owner:
- GPU

Shadow owner:
- forward shading

Notes:
- This path should continue to inherit terrain's world-space contract.

<!-- UPDATED 2026-05-15 by mc2-render-contract-synthesizer:
     action: CLARIFY
     reason: The VPL retirement re-homed the overlay-pz VISIBILITY GATE
       off cv->pz (D1 resolution), but the dedicated typed world-space
       overlay/decal batch path (this A3 target state) is still a deferred
       sibling slice. Clarify the boundary so the two are not conflated.
     source notes: plan 2026-05-14-vertex-project-loop-retirement.md,
       docs/superpowers/specs/2026-05-15-overlay-decal-gpu-port-slice-
       stub.md
     verification: stub present at docs/superpowers/specs/2026-05-15-
       overlay-decal-gpu-port-slice-stub.md; quad.cpp:2159-2189 gate
       re-home (visibility only, still M2d gos_PushTerrainOverlay submit)
-->
### A3. Terrain overlays and decals (target state)

Status:
- target state, not fully implemented
- the VPL retirement re-homed only the overlay-pz VISIBILITY GATE off
  cv->`pz` (see D1 resolution); the overlay still SUBMITS through the M2d
  `gos_PushTerrainOverlay` decal producer. The dedicated typed world-space
  batch path is a deferred sibling slice:
  `docs/superpowers/specs/2026-05-15-overlay-decal-gpu-port-slice-stub.md`
  (see also VPL-RETIREMENT-DEFERRED.md item 7).

Primary design:
- [2026-04-15-world-space-overlay-design.md](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/docs/plans/2026-04-15-world-space-overlay-design.md)

Types intended to move here:
- alpha cement / transition overlays
- craters
- footprints
- terrain decals

Authoritative submission space:
- raw MC2 world space

Projection owner:
- GPU, using a typed world-space batch path

Shadow owner:
- forward shading in the dedicated overlay/decal shaders

Required end state:
- no `rhw=1.0` bridge semantics
- no terrain-specific overlay behavior hidden inside generic textured shaders

## Bucket B: Projected-Space Authoritative

These paths are correctly projected by design and should not be migrated merely
for ideological consistency.

### B1. Water surface and water detail

Status:
- intentional projected path
- do not casually convert

Primary files:
- [mclib/quad.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/quad.cpp)
- [shaders/gos_tex_vertex.vert](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_tex_vertex.vert)
- [shaders/gos_tex_vertex.frag](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_tex_vertex.frag)

Authoritative submission space:
- CPU-projected space

Projection owner:
- `projectZ()`

Why it is projected on purpose:
- water projects a wave-displaced point, not just terrain vertex elevation
- the current alpha/edge behavior relies on that projected contract

Migration rule:
- do not migrate water piecemeal
- any future water rewrite must change submission, culling, and shader semantics together

### B2. Picking, cursor anchoring, and explicit screen-related helpers

Status:
- projected or screen-space by purpose

Examples:
- mouse picking support
- screen anchoring
- debugging helpers whose reason for existence is 2D placement

Migration rule:
- keep these projected unless the feature itself is redefined

## Bucket C: Screen-Space Authoritative

### C1. HUD, text, and menu/UI

Status:
- intentional screen-space paths

Primary files:
- UI and text shader paths
- menu and HUD submission code

Authoritative submission space:
- screen-space

Projection owner:
- caller / UI code

Shadow owner:
- none by default

Notes:
- scene post-processing should not implicitly redefine this contract

## Bucket D: Bridge Paths To Remove

These paths are temporary compatibility layers. They are allowed only while a
replacement path is being brought online.

<!-- UPDATED 2026-05-15 by mc2-render-contract-synthesizer:
     action: UPDATE
     reason: D1 (the highest-priority contract violation) is RESOLVED by
       the VPL retirement. Terrain quad projection authority is now the
       GPU (clipPos/Fix-B); the M2d overlay-pz visibility gate is now a
       cv->pz-INDEPENDENT on-site re-projection; the VPL body that
       produced the projected-depth metadata is DELETED. Status flipped
       active->resolved; the section is kept (per protocol: no silent
       REMOVE) with the resolution mechanism documented.
     source notes: plan 2026-05-14-vertex-project-loop-retirement.md,
       docs/superpowers/reviews/2026-05-15-step8-vpl-body-deletion-
       adversarial-review.md, docs/superpowers/reviews/2026-05-15-overlay-
       pz-v2-bit-identity-proof.md
     verification: quad.cpp:2159-2189 (clipInfo==0 sentinel + on-site
       projectForTerrainAdmission, no vertices[c]->pz read in the
       production path; only the demoted MC2_M2D_PZ_PARITY probe at
       :2213 reads pz, probe-only local); zero vertexProjectLoop in
       mclib/code/GameOS/shaders
-->
### D1. Terrain world-space submission gated by projected depth

Status:
- RESOLVED 2026-05-15 (VPL retirement, plan
  2026-05-14-vertex-project-loop-retirement.md, HEAD 96642cc)
- kept as a resolved-bridge record, not an active violation

Primary files:
- [mclib/quad.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/quad.cpp)
- [mclib/terrain.cpp](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/terrain.cpp)

Original problem (now closed):
- terrain base vertices submitted in world-space while triangle acceptance
  depended on `pz` values produced by the CPU VertexProjectLoop

How it was resolved:
- The VPL body that produced the per-vertex `px/py/pz/pw/clipInfo`
  projected-depth metadata is DELETED (zero `vertexProjectLoop` /
  `VPParitySnap` / `s_vpFast` / `s_vpParity` symbols remain in
  `mclib/`, `code/`, `GameOS/`, `shaders/` - only historical comments).
- Terrain-quad projection authority is the GPU via `clipPos`/Fix-B (see
  Bucket A1 Projection owner).
- The one remaining `pz`-shaped visibility gate - the M2d
  `gos_PushTerrainOverlay` decal producer's per-corner visibility test -
  is now a cv->`pz`-INDEPENDENT on-site re-projection in
  `mclib/quad.cpp:2159-2189`: it short-circuits on the
  `vertices[c]->clipInfo == 0` sentinel (`quad.cpp:2172`, reproducing the
  old VPL `pz=-0.5` off-screen sentinel) and otherwise re-projects from
  the same `(vx,vy,elevation)` triple via
  `eye->projectForTerrainAdmission(ov3D, osp)` at `mclib/quad_draw.cpp:909`. It does
  NOT read `vertices[c]->pz`. Bit-identical-by-construction to the old
  VPL-cv->`pz` behavior (proof:
  `docs/superpowers/reviews/2026-05-15-overlay-pz-v2-bit-identity-proof.md`).
  The only `vertices[c]->pz` read is inside the demoted
  `MC2_M2D_PZ_PARITY` probe-only local at `mclib/quad_draw.cpp:946`
  (belt-and-suspenders; default-off; see VPL-RETIREMENT-DEFERRED.md item 3).

### D2. IS_OVERLAY / `rhw=1.0` / terrainMVP bridge inside `gos_tex_vertex`

Status:
- active bridge
- acceptable short-term
- not end-state architecture

Primary files:
- [shaders/gos_tex_vertex.vert](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_tex_vertex.vert)
- [shaders/gos_tex_vertex.frag](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/gos_tex_vertex.frag)

Why it existed:
- it enabled world-space-like terrain overlays before a dedicated typed path existed

Why it should be retired:
- it hides terrain-specific semantics inside generic textured rendering
- it makes future maintenance harder

Replacement:
- dedicated typed world-space overlay/decal batches

### D3. Post-process shadow pass for world geometry that should self-shadow

Status:
- bridge / compatibility layer

Primary design:
- [2026-04-11-gbuffer-postprocess-shadow-design.md](A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/docs/plans/2026-04-11-gbuffer-postprocess-shadow-design.md)

Notes:
- the post pass was a valid bridge for legacy projected paths
- it should not remain the main long-term shadow strategy for world geometry

Target state:
- static terrain shadows own terrain macro-lighting
- dynamic local shadow pass owns moving-caster detail
- forward shading handles world-space geometry when possible
- post-process shadowing becomes optional fallback, not architectural center

## Allowed Legacy Containment

Some paths may remain legacy if the payoff from migration is low. These should
be clearly contained and documented rather than repeatedly half-modernized.

Candidates:
- some older object submission paths
- particle paths that are visually acceptable and low-risk
- editor/debug draw helpers

Rule:
- if a path remains legacy, mark it explicitly and do not route new world-space
  renderer features through it

## Migration Policy

### Approved migration order

1. document the path contract
2. classify submission space and visibility ownership
3. introduce a typed replacement path if needed
4. switch one family of draws atomically
5. delete the bridge code only after the replacement is verified

### Disallowed migration pattern

Do not change:
- submission semantics
- cull/visibility semantics
- shader expectations

in separate passes for the same draw family if the path is currently mixed.

That is how silent half-state regressions happen.

## Current Priorities

<!-- UPDATED 2026-05-15 by mc2-render-contract-synthesizer:
     action: UPDATE
     reason: Priority 1 (remove terrain's projected-depth-correctness
       dependency) is DELIVERED by the VPL retirement. Restated as DONE
       with the exact post-retirement cull contract, rather than left as
       an open priority.
     source notes: plan 2026-05-14-vertex-project-loop-retirement.md
     verification: terrain.cpp:1466/1544/1553-1559/1564/1678; quad.cpp:
       2159-2189; no VPL symbols remain
-->
### Priority 1 - DELIVERED 2026-05-15 (VPL retirement)

Terrain's dependency on projected-depth correctness is removed:
- the exact terrain cull contract is documented in Bucket A1 (the slim
  reduction loop is the sole CPU cull+reduction producer; the
  before-the-continue catastrophic-axis placement invariant is
  load-bearing)
- `pz` is no longer a load-bearing terrain correctness input: the VPL
  body is deleted, terrain-quad projection authority is the GPU
  (clipPos/Fix-B), and the M2d overlay-pz gate is cv->`pz`-independent

Retired / demoted scaffolding family (demote-don't-delete per the
worktree Debug-instrumentation rule - all default-off, inert):
- bucket-header SSBO: `MC2_BUCKET_HEADER_TRACE`
  (`GameOS/gameos/gos_terrain_indirect_atlas.cpp:305`)
- CPU-pack fallback: `MC2_TERRAIN_INDIRECT_CPU_FALLBACK`
  (`GameOS/gameos/gos_terrain_indirect_cement.cpp:828`)
- Fix A per-slot MVP snapshot: `MC2_RING_TRACE`
  (`GameOS/gameos/gos_terrain_indirect_cement.cpp:692`)
- `MC2_VPL_CULL` + `MC2_VPL_REDUCE`: retired to one-shot
  `event=retired` lifecycle lines (`mclib/terrain_init.cpp:827-846`); the
  legacy reference they compared against died with the VPL body, so a
  relocated self-comparison would be tautological
- parity-infra: FULLY DELETED (zero-consumer; zero `VPParity`/`s_vpParity`
  symbols remain anywhere in `mclib/`/`GameOS/`)
- Legacy CPU terrain-lighting path: HARD-RETIRED both-env. `MC2_TERRAIN_
  LIGHTING_GPU=0` AND `MC2_TERRAIN_LIGHTING_PARITY=1` no longer reach the
  CPU lighting block: `s_lightingGpuAuth = true` forces GPU authority at
  `mclib/quad_draw.cpp:81`; the `!s_lightingGpuAuth` block at
  `mclib/quad_draw.cpp:97` is unreachable. The VPL body was the sole writer of
  terrain `Vertex::hazeFactor`; a defensive `currentVertex->hazeFactor =
  0.0f` zero-init now lives at `mclib/mapdata.cpp:1154`. A one-shot
  `[TERRAIN_LIGHTING v1] event=legacy_cpu_path_retired` line fires if
  either env was set.

### Priority 2

Move terrain-adjacent overlays and decals onto typed world-space batches:
- alpha cement
- craters
- footprints

### Priority 3

Shrink the role of bridge shadow paths:
- rely less on post-process shadowing for world geometry
- keep projected exceptions explicit

## Non-Goals

These are not goals of the render contract cleanup:
- removing every `projectZ()` call in the codebase
- forcing water into the terrain world-space contract without a full redesign
- converting UI or picking to world-space for aesthetic consistency

## Review Checklist

When reviewing any future rendering change, ask:

1. What is the authoritative submission space?
2. Who owns visibility?
3. Who owns depth correctness?
4. Who owns shadows?
5. Is this a stable contract or a bridge state?
6. If it is a bridge, where is the exit plan documented?
