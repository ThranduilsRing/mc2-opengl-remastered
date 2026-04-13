# MC2 OpenGL Modding Guide

A practical guide to modifying the MechCommander 2 OpenGL renderer. Covers building, shader editing, texture upscaling, debug tools, and autonomous development workflows.

---

## 1. Quick Start

### Prerequisites

- **Visual Studio 2022 Build Tools** (MSVC v143)
- **CMake** (bundled with VS Build Tools, or standalone 3.10+)
- **Git** (for source management)
- **Python 3.10+** (for texture upscaling scripts only)

### Build

Always use `RelWithDebInfo`. Release builds crash with `GL_INVALID_ENUM` due to debug callback registration.

```bash
CMAKE="C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
"$CMAKE" --build build64 --config RelWithDebInfo --target mc2
```

Output: `build64/RelWithDebInfo/mc2.exe`

### Deploy

The game runs from a separate deployment directory, not the source tree. Deploy individual files with verification:

```bash
DEPLOY="A:/Games/mc2-opengl/mc2-win64-v0.1.1"
cp -f build64/RelWithDebInfo/mc2.exe "$DEPLOY/"
diff -q build64/RelWithDebInfo/mc2.exe "$DEPLOY/mc2.exe"
```

**Never use `cp -r`** on Windows/MSYS2 -- it silently fails. Always copy files individually and verify with `diff -q`.

Deploy shaders the same way:
```bash
for f in shaders/*.frag shaders/*.vert shaders/*.geom shaders/*.tcs shaders/*.tes shaders/include/*; do
    cp -f "$f" "$DEPLOY/$f"
    diff -q "$f" "$DEPLOY/$f"
done
```

### Run

```bash
cd "A:/Games/mc2-opengl/mc2-win64-v0.1.1"
./mc2.exe                          # normal gameplay
./mc2.exe -mission mis0101         # skip menus, load mission directly
./mc2.exe --validate --frames 60   # validation mode (see Section 7)
```

---

## 2. Rendering Pipeline

### Render Order

Each frame renders in this order:

1. **Skybox** -- procedural gradient with sun disc (`skybox.frag`)
2. **Terrain** -- tessellated PBR splatting (`gos_terrain.frag`, `.vert`, `.tcs`, `.tes`)
3. **Overlays** -- roads, cement, decals (`gos_tex_vertex.frag`)
4. **Water** -- separate overlay, not part of terrain splatting (`gos_tex_vertex.frag`)
5. **3D Objects** -- mechs, buildings, trees (`object_tex.frag`, `gos_vertex_lighted.frag`)
6. **Particles** -- gosFX effects (`gos_tex_vertex.frag`)
7. **Post-processing** -- bloom, shadow pass, FXAA, tonemapping (`postprocess.frag`, etc.)
8. **HUD/UI** -- 2D overlays (`gos_tex_vertex.frag`, `gos_text.frag`)

### G-Buffer / MRT

The renderer uses a minimal G-buffer with two render targets:

- **GL_COLOR_ATTACHMENT0** -- standard color output (RGBA8)
- **GL_COLOR_ATTACHMENT1** -- normal buffer (RGBA16F). Terrain writes real surface normals with alpha=1. Overlays/objects write a flat default with alpha=0. The alpha channel flags "is terrain" for the post-process shadow pass.
- **Depth** -- sampleable depth texture (was renderbuffer, converted for world-position reconstruction)

### Shadow Pipeline

Two independent shadow maps:

**Static terrain shadows** (4096x4096):
- World-fixed orthographic projection covering the visible map
- Multi-frame accumulation: doesn't clear after frame 1, accumulates as camera pans
- Re-renders when camera moves >100 units from last render position
- Ortho size uses `sqrt(2) * 1.05` factor for diagonal coverage

**Dynamic mech shadows** (2048x2048):
- Centered on ray-ground intersection point with 0.80 bias toward camera, radius 1200
- Direct GPU draw via `glUseProgram` + `glDrawElements` (bypasses material system)
- Forces `glDepthMask(GL_TRUE)` after shader bind (material system would override this)
- ~184us per frame (3.05% of frame time)

**Post-process shadow pass**:
- Fullscreen pass after all scene rendering
- Reconstructs world position from depth via inverse view-projection matrix
- Samples both static and dynamic shadow maps
- Darkens non-terrain pixels via multiplicative blending (terrain flagged by normal alpha)

### Post-Processing Chain

```
Scene FBO --> Bloom Threshold --> Gaussian Blur (2-pass) --> Composite
         --> Shadow Screen Pass (depth reconstruction + shadow sampling)
         --> FXAA
         --> ACES Filmic Tonemapping + Gamma Correction
         --> Default Framebuffer
```

All post-processing is managed by `gosPostProcess` in `gos_postprocess.cpp/.h`.

