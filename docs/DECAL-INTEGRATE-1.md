<!-- doc-status: historical | reviewed:2026-10-19 -->
# DECAL-INTEGRATE-1 — joining the two parked projected-decal branches

> Retired in lane 2 s64: SSAO and box decals, their GL/VK passes, gates,
> shaders and fixtures were deleted. Implementation deletion: `5fc80bf89`;
> shader artifacts: `6deb6a93e`. Box decals may return for terrain displacement
> and mech footprints, with a wired native consumer and a default-on path.

Slice that joins the two previously-parked projected-decal branches into one coherent,
default-OFF, **parallel-to-baked-craters** lane: it wires the runtime impact-decal RING
(producer) to the screen-space box-decal PROJECTION (consumer).

Grounding: `.claude/DECAL-INTEGRATE-RECON-1.md` (integration plan) +
`.claude/FRAME-RESOURCE-LEDGER-1.md` (partial G-buffer: `sceneDepthTex_`, `sceneNormalTex_`).

Status: **BUILT**. Branch `claude/decal-integrate-1` off nifty `f6f82880`.

---

## Ownership decision (load-bearing)

**PARALLEL_TO_BAKED — projected is NOT canonical.** The baked `drawDecals()` /
`decal.frag` / `crater.cpp` crater path is stock-visual load-bearing and proven across
the campaign soak. The projected (box-decal) lane is a **second, opt-in consumer** of the
same `dynamic_decal_ring` producer. Neither path removes or replaces the other:

- Producer: `mclib/dynamic_decal_ring.{h,cpp}` — 64-slot fixed ring of impact `Slot`s.
- Consumer #1 (UNCHANGED): `DynDecal::gatherToDecalBatch()` pushes 2 tris/slot into the
  existing `gos_PushDecal` / `gos_DrawDecals` baked path (gate `MC2_DYNAMIC_DECALS`).
- Consumer #2 (NEW this slice): `gosPostProcess::drawBoxDecals()` reads the same ring
  read-only and draws one screen-space projected decal per live impact site
  (gate `MC2_PROJECTED_DECALS`).

Promotion of projected over baked is an explicitly-**deferred**, evidence-gated future
decision (needs texture + surface-class masking + on-NVIDIA visual confirm). Recommended
default if never revisited: stay parallel-gated indefinitely.

---

## How the two branches were brought in

- `claude/box-decal-1` @ `86e634c0` (CONSUMER): cherry-picked **clean** (1 commit, no
  conflicts) → `gos_postprocess.{cpp,h}` box-decal pass + `box_decal.{vert,frag}`.
- `claude/decal-terrain-impact-mvp` @ `585d29c9` (PRODUCER): cherry-picked; the 1735-behind
  branch conflicted only in `txmmgr.cpp` and `GraphicsOptionsWindow.cpp` (nifty rewrote
  both wholesale — pure displacement, not semantic). Resolved by resetting those 2 files
  to nifty HEAD and **hand-reapplying the additive hunks** verified from
  `git show 585d29c9:<file>`:
  - `txmmgr.cpp`: `#include "dynamic_decal_ring.h"` + the `DynDecal::gatherToDecalBatch(frameLength)`
    call immediately before `gos_DrawDecals()` in `renderLists()`. `frameLength` is the
    engine-global last-frame duration in seconds (`mclib/timing.h`, already included).
  - `GraphicsOptionsWindow.cpp`: ring-status include + the "DynDecals: X/Y active" ImGui line.
  - `crater.cpp`, `mclib/dynamic_decal_ring.{h,cpp}`, `mclib/CMakeLists.txt` landed clean.

The 1735-commit branch was **never rebased**.

---

## The seam closure (the actual new work)

Before: box-decal drew ONE hardcoded screen-center test box
(`ndcCenter={0,0,0.5,1}`, procedural pattern) with no producer; the ring fed only the
baked path. Nothing connected impact world positions to box-decal uniforms.

Now: `drawBoxDecals()` is the ring's second consumer.

