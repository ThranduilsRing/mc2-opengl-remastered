<!-- doc-status: stale | reviewed:2026-10-11 -->
# Terrain Admission Hot-Path Recon
**Date:** 2026-05-22
**Branch:** claude/nifty-mendeleev (HEAD 308625e)
**Status:** READ-ONLY recon. No code changes.

---

## 1. Executive Summary

`projectForTerrainAdmission` (an inline wrapper delegating unconditionally to
`Camera::projectZ`) is called on every terrain vertex once per frame in the
`slimReduce` loop (`mclib/terrain.cpp:1783`) and on each of the four water-face
corners inside `TerrainQuad::draw` (`mclib/quad.cpp:1070,1115,1160,1205`).
On a 120x120 map (mc2_10) the slimReduce site fires ~14,400 times per frame;
the quad.cpp water sites add at most O(4 * water_quads) additional calls, each
deduped per vertex per frame via `calcThisFrame & 2`.

The complementary retirement campaigns (VPL, drawPass, inverseProject chain)
have all shipped. `projectForTerrainAdmission` is the last live `projectZ` call
in production (all six non-terrain wrappers use `projectModernClipGL` in their
default-Bypass path since F3/F4/F5). The slimReduce loop already carries
instrumented RDTSC sidecar timing under `SIDECAR_TERRAIN_ADMISSION_PERFRAME`
(`mclib/cpu_proj_cost_split.h:52`).

**Recommended slice:** replace the slimReduce `projectForTerrainAdmission` call
with `eye->extractFrustumPlanes` (called once per frame, outside the loop) +
`eye->quadAabbInFrustum(planes, vPos, vPos)` per vertex, gated behind a new
`MC2_TERRAIN_ADMISSION_MODERN` env var. Legacy behavior (exact `projectZ`
screen-rect path) preserved as default until empirical red-band canary passes.

---

## 2. Q1 -- projectForTerrainAdmission Call Shape and Iteration Count

### Site A: `mclib/terrain.cpp:1783` -- slimReduce loop

```
for (long ri = 0; ri < numberVertices; ++ri, ++rv)
{
    ...
    if (onScreenR || !clipUsesOnScreen)
    {
        inViewR = eye->projectForTerrainAdmission(vertex3D, sp);   // :1783
    }
    ...
}
```

Iteration shape: O(N) per frame where N = `numberVertices` = `realVerticesMapSide^2`.
- Typical mc2 maps: `realVerticesMapSide` = 120 -> ~14,400 calls/frame.
  (Validated: `terrain.cpp:326` lists 120 as a canonical map side;
  `mclib/terrain_init.cpp:1065` computes `ssNumActiveVerts = realVerticesMapSide * realVerticesMapSide`.)
- Guard: `(onScreenR || !clipUsesOnScreen)` skips the call when perspective is on
  (`usePerspective && Environment.Renderer != 3`) and the angular pre-cull rejected
  the vertex. In practice the angular cull saves roughly 30-60% of calls depending
  on camera zoom/pitch; the camera-facing half of the map still calls through every
  frame.

**Output consumed by two downstream paths:**
1. `rv->clipInfo = clipR` (cull-cascade write at `terrain.cpp:1795`) -- gates
   `objBlockInfo` + `objVertexActive` + solid-window candidate dispatch. CRITICAL
   load-bearing path. The projectZ return value is NOT directly the cull write;
   `clipR = clipUsesOnScreen ? onScreenR : inViewR` (`:1787`), so in the default
   perspective branch clipR == onScreenR and the projectZ call is used only for
   `inViewR`, which feeds the `px/py/pz/pw` raster write below.
2. `rv->px/py/pz/pw` at `mclib/terrain_raycast.cpp:22-25` -- raster coordinates for legacy
   `TerrainQuad::draw()` immediate path (runs on un-armed frames: mission deployment
   screen, unit-select screen, any frame where `IsFrameSolidArmed()` is false).

### Site B: `mclib/quad.cpp:1070,1115,1160,1205` -- water quad per-corner

```
if (!(vertices[N]->calcThisFrame & 2))
{
    if (clipped1 || clipped2)
    {
        clipData = eye->projectForTerrainAdmission(vertex3D, screenPos);   // :107x
        ...
        if (legacyWaterDraw)
        {
            vertices[N]->wx/wy/wz/ww = screenPos.x/y/z/w;
        }
        vertices[N]->calcThisFrame |= 2;   // dedupe: at most once/vertex/frame
    }
}
```

