//---------------------------------------------------------------------------
//
//	bdactor.cpp - This file contains the code for the building and tree appearance classes
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef BDACTOR_H
#include"bdactor.h"
#endif

#include <map>    // STATIC-REGISTRY-COVERAGE-RECON-1: top shape names
#include <mutex>  // STATIC-REGISTRY-COVERAGE-RECON-1: shape map guard

// [TOBJSPLIT v1] RDTSC includes for the BldgAppearance/TreeAppearance probe
// points. __rdtsc() overhead ~5-10ns (cost_split_instrumentation_is_observer_
// effect_dominated.md). Accumulators defined in code/terrobj.cpp.
#include <intrin.h>
#include <stdlib.h>
#include <atomic>  // std::atomic — FRAME-JOBS-2 precondition (s_animTypeIdleNowStatic)
// File-scope gate: one getenv per TU, process-start-constant, shared across
// BldgAppearance::recalcBounds and TreeAppearance::recalcBounds. Matches the
// file-scope pattern in code/terrobj.cpp:176.
static bool s_tobjSplitBdOn = (getenv("MC2_TOBJ_COST_SPLIT") != nullptr);

// BLDG-TYPE-ANIM-GATE-FIX-1 kill-switch.
// Unset/1 = new: idle animatable-type buildings eligible for static path.
// 0       = legacy: bldgTypeHasAnimations disqualifies entire type.
static bool s_bldgTypeAnimStaticEligible = []() -> bool {
    const char* v = getenv("MC2_BLDG_TYPE_ANIM_STATIC_ELIGIBLE");
    if (!v) return true;
    return !(v[0] == '0');
}();

// BLDG-TYPE-ANIM-GATE-FIX-1 diagnostic counters (cumulative).
// s_animTypeIdleNowStatic is atomic: FRAME-JOBS-2 will run BldgAppearance::touch()
// on worker threads; this counter is incremented there. relaxed ordering is
// sufficient — it is a diagnostic accumulator, not a synchronisation point.
static std::atomic<uint32_t> s_animTypeIdleNowStatic{0};
static uint32_t s_animStartInvalidated    = 0;
static uint32_t s_animStateToStateGesture = 0;

// STATIC-SCENE-PROXY-RECON-1: per-window classification counters.
// Gated by MC2_STATIC_PROXY_RECON env var. No behavior change — read-only.
// "proxy candidate" = object that is fully stable and could be baked at mission
// load rather than evaluated every frame in Phase 2 (touchSerialCommit).
// Rejection categories mirror the stableLightSkipEligible criteria exactly.
static const bool s_proxyReconEnabled = (getenv("MC2_STATIC_PROXY_RECON") != nullptr);
static std::atomic<int64_t> g_spr_phase2Calls{0};    // total touchSerialCommit calls (bldg+tree)
static std::atomic<int64_t> g_spr_bldgCalls{0};      // BldgAppearance::touchSerialCommit calls
static std::atomic<int64_t> g_spr_treeCalls{0};      // TreeAppearance::touchSerialCommit calls
static std::atomic<int64_t> g_spr_proxyCandidate{0}; // fully stable — all criteria met
static std::atomic<int64_t> g_spr_rejNoShape{0};     // bldgShape/treeShape is null (returned early)
static std::atomic<int64_t> g_spr_rejNoStaticReg{0}; // !staticReg.registered || recipeIndex < 0
static std::atomic<int64_t> g_spr_rejBadLightIdx{0}; // lightDataIndex == 0xFFFFFFFFu
static std::atomic<int64_t> g_spr_rejNoValidLight{0};// !hasValidStaticLight (bldg only)
static std::atomic<int64_t> g_spr_rejLightGenMismatch{0}; // lastLightEnvGen != currentLightEnvGen (bldg only)
static std::atomic<int64_t> g_spr_rejNeedsFullBake{0};    // needsFullBakeNextFrame
static std::atomic<int64_t> g_spr_callCounter{0};    // monotonic call count for print trigger

// STATIC-REGISTRY-COVERAGE-RECON-1: sub-classify the rej_no_static_reg population.
// Gated by MC2_STATIC_REG_COVERAGE env var. No behavior change — read-only.
// Answers why mc2_24 has ~72% rej_no_static_reg vs mc2_10 99% proxy_candidate.
// Sub-conditions (first-fail priority, same as stableLightSkipEligible order):
//   never_registered  — !staticReg.registered (registerStatic never completed)
//   no_recipe         — registered but recipeIndex<0 (shouldn't normally occur)
//   bake_not_enabled  — mc2LightBakeEnabled()=false blocks EmitBakedGpuLightData
//   not_in_bake_table — bake on but recipeIndex not yet in s_bakedStaticLight
// Also tracks: isStaticEligible() for the unregistered set, top shape names.
static const bool s_regCovEnabled = (getenv("MC2_STATIC_REG_COVERAGE") != nullptr);
static std::atomic<int64_t> g_rc_neverRegistered{0};   // !staticReg.registered
static std::atomic<int64_t> g_rc_noRecipe{0};          // registered && recipeIndex<0
static std::atomic<int64_t> g_rc_bakeNotEnabled{0};    // bake gate off (within rej_no_static_reg)
static std::atomic<int64_t> g_rc_notInBakeTable{0};    // bake on but recipe not in table
static std::atomic<int64_t> g_rc_isStaticEligibleYes{0}; // unregistered but would be eligible
static std::atomic<int64_t> g_rc_isStaticEligibleNo{0};  // unregistered and not eligible
// Top shape names for the never_registered bucket (guarded by mutex, main-thread only).
static std::mutex            g_rc_shapeMu;
static std::map<std::string, int> g_rc_shapeNames;

// T1.15 SpotLight_ illumination diagnostic — registration probe (bldg class).
// Env-gated per Debug Instrumentation Rule. First-hit is always-on (one stderr
// line per BldgAppearance instance that walks the lazy-init block). Periodic
// summary emits every 600 update() calls when env=1. Demote-not-delete.
static const bool s_spotDiagBldgEnabled = (getenv("MC2_SPOT_DIAG") != nullptr);
static unsigned long s_spotDiagBldgRegistered = 0;   // total lights added
static unsigned long s_spotDiagBldgOverflows  = 0;   // pool-overflow count
static unsigned long s_spotDiagBldgActors     = 0;   // actor first-hits
static unsigned long s_spotDiagBldgCalls      = 0;   // update() call counter

#include "gos_static_prop_killswitch.h"
#include "gos_static_prop_batcher.h"

// Slim-deploy texture gate (LOAD_TGA_THEN_GPU_KTX). The static-prop / building /
// tree appearance loaders below gate loadTexture() behind fileExists(<name>.tga).
// fileExists is .ktx2-blind, so when a slim deploy drops the redundant .tga the
// gate fails, loadTexture is never called, and route-2 (txmmgr.cpp,
// MC2_TEXMGR_KTX_PRIMARY) -- which builds a usable node from the BC7 .ktx2
// sidecar alone -- is never reached; the static-prop batcher then skips the
// packet (layer=-1) and buildings vanish. This helper lets the gate also pass
// when a same-stem .ktx2 sidecar exists, gated in parity with route-2 (only when
// MC2_TEXMGR_KTX_PRIMARY is enabled). bdactor-only by design: mech/vehicle paint
// (CPU_RGBA_REQUIRED, gos_LockTexture rewrite) must stay .tga and is NOT routed
// here. See docs/texture-residency-registry-recon.md.
static bool textureOrKtxSidecarExists(const char* tgaPath)
{
	if (fileExists(tgaPath))
		return true;
	const char* v = getenv("MC2_TEXMGR_KTX_PRIMARY");
	const bool ktxPrimary = (!v || !v[0]) ? true : (v[0] != '0');  // default mode 1
	if (!ktxPrimary)
		return false;
	char ktx[1024];
	strncpy(ktx, tgaPath, sizeof(ktx) - 1);
	ktx[sizeof(ktx) - 1] = 0;
	char* dot   = strrchr(ktx, '.');
	char* slash = strrchr(ktx, '/');
	if (dot && (!slash || dot > slash)) *dot = 0;
	if (strlen(ktx) + 6 < sizeof(ktx)) strcat(ktx, ".ktx2");
	return fileExists(ktx);
}
#include "gos_static_prop_registry.h"  // Stage 3.C: static-registry fast path
#include "cliff_decal_tuning.h"  // TERRAIN-DECAL-SLICE-0C: shared face-frame + live tuning
#include "gos_mech_killswitch.h"       // g_mechPreviewRenderDepth (component preview-fix)
#include "../GameAdapters/StaticPropRenderAdapter.h"  // M1 RenderWorld Tasks 8-11
#include <unordered_map>  // LODBUG probe: tracks per-actor previous bldgShape*
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <GL/glew.h>
#include "../RenderCore/KtxLoader.h"
#include "../RenderCore/MaterialGpu.h"
#include "gos_object_parity_query.h"  // IsDualEmitArmed — Stage 2.D.2 dual-emit hook
#include <set>            // [LIGHTSLOT v1] Task 0 cardinality gate
#include "gos_smoke.h"    // [LIGHTSLOT v1] SmokeMode::missionHasStarted()
#include "gos_object_recon_tracy.h"  // [OBJECT_RECON v1] slice-2 recon-zero
#include "cpu_proj_cost_split.h"      // F3 CPU projection cost-baseline (RAII scope)
#include "spotlight_diag.h"  // T1.16 — (E)-owned slot tagging for per-slot probe
#include "gos_profiler.h"  // PERF DIAGNOSTIC 2026-05-06: ZoneScopedN for per-update breakdown

static bool staticRegIsNaturalBuildingName(const char* name)
{
    if (!name)
        return false;
    return (std::strncmp(name, "Pine", 4) == 0) ||
           (std::strncmp(name, "Oak", 3) == 0) ||
           (std::strncmp(name, "Birch", 5) == 0) ||
           (std::strncmp(name, "Willow", 6) == 0) ||
           (std::strncmp(name, "rock_", 5) == 0);
}

// MODEL-OVERRIDE dual-shape (Slice 2): registry resolve + direct geometry import.
#include "model_override_registry.h"
#include "assimp_importer.h"

static bool buildingPbrGateEnabled()
{
	const char* v = getenv("MC2_BUILDING_PBR");
	return v && v[0] == '1' && v[1] == '\0';
}

static bool readTextFile(const char* path, std::string& out)
{
	std::ifstream f(path, std::ios::in | std::ios::binary);
	if (!f)
		return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

static bool extractJsonString(const std::string& text, const char* key, std::string& out)
{
	std::string needle = "\"";
	needle += key;
	needle += "\"";
	size_t p = text.find(needle);
	if (p == std::string::npos)
		return false;
	p = text.find(':', p + needle.size());
	if (p == std::string::npos)
		return false;
	p = text.find('"', p + 1);
	if (p == std::string::npos)
		return false;
	size_t e = text.find('"', p + 1);
	if (e == std::string::npos)
		return false;
	out.assign(text, p + 1, e - p - 1);
	return true;
}

static bool extractJsonNumber(const std::string& text, const char* key, float fallback, float& out)
{
	out = fallback;
	std::string needle = "\"";
	needle += key;
	needle += "\"";
	size_t p = text.find(needle);
	if (p == std::string::npos)
		return true;
	p = text.find(':', p + needle.size());
	if (p == std::string::npos)
		return false;
	char* end = NULL;
	const char* start = text.c_str() + p + 1;
	out = (float)strtod(start, &end);
	return end != start;
}

static bool extractJsonObject(const std::string& text, const char* key, std::string& out)
{
	std::string needle = "\"";
	needle += key;
	needle += "\"";
	size_t p = text.find(needle);
	if (p == std::string::npos)
		return false;
	p = text.find('{', p + needle.size());
	if (p == std::string::npos)
		return false;
	int depth = 0;
	for (size_t i = p; i < text.size(); ++i) {
		if (text[i] == '{')
			++depth;
		else if (text[i] == '}') {
			--depth;
			if (depth == 0) {
				out.assign(text, p, i - p + 1);
				return true;
			}
		}
	}
	return false;
}

static bool extractJsonBool(const std::string& text, const char* key, bool fallback, bool& out)
{
	out = fallback;
	std::string needle = "\"";
	needle += key;
	needle += "\"";
	size_t p = text.find(needle);
	if (p == std::string::npos)
		return true;
	p = text.find(':', p + needle.size());
	if (p == std::string::npos)
		return false;
	const char* start = text.c_str() + p + 1;
	while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
		++start;
	if (strncmp(start, "true", 4) == 0) {
		out = true;
		return true;
	}
	if (strncmp(start, "false", 5) == 0) {
		out = false;
		return true;
	}
	return false;
}

static bool extractBuildingPbrAxis(const char* sidecarPath, int& axis)
{
	axis = -1;
	std::string text;
	if (!readTextFile(sidecarPath, text))
		return false;

	std::string axisMapping;
	if (extractJsonString(text, "axis_mapping", axisMapping)) {
		size_t p = axisMapping.find("MC2_GLTF_AXIS=");
		if (p != std::string::npos) {
			p += strlen("MC2_GLTF_AXIS=");
			if (p < axisMapping.size() && axisMapping[p] >= '0' && axisMapping[p] <= '3') {
				axis = axisMapping[p] - '0';
				return true;
			}
		}
	}

	float numericAxis = -1.0f;
	if (extractJsonNumber(text, "gltf_axis", -1.0f, numericAxis)) {
		const int asInt = (int)numericAxis;
		if (asInt >= 0 && asInt <= 3) {
			axis = asInt;
			return true;
		}
	}
	return false;
}

static bool extractBuildingPbrYaw(const char* sidecarPath, float& yawDegrees)
{
	yawDegrees = 0.0f;
	std::string text;
	if (!readTextFile(sidecarPath, text))
		return false;
	return extractJsonNumber(text, "yaw_degrees", 0.0f, yawDegrees);
}

static void loadBuildingImportSourceWithSidecarAxis(TG_TypeMultiShape* shape, const char* importSourceBase)
{
	if (!shape || !importSourceBase || !importSourceBase[0]) {
		return;
	}

	FullPathFileName sidecarName;
	sidecarName.init(tglPath, importSourceBase, ".mcasset.json");

	int axis = -1;
	const bool hasAxis = extractBuildingPbrAxis(sidecarName, axis);
	float yawDegrees = 0.0f;
	const bool hasYaw = extractBuildingPbrYaw(sidecarName, yawDegrees);
	char axisValue[8] = {0};
	char yawValue[32] = {0};
	const char* oldAxis = getenv("MC2_GLTF_AXIS");
	const char* oldYaw = getenv("MC2_GLTF_YAW_DEG");
	std::string oldAxisValue = oldAxis ? oldAxis : "";
	std::string oldYawValue = oldYaw ? oldYaw : "";
	if (hasAxis) {
		snprintf(axisValue, sizeof(axisValue), "%d", axis);
		_putenv_s("MC2_GLTF_AXIS", axisValue);
	}
	if (hasYaw) {
		snprintf(yawValue, sizeof(yawValue), "%.3f", yawDegrees);
		_putenv_s("MC2_GLTF_YAW_DEG", yawValue);
	}

	shape->LoadFromFile(importSourceBase);

	if (hasYaw) {
		if (!oldYawValue.empty())
			_putenv_s("MC2_GLTF_YAW_DEG", oldYawValue.c_str());
		else
			_putenv_s("MC2_GLTF_YAW_DEG", "");
	}
	if (hasAxis) {
		if (!oldAxisValue.empty())
			_putenv_s("MC2_GLTF_AXIS", oldAxisValue.c_str());
		else
			_putenv_s("MC2_GLTF_AXIS", "");
	}
}

static std::string pathDir(const char* path)
{
	std::string s = path ? path : "";
	size_t p = s.find_last_of("\\/");
	return (p == std::string::npos) ? std::string() : s.substr(0, p + 1);
}

static std::string joinSidecarPath(const std::string& sidecarDir, const std::string& p)
{
	if (p.size() > 2 && p[1] == ':')
		return p;
	if (p.compare(0, 5, "data/") == 0 || p.compare(0, 5, "data\\") == 0)
		return p;
	if (!p.empty() && (p[0] == '/' || p[0] == '\\'))
		return p;
	std::string joined = sidecarDir + p;
	for (size_t i = 0; i < joined.size(); ++i)
		if (joined[i] == '/')
			joined[i] = '\\';
	return joined;
}

static DWORD uploadBuildingPbrKtx2(const char* path, bool srgb)
{
	RenderCore::KtxImage img;
	if (!RenderCore::ktxLoadRgba8(path, img) || !img.isCompressed || !GLEW_ARB_texture_compression_bptc)
		return 0;
	uint32_t fmt = srgb ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM : GL_COMPRESSED_RGBA_BPTC_UNORM;
	size_t mip0Bytes = img.pixels.size();
	if (img.mipByteOffsets.size() > 1)
		mip0Bytes = (size_t)img.mipByteOffsets[1];
	return gos_NewCompressedTexture2D(fmt, img.width, img.height, img.pixels.data(), mip0Bytes, path);
}

static bool loadBuildingPbrFromSidecar(BldgAppearanceType* type, const char* sidecarPath)
{
	if (!type || !buildingPbrGateEnabled())
		return false;

	std::string text;
	if (!readTextFile(sidecarPath, text))
		return false;
	if (text.find("\"materials\"") == std::string::npos ||
	    text.find("corrugated_steel_painted") == std::string::npos ||
	    text.find("CorrugatedSteel006A") == std::string::npos)
		return false;

	std::string normalPath;
	std::string ormPath;
	if (!extractJsonString(text, "normal", normalPath) ||
	    !extractJsonString(text, "orm", ormPath))
		return false;

	float tileScale = 2.0f;
	float roughnessBias = 0.0f;
	float metallicInfluence = 0.0f;
	if (!extractJsonNumber(text, "tile_scale", 2.0f, tileScale) ||
	    !extractJsonNumber(text, "roughness_bias", 0.0f, roughnessBias) ||
	    !extractJsonNumber(text, "metallic_influence", 0.0f, metallicInfluence))
		return false;

	const std::string dir = pathDir(sidecarPath);
	const std::string normalFull = joinSidecarPath(dir, normalPath);
	const std::string ormFull = joinSidecarPath(dir, ormPath);
	DWORD normalHandle = uploadBuildingPbrKtx2(normalFull.c_str(), false);
	DWORD ormHandle = uploadBuildingPbrKtx2(ormFull.c_str(), false);
	if (!normalHandle || !ormHandle)
		return false;

	RenderCore::MaterialGpu material = {};
	material.albedoTex = RenderCore::kMaterialTexAbsent;
	material.normalTex = 1;
	material.metallicRoughnessTex = 2;
	material.emissiveTex = RenderCore::kMaterialTexAbsent;
	material.flags = RenderCore::MaterialFlags::kNormalMap |
	                 RenderCore::MaterialFlags::kMetallicRoughness;
	material.baseColorFactor = 1.0f;
	material.metallicFactor = metallicInfluence;
	material.roughnessFactor = 1.0f + roughnessBias;

	GLuint ssbo = 0;
	glGenBuffers(1, &ssbo);
	if (ssbo == 0)
		return false;
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
	glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(material), &material, GL_STATIC_DRAW);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

	type->buildingPbrProgram = gos_getRenderMaterial("building_pbr");
	if (!type->buildingPbrProgram) {
		glDeleteBuffers(1, &ssbo);
		return false;
	}
	type->buildingPbrNormalTexture = normalHandle;
	type->buildingPbrOrmTexture = ormHandle;
	type->buildingPbrMaterialSsbo = (DWORD)ssbo;
	type->buildingPbrTileScale = tileScale;
	type->buildingPbrRoughnessBias = roughnessBias;
	type->buildingPbrMetallicInfluence = metallicInfluence;
	bool preserveLegacyAlpha = false;
	if (extractJsonBool(text, "preserve_legacy_alpha", false, preserveLegacyAlpha))
		type->buildingPbrPreserveLegacyAlpha = preserveLegacyAlpha;
	std::string footprint;
	if (extractJsonObject(text, "footprint_shadow", footprint)) {
		bool fpEnabled = false;
		float fpStrength = 0.35f;
		float fpSoftness = 2.0f;
		float fpHeightBias = 0.05f;
		if (extractJsonBool(footprint, "enabled", false, fpEnabled) &&
		    extractJsonNumber(footprint, "strength", 0.35f, fpStrength) &&
		    extractJsonNumber(footprint, "softness", 2.0f, fpSoftness) &&
		    extractJsonNumber(footprint, "height_bias", 0.05f, fpHeightBias)) {
			type->buildingFootprintShadowEnabled = fpEnabled;
			type->buildingFootprintShadowStrength = std::max(0.0f, std::min(fpStrength, 0.85f));
			type->buildingFootprintShadowSoftness = std::max(0.0f, fpSoftness);
			type->buildingFootprintShadowHeightBias = fpHeightBias;
		}
	}
	type->buildingPbrEnabled = true;
	fprintf(stderr, "[BUILDING_PBR] loaded sidecar=%s normal=%s orm=%s\n",
	        sidecarPath, normalFull.c_str(), ormFull.c_str());
	fflush(stderr);
	return true;
}

static bool buildingFootprintShadowRuntimeEnabled()
{
	const char* v = getenv("MC2_BUILDING_FOOTPRINT_SHADOW");
	return !v || v[0] != '0';
}

static DWORD footprintShadowColor(float strength)
{
	const unsigned int a = (unsigned int)(std::max(0.0f, std::min(strength, 0.85f)) * 255.0f + 0.5f);
	return (DWORD)(a << 24);
}

static Stuff::Vector3D buildingFootprintCorner(const Stuff::Vector3D& origin,
                                               float localX,
                                               float localY,
                                               float rotationDegrees,
                                               float z)
{
	Stuff::Vector3D v;
	v.x = localX;
	v.y = localY;
	v.z = 0.0f;
	if (rotationDegrees != 0.0f)
		Rotate(v, -rotationDegrees);
	v.x += origin.x;
	v.y += origin.y;
	v.z = z;
	return v;
}

static void pushFootprintTri(const Stuff::Vector3D& a, const Stuff::Vector3D& b,
                             const Stuff::Vector3D& c, DWORD ca, DWORD cb, DWORD cc)
{
	WorldOverlayVert tri[3] = {
		{ a.x, a.y, a.z, -1.0f, 0.0f, 1.0f, ca },
		{ b.x, b.y, b.z, -1.0f, 0.0f, 1.0f, cb },
		{ c.x, c.y, c.z, -1.0f, 0.0f, 1.0f, cc },
	};
	gos_PushDecal(tri, 0xffffffffu);
}

static void submitBuildingFootprintShadow(const BldgAppearance* appearance)
{
	if (!appearance || !appearance->appearType || !appearance->bldgShape)
		return;
	const BldgAppearanceType* type = appearance->appearType;
	if (!type->buildingFootprintShadowEnabled || !buildingFootprintShadowRuntimeEnabled())
		return;
	if (type->buildingFootprintShadowStrength <= 0.0f)
		return;

	const Stuff::Vector3D minBox = appearance->bldgShape->GetMinBox();
	const Stuff::Vector3D maxBox = appearance->bldgShape->GetMaxBox();
	const Stuff::Vector3D center = appearance->bldgShape->GetRootNodeCenter();
	const float softness = std::max(0.0f, type->buildingFootprintShadowSoftness);
	const float z = appearance->position.z + type->buildingFootprintShadowHeightBias;
	const DWORD inner = footprintShadowColor(type->buildingFootprintShadowStrength);
	const DWORD outer = 0x00000000u;

	const float ix0 = minBox.x + center.x;
	const float ix1 = maxBox.x + center.x;
	const float iy0 = minBox.z + center.z;
	const float iy1 = maxBox.z + center.z;
	const float ox0 = ix0 - softness;
	const float ox1 = ix1 + softness;
	const float oy0 = iy0 - softness;
	const float oy1 = iy1 + softness;

	Stuff::Vector3D i00 = buildingFootprintCorner(appearance->position, ix0, iy0, appearance->rotation, z);
	Stuff::Vector3D i10 = buildingFootprintCorner(appearance->position, ix1, iy0, appearance->rotation, z);
	Stuff::Vector3D i11 = buildingFootprintCorner(appearance->position, ix1, iy1, appearance->rotation, z);
	Stuff::Vector3D i01 = buildingFootprintCorner(appearance->position, ix0, iy1, appearance->rotation, z);
	Stuff::Vector3D o00 = buildingFootprintCorner(appearance->position, ox0, oy0, appearance->rotation, z);
	Stuff::Vector3D o10 = buildingFootprintCorner(appearance->position, ox1, oy0, appearance->rotation, z);
	Stuff::Vector3D o11 = buildingFootprintCorner(appearance->position, ox1, oy1, appearance->rotation, z);
	Stuff::Vector3D o01 = buildingFootprintCorner(appearance->position, ox0, oy1, appearance->rotation, z);

	pushFootprintTri(i00, i10, i11, inner, inner, inner);
	pushFootprintTri(i00, i11, i01, inner, inner, inner);

	pushFootprintTri(o00, o10, i10, outer, outer, inner);
	pushFootprintTri(o00, i10, i00, outer, inner, inner);
	pushFootprintTri(o10, o11, i11, outer, outer, inner);
	pushFootprintTri(o10, i11, i10, outer, inner, inner);
	pushFootprintTri(o11, o01, i01, outer, outer, inner);
	pushFootprintTri(o11, i01, i11, outer, inner, inner);
	pushFootprintTri(o01, o00, i00, outer, outer, inner);
	pushFootprintTri(o01, i00, i01, outer, inner, inner);
}

#ifndef CAMERA_H
#include"camera.h"
#endif

#ifndef DBASEGUI_H
#include"dbasegui.h"
#endif

#ifndef CIDENT_H
#include"cident.h"
#endif

#ifndef PATHS_H
#include"paths.h"
#endif

#ifndef OBJSTATUS_H
#include"objstatus.h"
#endif

#ifndef UTILITIES_H
#include"utilities.h"
#endif

#ifndef INIFILE_H
#include"inifile.h"
#endif

#ifndef ERR_H
#include"err.h"
#endif

#ifndef TXMMGR_H
#include"txmmgr.h"
#endif

#include"terrain_runtime.h"

#ifndef TIMING_H
#include"timing.h"
#endif

#ifndef CELINE_H
#include"celine.h"
#endif

#ifndef MOVE_H
#include"move.h"
#endif

#include "../code/unitdesg.h" /* just for definition of MIN_TERRAIN_PART_ID and MAX_MAP_CELL_WIDTH */
#include "../code/static_update_counters.h" /* [TOBJSPLIT v1] g_tobjAngularCyc / g_tobjProjCyc extern decls */
#include "gos_static_prop_batcher.h"
//******************************************************************************************
// GPU-VENDOR-DETECT-1: defined in GameOS/gameos/gos_render.cpp. True only on
// NVIDIA GPUs. The GPU-driven static-prop stack (compute cull -> indirect
// multidraw -> persistent-mapped readback) was only validated on AMD RDNA3; on
// NVIDIA registered static props (trees/buildings) can draw nothing. Until that
// is fixed on NVIDIA hardware, default the static->dynamic force-fallback ON for
// NVIDIA so props stay visible (legacy CPU draw). AMD is unaffected.
bool gos_IsNvidiaGPU();
// env unset -> default ON for NVIDIA, OFF elsewhere; env="0" -> force OFF
// (override the NVIDIA default); env set to non-"0" -> force ON everywhere.
static bool bdForceDynamicDefault(const char* envName) {
	const char* v = getenv(envName);
	if (v) return v[0] != '0';
	return gos_IsNvidiaGPU();
}

// STATIC-PROP REGISTRATION CONTRACT v1: surface (rate-limited) when the GPU static
// registry REJECTED a visibility submit for a registered actor — i.e. the actor held a
// dead/tombstoned recipe handle and would have vanished, but the contract routed it to
// the legacy full-bake fallback instead. Capped at 32 lines so it confirms the failure
// mode without spamming a long session (AMD never hits it — its registry isn't stale).
// reason values match GpuStaticPropRegistry::StaticSubmitResult.
static void bdLogStaticRegInvalid(const char* kind, int reason, int regIdx, const char* name) {
	static int s_logged = 0;
	if (s_logged >= 32) return;
	++s_logged;
	static const char* const kReason[] = {
		"Submitted", "NotRegistered", "MissingRange", "Tombstoned", "InvalidRecipe" };
	const char* rn = (reason >= 0 && reason < 5) ? kReason[reason] : "?";
	fprintf(stderr, "[STATIC_REG_INVALID] kind=%s reason=%s regIdx=%d actor=%s "
	                "(falling back to legacy re-bake)\n",
	        kind, rn, regIdx, name ? name : "(null)");
	fflush(stderr);
}

extern float	worldUnitsPerMeter;
extern bool 	drawTerrainGrid;
extern bool		useFog;

extern long 	mechRGBLookup[];
extern long 	mechRGBLookup2[];

// NS3 boundary: engine-canonical definitions (sole consumer is this TU + editor).
// Previously redefined in every game/tool main. Now defined here so the editor
// can link mclib directly without code/mechcmd2.cpp.
int ObjectTextureSize = 128;
bool reloadBounds = false;
MidLevelRenderer::MLRClipper * theClipper = NULL;

extern float	metersPerWorldUnit;
extern bool		useShadows;

extern bool useNonWeaponEffects;
extern bool useHighObjectDetail;
extern bool MLRVertexLimitReached;

#define SPINRATE					90.0f
#define BASE_NODE_RECYCLE_TIME		0.25f
#define MAX_WEAPON_NODES			4

//-----------------------------------------------------------------------------
// MODEL-OVERRIDE texture binding. The dual-shape override RENDER multishape
// (bldgRenderShape / treeRenderShape) is what the GPU static-prop batcher
// registers + draws (BldgAppearance/TreeAppearance::registerStatic). Its
// per-material texture NAMES are assigned by the Assimp importer
// (BuildTextureList -> data/tgl/<size>/<name>.tga). But those names are NOT
// resolved to GOS texture handles anywhere on the *type* shape — the stock
// per-instance loaders (~bdactor.cpp:3786/935) only touch the CreateFrom'd
// per-instance shape, leaving the registered type render shape with
// gosTextureHandle=0xFFFFFFFF. The batcher then sees W<=0 and falls back to a
// borrowed (wrong) layer. Resolve them here, on the type render multishape,
// immediately after a successful override import — mirrors the stock loaders.
static void LoadOverrideRenderShapeTextures(TG_TypeMultiShape* rs)
{
	if (!rs || !mcTextureManager)
		return;
	for (long i = 0; i < rs->GetNumTextures(); i++)
	{
		char txmName[1024];
		rs->GetTextureName(i, txmName, 256);
		if (txmName[0] == 0 || S_stricmp(txmName, "NULLTXM") == 0)
		{
			rs->SetTextureHandle(i, 0xffffffff);
			continue;
		}

		char texturePath[1024];
		sprintf(texturePath, "%s%d" PATH_SEPARATOR, tglPath, ObjectTextureSize);

		FullPathFileName textureName;
		textureName.init(texturePath, txmName, "");

		if (textureOrKtxSidecarExists(textureName))
		{
			// "a_"-prefixed names are the engine's alpha-channel convention.
			const bool alpha = (S_strnicmp(txmName, "a_", 2) == 0);
			DWORD gosTextureHandle = mcTextureManager->loadTexture(
				textureName,
				alpha ? gos_Texture_Alpha : gos_Texture_Solid,
				gosHint_DisableMipmap | gosHint_DontShrink);
			gosASSERT(gosTextureHandle != 0xffffffff);
			rs->SetTextureHandle(i, gosTextureHandle);
			rs->SetTextureAlpha(i, alpha);
			if (getenv("MC2_MODOVERRIDE_TRACE"))
			{
				fprintf(stderr, "[MODOVERRIDE_TEX] slot=%ld name='%s' -> gosHandle=%lu alpha=%d\n",
				        i, txmName, (unsigned long)gosTextureHandle, (int)alpha);
				fflush(stderr);
			}
		}
		else
		{
			rs->SetTextureHandle(i, 0xffffffff);
			if (getenv("MC2_MODOVERRIDE_TRACE"))
			{
				fprintf(stderr, "[MODOVERRIDE_TEX] slot=%ld name='%s' NOT FOUND (path=%s)\n",
				        i, txmName, (const char*)textureName);
				fflush(stderr);
			}
		}
	}
}

