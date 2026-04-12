# Shadow Debug Visualizer + Terrain Killswitch

**Date:** 2026-04-11
**Branch:** claude/nifty-mendeleev

## Problem

Dynamic shadow frustum centering is broken — the isometric camera eye is 2000-3000 units from the gameplay area, so the frustum misses all mechs. There's no way to see what's actually being written to the shadow FBO, making debugging blind.

## Solution

### 1. Shadow Map Visualizer

A corner-screen quad that renders the raw shadow depth texture with diagnostic color mapping.

**Shader** (`shaders/shadow_debug.frag`):
- `sampler2D shadowDebugMap` — raw depth texture (NOT comparison sampler)
- Color mapping:
  - Depth == 1.0 (cleared): **magenta (1, 0, 1)** — nothing was written here
  - Depth 0.0-1.0: grayscale ramp (black=near, white=far)
  - Depth == 0.0 (near plane clip): bright red
- `uniform int shadowDebugMode` — 0=static map, 1=dynamic map

**Rendering** (in `gosPostProcess::drawShadowDebugOverlay()`):
1. Called at end of `endScene()`, after composite pass
2. Save viewport → `glViewport(16, height - 272, 256, 256)` (top-left corner, below title bar)
3. Disable depth test
4. Temporarily set shadow texture compare mode to `GL_NONE` (need raw depth, not PCF comparison result)
5. Bind debug shader, bind shadow depth texture to unit 0
6. Draw `quadVAO_` (fills the 256x256 viewport region)
7. Restore texture compare mode to `GL_COMPARE_REF_TO_TEXTURE`
8. Restore viewport

**State** in `gosPostProcess`:
- `glsl_program* shadowDebugProg_` — compiled at init
- `bool showShadowDebug_` — toggle flag
- `int shadowDebugMode_` — 0=static, 1=dynamic

### 2. Terrain Draw Killswitch

- `bool terrainDrawEnabled_` in `gosRenderer`, default true
- Checked before terrain patch dispatch in `gosRenderer::drawTerrain()`
- API: `gos_SetTerrainDrawEnabled(bool)` / `gos_GetTerrainDrawEnabled()`
- Lets you see ONLY the objects (mechs, buildings) without terrain occluding the view

### 3. Hotkeys (in mission.cpp debug key section)

- **Ctrl+Shift+S** — cycle: off → show static shadow map → show dynamic shadow map → off
- **Ctrl+T** — toggle terrain drawing on/off

Both print status to stderr for console confirmation.

### 4. AMD Considerations

- Shadow depth textures use `GL_COMPARE_REF_TO_TEXTURE` for normal PCF sampling
- Debug visualizer temporarily switches to `GL_NONE` to read raw depth
- Must restore compare mode after debug draw — failing to do so breaks all shadow sampling
- Debug shader uses regular `sampler2D`, not `sampler2DShadow`

## Files

| File | Change |
|------|--------|
| `shaders/shadow_debug.frag` | NEW — depth visualization with diagnostic colors |
| `GameOS/gameos/gos_postprocess.h` | Add debug shader pointer, flags, overlay method |
| `GameOS/gameos/gos_postprocess.cpp` | Init/destroy debug shader, implement overlay, call from endScene |
| `GameOS/gameos/gameos_graphics.cpp` | Add terrainDrawEnabled toggle + API functions |
| `GameOS/include/gameos.hpp` | Declare terrain draw toggle + shadow debug toggle APIs |
| `code/mission.cpp` | Add Ctrl+Shift+S and Ctrl+T hotkeys |

## Success Criteria

- Shadow debug quad visible in top-left corner showing depth texture
- Magenta = nothing written, grayscale = depth values, red = near clip
- Can toggle between static and dynamic shadow maps
- Can toggle terrain off to isolate object rendering
- Tracy zones confirm dynamic shadow pass is executing
