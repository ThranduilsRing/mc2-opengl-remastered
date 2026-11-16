// assimp_importer.cpp — Track D Assimp-backed FBX/GLB mech importer.
//
// Geometry-only MVP slice. Populates TG_TypeMultiShape / TG_TypeShape from a
// .glb (preferred) or .fbx source, identically to what ParseASEFile produces
// for the same geometry. Animation, LOD swap, shadow mesh, palette swap, and
// .tglc cache are all M2 — see findings doc.
//
// Architectural invariant: import terminates at TG_TypeMultiShape. Renderer is
// downstream and unchanged. No Assimp types in any TG header.
//
// Spec: docs/superpowers/specs/2026-04-27-assimp-mech-importer-design.md
// Plan: docs/superpowers/plans/2026-04-27-assimp-mech-importer.md
// Findings: docs/superpowers/explorations/2026-05-02-track-d-mvp-adversarial-findings.md
#include "mc2_czstring.h"
#include "assimp_importer.h"
#include "unit_anim_state.h"
#include "content_ir.h"
#include "mech_texname_derive.h"  // GLB-TEXNAME-DERIVE-EXTRACT-1 pure name rules
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include "mc2fmt.h"
#include "diagnostic_trace.h"

#ifdef ENABLE_ASSIMP_IMPORTER

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/material.h>
#include <assimp/IOSystem.hpp>
#include <assimp/IOStream.hpp>

#include <memory>
#include <tuple>
#include <vector>
#include <string>
#include <string_view>
#include <set>
#include <map>
#include <unordered_map>
#include <array>
#include <cstring>
#include <cmath>
#include <limits>
#include <cstdio>
#include <algorithm>

namespace {
// Source resolution belongs to the frontend: Assimp must see the same mod/FST bytes
// as stock loading and cache identity, including external glTF buffers.
class ContentStream final : public Assimp::IOStream {
public:
    std::vector<uint8_t> bytes;
    size_t position = 0;
    size_t Read(void* buffer, size_t size, size_t count) override {
        if (!size) return 0;
        count = std::min(count, (bytes.size() - position) / size);
        std::memcpy(buffer, bytes.data() + position, count * size);
        position += count * size;
        return count;
    }
    size_t Write(const void*, size_t, size_t) override { return 0; }
    aiReturn Seek(size_t offset, aiOrigin origin) override {
        const int64_t base = origin == aiOrigin_CUR ? static_cast<int64_t>(position) :
            (origin == aiOrigin_END ? static_cast<int64_t>(bytes.size()) : 0);
        const int64_t delta = static_cast<int64_t>(offset);
        if (delta < -base || delta > static_cast<int64_t>(bytes.size()) - base) return aiReturn_FAILURE;
        position = static_cast<size_t>(base + delta); return aiReturn_SUCCESS;
    }
    size_t Tell() const override { return position; }
    size_t FileSize() const override { return bytes.size(); }
    void Flush() override {}
};

class ContentIO final : public Assimp::IOSystem {
public:
    std::vector<std::pair<std::string, uint64_t>> dependencies;
    bool Exists(mc2::czstring path) const override { return fileExists(path) != 0; }
    char getOsSeparator() const override { return '/'; }
    Assimp::IOStream* Open(mc2::czstring path, mc2::czstring mode) override {
        if (!mode || mode[0] != 'r') return nullptr;
        File source;
        if (source.open(path) != NO_ERR) return nullptr;
        if (source.fileSize() > (256u << 20)) {
            mc2::fmt::print_err("[TREE_IMPORT] error: {} is {} bytes, over the {} byte import limit; the override falls back to stock\n",
                mc2::fmt::safe(path), mc2::fmt::as_u64(source.fileSize()), mc2::fmt::as_u64(256u << 20));
            return nullptr;
        }
        auto stream = std::make_unique<ContentStream>();
        stream->bytes.resize(source.fileSize());
        if (!stream->bytes.empty() && source.read(stream->bytes.data(), static_cast<long>(stream->bytes.size())) !=
            static_cast<long>(stream->bytes.size())) return nullptr;
        const uint64_t digest = mc2::content::sourceBytesDigest(path, stream->bytes);
        const std::pair<std::string, uint64_t> identity{path, digest};
        if (std::find(dependencies.begin(), dependencies.end(), identity) == dependencies.end()) dependencies.push_back(identity);
        return stream.release();  // Assimp takes ownership until Close.
    }
    void Close(Assimp::IOStream* stream) override { std::unique_ptr<Assimp::IOStream> owner(stream); }
};

static std::string ContentFloatKey(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return std::to_string(bits);
}
}
#include <optional>
#include <ranges>
#include <algorithm>

#include "msl.h"
#include "tgl.h"
#include "mech_skel_import.h"  // BT2018-SKEL-ENGINE-1A: shared FK/skeleton math
#include "mech_anim_preview.h"
#include "mech_anim_runtime.h" // BT2018-SKEL-ENGINE-1B-RUNTIME: per-frame re-bake API
#include "file.h"              // ModCapabilityMounted (package capability)
#include "mech_clip_state.h"   // P9: gesture -> clip state machine + stride matching
#include "mech_grounding.h"

// BT2018-SKEL-ENGINE-1A — bind-pose bake trace (default OFF). MC2_MECH_SKEL_TRACE=1.
static const bool s_mechSkelTrace = false;
#define MECH_SKEL_TRACE(fmt, ...) \
    do { if (s_mechSkelTrace) { \
        fprintf(stderr, "[MECH_SKEL] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); } } while (0)


namespace {

//-----------------------------------------------------------------------------
// Coordinate transforms (spec §6).
//
// glTF is Y-up, right-handed (X-right, Y-up, Z-toward-viewer). The engine's
// world-up in Stuff space is stuff.Y (stock ASE trees load their up axis into
// position.y; the static_prop shader maps stuff.z->GL.up via MC2). The previous
// mapping (mc2.y=src.z, mc2.z=src.y) was written for an ASE/Max Z-up source and
// put the glTF up-axis (Y) into stuff.z -> every imported override mesh rendered
// LYING ON ITS SIDE. Correct mapping for Y-up glTF: up (Y) -> stuff.y. We negate
// X AND Z (two axis flips) so triangle winding / handedness is preserved (a
// single flip would invert winding and backface-cull the mesh).
// MC2_GLTF_AXIS (0..3) selects the axis mapping at runtime so orientation can be
// dialed in-game without rebuilds; MC2_GLTF_YOFF nudges the up component (stuff.y)
// to lift a buried mesh (pivot-not-at-base). All four mappings are even-parity
// (winding preserved). Bake the winning combo as the default once confirmed.
static inline int s_gltfAxis() { return mc2::gate::get<"MC2_GLTF_AXIS">(); }
static inline float s_gltfYoff() { static float o = 0.0f; return o; }
static inline float s_gltfYawDeg() { return mc2::gate::get<"MC2_GLTF_YAW_DEG">(); }
static inline void applyYaw(float& X, float& Z) {
	const float yaw = s_gltfYawDeg();
	if (fabsf(yaw) < 0.001f)
		return;
	const float r = yaw * 0.017453292519943295769f;
	const float c = cosf(r);
	const float s = sinf(r);
	const float x = X * c - Z * s;
	const float z = X * s + Z * c;
	X = x;
	Z = z;
}
static inline void axisMap(const aiVector3D& v, float& X, float& Y, float& Z) {
	switch (s_gltfAxis()) {
	default:
	case 0: X=-v.x; Y=-v.y; Z= v.z; break;  // -x,-y,z
	case 1: X=-v.x; Y= v.z; Z= v.y; break;  // -x,z,y (original 'on-side')
	case 2: X=-v.x; Y= v.y; Z=-v.z; break;  // -x,y,-z ('upside down')
	case 3: X=-v.x; Y=-v.z; Z=-v.y; break;  // -x,-z,-y
	}
	applyYaw(X, Z);
}
inline glm::vec3 toMC2Pos(const aiVector3D& v) {
	glm::vec3 p; float X,Y,Z; axisMap(v,X,Y,Z);
	p.x = X; p.y = Y + s_gltfYoff(); p.z = Z;
	return p;
}
inline glm::vec3 toMC2Vec(const aiVector3D& v) {
	glm::vec3 n; float X,Y,Z; axisMap(v,X,Y,Z);
	n.x = X; n.y = Y; n.z = Z;   // normals: no translation
	return n;
}
// UV V-flip (spec §6).
inline float toMC2V(float v) { return 1.0f - v; }

//-----------------------------------------------------------------------------
// BT2018-SKEL-ENGINE-1A — bind-pose bake context.
//
// When the source GLB is skinned (mech with a skeleton), each mesh-part's
// vertices live in bone-local space and only the skeleton assembles them. The
// rig stores a DIFFERENT inverse-bind per part for the same bone (proven in the
// harness), so we bake each part's OWN offset into its vertices at import, then
// apply the joint GLOBAL rest transform: v_world = restGlobal(bone) * offset * v
// (all in glTF space, BEFORE toMC2Pos). Result = the assembled bind pose, the
// same geometry the Python fk_bake static export produced. Animation is M2.
//
// Buildings/props/trees import with NO bones -> bake disabled -> unchanged path.
// BT2018-SKEL-ENGINE-1B-RUNTIME — per-part record for the per-frame re-bake.
// Captures, during the one-time merge, exactly what TickImportedMechs needs to
// re-pose this part from a clip global each frame: which source mesh, which bone
// (index into SkelBake::names), that part's bone offset, and where the part's
// vertices landed in the merged listOfTypeVertices. boneIndex < 0 => rigid stray
// (no bone) baked with scale only, mirroring PopulateMergedSkinnedShape.
struct ImportPartRec {
    unsigned meshIndex;
    int      boneIndex;
    aiMatrix4x4 off;
    unsigned vOff;
    unsigned vCount;
};

struct SkelBake {
    bool active = false;
    std::vector<std::string> names;
    std::vector<int> parents;
    std::vector<std::array<float, 16>> invBind;
    std::vector<mc2skel::GpuBone> rest;       // joint-global rest matrices, parallel to names
    std::map<std::string, int> nameIdx;
    float scale = 1.0f;                        // auto-scale bind pose to MC2 size
    std::string forcedClip;                    // 1B: posed-clip bake (empty = rest)
    int forcedFrame = 0;
    // 1B-RUNTIME: when non-null, PopulateMergedSkinnedShape records each part's
    // layout here so the import can register a per-frame re-bake (see below).
    std::vector<ImportPartRec>* recParts = nullptr;
    // counters (trace)
    int importedParts = 0, partsSingleBone = 0, partsOffsetBaked = 0, droppedParts = 0;
};

// Apply a row-major float[16] (joint global) to an Assimp point (w=1).
inline aiVector3D applyRowMajor16(const float* m, const aiVector3D& v) {
    return aiVector3D(
        m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3],
        m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7],
        m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11]);
}
// Direction transform (3x3 part only; for normals).
inline aiVector3D rotRowMajor16(const float* m, const aiVector3D& v) {
    return aiVector3D(
        m[0] * v.x + m[1] * v.y + m[2] * v.z,
        m[4] * v.x + m[5] * v.y + m[6] * v.z,
        m[8] * v.x + m[9] * v.y + m[10] * v.z);
}

// Fixed mech orientation (glTF Y-up -> MC2), confirmed in-game (== MC2_GLTF_AXIS=2:
// X=-x, Y=+y, Z=-z) and baked as the skinned-mech default. Even parity (two flips)
// so triangle winding is preserved. Buildings/props keep the global axisMap default;
// the mech's own up-axis differs, so it carries its own fixed mapping here.
inline glm::vec3 mechToMC2Pos(const aiVector3D& v) {
    glm::vec3 p; p.x = -v.x; p.y = v.y; p.z = -v.z; return p;
}
inline glm::vec3 mechToMC2Vec(const aiVector3D& v) {
    glm::vec3 n; n.x = -v.x; n.y = v.y; n.z = -v.z; return n;
}

// Damage/destruction/UI parts excluded from the intact bind-pose import (they
// overlap the intact geometry; needed later for damage states — POC drops them).
inline bool skelMeshDropped(mc2::czstring n) {
    // GLB-TEXNAME-DERIVE-EXTRACT-1: shared with the game-free harness.
    return mech_texname::isDroppedMeshName(n);
}

//-----------------------------------------------------------------------------
// The importer binds every mesh part rigidly to ONE bone (mBones[0]) and
// stores the bone index in a byte. A skinned mesh with several influences loses its
// weights; more than 256 bones wraps. Neither is supported: say so once (diag tag CONFIG,
// default-on) instead of silently rendering a wrong pose. Stock BT2018 GLBs never trigger it.
static void noteUnsupportedSkin(std::string_view what, std::string_view meshName) {
	static bool once = false;
	if (once) return;
	once = true;
	if (mc2_diag::tagRequested("CONFIG"))
		mc2::fmt::print_err("[CONFIG] glTF import: {} (mesh '{}'): imported rigidly on one bone, per-vertex skin weights dropped; further occurrences not logged\n",
			std::string(what).c_str(), std::string(meshName).c_str());
}

//-----------------------------------------------------------------------------
// Validator. Returns -1 on hard error; 0 on pass.
// Hard errors abort the import (no cache write equivalent here for MVP):
//   - any node name longer than 24 chars (TG_NODE_ID-1 — silent truncation
//     would break animation binding; spec §5)
//   - any duplicate node name (would break node ID lookup)
//   - no renderable mesh (numMeshes == 0)
// Helper-object-style nodes (handle_*, World*) are not flagged for now —
// the importer only consumes scene->mMeshes anyway.
bool ValidateScene(const aiScene* scene, std::string_view pathView) {
	const std::string pathStore(pathView);
	const char* path = pathStore.c_str();   // printf-style diagnostics below
	if (!scene || !scene->mRootNode) {
		PAUSE(("[importer] %s: Assimp returned null scene", path));
		return false;
	}
	if (scene->mNumMeshes == 0) {
		PAUSE(("[importer] %s: no meshes", path));
		return false;
	}

	// Walk the scene graph; collect node names; flag length and duplicates.
	std::set<std::string> seen;
	std::vector<const aiNode*> stack;
	stack.push_back(scene->mRootNode);
	while (!stack.empty()) {
		const aiNode* n = stack.back();
		stack.pop_back();
		const char* name = n->mName.C_Str();
		const size_t len = strlen(name);
		// VALIDATE-SCENE-ASSIMP-NAME-LEN-1: GLB/FBX node names routinely exceed
		// the legacy TG_NODE_ID (24-char) field (e.g. BT2018 'mad_centre_torso_
		// pelvis_dmg' = 27). That length cap is an MC2/ASE runtime-field limit, not
		// a glTF constraint — rejecting valid GLB names here is wrong. Do NOT abort;
		// the importer truncates only where the name is written into the fixed-size
		// nodeId (TG_TypeShape::InitFromImportedMesh, which logs original->truncated).
		// Node names only drive animation binding (M2); bind-pose/static import does
		// not bind by name.
		if (len > 0) {
			if (const auto [seenAt, isNew] = seen.insert(name); !isNew) {
				STOP(("[importer] %s: duplicate node name '%s' (breaks node-ID lookup)",
				      path, name));
				return false;
			}
		}
		for (unsigned i = 0; i < n->mNumChildren; i++)
			stack.push_back(n->mChildren[i]);
	}

	return true;
}

//-----------------------------------------------------------------------------
// Find the aiNode that references mesh index `meshIdx`. Walks the scene graph
// recursively; returns nullopt if no node owns the mesh (rare but possible in
// malformed files — caller falls back to root-level identity).
std::optional<const aiNode*> FindNodeForMesh(const aiNode* node, unsigned meshIdx) {
	for (unsigned i = 0; i < node->mNumMeshes; i++)
		if (node->mMeshes[i] == meshIdx) return node;
	for (unsigned i = 0; i < node->mNumChildren; i++) {
		if (auto found = FindNodeForMesh(node->mChildren[i], meshIdx)) return found;
	}
	return std::nullopt;
}

//-----------------------------------------------------------------------------
// Strip leading directory components from a texture path. Assimp can return
// absolute Windows paths, relative paths, or bare filenames depending on the
// source authoring tool; legacy texture manager looks up by base name.
const char* StripPath(mc2::czstring p) {
	const char* slash1 = strrchr(p, '/');
	const char* slash2 = strrchr(p, '\\');
	const char* base = (slash1 > slash2) ? slash1 + 1 : (slash2 ? slash2 + 1 : p);
	return base;
}

//-----------------------------------------------------------------------------
// MODEL-OVERRIDE texture binding: derive an MC2 texture NAME from a glTF/FBX
// material's base-color image so legacy texture manager can resolve it by name to a
// loose data/tgl/<size>/<name>.tga (file.cpp strips the size subdir) or BC7
// .ktx2 sidecar. MC2 stores the diffuse texture name WITH its ".tga" extension
// (see msl.cpp ParseASEFile + the shadow-X strlen-4 logic). So:
//   1. resolve the material's base-color/diffuse image to a filename,
//   2. strip any directory, strip the source extension (.png/.jpg/...),
//   3. sanitize to MC2-safe chars and lowercase,
//   4. append ".tga", clamped to the TG_Texture::textureName[256] field.
// Returns false when the material has NO base-color image (caller keeps the
// "NULLTXM" sentinel — truly-untextured material).
//
// Embedded GLB images: Assimp reports the texture path as "*<index>" into
// scene->mTextures[]. We resolve that to the embedded image's mFilename so the
// derived name matches the authored image stem, not an opaque index.
std::optional<std::string> DeriveMC2TextureName(const aiScene* scene, const aiMaterial* mat) {
	aiString path;
	bool have = false;
	// glTF baseColor lands on BASE_COLOR in newer Assimp, DIFFUSE in the
	// compatibility mapping. Try both.
	if (mat->GetTexture(aiTextureType_BASE_COLOR, 0, &path) == AI_SUCCESS && path.length > 0)
		have = true;
	else if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &path) == AI_SUCCESS && path.length > 0)
		have = true;
	if (!have)
		return std::nullopt;

	const char* raw = path.C_Str();

	// Resolve embedded "*N" to the source image filename when available.
	std::string resolved;
	if (raw[0] == '*' && scene != nullptr) {
		const unsigned idx = static_cast<unsigned>(atoi(raw + 1));
		if (idx < scene->mNumTextures && scene->mTextures[idx] != nullptr
		    && scene->mTextures[idx]->mFilename.length > 0) {
			resolved = scene->mTextures[idx]->mFilename.C_Str();
		}
	}
	const char* src = resolved.empty() ? raw : resolved.c_str();

	// Alpha-cutout detection. MC2's texture loader uses an "a_" name prefix as
	// the alpha-channel convention (bdactor.cpp LoadOverrideRenderShapeTextures:
	// names starting "a_" → gos_Texture_Alpha + SetTextureAlpha(true) → the
	// static-prop batcher flags STATIC_PROP_FLAG_ALPHA_TEST for the packet).
	// A glTF leaf-card material is alphaMode MASK/BLEND; prefix "a_" so the
	// deployed RGBA TGA is loaded with its alpha channel and cuts out. Detect
	// via the glTF alphaMode key, falling back to a foliage name heuristic.
	bool wantAlpha = false;
	aiString alphaMode;
	// glTF alphaMode is exposed as the importer string key "$mat.gltf.alphaMode".
	if (mat->Get("$mat.gltf.alphaMode", 0, 0, alphaMode) == AI_SUCCESS) {
		const char* am = alphaMode.C_Str();
		if (am && (strcmp(am, "MASK") == 0 || strcmp(am, "BLEND") == 0))
			wantAlpha = true;
	}
	if (!wantAlpha) {
		aiString matName;
		if (mat->Get(AI_MATKEY_NAME, matName) == AI_SUCCESS) {
			std::string mn = matName.C_Str();
			for (auto & mnElem : mn)
				if (mnElem >= 'A' && mnElem <= 'Z') mnElem = static_cast<char>((mnElem - 'A' + 'a'));
			if (mn.find("leaf") != std::string::npos ||
			    mn.find("leaves") != std::string::npos ||
			    mn.find("foliage") != std::string::npos)
				wantAlpha = true;
		}
	}

	// GLB-TEXNAME-DERIVE-EXTRACT-1: pure stem->sanitize->clamp->.tga->a_ rules in
	// mech_texname_derive.h (same arithmetic), exercised game-free by
	// tools/mech_texname_harness/. Empty stem -> no texture (legacy returned false).
	std::string name = mech_texname::deriveName(src, wantAlpha);
	if (name.empty())
		return std::nullopt;

	return name;
}

