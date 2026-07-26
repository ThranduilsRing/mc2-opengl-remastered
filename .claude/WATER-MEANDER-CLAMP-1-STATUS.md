# WATER-MEANDER-CLAMP-1 — the meander default deletes water; clamp attempted, measured, rejected

Branch `claude/nifty-mendeleev`. Commits `fb2428d3` (clamp + bookmarks),
`9520bc54` (un-scoop), `67b25f47` (revert clamp, park default at 0).
Deploy lane `mc2-win64-v0.4`, exe `67b25f47`, manifest `src_commit 67b25f4791ad`.

## Verdict

**`MC2_WATER_SHORE_MEANDER` default reverted 75 → 0 (OFF).** The distance clamp
the brief asked for was implemented, measured, and **rejected on its own
evidence** — it cannot work, for a structural reason given below. This is
outcome (4) in the mission: park the default at a value that stops the water
loss, and say so plainly. Saying so plainly.

tier1 **PASS 5/5**, `[DEPLOY_FINGERPRINT] OK: exe sha=67b25f4791ad matches
worktree HEAD` on all five missions.

## Measurement floor

Two-run `--verify` on every pose set used for a verdict: **byte-identical,
0.000%**. Floors published before any effect was claimed. All PNGs verified
non-zero (24,887,323 B each); disk held at ~11.8 GB and intermediates were
trimmed.

## Result 1 — the defect, quantified independently

Wide poses at normal gameplay altitude (alt 820–900, proj 30–34), not the
previous lane's alt-320 near pose. Net frame water vs `MEANDER=0`, mc2_17
`shelf_east_wide`:

| arm | net water | lost | gained | frame differs |
|---|---|---|---|---|
| **A=75 (shipping default)** | **−11.18 pts** (42.95% → 31.78%) | 11.23% | 0.05% | 16.48% |
| A=25 | −5.26 | | | 12.44% |
| A=15 | −2.95 | | | 10.64% |
| clamp D=96 | −7.54 | | | 14.83% |
| clamp D=24 | −2.52 | | | 10.45% |
| clamp D=12 | −1.26 | | | 5.99% |

**The loss is one-way: 11.23% lost against 0.05% gained.** That is the whole
story. The perturbation is zero-mean, but `if (shore <= 0.0) discard;` is not —
a seaward excursion adds nothing, a landward one destroys the fragment. A
zero-mean input through a one-sided operator is a net destroyer of water.

Visually at A=75 the bay is eaten by a large smeared brown blob with detached
dark islands floating in open water. That is the user's *"the mc2_17 water is
atrocious. the smoothing line looks terrible."*

Cause: A is a **thickness** carrying fBm gain 2.40, so A=75 is a ±180 wu
excursion — **larger than the entire water depth range of the map** (mc2_17
tops out at 120 wu; mc2_10 at 80). It exceeds the field it perturbs
*everywhere*, so it cannot act as an edge treatment; it punches holes in open
water. This is a stronger statement than the previous lane's `A/|grad|` framing
and does not depend on the gradient at all.

## Result 2 — my own fix, refuted

The brief's proposed distance clamp was built (FS recovers the world-space
thickness gradient from the screen-space Jacobian; both `WaterThickness` and
`WorldPos.xy` are linearly interpolated, so the derivative ratio is the exact
per-primitive plane gradient and is camera-independent). It works as designed —
water loss falls monotonically with D. **It is still the wrong fix, twice over:**

1. **Self-defeating.** It scales the perturbation by `|grad(thickness)|`. The
   staircase it exists to cure lives *precisely where `|grad|` is small* — flat
   terraced runs. So it suppresses the cure exactly where the cure is needed.
   Confirmed by eye: the sawtooth survives every clamped arm, while A=15
   *unclamped* visibly cures it into a smooth organic curve. The defect and the
   cure have the same cause, so no `|grad|`-keyed knob can separate them.
2. **It facets.** The gradient of a linearly interpolated varying is
   piecewise-constant per primitive, so the clamp is too, and hard triangular
   seams appear in the water at useful D.

Reverted rather than shipped default-off, so nobody re-litigates a gate that
provably cannot work.

## Result 3 — lowering the amplitude is not safe either (non-monotonic)

A=15 fixes mc2_17 handsomely. But on mc2_10 `shore_west_wide`, A=15 grows a
**hard blocky staircase that neither A=0 nor A=75 has.** `EffShoreExt` widens
tile admission *by A*, so a small A clips landward excursions back to tile edges
and re-creates the risers the meander exists to remove. No single global
amplitude is safe; the knob is not monotonic in quality.

