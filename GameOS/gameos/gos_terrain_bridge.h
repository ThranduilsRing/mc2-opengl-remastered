// GameOS/gameos/gos_terrain_bridge.h
//
// Tiny C-style accessor bridge so TerrainPatchStream (defined in its own
// .cpp) can reach gosRenderer state without #including gameos_graphics.cpp
// internals. Each function is implemented in gameos_graphics.cpp where the
// full gosRenderer type is visible.

#pragma once

// Forward decls for opaque pointer types used in signatures.
class gosRenderMaterial;

// Returns the active terrain material (the one terrainDrawIndexedPatches
// uses). Live pointer — do NOT cache across frames.
gosRenderMaterial* gos_terrain_bridge_getMaterial();

// Returns the GL program ID of the terrain material's currently-applied
// shader. Used by flush() to look up worldPos / worldNorm attribute
// locations once and cache them, avoiding the per-draw glGetAttribLocation
// stall. Returns 0 if no terrain material is resident yet.
unsigned int gos_terrain_bridge_getShaderProgram();

// Sets every direct uniform + texture bind that terrainDrawIndexedPatches
// sets EXCEPT the per-batch VBO upload. Call once per flush() before
// issuing per-bucket glDrawArrays. The function internally calls
// material->apply() (which calls glUseProgram), so direct glUniform*
// calls inside it are AFTER apply() per AMD rule line 10.
void gos_terrain_bridge_bindUniforms(gosRenderMaterial* material);

// Material lifecycle helpers — needed because gosRenderMaterial is defined
// inside gameos_graphics.cpp and is not visible from gos_terrain_patch_stream.cpp.
// flush() calls these after issuing per-bucket glDrawArrays.
void gos_terrain_bridge_applyVertexDeclaration(gosRenderMaterial* material);
void gos_terrain_bridge_endVertexDeclaration(gosRenderMaterial* material);
void gos_terrain_bridge_end(gosRenderMaterial* material);

// Translate engine gosTextureHandle → actual GL texture object name.
//
// CRITICAL: tex_resolve(textureIndex) returns the engine's gosTextureHandle
// (e.g. 56), NOT the GL texture name. They are NOT the same number. The
// engine's applyRenderStates() at gameos_graphics.cpp:2129–2135 always
// converts gos→GL via getTexture(gosHandle)->getTextureId() before
// glBindTexture. PatchStream must do the same.
//
// Returns 0 (the default no-texture) for INVALID_TEXTURE_ID, gosHandle==0,
// or any case where the texture isn't resident.
unsigned int gos_terrain_bridge_glTextureForGosHandle(unsigned int gosHandle);

// Renderer-owned PatchStream bucket draw. Binds the colormap for gosHandle
// on unit 0 and issues the draw. Does NOT touch the gosRenderer render-state
// cache — terrain state was established once by gos_terrain_bridge_bindUniforms.
void gos_terrain_bridge_drawPatchStreamBucket(
    unsigned int gosHandle,
    unsigned int firstVertex,
    unsigned int vertexCount);

// Call once before the per-bucket draw loop to set render states that are
// invariant across all buckets (ZCompare, ZWrite, AlphaMode, TextureAddress,
// Terrain). Only gos_State_Texture changes per bucket. glActiveTexture is
// set in drawSingleBucket() AFTER applyRenderStates(), matching the ordering
// of the original drawPatchStreamBucket() to prevent AMD driver clobbering.
// Does NOT call applyRenderStates() — the first drawSingleBucket() flushes
// all pending dirty flags including these invariants.
void gos_terrain_bridge_beginBucketLoop();

// Per-bucket draw call: sets gos_State_Texture for gosHandle, calls
// applyRenderStates(), issues glDrawArrays(GL_PATCHES, firstVertex, count).
// Call gos_terrain_bridge_beginBucketLoop() exactly once before the loop.
void gos_terrain_bridge_drawSingleBucket(
    unsigned int gosHandle,
    unsigned int firstVertex,
    unsigned int vertexCount);

// Call once after the per-bucket draw loop when MC2_PATCHSTREAM_DIRECT_TEXTURE_BIND
// is set. Unbinds the terrain sampler object from unit 0, then re-syncs the
// render-state cache (one applyRenderStates + glActiveTexture(GL_TEXTURE0)) so
// subsequent renderers see coherent state. Pass 0xFFFFFFFFu if no draws were
// issued (empty bucket loop); any other value (including 0 for evicted textures)
// triggers the cache sync. No-op when the fast path env var is not set.
void gos_terrain_bridge_endBucketLoop(unsigned int lastGosHandle);

// Returns the GL program ID of the thin-record VS-only terrain shader (gos_terrain_thin.vert +
// gos_terrain.frag). Returns 0 if not yet loaded or failed to compile.
unsigned int gos_terrain_bridge_getThinShaderProgram();

// Switches the active GL program to the thin terrain program and sets all uniforms
// (projection, cameraPos, shadow maps, PBR params — same as the tessellation path minus tess-only).
// Returns the ssboRecordBase uniform location in the thin program (for per-bucket setting),
// or -1 if the thin program is not ready.
// IMPORTANT: call gos_terrain_bridge_beginBucketLoop() first to dirty the Z/blend states.
int gos_terrain_bridge_bindThinUniforms();

// Like gos_terrain_bridge_drawSingleBucket but issues glDrawArrays(GL_TRIANGLES, ...).
// Binds gosHandle texture, calls applyRenderStates(), then draws. Does NOT change glUseProgram —
// assumes gos_terrain_bridge_bindThinUniforms() was already called.
void gos_terrain_bridge_drawSingleBucketTriangles(
    unsigned int gosHandle,
    unsigned int firstVertex,
    unsigned int vertexCount);

// --- Water fast-path bridge (Stage 2 of renderWater architectural slice) ---
//
// Spec: docs/superpowers/specs/2026-04-29-renderwater-fastpath-design.md.
// Recipe SSBO: GameOS/gameos/gos_terrain_water_stream.h.
// Shader pair: gos_terrain_water_fast.vert + gos_tex_vertex.frag.

// Returns GL program ID for the water-fast VS + gos_tex_vertex.frag shader.
// Returns 0 if not loaded or failed to compile.
unsigned int gos_terrain_bridge_getWaterFastShaderProgram();

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