//-----------------------------------------------------------------------------
// Build the multi-shape's TG_Texture[] from scene materials. One slot per
// Assimp material (one-to-one mapping); per-material diffuse texture name is
// extracted via aiTextureType_DIFFUSE channel 0. Materials with no diffuse
// texture get "NULLTXM" (matches ParseASEFile's empty-name fallback).
//
// Note: MVP skips ASE's Nx-base + Nx-shadow-X texture-list doubling. The
// shadow-X variant is engaged when a separate shadow shape file is present
// (existing engine path); the GLB-embedded shadow node is M2.
void BuildTextureList(const aiScene* scene, mc2::content::Asset& out) {
    for (unsigned i = 0; i < scene->mNumMaterials; ++i)
        out.materials.push_back({DeriveMC2TextureName(scene, scene->mMaterials[i]).value_or("NULLTXM"), false});
}

//-----------------------------------------------------------------------------
// Populate one TG_TypeShape from one aiMesh. Builds vertex/triangle vectors,
// then transfers ownership via the narrow construction API on TG_TypeShape.
//
// Returns 0 on success, -1 on failure.
// Older kit cooks split a physical pane at hard-normal/UV seams, giving its
// bevels independent on/off hashes. Join only coincident tagged glass vertices;
// retain the lowest authored ID and leave the render vertices untouched.
std::vector<DWORD> windowCellIds(const aiMesh& mesh) {
    std::vector<DWORD> ids(mesh.mNumVertices, 0);
    std::unordered_map<DWORD, DWORD> parent;
    std::multimap<float, std::pair<aiVector3D, DWORD>> positions;
    float scale = 1.0f;
    for (unsigned v = 0; v < mesh.mNumVertices; ++v) {
        const auto& p = mesh.mVertices[v];
        if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z))
            scale = std::max({scale, std::abs(p.x), std::abs(p.y), std::abs(p.z)});
    }
    const float tolerance = 4.0f * std::numeric_limits<float>::epsilon() * scale;
    auto root = [&](DWORD id) {
        while (parent[id] != id) { parent[id] = parent[parent[id]]; id = parent[id]; }
        return id;
    };
    for (unsigned v = 0; v < mesh.mNumVertices; ++v) {
        const float value = mesh.mTextureCoords[1][v].x;
        if (!std::isfinite(value) || value < 1.0f || value > 16777215.0f || value != std::floor(value)) continue;
        const DWORD id = static_cast<DWORD>(value);
        ids[v] = id;
        parent.try_emplace(id, id);
        const auto& p = mesh.mVertices[v];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
        for (auto at = positions.lower_bound(p.x - tolerance); at != positions.end(); ++at) {
            const auto& [sortedX, candidate] = *at;
            if (sortedX > p.x + tolerance) break;
            const auto& [position, canonicalId] = candidate;
            if (std::abs(p.y - position.y) > tolerance || std::abs(p.z - position.z) > tolerance) continue;
            const DWORD a = root(id), b = root(canonicalId);
            const auto [low, high] = std::minmax(a, b);
            parent[high] = low;
        }
        positions.emplace(p.x, std::pair{p, id});
    }
    for (auto& id : ids) if (id) id = root(id);
    return ids;
}

