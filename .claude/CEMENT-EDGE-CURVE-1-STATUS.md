# CEMENT-EDGE-CURVE-1 — STATUS

Branch `claude/nifty-mendeleev`. Ship commits **`81d9ace8`** (rev1) + **`d931fdbb`** (rev2).
Verified against HEAD `4b7aad66` (a sibling lane landed on top mid-run; rebuilt + redeployed).

**Bottom line: the briefed mechanism is REAL and LIVE by default, but it is NOT the
user's "atrocious outlines". The capture killed it as the explanation and found the
actual root cause. The standing ROAD-OVERLAY-LIGHT-1 attribution is BACKWARDS —
confirmed by measurement.**

---

## 1. META REVIEW — what the brief got right, and what it got wrong

Verified from `shaders/terrain_lod_chunk.frag`, not from status docs.

| Briefed premise | Verdict | Evidence |
|---|---|---|
| Albedo blends on `sqrt(pureConcrete)`, lighting on linear `pureConcrete` | **TRUE** | `:1365` `concreteColorBlend = sqrt(...)` → `:1548` albedo mix; `:1786` lighting mix is linear |
| The mismatch is live on a DEFAULT launch | **TRUE** | both lines are ungated and unconditional |
| It leaves a lit ring — "prove it with a capture" | **PROVEN LIVE, KILLED AS THE CAUSE** | gate moves 7–12% of pixels vs a measured 0.000% floor, but only **4–5/255** |
| `normalLight` makes it a ~3× night jump; ring is a **night** phenomenon | **REFUTED** | ring depth is comparable on DAY mc2_05 (−21.4/−28.3) and NIGHT mc2_23 (−15.1/−25.5) |
| `MC2_PAVED_OVERLAY_CLASS=1` would **ADD** outlines | **CONFIRMED** | 9–22% hard steps, max delta 20–29/255 (see §4) |

**Correction the brief missed.** `pureConcrete` is only *continuous* where it comes from
`smoothstep(2.0, 3.0, v_terrainType)`. The other two sources cannot produce an
intermediate value:

- `cementTransAlpha` is **binary by construction** — CEMENT-HARD-EDGE-1 replaced the
  bilinear cut with explicit half-plane tests, every branch assigning literal `1.0`/`0.0`
  (`:1015-1058`, comment: *"hard 0/1, never a curve"*).
- `cementHit` forces `pureConcrete = 1.0` (`:1361`).
- `cOwnA` (`:970`) is continuous but its branch is dead unless `MC2_CEMENT_ALPHA_ATLAS`
  is on, and that gate was **user-rejected**.

So the mismatch lives **only** in the one-tile-wide (128 wu) vertex-interpolated
terrainType ramp. That also means any fix here is structurally incapable of disturbing
the user-approved crisp cement transitions.

## 2. Measurement — floor MEASURED, and default parity PROVEN

Test bed `mc2-win64-abl-validate`. **The previous lane's `*_normal.ktx2` contamination
was reverted first** (8 files, backed up to the session scratchpad) so the install is
back to its shipped albedo-only state.

- **Two-run floor, mc2_23, all 6 poses: byte-identical → 0.000%** (measured, not recorded).
- **Default parity: all 6 poses byte-identical to the pre-change build** —
  `f5817cd62978 / 230819a2134e / 63a3d9759285 / 3827f091ab63 / e91000dd2628 / 007e602cce84`.
  The default-OFF claim is proven, not asserted.
- mc2_05 (user's own bookmarks) two-run floor: both poses byte-stable.
- 36/36 PNGs non-zero (disk was at 12 GB — checked).

mc2_23 is the **only stock night mission** (`DayLightPitch=9` → `isNight`), which is why
it is the right bed for the night claim.

## 3. The ring — MEASURED, and the real root cause

Using the gate's own changed-pixel mask as a **ramp locator** (it changes exactly the
pixels with `0 < pureConcrete < 1`), the transition band is **15–20 px wide** and covers
**7–12%** of a night frame. Its tonal profile:

| pose | terrain L | **band L** | concrete L | ring depth |
|---|---|---|---|---|
| runway_overview | 13.07 | **18.20** | 59.56 | **−18.11** |
| beacons_mid | 12.59 | **14.26** | 66.97 | **−25.52** |
| both_fields_overview | 12.70 | **17.00** | 63.26 | **−20.98** |

