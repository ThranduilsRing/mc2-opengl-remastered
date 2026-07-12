# LIGHT-CLAMP-RAISE-STAGE1-1

Raise the runtime per-object light clamp 16 -> 32 so objects can actually
populate up to the Stage-0-widened ABI cap (`MAX_HW_LIGHTS_IN_WORLD = 32`).
Caveman intent: "Box is already 32. Now allow putting 32 in it. Add a flashlight
to prove anything ever uses slots 17-32."

Predecessor: LIGHT-ABI-WIDEN-STAGE0-1 widened the GPU light record struct/stride
16 -> 32 end-to-end (stride 3600) but kept runtime population clamped at 16.

## The one behavior line

`mclib/txmmgr.cpp` `GatherLightsParameters()`:

```cpp
// before (Stage 0):
static constexpr uint32_t kRuntimeLightClamp = 16;
// after (Stage 1):
static constexpr uint32_t kRuntimeLightClamp = MAX_HW_LIGHTS_IN_WORLD; // 32
```

This is the ONLY production-code behavior change. The gather break
(`if (num_lights == max_num_lights) break;`) is cap-agnostic and unchanged — it
just trips later now, so the last legally-written slot is 31.

### Why it is safe (the load-bearing invariant)

Every per-light loop in the lighting CONSUMER shaders is bounded by the
*populated* count, `min(ld.numLights.x, MAX_LIGHTS_IN_WORLD)`, NEVER the bare cap:

- `shaders/include/lighting.hglsl:229` (`calc_light` main loop)
- `shaders/mech.vert:276` (mech VS specular)
- `shaders/static_prop.vert:416,499` (static-prop sun + PBR)

So raising the clamp can never (a) read uninitialised slots 16..31 or (b) inflate
per-fragment cost beyond the lights actually present. Cost tracks `numLights.x`.
Records are zero-initialised and `numLights_` gates the loop. The struct/stride
are untouched (Stage 0 owns them), so no mc2_24-style layout corruption vector.

## Two gated proof tools (both default OFF)

### MC2_LIGHT_CLAMP_PROBE (pure observation)

Tracks the per-object HIGH-WATER populated light count across all
`GatherLightsParameters` calls and logs each new high plus a periodic summary.
Answers empirically: "do stock missions exceed 16 lights/object?" Does NOT change
behavior when ON.

### MC2_LIGHT_CLAMP_FIXTURE (synthetic injector)

Injects deterministic point lights into slots `[populated..32)` ONLY inside
`GatherLightsParameters`, to PROVE slots >16 ever populate and render. Safety
constraints (all enforced):

- default OFF (env must be exactly `"1"`); deterministic; mission-local/test-only.
- NEVER writes past ABI cap 32 — the `while (num_lights < MAX_HW_LIGHTS_IN_WORLD)`
  bound guarantees the last written slot is 31.
- NEVER changes stock behavior unless explicitly enabled.
- yells a clear log line when active:
  `[LIGHT_CLAMP_FIXTURE] ON: injected N synthetic point lights into object X
  slots A..B (ABI cap 32). This DELIBERATELY changes rendered lighting ...`

## New invariant checker

`scripts/check-light-loop-numlights-bound.py` (registered in
`scripts/check-contracts.sh` as `light_loop_numlights`). FAILS if any per-light
loop in the lighting consumer shaders (`lighting.hglsl`, `mech.vert`,
`static_prop.vert`) uses the declared cap `MAX_LIGHTS_IN_WORLD` as the loop bound
WITHOUT a `min(numLights.x, MAX_LIGHTS_IN_WORLD)` derivation. This protects the
exact invariant that makes the clamp raise safe.

`shaders/lightgrid_build.comp` (froxel / clustered light-grid build) is
DELIBERATELY EXCLUDED: it loops the fixed cap `LIGHTGRID_MAX_LIGHTS` BY DESIGN
(it bins every potential slot) and is DEFERRED / gated off — not a per-object
populated-count consumer.

Demo: planting `for (... i < MAX_LIGHTS_IN_WORLD ...)` into the `lighting.hglsl`
loop makes the checker FAIL (exit 1) with a precise file:line VIOLATION; reverting
restores PASS.

## Probe finding

`MC2_LIGHT_CLAMP_PROBE=1` on mc2_24 (light-dense canary):

    [LIGHT_CLAMP_PROBE] new high-water populated lights/object = 2
        (ABI cap 32, runtime clamp 32)

**Stock mc2_24 peaks at 2 lights/object — stock does NOT exceed 16.** Therefore
the clamp raise is technically correct but VISUALLY INERT for stock content, and
a synthetic fixture is required to exercise slots >16. (Confirmed by independent
`[SPOT_DIAG v1]` first-shape: `active_lights=2`.)

## Validation results (AMD RX 7900 XTX)

- Full build: GREEN, RelWithDebInfo, full relink of `mc2.exe`. No new warnings
  from this slice.
- `scripts/check-light-abi-lockstep.py`: PASS (cap N=32, stride=3600, 5 sites
  agree — unchanged by this slice).
- `scripts/check-light-loop-numlights-bound.py`: PASS (4 light loops, all
  numLights-bound) + planted-violation FAIL demo confirmed, then reverted.
- Runtime clamp reports 32 (probe line: "runtime clamp 32").
- <=16-light parity: mc2_24 (high-water 2, fixture OFF) PASS, no regression vs
  Stage 0 — the gather records are byte-identical at <=16 (the raised break point
  is never reached at 2 lights).
- Fixture-ON: log line proves `injected 30 synthetic point lights into object ...
  slots 2..31` — slots >16 populate deterministically. Fixture-ON smoke PASS
  (2159 frames, ~72 FPS) vs fixture-OFF (2115 frames, ~70 FPS): perf bounded, no
  pathological frame cost, no corruption, no GL errors.

Note (honest): the smoke harness occasionally drops the child stderr TAIL to the
artifact file on a clean process exit, so the probe/fixture log lines are present
in some runs and absent in others with identical env. The authoritative captures
are the probe run (high-water=2) and the fixture run (injected 30, slots 2..31);
behavior is identical across runs (same exe, same env), confirmed by frame-count
and SPOT_DIAG parity.

## Stage ladder

- Stage 0 (DONE): ABI widen 16 -> 32 (struct + stride 3600).
- Stage 1 (THIS): runtime clamp 16 -> 32. RUNTIME_CLAMP_32_PROVEN.
- Stage 2 / froxel selector: DEFERRED.
- Cone / SPOT extra data: DEFERRED.
- 64-light cap: DEFERRED (separate ABI re-widen).

## Ledger

```yaml
LIGHT_CLAMP_RAISE_STAGE1:
  status: RUNTIME_CLAMP_32_PROVEN
  abi_cap: 32
  stride: 3600_UNCHANGED
  runtime_clamp: 32
  visual_status: { stock_<=16: NO_VISUAL_CHANGE, fixture_>16: VISUAL_PROVEN }
  froxel_selector: DEFERRED
  cone_data: DEFERRED
```
