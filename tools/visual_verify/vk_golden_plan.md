<!-- doc-status: current | reviewed:2026-10-16 -->
# VK golden set: blessing plan and the 14 refused regions

WHAT: how the Vulkan golden set is blessed from Vulkan captures, and what happens to the 14 regions the GL 1080p set refused.
WHY: GL deletion step 1 (the capture and tooling GL arms, `.claude/cleanup/gl-retirement-inventory-2026-10-15.md` sections 11-13) needs a
golden that was drawn by VK. Until 2026-10-16 `render_resolution.CURRENT_GOLDEN` was `golden-e9e3f19d6-1080p`, blessed from three GL runs
(`capture_env.backend = {requested: gl, drew: [gl]}`), so a VK candidate graded against it measured the GL/VK gap, not a regression.
Switched on 2026-10-16 to `golden-d9e1f3801716-vk-1080p` (section 4; done, see section 6).

## 1. The 14 refused regions of `golden-e9e3f19d6-1080p`

Source: `golden.json` `refused` (13 unstable by the all-pairs rule, 1 degenerate crop). The three GL runs are `A:/Games/mc2-lane2/.claude/l2cap/gold1080-r{1,2,3}`
(exe e9e3f19d6, frozen animation, deterministic clock). Viewed 2026-10-15 (lane 7): no run is shifted against another (phase correlation 0,0 on every
pose), the camera record `cap_pos` is identical in all three, and two poses (mc2_01 205644 day, mc2_10 204523 night top-down) have a mean pixel difference of 0.00 across the runs. On `mc2_10_saved_204753`
the r1/r3 difference is one large floodlight pool on the apron that changes colour and extent (warm in r1, white in r3) while geometry and
camera agree. Cause class of the 12 night-light refusals: **the GL night light feed is not run-to-run deterministic under
`MC2_SMOKE_FREEZE_ANIM` + the deterministic clock** (which lights reach the pool/cluster set, or their intensity phase, differs between launches). Not proven
which: the freeze gate covers mech gait and building animation frames only (`mclib/anim_freeze.h`); light selection, light animation and the
water clock are outside it.

Decision key: **keep** = the region stays in its manifest unchanged and is expected to be refused again (a declared hole of the VK set until its
precondition lands); **re-bless** = blessed into the VK set (rect unchanged unless stated) once its precondition lands; **retire** = deleted from
the region manifest in the commit that blesses the VK set.

| # | pose / region | sev | refused because (worst pair) | decision | reason / precondition |
|---|---|---|---|---|---|
| 1 | mc2_05_205843 / light_pools_feathered | med | unstable, 17.3% changed (all three pairs) | re-bless | night pool shape is the region's subject; needs P1 + parity row 1 |
| 2 | mc2_10_204611 / horizon_white_billboards | high | unstable, 11.1% (r1/r2) | retire | its check describes a GL-era defect (fullbright white impostors); a golden of it would freeze the defect; the horizon is covered by far_water_surface and the poses 204523 / 212019 |
| 3 | mc2_10_204611 / far_water_surface | med | unstable, 23.6% on all pairs | keep | the water surface animates on its own clock, which no capture gate freezes; re-bless only if P2 is built |
| 4 | mc2_10_204753 / light_clusters_census | med | unstable, 44.8% (r1/r3) | re-bless | the viewed floodlight-pool case; P1 + row 1 |
| 5 | mc2_10_204753 / runway_edge_lights | med | unstable, 56.0% (r1/r3) | re-bless | edge-light sprites + pools; P1 + rows 1, 5 |
| 6 | mc2_10_204753 / pad_outline_fence | low | unstable, 8.9% (r*/r3 only; r1 = r2) | retire | its check says the stipple is a legit asset, not a defect; it sits under the varying pool and adds nothing light_clusters_census does not grade |
| 7 | mc2_10_204753 / lamp_cones | low | unstable, 64.2% (r1/r3) | re-bless | lamp-base cone anchoring is a live VK question (row 1); P1 |
| 8 | mc2_10_204907 / distant_ridge_base | crit | degenerate: 1 colour, luma 0.0 | re-bless, rect re-authored | at 1080p the 4K-referenced rect lands on the black off-map slab above the ridge (viewed); move it onto the lit ridge-top compound (about x 1300-1600, y 400-520 of 1920x1080) and keep the "angle B goes all-dark" sentinel |
| 9 | mc2_10_204907 / spotlight_towers | med | unstable, 29.2% (r2/r3) | re-bless | P1 + row 1 |
| 10 | mc2_10_204907 / near_base_lighting | low | unstable, 4.6% (r2/r3; r1/r2 0.67%) | re-bless | marginal; P1 is expected to clear it |
| 11 | mc2_10_212258 / runway_pad_edges_blend | high | unstable, 13.2% (r*/r3 only; r1 = r2) | re-bless | pad edge shape is what VK rows 5 and 6 change; the noise is the pool over the pad; P1 + rows 5, 6 |
| 12 | mc2_10_212258 / runway_grid_complete | med | unstable, 19.8% (r2/r3) | re-bless | runway markings = row 6; P1 + row 6 |
| 13 | mc2_10_212258 / cement_tile_seams | low | unstable, 8.2% (r2/r3) | retire | its check grades an art-style seam, low severity, overlapping regions 11-12 |
| 14 | mc2_10_212258 / light_pools_sourcing | low | unstable, 26.9% (r2/r3) | re-bless | sourceless-pool sentinel; P1 + row 1 |

