# MC2-05-BRIGHTEN-1 — STATUS (2026-07-26)

Lane MC2-05-BRIGHTEN-1. Worktree `A:/Games/mc2-main`, branch
`claude/nifty-mendeleev`.

- Commits landed (all mine): `57d3d016`, plus the note amendment + this doc.
- Deploy: `A:/Games/mc2-opengl/mc2-win64-v0.3` (NOT `0.4c` — it holds the
  blessed golden baseline exe).
- **tier1: PASS (5/5) with explicit `DEPLOY_FINGERPRINT OK` on all five
  missions** (`exe sha=57d3d016570f matches worktree HEAD`), run with an
  explicit `--exe`. MOVER-COHERENCE 0 violations on all five.
  Contracts **72 passed / 0 failed**. `check-env-registry.sh` PASS (997 vars).
  `check-visual-bookmark-honesty.py` OK (33 pose files; 3 pre-existing
  acknowledged WARNs, none mine).
- Evidence: `scratch/m05b/`. **111 PNGs, 0 zero-byte.** Disk ~12 GB free
  throughout; no build tree added (Tracy still compiled in none).
- **★ Nothing about the DEFAULT RENDER changed.** `MC2_PROP_BASE_LIGHT_DIR`
  is still default-OFF. **The user's building face still looks exactly as it
  did on a default launch.** This lane removed a false blocker; it did not
  ship the fix.

| Objective | Verdict |
|---|---|
| **Explain the mc2_05 brightening** | ✅ **Explained, with a receipt (§2). It is a measurement confound, not a shader defect.** |
| **Is mc2_05 unique, or the only place we looked?** | ✅ **Neither. The stated distinguishing condition was false twice over (§3).** |
| **Fix it if it is a genuine defect** | ✅ **The defect was in the POSE SET, and it is fixed and verified (§4).** |
| **The design's "can never brighten" claim** | ✅ **VINDICATED. It was correct; the measurement that contradicted it was wrong.** |
| **Re-grade on the honest building-face poses** | ✅ **Done — 24 poses, 5 maps, all at a 0.000 % floor (§5).** |
| **mc2_05 night not degraded** | ⚠️ **Partly. Byte-identical on a default launch; the approved SPOTLIGHT FIXTURES remain ungradeable (§7).** |
| **Recommended default** | **Still OFF (§6)** — but for ONE remaining reason, not this one. |

---

## 1. ★ META REVIEW — two inherited premises checked, both failed

The brief told me not to re-derive four already-ruled-out items (terrain
byte-identity, OFF/OFF control, `gate=0.0` no-op, `get_base_light`
non-negative). I did not. I checked the two premises it *did* rest its lead on,
and neither survived.

**Premise A: "mc2_05 is the only tier map with a day/night CYCLE, so suspect a
night-keyed consumer."** The cycle does not exist. `Camera::update`'s per-frame
`updateDaylight()` call is **commented out** (`mclib/camera.cpp:2180`, with the
in-source comment `WE ARE NEVER TRANSITIONING ANYMORE`), and `updateDaylight`
is only ever called once with `bInitialize=true` (`camera.cpp:2766`).
`DayToNightTime=864000` is parsed (`camera.cpp:338`) and then never advances
anything. **`nightFactor` is a per-mission constant on every map.** The
distinguishing condition that pointed the whole investigation at night-keyed
consumers was dead code.

**Premise B: the effect is a property of the MAP.** It is not even a property
of the map. On mc2_05 the `refinery` and `east_compound` sites **darken
correctly** while `liao_compound` "brightens" — same map, same mission, same
constant `nightFactor`, opposite sign. That killed the night-consumer theory
before I read a single line of the frag shader, and redirected me to the site.

---

## 2. ★ THE MECHANISM, WITH ITS RECEIPT

**There is no brightening mechanism. The gate does not brighten mc2_05.**
The reported effect is a confounded measurement on an unstable pose.

**Step 1 — reproduced it first.** At strength 0.55 I measured
`liao_compound_lightaway` mean luminance 84.15 → 87.50 (+3.35) with 22.4 % of
pixels brighter, and `liao_compound_lightfacing` +2.36. So the finding was
real as reported; I did not dismiss it.

**Step 2 — the spatial signature.** Rendering the signed per-pixel delta
(`scratch/m05b/signed_liao.png`, I looked at it) shows two superimposed and
opposite things: the buildings themselves go **darker** (correct, the designed
effect), and a large **smooth radial blob** in the upper right goes brighter.
A smooth radial falloff is a point-light footprint, not a shading term.

