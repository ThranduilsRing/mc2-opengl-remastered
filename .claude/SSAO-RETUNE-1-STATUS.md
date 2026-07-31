# SSAO-RETUNE-1 — STATUS: diagnosis COMPLETE, two defects found, default stays OFF

Lane: SSAO-RETUNE-1 | worktree A:/Games/mc2-main | branch claude/nifty-mendeleev
HEAD at start 5d3e6f4a → moved to 02471415 mid-session (sibling lane committed; shared worktree is live)
Exe graded: `A:/Games/mc2-opengl/mc2-win64-v0.4c/mc2.exe`, embedded sha `6c47e149353c`
Date: 2026-07-26

## Bottom line

`MC2_SSAO` was latent-broken for the reason the brief identified, and the radius fix is
**confirmed working**: the AO buffer went from *literally zero occlusion* to real signal.
But grading the result at 1:1 surfaced a **second, independent defect** — the pass has no
real denoise — and with that in place the pass still does not deliver clean contact
shading at any radius tested. **Recommendation: keep `MC2_SSAO` default-OFF.** It is now a
*fixable* feature with a known remaining task list, not a mystery.

**No code was changed. Nothing was built, committed, or deployed.** The entire investigation
ran on the already-deployed 0.4c exe via the JSON tuning path.

## META REVIEW — brief premises checked against source

| Premise | Verdict | Evidence |
|---|---|---|
| `aoRadius_` defaults to 3.0 | **TRUE** | `gos_postprocess.cpp:758` |
| Radius is in world units | **TRUE** | `shaders/ssao.frag`: `uniform float aoRadius; // world-units sample radius` |
| AO debug buffer is white | **TRUE (re-measured from pixels)** | min 250/251/252, mean 254.99, **0 px < 250** |
| "no env override, so 3.0 is what ships" | **PARTLY FALSE — and this mattered** | No *env* var, correct. But `aoRadius` is a live `visual_tuning.json` key (`visual_tuning_profile.cpp:197`) + ImGui slider. This gave a **zero-rebuild** grading path and is how every number below was obtained. |
| tree clean | **TRUE where it counts** | `git status --porcelain -uno` = **0 tracked dirty**. My first-pass read of `preflight`'s `safe_to_touch=false` as a blocker was **wrong** — it reacts to untracked scratch. `GenerateBuildFingerprint.cmake:37` computes dirty tracked-only. Correction accepted from coordinator and verified independently. |
| Radius is *the* defect | **INCOMPLETE — it is defect #1 of 2** | see below |

## World scale — established empirically

- `mclib/terrain.cpp:116` — `Terrain::worldUnitsPerVertex = 128.0`
- `mclib/terrain.h:54` — `#define MAPCELL_DIM 3` ⇒ **42.67 wu per map cell**
- Hard invariant, not a soft default: `Assert(...==128)` at `code/gameobj.cpp:1714` and `mclib/terrain.cpp:1742`.

## Defect #1 — radius (CONFIRMED, and confirmed fixable)

`ssao.frag` offsets samples by `kKernel[i] * aoRadius`. The baked 16-tap kernel is **not
unit-normalised**: |v| min 0.048 / max 0.564 / **mean 0.289**.

| aoRadius | mean reach | max reach | mean reach ÷ 42.67 wu cell |
|---:|---:|---:|---:|
| **3.0 (ships)** | **0.87 wu** | 1.69 wu | **0.020** |
| 12 | 3.47 wu | 6.77 wu | 0.081 |
| 24 | 6.94 wu | 13.54 wu | 0.163 |
| 40 | 11.56 wu | 22.56 wu | 0.271 |

At the shipping default the mean tap lands 0.87 wu away — 2% of one map cell — which
reprojects to a deeply sub-pixel screen offset, so every tap reads the *same depth texel* as
the centre and no tap can register an occluder. A **second independent kill** sits in the same
shader: `rangeCheck = smoothstep(0.0, 1.0, aoRadius/dist)` drives to 0 for any occluder
farther than 3 wu. Occlusion is therefore **structurally exactly zero**, `ao = 1.0`, pure white.

### Checkpoint 1 — PASSED. The buffer stopped being white.

`MC2_SSAO=1 MC2_SSAO_DEBUG=1`, mc2_01 `mc2_01_buildings` bookmarks, radius via
`MC2_VISUAL_TUNING_FILE` (log confirms `[VisualTuning] applied 14 keys`):

| config | min | mean | px < 250 | px < 200 |
|---|---:|---:|---:|---:|
| r=3 (ships) | 250–252 | 254.99 | **0.0000 %** | 0.0000 % |
| r=40 | **42–81** | 253.41 | **1.54 – 5.10 %** | 0.28 – 1.19 % |

The radius diagnosis is **proven**. The AO buffer at r=40 shows correct *structure* —
occlusion at prop bases, the fence line, gantry cranes, wall edges.

## Defect #2 — no denoise (NEW, found by grading at 1:1)

Scene delta vs SSAO-OFF, `airport_bldgs_mid`, same exe, noise floor 0 (below):

| radius | maxAbs | meanAbs | px>4/255 | px>16/255 |
|---:|---:|---:|---:|---:|
| 12 | 84 | 0.395 | 2.64 % | 0.97 % |
| 24 | 111 | 0.587 | 3.57 % | 1.27 % |
| 40 | 111 | 0.637 | 4.06 % | 1.43 % |