Iteration shape: O(4 * water_quads_with_any_vertex_clipped) per frame.
Deduped via `calcThisFrame & 2` so each shared water-corner vertex projects once.
Water output consumed: `clipInfo` (water admission), `wx/wy/wz/ww` when
`legacyWaterDraw` is active.

### Site C: `mclib/quad.cpp:2114` -- pz-validity gate inside `fastPathEligible`

```
const bool pzNeeded =
    (useOverlayTexture && overlayHandle != 0xffffffff)
    || (terrainHandle != 0 && !gos_terrain_indirect::IsFrameSolidArmed());
if (pzNeeded) {
    for (int c = 0; c < 4; c++) {
        if (vertices[c]->clipInfo == 0) { pzc[c] = false; continue; }
        eye->projectForTerrainAdmission(ov3D, osp);   // :2114
        pzc[c] = (osp.z + TERRAIN_DEPTH_FUDGE >= 0.0f) && (osp.z + TERRAIN_DEPTH_FUDGE < 1.0f);
    }
}
```

This site fires ONLY when `fastPathEligible` AND (`overlayTexture active` OR `not
solid-armed`). In production with `MC2_TERRAIN_INDIRECT_OVERLAY` default-ON and
solid armed, it fires for the sparse overlay set. The pz value here is used
exclusively as a depth-range guard (`pzTri1/pzTri2` at `:2120-2127`), NOT for
frustum admission. This site is independent of the hot path and is NOT in scope
for this slice.

---

## 3. Q2 -- slimReduce, drawPass, VPL Retirement State

| Item | State | Evidence |
|---|---|---|
| VPL (VertexProjectLoop) | RETIRED 2026-05-18 | `docs/render-perf-snapshot.md:29,52`; commits `5296de4`, `0c8e06b`, `63a0b3e` |
| `slimReduce` loop | LIVE (permanent replacement for VPL) | `terrain.cpp:1670` `ZoneScopedN("Terrain::geometry slimReduce")` |
| `slimReduce` RED reduction (leastZ/mostZ etc.) | DELETED Phase 4 2026-05-19 | `mclib/terrain_raycast.cpp:41-45` confirms deletion; consumer chain retired in Phase 3 |
| drawPass `Terrain::render drawPass` per-quad loop | RETIRED default-ON 2026-05-17 | `CLAUDE.md:152`; `docs/render-perf-snapshot.md:24` (~1.7ms -> ~20us armed) |
| inverseProject / setInverseProject chain | RETIRED | `docs/superpowers/plans/2026-05-19-inverseproject-consumer-collapse.md` |
| `ProjectZBypassMode` on non-terrain wrappers | Default=Off (legacy path); Bypass available | `object_admission_predicate.h:71-74`; F3/F4/F5 shipped |
| `projectForTerrainAdmission` bypass mode | NONE -- hard projectZ call | `camera.h:568-584`; no bypass arm exists |

The slimReduce loop is the permanent per-frame vertex-walk; it cannot be retired
wholesale. This slice targets only the `projectForTerrainAdmission` call inside it.

---

## 4. Q3 -- `Camera::projectZ` Body Algorithm

Body is inline at `mclib/camera.h:470-558`. Algorithm:

1. Swizzle world point: `coords = (-point.x, point.z, point.y)` (Stuff axis
   convention `camera.h:485-487`).
2. `xformCoords = coords * worldToClip` (row-vector multiply, `camera.h:489`).
3. Perspective branch (`usePerspective`):
   - `rhw = 1/xformCoords.w` (or 1.0 if w==0).
   - `screen.x = (xformCoords.x * rhw) * viewMulX + viewAddX` (pixel-space X).
   - `screen.y = (xformCoords.y * rhw) * viewMulY + viewAddY` (pixel-space Y).
   - `screen.z = xformCoords.z * rhw`.
   - `screen.w = fabs(rhw)` (note: absolute value -- w-sign NOT preserved).
4. Screen-rect test: `if (screen.x < 0 || screen.y < 0 || screen.x > screenResolution.x || screen.y > screenResolution.y)` -> return FALSE.
5. Otherwise return TRUE.

**Critical detail for red-band:** behind-camera vertices have `xformCoords.w < 0`
(in D3D-homog convention), so `rhw = 1/w` is negative, which causes `screen.x/y`
to be remapped to potentially valid pixel coordinates (inside the screen rect).
The screen-rect test may PASS for such vertices. The `fabs(rhw)` write to
`screen.w` loses the sign. Per `memory/clip_w_sign_trap.md` (26 days old,
principle confirmed by codebase search): `clip.w` sign is NOT a reliable
front/back indicator; `screen.z in [0,1)` is the only reliable test in this
codebase. The pz gate at `quad.cpp:2116` closes this gap for the raster path.

