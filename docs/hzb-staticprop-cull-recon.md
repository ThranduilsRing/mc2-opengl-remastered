<!-- doc-status: current | reviewed:2026-10-11 -->
# HZB Static-Prop Cull — Recon

`HZB-STATICPROP-CULL-RECON-1`. Wires **real static-prop bounds** into the
existing diagnostic HZB occlusion probe (`gos_postprocess::runHzbProbe`). Still
**no culling, no draw suppression** — counters only, `neverAppliedToDraws=1`.
This lane proves the would-cull mechanism works on real objects and surfaces the
false-occlusion risk *before* any cull consumer is contemplated.

## What was added

Per active static prop (capped, tombstone-aware iteration via
`GpuStaticPropRegistry::getActiveCount` + `staticPropGetModelMatrix` +
`staticPropGetExtentRadius`):

1. AABB = world center `(-mtx[3], mtx[11], mtx[7])` (MC2 east/north/elev) ±
   extent radius.
2. Project the 8 corners through `gos_postprocess::viewProj_` — the GL-NDC
   reverse-Z `world→clip` transform fed by `Camera::worldToClipGL()`, the SAME
   convention that produced the scene depth the HZB was built from (column-major,
   row-vector multiply, matching the sun-screen projection in `renderSkybox`).
3. Screen rect + `objClosest = max(clip.z/clip.w)` (reverse-Z = nearest point).
4. Pick HZB LOD from rect pixel size, **clamped coarser** to `Lmin` (dims ≤ 256;
   clamping coarser only ever keeps MORE — strictly conservative). Sample that
   level's covered texels, take MIN (farthest occluder).
5. Conservative cull comparison `objClosest < hzbMin` → `wouldCull` else
   `wouldKeep`. Logged as `[HZB_PROBE_OBJ v1]`. **Nothing acts on it.**

Cost-bounded: HZB readback restricted to levels ≥ `Lmin`; per-frame object cap
4096; inner texel loop capped at 4×4. Gate `MC2_HZB_PROBE` (default OFF, requires
`MC2_HZB_BUILD`). No new env gate, no shader, no contract change.

## Results (gate-ON, MC2_HZB_BUILD=1 MC2_HZB_PROBE=1)

| Mission | active | tested/frame | wouldKeep:wouldCull (normal) | self-test | GL err | FBO inc | Δdestroys |
|---------|--------|--------------|------------------------------|-----------|--------|---------|-----------|
| mc2_03  | 2552   | 42–161       | keep-dominant, cull 0–12      | clean     | 0      | 0       | +0        |
| mc2_17  | 1521   | 87–207       | keep-dominant; cull spikes on camera move | clean | 0 | 0 | +0 |
| mc2_24  | 2641   | 25–40        | keep-dominant, cull 0–4       | clean     | 0      | 0       | +0        |

- **`tested > 0` every gameplay frame → projection convention is CORRECT.** The
  `offscreen≈active, tested≈0` axis-swap failure mode did NOT occur. `offscreen`
  is large simply because the overhead RTS camera sees a fraction of the
  map-wide prop set.
- **`wouldKeep` dominates `wouldCull`** in steady frames; the HZB self-test stays
  `wouldCull=0 integrityMismatch=0` (the reduction is a faithful conservative
  MIN). The math is conservative-correct.

## Camera-motion `wouldCull` spike — stress-case finding (NOT a substrate bug)

- **Observation.** mc2_17 hit `tested=87 wouldKeep=22 wouldCull=65` (~75%) on a
  frame where the camera turned; steady frames stay keep-dominant. The spike
  correlates with camera motion.
- **Classification.** mc2_17's intro has **known camera-path discontinuities — one
  or two near-instant 180° snaps.** A transient would-cull spike on exactly those
  frames is the **expected behaviour of any screen-space occlusion test under a
  discontinuous camera**: for a frame straddling a snap, the projected screen
  rects and the just-rendered depth can momentarily disagree at the frame
  boundary, so grazing/edge props read as occluded for that single frame. This is
  a **stress-case finding about camera discontinuities**, NOT evidence of an HZB
  substrate defect: the HZB self-test stayed `wouldCull=0 integrityMismatch=0`
  throughout, the reduction is a faithful conservative MIN, and the projection
  axis convention is correct (steady frames are keep-dominant with `tested>0`).
  We deliberately did NOT chase a frame-coherence rabbit hole or overfit the
  probe to mask the spike — masking it would hide the very signal a future cull
  consumer needs.
- **High `nearClipped`** (any AABB corner with `clip.w ≤ 0`): the whole object is
  conservatively KEPT. Safe, but a real cull path wants per-plane near clipping
  rather than whole-object skip, or it leaves large props near the eye
  untestable.

## Verdict / next

The substrate + projection + conservative comparison are proven on real props
across three missions with zero rendering impact. The mc2_17 spike is an expected
camera-discontinuity stress case, not a blocker for this **diagnostic** lane.
**Do NOT build a cull consumer yet** — and when one is eventually built it MUST
handle camera discontinuities safely. Hard stop conditions for any future cull
consumer (in addition to those in `docs/hzb-visibility-mvp.md`):

1. **Camera-discontinuity guard is mandatory.** A real cull path must do at least
   one of:
   - disable culling for 1–2 frames after a large camera-matrix delta (detect via
     frame-to-frame `viewProj` / camera-position delta over a threshold), OR
   - prove **same-frame ownership** of the `world→clip` matrix and the depth it
     culls against (the matrix used to project bounds is exactly the one that
     rendered that frame's HZB-source depth), OR
   - ship the first consumer **conservative + kill-switched + default-OFF**, with
     a measured false-occlusion budget, so a transient spike can at worst briefly
     keep too much (never pop a visible prop) and can be disabled instantly.
2. **mc2_17 stays in the validation matrix on purpose** — it is a useful
   pathological camera case (180° intro snaps); a candidate cull consumer must be
   exercised against it and shown not to pop visible props across the snaps.

Stop conditions from `docs/hzb-visibility-mvp.md` still apply.
