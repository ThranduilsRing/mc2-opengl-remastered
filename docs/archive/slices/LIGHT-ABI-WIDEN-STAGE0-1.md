# LIGHT-ABI-WIDEN-STAGE0-1 — widen the per-object GPU light record 16 -> 32

**Status:** ABI_WIDENED_PARITY_PROVEN. Pure ABI widening. NOT a lighting feature.
The box got bigger; we still only put 16 things in it.

## The macro trap (state it LOUDLY)

```
MAX_LIGHTS_IN_WORLD in tgl.h is the global light pool.
MAX_LIGHTS_IN_WORLD in lighting.hglsl is the per-object GPU ABI cap.
They are not the same concept.
```

- `mclib/tgl.h  MAX_LIGHTS_IN_WORLD = 1024` — the GLOBAL scene light pool
  (`camera.worldLights[1024]`). NOT touched by this slice. NOT the ABI cap.
- `shaders/include/lighting.hglsl  MAX_LIGHTS_IN_WORLD` — the PER-OBJECT GPU
  ABI cap (record inner-array size). This is what widened 16 -> 32.
- `mclib/tgl.h  MAX_HW_LIGHTS_IN_WORLD` — the C++ mirror of the per-object ABI
  cap. Must equal the GLSL `MAX_LIGHTS_IN_WORLD`. Also widened 16 -> 32.

Do NOT rename the two same-named macros. They are intentionally distinct
concepts that happen to share a name across two translation domains.

## What this slice does / does not do

- DOES: widen the per-object GPU light record (`ObjectLights` / `TG_HWLightsData`)
  inner arrays from 16 to 32 slots. This changes the SSBO record STRIDE
  (1808 -> 3600 bytes). All five lockstep sites change atomically.
- DOES NOT: raise the runtime light population. `GatherLightsParameters`
  (`mclib/txmmgr.cpp`) still fills at most 16 lights per object — hardcoded
  `kRuntimeLightClamp = 16`, deliberately decoupled from `MAX_HW_LIGHTS_IN_WORLD`
  so widening the macro does NOT lift the clamp. Slots 16..31 are always zero
  this slice. Zero rendered behavior change.
- DOES NOT: invent SPOT cone data, wire a froxel consumer, change the global
  pool, or refactor lighting.

## The new stride

```
record stride = N*112 + 16   (mat4=64 + dir 16 + color 16 + falloff 16 per light, + ivec4 tail 16)
N = 16 -> 1808 bytes (old)
N = 32 -> 3600 bytes (new)
```

The C++ `static_assert(sizeof(TG_HWLightsData) == 3600)` proves the size; the
GLSL std430 layout matches byte-for-byte (every member is mat4/vec4-array +
ivec4 tail — no scalar/vec2/vec3 straddle, so std430 == std140 here).

## The 5 lockstep sites (all-or-nothing — drift = silent corruption)

| # | Site | Symbol | New value |
|---|---|---|---|
| 1 | `shaders/include/lighting.hglsl` | `#define MAX_LIGHTS_IN_WORLD` | `32` |
| 2 | `mclib/tgl.h` | `#define MAX_HW_LIGHTS_IN_WORLD` | `32` |
| 3 | `mclib/tgl.h` | `static_assert(sizeof(TG_HWLightsData) == ...)` | `3600` |
| 4 | `GameOS/gameos/gameos_graphics.cpp` | `kLightRecordStride` | `3600` |
| 5 | `shaders/lightgrid_build.comp` | `#define LIGHTGRID_MAX_LIGHTS` | `32` |

Site 4 is the sneakiest: `gameos_graphics.cpp` cannot include `tgl.h`, so the
stride is a hand-copied literal. The documented mc2_24 regression (2026-05-02)
was caused by exactly this class of drift (one side widened, other forgotten).

## The tripwire

`scripts/check-light-abi-lockstep.py` (registered in `scripts/check-contracts.sh`)
greps all five sites, derives the expected stride from the cap (`N*112+16`), and
FAILS (nonzero exit) if any cap site disagrees or any stride site disagrees with
the derived value. It is the CI-time guard for the "edit one site, forget the
others" failure mode — in particular catches a stale `kLightRecordStride`.

Proven: PASS on the consistent tree; FAIL on a planted desync of the
`kLightRecordStride` literal (reverted after the demo).

## Parity proof

- Decoded values for the 16 POPULATED records (`light_to_world[0..15]`,
  `light_dir[0..15]`, `light_color[0..15]`, `light_falloff[0..15]`) are
  unchanged — same `GatherLightsParameters` fills the same first 16 slots; the
  widening only appends zeroed slots 16..31.
- Rendered frames are byte-identical before vs after (tier1 visual no-change).
  Raw record bytes are NOT offset-identical (inline-array widening shifts the
  later arrays' byte offsets), so SEMANTIC parity is the proof, and
  byte-identical frames are the "first 16 did not move" acceptance.

## Stage ladder

- **Stage 0 (this slice): ABI widen, runtime clamp stays 16.** DONE.
- **Stage 1: LIGHT-CLAMP-RAISE-STAGE1-1 — raise the runtime clamp** (bump
  `kRuntimeLightClamp` in `txmmgr.cpp` toward 32, gated for A/B). GO later. This
  is the first stage with an intended visual delta (dense-night missions get
  >16 contributing lights). Validate via visual A/B, not byte-identity.
- **Froxel selector: DEFERRED.** Per-tile light index pool from
  `lightgrid_build.comp` stage 1 as the selector (replaces per-object best-N).
  Separate milestone; needs additive SPOT cone data. Not this ladder rung.

## Ledger

```
LIGHT_ABI_WIDEN_STAGE0:
  status: ABI_WIDENED_PARITY_PROVEN
  per_object_cap_abi: 32
  runtime_clamp: 16_UNCHANGED
  visual_status: NO_VISUAL_CHANGE
  lockstep_sites: 5_AGREE_TRIPWIRE_ADDED
  next: LIGHT-CLAMP-RAISE-STAGE1-1 (GO later)
```