// EDITOR-STATIC-TEXTURE-PREWARM-1: public seam over LoadOverrideRenderShapeTextures.
// Only the editor palette prime path calls this (see bdactor.h). Keeps the helper
// static while letting the editor TU force a type render-shape's textures resident
// before its one-shot finalizeGeometry() texture probe runs. NULL-guarded by the
// helper itself (also guards mcTextureManager).
void Bldg_ForceRenderShapeTexturesResident(TG_TypeMultiShape* rs)
{
	if (rs)
		LoadOverrideRenderShapeTextures(rs);
}
//-----------------------------------------------------------------------------
// class BldgAppearanceType
void BldgAppearanceType::init (const char * fileName)
{
	AppearanceType::init(fileName);

	//----------------------------------------------
	FullPathFileName iniName;
	iniName.init(tglPath,fileName,".ini");

	FitIniFile iniFile;
	long result = iniFile.open(iniName);
	if (result != NO_ERR)
		STOP(("Could not find building appearance INI file %s",iniName));

	// ASSIMP-BLDG-IMPORT-1 — optional GLB probe for buildings/trees. Mirror of
	// mech3d.cpp [Import] pattern. An optional [Import] section with Source=
	// opts this asset into LoadFromFile (glb/fbx probe) for LOD0. Stock assets
	// with no [Import] block take the unchanged LoadTGMultiShapeFromASE path.
	char importSourceBase[256] = "";
	if (iniFile.seekBlock("Import") == NO_ERR &&
	    iniFile.readIdString("Source", importSourceBase, 255) == NO_ERR &&
	    importSourceBase[0])
	{
		char* dot = strrchr(importSourceBase, '.');
		if (dot) *dot = '\0';
	}

	result = iniFile.seekBlock("TGLData");
	if (result != NO_ERR)
		Fatal(result,"Could not find block in building appearance INI file");

	result = iniFile.readIdBoolean("SpinMe",spinMe);
	if (result != NO_ERR)
		spinMe = false;
		
	float nFrameRate = 0.0f;
	result = iniFile.readIdFloat("FrameRate",nFrameRate);
	if (result != NO_ERR)
		nFrameRate = 0.0f;

	result = iniFile.readIdBoolean("ForestClump",isForestClump);
	if (result != NO_ERR)
		isForestClump = false;
	   
	DWORD hotPinkRGB, hotGreenRGB, hotYellowRGB;
	result = iniFile.readIdULong("HotPinkRGB",hotPinkRGB);
	if (result != NO_ERR)
		hotPinkRGB = 0xffff00ff;
		
	result = iniFile.readIdULong("HotGreenRGB",hotGreenRGB);
	if (result != NO_ERR)
		hotGreenRGB = 0xff00ff00;
		
	result = iniFile.readIdULong("HotYellowRGB",hotYellowRGB);
	if (result != NO_ERR)
		hotYellowRGB = 0xffffff00;

	result = iniFile.readIdULong("TerrainLightRGB",terrainLightRGB);
	if (result != NO_ERR)
	{
		terrainLightRGB = 0xffffffff;
	}
	else
	{
		result = iniFile.readIdFloat("TerrainLightIntensity",terrainLightIntensity);
		if (result != NO_ERR)
			terrainLightIntensity = 0.5f;
			
		result = iniFile.readIdFloat("TerrainLightInnerRadius",terrainLightInnerRadius);
		if (result != NO_ERR)
			terrainLightInnerRadius = 100.0f;
			
		result = iniFile.readIdFloat("TerrainLightOuterRadius",terrainLightOuterRadius);
		if (result != NO_ERR)
			terrainLightOuterRadius = 250.0f;
	}
	
	char aseFileName[512];
	// MODEL-OVERRIDE dual-shape: capture the BASE (LOD0) asset name before the
	// damage block below reuses aseFileName, so the override resolve keys off
	// the base shape, not the damage shape.
	char bldgBaseName[512];
	bldgBaseName[0] = 0;
	result = iniFile.readIdString("FileName",aseFileName,511);
	if (result != NO_ERR)
	{
		//Check for LOD filenames instead
		for (int i=0;i<MAX_LODS;i++)
		{
			char baseName[256];
			char baseLODDist[256];
			sprintf(baseName,"FileName%d",i);
			sprintf(baseLODDist,"Distance%d",i);

			result = iniFile.readIdString(baseName,aseFileName,511);
			if (result == NO_ERR)
			{
				result = iniFile.readIdFloat(baseLODDist,lodDistance[i]);
				if (result != NO_ERR)
					STOP(("LOD %d has no distance value in file %s",i,fileName));
				// Push out LOD-swap thresholds so high-detail meshes stay visible
				// at greater zoom-out. See visual_preference_knobs.md.
				lodDistance[i] *= 5.0f;

				//----------------------------------------------
				// Base LOD shape.  In stand Pose by default.
				bldgShape[i] = new TG_TypeMultiShape;
				gosASSERT(bldgShape[i] != NULL);

				if (i == 0 && importSourceBase[0]) {
					loadBuildingImportSourceWithSidecarAxis(bldgShape[i], importSourceBase); // ASSIMP-BLDG-IMPORT-1: opt-in GLB probe
				} else {
					FullPathFileName bldgName;
					bldgName.init(tglPath,aseFileName,".ase");
					bldgShape[i]->LoadTGMultiShapeFromASE(bldgName);
				}

				if (!i)
					strncpy(bldgBaseName, aseFileName, sizeof(bldgBaseName) - 1);
			}
			else if (!i)
			{
				STOP(("No base LOD for shape %s",fileName));
			}
		}
	}
	else
	{
		//----------------------------------------------
		// Base shape.  In stand Pose by default.
		bldgShape[0] = new TG_TypeMultiShape;
		gosASSERT(bldgShape[0] != NULL);

		if (importSourceBase[0]) {
			loadBuildingImportSourceWithSidecarAxis(bldgShape[0], importSourceBase); // ASSIMP-BLDG-IMPORT-1: opt-in GLB probe
		} else {
			FullPathFileName bldgName;
			bldgName.init(tglPath,aseFileName,".ase");
			bldgShape[0]->LoadTGMultiShapeFromASE(bldgName);
		}

		strncpy(bldgBaseName, aseFileName, sizeof(bldgBaseName) - 1);
	}

	if (importSourceBase[0] && buildingPbrGateEnabled())
	{
		FullPathFileName sidecarName;
		sidecarName.init(tglPath, importSourceBase, ".mcasset.json");
		if (!loadBuildingPbrFromSidecar(this, sidecarName)) {
			fprintf(stderr, "[BUILDING_PBR] disabled for import '%s' sidecar=%s\n",
			        importSourceBase, (const char*)sidecarName);
			fflush(stderr);
		}
	}

	// MODEL-OVERRIDE dual-shape: render-only override for static props. The
	// stock bldgShape[] load above is UNCHANGED (collision authority). Here we
	// resolve a registry override for the BASE shape and, on hit, load the
	// replacement geometry into the separate render shape only. On any failure
	// we delete + NULL the render shape so render falls back to stock.
	bldgBaseName[sizeof(bldgBaseName) - 1] = 0;
	if (bldgBaseName[0])
	{
		// Env-gated discovery trace (matches MC2_ASSIMP_TRACE convention): logs every
		// static-prop appearance name seen, so a manifest can target props actually
		// present in a mission. No behavior change. MODEL-OVERRIDE Slice 3.
		if (getenv("MC2_MODOVERRIDE_TRACE"))
			fprintf(stderr, "[MODOVERRIDE_TRACE] staticProp '%s'\n", bldgBaseName);
		const ModelOverrideRecord* ov =
			ModelOverrideRegistry::instance().resolve("staticProp", bldgBaseName);
		if (ov)
		{
#ifdef ENABLE_ASSIMP_IMPORTER
			bldgRenderShape = new TG_TypeMultiShape;
			// Source path = manifest dir + record source (see Slice 1 registry).
			char overridePath[1024];
			snprintf(overridePath, sizeof(overridePath), "%s/%s",
			         ov->manifestDir.c_str(),
			         ov->sourceRelPath.c_str());
			// Guard the importer: Assimp may throw (DeadlyImportError). A throw
			// here would leak the freshly-new'd render shape and unwind into
			// non-exception-safe engine code, so collapse any throw to stock.
			try
			{
				// staticprop overrides cooked from stock .tgl carry the stock pivot already
				// -> no auto-ground (would shift them in depth). Trees keep autoGround=true.
				long r = ImportGeometryFromFile(overridePath, bldgRenderShape, /*autoGround=*/false);
				if (r != 0 || bldgRenderShape->GetNumShapes() == 0)
				{
					delete bldgRenderShape; bldgRenderShape = NULL;   // stock fallback
					fprintf(stderr, "[MODOVERRIDE] staticProp '%s': import failed (%s), using stock render\n",
					        bldgBaseName, overridePath);
					fflush(stderr);
				}
				else
				{
					// MODEL-OVERRIDE texture binding: resolve importer-assigned
					// texture names to GOS handles on the TYPE render shape.
					LoadOverrideRenderShapeTextures(bldgRenderShape);
					fprintf(stderr, "[MODOVERRIDE] staticProp '%s': render override applied (%s)\n",
					        bldgBaseName, overridePath);
					fflush(stderr);
					if (buildingPbrGateEnabled())
					{
						char sidecarPath[1024];
						snprintf(sidecarPath, sizeof(sidecarPath), "%s", overridePath);
						char* dot = strrchr(sidecarPath, '.');
						if (dot)
							strcpy(dot, ".mcasset.json");
						else
							strncat(sidecarPath, ".mcasset.json", sizeof(sidecarPath) - strlen(sidecarPath) - 1);
						if (!loadBuildingPbrFromSidecar(this, sidecarPath)) {
							fprintf(stderr, "[BUILDING_PBR] disabled for '%s' sidecar=%s\n",
							        bldgBaseName, sidecarPath);
							fflush(stderr);
						}
					}
					if (getenv("MC2_ANIMATED_PROP_PROBE"))
					{
						int ns = bldgRenderShape->GetNumShapes();
						fprintf(stderr, "[PROBE] '%s': %d node(s) loaded\n", bldgBaseName, ns);
						for (int _pi = 0; _pi < ns; ++_pi)
							fprintf(stderr, "[PROBE]   node[%d] = '%s'\n", _pi, bldgRenderShape->GetNodeId(_pi));
						if (rotationalNodeId[0] && S_stricmp(rotationalNodeId, "NONE") != 0)
						{
							// Type-level scan (no instance yet): checks if GLB has the expected node.
							bool found = false;
							for (int _qi = 0; _qi < ns; ++_qi)
							{
								if (S_stricmp(bldgRenderShape->GetNodeId(_qi), rotationalNodeId) == 0)
								{ found = true; break; }
							}
							fprintf(stderr, "[PROBE] '%s': AnimationNodeId='%s' %s in loaded GLB\n",
							        bldgBaseName, rotationalNodeId, found ? "FOUND" : "NOT FOUND");
						}
						fflush(stderr);
					}
				}
			}
			catch (...)
			{
				delete bldgRenderShape; bldgRenderShape = NULL;   // stock fallback
				fprintf(stderr, "[MODOVERRIDE] staticProp '%s': import threw (%s), using stock render\n",
				        bldgBaseName, overridePath);
				fflush(stderr);
			}
#else
			fprintf(stderr, "[MODOVERRIDE] staticProp '%s': override resolved but importer disabled, using stock render\n",
			        bldgBaseName);
			fflush(stderr);
#endif
		}
	}


	//destroyed state.
	result = iniFile.seekBlock("TGLDamage");
	if (result == NO_ERR)
	{
		result = iniFile.readIdString("FileName",aseFileName,511);
		if (result != NO_ERR)
			Fatal(result,"Could not find ASE FileName in building appearance INI file");
	
		FullPathFileName dmgName;
		dmgName.init(tglPath,aseFileName,".ase");
	
		bldgDmgShape = new TG_TypeMultiShape;
		gosASSERT(bldgDmgShape != NULL);
		bldgDmgShape->LoadTGMultiShapeFromASE(dmgName);

		if (!bldgDmgShape->GetNumShapes())
		{
			delete bldgDmgShape;
			bldgDmgShape = NULL;
		}
		else
		{
			// 2026-05-11 force-load damage-shape textures at appearType init.
			// LoadTGMultiShapeFromASE only sets texture NAMES, not handles —
			// the per-instance texture-load loop in setObjStatus only fires
			// when destruction happens at runtime. That's too late for
			// GpuStaticPropBatcher::finalizeGeometry, which builds its
			// per-packet texture array at mission-load. Without this loop,
			// damage-shape packets get layerForPacket=-1 and either render
			// with the wrong texture (orange-rectangle ghost) or get culled
			// from the multidraw, leaving destroyed buildings invisible.
			// Mirror the per-instance loop (bdactor.cpp:618-653) at the
			// type-level: load textures into mcTextureManager, set the
			// handles + alpha bits on the shared TG_TypeMultiShape so
			// every per-instance clone via CreateFrom inherits them.
			for (long i = 0; i < bldgDmgShape->GetNumTextures(); i++)
			{
				char txmName[1024];
				bldgDmgShape->GetTextureName(i, txmName, 256);
				char texturePath[1024];
				sprintf(texturePath, "%s%d" PATH_SEPARATOR, tglPath, ObjectTextureSize);
				FullPathFileName textureName;
				textureName.init(texturePath, txmName, "");
				if (textureOrKtxSidecarExists(textureName))
				{
					if (S_strnicmp(txmName, "a_", 2) == 0)
					{
						DWORD gosHandle = mcTextureManager->loadTexture(
							textureName, gos_Texture_Alpha,
							gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosHandle != 0xffffffff);
						bldgDmgShape->SetTextureHandle(i, gosHandle);
						bldgDmgShape->SetTextureAlpha(i, true);
					}
					else
					{
						DWORD gosHandle = mcTextureManager->loadTexture(
							textureName, gos_Texture_Solid,
							gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosHandle != 0xffffffff);
						bldgDmgShape->SetTextureHandle(i, gosHandle);
						bldgDmgShape->SetTextureAlpha(i, false);
					}
				}
				else
				{
					bldgDmgShape->SetTextureHandle(i, 0xffffffff);
				}
			}
		}
		
	}
	else
	{
		bldgDmgShape = NULL;
	}

	result = iniFile.seekBlock("TGLDestructEffect");
	if (result == NO_ERR)
	{
		result = iniFile.readIdString("FileName",destructEffect,59);
		if (result != NO_ERR)
			STOP(("Could not Find DestructEffectName in building appearance INI file"));
	
	}
	else
	{
		destructEffect[0] = 0;
	}

	//--------------------------------------------------------------------
	// Load Animation Information.
	// We can load up to 10 Animation States.
	for (int i=0;i<MAX_BD_ANIMATIONS;i++)
	{
		char blockId[512];
		sprintf(blockId,"Animation:%d",i);
		
		result = iniFile.seekBlock(blockId);
		if (result == NO_ERR)
		{
			char animName[512];
			result = iniFile.readIdString("AnimationName",animName,511);
			gosASSERT(result == NO_ERR);
			
			result = iniFile.readIdBoolean("LoopAnimation",bdAnimLoop[i]);
			gosASSERT(result == NO_ERR);
			
			result = iniFile.readIdBoolean("Reverse",bdReverse[i]);
			gosASSERT(result == NO_ERR);
			
			result = iniFile.readIdBoolean("Random",bdRandom[i]);
			gosASSERT(result == NO_ERR);
			
			result = iniFile.readIdLong("StartFrame",bdStartF[i]);
			if (result != NO_ERR)
				bdStartF[i] = 0;
				
 			//-------------------------------
			// We have an animation to load.
			FullPathFileName animPath;
			animPath.init(tglPath,animName,".ase");

			FullPathFileName otherPath;
			otherPath.init(tglPath,animName,".agl");

			if (fileExists(animPath) || fileExists(otherPath))
			{
				bdAnimData[i] = new TG_AnimateShape;
				gosASSERT(bdAnimData[i] != NULL);
	
				//--------------------------------------------------------
				// If this animation does not exist, it is not a problem!
				// Building will simply freeze until animation is "over"
				bdAnimData[i]->LoadTGMultiShapeAnimationFromASE(animPath,bldgShape[0]);
			}
			else
				bdAnimData[i] = NULL;
		}
		else
		{
			bdAnimData[i] = NULL;
		}
	}
	
	//--------------------------------------------------------------------
	// We can also load the node to pitch and yaw for spotlights/turrets.
	result = iniFile.seekBlock("AnimationNode");
	if (result == NO_ERR)
	{
		result = iniFile.readIdString("AnimationNodeId",rotationalNodeId,24);
		gosASSERT(result == NO_ERR);
	}
	else
	{
		strcpy(rotationalNodeId,"NONE");
	}
	
	if (nFrameRate != 0.0f)
	{
		for (long i=0;i<MAX_BD_ANIMATIONS;i++)
			setFrameRate(i,nFrameRate);
	}

	//-----------------------------------------------
	// Load up the Weapon Node Data.
	numWeaponNodes = 0;
	nodeData = NULL;
	result = iniFile.seekBlock("WeaponNode");
	if (result == NO_ERR)
	{
		nodeData = (NodeData *)AppearanceTypeList::appearanceHeap->Malloc(sizeof(NodeData)*(MAX_WEAPON_NODES));
		gosASSERT(nodeData != NULL);
		
		for (int i=0;i<MAX_WEAPON_NODES;i++)
		{
			char blockId[512];
			sprintf(blockId,"WeaponNodeId%d",i);
			
			char weaponName[512];
			result = iniFile.readIdString(blockId,weaponName,511);
			if (result != NO_ERR)
			{
				strcpy(weaponName,"NONE");
			}
			
			nodeData[i].nodeId = (char *)AppearanceTypeList::appearanceHeap->Malloc(strlen(weaponName)+1);
			gosASSERT(nodeData[i].nodeId != NULL);
				
			strcpy(nodeData[i].nodeId,weaponName);
			nodeData[i].weaponType = 0;
			numWeaponNodes++;
 		}
	}

	for (int i=0;i<MAX_LODS;i++)
	{
		if (bldgShape[i])
			bldgShape[i]->SetLightRGBs(hotPinkRGB, hotGreenRGB, hotYellowRGB);
	}
}

//----------------------------------------------------------------------------
void BldgAppearanceType::destroy (void)
{
	AppearanceType::destroy();

	for (long i=0;i<MAX_LODS;i++)
	{
		if (bldgShape[i])
		{
			delete bldgShape[i];
			bldgShape[i] = NULL;
		}
	}

	// MODEL-OVERRIDE dual-shape: free the render override shape if loaded.
	if (bldgRenderShape)
	{
		delete bldgRenderShape;
		bldgRenderShape = NULL;
	}
	if (buildingPbrMaterialSsbo)
	{
		GLuint ssbo = (GLuint)buildingPbrMaterialSsbo;
		glDeleteBuffers(1, &ssbo);
		buildingPbrMaterialSsbo = 0;
	}
	if (buildingPbrNormalTexture)
	{
		gos_DestroyTexture(buildingPbrNormalTexture);
		buildingPbrNormalTexture = 0;
	}
	if (buildingPbrOrmTexture)
	{
		gos_DestroyTexture(buildingPbrOrmTexture);
		buildingPbrOrmTexture = 0;
	}
	buildingPbrEnabled = false;

 	if (bldgDmgShape)
	{
		delete bldgDmgShape;
		bldgDmgShape = NULL;
	}
	
 	for (int i=0;i<MAX_BD_ANIMATIONS;i++)
	{
		if (bdAnimData[i])
		{
			delete bdAnimData[i];
			bdAnimData[i] = NULL;
		}
	}
}

//-----------------------------------------------------------------------------
void BldgAppearanceType::setAnimation (TG_MultiShapePtr shape, DWORD animationNum)
{
	gosASSERT(shape != NULL);
	gosASSERT(animationNum != 0xffffffff);
	gosASSERT(animationNum < MAX_BD_ANIMATIONS);

	if (bdAnimData[animationNum])
		bdAnimData[animationNum]->SetAnimationState(shape);
	else
		shape->ClearAnimation();
}

//-----------------------------------------------------------------------------
// class BldgAppearance
void BldgAppearance::setWeaponNodeUsed (long weaponNode)
{
	weaponNode -= appearType->numWeaponNodes;
   	if ((weaponNode >= 0) && (weaponNode < appearType->numWeaponNodes))
	{
		nodeUsed[weaponNode]++;
		nodeRecycle[weaponNode] = BASE_NODE_RECYCLE_TIME;
	}
}

//-----------------------------------------------------------------------------
Stuff::Vector3D BldgAppearance::getWeaponNodePosition (long nodeId)
{
	Stuff::Vector3D result = position;
	if ((nodeId < 0) || (nodeId >= appearType->numWeaponNodes))
		return result;
	
	//We already know we are using this node.  Do NOT increment recycle or nodeUsed!
		
   	//-------------------------------------------
   	// Create Matrix to conform to.
   	Stuff::UnitQuaternion qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = Stuff::EulerAngles(0.0f, yaw, 0.0f);
   
   	Stuff::Point3D xlatPosition;
   	xlatPosition.x = -position.x;
   	xlatPosition.y = TerrainRuntime::groundElevation(position);
   	xlatPosition.z = position.y;
   
   	Stuff::UnitQuaternion torsoRot;
   	torsoRot = Stuff::EulerAngles(0.0f,(turretYaw * DEGREES_TO_RADS),0.0f);
	if (rotationalNodeId == -1)
	{
		if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
			rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
		else
			rotationalNodeId = -2;
	}
   
	if (rotationalNodeId >= 0)
	   	bldgShape->SetNodeRotation(rotationalNodeId,&torsoRot);

	result = bldgShape->GetTransformedNodePosition(&xlatPosition,&qRotation,appearType->nodeData[nodeId].nodeId);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
 	return result;
}

//-----------------------------------------------------------------------------
Stuff::Vector3D BldgAppearance::getHitNode (void)
{
	if (hitNodeId == -1)
		hitNodeId = bldgShape->GetNodeNameId("hitnode");

	Stuff::Vector3D result = getNodeIdPosition(hitNodeId);
 	return result;
}

//-----------------------------------------------------------------------------
long BldgAppearance::getWeaponNode (long weaponType)
{
	//------------------------------------------------
	// Scan all weapon nodes and find least used one.
	long leastUsed = 999999999;
	long bestNode = -1;
	for (long i=0;i<appearType->numWeaponNodes;i++)
	{
		if (nodeUsed[i] < leastUsed)
		{
			leastUsed = nodeUsed[i];
			bestNode = i;
		}
	}
		
   	if ((bestNode < 0) || (bestNode >= appearType->numWeaponNodes))
   		return -1;

 	return bestNode;
}
		
//-----------------------------------------------------------------------------
float BldgAppearance::getWeaponNodeRecycle (long node)
{
	if ((node >= 0) && (node < appearType->numWeaponNodes))
		return nodeRecycle[node];
		
	return 0.0f;
}

//-----------------------------------------------------------------------------
void BldgAppearance::init (AppearanceTypePtr tree, GameObjectPtr obj)
{
	Appearance::init(tree,obj);
	appearType = (BldgAppearanceType *)tree;

	shapeMin.x = shapeMin.y = -25;
	shapeMax.x = shapeMax.y = 50;

    status = OBJECT_STATUS_NORMAL; // sebi: init so will not be garbage

	bdAnimationState =-1;
	currentFrame = 0.0f;
	bdFrameRate = 0.0f;
	isReversed = false;
	isLooping = false;
	setFirstFrame = false;
	canTransition = true;

	// Slice 2 (object-offload) substrate: never set true in Stage 2.A.
	needsFullBakeNextFrame = false;

	// Stage 3.D: zero-init static-registry state (mirror of TreeAppearance::init).
	staticReg = {};

	paintScheme = -1;
	objectNameId = 30469;
	hazeFactor = 0.0f;

	rotationalNodeId = -1;
	hitNodeId = activityNodeId = activityNode1Id = -1;

	currentFlash = duration = flashDuration = 0.0f;
	flashColor = 0x00000000;
	drawFlash = false;

	pointLight = NULL;
	lightId = 0xffffffff;
	forceLightsOut = false;

	// (E) T1.4: lazy-init key for SpotLight_ children. Vectors default-init
	// to empty; the first-night-visibility check in update() switches this
	// to true after the one-shot bldgShape walk. World position is invalid
	// (zero) at init() time per C-r1 C1, so registration is deferred.
	spotlightsRegistered_ = false;
	
	screenPos.x = screenPos.y = screenPos.z = screenPos.w = -999.0f;
	position.Zero();
	rotation = 0.0f;
	selected = 0;
	teamId = -1;
	homeTeamRelationship = 0;
	actualRotation = rotation;

	currentLOD = 0;
 	
	turretYaw = turretPitch = 0.0f;
	
	destructFX = NULL;
	activity = NULL;
	activity1 = NULL;
	isActivitying = false;

	OBBRadius = -1.0f;
	highZ = -1.0f;
	
	nodeUsed = NULL;
	nodeRecycle = NULL;
	
	beenInView = false;
	buildingPbrRenderActive = false;

	fogLightSet = false;
	if (appearType)
	{
		// MODEL-OVERRIDE dual-shape: build the per-instance RENDER shape from the
		// render accessor (override if present, else stock). Collision rebuilds
		// this same member from stock bldgShape[lod] when it needs passability.
		bldgShape = appearType->getBldgRenderShape(0)->CreateFrom();
		buildingPbrRenderActive = appearType->buildingPbrEnabled;

		//-------------------------------------------------
		// Load the texture and store its handle.
		for (int i=0;i<bldgShape->GetNumTextures();i++)
		{
			char txmName[1024];
			bldgShape->GetTextureName(i,txmName,256);

			char texturePath[1024];
			sprintf(texturePath,"%s%d" PATH_SEPARATOR,tglPath,ObjectTextureSize);
	
			FullPathFileName textureName;
			textureName.init(texturePath,txmName,"");
	
			if (textureOrKtxSidecarExists(textureName))
			{
				const bool forceOpaquePbrTexture = buildingPbrRenderActive &&
					!(appearType && appearType->buildingPbrPreserveLegacyAlpha);
				if (!forceOpaquePbrTexture && S_strnicmp(txmName,"a_",2) == 0)
				{
					DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
					gosASSERT(gosTextureHandle != 0xffffffff);
					bldgShape->SetTextureHandle(i,gosTextureHandle);
					bldgShape->SetTextureAlpha(i,true);
				}
				else
				{
					DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
					gosASSERT(gosTextureHandle != 0xffffffff);
					bldgShape->SetTextureHandle(i,gosTextureHandle);
					bldgShape->SetTextureAlpha(i,false);
				}
			}
			else
			{
				//PAUSE(("Warning: %s texture name not found",textureName));
				bldgShape->SetTextureHandle(i,0xffffffff);
			}
		}
		
		Stuff::Vector3D boxCoords[8];
		Stuff::Vector3D nodeCenter = bldgShape->GetRootNodeCenter();

		boxCoords[0].x = position.x + bldgShape->GetMinBox().x + nodeCenter.x;
		boxCoords[0].y = position.y + bldgShape->GetMinBox().z + nodeCenter.z;
		boxCoords[0].z = position.z + bldgShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[1].x = position.x + bldgShape->GetMinBox().x + nodeCenter.x;
		boxCoords[1].y = position.y + bldgShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[1].z = position.z + bldgShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[2].x = position.x + bldgShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[2].y = position.y + bldgShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[2].z = position.z + bldgShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[3].x = position.x + bldgShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[3].y = position.y + bldgShape->GetMinBox().z + nodeCenter.z;
		boxCoords[3].z = position.z + bldgShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[4].x = position.x + bldgShape->GetMinBox().x + nodeCenter.x;
		boxCoords[4].y = position.y + bldgShape->GetMinBox().z + nodeCenter.z;
		boxCoords[4].z = position.z + bldgShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[5].x = position.x + bldgShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[5].y = position.y + bldgShape->GetMinBox().z + nodeCenter.z;
		boxCoords[5].z = position.z + bldgShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[6].x = position.x + bldgShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[6].y = position.y + bldgShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[6].z = position.z + bldgShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[7].x = position.x + bldgShape->GetMinBox().x + nodeCenter.x;
		boxCoords[7].y = position.y + bldgShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[7].z = position.z + bldgShape->GetMinBox().y + nodeCenter.y;
		
 		float testRadius = 0.0;
		
		for (int i=0;i<8;i++)
		{
			testRadius = boxCoords[i].GetLength();
			if (OBBRadius < testRadius)
				OBBRadius = testRadius;

			if (boxCoords[i].z > highZ)
				highZ = boxCoords[i].z;
		}
		
		appearType->boundsUpperLeftX = (-OBBRadius * 2.0);
		appearType->boundsUpperLeftY = (-OBBRadius * 2.0);
		   					 
		appearType->boundsLowerRightX = (OBBRadius * 2.0);
		appearType->boundsLowerRightY = (OBBRadius);
		
		if (!appearType->getDesignerTypeBounds())
		{
			Stuff::Vector3D nodeCenter = bldgShape->GetRootNodeCenter();
			appearType->typeUpperLeft.Add(bldgShape->GetMinBox(),nodeCenter);
			appearType->typeLowerRight.Add(bldgShape->GetMaxBox(),nodeCenter);
		}
		
 		if (appearType->numWeaponNodes)
		{
			nodeUsed = (long *)AppearanceTypeList::appearanceHeap->Malloc(sizeof(long) * appearType->numWeaponNodes);
			gosASSERT(nodeUsed != NULL);
			memset(nodeUsed,0,sizeof(long) * appearType->numWeaponNodes);
			
			nodeRecycle = (float *)AppearanceTypeList::appearanceHeap->Malloc(sizeof(float) * appearType->numWeaponNodes);
			gosASSERT(nodeRecycle != NULL);
			
			for (long i=0;i<appearType->numWeaponNodes;i++)
				nodeRecycle[i] = 0.0f;
		}

		// Register this building's TG_TypeShape variants with the GPU static
		// prop batcher. Idempotent -- duplicate calls across instances are
		// cheap. Covers all LOD base shapes plus destroyed/damaged variants
		// and their shadow proxies. Registration happens after texture
		// handles are resolved so packets capture the correct GL handle.
		// MODEL-OVERRIDE: register the RENDER shape (override-or-stock) so the
		// batcher's s_typeIndex holds the override type-shapes that the
		// per-instance bldgShape (CreateFrom'd from getBldgRenderShape) submits
		// against. Registering the stock shape here would make submit() miss
		// (render type-shape not in s_typeIndex) -> CPU-fallback/cull, and the
		// override would never rasterize. Damage stays stock (out of MVP).
		const bool _bldgIsOverride = (appearType->bldgRenderShape != nullptr);
		for (int i = 0; i < MAX_LODS; ++i)
			GpuStaticPropBatcher::instance().registerMultiShape(appearType->getBldgRenderShape(i), _bldgIsOverride);
		GpuStaticPropBatcher::instance().registerMultiShape(appearType->bldgDmgShape);
	}
}

//-----------------------------------------------------------------------------
void BldgAppearance::setObjStatus (long oStatus)
{
	if (status != oStatus)
	{
		if ((oStatus == OBJECT_STATUS_DESTROYED) || (oStatus == OBJECT_STATUS_DISABLED))
		{
			if (appearType->bldgDmgShape)
			{
				if (bldgShape)
				{
					bldgShape->ClearAnimation();
					delete bldgShape;
					bldgShape = NULL;
				}
				
				bldgShape = appearType->bldgDmgShape->CreateFrom();
				buildingPbrRenderActive = false;
				if (bdAnimationState != -1)
					appearType->setAnimation(bldgShape,bdAnimationState);
				
				beenInView = false; 
				currentLOD = 0;
			}
			
			stopActivity();
		}
		
		if (oStatus == OBJECT_STATUS_NORMAL)
		{
			if (appearType->bldgShape[0])
			{
				if (bldgShape)
				{
					bldgShape->ClearAnimation();
					delete bldgShape;
					bldgShape = NULL;
				}

				// MODEL-OVERRIDE dual-shape: restore the per-instance RENDER
				// shape via the render accessor (override if present, else stock).
				bldgShape = appearType->getBldgRenderShape(0)->CreateFrom();
				buildingPbrRenderActive = appearType->buildingPbrEnabled;
				if (bdAnimationState != -1)
					appearType->setAnimation(bldgShape,bdAnimationState);

				beenInView = false;
			}

		}

		if (bldgShape)
		{
			//-------------------------------------------------
			// Load the texture and store its handle.
			for (long i=0;i<bldgShape->GetNumTextures();i++)
			{
				char txmName[1024];
				bldgShape->GetTextureName(i,txmName,256);
	
				char texturePath[1024];
				sprintf(texturePath,"%s%d" PATH_SEPARATOR,tglPath,ObjectTextureSize);
		
				FullPathFileName textureName;
				textureName.init(texturePath,txmName,"");
		
				if (textureOrKtxSidecarExists(textureName))
				{
					if (S_strnicmp(txmName,"a_",2) == 0)
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						bldgShape->SetTextureHandle(i,gosTextureHandle);
						bldgShape->SetTextureAlpha(i,true);
					}
					else
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						bldgShape->SetTextureHandle(i,gosTextureHandle);
						bldgShape->SetTextureAlpha(i,false);
					}
				}
				else
				{
					//PAUSE(("Warning: %s texture name not found",textureName));
					bldgShape->SetTextureHandle(i,0xffffffff);
				}
			}
		}

	}

	status = oStatus;
}

//-----------------------------------------------------------------------------
Stuff::Vector3D BldgAppearance::getNodeNamePosition (const char *nodeName)
{
	Stuff::Vector3D result = position;
	
   	//-------------------------------------------
   	// Create Matrix to conform to.
   	Stuff::UnitQuaternion qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = Stuff::EulerAngles(0.0f, yaw, 0.0f);
   
   	Stuff::Point3D xlatPosition;
   	xlatPosition.x = -position.x;
   	xlatPosition.y = position.z;
   	xlatPosition.z = position.y;
   
	result = bldgShape->GetTransformedNodePosition(&xlatPosition,&qRotation,nodeName);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
	return result;
}

//-----------------------------------------------------------------------------
Stuff::Vector3D BldgAppearance::getNodeIdPosition (long nodeId)
{
	Stuff::Vector3D result = position;
	
   	//-------------------------------------------
   	// Create Matrix to conform to.
   	Stuff::UnitQuaternion qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = Stuff::EulerAngles(0.0f, yaw, 0.0f);
   
   	Stuff::Point3D xlatPosition;
   	xlatPosition.x = -position.x;
   	xlatPosition.y = position.z;
   	xlatPosition.z = position.y;
   
	result = bldgShape->GetTransformedNodePosition(&xlatPosition,&qRotation,nodeId);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
	return result;
}

//-----------------------------------------------------------------------------
bool BldgAppearance::PerPolySelect (long mouseX, long mouseY)
{
	return bldgShape->PerPolySelect(mouseX,mouseY);
}

//-----------------------------------------------------------------------------
void BldgAppearance::setGesture (unsigned long gestureId)
{
	//------------------------------------------------------------
	// Check if state is possible.
	if (gestureId >= MAX_BD_ANIMATIONS)
		return;

	//------------------------------------------------------------
	// Check if object destroyed.  If so, no animation!
	if ((status == OBJECT_STATUS_DESTROYED) || (status == OBJECT_STATUS_DISABLED))
		return;
		
	if (gestureId == bdAnimationState)
		return;

	const bool naturalStaticExempt =
		appearType && staticRegIsNaturalBuildingName(appearType->name) && isStaticEligible();

	// BLDG-TYPE-ANIM-GATE-FIX-1: invalidate static registration on idle→animated.
	// When an idle building (bdAnimationState == -1) that is registered as a
	// static prop receives its first gesture, the GPU registry holds a stale
	// static-pose recipe. Invalidate before state change so the static draw is
	// removed before CPU animation begins. needsFullBakeNextFrame is NOT set —
	// invalidateStaticRegistration() clears staticReg, making IsStaticNow()
	// false, and the next update() runs the full bake naturally.
	// State-to-state (bdAnimationState != -1 → new state): already dynamic,
	// not registered, no action needed.
	if (bdAnimationState == -1 && staticReg.registered) {
		// Some natural props are authored as BUILDING appearances and receive a
		// dummy gesture=0 startup state even though there is no real animation
		// payload. Keep their static registration intact so pure render-static
		// trees/rocks remain skippable, while real animated/service buildings
		// still invalidate and wake normally.
		if (!naturalStaticExempt) {
			invalidateStaticRegistration();
			++s_animStartInvalidated;
		}
	}
	// Count all setGesture calls while already in a non-idle state.
	// Not an animation-end counter — bdAnimationState never returns to -1 after
	// the first setGesture() call: BldgAppearance has no idle-reset path, and
	// setGesture() itself guards gestureId >= MAX_BD_ANIMATIONS (blocking the
	// unsigned cast of -1). If a future refactor adds an idle-reset, this block
	// will need re-evaluation.
	if (bdAnimationState != -1) {
		++s_animStateToStateGesture;
	}

	//----------------------------------------------------------------------
	// If state is OK, set animation data, set first frame, set loop and
	// reverse flag, and start it going until you hear otherwise.
	appearType->setAnimation(bldgShape,gestureId);
	bdAnimationState = gestureId;
	currentFrame = 0.0f;
	if (appearType->bdStartF[gestureId])
		currentFrame = appearType->bdStartF[gestureId];
		
	isReversed = false;
	
	if (appearType->isReversed(bdAnimationState))
	{
		currentFrame = appearType->getNumFrames(bdAnimationState)-1;
		isReversed = true;
	}
	
	if (appearType->isRandom(bdAnimationState))
	{
		currentFrame = RandomNumber(appearType->getNumFrames(bdAnimationState)-1);
	}
	
	isLooping = appearType->isLooped(bdAnimationState);
	
	bdFrameRate = appearType->getFrameRate(bdAnimationState);
	
	setFirstFrame = true;
	if (bdFrameRate > Stuff::SMALL)
		canTransition = false;
	else
		canTransition = true;		//We can change immediately to another animation because we have no animation for this state!
}

//-----------------------------------------------------------------------------
void BldgAppearance::setMoverParameters (float turretRot, float lArmRot, float rArmRot, bool isAirborne)
{
	turretYaw = turretRot;
	turretPitch = lArmRot;
}

//-----------------------------------------------------------------------------
void BldgAppearance::setObjectParameters (const Stuff::Vector3D &pos, float Rot, long sel, long team, long homeRelations)
{
	rotation = Rot;

	position = pos;

	selected = sel;

	actualRotation = Rot;

	teamId = team;
	homeTeamRelationship = homeRelations;
}

//-----------------------------------------------------------------------------
bool BldgAppearance::isMouseOver (float px, float py)
{
	if (inView)
	{
		if ((px <= lowerRight.x) && (py <= lowerRight.y) &&
			(px >= upperLeft.x) &&
			(py >= upperLeft.y))
		{
			return inView;
		}
		else
		{
			return FALSE;
		}
	}
	
	return(inView);
}	

//-----------------------------------------------------------------------------
bool BldgAppearance::recalcBounds (void)
{
	// F3 CPU projection cost-baseline: aggregate per-actor scope into the
	// recalcBounds_perframe bucket. No-op when env OFF.
	::mc2_cpu_proj_cost::Scope _f3_recalcBounds_scope(
	    ::mc2_cpu_proj_cost::BUCKET_RECALCBOUNDS_PERFRAME);
	::mc2_cpu_proj_cost::add_workload_recalcbounds(1);
	// [TOBJSPLIT v1] accumulators declared in code/static_update_counters.h
	// (included above via ../code/static_update_counters.h).
	// Gate: file-scope s_tobjSplitBdOn (defined above, shared with TreeAppearance).

	setVisibilityGatesFromLegacy(false);

	if (eye)
	{
		//-------------------------------------------------------------------
		//NEW METHOD from the WAY BACK Days
		setVisibilityGatesFromLegacy(true);

		// [TOBJSPLIT v1] ANGULAR bracket: matrix-free sphere angular clip.
		// Reads cycle counter immediately before/after.
		{
		unsigned long long _tsA = s_tobjSplitBdOn ? __rdtsc() : 0ULL;
		if (eye->usePerspective)
		{
			Stuff::Vector3D cameraPos;
			cameraPos.x = -eye->getCameraOrigin().x;
			cameraPos.y = eye->getCameraOrigin().z;
			cameraPos.z = eye->getCameraOrigin().y;
			float vClipConstant = eye->verticalSphereClipConstant;
			float hClipConstant = eye->horizontalSphereClipConstant;

			Stuff::Vector3D objectCenter;
			objectCenter.Subtract(position,cameraPos);
			Camera::cameraFrame.trans_to_frame(objectCenter);
			float distanceToEye = objectCenter.GetApproximateLength();
			float clip_distance = fabs(1.0f / objectCenter.y);

			//Is vertex on Screen OR close enough to screen that its triangle MAY be visible?
			// WE have removed the atans here by simply taking the tan of the angle we want above.
			float object_angle = fabs(objectCenter.z) * clip_distance;
			float extent_angle = bldgShape->GetExtentRadius() / distanceToEye;
			if (object_angle > (vClipConstant + extent_angle))
			{
				//In theory, we would return here.  Object is NOT on screen.
				setVisibilityGatesFromLegacy(false);
			}
			else
			{
				object_angle = fabs(objectCenter.x) * clip_distance;
				if (object_angle > (hClipConstant + extent_angle))
				{
					//In theory, we would return here.  Object is NOT on screen.
					setVisibilityGatesFromLegacy(false);
				}
			}
		}
		if (s_tobjSplitBdOn) g_tobjAngularCyc.fetch_add(__rdtsc() - _tsA, std::memory_order_relaxed);
		}  // end ANGULAR bracket

		// recalcBounds projection body deleted 2026-05-18 (Task 2): the GPU
		// compute cull (gpu_cull::readback_isActorVisibleLagged) is the
		// substitutive twin of the per-frame screen projection. inView is now
		// coarse-angular-only -- a strict superset of the old projected value;
		// over-inclusion is correctness-safe (cull_gates_are_load_bearing.md).
		// screenPos/upperLeft/lowerRight are computed lazily at pick time
		// (objmgr.cpp findTerrainObjectByMouse, Task 4).
		// LATENT HAZARD: the deleted block also held the per-LOD-swap texture
		// (re)loader, dead today under the 2026-05-12 TEMP LOD-0 pin
		// (selectLOD forced 0 / (void)useHighObjectDetail in this function).
		// If that pin is reverted (when the LOD-1 invisibility root cause is
		// fixed), LOD selection + the per-LOD texture loader MUST be re-homed
		// BEFORE the revert lands -- BldgAppearance::init loads LOD-0 textures
		// ONLY; LOD-1+ would be unloaded after any LOD swap.
	}


	return(inView);
}

//-----------------------------------------------------------------------------
void BldgAppearance::recalcBoundsAndStamp() {
	// FRAME-JOBS-1 worker path. Do not call from game logic.
	extern uint32_t g_mc2FrameCounter;
	if (boundsFrame == g_mc2FrameCounter) return;
	recalcBounds();
	boundsFrame = g_mc2FrameCounter;
}