long ImportShapeFromMesh(const aiScene* scene, unsigned meshIdx,
                         mc2::content::Node& outShape, TG_TypeMultiShape* outMulti,
                         SkelBake* bake = nullptr, bool authoredColors = false,
                         const aiNode* instanceNode = nullptr, const aiMatrix4x4* instanceWorld = nullptr) {
	const aiMesh* mesh = scene->mMeshes[meshIdx];
	if (mesh->mNumVertices == 0 || mesh->mNumFaces == 0) {
		// Empty mesh — leave shape inited but do not populate. Engine treats
		// zero-vertex shapes as no-op renders (numVisibleFaces stays 0).
		return 0;
	}

	// BT2018-SKEL-ENGINE-1A: skinned-mech import. Drop damage/debris/UI parts,
	// then per-part decide the bind-pose bake (single bone, rigid).
	bool doMeshBake = false;
	const float* restM = nullptr;     // joint-global rest (row-major) for this part's bone
	aiMatrix4x4 partOffset;           // this part's own inverse-bind
	if (bake && bake->active) {
		if (skelMeshDropped(mesh->mName.C_Str())) {
			++bake->droppedParts;
			return 0;  // leave an empty shape (renders nothing)
		}
		++bake->importedParts;
		if (mesh->mNumBones >= 1) {
			if (mesh->mNumBones == 1) ++bake->partsSingleBone;
			else noteUnsupportedSkin("multi-bone mesh", mesh->mName.C_Str());
			const aiBone* bone = mesh->mBones[0];
			if (auto it = bake->nameIdx.find(bone->mName.C_Str()); it != bake->nameIdx.end()) {
				const auto& [name, index] = *it;
				restM = bake->rest[index].m;
				partOffset = bone->mOffsetMatrix;
				doMeshBake = true;
				++bake->partsOffsetBaked;
			}
		}
	}
	if (mesh->mNormals == nullptr) {
		// Should not happen because we pass aiProcess_GenSmoothNormals, but
		// belt-and-braces — the engine's lighting kernel reads .normal.
		PAUSE(("[importer] mesh %u has no normals after Generate pass", meshIdx));
		return -1;
	}

	// Resolve node identity by walking the scene graph for the node that
	// references this mesh. Multi-mesh-per-node is allowed; we just take the
	// first hit (the renderer iterates shapes, not nodes).
	const aiNode* meshNode = instanceNode ? instanceNode : FindNodeForMesh(scene->mRootNode, meshIdx).value_or(nullptr);
	const char* nodeNm = (meshNode && meshNode->mName.length > 0)
	                     ? meshNode->mName.C_Str() : "";
	const char* parentNm = "None";
	if (meshNode && meshNode->mParent && meshNode->mParent != scene->mRootNode
	    && meshNode->mParent->mName.length > 0) {
		parentNm = meshNode->mParent->mName.C_Str();
	}

	// Node pivot: decompose the node's local transform; take the translation
	// component, flipped to MC2 space. (Rotation/scale are not propagated to
	// TG_TypeShape — those live in the per-instance TG_ShapeRec / animation
	// channels, which are M2.)
	glm::vec3 center;
	center.x = center.y = center.z = 0.0f;
	if (meshNode && !instanceWorld) {
		aiVector3D pos, scale;
		aiQuaternion rot;
		meshNode->mTransformation.Decompose(scale, rot, pos);
		center = toMC2Pos(pos);
	}
	const bool keepPivot = instanceWorld &&
		(S_strnicmp(nodeNm, "SpotLight_", 10) == 0 || S_strnicmp(nodeNm, "LitWin", 6) == 0
		 || (meshNode && meshNode->mMetaData && meshNode->mMetaData->HasKey("mc2_pivot_tg_wu")));
	if (keepPivot) {
		center = toMC2Pos(aiVector3D(instanceWorld->a4, instanceWorld->b4, instanceWorld->c4));
		// Material splits below an animated source follow the same stock track.
		if (!(meshNode && meshNode->mParent && meshNode->mParent->mMetaData
		      && meshNode->mParent->mMetaData->HasKey("mc2_anim_node")))
			parentNm = "None";
	}
	// Baked instance: positions by the world matrix, normals by its inverse transpose; a mirroring
	// transform reverses winding so faces stay front-facing.
	aiMatrix3x3 normalXform;
	bool mirrored = false;
	if (instanceWorld) {
		const aiMatrix3x3 linear(*instanceWorld);
		normalXform = aiMatrix3x3(linear).Inverse().Transpose();
		mirrored = linear.Determinant() < 0.0f;
	}

	// Zeroed vertex + triangle vectors, moved into the shape once filled.
	std::vector<TG_TypeVertex> verts(mesh->mNumVertices);
	std::vector<TG_TypeTriangle> tris(mesh->mNumFaces);
	const auto windowCells = instanceWorld && mesh->HasTextureCoords(1) ? windowCellIds(*mesh) : std::vector<DWORD>{};

	// Per-vertex: position + normal in MC2 space; aRGBLight init to opaque
	// black (matches ParseASEFile's default — engine's lighting kernel
	// overwrites this every frame anyway).
	for (unsigned v = 0; v < mesh->mNumVertices; v++) {
		if (doMeshBake) {
			// Bind-pose bake: v_world = restGlobal(bone) * (offset_part * v),
			// normal by the same rotation. All in glTF space; toMC2* applies the
			// MC2 axis flip after, identical to the static (pre-baked) GLB path.
			aiVector3D pLocal = partOffset * mesh->mVertices[v];
			verts[v].position = toMC2Pos(applyRowMajor16(restM, pLocal) * bake->scale);
			aiVector3D nLocal = aiMatrix3x3(partOffset) * mesh->mNormals[v];
			aiVector3D nWorld = rotRowMajor16(restM, nLocal);
			nWorld.Normalize();
			verts[v].normal = toMC2Vec(nWorld);
		} else if (instanceWorld) {
			verts[v].position = toMC2Pos((*instanceWorld) * mesh->mVertices[v]);
			if (keepPivot) verts[v].position -= center;
			aiVector3D n = normalXform * mesh->mNormals[v];
			n.Normalize();
			verts[v].normal = toMC2Vec(n);
		} else {
			verts[v].position  = toMC2Pos(mesh->mVertices[v]);
			verts[v].normal    = toMC2Vec(mesh->mNormals[v]);
		}
		// Init OPAQUE WHITE, not black. Override render shapes (treeRenderShape/
		// bldgRenderShape) are drawn by the GPU static-prop batcher, which reads
		// a_aRGBLight from the type-level VBO (bdactor.cpp ~2698). The per-vertex
		// CPU lighting bake runs on the STOCK shape (treeShape), never on the
		// imported override shape, so its aRGBLight is never overwritten. Black
		// init => texture * 0 = solid-black tree. White init => the cooked albedo
		// shows (unlit, matching the "draw foliage UNLIT" prior art). When a bake
		// DOES run it overwrites this, so stock-lit shapes are unaffected.
		verts[v].aRGBLight = 0xffffffff;
        // No COLOR_0 means the same neutral baked fill as an untagged imported
        // prop. Explicit authored black and hot-colour tags still win.
        if (authoredColors && mesh->HasVertexColors(0)) {
            verts[v].aRGBLight = 0xff000000;
            const auto& color = mesh->mColors[0][v];
            auto channel = [](float value) -> uint32_t {
                if (!std::isfinite(value)) return 0;
                return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
            };
            verts[v].aRGBLight |= (channel(color.r) << 16) | (channel(color.g) << 8) | channel(color.b);
        }
		// Kit TEXCOORD_1 marks glass cells (zero elsewhere). Reuse the light word
		// for black/white seeds so stock geometry and the GPU vertex stride stay unchanged.
		if (!windowCells.empty()) {
			const DWORD id = windowCells[v];
			const DWORD seed = verts[v].aRGBLight;
			if (id != 0
			    && (seed == 0xff000000 || seed == 0xffffffff))
				verts[v].aRGBLight = (seed == 0xff000000 ? 0x7e000000u : 0x7d000000u)
				    | id;
		}

	}

	// UV channel 0 remains the diffuse channel; static glass cell IDs use UV1 above.
	const bool hasUV = mesh->HasTextureCoords(0);

	for (unsigned f = 0; f < mesh->mNumFaces; f++) {
		const aiFace& face = mesh->mFaces[f];
		// aiProcess_Triangulate guarantees 3-vertex faces; defensive check.
		if (face.mNumIndices != 3) {
			PAUSE(("[importer] mesh %u face %u has %u indices (expected 3)",
			       meshIdx, f, face.mNumIndices));
			return -1;
		}
		TG_TypeTriangle& t = tris[f];
		const unsigned c1 = mirrored ? 2u : 1u, c2 = mirrored ? 1u : 2u;
		t.Vertices[0] = face.mIndices[0];
		t.Vertices[1] = face.mIndices[c1];
		t.Vertices[2] = face.mIndices[c2];

		// Material index → texture slot: 1:1 mapping per BuildTextureList
		// (MVP scope; mech materials don't share atlases).
		t.localTextureHandle = mesh->mMaterialIndex;
		t.renderStateFlags   = 0;  // backface bit 0 = front-facing default

		// Face normal: average the three vertex normals. Same approximation
		// the renderer uses for lighting; sufficient for MVP. Per-face
		// recompute via cross-product would be more accurate but the engine
		// re-uses faceNormal mainly for backface culling, which is sign-only.
		glm::vec3 fn;
		fn.x = verts[face.mIndices[0]].normal.x
		     + verts[face.mIndices[1]].normal.x
		     + verts[face.mIndices[2]].normal.x;
		fn.y = verts[face.mIndices[0]].normal.y
		     + verts[face.mIndices[1]].normal.y
		     + verts[face.mIndices[2]].normal.y;
		fn.z = verts[face.mIndices[0]].normal.z
		     + verts[face.mIndices[1]].normal.z
		     + verts[face.mIndices[2]].normal.z;
		// Don't bother normalising — the renderer normalises on use.
		t.faceNormal = fn;

		if (hasUV) {
			t.uvdata.u0 = mesh->mTextureCoords[0][face.mIndices[0]].x;
			t.uvdata.v0 = toMC2V(mesh->mTextureCoords[0][face.mIndices[0]].y);
			t.uvdata.u1 = mesh->mTextureCoords[0][face.mIndices[c1]].x;
			t.uvdata.v1 = toMC2V(mesh->mTextureCoords[0][face.mIndices[c1]].y);
			t.uvdata.u2 = mesh->mTextureCoords[0][face.mIndices[c2]].x;
			t.uvdata.v2 = toMC2V(mesh->mTextureCoords[0][face.mIndices[c2]].y);
		}
	}

	// Hand ownership of the vectors to the shape (no copy).
	outShape.name = nodeNm; outShape.parent = parentNm;
	outShape.center = outShape.relativeCenter = center;
	if (keepPivot && strcmp(parentNm, "None") != 0)
	{
		aiMatrix4x4 parentWorld;
		for (const aiNode* ancestor = meshNode->mParent; ancestor; ancestor = ancestor->mParent)
			parentWorld = ancestor->mTransformation * parentWorld;
		outShape.relativeCenter -= toMC2Pos(aiVector3D(parentWorld.a4, parentWorld.b4, parentWorld.c4));
	}
	outShape.vertices = std::move(verts); outShape.triangles = std::move(tris);
	return 0;
}

//-----------------------------------------------------------------------------
// BT2018-SKEL-ENGINE-1A: skinned mech -> ONE merged, bind-pose-baked shape.
//
// A skinned GLB has many mesh-parts (69 for the marauder); importing them as
// per-mesh shapes left dropped parts as empty (NULL-vertex) shapes, which the
// mech GPU/recipe path chokes on (crash + no render). The proven-good static
// path is a SINGLE shape, so merge every intact baked part into one combined
// vertex/triangle buffer (per-triangle material index preserved), matching the
// fk_bake static export exactly. Caller has already done AllocateImportedShapes(1)
// + BuildTextureList + ResetBoundingBox.
long PopulateMergedSkinnedShape(const aiScene* scene, mc2::content::Asset& out, SkelBake& bake) {
	std::vector<unsigned> parts;
	DWORD totalV = 0, totalT = 0;
	for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
		const aiMesh* me = scene->mMeshes[m];
		if (me->mNumVertices == 0 || me->mNumFaces == 0) continue;
		if (skelMeshDropped(me->mName.C_Str())) { ++bake.droppedParts; continue; }
		if (me->mNumBones == 0) continue;  // skinned scene: skip any unrigged stray
		parts.push_back(m); totalV += me->mNumVertices; totalT += me->mNumFaces;
	}
	if (parts.empty() || totalV == 0) { PAUSE(("[importer] skinned merge: no intact parts")); return -1; }

	auto& shape = out.nodes.front();

	std::vector<TG_TypeVertex> verts(totalV);
	std::vector<TG_TypeTriangle> tris(totalT);

	DWORD vOff = 0, tOff = 0;
	for (unsigned m : parts) {
		const aiMesh* me = scene->mMeshes[m];
		++bake.importedParts;
		if (me->mNumBones == 1) ++bake.partsSingleBone;
		else noteUnsupportedSkin("multi-bone mesh", me->mName.C_Str());
		const aiBone* bone = me->mBones[0];
		const float* rm = nullptr;
		int boneIndex = -1;
		if (auto it = bake.nameIdx.find(bone->mName.C_Str()); it != bake.nameIdx.end()) {
			const auto& [name, index] = *it;
			rm = bake.rest[index].m;
			boneIndex = index;
		}
		aiMatrix4x4 off = bone->mOffsetMatrix;
		if (rm) ++bake.partsOffsetBaked;
		// 1B-RUNTIME: record this part's layout for the per-frame re-bake. The
		// vOff/vCount range is this part's slice of the merged listOfTypeVertices;
		// boneIndex indexes SkelBake::names (the clip-global array TickImportedMechs
		// re-evaluates each frame). -1 = no bone match (scale-only, as below).
		if (bake.recParts) {
			ImportPartRec pr;
			pr.meshIndex = m;
			pr.boneIndex = boneIndex;
			pr.off       = off;
			pr.vOff      = vOff;
			pr.vCount    = me->mNumVertices;
			bake.recParts->push_back(pr);
		}
		const bool hasUV = me->HasTextureCoords(0);

		for (unsigned v = 0; v < me->mNumVertices; ++v) {
			aiVector3D p, n;
			if (rm) {
				p = applyRowMajor16(rm, off * me->mVertices[v]) * bake.scale;
				n = rotRowMajor16(rm, aiMatrix3x3(off) * me->mNormals[v]);
			} else {
				p = me->mVertices[v] * bake.scale;
				n = me->mNormals[v];
			}
			n.Normalize();
			TG_TypeVertex& vt = verts[vOff + v];
			vt.position  = mechToMC2Pos(p);   // fixed mech axis (== MC2_GLTF_AXIS=2)
			vt.normal    = mechToMC2Vec(n);
			vt.aRGBLight = 0xffffffff;
		}
		for (unsigned f = 0; f < me->mNumFaces; ++f) {
			const aiFace& fc = me->mFaces[f];
			if (fc.mNumIndices != 3) continue;
			TG_TypeTriangle& t = tris[tOff + f];
			t.Vertices[0] = vOff + fc.mIndices[0];
			t.Vertices[1] = vOff + fc.mIndices[1];
			t.Vertices[2] = vOff + fc.mIndices[2];
			// Single skin slot 0 (the base atlas): resetPaintScheme binds the mech
			// skin from texture slot 0, and the GLB's material 0 is a blip atlas.
			t.localTextureHandle = 0;
			t.renderStateFlags = 0;
			glm::vec3 fn;
			fn.x = verts[t.Vertices[0]].normal.x + verts[t.Vertices[1]].normal.x + verts[t.Vertices[2]].normal.x;
			fn.y = verts[t.Vertices[0]].normal.y + verts[t.Vertices[1]].normal.y + verts[t.Vertices[2]].normal.y;
			fn.z = verts[t.Vertices[0]].normal.z + verts[t.Vertices[1]].normal.z + verts[t.Vertices[2]].normal.z;
			t.faceNormal = fn;
			if (hasUV) {
				t.uvdata.u0 = me->mTextureCoords[0][fc.mIndices[0]].x;
				t.uvdata.v0 = toMC2V(me->mTextureCoords[0][fc.mIndices[0]].y);
				t.uvdata.u1 = me->mTextureCoords[0][fc.mIndices[1]].x;
				t.uvdata.v1 = toMC2V(me->mTextureCoords[0][fc.mIndices[1]].y);
				t.uvdata.u2 = me->mTextureCoords[0][fc.mIndices[2]].x;
				t.uvdata.v2 = toMC2V(me->mTextureCoords[0][fc.mIndices[2]].y);
			}
		}
		vOff += me->mNumVertices; tOff += me->mNumFaces;
	}

	glm::vec3 zero; zero.x = zero.y = zero.z = 0.0f;
	shape.name = "imported_mech"; shape.parent = "None";
	shape.center = shape.relativeCenter = zero;
	shape.vertices = std::move(verts); shape.triangles = std::move(tris);
	return 0;
}

//-----------------------------------------------------------------------------
// Multi-shape bounding box (min/max corner + extentRadius). Mirrors what the
// ASE path does in LoadBinaryCopy: vertex-tight over every transformed vertex
// (accumulated in ImportShapeFromMesh in multi-shape-local space), then
// finalized here for extentRadius. Sets `out->maxBox`, `minBox`, `extentRadius`.
//
// Reset the multi-shape box to "empty" extremes before the per-mesh vertex
// accumulation in ImportShapeFromMesh. Call once before the mesh loop.
void ResetBoundingBox(TG_TypeMultiShape* out) {
	out->maxBox.x = out->maxBox.y = out->maxBox.z = -1.0e9f;
	out->minBox.x = out->minBox.y = out->minBox.z =  1.0e9f;
}

// Finalize the multi-shape box after every mesh has expanded min/max over its
// vertices (vertex-tight, see ImportShapeFromMesh). Handles the empty-mesh
// sentinel and computes the bounding-sphere radius.
void ComputeBoundingBox(TG_TypeMultiShape* out) {
	// Sentinel if no vertices contributed (no mesh expanded the box).
	if (out->maxBox.x < out->minBox.x) {
		out->maxBox.x = out->maxBox.y = out->maxBox.z = 0.0f;
		out->minBox.x = out->minBox.y = out->minBox.z = 0.0f;
	}

	const float dx = out->maxBox.x - out->minBox.x;
	const float dy = out->maxBox.y - out->minBox.y;
	const float dz = out->maxBox.z - out->minBox.z;
	out->extentRadius = 0.5f * sqrtf(dx * dx + dy * dy + dz * dz);
	out->extentRadius = std::max(out->extentRadius, 1.0f);  // defensive floor
}

} // anonymous namespace