**Single-vertex cost:** one matrix-vector multiply (4x4), one divide, five adds/muls
for viewport map, one rect test. No internal iteration. Cost scales linearly with
call count.

---

## 5. Q4 -- Available Cheap Conservative-Admit Primitives

### Primitive A: `extractFrustumPlanes` + `quadAabbInFrustum` (world-space AABB)

- Declaration: `mclib/camera.h:948-954`.
- Body: `mclib/camera.cpp:711-769`.
- Already used in production for terrain picking (`camera.cpp:870,899,927`).
- Algorithm: Gribb-Hartmann 6-plane extraction from `worldToClip`; per-AABB
  p-vertex test. For a point-AABB the test is exact (no conservatism above the
  geometry itself).
- Cost: `extractFrustumPlanes` = 1 call per frame (~24 muls + arithmetic, O(1)).
  `quadAabbInFrustum` for a point = 6 dot products + 6 comparisons per vertex.
  Compare to `projectZ`: 1 mat-vec mul + 1 div + 5 muls + 1 rect test per vertex.
  Both are ~same flop count but `quadAabbInFrustum` avoids the divide.
- Near-plane behavior: plane index 4 is `sw[4][k] = rZ[k]` (near plane under
  `glClipControl` native [0,1]; see `camera.cpp:702-730`). A behind-camera vertex
  has `dot(rZ, swizzled_point) < 0`, so the near-plane test correctly rejects it.
  This is the key property: frustum near-plane catches behind-camera vertices
  WITHOUT relying on `clip.w` sign or `screen.z in [0,1)`.
- Fitness for conservative-admit: EXCELLENT for the hot-path slimReduce. Over-
  admission (frustum AABB admits slightly more than the screen-rect test) is
  acceptable -- `clipInfo` is already a conservative superset at the cull boundary.
- **Fitness for the red-band class:** Theoretically sound (near plane rejects
  behind-camera verts), but the `projectz_overlay_findings.md` conclusion says
  "candidate predicates have not been shown to correctly reject the behind-camera
  class" for the SCREEN-RECT replacement. However, `quadAabbInFrustum` operates in
  world-space against the frustum planes (not screen-rect), which is a different
  family from the evaluated predicates (`legacyRectFinite`, `homogClip`,
  `rectSignedW`, `rectNearFar`, `rectGuard`). Empirical canary verification is
  required after ship before promoting to default.
- Per vertex data needed: `vx, vy, pVertex->elevation` -- already present in the
  slimReduce loop at `terrain.cpp:1694-1697`. No new data required.

### Primitive B: `projectModernClipGL` + `clipSpaceFrustumAdmitGL`

- `projectModernClipGL` at `camera.h` (grep: `camera.h:2490` area).
- Already used by `projectForObjectAdmission` in Bypass mode.
- Produces GL-NDC clip coords from `worldToClipGL` (F2 product).
- Cost: one mat-vec mul, no divide.
- `clipSpaceFrustumAdmitGL` at `mclib/object_admission_predicate.h:78` -- checks
  `clip.xy in [-w,w]`, `clip.z in [0,w]` which handles GL-NDC `clip.w > 0`
  convention correctly.
- Fitness: viable but requires `worldToClipGL` to be stable at slimReduce time.
  `worldToClipGL` is populated each frame in `gamecam.cpp` via `viewClipGL.Multiply`.
  More complex interaction with F1 unified-projection campaign (which is renaming
  related matrices). NOT recommended for this slice due to coupling risk.

### Primitive C: Angular pre-cull (existing `onScreenR`)

Already in place (`terrain.cpp:1690-1725`). Does NOT handle behind-camera verts
at close range (angular distance < `CLIP_THRESHOLD_DISTANCE = 768.0f` units,
`terrain.cpp:1528,1709`). Not sufficient on its own.

### Ranking

1. **`extractFrustumPlanes` + `quadAabbInFrustum`** -- already battle-tested in
   picking path, uses `worldToClip` (legacy matrix, F1-independent), correct near-
   plane behavior, O(1) frame setup, O(6) per vertex.
2. `projectModernClipGL` + `clipSpaceFrustumAdmitGL` -- valid but couples to F1
   campaign; defer.
3. Angular pre-cull alone -- insufficient; keep as-is as inner guard.

---

## 6. Q5 -- Existing Instrumentation

Per-frame ns timing for terrain admission exists and is wired:

- `SIDECAR_TERRAIN_ADMISSION_PERFRAME` at `mclib/cpu_proj_cost_split.h:52`.
- `terrain_admission_begin_ns()` / `terrain_admission_end_ns(int64_t)` at
  `cpu_proj_cost_split.h:121-122`; bodies at `cpu_proj_cost_split.cpp:388-396`.
