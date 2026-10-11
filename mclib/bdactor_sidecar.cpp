// BldgAppearance / TreeAppearance: building PBR sidecar loaders, footprint shadow and override render-shape texture residency.
// Declared in the class headers; shared names in bdactor_internal.h.
#include "bdactor_internal.h"
#include "mc2fmt.h"

bool buildingPbrGateEnabled()
{
	return mc2::gate::get<"MC2_BUILDING_PBR">();
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
	char* end = nullptr;
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

// META-FIX design B PART 1: failFlag threads AppearanceType::
// baseShapeLoadFailed through to the required-slot verify at the tail
// (the reviewed design's grep-verified import site the draft enumeration
// missed). NULL failFlag = verify-and-placeholder only.
void loadBuildingImportSourceWithSidecarAxis(TG_TypeMultiShape* shape, const char* importSourceBase,
                                                    bool* failFlag)
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
	// Remember the importer axis/yaw gates as typed values; the restore below
	// republishes them (an unset gate is restored as unset).
	const bool hadAxis = mc2::gate::isSet<"MC2_GLTF_AXIS">();
	const int oldAxisNumber = mc2::gate::get<"MC2_GLTF_AXIS">();
	const bool hadYaw = mc2::gate::isSet<"MC2_GLTF_YAW_DEG">();
	const float oldYawNumber = mc2::gate::get<"MC2_GLTF_YAW_DEG">();
	if (hasAxis) {
		mc2::fmt::to_buf(axisValue, "{:d}", mc2::fmt::as_i32(axis));
		_putenv_s("MC2_GLTF_AXIS", axisValue);
		mc2::gate::invalidate("MC2_GLTF_AXIS");
	}
	if (hasYaw) {
		mc2::fmt::to_buf(yawValue, "{:.3f}", yawDegrees);
		_putenv_s("MC2_GLTF_YAW_DEG", yawValue);
		mc2::gate::invalidate("MC2_GLTF_YAW_DEG");
	}

	// META-FIX design B PART 1: REQUIRED slot — checked import load.
	long rc = shape->LoadFromFile(importSourceBase);
	tglVerifyShapeLoaded(shape, rc, "bdactor.bldg_import",
		importSourceBase, failFlag);

	if (hasYaw) {
		char oldYawText[32] = {0};   // "" removes the variable
		if (hadYaw) *std::to_chars(oldYawText, oldYawText + sizeof(oldYawText) - 1, oldYawNumber).ptr = '\0';
		_putenv_s("MC2_GLTF_YAW_DEG", oldYawText);
		mc2::gate::invalidate("MC2_GLTF_YAW_DEG");
	}
	if (hasAxis) {
		char oldAxisText[16] = {0};
		if (hadAxis) *std::to_chars(oldAxisText, oldAxisText + sizeof(oldAxisText) - 1, oldAxisNumber).ptr = '\0';
		_putenv_s("MC2_GLTF_AXIS", oldAxisText);
		mc2::gate::invalidate("MC2_GLTF_AXIS");
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

bool loadBuildingPbrFromSidecar(BldgAppearanceType* type, const char* sidecarPath)
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
	return mc2::gate::get<"MC2_BUILDING_FOOTPRINT_SHADOW">();
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

void submitBuildingFootprintShadow(const BldgAppearance* appearance)
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
			if (mc2::gate::get<"MC2_MODOVERRIDE_TRACE">())
			{
				fprintf(stderr, "[MODOVERRIDE_TEX] slot=%ld name='%s' -> gosHandle=%lu alpha=%d\n",
				        i, txmName, (unsigned long)gosTextureHandle, (int)alpha);
				fflush(stderr);
			}
		}
		else
		{
			rs->SetTextureHandle(i, 0xffffffff);
			if (mc2::gate::get<"MC2_MODOVERRIDE_TRACE">())
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