//=============================================================================
// BT2018-SKEL-ENGINE-1B-RUNTIME — per-frame imported-mech animation.
//
// The merge above bakes ONE static pose into the shared TG type geometry. This
// re-poses that same merged `listOfTypeVertices` every frame from a looping clip
// (same FK/offset math, clip global per frame) so the imported mech MOVES. The
// CPU TransformMultiShape then re-reads the type verts into the instance each
// frame. Scope (1B): one forced clip, CPU mech path (MC2_GPU_MECHS=0), shared
// type → actors animate in lockstep. Per-actor + GPU path are later slices.
namespace {

// PER-ACTOR animation state (BT2018-SKEL-GPU-PER-ACTOR-1). Each imported-mech actor
// advances its OWN clip/time and produces its OWN model-delta palette, so two mechs of
// the same chassis no longer animate in lockstep. The type entry below holds the shared
// STATIC data (scene, skeleton, invMWrest, per-vertex bone…); this holds the per-frame
// mutable state. (The CPU re-bake path uses one shared instance — it mutates the shared
// listOfTypeVertices, so it cannot be per-actor without per-instance vertex buffers.)
struct ActorAnimState : mc2::sim::ImportedAnimState {
    std::vector<float> modelDelta;          // jointCount*16 row-major (placement-free)
    float gpuLift = 0.0f;                    // world-up foot-ground lift (per-frame)
    // P9 jump/land memory (phase), Once-clip hold (mode) and stride-matched playback (locomotion) live in ImportedAnimState: they are serialized sim state.
    float playRate = 1.0f;                   // P9: last stride rate (diagnostics)
    std::vector<mc2skel::GpuBone> lastGlobals;  // 1A: last-frame clip bone GLOBALS (FK),
                                                // parallel to ImportedAnimEntry::names —
                                                // read by GetImportedNodeWorld for firepoints.
};

struct ImportedAnimEntry {
    std::unique_ptr<Assimp::Importer> imp;   // owns scene; freed with the entry (UnregisterImportedType)
    const aiScene*      scene = nullptr;
    TG_TypeShape*       shape = nullptr;   // merged slot-0 shape (listOfTypeVertices)
    TG_TypeMultiShape*  multi = nullptr;   // for per-frame bbox refresh
    std::vector<std::string> names;        // skeleton, parallel to clip globals
    std::shared_ptr<const mc2skel::SkelPlan> plan;  // flattened node/channel tables for names (borrows scene; declared after imp so it dies first)
    std::string         pinnedClip;        // MC2_MECH_IMPORT_FORCE_CLIP (empty = 1C dynamic)
    std::string         initialClip;       // clip seeded into each new actor state
    std::string         showcaseClip;      // optional package clips.showcase
    std::map<std::string, float> strideSpeed;  // P9: locomotion clip -> planted-foot speed (world units/s)
    std::map<std::string, std::string> nodeManifest;  // 1A: MC2 node name -> source joint
    std::map<std::string, aiVector3D> socketOffsets;  // P10: socket name -> point in its joint's local frame
                                            // (from bt2018_mech_package.json "nodes").
    std::string aoTexName;                  // AO-1: materials.ao.tga (deriveName stem +
                                            // .tga); empty = no AO. Loaded -> unit 6.
    std::string normalTexName;              // NORMALS-1: materials.normal.tga; empty = no
                                            // normal map. Loaded -> unit 7.
    float               scale    = 1.0f;
    float               groundDy = 0.0f;   // MC2-space Y offset applied at import
    float               initialDuration = 1.0f;
    std::vector<ImportPartRec> parts;
    // 1B-GPU static data (shared across actors).
    bool                       gpuMode = false;
    std::vector<unsigned char> perVertexBone;   // per type-vertex bone index (0..N-1)
    std::vector<aiMatrix4x4>   invMWrest;        // (A2·S·R_i)^-1 per bone (A2 = VBO axis)
    aiMatrix4x4                AS1;              // A1·S (Z-up clip-side axis · scale)
    std::vector<aiVector3D>    boneLowest;       // per-bone lowest rest VBO vertex (model space)
    float                      restLift = 0.0f;  // static ground lift from REST pose (steady torso)
    mc2ground::Rig              grounding;
    // Per-frame state: GPU = one ActorAnimState per actor instance; CPU = one shared.
    std::map<const void*, ActorAnimState> actors;  // keyed by actor instance (mechShape*)
    ActorAnimState             cpuState;
};
std::vector<ImportedAnimEntry> g_importedAnims;
// Prune actor states untouched for this many frames (dead/despawned actors).
const unsigned ACTOR_STATE_STALE_FRAMES = 600u;

// BT2018-FIRST-CLASS P3: a mounted package with the "imported_mechs" capability ships its
// imported mechs animated on the GPU path (per-actor palettes); the env gates stay as
// opt-in for loose imports and an explicit =0 kills the package default.
static bool importedMechPackageMounted() {
    static const bool v = ModCapabilityMounted("imported_mechs");
    return v;
}

bool mechImportAnimateEnabled() {
    // Opt-in, default OFF (static FORCE_CLIP bake stays the safe path) unless a package asks.
    static const bool v = mc2::gate::get<"MC2_MECH_IMPORT_ANIMATE">() ||
        (importedMechPackageMounted() && mc2::gate::state<"MC2_MECH_IMPORT_ANIMATE">() != mc2::gate::Tri::Off);
    return v;
}

// 1B-GPU: route the imported mech through the GPU skinned-mech path (default OFF unless a package asks).
bool mechImportGpuEnabled() {
    static const bool v = mc2::gate::get<"MC2_MECH_IMPORT_GPU">() ||
        (importedMechPackageMounted() && mc2::gate::state<"MC2_MECH_IMPORT_GPU">() != mc2::gate::Tri::Off);
    return v;
}
// GPU palette clip-side axis case. Default 2 (== mechToMC2Pos, the VBO axis): with
// the actor shapeToWorld_root composed by the batcher, axis 2 stands the mech upright
// (user-confirmed in-engine). Tunable for other rigs.
int mechImportGpuAxis() {
    static int v = 2;
    return v;
}
// World-up (Stuff.z, the shader's up after its (-x,z,y) swap) lift applied to the
// imported GPU palette so the mech's feet sit on the terrain. Imported GLBs are
// pelvis-origin (not foot-origin like stock ASE), so placement grounds the pelvis
// and the feet sink; this lifts by the model's below-origin foot drop. Override/dial
// with MC2_MECH_IMPORT_GPU_LIFT (world units); 0 (default) uses the auto value.
float mechImportGpuLiftOverride(bool* hasOverride) {
    static const bool has = mc2::gate::isSet<"MC2_MECH_IMPORT_GPU_LIFT">();
    static const float v = mc2::gate::get<"MC2_MECH_IMPORT_GPU_LIFT">();
    if (hasOverride) *hasOverride = has;
    return v;
}
// Which world translation component the lift adds to (0=Stuff.x, 1=Stuff.y, 2=Stuff.z).
// Default 1 (Stuff.y) — tuning which axis is world-up for the placed imported mech.
int mechImportGpuLiftAxis() {
    static int v = 1;
    return v;
}

// Apply one of the four even-parity axis cases (mirrors the anon axisMap()).
inline void applyAxisCase(int c, const aiVector3D& v, float& X, float& Y, float& Z) {
    switch (c) {
        case 0: X=-v.x; Y=-v.y; Z= v.z; break;
        case 1: X=-v.x; Y= v.z; Z= v.y; break;   // Z-up (glTF Y -> Stuff Z)
        default:
        case 2: X=-v.x; Y= v.y; Z=-v.z; break;   // Y-up (== mechToMC2Pos, the VBO)
        case 3: X=-v.x; Y=-v.z; Z=-v.y; break;
    }
}
// Affine A for an axis case, with groundDy applied along the mapped up-axis
// (= A·(0,1,0)). Row-major aiMatrix4x4; A·p == axis-mapped point + ground.
aiMatrix4x4 buildAffineAxis(int c, float groundDy) {
    float x1,y1,z1, x2,y2,z2, x3,y3,z3, gx,gy,gz;
    applyAxisCase(c, aiVector3D(1,0,0), x1,y1,z1);
    applyAxisCase(c, aiVector3D(0,1,0), x2,y2,z2);
    applyAxisCase(c, aiVector3D(0,0,1), x3,y3,z3);
    applyAxisCase(c, aiVector3D(0,1,0), gx,gy,gz);   // up direction
    aiMatrix4x4 A;
    A.a1=x1; A.a2=x2; A.a3=x3; A.a4=groundDy*gx;
    A.b1=y1; A.b2=y2; A.b3=y3; A.b4=groundDy*gy;
    A.c1=z1; A.c2=z2; A.c3=z3; A.c4=groundDy*gz;
    A.d1=0;  A.d2=0;  A.d3=0;  A.d4=1;
    return A;
}
inline aiMatrix4x4 rowMajorToAi(const float* m) {
    aiMatrix4x4 r;
    r.a1=m[0];  r.a2=m[1];  r.a3=m[2];  r.a4=m[3];
    r.b1=m[4];  r.b2=m[5];  r.b3=m[6];  r.b4=m[7];
    r.c1=m[8];  r.c2=m[9];  r.c3=m[10]; r.c4=m[11];
    r.d1=m[12]; r.d2=m[13]; r.d3=m[14]; r.d4=m[15];
    return r;
}
inline void aiToRowMajor(const aiMatrix4x4& M, float* out16) {
    out16[0]=M.a1; out16[1]=M.a2; out16[2]=M.a3; out16[3]=M.a4;
    out16[4]=M.b1; out16[5]=M.b2; out16[6]=M.b3; out16[7]=M.b4;
    out16[8]=M.c1; out16[9]=M.c2; out16[10]=M.c3; out16[11]=M.c4;
    out16[12]=M.d1; out16[13]=M.d2; out16[14]=M.d3; out16[15]=M.d4;
}

// Clip loop length in seconds, or nullopt if the scene has no such clip.
std::optional<float> clipDurationSec(const aiScene* scene, std::string_view clip) {
    for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
        const aiAnimation* an = scene->mAnimations[a];
        if (clip == an->mName.C_Str()) {
            double tps = an->mTicksPerSecond != 0.0 ? an->mTicksPerSecond : 1000.0;
            return static_cast<float>((an->mDuration / tps));
        }
    }
    return std::nullopt;
}

// 1C/P9: per-frame clip choice lives in mclib/mech_clip_state.h (mc2clip::select): gesture
// state + measured speed -> clip, one-shot vs loop, stride-matched locomotion.

// P9: planted-foot speed of each locomotion clip, measured once at import. A clip plays in
// place, so while a foot is planted it slides backwards under the root at exactly the ground
// speed the clip was authored for. Playback rate = actual speed / this value, and the planted
// foot then stays put (mc2clip::strideRate). Units: the GPU model space (AS1 includes the
// import scale), up = y. Needs e.AS1, so it runs on the GPU path only.
void measureClipStrides(ImportedAnimEntry& e) {
    static const char* const kClips[] = {
        "atlas_moveCoreWalkFwd", "atlas_moveCoreRunFwd", "atlas_moveCoreSprintFwd", "atlas_moveCoreWalkBwd",
        "atlas_moveCoreWalkLimpFwdLeft", "atlas_moveCoreWalkLimpFwdRight" };
    int feet[2] = { -1, -1 };
    for (size_t i = 0; i < e.names.size(); ++i) {
        if (e.names[i] == "j_LFoot") feet[0] = static_cast<int>(i);
        if (e.names[i] == "j_RFoot") feet[1] = static_cast<int>(i);
    }
    if (feet[0] < 0 || feet[1] < 0) return;
    for (const char* clip : kClips) {
        auto dur = clipDurationSec(e.scene, clip);
        if (!dur || *dur < 0.1f) continue;
        const int frames = static_cast<int>(*dur * 30.0f);
        std::vector<aiVector3D> pos[2];
        std::vector<mc2skel::GpuBone> g;
        for (int f = 0; f <= frames; ++f) {
            double tt = 0, dd = 0;
            if (!mc2skel::EvaluateClipGpuBones(*e.plan, clip, static_cast<float>(f), g, &tt, &dd)) break;
            for (int k = 0; k < 2; ++k) {
                const aiMatrix4x4 M = e.AS1 * rowMajorToAi(g[feet[k]].m);
                pos[k].push_back(aiVector3D(M.a4, M.b4, M.c4));
            }
        }
        // One gait cycle moves each foot through its full fore-aft range twice (stance back,
        // swing forward); the planted half covers that range at ground speed, so speed =
        // 2 * range / duration along the foot's dominant horizontal axis. (A per-frame
        // "lowest frames" test picked swing frames on in-place BT clips and over-read ~4x.)
        float sum = 0.0f; int n = 0;
        for (auto & po : pos) {
            if (po.size() < 3) continue;
            const auto [xlo, xhi] = std::ranges::minmax(po | std::views::transform([](const aiVector3D& p) { return p.x; }));
            const auto [zlo, zhi] = std::ranges::minmax(po | std::views::transform([](const aiVector3D& p) { return p.z; }));
            sum += 2.0f * std::max(xhi - xlo, zhi - zlo) / *dur;
            ++n;
        }
        if (n > 0 && sum / n > 0.5f) {
            e.strideSpeed[clip] = sum / n;
            mc2::fmt::print_err("[MECH_IMPORT] stride '{}' planted-foot speed={:.2f} u/s (cycle {:.2f}s)\n", clip, sum / n, *dur);
        }
    }
}

