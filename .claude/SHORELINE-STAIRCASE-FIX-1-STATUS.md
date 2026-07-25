# SHORELINE-STAIRCASE-FIX-1 — STATUS (master-FAIL #1, CRITICAL)

**Lane:** shoreline-staircase-1 · worktree `A:/Games/mc2-main` · branch
`claude/nifty-mendeleev` · deploy lane `0.5.1`.
**Result:** SHIPPED (local commits only, NOT pushed). Killswitch-gated,
default-OFF byte-identical. Mechanism proven by exaggeration + eyeballed at
full zoom on two maps + the shore-staircase sentinel region.

## The defect
The **water-body outline** snaps to the terrain tile grid — ~140 px
axis-aligned steps with dead-straight (±1 px) risers at the mc2_10 top-down
"worst water" pose (`verify_verdicts/mc2_10_saved_204523/VERDICT.md`, region
`shore_upperright_inlet`, severity critical). Root cause: the water FS shore
alpha ramp `shoreBlend = max(alphaDepth, 1)` is only ~`alphaDepth` world-units
wide → a 1-2 px transition at top-down altitude that reads as a hard tile edge.

## Why NOT the cooked shoreline_mask SDF sidecar
`tools/terrain_beautify/cook_shoreline.py` header documents the **V3 demotion**:
a cooked horizontal-distance EDT never equals the *rendered* bilinear waterline,
so it either staircases (faithful source) or floats up-slope (smoothed source).
mc2_10 also has **no shoreline_mask sidecar in any lane** (mission_facts) and is
FST-archived. The already-wired `MC2_WATER_SHORE_MASK` path (`u_hasShoreMask`)
is therefore inert for mc2_10 — that's why the prior in-flight fix left the
staircase unchanged (VERDICT A==B). Cooking a mask would reproduce a known
dead-end. So the fix hugs the *rendered* waterline in-shader instead.

## The fix — MC2_WATER_SHORE_FEATHER (wide alpha feather)
The gate sets a **floor (world units) on the effective `alphaDepth`**. Key
identity that makes this one knob: the beach-ring geometry extent
`shoreExt = alphaDepth*0.5` **exactly equals** the shader shore band's land-side
extent `shoreBlend*0.5`, so raising the floor widens the FS shore smoothstep AND
the beach geometry in lockstep, preserving every existing invariant. The band is
driven per-fragment by `WaterThickness = waterElevation − interpolated terrain
z`, so it **hugs the rendered waterline by construction** and auto-widens
horizontally on flat shores (worst staircase), stays tight on cliffs.

`EffAlphaDepth(base) = floor>0 ? max(base, floor) : base` (default OFF → identity
→ byte-identical). Applied in lockstep across every water-build predicate:
- FS shore band: widened `alphaDepth` passed to the water bridge
  (`mclib/terrain.cpp` renderWaterFast call) — **no `gameos_graphics.cpp` edit**
  (that file is held by lane DEFERRED-BATCH-8A; the bridge just forwards its arg).
- Beach-ring `shoreExt`: CPU thin-record build + GPU-driven compute
  (`gos_terrain_water_stream.cpp`) and the narrow-append predicates
  (`mclib/terrain.cpp`), all via `EffAlphaDepth`.
- Parity `pu.alphaDepth` kept consistent so `CheckParityFrame` doesn't false-flag.

## Commits (local, NOT pushed)
- `a8ea6726` — fix(water): SHORELINE-STAIRCASE-1 — wide shore feather. Files:
  `GameOS/gameos/gos_terrain_water_stream.{h,cpp}`, `mclib/terrain.cpp`,
  `scripts/check-env-registry.sh`, `scripts/run_smoke.py`,
  `docs/tier1_env_vars.md`. (+63/−10, 6 files.)
- `ea93c1ad` — docs: recommended feather = 20 wu (evidence-based). Doc-only;
  does NOT change the binary (deployed exe stays `a8ea6726`).

## Gate
`MC2_WATER_SHORE_FEATHER` — verdict **OK** (gate_status): documented
(tier1_env_vars.md), smoke-allowlisted (run_smoke.py), registered
(check-env-registry.sh). Default unset/≤0 = OFF = byte-identical.
Recommended live value **20 wu** (taste range 16-28). Stays OFF in the deployed
install — enabling it into the launch config is a coordinator/user taste call.

## Verification (verify-by-exaggeration, FULL-ZOOM crops, two maps + sentinel)
Deployed `a8ea6726` (dirty-fingerprint — see caveat) to lane 0.5.1. Poses:
mc2_10_saved_204523 (primary "worst water" top-down) + mc2_01_saved_205644
(second-map shoreline). Golden = `tests/visual/goldens/v0.5.1-baseline` (encodes
the *pre-fix staircase*, so any real fix must diverge from it at the shore).

| Condition | vs golden | shore_upperright_inlet (critical sentinel) | Eyeball |
|---|---|---|---|
| Control (gate OFF) | **11/11 regions PASS** | matches golden | default byte-identical ✓ |
| Exaggerate =80 | 7 FAIL | changed 93.1%, struct 12.5 | hard 90° step **fully dissolved** (over-washed, as expected) |
| Final =32 | 7 FAIL | changed 71.3%, struct 6.4 | staircase gone; flat mc2_01 shore a bit washy |
| **Final =20** | 7 FAIL | changed 58.5%, **struct 3.36 (< 6.0 sentinel threshold)** | risers diffused, landmass still reads; natural soft shore both maps |

"FAIL vs golden" at the shore is the **desired** signal — the golden IS the
staircase; the control PASS proves default-off safety, the shore FAILs prove the
fix moved the exact defect pixels. At 20 the block-staircase *structural*
signature (the sentinel's `struct` metric — its defining feature) drops below the
sentinel's fail threshold.

Evidence paths (crops are candidate/golden pairs, full-res):
- `…/scratchpad/vv_control/` (report.md exit 0, 11 PASS)
- `…/scratchpad/vv_exag/` (feather=80) `contact_sheets/mc2_10_saved_204523/shore_upperright_inlet__{candidate,golden}.png`
- `…/scratchpad/vv_final32/`, `…/scratchpad/vv_final20/` (report.md + contact_sheets, both maps)
  (scratchpad root: `C:/Users/Joe/AppData/Local/Temp/claude/A--Games-mc2-opengl-src/f4bbf866-.../scratchpad/`)

## Residual risk / follow-ups
1. **Dirty-fingerprint deploy.** The exe embeds sha `a8ea6726` (correct) but
   MC2_BUILD_GIT_DIRTY=1 because the worktree carries ~174 pre-existing unrelated
   dirty files (not mine — my 6 files are committed). Exact source state is the
   commit + ambient dirt. For a release re-cut, rebuild from a clean tree.
2. **Vulkan backend not wired.** `vulkan_water_island.cpp` reads `alphaDepth`
   via its own push-constant source; the gate currently affects the **GL path
   only** (the verified backend). VK stays byte-identical (gate default-OFF); to
   extend, route the VK push-constant `alphaDepth` through `EffAlphaDepth`.
3. **Wide-feather is a soft-edge trade** (approved candidate). The band's *screen*
   width scales with shore slope, so very flat shores read softer at the same wu.
   The final value 20 is a starting taste point; the user should confirm live.
4. **Deployed install gate is OFF** → the release still shows the staircase until
   `MC2_WATER_SHORE_FEATHER=20` is set. Bake into launch config = coordinator call.
5. Not pushed. Not merged. tier1 smoke not re-run (doc/data unchanged binary;
   control capture already proved default-off render integrity across mc2_01+mc2_10).