1. Added a pure read-only `DynDecal::snapshotLiveSlots(Slot*, float* alpha, int max)` to
   the ring. It copies live slots + their lifetime-fade alpha **without** advancing or
   expiring any slot (that stays `gatherToDecalBatch()`'s job). Safe to call even when
   `MC2_DYNAMIC_DECALS` is off (returns 0).
2. `drawBoxDecals()` now: returns immediately unless `MC2_PROJECTED_DECALS`; snapshots the
   ring; if zero live impacts, draws nothing; otherwise loops the live slots issuing one
   projected box-decal per slot — `u_boxCenter = (wx,wy,wz)`,
   `u_boxHalf = (radius, ySpan, radius)`, `u_decalStrength = strength * slotAlpha`.
3. The hardcoded screen-center test box is gone (replaced by the ring loop).

Result: gate-ON → projected decals at real impact sites; gate-OFF → the pass does not run.

---

## Gate

`MC2_PROJECTED_DECALS` (default OFF). When OFF the projection pass returns before touching
any GL state → **gate-OFF byte-identical**. The baked-crater path runs regardless and is
untouched. Tuning env (all optional): `MC2_BOX_DECAL_STRENGTH`, `MC2_BOX_DECAL_YSPAN`,
`MC2_BOX_DECAL_NORMAL_REJECT`.

To see projected decals at impacts: `MC2_DYNAMIC_DECALS=1 MC2_PROJECTED_DECALS=1`
(plus `MC2_FX_FORCE_SPAWN` to drive impacts). `MC2_DYNAMIC_DECALS` populates the ring;
`MC2_PROJECTED_DECALS` turns on the projected consumer.

---

## Normal-reject — SHIPPED DISABLED by default (up-axis frame TODO)

The terrain GBuffer1 normal is a constant flat-up sentinel `(0.5,0.5,1.0)` decoding to a
**Z-up** `(0,0,1)` (`gos_terrain.frag` — all `rc_gbuffer1_*_flatUp()` sites), while
`box_decal.frag`'s reconstruct frame / `u_decalUpAxis` is **Y-up** `(0,1,0)`. A live
`dot(N, up)` reject would therefore discard ALL terrain pixels. So the reject threshold
defaults to `u_normalRejectCos = -1.5f` (the shader's documented disable: `<= -1.5`
never discards). The AABB world-clip (`abs(local) <= 1`) does the real coverage work.

**TODO (tracked, NOT this slice): reconcile the up-axis frame.** Reconcile
`u_decalUpAxis` against the actual reconstruct frame that the live `u_inverseViewProj`
produces (Z-up suspected from the MC2 world convention, NOT assumed Y-up), then the
normal-reject can be re-enabled with a positive threshold. Do not raise
`MC2_BOX_DECAL_NORMAL_REJECT` above -1.5 before that reconciliation, or terrain decals
vanish. This is a follow-up; the slice is de-risked by shipping the reject disabled.

---

## Out of scope (deferred follow-ups)

Removal/replacement of baked craters; promotion of projected to canonical; surface-class
masking (burnt/dirt/metal/snow → DECAL-SURFACE-CLASS-1); oriented (non-AABB) boxes; decal
texture atlas; reconciling/enabling the normal-reject by default; the missing scene-color
copy (`VFX-SCENECOLOR-GRAB-1`, unrelated).

---

## Validation

- Build: RelWithDebInfo, new TU `dynamic_decal_ring.cpp` registered in CMake; full relink.
- Gate-OFF (`MC2_PROJECTED_DECALS` unset): projection pass not run, no GL state touched,
  baked craters unaffected → byte-identical.
- Gate-ON + `MC2_GL_DEBUG_FATAL=1`: validates depth/normal sampler binds + per-slot draw +
  feedback-safety (samples the depth COPY, never the live attachment; COLOR0-only draw
  buffers so GBuffer1 normal / objectId untouched).
- Baked-crater no-regression: `MC2_DYNAMIC_DECALS=1` alone leaves `drawDecals` running.
- Visual confirm is non-deterministic (impact-driven via fixture) — see deliverable notes.

## Files changed

- `mclib/dynamic_decal_ring.h/.cpp` — new (ring) + `snapshotLiveSlots()` accessor.
- `mclib/crater.cpp`, `mclib/vk_scene_renderer_phases.cpp`, `GuiRuntime/GraphicsOptionsWindow.cpp`,
  `mclib/CMakeLists.txt` — ring producer wiring (cherry-pick + reapplied hunks).
- `shaders/box_decal.vert/.frag` — new (consumer shaders).
- `GameOS/gameos/gos_postprocess.{cpp,h}` — box-decal pass + the seam closure
  (ring-fed loop, `MC2_PROJECTED_DECALS` gate, normal-reject disabled default).
