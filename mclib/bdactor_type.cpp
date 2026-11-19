// BldgAppearance / TreeAppearance: BldgAppearanceType and TreeAppearanceType init/destroy.
// Declared in the class headers; shared names in bdactor_internal.h.
#include "mc2_czstring.h"
#include "bdactor_internal.h"
#include "content_error.h"
#include "texture_assets.h"
#include "mc2fmt.h"
#include "gos_static_prop_batcher.h"
#include <string_view>
#include "stock_lamp_definition.h"
#include "loose_asset_priority.h"
#include <zlib.h>
#include <unordered_map>

extern char missionName[];

static bool userBuildingAsset(std::string_view name)
{
	const std::string storage(name);
	const auto* path = storage.c_str();
	const auto* owner = LookupModOwner(path);
	if (!owner || std::string_view(owner) == "mc2-industrial-kit") {
		if (const auto archiveName = LookupArchivePath(path); !archiveName.empty()) {
			static std::unordered_map<std::string, bool> userArchives;
			auto [it, inserted] = userArchives.try_emplace(archiveName, false);
			auto& [archiveKey, isUserArchive] = *it;
			if (inserted) isUserArchive = userPackedBuildingArchive(archiveName, [](const auto& file) {
				std::ifstream input(file, std::ios::binary); // path-audit: loose-only root FST container checksum
				uLong crc = crc32(0, nullptr, 0);
				char buffer[65536];
				while (input.read(buffer, sizeof(buffer)) || input.gcount())
					crc = crc32(crc, reinterpret_cast<const Bytef*>(buffer), static_cast<uInt>(input.gcount()));
				return input.eof() ? static_cast<uint32_t>(crc) : 0u;
			});
			if (isUserArchive) return true;
		}
	}
	return userLooseBuildingAsset(name, owner ? owner : "",
		[&] { return ReadArchiveBytes(path, 64u * 1024u * 1024u); });
}

static bool userBuildingShape(std::string_view name)
{
	const std::string storage(name);
	for (const char* ext : {".tgl", ".ase", ".glb", ".gltf", ".fbx", ".mcasset.json", ".package.json", ".tga", ".ktx2"})
	{
		FullPathFileName path;
		path.init(tglPath, storage.c_str(), ext);
		if (userBuildingAsset(static_cast<const char*>(path))) return true;
	}
	return false;
}

static bool userBuildingTextures(TG_TypeMultiShape* shape)
{
	if (!shape) return false;
	for (long i = 0; i < shape->GetNumTextures(); ++i)
	{
		char name[1024];
		shape->GetTextureName(i, name, sizeof(name));
		// Standalone KTX2s and edited source bitmaps can customize a stock material.
		for (int size : {32, 64, 128, 256, 512, 1024, static_cast<int>(ObjectTextureSize)})
		{
			const auto path = std::filesystem::path(tglPath) / std::to_string(size) / name;
			if (userBuildingAsset(path.string().c_str())) return true;
			auto sidecar = path;
			sidecar.replace_extension(".txm");
			if (userBuildingAsset(sidecar.string().c_str())) return true;
			sidecar.replace_extension(".ktx2");
			if (userBuildingAsset(sidecar.string().c_str())) return true;
		}
	}
	return false;
}

static void discardBldgRenderOverrides(BldgAppearanceType& type)
{
	type.bldgRenderShape.reset();
	for (auto& lod : type.bldgRenderLods) lod.reset();
	type.bldgDmgRenderShape.reset();
	type.propMaterialSource = nullptr;
}

// Imports one manifest-relative override GLB into shape and resolves its textures; false (logged)
// on any failure, and the caller frees the shape and keeps stock. Static-prop overrides carry
// their pivot already (no auto-ground) and have node transforms applied.
static bool importOverrideRenderShape(const ModelOverrideRecord& ov, std::string_view relPath,
                                      std::string_view baseName, std::string_view state, TG_TypeMultiShape* shape)
{
#ifdef ENABLE_ASSIMP_IMPORTER
	char overridePath[1024];
	mc2::fmt::to_buf(overridePath, "{}/{}", ov.manifestDir.c_str(), relPath);
	// Guard the importer: Assimp may throw (DeadlyImportError); collapse any throw to stock.
	try
	{
		const long r = ImportGeometryFromFile(overridePath, shape, /*autoGround=*/false, !ov.materials.empty(),
		                                      /*bakeNodeTransforms=*/true);
		if (r == 0 && shape->GetNumShapes() > 0)
		{
			LoadOverrideRenderShapeTextures(shape);
			return true;
		}
		mc2::fmt::print_err("[MODOVERRIDE] staticProp '{}' {}: import failed ({}), using stock render\n",
			baseName, state, mc2::fmt::safe(overridePath));
	}
	catch (...)
	{
		mc2::fmt::print_err("[MODOVERRIDE] staticProp '{}' {}: import threw ({}), using stock render\n",
			baseName, state, mc2::fmt::safe(overridePath));
	}
	mc2::content::error(mc2::content::Kind::Shape, overridePath, "declared override import failed; using stock render");
	fflush(stderr);
	return false;
#else
	mc2::content::error(mc2::content::Kind::Shape, relPath, "declared override importer disabled; using stock render");
	mc2::fmt::print_err("[MODOVERRIDE] staticProp '{}': override resolved but importer disabled, using stock render\n", baseName);
	return false;
#endif
}