// BT2018-MECH-NODE-MANIFEST-1A: load the per-mech package's "nodes" map
// (MC2 semantic name -> source joint) from the GLB's sidecar
// <stem>.package.json. Minimal flat-map scan (no JSON dependency; the generator
// controls the format). Leaves the map empty (→ legacy node lookup) if absent.
void LoadMechPackage(mc2::czstring glbPath, std::map<std::string, std::string>& out,
                     std::string& aoOut, std::string& normalOut,
                     std::map<std::string, aiVector3D>* socketOut = nullptr,
                     std::string* showcaseOut = nullptr) {
    if (!glbPath) return;
    std::string p(glbPath);
    size_t dot = p.rfind(".glb");
    std::string side = (dot != std::string::npos ? p.substr(0, dot) : p) + ".package.json";
    FILE* f = fopen(side.c_str(), "rb");
    if (!f) {
        mc2::fmt::print_err("[MECH_IMPORT] no node manifest '{}' (firepoints -> legacy/origin)\n", side.c_str());
        return;
    }
    std::string buf;
    char tmp[4096]; size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) buf.append(tmp, n);
    fclose(f);
    if (showcaseOut) {
        const size_t clips = buf.find("\"clips\"");
        const size_t start = clips == std::string::npos ? clips : buf.find('{', clips);
        const size_t end = start == std::string::npos ? start : buf.find('}', start);
        const size_t key = start == std::string::npos ? start : buf.find("\"showcase\"", start);
        if (key < end) {
            const size_t colon = buf.find(':', key);
            const size_t v0 = colon == std::string::npos ? colon : buf.find('"', colon + 1);
            const size_t v1 = v0 == std::string::npos ? v0 : buf.find('"', v0 + 1);
            if (v1 < end) *showcaseOut = buf.substr(v0 + 1, v1 - v0 - 1);
        }
    }
    // "nodes" flat map
    size_t b = buf.find("\"nodes\"");
    if (b != std::string::npos) {
        b = buf.find('{', b);
        size_t end = (b == std::string::npos) ? std::string::npos : buf.find('}', b);
        if (b != std::string::npos && end != std::string::npos) {
            size_t i = b + 1;
            while (i < end) {
                size_t k0 = buf.find('"', i); if (k0 == std::string::npos || k0 >= end) break;
                size_t k1 = buf.find('"', k0 + 1); if (k1 == std::string::npos || k1 >= end) break;
                size_t colon = buf.find(':', k1 + 1); if (colon == std::string::npos || colon >= end) break;
                size_t v0 = buf.find('"', colon + 1); if (v0 == std::string::npos || v0 >= end) break;
                size_t v1 = buf.find('"', v0 + 1); if (v1 == std::string::npos || v1 > end) break;
                out[buf.substr(k0 + 1, k1 - k0 - 1)] = buf.substr(v0 + 1, v1 - v0 - 1);
                i = v1 + 1;
            }
        }
    }
    // P10: "socketOffsets" flat map, socket -> "x y z" in the joint's local frame
    // (tools/bt2018_mechs/sockets.py). Muzzles sit on the geometry, not at joint centres.
    if (socketOut) {
        size_t so = buf.find("\"socketOffsets\"");
        size_t b2 = (so == std::string::npos) ? so : buf.find('{', so);
        size_t e2 = (b2 == std::string::npos) ? b2 : buf.find('}', b2);
        for (size_t i = (b2 == std::string::npos ? 0 : b2 + 1); b2 != std::string::npos && i < e2; ) {
            size_t k0 = buf.find('"', i); if (k0 == std::string::npos || k0 >= e2) break;
            size_t k1 = buf.find('"', k0 + 1); if (k1 == std::string::npos || k1 >= e2) break;
            size_t v0 = buf.find('"', k1 + 1); if (v0 == std::string::npos || v0 >= e2) break;
            size_t v1 = buf.find('"', v0 + 1); if (v1 == std::string::npos || v1 > e2) break;
            float x = 0, y = 0, z = 0;
            if (sscanf(buf.substr(v0 + 1, v1 - v0 - 1).c_str(), "%f %f %f", &x, &y, &z) == 3)
                (*socketOut)[buf.substr(k0 + 1, k1 - k0 - 1)] = aiVector3D(x, y, z);
            i = v1 + 1;
        }
    }
    // AO-1: materials.ao.tga (runtime texture name; .ktx2 sidecar resolves it).
    size_t mat = buf.find("\"materials\"");
    if (mat != std::string::npos) {
        size_t aoKey = buf.find("\"ao\"", mat);
        if (aoKey != std::string::npos) {
            size_t tga = buf.find("\"tga\"", aoKey);
            if (tga != std::string::npos) {
                size_t colon = buf.find(':', tga);
                size_t v0 = (colon != std::string::npos) ? buf.find('"', colon + 1) : std::string::npos;
                size_t v1 = (v0 != std::string::npos) ? buf.find('"', v0 + 1) : std::string::npos;
                if (v0 != std::string::npos && v1 != std::string::npos)
                    aoOut = buf.substr(v0 + 1, v1 - v0 - 1);
            }
        }
        // NORMALS-1: materials.normal.tga (mirror the AO key; .ktx2 sidecar resolves it).
        size_t nrmKey = buf.find("\"normal\"", mat);
        if (nrmKey != std::string::npos) {
            size_t tga = buf.find("\"tga\"", nrmKey);
            if (tga != std::string::npos) {
                size_t colon = buf.find(':', tga);
                size_t v0 = (colon != std::string::npos) ? buf.find('"', colon + 1) : std::string::npos;
                size_t v1 = (v0 != std::string::npos) ? buf.find('"', v0 + 1) : std::string::npos;
                if (v0 != std::string::npos && v1 != std::string::npos)
                    normalOut = buf.substr(v0 + 1, v1 - v0 - 1);
            }
        }
    }
    mc2::fmt::print_err("[MECH_IMPORT] package '{}': {:d} nodes, ao='{}' normal='{}'\n", side.c_str(), mc2::fmt::as_size(out.size()), aoOut.c_str(), normalOut.c_str());
}

// Keep the import's own parse alive for the runtime re-bake (the GLB is parsed once; the caller
// hands over its Importer). Ownership transfers only once the entry is stored.
void RegisterImportedAnim(mc2::czstring path, TG_TypeMultiShape* out, const SkelBake& bake,
                          const std::vector<ImportPartRec>& parts, float groundDy,
                          std::unique_ptr<Assimp::Importer>& imp) {
    TG_TypeNodePtr slot = out->GetTypeNode(0);
    if (!slot || slot->GetNodeType() != SHAPE_NODE) return;
    const aiScene* scene = imp->GetScene();
    if (!scene) return;
    ImportedAnimEntry e;
    e.imp = std::move(imp); e.scene = scene;
    e.shape = static_cast<TG_TypeShape*>(slot);
    e.multi = out;
    e.names = bake.names;
    e.plan = mc2skel::BuildPlan(scene, e.names);
    LoadMechPackage(path, e.nodeManifest, e.aoTexName, e.normalTexName, &e.socketOffsets, &e.showcaseClip);
    if (!e.showcaseClip.empty() && !clipDurationSec(scene, e.showcaseClip)) {
        if (mc2_diag::tagRequested("MECH_IMPORT"))
            mc2::fmt::print_err("[MECH_IMPORT] unknown showcase clip '{}' in '{}'; using fallback\n", e.showcaseClip, path);
        e.showcaseClip.clear();
    }
    e.pinnedClip = bake.forcedClip;   // empty -> 1C dynamic selection
    // Initial clip: the pinned clip if any, else idle. Fall back to whatever the
    // scene's first clip is when even idle is absent, so the tick always animates.
    e.initialClip = !bake.forcedClip.empty() ? bake.forcedClip : std::string("atlas_moveCoreIdle");
    auto d = clipDurationSec(scene, e.initialClip);
    if (!d && scene->mNumAnimations > 0) {
        e.initialClip = scene->mAnimations[0]->mName.C_Str();
        d = clipDurationSec(scene, e.initialClip);
    }
    e.scale = bake.scale;
    e.groundDy = groundDy;
    e.initialDuration = (d && *d > 1e-4f) ? *d : 1.0f;
    e.parts = parts;

    // BT's prefab enables GrounderIK + LimbIK. Reconstruct sole support from the
    // actual intact meshes, including articulated toes, rather than a height dial.
    auto matrix = [](const mc2skel::GpuBone& bone) {
        glm::mat4 m;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) m[c][r] = bone.m[r * 4 + c];
        return m;
    };
    e.grounding.floor = 1e30f;
    for (int side = 0; side < 2; ++side) {
        auto& leg = e.grounding.legs[side];
        const std::string prefix = side == 0 ? "j_L" : "j_R";
        auto index = [&](std::string_view suffix) {
            const auto it = bake.nameIdx.find(prefix + std::string(suffix));
            int found = -1;
            if (it != bake.nameIdx.end()) {
                const auto& [boneName, boneIndex] = *it;
                found = boneIndex;
            }
            return found;
        };
        leg.thigh = index("Thigh"); leg.calf = index("Calf"); leg.foot = index("Foot");
        if (leg.thigh < 0 || leg.calf < 0 || leg.foot < 0) continue;
        leg.restFoot = matrix(bake.rest[leg.foot]);
        for (size_t b = 0; b < bake.parents.size(); ++b) {
            int ancestor = static_cast<int>(b);
            while (ancestor >= 0 && ancestor != leg.foot) ancestor = bake.parents[ancestor];
            if (ancestor == leg.foot && static_cast<int>(b) != leg.foot)
                leg.children.push_back(static_cast<int>(b));
        }
        for (const auto& part : parts) {
            const int b = part.boneIndex;
            if (b != leg.foot && std::find(leg.children.begin(), leg.children.end(), b) == leg.children.end()) continue;
            const aiMesh* mesh = scene->mMeshes[part.meshIndex];
            for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
                const aiVector3D local = part.off * mesh->mVertices[v];
                leg.sole.push_back({b, {local.x, local.y, local.z}});
                e.grounding.floor = std::min(e.grounding.floor,
                    applyRowMajor16(bake.rest[b].m, local).y);
            }
        }
    }

    // 1B-GPU: precompute the model-delta inputs. VBO = A2·S·R_i·p̃ (A2 = mechToMC2Pos,
    // Y-up, axis case 2). Per-frame model delta D_i = (A1·S·C_i)·(A2·S·R_i)^-1 maps the
    // Y-up rest vertex to the Z-up animated MODEL pose (A1 = case `gpuAxis`). The batcher
    // then composes the live shapeToWorld_root for placement.
    if (mechImportGpuEnabled() && !bake.rest.empty()) {
        e.gpuMode = true;
        aiMatrix4x4 S; S.a1 = S.b2 = S.c3 = bake.scale; S.d4 = 1.0f;
        const aiMatrix4x4 A2 = buildAffineAxis(2, groundDy);                  // VBO axis (Y-up)
        const aiMatrix4x4 A1 = buildAffineAxis(mechImportGpuAxis(), groundDy); // clip axis (Z-up)
        e.AS1 = A1 * S;
        const aiMatrix4x4 A2S = A2 * S;
        e.invMWrest.resize(bake.rest.size());
        for (size_t i = 0; i < bake.rest.size(); ++i) {
            aiMatrix4x4 MW = A2S * rowMajorToAi(bake.rest[i].m);
            MW.Inverse();
            e.invMWrest[i] = MW;
        }
        if (bake.rest.size() > 256) noteUnsupportedSkin("more than 256 bones (bone index wraps)", path);
        const int nv = e.shape->GetNumTypeVertices();
        e.perVertexBone.assign(nv > 0 ? nv : 0, 0);
        for (const ImportPartRec& pr : parts) {
            const int bi = (pr.boneIndex >= 0) ? pr.boneIndex : 0;
            for (unsigned v = 0; v < pr.vCount && static_cast<int>((pr.vOff + v)) < nv; ++v)
                e.perVertexBone[pr.vOff + v] = static_cast<unsigned char>((bi & 0xFF));
        }
        // Per-frame foot grounding: cache each bone's lowest rest VBO vertex (model
        // space, world-up = Stuff.y). Each frame the tick transforms these by the bone
        // model delta and lifts the placed mech so the lowest one sits on the terrain
        // — so the walk stride's planted foot never buries (rest-only lift would).
        {
            e.boneLowest.assign(bake.rest.size(), aiVector3D(0, 1e30f, 0));
            const TG_TypeVertex* tv = e.shape->GetTypeVertices();
            const int ntv = e.shape->GetNumTypeVertices();
            for (int vi = 0; vi < ntv && vi < static_cast<int>(e.perVertexBone.size()); ++vi) {
                const int b = e.perVertexBone[vi];
                if (b >= 0 && b < static_cast<int>(e.boneLowest.size()) && tv[vi].position.y < e.boneLowest[b].y)
                    e.boneLowest[b] = aiVector3D(tv[vi].position.x, tv[vi].position.y, tv[vi].position.z);
            }
            // Static ground lift from the REST pose. Using the rest lift every frame
            // (instead of recomputing the lowest animated vertex) keeps the torso
            // vertically steady — the per-frame lowest oscillates as legs articulate,
            // which caused a per-step "jack-in-a-box" bob. See buildGpuModelDelta.
            float lowestY = 1e30f;
            for (size_t i = 0; i < bake.rest.size() && i < e.invMWrest.size(); ++i) {
                aiMatrix4x4 D = (e.AS1 * rowMajorToAi(bake.rest[i].m)) * e.invMWrest[i];
                if (i < e.boneLowest.size()) {
                    const aiVector3D p = D * e.boneLowest[i];
                    lowestY = std::min(lowestY, p.y);
                }
            }
            e.restLift = (lowestY < 1e29f) ? -lowestY : 0.0f;
        }
        measureClipStrides(e);
        MECH_SKEL_TRACE("file='%s' 1B-GPU registered joints=%zu verts=%d axis=%d (bbox y[%.2f..%.2f])",
                        path, bake.rest.size(), nv, mechImportGpuAxis(),
                        out->minBox.y, out->maxBox.y);
    }

    MECH_SKEL_TRACE("file='%s' 1B-RUNTIME registered pinned='%s' active='%s' dur=%.3fs parts=%zu gpu=%d",
                    path, bake.forcedClip.c_str(), e.initialClip.c_str(), e.initialDuration, parts.size(),
                    static_cast<int>(mechImportGpuEnabled()));
    g_importedAnims.push_back(std::move(e));
}

} // namespace

bool mc2mechanim::AnyImportedAnim() { return !g_importedAnims.empty(); }

// IMPORTED-ACTOR-STABLE-KEY-1: erase this actor's per-frame state from every imported
// type so a freed-then-reused instance address can't inherit a stale pose. Idempotent.
// IMPORTED-TYPE-LIFETIME-1: an entry holds raw shape/multi pointers into a type that the
// type list frees; drop the entry (and its aiScene) when that type dies, else a reused
// address resolves to a stale entry (wrong skeleton / use-after-free). Idempotent.
void mc2mechanim::UnregisterImportedType(const void* multi) {
    if (!multi) return;
    std::erase_if(g_importedAnims, [multi](const ImportedAnimEntry& e) {
        return static_cast<const void*>(e.multi) == multi;
    });
}

void mc2mechanim::UnregisterImportedActor(const void* actorKey) {
    if (!actorKey) return;
    for (ImportedAnimEntry& e : g_importedAnims)
        e.actors.erase(actorKey);
}

