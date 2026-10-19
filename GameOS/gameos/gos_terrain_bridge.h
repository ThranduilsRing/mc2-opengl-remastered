// GameOS/gameos/gos_terrain_bridge.h
//
// Tiny C-style accessor bridge so TerrainPatchStream (defined in its own
// .cpp) can reach gosRenderer state without #including gameos_graphics.cpp
// internals. Each function is implemented in gameos_graphics.cpp where the
// full gosRenderer type is visible.

#pragma once

#include <cstdint>  // uint32_t — used by B4 mask-SOLID bridge signature.

// --- Water fast-path bridge (Stage 2 of renderWater architectural slice) ---
//
// Spec: docs/superpowers/specs/2026-04-29-renderwater-fastpath-design.md.
// Recipe SSBO: GameOS/gameos/gos_terrain_water_stream.h.
// Shader pair: gos_terrain_water_fast.vert + gos_tex_vertex.frag.

// gos_terrain_bridge_getWaterFastShaderProgram deleted by DEAD-BRIDGE-DELETE-1:
// zero external callers tree-wide (dead wrapper around live getWaterFastProgram()).

// Issue the water fast path. Bumps the active program to the water-fast
// shader, sets all uniforms (projection chain, mission-stable + per-frame),
// binds SSBOs at bindings 5/6 (recipe + frame), draws base + (optional)
// detail layer. Saves and restores depthMask/blend/program state.
//
// Inputs:
//   recordCount        — N (number of WaterRecipe entries to draw; 6 verts each)
//   waterGosHandle     — engine gosHandle for the base water texture
//   waterDetailGosHandle — engine gosHandle for the spray/detail texture
//                          (0xffffffff to skip detail pass)
//   waterElevation     — Terrain::waterElevation
//   alphaDepth         — MapData::alphaDepth
//   alphaEdgeByte/MiddleByte/DeepByte — alpha bytes pulled from
//       Terrain::alpha{Edge,Middle,Deep} >> 24
//   mapTopLeftX/Y      — Terrain::mapTopLeft3d.x/.y
//   frameCos, frameCosAlpha — Terrain::frameCos / frameCosAlpha (per-frame)
//   oneOverTF, oneOverWaterTF — UV scales for base/detail
//   cloudOffsetX/Y     — base UV offset
//   sprayOffsetX/Y     — detail UV offset
//   maxMinUV           — UV wrap floor
// Slice A — cement-overlay (decal) static-bake draw bridge.
//
// Called by gos_terrain_indirect::DrawDecalStatic from the
// Render.TerrainOverlaysStatic zone in txmmgr.cpp. Reproduces the EXACT
// drawTerrainOverlays() state block + overlay shader/uniforms/VAO, but
// draws the mission-static decal VBO with per-overlayTexId draw ranges
// instead of the per-frame M2d batch, and does NOT clear it (mirrors
// DrawMineStatic semantics).
//
// One draw range = { texHandle, firstVert, vertCount } (mirror of
// gameos_graphics.cpp OverlayBatchEntry_). draws points at an array of
// drawCount such PODs; vboGL is the GL_ARRAY_BUFFER name holding
// WorldOverlayVert verts (28-byte stride). Returns false if the overlay
// program is not loaded or input is empty.
struct GosDecalStaticDraw {
    unsigned int texHandle;
    unsigned int firstVert;
    unsigned int vertCount;
};
bool gos_terrain_bridge_drawDecalStatic(unsigned int               vboGL,
                                        const GosDecalStaticDraw*  draws,
                                        int                        drawCount);

void gos_terrain_bridge_renderWaterFast(
    unsigned int recordCount,
    unsigned int waterGosHandle,
    unsigned int waterDetailGosHandle,
    float waterElevation,
    float alphaDepth,
    unsigned int alphaEdgeByte,
    unsigned int alphaMiddleByte,
    unsigned int alphaDeepByte,
    float mapTopLeftX,
    float mapTopLeftY,
    float frameCos,
    float frameCosAlpha,
    float oneOverTF,
    float oneOverWaterTF,
    float cloudOffsetX,
    float cloudOffsetY,
    float sprayOffsetX,
    float sprayOffsetY,
    float maxMinUV);