void BldgAppearanceType::init (mc2::czstring fileName)
{
	AppearanceType::init(fileName);

	FullPathFileName iniName;
	iniName.init(tglPath,fileName,".ini");

	FitIniFile iniFile;
	long result = iniFile.open(iniName);
	if (result != NO_ERR)
	{
		bldgShape[0] = std::make_unique<TG_TypeMultiShape>();
		tglVerifyShapeLoaded(bldgShape[0].get(), -1, "appearance definition", iniName, &baseShapeLoadFailed);
		return;
	}
	bool userArt = userBuildingAsset(static_cast<const char*>(iniName));
	std::string lampDefinition(iniFile.fileSize(), '\0');
	iniFile.read(0, reinterpret_cast<MemoryPtr>(lampDefinition.data()), static_cast<long>(lampDefinition.size()));
	stockTerrainLamp = mc2_stock_lamp::definition(fileName, lampDefinition,
		LookupModOwner(iniName) != nullptr);

	// ASSIMP-BLDG-IMPORT-1 — optional GLB probe for buildings/trees. Mirror of
	// mech3d.cpp [Import] pattern. An optional [Import] section with Source=
	// opts this asset into LoadFromFile (glb/fbx probe) for LOD0. Stock assets
	// with no [Import] block take the unchanged LoadTGMultiShapeFromASE path.
	char importSourceBase[256] = "";
	tglReadImportSource(iniFile, importSourceBase);

	result = iniFile.seekBlock("TGLData");
	if (result != NO_ERR)
	{
		bldgShape[0] = std::make_unique<TG_TypeMultiShape>();
		tglVerifyShapeLoaded(bldgShape[0].get(), -1, "appearance definition", iniName, &baseShapeLoadFailed);
		return;
	}

	result = iniFile.readIdBoolean("SpinMe",spinMe);
	if (result != NO_ERR)
		spinMe = S_stricmp(fileName, "WalkWayPoint") == 0 ||
			S_stricmp(fileName, "RunWayPoint") == 0 ||
			S_stricmp(fileName, "JumpWayPoint") == 0;
		
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
		for (int i=0;i<MAX_LODS;i++)
		{
			char baseName[256];
			char baseLODDist[256];
			mc2::fmt::to_buf(baseName, "FileName{:d}", mc2::fmt::as_i32(i));
			mc2::fmt::to_buf(baseLODDist, "Distance{:d}", mc2::fmt::as_i32(i));

			result = iniFile.readIdString(baseName,aseFileName,511);
			if (result == NO_ERR)
			{
				userArt |= userBuildingShape(aseFileName);
				result = iniFile.readIdFloat(baseLODDist,lodDistance[i]);
				if (result != NO_ERR)
					mc2::content::error(mc2::content::Kind::Shape, static_cast<const char*>(iniName), "LOD missing distance; using default");
				// Push out LOD-swap thresholds so high-detail meshes stay visible
				// at greater zoom-out. See visual_preference_knobs.md.
				lodDistance[i] *= 5.0f;

				bldgShape[i] = std::make_unique<TG_TypeMultiShape>();
				gosASSERT(bldgShape[i] != nullptr);

				if (i == 0 && importSourceBase[0]) {
					loadBuildingImportSourceWithSidecarAxis(bldgShape[i].get(), importSourceBase, &baseShapeLoadFailed); // ASSIMP-BLDG-IMPORT-1: opt-in GLB probe
				} else if (i == 0) {
					FullPathFileName bldgName;
					bldgName.init(tglPath,aseFileName,".ase");
					// META-FIX design B PART 1: REQUIRED slot (LOD0) — checked load.
					tglLoadShapeChecked(bldgShape[i].get(), bldgName,
						"bdactor.bldg_base", &baseShapeLoadFailed);
				} else {
					FullPathFileName bldgName;
					bldgName.init(tglPath,aseFileName,".ase");
					// Declared-but-absent higher LODs are NORMAL stock-data
					// degradation (live tier1 evidence: 2storycivlivingl2.tgl
					// absent while the ini declares FileName2 — 4/5 tier1
					// missions ship this) — only LOD0 is the required slot.
					bldgShape[i]->LoadTGMultiShapeFromASE(bldgName); // TGL-LOAD-EXEMPT(declared LOD>0 absent = stock degradation; LOD0 is required)
				}

				if (!i)
					strncpy(bldgBaseName, aseFileName, sizeof(bldgBaseName) - 1);
			}
			else if (!i)
			{
				bldgShape[0] = std::make_unique<TG_TypeMultiShape>();
				tglVerifyShapeLoaded(bldgShape[0].get(), -1, "appearance definition", iniName, &baseShapeLoadFailed);
			}
		}
	}
	else
	{
		userArt |= userBuildingShape(aseFileName);
		bldgShape[0] = std::make_unique<TG_TypeMultiShape>();
		gosASSERT(bldgShape[0] != nullptr);

		if (importSourceBase[0]) {
			loadBuildingImportSourceWithSidecarAxis(bldgShape[0].get(), importSourceBase, &baseShapeLoadFailed); // ASSIMP-BLDG-IMPORT-1: opt-in GLB probe
		} else {
			FullPathFileName bldgName;
			bldgName.init(tglPath,aseFileName,".ase");
			// META-FIX design B PART 1: REQUIRED slot — checked load.
			tglLoadShapeChecked(bldgShape[0].get(), bldgName,
				"bdactor.bldg_base", &baseShapeLoadFailed);
		}

		strncpy(bldgBaseName, aseFileName, sizeof(bldgBaseName) - 1);
	}


	result = iniFile.seekBlock("TGLDamage");
	if (result == NO_ERR)
	{
		result = iniFile.readIdString("FileName",aseFileName,511);
		if (result != NO_ERR)
		{
			mc2::content::error(mc2::content::Kind::Shape, static_cast<const char*>(iniName), "missing damage FileName");
			aseFileName[0] = '\0';
		}
	
		userArt |= userBuildingShape(aseFileName);
		FullPathFileName dmgName;
		dmgName.init(tglPath,aseFileName,".ase");
	
		bldgDmgShape = std::make_unique<TG_TypeMultiShape>();
		gosASSERT(bldgDmgShape != nullptr);
		bldgDmgShape->LoadTGMultiShapeFromASE(dmgName); // TGL-LOAD-EXEMPT(optional damage shape; GetNumShapes-checked delete-on-fail)

		if (!bldgDmgShape->GetNumShapes())
		{
			bldgDmgShape.reset();
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
			// Mirror the per-instance loop (bdactor.cpp) at the
			// type-level: load textures into mc2::textures::ready(), set the
			// handles + alpha bits on the shared TG_TypeMultiShape so
			// every per-instance clone via CreateFrom inherits them.
			for (long i = 0; i < bldgDmgShape->GetNumTextures(); i++)
			{
				char txmName[1024];
				bldgDmgShape->GetTextureName(i, txmName, 256);
				char texturePath[1024];
				mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));
				FullPathFileName textureName;
				textureName.init(texturePath, txmName, "");
				if (textureOrKtxSidecarExists(textureName))
				{
					if (S_strnicmp(txmName, "a_", 2) == 0)
					{
						DWORD gosHandle = mc2::textures::load(
							textureName, gos_Texture_Alpha,
							gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosHandle != 0xffffffff);
						bldgDmgShape->SetTextureHandle(i, gosHandle);
						bldgDmgShape->SetTextureAlpha(i, true);
					}
					else
					{
						DWORD gosHandle = mc2::textures::load(
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
		bldgDmgShape = nullptr;
	}

	result = iniFile.seekBlock("TGLDestructEffect");
	if (result == NO_ERR)
	{
		result = iniFile.readIdString("FileName",destructEffect,59);
		if (result != NO_ERR)
			mc2::content::error(mc2::content::Kind::Shape, static_cast<const char*>(iniName), "missing DestructEffectName");
	
	}
	else
	{
		destructEffect[0] = 0;
	}

	for (int i=0;i<MAX_BD_ANIMATIONS;i++)
	{
		char blockId[512];
		mc2::fmt::to_buf(blockId, "Animation:{:d}", mc2::fmt::as_i32(i));
		
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
				
			for (const char* ext : {".ase", ".agl"}) {
				FullPathFileName path;
				path.init(tglPath, animName, ext);
				userArt |= userBuildingAsset(static_cast<const char*>(path));
			}
			FullPathFileName animPath;
			animPath.init(tglPath,animName,".ase");

			FullPathFileName otherPath;
			otherPath.init(tglPath,animName,".agl");

			if (fileExists(animPath) || fileExists(otherPath))
			{
				bdAnimData[i] = std::make_unique<TG_AnimateShape>();
				gosASSERT(bdAnimData[i] != nullptr);
	
				// If this animation does not exist, it is not a problem!
				// Building will simply freeze until animation is "over"
				bdAnimData[i]->LoadTGMultiShapeAnimationFromASE(animPath,bldgShape[0].get());
			}
			else
				bdAnimData[i] = nullptr;
		}
		else
		{
			bdAnimData[i] = nullptr;
		}
	}
	
	if (importSourceBase[0]) userArt |= userBuildingShape(importSourceBase);
	for (const auto& shape : bldgShape) userArt |= userBuildingTextures(shape.get());
	userArt |= userBuildingTextures(bldgDmgShape.get());

	// MODEL-OVERRIDE dual-shape: render-only overrides for static props. The stock
	// bldgShape[] / bldgDmgShape loads stay UNCHANGED (collision, passability and LOS
	// authority). A registry record supplies the intact render shape and, when it was
	// cooked with one, the destroyed render shape; the mission's conditions.json may pick
	// a worn/abandoned variant (default pristine). Any failure falls back to stock.
	bldgBaseName[sizeof(bldgBaseName) - 1] = 0;
	if (mc2::gate::get<"MC2_DEBUG_STATE_DUMP">())
		mc2::fmt::print_err("[BLDG_IDENTITY] {}|key={}\n", fileName, bldgBaseName);
	if (bldgBaseName[0])
	{
		if (mc2::gate::get<"MC2_MODOVERRIDE_TRACE">())
			mc2::fmt::print_err("[MODOVERRIDE_TRACE] staticProp '{}'\n", mc2::fmt::safe(bldgBaseName));
		const ModelOverrideRecord* ov = ModelOverrideRegistry::instance().resolve("staticProp", bldgBaseName);
		const bool userOverridesKit = ov && userArt && ov->manifestDir == "mods/mc2-industrial-kit/data/model_overrides";
		if (userOverridesKit)
			mc2::fmt::print_err("[MODOVERRIDE] building '{}': user assets take priority over bundled kit\n", fileName);
		if (ov && !userOverridesKit)
		{
			const std::string condName = ModelOverrideRegistry::instance().selectedCondition(missionName, bldgBaseName);
			const ModelOverrideCondition* cond = ov->condition(condName);
			const std::string& intactSrc = cond ? cond->intact.sourceRelPath : ov->sourceRelPath;
			const ModelOverrideChain& destroyed = (cond && cond->destroyed.present()) ? cond->destroyed : ov->destroyed;
			bldgRenderShape = std::make_unique<TG_TypeMultiShape>();
			if (!importOverrideRenderShape(*ov, intactSrc, bldgBaseName, "intact", bldgRenderShape.get()))
			{
				bldgRenderShape.reset();   // stock fallback
			}
			if (bldgRenderShape)
			{
				propMaterialSource = ov;
				const auto& lods = cond ? cond->intact.lods : ov->lods;
				for (const auto& lod : lods)
				{
					if (lod.lod <= 0 || lod.lod >= MAX_LODS || bldgRenderLods[lod.lod]) continue;
					auto shape = std::make_unique<TG_TypeMultiShape>();
					if (importOverrideRenderShape(*ov, lod.sourceRelPath, bldgBaseName, "LOD", shape.get()))
					{
						bldgRenderLods[lod.lod] = std::move(shape);
						if (lodDistance[lod.lod] <= 0) lodDistance[lod.lod] = lod.distance;
					}
				}
				if (destroyed.present())
				{
					bldgDmgRenderShape = std::make_unique<TG_TypeMultiShape>();
					if (!importOverrideRenderShape(*ov, destroyed.sourceRelPath, bldgBaseName, "destroyed", bldgDmgRenderShape.get()))
					{
						bldgDmgRenderShape.reset();   // stock damage fallback
					}
				}
				mc2::fmt::print_err("[MODOVERRIDE] staticProp '{}': render override applied (condition={} destroyed={})\n",
					mc2::fmt::safe(bldgBaseName), mc2::fmt::safe(cond ? condName.c_str() : "pristine"),
					mc2::fmt::safe(bldgDmgRenderShape ? "override" : "stock"));
				fflush(stderr);
			}
		}
	}

	// A flattened bake must never freeze a stock moving part. Keep the stock
	// render chain until every cooked LOD retains the animation's nodes.
	if (bldgRenderShape)
	{
		const char* missing = nullptr;
		for (const auto& animation : bdAnimData)
		{
			if (!animation) continue;
			for (long lod = 0; lod < MAX_LODS && !missing; ++lod)
				missing = animation->MissingAnimationNode(getBldgRenderShape(lod));
			if (missing) break;
		}
		if (missing)
		{
			mc2::fmt::print_err("[MODOVERRIDE] staticProp '{}': missing animation node '{}'; keeping stock moving parts\n",
				mc2::fmt::safe(bldgBaseName), mc2::fmt::safe(missing));
			discardBldgRenderOverrides(*this);
		}
	}
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
	strcpy(elevationNodeId, "NONE");
	if (iniFile.seekBlock("AnimationNode") == NO_ERR)
		iniFile.readIdString("ElevationNodeId", elevationNodeId, TG_NODE_ID);
	if (bldgRenderShape && S_stricmp(elevationNodeId, "NONE") != 0) {
		bool complete = true;
		for (long lod = 0; lod < MAX_LODS && complete; ++lod) {
			auto* shape = getBldgRenderShape(lod);
			for (const char* joint : {rotationalNodeId, elevationNodeId}) {
				bool found = false;
				for (long node = 0; node < shape->GetNumShapes(); ++node)
					found |= S_stricmp(shape->GetNodeId(node), joint) == 0;
				complete &= found;
			}
		}
		if (!complete) {
			mc2::fmt::print_err("[MODOVERRIDE] staticProp '{}': missing searchlight joint; keeping stock moving parts\n",
				mc2::fmt::safe(bldgBaseName));
			discardBldgRenderOverrides(*this);
		}
	}
	
	if (nFrameRate != 0.0f)
	{
		for (long i=0;i<MAX_BD_ANIMATIONS;i++)
			setFrameRate(i,nFrameRate);
	}

	numWeaponNodes = 0;
	nodeData = nullptr;
	result = iniFile.seekBlock("WeaponNode");
	if (result == NO_ERR)
	{
		nodeDataStore = std::make_unique<NodeData[]>(MAX_WEAPON_NODES);
		nodeData = nodeDataStore.get();
		
		for (int i=0;i<MAX_WEAPON_NODES;i++)
		{
			char blockId[512];
			mc2::fmt::to_buf(blockId, "WeaponNodeId{:d}", mc2::fmt::as_i32(i));
			
			char weaponName[512];
			result = iniFile.readIdString(blockId,weaponName,511);
			if (result != NO_ERR)
			{
				strcpy(weaponName,"NONE");
			}
			
			nodeData[i].setNodeId(weaponName);
			nodeData[i].weaponType = 0;
			numWeaponNodes++;
 		}
	}

	for (auto & bldgShapeElem : bldgShape)
	{
		if (bldgShapeElem)
			bldgShapeElem->SetLightRGBs(hotPinkRGB, hotGreenRGB, hotYellowRGB);
	}

	// An authored window color is the legacy signal for an occupied building.
	// Give it restrained warm foundation spill when no terrain lamp was authored.
	if (terrainLightRGB == 0xffffffff && hotPinkRGB != 0xffff00ff
		&& (hotPinkRGB & 0x00ffffff) != 0 && !isForestClump)
	{
		terrainLightRGB = 0xffe8c870;
		terrainLightIntensity = 0.12f;
		terrainLightInnerRadius = 0.0f;
		terrainLightOuterRadius = 96.0f;
	}

	// Stock-derived override GLBs carry the stock lighting tags (COLOR_0), so the
	// hot colours apply to them exactly as to the stock shapes.
	for (TG_TypeMultiShape* rs : {bldgRenderShape.get(), bldgDmgRenderShape.get()})
		if (rs && propMaterialSource && !propMaterialSource->materials.empty())
			rs->SetLightRGBs(hotPinkRGB, hotGreenRGB, hotYellowRGB);
}

void BldgAppearanceType::registerRenderShapes (void)
{
	const bool isOverride = (bldgRenderShape != nullptr);
	bool stockAlphaLighting = false;
	if (isOverride && bldgShape[0])
		for (long t = 0; t < bldgShape[0]->GetNumTextures(); ++t) {
			char name[256]{};
			bldgShape[0]->GetTextureName(t, name, sizeof(name));
			stockAlphaLighting |= S_strnicmp(name, "a_", 2) == 0;
		}
	for (int i = 0; i < MAX_LODS; ++i)
		GpuStaticPropBatcher::instance().registerMultiShape(getBldgRenderShape(i), isOverride,
			isOverride ? propMaterialSource : nullptr, false, stockAlphaLighting);
	if (isOverride) {
		// Authored LODs: the batcher picks one per instance per frame by camera distance (stock shapes have none).
		TG_TypeMultiShape* lodShapes[MAX_LODS]{};
		float lodDists[MAX_LODS]{};
		for (int i = 0; i < MAX_LODS; ++i) {
			lodShapes[i] = (i == 0 || bldgRenderLods[i]) ? getBldgRenderShape(i) : nullptr;
			lodDists[i] = lodDistance[i];
		}
		// The stock .fit LOD distances (x5) already fill lodDistance[]; the authored distances win here.
		if (propMaterialSource)
			for (const auto& lod : propMaterialSource->lods)
				if (lod.lod > 0 && lod.lod < MAX_LODS && lod.distance > 0.0f) lodDists[lod.lod] = lod.distance;
		GpuStaticPropBatcher::instance().registerLodGroup(std::span<TG_TypeMultiShape* const>(lodShapes, MAX_LODS), std::span<const float>(lodDists, MAX_LODS));
	}
	GpuStaticPropBatcher::instance().registerMultiShape(getBldgDmgRenderShape(), bldgDmgRenderShape != nullptr,
		bldgDmgRenderShape ? propMaterialSource : nullptr);
}

void BldgAppearanceType::destroy (void)
{
	AppearanceType::destroy();

	nodeDataStore.reset();
	nodeData = nullptr;

	for (auto & bldgShapeElem : bldgShape)
	{
		bldgShapeElem.reset();
	}

	// MODEL-OVERRIDE dual-shape: free the render override shape if loaded.
	discardBldgRenderOverrides(*this);

	bldgDmgShape.reset();
	
 	for (auto & bdAnimDataElem : bdAnimData)
	{
		bdAnimDataElem.reset();
	}
}

void BldgAppearanceType::setAnimation (TG_MultiShapePtr shape, DWORD animationNum)
{
	gosASSERT(shape != nullptr);
	gosASSERT(animationNum != 0xffffffff);
	gosASSERT(animationNum < MAX_BD_ANIMATIONS);

	if (bdAnimData[animationNum])
		bdAnimData[animationNum]->SetAnimationState(shape);
	else
		shape->ClearAnimation();
}

void TreeAppearanceType::init (mc2::czstring fileName)
{
	loadInstanceObserved = false;
	textureBindings.clear();
	AppearanceType::init(fileName);

	FullPathFileName iniName;
	iniName.init(tglPath,fileName,".ini");

	FitIniFile iniFile;
	
	long result = iniFile.open(iniName);
	if (result != NO_ERR)
	{
		treeShape[0] = std::make_unique<TG_TypeMultiShape>();
		tglVerifyShapeLoaded(treeShape[0].get(), -1, "appearance definition", iniName, &baseShapeLoadFailed);
		return;
	}

	// ASSIMP-TREE-IMPORT-1 — optional GLB probe for trees. Mirror of
	// mech3d.cpp [Import] pattern. LOD0 only; LOD1+ and damage stay ASE.
	char importSourceBase[256] = "";
	tglReadImportSource(iniFile, importSourceBase);

	result = iniFile.seekBlock("TGLData");
	if (result != NO_ERR)
	{
		treeShape[0] = std::make_unique<TG_TypeMultiShape>();
		tglVerifyShapeLoaded(treeShape[0].get(), -1, "appearance definition", iniName, &baseShapeLoadFailed);
		return;
	}

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
		for (long i=0;i<MAX_LODS;i++)
		{
			char baseName[256];
			char baseLODDist[256];
			mc2::fmt::to_buf(baseName, "FileName{:d}", mc2::fmt::as_i32(i));
			mc2::fmt::to_buf(baseLODDist, "Distance{:d}", mc2::fmt::as_i32(i));

			result = iniFile.readIdString(baseName,aseFileName,511);
			if (result == NO_ERR)
			{
				result = iniFile.readIdFloat(baseLODDist,lodDistance[i]);
				if (result != NO_ERR)
					mc2::content::error(mc2::content::Kind::Shape, static_cast<const char*>(iniName), "LOD missing distance; using default");
				// Push out LOD-swap thresholds so high-detail meshes stay visible
				// at greater zoom-out. See visual_preference_knobs.md.
				lodDistance[i] *= 5.0f;

				treeShape[i] = std::make_unique<TG_TypeMultiShape>();
				gosASSERT(treeShape[i] != nullptr);

				if (i == 0 && importSourceBase[0]) {
					// META-FIX design B PART 1: REQUIRED slot — checked import.
					long rc = treeShape[i]->LoadFromFile(importSourceBase); // ASSIMP-TREE-IMPORT-1: opt-in GLB probe
					tglVerifyShapeLoaded(treeShape[i].get(), rc, "bdactor.tree_import",
						importSourceBase, &baseShapeLoadFailed);
				} else if (i == 0) {
					FullPathFileName treeName;
					treeName.init(tglPath,aseFileName,".ase");
					// META-FIX design B PART 1: REQUIRED slot (LOD0) — checked load.
					tglLoadShapeChecked(treeShape[i].get(), treeName,
						"bdactor.tree_base", &baseShapeLoadFailed);
				} else {
					FullPathFileName treeName;
					treeName.init(tglPath,aseFileName,".ase");
					// Declared-but-absent higher LODs = stock degradation.
					treeShape[i]->LoadTGMultiShapeFromASE(treeName); // TGL-LOAD-EXEMPT(declared LOD>0 absent = stock degradation; LOD0 is required)
				}

				// Should only be necessary for trees.  Easy to data drive
				treeShape[i]->SetAlphaTest(true);
				treeShape[i]->SetFilter(true);

				if (!i)
					strncpy(treeBaseName, aseFileName, sizeof(treeBaseName) - 1);
			}
			else if (!i)
			{
				treeShape[0] = std::make_unique<TG_TypeMultiShape>();
				tglVerifyShapeLoaded(treeShape[0].get(), -1, "appearance definition", iniName, &baseShapeLoadFailed);
			}
		}
	}
	else
	{
		treeShape[0] = std::make_unique<TG_TypeMultiShape>();
		gosASSERT(treeShape[0] != nullptr);

		if (importSourceBase[0]) {
			// META-FIX design B PART 1: REQUIRED slot — checked import.
			long rc = treeShape[0]->LoadFromFile(importSourceBase); // ASSIMP-TREE-IMPORT-1: opt-in GLB probe
			tglVerifyShapeLoaded(treeShape[0].get(), rc, "bdactor.tree_import",
				importSourceBase, &baseShapeLoadFailed);
		} else {
			FullPathFileName treeName;
			treeName.init(tglPath,aseFileName,".ase");
			// META-FIX design B PART 1: REQUIRED slot — checked load.
			tglLoadShapeChecked(treeShape[0].get(), treeName,
				"bdactor.tree_base", &baseShapeLoadFailed);
		}

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
		if (mc2::gate::get<"MC2_MODOVERRIDE_TRACE">())
			mc2::fmt::print_err("[MODOVERRIDE_TRACE] tree '{}'\n", mc2::fmt::safe(treeBaseName));
		const ModelOverrideRecord* ov =
			ModelOverrideRegistry::instance().resolve("tree", treeBaseName);
		if (ov)
		{
            const std::string conditionName = ModelOverrideRegistry::instance().selectedCondition(missionName, treeBaseName);
            const auto* condition = ov->condition(conditionName);
            const auto& source = condition ? condition->intact.sourceRelPath : ov->sourceRelPath;
            const auto& lods = condition ? condition->intact.lods : ov->lods;
            const auto& damage = condition && condition->destroyed.present() ? condition->destroyed : ov->destroyed;
            auto import = [&](std::string_view path, std::string_view state) {
                auto shape = std::make_unique<TG_TypeMultiShape>();
                if (!importOverrideRenderShape(*ov, path, treeBaseName, state, shape.get())) return std::unique_ptr<TG_TypeMultiShape>{};
                shape->SetAlphaTest(true);
                shape->SetFilter(true);
                return shape;
            };
            auto intact = import(source, "intact");
            if (intact) {
                treeRenderShape[0] = std::move(intact);
                treeRenderShapeLodCount = 1;
                propMaterialSource = ov;
                for (const auto& lod : lods) {
                    if (lod.lod <= 0 || lod.lod >= MAX_TREE_RENDER_LODS || treeRenderShape[lod.lod]) continue;
                    auto shape = import(lod.sourceRelPath, "lod");
                    if (shape) {
                        treeRenderShape[lod.lod] = std::move(shape);
                        renderLodDistance[lod.lod] = lod.distance;
                        treeRenderShapeLodCount = std::max(treeRenderShapeLodCount, static_cast<long>(lod.lod + 1));
                    }
                }
                if (damage.present()) treeDmgRenderShape = import(damage.sourceRelPath, "destroyed");
                mc2::fmt::print_err("[MODOVERRIDE] tree '{}': render override applied (condition={} destroyed={})\n",
                    treeBaseName, conditionName.empty() ? "pristine" : conditionName.c_str(), treeDmgRenderShape ? "override" : "stock");
            }

		}
	}

	result = iniFile.seekBlock("TGLDamage");
	if (result == NO_ERR)
	{
		result = iniFile.readIdString("FileName",aseFileName,511);
		if (result != NO_ERR)
		{
			mc2::content::error(mc2::content::Kind::Shape, static_cast<const char*>(iniName), "missing damage FileName");
			aseFileName[0] = '\0';
		}
	
		FullPathFileName dmgName;
		dmgName.init(tglPath,aseFileName,".ase");
	
		treeDmgShape = std::make_unique<TG_TypeMultiShape>();
		gosASSERT(treeDmgShape != nullptr);
		treeDmgShape->LoadTGMultiShapeFromASE(dmgName); // TGL-LOAD-EXEMPT(optional damage shape; GetNumShapes-checked delete-on-fail)

		if (!treeDmgShape->GetNumShapes())
		{
			treeDmgShape.reset();
		}
		else
		{
			// TreeAppearance::setObjStatus probes every damage-shape texture on the destroy frame (cold: 181-626 ms). Prime the
			// per-generation memo here, behind the loading screen, so the destroy swap is probe-free.
			ZoneScopedN("Tree.WarmDamageProbes");
			for (long i = 0; i < treeDmgShape->GetNumTextures(); i++)
			{
				char txmName[1024];
				treeDmgShape->GetTextureName(i, txmName, 256);
				char texturePath[1024];
				mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));
				FullPathFileName textureName;
				textureName.init(texturePath, txmName, "");
				textureOrKtxSidecarExists(textureName);
			}
		}
		
	}
	else
	{
		treeDmgShape = nullptr;
	}

}

