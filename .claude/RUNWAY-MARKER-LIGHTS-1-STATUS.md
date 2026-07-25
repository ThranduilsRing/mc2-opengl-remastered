# RUNWAY-MARKER-LIGHTS-1 — STATUS

**Verdict: RESOLVED (agent-verified at 4K, user click-through pending).**

User verdict that opened this lane, verbatim: *"whatever the hell it is doing
with the runway marker lights is atrociously hideous, it needs to stop."*

Lane: `RUNWAY-MARKER-LIGHTS-1` (worktree `A:/Games/mc2-main`, branch
`claude/nifty-mendeleev`, deploy lane `0.4`).

## TL;DR

A gate that landed earlier the same session — `MC2_LIGHT_BILLBOARD_FACING`,
**default ON** as of `f412423e` (lane LIGHT-BILLBOARD-GPU-1) — was the cause.
Its minimum-screen-size clamp was **uncapped**, so every runway/taxiway light
fixture held a constant, growing, hard-edged bright block at *every* camera
distance. I capped the clamp and flipped the gate **default-OFF**. Reproduced
the ugliness and the cure at 4K, the user's actual resolution.

## Root cause

`shaders/static_prop.vert`, the light-billboard block:

```glsl
if (sil > 1e-9) k = max(1.0, kLbMinNdc / sil);   // NO upper bound
```

`sil` is the fixture's projected silhouette and falls off as 1/distance, so `k`
grows **without limit**. The clamp's literal intent was to hold a constant
on-screen size, and that is correct at playable RTS range — it is what made the
lights visible at grazing angles, the legitimate win of that lane. Past that
range it means several hundred regularly-spaced runway and taxiway fixtures
**never shrink**. At map-overview altitude, where a marker light should be
sub-pixel or gone, they are all still full-size, full-brightness, hard-edged
geometry. Regular spacing + constant screen size = the dashed slashes the user
saw. Nothing was "streaking" in the motion-blur sense; these are rows of
oversized static blocks reading as dashes.

**Secondary error, ~1.5x compounding.** `kLbRefRadius` was set to `0.8`, the
authored top-cap radius. But the rigid billboard maps local **Z** (the
2.85-unit post axis) onto `toCam`, so the post foreshortens and what the viewer
actually sees is the local **XY cross-section** — measured from
`runwaylight.ase`, extents `2.015 x 2.326`, silhouette radius ~`1.2`. The clamp
was sizing against a feature that is never on screen, over-inflating everything
by a further ~1.5x.

## What I turned off vs. what I fixed

| | Action | Rationale |
|---|---|---|
| `MC2_LIGHT_BILLBOARD_FACING` | default **ON -> OFF** | The user's standing rule: new visual treatments ship default-OFF byte-identical. Unset or `=0` emits no instance flag bit, the shader block never runs, frame is byte-identical stock. |
| clamp upper bound | **fixed** — `clamp(kLbMinNdc/sil, 1.0, kLbMaxScale)`, `kLbMaxScale = 2.5` | Keeps the legitimate win (fixtures survive rasterisation at playable range) and restores normal 1/distance shrink beyond it. |
| `kLbRefRadius` | **fixed** — `0.8 -> 1.2` | Size against the cross-section the viewer actually sees. |
| `MC2_LIGHT_BILLBOARD_TWOSIDED` | **left alone** | Already known near-inert (predecessor lane measured it); not implicated. |

Nothing in LIGHT-REALISM-1 (cone glow / beacon emission, `MC2_LIGHT_REALISM`,
default OFF) was implicated — it was already default-OFF and is not in the
capture path.

## Gates + recommended defaults

- **`MC2_LIGHT_BILLBOARD_FACING`** — unset or `=0`: **DEFAULT, OFF**, stock,
  byte-identical. **This is the clean way to disable the modern marker-light
  treatment entirely.** `=1`: opt-in capped treatment. `=2`:
  verify-by-exaggeration. `gate_status` verdict **OK** (smoke-allowlisted,
  registry-allowlisted, documented in `docs/tier1_env_vars.md`).
- **Recommended ship default: OFF**, as landed. If the user likes the capped
  look in click-through, `=1` is the value to promote — but only after they see
  the overview pose.