namespace {
void buildGpuModelDelta(const ImportedAnimEntry&, ActorAnimState&, const std::vector<mc2skel::GpuBone>&);
}
static void rebakeImportedPresentation(ImportedAnimEntry& e, const std::vector<mc2skel::GpuBone>& globals) {
    TG_TypeVertex* vt = e.shape->GetTypeVerticesMutable();
    if (!vt) return;
    ResetBoundingBox(e.multi);
    for (const ImportPartRec& pr : e.parts) {
        const aiMesh* me = e.scene->mMeshes[pr.meshIndex];
        const float* rm = (pr.boneIndex >= 0 && pr.boneIndex < static_cast<int>(globals.size()))
                              ? globals[pr.boneIndex].m : nullptr;
        for (unsigned v = 0; v < pr.vCount && v < me->mNumVertices; ++v) {
            aiVector3D p, n;
            if (rm) {
                p = applyRowMajor16(rm, pr.off * me->mVertices[v]) * e.scale;
                n = rotRowMajor16(rm, aiMatrix3x3(pr.off) * me->mNormals[v]);
            } else {
                p = me->mVertices[v] * e.scale;
                n = me->mNormals[v];
            }
            n.Normalize();
            TG_TypeVertex& d = vt[pr.vOff + v];
            d.position   = mechToMC2Pos(p);
            d.position.y += e.groundDy;          // preserve the import-grounded offset
            d.normal     = mechToMC2Vec(n);
            e.multi->minBox.x = std::min(e.multi->minBox.x, d.position.x);
            e.multi->minBox.y = std::min(e.multi->minBox.y, d.position.y);
            e.multi->minBox.z = std::min(e.multi->minBox.z, d.position.z);
            e.multi->maxBox.x = std::max(e.multi->maxBox.x, d.position.x);
            e.multi->maxBox.y = std::max(e.multi->maxBox.y, d.position.y);
            e.multi->maxBox.z = std::max(e.multi->maxBox.z, d.position.z);
        }
    }
    ComputeBoundingBox(e.multi);
}

void mc2mechanim::CopyImportedActorPose(const void* source, const void* destination) {
    if (!source || !destination) return;
    for (auto& entry : g_importedAnims) {
        const auto found = entry.actors.find(source);
        if (found != entry.actors.end()) {
            const auto& [foundKey, foundState] = *found;
            entry.actors[destination] = foundState;
            auto& draw = entry.actors[destination];
            if (entry.gpuMode) buildGpuModelDelta(entry, draw, draw.lastGlobals);
            else rebakeImportedPresentation(entry, draw.lastGlobals);
        }
    }
}

bool mc2mechanim::ImportedGpuEnabled() { return mechImportGpuEnabled(); }

int mc2mechanim::ImportedGpuTypeInfo(const void* typeMulti,
                                     const unsigned char** perVertexBone, int* numVerts) {
    for (const ImportedAnimEntry& e : g_importedAnims) {
        if (e.gpuMode && static_cast<const void*>(e.multi) == typeMulti) {
            if (perVertexBone) *perVertexBone = e.perVertexBone.empty() ? nullptr : e.perVertexBone.data();
            if (numVerts) *numVerts = static_cast<int>(e.perVertexBone.size());
            return static_cast<int>(e.invMWrest.size());   // joint count
        }
    }
    return 0;
}

int mc2mechanim::ImportedGpuModelDelta(const void* actorKey, const float** mats16) {
    for (const ImportedAnimEntry& e : g_importedAnims) {
        if (!e.gpuMode) continue;
        auto it = e.actors.find(actorKey);
        if (it != e.actors.end()) {
            const auto& [stateKey, state] = *it;
            if (mats16) *mats16 = state.modelDelta.empty() ? nullptr : state.modelDelta.data();
            return static_cast<int>((state.modelDelta.size() / 16));
        }
    }
    return 0;
}

float mc2mechanim::ImportedGpuLift(const void* actorKey) {
    bool ovr = false;
    float v = mechImportGpuLiftOverride(&ovr);
    if (ovr) return v;
    for (const ImportedAnimEntry& e : g_importedAnims) {
        if (!e.gpuMode) continue;
        auto it = e.actors.find(actorKey);
        if (it != e.actors.end()) {
            const auto& [stateKey, state] = *it;
            return state.gpuLift;
        }
    }
    return 0.0f;
}

int mc2mechanim::ImportedGpuLiftAxis() { return mechImportGpuLiftAxis(); }

// AO-1: the imported mech's AO texture name (materials.ao.tga from the package), or
// nullptr if this type isn't imported / declares no AO. The caller (mech3d
// resetPaintScheme) loads it via mc2::textures::ready() and binds it on unit 6.
const char* mc2mechanim::ImportedMechAoTexName(const void* typeKey) {
    for (const ImportedAnimEntry& e : g_importedAnims)
        if (static_cast<const void*>(e.multi) == typeKey)
            return e.aoTexName.empty() ? nullptr : e.aoTexName.c_str();
    return nullptr;
}

// NORMALS-1: the imported mech's normal-map texture name (materials.normal.tga from the
// package), or nullptr if this type isn't imported / declares no normal. The caller
// (mech3d resetPaintScheme) loads it via mc2::textures::ready() and binds it on unit 7.
const char* mc2mechanim::ImportedMechNormalTexName(const void* typeKey) {
    for (const ImportedAnimEntry& e : g_importedAnims)
        if (static_cast<const void*>(e.multi) == typeKey)
            return e.normalTexName.empty() ? nullptr : e.normalTexName.c_str();
    return nullptr;
}

// BT2018-MECH-NODE-MANIFEST-1A: resolve an MC2 semantic node name to the imported
// mech's animated joint WORLD position. Read-only: no FK mutation, no clip change.
//   model pos = translation of (AS1 * C_i)   [C_i = live clip global of the joint]
//   world pos = Msw * model pos              [Msw = actor root shapeToWorld 3x4]
//   + foot-ground lift on the world-up axis  [matches the GPU batcher placement]
// Returns false (caller -> legacy TG node lookup) when not an imported mech, no
// manifest, the name is unmapped, or the joint/globals are absent.
bool mc2mechanim::GetImportedNodeWorld(const void* actorKey, const void* typeKey,
                                       const char* mc2Name, const float* rootToWorld12,
                                       float outXYZ[3]) {
    if (!mc2Name || !rootToWorld12 || !outXYZ) return false;
    const bool diag = false;
    for (ImportedAnimEntry& e : g_importedAnims) {
        if (static_cast<const void*>(e.multi) != typeKey) continue;
        if (e.nodeManifest.empty()) {
            if (diag) { static int w=0; if (w++<8) mc2::fmt::print_err("[NODE-DIAG] '{}': manifest EMPTY\n", mc2::fmt::safe(mc2Name)); }
            return false;
        }
        auto mit = e.nodeManifest.find(mc2Name);
        if (mit == e.nodeManifest.end()) {
            // De-numbered fallback: chassis name extra slots (weapon_lefttorso2,
            // weapon_rightarm3, ...) map to the same joint as the base node name.
            std::string base(mc2Name);
            while (!base.empty() && base.back() >= '0' && base.back() <= '9') base.pop_back();
            if (base != mc2Name) mit = e.nodeManifest.find(base);
        }
        if (mit == e.nodeManifest.end()) {
            static int s_warnName = 0;
            if (s_warnName++ < 8)
                mc2::fmt::print_err("[MECH_IMPORT] node '{}' not in manifest (-> legacy)\n", mc2::fmt::safe(mc2Name));
            return false;
        }
        const auto& [manifestNode, jointName] = *mit;
        int idx = -1;
        for (size_t i = 0; i < e.names.size(); ++i)
            if (e.names[i] == jointName) { idx = static_cast<int>(i); break; }
        if (idx < 0) {
            static int s_warnJoint = 0;
            if (s_warnJoint++ < 8)
                mc2::fmt::print_err("[MECH_IMPORT] node '{}' joint '{}' absent (-> legacy)\n", mc2::fmt::safe(mc2Name), jointName.c_str());
            return false;
        }
        ActorAnimState* s = nullptr;
        auto ait = e.actors.find(actorKey);
        if (ait != e.actors.end()) {
            auto& [stateKey, state] = *ait;
            s = &state;
        }
        else if (!actorKey && !e.cpuState.lastGlobals.empty()) s = &e.cpuState;
        if (!s || idx >= static_cast<int>(s->lastGlobals.size())) {
            if (diag) { static int w=0; if (w++<8) mc2::fmt::print_err("[NODE-DIAG] '{}' joint '{}' idx={:d}: no actor state / lastGlobals empty (actors={:d})\n", mc2::fmt::safe(mc2Name), jointName.c_str(), mc2::fmt::as_i32(idx), mc2::fmt::as_size(e.actors.size())); }
            return false;
        }
        // F = Msw * (AS1 * C_i); its translation (gx,gy,gz) = F[3],F[7],F[11] — the
        // SAME composed node-world translation the batcher writes. The stock weapon
        // node path (msl.cpp TG_MultiShape::GetTransformedNodePosition) reads a node's
        // shapeToWorld.entries[3,7,11] and returns:
        //     result.x = -entries[3];  result.z = entries[7];  result.y = entries[11];
        // i.e. NEGATE x, and (.y,.z) = (entries[11], entries[7]). Mirror that exactly so
        // an imported firepoint is in the identical frame callers (mover.cpp) consume
        // (.z = world elevation). Lift is applied in the pre-permute frame on the
        // liftAxis component, matching the batcher (F[3+4*liftAxis] += lift).
        aiMatrix4x4 M = e.AS1 * rowMajorToAi(s->lastGlobals[idx].m);
        float mx = M.a4, my = M.b4, mz = M.c4;
        // P10: a socket with an authored local point fires from actor x posed joint x point.
        if (e.socketOffsets.contains(mc2Name)) {
            const aiVector3D p = M * e.socketOffsets.at(mc2Name);
            mx = p.x; my = p.y; mz = p.z;
        }
        const float* w = rootToWorld12;
        float gx = w[0]*mx + w[1]*my + w[2]*mz  + w[3];
        float gy = w[4]*mx + w[5]*my + w[6]*mz  + w[7];
        float gz = w[8]*mx + w[9]*my + w[10]*mz + w[11];
        gy += e.restLift; // gameplay sockets use fixed authored grounding, independent of visual lift settings
        outXYZ[0] = -gx;   // result.x = -entries[3]
        outXYZ[1] =  gz;   // result.y =  entries[11]
        outXYZ[2] =  gy;   // result.z =  entries[7]  (world elevation)
        if (diag) { static int w=0; if (w++<16) mc2::fmt::print_err("[NODE-DIAG] '{}'->'{}' OK world=({:.1f},{:.1f},{:.1f}) lift={:.2f}\n", mc2::fmt::safe(mc2Name), jointName.c_str(), outXYZ[0], outXYZ[1], outXYZ[2], s->gpuLift); }
        return true;
    }
    if (diag) { static int w=0; if (w++<8) mc2::fmt::print_err("[NODE-DIAG] '{}': no imported entry matched typeKey={:016X} (registered={:d})\n", mc2::fmt::safe(mc2Name), mc2::fmt::as_ptr(typeKey), mc2::fmt::as_size(g_importedAnims.size())); }
    return false;
}

