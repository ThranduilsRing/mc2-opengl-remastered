# CEMENT-INTRACELL-1 — STATUS

**Lane:** CEMENT-INTRACELL-1 · branch `claude/nifty-mendeleev` · 2026-07-28
**Commits:** `c7e0499c` (ARM C) · `f51cd63d` (background-reference fix)
**Deliverable:** `release_assets/0.5.2/REVIEW/INDEX.html` → **§19**, now
*"Concrete tone — ARM A vs ARM B vs **ARM C** (your choice)"* (`#19_cement_concrete`),
112 blocks (27 appended). Builder: `scratch/cic1/build_sec19c.py` (idempotent,
extends the tracked manifest — the HTML is never hand-edited).
**Verdict:** the premise both prior arms rest on is **refuted by the art itself**.

---

## 1. What the dark regions are

CEMENT-ATLAS-CELL-2 measured a large dark region in **78 of 110** mc2_24 solid
cement cells (36–52% of the tile at L≈60 against concrete L≈147) and recorded it
as *"authored material (joints, tarmac, shadowed detail)"*, explicitly deciding
it was **not** paint because no texel exceeded its cell band by 45 luma.

That test looked only at the **bright** side. I dumped the atlas with the tool
that lane built (`MC2_CEMENT_ATLAS_DUMP`) and **rendered it**:

> The dark regions are a **baked asphalt ROAD TILESET** — straight runs, curves,
> T-junctions, crossroads with crosswalk stripes, end caps, and a multi-lane
> highway-with-guardrail piece — painted onto a concrete background.

Asphalt is dark, which is exactly why a bright-side test could not see it. The
shapes are unambiguous under inspection: 20 px strips in a 64 px tile, running
edge-to-edge, with `darkfrac` quantised at 0.31 (= 20/64, a full band), 0.14
(end/junction) and 0.53 (crossroads = two crossing bands). Contact sheets are in
the pack (`19_cement_concrete/mc2_24_atlas_roadtileset.png`, `mc2_23_…`).

**Answering the brief's specific questions:** shape = tileset primitives, not
blobs or gradients; position = edge-to-edge bands on tile centre lines, so they
mate with *specific* neighbours; tiling coherence = pieces designed to connect
(a full-band cell wraps L↔R seamlessly, `seam_lr ≈ 1.9`; an end-cap deliberately
does not, `seam_lr ≈ 21`). They are continuous ground detail, authored to be
seen — not an artefact.

## 2. The reframe: there is no concrete tone problem

With the road texels excluded, the concrete **background** is already one colour
on every map:

| map | concrete background L | spread | std |
|---|---|---|---|
| mc2_24 | 144.7 – 149.5 | **4.7** | 0.89 |
| mc2_05 | 145.7 – 148.9 | 3.2 | 0.75 |
| mc2_03 | 146.3 – 148.7 | 2.4 | 0.71 |
| mc2_10 | 146.6 – 148.6 | 2.0 | 0.55 |
| mc2_23 | 105.7 – 148.9 | 43.2 | 7.64 |
| mc2_01 | 105.7 – 148.4 | 42.7 | 5.16 |

mc2_23's and mc2_01's spans are driven entirely by **two** and **one** cells
(all at bg 105.7, chroma 0.152). Rendered, those are **wide highway pieces with
guardrails** — road, not concrete. Excluding them, mc2_23 is 144.8–148.9.

**So what makes concrete "magically become a different color" inside a cell is a
road**: background L≈147 against asphalt L≈82, a **~65-luma hard-edged step** —
precisely the class the user scoped *out*. That scoping is true for the
`&Overlays` class; it is **not** true for this one, which is baked into the
terrain texture catalog, so `TileIsRoadArt()` reports *"solid cement, not road"*
and every concrete lever treats a road as concrete.

## 3. Treatment chosen, and why

The brief offered: normalise the dark region's luma; suppress it as a
double-shading artefact; or leave it and evidence that the art is authored that
way. **The third, plus the fix it implies.** Normalising or suppressing would
delete the map's road network. So ARM C leaves the art alone and instead makes
the engine *know* it is a road:

**`MC2_CEMENT_ROAD_ART_DETECT=1`** (default OFF) — split each cell against its
own `[P60,P95]` upper mode (the concrete background) rather than the whole-cell
band mean, which on a road-heavy cell is dragged down by the road and hides it;
flag a cell with >5% of texels more than 40 luma below that background as road;
**exclude flagged cells from ARM A's tone pass, ARM B's remap, and the canonical
cell choice.**

### What ARM C prevents — the finding that matters most

Without it, in-engine on mc2_24:

```
[CEMENT_CONCRETE_CANON v1] canon_cell=10 canon_road_frac=0.312 remapped=109 kept_road=0   armC=0
[CEMENT_CONCRETE_CANON v1] canon_cell=0  canon_road_frac=0.000 remapped=31  kept_road=78  armC=1
```

**ARM B's canonical cell is itself a road tile on 5 of the 6 maps.** ARM B erases
every junction, curve and end cap *and stamps one straight road segment across
every plain concrete tile on the map*. The mc2_23 `runway_overview` pair in §19
shows it plainly: a coherent airbase road grid becomes a repeating stipple of
road fragments. **ARM B as it stands is destructive and should not be promoted.**

## 4. Measurements (two-run floor published first)

⛔ = floor exceeds the effect ⇒ **no claim**. mc2_10 not used (floor 27–31%).

| map | pose | floor % | ARM A % | ARM B % | **ARM B+C %** | B vs B+C % | hard-step OFF / B / B+C |
|---|---|---|---|---|---|---|---|
| mc2_23 | runway_overview | **0.0000** | 59.8 | 31.8 | **0.0** | 31.8 | 3.014 / 3.202 / **3.014** |
| mc2_23 | runway_mid | **0.0000** | 68.6 | 32.8 | **0.0** | 32.8 | 0.657 / 0.758 / **0.657** |
| mc2_23 | runway_ground | **0.0000** | 63.7 | 38.7 | **0.0** | 38.7 | 0.340 / 0.382 / **0.340** |
| mc2_23 | both_fields_overview | 1.0288 ⛔ | 32.8 | 17.9 | 1.0 | 17.9 | 2.369 / 2.487 / 2.370 |
| mc2_23 | beacons_mid | 17.4930 ⛔ | 14.8 | 18.9 | 18.6 | 21.6 | — |
| mc2_24 | industrial_lightfacing | **0.0000** | 52.9 | 26.9 | **0.0** | 26.9 | 3.029 / 3.042 / **3.029** |
| mc2_24 | fortress_lightaway | **0.0000** | 53.6 | 24.7 | **0.0** | 24.7 | 0.581 / 0.630 / **0.581** |
| mc2_24 | saved_192316 | 0.0198 | 43.9 | 21.7 | **0.0** | 21.7 | 3.079 / **3.697** / **3.079** |
| mc2_24 | base_near | **0.0000** | 16.4 | 4.3 | 0.4 | 3.9 | 0.942 / 0.918 / 0.938 |
| mc2_24 | water_near (control) | **0.0000** | 0.0 | 0.0 | 0.0 | 0.0 | 0.000 / 0.000 / 0.000 |
| mc2_05 | refinery_lightfacing | **0.0000** | 5.2 | 11.4 | **0.0** | 11.4 | 0.651 / 0.656 / **0.651** |
| mc2_05 | refinery_lightaway | **0.0000** | 4.8 | 7.9 | **0.0** | 7.9 | 0.554 / 0.553 / **0.554** |
| mc2_05 | saved_101748 (user pose) | **0.0000** | 0.0 | 0.0 | 0.0 | 0.0 | 0.012 / 0.012 / 0.012 |
| mc2_05 | saved_102009 (user pose) | 13.7105 ⛔ | 18.0 | 15.6 | 13.7 | 16.7 | — |
| mc2_05 | control_no_buildings | **0.0000** | 0.0 | 0.0 | 0.0 | 0.0 | 0.000 / 0.000 / 0.000 |

**ARM B+C is 0.0% on every floor-valid pose.** That is not weakness: it means
**100% of ARM B's measured effect on those views was road destruction**.

**ARM C alone vs OFF** (mc2_23, measured — not asserted): `runway_ground`,
`runway_mid`, `runway_overview` all **0.0000% changed, max ΔL 0.00**. The three
nonzero poses (6.7 / 18.7 / 1.05%) each sit at or below their own two-run floor
(9.6 / 17.5 / 1.03%) — capture noise, no claim. **ARM C is a pixel no-op.**