//-----------------------------------------------------------------------------
bool BldgAppearance::playDestruction (void)
{
	//Check if there is a Destruct FX
	if (appearType->destructEffect[0])
	{
		//--------------------------------------------
		// Yes, load it on up.
		unsigned flags = gosFX::Effect::ExecuteFlag;

		Check_Object(gosFX::EffectLibrary::Instance);
		gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(appearType->destructEffect);
		
		if (gosEffectSpec)
		{
			destructFX = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
			gosASSERT(destructFX != NULL);
		
			MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
		
			Stuff::Point3D			tPosition;
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			
            //Stuff::Vector3D offsetPosition;
			//offsetPosition.x = Terrain::worldUnitsPerVertex / 3.0f;
			//offsetPosition.y = -(Terrain::worldUnitsPerVertex / 3.0f);
			//offsetPosition.z = 0.0f;

			//OppRotate(offsetPosition,rotation);

			Stuff::Vector3D actualPosition = position;
			//actualPosition.Add(position,offsetPosition);

			tPosition.x = -actualPosition.x;
			tPosition.y = actualPosition.z;
			tPosition.z = actualPosition.y;

			float yaw = (180.0f + rotation) * DEGREES_TO_RADS;
			Stuff::UnitQuaternion rot;
			rot = Stuff::EulerAngles(0.0f, yaw, 0.0f);

			shapeOrigin.BuildRotation(rot);
			shapeOrigin.BuildTranslation(tPosition);
			
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);
			destructFX->Start(&info);
			
			return true;
		}
		
		return false;
	}
	
	return false;		//We didn't have a destruct effect.  Tell the object to play its default.
}

//-----------------------------------------------------------------------------
long BldgAppearance::render (long depthFixup)
{
	// GPU-batcher path bypasses inView here — the whole point of C2 is
	// letting the GPU clipper decide visibility. The legacy angular-cull
	// recalcBounds has a ~87% false-negative rate at wolfman zoom; under
	// the GPU path we render every actor and trust the GPU.
	// PREVIEW-FIX: the SimpleCamera component/weapon preview (MechLab loadout)
	// owns visibility via the UI; force the render gate on in that context.
	if (inView || g_useGpuStaticProps || g_mechPreviewRenderDepth > 0)
	{
		uint32_t color = SD_BLUE;
		uint32_t highLight = 0x007f7f7f;
		if ((teamId > -1) && (teamId < 8)) {
			static unsigned long highLightTable[3] = {0x00007f00, 0x0000007f, 0x007f0000};
			static uint32_t colorTable[3] = {SB_GREEN | 0xff000000, SB_BLUE | 0xff000000, SB_RED| 0xff000000};
			color = colorTable[homeTeamRelationship];
			highLight = highLightTable[homeTeamRelationship];
		}

		if (selected & DRAW_COLORED)
		{
			bldgShape->SetARGBHighLight(highLight);
		}
		else
		{
			bldgShape->SetARGBHighLight(highlightColor);
		}
		
		if (drawFlash)
		{
			bldgShape->SetARGBHighLight(flashColor);
		}
		
		//---------------------------------------------
		// Call Multi-shape render stuff here.
		// Slice 1 path (g_useGpuObjects). No cull bypass; submitMultiShape
		// is per-child Layer-B by construction. Returns false only when
		// EVERY child is ineligible.
		//
		// Caller-side accounting: recordEligibleActor() fires unconditionally
		// when slice 1 reaches this site (so a null shape or skipped submit
		// still counts toward eligible_actors). recordCpuFallback() fires
		// when no submit succeeded.
		bool submittedToGpu = false;
		// PREVIEW-FIX: in the SimpleCamera component/weapon preview, skip the GPU
		// static-prop submit (the batcher flushes with the world snapshot/terrain
		// MVP, not the UI camera) so submittedToGpu stays false and the legacy CPU
		// bldgShape->Render() below runs, honoring this SimpleCamera.
		const bool buildingPbrOverrideActive = appearType && buildingPbrRenderActive;
		if (g_useGpuObjects && g_mechPreviewRenderDepth == 0 && !buildingPbrOverrideActive)
		{
			GpuStaticPropBatcher::instance().recordEligibleActor(
				GpuStaticPropPopulation::Building);

			// Stage 3.D: static registry fast path (mirror of TreeAppearance
			// at bdactor.cpp:4123). Set MC2_FORCE_DYNAMIC_BUILDINGS=1 to force
			// fallback to dynamic submitMultiShape for boundary diagnosis.
			// 2026-05-10 diag: per-frame counters to localise buildings-don't-
			// render bug (substrate=ON misses buildings; killswitch shows them).
			static uint64_t s_diag_render_calls = 0;
			static uint64_t s_diag_static_now_true = 0;
			static uint64_t s_diag_lightidx_uintmax = 0;
			static uint64_t s_diag_markVisible = 0;
			static uint64_t s_diag_static_now_false_reg = 0;
			static uint64_t s_diag_static_now_false_eligible = 0;
			static uint64_t s_diag_static_now_false_other = 0;
			static uint64_t s_diag_dyn_submit = 0;
			++s_diag_render_calls;
			const bool isnow = IsStaticNow();
			// [SEAMPROBE] stage 10: per-render admission gate for the override
			// building (hangar). Logs IsStaticNow() components so we can see why
			// markVisible() never fires for the prop while it fires for trees.
			{
				static const bool s_seamRender = (getenv("MC2_MODOVERRIDE_TRACE") != nullptr);
				static int s_seamRenderLogged = 0;
				if (s_seamRender && appearType && appearType->bldgRenderShape
				        && s_seamRenderLogged < 12) {
					++s_seamRenderLogged;
					fprintf(stderr,
						"[SEAMPROBE] bldg render name=%s isnow=%d reg=%d shapeMatch=%d "
						"needBake=%d elig=%d recipeIdx=%d | spin=%d "
						"drawFlash=%d destructFX=%d activity=%d activity1=%d animState=%d\n",
						appearType->name, (int)isnow, (int)staticReg.registered,
						(int)(staticReg.shape == bldgShape), (int)needsFullBakeNextFrame,
						(int)isStaticEligible(), staticReg.recipeIndex,
						(int)(appearType?appearType->spinMe:-1),
						(int)(drawFlash!=0), (int)(destructFX!=NULL),
						(int)(activity!=0), (int)(activity1!=0),
						(int)bdAnimationState);
					fflush(stderr);
				}
			}
			if (isnow) ++s_diag_static_now_true;
			else {
				if (!staticReg.registered) ++s_diag_static_now_false_reg;
				else if (!isStaticEligible()) ++s_diag_static_now_false_eligible;
				else ++s_diag_static_now_false_other;
			}
			if (isnow) {
				static const bool s_forceDynamicBldgs =
				    bdForceDynamicDefault("MC2_FORCE_DYNAMIC_BUILDINGS");
				if (s_forceDynamicBldgs) {
					invalidateStaticRegistration();
					// Fall through to the dynamic path below.
				} else if (bldgShape && bldgShape->getCachedGpuLightIndex() == UINT32_MAX) {
					// Light gather failed this frame — invalidate so dynamic
					// path re-runs and re-registers next frame.
					++s_diag_lightidx_uintmax;
					invalidateStaticRegistration();
				} else {
					// 2026-05-11: pass per-actor captured lightDataIndex so
					// flush() can read it (when MC2_STATIC_PER_INSTANCE_LIGHT=1)
					// instead of multi->getCachedGpuLightIndex() — the per-multi
					// scratch slot is last-writer-wins across sibling instances.
					// STATIC-PROP REGISTRATION CONTRACT v1 (see TreeAppearance::render):
					// only suppress legacy when the registry accepted a LIVE recipe; on a
					// dead/tombstoned handle, invalidate so the full-bake path re-registers
					// + draws this frame instead of the prop vanishing.
					const GpuStaticPropRegistry::StaticSubmitResult bldgRes =
						GpuStaticPropRegistry::markVisibleChecked(
							staticReg.recipeIndex,
							staticReg.lightDataIndex,
							bldgShape ? bldgShape->GetExtentRadius() : 0.0f);
					if (bldgRes == GpuStaticPropRegistry::StaticSubmitResult::Submitted) {
						++s_diag_markVisible;
						submittedToGpu = true;
					} else {
						bdLogStaticRegInvalid("bldg", (int)bldgRes, staticReg.recipeIndex,
							appearType ? appearType->name : nullptr);
						invalidateStaticRegistration();  // dead handle -> full-bake re-registers below
					}
				}
			}
			static const bool s_bldgDiagTrace = (getenv("MC2_BLDG_DIAG_TRACE") != nullptr);
			if (s_bldgDiagTrace && (s_diag_render_calls % 600) == 0) {
				fprintf(stderr,
					"[BLDG_DIAG v1] event=summary calls=%llu staticNow=%llu "
					"notreg=%llu notelig=%llu other=%llu lightidxUM=%llu markVis=%llu dynSubmit=%llu\n",
					(unsigned long long)s_diag_render_calls,
					(unsigned long long)s_diag_static_now_true,
					(unsigned long long)s_diag_static_now_false_reg,
					(unsigned long long)s_diag_static_now_false_eligible,
					(unsigned long long)s_diag_static_now_false_other,
					(unsigned long long)s_diag_lightidx_uintmax,
					(unsigned long long)s_diag_markVisible,
					(unsigned long long)s_diag_dyn_submit);
				fflush(stderr);
			}
			(void)s_diag_dyn_submit;  // updated below if we go to the dyn path

			if (!submittedToGpu && bldgShape)
			{
				// Stage 3.D: shape-swap invalidation. IsStaticNow's
				// staticReg.shape==bldgShape check routed us here when
				// bldgShape was reassigned (LOD swap, damage→bldgDmgShape),
				// but staticReg.registered=true still blocks the registration
				// block below. Invalidate the stale entry first.
				// PERF DIAGNOSTIC 2026-05-07: see TreeAppearance::render for the
				// per-frame churn analysis. Buildings have damage-state swaps
				// (intact → dmg shape) but typically no LOD swap; this rate
				// should be much lower than the tree counterpart.
				if (staticReg.registered && staticReg.shape != bldgShape) {
					invalidateStaticRegistration();
				}

				// Slice 2 (object-offload) — Stage 2.C+: pass appearType->name
				// as callerName so [OBJBATCHER v1] event=late_register can
				// identify which actor class owns the unregistered type.
				const char* callerName = (appearType ? appearType->name : nullptr);
				submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
					bldgShape, GpuStaticPropPopulation::Building, callerName);
				if (submittedToGpu) ++s_diag_dyn_submit;
				// Slice 2 (object-offload) — Stage 2.B: late-registration
				// recovery flag. When submitMultiShape failed because a leaf
				// type was unregistered, mark the actor for full-bake on the
				// NEXT update — defensive hygiene that ensures positions-only
				// is never run on this actor before its type registers.
				if (!submittedToGpu &&
				    GpuStaticPropBatcher::instance().wasLastFailureLateRegistration())
				{
					needsFullBakeNextFrame = true;
					invalidateStaticRegistration();  // clear any stale registration
				}

				// Stage 3.D: registration block. On the first successful full-bake
				// submission with no late-reg flag AND with this instance currently
				// static-eligible, snapshot the leaf batch into the registry.
				// Subsequent frames use the static path above.
				if (submittedToGpu && !staticReg.registered
				        && GpuStaticPropRegistry::isEnabled()
				        && !needsFullBakeNextFrame
				        && isStaticEligible()) {
					const auto& batch =
						GpuStaticPropBatcher::instance().getLastBuiltBatch();
					// M1 RenderWorld route (Slice M1 Task 8). Adapter performs
					// sentinel translation; staticReg.recipeIndex remains int32_t
					// per plan Decision D4 (slot-side storage stays legacy in M1).
					int32_t legacyIdx = -1;
					(void)GameAdapters::StaticProp::syncStaticProp(
						bldgShape, batch.data(), batch.size(), &legacyIdx);
					staticReg.recipeIndex = legacyIdx;
					staticReg.registered  = (staticReg.recipeIndex >= 0);
					staticReg.shape       = bldgShape;
					if (staticReg.registered) {
						// SHADOW-STATIC-BUILDINGS-2: tag re-registered building recipe.
						GpuStaticPropRegistry::setRecipePopulation(
							staticReg.recipeIndex, GpuStaticPropPopulation::Building);
						// H4 follow-up (2026-05-07): per-frame re-registration
						// after damage/shape swap has the same lightData_ gap as
						// mission-load registerStatic(). Force one full update()
						// so touch() cannot resubmit default-zero lightData_.
						// Spec: docs/superpowers/specs/2026-05-07-lod-swap-static-registry-churn.md
						needsFullBakeNextFrame = true;
					}
				}
			}
			if (!submittedToGpu)
			{
				GpuStaticPropBatcher::instance().recordCpuFallback(
					GpuStaticPropPopulation::Building);
			}
		}
		// Legacy bypass-cull path (g_useGpuStaticProps). Mutually exclusive
		// with slice 1 — gated on !g_useGpuObjects so the two paths cannot
		// coexist. Tagged Legacy so Gate F's fallback-rate is computed only
		// over slice-1 populations. See spec R1.
		if (!submittedToGpu && !g_useGpuObjects && g_useGpuStaticProps && bldgShape && !buildingPbrOverrideActive)
		{
			submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
				bldgShape, GpuStaticPropPopulation::Legacy);
		}
		if (!submittedToGpu)
		{
			if (buildingPbrOverrideActive)
				submitBuildingFootprintShadow(this);
			if (buildingPbrOverrideActive) {
				TG_SetRenderShapePbrOverride(appearType->buildingPbrProgram,
					appearType->buildingPbrNormalTexture,
					appearType->buildingPbrOrmTexture,
					appearType->buildingPbrMaterialSsbo,
					appearType->buildingPbrTileScale,
					appearType->buildingPbrRoughnessBias,
					appearType->buildingPbrMetallicInfluence);
			}
			if (appearType->spinMe)
				bldgShape->Render(false,0.00001f);
			else if (!depthFixup)
				bldgShape->Render();
			else if (depthFixup > 0)
				bldgShape->Render(false,0.9999999f);
			else if (depthFixup < 0)
				bldgShape->Render(false,0.00001f);
			if (buildingPbrOverrideActive)
				TG_ClearRenderShapePbrOverride();
		}

		// LODBUG probe — post-swap submit observation.  When the actor's
		// bldgShape pointer changed since the last render() call for this
		// actor, log the submit outcome.  Almost all shape-pointer changes
		// are LOD swaps (recalcBounds:1437/1450 reassigns bldgShape via
		// CreateFrom); damage swaps would also trigger but are rare during
		// 30s passive smoke.  Off by default — env-gated.  Tracks state via
		// a static unordered_map keyed on the actor pointer so we don't
		// touch the BldgAppearance class layout.
		{
			static const bool s_lodBugTrace =
				(getenv("MC2_LODBUG_TRACE") != nullptr);
			if (s_lodBugTrace) {
				static std::unordered_map<BldgAppearance*, TG_MultiShape*>
					s_prevShape;
				auto it = s_prevShape.find(this);
				TG_MultiShape* prev =
					(it != s_prevShape.end()) ? it->second : nullptr;
				if (prev && prev != bldgShape) {
					printf("[LODBUG v1] event=post_swap_render actor=%p "
					       "prevShape=%p newShape=%p currentLOD=%u "
					       "inView=%d submittedToGpu=%d\n",
					       (void*)this, (void*)prev, (void*)bldgShape,
					       (unsigned)currentLOD, (int)inView,
					       (int)submittedToGpu);
					fflush(stdout);
				}
				s_prevShape[this] = bldgShape;
			}
		}

		if (selected & DRAW_BARS)
		{
			if (!appearType->spinMe)
			{
				drawBars();

				//drawSelectBrackets(color);
			}
		}

		if ( selected & DRAW_TEXT )
		{
			if (objectNameId != -1)
			{
				char tmpString[255];
				cLoadString(objectNameId, tmpString, 254);

				drawTextHelp(tmpString, color);
			}
		}
		
		//------------------------------------------
		// Render GOS FX if necessary
		if (destructFX && destructFX->IsExecuted())
		{
			gosFX::Effect::DrawInfo drawInfo;
			drawInfo.m_clipper = theClipper;
			
			MidLevelRenderer::MLRState mlrState;
			mlrState.SetDitherOn();
			mlrState.SetTextureCorrectionOn();
			mlrState.SetZBufferCompareOn();
			mlrState.SetZBufferWriteOn();
	
			drawInfo.m_state = mlrState;
			drawInfo.m_clippingFlags = 0x0;

 			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			Stuff::Point3D			tPosition;
			
			//Stuff::Vector3D offsetPosition;
			//offsetPosition.x = Terrain::worldUnitsPerVertex / 3.0f;
			//offsetPosition.y = -(Terrain::worldUnitsPerVertex / 3.0f);
			//offsetPosition.z = 0.0f;

			//OppRotate(offsetPosition,rotation);

			Stuff::Vector3D actualPosition = position;
			//actualPosition.Add(position,offsetPosition);

			tPosition.x = -actualPosition.x;
			tPosition.y = actualPosition.z;
			tPosition.z = actualPosition.y;

			float yaw = (180.0f + rotation) * DEGREES_TO_RADS;
			Stuff::UnitQuaternion rot;
			rot = Stuff::EulerAngles(0.0f, yaw, 0.0f);
 			shapeOrigin.BuildRotation(rot);
			shapeOrigin.BuildTranslation(tPosition);
			
			drawInfo.m_parentToWorld = &shapeOrigin;
			
			if (!MLRVertexLimitReached)
				destructFX->Draw(&drawInfo);
		}
		
		if (isActivitying)
		{
			gosFX::Effect::DrawInfo drawInfo;
			drawInfo.m_clipper = theClipper;

			MidLevelRenderer::MLRState mlrState;
			mlrState.SetDitherOn();
			mlrState.SetTextureCorrectionOn();
			mlrState.SetZBufferCompareOn();
			mlrState.SetZBufferWriteOn();
	
			drawInfo.m_state = mlrState;
			drawInfo.m_clippingFlags = 0x0;

			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			Stuff::LinearMatrix4D	localResult;

			if (activityNodeId == -1)
				activityNodeId = bldgShape->GetNodeNameId("activity_node");
			Stuff::Vector3D dustPos = getNodeIdPosition(activityNodeId);

			if (rotationalNodeId == -1)
			{
				if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
	   				rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
				else
					rotationalNodeId = -2;
			}

			if (rotationalNodeId >= 0)
				dustPos = getNodeIdPosition(rotationalNodeId);
				
			Stuff::Point3D wakePos;
			wakePos.x = -dustPos.x;
			wakePos.y = dustPos.z;
			wakePos.z = dustPos.y;
			
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(wakePos);
					
			/*
			Stuff::UnitQuaternion effectRot;
			effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
			localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
			localResult.Multiply(localToWorld,shapeOrigin);
			*/

			drawInfo.m_parentToWorld = &shapeOrigin;
			if (!MLRVertexLimitReached && activity)
				activity->Draw(&drawInfo);
			
			if (activity1)
			{
				if (activityNodeId == -1)
					activityNodeId = bldgShape->GetNodeNameId("activity_node");
				Stuff::Vector3D dustPos = getNodeIdPosition(activityNodeId);
	
				if (rotationalNodeId == -1)
				{
					if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
		   				rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
					else
						rotationalNodeId = -2;
				}
	
				if (rotationalNodeId >= 0)
					dustPos = getNodeIdPosition(rotationalNodeId);
					
				Stuff::Point3D wakePos;
				wakePos.x = -dustPos.x;
				wakePos.y = dustPos.z;
				wakePos.z = dustPos.y;
				
				shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
				shapeOrigin.BuildTranslation(wakePos);
						
				/*
				Stuff::UnitQuaternion effectRot;
				effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
				localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
				localResult.Multiply(localToWorld,shapeOrigin);
				*/
	
				drawInfo.m_parentToWorld = &shapeOrigin;
				if (!MLRVertexLimitReached)
					activity1->Draw(&drawInfo);
			}
		}

 			   
//		selected = FALSE;
//#define DRAW_BOX
#ifdef DRAW_BOX
		//---------------------------------------------------------
		// Render the Bounding Box to see if it is OK.
		Stuff::Vector3D nodeCenter = bldgShape->GetRootNodeCenter();
		Stuff::Vector3D boxStart;
		Stuff::Vector3D boxEnd;
		boxStart.x = -(bldgShape->GetMinBox().x + nodeCenter.x);
		boxStart.z = bldgShape->GetMinBox().y + nodeCenter.y;
		boxStart.y = bldgShape->GetMinBox().z + nodeCenter.z;
		
		boxEnd.x = -(bldgShape->GetMaxBox().x + nodeCenter.x);
		boxEnd.z = bldgShape->GetMaxBox().y + nodeCenter.y;
		boxEnd.y = bldgShape->GetMaxBox().z + nodeCenter.z;
		
 		Stuff::Vector3D boxCoords[8];
		Stuff::Vector3D addCoords;
		
		addCoords.x = boxStart.x;
		addCoords.y = boxStart.y;
		addCoords.z = boxEnd.z;
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[0].Add(position,addCoords);

		addCoords.x = boxStart.x;
		addCoords.y = boxEnd.y;  
		addCoords.z = boxEnd.z;  		
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[1].Add(position,addCoords);

 		addCoords.x = boxEnd.x; 
		addCoords.y = boxEnd.y; 
		addCoords.z = boxEnd.z; 		
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[2].Add(position,addCoords);
		
 		addCoords.x = boxEnd.x;   
		addCoords.y = boxStart.y; 
		addCoords.z = boxEnd.z;   		
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[3].Add(position,addCoords);
		
 		addCoords.x = boxStart.x;
		addCoords.y = boxStart.y; 
		addCoords.z = boxStart.z; 		
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[4].Add(position,addCoords);
 					  
 		addCoords.x = boxEnd.x;   
		addCoords.y = boxStart.y;   
		addCoords.z = boxStart.z; 
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[5].Add(position,addCoords);
		
 		addCoords.x = boxEnd.x;   
		addCoords.y = boxEnd.y;   
		addCoords.z = boxStart.z; 
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[6].Add(position,addCoords);
		
 		addCoords.x = boxStart.x; 
		addCoords.y = boxEnd.y;   
		addCoords.z = boxStart.z; 
		if (rotation != 0.0f)
			Rotate(addCoords,-rotation);
 		
		boxCoords[7].Add(position,addCoords);

		Stuff::Vector4D screenPos[8];
		for (long i=0;i<8;i++)
		{
			// [PROJECTZ:ScreenXYOracle id=bdactor_box_wire_a]
			eye->projectForScreenXY(boxCoords[i],screenPos[i]);
		}

		{
			LineElement newElement(screenPos[0],screenPos[1],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[0],screenPos[4],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[0],screenPos[3],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[5],screenPos[4],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[5],screenPos[6],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[5],screenPos[3],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[2],screenPos[3],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[2],screenPos[6],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[2],screenPos[1],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[7],screenPos[1],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[7],screenPos[6],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[7],screenPos[4],XP_WHITE,NULL,-1);
			newElement.draw();
		}
#endif
#undef DRAW_BOX
	}
	return NO_ERR;
}

//-----------------------------------------------------------------------------
long BldgAppearance::renderShadows (void)
{
	return NO_ERR;
}

//-----------------------------------------------------------------------------
// [LIGHTBAKE-PROOF v1] A/B parity trace (defined in mclib/txmmgr.cpp). File-scope
// declaration — never declared inside a function body. Proves the baked permanent
// slot == the gathered transient record byte/hash.
extern void mc2LightBakeParityCheck(int32_t recipeIndex, const TG_HWLightsData* gatheredLeaf,
                                    float wx, float wy, float wz, const char* appearance);

// [LIGHTBAKE v1] Static-actor lighting mission-load bake gate. Replaces
// the raw shape->CacheGpuLightData() at the 4 static (bldg/tree) call
// sites. The trailing staticReg.lightDataIndex =
// shape->getCachedGpuLightIndex() per-instance capture is UNCHANGED
// (both CacheGpuLightData and EmitBakedGpuLightData set
// cachedGpuLightIndex_). Key = monotonic-never-reused registry
// recipeIndex; invalidate (destruction/LOD swap) routes through
// invalidateStaticRegistration -> GpuStaticPropRegistry::invalidate ->
// mc2EraseBakedStaticLight -> lazy re-bake of the same position-derived
// constant. Kill-switch MC2_LIGHTBAKE (=0 -> unchanged D2 path
// bit-for-bit). Mechs never reach this (mech3d.cpp calls
// CacheGpuLightData directly); generic props take the no-actor-light
// path. C++-only. See docs/superpowers/plans/
// 2026-05-17-static-lighting-bake-SIMPLIFIED.md
// ============================================================================
// [LIGHTSLOT v1] Task 0 — light-slot cardinality PROOF GATE (diagnostic only,
// env-gated MC2_LIGHTSLOT_TRACE, ZERO behavior change). Proves whether the
// unique static-prop light slots consumed by an override forest are bounded by
// ~(types x LODs) after addLightDataStructure content dedup + the persistent
// baked table, or scale with INSTANCES (K). Pure counting; emits ONE line per
// map at mission_ready (NOT per-frame). All txmmgr numbers read via accessor
// free fns (defined mclib/txmmgr.cpp).
// ============================================================================
extern uint32_t g_mc2FrameCounter;  // mclib/tgl.cpp
// Env bool parser: unset returns `def`; "0"/"false"/"off"/"no" disable;
// everything else enables. Keep local to this TU so the diagnostics below
// can stay self-contained.
static bool ParseEnvBool(const char* name, bool def = false) {
    const char* v = getenv(name);
    if (!v || !*v) return def;
    if (v[0]=='0' && !v[1]) return false;
    if (!_stricmp(v, "false") || !_stricmp(v, "off") || !_stricmp(v, "no")) return false;
    return true;
}

static bool mc2StableLightSkipEnabled() {
    static const bool s_enabled = ParseEnvBool("MC2_STABLE_LIGHT_SKIP");
    return s_enabled;
}

static bool mc2StableLightSkipDiagEnabled() {
    static const bool s_enabled = ParseEnvBool("MC2_STABLE_LIGHT_SKIP_DIAG");
    return s_enabled;
}

static uint64_t mc2StaticLightEnvironmentGeneration(const Camera* eye) {
    (void)eye;
    // L1b: intentionally coarse. Global mission lighting is effectively stable.
    // This stays default-off and is only used for bounded diagnostics here.
    return 0;
}

namespace {
struct StableLightSkipDiag {
    bool enabled = false;
    bool envPrinted = false;
    bool initialized = false;
    uint32_t frame = 0xFFFFFFFFu;
    uint64_t armed = 0;
    uint64_t eligible = 0;
    uint64_t skip = 0;
    uint64_t emit = 0;
    uint64_t miss_notRegistered = 0;
    uint64_t miss_noValidLight = 0;
    uint64_t miss_envGen = 0;
    uint64_t miss_needsFullBake = 0;
    uint64_t miss_invalidLightIdx = 0;
};

static StableLightSkipDiag s_stableLightSkipDiag;

static void flushStableLightSkipDiag(uint32_t completedFrame)
{
    if (!s_stableLightSkipDiag.enabled)
        return;
    if (!(completedFrame < 5 || (completedFrame % 60u) == 0u))
        return;

    std::fprintf(stderr,
        "[STABLE_LIGHT_SKIP] armed=%llu eligible=%llu skip=%llu emit=%llu "
        "miss_notRegistered=%llu miss_noValidLight=%llu miss_envGen=%llu "
        "miss_needsFullBake=%llu miss_invalidLightIdx=%llu\n",
        (unsigned long long)s_stableLightSkipDiag.armed,
        (unsigned long long)s_stableLightSkipDiag.eligible,
        (unsigned long long)s_stableLightSkipDiag.skip,
        (unsigned long long)s_stableLightSkipDiag.emit,
        (unsigned long long)s_stableLightSkipDiag.miss_notRegistered,
        (unsigned long long)s_stableLightSkipDiag.miss_noValidLight,
        (unsigned long long)s_stableLightSkipDiag.miss_envGen,
        (unsigned long long)s_stableLightSkipDiag.miss_needsFullBake,
        (unsigned long long)s_stableLightSkipDiag.miss_invalidLightIdx);
    std::fflush(stderr);
}

static void noteStableLightSkipFrame(uint32_t frame)
{
    if (!s_stableLightSkipDiag.enabled)
        return;

    if (!s_stableLightSkipDiag.initialized) {
        s_stableLightSkipDiag.initialized = true;
        s_stableLightSkipDiag.frame = frame;
        if (!s_stableLightSkipDiag.envPrinted) {
            const char* v = getenv("MC2_STABLE_LIGHT_SKIP");
            std::fprintf(stderr, "[STABLE_LIGHT_SKIP] MC2_STABLE_LIGHT_SKIP=%s\n",
                (v && *v) ? v : "(unset)");
            std::fflush(stderr);
            s_stableLightSkipDiag.envPrinted = true;
        }
        return;
    }

    if (frame == s_stableLightSkipDiag.frame)
        return;

    flushStableLightSkipDiag(s_stableLightSkipDiag.frame);
    s_stableLightSkipDiag.frame = frame;
    s_stableLightSkipDiag.armed = 0;
    s_stableLightSkipDiag.eligible = 0;
    s_stableLightSkipDiag.skip = 0;
    s_stableLightSkipDiag.emit = 0;
    s_stableLightSkipDiag.miss_notRegistered = 0;
    s_stableLightSkipDiag.miss_noValidLight = 0;
    s_stableLightSkipDiag.miss_envGen = 0;
    s_stableLightSkipDiag.miss_needsFullBake = 0;
    s_stableLightSkipDiag.miss_invalidLightIdx = 0;
}

static inline void recordStableLightSkipDiag(bool armed, bool eligible,
    bool notRegistered, bool noValidLight, bool envGenMiss, bool needsFullBake,
    bool invalidLightIdx)
{
    if (!mc2StableLightSkipDiagEnabled())
        return;

    s_stableLightSkipDiag.enabled = true;
    noteStableLightSkipFrame(g_mc2FrameCounter);

    if (armed)
        ++s_stableLightSkipDiag.armed;
    if (eligible)
        ++s_stableLightSkipDiag.eligible;

    if (armed && eligible) {
        ++s_stableLightSkipDiag.skip;
    } else {
        ++s_stableLightSkipDiag.emit;
        if (armed && notRegistered)
            ++s_stableLightSkipDiag.miss_notRegistered;
        if (armed && noValidLight)
            ++s_stableLightSkipDiag.miss_noValidLight;
        if (armed && envGenMiss)
            ++s_stableLightSkipDiag.miss_envGen;
        if (armed && needsFullBake)
            ++s_stableLightSkipDiag.miss_needsFullBake;
        if (armed && invalidLightIdx)
            ++s_stableLightSkipDiag.miss_invalidLightIdx;
    }
}
} // namespace

// ============================================================================
// [LIGHTBRIDGE-STABLE-SKIP-WIRE-1] Gate + diagnostics
// MC2_LIGHTBRIDGE_STABLE_SKIP=1  — enable the skip path (default OFF)
// MC2_LIGHTBRIDGE_STABLE_SKIP_DIAG=1 — enable per-frame diagnostic print (independent)
// ============================================================================
extern uint32_t mc2StaticLightHighWater();  // txmmgr.cpp — s_staticLightHighWater accessor

static bool mc2LightbridgeStableSkipEnabled() {
    static const bool s_enabled = ParseEnvBool("MC2_LIGHTBRIDGE_STABLE_SKIP");
    return s_enabled;
}
static bool mc2LightbridgeStableSkipDiagEnabled() {
    static const bool s_diagEnabled = ParseEnvBool("MC2_LIGHTBRIDGE_STABLE_SKIP_DIAG");
    return s_diagEnabled;
}
// STABLE-LIGHT-SKIP-BROADEN-1: extend the LBSS resubmit-skip to the legacy
// touch() Path-B variants (BldgAppearance::touch / TreeAppearance::touch).
// touchSerialCommit() already skips the redundant ResubmitCachedGpuLightData()
// when stableLightSkipEligible, but that path only runs when the FRAME-JOBS
// touch split is armed (MC2_FRAME_JOBS + MC2_FRAME_JOBS_TOUCH, BOTH default-OFF).
// In stock config the live path is touch(), which computed stableLightSkipEligible
// for diagnostics but resubmitted UNCONDITIONALLY — paying a per-static-prop GPU
// light resubmit every frame for provably-stable props. This gate broadens the
// skip to that path. Separate killswitch so it can be flipped independently of
// the serial-commit skip (which requires MC2_FRAME_JOBS to even run). Default OFF
// => byte-identical (the resubmit still runs). Requires MC2_LIGHTBRIDGE_STABLE_SKIP
// + MC2_STABLE_LIGHT_SKIP armed (same invariant proof: baked static prefix
// [0..s_staticLightHighWater) persists frame-to-frame under MC2_LIGHTBAKE).
static bool mc2StableLightSkipTouchEnabled() {
    static const bool s_enabled = ParseEnvBool("MC2_STABLE_LIGHT_SKIP_TOUCH");
    return s_enabled;
}

namespace {

// Per-frame counters for LIGHTBRIDGE_STABLE_SKIP diagnostics.
// Printed every 300 frames when MC2_LIGHTBRIDGE_STABLE_SKIP_DIAG=1.
struct LightbridgeStableSkipDiag {
    uint32_t frame         = 0xFFFFFFFFu;
    // Building counters
    uint64_t b_armed       = 0;  // stableLightSkipArmed
    uint64_t b_eligible    = 0;  // stableLightSkipEligible
    uint64_t b_taken       = 0;  // skip actually fired
    uint64_t b_blk_idx     = 0;  // blocked: invalid lightDataIndex
    uint64_t b_blk_gen     = 0;  // blocked: generation mismatch
    uint64_t b_blk_noelig  = 0;  // blocked: armed but not eligible
    // Tree counters
    uint64_t t_taken       = 0;
    uint64_t t_blk_idx     = 0;
    uint64_t t_total       = 0;
};

static LightbridgeStableSkipDiag s_lbssDiag;

static void lbssDiagPrint(const LightbridgeStableSkipDiag& d)
{
    fprintf(stderr,
        "LIGHTBRIDGE_STABLE_SKIP: enabled=%d armed=%llu eligible=%llu taken=%llu "
        "blocked_invalid_index=%llu blocked_generation_mismatch=%llu blocked_no_eligible=%llu "
        "static_prefix_count=%u\n",
        (int)mc2LightbridgeStableSkipEnabled(),  // references file-scope fn, not anon-ns
        (unsigned long long)d.b_armed,
        (unsigned long long)d.b_eligible,
        (unsigned long long)d.b_taken,
        (unsigned long long)d.b_blk_idx,
        (unsigned long long)d.b_blk_gen,
        (unsigned long long)d.b_blk_noelig,
        mc2StaticLightHighWater());
    fprintf(stderr,
        "LIGHTBRIDGE_TREE_STABLE_SKIP: total=%llu taken=%llu blocked_invalid_index=%llu\n",
        (unsigned long long)d.t_total,
        (unsigned long long)d.t_taken,
        (unsigned long long)d.t_blk_idx);
    fflush(stderr);
}

static void lbssDiagRollFrame(uint32_t frame)
{
    if (!mc2LightbridgeStableSkipDiagEnabled()) return;
    if (frame == s_lbssDiag.frame) return;
    // Print on new frame boundary (every 300 frames)
    if (s_lbssDiag.frame != 0xFFFFFFFFu && (s_lbssDiag.frame % 300u) == 0u)
        lbssDiagPrint(s_lbssDiag);
    s_lbssDiag = LightbridgeStableSkipDiag{};
    s_lbssDiag.frame = frame;
}

static inline void lbssRecordBldg(bool eligible, bool taken,
    bool blk_idx, bool blk_gen, bool blk_noelig)
{
    if (!mc2LightbridgeStableSkipDiagEnabled()) return;
    lbssDiagRollFrame(g_mc2FrameCounter);
    ++s_lbssDiag.b_armed;   // always called when armed (caller gate)
    if (eligible) ++s_lbssDiag.b_eligible;
    if (taken)    ++s_lbssDiag.b_taken;
    if (blk_idx)  ++s_lbssDiag.b_blk_idx;
    if (blk_gen)  ++s_lbssDiag.b_blk_gen;
    if (blk_noelig) ++s_lbssDiag.b_blk_noelig;
}

static inline void lbssRecordTree(bool taken, bool blk_idx)
{
    if (!mc2LightbridgeStableSkipDiagEnabled()) return;
    lbssDiagRollFrame(g_mc2FrameCounter);
    ++s_lbssDiag.t_total;
    if (taken)   ++s_lbssDiag.t_taken;
    if (blk_idx) ++s_lbssDiag.t_blk_idx;
}

} // namespace (lbss diag)

// ============================================================================
// FRAME-JOBS-2F: TOUCH-ENTRY-UNIFICATION diagnostics
// MC2_FRAME_JOBS_TOUCH_DIAG=1 — enable per-300-frame print (independent of lbss diag)
// Counters are cumulative per-frame accumulators, reset every 300 frames.
// ============================================================================
static bool mc2FrameJobsTouchDiagEnabled() {
    static const bool s_enabled = ParseEnvBool("MC2_FRAME_JOBS_TOUCH_DIAG");
    return s_enabled;
}