Totals: **keep 1, retire 3, re-bless 10** (one of them with a new rect). The 25 regions the GL set did bless are all re-blessed from VK runs.

Preconditions named above:
- **P1, night light determinism.** Two same-exe launches of a night pose under the capture env must agree on the light set. Proof: three VK
  captures of `mc2_10_saved_204753`, all pairs under every region threshold. Owner: lane 1 (sim/determinism) for the light producer, lane 2 for
  the VK pool consumer. Until P1 lands, a VK bless refuses the same 9 light regions and that is acceptable: they are declared holes, not coverage.
- **P2, water clock under the freeze** (optional): a capture-mode water time pinned to the sim frame. No lane owns it; without it region 3 stays
  a declared hole.
- **Parity rows** (`.claude/VK-PARITY-GAPS.md`): a region is not re-blessed from VK while a row that changes its pixels is MISSING or PARTIAL,
  or the VK golden would freeze the gap. Open rows touching golden poses: 1 (pools), 2 (mech shading), 4 (shadow fade), 5 (night apron), 6 (overlays),
  7 (shore edge), 9 / 14 (terrain relief, cliff), 17 (static building map), 20 (option paths).

## 2. Blessing the VK set

| item | value | why |
|---|---|---|
| poses | the 10 poses vendored in `golden-e9e3f19d6-1080p/poses` (mc2_01 205644, mc2_05 205843, mc2_10 204523 / 204611 / 204753 / 204907 / 205952 / 212019 / 212140 / 212258) | same coverage as the GL set, so the region manifests carry over; day and night are both present |
| added pose | the mc2_10 night runway low bookmark lane 2 added for rows 5 and 15, once it has a region manifest | the GL poses sit too high to show lit windows and fixtures (row 15) |
| resolution | 1920x1080 (`render_resolution.PINNED_WIDTH/HEIGHT`, applied by `run_visual_capture.py`) | the tier1 and golden size; `check-capture-resolution.py --current` enforces it |
| backend | `--env MC2_RENDER_BACKEND=vulkan --env MC2_VK_SHARED_GBUFFER=1` (the umbrella) | the release path; `capture_run.json` records `requested` and `drew`, readback `vk_present` |
| animation | `--env MC2_SMOKE_FREEZE_ANIM=1`, deterministic clock on (the default), trigger frame 431, settle 30 | owner ruling: captures run with animations frozen |
| options | the 17 options.cfg render options at their defaults; recorded per run; no `--vary` | `bless.py` refuses runs with differing or missing options |
| validation | off for the capture runs | the validation layer is proven in the VK tier1 (zero errors); capture runs keep their timing |
| runs | three separate launches, lease-serialized, one install, one exe | the three-run all-pairs rule (`MIN_RUNS`) |
| noise floor | per region the manifest `thresholds`; the all-pairs grade is the floor; a region that exceeds its own threshold run to run is refused, never loosened | thresholds are drift budgets for a candidate, so they must hold run to run |