---

## 3. Shader Modding

### Shader File Map

| File | Purpose |
|------|---------|
| `gos_terrain.frag/vert/tcs/tes` | Terrain rendering: PBR splatting, normal mapping, POM, cloud shadows, height fog, triplanar cliffs, shadow sampling |
| `gos_tex_vertex.frag/vert` | Generic textured: overlays, water, particles, UI |
| `gos_tex_vertex_lighted.frag` | Lit textured vertices |
| `gos_vertex.frag/vert` | Untextured colored vertices |
| `gos_vertex_lighted.frag` | Lit untextured vertices |
| `object_tex.frag` | 3D objects (mechs, buildings) |
| `gos_text.frag` | Text rendering |
| `skybox.frag/vert` | Procedural sky gradient + sun disc |
| `bloom_threshold.frag` | Bright-pass extraction for bloom |
| `bloom_blur.frag` | Gaussian blur (horizontal + vertical) |
| `postprocess.frag` | Final composite, FXAA, tonemapping |
| `shadow_depth.frag` | Shadow map depth pass |
| `shadow_terrain.frag/vert/tcs/tes` | Terrain shadow pre-pass |
| `include/shadow.hglsl` | Shared shadow sampling (Poisson PCF) |

### Critical Rules

**No `#version` in shader files.** The version directive is passed as a prefix string to `makeProgram()`:
```cpp
glsl_program::makeProgram("terrain", "gos_terrain.vert", "gos_terrain.frag", "#version 420\n");
```

Putting `#version` directly in the file causes compile errors because the prefix is prepended first.

**Hot-reload fails silently.** If a shader fails to compile, the old shader stays active. Always check `stderr` for `SHADER COMPILE ERROR` or `SHADER LINK ERROR` messages.

**Uniform API: deferred before `apply()`.** Use `prog->setFloat()`/`setInt()` before the draw call (flushed during `apply()`). If you need direct GL uniforms, call them after `apply()`:
```cpp
// Deferred (before draw):
material->setFloat("myParam", 1.0f);
// ... draw call happens, apply() flushes these

// Direct (after manual apply):
material->apply();
glUniform1f(glGetUniformLocation(prog, "myParam"), 1.0f);
```

### Adding a New Post-Process Effect

1. Write your fragment shader (e.g., `my_effect.frag`)
2. In `gos_postprocess.h`, add an FBO/texture pair and an enable flag
3. In `gos_postprocess.cpp`:
   - Create the shader in `init()` using `glsl_program::makeProgram()`
   - Create FBO + texture in `resize()`
   - Add your pass between existing passes in `endScene()`
   - Bind previous pass output as input, render fullscreen quad
4. Optionally add a debug hotkey toggle in `gameosmain.cpp`

---

## 4. Debug Hotkeys

All debug toggles use Right Alt as modifier to avoid conflicts with game controls.

| Key | Function |
|-----|----------|
| RAlt+F1 | Toggle bloom |
| RAlt+F2 | Cycle shadow debug overlay (static -> dynamic -> off) |
| RAlt+F3 | Toggle shadows on/off |
| RAlt+F5 | Toggle terrain draw (killswitch) |
| RAlt+4 | Cycle screen shadow modes |
| RAlt+5 | Toggle GPU grass |
| RAlt+6 | Toggle god rays |
| RAlt+7 | Toggle shoreline foam |
| RAlt+9 | Toggle SSAO |
| RAlt+D | Toggle debug draw calls |
| RAlt+Escape | Quit |
| F6-F12 | Tessellation parameters (no modifier needed) |
| [ / ] | Decrease/increase shadow softness |

**Never use Alt+F4** -- it closes the window via Windows OS, not a game hotkey.

---

## 5. Texture Upscaling

### Overview

The texture upscaling pipeline uses AI super-resolution to upscale MC2's original textures from ~64x64 to 256x256 (4x). This dramatically improves terrain and object detail.

### Setup