namespace {

struct TouchEntryDiag {
    uint32_t frame                    = 0xFFFFFFFFu;
    uint64_t serial_commit_hits       = 0;  // touchSerialCommit() calls (stamp set)
    uint64_t legacy_skipped           = 0;  // touch() early-returns (Path B suppressed)
    uint64_t legacy_ran_nosplit       = 0;  // touch() ran with no prior split commit
};

static TouchEntryDiag s_touchEntryDiag;

static void touchEntryDiagRollFrame(uint32_t frame)
{
    if (!mc2FrameJobsTouchDiagEnabled()) return;
    if (frame == s_touchEntryDiag.frame) return;
    if (s_touchEntryDiag.frame != 0xFFFFFFFFu && (s_touchEntryDiag.frame % 300u) == 0u) {
        fprintf(stderr,
            "FRAME_JOBS_TOUCH_ENTRY: serial_commit_hits=%llu legacy_skipped=%llu "
            "double_touch_prevented=%llu legacy_ran_nosplit=%llu\n",
            (unsigned long long)s_touchEntryDiag.serial_commit_hits,
            (unsigned long long)s_touchEntryDiag.legacy_skipped,
            (unsigned long long)s_touchEntryDiag.legacy_skipped,  // same as legacy_skipped
            (unsigned long long)s_touchEntryDiag.legacy_ran_nosplit);
        fflush(stderr);
    }
    s_touchEntryDiag = TouchEntryDiag{};
    s_touchEntryDiag.frame = frame;
}

} // namespace (touch-entry diag)

// [LIGHTSLOT v1] accessor free fns (defined mclib/txmmgr.cpp, global scope).
extern uint32_t mc2LightSlotBakedHighWater();
extern uint64_t mc2LightSlotDedupHits();
extern uint64_t mc2LightSlotActorKeyHits();
extern uint32_t mc2LightSlotTableCount();
namespace mc2_lightslot_trace {
    static bool          s_enabled      = false;
    static bool          s_inited       = false;
    static bool          s_emitted      = false;
    static uint32_t      s_armFrame     = 0;     // frame at which mission first seen ready
    static uint32_t      s_accFrame     = 0xFFFFFFFFu; // frame the current accumulation belongs to
    static uint64_t      s_instances    = 0;     // K: static-prop tree-instance captures THIS frame
    static std::set<uint32_t> s_distinctSlots;   // D/U: distinct lightDataIndex over static instances (this frame)
    static std::set<const void*> s_types;        // N: distinct override tree TYPES (by appearType ptr, this frame)

    static inline void maybeEmit();    // fwd
    static inline void initFromEnv();  // fwd

    // TREE-OVERRIDE-LOD-MVP Task 3 (K×M GATE): registration-time accumulators for
    // the higher-LOD (lod>0) baked light slots. The per-frame s_distinctSlots set
    // only ever sees activeLOD=0 (pinned), so it cannot reveal whether LOD1's
    // per-instance bake created a SECOND lightData_ slot per instance. These
    // counters capture the lightData_ index every lod>0 registration bake
    // resolves to: LOD1_baked_instances counts the bakes; LOD1_distinct_slots is
    // the set of distinct lightData_ indices those bakes resolved to. If LODs of
    // one instance dedup to LOD0's slot, LOD1_distinct_slots stays ~K (PASS); if
    // each instance's LOD1 takes a fresh slot, it grows toward K (additive → K×M
    // total). These are cross-frame (registration is one-shot at mission load).
    static uint64_t      s_lodBakeCount = 0;
    static std::set<uint32_t> s_lodDistinctSlots;
    static inline void recordLodBakeSlot(uint32_t lightDataIndex) {
        initFromEnv();
        if (!s_enabled) return;
        ++s_lodBakeCount;
        if (lightDataIndex != 0xFFFFFFFFu)
            s_lodDistinctSlots.insert(lightDataIndex);
    }

    static inline void initFromEnv() {
        if (s_inited) return;
        s_inited   = true;
        s_enabled  = (std::getenv("MC2_LIGHTSLOT_TRACE") != nullptr);
    }

    // Record one static-prop tree instance's resolved light slot + its type.
    // Accumulators are per-FRAME (reset on frame change) so s_instances is a
    // true K (instance count), not frame x instance, and the distinct-slot set
    // is a single-frame snapshot of the whole forest.
    static inline void recordInstance(const void* appearType, bool isOverride,
                                      uint32_t lightDataIndex) {
        initFromEnv();
        if (!s_enabled || s_emitted) return;
        if (g_mc2FrameCounter != s_accFrame) {
            // Frame boundary: the just-completed frame (s_accFrame) holds a
            // full snapshot of every static tree touched. Try to emit from it
            // BEFORE clearing for the new frame.
            maybeEmit();
            s_accFrame = g_mc2FrameCounter;
            s_instances = 0;
            s_distinctSlots.clear();
            s_types.clear();
        }
        ++s_instances;
        if (lightDataIndex != 0xFFFFFFFFu)
            s_distinctSlots.insert(lightDataIndex);
        if (appearType && isOverride)
            s_types.insert(appearType);
    }

    // Emit one summary line once, a few frames after mission_ready so every
    // instance has been touched at least once. Called from the tree capture
    // site (cheap; bails immediately once emitted / disabled).
    static inline void maybeEmit() {
        if (!s_enabled || s_emitted) return;
        if (!SmokeMode::missionHasStarted()) return;
        if (s_armFrame == 0) { s_armFrame = s_accFrame; return; }
        // Emit the just-completed frame's snapshot once it is >= 8 frames past
        // mission_ready (forest fully touched + distinct-slot set settled).
        if (s_accFrame == 0xFFFFFFFFu || s_accFrame < s_armFrame + 8) return;
        if (s_instances == 0) return;

        const uint64_t H = mc2LightSlotDedupHits() + mc2LightSlotActorKeyHits();
        const uint32_t B = mc2LightSlotBakedHighWater();
        const size_t   U = s_distinctSlots.size();   // distinct slots attributable to static-prop instances
        const size_t   D = s_distinctSlots.size();   // per-instance distinct (same set — see report)
        std::printf(
            "[LIGHTSLOT v1] static_prop_instances=%llu override_tree_types=%zu "
            "registered_recipes=%u unique_light_slots=%zu dedup_hits=%llu "
            "baked_slots=%u per_instance_distinct=%zu table_count=%u "
            "lod_bake_count=%llu lod_distinct_slots=%zu\n",
            (unsigned long long)s_instances, s_types.size(),
            B /* R ~= baked recipe high-water */, U,
            (unsigned long long)H, B, D, mc2LightSlotTableCount(),
            (unsigned long long)s_lodBakeCount, s_lodDistinctSlots.size());
        std::fflush(stdout);
        s_emitted = true;
    }
}

static void mc2CacheOrBakeStaticGpuLight(TG_MultiShape* shape,
                                         bool registered, int32_t recipeIndex)
{
	extern bool mc2LightBakeEnabled();
	extern bool mc2GetBakedStaticLight(int32_t, TG_HWLightsData&);
	extern void mc2SetBakedStaticLight(int32_t, const TG_HWLightsData&);
	extern void mc2WriteStaticLightSlot(int32_t, const TG_HWLightsData&);  // [LIGHTBAKE v2]
	if (!shape) return;
	if (!mc2LightBakeEnabled() || !registered || recipeIndex < 0) {
		shape->CacheGpuLightData();                  // unchanged D2/legacy path
		return;
	}
	TG_HWLightsData baked;
	if (mc2GetBakedStaticLight(recipeIndex, baked)) {
		shape->EmitBakedGpuLightData(recipeIndex, baked);    // HIT: recompute retired
	} else {
		shape->CacheGpuLightData();                          // MISS: real gather (frame 1 / post-invalidate)
		// C1 (adversarial review): CacheGpuLightData early-returns when
		// !g_useGpuObjects && !g_useGpuMechs (supported MC2_GPU_OBJECTS=0
		// operator config), leaving cachedGpuLightIndex_ at the
		// 0xFFFFFFFF sentinel and leaf->lightData_ stale. Only persist
		// the bake if the gather actually ran (valid index) -- else leave
		// uncached so it retries next frame; never persist a no-op
		// snapshot (would poison s_bakedStaticLight until invalidate).
		const TG_HWLightsData* leaf = shape->peekCachedLeafLightData();
		if (leaf && shape->getCachedGpuLightIndex() != 0xFFFFFFFFu) {
			mc2SetBakedStaticLight(recipeIndex, *leaf);      // mission source-of-truth (re-bake/invalidate)
			// [LIGHTBAKE v2] write the PERMANENT static slot once
			// (lightData_[recipeIndex] CPU mirror + advance S), then
			// point this multi at it -- identical end-state to the HIT
			// path, so from this frame on there is NO per-frame
			// addLightDataStructure for this recipe.
			mc2WriteStaticLightSlot(recipeIndex, *leaf);
			// [LIGHTBAKE-PROOF v1] slot-write integrity check (match=1 = permanent slot
			// faithfully stores the gathered leaf; NOT an independent A/B — see the
			// function comment in txmmgr.cpp). pos/appearance are diagnostic labels.
			// Slot written synchronously just above. No-op unless MC2_LIGHTBAKE_PARITY.
			mc2LightBakeParityCheck(recipeIndex, leaf, 0.0f, 0.0f, 0.0f, nullptr);
			shape->EmitBakedGpuLightData(recipeIndex, *leaf);
		}
	}
}

//-----------------------------------------------------------------------------
long BldgAppearance::update (bool animate)
{
	::mc2_object_recon::Scope _recon_bldg_(
		&::mc2_object_recon::g_per_frame.bldg_update_ns,
		&::mc2_object_recon::g_per_frame.bldg_update_calls);
	Stuff::Point3D xlatPosition;
	Stuff::UnitQuaternion rot;

	//----------------------------------------
	// Recycle the weapon Nodes
	if (nodeRecycle)
	{
		for (long i=0;i<appearType->numWeaponNodes;i++)
		{
			if (nodeRecycle[i] > 0.0f)
			{
				nodeRecycle[i] -= frameLength;
				if (nodeRecycle[i] < 0.0f)
					nodeRecycle[i] = 0.0f;
			}
		}
	}

   	if (appearType->terrainLightRGB != 0xffffffff && (eye->nightFactor > 0.0f) && !forceLightsOut)
   	{
   		if (!pointLight)
   		{
   			pointLight = (TG_LightPtr)malloc(sizeof(TG_Light));
   			pointLight->init(TG_LIGHT_TERRAIN);
   			lightId = eye->addWorldLight(pointLight);
   	
   			pointLight->SetaRGB(appearType->terrainLightRGB);
   			pointLight->SetIntensity(appearType->terrainLightIntensity);
   			pointLight->SetFalloffDistances(appearType->terrainLightInnerRadius, appearType->terrainLightOuterRadius);
   		}
		
		if (pointLight)
		{
			Stuff::Point3D ourPosition;
			ourPosition.x = -position.x;
			ourPosition.y = position.z;
			ourPosition.z = position.y;
	
			pointLight->direction = ourPosition;
	
			Stuff::LinearMatrix4D lightToWorldMatrix;
			lightToWorldMatrix.BuildTranslation(ourPosition);
			lightToWorldMatrix.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			pointLight->SetLightToWorld(&lightToWorldMatrix);
			pointLight->SetPosition(&position);
			pointLight->SetIntensity(appearType->terrainLightIntensity * eye->getNightFactor());
		}
   	}
	else
	{
		//Turn the lights off!
		//Need to kill the light source here too!
		if (pointLight)
		{
			eye->removeWorldLight(lightId,pointLight);
			free(pointLight);
			pointLight = NULL;
		}
	}

	// (E) T3.1: SpotLight_-child illumination. Distinct from the per-building
	// terrain pointLight above (R7). World position is valid here in update()
	// (not in init() per C-r1 C1). Lazy first-night register; subsequent frames
	// do per-frame SetPosition + active toggle ONLY (R3: no per-frame add/remove).
	// Gate retired in T3.1 (Stage 3 substitutive completion); behavior is now
	// unconditional. See docs/superpowers/plans/2026-05-20-spotlight-real-illumination-plan.md
	if (bldgShape)
	{
		if (!spotlightsRegistered_ && eye && eye->isNight)
		{
			// Public accessors (msl.h:431 GetNumShapes, msl.h:438 GetShapeRec)
			// because BldgAppearance is NOT in the TG_MultiShape friend list at
			// msl.h:251-256 (C-r4 C1 fix).
			// [SPOT_DIAG v1] T1.15 per-actor registration counters.
			int diagChildrenWalked = 0;
			int diagSpotlightsFound = 0;
			int diagRegistered = 0;
			int diagOverflow = 0;
			for (int i = 0; i < bldgShape->GetNumShapes(); ++i)
			{
				const TG_ShapeRec* recp = bldgShape->GetShapeRec(i);
				if (!recp) continue;
				TG_Shape* child = recp->node;
				// Mirror canonical batcher guards at
				// gos_static_prop_batcher.cpp:2477 (C-r3 M1).
				if (!child || !recp->processMe) continue;
				++diagChildrenWalked;
				if (!child->GetIsSpotlight()) continue;  // tgl.h:951
				++diagSpotlightsFound;

				// Resolve node-NAME id (NOT listOfShapes index) per the anubis
				// pattern at mech3d.cpp:3336-3338 (C-r3 C1).
				const char* nodeName = child->getNodeName();  // tgl.h:964
				if (!nodeName) continue;
				long nodeId = bldgShape->GetNodeNameId(nodeName);
				if (nodeId == -1) continue;

				TG_LightPtr light = (TG_LightPtr)malloc(sizeof(TG_Light));
				light->init(TG_LIGHT_POINT);            // v1 — POINT (OQ2)
				light->SetaRGB(0xffe8c870);             // OQ3 warm hardcoded
				light->SetIntensity(0.5f);              // OQ4 initial
				light->SetFalloffDistances(20.0f, 80.0f); // OQ4

				long slotId = eye->addWorldLight(light);  // camera.h:805
				if (slotId < 0) { free(light); continue; }  // pool overflow

				spotlightLights_.push_back(light);
				spotlightSlotIds_.push_back(static_cast<DWORD>(slotId));
				spotlightNodeIds_.push_back(static_cast<int>(nodeId));
				// T1.16 — tag this slot as (E)-owned, source=Bldg, so the
				// Camera::updateLights per-slot probe can recognize it.
				mc2_spotlight_diag::tag_slot(slotId, mc2_spotlight_diag::Bldg);
				++diagRegistered;
			}
			// Pool overflow detection: any spotlight found but not registered
			// where node-id resolution succeeded is treated as overflow signal.
			// Simpler: overflow = spotlights_found - registered (under-counts
			// nodeName misses; honest enough for the H1 disambiguation).
			diagOverflow = diagSpotlightsFound - diagRegistered;
			if (diagOverflow < 0) diagOverflow = 0;
			s_spotDiagBldgRegistered += (unsigned long)diagRegistered;
			s_spotDiagBldgOverflows  += (unsigned long)diagOverflow;
			++s_spotDiagBldgActors;
			// First-hit per actor — always-on (one stderr line per actor that
			// walks lazy-init, regardless of env). Cap noise by emitting only
			// for the first 8 actors to avoid log flood; cap is generous enough
			// to confirm registration is firing across distinct buildings.
			if (s_spotDiagBldgActors <= 8) {
				std::fprintf(stderr,
					"[SPOT_DIAG v1] event=first_register class=bldg actor_id=%p "
					"children_walked=%d spotlights_found=%d registered=%d overflow=%d\n",
					(void*)this, diagChildrenWalked, diagSpotlightsFound,
					diagRegistered, diagOverflow);
				std::fflush(stderr);
			}
			spotlightsRegistered_ = true;
		}
		// [SPOT_DIAG v1] T1.15 per-summary registration aggregate (bldg).
		// Increments every BldgAppearance::update() call; emit every 600 calls
		// when env=1. `calls=N` rather than `frames=N` because update() runs per
		// actor per frame (many actors -> many calls per frame).
		if (s_spotDiagBldgEnabled) {
			++s_spotDiagBldgCalls;
			if ((s_spotDiagBldgCalls % 600) == 0) {
				std::fprintf(stderr,
					"[SPOT_DIAG v1] event=registration_summary class=bldg "
					"calls=%lu actors_first_hit=%lu lights_registered=%lu overflows=%lu\n",
					s_spotDiagBldgCalls, s_spotDiagBldgActors,
					s_spotDiagBldgRegistered, s_spotDiagBldgOverflows);
				std::fflush(stderr);
			}
		}

		// Per-frame in-place update. Runs UNCONDITIONALLY once registered
		// (NOT inside the isNight gate above) — C-r2 C2: day->night->day
		// transitions toggle active via the gate below; lights stay allocated.
		for (size_t k = 0; k < spotlightLights_.size(); ++k)
		{
			Stuff::Vector3D childPos =
				getNodeIdPosition(spotlightNodeIds_[k]);
			spotlightLights_[k]->SetPosition(&childPos);
			// Rule-2 correctness fix: lightToWorld is consumed at
			// msl.cpp:1659 (s_lightToShape = lightToWorld * worldToShape).
			// Without setting it, lightToShape collapses to worldToShape
			// alone and the light's effective world position falls back to
			// origin regardless of the `position` field for some consumers.
			// Anubis sets this same matrix; existing per-building pointLight
			// at bdactor.cpp:1955 also sets it. Translation-only suffices
			// for POINT lights (matches the anubis/terrain-light pattern).
			Stuff::LinearMatrix4D lightToWorldMatrix;
			Stuff::Point3D childPosP;
			childPosP.x = childPos.x; childPosP.y = childPos.y; childPosP.z = childPos.z;
			lightToWorldMatrix.BuildTranslation(childPosP);
			lightToWorldMatrix.BuildRotation(Stuff::EulerAngles(0.0f, 0.0f, 0.0f));
			spotlightLights_[k]->SetLightToWorld(&lightToWorldMatrix);
			// eye->isNight is a bare field at camera.h:272 (C-r3 C2). visible
			// matches anubis at mech3d.cpp:3353 (C-r1 C5). forceLightsOut
			// matches the existing per-building pointLight gate at :1933.
			spotlightLights_[k]->active =
				(eye && eye->isNight && visible && !forceLightsOut);
		}
	}

	if (forceLightsOut)
		bldgShape->SetLightsOut(true);
		
	//Update flashing regardless of view!!!
	if (duration > 0.0f)
	{
		duration -= frameLength;
		currentFlash -= frameLength;
		if (currentFlash < 0.0f)
		{
			drawFlash ^= true;
			currentFlash = flashDuration;
		}
	}
	else
	{
		drawFlash = false;
	}

	// Under the GPU static-prop path, compute xlatPosition/rot + fog/light
	// for every building so the later TransformMultiShape (also gated on
	// g_useGpuStaticProps) has valid inputs.
	if (inView || g_useGpuStaticProps)
	{
		if (appearType->spinMe)
			rotation += SPINRATE * frameLength;

 		if (rotation > 180)
			rotation -= 360;

		if (rotation < -180)
			rotation += 360;

		//-------------------------------------------
		// Does math necessary to draw Tree
		float yaw = rotation * DEGREES_TO_RADS;
		rot = Stuff::EulerAngles(0.0f, yaw, 0.0f);
	
		if (appearType->spinMe && land)
		{
			//Make sure we are above the water level
			if (position.z < Terrain::waterElevation)
				position.z = Terrain::waterElevation;
		}

		xlatPosition.x = -position.x;
		xlatPosition.y = position.z;
		xlatPosition.z = position.y;

		if (!fogLightSet)
		{
			unsigned char lightr,lightg,lightb;
			float lightIntensity = 1.0f;
			if (land)
				lightIntensity = land->getTerrainLight(position);

			lightr = eye->getLightRed(lightIntensity);
			lightg = eye->getLightGreen(lightIntensity);
			lightb = eye->getLightBlue(lightIntensity);

			lightRGB = (lightr<<16) + (lightg<<8) + lightb;

			fogRGB = 0xff<<24;
			float fogStart = eye->fogStart;
			float fogFull = eye->fogFull;

			if (xlatPosition.y < fogStart)
			{
				float fogFactor = fogStart - xlatPosition.y;
				if (fogFactor < 0.0)
					fogRGB = 0xff<<24;
				else
				{
					fogFactor /= (fogStart - fogFull);
					if (fogFactor <= 1.0)
					{
						fogFactor *= fogFactor;
						fogFactor = 1.0 - fogFactor;
						fogFactor *= 256.0;
					}
					else
					{
						fogFactor = 256.0;
					}

					unsigned char fogResult = float2long(fogFactor);
					fogRGB = fogResult << 24;
				}
			}
			else
			{
				fogRGB = 0xff<<24;
			}

			fogLightSet = true;
		}
	
		eye->setLightColor(0,lightRGB);
		eye->setLightIntensity(0,1.0);

		if (useFog)
			bldgShape->SetFogRGB(fogRGB);
		else
			bldgShape->SetFogRGB(0xffffffff);
	
		Stuff::UnitQuaternion turretRot;
		turretRot = Stuff::EulerAngles((turretPitch * DEGREES_TO_RADS),(turretYaw * DEGREES_TO_RADS),0.0f);
		if (rotationalNodeId == -1)
	   		rotationalNodeId = bldgShape->SetNodeRotation(appearType->rotationalNodeId,&turretRot);
   
	   	bldgShape->SetNodeRotation(rotationalNodeId,&turretRot);
	}

	float oldFrame = currentFrame;
	if (animate && bdFrameRate != 0.0f)
	{
		//--------------------------------------------------------
		// Make sure animation runs no faster than bdFrameRate fps.
		float frameInc = bdFrameRate * frameLength;
		
		//---------------------------------------
		// Increment Frames -- Everything else!
		if (frameInc != 0.0f)
		{
			if (!setFirstFrame)		//DO NOT ANIMATE ON FIRST FRAME!  Wait a bit!
			{
				if (isReversed)
					currentFrame -= frameInc;
				else
					currentFrame += frameInc;
			}
			else
			{
				setFirstFrame = false;
			}
	
			//--------------------------------------
			//Check Positive overflow of Animation
			if (currentFrame >= appearType->getNumFrames(bdAnimationState))
			{
				if (isLooping)
					currentFrame -= appearType->getNumFrames(bdAnimationState);
				else
					currentFrame = appearType->getNumFrames(bdAnimationState) - 1;
					
				canTransition = true;		//Whenever we have completed one cycle or at last frame, OK to move on!
			}
			
	
			//--------------------------------------
			//Check negative overflow of gesture
			if (currentFrame < 0)
			{
				if (isLooping)
					currentFrame += appearType->getNumFrames(bdAnimationState); 
				else
					currentFrame = 0.0f; 
					
				canTransition = true;		//Whenever we have completed one cycle or at last frame, OK to move on!
			}
		}
		
		bldgShape->SetFrameNum(currentFrame);
	}

	// Under the GPU static-prop path we need listOfColors / shapeToWorld
	// fresh every frame regardless of inView so the batcher can safely
	// memcpy from shape->listOfColors during submit().
	if (inView || g_useGpuStaticProps)
	{
		bldgShape->SetUseShadow(false);

		bldgShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
		// Slice 2 (object-offload) — Stage 2.B: eligibility hoist.
		// Branch lives INSIDE the existing inView||g_useGpuStaticProps cull
		// gate to preserve slice 1's R1 invariant (no cull bypass).
		// Run positions-only when:
		//   - g_useGpuObjects is on, AND
		//   - this actor did not hit a late-registration recovery last frame
		//     (needsFullBakeNextFrame is a NEW bool from Stage 2.A; set by
		//     BldgAppearance::render when submitMultiShape returns false with
		//     wasLastFailureLateRegistration() true), AND
		//   - the multi-shape's leaves are all registered with the slice 1
		//     batcher (isMultiShapeEligibleForGpuObjects mirrors slice 1's
		//     render-time per-child gates EXCEPT late-reg, which is handled
		//     via the recovery flag above).
		// Otherwise full bake. The full bake clears the recovery flag —
		// re-establishing valid .argb before render reads it.
		// PERF DIAGNOSTIC 2026-05-06: Tracy zones to attribute the 9.52 µs/call
		// observed in TerrainObject::update appearanceUpdate. Six theories under
		// investigation; these zones discriminate between them. Demote to silent
		// (or remove) once the regression is identified.
		bool gpuEligible;
		{
			// PREVIEW-FIX: never take the GPU positions-only/hierarchy-only fast
			// path in the SimpleCamera preview — the CPU MLR draw needs the full
			// listOfVertices (positions + argb) that only TransformMultiShape bakes.
			// BUILDING-PBR: the gated PBR path currently renders through the same
			// CPU MLR queue, so it needs that full bake too. Keep this in lockstep
			// with the render-side GPU submit bypass.
			const bool buildingPbrCpuRenderActive = appearType && appearType->buildingPbrEnabled;
			gpuEligible = g_useGpuObjects &&
			              (g_mechPreviewRenderDepth == 0) &&
			              !buildingPbrCpuRenderActive &&
			              !needsFullBakeNextFrame &&
			              GpuStaticPropBatcher::instance().isMultiShapeEligibleForGpuObjects(bldgShape);
		}

		if (gpuEligible)
		{
			// Stage 2.D.2 fix: cache GPU light data NOW, while worldLights[0]->aRGB
			// is the per-actor terrain-scaled value set at line 2144 above.
			// By the time submitMultiShape() runs (during renderLists()), later
			// actors have overwritten worldLights[0]->aRGB for their positions.
			{
				mc2CacheOrBakeStaticGpuLight(bldgShape, staticReg.registered, staticReg.recipeIndex);
				// 2026-05-11 per-instance capture: snapshot the multi's just-written
				// cache slot for THIS actor before sibling actors of the same
				// multi-type overwrite it. Ferried to RecipeRange via markVisible().
				staticReg.lightDataIndex = bldgShape->getCachedGpuLightIndex();
			}
			{
				// GPU-INSTANCE-SKIP-POOLS-1 (2026-06-03): see TreeAppearance::
				// update for the full rationale. gpuEligible guarantees every
				// leaf is registered; run the ZERO-POOL hierarchy walk so no TGL
				// frame pool is allocated per visible instance. submit reads
				// rec.shapeToWorld (populated by the walk) + the debug-only
				// zero-padded Colors SSBO. MC2_LEGACY_INSTANCE_POOLS=1 reverts.
				if (gos_StaticPropLegacyInstancePools())
					bldgShape->TransformMultiShape_PositionsOnly (&xlatPosition,&rot);
				else
					bldgShape->TransformMultiShape_HierarchyOnly (&xlatPosition,&rot);
			}
			// Stage 2.D.2: on the dual-emit frame (latch Armed), also run
			// the full bake so listOfTriangles[].aRGBLight is populated for
			// the parity snapshot captured in submit(). This call is a pure
			// CPU-side data write; it does NOT affect GPU output (the shader
			// uses a_aRGBLight from the type-level VBO, not listOfVertices.argb).
			// No addRenderShape: GPU-eligible actors reach this branch only
			// when g_useGpuObjects is true. The legacy Render() path (which
			// calls addTriangle) is bypassed because submitMultiShape()
			// handles the GPU draw instead. In Renderer 3 (GL 4.3), the
			// addTriangle queue is never flushed to hardware — only the GPU
			// batcher's direct draw is visible. So calling TransformMultiShape
			// here (for the parity snapshot) does NOT result in double-draw.
			// Stage 2.D.3: per-actor gate. Bootstrap arm returns true for
			// every shape; sample arm returns true only for the picked actor.
			if (gos_object_parity::IsDualEmitArmedForActor(bldgShape)) {
				bldgShape->TransformMultiShape (&xlatPosition,&rot);
			}
		}
		else
		{
			// GPU-INSTANCE-SKIP-POOLS-1 (2026-06-03): see TreeAppearance::update
			// for the full rationale + A/B evidence. For a registered type the
			// full-bake pool content is vestigial (GPU draws from the per-type
			// VBO + rec.shapeToWorld; light cache below is pool-independent), so
			// run the zero-pool hierarchy walk. mc2CacheOrBakeStaticGpuLight still
			// runs to seed the light index. MC2_LEGACY_INSTANCE_POOLS=1 reverts.
			// PREVIEW-FIX: force the full bake in the SimpleCamera preview so the
			// CPU MLR draw has complete listOfVertices (hierarchy-only leaves it stale).
			const bool buildingPbrCpuRenderActive = appearType && appearType->buildingPbrEnabled;
			if (!buildingPbrCpuRenderActive &&
			    !gos_StaticPropLegacyInstancePools() &&
			    (g_mechPreviewRenderDepth == 0) &&
			    GpuStaticPropBatcher::instance().isMultiShapeEligibleForGpuObjects(bldgShape))
				bldgShape->TransformMultiShape_HierarchyOnly (&xlatPosition,&rot);
			else
				bldgShape->TransformMultiShape (&xlatPosition,&rot);
			// 2026-05-10: also seed cachedGpuLightIndex_ in the full-bake
			// branch. Without this, the first-frame transition out of the
			// H4 latch (set by registerStatic at :2754) leaves the index
			// at UINT32_MAX, and the static-path render gate at :1612
			// (`getCachedGpuLightIndex() == UINT32_MAX → invalidate`)
			// invalidates the registration on the very next render —
			// markVisible() never fires, registry::flush() short-circuits,
			// substrate gets no static-prop records, and the cull writes
			// 0 to all bucketCountData. The fix mirrors the gpuEligible
			// branch's CacheGpuLightData call at :2314 so any path
			// through update() seeds the light index. Cheap: same call
			// already runs unconditionally in submitMultiShape; here we
			// just hoist its effect to be visible to render() this frame.
			mc2CacheOrBakeStaticGpuLight(bldgShape, staticReg.registered, staticReg.recipeIndex);
			// 2026-05-11 per-instance capture: see gpuEligible branch above.
			staticReg.lightDataIndex = bldgShape->getCachedGpuLightIndex();
			needsFullBakeNextFrame = false;
		}

		if ((turn > 3) && useShadows)
			beenInView = true;
			
		//------------------------------------------------
		// Update GOSFX
		if (destructFX && destructFX->IsExecuted())
		{
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D 	localToWorld;
			Stuff::Point3D			tPosition;
				
			//Stuff::Vector3D offsetPosition;
			//offsetPosition.x = Terrain::worldUnitsPerVertex / 3.0f;
			//offsetPosition.y = -(Terrain::worldUnitsPerVertex / 3.0f);
			//offsetPosition.z = 0.0f;

			//OppRotate(offsetPosition,rotation);

			Stuff::Vector3D actualPosition = position;
			//actualPosition.Add(position,offsetPosition);

			tPosition.x = -actualPosition.x;
			tPosition.y = actualPosition.z;
			tPosition.z = actualPosition.y;

			float yaw = (180.0f + rotation) * DEGREES_TO_RADS;
			Stuff::UnitQuaternion rot;
			rot = Stuff::EulerAngles(0.0f, yaw, 0.0f);
 			shapeOrigin.BuildRotation(rot);
			shapeOrigin.BuildTranslation(tPosition);

	 		Stuff::OBB boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
	
			bool result = destructFX->Execute(&info);
			if (!result)
			{
				destructFX->Kill();
				delete destructFX;
				destructFX = NULL;
			}
		}
		
		if (isActivitying)
		{
			Stuff::LinearMatrix4D 	shapeOrigin;
			Stuff::LinearMatrix4D	localToWorld;
			Stuff::LinearMatrix4D	localResult;
					
			if (activityNodeId == -1)
				activityNodeId = bldgShape->GetNodeNameId("activity_node");
			Stuff::Vector3D dustPos = getNodeIdPosition(activityNodeId);

			if (rotationalNodeId == -1)
			{
				if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
	   				rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
				else
					rotationalNodeId = -2;
			}

			if (rotationalNodeId >= 0)
				dustPos = getNodeIdPosition(rotationalNodeId);
 			
			Stuff::Point3D wakePos;
			wakePos.x = -dustPos.x;
			wakePos.y = dustPos.z;
			wakePos.z = dustPos.y;
			
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(wakePos);
					
			/*
			Stuff::UnitQuaternion effectRot;
			effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
			localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
			localResult.Multiply(localToWorld,shapeOrigin);
			*/
			
			Stuff::OBB boundingBox;
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
			
			// sebi: make as all other do with Execute(), otherwise ther is constanrt assert in Execute() function (at line Verify(IsExecuted()) )
			if (activity && activity->IsExecuted())
			{
				bool result = activity->Execute(&info);
				if (!result)
				{
					activity->Kill();		//Effect is over.  Otherwise, wait until hit!
					delete activity;
					activity = NULL;
				}

			}
			
			if (activity1)
			{
				if (activityNodeId == -1)
					activityNodeId = bldgShape->GetNodeNameId("activity_node");
				Stuff::Vector3D dustPos = getNodeIdPosition(activityNodeId);
	
				if (rotationalNodeId == -1)
				{
					if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
		   				rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
					else
						rotationalNodeId = -2;
				}
	
				if (rotationalNodeId >= 0)
					dustPos = getNodeIdPosition(rotationalNodeId);
				
				Stuff::Point3D wakePos;
				wakePos.x = -dustPos.x;
				wakePos.y = dustPos.z;
				wakePos.z = dustPos.y;
				
				shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
				shapeOrigin.BuildTranslation(wakePos);
						
				/*
				Stuff::UnitQuaternion effectRot;
				effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
				localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
				localResult.Multiply(localToWorld,shapeOrigin);
				*/
				
				Stuff::OBB boundingBox;
				gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,&boundingBox);
				
				//sebi:
				if (activity1 && activity1->IsExecuted())
				{
					bool result = activity1->Execute(&info);
					if (!result)
					{
						activity1->Kill();		//Effect is over.  Otherwise, wait until hit!
						delete activity1;
						activity1 = NULL;
					}
				}
				//

			}
		}
	}
	
	return TRUE;
}

//-----------------------------------------------------------------------------
void BldgAppearance::startActivity (long effectId, bool loop)
{
	//Check if we are already playing one.  If not, be active!
	
	//First, check if its even loaded.
	// can easily preload this.  Should we?  NO.  We don't know what will be passed in.
	if (!activity && useNonWeaponEffects)
	{
   		if (strcmp(weaponEffects->GetEffectName(effectId),"NONE") != 0)
   		{
			//--------------------------------------------
			// Yes, load it on up.
			unsigned flags = gosFX::Effect::ExecuteFlag|gosFX::Effect::LoopFlag;
			if (!loop)
				flags = gosFX::Effect::ExecuteFlag;

			Check_Object(gosFX::EffectLibrary::Instance);
			gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(effectId));
			
			if (gosEffectSpec)
			{
				activity = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
				gosASSERT(activity != NULL);
				
				Stuff::Vector3D testPos = getNodeNamePosition("activity_node1");
				if (testPos != position)
				{
					activity1 = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
					gosASSERT(activity1 != NULL);
				}

  				MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
			}
		}
	}
	
	if (!isActivitying && activity)		//Start the effect if we are not running it yet!!
	{
		Stuff::LinearMatrix4D 	shapeOrigin;
		Stuff::LinearMatrix4D	localToWorld;
		Stuff::LinearMatrix4D	localResult;
		
		if (activityNodeId == -1)
   			activityNodeId = bldgShape->GetNodeNameId("activity_node");
   		Stuff::Vector3D nodePos = getNodeIdPosition(activityNodeId);

   		if (rotationalNodeId == -1)
   		{
   			if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
      				rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
   			else
   				rotationalNodeId = -2;
   		}

   		if (rotationalNodeId >= 0)
   			nodePos = getNodeIdPosition(rotationalNodeId);

 		Stuff::Point3D wakePos;
		wakePos.x = -nodePos.x;
		wakePos.y = nodePos.z;	//Wake is at Water Level!
		wakePos.z = nodePos.y;
		
 		shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
		shapeOrigin.BuildTranslation(wakePos);
				
		/*
		Stuff::UnitQuaternion effectRot;
		effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
		localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
		localResult.Multiply(localToWorld,shapeOrigin);
		*/
			
 		gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);

		activity->Start(&info);
		
		if (activity1)
		{
			if (activityNode1Id == -1)
				activityNode1Id = bldgShape->GetNodeNameId("activity_node1");
			Stuff::Vector3D nodePos = getNodeIdPosition(activityNode1Id);

			if (rotationalNodeId == -1)
			{
				if (S_stricmp(appearType->rotationalNodeId,"NONE") != 0)
	   				rotationalNodeId = bldgShape->GetNodeNameId(appearType->rotationalNodeId);
				else
					rotationalNodeId = -2;
			}

			if (rotationalNodeId >= 0)
				nodePos = getNodeIdPosition(rotationalNodeId);
			
 			Stuff::Point3D wakePos;
			wakePos.x = -nodePos.x;
			wakePos.y = nodePos.z;	//Wake is at Water Level!
			wakePos.z = nodePos.y;
			
			shapeOrigin.BuildRotation(Stuff::EulerAngles(0.0f,0.0f,0.0f));
			shapeOrigin.BuildTranslation(wakePos);
					
			/*
			Stuff::UnitQuaternion effectRot;
			effectRot = Stuff::EulerAngles(0.0f,rotation * DEGREES_TO_RADS,0.0f);
			localToWorld.Multiply(gosFX::Effect_Against_Motion,effectRot);
			localResult.Multiply(localToWorld,shapeOrigin);
			*/
				
			gosFX::Effect::ExecuteInfo info((Stuff::Time)scenarioTime,&shapeOrigin,NULL);
	
			activity1->Start(&info);
		}
		
		isActivitying = true;
	}
}

//-----------------------------------------------------------------------------
void BldgAppearance::stopActivity (void)
{
	if (isActivitying)		//Stop the effect if we are running it!!
	{
		if(activity) //sebi
			activity->Kill();
		if (activity1)
			activity1->Kill();
	}
	
	isActivitying = false;
}

//-----------------------------------------------------------------------------
void BldgAppearance::flashBuilding (float dur, float fDuration, DWORD color)
{
	duration = dur;
	flashDuration = fDuration;
	flashColor = color;
	drawFlash = true;
	currentFlash = flashDuration;
}

//-----------------------------------------------------------------------------
// Stage 3.D: BldgAppearance static-registry path. Mirror of TreeAppearance's
// IsStaticNow / touch / invalidateStaticRegistration / isStaticEligible.
// Registry-replay eligibility for buildings is stricter than for trees because
// buildings have many dynamic states (animation, spin, flash, destruct FX).
// memory/bldg_animation_lod_swap_unsafe.md is the load-bearing reason: animated
// types share LOD-0 node-index state across instances, so replaying a recipe
// for an animated building drives the wrong node when LOD swaps.

