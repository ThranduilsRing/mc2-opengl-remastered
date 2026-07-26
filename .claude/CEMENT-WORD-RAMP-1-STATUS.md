# CEMENT-WORD-RAMP-1 — STATUS

Branch `claude/nifty-mendeleev`. Commits **`bed55a12`** + **`0b26eebe`**.
Gate `MC2_CEMENT_RAMP_WORD`, **default OFF**. `gate_status` verdict **OK**, no flags.

> **BOTTOM LINE — READ THIS FIRST.** The lane shipped a real, measured, default-OFF
> improvement to a real defect. **But the mid-lane user correction was right and my
> inherited brief was wrong about which defect the user is complaining about.** The
> band my brief sent me to fix is *not* the thing the user is pointing at. Evidence
> below. **The user's actual complaint is now located and measured, and it is a
> different mechanism: adjacent CLASSIFIED concrete tiles sample DIFFERENT cement
> atlas cells whose tones differ by up to 90 luma in a single frame.**
> The next lane should work that, not this.

---

## 1. Reconciliation with the user correction (the required item)

The user said: *"the lighter tiles around the runway are TRANSITION tiles, which are
drawn by the shader."* My brief said the band is unclassified and falls back to the
dark colormap. **These are two different features, and both statements are partly
right — about different pixels.**

Settled empirically with the class-diag (`MC2_TERRAIN_LOD_CHUNK_DIAG=32768`).
**The brief's legend was incomplete**: the diag already distinguishes
**RED = solid cement**, **GREEN = transition**, **BLACK = colormap-only** — the
existence of a GREEN class is the fact several lanes have been missing.

Correlating the diag against the ramp-locator mask on mc2_23, default launch:

| pose | band px | **band class composition** | band L | RED/solid L |
|---|---|---|---|---|
| runway_overview | 37 561 | **BLACK/unclassified 100.00%** | 19.9 | 72.4 |
| beacons_ground | 9 230 | **BLACK/unclassified 100.00%** | 16.7 | 89.9 |
| runway_mid | 33 968 | **BLACK/unclassified 100.00%** | 21.8 | 53.1 |

Two findings, both against the brief:

1. **The band I was sent to fix is 100% BLACK/unclassified** — so my brief's
   albedo-source premise is *correct for that band*. It is **darker** than the pad
   (19.9 vs 72.4).
2. **There are no GREEN/transition pixels at all in these frames.** So the band is
   **not** a transition tile, and the user's *"lighter tiles"* — which they describe as
   **lighter** than the adjacent runway — **cannot be the band I measured**, which is
   darker than everything around it.

**So: different features.** My lane chased a dark unclassified ring; the user is
pointing at something lighter. The user's wording ("transition tiles") does not match
the engine's transition class here, but their *observation* — lighter tiles bordering
darker runway strips — is real and is explained below.

## 2. What the user is actually seeing — located and measured

