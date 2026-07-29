# SSAO-BLUR-1 — STATUS: blur LANDED and PROVEN WORKING. Default still OFF.

Lane: SSAO-BLUR-1 | worktree A:/Games/mc2-main | branch claude/nifty-mendeleev
Commits: `be445b02` (blur) → `7ca604d1` (sigma env-tunable) → `3bf826ff` (sigma default 0.10)
Graded exe: `A:/Games/mc2-opengl/mc2-win64-abl-validate/mc2.exe`
tier1: **PASS 5/5** with `[DEPLOY_FINGERPRINT] OK: exe sha=31e1a9e81cc0 matches worktree HEAD`
Date: 2026-07-28 | Predecessor: `.claude/SSAO-RETUNE-1-STATUS.md`

## Bottom line

Defect #2 is **fixed and measured**. The blur removes **−68.6 %** of the AO
buffer's high-frequency noise on the pose that actually carries noise, and the
1:1 crop shows raw salt-and-pepper dither becoming coherent shading.

**But `MC2_SSAO` should stay default-OFF**, for a *new*, third reason that only
became visible once the wide gameplay-altitude poses were measured: at the
corrected radius the AO is not a contact term. On oblique/cross-map views
**21–75 % of screen pixels are darkened**, as a broad mottled wash rather than a
contact line. Denoising made that wash *clean*; it did not make it *correct*.

## META REVIEW — brief premises re-checked against source

| Premise | Verdict | Evidence |
|---|---|---|
| `runSSAO` has two draws, no blur | **TRUE** | `gos_postprocess.cpp:2777-2856` read in full |
| Only denoise is a 4-tap ±0.5-texel box | **TRUE** | `ssao_apply.frag`; its comment does claim "denoises" |
| Slider caps at 32.0, setter at 64.0 | **TRUE but misleading** | the slider binds `&pp->aoRadius_` **directly** and never calls `gos_SetSsaoRadius`, so the setter's 64.0 never applied — **32.0 was the only real cap** |
| brief path `code/GraphicsOptionsWindow.cpp` | **WRONG** | actual `GuiRuntime/GraphicsOptionsWindow.cpp` |
| brief path `code/visual_tuning_profile.cpp` | **WRONG** | actual `GameOS/gameos/visual_tuning_profile.cpp` |
| `MC2_VISUAL_TUNING_FILE` unregistered/undocumented | **TRUE, one worse** | also **not in the `run_smoke.py` allowlist** (ENV_DROP_RISK) |

**New finding:** `MC2_VK_POST_SSAO` (default-OFF) reuses the `ssao`/`ssao_apply`
SPIR-V **verbatim** and takes highest precedence at the SSAO site, so it does
**not** get this blur. Documented; porting it is open follow-up.

## Evidence — mechanism proof

Two arms, same exe, `MC2_SSAO=1 MC2_SSAO_DEBUG=1` (raw AO buffer), mc2_01, 9
bookmarks, `--runs 2`. **Noise floor 0: all 9 bookmarks byte-stable in both
arms.** 6/9 hashes differ between arms → the blur path demonstrably executes.
0 zero-byte PNGs.

HF noise = mean |pixel − 4-neighbour mean| on the **true half-res grid**
(stride-2). Measuring at full res is wrong here — the apply pass upsamples the
half-res buffer, so a full-res metric measures interpolation, not the estimator.

| pose | HF blur-OFF | s=0.02 | s=0.10 | s=0.40 |
|---|---:|---:|---:|---:|
| **cliff_wall_sideon** | 2.8584 | 1.3567 (**−52.5 %**) | **0.8966 (−68.6 %)** | 0.8768 (−69.3 %) |
| cliff_side_on | 1.2009 | −1.9 % | −3.3 % | −2.9 % |
| cross_map_view | 0.4842 | +0.0 % | +0.0 % | +0.0 % |
| oblique_fog | 0.4098 | +0.0 % | +0.0 % | +0.0 % |
| oblique_horizon | 0.4599 | −0.2 % | −0.2 % | −0.2 % |

