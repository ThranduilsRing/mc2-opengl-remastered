# LETTERBOX-TOP-RESTORE-1 — status

**Verdict:** SHIPPED. Fix commit `25aa6180` on `claude/nifty-mendeleev`
(worktree `A:/Games/mc2-main`). tier1 5/5 PASS.

## Report

User: on the mc2_10 night intro the TOP cinematic letterbox bar is transparent
(scene / fog-void shows through it) while the BOTTOM bar is correct solid black.
Both bars are intentional and must be opaque black.

## Root cause — NOT a regression, and not the top bar specifically

Stock `code/gamecam.cpp` ramps **both** the bar EXTENT (`letterBoxPos`) **and**
the bar ALPHA (`letterBoxAlpha = percent * 255`) over
`Camera::MaxLetterBoxTime = 5.0f`. For the whole 5-second swoop-in **both bars
are translucent by exactly the same amount**.

Measured, mc2_10 night, free-capture frame 100 (3840x2160):

| | value |
|---|---|
| top bar extent | 214 px (0.0990 of height) |
| bottom bar extent | 215 px (0.0995 of height) |
| ramp progress | 0.0986 / 0.15 max = **65.7 %** |
| implied `letterBoxAlpha` | **168 / 255 → bars 34 % transparent** |
| top-bar luminance min/avg/max | 3.0 / 8.1 / 16.0 |
| bottom-bar luminance min/avg/max | 4.0 / 6.5 / 13.0 |

The extents are symmetric and the alphas are equal. It **reads** as "top broken,
bottom fine" purely because of what sits BEHIND each bar: the top bar covers the
bright horizon/sky, the bottom bar covers dark near-field terrain. Identical
translucency is glaring on top and invisible on the bottom.

At capture frame 400 (ramp complete) both bars are pure `(0,0,0)` at the full
0.15 extent — the engine was doing exactly what stock told it to.

## Hypotheses ruled out — by evidence, not by reasoning

| Hypothesis | Disproof |
|---|---|
| Edge-fog / OOB-fog painting over the bar (`16c2244a` MC2_NIGHT_FOG_DIM, `98ed1d32` far-backdrop) | `MC2_OOB_FOG=0 MC2_EDGE_FOG=0` A/B at the same frame is **byte-identical** (`ImageChops.difference` bbox = `None`); logs confirm `[OOB_FOG v1] enabled=0`. Fog never touches the bar region at this pose. |
| A commit from this session | v0.5.1 (pre-session, 2026-07-23) vs tip render the **same** bar luminance stats at the same frame (top 3.0/8.1/16.0, bottom 4.0/6.5/13.0). No delta. |
| deferred-1 "floored the letterbox black bars" WIP leaking via the WAVE-3 merge (`b13a7cab`) | The WIP is **uncommitted** in `A:/Games/mc2-deferred-1` (`applyBackdropNightFloor` untextured-gate widening); nothing in `110281e7..f96f3a8c` touches letterbox; and it rode `73755184`, which nifty already reverted (`6e7bfda9`). Its stated effect was bars a hair *lighter*, not transparent. |
| Quad winding / backface cull killing one bar (the two letterbox quads genuinely have opposite winding) | Bar extents are symmetric to within 1 px — nothing is being culled. |
| HUD bottom-band shrink (`s_hud_scale` 0.85) | The letterbox is drawn in the scene pass (`gos_State_IsHUD` is not set), so it never enters `flushHUDBatch`. |

## Fix

`code/gamecam.cpp` — gate **`MC2_LETTERBOX_OPAQUE`** (killswitch, **default ON**):
the letterbox quad `argb` alpha is forced to `0xff` so the bars are opaque black
from the first frame, while the EXTENT wipe still animates (that is the part
carrying the cinematic read). `MC2_LETTERBOX_OPAQUE=0` restores the stock alpha
ramp verbatim.

The separate `fadeAlpha` fade-to-black quad below it is a real fade and is
untouched.

Registered in `scripts/check-env-registry.sh` (PASS, 973 vars accounted for).
`docs/tier1_env_vars.md` intentionally not touched — advisory-only checker, and
the file is claimed by two other live lanes (same treatment `MC2_OOB_LETTERBOX`
already gets).

## Verification

Same mission / seed / fixed-timestep / capture frame, before vs after:

| | top bar (min/avg/max lum) | bottom bar |
|---|---|---|
| BEFORE (`cap_f100`) | 3.0 / 8.1 / 16.0 | 4.0 / 6.5 / 13.0 |
| AFTER (`cap_fix`) | **0.0 / 0.0 / 0.0** | **0.0 / 0.0 / 0.0** |

Crops (both bars, before and after) in
`.claude/captures/letterbox-top-restore-1/`:
`before_TOPBAR_crop.png`, `before_BOTBAR_crop.png`,
`after_TOPBAR_crop.png`, `after_BOTBAR_crop.png`, plus `*_full.png`.

Repro/capture recipe: launch with `MC2_SMOKE_MODE=1 MC2_SMOKE_SEED=0xC0FFEE
MC2_SMOKE_FIXED_TIMESTEP=1 MC2_VISUAL_CAPTURE_FRAME=100
MC2_VISUAL_CAPTURE_DIR=<dir> MC2_VISUAL_CAPTURE_EXIT_AFTER=1`,
`--profile stock --mission mc2_10`. Frame 100 lands mid-ramp; frame 400 is
post-ramp (bars already black even before the fix).

- tier1 (5/5 PASS) against `A:/Games/mc2-opengl/mc2-win64-v0.4`,
  exe md5 `caf8f817000ef697cc338e7952886b6b`, manifest `src_commit
  25aa618027bb-dirty`, verified equal to `build64/RelWithDebInfo/mc2.exe`.
- `[DEPLOY_FINGERPRINT] MISMATCH` in the smoke output is expected and benign:
  HEAD moved to `5213d257` (a parallel lane committed) after my build, and the
  shared worktree carried three other live lanes' uncommitted files
  (`mclib/cone_glow.cpp`, `mclib/genactor.cpp`,
  `shaders/gos_tex_vertex_lighted.vert`). The exe's embedded sha is `25aa6180`
  = this fix's commit, so attribution is intact.

## Follow-up (not done here)

`gos_postprocess.cpp` OOB/edge fog *can* in principle blend over the letterbox at
poses where out-of-bounds sky falls inside a bar (the bars are drawn in the scene
pass with `ZWrite 0`, so the fog's far-depth test still passes there). It does not
happen at the pose captured (proved byte-identical above), and it is now cosmetically
moot for the bars themselves, but it remains a latent ordering hazard worth a
dedicated look if bar tinting is ever reported at a different camera pitch.