One-command bless (new in this lane: `bless.py --backend`): it refuses unless every run asked for the named backend, was drawn by it alone and
declared `MC2_SMOKE_FREEZE_ANIM=1` plus the backend's own env (vulkan: the umbrella pair), and records `golden.json` `backend`.

```
py -3 tools/visual_verify/capture.py --install <lane install> --bookmarks all --out .claude/l2cap/vkgold-r1 \
    --env MC2_RENDER_BACKEND=vulkan --env MC2_VK_SHARED_GBUFFER=1 --env MC2_SMOKE_FREEZE_ANIM=1      (then r2, r3)
py -3 tools/visual_verify/bless.py --backend vulkan --run .claude/l2cap/vkgold-r1 --run .claude/l2cap/vkgold-r2 \
    --run .claude/l2cap/vkgold-r3 --to tests/visual/goldens/golden-<exe sha12>-vk-1080p
```

Acceptance before the set becomes current: (1) `verify.py --golden <new set>` on run r2 = every blessed region PASS; (2) a person views every
pose of r1 next to the same-exe GL capture (`tools/visual_verify/ab.py`) and the montage is committed under `.claude/l2cap/montages/`;
(3) the refused list equals the keep column above plus anything new, each with a reason; (4) the three retired regions are deleted from
`tools/visual_verify/regions/*.json` and region 8 re-authored in the same commit; (5) `render_resolution.CURRENT_GOLDEN` names the VK set;
(6) both GL sets get a `RETIRED.md` row (`golden-e9e3f19d6-1080p`, `reset-50331d5`), deleted with the GL arms.

Order: re-author region 8 and retire 2, 6, 13 (manifest edit, no capture) -> P1 -> the parity rows touching each pose -> three VK runs -> bless
-> acceptance -> GL deletion step 1. An interim VK set may be blessed before every row closes, under its own name and never as CURRENT_GOLDEN,
to give the GL-arm removal a VK reference; the final set is re-blessed after the last row closes.

## 3. Blessed 2026-10-16: `golden-e350d0b4a5c6-vk-1080p` (interim, not CURRENT_GOLDEN)

Retired the same day (superseded by the section 4 set): archived under `E:/mc2-archive-2026-10/docs/vk-interim-golden-2026-10-16/`,
row in `tests/visual/goldens/RETIRED.md`. The record below is kept as the provenance of the bless method.

| item | value |
|---|---|
| runs | `.claude/l2cap/vkgold26-r{1,2,3}`, exe e350d0b4a5c6 (clean), lane install, the 10 poses of section 2, umbrella pair + `MC2_SMOKE_FREEZE_ANIM=1`, validation off |
| P1 | LANDED (06c4c08be): the capture harness sets `MC2_DETERMINISTIC_RNG=1` with the deterministic clock. The varying night light was the turret searchlights, whose idle sweep targets come from the simulation RNG (wall-clock seeded in play); `MC2_LIGHT_POOL_DUMP` showed slots 58 / 59 at different positions in two launches |
| noise floor (mean abs RGB sum / px over 30, all pairs) | 205644 0.002 / 61; 205843 0.000 / 1; 204523 0.000 / 0; 204611 0.004 / 15; 204753 0.000 / 0; 204907 0.001 / 0; 205952 0.004 / 7; 212019, 212140, 212258 0.000 / 0. GL at 204753 (three runs) 0.000 / 0 |
| blessed | 36 of 36 regions, 0 refused; `verify.py --golden golden-e350d0b4a5c6-vk-1080p` on r2: 10 poses PASS, 36 regions PASS |
| retired (manifest edit, same commit) | 2 `horizon_white_billboards`, 6 `pad_outline_fence`, 13 `cement_tile_seams` |
| region 8 `distant_ridge_base` | blessed with its rect UNCHANGED: under VK the 4K-referenced rect lands on the lit ridge-top compound (viewed, `scratch` grid overlay of r1); the rect proposed in section 1 (x 1300-1600, y 400-520) lands on the lake and was not taken |
| region 3 `far_water_surface` | blessed: with the RNG seeded the water is stable run to run (204611 worst pair 0.004 mean), so P2 is not needed for this pose |
| viewed | every pose of r1 next to the same-exe GL capture `gl26-r1`: `.claude/l2cap/montages/s26-vkgold-gl-vk-{1,2}.png` (GL left, VK right); VK-GL mean 205644 2.49, 205843 6.07, 204523 1.67, 204611 3.53, 204753 3.75, 204907 5.14, 205952 10.1 (the white GL map-edge skirt), 212019 4.17, 212140 3.11, 212258 3.04 |
| why interim | parity rows that change these poses are still open (1, 2, 4, 6, 7, 9 at least), so this set is the VK reference for GL deletion step 1, not the final golden; re-bless after the last row closes and then point `CURRENT_GOLDEN` at it |