Numerically this looks like a big win over the ~2–4/255 the brief reported at r=3. **It does
not survive looking at it.** At 1:1 on a container prop (`scratch/ssaoretune1/sweep_container.png`,
rows OFF/12/24/40) the added darkening is **speckled dither noise**, concentrated on the
container's *end face and edges* — including flat faces nothing occludes — and it grows with
radius. The desired ground-contact darkening where the prop meets the tarmac is barely touched.

Confirmed at source, not inferred from pixels:

- `runSSAO` (`gos_postprocess.cpp`) issues exactly **two** draws — AO gen into `ssaoFBO_`, then
  apply into `sceneFBO_`. **There is no blur pass.** `grep -i blur` over the SSAO path: 0 hits.
- The only denoise is a **4-tap box spanning ±0.5 half-res texel** inside `ssao_apply.frag`.
  Its own comment claims it "upsamples + denoises". A 1-texel box cannot denoise a 16-sample
  stochastic kernel with per-pixel `hash12` rotation. Every standard SSAO has a depth-aware
  bilateral blur here; this one does not.

⇒ At any radius large enough to produce visible occlusion, the pass also produces visible
noise. That is why raising the radius alone does not make this shippable.

## Cost — NOT MEASURED, deliberately

No perf number was taken. Tracy is compiled into none of the 29 build trees and disk is at
~13 GB, so per the standing constraint a 30th tree was not authorised. **I am not guessing a
number.** What is known statically: the cost is a half-res 16-tap fullscreen gen + a fullscreen
apply, **independent of prop coverage** — it is paid in full on every frame including prop-free
areas. That shape argues against default-ON for a contact term, but it is not a measurement.

## Method / rigour

- **Noise floor = 0.** `--runs 3 --warmup 1` at r=24: all 3 bookmarks byte-stable
  (`29b56b85ad7e` / `d634b2998b13` / `5aae9a7d6a6e`). r=24 also reproduced the identical hash
  in a separate earlier run. Every delta above is real signal, not drift.
- Zero-byte PNG check: 0 of all captures. Evidence in `scratch/ssaoretune1/` (361 MB after trim).

### What I did NOT verify (stated plainly rather than implied)

The full standing capture matrix — mc2_01/03/10/24, ≥3 locations each, day **and night**, plus
the mc2_05 night non-degradation proof — was **not run**. Grading was mc2_01 day, 3 bookmarks.
That matrix exists to clear a *default flip*; since the evidence says do not flip, running it
would be premature. **It must be run before any future default-ON proposal**, and defect #2
must be fixed first or it will simply reproduce noise across 4 maps.

## Recommendation

**Keep `MC2_SSAO` default-OFF.** Not for the old reason ("costs a lot, does nothing") — that
reason was based on a broken pass. The new reason is specific and actionable:

1. **Fix the denoise** — add a depth-aware bilateral blur between gen and apply. This is the
   blocking item; without it the pass trades invisible-and-useless for visible-and-noisy.
2. **Then set radius in the 12–24 band** (3.5–7 wu mean reach — a contact term that stays well
   inside one 42.7 wu cell and does not compete with the shadow cascades). **Not 40** — 40
   measurably increases false occlusion on flat faces. Note prior art
   (`.claude/vk-post-ssao/visual_tuning_aoboost.json`) independently landed on 24.
3. **Move both clamps.** ImGui slider maxes at **32.0** (`GraphicsOptionsWindow.cpp:2041`),
   setter clamps at **64.0** (`gos_postprocess.cpp:631`). Both ranges assume a unit scale ~40×
   smaller than MC2's. Less urgent now the target band is 12–24, but the defaults are still
   miscalibrated and will mislead the next person.
4. **Re-grade `aoBias` (0.0025) last** — it was tuned against a pass that never fired, so its
   current value carries no information.
5. **Add a registered `MC2_SSAO_RADIUS` env tunable.** The JSON path works and is what I used,
   but it is undocumented (`MC2_VISUAL_TUNING_FILE` has a reader at
   `visual_tuning_profile.cpp:301`, is **not registered and not in tier1 docs**). The absence of
   a first-class knob is a real part of why this sat broken.

## Coordination — PROP-BASE-LIGHT-DIR-1 (unchanged, still live)

That sibling lane is adding a directional base light so faces turned away from the sun stop
reading flat-unlit; it committed to this worktree during this session (HEAD → `02471415`). A
working SSAO darkens **inside corners and contacts** — a different term, but landing on the same
building pixels. Once SSAO produces clean signal the two **must be graded together** or prop
recesses will double-darken. Do not tune SSAO strength against a build predating their change.
`shaders/static_prop.vert` is theirs — this lane never touched it.

## Artifacts

- `scratch/ssaoretune1/dbg_r40/` — AO buffer, radius 40 (mechanism proof, "after")
- `scratch/shadowdec1/ssaodbg/` — AO buffer, radius 3 (mechanism proof, "before", white)
- `scratch/ssaoretune1/scene_off|scene_r12|scene_r24|scene_r40/` — scene A/B
- `scratch/ssaoretune1/sweep_container.png` — **the 1:1 crop that overturned the retune**
- `scratch/ssaoretune1/cmp_mid_wide.png` — wide scene + AO buffer strip
