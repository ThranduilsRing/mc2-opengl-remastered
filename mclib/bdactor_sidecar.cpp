// BldgAppearance / TreeAppearance: [Import] sidecar axis/yaw and override render-shape texture residency.
// Declared in the class headers; shared names in bdactor_internal.h.
#include "mc2_czstring.h"
#include "bdactor_internal.h"
#include "texture_assets.h"
#include <optional>
#include <string_view>
#include "mc2fmt.h"

#ifndef PLATFORM_WINDOWS
#include "platform_io.h"
#endif

// The sidecar path comes from FullPathFileName (lower-cased); the asset on disk is mixed case
// (HangarGLB.mcasset.json). Windows is case-insensitive; on Linux a plain ifstream missed it, so
// the axis/yaw never applied and the GLB imported with the default orientation (hangar on its end).
// Same case-folding resolver File::open uses (mc2_native_path); Windows behaviour is unchanged.
static std::optional<std::string> readTextFile(mc2::czstring path)
{
	std::ifstream f(path, std::ios::in | std::ios::binary);
#ifndef PLATFORM_WINDOWS
	if (!f)
		f.open(mc2_native_path(path), std::ios::in | std::ios::binary);
#endif
	if (!f)
		return std::nullopt;
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

static std::optional<std::string> extractJsonString(std::string_view text, std::string_view key)
{
	std::string needle = "\"";
	needle += key;
	needle += "\"";
	size_t p = text.find(needle);
	if (p == std::string::npos)
		return std::nullopt;
	p = text.find(':', p + needle.size());
	if (p == std::string::npos)
		return std::nullopt;
	p = text.find('"', p + 1);
	if (p == std::string::npos)
		return std::nullopt;
	size_t e = text.find('"', p + 1);
	if (e == std::string::npos)
		return std::nullopt;
	return std::string(text.substr(p + 1, e - p - 1));
}

static std::optional<float> extractJsonNumber(const std::string& text, std::string_view key, float fallback)
{
	std::string needle = "\"";
	needle += key;
	needle += "\"";
	size_t p = text.find(needle);
	if (p == std::string::npos)
		return fallback;
	p = text.find(':', p + needle.size());
	if (p == std::string::npos)
		return std::nullopt;
	char* end = nullptr;
	const char* start = text.c_str() + p + 1;
	const float value = static_cast<float>(strtod(start, &end));
	if (end == start) return std::nullopt;
	return value;
}

static std::optional<int> extractBuildingPbrAxis(mc2::czstring sidecarPath)
{
	const auto text = readTextFile(sidecarPath);
	if (!text)
		return std::nullopt;

	if (const auto axisMapping = extractJsonString(*text, "axis_mapping")) {
		size_t p = axisMapping->find("MC2_GLTF_AXIS=");
		if (p != std::string::npos) {
			p += strlen("MC2_GLTF_AXIS=");
			if (p < axisMapping->size() && (*axisMapping)[p] >= '0' && (*axisMapping)[p] <= '3') {
				return (*axisMapping)[p] - '0';
			}
		}
	}

	if (const auto numericAxis = extractJsonNumber(*text, "gltf_axis", -1.0f)) {
		const int asInt = static_cast<int>(*numericAxis);
		if (asInt >= 0 && asInt <= 3) {
			return asInt;
		}
	}
	return std::nullopt;
}

static std::optional<float> extractBuildingPbrYaw(mc2::czstring sidecarPath)
{
	const auto text = readTextFile(sidecarPath);
	if (!text)
		return std::nullopt;
	return extractJsonNumber(*text, "yaw_degrees", 0.0f);
}

// META-FIX design B PART 1: failFlag threads AppearanceType::
// baseShapeLoadFailed through to the required-slot verify at the tail
// (the reviewed design's grep-verified import site the draft enumeration
// missed). NULL failFlag = verify-and-placeholder only.
void loadBuildingImportSourceWithSidecarAxis(TG_TypeMultiShape* shape, mc2::czstring importSourceBase,
                                                    bool* failFlag)
{
	if (!shape || !importSourceBase || !importSourceBase[0]) {
		return;
	}

	FullPathFileName sidecarName;
	sidecarName.init(tglPath, importSourceBase, ".mcasset.json");

	const auto axis = extractBuildingPbrAxis(sidecarName);
	const auto yawDegrees = extractBuildingPbrYaw(sidecarName);
	char axisValue[8] = {0};
	char yawValue[32] = {0};
	// Remember the importer axis/yaw gates as typed values; the restore below
	// republishes them (an unset gate is restored as unset).
	const bool hadAxis = mc2::gate::isSet<"MC2_GLTF_AXIS">();
	const int oldAxisNumber = mc2::gate::get<"MC2_GLTF_AXIS">();
	const bool hadYaw = mc2::gate::isSet<"MC2_GLTF_YAW_DEG">();
	const float oldYawNumber = mc2::gate::get<"MC2_GLTF_YAW_DEG">();
	if (axis) {
		mc2::fmt::to_buf(axisValue, "{:d}", mc2::fmt::as_i32(*axis));
		_putenv_s("MC2_GLTF_AXIS", axisValue);
		mc2::gate::invalidate("MC2_GLTF_AXIS");
	}
	if (yawDegrees) {
		mc2::fmt::to_buf(yawValue, "{:.3f}", *yawDegrees);
		_putenv_s("MC2_GLTF_YAW_DEG", yawValue);
		mc2::gate::invalidate("MC2_GLTF_YAW_DEG");
	}

	// META-FIX design B PART 1: REQUIRED slot — checked import load.
	long rc = shape->LoadFromFile(importSourceBase);
	tglVerifyShapeLoaded(shape, rc, "bdactor.bldg_import",
		importSourceBase, failFlag);

	// Mark synthetic body fill separately from authored light and window IDs.
	// The prop shader keeps this white by day and removes it at full night.
	for (long i = 0; i < shape->GetNumShapes(); ++i) {
		auto* node = shape->GetTypeNode(i);
		if (!node || node->GetNodeType() != SHAPE_NODE
			|| S_strnicmp(node->getNodeId(), "LitWin_", 6) == 0) continue;
		auto* body = static_cast<TG_TypeShape*>(node);
		auto* vertices = body->GetTypeVerticesMutable();
		for (int v = 0; v < body->GetNumTypeVertices(); ++v)
			if (vertices[v].aRGBLight == 0xffffffffu)
				vertices[v].aRGBLight = 0xfeffffffu;
	}

	if (yawDegrees) {
		char oldYawText[32] = {0};   // "" removes the variable
		if (hadYaw) *std::to_chars(oldYawText, oldYawText + sizeof(oldYawText) - 1, oldYawNumber).ptr = '\0';
		_putenv_s("MC2_GLTF_YAW_DEG", oldYawText);
		mc2::gate::invalidate("MC2_GLTF_YAW_DEG");
	}
	if (axis) {
		char oldAxisText[16] = {0};
		if (hadAxis) *std::to_chars(oldAxisText, oldAxisText + sizeof(oldAxisText) - 1, oldAxisNumber).ptr = '\0';
		_putenv_s("MC2_GLTF_AXIS", oldAxisText);
		mc2::gate::invalidate("MC2_GLTF_AXIS");
	}
}

// MODEL-OVERRIDE texture binding. The dual-shape override RENDER multishape
// (bldgRenderShape / treeRenderShape) is what the GPU static-prop batcher
// registers + draws (BldgAppearance/TreeAppearance::registerStatic). Its
// per-material texture NAMES are assigned by the Assimp importer
// (BuildTextureList -> data/tgl/<size>/<name>.tga). But those names are NOT
// resolved to GOS texture handles anywhere on the *type* shape — the stock
// per-instance loaders (~bdactor.cpp) only touch the CreateFrom'd
// per-instance shape, leaving the registered type render shape with
// gosTextureHandle=0xFFFFFFFF. The batcher then sees W<=0 and falls back to a
// borrowed (wrong) layer. Resolve them here, on the type render multishape,
// immediately after a successful override import — mirrors the stock loaders.
void LoadOverrideRenderShapeTextures(TG_TypeMultiShape* rs)
{
	if (!rs || !mc2::textures::ready())
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
		mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));

		FullPathFileName textureName;
		textureName.init(texturePath, txmName, "");

		if (textureOrKtxSidecarExists(textureName))
		{
			// "a_"-prefixed names are the engine's alpha-channel convention.
			const bool alpha = (S_strnicmp(txmName, "a_", 2) == 0);
			DWORD gosTextureHandle = mc2::textures::load(
				textureName,
				alpha ? gos_Texture_Alpha : gos_Texture_Solid,
				gosHint_DisableMipmap | gosHint_DontShrink);
			gosASSERT(gosTextureHandle != 0xffffffff);
			rs->SetTextureHandle(i, gosTextureHandle);
			rs->SetTextureAlpha(i, alpha);
			if (mc2::gate::get<"MC2_MODOVERRIDE_TRACE">())
			{
				mc2::fmt::print_err("[MODOVERRIDE_TEX] slot={:d} name='{}' -> gosHandle={:d} alpha={:d}\n", mc2::fmt::as_i32(i), mc2::fmt::safe(txmName), mc2::fmt::as_u32(static_cast<unsigned long>(gosTextureHandle)), mc2::fmt::as_i32(static_cast<int>(alpha)));
				fflush(stderr);
			}
		}
		else
		{
			rs->SetTextureHandle(i, 0xffffffff);
			if (mc2::gate::get<"MC2_MODOVERRIDE_TRACE">())
			{
				mc2::fmt::print_err("[MODOVERRIDE_TEX] slot={:d} name='{}' NOT FOUND (path={})\n", mc2::fmt::as_i32(i), mc2::fmt::safe(txmName), mc2::fmt::safe(static_cast<const char*>(textureName)));
				fflush(stderr);
			}
		}
	}
}

// EDITOR-STATIC-TEXTURE-PREWARM-1: public seam over LoadOverrideRenderShapeTextures.
// Only the editor palette prime path calls this (see bdactor.h). Keeps the helper
// static while letting the editor TU force a type render-shape's textures resident
// before its one-shot finalizeGeometry() texture probe runs. NULL-guarded by the
// helper itself (also guards mc2::textures::ready()).
void Bldg_ForceRenderShapeTexturesResident(TG_TypeMultiShape* rs)
{
	if (rs)
		LoadOverrideRenderShapeTextures(rs);
}