namespace {
// Advance `s`'s clip selection (1C) + clip time one frame, then sample the active
// clip's bone globals. Shared by the per-actor GPU path and the shared CPU path.
// Returns false if the clip can't be evaluated (caller skips this frame).
bool sampleActorClip(const ImportedAnimEntry& e, ActorAnimState& s, float dt,
                     const mc2mechanim::MechMotion& motion, std::vector<mc2skel::GpuBone>& globals,
                     bool advanceClock) {
    // ASSIMP-MECH-PAUSE-GATE-1: when the game is paused (advanceClock==false) freeze
    // BOTH the clip clock and the motion-driven clip selection, then fall through to
    // re-sample the pose at the frozen clipTimeSec. This re-bakes the SAME pose every
    // paused frame (geometry/skinning still runs) instead of striding in place.
    auto* preview = motion.preview ? mc2mechanim::previewAnimation : nullptr;
    if (preview) {
        const auto clip = mc2clip::resolveClip(preview->clip, s.activeClip,
            [&](std::string_view name) { return clipDurationSec(e.scene, std::string(name)).has_value(); });
        if (s.activeClip != clip) { s.activeClip = clip; s.clipTimeSec = 0; }
        s.durationSec = clipDurationSec(e.scene, s.activeClip).value_or(0);
        s.mode = preview->loop ? mc2clip::Mode::Loop : mc2clip::Mode::Once;
        s.playRate = preview->playing ? preview->rate : 0;
        if (preview->seek >= 0) { s.clipTimeSec = preview->seek; preview->seek = -1; }
        if (preview->clips.empty()) for (unsigned i = 0; i < e.scene->mNumAnimations; ++i) {
            const auto* a = e.scene->mAnimations[i];
            preview->clips.push_back({a->mName.C_Str(), clipDurationSec(e.scene, a->mName.C_Str()).value_or(0)});
        }
    } else if (advanceClock && motion.preview) {
        mc2clip::Phase phase;
        const auto gesture = mc2clip::select(motion.gestureId, 0, 0, 0, false, false, "", phase);
        const auto clip = mc2clip::previewClip(e.showcaseClip, gesture.clip, e.scene->mNumAnimations ? e.scene->mAnimations[0]->mName.C_Str() : "",
            [&](std::string_view name) { return clipDurationSec(e.scene, std::string(name)).has_value(); });
        if (s.activeClip != clip) { s.activeClip = clip; s.clipTimeSec = 0; }
        s.durationSec = clipDurationSec(e.scene, s.activeClip).value_or(1.0f);
        s.mode = mc2clip::Mode::Loop;
        s.playRate = 1.0f;
    } else if (advanceClock && e.pinnedClip.empty()) {
        float speed = 0.0f, turnRate = 0.0f;
        if (s.havePrev) {
            const float ddx = motion.px - s.prevPx, ddy = motion.py - s.prevPy, ddz = motion.pz - s.prevPz;
            speed = sqrtf(ddx*ddx + ddy*ddy + ddz*ddz) / dt;
            float dh = motion.legHeadingDeg - s.prevHeading;
            while (dh > 180.0f) dh -= 360.0f;
            while (dh < -180.0f) dh += 360.0f;
            turnRate = dh / dt;
        }
        s.prevPx = motion.px; s.prevPy = motion.py; s.prevPz = motion.pz;
        s.prevHeading = motion.legHeadingDeg; s.havePrev = true;
        const bool standing = (motion.gestureId != 4 && motion.gestureId != 7 &&
                               motion.gestureId != 9 && motion.gestureId != 20);
        s.turnStreak = (standing && speed < 1.0f && fabsf(turnRate) > 12.0f) ? (s.turnStreak + 1) : 0;
        // P9: event clips (hits, falls, get-up, death, jump phases) switch at once and play
        // from their first frame; locomotion keeps the 4-frame debounce and its phase.
        const bool finished = (s.mode == mc2clip::Mode::Once) && s.clipTimeSec >= s.durationSec - 1e-4f;
        const mc2clip::Choice want = mc2clip::select(motion.gestureId, speed, turnRate, s.turnStreak,
                                                     motion.destroyed, finished, s.activeClip.c_str(), s.phase);
        auto adopt = [&](const mc2clip::Choice& c, bool restart) {
            if (auto nd = clipDurationSec(e.scene, c.clip)) {
                s.activeClip = c.clip;
                s.durationSec = *nd > 1e-4f ? *nd : 1.0f;
                s.mode = c.mode;
                s.locomotion = c.locomotion;
                if (restart) s.clipTimeSec = 0.0f;
            }
            s.pendingStreak = 0;
        };
        if (s.activeClip == want.clip) {
            s.pendingStreak = 0;
            s.locomotion = want.locomotion;
        } else if (want.immediate) {
            adopt(want, true);
        } else {
            if (s.pendingClip == want.clip) ++s.pendingStreak;
            else { s.pendingClip = want.clip; s.pendingStreak = 1; }
            if (s.pendingStreak >= 4) adopt(want, s.mode == mc2clip::Mode::Once);
        }
        s.playRate = 1.0f;
        if (s.locomotion) {
            if (e.strideSpeed.contains(s.activeClip))
                s.playRate = mc2clip::strideRate(speed, e.strideSpeed.at(s.activeClip));
        }
    }
    const bool holdAtEnd = (preview || e.pinnedClip.empty()) && s.mode == mc2clip::Mode::Once;
    if (advanceClock) s.clipTimeSec += dt * (preview || e.pinnedClip.empty() ? s.playRate : 1.0f);
    if (s.durationSec > 0.0f) {
        if (holdAtEnd) { s.clipTimeSec = std::min(s.clipTimeSec, s.durationSec); }
        else while (s.clipTimeSec >= s.durationSec) s.clipTimeSec -= s.durationSec;
    }
    if (preview) {
        preview->active = s.activeClip; preview->time = s.clipTimeSec; preview->duration = s.durationSec;
        preview->paletteBones = static_cast<int>(e.invMWrest.size());
    }
    const float frame = s.clipTimeSec * 30.0f;
    double tt = 0, dd = 0;
    return mc2skel::EvaluateClipGpuBones(*e.plan, s.activeClip, frame, globals, &tt, &dd);
}

// Build the GPU per-bone MODEL delta from sampled globals into s.modelDelta, plus the
// per-frame foot-ground lift into s.gpuLift.
void buildGpuModelDelta(const ImportedAnimEntry& e, ActorAnimState& s,
                        const std::vector<mc2skel::GpuBone>& globals) {
    const size_t n = (globals.size() < e.invMWrest.size()) ? globals.size() : e.invMWrest.size();
    // Default: static rest lift (steady torso). MC2_MECH_IMPORT_DYNAMIC_LIFT=1 restores
    // the old per-frame lowest-vertex lift (follows ground but bobs each step).
    const bool dynamicLift = false;
    float lowestY = 1e30f;
    for (size_t i = 0; i < n; ++i) {
        aiMatrix4x4 D = (e.AS1 * rowMajorToAi(globals[i].m)) * e.invMWrest[i];
        aiToRowMajor(D, &s.modelDelta[i * 16]);
        if (dynamicLift && i < e.boneLowest.size()) {
            const aiVector3D p = D * e.boneLowest[i];
            lowestY = std::min(lowestY, p.y);
        }
    }
    s.gpuLift = dynamicLift ? ((lowestY < 1e29f) ? -lowestY : 0.0f) : e.restLift;
}
} // namespace

void mc2mechanim::TickImportedMechs(float dt, unsigned frameStamp, const MechMotion& motion,
                                    const void* actorKey, const void* typeKey, bool advanceClock,
                                    mc2::sim::ImportedAnimState* authoritative) {
    if (g_importedAnims.empty()) return;
    if (dt <= 0.0f) dt = 1.0f / 30.0f;   // defensive: never divide by zero below
    // Find the type entry for this actor's chassis (keyed by its TG_TypeMultiShape).
    ImportedAnimEntry* te = nullptr;
    for (ImportedAnimEntry& en : g_importedAnims)
        if (static_cast<const void*>(en.multi) == typeKey) { te = &en; break; }
    if (!te) return;
    ImportedAnimEntry& e = *te;

    {
        // PER-ACTOR: each actor instance advances its OWN clip + model-delta palette.
        ActorAnimState& s = e.actors[actorKey];
        if (authoritative) static_cast<mc2::sim::ImportedAnimState&>(s) = *authoritative;
        if (s.activeClip.empty()) {                 // first tick for this actor
            s.activeClip   = e.initialClip;
            s.durationSec  = e.initialDuration;
            s.modelDelta.assign(e.invMWrest.size() * 16, 0.0f);
        }
        if (s.lastFrame == frameStamp && !s.lastGlobals.empty()) return;
        s.lastFrame = frameStamp;
        std::vector<mc2skel::GpuBone> globals;
        if (sampleActorClip(e, s, dt, motion, globals, advanceClock)) {
            if (s.activeClip.starts_with("atlas_moveCore")) {
                std::vector<glm::mat4> pose(globals.size());
                for (size_t b = 0; b < globals.size(); ++b)
                    for (int r = 0; r < 4; ++r)
                        for (int c = 0; c < 4; ++c) pose[b][c][r] = globals[b].m[r * 4 + c];
                mc2ground::solve(e.grounding, pose);
                if (s_mechSkelTrace && frameStamp % 6 == 0) {
                    float soles[2] = {1e30f, 1e30f};
                    for (int side = 0; side < 2; ++side)
                        for (const auto& p : e.grounding.legs[side].sole)
                            soles[side] = std::min(soles[side], (pose[p.bone] * glm::vec4(p.local, 1)).y);
                    const auto torso = std::find(e.names.begin(), e.names.end(), "j_Spine2");
                    const size_t b = static_cast<size_t>(torso - e.names.begin());
                    const float pitch = b < pose.size() ? std::atan2(pose[b][1].z, pose[b][1].y) * 57.29578f : 0;
                    MECH_SKEL_TRACE("ground frame=%u clip=%s time=%.4f soles=(%.4f,%.4f) pitch=%.3f",
                        frameStamp, s.activeClip.c_str(), s.clipTimeSec, (soles[0] - e.grounding.floor) * e.scale,
                        (soles[1] - e.grounding.floor) * e.scale, pitch);
                }
                for (size_t b = 0; b < globals.size(); ++b)
                    for (int r = 0; r < 4; ++r)
                        for (int c = 0; c < 4; ++c) globals[b].m[r * 4 + c] = pose[b][c][r];
            }
            s.lastGlobals = std::move(globals);   // 1A: firepoint/hit-node source
            mc2clip::publishPreviewPose(motion.preview, e.gpuMode,
                [&] { buildGpuModelDelta(e, s, s.lastGlobals); },
                [&] { rebakeImportedPresentation(e, s.lastGlobals); });
        }
        if (authoritative) *authoritative = static_cast<const mc2::sim::ImportedAnimState&>(s);
        // Prune despawned actors' states (untouched for a while).
        for (auto it = e.actors.begin(); it != e.actors.end(); ) {
            const auto& [pruneKey, pruneState] = *it;
            const unsigned age = frameStamp - pruneState.lastFrame;
            if (pruneKey != actorKey && age < 0x80000000u && age > ACTOR_STATE_STALE_FRAMES)
                it = e.actors.erase(it);
            else ++it;
        }
        return;
    }


}

//=============================================================================
// Public entry point.
// Every (node, mesh) pair under the root with its accumulated world transform, in depth-first
// node order: the instance list a static import bakes (a mesh used by two nodes appears twice).
static void CollectMeshInstances(const aiNode* node, const aiMatrix4x4& parent,
                                 std::vector<std::tuple<unsigned, const aiNode*, aiMatrix4x4>>& out) {
	const aiMatrix4x4 world = parent * node->mTransformation;
	for (unsigned i = 0; i < node->mNumMeshes; ++i)
		out.emplace_back(node->mMeshes[i], node, world);
	if (!node->mNumMeshes && node->mMetaData && node->mMetaData->HasKey("mc2_anim_node"))
		out.emplace_back(~0u, node, world);
	for (unsigned c = 0; c < node->mNumChildren; ++c)
		CollectMeshInstances(node->mChildren[c], world, out);
}

static bool ContentNumber(const aiMetadata* metadata, unsigned index, float& out) {
    if (!metadata || index >= metadata->mNumProperties) return false;
    double d = 0; float f = 0; int32_t i = 0; uint64_t u = 0;
    if (metadata->Get(index, d)) out = static_cast<float>(d);
    else if (metadata->Get(index, f)) out = f;
    else if (metadata->Get(index, i)) out = static_cast<float>(i);
    else if (metadata->Get(index, u)) out = static_cast<float>(u);
    else return false;
    return std::isfinite(out);
}

static bool ContentVector(const aiMetadata* metadata, std::string_view key, glm::vec3& out) {
    aiMetadata values;
    return metadata && metadata->Get(std::string(key), values) && values.mNumProperties == 3 &&
        ContentNumber(&values, 0, out.x) && ContentNumber(&values, 1, out.y) && ContentNumber(&values, 2, out.z);
}

static float ContentScalar(const aiMetadata* metadata, std::string_view key, float fallback) {
    for (unsigned i = 0; metadata && i < metadata->mNumProperties; ++i)
        if (metadata->mKeys[i] == aiString(std::string(key))) { float value; return ContentNumber(metadata, i, value) ? value : fallback; }
    return fallback;
}

static void CollectContentSockets(const aiNode* node, mc2::content::Asset& asset) {
    const auto* md = node->mMetaData;
    aiString schema;
    if (S_strnicmp(node->mName.C_Str(), "SpotLight_", 10) == 0 || S_strnicmp(node->mName.C_Str(), "LitWin", 6) == 0) {
        mc2::content::Socket legacy;
        if (ContentVector(md, "mc2_pivot_tg_wu", legacy.position)) {
            legacy.name = legacy.parent = node->mName.C_Str();
            legacy.type = S_strnicmp(legacy.name.c_str(), "SpotLight_", 10) == 0 ? "spot" : "point";
            legacy.intensityClass = "legacy";
            asset.sockets.push_back(std::move(legacy));
        }
    }
    if (md && md->Get(std::string("mc2_light_schema"), schema) && schema == aiString("mc2-building-light/1")) {
        mc2::content::Socket socket;
        socket.name = node->mName.C_Str(); socket.parent = socket.name;
        aiString type, intensity;
        if (md->Get(std::string("mc2_light_type"), type)) socket.type = type.C_Str();
        if (md->Get(std::string("mc2_light_intensity_class"), intensity)) socket.intensityClass = intensity.C_Str();
        md->Get(std::string("mc2_light_active"), socket.active);
        md->Get(std::string("mc2_light_lights_out_on_destroy"), socket.lightsOutOnDestroy);
        if (ContentVector(md, "position_tg_wu", socket.position) && ContentVector(md, "direction_tg", socket.direction)) {
            ContentVector(md, "mc2_light_colour_linear", socket.colour);
            socket.range = ContentScalar(md, "range_wu", 0);
            socket.outerConeDegrees = ContentScalar(md, "mc2_light_outer_cone_degrees", 0);
            asset.sockets.push_back(std::move(socket));
        }
    }
    for (unsigned c = 0; c < node->mNumChildren; ++c) CollectContentSockets(node->mChildren[c], asset);
}

