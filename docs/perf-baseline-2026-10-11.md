<!-- doc-status: current | reviewed:2026-10-11 -->
# Perf baseline 2026-10-11 (GL and VK, mc2_01 and mc2_10)

The reference the C++23 / S5 perf steps are measured against. Capture only, no engine code changed. Tip `c14761546`
(deployed to `A:/Games/mc2-opengl/mc2-win64-lane2`, exe-guard fingerprint verified), RX 7900 XTX, 3840x2160 drawable,
stock profile, shipped default gates.

**Resolution caveat (2026-10-12):** the 3840x2160 here was the install's options.cfg size, not a choice. Since then captures and Tracy traces are
pinned to 1920x1080 (`scripts/smoke_lib/render_resolution.py`), so a trace taken today is a quarter of the pixels and is NOT comparable with the
numbers below (fragment-bound zones move most). Re-take this baseline at the pinned size before using it as a regress reference; the options.cfg
render options of a trace are recorded next to it (`<trace>.options.json`) and `compare`/`regress` refuse a mismatch.

## How it was measured
- `tools/tracy_capture_compare.py baseline` per mission, both arms, 20 s Tracy window, free timestep, `MC2_FPS_CAP=0`
  (uncapped, render-bound), `MC2_LOG=1`, window shown but parked offscreen (never minimised: no `Frame.BackgroundThrottle`
  zone appears in any trace, so no arm slept). VK arm = backend vulkan + umbrella `MC2_VK_SHARED_GBUFFER=1`, shadow
  interop off; its logs show `[VK_PRESENT] present path UP`, so it did not fail soft to GL.
- Deploy lease on the lane2 folder held for the whole session; no other `mc2.exe` was running.
- mc2_10 uses the harness pose `-6656,-6656,150.625,10.9094,6045.2358@600`; mc2_01 has no pose (mission start camera).
- Numbers come from `tools/perf_baseline_report.py` (added with this baseline): frame time = delta between consecutive
  `DrawScreen` starts; the first 40% of frames are dropped as load / settle (mc2_10: the first 650, the pose pins at
  frame 600); deltas over 200 ms are discarded. Zones ranked by SELF time per frame in that same window, rebuilt from
  `tracy-csvexport --unwrap` (the aggregate CSV cannot be windowed and would include mission-load zones).
- One capture per arm, so no run-to-run spread is measured here: treat differences under ~3% as noise until a repeat
  confirms them. Raw traces are in `release_assets/tracy-baselines/2026-10-11-mc2_01|mc2_10/` (gitignored).

## Frame time (steady window)
| mission | arm | frames | mean ms | p50 | p95 | p99 | fps mean | fps at p99 |
|---|---|--:|--:|--:|--:|--:|--:|--:|
| mc2_01 | GL | 1167 | 6.75 | 6.70 | 7.65 | 8.68 | 148.2 | 115.2 |
| mc2_01 | VK | 1516 | 5.32 | 5.25 | 6.65 | 7.26 | 188.0 | 137.8 |
| mc2_10 | GL | 398 | 13.85 | 13.97 | 16.16 | 17.88 | 72.2 | 55.9 |
| mc2_10 | VK | 619 | 13.81 | 13.67 | 16.86 | 18.62 | 72.4 | 53.7 |

## What the tables say
- Both missions are GPU-completion bound at 4K. On GL the top zone is `SwapWindow.SDL` (4.6 ms of 6.75 on mc2_01, 7.4 of 13.9 on
  mc2_10): the driver blocks there until queued GPU work drains. On VK the same wait shows up as the fence zones
  (`VKIsl.Props.FenceWait` on mc2_01, `VKIsl.Legacy.FenceWait` 5.5 ms on mc2_10), because the VK islands have no Tracy zones of
  their own: their cost is inside whatever waits for them.
- CPU work that is actually ours is small and flat: per frame the next biggest self-time zones are 0.2-1.0 ms each
  (`LightUpload.SubData`, `RenderLists.DynShadowCSM`, `GameCamera::render objects`, `TG.MultiShape.*`,
  `GameLogic.Units.TerrainObjects`, `addLightDataStructure scan`, static-prop snapshot / extract zones). `LightUpload.SubData`
  is ~1 ms on mc2_10 and ~0.18 ms on mc2_01, the largest CPU line that scales with the map.
- mc2_10 is the stricter reference: VK is not faster than GL there (13.8 ms both) while on mc2_01 VK leads by ~21%.
  A C++23 step that moves CPU zones will not show in frame time while the GPU wait dominates; measure those steps
  by the zone tables, and use frame time only to catch a regression.