void TreeAppearanceType::registerRenderShapes()
{
    const bool overridden = treeRenderShapeLodCount > 0;
    const long count = overridden ? treeRenderShapeLodCount : MAX_LODS;
    for (long lod = 0; lod < count; ++lod)
        GpuStaticPropBatcher::instance().registerMultiShape(getTreeRenderShape(lod), overridden, overridden ? propMaterialSource : nullptr, true);
    if (overridden && treeRenderShapeLodCount > 1) {
        // The batcher re-types each instance to the LOD its main-camera distance selects; instances stay registered as LOD0.
        TG_TypeMultiShape* shapes[MAX_TREE_RENDER_LODS]{};
        float dists[MAX_TREE_RENDER_LODS]{};
        for (long lod = 0; lod < MAX_TREE_RENDER_LODS; ++lod) {
            shapes[lod] = (lod == 0 || treeRenderShape[lod]) ? getTreeRenderShape(lod) : nullptr;
            dists[lod] = renderLodDistance[lod];
        }
        GpuStaticPropBatcher::instance().registerLodGroup(std::span<TG_TypeMultiShape* const>(shapes, MAX_TREE_RENDER_LODS),
            std::span<const float>(dists, MAX_TREE_RENDER_LODS), true);
    }
    GpuStaticPropBatcher::instance().registerMultiShape(getTreeDamageRenderShape(), treeDmgRenderShape != nullptr,
        treeDmgRenderShape ? propMaterialSource : nullptr, true);
}

void TreeAppearanceType::destroy (void)
{
    textureBindings.clear();
    treeDmgRenderShape.reset();
    propMaterialSource = nullptr;
	AppearanceType::destroy();

	for (auto & treeShapeElem : treeShape)
	{
		treeShapeElem.reset();
	}

	for (auto & treeRenderShapeElem : treeRenderShape)
	{
		treeRenderShapeElem.reset();
	}
	treeRenderShapeLodCount = 0;

	treeDmgShape.reset();
	
}