## Evidence

Rig: `tools/visual_verify/capture.py`, install
`A:/Games/mc2-opengl/mc2-win64-abl-validate` (exe md5 `9ff0b9a0…` == my build),
mc2_10 night, fixed clock, `MC2_TERRAIN_LIGHT_POOLS=0` per the documented
pools-drift discipline (~6.8% run-to-run otherwise). **Captured at 3840x2160**,
the user's actual resolution — see the measurement note below.

Three configs: `BEFORE` = pre-fix shader + gate ON (what the user saw);
`AFTER_optin` = capped shader + gate ON; `AFTER_default` = gate unset (ship
state, stock).

BEFORE vs AFTER_optin, changed pixels (`|d| > 8`), and BEFORE vs
AFTER_default (ship state):

| Pose | alt | optin changed | % of changed that DARKEN | max delta | default changed | verdict |
|---|---|---|---|---|---|---|
| `205952` map-overview, whole valley | 6045 | 0.0066% (549 px) in a **136 x 13 px horizontal band** | **95%** | 240 | 0.0066% (550 px) | the streak itself, removed |
| `204753` airbase near-top-down | 5433 | 0.0219% (1817 px) | 59% | 238 | 0.0222% (1843 px) | oversized markers reduced |
| `204907` airbase oblique | 5620 | 0.1087% (9019 px) | 44% | 237 | 0.1089% (9030 px) | largest effect; apron + taxiway rows reduced |
| `204937` close ground-level | 1257 | **0.0000% (0 px)** | — | **0** | 0.0450% (3736 px) | **opt-in byte-identical — cap correctly inactive close up** |

Max deltas of ~240/255 confirm these are near-full-range bright pixels being
removed, far above the pools-off noise floor (~0.01% at low amplitude).

**Honest reading of the "darken" column.** At the decisive overview pose 95% of
changed pixels darken, which is the clean signal — over-bright geometry removed.
It is *not* 100% anywhere, and at the oblique pose it is only 44%. That is
expected and is not a red flag: shrinking a *coloured* fixture both removes its
bright pixels and reveals whatever terrain was behind it, and the fixture's own
red/green/blue channels shift as its footprint changes, so a shrink necessarily
produces some brighter pixels too. I originally recorded these as ~100% dimming
from the 800x600 pass; that figure was an artifact of computing the ratio
against total pixels rather than changed pixels, and is corrected here. The
verdict rests on the crops and the overview band, not on this ratio.

The `204937` row is the cleanest single proof of the cap's design: with the
gate opted in, the close pose is **byte-identical** to the pre-fix build (`n=0`,
max delta `0`) — the clamp is provably inactive at close range, so nothing about
close-up gameplay changed. The same pose differs by 3736 px against the
default-OFF build simply because default-OFF removes the billboard treatment
entirely.

**The decisive artifact** is the `205952` overview crop. BEFORE: a row of
hard-edged white/red/green blocks strung across a dark hillside, floating
without context — the user's description, reproduced. AFTER: a faint line of
small pale specks that read as distant structures. At `204907` the oversized
cyan apron markers and orange taxiway cones shrink to small glows while
remaining readable, so the lane's original goal survives.

Crops (1:1, brightened 2.2x for night legibility) in the review pack:
`release_assets/0.5.2/REVIEW/runway-marker-lights/`.

## Measurement note — READ THIS BEFORE RE-VERIFYING

**Capture at the user's resolution.** My first pass ran at the capture install's
default 800x600 and was nearly useless: the overview delta was 44 pixels and
looked like noise. At 3840x2160 the same delta is a coherent 136x13 band and the
crop is unambiguous. `kLbMinNdc` is an **NDC** quantity, so the artifact's
severity scales with vertical resolution — an 800x600 A/B systematically
under-reports this entire class of bug. The user runs 3840x2160
(`mc2-win64-v0.4/options.cfg`).