namespace {
	// Type-level "any animation defined for this building type". Even if the
	// current instance isn't actively animating, an animated TYPE is excluded
	// from the static path because LOD swap could surface the animation later
	// and break the cached recipe.
	bool bldgTypeHasAnimations(const BldgAppearanceType* t) {
		if (!t) return false;
		for (long i = 0; i < MAX_BD_ANIMATIONS; ++i) {
			if (t->bdAnimData[i] != nullptr) return true;
		}
		return false;
	}

	bool bldgGestureHasAnimation(const BldgAppearanceType* t, long gestureId) {
		if (!t) return false;
		if (gestureId < 0 || gestureId >= MAX_BD_ANIMATIONS) return false;
		return t->bdAnimData[gestureId] != nullptr;
	}
} // anon namespace

bool BldgAppearance::isStaticEligible() const
{
	// Type-level disqualifiers.
	if (!appearType)        return false;
	if (appearType->spinMe) return false;
	// BLDG-TYPE-ANIM-GATE-FIX-1 (nifty): legacy type-level animation-capacity
	// check. Kill-switch MC2_BLDG_TYPE_ANIM_STATIC_ELIGIBLE=0 restores old
	// behaviour. When enabled (default), the bdAnimationState guard below is
	// sufficient.
	if (!s_bldgTypeAnimStaticEligible && bldgTypeHasAnimations(appearType))
		return false;
	// Instance-level disqualifiers.
	if (drawFlash)          return false;
	if (destructFX)         return false;
	if (activity)           return false;
	if (activity1)          return false;
	// MODEL-OVERRIDE admission fix (SEAMPROBE-1, override branch): a renderOnly
	// override prop substitutes a static .glb render shape that carries NO MC2
	// bdAnimData; setAnimation() on it is a no-op. Stock buildings get a default
	// idle gesture (bdAnimationState=0) set during init/update even when the
	// TYPE has no animations — which spuriously trips the guard below for an
	// override prop, leaving its static recipe permanently un-admitted
	// (markVisible never fires -> no substrate record -> never drawn). When this
	// is an override-backed type with no real animations, the cached recipe is a
	// faithful representation regardless of bdAnimationState, so skip the guard.
	// Strictly scoped to bldgRenderShape!=nullptr: stock path byte-identical.
	// Also require no rotational node: turrets/radars rotate via rotationalNodeId
	// (code-driven), not bdAnimData, so bldgTypeHasAnimations() misses them.
	// A renderOnly static .glb cannot represent a rotating part -> never admit.
	const bool hasRotationalNode =
		appearType->rotationalNodeId[0] != '\0'
		&& S_stricmp(appearType->rotationalNodeId, "NONE") != 0;
	const bool overrideStatic =
		appearType->bldgRenderShape && !bldgTypeHasAnimations(appearType) && !hasRotationalNode;
	const bool activeGestureHasAnimation =
		bdAnimationState != -1 && bldgGestureHasAnimation(appearType, bdAnimationState);
	if (activeGestureHasAnimation && !overrideStatic) return false;  // currently animating
	return true;
}

// Task 5 (Track B): mission-load bulk static-prop registration.
// Called from GameObjectManager::registerStaticPropsForMissionLoad() after
// primeTerrainObjectsForMissionLoad() has set position/rotation on every
// actor. Populates shapeToWorld matrices via TransformMultiShape_PositionsOnly,
// builds a recipe batch per leaf via buildRecipeFromShape, and registers with
// GpuStaticPropRegistry. HC-1: writes directly to typed staticReg member.
void BldgAppearance::registerStatic() {
	// [SEAMPROBE] stage 2: registerStatic entry + which early-return fires.
	// Gate on the override target (hangar) to avoid log spam.
	const bool seamProbe = (getenv("MC2_MODOVERRIDE_TRACE") != nullptr)
		&& appearType && appearType->bldgRenderShape;
	if (seamProbe)
		fprintf(stderr, "[SEAMPROBE] bldg registerStatic ENTER name=%s registered=%d hasShape=%d enabled=%d eligible=%d renderShape=%p\n",
			appearType->name, (int)staticReg.registered, (int)(bldgShape!=NULL),
			(int)GpuStaticPropRegistry::isEnabled(), (int)isStaticEligible(),
			(void*)appearType->bldgRenderShape), fflush(stderr);
	if (staticReg.registered) {
		return;
	}
	if (!bldgShape) {
		return;
	}
	if (!GpuStaticPropRegistry::isEnabled()) {
		return;
	}
	if (!isStaticEligible()) {
		if (seamProbe) fprintf(stderr, "[SEAMPROBE] bldg registerStatic ABORT name=%s reason=!isStaticEligible\n", appearType->name), fflush(stderr);
		return;
	}

	// MODEL-OVERRIDE: register the RENDER shape geometry into s_typeIndex HERE,
	// in the mission-load pre-pass (runs before StaticProp::finalizeGeometry).
	// An override type-shape otherwise first registers late in per-instance
	// init() -> buildRecipeFromShape() below misses s_typeIndex -> aborts to the
	// CPU first-render fallback, which is catastrophic with ~1k tree instances
	// (per-instance pool copies overflow). Idempotent for stock (already
	// registered). getBldgRenderShape == stock when no override is present.
	const bool _regIsOverride = (appearType->bldgRenderShape != nullptr);
	for (int i = 0; i < MAX_LODS; ++i)
		GpuStaticPropBatcher::instance().registerMultiShape(appearType->getBldgRenderShape(i), _regIsOverride);

	// G5 RENDER-PATH OBSERVABILITY (descriptive; env-gated; ZERO behavior change):
	// one line per registered static-prop type recording which render path it takes.
	// path = override_multidraw (cooked/override geometry, layer-0 route) vs legacy_static.
	// Full MeshCapability bits come once the engine reads the cooked manifest.json (later
	// integration); for now we log what the seam knows. Schema-grep: \[RENDER_PATH v[0-9]+\].
	if (getenv("MC2_RENDER_PATH")) {
		fprintf(stderr, "[RENDER_PATH v1] key=staticprop:%s isOverride=%d path=%s pools=%s\n",
		        appearType->name, _regIsOverride ? 1 : 0,
		        _regIsOverride ? "override_multidraw" : "legacy_static",
		        gos_StaticPropLegacyInstancePools() ? "legacy" : "skipped");
		fflush(stderr);
	}

	if (getenv("MC2_MODOVERRIDE_TRACE") && appearType->bldgRenderShape) {
		TG_TypeMultiShape* rs = appearType->bldgRenderShape;
		fprintf(stderr, "[MODOVERRIDE_TRACE] bldg registerStatic name=%s renderShape=%p numShapes=%ld leaf0=%p leaf1=%p finalized_guess(typeNodes_above)\n",
		        appearType->name, (void*)rs, rs->GetNumShapes(),
		        rs->GetNumShapes() > 0 ? (void*)rs->GetTypeNode(0) : nullptr,
		        rs->GetNumShapes() > 1 ? (void*)rs->GetTypeNode(1) : nullptr);
		fflush(stderr);
	}

	// Compute transform — same coordinate convention as BldgAppearance::update().
	// At mission-load time position.z may not yet hold terrain elevation (set by
	// bldng.cpp:810 on first update), so use getTerrainElevation() directly.
	float yaw = rotation * DEGREES_TO_RADS;
	Stuff::UnitQuaternion rot;
	rot = Stuff::EulerAngles(0.0f, yaw, 0.0f);
	Stuff::Point3D xlatPosition;
	xlatPosition.x = -position.x;
	xlatPosition.y = TerrainRuntime::groundElevation(position);
	xlatPosition.z = position.y;
	bldgShape->TransformMultiShape_BuildRecipe(&xlatPosition, &rot);

	// TERRAIN-DECAL-SLICE-0A — CLIFF_WALL face frame (mesh-decal system).
	// Default-OFF gate MC2_TERRAIN_DECAL. When ON and this appearance is the
	// cliff-wall decal type ("MarbleCliff"), build an EXPLICIT face-frame mat4
	// for the placed instance INSTEAD of the yaw-only recipe transform. This is
	// the CLIFF_WALL conform mode from docs/superpowers/specs/
	// 2026-07-03-terrain-mesh-decal-system-design.md: up = world vertical,
	// facing = outward (downslope) horizontal cliff normal, tangent = contour.
	// A small outward offset lifts the wall off the terrain face to avoid
	// z-fighting. Row-vector convention: world = shapeRow * M, so each 3x3 ROW
	// (X_Axis/Y_Axis/Z_Axis) holds the world direction of the mesh's local axis
	// (GetLocalForward reads (FORWARD_AXIS, *)). The generated wall GLB authors
	// width->local X, height->local Y, relief/face->local Z.
	//   Shape-world axes (from xlatPosition remap): Xsw = -worldX, Ysw = up,
	//   Zsw = worldY. Terrain normal (nx,ny,nz){world, nz up} maps to shape-world
	//   (-nx, nz, ny); its horizontal projection (-nx,0,ny) is the outward facing.
	static const bool s_terrainDecal =
	    (getenv("MC2_TERRAIN_DECAL") != nullptr && getenv("MC2_TERRAIN_DECAL")[0] != '0');
	// TERRAIN-DECAL-SLICE-0B/0C — placement knobs live in CliffDecalTuning (mutable,
	// seeded from the MC2_TERRAIN_DECAL_* env defaults so unset == byte-identical to
	// Slice-0A). The face-frame math is factored into CliffDecalTuning::buildCliffWall
	// Matrix(), shared by this registration path and the live ImGui tuning panel.
	bool useCliffWallFrame = false;
	Stuff::Matrix4D cliffWallXform;
	Stuff::Vector3D decalNAcc;  // captured for the live-update panel
	if (s_terrainDecal && appearType && appearType->name &&
	    _stricmp(appearType->name, "MarbleCliff") == 0) {
		// Sample the terrain normal at the placement site (average the 4
		// enclosing corners via small offsets for stability — the raw normal is
		// per-triangle/faceted). worldUnitsPerVertex ~ heightfield cell size.
		Stuff::Vector3D nAcc; nAcc.Zero();
		const float d = 32.0f; // ~half a heightfield cell for a 4-corner average
		const float dx[4] = { -d,  d, -d,  d };
		const float dy[4] = { -d, -d,  d,  d };
		Terrain* landPtr = land; // file-scope terrain pointer
		for (int c = 0; c < 4; ++c) {
			Stuff::Vector3D sp = position;
			sp.x += dx[c]; sp.y += dy[c];
			Stuff::Vector3D nc = landPtr ? landPtr->getTerrainNormal(sp)
			                             : Stuff::Vector3D(0.0f, 0.0f, 1.0f);
			nAcc.x += nc.x; nAcc.y += nc.y; nAcc.z += nc.z;
		}
		decalNAcc = nAcc;
		// Build the initial CLIFF_WALL face frame from the current (env-seeded) knobs.
		CliffDecalTuning::buildCliffWallMatrix(
		    xlatPosition, nAcc, CliffDecalTuning::knobs(), cliffWallXform);
		useCliffWallFrame = true;
		if (getenv("MC2_TERRAIN_DECAL_TRACE")) {
			const CliffDecalTuning::Knobs& k = CliffDecalTuning::knobs();
			fprintf(stderr, "[TERRAIN_DECAL v1] CLIFF_WALL name=%s pos=(%.1f,%.1f) "
			        "scale=%.3f offset=%.1f lateral=%.1f lift=%.1f yaw=%.1f\n",
			        appearType->name, position.x, position.y,
			        k.scale, k.offset, k.lateral, k.lift, k.yawDeg),
			    fflush(stderr);
		}
	}

	// Build per-leaf recipe batch.
	// Use public GetNumShapes()/GetShapeRec() — numTG_Shapes/listOfShapes are protected.
	std::vector<GpuStaticPropInstance> batch;
	const int numShapes = static_cast<int>(bldgShape->GetNumShapes());
	batch.reserve(numShapes);
	// 2026-05-10 diag: count outcomes for buildings to see why so few reach count>1.
	int diag_total = 0, diag_skip_processMe = 0, diag_skip_helper = 0,
	    diag_skip_unreg = 0, diag_added = 0;
	for (int i = 0; i < numShapes; ++i) {
		++diag_total;
		const TG_ShapeRec* rec = bldgShape->GetShapeRec(i);
		if (!rec || !rec->processMe || !rec->node) { ++diag_skip_processMe; continue; }
		TG_Shape* child = rec->node;
		// 2026-05-10 fix: skip non-SHAPE_NODE children (helpers, spotlight
		// emitters, animation roots) — mirrors GpuStaticPropBatcher::submitMultiShape
		// at gos_static_prop_batcher.cpp:2047. Without this, the loop hits a
		// helper, buildRecipeFromShape's static_cast<TG_TypeShape*>(myType)
		// produces a pointer that isn't in s_typeIndex, returns false, and
		// the entire building registration aborts on its FIRST helper. This
		// is why mc2_10 buildings (warehouses, S_admin, control) previously
		// failed registerStatic and never reached the substrate.
		if (!child->IsShapeNode()) { ++diag_skip_helper; continue; }
		uint32_t flags = 0;
		if (child->GetLightsOut())   flags |= (1u << 0);
		if (child->GetIsWindow())    flags |= (1u << 1);
		if (child->GetIsSpotlight()) flags |= (1u << 2);
		// rec->shapeToWorld is LinearMatrix4D; convert to Matrix4D for buildRecipeFromShape().
		Stuff::Matrix4D xform(rec->shapeToWorld);
		// TERRAIN-DECAL-SLICE-0A: replace the yaw-only leaf transform with the
		// explicit CLIFF_WALL face frame when the decal gate armed this type.
		// Uniform scale only (Slice 0) — the wall GLB carries its own world-scale
		// geometry, so the face frame is a pure rotation+translation (no scale).
		if (useCliffWallFrame) {
			xform = cliffWallXform;
			// TERRAIN-DECAL-FILL-1: tag this decal leaf so static_prop.frag applies
			// the shadow-side ambient/fill floor (u_terrainDecalFill) ONLY to the
			// cliff decal — faces pointing away from the sun otherwise render as a
			// black void (static-prop lighting is max(N·L,0) with no ambient floor).
			// Bit 3 mirrors kFlagDecalFill in static_prop.frag; no other static prop
			// ever sets it, so all other props stay byte-identical.
			flags |= (1u << 3);
		}
		GpuStaticPropInstance inst;
		if (!GpuStaticPropBatcher::instance().buildRecipeFromShape(
				child, xform,
				static_cast<uint32_t>(child->GetARGBHighlight()),
				static_cast<uint32_t>(child->GetFogRGB()),
				flags, &inst)) {
			++diag_skip_unreg;
			if (seamProbe)
				fprintf(stderr, "[SEAMPROBE] bldg buildRecipe MISS name=%s leaf=%d child=%p node=%s (override type not in s_typeIndex) -> ABORT\n",
					appearType->name, i, (void*)child, child->getNodeName()), fflush(stderr);
			return;  // unregistered type — abort; first-render fallback covers it
		}
		if (seamProbe)
			fprintf(stderr, "[SEAMPROBE] bldg buildRecipe HIT name=%s leaf=%d child=%p node=%s typeID=%u\n",
				appearType->name, i, (void*)child, child->getNodeName(), inst.typeID), fflush(stderr);
		// EDITOR-STATIC-RECIPE-FROM-TYPE-1: the recipe is built from the INSTANCE
		// bldgShape (a CreateFrom copy, bdactor.cpp:1440). For a building placed at
		// runtime in the editor, that copy's leaf child->myType registers AFTER the
		// one-shot finalizeGeometry() latch -> it gets a typeID in s_typeIndex but
		// its geometry is NOT in the immutable VBO (batcher_getTypeDrawInfo
		// indexCount==0) -> the draw replays an empty type -> invisible body (shadow
		// still draws from the correct worldCenter record). Re-point the recipe at the
		// canonical TYPE render-shape leaf (getBldgRenderShape, primed by
		// primeAllBuildingAppearanceTypes BEFORE finalize, so it carries real VBO
		// geometry); the instance still supplies transform/light/flags. Editor-only:
		// in-game the instance leaves register pre-finalize, so this is a no-op. Guard:
		// only re-point when the canonical leaf resolves to a typeID that actually has
		// finalized geometry, else leave inst.typeID untouched.
		{
			extern bool InEditor;   // mech3d.cpp
			if (InEditor && appearType) {
				TG_TypeMultiShape* typeRS = appearType->getBldgRenderShape(0);
				if (typeRS && i < typeRS->GetNumShapes()) {
					TG_TypeNodePtr typeLeaf = typeRS->GetTypeNode(i);
					uint32_t canonId = 0, idxc = 0, fi = 0, ic = 0; int32_t bv = 0;
					const bool gotId = typeLeaf && batcher_typeIdForTypeShape((const void*)typeLeaf, &canonId);
					const bool gotGeo = gotId && batcher_getTypeDrawInfo(canonId, &idxc, &fi, &bv, &ic);
					if (gotGeo && idxc > 0) {
						inst.typeID = canonId;
					}
				}
			}
		}
		batch.push_back(inst);
		++diag_added;
	}
	// 2026-05-10 diag: env-gated per-building outcome. MC2_BLDG_REG_TRACE=1.
	{
		static const bool s_trace = (getenv("MC2_BLDG_REG_TRACE") != nullptr);
		static int s_loggedCount = 0;
		if (s_trace && s_loggedCount < 80) {
			++s_loggedCount;
			fprintf(stderr,
				"[BLDG_REG_DIAG v1] appearType=%s numShapes=%d total=%d processMe_skip=%d "
				"helper_skip=%d unreg_skip=%d added=%d\n",
				(appearType ? appearType->name : "<null>"),
				numShapes, diag_total, diag_skip_processMe, diag_skip_helper,
				diag_skip_unreg, diag_added);
			fflush(stderr);
		}
		(void)diag_total; (void)diag_skip_processMe; (void)diag_skip_helper;
		(void)diag_skip_unreg; (void)diag_added;
	}
	if (batch.empty()) {
		return;
	}

	int32_t regIdx = -1;
	(void)GameAdapters::StaticProp::syncStaticProp(
		bldgShape, batch.data(), batch.size(), &regIdx);
	if (seamProbe)
		fprintf(stderr, "[SEAMPROBE] bldg syncStaticProp name=%s batchSize=%zu regIdx=%d firstTypeID=%u\n",
			appearType->name, batch.size(), (int)regIdx,
			batch.empty()?0xFFFFFFFFu:batch[0].typeID), fflush(stderr);
	if (regIdx >= 0) {
		staticReg.registered  = true;
		staticReg.shape       = bldgShape;
		staticReg.recipeIndex = regIdx;
		// TERRAIN-DECAL-SLICE-0C: capture this decal's live-update context so the
		// "Cliff Decal" ImGui panel can recompute + re-upload its transform when a
		// slider moves. Only for the CLIFF_WALL decal (useCliffWallFrame); other
		// buildings never register a live decal target.
		if (useCliffWallFrame) {
			CliffDecalTuning::captureDecalContext(regIdx, xlatPosition, decalNAcc);
		}
		// SHADOW-STATIC-BUILDINGS-2: tag this recipe Building so the world-fixed
		// static shadow map replays it (visibility-independent; not per-frame buckets).
		GpuStaticPropRegistry::setRecipePopulation(regIdx, GpuStaticPropPopulation::Building);
		// H4 fix (2026-05-06): registerStatic only ran TransformMultiShape_BuildRecipe
		// (positions only); leaf TG_Shape::lightData_ is still default/zero. Without
		// this flag, IsStaticNow() returns true on the very next frame, UPDATE_SKIP
		// fires, touch() re-submits the empty lightData_ via addLightDataStructure
		// → all-zero lighting slot → black actor. Setting needsFullBakeNextFrame
		// uses the existing late-reg recovery mechanism: IsStaticNow() returns
		// false until the next update() runs a full TransformMultiShape and clears
		// the flag (bdactor.cpp:2313). One-time cost of one extra update() per
		// mission-load-registered actor; recovers the UPDATE_SKIP perf win
		// every frame thereafter. Spec:
		// docs/superpowers/specs/2026-05-06-update-skip-touch-residual-debug-strategy.md
		needsFullBakeNextFrame = true;
		// [G1-STATIC-EAGER-LIGHT v1] Eager fallback: write a non-zero ambient slot so
		// lightData_[recipeIndex] is never pure-zero if this prop is never on screen.
		// CANNOT call CacheGpuLightData here -- worldLights[] are not populated until
		// the first update() (SetLightList requires a live eye/camera). Instead we seed
		// a minimal ambient record (numLights_=1, dim neutral color) so GPU-Scene cull
		// can upload this slot without blacking the prop. The first real update() calls
		// mc2CacheOrBakeStaticGpuLight -> MISS path -> CacheGpuLightData + mc2WriteStaticLightSlot
		// which overwrites this fallback with the terrain-correct light. Gate default-OFF;
		// unset = byte-identical to existing behavior.
		{
			extern void mc2SetBakedStaticLight(int32_t, const TG_HWLightsData&);
			extern void mc2WriteStaticLightSlot(int32_t, const TG_HWLightsData&);
			static const bool s_eagerLightBake = (getenv("MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE") != nullptr);
			if (s_eagerLightBake) {
				TG_HWLightsData fallback;
				// Minimal ambient: one dim neutral-white light. Overwritten on first
				// in-view update() by the terrain-correct bake (MISS path). Non-zero
				// numLights_ is the sentinel the zero-probe checks.
				fallback.numLights_ = 1;
				fallback.lightColor[0][0] = 0.3f;
				fallback.lightColor[0][1] = 0.3f;
				fallback.lightColor[0][2] = 0.3f;
				fallback.lightColor[0][3] = 1.0f;
				mc2SetBakedStaticLight(regIdx, fallback);
				mc2WriteStaticLightSlot(regIdx, fallback);
			}
	}
}
}

bool BldgAppearance::isStaticRegistered() const { return staticReg.registered; }

int32_t BldgAppearance::getStaticRecipeIndex() const {
    return staticReg.registered ? staticReg.recipeIndex : -1;
}

// STATIC-REG-PREWARM-QUEUE-1: mission-load off-screen light bake.
// Called after finalizeGeometry() + eye init, before the first update() frame.
// Purpose: drain the H4 latch (needsFullBakeNextFrame=true) that registerStatic()
// arms, using the permanent SSBO path (mc2CacheOrBakeStaticGpuLight → MISS →
// mc2WriteStaticLightSlot). Off-screen props that never reach render() will
// have a valid cached light slot and will pass stableLightSkipEligible from
// frame 1 onward, eliminating the rej_no_static_reg bucket on mc2_24.
// The approach mirrors the existing gpuEligible non-full-bake branch in update():
//   SetLightList → TransformMultiShape_HierarchyOnly → mc2CacheOrBakeStaticGpuLight
// We re-use the static mc2CacheOrBakeStaticGpuLight() declared above in this TU.
bool BldgAppearance::prewarmStaticLightBake(Camera* cam)
{
	// Gate: only bake objects that are registered and have the latch set.
	if (!staticReg.registered || staticReg.recipeIndex < 0)
		return false;
	if (!needsFullBakeNextFrame)
		return false;
	if (!bldgShape)
		return false;
	if (!cam || cam->getNumLights() == 0)
		return false;

	// Populate shapeToWorld hierarchy (no full pool bake — mirrors gpuEligible branch).
	bldgShape->SetLightList(cam->getWorldLights(), cam->getNumLights());

	// Build xlatPosition/rot from the appearance world position/rotation.
	// Same coordinate transform as BldgAppearance::update (line ~3281):
	Stuff::Point3D xlatPosition;
	xlatPosition.x = -position.x;
	xlatPosition.y =  position.z;
	xlatPosition.z =  position.y;

	float yaw = rotation * DEGREES_TO_RADS;
	Stuff::UnitQuaternion rot;
	rot = Stuff::EulerAngles(0.0f, yaw, 0.0f);

	// Hierarchy-only walk: fills shapeToWorld matrices without touching TGL pools.
	// Safe to call from mission load: no GL calls, no frame-pool alloc.
	bldgShape->TransformMultiShape_HierarchyOnly(&xlatPosition, &rot);

	// Bake light into permanent SSBO slot (MISS path: CacheGpuLightData + mc2WriteStaticLightSlot).
	mc2CacheOrBakeStaticGpuLight(bldgShape, staticReg.registered, staticReg.recipeIndex);
	staticReg.lightDataIndex = bldgShape->getCachedGpuLightIndex();

	if (staticReg.lightDataIndex != 0xFFFFFFFFu) {
		staticReg.hasValidStaticLight = true;
		needsFullBakeNextFrame = false;
		return true;  // baked successfully
	}
	return false;  // gpu objects not enabled or CacheGpuLightData early-returned
}

bool BldgAppearance::IsStaticNow() const
{
	return staticReg.registered
		&& staticReg.shape == bldgShape
		&& !needsFullBakeNextFrame
		&& isStaticEligible();
}

void BldgAppearance::touch()
{
	// FRAME-JOBS-2F: if touchSerialCommit() already ran this frame (split path active),
	// skip redundant Path B work. When MC2_FRAME_JOBS_TOUCH=0, stamp is never set
	// (0xFFFFFFFFu initial value cannot match any real frame counter).
	if (mc2FrameJobsTouchDiagEnabled()) {
		touchEntryDiagRollFrame(g_mc2FrameCounter);
		if (touchSerialCommitFrame == g_mc2FrameCounter) {
			++s_touchEntryDiag.legacy_skipped;
			return;
		}
		++s_touchEntryDiag.legacy_ran_nosplit;
	} else {
		if (touchSerialCommitFrame == g_mc2FrameCounter)
			return;
	}

	// BLDG-TYPE-ANIM-GATE-FIX-1: count touch() calls for newly-eligible pop:
	// type has animation data, instance is idle (bdAnimationState==-1), and
	// currently registered (staticReg.registered). Nonzero delta proves fix works.
	if (bldgTypeHasAnimations(appearType) && bdAnimationState == -1 && staticReg.registered)
		s_animTypeIdleNowStatic.fetch_add(1, std::memory_order_relaxed);
	// MC2_STATIC_UPDATE_SKIP defaults TRUE (terrobj.cpp:92); touch() is the
	// DEFAULT path. update() runs only when the env var is explicitly cleared.
	if (bldgShape) {
		const bool stableLightSkipArmed = mc2StableLightSkipEnabled();
		const uint64_t currentLightEnvGen = mc2StaticLightEnvironmentGeneration(eye);
		const bool stableLightSkipEligible =
			staticReg.registered &&
			staticReg.recipeIndex >= 0 &&
			staticReg.hasValidStaticLight &&
			staticReg.lightDataIndex != 0xFFFFFFFFu &&
			staticReg.lastLightEnvGen == currentLightEnvGen &&
			!needsFullBakeNextFrame;
		recordStableLightSkipDiag(
			stableLightSkipArmed,
			stableLightSkipEligible,
			!staticReg.registered || staticReg.recipeIndex < 0,
			!staticReg.hasValidStaticLight,
			staticReg.hasValidStaticLight && (staticReg.lastLightEnvGen != currentLightEnvGen),
			needsFullBakeNextFrame,
			staticReg.lightDataIndex == 0xFFFFFFFFu);

		// STABLE-LIGHT-SKIP-BROADEN-1: broaden the LBSS resubmit-skip that
		// touchSerialCommit() takes (bdactor.cpp ~4490) to THIS legacy path. In
		// stock config touch() is the live path (FRAME-JOBS touch split default-OFF),
		// so touchSerialCommit()'s skip never fires and every stable static building
		// pays a redundant ResubmitCachedGpuLightData() per frame. When
		// stableLightSkipEligible the cached lightDataIndex from a prior frame is
		// still valid on the GPU (baked static prefix [0..s_staticLightHighWater)
		// persists frame-to-frame under MC2_LIGHTBAKE — same invariant proof as the
		// serial-commit skip below), so that resubmit is pure redundant recompute.
		// SAFER-THAN-SERIAL-VARIANT: we still call bldgShape->Touch() (one field
		// write advancing lastTurnTransformed) before returning — the non-split
		// path bundles Touch() with the resubmit, whereas the split path runs Touch()
		// in touchWorkerPrepass(). Skipping only the expensive resubmit keeps the
		// legacy CPU-render staleness guard (tgl.cpp:3000) byte-identical. staticReg
		// fields are already correct from the last full update()/resubmit. Gate:
		// mc2StableLightSkipTouchEnabled() (default OFF => resubmit still runs).
		if (mc2StableLightSkipTouchEnabled() && mc2LightbridgeStableSkipEnabled()
		    && stableLightSkipArmed && stableLightSkipEligible) {
			lbssRecordBldg(/*eligible=*/true, /*skipTaken=*/true,
			               /*blk_idx=*/false, /*blk_gen=*/false, /*blk_noelig=*/false);
			bldgShape->Touch();
			return;
		}

		// [LIGHTBRIDGE v1] C6 retirement: repoint to the primed 38d8720 slot
		// (zero FNV/memcmp; cachedFrame_ stamped). MISS keeps the legacy
		// resubmit (NOT CacheGpuLightData -- terrain-color-staleness,
		// msl.cpp:1874-1887). MC2_LIGHTBAKE=0 -> legacy path bit-for-bit.
		extern bool mc2LightBakeEnabled();
		extern bool mc2IsBakedStaticLightPresent(int32_t);
		// THREAD-SAFETY CLASSIFICATION: EmitBakedGpuLightData — WORKER_SAFE.
		// (1) Writes only per-instance members cachedGpuLightIndex_ and cachedFrame_
		//     on the TG_MultiShape pointed to by bldgShape (unique per BldgAppearance
		//     instance, never aliased across actors). (2) Makes NO GL calls
		//     (glBufferSubData, glMapBuffer, etc.). (3) The 'baked' struct is
		//     explicitly discarded ((void)baked in msl.cpp:2114) — the slot index IS
		//     recipeIndex, which is per-recipe-constant. Two calls on different shape
		//     instances cannot conflict. ResubmitCachedGpuLightData (the else branch)
		//     also writes only per-instance members on the same hot path; no shared
		//     pool write occurs unless the repoint fast-path misses, in which case it
		//     calls addLightDataStructure (now mutex-protected by s_lightDataMapMu).
		// [LIGHTBRIDGE-BAKED-PROBE-1] probe only — EmitBakedGpuLightData discards baked
		if (mc2LightBakeEnabled()
		    && staticReg.registered && staticReg.recipeIndex >= 0
		    && mc2IsBakedStaticLightPresent(staticReg.recipeIndex)) {
			TG_HWLightsData baked{};
			bldgShape->EmitBakedGpuLightData(staticReg.recipeIndex, baked);
		} else {
			bldgShape->ResubmitCachedGpuLightData();
		}
		// 2026-05-11 per-instance capture: snapshot the just-resubmitted slot
		// for THIS actor before sibling instances of the same multi-type
		// overwrite multi->cachedGpuLightIndex_ in the same update phase.
		staticReg.lightDataIndex = bldgShape->getCachedGpuLightIndex();
		staticReg.hasValidStaticLight = (staticReg.lightDataIndex != 0xFFFFFFFFu);
		staticReg.lastLightEnvGen = currentLightEnvGen;
		bldgShape->Touch();
	}
}

// FRAME-JOBS-2D: lock-free per-instance prep; runs on worker threads.
// bldgShape->Touch() is lock-free (verified: sets lastTurnTransformed only, tgl.cpp:4073).
// s_animTypeIdleNowStatic.fetch_add is atomic — worker-safe.
void BldgAppearance::touchWorkerPrepass()
{
	if (bldgTypeHasAnimations(appearType) && bdAnimationState == -1 && staticReg.registered)
		s_animTypeIdleNowStatic.fetch_add(1, std::memory_order_relaxed);
	if (bldgShape)
		bldgShape->Touch();
}

