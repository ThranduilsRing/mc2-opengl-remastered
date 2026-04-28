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