Visual proof: `scratch/ssaoblur1/CMP_ao_blur_1to1.png` (OFF | s0.02 | s0.10, 2×
nearest, densest-occlusion crop). OFF is unmistakable salt-and-pepper dither —
the exact defect SSAO-RETUNE-1 described. ON is smooth and coherent.

### A diagnosis I got wrong, and the measurement that refuted it

Commit `7ca604d1`'s message claims the blur was inert on oblique poses because
grazing surfaces make adjacent texels differ in depth by more than the tolerance,
so every neighbour tap was rejected. **The s=0.40 arm refutes this**: at 40 %
relative depth there is effectively no edge-stopping left and those poses still
show +0.0 % / −0.2 %. The real reason is simpler — **those poses have almost no
HF noise to begin with** (0.41–0.48 vs 2.86 on `cliff_wall_sideon`). The AO there
is a broad smooth wash, not speckle, so a denoiser correctly has nothing to
remove. The blur is behaving properly. Corrected in `3bf826ff`'s message; the
wrong text survives in `7ca604d1` because amending is forbidden.

## Chosen values

- **`aoRadius` 3.0 → 24.0.** SSAO-RETUNE-1's 12–24 band; 40 measurably invents
  occlusion on flat faces; independent convergence with
  `.claude/vk-post-ssao/visual_tuning_aoboost.json`.
- **`aoBlurDepthSigma` 0.10**, by the sweep above: takes essentially all the
  available denoise while keeping a real edge-stop. 0.40 buys +0.7 pp more and
  bleeds more occlusion outward (`%<250` 21.98 → 22.04 vs 19.39 blur-off).
- **`aoBias` deliberately untouched.** The mission says re-grade it last. It is
  a *depth-compare* bias; grading it needs a controlled occluder-distance probe,
  not a noise metric. Changing it on intuition would relaunder the same "tuned
  against a pass that never fired" problem. **Still carries no information.**

## Clamp fixes

- ImGui radius slider **32.0 → 64.0**, now agreeing with the setter's 64.0
  (~1.5 map cells; past that AO competes with the shadow cascades).
- **"Reset" restored `aoRadius = 3.0`** — the structurally dead value — so
  pressing Reset silently turned the feature off. Now restores shipping defaults.
- Added blur on/off + sigma sliders.

## Gates

| var | verdict |
|---|---|
| `MC2_SSAO_BLUR` (default **ON** with SSAO) | registered + tier1-documented + smoke-allowlisted — `gate_status` **OK, 0 flags** |
| `MC2_SSAO_BLUR_SIGMA` | registered + documented + allowlisted |
| `MC2_VISUAL_TUNING_FILE` | **ENV_DROP_RISK closed** — documented + allowlisted |

## VERDICT on the default: keep `MC2_SSAO` OFF

The blur was the blocking item and it is fixed. It is *not* what still blocks the
feature. What blocks it now is visible in the same data:

`%<250` (fraction of the AO buffer carrying any darkening) at radius 24:
`oblique_fog` **74.7 %**, `oblique_horizon` **58.9 %**, `cross_map_view`
**58.3 %**, `cliff_side_on` **21.8 %**.

A contact-occlusion term should touch a few percent of a frame. Darkening
three-quarters of a wide view is a **global dimming**, not grounding — and it
appears precisely on the full-frame wide gameplay-altitude poses the standing
capture rule asks the verdict to be made on. Note this is *not* inconsistent with
SSAO-RETUNE-1's 1.54–5.10 % at r=40: they graded building close-ups, I graded the
standard wide mc2_01 set. The pose class is the difference, which is exactly why
the wide poses are the ones that decide a default.

⚠ **I did not isolate the cause.** Grazing-angle self-occlusion (hemisphere taps
diving into a near-tangential surface) is my hypothesis and it fits, but I
asserted an unproven mechanism once already this lane and was refuted. Treat it
as an open question, not a finding.

