# Cement/Overlay/Shadow Codemap
Generated 2026-04-15. Line numbers may drift; use as search anchors.

## Data flow for alpha cement perimeter tiles

### Classification (mclib/terrtxm.cpp)
- `isCementType()` line 1074 — type 10 (BASE) or 13-20 (START..END)
- `TerrainTextures::getTexture()` line 1366 — if all 4 corners cement → CEMENT_FLAG only (solid)
  - If any corner is non-cement → CEMENT_FLAG | ALPHA_FLAG (perimeter/transition)
- `TerrainTextures::isCement()` line 320 — checks MC2_TERRAIN_CEMENT_FLAG
- `TerrainTextures::isAlpha()` line 325 — checks MC2_TERRAIN_ALPHA_FLAG

### Draw submission (mclib/quad.cpp)
- `terrainTypeToMaterial()` line 46 — maps terrain type to 0=Rock,1=Grass,2=Dirt,3=Concrete
  - Stored in frgb lowest byte → fog.x in shader (0-3/255 ≈ 0.0-0.012 range)
- `setOverlayWorldCoords()` line 1452 — sets rhw=1.0f → triggers OverlayUsesWorldPos=1.0 in vertex shader
- Solid cement (isAlpha=false) line 334 — drawn via gos_terrain.frag (MC2_DRAWSOLID), no overlay
- Alpha cement (isAlpha=true) line 312 — drawn as overlay: `MC2_ISTERRAIN | MC2_DRAWALPHA | MC2_ISCRATERS`
  - Terrain below drawn first with white vertex color (line 1506: forced to 0xffffffff)
  - Overlay drawn with original per-vertex lightRGB (lines 1572-1574) ← source of perimeter hue contamination
    (Fixed 2026-04-15: now uses vertexLum scalar instead of full vertex color multiply)

### Render dispatch (mclib/txmmgr.cpp)
- MC2_ISCRATERS overlays rendered with `gos_State_Overlay=1` at line 1470
- This triggers Overlay.SplitDraw in gameos_graphics.cpp

### Overlay.SplitDraw (GameOS/gameos/gameos_graphics.cpp)
- Entry condition: `curStates_[gos_State_Overlay] && terrain_mvp_valid_` at line 2935
- Writes stencil=1 for every drawn pixel: lines 2991-2994
  `glStencilFunc(GL_ALWAYS, 1, 0xFF); glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);`
- Does NOT write to depth (depthMask=GL_FALSE line 2996)
- Uses IS_OVERLAY variant of gos_tex_vertex shader

## Shadow pipeline for overlay tiles

### Double-shadow problem (2026-04-15 finding)
Overlay tiles get shadow applied TWICE:
1. **Inline**: `calcShadow()` in `gos_tex_vertex.frag` IS_OVERLAY path (line 62)
2. **Post-process**: `shadow_screen.frag` via stencil/GBuffer mechanism (see below)

Interior concrete (solid, gos_terrain.frag) gets shadow only once — from gos_terrain.frag `calcShadow()`.
Interior concrete is NOT stencil=1, its GBuffer1.alpha=1 → shadow_screen SKIPS it.

### GBuffer1 / shadow_screen pipeline (GameOS/gameos/gos_postprocess.cpp)
- `clearOverlayAlpha()` line 611:
  - Binds only GL_COLOR_ATTACHMENT1 (GBuffer1/normal buffer)
  - Stencil test: GL_EQUAL, ref=1 (only pixels where stencil=1)
  - Writes `vec4(0.5, 0.5, 1.0, 0.0)` → alpha=0 = non-terrain flag
- `runScreenShadow()` line 645:
  - Reads sceneDepthTex + sceneNormalTex (GBuffer1)
  - `isTerrain = normalData.a > 0.5` (shadow_screen.frag line 74)
  - For non-terrain pixels: reconstructs worldPos from depth, samples shadow maps, outputs darkening
  - Multiplicative blending: `GL_DST_COLOR, GL_ZERO` → multiplies scene color by shadow factor
- Execution order (endScene line 959): clearOverlayAlpha → runScreenShadow → shoreline → SSAO → ...

## Fix needed

Remove `calcShadow()` and `calcDynamicShadow()` from `gos_tex_vertex.frag` IS_OVERLAY path.
Overlays will still receive shadow from `shadow_screen.frag` (via stencil mechanism).
This eliminates double-shadow and should fix both:
- Perimeter tone too dark vs. interior
- Mission marker shadow (if marker uses IS_OVERLAY/MC2_ISCRATERS path; unclear)

If mission marker still shadowed after that fix: it is also going through IS_OVERLAY
→ receives shadow_screen darkening → to fix, must prevent stencil=1 for marker draws,
  or write GBuffer1.alpha=1 for marker pixels (requires MRT output in overlay shader).

## Mission marker rendering path (UNCLEAR)
- gos_tex_vertex_lighted.frag has NO shadow sampling (removed 2026-04-14 session)
- If marker uses lighted path: no shadow from shader, no stencil=1 write → not darkened by shadow_screen
- If marker uses IS_OVERLAY/gos_tex_vertex.frag path (via MC2_ISCRATERS + rhw=1.0):
  → receives calcShadow (inline) + shadow_screen → double-shadow
  → removing calcShadow from overlay shader: marker still darkened by shadow_screen
  → to fully fix: need to prevent stencil=1 for marker or make marker write GBuffer1.alpha=1

## Shader variants
- `gos_tex_vertex.frag` with IS_OVERLAY: cement edges, craters, potentially markers
  → has calcShadow (inline) + gets shadow_screen (via stencil)
- `gos_tex_vertex.frag` without IS_OVERLAY: water, other non-terrain quads
  → no shadow
- `gos_tex_vertex_lighted.frag`: TG_Shape 3D objects rendered via drawIndexedTris.Lighted
  → no shadow (removed 2026-04-14)
- `gos_terrain.frag`: tessellated terrain (solid concrete, grass, etc.)
  → inline calcShadow, GBuffer1.alpha=1 → shadow_screen skips these pixels