// FRAME-JOBS-2D: light-data resubmit; runs serially on main thread after worker join.
void BldgAppearance::touchSerialCommit()
{
	// FRAME-JOBS-2F: stamp this frame unconditionally so touch() (Path B, terrain object
	// loop) returns immediately. Set BEFORE any early returns including stable-skip:
	// if stable-skip fires and we return early, the stamp still prevents Path B from
	// re-doing work that stable-skip correctly decided to omit.
	touchSerialCommitFrame = g_mc2FrameCounter;
	if (mc2FrameJobsTouchDiagEnabled()) {
		touchEntryDiagRollFrame(g_mc2FrameCounter);
		++s_touchEntryDiag.serial_commit_hits;
	}

	if (!bldgShape) {
		if (s_proxyReconEnabled) {
			g_spr_phase2Calls.fetch_add(1, std::memory_order_relaxed);
			g_spr_bldgCalls.fetch_add(1, std::memory_order_relaxed);
			g_spr_rejNoShape.fetch_add(1, std::memory_order_relaxed);
			g_spr_callCounter.fetch_add(1, std::memory_order_relaxed);
		}
		return;
	}
	const bool stableLightSkipArmed = mc2StableLightSkipEnabled();
	const uint64_t currentLightEnvGen = mc2StaticLightEnvironmentGeneration(eye);
	const bool stableLightSkipEligible =
		staticReg.registered &&
		staticReg.recipeIndex >= 0 &&
		staticReg.hasValidStaticLight &&
		staticReg.lightDataIndex != 0xFFFFFFFFu &&
		staticReg.lastLightEnvGen == currentLightEnvGen &&
		!needsFullBakeNextFrame;
	recordStableLightSkipDiag(
		stableLightSkipArmed,
		stableLightSkipEligible,
		!staticReg.registered || staticReg.recipeIndex < 0,
		!staticReg.hasValidStaticLight,
		staticReg.hasValidStaticLight && (staticReg.lastLightEnvGen != currentLightEnvGen),
		needsFullBakeNextFrame,
		staticReg.lightDataIndex == 0xFFFFFFFFu);

	// STATIC-SCENE-PROXY-RECON-1: classify this object as proxy candidate or not.
	// Must run BEFORE the LBSS early return below (most objects skip via LBSS on mc2_24).
	// Mirrors stableLightSkipEligible criteria exactly (stableLightSkipEligible is already
	// computed above). No behavior change.
	// Note: falling state (OBJECT_FLAG_FALLING) lives on the Building game object,
	// not BldgAppearance — not accessible here without an owner pointer. Omitted;
	// falling buildings will be counted as rej_no_static_reg (IsStaticNow calls
	// invalidateStaticRegistration on fall, clearing staticReg.registered).
	if (s_proxyReconEnabled) {
		g_spr_phase2Calls.fetch_add(1, std::memory_order_relaxed);
		g_spr_bldgCalls.fetch_add(1, std::memory_order_relaxed);

		// Mirror stableLightSkipEligible exactly:
		bool isProxy = stableLightSkipEligible;
		if (!isProxy) {
			// Attribute the first failing condition (same priority as stableLightSkipEligible)
			if (!staticReg.registered || staticReg.recipeIndex < 0) {
				g_spr_rejNoStaticReg.fetch_add(1, std::memory_order_relaxed);

				// STATIC-REGISTRY-COVERAGE-RECON-1: sub-classify the rej_no_static_reg bucket.
				// Runs only when MC2_STATIC_REG_COVERAGE=1 to avoid overhead on normal runs.
				if (s_regCovEnabled) {
					extern bool mc2LightBakeEnabled();
					extern bool mc2IsBakedStaticLightPresent(int32_t);
					if (!staticReg.registered) {
						g_rc_neverRegistered.fetch_add(1, std::memory_order_relaxed);
						// isStaticEligible() for the never-registered set: tells us whether
						// the building COULD have been registered but wasn't (eligible=true),
						// or is inherently ineligible (animated, spinning, etc.).
						if (isStaticEligible())
							g_rc_isStaticEligibleYes.fetch_add(1, std::memory_order_relaxed);
						else
							g_rc_isStaticEligibleNo.fetch_add(1, std::memory_order_relaxed);
						// Track shape name for the top-N breakdown.
						if (appearType && appearType->name) {
							std::lock_guard<std::mutex> lk(g_rc_shapeMu);
							g_rc_shapeNames[appearType->name]++;
						}
					} else {
						// registered=true but recipeIndex<0 — should not normally occur
						// (registration sets registered = (recipeIndex >= 0)).
						g_rc_noRecipe.fetch_add(1, std::memory_order_relaxed);
					}
					// Also probe the bake gate and bake-table presence, independent of
					// registered flag, to reveal whether bake infrastructure is live at all.
					if (!mc2LightBakeEnabled()) {
						g_rc_bakeNotEnabled.fetch_add(1, std::memory_order_relaxed);
					} else if (staticReg.registered && staticReg.recipeIndex >= 0
					           && !mc2IsBakedStaticLightPresent(staticReg.recipeIndex)) {
						g_rc_notInBakeTable.fetch_add(1, std::memory_order_relaxed);
					}
				}
			} else if (!staticReg.hasValidStaticLight)
				g_spr_rejNoValidLight.fetch_add(1, std::memory_order_relaxed);
			else if (staticReg.lightDataIndex == 0xFFFFFFFFu)
				g_spr_rejBadLightIdx.fetch_add(1, std::memory_order_relaxed);
			else if (staticReg.lastLightEnvGen != currentLightEnvGen)
				g_spr_rejLightGenMismatch.fetch_add(1, std::memory_order_relaxed);
			else if (needsFullBakeNextFrame)
				g_spr_rejNeedsFullBake.fetch_add(1, std::memory_order_relaxed);
		}
		if (isProxy) g_spr_proxyCandidate.fetch_add(1, std::memory_order_relaxed);

		const int64_t n = g_spr_callCounter.fetch_add(1, std::memory_order_relaxed);
		if (n > 0 && (n % 15000) == 0) {
			const int64_t ph2   = g_spr_phase2Calls.exchange(0, std::memory_order_relaxed);
			const int64_t bldg  = g_spr_bldgCalls.exchange(0, std::memory_order_relaxed);
			const int64_t tree  = g_spr_treeCalls.exchange(0, std::memory_order_relaxed);
			const int64_t proxy = g_spr_proxyCandidate.exchange(0, std::memory_order_relaxed);
			const int64_t rNS   = g_spr_rejNoStaticReg.exchange(0, std::memory_order_relaxed);
			const int64_t rBL   = g_spr_rejBadLightIdx.exchange(0, std::memory_order_relaxed);
			const int64_t rNVL  = g_spr_rejNoValidLight.exchange(0, std::memory_order_relaxed);
			const int64_t rLG   = g_spr_rejLightGenMismatch.exchange(0, std::memory_order_relaxed);
			const int64_t rNFB  = g_spr_rejNeedsFullBake.exchange(0, std::memory_order_relaxed);
			const int64_t rNSh  = g_spr_rejNoShape.exchange(0, std::memory_order_relaxed);
			printf("STATIC_PROXY_RECON: ph2_calls=%lld bldg=%lld tree=%lld"
			       " proxy_candidate=%lld(%.1f%%)"
			       " rej_no_static_reg=%lld rej_bad_light_idx=%lld"
			       " rej_no_valid_light=%lld rej_light_gen_mismatch=%lld"
			       " rej_needs_full_bake=%lld rej_no_shape=%lld\n",
			       (long long)ph2, (long long)bldg, (long long)tree,
			       (long long)proxy,
			       ph2 > 0 ? (proxy * 100.0 / ph2) : 0.0,
			       (long long)rNS, (long long)rBL,
			       (long long)rNVL, (long long)rLG,
			       (long long)rNFB, (long long)rNSh);

			// STATIC-REGISTRY-COVERAGE-RECON-1: print sub-breakdown when gate is on.
			if (s_regCovEnabled) {
				const int64_t rcNR  = g_rc_neverRegistered.exchange(0, std::memory_order_relaxed);
				const int64_t rcNRp = g_rc_noRecipe.exchange(0, std::memory_order_relaxed);
				const int64_t rcBN  = g_rc_bakeNotEnabled.exchange(0, std::memory_order_relaxed);
				const int64_t rcNBT = g_rc_notInBakeTable.exchange(0, std::memory_order_relaxed);
				const int64_t rcEY  = g_rc_isStaticEligibleYes.exchange(0, std::memory_order_relaxed);
				const int64_t rcEN  = g_rc_isStaticEligibleNo.exchange(0, std::memory_order_relaxed);
				printf("STATIC_REG_COVERAGE: total_rej_no_static_reg=%lld"
				       " sub: never_registered=%lld no_recipe=%lld"
				       " bake_not_enabled=%lld not_in_bake_table=%lld"
				       " is_static_eligible_yes=%lld is_static_eligible_no=%lld\n",
				       (long long)rNS,
				       (long long)rcNR, (long long)rcNRp,
				       (long long)rcBN, (long long)rcNBT,
				       (long long)rcEY, (long long)rcEN);
				// Top-20 shape names from never_registered bucket.
				{
					std::lock_guard<std::mutex> lk(g_rc_shapeMu);
					if (!g_rc_shapeNames.empty()) {
						// Copy to a vector for top-N sort.
						std::vector<std::pair<int,std::string>> sorted;
						sorted.reserve(g_rc_shapeNames.size());
						for (auto& kv : g_rc_shapeNames)
							sorted.push_back({kv.second, kv.first});
						std::sort(sorted.begin(), sorted.end(),
						          [](const std::pair<int,std::string>& a, const std::pair<int,std::string>& b){
						              return a.first > b.first;
						          });
						printf("STATIC_REG_COVERAGE: top_shapes(name:count):");
						int shown = 0;
						for (auto& p : sorted) {
							if (shown >= 20) break;
							printf(" %s:%d", p.second.c_str(), p.first);
							++shown;
						}
						printf("\n");
						g_rc_shapeNames.clear();
					}
				}
			}
			fflush(stdout);
		}
	}

	// [LIGHTBRIDGE-STABLE-SKIP-WIRE-1]: skip Section C (EmitBakedGpuLightData +
	// getCachedGpuLightIndex + staticReg writes) when MC2_LIGHTBRIDGE_STABLE_SKIP=1
	// and this object's baked light slot is known valid for this frame.
	//
	// INVARIANT proof (verified 2026-06-20):
	//   Q1: resetLightData() rebases lightDataStructuresCount to s_staticLightHighWater
	//       (not 0) when MC2_LIGHTBAKE is on. lightData_[0..s_staticLightHighWater) is
	//       NEVER memset'd or cleared — it persists intact frame-to-frame.
	//   Q2: EmitBakedGpuLightData(recipeIndex, ...) sets cachedGpuLightIndex_ =
	//       static_cast<uint32_t>(recipeIndex) — i.e. the permanent static prefix slot.
	//   Q3: staticReg.lightDataIndex = getCachedGpuLightIndex() = recipeIndex, which
	//       is an index into the static prefix [0..s_staticLightHighWater).
	//   Q4: renderLists() uploads lightData_[0..max(lightDataStructuresCount,64)] which
	//       always covers [0..s_staticLightHighWater) since count >= S after rebase.
	//       With MC2_STATIC_LIGHT_UPLOAD_SPLIT the prefix is uploaded once-per-dirty;
	//       suffix is uploaded every frame. Either path keeps [0..S) in GPU memory.
	// Therefore: staticReg.lightDataIndex from a prior frame remains valid on the GPU
	// for stable objects — we can skip resubmit entirely when stableLightSkipEligible.
	// DO NOT remove the else-branch fallback — it handles any frame where light state
	// changed, needsFullBakeNextFrame, or slot was invalidated.
	if (mc2LightbridgeStableSkipEnabled() && stableLightSkipArmed) {
		// FRAME-JOBS-2F Fix-A: stableLightSkipEligible already requires
		// (lightDataIndex != 0xFFFFFFFF) and (lastLightEnvGen == currentLightEnvGen),
		// so blk_idx and blk_gen were always false — removed (always-zero, misleading).
		// skipTaken == stableLightSkipEligible by definition now.
		const bool skipTaken  = stableLightSkipEligible;
		const bool blk_noelig = !stableLightSkipEligible;
		lbssRecordBldg(stableLightSkipEligible, skipTaken, /*blk_idx=*/false, /*blk_gen=*/false, blk_noelig);
		if (skipTaken)
			return;
	}

	extern bool mc2LightBakeEnabled();
	extern bool mc2IsBakedStaticLightPresent(int32_t);
	// [LIGHTBRIDGE-BAKED-PROBE-1] probe only — EmitBakedGpuLightData discards baked
	if (mc2LightBakeEnabled()
	    && staticReg.registered && staticReg.recipeIndex >= 0
	    && mc2IsBakedStaticLightPresent(staticReg.recipeIndex)) {
		TG_HWLightsData baked{};
		bldgShape->EmitBakedGpuLightData(staticReg.recipeIndex, baked);
	} else {
		bldgShape->ResubmitCachedGpuLightData();
	}
	staticReg.lightDataIndex = bldgShape->getCachedGpuLightIndex();
	staticReg.hasValidStaticLight = (staticReg.lightDataIndex != 0xFFFFFFFFu);
	staticReg.lastLightEnvGen = currentLightEnvGen;
	// NOTE: bldgShape->Touch() is in touchWorkerPrepass() (verified lock-free)
}

void BldgAppearance::invalidateStaticRegistration()
{
	if (staticReg.registered && staticReg.recipeIndex >= 0)
		GameAdapters::StaticProp::destroyStaticPropByIndex(staticReg.recipeIndex);
	staticReg = {};
}

//-----------------------------------------------------------------------------
void BldgAppearance::destroy (void)
{
	// Stage 3.D: NULL the registry's RecipeRange::multi pointer before
	// bldgShape is freed below. Mirrors TreeAppearance::destroy ordering.
	invalidateStaticRegistration();

	if ( bldgShape )
	{
		delete bldgShape;
		bldgShape = NULL;
	}

	if (destructFX)
	{
		destructFX->Kill();
		delete destructFX;
		destructFX = NULL;
	}
	
	//Turn the lights off!
	//Need to kill the light source here too!
	if (pointLight)
	{
		if (eye)
			eye->removeWorldLight(lightId,pointLight);

		free(pointLight);
		pointLight = NULL;
	}

	// (E) T1.5: paired cleanup for SpotLight_-child illumination from T1.4.
	// IMPORTANT (C-r2 M5): bldgShape was deleted above; do NOT call
	// getNodeIdPosition or any bldgShape method here. Use CACHED state
	// (spotlightLights_/spotlightSlotIds_/spotlightNodeIds_) only. Unlike
	// `pointLight` (alloc/free per night/day boundary), spotlightLights_
	// stay allocated for the building's lifetime — this destroy hook is the
	// only cleanup site.
	for (size_t k = 0; k < spotlightLights_.size(); ++k)
	{
		if (eye)
			eye->removeWorldLight(spotlightSlotIds_[k], spotlightLights_[k]);
		// T1.16 — pair untag with removeWorldLight before free().
		mc2_spotlight_diag::untag_slot(static_cast<long>(spotlightSlotIds_[k]));
		free(spotlightLights_[k]);
	}
	spotlightLights_.clear();
	spotlightSlotIds_.clear();
	spotlightNodeIds_.clear();
	spotlightsRegistered_ = false;

	if (activity)
	{
		activity->Kill();
		delete activity;
		activity = NULL;
	}

	if (activity1)
	{
		activity1->Kill();
		delete activity1;
		activity1 = NULL;
	}

	appearanceTypeList->removeAppearance(appearType);
}

#define HEIGHT_THRESHOLD 10.0f

//-----------------------------------------------------------------------------

long BldgAppearance::calcCellsCovered (Stuff::Vector3D& pos, short* cellList) {

	gosASSERT((Terrain::realVerticesMapSide * MAPCELL_DIM) == GameMap->width);
	long numCoords = 0;
	long maxCoords = cellList[0];

	//MUST force building to HIGHEST LOD!!!  IMpassability data is only valid at this LOD!!
	// Building will reset its LOD on next draw!!
	if (currentLOD)
	{
		currentLOD = 0;
	
		bldgShape->ClearAnimation();
		delete bldgShape;
		bldgShape = NULL;
	
		bldgShape = appearType->bldgShape[currentLOD]->CreateFrom();
		buildingPbrRenderActive = false;
		if (bdAnimationState != -1)
			appearType->setAnimation(bldgShape,bdAnimationState);
	}

	//-------------------------------------------------------------
	// New way.  For each vertex in each shape, translate to world
	for (int i=0;i<bldgShape->GetNumShapes();i++)
	{
		//Check if the artists meant for this piece to NOT block passability!!
		if (S_strnicmp(bldgShape->GetNodeId(i),"_PAB",4) != 0)
		{
			for (int j=0;j<bldgShape->GetNumVerticesInShape(i);j++) 
			{
				Stuff::Vector3D vertexPos, worldPos;
				vertexPos = bldgShape->GetShapeVertexInEditor(i,j,-rotation);
				worldPos.Add(pos,vertexPos);
	
				bool recordCell = false;
				if (appearType->isForestClump)
					recordCell = (vertexPos.z <= 1.0f);
				else
					recordCell = (vertexPos.z >= 1.0f);
				if (recordCell) 
				{
					int cellR, cellC;
					land->worldToCell(worldPos,cellR,cellC);
					if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR)
						|| (0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
					{
						//gosASSERT(false);
						continue;
					}
	//				if (GameMap->inBounds(cellR, cellC)) {
						//-------------------
						// Record the cell...
						if (numCoords > (maxCoords - 2))
							Fatal(numCoords, "BldgAppearance.markMoveMap: too many coords for cellList ");
							
						cellList[numCoords++] = (short)cellR;
						cellList[numCoords++] = (short)cellC;
	//				}
				}
			}
		}
	}
	
	return(numCoords);
}

//-----------------------------------------------------------------------------