Ran the cement **layer-index** diag (`MC2_TERRAIN_LOD_CHUNK_DIAG=131072`, hashes each
tile's atlas cell to a colour) and correlated cell identity against beauty luma
**within the RED/solid class only** — i.e. among tiles that are all correctly
classified concrete:

**mc2_23 `runway_overview`, 275 998 solid-cement px, top cells by area:**

| atlas cell (hash) | share of solid concrete | **beauty luma** |
|---|---|---|
| (25,25,25) | 37.1% | **79.4** |
| (224,29,246) | 13.3% | **60.9** |
| (75,133,45) | 8.0% | **49.1** |
| (134,94,149) | 6.2% | **122.3** |
| (250,213,213) | 4.6% | **49.1** |
| (242,163,42) | 2.5% | **134.8** |

`runway_mid` shows the same pattern across 8 cells spanning **43.0 → 61.4** luma.

**That is the user's complaint, quantitatively: within a single frame, tiles that are
all correctly-classified concrete draw from at least 8 different atlas cells whose
tones range 43 → 135 luma.** Concrete "magically becomes a different color" tile to
tile because **the tiles are literally sampling different textures**. Which cell is
lighter or darker than its neighbour is arbitrary, so it reads as light bands beside
dark runway strips.

This is exactly what the user's ruling prescribes — *"the transition tiles should just
use the same concrete texture"* — generalised by the measurement to: **tiles bordering
a pad should sample the cell the pad uses.** The existing `CEMENT-LAYER-DIAG-1` bit was
built for precisely this question and had never been pointed at the complaint.

**I did NOT implement that fix.** The ruling arrived with the lane's budget nearly
spent, and it requires the RenderDoc attribution below first.

## 3. RenderDoc attribution — NOT DONE, and I accept the criticism

The user's process criticism is correct and I did not discharge it. `tools/renderdoc/`
(`capture.py`, `attribute.py`, `scene_attrib.py`, `rd_worker.py`, `states.py`) exists
and was **not** used by this lane. Everything above is still pixel-and-diag evidence.
The diag is stronger than raw pixels — it reports *which shader branch* coloured a
fragment and *which atlas cell index* it used — but it is still the shader
self-reporting, **not** a draw/program/texture-binding attribution.

**The unanswered question that only RenderDoc closes:** are those differing atlas
cells different *cells of one atlas texture*, or different *textures/bindings*? §2
proves the cell **index** differs; it does not prove what is bound. The next lane must
capture at the user's pose, take one pixel in a lighter band and one in the adjacent
runway, and report **eid, program (by SPIR-V disasm), and texture binding + cell** for
each — pixel history against the **pre-composite HDR scene FBO**, not the backbuffer.

## 4. What shipped (honest scope)

`MC2_CEMENT_RAMP_WORD=0|1`, **default OFF**. Gives the BLACK/unclassified ramp band an
albedo source: on the bare-terrain tile it borrows the nearest orthogonal cement
neighbour's atlas cell into `base` with weight `sqrt(ramp)`. Because the downstream
albedo mix multiplies by `concreteColorBlend = sqrt(pureConcrete)`, the net cement
fraction is **exactly linear in the ramp** — the same weight the lighting exemption
uses, so albedo and lighting agree across the band. It **never sets `cementHit`**, so it
adds no new 0.5 contour.

Two rejected sub-designs, both killed by measurement, both recorded so they are not retried:

- **Requiring the `RUNWAY-EDGE-BLEND` open-border mask (bits 19:16).** A transition tile
  is itself cement, so a pad never flags that side open — which is where the band lives.
  Left the ring ~80% intact. Removed in `0b26eebe`.
- **The CPU-side widening of that mask to transition donors** (`PopulateRecipeCementWords`).
  Dead once the shader stopped consulting the mask. **Reverted — `gos_terrain_indirect.cpp`
  is back to stock**, so this lane touches no CPU classification at all. This also means
  the brief's "beware the SOLID slot→layer map trap" never applied.

For the record, the shipped `MC2_RUNWAY_EDGE_BLEND` was **measured and found not to be
the cure**: it marks only solid donors (**71 tiles** on mc2_23 vs 272 when transition
donors are admitted) and recovers only **1–2.6 luma** of a 30–58 luma deficit while
*introducing* 0.6–10.8% hard-step pixels via its `cementHit` snap.

## 5. Measurement

Capture env: `MC2_STATIC_POP_SPLIT`, `MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE`,
`MC2_GPU_CULL_STATIC_FROZEN_RECORDS` **unset in my shell before every capture**, and the
sibling lane's `CAPTURE-ENV-PIN-1` (`scripts/mc2_env_pin.py`, landed as `1bbb1f9c`) was
in force, so arms were declared via `--env-declare` and all other ambient `MC2_*` was
scrubbed. The effective env is recorded in every manifest. All PNGs non-zero.

**Two-run floor, mc2_23, 6 poses: byte-identical → 0.000%** (measured). `beacons_mid`
drifted on one later repeat, so it is flagged intermittently unstable.

Ring metric (band located by the `MC2_CEMENT_EDGE_CURVE` changed-pixel mask, which is
exactly `0 < pureConcrete < 1`). **`band − terrain` is the defect-relevant number**: a
*negative* value means the band is darker than the terrain on both sides — a dark
outline. `combo` = `MC2_CEMENT_RAMP_WORD=1` + `MC2_CEMENT_EDGE_CURVE=0.25`.

| pose | arm | terrain L | band L | conc L | **band−terr** | ring depth | hard-step % |
|---|---|---|---|---|---|---|---|
| runway_ground | default | 22.74 | 16.03 | 70.00 | **−6.71** | −30.34 | 0.00 |
| | ramp_on | 23.53 | 22.11 | 69.99 | **−1.42** | −24.65 | 2.33 |
| | combo | 23.54 | 26.47 | 69.95 | **+2.93** | −20.27 | 2.77 |
| runway_mid | default | 30.99 | 21.79 | 75.62 | **−9.20** | −31.52 | 0.00 |
| | ramp_on | 30.99 | 27.71 | 75.62 | **−3.28** | −25.59 | 2.85 |
| | combo | 31.20 | 33.67 | 75.62 | **+2.47** | −19.74 | 3.54 |
| runway_overview | default | 29.82 | 19.91 | 76.21 | **−9.91** | −33.11 | 0.00 |
| | ramp_on | 29.97 | 25.27 | 76.21 | **−4.70** | −27.82 | 5.37 |
| | combo | 30.02 | 30.92 | 76.21 | **+0.90** | −22.19 | 7.30 |
| beacons_ground | default | 26.61 | 16.66 | 122.63 | **−9.95** | −57.96 | 0.00 |
| | ramp_on | 26.61 | 23.59 | 122.63 | **−3.02** | −51.03 | 7.04 |
| | combo | 26.66 | 29.84 | 122.63 | **+3.18** | −44.81 | 9.75 |
| beacons_mid | default | 15.56 | 15.70 | 93.52 | **+0.14** | −38.84 | 0.00 |
| | combo | 15.83 | 27.49 | 93.50 | **+11.66** | −27.18 | 4.00 |
| both_fields_overview | default | 22.17 | 20.53 | 83.25 | **−1.64** | −32.18 | 0.00 |
| | combo | 22.27 | 32.27 | 83.25 | **+10.00** | −20.49 | 8.87 |

**Result on the band it targets:** default is **non-monotone** on 5/6 poses (band darker
than the terrain either side = a visible dark outline). `ramp_on` alone removes most of
it; `combo` makes **all 6 poses monotone** (terrain → band → concrete). Ring depth
closes only ~33–37%, because the band still receives partial lighting exemption — the
remaining half is the lighting curve, which is why `combo` and not `ramp_on` is the
arm that lands it.

**Hard steps: 2.8–9.8% (combo).** Lower than `MC2_PAVED_OVERLAY_CLASS`'s 9.2–21.7% but
**not zero, and I will not claim it is clean.** Caveat on the metric: it counts spatial
gradients of the *delta* field, which include the pre-existing crisp cement edge (which
is user-approved and unchanged). It has **not** been separated from genuinely new steps.

**Crisp transitions untouched — structural, not statistical.** The block is guarded by
`!cementHit`, and `cementHit` is forced true on every `cementTransAlpha > 0.5` and solid
interior fragment, so it cannot execute on the marching-squares cut. It writes only
`base`, never `cementTransAlpha`, never `cementHit`, never `pureConcrete`. And with
`gos_terrain_indirect.cpp` reverted to stock, no cement word changes at all.

## 6. Default recommendation

**Leave `MC2_CEMENT_RAMP_WORD` default OFF. Do not promote it.** It fixes a real defect
that is measurably *not* the one the user is complaining about, at a 2.8–9.8% hard-step
cost that has not been shown to be free of new steps. Promoting it now would repeat the
exact pattern that got `MC2_CEMENT_TONE_MATCH` and `MC2_PAVED_OVERLAY_CLASS` rejected:
shipping a lighting/blend adjustment against a complaint whose cause is elsewhere.

## 7. Not done / open — hand this to the next lane

1. **The user's ruling is NOT implemented.** *"The transition tiles should just use the
   same concrete texture."* §2 measures the defect (8+ cells, 43→135 luma, all inside
   correctly-classified concrete). The fix is to point bordering tiles at the pad's cell.
2. **RenderDoc attribution NOT done** (§3) — required before that fix, per the ruling.
3. **tier1 NOT VALID.** 5/5 missions PASS (mc2_01/03/10/17/24) but the runner returned
   **`[DEPLOY_FINGERPRINT] HARD FAIL — RUN INVALID`**: the shared `build64` is contended
   by concurrent sibling lanes, and the deployed exe carries `67b25f4791ad` (another
   lane's commit), not my `0b26eebe`. **There is no `DEPLOY_FINGERPRINT OK`, so this is
   not a tier1 pass and must not be recorded as one.** Needs a re-run from an
   uncontended build.
4. **Coverage gap in the capture matrix.** mc2_23 (night) fully; mc2_05 (day) baseline
   only — `user_102009_lit_compound` drifted on the two-run floor and the day arms were
   not captured. mc2_01/03/10/24 not captured. No control-without-pavement pose.
5. **`gos_terrain_indirect.cpp` is stock** — the alpha-atlas / `bit23 OWN_ALPHA` machinery
   the brief pointed at was ultimately not needed or touched.