## 4. Re-blessed 2026-10-16: `golden-d9e1f3801716-vk-1080p` (CURRENT_GOLDEN since the 2026-10-16 switch)

| item | value |
|---|---|
| runs | `.claude/l2cap/vkgold27-r{1,2,3}`, exe d9e1f3801716 (clean), same poses, env and options as section 3; noise floor all pairs at most 0.004 mean / 57 px over 30 (204611) |
| blessed | 36 of 36 regions, 0 refused; `verify.py` r2 against it: 10 poses PASS, 36 regions PASS. `golden.json` `capture_env.harness_env` now records the harness settings (`MC2_DETERMINISTIC_RNG=1`, `MC2_SMOKE_FIXED_TIMESTEP=1`, seed, size) |
| why re-blessed | r2 against the section-3 set: 14 regions FAIL, every one a legitimate parity change measured against the same-exe GL capture. Row 1 (prop light pools, bcb9b5623), 11 regions: 204523 compound_light_wedges (VK-GL in the rect 3.99 -> 1.39), 204611 far_water_surface (10.46 -> 3.30), 204753 light_clusters_census (7.79 -> 2.43), runway_edge_lights (6.12 -> 1.85), lamp_cones (10.45 -> 1.55), 204907 distant_ridge_base (17.5 -> 1.94 at its 1080p rect), spotlight_towers (9.08 -> 2.81), 212019 ghost_wedge_over_road (5.61 -> 5.19), searchlight_cones_compound (6.29 -> 3.53), light_pools_road (4.00 -> 3.03), 212258 light_pools_sourcing (7.35 -> 1.14). Row 9 (map-edge feather, 8d7f14ec8), 3 regions on 205843: cement_pads_dark, mech_night_shadow_fan, shoreline_dark_water (pose VK-GL 4.52 -> 0.52) |
| viewed | every pose of r1 next to the GL capture `s27b-gl` (GL path code-identical to this exe): `.claude/l2cap/montages/s27-vkgold-gl-vk.png` (GL, VK, VK, diff, diff); VK-GL mean 205644 2.48, 205843 0.52, 204523 1.49, 204611 1.89, 204753 2.01, 204907 1.99, 205952 2.79, 212019 2.42, 212140 3.11, 212258 1.57 (section 3: 2.49 / 6.07 / 1.67 / 3.53 / 3.75 / 5.14 / 10.1 / 4.17 / 3.11 / 3.04) |

### CURRENT_GOLDEN decision (2026-10-16)
Not yet. Of the rows that change these poses, 1, 4, 6, 7 and 9 are closed with numbers (`.claude/VK-PARITY-GAPS.md`); row 2 (mech shading)
is ported but not proven, because the golden poses show mechs at a few dozen pixels and the term oracle there is inconclusive. Switch when:
(1) row 2 is proven on a close-up mech bookmark (day and night, imported and stock mech) or ruled to need no golden change; (2) if that row
moves pixels in these poses, re-bless (three runs, the section 2 recipe); (3) `render_resolution.CURRENT_GOLDEN` names the VK set in the same
commit as the `RETIRED.md` rows for `golden-e9e3f19d6-1080p` and `reset-50331d5`; (4) `verify.py` refuses a candidate whose backend differs
from the golden's (section 5, first item) so a GL capture cannot grade against a VK golden silently. Until then
`golden-d9e1f3801716-vk-1080p` is the VK reference GL deletion step 1 can use.
GL deletion step 1 then needs: the capture and tooling GL arms of `.claude/cleanup/gl-retirement-inventory-2026-10-15.md` sections 11-13
(`capture_ab.py` noise floor on two VK arms, `ab.py` and the term-oracle scripts kept only as long as a GL exe exists, the GL tier1 arm
dropped from the per-row proof), with this set as the regression guard: `verify.py` 36/36 on a VK run of the post-deletion exe.