**Step 3 — the control the previous lane never ran.** It recorded a
**0.000 %** gate-OFF/gate-OFF floor for these poses. I ran it. It is
**15.043 %** at maxAbs 28 (`lightfacing` 14.766 % / 29). And the OFF-vs-OFF
delta is **the same radial blob in the same place**
(`scratch/m05b/sig_OFFvsOFF2.png`). The "signal" and the "noise" are the same
object.

**Step 4 — attribution.** Night object light pools are **dimmed near movers**:
`MC2_LIGHT_POOL_MOVER_DIM`, default **0.25**
(`GameOS/gameos/gos_terrain_lighting.cpp:651`), under
`MC2_TERRAIN_LIGHT_POOLS` / `MC2_LIGHT_POOL_OBJECTS`, **both default ON**
(`:637`, `:641`). Movers do not resolve to the same position twice — the same
root cause already on record for the mc2_10 drift. So this pose's pool
amplitude is a function of mover state, and the pose measures mover positions.

**Step 5 — the decisive receipt.** Same pose, `liao_compound_lightaway`:

| arm | condition | changed % | maxAbs | mean lum |
|---|---|--:|--:|---|
| gate-OFF vs gate-OFF | pools **ON** (default) | 0.000 % | 0 | 83.20 → 83.20 |
| **gate-ON vs gate-ON** | pools **ON** (default) | **46.389 %** | **82** | **87.50 → 94.14** |
| gate-OFF vs gate-OFF | pools **OFF** | **0.000 %** | **0** | 43.69 → 43.69 |
| gate-ON vs gate-ON | pools **OFF** | **0.000 %** | **0** | 43.36 → 43.36 |
| gate-OFF vs gate-ON | pools **OFF** | 4.486 % | 44 | 43.69 → **43.36**, brighter **0.00 %** |

**Two runs with IDENTICAL settings differ by 6.64 mean luminance and 46.4 % of
pixels, 30.1 % of them brighter.** The entire reported blocker was +7.4
luminance. **The run-to-run swing is the same size as the effect it was used to
measure.** Turning the pools off collapses every arm to byte-identical and
resolves the gate to a clean **−0.33 with 0.00 % of pixels brighter**.

Across five gate-OFF samples the OFF arm sat at 83.20 four times out of five;
the gate-ON arm ranged 85.97 / 87.50 / 94.14. The gate perturbs enough startup
work to tip this knife-edge pose into the mover-chaotic basin — but *which*
basin it lands in is not a lighting measurement either way.

**Corroboration from source, independently.** An exhaustive trace of every
consumer of `lit` downstream of the prop vertex path found **no site with
`d(output)/d(lit) < 0` at default gates**. The one genuinely sign-negative
expression (`static_prop.frag:446-449`, `shadowW = 1.0 - clamp(litMax,0,1)`,
the shape the brief predicted) is **inert by default**: it requires
`kFlagDecalFill`, set only under `MC2_TERRAIN_DECAL`, which is default-OFF —
and even armed it needs `lit_ch < 0.20 * litMax` to invert. `calcLightPools`,
`v_fixtureGlow`, the PBR specular block and the GBuffer1 writes are all
**independent of `lit`**. There is no auto-exposure and no bloom in this tree.
So the algebra and the measurement now agree: **`lit` can only decrease, and
the frame only darkens.**

---

## 3. Is mc2_05 unique, or merely the only place we looked?

**Neither — the premise was wrong.** There is no day/night cycle on any map
(§1). The real distinguishing condition is **a mover-coupled night light pool
inside the frame**, which is a property of the *site*, not the map: two other
mc2_05 sites darken correctly at a 0.000 % floor.

I did not need a forced-night-factor experiment on other maps to test the
original hypothesis, because the hypothesis was falsified within mc2_05 itself.
What I did instead was re-grade every map (§5): **no pose on any of the five
maps brightens.**

---

## 4. What I changed

**One tracked test asset and two doc strings. No code, no shaders.**

`tests/visual/bookmarks/mc2_05_building_faces.json`:
- **Removed** `liao_compound_lightaway` / `liao_compound_lightfacing`. This is
  the same action the previous lane took on the LiaoSpotlight yard pose for the
  same reason (floor exceeds the effect). Leaving them in was an active harm:
  the determinism ratchet grades all pairs across 3 runs, and a pose with a
  46 % gate-ON floor flaps it.
- **Added** `refinery_lightfacing` to restore a 180°-opposed pair at a site
  that measures a 0.000 % floor, so the set does not lose its orientation
  contrast when the liao pair goes.
- **Rewrote the note** with the measured floors, the pool/mover attribution,
  and the correction to the day/night-cycle claim so it cannot be cited again.

`RenderCore/RendererFeatureRegistry.h` (`MC2_PROP_BASE_LIGHT_DIR` entry) —
two clauses that did not match the shipped code:
- `base_light` → the measured truth (`ambientTerm`), the correction the
  previous lane made in code but not in the registry.
