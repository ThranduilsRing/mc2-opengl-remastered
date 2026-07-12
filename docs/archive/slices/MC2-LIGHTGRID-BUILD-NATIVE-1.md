# MC2-LIGHTGRID-BUILD-NATIVE-1

MC2-native, clean-room GPU **light-bin grid builder**. Stacks on
CLUSTER-DEPTH-PYRAMID-NATIVE-1 (the per-tile min/max depth substrate). **INERT
infrastructure**: builds and CPU-verifies a per-tile light-index grid; there is
**no shading consumer** and **zero visual change**. Mirrors the depth-pyramid
slice's build+parity+plant pattern.

```
LIGHTGRID_BUILD_NATIVE:
  status: INFRA_PARITY_PROVEN
  visual_status: NO_VISUAL_CHANGE
  gate_default: OFF
  consumer_status: NO_SHADING_CONSUMER
  cull_geometry: SPHERE_ONLY
  cone_culling: DEFERRED_NO_SOURCE_DATA
  light_cap: 16_UNCHANGED
```

## Files

| File | Role |
|---|---|
| `GameOS/gameos/gos_lightgrid_build.{h,cpp}` | Gate trio, GPU buffers/programs, sphere+grid dispatch, CPU parity ref |
| `shaders/lightgrid_build.comp` | Two compute stages in one file (selected by `LIGHTGRID_STAGE` macro) |
| `GameOS/gameos/gos_postprocess.cpp` | ONE frame hook in `endScene()` + Shutdown |
| `GameOS/gameos/gos_cluster_depth_pyramid.{h,cpp}` | +4 read-only accessors (`TileTexture/TileGridW/TileGridH/TileSize`) so the builder consumes the pyramid |
| `GameOS/gameos/gos_gpu_sync.cpp` | Explicit `ComputeShader→BufferReadback` typed edge (self-documenting) |
| `GameOS/gameos/CMakeLists.txt` | Registers the new TU |
| `docs/tier1_env_vars.md` | Gate documentation |

## Frame hook (single wiring point)

`gosPostProcess::endScene()`, immediately **after** `cluster_depth_pyramid::Run`
(depth → pyramid → lightgrid build) and after the per-frame ObjectLights SSBO
upload (binding 20):

```cpp
lightgrid_build::Run(inverseViewProj_, width_, height_);
```

## Gates (default OFF, `envFlagDefaultOff`)

| Gate | Requires | Effect |
|---|---|---|
| `MC2_LIGHTGRID_BUILD` | — | Master. OFF = **true no-op, byte-identical** (nothing allocated/dispatched). Also requires `MC2_CLUSTER_DEPTH_PYRAMID=1` (no depth pyramid tile texture ⇒ pass skips with one warning). |
| `MC2_LIGHTGRID_VERIFY` | master | One-shot CPU-vs-GPU parity check. Prefers the first frame with `numLights>0` (waits ≤1200 frames) so the cull runs against a real sphere; falls back to the empty case. |
| `MC2_LIGHTGRID_PLANT` | verify | Negative self-test: corrupts one CPU reference tile's membership set so the comparison **must** report FAIL. |

## Cull geometry — `MC2LightCullSphere` (std430, 32 B)

The core new data. Derived from `ObjectLights` (binding 20) by the GPU sphere
stage — the grid builder is **never** fed raw `ObjectLights` (both reference
implementations pre-bake cull geometry for a reason).

```glsl
struct MC2LightCullSphere {   // 32 bytes, no vec4 straddle
    vec4 centerRadius;        // 0:  xyz = world center, w = radius
    uint lightIndex;          // 16: index back into ObjectLights
    uint type;                // 20: light_dir.w (POINT/SPOT both bin as sphere)
    uint pad0;                // 24
    uint pad1;                // 28
};
```

- **center** = `light_to_world[i][3].xyz` (std430 mat4 column 3 = translation).
- **radius** = `light_falloff[i].y` (far distance).
- **type**   = `light_dir[i].w`.
- **SPHERE ONLY.** POINT and SPOT are both binned as spheres. MC2 stores **no
  cone half-angle** for SPOT (the GLSL SPOT path reuses POINT math), so **cone
  culling is DEFERRED — no source data.** No new SPOT cone field is introduced.
- Inactive slots (index ≥ active count) get `radius = 0`; the grid stage rejects
  them via its `r > 0.0` test, so **no per-frame CPU readback of the active
  count is needed** (an earlier per-frame `glGetBufferSubData` here caused a
  mc2_24 heartbeat freeze — removed).

## Grid Z-bin choice — **NZ = 1** (single Z interval per tile)

