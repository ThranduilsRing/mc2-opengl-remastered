<!-- doc-status: current | reviewed:2026-10-11 -->
# Oracle Gate — Legacy Dynamic Pipeline (mech/vehicle/turret/effects)

Regression gate for the dynamic-object render pipeline work (slices S2 TG render-path,
S4 material→SSBO, S5 HZB dynamic cull, S6 gesture table). Establishes the **baseline** that
every later slice must not regress, and splits checks into **agent-checkable** (counter-based,
verifiable headless from smoke logs) vs **user-visual** (needs eyes / screenshots).

> Step 1 of the caveman orchestration plan. No S2+ implementation until this gate exists. ✅ it now does.

## Baseline refs

| Item | Value |
|------|-------|
| Branch | `claude/nifty-mendeleev` |
| HEAD commit | `43e22401` |
| Dirty-tree snapshot (dangling) | `fe4e55e4` (`git stash create`, non-destructive — captures the exact working tree incl. the load-bearing GpuMechBatcher LOD re-register fix in `mech3d.cpp::init`) |
| Baseline capture | `tests/smoke/artifacts/2026-06-09T19-27-36/` (mission mc2_01, 30s, PASS) |

**Why a snapshot, not clean HEAD:** clean HEAD lacks the uncommitted `mech3d.cpp` GpuMechBatcher
LOD re-registration fix → *invisible mechs on any map after the first session* → would poison the
mech-paint/faction oracle. Baseline MUST be the dirty tree. Reproduce it with
`git stash apply fe4e55e4` (or just compare against the live working tree).

## Capture command (canonical)

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --keep-logs
```

- **NEVER** `--kill-existing` (taskkills concurrent mc2.exe → false `crash_silent`; run_smoke holds its own lock).
- **NEVER** run concurrent with another smoke (tier1 or otherwise) — the lock will reject the second run; if you see a stale `tests/smoke/artifacts/smoke.lock`, verify the named PID is dead before removing.
- For broad coverage use the canonical tier1 gate instead (`--tier tier1`, 5 missions mc2_01/03/10/17/24).
- Artifacts land in a fresh `tests/smoke/artifacts/<timestamp>/` (`mc2_01.log`, `report.json`, `report.md`). **No screenshots** — this path emits counters/logs only.

## Agent-checkable gate (from `mc2_01.log` + `report.json`)

All values below are the mc2_01 baseline. A later slice **PASSES** only if every one still holds.

| Oracle item | Counter (log tag) | Baseline | Pass criterion |
|-------------|-------------------|----------|----------------|
| Mech paint / team color | `[MECH_MATERIAL_GPU v1] mechs=12 mismatches=0` | mismatches=0 | **mismatches == 0** (mission-independent — covers faction coloring too) |
| Material / texture bind | `[MATERIAL_GPU v4] emitted=136 mismatches=0` | mismatches=0 | **mismatches == 0** |
| Texture resolve | `[TEX_RESOLVE v1] mismatches=0 oob=0` | 0 / 0 | **mismatches == 0 AND oob == 0** |
| Render snapshot correctness | `[RENDER_SNAPSHOT v3] count_mismatch=0 pkt_mismatch=0 meta_mismatch=0 fallback=0` | all 0 | **all four == 0, using_snapshot=1** |
| Visibility validity | `[VISIBILITY v1] mech_valid=1 sp_valid=1 view_valid=1` | valid=1 | **all _valid flags == 1** |
| Terrain parity (unrelated, must not break) | `[TERRAIN_INDIRECT_PARITY v1] total_mismatches=0` | 0 | **total_mismatches == 0** |
| Effect draw counts | `[FX_COUNT v1]` (env `MC2_FX_COUNT_LOG=1`) | see below | counts in expected band, no site drops to 0 unexpectedly |
| Stability | `report.json destroys_delta` | 0 | **== 0**, result == PASS, buckets empty |
| FPS (informational) | `report.json avg_fps` | 80.9 (window minimized, ~100fps SDL cap — NOT GPU throughput) | not a hard gate; track for gross regressions |

### Effect-count oracle — `MC2_FX_COUNT_LOG`

Added in `mclib/mech3d.cpp` (env-gated, default OFF, zero behavior change when unset). Counts
draw attempts at the **11 GOSFX `->Draw()` sites** in `Mech3DAppearance::render()`. Emits a
`[FX_COUNT v1] event=summary ...` line to stderr every 9000 ticks.

```bash
MC2_FX_COUNT_LOG=1 py -3 scripts/run_smoke.py --mission mc2_01 --duration 30 --keep-logs
```

11 sites: `rDust lDust jump lBoom rBoom critSmoke smoke wake heliDust rArmSmoke lArmSmoke`.

Emits two line types to **stdout** (NOT stderr — smoke logs capture stdout only):
- `[FX_COUNT v1] event=init flag=<0|1>` once at first `render()` (proves env reach + render runs).
- `[FX_COUNT v1] event=shutdown total=N rDust=.. lDust=.. ...` once at clean exit (atexit, mirrors `shutdownTexResolveTable`).

Implementation notes (load-bearing, learned the hard way):
- Must use `printf`/stdout, not `fprintf(stderr)` — the smoke harness captures stdout only.
- atexit is registered from inside `render()` (a guaranteed-run path), NOT a static-init object — an anonymous-namespace static with only a registration side effect can be elided by MSVC `/OPT:REF`.
- `MC2_FX_COUNT_LOG` had to be added to the `run_smoke.py` env-propagation whitelist (~line 285) — Popen replaces the child env, so non-whitelisted vars never reach `mc2.exe`.

**Short idle smoke = all zeros (expected):** the default 30s smoke is a near-idle fly-through —
mechs barely move, so no effect site triggers. Verified mc2_01 + mc2_24 (30s) + mc2_10 (60s) all
→ `total=0`. The mech-appearance sites only fire on specific states: dust=walking footfall,
jumpJet=jumping, shoulderBoom/armSmoke=arm blown off, critSmoke/smoke=mech damaged, wake=in water.
Building/terrain/weapon-impact "fires" go through Carnage/WeaponBolt, NOT these 11 sites.

**REAL non-zero baseline — mc2_17 @ 150s (mech fight starts ~2min):**
```
[FX_COUNT v1] event=shutdown total=1307 rDust=0 lDust=0 jump=0 lBoom=0 rBoom=0
              critSmoke=0 smoke=0 wake=349 heliDust=0 rArmSmoke=0 lArmSmoke=958
