# SHORELINE-GEOMETRY-1 — STATUS (master-FAIL #1 re-open, CRITICAL)

**Lane:** SHORELINE-GEOMETRY-1 · worktree `A:/Games/mc2-main` · branch
`claude/nifty-mendeleev` · verification lane
`A:/Games/mc2-opengl/releases/mc2-win64-shoreline-test`.
**Commit:** `4be51440` (local only, NOT pushed).
**Gate:** `MC2_WATER_SHORE_MEANDER` (+ `MC2_WATER_SHORE_MEANDER_FREQ`),
default OFF, byte-identical.
**Result:** SHIPPED. Silhouette is now an organic curve on the critical
mc2_10 regions; verified by exaggeration, full-zoom crops on two maps, the
shore-staircase sentinel, and tier1.

## The rejection this slice answers
The user rejected `MC2_WATER_SHORE_FEATHER` (`a8ea6726`): *"still looks
terrible. there is a literal stair step? and all sorts of weird shore
geometry."* They annotated `shore_right_landmass` (mc2_10) with a red line
showing a smooth sinuous coastline against the shipped rectangular steps.

## Root cause (this is the load-bearing finding — the prior slice had it wrong)
The prior status doc blamed a too-narrow alpha ramp. That is not the mechanism.

The water FS discards on `WaterThickness = waterElevation − interpolated
terrain z`, which is **bilinearly interpolated across the tile**. Its
iso-contour is therefore already sub-tile and smooth *in principle*. The
staircase survives because the stock heightfield is **terraced**: long runs of
shore vertices share a single elevation, so the iso-contour of that field
degenerates onto the tile lattice — each boundary tile contributes exactly one
straight segment, and those segments chain into the ~140 px axis-aligned and
45-degree risers the user annotated.

Consequence: **widening the alpha band blurs a riser but cannot bend it.** The
first slice was an alpha fix for a geometry-shaped defect, which is precisely
what the user saw through.

## The fix — MC2_WATER_SHORE_MEANDER (perturb the field, not the alpha)
Add a **static, zero-mean, world-space 2-octave fBm displacement** (amplitude
`A` world units) to the thickness that drives the shore term and the `discard`,
so the contour meanders through tile interiors instead of tracking the lattice.

Key properties:
- **Self-scaling.** Horizontal contour displacement is `A / |grad(thickness)|`
  — large across the flat terraces that produce the worst steps, small on
  cliffs where the coast already reads organic. Same adaptive principle the
  feather had, but applied where it actually changes shape.
- **Time-invariant.** A swimming coastline would be far worse than a stepped
  one, so the noise is keyed only on `WorldPos.xy` — no `time` term. Seam-free
  and camera-independent (continuous world coords, precision-safe fract-early
  hash), reusing the `fbm3`/`vnoise`/`h21` helpers already in the water FS.
- **Shape-only.** Only the shore/alpha term is perturbed; the depth **tint**
  still uses the true `WaterThickness`, so shallow/deep colour stays keyed to
  real terrain and only the coastline silhouette changes.
- **Geometry follows the contour.** `WaterStream::EffShoreExt(baseAlphaDepth)`
  is now the **single source of truth** for tile admission and widens by `A`.
  Without this, landward excursions clip back to the stock tile boundary and
  the step reappears. It replaces four hand-duplicated copies of the
  `effAD*0.5f : 15.0f` expression, so the CPU stream build, both
  `mclib/terrain.cpp` narrow-append branches, and the `u_shoreExt` fed to
  `gpu_driven_water.comp` can no longer drift apart.

`A` unset/≤0 → FS skips the term and `EffShoreExt` collapses to the stock
expression → byte-identical.

## Disposition of the old gate
`MC2_WATER_SHORE_FEATHER` **stays, default-OFF, and should NOT be enabled in
the shipped launch config.** Recommendation: treat it as **superseded** for the
staircase defect — it does not address the silhouette and the user has already
rejected it by name. It is retained (not deleted) because it is an independent,
harmless soft-edge taste knob that composes cleanly with the meander (the two
are orthogonal: band width vs contour shape), and deleting it would churn a
registered gate for no benefit. If the coordinator wants one knob only, retire
it in a follow-up dead-gate sweep.