**Hard-step %** (adjacent-pixel luma jump >16; the metric that sank
`MC2_PAVED_OVERLAY_CLASS` at 0.6–10.8pp): ARM B alone **raises** it up to
**+0.618pp** (mc2_24 saved_192316). **ARM B+C returns it to the OFF value exactly
on every floor-valid pose; ARM C's own delta is 0.000pp.** Bar was ≤0.62pp — met
with zero.

**Intra-cell spread** is unchanged by ARM C *deliberately*: the ~65-luma step
(concrete L≈147 → asphalt L≈82) is the road, and ARM C exists to protect it.

## 5. Honest limits

- **The user's complaint is not cured.** I refuted the premise both prior arms
  rested on and blocked a destructive choice. Why the mc2_23 apron reads as
  patchy *if the concrete is one colour* is still open — remaining suspects are
  the tile-level lighting term and how the road tileset **pieces are placed** on
  the map (a mismatched piece leaves a stub or broken run). Neither is an
  atlas-tone question. Stated in the pack, not buried.
- **`bg_span` discrepancy, disclosed.** The engine census reports mc2_24
  `bg_span=17.6`, my offline analysis 4.7. The shipped two-pass background
  reference is still road-weighted on the heaviest crossroads cells (>40% road
  coverage), so it **over**states the span — conservative direction, and it does
  not change detection (`family_road=78` both ways) or any arm's behaviour.
  Tightening it further would be a third rebuild for a census cosmetic.
- **Byte-identity when OFF** is established by construction + the measured ARM C
  0.0000%, not by `run_golden_parity.py` against the blessed golden.
- `TileIsRoadArt()` remains wrong for this class of road even with ARM C on —
  ARM C fixes the concrete *arms*, not the oracle. Widening the oracle would
  change road classification for other consumers and was out of scope.

## 6. Process / deviations

- **Isolated build**, per the brief: reused `A:/Games/mc2-cac2` (the predecessor
  lane's tree, left in place for exactly this) rather than creating a 30th
  worktree on a 99%-full disk. Checked out **detached at my own sha**, clean tree,
  `MC2_BUILD_GIT_DIRTY=0`.
- **Deploy lane deviation, disclosed:** deployed to `mc2-win64-v0.4d-rc1` rather
  than a brand-new lane. It is the immediately-preceding lane of this same arc,
  was unlocked and unleased, and is **not** in the forbidden set (v0.3 / v0.4 /
  v0.4c / `refbuild-cf97ed52`). A fresh multi-GB lane on 13–16 GB free was the
  riskier act.
- **tier1 5/5 PASS**, `[DEPLOY_FINGERPRINT] OK: exe sha=c7e0499c20c7 matches
  worktree HEAD` logged on **all five** missions (that run was on `c7e0499c`;
  `f51cd63d` is a census-only refinement built and deployed after it).
- `run_smoke` refused to start with `MC2_STATIC_POP_SPLIT` /
  `MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE` / `MC2_GPU_CULL_STATIC_FROZEN_RECORDS`
  ambient — the ENV-DROP guard fired correctly; unset per the brief.
- Gate hygiene: `gate_status(MC2_CEMENT_ROAD_ART_DETECT)` verdict **OK**
  (registered, documented, smoke-allowlisted, default OFF, 2 readers).
  `check-env-registry.sh` **PASS** (1009 vars accounted for). No unregistered
  gate added while CONTRACT-REPAIR-2 is live.
- Disk: 16.0 → 13 GB. Capture sets under `A:/Games/mc2-cac2/scratch/cic1/`
  (caps, atlas dumps, logs, `metrics_*.json`) — prunable.

## 7. Open / user-gated

- **The choice**, now from four frames: OFF · ARM A · ARM B · **ARM B + ARM C**.
  The lane's evidenced position: **ARM B must not ship without ARM C**, and on
  the floor-valid poses ARM B+C is indistinguishable from OFF — so the honest
  recommendation is **OFF**.
- Whether to widen `TileIsRoadArt()` for the baked-road class.
- The real cause of the mc2_23 apron patchwork (see §5) — needs a new lane.
- `A:/Games/mc2-cac2` still holds a warm `build64` at `f51cd63d`.