## mc2_01 (no pose)
### 2026-10-11-mc2_01-gl.tracy

1167 frames in the steady window (777 dropped as warm-up); frame time ms: mean 6.75, p50 6.70, p95 7.65, p99 8.68, max 20.14 (148.2 fps mean, 115.2 fps at p99).

| # | zone | self ms/frame | inclusive ms/frame | calls/frame | thread(s) |
|---|---|---|---|---|---|
| 1 | `SwapWindow.SDL` | 4.569 | 4.569 | 1.0 | 1 |
| 2 | `addLightDataStructure scan` | 0.184 | 0.184 | 237.2 | 1 |
| 3 | `LightUpload.SubData` | 0.180 | 0.180 | 1.0 | 1 |
| 4 | `RenderLists.DynShadowCSM` | 0.151 | 0.151 | 1.0 | 1 |
| 5 | `RenderLists.StaticPropRegistryFlush` | 0.150 | 0.150 | 1.0 | 1 |
| 6 | `StaticProp.SnapshotBuild` | 0.121 | 0.121 | 1.0 | 1 |
| 7 | `GameCamera::render objects` | 0.099 | 0.157 | 1.0 | 1 |
| 8 | `RenderLists.DynShadowDraw` | 0.093 | 0.093 | 1.0 | 1 |
| 9 | `GameLogic.Units.TerrainObjects` | 0.081 | 0.346 | 1.0 | 1 |
| 10 | `GpuStaticProps.Flush` | 0.073 | 0.195 | 1.0 | 1 |
| 11 | `Terrain::geometry chunkActiveProd` | 0.053 | 0.053 | 1.0 | 1 |
| 12 | `GOM.TerrainObjects.ObjUpdate` | 0.053 | 0.233 | 267.0 | 1 |
| 13 | `Extract.SP.Fill` | 0.050 | 0.050 | 0.0 | 1 |
| 14 | `GameLogic.Mission.Collisions` | 0.044 | 0.044 | 1.0 | 1 |
| 15 | `GameCamera::render shadows` | 0.033 | 0.033 | 1.0 | 1 |

### 2026-10-11-mc2_01-vk.tracy

1516 frames in the steady window (1010 dropped as warm-up); frame time ms: mean 5.32, p50 5.25, p95 6.65, p99 7.26, max 9.30 (188.0 fps mean, 137.8 fps at p99).

| # | zone | self ms/frame | inclusive ms/frame | calls/frame | thread(s) |
|---|---|---|---|---|---|
| 1 | `VKIsl.Props.FenceWait` | 1.801 | 1.801 | 1.0 | 1 |
| 2 | `Render.GpuMechs` | 0.426 | 0.471 | 1.0 | 1 |
| 3 | `renderWaterFastPath` | 0.392 | 0.432 | 1.0 | 1 |
| 4 | `Render.PostProcess` | 0.261 | 0.302 | 1.0 | 1 |
| 5 | `addLightDataStructure scan` | 0.178 | 0.178 | 242.0 | 1 |
| 6 | `LightUpload.SubData` | 0.177 | 0.177 | 1.0 | 1 |
| 7 | `VKIsl.Shared.LightFeed` | 0.164 | 0.164 | 1.0 | 1 |
| 8 | `RenderLists.DynShadowCSM` | 0.154 | 0.154 | 1.0 | 1 |
| 9 | `RenderLists.StaticPropRegistryFlush` | 0.136 | 0.136 | 1.0 | 1 |
| 10 | `GameCamera::render objects` | 0.100 | 0.104 | 1.0 | 1 |
| 11 | `RenderLists.DynShadowDraw` | 0.097 | 0.097 | 1.0 | 1 |
| 12 | `GameLogic.Units.TerrainObjects` | 0.076 | 0.330 | 1.0 | 1 |
| 13 | `VKIsl.Present.QueuePresent` | 0.069 | 0.069 | 1.0 | 1 |
| 14 | `GOM.TerrainObjects.ObjUpdate` | 0.050 | 0.223 | 267.0 | 1 |
| 15 | `Terrain::geometry chunkActiveProd` | 0.049 | 0.049 | 1.0 | 1 |

## mc2_10 (harness pose, after the camera pins)
### 2026-10-11-mc2_10-gl.tracy

398 frames in the steady window (650 dropped as warm-up); frame time ms: mean 13.85, p50 13.97, p95 16.16, p99 17.88, max 42.78 (72.2 fps mean, 55.9 fps at p99).

