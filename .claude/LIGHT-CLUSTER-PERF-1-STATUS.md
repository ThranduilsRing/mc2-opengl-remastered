# LIGHT-CLUSTER-PERF-1 — status

Lane: `LIGHT-CLUSTER-PERF-1` · worktree `A:/Games/mc2-main` · branch `claude/nifty-mendeleev`
Commits: `93c22e39` (the slice), `cadf9a92` (smoke-allowlist bug found en route)
Deploy lane: `0.4c` → `A:/Games/mc2-opengl/mc2-win64-v0.4c`

## Verdict up front

The **implementation is complete, landed, deployed and gated**, and its mechanism
is measured and working. The **frame-time table the mission asked for is NOT
delivered**, because the available measurement harness cannot reproduce the
regression it was supposed to measure. Details in "Measurement — honest
failure" below. Do not let the green checkmarks above that section imply the
perf claim was proven end-to-end; it was not.

## Meta review — premises checked against reader source

| Premise (as briefed) | Verdict | Source |
|---|---|---|
| Cap raised 64 → 256, default-ON | CONFIRMED | `gos_terrain_lighting.cpp` `defaultCap = 256u`, commit `5213d257` |
| Pool is an unbounded SSBO, no uniform-block limit | CONFIRMED | `terrain_lod_chunk.frag` binding 28 `PoolLight poolLights[]` |
| Driver is the per-fragment loop, not storage | CONFIRMED structurally | `for (i = 0; i < u_numPoolLights; ++i)` runs per fragment |
| mc2_10 wants 142 fixtures | **UNDERSTATED** | live receipt shows **187–201** packed at the busy night pose |
| `MC2_LIGHT_POOL_MAX` is a proper gate | **FALSE** | was unregistered + undocumented; fixed in this slice |

## What shipped

World-space XY **light grid**. `BuildAndUploadLightGrid` (producer, after the
pack loop) stamps each packed light into every grid cell its ground circle
overlaps, uploading a per-cell `{firstIndex,count}` header (binding 29) and a
cell-major index list (binding 30). `terrain_lod_chunk.frag` maps
`v_worldPos.xy` to one cell and iterates only that cell's lights.

**Exact, not approximate — no pixel tolerance is claimed or needed.** A light
contributes only where `len < farDistance`, i.e. only inside its own ground
circle, and the producer stamps that same circle using the **unscaled**
`farDistance`. The shader's mover-radius shrink (`u_lightPoolMoverScale <= 1`)
only ever *reduces* reach, so each cell list is a conservative superset of the
actual contributors. Fragments outside the grid bbox lie outside every circle,
so the empty range they receive would have summed exactly zero anyway.

Gates: `MC2_LIGHT_GRID` (default ON; `=0` uploads `u_lightGridDim=0` and the frag
takes the original linear loop with direct indexing — the killswitch contract).
`MC2_LIGHT_GRID_DIM` tunes cells/axis (default 64). Inert below 8 packed lights
and whenever `MC2_TERRAIN_LIGHT_POOLS=0`.

Scope: only the LOD-chunk terrain consumer is wired. The overlay / mech /
static-prop pool consumers have no `u_lightGridDim`, so `glGetUniformLocation`
returns -1 and they keep the linear loop, unchanged.

## Mechanism measurement — this part IS solid

Live receipts, mc2_10, canonical busy night pose
(`MC2_CAMERA_PARK=-6656,-6656,150.625,10.9094,6045.2358@600`, confirmed applied
by a `[CAMERA_PARK v1]` line):

```
[LIGHT_POOL_CAP v1] peak_wanted=201 packed=201 cap=256 dropped=0
[LIGHT_GRID   v1] dim=64 lights=187 stamped=2027 peak_cell=17 avg_cell=0.49
[LIGHT_GRID   v1] dim=64 lights=193 stamped=2084 peak_cell=18 avg_cell=0.51
[LIGHT_GRID   v1] dim=64 lights=187 stamped=2331 peak_cell=19 avg_cell=0.57
[LIGHT_GRID   v1] dim=64 lights=121 stamped=4656 peak_cell=20 avg_cell=1.14
```

Per-fragment loop iterations at the busiest night pose:

| | before | after |
|---|---|---|
| worst-case fragment | ~200 | **≤ 20** (10x fewer) |
| average fragment | ~200 | **< 1.2** |

