# EDGE-LIGHT-BILLBOARD-CULL-1 — STATUS (master-FAIL #8, MED)

> **SUPERSEDED 2026-07-24 by `.claude/LIGHT-BILLBOARD-GPU-1-STATUS.md`.**
> The diagnosis below is **FALSIFIED**. These props are not flat quads (they are
> 6-vertex / 7-face triangular prisms with a horizontal emissive top cap) and
> they are not being backface-culled. The real mechanism is sub-pixel collapse
> of that horizontal cap at grazing view angles, so the "real fix (deferred)"
> two-sided GPU draw bucket specified at the bottom of this file would **not**
> have worked. Read the superseding doc instead; the sections below are kept
> only for the measurement notes and the commit record.

**Verdict: PARTIAL / FOUNDATION.** Mechanism confirmed and the legacy CPU
render path is fixed + gated, but the change is **near-inert under the default
runtime** because the beacon/runway light props render through the GPU
static-prop batcher (hardware `GL_CULL_FACE GL_BACK`), which this change does
not touch. Item #8 is **not comprehensively resolved** — the complete fix is a
GPU-batcher two-sided draw (specified below) and is deliberately deferred as
too invasive for a narrow, wave-3-merge-safe slice. Do not mark #8 closed.

Lane: `edge-light-billboard-cull-1` (worktree `A:/Games/mc2-main`, branch
`claude/nifty-mendeleev`). Released.

## Commit

- `9f8c8086` (amended message) — `mclib/tgl.{h,cpp}` + `scripts/run_smoke.py` +
  `scripts/check-env-registry.sh` + `docs/tier1_env_vars.md`.

## Gate

- `MC2_LIGHT_BILLBOARD_TWOSIDED` — killswitch, **default ON**. `=0` restores
  legacy backface culling. `gate_status` verdict **OK** (registered in the
  run_smoke passthrough allowlist + check-env-registry ALLOWLIST + documented in
  docs/tier1_env_vars.md). Byte-identical stock when off; effectively
  byte-identical under the default runtime even when on (see efficacy below).

## Diagnosis (what these lights are)