## Files / commit
`4be51440` — fix(water): SHORELINE-GEOMETRY-1 — organic coastline via
shore-contour meander:
- `GameOS/gameos/gos_terrain_water_stream.{h,cpp}` — gates + `EffShoreExt`
- `GameOS/gameos/gameos_graphics.cpp` — two uniform uploads (MDI water path)
- `mclib/terrain.cpp` — both narrow-append mirrors routed through `EffShoreExt`
- `shaders/gos_terrain_water_mdi.frag` — the meander term
- `scripts/check-env-registry.sh`, `scripts/run_smoke.py`,
  `docs/tier1_env_vars.md` — registration/allowlist/docs

`gate_status(MC2_WATER_SHORE_MEANDER)` verdict **OK** (documented,
smoke-allowlisted, registry-allowlisted, 1 reader, no flags).

## Verification
Deployed `4be51440` to an isolated lane (`mc2-win64-shoreline-test`, cloned
from v0.5.1) so the shipped v0.5.1/v0.5.2 release dirs were never touched.
Poses: `mc2_10_saved_204523` (the "worst water" top-down) and
`mc2_01_saved_205644` (second water map). All A/B is gate-OFF vs gate-ON
**within the same exe**.

| Condition | vs golden v0.5.2-baseline-PROPOSED | Eyeball (full-zoom) |
|---|---|---|
| Control (OFF) | **11/11 PASS**, every shore metric exactly 0.0 | default byte-identical ✓ |
| Exaggerate A=120 | shore FAIL, inlet chg 36.1% struct 11.4 | **mechanism proven** — fully organic sinuous coast, every rectangle gone (over-driven, eats land) |
| A=45 | inlet chg 27.2% struct 6.05 | right coast organic; **upper-left square corners survive** |
| A=60 | inlet chg 29.3% struct 7.56 | good; residual squarish notch upper-left |
| **A=75 (recommended)** | inlet chg 31.2% struct 8.81; landmass chg 9.4% struct 2.53 | **upper-left fully organic, right coast a flowing sinuous curve, landmass preserved** |

tier1 smoke on this exe (`--exe .../mc2-win64-shoreline-test/mc2.exe`):
**5/5 PASS**, 0 mover-coherence violations, diag-state PASS.

Evidence: `<scratchpad>/sg_{off,45,60,75,exag}/` (report.json + contact_sheets,
both maps). REVIEW pack refreshed:
`release_assets/0.5.2/REVIEW/01_shoreline/` (+ `README.md` explaining each crop).

### Reading the sentinel metric correctly (correction to the prior slice)
`struct_delta` measures divergence **from the golden**, and the golden encodes
the staircase. The prior slice reported `struct 3.36 < 6.0` as success — but a
*low* value means the structure barely moved, which is exactly why the user
still saw steps. For a deliberate silhouette fix the shore regions **must**
exceed the threshold; the guarantee that this is not collateral damage comes
from the control run being exactly 0.0 everywhere. Shore FAIL-vs-golden here is
the intended signal, not a regression.

## Honest residual on silhouette quality
1. **mc2_10 (the map that was graded): the steps are gone.** At A=75 both
   critical regions read as curves, including the headline inlet whose hard
   90-degree step becomes a curved diagonal. I am confident this matches the
   intent of the user's red line.
2. **mc2_01 advances the waterline.** Its beach is a shallow flat gradient, so
   `A/|grad|` travels much further there — at A=75 the water creeps up the
   beach (chg 10.1%). It reads as a natural tide line, not a defect, but it is
   a real side effect and the reason A is a taste knob rather than a constant.
   A per-map amplitude (or clamping the displacement to a max world distance
   instead of a max thickness) would decouple the two; deferred as a follow-up.
3. **This is a shading-space contour fix, not tessellation.** The water mesh is
   still 2 fixed triangles per admitted tile; the silhouette is produced by the
   FS discard. That is sufficient because the discard contour *is* the visible
   land/water boundary — but it means the meander cannot express detail finer
   than the fragment level near-silhouette at extreme zoom, and it does not
   change the terrain's own texture tiling (unrelated defect class).
4. **GL path only.** `vulkan_water_island.cpp` sources `alphaDepth` from its own
   push constant; VK stays byte-identical (gate default-OFF). Extending it
   means routing the two uniforms into the VK push-constant block.
5. **Not pushed, not merged, gate OFF in every install.** Enabling
   `MC2_WATER_SHORE_MEANDER=75` in the shipped launch config is a
   coordinator/user taste call.
6. Three other lanes (GHOST-LIGHT-VISIBILITY-1, RUNWAY-EDGE-DILATE-1,
   LIGHT-REALISM-1) committed on top of `4be51440` during this session; my
   commit is an ancestor of HEAD and my verification exe predates theirs, so
   the A/B is uncontaminated.