- Called inside `projectForTerrainAdmission` wrapper at `camera.h:571,582`.
- Accumulated into `s_curFrameSidecar[SIDECAR_TERRAIN_ADMISSION_PERFRAME]` per
  frame; emitted by the existing COST_SPLIT trace path.

`[SLIMSPLIT v1]` RDTSC brackets at `terrain.cpp:1782-1784` additionally capture
the PROJ sub-bucket within slimReduce (`g_ssProjCyc / g_ssProjCall`).

**What is already measurable without code changes:**
- `MC2_COST_SPLIT_TRACE=1` emits per-frame terrain admission ns total.
- `MC2_SLIM_SPLIT_TRACE=1` emits PROJ/CULL/RED sub-bucket breakdown.

**What needs adding for the slice:**
- A `[TERRAIN_ADMISSION v1]` lifecycle line at Modern/Legacy gate initialization
  (env read, one-shot stderr), matching debug instrumentation rule in CLAUDE.md.
- A per-frame Modern-path workload counter (reuse `SIDECAR_TERRAIN_ADMISSION_PERFRAME`
  or add a sibling `SIDECAR_TERRAIN_ADMISSION_MODERN_PERFRAME`) so the COST_SPLIT
  trace can compare old vs new path.
- Existing `terrain_admission_begin_ns` / `end_ns` wrapping the Modern path provides
  the ns comparison automatically if the wrapper is called from Modern path too.

No Tracy zones should be added to the per-vertex hot loop (100ns floor rule in CLAUDE.md).

---

## 7. Q6 -- Red-Band Class Behavioral Requirement

From `memory/projectz_overlay_findings.md` (2026-04-26, confirmed by codebase search):

> "Behind-camera terrain vertices produce `screen.x/y` far outside viewport bounds;
> hardware clips them to viewport boundary, creating diagonal bands. These are the
> 'dangerous class' -- geometry the legacy projectZ gate is currently the only thing
> catching."

Constraint: any replacement for `projectForTerrainAdmission` MUST correctly reject
vertices that are behind the camera (i.e., between the camera origin and the near
plane, with negative camera-space Z). The legacy `projectZ` screen-rect test catches
these because `rhw = 1/w < 0` maps them to screen coords far outside `[0, screenResolution]`.

The `projectz_overlay_findings.md` conclusion (still held as architectural law):
"terrain admission must stay legacy until a replacement EXPLICITLY handles the
red-band class." The five previously evaluated predicates
(`legacyRectFinite`, `homogClip`, `rectSignedW`, `rectNearFar`, `rectGuard`) were
NOT shown to correctly reject this class.

`quadAabbInFrustum` was NOT among the evaluated candidates. It operates via
world-space frustum planes rather than screen-rect, and its near-plane test (plane
index 4 = `rZ` row of `worldToClip`, `camera.cpp:730`) correctly rejects vertices
with `dot(rZ, swizzled_point) < 0` regardless of `clip.w` sign. This is a
different geometric predicate family. It is theoretically sound for the red-band
class but requires visual canary validation before being promoted to default.

**Empirical gate for promotion:** run tier1 smoke with `MC2_TERRAIN_ADMISSION_MODERN=1`
and compare against baseline. Red diagonal bands appearing = reject. Zero false-
rejects (terrain visible at all camera angles including low pitch) = pass.

---

## 8. Slice Plan Recommendation

### Name: `terrain-admission-frustum-modern`

### Goal
Replace the ~14,400/frame `projectZ` matrix-multiply calls in the slimReduce loop
with a per-vertex world-space frustum-plane test. Legacy behavior preserved under
env fallback.

### Scope

**Step 1 (frame setup -- outside loop):**
In `Terrain::geometry` before the `ZoneScopedN("Terrain::geometry slimReduce")` zone,
extract frustum planes once: `float s_planes[6][4]; eye->extractFrustumPlanes(s_planes);`

**Step 2 (per-vertex gate -- inside slimReduce loop, replaces `projectForTerrainAdmission`):**

```cpp
// MC2_TERRAIN_ADMISSION_MODERN (default: off until visual canary passes)
if (s_admissionModern) {
    Stuff::Vector3D vPos(rv->vx, rv->vy, rv->pVertex->elevation);
    inViewR = eye->quadAabbInFrustum(s_planes, vPos, vPos);
    // sp stays sentinel -- px/py/pz/pw write below guards on onScreenR,
    // which still comes from angular pre-cull (unchanged).
} else {
    inViewR = eye->projectForTerrainAdmission(vertex3D, sp);
}
```

