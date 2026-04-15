# World-Space Overlay Rewrite — Design Prompt

## Goal

Replace the 25-year-old CPU-workaround overlay system used to render cement/runway perimeter tiles, solid cement, craters, and mech footprints with a clean GPU-native world-space draw path. Eliminate all IS_OVERLAY machinery, the broken stencil/GBuffer1 post-process shadow route, and the `terrainMVP` re-projection hack.

## What goes through the overlay path today

The audit (April 2026) identified four distinct draw types all sharing the same `MC2_ISCRATERS` flag and `rhw=1.0` world-coord trick:

| Type | Source | Flags | Notes |
|------|--------|-------|-------|
| Alpha cement (perimeter/transition tiles) | `quad.cpp` ~318 | `MC2_ISTERRAIN\|MC2_DRAWALPHA\|MC2_ISCRATERS` | Cement type mixed with grass/dirt corner |
| Solid cement (pure interior quads via overlay) | `quad.cpp` ~330 | `MC2_ISTERRAIN\|MC2_ISCRATERS` | No alpha blend needed |
| Bomb craters | `crater.cpp` ~285 | `MC2_ISCRATERS\|MC2_DRAWALPHA\|MC2_ISTERRAIN` | 32×32px decal, texture clamp |
| Mech footprints | `crater.cpp` ~291 | `MC2_ISCRATERS\|MC2_DRAWALPHA` | 16×16px decal, texture clamp |

All four go through `Overlay.SplitDraw` in `gameos_graphics.cpp` (~line 2935), all use the IS_OVERLAY shader variant, and all suffer from the same broken stencil shadow path.

---

## Background — why it's bad

MC2's original renderer was fully 2D-projected on the CPU. To render overlay tiles on top of terrain, it reused the 2D quad vertex format and smuggled world coordinates through `gos_VERTEX.rhw = 1.0f` as a flag bit.

The current OpenGL port preserves all of this:

### CPU side
- `setOverlayWorldCoords()` (`quad.cpp` ~1452) — writes raw MC2 world coords into `gos_VERTEX.x/y/z`, sets `rhw = 1.0f`
- Cement/crater verts submitted via `gos_drawIndexedTriangles` with overlay flags → queued into the material batch system

### GPU / gameos side
- `Overlay.SplitDraw` (`gameos_graphics.cpp` ~2935) — triggered by `curStates_[gos_State_Overlay] && terrain_mvp_valid_`:
  - Injects `terrainMVP` + `terrainViewport` so the vertex shader can convert world → screen pixels → NDC (3-step chain)
  - Injects shadow uniforms via `gos_SetupObjectShadows(mat)` and a `time` direct GL call after `apply()`
  - Writes `stencil=1` per pixel (intended for post-process shadow pass — never worked reliably)
  - Does NOT write depth
- `clearOverlayAlpha` (`gos_postprocess.cpp` ~611) — tries to zero GBuffer1.alpha for stencil=1 pixels so `shadow_screen` can shadow them; broken because `sceneFBO_` is likely not bound during `SplitDraw`

### Shader side
- `gos_tex_vertex.vert` IS_OVERLAY — detects `abs(pos.w - 1.0) < 0.0001`, re-projects via terrainMVP
- `gos_tex_vertex.frag` IS_OVERLAY — tone correction + inline calcShadow (cloud shadow range artificially narrowed to 0.925 to compensate for missing pureConcrete normalLight boost)

### What's broken / janky
- Stencil shadow route has never worked (overlays show as terrain/brown in shadow debug)
- Cloud shadow range must be tuned differently from terrain due to luminance mismatch — fragile
- `rhw` flag detection is opaque and error-prone
- Every new effect on overlay tiles requires duplicating shader logic from gos_terrain.frag

---

## Proposed replacement: unified world-space decal batch

All four overlay types are flat horizontal quads in world space. Draw them as plain 3D geometry in two batches after terrain:

1. **Terrain overlays** (cement edges, solid cement) — opaque, depth-write on, full terrain-style shading
2. **Decals** (craters, footprints) — alpha-blended, depth-write off, depth-test on, texture clamp

### New shaders

**`terrain_overlay.vert`** — same for both batches:
```glsl
layout(location=0) in vec3 worldPos;
layout(location=1) in vec2 texcoord;
layout(location=2) in vec4 color;

uniform mat4 viewProj;

out vec3 WorldPos;
out vec2 Texcoord;
out vec4 Color;

void main() {
    WorldPos = worldPos;
    Texcoord = texcoord;
    Color = color;
    gl_Position = viewProj * vec4(worldPos, 1.0);
}
```

No `terrainMVP`, no `terrainViewport`, no `rhw` detection.

**`terrain_overlay.frag`** (cement tiles):
- Texture + vertex color
- Tone correction (same as current IS_OVERLAY but without the luminance compensation hacks)
- `calcShadow` + `calcDynamicShadow` via `#include <include/shadow.hglsl>`
- Cloud shadow FBM full range `mix(0.70, 1.0, ...)` — luminance now matches interior since both go through the same pipeline
- Fog
- MRT write: GBuffer1 `alpha=1.0` (terrain flag) so `shadow_screen` skips it

