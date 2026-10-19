// mech_skel_import.h — shared skeleton/FK importer math (MECH-IMPORT-HARNESS-1C).
//
// ONE math path for both the standalone harness and the in-game importer. If the
// harness `gpu-bones` output is green, the game receives the identical bone
// matrices, because both call THESE functions.
//
// Assimp types appear here (aiScene*) — this is an INTERNAL importer header,
// included only by assimp_importer.cpp (game) and mech_import_harness.cpp
// (tool). It must NEVER be included by TG headers (tgl.h/msl.h): the invariant
// "Assimp types do not leak into TG/engine headers" is preserved.
//
// Output is plain data (row-major float[16]) so callers convert to their own
// matrix/GpuMechBone type without depending on Assimp.
#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

struct aiScene;  // fwd; full def only in the .cpp

namespace mc2skel {

// Per-bone JOINT GLOBAL (FK world) matrix at a given pose. Row-major (matches
// Assimp aiMatrix4x4 memory order and the engine's GpuMechBone upload layout).
//
// NOTE: this is the joint global ONLY, not global*inverseBind. The BT2018 rig
// stores a different inverse-bind per mesh-part for the same bone (verified:
// 19/28 bones have 2 distinct offsets across skins), so inverse-bind cannot be
// folded into one per-bone matrix. The importer bakes each part's own offset
// into its vertices; runtime skin = GpuBone(joint) * (offset_part * vertex). For
// the GPU this is the standard skinning matrix once verts are in bone-local space.
struct GpuBone { float m[16]; };

// Ordered, de-duplicated set of bones referenced by any mesh in the scene.
// parentIndex[i] = index (into these same arrays) of the nearest ancestor that
// is also a skinning bone, or -1 if its parent chain leaves the bone set.
// inverseBind[i] = the bone's offset matrix (row-major).
void BuildSkeleton(const aiScene* scene,
                   std::vector<std::string>& boneNames,
                   std::vector<int>& parentIndex,
                   std::vector<std::array<float, 16>>& inverseBind);

// Evaluate `clip` at `frame` (interpreted at 30 fps) and return per-bone skin
// matrices parallel to `boneNames`. Bones not animated by the clip fall back to
// their rest (node default) transform. Returns false if the clip is missing.
// outTimeTicks/outDurTicks (optional) report the sampled time and clip length.
bool EvaluateClipGpuBones(const aiScene* scene,
                          std::string_view clip,
                          float frame,
                          const std::vector<std::string>& boneNames,
                          std::vector<GpuBone>& out,
                          double* outTimeTicks = nullptr,
                          double* outDurTicks = nullptr);

// Rest pose (no animation): per-bone joint-global from node default transforms,
// parallel to boneNames. Used for bind-pose-parity import (bake each part's own
// offset into its vertices, then world = restGlobal(bone) * (offset * vertex)).
void EvaluateRestGpuBones(const aiScene* scene,
                          const std::vector<std::string>& boneNames,
                          std::vector<GpuBone>& out);

}  // namespace mc2skel
