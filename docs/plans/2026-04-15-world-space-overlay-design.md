# World-Space Overlay Rewrite — Refined Design
_Brainstormed 2026-04-15. Supersedes the high-level prompt in `cement-overlay-rewrite.md`._

## Goal

Replace the IS_OVERLAY / `rhw=1.0` / terrainMVP workaround stack for alpha cement tiles, bomb craters, and mech footprints with a clean GPU-native world-space draw path using typed vertex buffers and dedicated draw calls outside the `mcTextureManager` batch system.

## Scope (nifty-mendeleev branch)

| Type | Current path | New path |
|------|-------------|----------|
| Alpha cement (perimeter/transition) | `MC2_ISTERRAIN\|MC2_DRAWALPHA\|MC2_ISCRATERS` → IS_OVERLAY shader | `TerrainOverlayBatch` → `terrain_overlay.frag` |
| Bomb craters | `MC2_ISCRATERS\|MC2_DRAWALPHA\|MC2_ISTERRAIN` → IS_OVERLAY shader | `DecalBatch` → `decal.frag` |
| Mech footprints | `MC2_ISCRATERS\|MC2_DRAWALPHA` → IS_OVERLAY shader | `DecalBatch` → `decal.frag` |
| Solid cement | `MC2_DRAWSOLID` → gos_terrain.frag (tessellated) | **unchanged — already correct** |
| Water | non-overlay gos_tex_vertex path | **unchanged** |

## Approach: Typed buffers + keep terrainMVP (Approach B)

The overlay vertex shader keeps the `terrainMVP + terrainViewport + mvp` projection chain — the same math the TES uses, now applied unconditionally on typed world-space inputs.

**Why not switch to a pure `viewProj` matrix now:**
`worldToClip` already exists in `gamecam.cpp` and a proper `overlayViewProj = worldToClip * mc2_to_mlr_axisswap` could be precomputed. However, terrain tessellation still uses the DX-era chain. Splitting overlay and terrain onto different projection paths would create a temporary inconsistency in the z-buffer. The full OpenGL migration (terrain + overlays together) is planned as a follow-on step; at that point both switch to `viewProj` simultaneously.

## Vertex format

```cpp
// GameOS/gameos/gameos_graphics.h  (or new gosOverlay.h)
struct WorldOverlayVert {
    float wx, wy, wz;   // MC2 world space (x=east, y=north, z=elev)
    float u, v;
    float fog;          // [0,1], 1=clear, 0=full fog — matches FogValue convention
    uint32_t argb;      // BGRA, same packing as gos_VERTEX
};
```

## CPU-side API (new GameOS functions)

```cpp
// Called from mclib — replaces gos_SetRenderState(gos_State_Overlay) + gos_drawIndexedTriangles
void gos_PushTerrainOverlay(const WorldOverlayVert* verts, int count, uint32_t texHandle);
void gos_PushDecal(const WorldOverlayVert* verts, int count, uint32_t texHandle);
```

Both functions accumulate into per-frame resizable VBOs in `gosRenderer` (gameos_graphics.cpp). Buffers are reset at frame start. No `gos_VERTEX`, no `rhw`, no `gos_State_Overlay` state.

## GPU batch structures (gameos_graphics.cpp)

```cpp
struct TerrainOverlayBatch {
    GLuint vbo, ibo;
    std::vector<WorldOverlayVert> verts;
    std::vector<uint16_t> indices;
    uint32_t texHandle;
};

struct DecalEntry { uint32_t texHandle; uint16_t firstIndex, indexCount; };
struct DecalBatch {
    GLuint vbo, ibo;
    std::vector<WorldOverlayVert> verts;
    std::vector<uint16_t> indices;
    std::vector<DecalEntry> draws;
};
```

## Shaders

### `shaders/terrain_overlay.vert` (new, shared by both batches)