Runway/airbase "edge lights" are TGL props: `data/tgl/runwaylight.tgl` (node
`Redbeaconlight03`) + `red/green/bluebeaconlight.tgl` (nodes `*beaconlight*`),
all sampling the shared `Lightsnflags.tga` atlas. Each is a **single flat
quad** with **no billboarding**. As the RTS camera orbits/tilts, the quad's
face normal flips away from the camera and the light is backface-culled → it
"pops out by view angle" (the #8 complaint). Node-name substring `beaconlight`
cleanly separates them from the flags (`davianflag`/`Liaoflag`) and the
barrels/streetlight/lockers props that also draw the `Lightsnflags` atlas.

## What shipped

- `TG_Shape::isLightBillboard` flag, set once at shape load from the node name
  (`tgNameContainsCI(name,"beaconlight")`), mirroring the existing
  `isSpotlight`/`isWindow` classification (`tgl.cpp` ~376/595).
- Two-sided OR-term added at **both** `TG_Shape::Render` per-triangle
  backface-cull sites (`tgl.cpp` ~2581 / ~3032):
  `(cosine>0) || isSpotlight || (isLightBillboard && MC2_LIGHT_BILLBOARD_TWOSIDED)`
  — identical treatment to the spotlight no-cull exemption.

## Efficacy — VERIFIED, and it is the honest problem

Rig: `tools/visual_verify/capture.py` against a fresh-exe swap into an idle
install (issuefix-test; backed up + restored), night airbase mc2_10, both saved
poses (204753 rot-1.25/proj25 near-top-down; 204907 rot-80/proj12.6 oblique),
`MC2_TERRAIN_LIGHT_POOLS=0` to remove the animated night-pool nondeterminism.

**Critical measurement gotcha (documented for the next agent):** the fixed
`MC2_SMOKE_FIXED_TIMESTEP` clock is **NOT** enough for byte-stable captures at
this scene — the animated night light pools drift ~**6.8%** of pixels
**run-to-run** (two gate-ON launches differ MORE than gate-ON vs gate-OFF). Any
full-frame A/B at this pose with pools ON is dominated by that noise (the two
big green/red pool blobs in the first heatmaps were animation, not the fix).
Pools-OFF drops the noise floor to ~0.01%.

Pools-off A/B (ON vs OFF), above a rigorous ON-vs-ON2 noise floor:

| Pose | noise floor (ON vs ON2) | gate (ON vs OFF) | net |
|---|---|---|---|
| 204753 (near-top-down) | 0.009% | 0.013% | near-null |
| 204907 (oblique, rot-80) | 0.016% | 0.018% | tiny, localized |

At 204907 the gate-only (noise-stable) delta is a **single small beacon glow
wedge + a few building-top beacon dots** that change between ON/OFF — real proof
the mechanism fires for the cards that reach the legacy path, but **minimal**.
At neither static pose does the fix restore a runway line of edge lights.

**Root cause of the near-null:** the beacon props render through the GPU
static-prop batcher (`GameOS/gameos/gos_static_prop_batcher.cpp`), whose flush
relies on the **global `GL_CULL_FACE GL_BACK`** state (see the comment at
batcher line ~5945). My change is in the legacy `TG_Shape::Render` CPU
backface loop, which is not the path these props take at runtime. Spotlights
sidestep culling because they are (a) skipped by the batcher (batcher ~5161
`if GetIsSpotlight() continue`) and (b) exempt in the CPU loop; light billboards
get neither the batcher skip nor a batcher-side two-sided draw.

## The real fix (deferred — do this user-present)

Two-sided the light-billboard prop draws **in the GPU batcher**. Options, in
increasing narrowness:

1. **Two-sided draw bucket (preferred, correct).** Propagate an
   `isLightBillboard` bit into the per-type dispatch meta (the flags word is
   already built at `gos_static_prop_batcher.cpp` ~5166-5175, carrying
   spotlight/window/alphatest bits) and, in the per-packet draw loop
   (~7062-7137), bracket light-billboard-type draws with
   `glDisable(GL_CULL_FACE)` / restore. Invasive: the loop is the hot
   snapshot/dual-path/BC7-bucketed path; high merge risk (batcher is adjacent
   to files 8C touches). Needs its own tier1 + A/B.
2. **Batcher skip → legacy fallback.** Add `if (child->isLightBillboard)
   continue;` alongside the spotlight skip (~5161) so these cards fall back to
   the legacy path where THIS change already two-sides them. Simpler but risks
   double-draw / disappearance depending on the `BldgAppearance::render`
   fallback flow — verify carefully.

**Also note:** two-sided is necessary but **not sufficient**. At true top-down
the fixed-orientation card goes edge-on (zero projected area) regardless of
cull. The complete cure is to render these light cards as **camera-facing
billboards** (the master-FAIL literally calls them "billboards"). That is a
larger arc — reuse the isLightBillboard classification landed here.

## Merge / risk

- Change is tiny and localized (classification + one OR-term at two sites +
  three allowlist/doc lines); default-safe; ~8 clean headless game launches on
  the new exe (no crash/regression).
- **Cross-worktree overlap:** `mclib/tgl.cpp` is also claimed by
  `DEFERRED-BATCH-8C` (worktree `mc2-deferred-1`). My edits are at shape-setup
  (376/595) and the backface-cull sites (2581/3032) — away from shoreline/
  runway-edge work — so textual merge risk is low, but reconcile at wave-3.
- tier1 not run here (shared build64 + deploy lanes contended by sibling
  mc2-main lanes; disk A: tight). Recommended before merge; the change is inert
  under default runtime so risk is minimal.

## Files / evidence

- Code: `mclib/tgl.cpp`, `mclib/tgl.h`.
- Gate plumbing: `scripts/run_smoke.py`, `scripts/check-env-registry.sh`,
  `docs/tier1_env_vars.md` (`### MC2_LIGHT_BILLBOARD_TWOSIDED`).
- Captures (scratchpad): `vv_edgelight_ON/ON2/OFF` (pools-on, noise demo),
  `vv_po_ON/ON2/OFF` (204753 pools-off), `vv_b_ON/ON2/OFF` (204907 pools-off),
  `b_gateonly_full.png` (the localized beacon-wedge delta).