*ring depth = band luma − the plain terrain↔concrete midpoint; 0 would mean "smooth blend".*

The band hugs the **terrain** value and sits 15–25 luma **below** a straight blend — a
dark ring tracing every pad, then a sudden jump to bright concrete. **That is the user's
complaint.**

**Sharpening does not close it.** Sweeping the exponent 1.0 → 0.5 → 0.25 → 0.10 moved
ring depth only −18.1 → −15.0 on `runway_overview`, and made `beacons_mid` *worse*
(−25.5 → −27.9, because the concrete interior brightens faster than the band does).

**ROOT CAUSE (source + measurement).** The band has the wrong **albedo source**, not the
wrong lighting curve. At `:1548` albedo mixes toward `base`, and `base` is the **baked
colormap** unless a cement **word** exists for that tile. Pads with a word get
`cementAtlas × u_missionFlatLight` (`:957`) → L≈60–87. The terrainType ramp has **no
cement word**, so it keeps the dark colormap (L≈14–25) and receives *only* the lighting
exemption. Even at full exemption it tops out around L≈24 — it can never reach concrete
tone by lighting alone.

This is why every prior lever failed: `MC2_CEMENT_LIT`, the tone-match gates and this one
are all lighting-side, and the discontinuity is albedo-side. **Curing it requires
extending cement-word coverage to the terrainType ramp** (a CPU-side classification
change, adjacent to PAVED-OVERLAY-CLASS mode 2), not a shader curve.

## 4. `MC2_PAVED_OVERLAY_CLASS` — the standing attribution is BACKWARDS

Source: `pavedOverlayFlat` (`:1794`) is a **hard, per-tile, unfeathered** flag setting
`normalLight = 1.0` with zero feathering. Measured at the user's night poses:

| arm | max delta | mean abs delta @ changed px | **hard-step %** |
|---|---|---|---|
| `MC2_CEMENT_EDGE_CURVE` | 4–5/255 | ~1.8 | **0.00** (purely feathered) |
| `MC2_PAVED_OVERLAY_CLASS=1` | **20–29/255** | ~11.2 | **9.2–21.7** |

`MC2_PAVED_OVERLAY_CLASS=1` introduces genuinely **hard steps** into the image at 5–7×
the per-pixel magnitude. **It ADDS hard-edged outlines rather than removing them.**

**Retraction, stated explicitly:** the ROAD-OVERLAY-LIGHT-1 attribution — that the user's
"weird coloured edge" *is* the classification boundary and that closing the classification
gap cures it — is **wrong in the direction it claims**. Closing the gap in mode 1 adds
hard edges. Separately, the gate is **default-OFF**, so it could never have been the cause
of what the user sees on a default launch. This should not propagate into another status doc.

## 5. What shipped

`MC2_CEMENT_EDGE_CURVE` = **exponent** of the cement lighting-exemption ramp,
clamped `[0.05, 1.0]`, **default 1.0 = stock linear = bit-identical**.

- `pow(0,x)=0`, `pow(1,x)=1` → binary CEMENT-HARD-EDGE-1 path untouched at every value.
- Shader `max(u_cementEdgeCurve, 0.05)` guards the uninitialised-uniform case
  (`pow(x,0)=1` would flat-light the whole terrain).
- Registered (`RendererFeatureRegistry.h`), documented (`docs/tier1_env_vars.md`),
  smoke-allowlisted (`run_smoke.py`); `gate_status` verdict **OK**, no flags.
- Kept because it is a correct, parity-proven improvement to the lighting half **and** it
  is the ramp-locator instrument used above — **not** because it fixes the complaint.
  Documentation states this explicitly in both the registry and the tier1 doc.

**tier1: PASS 5/5, `[DEPLOY_FINGERPRINT] OK: exe sha=4b7aad66364a matches worktree HEAD`
on all five missions.**

## 6. Not done / open

- The actual cure — cement-word coverage for the terrainType ramp — is **not attempted**;
  it is CPU-side classification work and needs its own lane.
- mc2_01 / mc2_03 / mc2_10 / mc2_24 and the class-diag (`=32768`) overlays not captured;
  the mechanism was settled on mc2_23 + mc2_05 before the matrix was worth the disk.
- `abl-validate` normals moved to the session scratchpad, not deleted — restore from
  `releases/0.5 testing` if that install's content matters.