void BldgAppearance::markTerrain (_ScenarioMapCellInfo* pInfo, int type, int counter)
{
	if (appearType->spinMe)			//We are a marker
		return;						//Do not mark impassable
		
	//MUST force building to HIGHEST LOD!!!  IMpassability data is only valid at this LOD!!
	// Building will reset its LOD on next draw!!
	if (currentLOD)
	{
		currentLOD = 0;
	
		bldgShape->ClearAnimation();
		delete bldgShape;
		bldgShape = NULL;
	
		bldgShape = appearType->bldgShape[currentLOD]->CreateFrom();
		buildingPbrRenderActive = false;
		if (bdAnimationState != -1)
			appearType->setAnimation(bldgShape,bdAnimationState);
	}

	int cellR, cellC;
	land->worldToCell(position, cellR, cellC);
	if (appearType->isForestClump)
	{
		//-------------------------------------------------------------
		// New way.  For each vertex in each shape, translate to world
		for (int i=0;i<bldgShape->GetNumShapes();i++)
		{
			//Check if the artists meant for this piece to NOT block passability!!
			if (S_strnicmp(bldgShape->GetNodeId(i),"_PAB",4) != 0)
			{
				for (int j=0;j<bldgShape->GetNumVerticesInShape(i);j++)
				{
					Stuff::Vector3D vertexPos, worldPos;
					vertexPos = bldgShape->GetShapeVertexInEditor(i,j,-rotation);
					worldPos.Add(position,vertexPos);
		
					int cellR, cellC;
					land->worldToCell(worldPos,cellR,cellC);
					if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
						(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
					{
						continue;
					}
					
					_ScenarioMapCellInfo*pTmp = &(pInfo[cellR * Terrain::realVerticesMapSide * MAPCELL_DIM + cellC]);
	
					if (vertexPos.z <= 1.0f)
					{
						pTmp->passable = true;
						pTmp->gate = false;
						pTmp->forest = true;
						//pTmp->specialType = type;
						//pTmp->specialID = counter;
					}
					
					float cellLocalHeight = vertexPos.z * metersPerWorldUnit * 0.25f;
					if (cellLocalHeight > 15.0f)
						cellLocalHeight = 15.0f;
						
					//ONLY mark LOS on cells that are impassable with forests.  Maybe everything?
					if (pTmp->passable && (pTmp->lineOfSight < cellLocalHeight))
						pTmp->lineOfSight = cellLocalHeight+0.5f;
				}
			}
		}
	}
	else
	{
		if ((type == SPECIAL_GATE) || (type == SPECIAL_WALL))
		{
			if (appearType->bldgShape[0])
			{
				bldgShape->ClearAnimation();
				delete bldgShape;
				bldgShape = NULL;
					
				bldgShape = appearType->bldgShape[0]->CreateFrom();
				buildingPbrRenderActive = false;
				if (bdAnimationState != -1)
					appearType->setAnimation(bldgShape,bdAnimationState);
			}
		}

		if (type == SPECIAL_LAND_BRIDGE)
		{
			if (appearType->bldgDmgShape)
			{
				bldgShape->ClearAnimation();
				delete bldgShape;
				bldgShape = NULL;
					
				bldgShape = appearType->bldgDmgShape->CreateFrom();
				buildingPbrRenderActive = false;
				if (bdAnimationState != -1)
					appearType->setAnimation(bldgShape,bdAnimationState);
			}
		}

		//-------------------------------------------------------------
		// New way.  For each vertex in each shape, translate to world
		for (int i=0;i<bldgShape->GetNumShapes();i++)
		{
			//Check if the artists meant for this piece to NOT block passability!!
			if (S_strnicmp(bldgShape->GetNodeId(i),"_PAB",4) != 0)
			{
				for (int j=0;j<bldgShape->GetNumVerticesInShape(i);j++)
				{
					Stuff::Vector3D vertexPos, worldPos;
					vertexPos = bldgShape->GetShapeVertexInEditor(i,j,-rotation);
					worldPos.Add(position,vertexPos);
		
					int cellR, cellC;
					land->worldToCell(worldPos,cellR,cellC);
					if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
						(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
					{
						continue;
					}
					_ScenarioMapCellInfo*pTmp = &(pInfo[cellR * Terrain::realVerticesMapSide * MAPCELL_DIM + cellC]);
	
					if (vertexPos.z >= 1.0f)
					{
						pTmp->passable = false;
						
						if (((type == SPECIAL_GATE) || (type == SPECIAL_WALL)))
						{
							pTmp->passable = true;
							pTmp->specialID = counter;
							pTmp->specialType = type;
							if (type == SPECIAL_GATE)
								pTmp->gate = true;
						}
						else if (type == SPECIAL_LAND_BRIDGE)
						{
							pTmp->passable = true;
							pTmp->specialID = counter;
							pTmp->specialType = type;
						}
						else if (type == 18)
						{
							pTmp->specialID = 0;
							pTmp->specialType = SPECIAL_NONE;
							pTmp->passable = true;
						}
						else
						{
							pTmp->specialID = 0;
							pTmp->specialType = SPECIAL_NONE;
						}
							
						if (type != 18)
						{
							float cellLocalHeight = vertexPos.z * metersPerWorldUnit * 0.25f;
							if (cellLocalHeight > 15.0f)
								cellLocalHeight = 15.0f;
								
							if (pTmp->lineOfSight < cellLocalHeight)
								pTmp->lineOfSight = cellLocalHeight+0.5f;
						}
					}
				}
			}
		}
		
 		//Switch to destroyed state to mark impassable.  The destroyed impassability will NEVER change!!
		// When a gate opens or a wall or gate is destroyed, we only want to mark stuff that is going 
		// away passable and long range capable.
		if ((type == SPECIAL_GATE) || (type == SPECIAL_WALL))
		{
			if (appearType->bldgDmgShape)
			{
				bldgShape->ClearAnimation();
				delete bldgShape;
				bldgShape = NULL;
					
				bldgShape = appearType->bldgDmgShape->CreateFrom();
				buildingPbrRenderActive = false;
				if (bdAnimationState != -1)
					appearType->setAnimation(bldgShape,bdAnimationState);
			}
		}

		if (type == SPECIAL_LAND_BRIDGE)
		{
			if (appearType->bldgShape[0])
			{
				bldgShape->ClearAnimation();
				delete bldgShape;
				bldgShape = NULL;
					
				bldgShape = appearType->bldgShape[0]->CreateFrom();
				buildingPbrRenderActive = false;
				if (bdAnimationState != -1)
					appearType->setAnimation(bldgShape,bdAnimationState);
			}
		}

			
		//-------------------------------------------------------------
		// New way.  For each vertex in each shape, translate to world
		for (int i=0;i<bldgShape->GetNumShapes();i++)
		{
			//Check if the artists meant for this piece to NOT block passability!!
			if (S_strnicmp(bldgShape->GetNodeId(i),"_PAB",4) != 0)
			{
				for (int j=0;j<bldgShape->GetNumVerticesInShape(i);j++)
				{
					Stuff::Vector3D vertexPos, worldPos;
					vertexPos = bldgShape->GetShapeVertexInEditor(i,j,-rotation);
					worldPos.Add(position,vertexPos);
		
					int cellR, cellC;
					land->worldToCell(worldPos,cellR,cellC);
					if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
						(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
					{
						continue;
					}
					_ScenarioMapCellInfo*pTmp = &(pInfo[cellR * Terrain::realVerticesMapSide * MAPCELL_DIM + cellC]);
	
					if (vertexPos.z >= 1.0f)
					{
						if (type == 18)
						{
							pTmp->passable = true;
							pTmp->specialID = 0;
							pTmp->specialType = SPECIAL_NONE;
						}
						else
						{
							pTmp->passable = false;
							pTmp->gate = false;			//Perfectly OK to mark these again,  They are no longer special!!
							pTmp->specialID = 0;
							pTmp->specialType = SPECIAL_NONE;
						}
					}
				}
			}
		}
			
		if ((status != OBJECT_STATUS_DESTROYED) && appearType->bldgShape[0])
		{
			bldgShape->ClearAnimation();
			delete bldgShape;
			bldgShape = NULL;
						
			bldgShape = appearType->bldgShape[0]->CreateFrom();
			buildingPbrRenderActive = false;
			if (bdAnimationState != -1)
				appearType->setAnimation(bldgShape,bdAnimationState);
		}
		else if ((status == OBJECT_STATUS_DESTROYED) && appearType->bldgDmgShape)
		{
			bldgShape->ClearAnimation();
			delete bldgShape;
			bldgShape = NULL;
					
			bldgShape = appearType->bldgDmgShape->CreateFrom();
			buildingPbrRenderActive = false;
			if (bdAnimationState != -1)
				appearType->setAnimation(bldgShape,bdAnimationState);
		}
	}
}

//-----------------------------------------------------------------------------

long BldgAppearance::markMoveMap (bool passable, long* lineOfSightRect, bool useHeight, short* cellList)
{
	int minRow = 9999;
	int maxRow = 0;
	int minCol = 9999;
	int maxCol = 0;

	//MUST force building to HIGHEST LOD!!!  IMpassability data is only valid at this LOD!!
	// Building will reset its LOD on next draw!!
	TG_MultiShapePtr tempBldgShape = bldgShape;

	if (currentLOD)
	{
		tempBldgShape = appearType->bldgShape[currentLOD]->CreateFrom();
		if (bdAnimationState != -1)
			appearType->setAnimation(tempBldgShape,bdAnimationState);
	}

	int numCoords = 0;
	if (cellList) {
		gosASSERT(!useHeight);
		//----------------------------------------------------------------------------------
		// Store the max number of coords allowed in the first cell. Can overwrite it now...
		int maxCoords = cellList[0];
		//-------------------------------------------------------------
		// New way.  For each vertex in each shape, translate to world
		for (int i = 0; i < tempBldgShape->GetNumShapes(); i++) 
		{
			//Check if the artists meant for this piece to NOT block passability!!
			if (S_strnicmp(tempBldgShape->GetNodeId(i),"_PAB",4) != 0)
			{
				for (int j=0;j<tempBldgShape->GetNumVerticesInShape(i);j++) 
				{
					Stuff::Vector3D vertexPos, worldPos;
					vertexPos = tempBldgShape->GetShapeVertexInWorld(i,j,-rotation);
					worldPos.Add(position,vertexPos);
	
					int cellR, cellC;
					land->worldToCell(worldPos,cellR,cellC);
					if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
						(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
					{
						continue;
					}
					
					//----------------------------
					// Building lineOfSightRect...
					if (cellR < minRow)
						minRow = cellR;
					if (cellR > maxRow)
						maxRow = cellR;
					if (cellC < minCol)
						minCol = cellC;
					if (cellC > maxCol)
						maxCol = cellC;
						
					//-------------------
					// Record the cell...
					if (numCoords > (maxCoords - 2))
						Fatal(numCoords, "BldgAppearance.markMoveMap: too many coords for cellList ");
					cellList[numCoords++] = (short)cellR;
					cellList[numCoords++] = (short)cellC;
				}
			}
		}
	}
	else 
	{
		//-------------------------------------------------------------
		// New way.  For each vertex in each shape, translate to world
		for (int i=0;i<tempBldgShape->GetNumShapes();i++)
		{
			//Check if the artists meant for this piece to NOT block passability!!
			if (S_strnicmp(tempBldgShape->GetNodeId(i),"_PAB",4) != 0)
			{
				for (int j=0;j<tempBldgShape->GetNumVerticesInShape(i);j++)
				{
					Stuff::Vector3D vertexPos, worldPos;
					vertexPos = tempBldgShape->GetShapeVertexInWorld(i,j,-rotation);
					worldPos.Add(position,vertexPos);
	
					int cellR, cellC;
					land->worldToCell(worldPos,cellR,cellC);
					if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
						(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
					{
						continue;
					}
					
					//----------------------------
					// Building lineOfSightRect...
					if (cellR < minRow)
						minRow = cellR;
					if (cellR > maxRow)
						maxRow = cellR;
					if (cellC < minCol)
						minCol = cellC;
					if (cellC > maxCol)
						maxCol = cellC;
						
					//----------------
					// Mark the map...
					MapCellPtr curCell = GameMap->getCell(cellR, cellC);
					if (appearType->isForestClump) {
						if (vertexPos.z <= 1.0f)
							curCell->setPassable(passable);
						}
					else {
						if (vertexPos.z >= 1.0f)
							curCell->setPassable(passable);
					}
				}
			}
		}
	}
	
	if (lineOfSightRect) {
		lineOfSightRect[0] = minRow;
		lineOfSightRect[1] = minCol;
		lineOfSightRect[2] = maxRow;
		lineOfSightRect[3] = maxCol;
	}

	if (tempBldgShape != bldgShape)
	{
		tempBldgShape->ClearAnimation();
		delete tempBldgShape;
		tempBldgShape = NULL;
	}

	return(numCoords/2);
}

//-----------------------------------------------------------------------------

void BldgAppearance::markLOS (bool clearIt)
{
	//MUST force building to HIGHEST LOD!!!  IMpassability data is only valid at this LOD!!
	// Building will reset its LOD on next draw!!
	TG_MultiShapePtr tempBldgShape = bldgShape;
	if (currentLOD)
	{
		tempBldgShape = appearType->bldgShape[0]->CreateFrom();
		if (bdAnimationState != -1)
			appearType->setAnimation(tempBldgShape,bdAnimationState);
	}

	//-------------------------------------------------------------
	// New way.  For each vertex in each shape, translate to world
	for (int i=0;i<tempBldgShape->GetNumShapes();i++)
	{
		//Check if the artists meant for this piece to NOT block LOS!!
		// Probably should check for light cones,too!
		
		if ((S_strnicmp(tempBldgShape->GetNodeId(i),"LOS_",4) != 0) &&
			(S_strnicmp(tempBldgShape->GetNodeId(i),"SpotLight_",10) != 0))
		{
			for (int j=0;j<tempBldgShape->GetNumVerticesInShape(i);j++)
			{
				Stuff::Vector3D vertexPos, worldPos;
				vertexPos = tempBldgShape->GetShapeVertexInEditor(i,j,-rotation);
//				vertexPos = tempBldgShape->GetShapeVertexInWorld(i,j,-rotation);
				worldPos.Add(position,vertexPos);
	
				int cellR, cellC;
				land->worldToCell(worldPos,cellR,cellC);
				if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
					(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
				{
					continue;
				}
				
				//----------------
				// Mark the map...
				MapCellPtr curCell = GameMap->getCell(cellR, cellC);

				if (!clearIt)
				{
					float currentCellHeight = curCell->getLocalHeight();
					
					float cellLocalHeight = vertexPos.z * metersPerWorldUnit * 0.25f;
					if (cellLocalHeight > 15.0f)
						cellLocalHeight = 15.0f;

					if (cellLocalHeight > currentCellHeight)
						curCell->setLocalHeight(cellLocalHeight+0.5f);
				}
				else	//We want to clear all LOS height INFO.  We're about to change shape!!
				{
					curCell->setLocalHeight(0.0f);
				}
			}
		}
	}

	if (tempBldgShape != bldgShape)
	{
		tempBldgShape->ClearAnimation();
		delete tempBldgShape;
		tempBldgShape = NULL;
	}
}

//-----------------------------------------------------------------------------

void BldgAppearance::calcAdjCell (long& row, long& col)
{
	//MUST force building to HIGHEST LOD!!!  IMpassability data is only valid at this LOD!!
	// Building will reset its LOD on next draw!!
	if (currentLOD)
	{
		currentLOD = 0;
	
		bldgShape->ClearAnimation();
		delete bldgShape;
		bldgShape = NULL;
	
		bldgShape = appearType->bldgShape[currentLOD]->CreateFrom();
		buildingPbrRenderActive = false;
		if (bdAnimationState != -1)
			appearType->setAnimation(bldgShape,bdAnimationState);
	}

	//-------------------------------------------------------------
	// New way.  For each vertex in each shape, translate to world
	int numVert = 0;
	for (int i=0;i<bldgShape->GetNumShapes();i++)
	{
		for (int j=0;j<bldgShape->GetNumVerticesInShape(i);j++)
		{
			Stuff::Vector3D vertexPos, worldPos;
			vertexPos = bldgShape->GetShapeVertexInWorld(i,j,-rotation);
			worldPos.Add(position,vertexPos);

			{
				numVert++;
				int cellR, cellC;
				land->worldToCell(worldPos,cellR,cellC);
				
				//MapCellPtr curCell = GameMap->getCell(cellR, cellC);
				//curCell->setPassable(passable);	
			}
		}
	}
}

//-----------------------------------------------------------------------------
// class TreeAppearanceType
void TreeAppearanceType::init (const char * fileName)
{
	AppearanceType::init(fileName);

	FullPathFileName iniName;
	iniName.init(tglPath,fileName,".ini");

	FitIniFile iniFile;
	
	long result = iniFile.open(iniName);
	if (result != NO_ERR)
		Fatal(result,"Could not find building appearance INI file");

	// ASSIMP-TREE-IMPORT-1 — optional GLB probe for trees. Mirror of
	// mech3d.cpp [Import] pattern. LOD0 only; LOD1+ and damage stay ASE.
	char importSourceBase[256] = "";
	if (iniFile.seekBlock("Import") == NO_ERR &&
	    iniFile.readIdString("Source", importSourceBase, 255) == NO_ERR &&
	    importSourceBase[0])
	{
		char* dot = strrchr(importSourceBase, '.');
		if (dot) *dot = '\0';
	}

	result = iniFile.seekBlock("TGLData");
	if (result != NO_ERR)
		Fatal(result,"Could not find block in building appearance INI file");

	result = iniFile.readIdBoolean("ForestClump",isForestClump);
	if (result != NO_ERR)
		isForestClump = false;

 	char aseFileName[512];
	// MODEL-OVERRIDE dual-shape: capture the BASE (LOD0) asset name before the
	// damage block below reuses aseFileName.
	char treeBaseName[512];
	treeBaseName[0] = 0;
	result = iniFile.readIdString("FileName",aseFileName,511);
	if (result != NO_ERR)
	{
		//Check for LOD filenames instead
		for (long i=0;i<MAX_LODS;i++)
		{
			char baseName[256];
			char baseLODDist[256];
			sprintf(baseName,"FileName%d",i);
			sprintf(baseLODDist,"Distance%d",i);

			result = iniFile.readIdString(baseName,aseFileName,511);
			if (result == NO_ERR)
			{
				result = iniFile.readIdFloat(baseLODDist,lodDistance[i]);
				if (result != NO_ERR)
					STOP(("LOD %d has no distance value in file %s",i,fileName));
				// Push out LOD-swap thresholds so high-detail meshes stay visible
				// at greater zoom-out. See visual_preference_knobs.md.
				lodDistance[i] *= 5.0f;

				//----------------------------------------------
				// Base LOD shape.  In stand Pose by default.
				treeShape[i] = new TG_TypeMultiShape;
				gosASSERT(treeShape[i] != NULL);

				if (i == 0 && importSourceBase[0]) {
					treeShape[i]->LoadFromFile(importSourceBase); // ASSIMP-TREE-IMPORT-1: opt-in GLB probe
				} else {
					FullPathFileName treeName;
					treeName.init(tglPath,aseFileName,".ase");
					treeShape[i]->LoadTGMultiShapeFromASE(treeName);
				}

				//---------------------------------------------------------
				// Should only be necessary for trees.  Easy to data drive
				treeShape[i]->SetAlphaTest(true);
				treeShape[i]->SetFilter(true);

				if (!i)
					strncpy(treeBaseName, aseFileName, sizeof(treeBaseName) - 1);
			}
			else if (!i)
			{
				STOP(("No base LOD for shape %s",fileName));
			}
		}
	}
	else
	{
		//----------------------------------------------
		// Base shape.  In stand Pose by default.
		treeShape[0] = new TG_TypeMultiShape;
		gosASSERT(treeShape[0] != NULL);

		if (importSourceBase[0]) {
			treeShape[0]->LoadFromFile(importSourceBase); // ASSIMP-TREE-IMPORT-1: opt-in GLB probe
		} else {
			FullPathFileName treeName;
			treeName.init(tglPath,aseFileName,".ase");
			treeShape[0]->LoadTGMultiShapeFromASE(treeName);
		}

		//---------------------------------------------------------
		// Should only be necessary for trees.  Easy to data drive
		treeShape[0]->SetAlphaTest(true);
		treeShape[0]->SetFilter(true);

		strncpy(treeBaseName, aseFileName, sizeof(treeBaseName) - 1);
	}

	// MODEL-OVERRIDE dual-shape: render-only override for trees. The stock
	// treeShape[] load above is UNCHANGED (collision authority). On registry
	// hit, load replacement geometry into the separate render shape only.
	treeBaseName[sizeof(treeBaseName) - 1] = 0;
	if (treeBaseName[0])
	{
		// Env-gated discovery trace (see staticProp site). MODEL-OVERRIDE Slice 3.
		if (getenv("MC2_MODOVERRIDE_TRACE"))
			fprintf(stderr, "[MODOVERRIDE_TRACE] tree '%s'\n", treeBaseName);
		const ModelOverrideRecord* ov =
			ModelOverrideRegistry::instance().resolve("tree", treeBaseName);
		if (ov)
		{
#ifdef ENABLE_ASSIMP_IMPORTER
			// TREE-OVERRIDE-LOD-MVP Task 1: the existing import fills LOD0 only.
			// Higher LODs stay NULL until Task 3 populates them; LOD0-only ⇒
			// getTreeRenderShape(any) → LOD0 → identical to pre-LOD behavior.
			treeRenderShape[0] = new TG_TypeMultiShape;
			char overridePath[1024];
			snprintf(overridePath, sizeof(overridePath), "%s/%s",
			         ov->manifestDir.c_str(),
			         ov->sourceRelPath.c_str());
			// Guard the importer: Assimp may throw (DeadlyImportError). A throw
			// here would leak the freshly-new'd render shape and unwind into
			// non-exception-safe engine code, so collapse any throw to stock.
			try
			{
				long r = ImportGeometryFromFile(overridePath, treeRenderShape[0]);
				if (r != 0 || treeRenderShape[0]->GetNumShapes() == 0)
				{
					delete treeRenderShape[0]; treeRenderShape[0] = NULL;   // stock fallback
					treeRenderShapeLodCount = 0;
					fprintf(stderr, "[MODOVERRIDE] tree '%s': import failed (%s), using stock render\n",
					        treeBaseName, overridePath);
					fflush(stderr);
				}
				else
				{
					// Mirror the stock tree flag setup (treeShape[i]->SetAlphaTest(true)
					// / SetFilter(true) at the stock LOD-load sites above, ~3464/3490)
					// so the override renders with identical alpha-test/filter state.
					// Intentional duplication — keep in sync with the stock sites.
					treeRenderShape[0]->SetAlphaTest(true);
					treeRenderShape[0]->SetFilter(true);
					// MODEL-OVERRIDE texture binding: resolve the importer-assigned
					// texture names to GOS handles on the TYPE render shape so the
					// GPU batcher draws with the override's OWN textures.
					LoadOverrideRenderShapeTextures(treeRenderShape[0]);
					// TREE-OVERRIDE-LOD-MVP Task 1: LOD0 populated.
					treeRenderShapeLodCount = 1;
					fprintf(stderr, "[MODOVERRIDE] tree '%s': render override applied (%s)\n",
					        treeBaseName, overridePath);
					fflush(stderr);

					// TREE-OVERRIDE-LOD-MVP Task 3: load each manifest lods[] entry
					// into treeRenderShape[lod]. Each LOD gets its OWN import,
					// vertex-tight mesh-local bounds (ImportGeometryFromFile sets
					// that), and its textures resolved (decimated LODs share the
					// LOD0 texture set — same material names). A failed/odd-index
					// LOD is dropped (stock-clamp via getTreeRenderShape) without
					// disturbing LOD0. treeRenderShapeLodCount becomes the highest
					// populated index + 1 so the per-LOD register loop (M-C) and
					// the K×M light gate (Task 3 GATE) see both LODs.
					for (const ModelOverrideLod& lentry : ov->lods)
					{
						const int li = lentry.lod;
						if (li <= 0 || li >= MAX_LODS) {
							fprintf(stderr, "[MODOVERRIDE] tree '%s': lod %d out of range, skipped\n",
							        treeBaseName, li);
							fflush(stderr);
							continue;
						}
						if (treeRenderShape[li]) {
							fprintf(stderr, "[MODOVERRIDE] tree '%s': duplicate lod %d, skipped\n",
							        treeBaseName, li);
							fflush(stderr);
							continue;
						}
						char lodPath[1024];
						snprintf(lodPath, sizeof(lodPath), "%s/%s",
						         ov->manifestDir.c_str(),
						         lentry.sourceRelPath.c_str());
						treeRenderShape[li] = new TG_TypeMultiShape;
						try {
							long lr = ImportGeometryFromFile(lodPath, treeRenderShape[li]);
							if (lr != 0 || treeRenderShape[li]->GetNumShapes() == 0) {
								delete treeRenderShape[li]; treeRenderShape[li] = NULL;
								fprintf(stderr, "[MODOVERRIDE] tree '%s': lod %d import failed (%s), dropped\n",
								        treeBaseName, li, lodPath);
								fflush(stderr);
								continue;
							}
							treeRenderShape[li]->SetAlphaTest(true);
							treeRenderShape[li]->SetFilter(true);
							LoadOverrideRenderShapeTextures(treeRenderShape[li]);
							if (li + 1 > treeRenderShapeLodCount)
								treeRenderShapeLodCount = li + 1;
							fprintf(stderr, "[MODOVERRIDE] tree '%s': lod %d applied (%s) lodCount=%ld\n",
							        treeBaseName, li, lodPath, treeRenderShapeLodCount);
							fflush(stderr);
						}
						catch (...) {
							delete treeRenderShape[li]; treeRenderShape[li] = NULL;
							fprintf(stderr, "[MODOVERRIDE] tree '%s': lod %d import threw (%s), dropped\n",
							        treeBaseName, li, lodPath);
							fflush(stderr);
						}
					}
				}
			}
			catch (...)
			{
				delete treeRenderShape[0]; treeRenderShape[0] = NULL;   // stock fallback
				treeRenderShapeLodCount = 0;
				fprintf(stderr, "[MODOVERRIDE] tree '%s': import threw (%s), using stock render\n",
				        treeBaseName, overridePath);
				fflush(stderr);
			}
#else
			fprintf(stderr, "[MODOVERRIDE] tree '%s': override resolved but importer disabled, using stock render\n",
			        treeBaseName);
			fflush(stderr);
#endif
		}
	}

	result = iniFile.seekBlock("TGLDamage");
	if (result == NO_ERR)
	{
		result = iniFile.readIdString("FileName",aseFileName,511);
		if (result != NO_ERR)
			Fatal(result,"Could not find ASE FileName in building appearance INI file");
	
		FullPathFileName dmgName;
		dmgName.init(tglPath,aseFileName,".ase");
	
		treeDmgShape = new TG_TypeMultiShape;
		gosASSERT(treeDmgShape != NULL);
		treeDmgShape->LoadTGMultiShapeFromASE(dmgName);

		if (!treeDmgShape->GetNumShapes())
		{
			delete treeDmgShape;
			treeDmgShape = NULL;
		}
		
	}
	else
	{
		treeDmgShape = NULL;
	}

 	//No Animations at present.
}

//----------------------------------------------------------------------------
void TreeAppearanceType::destroy (void)
{
	AppearanceType::destroy();

	for (long i=0;i<MAX_LODS;i++)
	{
		if (treeShape[i])
		{
			delete treeShape[i];
			treeShape[i] = NULL;
		}
	}

	// MODEL-OVERRIDE dual-shape: free every populated render override LOD.
	// TREE-OVERRIDE-LOD-MVP Task 1: per-LOD chain.
	for (long i = 0; i < MAX_LODS; i++)
	{
		if (treeRenderShape[i])
		{
			delete treeRenderShape[i];
			treeRenderShape[i] = NULL;
		}
	}
	treeRenderShapeLodCount = 0;

	if (treeDmgShape)
	{
		delete treeDmgShape;
		treeDmgShape = NULL;
	}
	
}

//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// class TreeAppearance
void TreeAppearance::init (AppearanceTypePtr tree, GameObjectPtr obj)
{
	Appearance::init(tree,obj);
	appearType = (TreeAppearanceType *)tree;

	shapeMin.x = shapeMin.y = -25;
	shapeMax.x = shapeMax.y = 50;
	
	paintScheme = -1;
	objectNameId = 30862;
	
	hazeFactor = 0.0f;

	screenPos.x = screenPos.y = screenPos.z = screenPos.w = -999.0f;
	position.Zero();
	rotation = 0.0;;
	selected = 0;
	teamId = 0;
	homeTeamRelationship = 0;
	actualRotation = rotation;

	OBBRadius = -1.0f;

	currentLOD = 0;
	
	beenInView = false;
	
	fogLightSet = false;
	lightRGB = fogRGB = 0xffffffff;

    // sebi: init so will not be garbage
    status = OBJECT_STATUS_NORMAL;
    forceLightsOut = false;
    // Slice 2 (object-offload) substrate flag; set true by GPU batcher on late
    // registration to force a full TransformMultiShape next frame.
    needsFullBakeNextFrame = false;
    // TREE-OVERRIDE-LOD-MVP Task 2: zero-init per-LOD static registration.
    for (long _l = 0; _l < MAX_LODS; ++_l)
        staticReg[_l] = StaticRegistration{};
    activeLOD = 0;  // pinned 0 until Task 5 adds distance selection
    treeShape = NULL;
    //

	if (appearType)
	{
		// MODEL-OVERRIDE dual-shape: build the per-instance RENDER shape from the
		// render accessor (override if present, else stock). Collision (markTerrain/
		// markLOS) rebuilds this member from stock treeShape[lod] when needed.
		treeShape = appearType->getTreeRenderShape(0)->CreateFrom();

		//-------------------------------------------------
		// Load the texture and store its handle.
		for (long i=0;i<treeShape->GetNumTextures();i++)
		{
			char txmName[1024];
			treeShape->GetTextureName(i,txmName,256);
	
			char texturePath[1024];
			sprintf(texturePath,"%s%d" PATH_SEPARATOR,tglPath,ObjectTextureSize);
	
			FullPathFileName textureName;
			textureName.init(texturePath,txmName,"");
	
			if (textureOrKtxSidecarExists(textureName))
			{
				if (S_strnicmp(txmName,"a_",2) == 0)
				{
					DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
					gosASSERT(gosTextureHandle != 0xffffffff);
					treeShape->SetTextureHandle(i,gosTextureHandle);
					treeShape->SetTextureAlpha(i,true);
				}
				else
				{
					DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
					gosASSERT(gosTextureHandle != 0xffffffff);
					treeShape->SetTextureHandle(i,gosTextureHandle);
					treeShape->SetTextureAlpha(i,false);
				}
			}
			else
			{
				//PAUSE(("Warning: %s texture name not found",textureName));
				treeShape->SetTextureHandle(i,0xffffffff);
			}
		}
		
		Stuff::Vector3D boxCoords[8];
		Stuff::Vector3D nodeCenter = treeShape->GetRootNodeCenter();

		boxCoords[0].x = position.x + treeShape->GetMinBox().x + nodeCenter.x;
		boxCoords[0].y = position.y + treeShape->GetMinBox().z + nodeCenter.z;
		boxCoords[0].z = position.z + treeShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[1].x = position.x + treeShape->GetMinBox().x + nodeCenter.x;
		boxCoords[1].y = position.y + treeShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[1].z = position.z + treeShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[2].x = position.x + treeShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[2].y = position.y + treeShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[2].z = position.z + treeShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[3].x = position.x + treeShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[3].y = position.y + treeShape->GetMinBox().z + nodeCenter.z;
		boxCoords[3].z = position.z + treeShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[4].x = position.x + treeShape->GetMinBox().x + nodeCenter.x;
		boxCoords[4].y = position.y + treeShape->GetMinBox().z + nodeCenter.z;
		boxCoords[4].z = position.z + treeShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[5].x = position.x + treeShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[5].y = position.y + treeShape->GetMinBox().z + nodeCenter.z;
		boxCoords[5].z = position.z + treeShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[6].x = position.x + treeShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[6].y = position.y + treeShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[6].z = position.z + treeShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[7].x = position.x + treeShape->GetMinBox().x + nodeCenter.x;
		boxCoords[7].y = position.y + treeShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[7].z = position.z + treeShape->GetMinBox().y + nodeCenter.y;
		
		float testRadius = 0.0;
		
		for (int i=0;i<8;i++)
		{
			testRadius = boxCoords[i].GetLength();
			if (OBBRadius < testRadius)
				OBBRadius = testRadius;
		}

		
		appearType->boundsUpperLeftX = (-OBBRadius * 2.0);
		appearType->boundsUpperLeftY = (-OBBRadius * 2.0);
		   					 
		appearType->boundsLowerRightX = (OBBRadius * 2.0);
		appearType->boundsLowerRightY = (OBBRadius);
		
		if (!appearType->getDesignerTypeBounds())
		{
			appearType->typeUpperLeft = treeShape->GetMinBox();
			appearType->typeLowerRight = treeShape->GetMaxBox();
		}

		// GPU static-prop batcher: register the RENDER shape (override-or-stock)
		// so override geometry actually rasterizes (see BldgAppearance reg site
		// for the s_typeIndex rationale). Damage stays stock (out of MVP).
		// TREE-OVERRIDE-LOD-MVP M-C: for override types iterate ONLY populated
		// LODs (treeRenderShapeLodCount) so we register each distinct override
		// LOD shape once — NOT MAX_LODS times (which would upload LOD0 3× since
		// getTreeRenderShape clamps unpopulated indices back to LOD0). Stock
		// types keep the full MAX_LODS loop: getTreeRenderShape(i) returns the
		// distinct stock treeShape[i] per LOD, all of which must register.
		const bool _treeIsOverride = (appearType->treeRenderShapeLodCount > 0);
		const int _treeRegLods = _treeIsOverride ? (int)appearType->treeRenderShapeLodCount : MAX_LODS;
		for (int i = 0; i < _treeRegLods; ++i)
			GpuStaticPropBatcher::instance().registerMultiShape(appearType->getTreeRenderShape(i), _treeIsOverride);
		GpuStaticPropBatcher::instance().registerMultiShape(appearType->treeDmgShape);
	}

	pitch = yaw = 0.0f;
}

//-----------------------------------------------------------------------------
void TreeAppearance::setObjStatus (long oStatus)
{
	if (status != oStatus)
	{
		if ((oStatus == OBJECT_STATUS_DESTROYED) || (oStatus == OBJECT_STATUS_DISABLED))
		{
			if (appearType->treeDmgShape)
			{
				if (treeShape)
				{
					treeShape->ClearAnimation();
					delete treeShape;
					treeShape = NULL;
				}
				
				treeShape = appearType->treeDmgShape->CreateFrom();
				beenInView = false; 
			}
			
		}

		if (oStatus == OBJECT_STATUS_NORMAL)
		{
			if (appearType->treeShape[0])
			{
				if (treeShape)
				{
					treeShape->ClearAnimation();
					delete treeShape;
					treeShape = NULL;
				}

				// MODEL-OVERRIDE dual-shape: restore per-instance RENDER shape
				// via the render accessor (override if present, else stock).
				treeShape = appearType->getTreeRenderShape(0)->CreateFrom();
				beenInView = false;
			}

		}

		//-------------------------------------------------
		// Load the texture and store its handle.
		if (treeShape)
		{
			for (long i=0;i<treeShape->GetNumTextures();i++)
			{
				char txmName[1024];
				treeShape->GetTextureName(i,txmName,256);
		
				char texturePath[1024];
				sprintf(texturePath,"%s%d" PATH_SEPARATOR,tglPath,ObjectTextureSize);
		
				FullPathFileName textureName;
				textureName.init(texturePath,txmName,"");
		
				if (textureOrKtxSidecarExists(textureName))
				{
					if (S_strnicmp(txmName,"a_",2) == 0)
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						treeShape->SetTextureHandle(i,gosTextureHandle);
						treeShape->SetTextureAlpha(i,true);
					}
					else
					{
						DWORD gosTextureHandle = mcTextureManager->loadTexture(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						treeShape->SetTextureHandle(i,gosTextureHandle);
						treeShape->SetTextureAlpha(i,false);
					}
				}
				else
				{
					//PAUSE(("Warning: %s texture name not found",textureName));
					treeShape->SetTextureHandle(i,0xffffffff);
				}
			}
		}

	}

	status = oStatus;
}

//-----------------------------------------------------------------------------
void TreeAppearance::setObjectParameters (const Stuff::Vector3D &pos, float Rot, long sel, long team, long homeRelations)
{
	rotation = Rot;

	position = pos;

	selected = sel;

	actualRotation = Rot;

	teamId = team;
	homeTeamRelationship = homeRelations;
}

//-----------------------------------------------------------------------------
void TreeAppearance::setMoverParameters (float pitchAngle, float lArmRot, float rArmRot, bool isAirborne)
{
	pitch = pitchAngle;
}

//-----------------------------------------------------------------------------
bool TreeAppearance::isMouseOver (float px, float py)
{
	if (inView)
	{
		if ((px <= lowerRight.x) && (py <= lowerRight.y) &&
			(px >= upperLeft.x) &&
			(py >= upperLeft.y))
		{
			return inView;
		}
		else
		{
			return FALSE;
		}
	}
	
	return(inView);
}	

//-----------------------------------------------------------------------------
bool TreeAppearance::recalcBounds (void)
{
	// F3 CPU projection cost-baseline: aggregate per-actor scope into the
	// recalcBounds_perframe bucket. No-op when env OFF.
	::mc2_cpu_proj_cost::Scope _f3_recalcBounds_scope(
	    ::mc2_cpu_proj_cost::BUCKET_RECALCBOUNDS_PERFRAME);
	::mc2_cpu_proj_cost::add_workload_recalcbounds(1);
	// [TOBJSPLIT v1] accumulators declared in code/static_update_counters.h
	// (included above via ../code/static_update_counters.h).
	// Gate: file-scope s_tobjSplitBdOn (defined above, shared with BldgAppearance).

	setVisibilityGatesFromLegacy(false);

	if (eye)
	{
		//-------------------------------------------------------------------
		//NEW METHOD from the WAY BACK Days
		setVisibilityGatesFromLegacy(true);

		// [TOBJSPLIT v1] ANGULAR bracket: matrix-free sphere angular clip.
		// Reads cycle counter immediately before/after.
		{
		unsigned long long _tsA = s_tobjSplitBdOn ? __rdtsc() : 0ULL;
		if (eye->usePerspective)
		{
			Stuff::Vector3D cameraPos;
			cameraPos.x = -eye->getCameraOrigin().x;
			cameraPos.y = eye->getCameraOrigin().z;
			cameraPos.z = eye->getCameraOrigin().y;
			float vClipConstant = eye->verticalSphereClipConstant;
			float hClipConstant = eye->horizontalSphereClipConstant;

			Stuff::Vector3D objectCenter;
			objectCenter.Subtract(position,cameraPos);
			Camera::cameraFrame.trans_to_frame(objectCenter);
			float distanceToEye = objectCenter.GetApproximateLength();
			float clip_distance = fabs(1.0f / objectCenter.y);

			//Is vertex on Screen OR close enough to screen that its triangle MAY be visible?
			// WE have removed the atans here by simply taking the tan of the angle we want above.
			float object_angle = fabs(objectCenter.z) * clip_distance;
			float extent_angle = treeShape->GetExtentRadius() / distanceToEye;
			if (object_angle > (vClipConstant + extent_angle))
			{
				//In theory, we would return here.  Object is NOT on screen.
				setVisibilityGatesFromLegacy(false);
			}
			else
			{
				object_angle = fabs(objectCenter.x) * clip_distance;
				if (object_angle > (hClipConstant + extent_angle))
				{
					//In theory, we would return here.  Object is NOT on screen.
					setVisibilityGatesFromLegacy(false);
				}
			}
		}
		if (s_tobjSplitBdOn) g_tobjAngularCyc.fetch_add(__rdtsc() - _tsA, std::memory_order_relaxed);
		}  // end ANGULAR bracket

		// recalcBounds projection body deleted 2026-05-18 (Task 3, Tree mirror of Task 2):
		// the GPU compute cull (gpu_cull::readback_isActorVisibleLagged) is the
		// substitutive twin of the per-frame screen projection. inView is now
		// coarse-angular-only -- a strict superset of the old projected value;
		// over-inclusion is correctness-safe (cull_gates_are_load_bearing.md).
		// Trees are never pick targets (objmgr.cpp findObjectByMouse skips
		// getObjectClass()==TREE), so screenPos/upperLeft/lowerRight have no
		// pick-path consumer -- no Task-4 re-home needed for Tree.
		// LATENT HAZARD: the deleted block also held the per-LOD-swap texture
		// (re)loader, dead today under the 2026-05-12 TEMP LOD-0 pin
		// (selectLOD forced 0 in this function).
		// If that pin is reverted (when the LOD-1 invisibility root cause is
		// fixed), LOD selection + the per-LOD texture loader MUST be re-homed
		// BEFORE the revert lands -- TreeAppearance::init loads LOD-0 textures
		// ONLY; LOD-1+ would be unloaded after any LOD swap.
	}

	return(inView);
}

//-----------------------------------------------------------------------------
void TreeAppearance::recalcBoundsAndStamp() {
	// FRAME-JOBS-1 worker path. Do not call from game logic.
	extern uint32_t g_mc2FrameCounter;
	if (boundsFrame == g_mc2FrameCounter) return;
	recalcBounds();
	boundsFrame = g_mc2FrameCounter;
}

//-----------------------------------------------------------------------------
long TreeAppearance::render (long depthFixup)
{
	// Mirror BldgAppearance::render: bypass inView under GPU path — the
	// GPU clipper decides visibility, and the legacy angular cull has a
	// ~87% false-negative rate at wolfman zoom.
	if (inView || g_useGpuStaticProps)
	{
		long color = SD_BLUE;
		//unsigned long highLight = 0x007f7f7f;
		if ((teamId > -1) && (teamId < 8)) {
			//static unsigned long highLightTable[3] = {0x00007f00, 0x0000007f, 0x007f0000};
			static uint32_t colorTable[3] = {SB_GREEN | 0xff000000, SB_BLUE | 0xff000000, SB_RED | 0xff000000};
			color = colorTable[homeTeamRelationship];
			//highLight = highLightTable[homeTeamRelationship];
		}
		//---------------------------------------------
		// Call Multi-shape render stuff here.
		// Slice 1 path (g_useGpuObjects). Same shape as BldgAppearance::render.
		bool submittedToGpu = false;
		if (g_useGpuObjects)
		{
			GpuStaticPropBatcher::instance().recordEligibleActor(
				GpuStaticPropPopulation::Tree);

			// Stage 3.C: static registry fast path. If this tree's instance was
			// previously registered and position/shape are stable, inject it into
			// the batcher via markVisible() (processed at registry flush) instead of
			// running the full submitMultiShape() compute path.
			// CacheGpuLightData() is called here (not in touch()) so the light-index
			// refresh is co-located with the render-side emission that needs it.
			// The UINT32_MAX guard handles the degenerate case where no light data
			// is available: invalidate and fall through to the dynamic path.
			// Does NOT return early — selection visualization (drawBars/drawBrackets)
			// at lines 4141-4161 must still run if selected is non-zero.
			if (IsStaticNow()) {
				// Diagnostic 2026-05-05 (advisor-recommended boundary test): set
				// MC2_FORCE_DYNAMIC_TREES=1 to force the static path to fall back
				// to dynamic submitMultiShape. If "black billboard square" trees
				// disappear with the env var set, the static replay path is the
				// failing boundary. If they remain, shared draw/material/global
				// state is guilty. Revert by unsetting the env var (no rebuild).
				static const bool s_forceDynamicTrees =
				    bdForceDynamicDefault("MC2_FORCE_DYNAMIC_TREES");
				if (s_forceDynamicTrees) {
					invalidateStaticRegistration();
					// Fall through to the dynamic path below.
				} else if (treeShape->getCachedGpuLightIndex() == UINT32_MAX) {
					// Light-data gather failed this frame — invalidate so the dynamic
					// path re-runs and re-registers next frame with correct lights.
					invalidateStaticRegistration();
					// Fall through to the if (!submittedToGpu && treeShape) dynamic path below.
				} else {
					// 2026-05-11: see BldgAppearance::render markVisible site.
					// TREE-OVERRIDE-LOD-MVP Task 2: replay the active LOD's recipe +
					// light slot (activeLOD pinned 0 → identical to single-LOD today).
					// STATIC-PROP REGISTRATION CONTRACT v1: markVisibleChecked() reports
					// whether the registry ACCEPTED a live recipe. Only suppress the legacy
					// fallback (submittedToGpu=true) when it did. On a dead/tombstoned handle
					// (the "trees vanish on replay" bug) invalidate so the !submittedToGpu
					// full-bake path below re-registers + draws this frame — GPU cache miss
					// becomes a legacy re-bake, NOT an invisible prop.
					const GpuStaticPropRegistry::StaticSubmitResult treeRes =
						GpuStaticPropRegistry::markVisibleChecked(
							staticReg[activeLOD].recipeIndex,
							staticReg[activeLOD].lightDataIndex,
							treeShape ? treeShape->GetExtentRadius() : 0.0f);
					if (treeRes == GpuStaticPropRegistry::StaticSubmitResult::Submitted) {
						// [LIGHTSLOT v1] Task 0: registered static-replay trees record
						// their per-frame slot here (the steady-state path) so the
						// distinct-slot set captures the whole forest, not just frame-1
						// full-bake instances.
						mc2_lightslot_trace::recordInstance(
							appearType,
							appearType && appearType->treeRenderShapeLodCount > 0,
							staticReg[activeLOD].lightDataIndex);
						submittedToGpu = true;
					} else {
						bdLogStaticRegInvalid("tree", (int)treeRes,
							staticReg[activeLOD].recipeIndex,
							appearType ? appearType->name : nullptr);
						invalidateStaticRegistration();  // drop dead handle -> full-bake re-registers below
					}
				}
			}
			if (!submittedToGpu && treeShape)
			{
				// Stage 3.C / M1: shape-swap invalidation. IsStaticNow()'s
				// staticReg.shape==treeShape check routes us here when treeShape was
				// reassigned (LOD swap at bdactor.cpp:3984, damage at ~3596/3626), but
				// staticReg.registered==true still blocks the registration block below.
				// Invalidate the stale entry first so the new shape gets registered.
				// PERF DIAGNOSTIC 2026-05-07: count this branch — per capture 3
				// analysis, LOD-swap-driven invalidate+re-register on trees was
				// running tens of times per frame, leaking recipe slots and
				// pin-count churn. Tracy zone makes the rate visible per-frame.
				// TREE-OVERRIDE-LOD-MVP Task 2: the dynamic recovery path only ever
				// deals with the live LOD0 render-instance (treeShape); key on the
				// active LOD tuple (activeLOD pinned 0).
				// forced-LOD FIX: only the live LOD0 instance can suffer treeShape reassignment;
				// guard to activeLOD==0 so a higher LOD's nullptr .shape never triggers invalidation.
				if (activeLOD == 0 && staticReg[0].registered && staticReg[0].shape != treeShape) {
					invalidateStaticRegistration();
				}

				// Stage 2.C+: see BldgAppearance::render for callerName intent.
				const char* callerName = (appearType ? appearType->name : nullptr);
				submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
					treeShape, GpuStaticPropPopulation::Tree, callerName);
				// Slice 2 (object-offload) — Stage 2.B: see BldgAppearance::render
				// for full rationale on the late-reg recovery flag.
				if (!submittedToGpu &&
				    GpuStaticPropBatcher::instance().wasLastFailureLateRegistration())
				{
					needsFullBakeNextFrame = true;
					invalidateStaticRegistration();  // clear stale registration if any
				}
				// Stage 3.C: registration block. On the first successful full-bake
				// submission with no late-reg flag, snapshot the leaf batch into the
				// registry. Subsequent frames use the static path above.
				// Pass treeShape as multi so flush() can patch lightDataIndex each frame.
				if (submittedToGpu && !staticReg[activeLOD].registered
				        && GpuStaticPropRegistry::isEnabled()
				        && !needsFullBakeNextFrame) {
					const auto& batch =
						GpuStaticPropBatcher::instance().getLastBuiltBatch();
					// M1 RenderWorld route (Slice M1 Task 10).
					int32_t legacyIdx = -1;
					(void)GameAdapters::StaticProp::syncStaticProp(
						treeShape, batch.data(), batch.size(), &legacyIdx);
					staticReg[activeLOD].recipeIndex = legacyIdx;
					staticReg[activeLOD].registered  = (staticReg[activeLOD].recipeIndex >= 0);
					staticReg[activeLOD].shape        = treeShape;
					if (staticReg[activeLOD].registered) {
						// H4 follow-up (2026-05-07): per-frame re-registration
						// after LOD/shape swap has the same lightData_ gap as
						// mission-load registerStatic(). Force one full update()
						// so touch() cannot resubmit default-zero lightData_.
						// Spec: docs/superpowers/specs/2026-05-07-lod-swap-static-registry-churn.md
						needsFullBakeNextFrame = true;
					}
				}
			}
			if (!submittedToGpu)
			{
				GpuStaticPropBatcher::instance().recordCpuFallback(
					GpuStaticPropPopulation::Tree);
			}
		}
		// Legacy bypass-cull path. Mutually exclusive with slice 1 — gated on
		// !g_useGpuObjects. Tagged Legacy so Gate F's fallback-rate is computed
		// only over slice-1 populations. See spec R1.
		if (!submittedToGpu && !g_useGpuObjects && g_useGpuStaticProps && treeShape)
		{
			submittedToGpu = GpuStaticPropBatcher::instance().submitMultiShape(
				treeShape, GpuStaticPropPopulation::Legacy);
		}
		if (!submittedToGpu)
			treeShape->Render();

		if (selected & DRAW_BARS)
		{
			drawBars();
		}

		if ( selected & DRAW_BRACKETS )
		{
			drawSelectBrackets(color);
		}

		if ( selected & DRAW_TEXT )
		{

			if (objectNameId != -1)
			{
				char tmpString[255];
				cLoadString(objectNameId, tmpString, 254);

				drawTextHelp(tmpString, color);
			}
		}

		// I don't want my selection reset each time I draw HKG
//		selected = FALSE;

		//---------------------------------------------------------
		// Render the Bounding Box to see if it is OK.
#ifdef DRAW_BOX
		Stuff::Vector3D nodeCenter = treeShape->GetRootNodeCenter();

		boxCoords[0].x = position.x + treeShape->minBox.x + nodeCenter.x;
		boxCoords[0].y = position.y + treeShape->minBox.z + nodeCenter.z;
		boxCoords[0].z = position.z + treeShape->maxBox.y + nodeCenter.y;
		
		boxCoords[1].x = position.x + treeShape->minBox.x + nodeCenter.x;
		boxCoords[1].y = position.y + treeShape->maxBox.z + nodeCenter.z;
		boxCoords[1].z = position.z + treeShape->maxBox.y + nodeCenter.y;
		
		boxCoords[2].x = position.x + treeShape->maxBox.x + nodeCenter.x;
		boxCoords[2].y = position.y + treeShape->maxBox.z + nodeCenter.z;
		boxCoords[2].z = position.z + treeShape->maxBox.y + nodeCenter.y;
		
		boxCoords[3].x = position.x + treeShape->maxBox.x + nodeCenter.x;
		boxCoords[3].y = position.y + treeShape->minBox.z + nodeCenter.z;
		boxCoords[3].z = position.z + treeShape->maxBox.y + nodeCenter.y;
		
		boxCoords[4].x = position.x + treeShape->minBox.x + nodeCenter.x;
		boxCoords[4].y = position.y + treeShape->minBox.z + nodeCenter.z;
		boxCoords[4].z = position.z + treeShape->minBox.y + nodeCenter.y;
		
		boxCoords[5].x = position.x + treeShape->maxBox.x + nodeCenter.x;
		boxCoords[5].y = position.y + treeShape->minBox.z + nodeCenter.z;
		boxCoords[5].z = position.z + treeShape->minBox.y + nodeCenter.y;
		
		boxCoords[6].x = position.x + treeShape->maxBox.x + nodeCenter.x;
		boxCoords[6].y = position.y + treeShape->maxBox.z + nodeCenter.z;
		boxCoords[6].z = position.z + treeShape->minBox.y + nodeCenter.y;
		
		boxCoords[7].x = position.x + treeShape->minBox.x + nodeCenter.x;
		boxCoords[7].y = position.y + treeShape->maxBox.z + nodeCenter.z;
		boxCoords[7].z = position.z + treeShape->minBox.y + nodeCenter.y;

		Stuff::Vector4D screenPos[8];
		for (long i=0;i<8;i++)
		{
			// [PROJECTZ:ScreenXYOracle id=bdactor_box_wire_b]
			eye->projectForScreenXY(boxCoords[i],screenPos[i]);
		}

		{
			LineElement newElement(screenPos[0],screenPos[1],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[0],screenPos[4],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[0],screenPos[3],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[5],screenPos[4],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[5],screenPos[6],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[5],screenPos[3],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[2],screenPos[3],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[2],screenPos[6],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[2],screenPos[1],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[7],screenPos[1],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[7],screenPos[6],XP_WHITE,NULL,-1);
			newElement.draw();
		}
		
		{
			LineElement newElement(screenPos[7],screenPos[4],XP_WHITE,NULL,-1);
			newElement.draw();
		}

#endif
	}
	return NO_ERR;
}

//-----------------------------------------------------------------------------
long TreeAppearance::renderShadows (void)
{
	return NO_ERR;
}

//-----------------------------------------------------------------------------
long TreeAppearance::update (bool animate)
{
	::mc2_object_recon::Scope _recon_tree_(
		&::mc2_object_recon::g_per_frame.tree_update_ns,
		&::mc2_object_recon::g_per_frame.tree_update_calls);
	if (rotation > 180)
		rotation -= 360;

	if (rotation < -180)
		rotation += 360;

	// TREE-OVERRIDE-LOD-MVP-1: per-frame active-LOD pick. Runs in BOTH update()
	// and touch() — registered trees take the touch() skip path every frame, so
	// selecting only in update() froze activeLOD at first registration.
	selectActiveLOD();

	//-------------------------------------------
	// Does math necessary to draw Tree
	Stuff::UnitQuaternion rot;
	float yawAngle = (rotation * DEGREES_TO_RADS) + (yaw * DEGREES_TO_RADS);
	float pitchAngle = (pitch * DEGREES_TO_RADS);
	rot = Stuff::EulerAngles(pitchAngle, yawAngle, 0.0f);

	Stuff::Point3D xlatPosition;
	xlatPosition.x = -position.x;
	xlatPosition.y = position.z;
	xlatPosition.z = position.y;

	if (!fogLightSet)
	{
		unsigned char lightr,lightg,lightb;
		float lightIntensity = 1.0f;
		if (land)
			lightIntensity = land->getTerrainLight(position);

		lightr = eye->getLightRed(lightIntensity);
		lightg = eye->getLightGreen(lightIntensity);
		lightb = eye->getLightBlue(lightIntensity);

		lightRGB = (lightr<<16) + (lightg<<8) + lightb;

		fogRGB = 0xff<<24;
		float fogStart = eye->fogStart;
		float fogFull = eye->fogFull;

		if (xlatPosition.y < fogStart)
		{
			float fogFactor = fogStart - xlatPosition.y;
			if (fogFactor < 0.0)
				fogRGB = 0xff<<24;
			else
			{
				fogFactor /= (fogStart - fogFull);
				if (fogFactor <= 1.0)
				{
					fogFactor *= fogFactor;
					fogFactor = 1.0 - fogFactor;
					fogFactor *= 256.0;
				}
				else
				{
					fogFactor = 256.0;
				}

				unsigned char fogResult = float2long(fogFactor);
				fogRGB = fogResult << 24;
			}
		}
		else
		{
			fogRGB = 0xff<<24;
		}

		fogLightSet = true;
	}

	if (useFog)
		treeShape->SetFogRGB(fogRGB);
	else
		treeShape->SetFogRGB(0xffffffff);

	DWORD oldRGB = eye->getLightColor(1);

	eye->setLightColor(1,lightRGB);
	eye->setLightIntensity(1,1.0);

	if (forceLightsOut)
		treeShape->SetLightsOut(true);

	// Under the GPU static-prop path we need listOfColors / shapeToWorld
	// fresh every frame regardless of inView so submitMultiShape can safely
	// read shape->listOfVertices during submit().
	if (inView || g_useGpuStaticProps)
	{
		treeShape->SetUseShadow(false);

		TG_LightPtr light = eye->getWorldLight(0);
		light->active = false;

		treeShape->SetLightList(eye->getWorldLights(),eye->getNumLights());
		// Slice 2 (object-offload) — Stage 2.B: eligibility hoist.
		// See BldgAppearance::update for the full rationale; same shape.
		// Branch lives INSIDE the existing inView||g_useGpuStaticProps cull
		// gate to preserve slice 1's R1 invariant.
		// PERF DIAGNOSTIC 2026-05-06: Tracy zones — see BldgAppearance::update
		// for the same instrumentation set. Same theories under investigation.
		bool gpuEligible;
		{
			gpuEligible = g_useGpuObjects &&
			              !needsFullBakeNextFrame &&
			              GpuStaticPropBatcher::instance().isMultiShapeEligibleForGpuObjects(treeShape);
		}

		if (gpuEligible)
		{
			// Stage 2.D.2 fix: cache GPU light data while lights are per-actor-correct.
			{
				// TREE-OVERRIDE-LOD-MVP K×M FIX (2026-06-03): per-instance light slot is
				// LOD-INDEPENDENT. treeShape is always the LOD0 render-instance and the
				// gather is position-keyed (terrain light by world pos), not geometry-keyed.
				// Bake ONCE keyed on LOD0 and SHARE that single slot with whatever LOD is
				// active -> light table stays U~=K (pre-LOD baseline) flat across LOD count,
				// never K×M. Drawn geometry still swaps per-LOD via staticReg[lod].recipeIndex.
				mc2CacheOrBakeStaticGpuLight(treeShape, staticReg[0].registered, staticReg[0].recipeIndex);
				// 2026-05-11 per-instance capture (mirror of BldgAppearance::update).
				const uint32_t _sharedLightSlot = treeShape->getCachedGpuLightIndex();
				staticReg[0].lightDataIndex = _sharedLightSlot;
				staticReg[activeLOD].lightDataIndex = _sharedLightSlot;  // share LOD0 slot
				// [LIGHTSLOT v1] Task 0 cardinality gate (diagnostic only).
				mc2_lightslot_trace::recordInstance(
					appearType,
					appearType && appearType->treeRenderShapeLodCount > 0,
					staticReg[activeLOD].lightDataIndex);
			}
			{
				// GPU-INSTANCE-SKIP-POOLS-1 (2026-06-03): gpuEligible already
				// guarantees every leaf is registered in s_typeIndex, so geometry
				// is in the immutable per-type VBO and lighting is O(1) via
				// lightDataIndex. Legacy _PositionsOnly allocated all six TGL
				// frame pools sized to the mesh per visible instance (peak = Σ
				// over the forest → overflow on a heavy override mesh), yet the
				// GPU draw consumes NONE of that content (submit reads
				// rec.shapeToWorld + the debug-only zero-padded Colors SSBO). Run
				// the ZERO-POOL hierarchy walk instead — still populates
				// rec.shapeToWorld per leaf, no pool alloc.
				// MC2_LEGACY_INSTANCE_POOLS=1 reverts to the old path.
				if (gos_StaticPropLegacyInstancePools())
					treeShape->TransformMultiShape_PositionsOnly (&xlatPosition,&rot);
				else
					treeShape->TransformMultiShape_HierarchyOnly (&xlatPosition,&rot);
			}
			// Stage 2.D.2: dual-emit full bake — same rationale as BldgAppearance
			// above. Populates listOfTriangles[].aRGBLight for snapshot in submit().
			// Stage 2.D.3: per-actor gate (see BldgAppearance::update above).
			if (gos_object_parity::IsDualEmitArmedForActor(treeShape)) {
				treeShape->TransformMultiShape (&xlatPosition,&rot);
			}
		}
		else
		{
			// GPU-INSTANCE-SKIP-POOLS-1 (2026-06-03): this "full-bake" else-branch
			// is where registered override trees land — the gpuEligible branch
			// above is gated by !needsFullBakeNextFrame.
			// CORRECTION (2026-06-03, recon docs/model-override-lighting-lod-recon.md):
			// the earlier claim here — "32-slot light UBO returns UINT32_MAX under
			// forest contention, perpetually re-arming the full-bake latch" — is
			// STALE/FALSE. The static-prop light table is an UNBOUNDED grow-on-demand
			// SSBO (b41baec); there is no 32-slot cap and no allocator overflow
			// sentinel. cachedGpuLightIndex_==0xFFFFFFFF means "not yet cached / gather
			// didn't run", NOT overflow. Registered override trees use the persistent
			// baked-light table (MC2_LIGHTBAKE, O(1) re-ship) after frame 1. The
			// needsFullBakeNextFrame re-arm is the LOD-swap-black guard + mission-load
			// transient, not a lighting overflow. The forest's real cost is GPU
			// LOD/overdraw (trees pinned LOD0, full 706k-tri at all distances), not
			// CPU lighting. The pool-skip rationale below is independently verified and
			// stands regardless. Verified A/B: this
			// branch full-baking the 6×~535k-vert lush forest pegged the TGL pools
			// to 99% (legacy), yet the trees draw via the GPU registry/substrate
			// indirect path (buckets=227), NOT via the CPU TG_Shape::Render() that
			// reads the pools (submit_trees=0, gpu indirect_draw present). Pixel
			// A/B (same camera, legacy 99% vs skip 0%) shows IDENTICAL tree canopy.
			// So for a registered type the full-bake pool content is vestigial:
			// run the ZERO-POOL hierarchy walk to populate rec.shapeToWorld and
			// still call mc2CacheOrBakeStaticGpuLight below (pool-independent — it
			// only walks the hierarchy + GatherGpuObjectLightDataOnly). Result:
			// pool peak 99%→0% on the heavy forest, no visual change.
			// MC2_LEGACY_INSTANCE_POOLS=1 reverts to the full bake.
			if (!gos_StaticPropLegacyInstancePools() &&
			    GpuStaticPropBatcher::instance().isMultiShapeEligibleForGpuObjects(treeShape))
				treeShape->TransformMultiShape_HierarchyOnly (&xlatPosition,&rot);
			else
				treeShape->TransformMultiShape (&xlatPosition,&rot);
			// 2026-05-10: mirror of the BldgAppearance fix at :2339-2341.
			// Seed cachedGpuLightIndex_ in the full-bake branch so the
			// next render() doesn't fail the UINT32_MAX gate at :4341
			// and invalidate the freshly-set staticReg.
			// TREE-OVERRIDE-LOD-MVP K×M FIX (2026-06-03): bake keyed on LOD0, share slot
			// to active LOD (see gpuEligible branch above for rationale).
			mc2CacheOrBakeStaticGpuLight(treeShape, staticReg[0].registered, staticReg[0].recipeIndex);
			// 2026-05-11 per-instance capture (mirror of gpuEligible branch).
			const uint32_t _sharedLightSlot = treeShape->getCachedGpuLightIndex();
			staticReg[0].lightDataIndex = _sharedLightSlot;
			staticReg[activeLOD].lightDataIndex = _sharedLightSlot;  // share LOD0 slot
			// [LIGHTSLOT v1] Task 0 cardinality gate (diagnostic only). This is
			// the branch registered override trees actually land in (gpuEligible
			// above is gated by !needsFullBakeNextFrame).
			mc2_lightslot_trace::recordInstance(
				appearType,
				appearType && appearType->treeRenderShapeLodCount > 0,
				staticReg[activeLOD].lightDataIndex);
			needsFullBakeNextFrame = false;
		}

		light->active = true;

		if ((turn > 3) && useShadows)
			beenInView = true;
	}
	
	//Set Ambient back to normal color.
	eye->setLightColor(1,oldRGB);

	return TRUE;
}

//-----------------------------------------------------------------------------

void TreeAppearance::markTerrain (_ScenarioMapCellInfo* pInfo, int type, int counter)
{
	//MUST force tree to HIGHEST LOD!!!  Impassability data is only valid at this LOD!!
	// Tree will reset its LOD on next draw!!
	if (currentLOD)
	{
		currentLOD = 0;
	
		treeShape->ClearAnimation();
		delete treeShape;
		treeShape = NULL;
	
		treeShape = appearType->treeShape[currentLOD]->CreateFrom();
	}

	//-------------------------------------------------------------
	// New way.  For each vertex in each shape, translate to world
	for (int i=0;i<treeShape->GetNumShapes();i++)
	{
		//Check if the artists meant for this piece to NOT block passability!!
		if (S_strnicmp(treeShape->GetNodeId(i),"_PAB",4) != 0)
		{
			for (int j=0;j<treeShape->GetNumVerticesInShape(i);j++)
			{
				Stuff::Vector3D vertexPos, worldPos;
				vertexPos = treeShape->GetShapeVertexInEditor(i,j,-rotation);
				worldPos.Add(position,vertexPos);
	
				int cellR, cellC;
				land->worldToCell(worldPos,cellR,cellC);
				if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
					(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
				{
					continue;
				}
				_ScenarioMapCellInfo*pTmp = &(pInfo[cellR * Terrain::realVerticesMapSide * MAPCELL_DIM + cellC]);

				if (vertexPos.z >= 1.0f)
				{
					pTmp->forest = true;
						
					float cellLocalHeight = vertexPos.z * metersPerWorldUnit * 0.25f;
					if (cellLocalHeight > 15.0f)
						cellLocalHeight = 15.0f;
						
					if (pTmp->lineOfSight < cellLocalHeight)
						pTmp->lineOfSight = cellLocalHeight+0.5f;
				}
			}
		}
	}
}

//-----------------------------------------------------------------------------
void TreeAppearance::markLOS (bool clearIt)
{
	//MUST force building to HIGHEST LOD!!!  IMpassability data is only valid at this LOD!!
	// Building will reset its LOD on next draw!!
	if (currentLOD)
	{
		currentLOD = 0;
	
		treeShape->ClearAnimation();
		delete treeShape;
		treeShape = NULL;
	
		treeShape = appearType->treeShape[currentLOD]->CreateFrom();
	}

	//-------------------------------------------------------------
	// New way.  For each vertex in each shape, translate to world
	for (int i=0;i<treeShape->GetNumShapes();i++)
	{
		//Check if the artists meant for this piece to NOT block LOS!!
		// Probably should check for light cones,too!
		
		if ((S_strnicmp(treeShape->GetNodeId(i),"LOS_",4) != 0) &&
			(S_strnicmp(treeShape->GetNodeId(i),"SpotLight_",10) != 0))
		{
			for (int j=0;j<treeShape->GetNumVerticesInShape(i);j++)
			{
				Stuff::Vector3D vertexPos, worldPos;
				vertexPos = treeShape->GetShapeVertexInEditor(i,j,-rotation);
//				vertexPos = treeShape->GetShapeVertexInWorld(i,j,-rotation);
				worldPos.Add(position,vertexPos);
	
				int cellR, cellC;
				land->worldToCell(worldPos,cellR,cellC);
				if ((0 > cellR) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellR) || 
					(0 > cellC) || (Terrain::realVerticesMapSide * MAPCELL_DIM <= cellC))
				{
					continue;
				}
				
				//----------------
				// Mark the map...
				MapCellPtr curCell = GameMap->getCell(cellR, cellC);

				float currentCellHeight = curCell->getLocalHeight();
				
				float cellLocalHeight = vertexPos.z * metersPerWorldUnit * 0.25f;
				if (cellLocalHeight > 15.0f)
					cellLocalHeight = 15.0f;
				
				if (!clearIt)
				{
					if (cellLocalHeight > currentCellHeight)
						curCell->setLocalHeight(cellLocalHeight+0.5f);
				}
				else	//We want to clear all LOS height INFO.  We're about to change shape!!
				{
					curCell->setLocalHeight(0.0f);
				}
			}
		}
	}
}

//-----------------------------------------------------------------------------

bool TreeAppearance::IsStaticNow() const
{
	// TREE-OVERRIDE-LOD-MVP Task 2: the live per-instance render shape is the
	// active LOD's shape (LOD0 today; activeLOD pinned 0). The shape-key check
	// guards against treeShape reassignment (LOD/damage swaps).
	// TREE-OVERRIDE-LOD-MVP forced-LOD FIX (2026-06-03): the shape-key guards
	// treeShape reassignment, which only applies to the live LOD0 render-instance.
	// Higher LODs have no live per-instance shape (staticReg[lod>=1].shape==nullptr),
	// so the key is bypassed for activeLOD!=0 — markVisible replays the pre-registered
	// LOD recipe directly (drawing the correct LOD geometry).
	return staticReg[activeLOD].registered
		&& (activeLOD == 0 ? staticReg[0].shape == treeShape : true)
		&& !needsFullBakeNextFrame;
}

void TreeAppearance::selectActiveLOD()
{
	// Per-frame distance-driven LOD pick. MUST run on BOTH update() and touch()
	// (registered trees take the touch() skip path under MC2_STATIC_UPDATE_SKIP=1).
	int _maxAvailLOD = 0;
	for (int _l = MAX_LODS - 1; _l > 0; --_l)
		if (staticReg[_l].registered) { _maxAvailLOD = _l; break; }
	int _wantLOD = activeLOD;   // default: STAY (deadband) — anti-thrash
	static const char* s_forceLodStr = getenv("MC2_FORCE_LOD");
	if (s_forceLodStr) {
		_wantLOD = atoi(s_forceLodStr);            // debug override
	} else if (_maxAvailLOD >= 1 && eye) {
		static const float s_impostorDist = [](){
			const char* e = getenv("MC2_IMPOSTOR_DIST"); return e ? (float)atof(e) : 800.0f;
		}();
		// HYSTERESIS: separate near/far thresholds so a tree sitting near the
		// boundary does NOT oscillate activeLOD every frame. Oscillation forced a
		// full update()+submitMultiShape+re-register churn each frame, backing up
		// the GL command queue that then drained at SDL_GL_SwapWindow (25-47ms).
		const float _far  = s_impostorDist * 1.08f;
		const float _near = s_impostorDist * 0.92f;
		Stuff::Vector3D _camP;
		_camP.x = -eye->getCameraOrigin().x;
		_camP.y =  eye->getCameraOrigin().z;
		_camP.z =  eye->getCameraOrigin().y;
		Stuff::Vector3D _d; _d.Subtract(position, _camP);
		const float _dist = _d.GetApproximateLength();
		if (activeLOD < _maxAvailLOD && _dist > _far)  _wantLOD = _maxAvailLOD; // far->impostor
		else if (activeLOD > 0       && _dist < _near) _wantLOD = 0;            // near->LOD0
	}
	if (_wantLOD < 0) _wantLOD = 0;
	if (_wantLOD > _maxAvailLOD) _wantLOD = _maxAvailLOD;
	if (_wantLOD != activeLOD) {
		activeLOD = _wantLOD;
		// Only force a full re-bake if the target LOD isn't already registered+baked.
		// Switching between pre-registered LODs (shared LOD0 light slot) needs NO
		// re-bake — stay on the cheap touch() path; markVisible(staticReg[activeLOD])
		// in render() picks up the new recipe. This avoids the per-frame re-register
		// churn that stalled SDL_GL_SwapWindow.
		if (!staticReg[activeLOD].registered)
			needsFullBakeNextFrame = true;
	}
}

void TreeAppearance::touch()
{
	// FRAME-JOBS-2F: if touchSerialCommit() already ran this frame (split path active),
	// skip redundant Path B work. See BldgAppearance::touch for full contract.
	if (mc2FrameJobsTouchDiagEnabled()) {
		touchEntryDiagRollFrame(g_mc2FrameCounter);
		if (touchSerialCommitFrame == g_mc2FrameCounter) {
			++s_touchEntryDiag.legacy_skipped;
			return;
		}
		++s_touchEntryDiag.legacy_ran_nosplit;
	} else {
		if (touchSerialCommitFrame == g_mc2FrameCounter)
			return;
	}

	selectActiveLOD();  // re-evaluate LOD every frame on the skip path
	// Stage 3.C: called by the outer-skip gate instead of update() when this
	// tree is registered and stable. Re-submits the cached lightData_ (set
	// during the last update() call) to get a fresh UBO slot index for this
	// frame — no s_listOfLights dependency, no terrain lookup needed.
	// Touch() advances lastTurnTransformed so TG_Shape::Render()'s staleness
	// guard doesn't suppress the legacy fallback path.
	if (treeShape) {
		// STABLE-LIGHT-SKIP-BROADEN-1: broaden the LBSS resubmit-skip that
		// TreeAppearance::touchSerialCommit() takes (bdactor.cpp ~6709) to this
		// legacy touch() path (the live path in stock config — FRAME-JOBS touch
		// split default-OFF). Eligibility mirrors the serial-commit tree criteria
		// exactly: registered + valid recipeIndex + valid lightDataIndex +
		// !needsFullBakeNextFrame (trees don't track lastLightEnvGen — mission
		// lighting is effectively static for trees). When eligible the cached
		// lightDataIndex is still valid on the GPU (same baked-prefix persistence
		// invariant), so the resubmit below is redundant. Still call treeShape->
		// Touch() (advance lastTurnTransformed) — skip only the expensive resubmit.
		// Gate: mc2StableLightSkipTouchEnabled() (default OFF => resubmit still runs).
		{
			const bool treeRegistered = (staticReg[activeLOD].registered && staticReg[activeLOD].recipeIndex >= 0);
			const bool treeIdxValid   = (staticReg[activeLOD].lightDataIndex != 0xFFFFFFFFu);
			const bool treeEligible   = treeRegistered && treeIdxValid && !needsFullBakeNextFrame;
			if (mc2StableLightSkipTouchEnabled() && mc2LightbridgeStableSkipEnabled() && treeEligible) {
				lbssRecordTree(/*skipTaken=*/true, /*blk_idx=*/false);
				treeShape->Touch();
				return;
			}
		}
		// [LIGHTBRIDGE v1] C6 retirement: repoint to the primed 38d8720 slot
		// (zero FNV/memcmp; cachedFrame_ stamped). MISS keeps the legacy
		// resubmit (NOT CacheGpuLightData -- terrain-color-staleness,
		// msl.cpp:1874-1887). MC2_LIGHTBAKE=0 -> legacy path bit-for-bit.
		extern bool mc2LightBakeEnabled();
		extern bool mc2IsBakedStaticLightPresent(int32_t);
		// [LIGHTBRIDGE-BAKED-PROBE-1] probe only — EmitBakedGpuLightData discards baked
		if (mc2LightBakeEnabled()
		    && staticReg[activeLOD].registered && staticReg[activeLOD].recipeIndex >= 0
		    && mc2IsBakedStaticLightPresent(staticReg[activeLOD].recipeIndex)) {
			TG_HWLightsData baked{};
			treeShape->EmitBakedGpuLightData(staticReg[activeLOD].recipeIndex, baked);
		} else {
			treeShape->ResubmitCachedGpuLightData();
		}
		// 2026-05-11 per-instance capture: see BldgAppearance::touch.
		staticReg[activeLOD].lightDataIndex = treeShape->getCachedGpuLightIndex();
		treeShape->Touch();
	}
}

// FRAME-JOBS-2D: lock-free per-instance prep; runs on worker threads.
// selectActiveLOD() reads per-instance state only (no shared mutation).
// treeShape->Touch() is lock-free (verified: sets lastTurnTransformed only, tgl.cpp:4073).
void TreeAppearance::touchWorkerPrepass()
{
	selectActiveLOD();
	if (treeShape)
		treeShape->Touch();
}

// FRAME-JOBS-2D: light-data resubmit; runs serially on main thread after worker join.
void TreeAppearance::touchSerialCommit()
{
	// FRAME-JOBS-2F: stamp this frame unconditionally so touch() (Path B, terrain object
	// loop) returns immediately. Set BEFORE any early returns including stable-skip.
	touchSerialCommitFrame = g_mc2FrameCounter;
	if (mc2FrameJobsTouchDiagEnabled()) {
		touchEntryDiagRollFrame(g_mc2FrameCounter);
		++s_touchEntryDiag.serial_commit_hits;
	}

	if (!treeShape) {
		if (s_proxyReconEnabled) {
			g_spr_phase2Calls.fetch_add(1, std::memory_order_relaxed);
			g_spr_treeCalls.fetch_add(1, std::memory_order_relaxed);
			g_spr_rejNoShape.fetch_add(1, std::memory_order_relaxed);
			g_spr_callCounter.fetch_add(1, std::memory_order_relaxed);
		}
		return;
	}

	// FRAME-JOBS-2F Step 5: needsFullBakeNextFrame guard — mirrors BldgAppearance's
	// stableLightSkipEligible which includes !needsFullBakeNextFrame. In practice,
	// objects with needsFullBakeNextFrame=true do not reach touchSerialCommit via the
	// split path (IsStaticNow() returns false), but add explicit guard for safety.
	// When true, fall through to full commit (do not apply stable-skip).
	const bool forceFullCommit = needsFullBakeNextFrame;

	// STATIC-SCENE-PROXY-RECON-1: classify tree as proxy candidate or not.
	// Must run BEFORE the LBSS early return below (most trees skip via LBSS).
	// Trees don't track lastLightEnvGen (mission lighting is static for trees),
	// so the criteria are: registered + valid recipeIndex + valid lightDataIndex
	// + !needsFullBakeNextFrame. Mirrors the tree stable-skip check exactly.
	if (s_proxyReconEnabled) {
		g_spr_phase2Calls.fetch_add(1, std::memory_order_relaxed);
		g_spr_treeCalls.fetch_add(1, std::memory_order_relaxed);

		const bool treeRegistered = (staticReg[activeLOD].registered && staticReg[activeLOD].recipeIndex >= 0);
		const bool treeIdxValid   = (staticReg[activeLOD].lightDataIndex != 0xFFFFFFFFu);
		const bool isProxy = treeRegistered && treeIdxValid && !forceFullCommit;
		if (!isProxy) {
			if (!treeRegistered)
				g_spr_rejNoStaticReg.fetch_add(1, std::memory_order_relaxed);
			else if (!treeIdxValid)
				g_spr_rejBadLightIdx.fetch_add(1, std::memory_order_relaxed);
			else if (forceFullCommit)
				g_spr_rejNeedsFullBake.fetch_add(1, std::memory_order_relaxed);
		}
		if (isProxy) g_spr_proxyCandidate.fetch_add(1, std::memory_order_relaxed);
		// Tree path also bumps the shared call counter so the print trigger fires
		// on tree-heavy missions where bldg count alone wouldn't reach the modulo.
		g_spr_callCounter.fetch_add(1, std::memory_order_relaxed);
	}

	// [LIGHTBRIDGE-STABLE-SKIP-WIRE-1] Tree path: if MC2_LIGHTBRIDGE_STABLE_SKIP=1 and
	// the active LOD's baked light slot is valid, skip EmitBakedGpuLightData entirely.
	// INVARIANT: same as BldgAppearance — lightData_[recipeIndex] persists frame-to-frame;
	// staticReg[activeLOD].lightDataIndex == recipeIndex from prior frame's emit is valid.
	// Trees don't track lastLightEnvGen (mission lighting is effectively static for trees).
	if (mc2LightbridgeStableSkipEnabled() && !forceFullCommit) {
		const bool indexValid = (staticReg[activeLOD].lightDataIndex != 0xFFFFFFFFu);
		const bool registered = (staticReg[activeLOD].registered && staticReg[activeLOD].recipeIndex >= 0);
		const bool skipTaken  = (registered && indexValid);
		lbssRecordTree(skipTaken, registered && !indexValid);
		if (skipTaken)
			return;
	}

	extern bool mc2LightBakeEnabled();
	extern bool mc2IsBakedStaticLightPresent(int32_t);
	// [LIGHTBRIDGE-BAKED-PROBE-1] probe only — EmitBakedGpuLightData discards baked
	if (mc2LightBakeEnabled()
	    && staticReg[activeLOD].registered && staticReg[activeLOD].recipeIndex >= 0
	    && mc2IsBakedStaticLightPresent(staticReg[activeLOD].recipeIndex)) {
		TG_HWLightsData baked{};
		treeShape->EmitBakedGpuLightData(staticReg[activeLOD].recipeIndex, baked);
	} else {
		treeShape->ResubmitCachedGpuLightData();
	}
	staticReg[activeLOD].lightDataIndex = treeShape->getCachedGpuLightIndex();
	// NOTE: treeShape->Touch() is in touchWorkerPrepass() (verified lock-free)
}

void TreeAppearance::invalidateStaticRegistration()
{
	// TREE-OVERRIDE-LOD-MVP Task 2: tear down every registered LOD recipe.
	for (long lod = 0; lod < MAX_LODS; ++lod) {
		if (staticReg[lod].registered && staticReg[lod].recipeIndex >= 0)
			GameAdapters::StaticProp::destroyStaticPropByIndex(staticReg[lod].recipeIndex);
		staticReg[lod] = StaticRegistration{};
	}
}

// Task 5 (Track B): mission-load bulk static-prop registration (mirror of
// BldgAppearance::registerStatic). HC-1: writes directly to typed staticReg.
void TreeAppearance::registerStatic() {
	const bool seamProbe = (getenv("MC2_MODOVERRIDE_TRACE") != nullptr)
		&& appearType && appearType->treeRenderShape[0];
	if (seamProbe)
		fprintf(stderr, "[SEAMPROBE] tree registerStatic ENTER name=%s registered=%d hasShape=%d enabled=%d renderShape=%p\n",
			appearType->name, (int)staticReg[0].registered, (int)(treeShape!=NULL),
			(int)GpuStaticPropRegistry::isEnabled(), (void*)appearType->treeRenderShape[0]), fflush(stderr);
	// TREE-OVERRIDE-LOD-MVP Task 2: registered-state is now per-LOD. LOD0 is
	// the canonical "already done" gate (it always exists — override or stock).
	if (staticReg[0].registered) return;
	if (!treeShape)              return;
	if (!GpuStaticPropRegistry::isEnabled()) return;

	// MODEL-OVERRIDE: register render-shape geometry before finalizeGeometry
	// (see BldgAppearance::registerStatic for full rationale — late init() reg
	// otherwise drops ~1k override trees to the CPU first-render path).
	// TREE-OVERRIDE-LOD-MVP M-C: for override types iterate ONLY populated LODs
	// (each distinct override LOD shape uploaded once); stock types keep the
	// full MAX_LODS loop (distinct stock treeShape[i] per LOD).
	const bool _treeRegIsOverride = (appearType->treeRenderShapeLodCount > 0);
	const int  _treeRegLods = _treeRegIsOverride
		? (int)appearType->treeRenderShapeLodCount : MAX_LODS;
	for (int i = 0; i < _treeRegLods; ++i)
		GpuStaticPropBatcher::instance().registerMultiShape(appearType->getTreeRenderShape(i), _treeRegIsOverride);

	if (getenv("MC2_MODOVERRIDE_TRACE") && appearType->treeRenderShape[0]) {
		TG_TypeMultiShape* rs = appearType->treeRenderShape[0];
		fprintf(stderr, "[MODOVERRIDE_TRACE] tree registerStatic name=%s renderShape=%p numShapes=%ld leaf0=%p leaf1=%p lodCount=%ld\n",
		        appearType->name, (void*)rs, rs->GetNumShapes(),
		        rs->GetNumShapes() > 0 ? (void*)rs->GetTypeNode(0) : nullptr,
		        rs->GetNumShapes() > 1 ? (void*)rs->GetTypeNode(1) : nullptr,
		        appearType->treeRenderShapeLodCount);
		fflush(stderr);
	}

	// Compute transform — same coordinate convention as TreeAppearance::update().
	// yaw includes the per-instance yaw offset (matches first-render path exactly).
	float yawAngle = (rotation * DEGREES_TO_RADS) + (yaw * DEGREES_TO_RADS);
	float pitchAngle = (pitch * DEGREES_TO_RADS);
	Stuff::UnitQuaternion rot;
	rot = Stuff::EulerAngles(pitchAngle, yawAngle, 0.0f);
	Stuff::Point3D xlatPosition;
	xlatPosition.x = -position.x;
	xlatPosition.y = TerrainRuntime::groundElevation(position);
	xlatPosition.z = position.y;

	// TREE-OVERRIDE-LOD-MVP M-A (load-bearing): build the recipe AND capture the
	// light slot per LOD FROM getTreeRenderShape(lod)'s OWN geometry. If we only
	// swapped recipeIndex while the recipe geometry stayed LOD0, per-LOD
	// selection (Task 5) would draw the SAME mesh → no perf win, and the
	// IsStaticNow shape-key would break. So each populated LOD gets its own
	// transformed render-instance, its own buildRecipeFromShape pass, its own
	// syncStaticProp recipe index, and (via the post-register bake re-arm) its
	// own permanent baked light slot.
	//
	// LOD0 reuses the existing per-instance `treeShape` (which is
	// CreateFrom(getTreeRenderShape(0))) so the LOD0 recipe + the IsStaticNow
	// `staticReg[0].shape == treeShape` key are byte-identical to pre-LOD
	// behavior. Higher LODs build from a throwaway CreateFrom(getTreeRenderShape
	// (lod)) instance that is freed before return — its geometry already lives in
	// the immutable per-type VBO (registered above), and only its recipe/light
	// slot are persisted in staticReg[lod]. treeShape stays collision/bounds +
	// LOD0 render; collision reads stock treeShape[] (untouched).
	//
	// M4: a buildRecipe MISS for a given LOD marks ONLY that LOD unavailable and
	// CONTINUES — never aborts the whole instance or half-populates. LOD0 MISS
	// still leaves staticReg[0].registered=false (first-render fallback covers
	// it, exactly as the single-LOD abort did before).
	bool anyRegistered = false;
	const int lodCount = _treeRegIsOverride ? (int)appearType->treeRenderShapeLodCount : 1;
	for (int lod = 0; lod < lodCount && lod < MAX_LODS; ++lod) {
		// Render-instance for THIS LOD: reuse treeShape for LOD0; temp for >0.
		TG_MultiShape* lodShape = (lod == 0)
			? treeShape
			: appearType->getTreeRenderShape(lod)->CreateFrom();
		if (!lodShape) { staticReg[lod].registered = false; continue; }

		lodShape->TransformMultiShape_BuildRecipe(&xlatPosition, &rot);

		std::vector<GpuStaticPropInstance> batch;
		const int numShapes = static_cast<int>(lodShape->GetNumShapes());
		batch.reserve(numShapes);
		int t_diag_total=0,t_diag_skip_pm=0,t_diag_skip_h=0,t_diag_skip_unreg=0,t_diag_added=0;
		bool lodMiss = false;
		for (int i = 0; i < numShapes; ++i) {
			++t_diag_total;
			const TG_ShapeRec* rec = lodShape->GetShapeRec(i);
			if (!rec || !rec->processMe || !rec->node) { ++t_diag_skip_pm; continue; }
			TG_Shape* child = rec->node;
			// 2026-05-10 fix: skip non-SHAPE_NODE helpers (mirror of submitMultiShape's
			// filter) — see BldgAppearance::registerStatic for full rationale.
			if (!child->IsShapeNode()) { ++t_diag_skip_h; continue; }
			uint32_t flags = 0;
			if (child->GetLightsOut())   flags |= (1u << 0);
			if (child->GetIsWindow())    flags |= (1u << 1);
			if (child->GetIsSpotlight()) flags |= (1u << 2);
			Stuff::Matrix4D xform(rec->shapeToWorld);
			GpuStaticPropInstance inst;
			if (!GpuStaticPropBatcher::instance().buildRecipeFromShape(
					child, xform,
					static_cast<uint32_t>(child->GetARGBHighlight()),
					static_cast<uint32_t>(child->GetFogRGB()),
					flags, &inst)) {
				++t_diag_skip_unreg;
				if (seamProbe)
					fprintf(stderr, "[SEAMPROBE] tree buildRecipe MISS name=%s lod=%d leaf=%d child=%p node=%s -> LOD unavailable\n",
						appearType->name, lod, i, (void*)child, child->getNodeName()), fflush(stderr);
				// M4: mark THIS LOD unavailable and stop building it; do NOT
				// abort the whole instance — other LODs still register.
				lodMiss = true;
				break;
			}
			if (seamProbe)
				fprintf(stderr, "[SEAMPROBE] tree buildRecipe HIT name=%s lod=%d leaf=%d child=%p node=%s typeID=%u\n",
					appearType->name, lod, i, (void*)child, child->getNodeName(), inst.typeID), fflush(stderr);
			// EDITOR-STATIC-RECIPE-FROM-TYPE-1 (mirror of BldgAppearance::registerStatic):
			// re-point the recipe at the canonical primed TYPE leaf
			// (getTreeRenderShape(lod), which carries finalized VBO geometry) instead of
			// the CreateFrom instance copy's leaf, which late-registers after the editor
			// finalize latch -> typeID with no geometry -> invisible. Editor-only no-op
			// in-game. See the building site for full rationale.
			{
				extern bool InEditor;   // mech3d.cpp
				if (InEditor && appearType) {
					TG_TypeMultiShape* typeRS = appearType->getTreeRenderShape(lod);
					if (typeRS && i < typeRS->GetNumShapes()) {
						TG_TypeNodePtr typeLeaf = typeRS->GetTypeNode(i);
						uint32_t canonId = 0, idxc = 0, fi = 0, ic = 0; int32_t bv = 0;
						if (typeLeaf
						    && batcher_typeIdForTypeShape((const void*)typeLeaf, &canonId)
						    && batcher_getTypeDrawInfo(canonId, &idxc, &fi, &bv, &ic)
						    && idxc > 0) {
							inst.typeID = canonId;
						}
					}
				}
			}
			batch.push_back(inst);
			++t_diag_added;
		}
		{
			static const bool s_trace = (getenv("MC2_TREE_REG_TRACE") != nullptr);
			static int s_treeRegLogged = 0;
			if (s_trace && s_treeRegLogged < 80) {
				++s_treeRegLogged;
				fprintf(stderr,
					"[TREE_REG_DIAG v1] appearType=%s lod=%d numShapes=%d total=%d pm_skip=%d "
					"h_skip=%d unreg_skip=%d added=%d\n",
					(appearType ? appearType->name : "<null>"), lod,
					numShapes, t_diag_total, t_diag_skip_pm, t_diag_skip_h,
					t_diag_skip_unreg, t_diag_added);
				fflush(stderr);
			}
			(void)t_diag_total; (void)t_diag_skip_pm; (void)t_diag_skip_h;
			(void)t_diag_skip_unreg; (void)t_diag_added;
		}

		int32_t regIdx = -1;
		if (!lodMiss && !batch.empty()) {
			// TREE-OVERRIDE-LOD-MVP forced-LOD FIX (2026-06-03): pass treeShape
			// (the ALWAYS-LIVE LOD0 per-instance shape) as the recipe `multi`, NOT
			// the throwaway lodShape — lodShape is deleted before the next frame, so
			// rng.multi would dangle and flush()'s stale-frame gate
			// (rng.multi->getCachedFrame() != currentFrame) would fire every frame →
			// recipe skipped → LOD1 instances drawn=0 → trees invisible. The DRAWN
			// geometry stays LOD1: it is carried by the batch entries' typeIDs (built
			// from lodShape's leaves via buildRecipeFromShape above, pointing at LOD1's
			// VBO region), not by `multi`. `multi` is used only for the frame-stamp
			// gate, the per-frame lightDataIndex fallback (shared LOD0 slot — correct),
			// and texture pins (LOD1 shares LOD0's texture set). treeShape's frame
			// stamp is refreshed every frame by touch()/update(), keeping the LOD1
			// recipe live exactly like LOD0.
			(void)GameAdapters::StaticProp::syncStaticProp(
				treeShape, batch.data(), batch.size(), &regIdx);
			if (seamProbe)
				fprintf(stderr, "[SEAMPROBE] tree syncStaticProp name=%s lod=%d batchSize=%zu regIdx=%d firstTypeID=%u\n",
					appearType->name, lod, batch.size(), (int)regIdx,
					batch.empty()?0xFFFFFFFFu:batch[0].typeID), fflush(stderr);
		}
		if (regIdx >= 0) {
			staticReg[lod].registered  = true;
			// shape key: LOD0 keys on treeShape (the live render-instance);
			// the IsStaticNow check is LOD0-only (see IsStaticNow). Higher LODs
			// have no live per-instance shape, so we record the type's render
			// shape for diagnostics only — never compared at draw time.
			// TREE-OVERRIDE-LOD-MVP K×M/forced-LOD FIX (2026-06-03): store nullptr for
			// lod>=1, NOT the throwaway lodShape (freed below). A dangling .shape made
			// IsStaticNow()'s shape-key fail for activeLOD>=1 → recovery path re-registered
			// treeShape (LOD0) into staticReg[lod] → forced LOD drew LOD0 geometry (STOP).
			// Higher-LOD .shape is never compared at draw time (IsStaticNow bypasses the
			// key when activeLOD!=0; recovery check is activeLOD==0-only).
			staticReg[lod].shape       = (lod == 0) ? treeShape : nullptr;
			staticReg[lod].recipeIndex = regIdx;
			anyRegistered = true;

			// TREE-OVERRIDE-LOD-MVP Task 3 (K×M GATE): bake the higher-LOD light
			// slot HERE, at registration, from this LOD's OWN transformed render-
			// instance. LOD0's light is baked by the next update() (activeLOD=0,
			// byte-identical to pre-LOD), but a higher LOD never becomes active
			// while activeLOD is pinned 0 (Task 5 adds distance selection), so its
			// light would otherwise never gather — and the K×M gate could not see
			// whether LOD1's per-instance bake creates a SECOND lightData_ slot or
			// dedups to LOD0's. mc2CacheOrBakeStaticGpuLight walks the hierarchy +
			// GatherGpuObjectLightDataOnly (position-keyed) and routes the content
			// through addLightDataStructure's FNV+memcmp dedup. Same world position
			// as LOD0 ⇒ byte-identical TG_HWLightsData ⇒ dedup HIT ⇒ shares LOD0's
			// lightData_ slot (the architectural PASS basis). It DOES consume a
			// distinct recipe-keyed baked slot (B grows with recipes, bounded by
			// types×LODs — expected, not instance growth). LOD0 skipped here to
			// keep the LOD0-only baseline bit-identical.
			// TREE-OVERRIDE-LOD-MVP K×M FIX (2026-06-03): do NOT bake/gather a separate
			// light slot per LOD. Higher LODs SHARE the LOD0 actor light slot -- same
			// world position => identical terrain lighting, and the gather is position-
			// keyed not geometry-keyed, so a per-LOD bake here only risked a fresh
			// per-instance slot (the K×M STOP) for zero lighting gain. update() resolves
			// staticReg[0].lightDataIndex each frame and shares it to the active LOD;
			// seed the share here so a higher LOD that becomes active before the next
			// update() still points at LOD0's slot.
			if (lod != 0) {
				staticReg[lod].lightDataIndex = staticReg[0].lightDataIndex;
				// K×M GATE (now PASS by construction): record the SHARED LOD0 slot, not a
				// per-LOD bake. The real proof is recordInstance()'s U counter staying flat
				// across LOD count.
				mc2_lightslot_trace::recordLodBakeSlot(staticReg[0].lightDataIndex);
				// SHADOW-FOLIAGE: far/impostor LODs (flat alpha cards) must NOT cast solid
				// rectangular blob shadows (shadow depth pass has no alpha discard). The near
				// LOD0 still casts when active; the far LOD's shadow is low-value anyway.
				GpuStaticPropRegistry::setRecipeNoShadow(regIdx, true);
			}
		} else {
			// M4: LOD unavailable — leave registered=false, continue.
			staticReg[lod].registered  = false;
		}

		// Free the throwaway higher-LOD render-instance (LOD0 == treeShape, kept).
		if (lod != 0 && lodShape) {
			lodShape->ClearAnimation();
			delete lodShape;
		}
	}

	if (anyRegistered) {
		// H4 fix (2026-05-06): see BldgAppearance::registerStatic for full
		// rationale. registerStatic only ran TransformMultiShape_BuildRecipe
		// (positions only); leaf TG_Shape::lightData_ is still default/zero.
		// needsFullBakeNextFrame = true forces the first post-registration
		// frame through full update() (populating lightData_) so every
		// registered LOD recipe gets its permanent baked light slot before the
		// first static replay; subsequent frames proceed via UPDATE_SKIP /
		// static-replay with valid cached data.
		// Spec: docs/superpowers/specs/2026-05-06-update-skip-touch-residual-debug-strategy.md
		needsFullBakeNextFrame = true;
		// [G1-STATIC-EAGER-LIGHT v1] Eager fallback: mirror of BldgAppearance::registerStatic.
		// See that site for full rationale. Gate default-OFF; unset = byte-identical.
		{
			extern void mc2SetBakedStaticLight(int32_t, const TG_HWLightsData&);
			extern void mc2WriteStaticLightSlot(int32_t, const TG_HWLightsData&);
			static const bool s_eagerLightBake = (getenv("MC2_GPU_CULL_STATIC_EAGER_LIGHT_BAKE") != nullptr);
			if (s_eagerLightBake) {
				TG_HWLightsData fallback;
				fallback.numLights_ = 1;
				fallback.lightColor[0][0] = 0.3f;
				fallback.lightColor[0][1] = 0.3f;
				fallback.lightColor[0][2] = 0.3f;
				fallback.lightColor[0][3] = 1.0f;
				// MERGE FIX (cook tree-LOD x gpucull G1): gpucull used a single regIdx; the cook
				// branch rewrote this fn for per-LOD recipes (staticReg[lod]). Eager-bake all.
				for (int _lod = 0; _lod < MAX_LODS; ++_lod) {
					if (staticReg[_lod].registered && staticReg[_lod].recipeIndex >= 0) {
						mc2SetBakedStaticLight(staticReg[_lod].recipeIndex, fallback);
						mc2WriteStaticLightSlot(staticReg[_lod].recipeIndex, fallback);
					}
				}
			}
		}
	}
}

// TREE-OVERRIDE-LOD-MVP Task 2: report the active LOD's registration (pinned 0).
bool TreeAppearance::isStaticRegistered() const { return staticReg[activeLOD].registered; }

int32_t TreeAppearance::getStaticRecipeIndex() const {
    return staticReg[activeLOD].registered ? staticReg[activeLOD].recipeIndex : -1;
}

// STATIC-REG-PREWARM-QUEUE-1: mission-load off-screen light bake for trees.
// Mirrors BldgAppearance::prewarmStaticLightBake — see that function for rationale.
// Trees bake LOD0 only (activeLOD is pinned 0 at mission load; Task 5 adds distance
// selection later). LOD0 is the canonical light key; other LODs share the slot.
bool TreeAppearance::prewarmStaticLightBake(Camera* cam)
{
	const int lod = 0;  // activeLOD is always 0 at mission-load prewarm time
	if (!staticReg[lod].registered || staticReg[lod].recipeIndex < 0)
		return false;
	if (!needsFullBakeNextFrame)
		return false;
	if (!treeShape)
		return false;
	if (!cam || cam->getNumLights() == 0)
		return false;

	treeShape->SetLightList(cam->getWorldLights(), cam->getNumLights());

	// xlatPosition/rot: same transform as TreeAppearance::update (line ~6129-6132).
	// Trees use pitch + yaw combined; position.y is world-Y but terrain height
	// is overridden in touchSerialCommit (registerStatic path uses position directly).
	Stuff::Point3D xlatPosition;
	xlatPosition.x = -position.x;
	xlatPosition.y =  position.z;
	xlatPosition.z =  position.y;

	float yawAngle   = (rotation * DEGREES_TO_RADS) + (yaw * DEGREES_TO_RADS);
	float pitchAngle = (pitch    * DEGREES_TO_RADS);
	Stuff::UnitQuaternion rot;
	rot = Stuff::EulerAngles(pitchAngle, yawAngle, 0.0f);

	treeShape->TransformMultiShape_HierarchyOnly(&xlatPosition, &rot);

	// Bake into permanent SSBO slot for LOD0.
	mc2CacheOrBakeStaticGpuLight(treeShape, staticReg[lod].registered, staticReg[lod].recipeIndex);
	const uint32_t lightIdx = treeShape->getCachedGpuLightIndex();
	// Share light slot across all LODs (matches update() K×M fix).
	for (int l = 0; l < MAX_LODS; ++l)
		staticReg[l].lightDataIndex = lightIdx;

	if (lightIdx != 0xFFFFFFFFu) {
		needsFullBakeNextFrame = false;
		return true;
	}
	return false;
}

// BLDG-TYPE-ANIM-GATE-FIX-1 counter accessors (header: bldg_anim_gate_counters.h).
uint32_t g_bldgAnimGate_typeIdleNowStatic()    { return s_animTypeIdleNowStatic.load(std::memory_order_relaxed); }
uint32_t g_bldgAnimGate_animStartInvalidated() { return s_animStartInvalidated; }
uint32_t g_bldgAnimGate_animStateToState()     { return s_animStateToStateGesture; }

//-----------------------------------------------------------------------------

void TreeAppearance::destroy (void)
{
	invalidateStaticRegistration(); // Stage 3.C: NULL RecipeRange::multi before treeShape is freed

	if ( treeShape )
	{
		delete treeShape;
		treeShape = NULL;
	}

	appearanceTypeList->removeAppearance(appearType);
}


//*****************************************************************************