That is the quantity the 34% cost is made of, and it is down by an order of
magnitude in the worst case. It is direct evidence the optimization does what it
was designed to do — but it is *not* a frame-time measurement, and I am not
going to present it as one.

## Measurement — honest failure

`tools/tracy_capture_compare.py` hard-codes `MC2_SMOKE_MODE=1`, which starts the
game window minimized. `gameosmain.cpp:1795` then throttles the loop with a
10 ms sleep per frame whenever the window is minimized or hidden. In that
configuration a fragment-bound cost is not observable.

The proof that the harness is unfit for this particular question is in its own
output: across a full 3-arm x 3-pose matrix, the **cap=64 and cap=256 arms were
indistinguishable** (night-busy p50 2.590 ms vs 2.574 ms). If the harness could
see the regression, those two arms would differ by the briefed ~34%. It cannot,
so its verdict on my fix (2.623 ms) is equally meaningless. Raw data kept at
`<scratchpad>/perf/results.json` — **do not cite it**; it is recorded only as
evidence of the harness limitation.

Also note the numbers are `DrawScreen` *zone* durations, not whole-frame times,
and ~2.6 ms / 386 FPS is 4-6x faster than the briefed 9.81 ms baseline — another
independent sign the measured configuration is not the briefed one.

**To finish this properly** someone needs a capture path that runs a visible,
non-minimized window at real resolution. That is a harness change
(`tracy_capture_compare.py` needs a `--show-window` equivalent, as
`run_visual_capture.py` already has), not a change to this slice.

## Bugs found en route (both fixed here)

1. **`MC2_CAMERA_PARK` was never in `run_smoke.py`'s passthrough allowlist.**
   Every pose-pinned smoke run through `run_smoke.py` silently measured the
   *default* camera instead of the requested pose. The runner's own `[ENV-DROP]`
   warning caught it. **Any prior pose-pinned evidence gathered via
   `run_smoke.py` is suspect** and should be re-checked. Fixed in `cadf9a92`.
2. **`MC2_LIGHT_POOL_MAX` shipped unregistered and undocumented** in `5213d257`.
   Registered here, and allowlisted so a cap=64 baseline arm is reproducible.

## Gates run

- contracts **67/67** (`check-gpu-buffer-owners` correctly caught both new SSBOs
  as un-owned on the first pass; resolved by giving them real `GpuBufferOwner`
  identity + `RenderResourceId` 48/49, not an exclusion tag)
- harnesses **16/16**
- tier1 **5/5 PASS**, `DEPLOY_FINGERPRINT OK` on all five (exe `04ac21e32ea4`)
- `check-env-registry.sh` PASS; both new gates registered + smoke-allowlisted
- deployed exe md5-verified against `build64/RelWithDebInfo/mc2.exe`; gate string
  `MC2_LIGHT_GRID` confirmed present in the deployed binary

## NOT done — required before this can be called finished

- **Frame-time table** (p50/p99, 3 poses x 3 arms) — blocked on the harness gap above.
- **Visual verification.** No captures were taken. The correctness argument is
  exact-by-construction and the mechanism receipts are consistent with it, but
  the standing capture rule (wide shots, ≥3 locations/map, day+night, across
  mc2_01/03/05/10/24, mc2_05's user-approved night lighting proven unchanged) has
  **not** been satisfied. `MC2_LIGHT_GRID=0` is the killswitch if anything looks
  wrong.
- Therefore: **whether `MC2_LIGHT_REALISM` becomes affordable is still unanswered.**
  The loop-iteration collapse (200 → ≤20) is strong reason to expect the ~34% is
  largely recovered, but "expect" is not "measured", and that gate is the user's
  call on evidence I did not produce.

## Lane hygiene

`shaders/terrain_lod_chunk.frag` is claimed by three other lanes
(CEMENT-ALPHA-LAND-1, CEMENT-UNIFORM-2, ROAD-OVERLAY-LIGHT-1). All were stale
with no in-tree edits at the time of writing, and this slice's edits are confined
to the pool-loop region — disjoint from cement/road-overlay work. Other lanes
committed to `claude/nifty-mendeleev` throughout this session; `93c22e39` and
`cadf9a92` are intact ancestors of HEAD.