## Result 4 — the mc2_10 hard constraint, met at byte level

**On the shipping exe `67b25f47`, mc2_10 at A=75 is BYTE-IDENTICAL to A=0 on
all three water poses (`maxdelta 0`, 0.000%).** Turning the default off changes
mc2_10 *not at all*. The brief's hard constraint — proven, not argued — is met
at the strongest available standard.

### Retraction

An earlier arm of this lane, on the intermediate build `d931fdbb`, showed A=75
deleting an inlet on mc2_10 `shore_south_wide`, and **commit `67b25f47`'s
message asserts that as fact.** It does not reproduce on the shipping exe and
**that claim is retracted.** Attribution test: holding the water gate fixed at
A=75 and changing only the exe (`d931fdbb` → `67b25f47`) moves mc2_10 by
3.6–8.4% — the drift is concurrent **sibling-lane** commits on the shared branch
(CEMENT-EDGE-CURVE-1 and neighbours), not this lane. mc2_17 was byte-identical
across the same exe change, which is why its numbers are unaffected.

Read the commit message with that correction applied.

## Result 5 — controls

- **No water in frame**: A=75 vs A=15 byte-identical (`maxdelta 0`) on 4 of 5
  `mc2_10_building_faces` poses. The 5th (`control_no_buildings`) has water in
  shot, so it legitimately moves — it is a buildings control, not a water one.
- **mc2_17 default vs `MEANDER=0` reference: 0.000%, maxdelta 0** on all three
  poses. The new default *is* the meander-off path, proven byte-level.
- **mc2_17 default vs old shipping A=75**: +11.18 pts water recovered on
  `shelf_east_wide`. The deletion is undone.
- mc2_01 at A=75 lost 0.41 / 0.63 pts (the documented beach creep — small).
  mc2_05 at A=75 was byte-identical to A=0 at both poses.

## Deliverable — four water bookmark files

`mc2_10` (the map the A=75 approval was granted on) had **no pose framing water
at all**; `mc2_05` had none either. Coordinates are decoded from each mission
`.pak` MapData packet with water = `elev <= [Water] Elevation`, each pose
centring a ~50%-water coastline neighbourhood at normal gameplay altitude — not
guessed. All pass `check-visual-bookmark-honesty.py` and were **eyeball-verified
to frame real shoreline** (open water + land + waterline), the check the four
deleted fixtures failed.

- `tests/visual/bookmarks/mc2_10_water.json` (3 poses) ← the prerequisite
- `tests/visual/bookmarks/mc2_17_water_wide.json` (3 poses, presentation-grade)
- `tests/visual/bookmarks/mc2_01_shoreline.json` (2 poses)
- `tests/visual/bookmarks/mc2_05_shoreline.json` (2 poses)

Shore `|grad|` per map, decoded from the paks (wu elev per wu horizontal, median):
mc2_17 **0.101** (flattest), mc2_10 0.219, mc2_05 0.259, mc2_01 0.366.

## Honest gaps

- **The staircase is back.** Parking at 0 restores the stock waterline, which
  includes the tile-lattice risers the meander was introduced to cure. That is
  a look; deleting water is a defect. The user can re-approve any amplitude via
  `MC2_WATER_SHORE_MEANDER=75`, which restores the old behaviour exactly.
- **No night/day matrix was controlled.** mc2_10 and mc2_17 render dim at these
  poses; I did not drive a time-of-day axis and do not claim one. All arms
  within a comparison share lighting, so the deltas are valid, but "day AND
  night" was not delivered as a controlled variable.
- **The water-fraction metric is a hue heuristic** (`B+G−2R > 12`), and it is
  unreliable on dim maps — it produced non-monotonic readings on mc2_05/mc2_10.
  Every load-bearing claim above is backed by a true pixel diff or a byte-level
  hash, not by the heuristic.
- **Shared-worktree contamination is real.** Sibling lanes committed to this
  branch four times mid-session. Quantified and attributed in Result 4 rather
  than hand-waved; mc2_17's numbers are clean across it.
- A future real fix must make the perturbation **one-sided-safe** — e.g. bound
  it so it can never drive `shore` below the discard threshold in water deeper
  than the shore band, decoupling silhouette bending from fragment destruction.
  Not attempted here.
