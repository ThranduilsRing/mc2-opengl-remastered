# Tracy Profiler Integration — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add always-on Tracy CPU+GPU profiling with 18 zones covering frame, shadow, camera, render, and post-process paths.

**Architecture:** Vendor Tracy source into `3rdparty/tracy/`, compile `TracyClient.cpp` into the `gameos` library, define `TRACY_ENABLE` globally. A thin wrapper header `gos_profiler.h` provides the single include. GPU zones use OpenGL timer queries via `TracyOpenGL.hpp`.

**Tech Stack:** Tracy (vendored), CMake, MSVC, OpenGL 4.2, GLEW

**Design doc:** `docs/plans/2026-04-11-tracy-profiler-design.md`

---

### Task 1: Vendor Tracy Source

**Files:**
- Create: `3rdparty/tracy/` (entire public/ directory from Tracy repo)

**Step 1: Download and extract Tracy public directory**

```bash
cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
# Clone Tracy repo to temp location, copy public/ dir
git clone --depth 1 https://github.com/wolfpld/tracy.git /tmp/tracy-src
cp -r /tmp/tracy-src/public/* 3rdparty/tracy/
rm -rf /tmp/tracy-src
```

This copies the full `public/` tree:
- `3rdparty/tracy/TracyClient.cpp` — single compilation unit
- `3rdparty/tracy/tracy/Tracy.hpp` — CPU macros
- `3rdparty/tracy/tracy/TracyOpenGL.hpp` — GPU macros
- `3rdparty/tracy/client/` — internal client implementation
- `3rdparty/tracy/common/` — socket, lz4, system utilities

**Step 2: Verify the structure**

```bash
ls 3rdparty/tracy/TracyClient.cpp
ls 3rdparty/tracy/tracy/Tracy.hpp
ls 3rdparty/tracy/tracy/TracyOpenGL.hpp
```

Expected: all three files exist.

**Step 3: Commit**

```bash
git add 3rdparty/tracy/
git commit -m "vendor: add Tracy profiler v0.11.x source (public/ directory)"
```

---

### Task 2: CMake Integration

**Files:**
- Modify: `CMakeLists.txt` (root, lines 36-41 MSVC block, line 78, line 195)
- Modify: `GameOS/gameos/CMakeLists.txt` (line 29, line 36-37)

**Step 1: Add TRACY_ENABLE define and include path to root CMakeLists.txt**

In `CMakeLists.txt`, after the existing `add_definitions` block (after line 53):

```cmake
# Tracy profiler (always-on, ~1ns overhead when not connected)
add_definitions(-DTRACY_ENABLE)
```

Add tracy to the include path. After line 78 (`set(THIRDPARTY_INCLUDE_DIRS ...)`):

```cmake
# Tracy headers are at 3rdparty/tracy/ — includes resolve as <tracy/Tracy.hpp>
list(APPEND THIRDPARTY_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tracy")
```

Add `ws2_32` to Windows link libs. In the `if(NOT WIN32)` / `else()` block at line 188, change:

```cmake
set(ADDITIONAL_LIBS winmm)
```

to:

```cmake
set(ADDITIONAL_LIBS winmm ws2_32 dbghelp)
```

(Tracy needs `ws2_32` for network streaming and `dbghelp` for callstack resolution on Windows.)

**Step 2: Add TracyClient.cpp to gameos library**

In `GameOS/gameos/CMakeLists.txt`, add to the SOURCES list (after `utils/timing.cpp`):

```cmake
    ${CMAKE_SOURCE_DIR}/3rdparty/tracy/TracyClient.cpp
```

**Step 3: Re-run CMake configure**

```bash
CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
"$CMAKE" -B build64 -G "Visual Studio 17 2022" -A x64
```

Expected: configures without error, shows Tracy-related defines in output.

**Step 4: Build to verify compilation**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2
```

Expected: builds successfully. TracyClient.cpp compiles without errors.

**Step 5: Commit**

```bash
git add CMakeLists.txt GameOS/gameos/CMakeLists.txt
git commit -m "build: integrate Tracy profiler into CMake build"
```

---

### Task 3: Create Wrapper Header

**Files:**
- Create: `GameOS/gameos/gos_profiler.h`

**Step 1: Write the wrapper header**

```cpp
// gos_profiler.h — MC2 profiling wrapper
// Include this in any file that needs profiling zones.
// Tracy is always compiled in; overhead is ~1ns per zone when profiler not connected.
#pragma once