- "Applied **BEFORE** the FAR-BACKDROP-LIT mix" → **AFTER**. The shader
  computes `seed` (including the mix) first and multiplies at the `lit`
  assignment. Moot while `MC2_FAR_BACKDROP_LIT` defaults to 0, but it was
  simply false.
- Recorded the never-brightens re-verification.

**Verification of the fix**, revised set, **DEFAULT gates (pools ON)**:

| pose | floor % | chg % | mean lum OFF→ON | brighter % |
|---|--:|--:|---|--:|
| refinery_lightaway | **0.000** | 14.240 | 72.11 → 70.92 | **0.00** |
| refinery_lightfacing *(new)* | **0.000** | 18.149 | 81.40 → 80.25 | **0.00** |
| east_compound_lightfacing | **0.000** | 8.269 | 101.76 → 101.26 | **0.00** |
| control_no_buildings | **0.000** | 0.000 | 9.88 → 9.88 | **0.00** |

All four gradeable at default gates, with no special env required.

---

## 5. ★ RE-GRADE — 24 poses, 5 maps, noise floor published per pose

Every row below has its own two-run gate-OFF floor in the first column. **The
`brighter %` column is the whole verdict: it is 0.00 everywhere.**

| pose | floor % | chg % | maxAbs | mean lum OFF→ON | brighter % | darker % |
|---|--:|--:|--:|---|--:|--:|
| mc2_01 control | 0.000 | 0.000 | 0 | 57.61 → 57.61 | 0.00 | 0.00 |
| mc2_01 hangar_lightaway | 0.000 | 1.101 | 27 | 69.11 → 69.06 | 0.00 | 0.84 |
| mc2_01 quonset_lightaway | 0.000 | 1.157 | 25 | 77.00 → 76.96 | 0.00 | 0.93 |
| mc2_01 quonset_lightfacing | 0.000 | 0.192 | 15 | 80.43 → 80.42 | 0.00 | 0.13 |
| mc2_03 control | 0.000 | 0.000 | 0 | 32.84 → 32.84 | 0.00 | 0.00 |
| mc2_03 civstore_lightaway | 0.000 | 5.878 | 23 | 28.52 → 28.40 | 0.00 | 4.89 |
| mc2_03 industrial_lightaway | 0.000 | 9.272 | 50 | 56.27 → 56.11 | 0.00 | 7.67 |
| mc2_03 industrial_lightfacing | 0.000 | 10.879 | 42 | 53.29 → 53.10 | 0.00 | 9.15 |
| mc2_03 sam_lightaway | 0.009 | 5.128 | 217 | 62.05 → 61.95 | 0.00 | 4.17 |
| mc2_03 sam_lightfacing | 0.010 | 5.139 | 179 | 62.10 → 62.00 | 0.01 | 4.35 |
| **mc2_10 compound_lightaway** | 0.000 | **20.289** | 30 | 34.59 → 33.70 | **0.00** | **20.18** |
| **mc2_10 compound_lightfacing** | 0.000 | **5.341** | 21 | 31.54 → 31.31 | **0.00** | **5.16** |
| mc2_10 control | 0.000 | 0.273 | 16 | 35.31 → 35.30 | 0.00 | 0.27 |
| mc2_10 settlement_lightaway | 0.000 | 4.755 | 34 | 42.25 → 41.94 | 0.00 | 4.57 |
| mc2_10 settlement_lightfacing | 0.000 | 5.184 | 26 | 35.85 → 35.42 | 0.00 | 5.17 |
| mc2_24 control | 0.000 | 0.118 | 12 | 75.44 → 75.44 | 0.00 | 0.08 |
| mc2_24 fortress_lightaway | 0.000 | 0.003 | 3 | 104.29 → 104.29 | 0.00 | 0.00 |
| mc2_24 industrial_lightfacing | 0.000 | 2.943 | 27 | 118.32 → 118.21 | 0.00 | 2.29 |
| mc2_24 palace_lightaway | 0.000 | 1.178 | 12 | 64.58 → 64.57 | 0.00 | 0.38 |
| mc2_24 palace_lightfacing | 0.000 | 4.318 | 20 | 62.62 → 62.51 | 0.00 | 2.98 |
| mc2_05 ×4 (revised set) | 0.000 | 0–18.1 | ≤242 | all down | **0.00** | — |

