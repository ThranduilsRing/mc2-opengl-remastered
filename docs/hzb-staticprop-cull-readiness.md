<!-- doc-status: current | reviewed:2026-10-11 -->
# HZB Static-Prop Cull — Readiness

`HZB-STATICPROP-CULL-READINESS-1`. Hardens the diagnostic static-prop HZB probe
so we can DECIDE whether a future default-OFF static-prop cull consumer is safe.
Still **diagnostic only — no culling, no draw suppression, `neverAppliedToDraws=1`.**
Builds on `HZB-STATICPROP-CULL-RECON-1`. Gate: `MC2_HZB_PROBE` (requires
`MC2_HZB_BUILD`); no new gate.

## Camera-discontinuity detection (HZB-CAMERA-DISCONTINUITY-GUARD-1)

The probe derives the camera pose each frame with no new plumbing: it unprojects
the NDC near-centre `(0,0,1)` and far-centre `(0,0,0)` through `inverseViewProj_`
(reverse-Z, ZERO_TO_ONE) to get a world near-point (camera-position proxy) and a
forward vector `normalize(far − near)`. `viewProj_`/`inverseViewProj_` are set
every frame in `setTerrainMVP` **before** the scene depth render, so they are
same-frame coherent with the HZB-source depth (this also resolves the recon's
open frame-coherence question: matrix and depth ARE from the same frame).

Frame-to-frame it compares against the previous frame's pose:
- `fwdAngleDeg` — angle between this/previous forward vectors.
- `posDelta` — world-unit distance between this/previous near-points.

A frame is flagged **`unsafeForCull`** when `fwdAngleDeg > 30°` OR
`posDelta > 0.25 × mapHalfExtent` (position guard only when the map extent is
known). A smooth gameplay pan is a few deg/frame; a 180° snap is unmistakable.
Raw deltas are always logged so the thresholds can be re-tuned from data — the
guard never hides the raw signal.

## mc2_17 camera snap

mc2_17's intro contains one or two near-instant ~180° camera snaps. On those
frames `cameraDiscontinuity=1` and the (large) raw would-cull count is moved into
`skippedCameraDiscont` rather than `wouldCullGuarded`. This is the intended
behaviour of the guard: a screen-space occlusion test is unreliable on a frame
that straddles a camera discontinuity, so a future cull consumer must not act on
those frames. The spike is a **stress case**, not an HZB substrate defect.

## Counters (`MC2_HZB_PROBE`, logged bounded: first 5 frames, every 600, and
every unsafe frame)

`[HZB_PROBE_OBJ v2]`:
- `active` / `scanned` — active static props / how many we fetched this frame.
- `tested` — projected on-screen and depth-tested. `tested>0` in gameplay frames
  confirms the projection convention is correct (axis-swap would zero it).
- `wouldKeep` — kept (object nearest point in front of the footprint occluder).
- `wouldCullRaw` — raw conservative cull decisions (ALWAYS counted).
- `wouldCullGuarded` — raw culls on SAFE frames = what a guarded consumer would
  actually cull.
- `skippedCameraDiscont` — raw culls suppressed because the frame is unsafe.
  Invariant: `wouldCullRaw == wouldCullGuarded + skippedCameraDiscont`.
- `nearClippedKeep` — AABB crossed the near plane → conservatively KEPT.
- `offscreen` — projected rect fully off-screen.
- `invalidRect` — non-finite depth/rect (should be ~0).
- `cameraDiscontinuity` / `fwdAngleDeg` / `posDelta` / `discontFramesCumulative`.

`[HZB_PROBE_LOD v1]` — histogram of the selected HZB LOD over tested objects.
`[HZB_PROBE_CULLCAND v1]` — bounded sample (≤8/frame, SAFE frames only) of guarded
would-cull candidates: prop index, screen rect (UV), `objClosest`, `hzbMin`,
`gap = objClosest − hzbMin` (negative = behind the occluder), selected LOD.

## Interpreting raw vs guarded

- **Steady (safe) frames:** `wouldCullGuarded` is the trustworthy occlusion rate.
  Inspect `[HZB_PROBE_CULLCAND]`: a large negative `gap` = deeply hidden (true
  occlusion); a tiny `gap` (within a texel of the occluder) = a marginal,
  LOD/grazing-angle case that a conservative consumer should treat cautiously.
- **Unsafe frames:** ignore `wouldCullRaw`; it is expected to spike. The guard
  has moved it to `skippedCameraDiscont`.

## Depth-gap margin sweep (HZB-CULL-MARGIN-SWEEP-1)

