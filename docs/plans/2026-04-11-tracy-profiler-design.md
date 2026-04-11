# Tracy Profiler Integration Design

**Date:** 2026-04-11
**Branch:** claude/nifty-mendeleev
**Status:** Approved

## Goal

Integrate Tracy Profiler with OpenGL GPU zones into the MC2 build to provide real-time CPU+GPU flame charts. This replaces ad-hoc printf timing and gives immediate visibility into frame-time breakdown, stalls, and GPU pipeline behavior. AMD RGP remains available externally for deep shader-level analysis.

## Integration Method

**Vendored copy-paste** into `3rdparty/tracy/`. Tracy is a single-compilation-unit library:
- `TracyClient.cpp` — compiled once, linked into `gameos` library
- `tracy/Tracy.hpp` — CPU zones, frame marks
- `tracy/TracyOpenGL.hpp` — GPU context + GPU zones via GL timer queries

Matches existing pattern of vendored SDL2/GLEW/ZLIB in `3rdparty/`.

## Build Configuration

- `TRACY_ENABLE` defined globally via `add_definitions(-DTRACY_ENABLE)` in root CMakeLists.txt
- Always compiled in; ~1ns overhead per zone when Tracy GUI not connected
- Link `ws2_32` on Windows (Tracy uses Winsock for network streaming)
- `TracyClient.cpp` added to `GameOS/gameos/CMakeLists.txt` source list

## Wrapper Header: `GameOS/gameos/gos_profiler.h`

Single include for all MC2 source files. Contains:
```cpp
#pragma once
#include <tracy/Tracy.hpp>
#include <tracy/TracyOpenGL.hpp>
```

## GPU Context

`TracyGpuContext` called once after GL context creation in `gameosmain.cpp`, inside `gos_CreateRenderer()` or immediately after. This initializes Tracy's GL timer query ring buffer.

`TracyGpuCollect` called once per frame after `swap_window()` to harvest completed GPU timer results.

## Zone Placement (18 zones)

### Frame-Level (gameosmain.cpp)

| Zone | Type | Location | Purpose |
|---|---|---|---|
| `FrameMark` | Frame | After `swap_window()` (line ~397) | Frame delimiter for Tracy timeline |
| `GameLogic` | CPU | Around `DoGameLogic()` (line 388) | Total game simulation time |
| `DrawScreen` | CPU | Around `draw_screen()` (line 396) | Total render time |
| `SwapWindow` | CPU | Around `swap_window()` (line 397) | Swap/vsync stall |

### Shadow Pipeline (gameos_graphics.cpp, txmmgr.cpp, gos_postprocess.cpp)

| Zone | Type | Location | Purpose |
|---|---|---|---|
| `Shadow.StaticBuild` | CPU+GPU | `txmmgr.cpp:1072` | One-time static shadow render at map load |
| `Shadow.StaticPrePass` | GPU | `beginShadowPrePass()` | FBO bind, clear, shader setup |
| `Shadow.StaticBatch` | GPU | `drawShadowBatchTessellated()` | Per-batch tessellated shadow draw (inner loop) |
| `Shadow.StaticEnd` | CPU | `endShadowPrePass()` | Comparison mode restore |
| `Shadow.DynMatrixBuild` | CPU | `buildDynamicLightMatrix()` | Per-frame frustum matrix computation |
| `Shadow.DynPass` | GPU | `txmmgr.cpp:1112` | Full dynamic shadow pass |
| `Shadow.DynObjectBatch` | CPU+GPU | `drawShadowObjectBatch()` | GPU readback + CPU transform + re-upload (known stall point) |

### Camera / Matrix Pipeline (gamecam.cpp, txmmgr.cpp)

| Zone | Type | Location | Purpose |
|---|---|---|---|
| `Camera.BuildMVP` | CPU | `gamecam.cpp:147-184` | terrainMVP composition + axis swap + uploads |
| `Camera.UpdateRenderers` | CPU | `gameosmain.cpp:206` | Full MC2 update-render cycle |
| `Camera.SceneDataUpload` | CPU | `txmmgr.cpp:986-1001` | UBO updates (fog, camera pos, scene params) |

### Render Phases (txmmgr.cpp, gos_postprocess.cpp)

| Zone | Type | Location | Purpose |
|---|---|---|---|
| `Render.3DObjects` | CPU+GPU | `txmmgr.cpp:1006-1062` | DRAWSOLID hardware vertex nodes (ShapeRenderer) |
| `Render.TerrainSolid` | CPU+GPU | `txmmgr.cpp:1122-1175` | DRAWSOLID terrain (tessellated) |
| `Render.Overlays` | CPU+GPU | After terrain solid | DRAWALPHA + craters + water |
| `Render.PostProcess` | GPU | `endScene()` | Bloom ping-pong + composite |

### GPU Zone Summary

TracyGpuZone on these 6 passes (the GPU-heavy ones):
1. `Shadow.StaticBatch`
2. `Shadow.DynPass`
3. `Shadow.DynObjectBatch`
4. `Render.TerrainSolid`
5. `Render.3DObjects`
6. `Render.PostProcess`

## AMD RGP Compatibility

RGP works externally via Radeon Developer Panel -- no code changes. Tracy GPU zones use standard `GL_TIME_ELAPSED` queries, fully supported on RX 7900 XTX. Both tools run simultaneously: Tracy for unified CPU+GPU timeline, RGP for shader occupancy / wavefront analysis.

## Known Concern: drawShadowObjectBatch GPU Readback

`drawShadowObjectBatch()` calls `glGetBufferSubData()` to read vertex/index data from GPU, transforms on CPU with `alloca`, then re-uploads via `drawShadowBatchTessellated()`. This is a GPU sync point that forces pipeline drain. Tracy will immediately highlight this as a stall. This is a prime candidate for future optimization (CPU-side vertex cache or compute shader transform).

## File Changes Summary

| File | Change |
|---|---|
| `3rdparty/tracy/` | New: vendored Tracy source |
| `CMakeLists.txt` | Add `TRACY_ENABLE` define, tracy include path, ws2_32 link |
| `GameOS/gameos/CMakeLists.txt` | Add `TracyClient.cpp` to sources |
| `GameOS/gameos/gos_profiler.h` | New: wrapper header |
| `GameOS/gameos/gameosmain.cpp` | TracyGpuContext, FrameMark, GameLogic/DrawScreen/SwapWindow zones |
| `GameOS/gameos/gameos_graphics.cpp` | Shadow zones (6), terrain batch zone |
| `GameOS/gameos/gos_postprocess.cpp` | PostProcess zone, DynMatrixBuild zone |
| `code/gamecam.cpp` | Camera.BuildMVP, Camera.UpdateRenderers zones |
| `mclib/txmmgr.cpp` | RenderLists phases (4 zones), static shadow build zone, SceneDataUpload zone |