```glsl
layout(location=0) in vec3 worldPos;
layout(location=1) in vec2 texcoord;
layout(location=2) in vec2 fogAndPad;   // fog in .x
layout(location=3) in vec4 color;       // unpacked RGBA

uniform mat4 terrainMVP;
uniform vec4 terrainViewport;
uniform mat4 mvp;

out vec3 WorldPos;
out vec2 Texcoord;
out float FogValue;
out vec4 Color;

void main() {
    WorldPos  = worldPos;
    Texcoord  = texcoord;
    FogValue  = fogAndPad.x;
    Color     = color;

    // Same projection chain as TES — terrainMVP gives screen-pixel-space coords,
    // viewport converts to NDC, mvp finalises.  Unconditional (no rhw detection).
    vec4 clip4 = terrainMVP * vec4(worldPos, 1.0);
    float rhw  = 1.0 / clip4.w;
    vec3  px;
    px.x = clip4.x * rhw * terrainViewport.x + terrainViewport.z;
    px.y = clip4.y * rhw * terrainViewport.y + terrainViewport.w;
    px.z = clip4.z * rhw;
    vec4 ndc = mvp * vec4(px, 1.0);
    float absW = abs(clip4.w);
    gl_Position = vec4(ndc.xyz * absW, absW);
}
```

### `shaders/terrain_overlay.frag` (new — cement tiles)

- Tone correction on raw texture (same logic as current IS_OVERLAY, vertex luminance re-applied separately)
- Cloud shadow FBM **full range** `mix(0.70, 1.0, ...)` — the luminance compensation hack (`0.925`) goes away because these pixels now write GBuffer1 `alpha=1`, telling `shadow_screen` to skip them (same as interior terrain)
- `calcShadow` + `calcDynamicShadow` via `#include <include/shadow.hglsl>`
- Fog using `FogValue` varying
- MRT: `layout(location=1) out vec4 GBuffer1 = vec4(0.5, 0.5, 1.0, 1.0)` — terrain flag

### `shaders/decal.frag` (new — craters, footprints)

- Texture × vertex color, no tone correction
- `calcShadow` + `calcDynamicShadow`
- Cloud FBM (narrower range — craters are already dark)
- Fog
- MRT: GBuffer1 alpha=1

## Render order after rewrite

```
drawTerrain()            gos_terrain.frag       depth-write on
drawTerrainOverlays()    terrain_overlay.frag   depth-write on,  depth-test LEQUAL
drawDecals()             decal.frag             alpha blend,     depth-write off, polygonOffset(-1,-1)
draw3DObjects()          (unchanged)
post-process             clearOverlayAlpha REMOVED
drawHUD()
```

## What gets deleted

| Location | Item |
|----------|------|
| `shaders/gos_tex_vertex.vert` | `#ifdef IS_OVERLAY` block (terrainMVP, terrainViewport, rhw detection, MC2WorldPos, OverlayUsesWorldPos) |
| `shaders/gos_tex_vertex.frag` | `#ifdef IS_OVERLAY` block (tone correction, cloud shadow, calcShadow) |
| `GameOS/gameos/gameos_graphics.cpp` | `Overlay.SplitDraw` (~80 lines incl. diagnostics, terrainMVP upload, time uniform, stencil setup) |
| `GameOS/gameos/gos_postprocess.cpp` | `clearOverlayAlpha()` |
| `mclib/quad.cpp` | `setOverlayWorldCoords()`, IS_OVERLAY addTriangle/addVertices calls |
| `mclib/crater.cpp` | IS_OVERLAY addVertices calls |
| `mclib/txmmgr.cpp` | `Render.CraterOverlays` loop, `gos_State_Overlay` set/clear |

## Prerequisites

### Mission marker audit (step 1)

Before deleting `Overlay.SplitDraw`, audit every `gos_State_Overlay` callsite in mclib and confirm no mission-marker or UI-overlay draw goes through that path. If any do, gate them first (either migrate to a billboard batch or explicitly skip stencil marking for non-terrain draws).

The shadow-under-marker bug from earlier sessions is consistent with markers accidentally going through IS_OVERLAY — this audit may resolve that root cause.

## Relation to GPU roadmap items

This rewrite is the architectural proof-of-concept for three planned GPU improvements:

| Goal | How overlay rewrite helps |
|------|--------------------------|
| GPU-driven terrain vertex building | Establishes the typed world-space VBO + per-frame reset pattern outside mcTextureManager |
| renderLists() batch consolidation | Removes the `Render.CraterOverlays` loop; shows how draw calls can bypass the batch system |
| gosFX GPU-instanced particles | `DecalBatch` is structurally identical to a particle billboard batch; `WorldOverlayVert` extends directly |