1. Download [realesrgan-ncnn-vulkan](https://github.com/xinntao/Real-ESRGAN-ncnn-vulkan/releases) and extract to `realesrgan-ncnn-vulkan/` in the repo root
2. Place model files (e.g., `4x-UltraSharpV2.safetensors`) in `esrgan_models/`
3. Run the upscaler:

```bash
python upscale_gpu.py --input data/art/ --output mc2srcdata/art_4x_gpu/ --scale 4
```

### Deploying Upscaled Textures

MC2 has a loose file override system. Files in `data/` directories override the FST archive:

- `data/art/` overrides art textures
- `data/tgl/` overrides TGL model textures
- `data/objects/` overrides object textures

`File::open()` tries disk first, falls back to FastFile archive.

Copy upscaled textures to the deploy directory:
```bash
cp -f mc2srcdata/art_4x_gpu/*.tga "$DEPLOY/data/art/"
```

### Buffer Size Fix

Upscaled textures are larger and may exceed the default decompression buffer. If textures fail to load, check `MAX_LZ_BUFFER_SIZE` in `mclib/txmmgr.h` -- it was increased from 263KB to 8MB for 4x textures.

---

## 6. Known Issues and Driver Quirks

### Known Issues

- **Post-processing applies to HUD**: Bloom, FXAA, and tonemapping affect the HUD/UI because scene and HUD share the same framebuffer. Fix requires rendering the scene and HUD into separate FBOs.
- **Shadow banding shifts with camera rotation**: Terrain geometry is view-dependent (tessellation varies with camera angle), causing shadow edge positions to shift when rotating.
- **SSAO disabled by default**: The half-resolution 16-sample hemisphere approach produces visible noise/banding at steep camera angles and has minimal visual impact at standard RTS zoom distances. Infrastructure is kept for future experimentation (RAlt+9 to toggle).

### AMD Driver Rules (RX 7900 XTX / RDNA3)

These quirks were discovered on AMD drivers and may affect other AMD GPUs:

- **sampler2DArray crash**: Declaring `sampler2DArray` in a shader that doesn't use it can crash the driver. Only declare samplers that are actually sampled.
- **Attribute 0 must be used**: If vertex attribute 0 is declared but not used in the vertex shader, AMD drivers may produce incorrect results or crash. Always use attribute 0 for position.
- **gl_FragDepth precision**: Writing `gl_FragDepth` in a fragment shader on AMD requires careful precision handling. Inconsistent depth writes can cause z-fighting.
- **Feedback loops**: Reading from a texture that is also bound as a render target (even a different mip/layer) triggers undefined behavior on AMD. Always use separate textures for read and write.

---

## 7. Autonomous Development

### Validation Mode

The `--validate` flag enables autonomous build-test iteration. The game loads a mission, renders N frames, captures telemetry and an optional screenshot, then exits with a status code.

```bash
mc2.exe --validate --frames 60 --log validate.json --screenshot validate.tga
```

**Telemetry output** (`validate.json`):
```json
{
  "frames": 60,
  "avg_frame_ms": 16.2,
  "max_frame_ms": 34.1,
  "gl_errors": [],
  "shader_errors": [],
  "screenshot": "validate.tga",
  "exit_code": 0
}
```

**Exit codes**: 0 = success, 1 = error (shader compile failure or GL errors).

**Feature overrides**:
```bash
mc2.exe --validate --disable bloom --enable shadows --frames 30
```

### Autonomous Iteration Workflow

For AI-assisted development (e.g., with Claude Code):

1. Edit shader or C++ source
2. Build: `cmake --build build64 --config RelWithDebInfo --target mc2`
3. Deploy: copy exe + changed shaders to deploy directory
4. Validate: `mc2.exe --validate --frames 60 --log validate.json`
5. Read `validate.json` -- check `exit_code`, `shader_errors`, `avg_frame_ms`
6. If errors: diagnose from shader error messages, fix, repeat from step 1
7. If clean: the change is safe, commit

No screenshot evaluation is needed for most changes -- the JSON telemetry catches crashes, shader failures, and GL errors automatically.

### Tracy Profiler

Tracy is always compiled in (`TRACY_ENABLE`). To profile:

1. Download [Tracy Profiler](https://github.com/wolfpld/tracy/releases) GUI
2. Launch mc2.exe
3. Connect Tracy GUI to localhost
4. See real-time flame charts with 18 CPU+GPU zones

Key zones to watch:
- `Shadow.StaticBatch` / `Shadow.DynPass` -- shadow rendering cost
- `Render.TerrainSolid` -- terrain draw cost
- `Render.PostProcess` -- post-processing cost
- `Render.3DObjects` -- mech/building rendering

Add new zones with:
```cpp
#include "gos_profiler.h"
ZoneScopedN("MyZone");           // CPU zone
TracyGpuZone("MyGPUZone");      // GPU zone (timer query)
```

---

## Appendix: Coordinate Spaces

MC2 uses two coordinate spaces that must not be mixed:

**Raw MC2** (used in shadow code, terrain, light direction):
- x = east, y = north, z = elevation (Z-up)

**Swizzled GL / Stuff/MLR** (used in camera, vertex transforms):
- x = left, y = elevation, z = forward (Y-up)

**Transform**: `MC2.x = -Stuff.x`, `MC2.y = Stuff.z`, `MC2.z = Stuff.y`

Getting the swizzle wrong doesn't produce garbled output -- it produces *nothing*, because geometry ends up outside the frustum. If you see all-magenta in a shadow map or objects disappear, check the coordinate space conversion first.