**`decal.frag`** (craters, footprints):
- Texture sample with alpha blend
- No tone correction — decal textures are authored to look correct as-is
- `calcShadow` + `calcDynamicShadow` (craters in shadow of buildings should be darker)
- Cloud shadow (subtle — craters are dark already)
- Fog
- MRT write: GBuffer1 `alpha=1.0` (terrain flag, same skip logic)

### CPU side changes

**`mclib/quad.cpp`**

Replace `setOverlayWorldCoords()` + `gos_drawIndexedTriangles(overlay flags)` with direct pushes to two new plain vertex arrays:

```cpp
struct WorldDecalVert { float x, y, z, u, v; uint32_t color; };

// In gameos or a shared header:
extern void pushTerrainOverlayVert(WorldDecalVert v);  // cement
extern void pushDecalVert(WorldDecalVert v, GLuint texId);  // crater/footprint
```

The key change: world coords go directly into a typed buffer — no `gos_VERTEX`, no `rhw`, no material state flags.

**`mclib/crater.cpp`**

Replace `gos_drawIndexedTriangles(MC2_ISCRATERS | MC2_DRAWALPHA ...)` calls (~285, ~291, ~545, ~550) with `pushDecalVert()`.

**`GameOS/gameos/gameos_graphics.cpp`**

- Add `TerrainOverlayBatch` and `DecalBatch` (VBO + index buffer, grown dynamically, reset each frame)
- Render order after terrain draw:
  1. `drawTerrainOverlays()` — opaque, depth-write on, same render state as terrain
  2. `drawDecals()` — alpha blend, depth-write off, polygon offset to prevent z-fighting
- Delete `Overlay.SplitDraw` entirely (or reduce to non-terrain overlays if mission markers still need it — investigate first)
- Delete `gos_SetupObjectShadows` IS_OVERLAY injection (the `time` + shadow uniform block)

**`GameOS/gameos/gos_postprocess.cpp`**

- Delete `clearOverlayAlpha` — no longer needed

### What gets deleted

- `gos_tex_vertex.frag` IS_OVERLAY block (`#ifdef IS_OVERLAY ... #endif` in both vert and frag)
- `gos_tex_vertex.vert` IS_OVERLAY block
- `Overlay.SplitDraw` in `gameos_graphics.cpp` (~80 lines)
- `clearOverlayAlpha` in `gos_postprocess.cpp`
- `terrainMVP` / `terrainViewport` uniforms from `gos_tex_vertex.vert`
- `OverlayUsesWorldPos` / `MC2WorldPos` varyings
- `time` glUniform1f injection in Overlay.SplitDraw
- The `rhw=1.0` detection logic throughout

### What to keep / investigate

- **Mission markers / objective indicators** — unknown whether they use `rhw=1.0` / `gos_State_Overlay`. Audit before deleting `Overlay.SplitDraw`. If they do, they're a candidate for the same treatment (or a simple billboard batch).
- **`gos_tex_vertex` non-overlay path** — water animation, misc quads. Not affected by this rewrite.
- **`gos_State_Overlay`** state flag — can be removed once all callers are migrated.

---

## Render order after rewrite

```
drawTerrain()           — tessellated terrain, gos_terrain.frag, depth-write on
drawTerrainOverlays()   — cement edges + solid cement, terrain_overlay.frag, depth-write on
drawDecals()            — craters + footprints, decal.frag, alpha blend, depth-write off
draw3DObjects()         — mechs, buildings, etc. (unchanged)
post-process stack      — shadow_screen, bloom, etc. (clearOverlayAlpha removed)
drawHUD()
```

---

## Files to touch

| File | Change |
|------|--------|
| `shaders/terrain_overlay.vert` | New — simple world-space vertex shader (shared by both batches) |
| `shaders/terrain_overlay.frag` | New — terrain-style frag for cement tiles |
| `shaders/decal.frag` | New — alpha-blend frag for craters/footprints |
| `mclib/quad.cpp` | Replace overlay submission with `pushTerrainOverlayVert()` |
| `mclib/crater.cpp` | Replace overlay submission with `pushDecalVert()` |
| `GameOS/gameos/gameos_graphics.cpp` | Add two new batches + draw calls; delete Overlay.SplitDraw |
| `GameOS/gameos/gos_postprocess.cpp` | Delete `clearOverlayAlpha` |
| `shaders/gos_tex_vertex.vert` | Remove IS_OVERLAY variant |
| `shaders/gos_tex_vertex.frag` | Remove IS_OVERLAY variant |

---

## Session context to read first

Before starting implementation, read:
- `docs/architecture.md` — render pipeline order, coordinate spaces
- `docs/cement-overlay-codemap.md` — current data flow with line numbers (some details now superseded by this doc)
- `mclib/quad.cpp` lines 300-360 (cement submission) and ~1440-1580 (`setOverlayWorldCoords`, vertex color assignment)
- `mclib/crater.cpp` lines 280-556 (crater/footprint submission and render)
- `mclib/txmmgr.cpp` lines 1229-1394 (overlay render dispatch — to understand what to delete)
- `GameOS/gameos/gameos_graphics.cpp` lines 2935-3020 (`Overlay.SplitDraw`)
- `shaders/gos_tex_vertex.vert` + `.frag` IS_OVERLAY sections

Worktree: `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
Deploy: `A:/Games/mc2-opengl/mc2-win64-v0.1.1/`
Build: `--config RelWithDebInfo --target mc2`