```
Fight blew left arms off (lArmSmoke=958) + mechs waded water (wake=349). 10593 frames, parity
intact throughout (MECH_MATERIAL_GPU mechs=34 mismatches=0, RENDER_SNAPSHOT all 0, TEX_RESOLVE 0/0).

Capture recipe for an effect baseline:
```bash
MC2_FX_COUNT_LOG=1 py -3 scripts/run_smoke.py --mission mc2_17 --duration 150 --keep-logs
# mc2_17: mech fight ~2min in (arm-blow + water effects). >2min duration required.
```
NOTE: long-duration runs (>30s) report `engine_reported_fail` when the unattended AI **loses the
mission** — this is a GAMEPLAY outcome, not a renderer regression (verify Δdestroys + parity
counters, which stay clean). Re-capture after any S1/S2/S6 effect slice; a per-site count
diverging on the same mission/duration = effect regression.

## Pixel oracle — deterministic headless screenshot (NEW)

The smoke window runs minimized, so the default framebuffer (FBO 0) reads black.
The capture reads the **offscreen post-process scene FBO** instead (always valid):

```bash
MC2_SCREENSHOT_AT_FRAME=<N> MC2_SCREENSHOT_PATH=<abs.tga> \
  py -3 scripts/run_smoke.py --mission mc2_17 --duration <s> --keep-logs
```
- Hook: gameosmain.cpp, just before SwapWindow. Reads `getGosPostProcess()->getSceneFBO()`
  (RGBA16F, full render res) via glReadPixels → TGA, once when frame counter hits N. Default OFF.
- The TGA is **pre-tonemap HDR clamped to 8-bit** → dim. For viewing, apply exposure+gamma
  (`clip(rgb*4,0,1)**(1/2.2)`), or just view straight for bright frames.
- Convert TGA→PNG (PIL) to view. `[SCREENSHOT v1] frame=N wrote ... (WxH, sceneFBO)` confirms.
- **Framing matters:** the smoke fly-through camera points wherever the mission script puts it.
  mc2_17 @ frame 600 (mission start) frames the player lance → mech + paint + props visible
  (`tests/smoke/artifacts/oracle_mc2_17_f600.tga`). Later frames (8000) drift to terrain/water.
  Pick a frame that frames the subject you're regressing.

This turns the user-visual items below into **agent-checkable pixel diffs** (capture baseline frame N,
re-capture after a slice, diff). Absolute color correctness still benefits from one human confirmation.

## User-visual items (now pixel-capturable per above)

Capture the same mission+frame before touching S2+, re-check after.

| # | Item | What to look for |
|---|------|------------------|
| 1 | Selection highlight | click a mech → correct ARGB highlight overlay; clears on deselect |
| 2 | Turret/barrel rotation | smooth interpolation to heading, no snapping/jitter |
| 3 | Effect *positions* (count is agent-checkable; placement is not) | weapon-fire muzzle origin, jump-jet at foot/leg nodes, dust at foot contact, wake at waterline |
| 4 | Mech paint / faction color | team color on mech hull (mc2_17 f600 shows a green/teal lance mech); IS vs Clan distinct |

## Headless limitations

- The smoke path produces **counters/logs only, no pixel screenshots** (the `water-diff` artifact set's PNGs were a special visual-diff run, not the default path). Agent cannot see pixels.
- `mc2-render-state` MCP `get_latest_artifact_paths` / `summarize_latest_capture` can point at a **stale** set — trust the on-disk newest `tests/smoke/artifacts/<timestamp>/` with a `report.json`.
- `run_capture_baseline` MCP tool self-caps at 150s; a 30s mission + build/launch overhead exceeds it and the underlying `run_smoke.py` + `mc2.exe` keep running orphaned after the MCP gives up. Prefer launching `run_smoke.py` directly via shell for captures.
- FPS from minimized-window runs reflects the SDL ~100fps minimized cap, not GPU throughput — never read it as a perf result.

## Hard rule

No S2 (TG render-path) / S4 / S5 / S6 implementation lands without re-running this gate and
confirming **all agent-checkable criteria hold** + the relevant user-visual items eyeballed.