**Mean luminance is down or flat on every one of the 24 poses. Not one pixel
on any map is brighter above the 0.5-luminance threshold** (the single 0.01 %
on `mc2_03 sam_lightfacing` sits at that pose's own 0.010 % mover floor).

**The mc2_10 acceptance test reproduces exactly.** The light-away view moves
20.289 % of pixels; the same buildings from 180° opposite move 5.341 %. That
4:1 orientation asymmetry is the demonstration that the term does what the
user asked for, and it is now confirmed on an independently built and deployed
exe. Day maps are near no-ops (mc2_01 ≤1.2 %, mc2_24 ≤4.3 %), as expected when
the sun term dominates and is not scaled.

`maxAbs` 217/179/242 on the `sam` and `east_compound` rows are single-frame
mover flashes, visible in those poses' own floors; they are not gate effects.

---

## 6. Recommendation

**Keep `MC2_PROP_BASE_LIGHT_DIR` default-OFF — but the mc2_05 blocker is
CLEARED and should not be cited again.**

What changed since the previous lane: its §4 blocker ("the gate brightens
mc2_05 by up to +71 %, on user-approved territory") **does not exist**. The
gate darkens strictly on all 24 poses across all five maps.

**The one remaining blocker is the previous lane's §5, not its §4:** the
`kFlagIsAlphaTest` exclusion never fires, because `TreeAppearance::
registerStatic` (`mclib/bdactor.cpp:7329-7332`) still writes the retired
`GetIsSpotlight()` meaning into bit 2. Foliage is therefore being modulated by
a term explicitly designed to exclude it. That is undesigned behaviour on the
default-render path once the gate is on, and it is a sufficient reason on its
own not to flip. I did not fix it: it also revives two-sided `abs(NdotL)`
foliage lighting for every statically-registered tree
(`static_prop.vert:731`), which is a default-render change needing its own
gate and its own click-through. It remains a clean, well-scoped slice.

Ordered next steps:
1. **Land the `kFlagIsAlphaTest` fix as its own gated slice.**
2. **Then flip to 0.55**, on the §5 evidence here plus the previous lane's
   per-surface map (walls 20–27 %, roofs ~9 %, sun-facing faces 0–5 %, terrain
   exactly 0 %). I did not re-run the strength sweep; the 0.55 value rests on
   the previous lane's saturation measurement, which I did not verify.
3. **User click-through on mc2_05 and mc2_10 at 0.55** before any default flip
   — mc2_05 is approved territory and the ON state does change it (§7).
4. Optional: a stable mc2_05 spotlight-fixture pose (§7), and a stable
   liao_compound pose if that site is wanted back.

---

## 7. ★ Not done / not claimed

- **mc2_05 night non-degradation is NOT fully proven, and I will not claim it.**
  On a **default launch** mc2_05 is byte-identical, because the gate is OFF —
  that part is certain. But with the gate **ON** at 0.55 mc2_05 *does* change
  (−0.5 to −1.2 mean luminance on three poses). "Darker" is the intended
  direction, but mc2_05's look is **user-approved**, so whether that change is
  acceptable is the user's call, not a measurement. Worse, **the LiaoSpotlight
  fixtures — which ARE the approved night look — remain ungradeable** (the
  previous lane measured an 84–91 % floor from the animated cone glow and
  removed the pose; I did not author a stable replacement). So the specific
  thing the user approved is still unverified under the gate.
- **The mover/light-pool nondeterminism itself is NOT fixed.** I explained and
  routed around it. That night light pools are mover-coupled by default
  (`MC2_LIGHT_POOL_MOVER_DIM=0.25`) means **any** pose with a night pool and a
  passing mover is ungradeable, on any map. Other pose sets may carry the same
  latent false floor; I only audited mc2_05. Worth a sweep.
- **Day AND night is covered ACROSS maps, not WITHIN a map.** mc2_05/mc2_10
  are night, mc2_01/03/24 day, each at its mission-fixed `nightFactor`. I did
  not force a night factor on a day map, and with `updateDaylight` disabled
  (§1) there is no supported way to do so.
- **Perf — NOT MEASURED.** Tracy is compiled in none of the build trees and the
  brief forbids adding one. One dot + one mix per vertex is asserted cheap.
- **Strength sweep — NOT RE-RUN.** Only 0.55 was graded here.
- **Golden regions not graded, no re-bless proposed.** No release artifacts
  touched (`release_assets/`, `releases/` are user-gated).
- **A latent NaN asymmetry, noted not fixed:** `ndl` uses `max(dot(...),0.0)`,
  which launders a NaN to 0, whereas the new `mix(1.0-s, 1.0, wrap)`
  propagates it into `lit`. Only reachable if `u_terrainSunMC2` can be zero.
- **The `east_compound` pose carries occasional single-frame mover flashes**
  (0.04 % floor, maxAbs ~250). Kept — it is far below the effect — but it is
  not a 0-floor pose in every run.
- No push, no `--amend`, no foreign-process kills. Lane overlap noted:
  `FIXTURE-GLOW-1` claims both static_prop shaders; **I edited neither.**
