# SSAO-BLUR-1 — STATUS: blur IMPLEMENTED, NOT BUILT, NOT MEASURED. Default stays OFF.

Lane: SSAO-BLUR-1 | worktree A:/Games/mc2-main | branch claude/nifty-mendeleev
HEAD at start/end: `6440c84c` (unchanged — see "Build blocked")
Date: 2026-07-28
Predecessor: `.claude/SSAO-RETUNE-1-STATUS.md` (diagnosis; two defects)

## Bottom line, stated plainly

The missing denoise is **written, registered, documented, and the new shader is
glslang-validated**. It is **not compiled, not deployed, not run, and not graded.**
No screenshot, no AO-buffer before/after, no radius verdict from pixels, no cost
number. **The build was blocked by a live sibling lane holding the tree**, and I
chose to stop rather than produce an unattributable exe.

So: **`MC2_SSAO` must stay default-OFF**, and this lane does *not* discharge the
"re-judge the feature" half of its mission. What it delivers is the blocking
prerequisite (defect #2) plus the clamp/registration cleanup, in a state the next
session can build and grade directly.

## META REVIEW — brief premises re-checked against source (not status docs)

| Premise | Verdict | Evidence |
|---|---|---|
| `runSSAO` has exactly two draws, no blur pass | **TRUE** | read `gos_postprocess.cpp:2777-2856`; two `glDrawArrays`, nothing between |
| Only denoise is a 4-tap ±0.5-texel box in `ssao_apply.frag` | **TRUE** | read in full; comment does claim it "upsamples + denoises" |
| `aoRadius_` ships 3.0 | **TRUE** | ctor init list |
| ImGui slider caps at 32.0 | **TRUE**, and **worse than described** | `GuiRuntime/GraphicsOptionsWindow.cpp:2041`. The brief implies the 64.0 setter clamp bounds the slider. It does not: the slider binds `&pp->aoRadius_` **directly** and never calls `gos_SetSsaoRadius`, so the setter's 64.0 was never in the slider's path — **32.0 was the only real cap.** |
| Brief path `code/GraphicsOptionsWindow.cpp` | **WRONG PATH** | actual `GuiRuntime/GraphicsOptionsWindow.cpp` |
| Brief path `code/visual_tuning_profile.cpp` | **WRONG PATH** | actual `GameOS/gameos/visual_tuning_profile.cpp` |
| `MC2_VISUAL_TUNING_FILE` unregistered + undocumented | **TRUE, and one worse** | `gate_status`: also **NOT in the `run_smoke.py` passthrough allowlist** (ENV_DROP_RISK) — a smoke-driven tuning sweep silently graded built-in defaults |
| "the useful band is ~12–48" | **partly restated** | 40 was measured to invent occlusion on flat faces, so I treat the *target* band as 12–24 and use 64 only as a slider ceiling |

### One new finding the brief did not contain

`MC2_VK_POST_SSAO` (rung 8.2, default-OFF opt-in) **reuses the `ssao`/`ssao_apply`
SPIR-V verbatim** and takes highest precedence at the SSAO site. It therefore does
**not** get this blur — with that gate ON the speckle returns. Documented in the
registry entry and tier1 docs as open follow-up. Not fixed here.

## What changed (all default-safe: `MC2_SSAO` is OFF, so no default launch moves)

1. **`shaders/ssao_blur.frag` (new)** — separable depth-aware (joint bilateral)
   blur. 9-tap Gaussian shape, run H then V → 9×9 effective support for 18 taps,
   at half res. Each tap weighted by **relative** depth difference `|d-dc|/dc`,
   not absolute: window depth under reverse-Z is strongly non-linear, so an
   absolute epsilon is far too permissive up close and far too strict in the
   distance. Sky/cleared taps (`depth<=0.0001`, where `ssao.frag` wrote AO=1) are
   excluded and the centre passes through untouched, so the background stays
   intact and sky depth cannot poison the weight. Off-screen taps dropped and
   weights renormalised rather than clamp-sampling the edge texel.
2. **`gos_postprocess.{h,cpp}`** — one extra half-res R16F ping-pong target
   (`ssaoBlurTex_`/`ssaoBlurFBO_`) + program + create/destroy. Blur runs H into
   the scratch target then V **back into `ssaoColorTex_`**, so `SsaoOcclusion`
   and the **unchanged** `ssao_apply` pass always read the final AO — no
   downstream reader learns the blur exists. Scratch target deliberately *not*
   published as a RenderResource (it would be a second, wrong handle for "the AO
   buffer"). Gate `MC2_SSAO_BLUR`, **default ON with SSAO**, `=0` reproduces the
   old two-draw chain for A/B on one exe.
3. **`aoRadius_` default 3.0 → 24.0**, and `gos_GetSsaoRadius()`'s null fallback
   with it. Grounds: SSAO-RETUNE-1's measured 12–24 band, r=40 rejected for
   inventing occlusion on flat faces, and independent convergence with
   `.claude/vk-post-ssao/visual_tuning_aoboost.json`. **I did not verify 24 with
   pixels** — see "Not verified".
4. **Clamp fix** — slider 32.0 → 64.0, now agreeing with the setter's 64.0
   (~1.5 map cells; past that AO stops being a contact term and competes with the
   shadow cascades). Comment records that the slider bypasses the setter.
5. **"Reset" button fixed** — it restored `aoRadius = 3.0`, i.e. the structurally
   dead value, so pressing Reset silently turned the feature off. Now restores the
   shipping defaults incl. the new blur controls.
6. **Registration/docs** — `MC2_SSAO_BLUR` added to `RendererFeatureRegistry.h`,
   `docs/tier1_env_vars.md`, and the `run_smoke.py` passthrough allowlist
   (`gate_status` → **verdict OK, no flags**). `MC2_VISUAL_TUNING_FILE`
   documented and allowlisted, closing its ENV_DROP_RISK.
7. **ImGui** — blur on/off + depth-sigma sliders so the next session can grade
   without a rebuild.

## Verification actually performed — and its limits

- **`glslangValidator -S frag` PASS** on `ssao_blur.frag`, run identically against
  `ssao.frag` and `ssao_apply.frag` as known-good controls (all three PASS). This
  proves the new shader **compiles**. It proves nothing about whether it looks right.
- **`gate_status MC2_SSAO_BLUR` → verdict OK, 0 flags**, 2 readers.
- Manual diff review for the classic hazards: no feedback loop (no texture is both
  sampler-bound and framebuffer-attached at any *draw*), ctor init-list order
  matches declaration order, teardown mirrors creation, access sections correct.

**The C++ is UNCOMPILED.** Static review is not a compile. Treat every C++ claim
above as unproven until a build runs.

## Build blocked — why, specifically

`preflight` → `safe_to_touch=false`. Unlike SSAO-RETUNE-1 (where this was only
untracked scratch), this time **two tracked key-source files are dirty**:
`GameOS/gameos/gos_terrain_indirect.cpp` (+277) and
`RenderCore/RendererFeatureRegistry.h` (+16) — 293 uncommitted lines belonging to
**CEMENT-ATLAS-CELL-2**, which is **live** (lane `last_touch` 02:37Z, *after* my
02:35Z conflict check; `RendererFeatureRegistry.h` mtime ~60 s before I looked)
and holds deploy lane `0.4d-rc1`. `build64/RelWithDebInfo` has output from the
last 30 minutes — that agent is mid-build in the shared tree.

Consequences, in order:
1. `build_fingerprint.h` computes dirty **tracked-only across the whole tree**, so
   **no build I make can be attributable** while their files sit dirty — the
   documented 2026-07-12 P0. A `DEPLOY_FINGERPRINT OK` is unobtainable, and the
   standing bookend forbids reporting a tier1 PASS without one.
2. Building `build64` now compiles their half-finished terrain code into my exe.
3. Deploying would contend their `0.4d-rc1` lane.
4. A 30th build tree purely to escape this is **explicitly not authorised** (disk).

I stopped rather than take any of those. **This is the "wait for user direction"
branch of the `safe_to_touch=false` rule, and this time it genuinely applies.**

## Commit shape — deliberately partial, and why

`check_conflict` shows CEMENT-ATLAS-CELL-2 **also claims all three shared
registration files** I had to touch: `RendererFeatureRegistry.h`,
`scripts/run_smoke.py`, `docs/tier1_env_vars.md`.

- **Committed** (100 % mine, uncontended): `shaders/ssao_blur.frag`,
  `GameOS/gameos/gos_postprocess.{h,cpp}`,
  `GuiRuntime/GraphicsOptionsWindow.cpp`, this status doc.
- **Left UNCOMMITTED**: the three shared files. `git add`-ing them would sweep the
  sibling's in-progress work into my commit — the named commit-scoop failure.

⚠ **Therefore the commit is intentionally incoherent**: it contains code reading
`MC2_SSAO_BLUR` while that gate's registration sits uncommitted. `check-env-registry`
may flag it until the companion edits land. Nothing is pushed. Whoever lands
CEMENT-ATLAS-CELL-2 must carry the three files' SSAO-BLUR-1 hunks with them, or
this lane must re-commit them once that lane clears. My additions in all three are
additive and in separate regions; I verified they survived the sibling's concurrent
writes.

## NOT verified — the honest list

Everything in the mission's measurement half:

- **No build, no deploy, no tier1, no `DEPLOY_FINGERPRINT`.**
- **No AO-buffer before/after** (`MC2_SSAO_DEBUG`) — the mechanism proof for the
  blur does not exist. The blur is *reasoned* to fix the speckle; it is not *shown* to.
- **radius 24 is not pixel-verified by me.** It inherits SSAO-RETUNE-1's band and
  the vk-post-ssao prior art. It may still be wrong once the blur changes the
  noise-vs-radius tradeoff — the blur should *permit a larger* radius than the
  un-blurred pass could tolerate, so 24 may prove conservative. **Re-grade it.**
- **`aoBias` (0.0025) deliberately untouched.** The mission says re-grade it last;
  I cannot grade it at all without a build, and changing a value I cannot measure
  would just relaunder the same "tuned against a pass that never fired" problem.
- **No cost number.** Tracy is in none of the 29 trees, a 30th is unauthorised, and
  I will not guess. What is known *statically*: two extra half-res 9-tap draws on
  top of an existing half-res 16-tap gen + full-res apply. This is **full-screen
  cost**, paid every frame including prop-free views — it is *not* bounded by the
  0.25–0.73 % prop pixel coverage that bounds `MC2_PROP_SHADOW_RECEIVE`. That shape
  is an argument against default-ON, but **it is not a measurement.** Per the
  brief's cost rule: a real GPU number requires a Tracy tree, so I am saying so and
  stopping.
- **No capture matrix** (mc2_01/03/10/24, ≥3 locations, day+night, mc2_05 night
  non-degradation). Nothing to capture without a build.

## Coordination — what SSAO adds that the prop-lighting lanes cannot

`MC2_PROP_SHADOW_RECEIVE` moves 0.003–0.10 % of pixels with no visible building
self-shadowing, because a face turned away from the light already has `ndl = 0` —
shadowing cannot darken what is already unlit. PROP-AMBIENT-POLARITY-1 /
PROP-BASE-LIGHT-DIR-1 attack that by changing the *base/ambient* term.

SSAO is a **third, non-overlapping** term: it darkens by **local geometric
enclosure** — inside corners, undersides, the contact line where a prop meets the
ground, gaps between crates — which is exactly where a directional light term and
a shadow term are both blind. That is the case for it existing at all.

The flip side, and the coordination hazard: those lanes are **raising** the light
on turned-away faces while SSAO **lowers** it in recesses, and they land on the
same building pixels. Tune SSAO strength only against a build that already carries
the prop-lighting change, or prop recesses will double-darken. Do not grade this
against a pre-PROP-BASE-LIGHT-DIR-1 exe.

## Next session — exact resume

1. Wait for CEMENT-ATLAS-CELL-2 to commit; confirm `preflight` clean.
2. Re-commit the three shared-file hunks if that lane did not carry them.
3. Build; **confirm the exe carries your sha** (`deploy_status`) before anything.
4. `MC2_SSAO=1 MC2_SSAO_DEBUG=1`, A/B `MC2_SSAO_BLUR=0` vs `1` at fixed radius on
   the mc2_01 `mc2_01_buildings` bookmarks (mc2_01 is the clean ≤0.004 % pose).
   That is the mechanism proof and it is a **one-exe, two-run** comparison.
5. Then sweep radius 12/24/36 with blur ON via `MC2_VISUAL_TUNING_FILE` (no
   rebuild) and re-inspect `sweep_container.png`'s flat end face at 1:1 — the crop
   that overturned SSAO-RETUNE-1. Publish a two-run floor per pose first.
6. Re-grade `aoBias` last.
7. Only then run the full matrix and judge the default. **"Still not worth it"
   remains a legitimate outcome** — but now it would be judged against a pass that
   actually denoises.