### CURRENT_GOLDEN recommendation (2026-10-16, session 28): SWITCH to `golden-d9e1f3801716-vk-1080p`
Every condition above is met: (1) row 2 is proven on the close-up bookmarks `mc2_01_saved_mechday` / `mc2_10_saved_mechnight` (specular
term VK vs GL 0.11 on 26.46 by day, 0.32 on 10.21 at night once GL's whole-pixel face multiplier, now row 24, is divided out) with no VK
code change; (2) so no golden pose moves and no re-bless is needed: the final exe 67b4b029a grades 36/36 against this set (`s28e-vk`), as
d37588c7c did (`s28c-vk`, every pose within the noise floor of `s27d-vk`); (3) open rows that could move these poses: 24 (GL-only face
darkening, mechs are a few dozen px here) and 18 (no stock content), neither a VK change pending; (4) `verify.py` refuses a candidate drawn
by another backend (908e2ad90, `--allow-backend-mismatch` to override, logged; pytest `test_verify_backend.py`).
DONE 2026-10-16 (324fa2a36, section 6); the recipe below is kept as the record of what the switch commit did. The switch commit: in `scripts/smoke_lib/render_resolution.py` replace
`CURRENT_GOLDEN = "golden-e9e3f19d6-1080p"` with `CURRENT_GOLDEN = "golden-d9e1f3801716-vk-1080p"` and the comment line above it with
"the VK set blessed at the pinned size; the GL sets are retired (RETIRED.md)"; add `RETIRED.md` rows for `golden-e9e3f19d6-1080p` (GL,
superseded by the VK set) and `reset-50331d5` (GL, 4K); run `tests/smoke/test_resolution_tier.py` and
`scripts/check-capture-resolution.py --current`. After it, `verify.py --golden current` refuses GL captures, so any GL A/B goes through
`capture_ab.py` / the term oracle, not the golden.
GL deletion step 1 prerequisites: this switch; the step-1 list in `.claude/cleanup/lane2-state.md` (session 28); `capture_ab.py` noise
floor on two VK arms; the owner's fail-soft ruling (inventory section 14 question 1).

## 5. Follow-ups (not done here)
- DONE 908e2ad90: `verify.py` refuses a candidate whose backend differs from the golden's.
- DONE 2026-10-16 (session 29): `capture_ab.py` noise floor is two VK arms; the GL arm (`--gl-arm`) was deleted in session 38.
- `tests/callers.json` is stale at the lane base 06f3e7296 (`tools/callers.py --check`); regenerate at merge.

## 6. Switched 2026-10-16 (session 29) and the owner rulings

- `render_resolution.CURRENT_GOLDEN` = `golden-d9e1f3801716-vk-1080p`; `RETIRED.md` rows and `RETIRED_SETS` entries for `golden-e9e3f19d6-1080p` and
  `reset-50331d5`; the directories left the tree (tarball + sha256 at `E:/mc2-archive-2026-10/docs/gl-goldens-2026-10-16/`, extraction diffed against the tree
  before removal). `verify.py --golden current` now grades only Vulkan captures, and refuses a golden that records no backend.
- Owner ruling (2026-10-16): once GL is deleted, a failed Vulkan init shows a clear message and exits. There is no fallback to another renderer, so the
  fail-soft paths (`forceGLFallback`, the smoke `capture_backend` mismatch check, the VK present fail-soft notes in `tracy_capture_compare.py`) go with GL.
- GL deletion step 1 status: see `.claude/cleanup/gl-retirement-inventory-2026-10-15.md` section 15.