`[HZB_PROBE_MARGIN v1]` counts guarded would-cull candidates (SAFE frames) that
survive `gap < -margin` for a fixed margin ladder, plus `minGap` / `maxGap` /
`closestToZeroNegGap` / `numMarginal (-1e-4 < gap < 0)`. Representative
steady-frame results (mc2_03/17/24, gate-ON):

| margin   | mc2_03 busy | mc2_17 busy | mc2_17 light | mc2_24 |
|----------|-------------|-------------|--------------|--------|
| 0.00000  | 38          | 68          | 20           | 7      |
| 0.00005  | 38          | 68          | 19           | 7      |
| 0.00010  | 37          | 68          | 19           | 7      |
| 0.00025  | 37          | 68          | 18           | 6      |
| 0.00050  | 34          | 68          | 16           | 6      |
| 0.00100  | 26          | 64          | 16           | 6      |

- The **bulk of guarded culls are DEEP true occlusions**: minGap ≈ −0.016
  (mc2_03) to −0.043…−0.065 (mc2_17); mc2_17's busy frame keeps all 68 culls
  through a 1e-3 margin → stable across camera movement, not knife-edge.
- **Marginal candidates are rare** — `numMarginal` was 0–1 per frame. The
  closest-to-zero guarded candidates were `-0.000052` (mc2_03), `-0.000011`
  (mc2_17), `-0.000122` (mc2_24).
- A **1e-4 (0.00010) margin** removes the near-zero grazing candidates (≤1 per
  busy frame) while sparing essentially all confident culls. 2.5e-4 begins to
  shave confident culls (mc2_03 37→37, mc2_24 7→6); 1e-3 is clearly too
  aggressive (mc2_03 38→26).

**Recommended minimum margin for any future cull consumer: `gap < -1.0e-4`**
(reverse-Z window-depth units), i.e. cull only when the object's nearest point is
behind the footprint occluder by more than 1e-4. This is the knee of the sweep:
it eliminates the marginal/grazing false-positive risk surfaced by the readiness
slice while retaining the deep true occlusions.

## Acceptance bar for a future real cull consumer (`HZB-STATICPROP-CULL-CONSUMER-0`)

Do NOT build a consumer until ALL hold, measured across `mc2_03`, `mc2_17`,
`mc2_24` (mc2_17 stays in the matrix as the pathological camera case):
1. `wouldCullGuarded` candidates, inspected via `[HZB_PROBE_CULLCAND]` (and/or a
   later visual highlight), are TRUE occlusions — no visible prop in the guarded
   set. The measured false-occlusion rate on safe frames is ~0.
2. The camera-discontinuity guard fires on the mc2_17 snaps (it does) and the
   consumer consumes `unsafeForCull` to skip those frames.
3. The consumer is default-OFF, kill-switched, and conservative (a failure can at
   worst keep too much, never pop a visible prop).
4. HZB self-test stays `wouldCull=0 integrityMismatch=0`; 0 GL errors; +0 destroys.

## Readiness verdict (post margin sweep)

**The first consumer is READY to be PLANNED with a mandatory margin + the
discontinuity guard — not yet to ship culling.** The margin sweep shows the
marginal candidates are few, near zero, and removed by a 1e-4 margin, while the
bulk of guarded culls are deep, stable true occlusions. A
`HZB-STATICPROP-CULL-CONSUMER-0` may therefore be designed, provided it:
- culls only on `gap < -1.0e-4` (recommended margin above),
- consumes `unsafeForCull` to skip camera-discontinuity frames,
- is **default-OFF, kill-switched, conservative** (worst case keeps too much),
- and, during bring-up behind the gate, is visually spot-checked against the
  `[HZB_PROBE_CULLCAND]` candidates to confirm no visible prop is culled (the
  overlay was skipped here; visual confirmation moves to consumer bring-up).

Not a blanket green light to suppress draws: culling stays blocked until that
consumer exists, is gated OFF by default, and its on-screen false-occlusion is
measured ~0 across mc2_03/17/24 (mc2_17 retained as the pathological camera case).

## Stop conditions

- If camera state were unavailable/ambiguous: document and do not fake it.
  (It is available — unprojection via `inverseViewProj_`, reliably set per frame.)
- If `[HZB_PROBE_CULLCAND]` shows VISIBLE props in the guarded set on steady
  frames → keep this lane diagnostic-only; do NOT proceed to a consumer.
- A debug overlay drawing candidate rects was scoped but is MEDIUM effort
  (shader + ImGui plumbing); per the slice's stop condition it was SKIPPED in
  favour of the bounded `[HZB_PROBE_CULLCAND]` log. Revisit only if the logs are
  inconclusive.
- All stop conditions from `docs/hzb-visibility-mvp.md` and
  `docs/hzb-staticprop-cull-recon.md` still apply. **No real culling yet.**