| # | zone | self ms/frame | inclusive ms/frame | calls/frame | thread(s) |
|---|---|---|---|---|---|
| 1 | `SwapWindow.SDL` | 7.405 | 7.405 | 1.0 | 1 |
| 2 | `LightUpload.SubData` | 0.955 | 0.955 | 1.0 | 1 |
| 3 | `Extract.SP.Fill` | 0.532 | 0.532 | 0.5 | 1 |
| 4 | `StaticProp.SnapshotBuild` | 0.408 | 0.408 | 1.0 | 1 |
| 5 | `RenderLists.DynShadowCSM` | 0.355 | 0.355 | 1.0 | 1 |
| 6 | `GameCamera::render objects` | 0.343 | 0.401 | 1.0 | 1 |
| 7 | `TG.MultiShape.PerShapeLoop` | 0.294 | 0.546 | 92.5 | 1 |
| 8 | `GameLogic.Units.Mechs` | 0.291 | 0.501 | 1.0 | 1 |
| 9 | `GameLogic.Units.TerrainObjects` | 0.260 | 0.819 | 1.0 | 1 |
| 10 | `TG.MultiShape.PerLeaf` | 0.246 | 0.246 | 106.1 | 1 |
| 11 | `addLightDataStructure scan` | 0.232 | 0.232 | 299.1 | 1 |
| 12 | `ExtractRenderSnapshot` | 0.203 | 0.873 | 1.0 | 1 |
| 13 | `GOM.TerrainObjects.ObjUpdate` | 0.194 | 0.476 | 536.2 | 1 |
| 14 | `GpuStaticProps.Flush` | 0.149 | 0.558 | 1.0 | 1 |
| 15 | `RenderLists.DynShadowDraw` | 0.144 | 0.144 | 1.0 | 1 |

### 2026-10-11-mc2_10-vk.tracy

619 frames in the steady window (650 dropped as warm-up); frame time ms: mean 13.81, p50 13.67, p95 16.86, p99 18.62, max 20.60 (72.4 fps mean, 53.7 fps at p99).

| # | zone | self ms/frame | inclusive ms/frame | calls/frame | thread(s) |
|---|---|---|---|---|---|
| 1 | `VKIsl.Legacy.FenceWait` | 5.534 | 5.534 | 2.0 | 1 |
| 2 | `LightUpload.SubData` | 0.987 | 0.987 | 1.0 | 1 |
| 3 | `VKIsl.Props.FenceWait` | 0.504 | 0.504 | 1.0 | 1 |
| 4 | `Render.GpuMechs` | 0.449 | 0.525 | 1.0 | 1 |
| 5 | `VKIsl.Shared.LightFeed` | 0.447 | 0.447 | 1.0 | 1 |
| 6 | `renderWaterFastPath` | 0.407 | 0.443 | 1.0 | 1 |
| 7 | `RenderLists.DynShadowCSM` | 0.400 | 0.400 | 1.0 | 1 |
| 8 | `GameCamera::render objects` | 0.378 | 0.388 | 1.0 | 1 |
| 9 | `addLightDataStructure scan` | 0.323 | 0.323 | 432.8 | 1 |
| 10 | `TG.MultiShape.PerLeaf` | 0.284 | 0.284 | 141.2 | 1 |
| 11 | `Extract.SP.Fill` | 0.283 | 0.283 | 0.2 | 1 |
| 12 | `TG.MultiShape.PerShapeLoop` | 0.280 | 0.570 | 117.8 | 1 |
| 13 | `Render.PostProcess` | 0.264 | 0.306 | 1.0 | 1 |
| 14 | `GameLogic.Units.TerrainObjects` | 0.256 | 0.930 | 1.0 | 1 |
| 15 | `GOM.TerrainObjects.ObjUpdate` | 0.223 | 0.595 | 572.9 | 1 |

## Reproduce
```
py -3 tools/tracy_capture_compare.py baseline --install A:/Games/mc2-opengl/mc2-win64-lane2 --mission mc2_01 --pose "" --seconds 20 --name <name>
py -3 tools/tracy_capture_compare.py baseline --install A:/Games/mc2-opengl/mc2-win64-lane2 --mission mc2_10 --seconds 20 --name <name>
py -3 tools/perf_baseline_report.py [--skip-frames 650] release_assets/tracy-baselines/<name>/<name>-gl.tracy ...
```
Hold the lane2 deploy lease (`smoke_lib.deploy_lease`) around the capture and never run it while another `mc2.exe` is up.