long ImportGeometryFromFile(mc2::czstring path, TG_TypeMultiShape* out, bool autoGround, bool authoredColors,
                            bool bakeNodeTransforms) {
	if (!path || !out) return -1;
	// Every .glb checks the native cook cache before any parse (S1). Only unskinned, self-contained
	// GLBs ever store under this key (see the staticParameters assignment after the parse), so a hit
	// is always a valid unskinned IR; skinned GLBs and external-buffer GLBs never hit.
	const bool staticGlb = std::string_view(path).ends_with(".glb");
	const std::string staticParameters = std::to_string(s_gltfAxis()) + ":" + ContentFloatKey(s_gltfYoff()) + ":" +
		ContentFloatKey(s_gltfYawDeg()) + ":" + std::to_string(authoredColors) +
		(bakeNodeTransforms ? ":static-glb/5-pane-cells" : ":flat-glb/1");
	if (staticGlb) {
		mc2::content::Asset cached;
		cached.ground = autoGround ? 2 : 0;
		cached.parameters = staticParameters;
		if (mc2::content::loadCached(cached, path)) return out->BuildContentIR(std::move(cached), path);
	}
	auto impOwner = std::make_unique<Assimp::Importer>();
	Assimp::Importer& imp = *impOwner;
	auto io = std::make_unique<ContentIO>();
	auto* sources = io.get();
	imp.SetIOHandler(io.release());
	const aiScene* scene = imp.ReadFile(path,
		aiProcess_Triangulate           |
		aiProcess_GenSmoothNormals      |
		aiProcess_JoinIdenticalVertices |
		aiProcess_ValidateDataStructure |
		aiProcess_SortByPType);

	if (!scene) {
		PAUSE(("[importer] %s: Assimp ReadFile failed: %s",
		       path, imp.GetErrorString()));
		return -1;
	}

	if (!ValidateScene(scene, path)) {
		return -1;
	}

	// BT2018-SKEL-ENGINE-1A: if the scene is skinned (a mech with a skeleton),
	// build the shared skeleton + rest-pose joint globals so each mesh-part can
	// be baked to the assembled bind pose below. Static GLB/props (no bones) skip
	// this entirely -> unchanged import path.
	SkelBake bake;
	// 1B-RUNTIME state (used only on the skinned mech path).
	std::vector<ImportPartRec> animParts;
	bool animateImport = false;
	float groundDyApplied = 0.0f;
	bool sceneHasBones = false;
	for (unsigned m = 0; m < scene->mNumMeshes; ++m)
		if (scene->mMeshes[m]->mNumBones > 0) { sceneHasBones = true; break; }
	if (sceneHasBones) {
		mc2skel::BuildSkeleton(scene, bake.names, bake.parents, bake.invBind);
		// BT2018-SKEL-ENGINE-1B: by default bake the REST (bind) pose. If
		// MC2_MECH_IMPORT_FORCE_CLIP=<clipName> is set (+ optional
		// MC2_MECH_IMPORT_FORCE_FRAME=<n>, default 0), bake that clip frame's pose
		// instead — a STATIC posed import (no per-frame runtime yet). Same joint-
		// global path (mc2skel::EvaluateClipGpuBones) the harness oracle uses, so
		// mech_bone_parity --clip/--frame proves they match. Unknown clip -> rest.
		const bool hasForceClip = mc2::gate::isSet<"MC2_MECH_IMPORT_FORCE_CLIP">();
		const std::string forceClipStr = hasForceClip ? mc2::gate::str<"MC2_MECH_IMPORT_FORCE_CLIP">() : std::string();
		const char* fc = hasForceClip ? forceClipStr.c_str() : nullptr;
		if (mechImportGpuEnabled()) {
			// 1B-GPU: VBO must be the assembled REST pose (the runtime model delta is
			// relative to rest); ignore FORCE_CLIP for the static bake. The clip still
			// drives the per-frame palette at runtime.
			mc2skel::EvaluateRestGpuBones(scene, bake.names, bake.rest);
			if (fc) bake.forcedClip = fc;   // FORCE_CLIP still pins the runtime clip
			MECH_SKEL_TRACE("file='%s' GPU mode -> REST bake (runtime model-delta palette)", path);
		} else if (fc) {
			int ff = 0;
			ff = mc2::gate::get<"MC2_MECH_IMPORT_FORCE_FRAME">();
			double bt = 0, bd = 0;
			if (mc2skel::EvaluateClipGpuBones(scene, fc, static_cast<float>(ff), bake.names, bake.rest, &bt, &bd)) {
				bake.forcedClip = fc;
				bake.forcedFrame = ff;
				MECH_SKEL_TRACE("file='%s' FORCE_CLIP='%s' frame=%d (t=%.1f/%.1f) -> posed bake", path, fc, ff, bt, bd);
			} else {
				mc2skel::EvaluateRestGpuBones(scene, bake.names, bake.rest);
				MECH_SKEL_TRACE("file='%s' FORCE_CLIP='%s' NOT FOUND -> rest pose", path, fc);
			}
		} else {
			mc2skel::EvaluateRestGpuBones(scene, bake.names, bake.rest);
		}
		for (size_t i = 0; i < bake.names.size(); ++i) bake.nameIdx[bake.names[i]] = static_cast<int>(i);
		bake.active = !bake.names.empty();
		// 1B-RUNTIME / 1C: opt-in gate enables per-frame animation. With a forced
		// clip it loops that one clip (1B); without, the clip is chosen each frame
		// from the mech's movement (1C). Gate off → static bind/posed bake stands.
		animateImport = mechImportAnimateEnabled() || mechImportGpuEnabled();

		// Auto-scale the assembled bind pose to MC2 mech size: the skinned GLB is
		// authored at Unity scale (~0.15u tall); stock-equivalent mechs are ~25u.
		// Measure the intact bind-pose extent (glTF space) and scale to target
		// height (matches the Python fk_bake --target_height). Override via
		// MC2_MECH_SKEL_HEIGHT. Single-pass measure over intact, bone-bound meshes.
		if (bake.active) {
			const float targetH = mc2::gate::get<"MC2_MECH_SKEL_HEIGHT">();  // default 50 confirmed in-game (marauder)
			float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
			for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
				const aiMesh* me = scene->mMeshes[m];
				if (me->mNumBones == 0 || skelMeshDropped(me->mName.C_Str())) continue;
				auto it = bake.nameIdx.find(me->mBones[0]->mName.C_Str());
				if (it == bake.nameIdx.end()) continue;
				const auto& [boneName, boneIdx] = *it;
				const float* rm = bake.rest[boneIdx].m;
				const aiMatrix4x4& off = me->mBones[0]->mOffsetMatrix;
				for (unsigned v = 0; v < me->mNumVertices; ++v) {
					aiVector3D p = applyRowMajor16(rm, off * me->mVertices[v]);
					float c[3] = {p.x, p.y, p.z};
					for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], c[k]); hi[k] = std::max(hi[k], c[k]); }
				}
			}
			float ext = 0.0f;
			for (int k = 0; k < 3; ++k) ext = (hi[k] - lo[k] > ext) ? (hi[k] - lo[k]) : ext;
			if (ext > 1e-6f) bake.scale = targetH / ext;
			MECH_SKEL_TRACE("file='%s' SKINNED: bones=%zu bindExtent=%.4f scale=%.2f -> targetH=%.1f",
			                path, bake.names.size(), ext, bake.scale, targetH);
		}

		// MECH-BONE-PARITY-GATE-1: dump the engine's REST joint-global bone
		// matrices (the same mc2skel output the bake consumes) as JSON when
		// MC2_MECH_SKEL_BONE_DUMP=<path> is set. Same shape as the harness
		// `gpu-bones --rest` output, so a diff script can prove the engine and the
		// CLI oracle compute identical bones (and, in 1B, identical per-frame).
		if (mc2::gate::isSet<"MC2_MECH_SKEL_BONE_DUMP">()) {
			const std::string dumpPathStr = mc2::gate::str<"MC2_MECH_SKEL_BONE_DUMP">();
			const char* dumpPath = dumpPathStr.c_str();
			if (FILE* bf = fopen(dumpPath, "w")) {
				double sum = 0.0;
				for (const auto& b : bake.rest) for (const auto mElem : b.m) sum += mElem;
				const char* clipLabel = bake.forcedClip.empty() ? "rest" : bake.forcedClip.c_str();
				mc2::fmt::print(bf, "{{\n  \"clip\": \"{}\",\n  \"frame\": {:d},\n  \"boneCount\": {:d},\n  \"checksum\": {:.6f},\n  \"bones\": [\n", mc2::fmt::safe(clipLabel), mc2::fmt::as_i32(bake.forcedFrame), mc2::fmt::as_size(bake.names.size()), sum);
				for (size_t i = 0; i < bake.names.size(); ++i) {
					mc2::fmt::print(bf, "    {{\"index\": {:d}, \"name\": \"{}\", \"m\": [", mc2::fmt::as_size(i), bake.names[i].c_str());
					for (int k = 0; k < 16; ++k) mc2::fmt::print(bf, "{}{:.6f}", mc2::fmt::safe(k ? ", " : ""), bake.rest[i].m[k]);
					mc2::fmt::print(bf, "]}}{}\n", mc2::fmt::safe(i + 1 < bake.names.size() ? "," : ""));
				}
				mc2::fmt::print(bf, "  ]\n}}\n");
				fclose(bf);
				MECH_SKEL_TRACE("bone dump -> %s (boneCount=%zu checksum=%.6f)",
				                dumpPath, bake.names.size(), sum);
			}
		}
	}

	mc2::content::Asset asset;
	asset.ground = autoGround ? 2 : 0;
    asset.parameters = std::to_string(s_gltfAxis()) + ":" + ContentFloatKey(s_gltfYoff()) + ":" +
        ContentFloatKey(s_gltfYawDeg()) + ":" + std::to_string(authoredColors) + ":" + std::to_string(bakeNodeTransforms) +
        ":" + bake.forcedClip + ":" + std::to_string(bake.forcedFrame) + ":" + ContentFloatKey(bake.scale);
    if (bakeNodeTransforms) asset.parameters += ":nested-pivots/1:windows/3-pane-cells";
    std::sort(sources->dependencies.begin(), sources->dependencies.end());
    for (const auto& [dependencyPath, digest] : sources->dependencies)
        asset.parameters += ":" + std::to_string(digest);
    // Skinned imports keep their skeleton/clip path; they never populate the
    // static cache namespace that can bypass scene decoding.
    // Self-contained means the main file is the only dependency Assimp opened.
    if (staticGlb && !bake.active && sources->dependencies.size() <= 1) asset.parameters = staticParameters;
    if (!bake.active && mc2::content::loadCached(asset, path))
        return out->BuildContentIR(std::move(asset), path);
    CollectContentSockets(scene->mRootNode, asset);
	if (bake.active) {
		// SKINNED MECH: merge every intact baked part into ONE shape (the proven
		// static-GLB structure). Per-mesh shapes would leave dropped parts as empty
		// NULL-vertex shapes that crash the mech GPU/recipe path.
		asset.nodes.resize(1);
		// Mech skin = texture SLOT 0 (resetPaintScheme binds slot 0). The skinned
		// GLB lists blip atlases at material 0/1, so build a single-slot list with
		// the BASE atlas (the intact body's material) at slot 0; all merged tris
		// reference slot 0. (Weapons/second-atlas mechs are a later slice.)
		// Pick the variant skin (Widow/BHA/SLDF) over the plain Base when a GLB ships
		// both. A skin material name contains the variant token ("widow"/"bha"/"sldf");
		// base is plain "<chassis>_base". First skinned mesh sets a fallback; the scan
		// keeps going and upgrades to a variant if one shows up.
		auto isVariantMat = [](mc2::czstring mn){
			if (!mn) return false;
			std::string s(mn); for (char& c : s) c = static_cast<char>(tolower(c));
			return s.find("widow") != std::string::npos
			    || s.find("bha")   != std::string::npos
			    || s.find("sldf")  != std::string::npos;
		};
		std::string baseTex = "NULLTXM";
		bool gotVariant = false;
		for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
			const aiMesh* me = scene->mMeshes[m];
			if (me->mNumVertices == 0 || me->mNumBones == 0 || skelMeshDropped(me->mName.C_Str())) continue;
			const aiMaterial* mat = scene->mMaterials[me->mMaterialIndex];
			aiString matName; mat->Get(AI_MATKEY_NAME, matName);
			auto nm = DeriveMC2TextureName(scene, mat);
			if (!nm) continue;
			if (isVariantMat(matName.C_Str())) { baseTex = *nm; gotVariant = true; break; }
			if (!gotVariant && baseTex == "NULLTXM") baseTex = *nm;
		}
		asset.materials.push_back({baseTex, false});
		MECH_SKEL_TRACE("file='%s' mech skin slot0 tex='%s'", path, baseTex.c_str());
		// 1B-RUNTIME: when animating, capture each part's layout so the merge can be
		// registered for per-frame re-bake (requires a valid forced clip).
		if (animateImport) bake.recParts = &animParts;
		long r = PopulateMergedSkinnedShape(scene, asset, bake);
		if (r != 0) return r;
	} else if (bakeNodeTransforms) {
	// Static override: one TG_TypeShape per (node, mesh) instance with its world transform baked.
	std::vector<std::tuple<unsigned, const aiNode*, aiMatrix4x4>> instances;
	CollectMeshInstances(scene->mRootNode, aiMatrix4x4(), instances);
	asset.nodes.resize(instances.size());
	BuildTextureList(scene, asset);
	for (size_t i = 0; i < instances.size(); ++i) {
		const auto& [meshIdx, node, world] = instances[i];
		if (meshIdx == ~0u) {
			auto& helper = asset.nodes[i];
			helper.name = node->mName.C_Str(); helper.parent = "None"; helper.helper = true;
			helper.center = helper.relativeCenter = toMC2Pos(aiVector3D(world.a4, world.b4, world.c4));
			if (node->mParent && node->mParent->mMetaData && node->mParent->mMetaData->HasKey("mc2_anim_node")) {
				helper.parent = node->mParent->mName.C_Str();
				helper.relativeCenter = toMC2Pos(aiVector3D(node->mTransformation.a4, node->mTransformation.b4, node->mTransformation.c4));
			}
			continue;
		}
		long r = ImportShapeFromMesh(scene, meshIdx, asset.nodes[i], nullptr, nullptr,
		                             authoredColors, node, &world);
		if (r != 0) {
			return r;
		}
	}
	} else {
	// Allocate the multi-shape's listOfTypeShapes[] and per-slot TG_TypeShape
	// instances. One Assimp mesh → one TG_TypeShape (MVP; LODs not embedded).
	asset.nodes.resize(scene->mNumMeshes);

	// Build the multi-shape's TG_Texture[] before populating shapes — the
	// per-shape TG_TinyTexture wiring (CreateListOfTextures) reads from the
	// multi-shape table.
	BuildTextureList(scene, asset);

	// Vertex-tight box: reset to empty extremes, then each ImportShapeFromMesh
	// expands min/max over its vertices; ComputeBoundingBox finalizes below.

	// Populate each shape from its mesh.
	for (unsigned i = 0; i < scene->mNumMeshes; i++) {
		long r = ImportShapeFromMesh(scene, i, asset.nodes[i], nullptr,
		                             bake.active ? &bake : nullptr, authoredColors);
		if (r != 0) {
			return r;
		}
	}
	}  // end else (non-skinned per-mesh path)

	if (bake.active) {
		MECH_SKEL_TRACE("file='%s' bones=%zu imported_parts=%d single_bone=%d "
		                "offset_baked=%d dropped=%d selected_clip=bindpose",
		                path, bake.names.size(), bake.importedParts,
		                bake.partsSingleBone, bake.partsOffsetBaked, bake.droppedParts);
	}

	if (out->BuildContentIR(std::move(asset), path, &groundDyApplied) != NO_ERR) return -1;

	// 1B-RUNTIME: register the merged skinned mech for per-frame re-bake. After
	// auto-ground so the stored offset matches the static pose. Reuses the forced
	// clip as the loop. Keeps its own Assimp scene alive (session lifetime).
	if (animateImport && bake.active && !animParts.empty())
		RegisterImportedAnim(path, out, bake, animParts, groundDyApplied, impOwner);

	SPEW(("ASSIMP", "%s: %u meshes, %u materials imported",
	      path, scene->mNumMeshes, scene->mNumMaterials));
	return 0;
}

#endif // ENABLE_ASSIMP_IMPORTER