Justification: this lane is build+validate infra at a **16-light cap**. One
near/far interval per tile, taken from the depth pyramid, is sufficient to
exercise and parity-prove the full sphere/frustum/depth cull and the LDS append
without the cost/complexity of froxel Z-slicing. The builder is **tile/froxel-
neutral**: lifting NZ later only adds an outer Z loop + a wider header. Froxel
Z-bins, volumetric fog, and the cap lift are explicitly out of scope.

## Append pattern — BT-shaped (LDS stage + single reserve)

One workgroup == one screen tile (local size = cap = 16; threads stride
spheres). Survivors are staged in LDS via an LDS atomic counter, then thread 0
does **ONE** global `atomicAdd` reserve into a global cursor, the survivors are
compact-written to a global light-index pool, and a per-tile `(offset,count)`
RG32UI header image is stored. This is the AMD-friendly stage-then-single-reserve
discipline — **NOT** a per-candidate global-atomic storm.

### Output buffers
- Global compact **light-index pool** SSBO (`uint[nTiles*cap]`).
- Per-tile **(offset,count) header** `image2D` RG32UI.
- Single-uint **global allocation cursor** SSBO (reset to 0 each frame).
- Plus the intermediate **sphere pool** SSBO + a one-uint **sphere count**.

## Reversed-Z handling (load-bearing — shader + checker + this doc)

MC2 renders reversed-Z (`glClipControl(GL_ZERO_TO_ONE)`; near ≈ 1, far ≈ 0). The
depth pyramid stores raw extents: **R = numeric MIN, G = numeric MAX**. Therefore
the **NEAREST** surface in a tile is the numeric **MAX (channel G)** and the
**FARTHEST** is the numeric **MIN (channel R)**. Both the GLSL grid stage and the
C++ CPU reference read **near = MAX(G), far = MIN(R)**. World-space frustum
corners are reconstructed by `u_invViewProj * vec4(ndc, depth, 1.0)` — depth is
passed **directly as clip z** (no `[-1,1]` remap) to match the engine's
`edge_fog.frag`/`fog_oob.frag` unprojection, and `inverseViewProj_` is uploaded
`GL_FALSE` (the transpose convention every post pass uses).

## Sync (typed edges, no raw `glMemoryBarrier`)

All ordering via `gpuSyncBarrier(...)`:
- `ComputeShader→ComputeShader` — sphere stage writes → grid stage reads.
- `ClearBuffer→ComputeShader` — cursor reset → grid reservation.
- `ComputeImageWrite→TextureSample` — depth pyramid imageStore → grid sample.
- `ComputeImageWrite→TextureReadback` — grid header → CPU readback (verify).
- `ComputeShader→BufferReadback` — index pool → CPU readback (verify). Added as
  an explicit self-documenting arm in `barrierBitsFor` (`GL_BUFFER_UPDATE_BARRIER_BIT`).

`grep glMemoryBarrier gos_lightgrid_build.cpp shaders/lightgrid_build.comp` ⇒ none.

## Parity model

GPU grid vs CPU reference compare **per-tile (count, membership SET)** — NOT pool
offsets. The global atomic reserve order across tiles is GPU-schedule-
nondeterministic, so offsets are not a stable parity surface; the light **set**
binned to each tile is deterministic and is what a shading consumer would read.
The CPU reference reads back the **GPU-built** sphere buffer (same records the
GPU consumed), the depth pyramid, the grid header, and the index pool, then
recomputes the binning with byte-identical math. **No float tolerance is needed**
— count and set membership are exact integer comparisons.

## Verification evidence (AMD RX 7900 XTX, RelWithDebInfo, deploy `0.4c`)

- **Build:** green (full relink; new TU compiled clean).
- **Gate-OFF tier1:** 5/5 PASS — byte-identical no-op.
- **Gate-ON tier1** (`MC2_GL_DEBUG_FATAL=1`): 5/5 PASS, no GL errors, visually inert.
- **CPU/GPU parity:** all 5 tier1 missions `PARITY PASS tiles=475 lights=1
  mismatches=0 worst_count_delta=0` — 0 mismatches, no tolerance.
- **PLANT (runtime-captured, mc2_24):**
  ```
  [LIGHTGRID_BUILD v1] PLANT: corrupted CPU tile 0 membership set (expecting a mismatch below)
  [LIGHTGRID_BUILD v1] PARITY FAIL tiles=475 lights=1 mismatches=1 worst_count_delta=1 plant=1 (planted-error expects FAIL)
  ```
- **No raw `glMemoryBarrier`** in new code; typed edges added.
- **No ObjectLights ABI break, no 16-cap lift, no cone data.**

## Out of scope (explicitly NOT built)

Lighting/shading consumer · any visual change · 16-cap lift
(`MAX_LIGHTS_IN_WORLD` stays 16) · cone culling / new SPOT cone data · froxel
Z-bins / volumetric fog · decals · any BT/MW buffer-layout clone · ObjectLights
ABI change.