Note: `clipR = clipUsesOnScreen ? onScreenR : inViewR` at `terrain.cpp:1787` --
in the default perspective path `clipR == onScreenR` regardless of `inViewR`,
so the cull-cascade write is unaffected by the admission change. Only the
`px/py/pz/pw` raster write at `mclib/terrain_raycast.cpp:22-25` consumes `sp`. Under the
Modern path `sp` stays sentinel (-10000) which is the existing off-screen value;
the legacy raster path reads `rv->px/py` as the screen coordinate, so un-armed
frames with `legacyDraw` active will use sentinel coords. If this is a problem
for un-armed frames, the sentinel write can be kept conditional on `!s_admissionModern`.

**Step 3 (env gate):**
`static const bool s_admissionModern = (std::getenv("MC2_TERRAIN_ADMISSION_MODERN") != nullptr);`
One-shot `[TERRAIN_ADMISSION v1]` stderr lifecycle line at init (matches debug
instrumentation rule). Default: Legacy (env not set).

**Step 4 (quad.cpp water site -- DEFERRED):**
`quad.cpp:1070,1115,1160,1205` (water corner admission) can be modernized in a
follow-up slice. The `calcThisFrame & 2` dedup already limits the cost; the water
path is not the primary hot spot.

**Step 5 (quad.cpp pz site -- NOT IN SCOPE):**
`quad.cpp:2114` uses `projectForTerrainAdmission` for depth-range check
(`pz_adj in [0,1)`), not for frustum admission. This cannot be replaced with a
frustum test because it gates on `screen.z`, not position. Keep as-is.

### Pass Criteria

1. `MC2_SLIM_SPLIT_TRACE` shows PROJ bucket drops materially under `MC2_TERRAIN_ADMISSION_MODERN=1`.
2. Visual canary: no red diagonal bands visible at any camera angle / zoom under Modern mode.
3. `false_rejects == 0`: no terrain disappears that was visible under Legacy mode.
4. `MC2_TERRAIN_ADMISSION_MODERN` unset (default): behavior byte-identical to pre-slice.
5. Tier1 5/5 pass under Modern mode before promoting to default.

### Promotion Criteria (default flip)

See Section 10 below.

### Estimated Slice Surface

| File | Change |
|---|---|
| `mclib/terrain.cpp` | extract planes before slimReduce; env gate inside loop (~10 lines net) |
| `mclib/camera.h` | no change (extractFrustumPlanes + quadAabbInFrustum already declared) |
| `mclib/cpu_proj_cost_split.h/.cpp` | optional: add Modern sidecar counter (~5 lines) |
| Total | ~3 files, ~20 LoC |

---

## 9. What Is NOT in This Slice

- Retire all legacy projection (`Camera::projectZ` body, `worldToClip` matrix).
- Retire MLR or any gosFX caller.
- Solve every red-band edge case (canary gate protects; unsolved cases stay Legacy default).
- Rebuild `cameraToClip` or `worldToClipGL` (F1 campaign handles that separately).
- Prove every terrain caller is modern (only slimReduce hot path targeted).
- Modernize `mclib/quad_setup.cpp:631-766` water sites (deferred follow-up).
- Remove the `projectForTerrainAdmission` wrapper (survives as fallback).
- Touch `quad.cpp:2114` pz-validity gate (different semantic; not frustum admission).

---

## 10. Promotion Criteria for Default Flip

Before changing `s_admissionModern` from env-opt-in to default-on:

1. **Tier1 visual canary clean at all 5 missions** under Modern mode: no red diagonal
   bands visible when panning / pitching camera to extreme low angles where behind-
   camera verts were previously the red-band source.
2. **Wolfman zoom canary**: confirm buildings and mechs still appear at wolfman
   altitude (87% false-negative risk from angular cull documented in prior sessions
   is NOT introduced by this change since `onScreenR` is preserved unchanged; verify).
3. **Un-armed frame (mission deployment screen) visual check**: confirm no terrain
   corruption. The `px/py/pz/pw` raster sentinel write path needs to be tested
   on at least one un-armed frame (deploy screen or force `IsFrameSolidArmed=false`
   via env).
4. **COST_SPLIT proof**: user-driven Tracy capture shows `terrain_admission_perframe`
   ns materially lower under Modern vs Legacy at wolfman zoom (the canonical perf
   measurement point per `docs/render-perf-snapshot.md`). Must be substitutive, not
   additive (per `memory/feedback_offload_must_be_substitutive_not_additive.md`).

All four gates must pass before committing the `s_admissionModern = true` default flip.