#include <tracy/Tracy.hpp>
#include <tracy/TracyOpenGL.hpp>
```

That's the entire file. Tracy's own headers check for `TRACY_ENABLE` internally.

**Step 2: Commit**

```bash
git add GameOS/gameos/gos_profiler.h
git commit -m "feat: add gos_profiler.h Tracy wrapper header"
```

---

### Task 4: Frame-Level Zones (gameosmain.cpp)

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp`

**Step 1: Add include**

At the top of `gameosmain.cpp`, after the existing includes (around line 15):

```cpp
#include "gos_profiler.h"
```

**Step 2: Add TracyGpuContext after GL context creation**

In `gameosmain_main()` (or equivalent), after `gos_CreateRenderer(ctx, win, w, h)` at line 363, add:

```cpp
    TracyGpuContext;
```

This must be called exactly once, after the GL context is current.

**Step 3: Add frame zones in the main loop**

The main loop is at lines 380-404. Modify to:

```cpp
    while( !g_exit ) {

        uint64_t start_tick = timing::gettickcount();
        timing::sleep(10*1000000);

        {
            ZoneScopedN("GameLogic");
            if(gos_RenderGetEnableDebugDrawCalls()) {
                gos_RenderUpdateDebugInput();
            } else {
                Environment.DoGameLogic();
            }
        }

        process_events();

        gos_RendererHandleEvents();

        {
            ZoneScopedN("DrawScreen");
            graphics::make_current_context(ctx);
            draw_screen();
        }

        {
            ZoneScopedN("SwapWindow");
            graphics::swap_window(win);
        }

        TracyGpuCollect;
        FrameMark;

        g_exit |= gosExitGameOS();

        uint64_t end_tick = timing::gettickcount();
        uint64_t dt = timing::ticks2ms(end_tick - start_tick);
        frameRate = 1000.0f / (float)dt;
    }
```

Key points:
- `TracyGpuCollect` harvests completed GPU timer queries (must be after swap, before FrameMark)
- `FrameMark` delimits frames in Tracy's timeline
- Each zone is a scoped block with `ZoneScopedN("name")`

**Step 4: Add Camera.UpdateRenderers zone in draw_screen()**

In the `draw_screen()` function (around line 205-207):

```cpp
    {
        ZoneScopedN("Camera.UpdateRenderers");
        gos_RendererBeginFrame();
        Environment.UpdateRenderers();
        gos_RendererEndFrame();
    }
```