**Build provenance caveat — read before quoting the absolute look.** This
worktree is shared and was carrying ~184 dirty files from parallel lanes when I
built, including four uncommitted engine/shader files (`mclib/cone_glow.cpp`,
`mclib/genactor.cpp`, `mclib/txmmgr.cpp`,
`shaders/gos_tex_vertex_lighted.vert`). My own changes were committed before the
build, but those other lanes' WIP is compiled into the exe I captured with. This
does **not** weaken the A/B: all three configs used the **same exe** and varied
only `static_prop.vert` and the env gate, so the comparison is internally valid
and the attribution to the clamp is sound. It does mean the *absolute* night
appearance in these crops is not a pure `b3d53b21` frame, and the crops should
not be used as a golden or quoted as "what v0.5.2 looks like".

**Why the regression shipped.** The predecessor lane verified at five camera
angles, all at airbase range, and none at map-overview altitude — the only
altitude where an uncapped constant-screen-size clamp misbehaves. Any future
change to this block must include an overview pose.

## Smoke / gates

- `check-env-registry.sh` — **PASS** (975 MC2_* vars accounted for).
- `gate_status MC2_LIGHT_BILLBOARD_FACING` — **OK**, no flags
  (smoke-allowlisted + registry-allowlisted + documented).
- `run_smoke.py --tier tier1 --duration 30` — **5/5 PASS**
  (mc2_01/03/10/17/24, Δdestroys +0 on all).
  **But read this caveat:** the runner raised
  `[DEPLOY_FINGERPRINT] HARD FAIL — smoke ran an exe whose fingerprint does not
  match this worktree HEAD`. tier1 executed the **stale 0.4 deploy exe
  (`16c2244a`), not my build**, so *that 5/5 does not validate this change*. I
  did not deploy to `0.4` to fix it: `0.4` is the user-facing install, its
  manifest already records a dirty-tree build, and pushing a
  184-dirty-file shared-worktree build into it would make attribution worse, not
  better. Deploying to `0.4` is left as a user call.
- **What *did* exercise my build:** 12 full launches of my exe
  (md5 `9ff0b9a0…`) during capture — 3 configs x 4 poses, each a complete mc2_10
  mission load plus a 45s run, all reaching the deterministic trigger frame
  (`det=True` 12/12). No crash, no shader-compile failure (the modified
  `static_prop.vert` is compiled at runtime, so a bad edit would have failed
  loudly there), no load regression. That is real runtime validation of this
  exe, and for a change that is one shader constant plus an env default it is
  arguably a stronger test than tier1 — but it is **not** the tier1 gate, and I
  am not claiming it as one.

## Commits

- `05dbd1f3` — carries the code fix (shader clamp cap + refRadius, gate default
  flip, docs, env registry). **Attribution caveat:** this commit's message is
  LIGHT-BILLBOARD-GPU-1's docs message. That lane committed while my five files
  were staged in this shared worktree and scooped them. Content is intact and
  correct; not amended (shared-worktree rule). This document is the authoritative
  record of what those five files' changes are and why.
- Follow-up docs commit (this file + the supersession note appended to
  `.claude/LIGHT-BILLBOARD-GPU-1-STATUS.md`).

## Coordination

- Claimed `shaders/static_prop.vert`, `gos_static_prop_batcher.{cpp,h}`,
  `docs/tier1_env_vars.md`, `scripts/check-env-registry.sh` in the lane registry.
- **LIGHT-BILLBOARD-GPU-1** (contended, owns the same block): not reachable by
  SendMessage — separate session. Coordinated via the two durable channels: the
  lane-registry note, and a "SUPERSEDED IN PART" section appended to
  `.claude/LIGHT-BILLBOARD-GPU-1-STATUS.md` telling that lane not to flip the
  default back to ON without a user click-through on the overview pose, and to
  argue the cap value rather than the cap's existence.
- **LIGHT-REALISM-1** — no overlap; its beacon glow is default-OFF and
  complementary (glow *around* the fixture vs. the fixture's own size).

## Open / user-gated

1. **Click-through on the overview pose** — the whole point. Not agent-closable.
2. Decide whether `=1` (capped opt-in) should ever become the default. My
   recommendation: leave OFF unless the user actively likes it.
3. The capture install `mc2-win64-abl-validate` was borrowed and **restored**
   (exe, shader, `options.cfg` resolution) — see the restore step in this lane's
   log.
