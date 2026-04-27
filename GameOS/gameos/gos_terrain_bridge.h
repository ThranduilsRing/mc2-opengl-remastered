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

// Returns the legacy terrain_extra_vb_ GL buffer ID. Used by
// TerrainPatchStream::flush() to issue the single consolidated per-frame
// updateBuffer for grass + any legacy extras reader.
unsigned int gos_terrain_bridge_getExtraVB();

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