**What SSAO adds that the prop-lighting lanes cannot** (coordination): a face
turned away from the light already has `ndl = 0`, so neither
`MC2_PROP_SHADOW_RECEIVE` (measured 0.003–0.10 % of pixels) nor a directional
base-light term can darken *enclosure* — inside corners, undersides, the
prop/ground contact line. That is a real, non-overlapping third term and the case
for SSAO existing. The hazard is the mirror image: those lanes **raise** light on
turned-away faces while SSAO **lowers** it in recesses, on the same building
pixels. Tune SSAO strength only against a build carrying the prop-lighting
change, or recesses double-darken.

## Cost — NOT MEASURED, deliberately

Tracy is compiled into **none** of the 29 build trees and disk is ~12 GB, so a
30th tree for perf is not authorised. **I am not guessing a number.** Statically:
this adds **two half-res 9-tap fullscreen draws** on top of an existing half-res
16-tap gen + full-res apply. Total SSAO cost is **full-screen and paid every
frame including prop-free views** — explicitly *not* bounded by the 0.25–0.73 %
prop pixel coverage that bounds `MC2_PROP_SHADOW_RECEIVE`. That shape argues
against default-ON for a contact term, **but it is not a measurement.** Per the
brief's cost rule: a real GPU number requires a Tracy tree — saying so and stopping.

(tier1 FPS 139–141 across 5 missions is **not** a cost measurement: SSAO is
default-OFF there, the window is minimised at a ~100 fps SDL cap, and the harness
is fixed-timestep. Do not cite it.)

## NOT verified

- **The full capture matrix was not run**: mc2_01 only, day only. No mc2_03 /
  mc2_10 / mc2_24, no night, **no mc2_05 night non-degradation proof**. That
  matrix exists to clear a *default flip*; the evidence says do not flip, so
  running it would be premature. **It must be run before any default-ON proposal.**
- No scene-composited A/B (all grading was on the raw AO buffer via
  `MC2_SSAO_DEBUG`). The buffer is the mechanism proof; the scene is the benefit
  proof, and that half is unrun.
- `aoBias` ungraded (above).
- The three white poses (`runway_overview`, `pyramid_islands_topdown`,
  `edge_fog_corner_southwest`) returned AO = 255 everywhere with an **identical
  hash in every arm**. I did not determine whether those poses genuinely have no
  occlusion or failed to frame geometry. Do not treat them as evidence either way.

## Coordination notes

`build64` and `RendererFeatureRegistry.h` are contended by live siblings.
Mid-session a sibling's in-flight registry edit (`LightPosColumnFix = 53`,
`COUNT = 54` with only a *moved* table row) broke the build with
`kFeatureTable length must match RendererFeature::COUNT` — **not this lane's
change**; it cleared when they committed (`31e1a9e8`).

⚠ **Trap worth recording:** that failed build still printed
`deploy COMPLETE … src_commit 3bf826ff`, because `deploy_payload.py` happily
shipped the *previous* binary. `deploy_status` then reported
**manifest "OK" next to a real `FINGERPRINT-MISMATCH`** (exe `7ca604d1` vs
manifest `3bf826ff`). This is exactly the failure the fingerprint bookend exists
for: **the manifest line is not proof; the embedded exe sha is.**

## Next session

1. Grade the **scene-composited** A/B (not just the AO buffer) at radius 24 with
   blur on, wide poses, mc2_01/03/10/24, day + night, + mc2_05 night.
2. Investigate the 58–75 % darkening on wide poses before any further tuning —
   isolate whether it is grazing-angle self-occlusion, and if so whether a
   normal-vs-view-angle falloff or a smaller radius fixes it without losing the
   contact line. **This is now the blocking item, not noise.**
3. Re-grade `aoBias` with a controlled occluder-distance probe.
4. Port the blur to the native VK SSAO path (`MC2_VK_POST_SSAO`).
5. Only then re-judge the default. **"Still not worth it" remains legitimate** —
   but it would now be judged against a pass that genuinely denoises.