**Step 5: Build and verify**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2
```

Expected: compiles without errors.

**Step 6: Commit**

```bash
git add GameOS/gameos/gameosmain.cpp
git commit -m "feat: add Tracy frame-level zones (GameLogic, DrawScreen, SwapWindow)"
```

---

### Task 5: Shadow Pipeline Zones (gameos_graphics.cpp)

**Files:**
- Modify: `GameOS/gameos/gameos_graphics.cpp`

**Step 1: Add include**

At the top, after existing includes:

```cpp
#include "gos_profiler.h"
```

**Step 2: Zone in beginShadowPrePass() — line 2102**

Add at the start of the function body, after the early-return guard:

```cpp
void gosRenderer::beginShadowPrePass() {
    gosPostProcess* pp = getGosPostProcess();
    if (!pp || !pp->shadowsEnabled_ || !shadow_terrain_material_) return;

    ZoneScopedN("Shadow.StaticPrePass");
    TracyGpuZone("Shadow.StaticPrePass");

    // ... rest of function unchanged
```

**Step 3: Zone in drawShadowBatchTessellated() — line 2140**

```cpp
void gosRenderer::drawShadowBatchTessellated(gos_VERTEX* vertices, int numVerts,
    WORD* indices, int numIndices,
    const gos_TERRAIN_EXTRA* extras, int extraCount)
{
    if (!shadow_prepass_active_ || numVerts <= 0 || extraCount <= 0) return;

    ZoneScopedN("Shadow.StaticBatch");
    TracyGpuZone("Shadow.StaticBatch");

    // ... rest of function unchanged
```

**Step 4: Zone in endShadowPrePass() — line 2295**

```cpp
void gosRenderer::endShadowPrePass() {
    if (!shadow_prepass_active_) return;

    ZoneScopedN("Shadow.StaticEnd");

    // ... rest of function unchanged
```

**Step 5: Zone in drawShadowObjectBatch() — line 2225**

This is the GPU readback stall point — both CPU and GPU zones:

```cpp
void gosRenderer::drawShadowObjectBatch(HGOSBUFFER vb, HGOSBUFFER ib,
    HGOSVERTEXDECLARATION vdecl, const float* worldMatrix4x4)
{
    if (!shadow_prepass_active_ || !vb || !ib || ib->count_ == 0) return;

    ZoneScopedN("Shadow.DynObjectBatch");
    TracyGpuZone("Shadow.DynObjectBatch");

    // ... rest of function unchanged
```

**Step 6: Build and verify**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2
```

**Step 7: Commit**

```bash
git add GameOS/gameos/gameos_graphics.cpp
git commit -m "feat: add Tracy zones for shadow pipeline (prepass, batch, object)"
```

---

### Task 6: Camera + Render Phase Zones (gamecam.cpp, txmmgr.cpp)

**Files:**
- Modify: `code/gamecam.cpp`
- Modify: `mclib/txmmgr.cpp`

**Step 1: Add include to gamecam.cpp**

At the top, after existing includes:

```cpp
#include "gos_profiler.h"
```

**Step 2: Camera.BuildMVP zone in gamecam.cpp**

Wrap the terrainMVP block at line 147-184:

```cpp
        {
            ZoneScopedN("Camera.BuildMVP");
            // Compose terrainMVP: MC2 world coords -> GL clip coords
            const float* W = (const float*)&worldToClip;
            // ... existing MVP code through gos_SetTerrainLightDir ...
            #undef WTC
        }
```

Note: the `#define WTC` / `#undef WTC` must stay inside the block.

**Step 3: Add include to txmmgr.cpp**

At the top, after existing includes:

```cpp
#include "gos_profiler.h"
```

**Step 4: Zones inside MC_TextureManager::renderLists()**

After the existing state setup and fog (around line 965), before the first render loop:

**Camera.SceneDataUpload** — wrap lines 986-1001:
```cpp
    {
        ZoneScopedN("Camera.SceneDataUpload");
        sceneData_->fog_start = eye->fogStart;
        // ... through gos_UpdateBuffer(sceneDataBuffer_, ...) ...
    }
```

**Shadow.StaticBuild** — wrap the static shadow block at lines 1072-1103:
```cpp
    if (gos_IsTerrainTessellationActive() && !gos_StaticShadowsRendered()) {
        ZoneScopedN("Shadow.StaticBuild");
        TracyGpuZone("Shadow.StaticBuild");
        gos_RenderStaticShadows();
        gos_BeginShadowPrePass();
        // ... existing loop ...
        gos_EndShadowPrePass();
        gos_MarkStaticShadowsRendered();
    }
```

**Shadow.DynPass** — wrap the dynamic shadow block at lines 1105-1117:
```cpp
    if (gos_IsTerrainTessellationActive() && g_numShadowShapes > 0) {
        ZoneScopedN("Shadow.DynPass");
        TracyGpuZone("Shadow.DynPass");
        // ... existing dynamic shadow code ...
    }
```

**Render.3DObjects** — wrap the hardware vertex node loop at lines 1006-1062:
```cpp
    {
        ZoneScopedN("Render.3DObjects");
        TracyGpuZone("Render.3DObjects");
        for (size_t i = 0; i<nextAvailableHardwareVertexNode; i++)
        {
            // ... existing 3D object rendering loop unchanged ...
        }
    }
```

**Render.TerrainSolid** — wrap the DRAWSOLID terrain loop at lines 1122-1175:
```cpp
    {
        ZoneScopedN("Render.TerrainSolid");
        TracyGpuZone("Render.TerrainSolid");
        bool bSkip_DRAWSOLID = false;
        for (long i=0;i<nextAvailableVertexNode && !bSkip_DRAWSOLID;i++)
        {
            // ... existing terrain draw loop unchanged ...
        }
    }
```

**Render.Overlays** — wrap the remaining alpha/crater/water loops (after terrain solid, through end of renderLists). Find the DRAWALPHA loops and water draw, wrap them:
```cpp
    {
        ZoneScopedN("Render.Overlays");
        // ... DRAWALPHA terrain, DRAWALPHA detail, craters, water ...
    }
```

**Step 5: Build and verify**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2
```

**Step 6: Commit**

```bash
git add code/gamecam.cpp mclib/txmmgr.cpp
git commit -m "feat: add Tracy zones for camera MVP, render phases, scene upload"
```

---

### Task 7: PostProcess + Dynamic Shadow Matrix Zones (gos_postprocess.cpp)

**Files:**
- Modify: `GameOS/gameos/gos_postprocess.cpp`

**Step 1: Add include**

```cpp
#include "gos_profiler.h"
```

**Step 2: Zone in endScene() — line 345**

```cpp
void gosPostProcess::endScene()
{
    ZoneScopedN("Render.PostProcess");
    TracyGpuZone("Render.PostProcess");

    // ... rest of function unchanged
```

**Step 3: Zone in buildDynamicLightMatrix()**

Find `buildDynamicLightMatrix` and add after the early-return guard:

```cpp
    ZoneScopedN("Shadow.DynMatrixBuild");
```

(CPU only — this is matrix math, no GL calls.)

**Step 4: Build and verify**

```bash
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2
```

**Step 5: Commit**

```bash
git add GameOS/gameos/gos_postprocess.cpp
git commit -m "feat: add Tracy zones for post-process and dynamic shadow matrix"
```

---

### Task 8: Full Build + Smoke Test

**Step 1: Clean build**

```bash
CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2 --clean-first
```

Expected: full build succeeds with no warnings from Tracy or profiling code.

**Step 2: Deploy and run**

Use `/mc2-deploy` skill to deploy, then launch mc2.exe. Verify:
- Game starts and runs normally
- No crashes or GL errors from timer queries
- Performance is not noticeably degraded (Tracy not connected = ~1ns/zone)

**Step 3: Connect Tracy Profiler**

- Download Tracy profiler GUI from https://github.com/wolfpld/tracy/releases (get the Windows GUI binary)
- Launch the Tracy profiler GUI
- Launch mc2.exe
- Tracy should auto-connect and show the flame chart with all 18 zones
- Verify frame marks show up, GPU zones appear on a separate GPU timeline

**Step 4: Final commit with any fixups**

```bash
git add -A
git commit -m "feat: Tracy profiler integration complete (18 CPU+GPU zones)"
```

---

### Task 9: Update CLAUDE.md

**Files:**
- Modify: `CLAUDE.md`
- Modify: `docs/architecture.md`

**Step 1: Add profiling section to CLAUDE.md**

Add after the Known Issues section:

```markdown
## Profiling
- **Tracy Profiler** always compiled in (`TRACY_ENABLE`). Connect Tracy GUI to see real-time flame charts.
- **GPU zones** on shadow passes, terrain draw, 3D objects, post-process. Uses GL timer queries.
- **AMD RGP** works externally via Radeon Developer Panel for shader-level analysis.
- Include `gos_profiler.h` to add new zones. Use `ZoneScopedN("Name")` for CPU, add `TracyGpuZone("Name")` for GPU-heavy code.
```

**Step 2: Add profiling notes to docs/architecture.md**

Add a new section:

```markdown
## Profiling (Tracy)
18 zones instrument the frame: GameLogic, DrawScreen, SwapWindow, Camera.BuildMVP, Camera.UpdateRenderers, Camera.SceneDataUpload, Shadow.StaticBuild, Shadow.StaticPrePass, Shadow.StaticBatch, Shadow.StaticEnd, Shadow.DynMatrixBuild, Shadow.DynPass, Shadow.DynObjectBatch, Render.3DObjects, Render.TerrainSolid, Render.Overlays, Render.PostProcess. GPU zones (TracyGpuZone) on: StaticBatch, DynPass, DynObjectBatch, 3DObjects, TerrainSolid, PostProcess. TracyGpuContext initialized after GL context creation. TracyGpuCollect called after swap_window each frame.
```

**Step 3: Commit**

```bash
git add CLAUDE.md docs/architecture.md
git commit -m "docs: add Tracy profiling reference to CLAUDE.md and architecture.md"
```
