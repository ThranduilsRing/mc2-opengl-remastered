// assimp_importer.h — Track D Assimp-backed FBX/GLB mech importer.
//
// Public surface for the import path. When ENABLE_ASSIMP_IMPORTER is OFF the
// header still compiles (decls are gated) so call sites can include without
// `#ifdef`s; in that mode `LoadFromFile` falls through to the legacy ASE path.
//
// Spec: docs/superpowers/specs/2026-04-27-assimp-mech-importer-design.md
// Plan: docs/superpowers/plans/2026-04-27-assimp-mech-importer.md
//
// Architectural invariant: import terminates at the existing TG_TypeMultiShape
// runtime structures. Rendering is downstream and unchanged. No Assimp types
// are exposed in TG_* class headers.
#pragma once

#ifdef ENABLE_ASSIMP_IMPORTER

class TG_TypeMultiShape;

// Import geometry from a GLB or FBX file into an already-constructed
// TG_TypeMultiShape. Returns NO_ERR on success, -1 on failure.
//
// `path` is the full path to the source file (with extension). The TG_TypeMultiShape
// must be freshly constructed (init() called); the importer populates numTG_TypeShapes,
// listOfTypeShapes, listOfTextures, numTextures, and bounding box.
//
// Coordinate transform: source -> MC2 axis flip (mc2.x = -src.x, mc2.y = src.z,
// mc2.z = src.y) applied to positions and normals per Section 6 of spec.
// V-flip (v_mc2 = 1.0 - v_src) applied to UV coordinates.
//
// MVP scope (geometry-only): no animation, no LOD swap, no shadow mesh,
// no shared-gesture aliasing. Texture handles initialised to sentinel values
// (0xffffffff) and resolved later by the engine's legacy texture manager — the
// importer must NOT cache live gosTextureHandle values (memory:
// mc2_texture_handle_is_live.md).
// autoGround: when true (default), apply MC2_GLTF_GROUND base-snapping (for trees /
// pivot-centered authored meshes). Pass false for stock-derived staticprop overrides
// (cooked from .tgl) — they already carry the stock pivot; grounding would shift them.
long ImportGeometryFromFile(const char* path, TG_TypeMultiShape* out, bool autoGround = true);

#endif // ENABLE_ASSIMP_IMPORTER
