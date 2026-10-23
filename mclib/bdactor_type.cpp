// BldgAppearance / TreeAppearance: BldgAppearanceType and TreeAppearanceType init/destroy.
// Declared in the class headers; shared names in bdactor_internal.h.
#include "bdactor_internal.h"
#include "texture_assets.h"
#include "mc2fmt.h"
#include "gos_static_prop_batcher.h"
#include <string_view>

extern char missionName[];

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
	fflush(stderr);
	return false;
#else
	mc2::fmt::print_err("[MODOVERRIDE] staticProp '{}': override resolved but importer disabled, using stock render\n", baseName);
	return false;
#endif
}

void BldgAppearanceType::init (const char * fileName)
{
	AppearanceType::init(fileName);

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
		for (int i=0;i<MAX_LODS;i++)
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
					STOP(("LOD %d has no distance value in file %s",i,fileName));
				// Push out LOD-swap thresholds so high-detail meshes stay visible
				// at greater zoom-out. See visual_preference_knobs.md.
				lodDistance[i] *= 5.0f;

				bldgShape[i] = new TG_TypeMultiShape;
				gosASSERT(bldgShape[i] != nullptr);

				if (i == 0 && importSourceBase[0]) {
					loadBuildingImportSourceWithSidecarAxis(bldgShape[i], importSourceBase, &baseShapeLoadFailed); // ASSIMP-BLDG-IMPORT-1: opt-in GLB probe
				} else if (i == 0) {
					FullPathFileName bldgName;
					bldgName.init(tglPath,aseFileName,".ase");
					// META-FIX design B PART 1: REQUIRED slot (LOD0) — checked load.
					tglLoadShapeChecked(bldgShape[i], bldgName,
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
				STOP(("No base LOD for shape %s",fileName));
			}
		}
	}
	else
	{
		bldgShape[0] = new TG_TypeMultiShape;
		gosASSERT(bldgShape[0] != nullptr);

		if (importSourceBase[0]) {
			loadBuildingImportSourceWithSidecarAxis(bldgShape[0], importSourceBase, &baseShapeLoadFailed); // ASSIMP-BLDG-IMPORT-1: opt-in GLB probe
		} else {
			FullPathFileName bldgName;
			bldgName.init(tglPath,aseFileName,".ase");
			// META-FIX design B PART 1: REQUIRED slot — checked load.
			tglLoadShapeChecked(bldgShape[0], bldgName,
				"bdactor.bldg_base", &baseShapeLoadFailed);
		}

		strncpy(bldgBaseName, aseFileName, sizeof(bldgBaseName) - 1);
	}

	// MODEL-OVERRIDE dual-shape: render-only overrides for static props. The stock
	// bldgShape[] / bldgDmgShape loads stay UNCHANGED (collision, passability and LOS
	// authority). A registry record supplies the intact render shape and, when it was
	// cooked with one, the destroyed render shape; the mission's conditions.json may pick
	// a worn/abandoned variant (default pristine). Any failure falls back to stock.
	bldgBaseName[sizeof(bldgBaseName) - 1] = 0;
	if (bldgBaseName[0])
	{
		if (mc2::gate::get<"MC2_MODOVERRIDE_TRACE">())
			mc2::fmt::print_err("[MODOVERRIDE_TRACE] staticProp '{}'\n", mc2::fmt::safe(bldgBaseName));
		if (const ModelOverrideRecord* ov = ModelOverrideRegistry::instance().resolve("staticProp", bldgBaseName))
		{
			const std::string condName = ModelOverrideRegistry::instance().selectedCondition(missionName, bldgBaseName);
			const ModelOverrideCondition* cond = ov->condition(condName);
			const std::string& intactSrc = cond ? cond->intact.sourceRelPath : ov->sourceRelPath;
			const ModelOverrideChain& destroyed = (cond && cond->destroyed.present()) ? cond->destroyed : ov->destroyed;
			bldgRenderShape = new TG_TypeMultiShape;
			if (!importOverrideRenderShape(*ov, intactSrc, bldgBaseName, "intact", bldgRenderShape))
			{
				delete bldgRenderShape;
				bldgRenderShape = nullptr;   // stock fallback
			}
			if (bldgRenderShape)
			{
				propMaterialSource = ov;
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

	result = iniFile.seekBlock("TGLDamage");
	if (result == NO_ERR)
	{
		result = iniFile.readIdString("FileName",aseFileName,511);
		if (result != NO_ERR)
			Fatal(result,"Could not find ASE FileName in building appearance INI file");
	
		FullPathFileName dmgName;
		dmgName.init(tglPath,aseFileName,".ase");
	
		bldgDmgShape = new TG_TypeMultiShape;
		gosASSERT(bldgDmgShape != nullptr);
		bldgDmgShape->LoadTGMultiShapeFromASE(dmgName); // TGL-LOAD-EXEMPT(optional damage shape; GetNumShapes-checked delete-on-fail)

		if (!bldgDmgShape->GetNumShapes())
		{
			delete bldgDmgShape;
			bldgDmgShape = nullptr;
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
			STOP(("Could not Find DestructEffectName in building appearance INI file"));
	
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
				
			FullPathFileName animPath;
			animPath.init(tglPath,animName,".ase");

			FullPathFileName otherPath;
			otherPath.init(tglPath,animName,".agl");

			if (fileExists(animPath) || fileExists(otherPath))
			{
				bdAnimData[i] = new TG_AnimateShape;
				gosASSERT(bdAnimData[i] != nullptr);
	
				// If this animation does not exist, it is not a problem!
				// Building will simply freeze until animation is "over"
				bdAnimData[i]->LoadTGMultiShapeAnimationFromASE(animPath,bldgShape[0]);
			}
			else
				bdAnimData[i] = nullptr;
		}
		else
		{
			bdAnimData[i] = nullptr;
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

	for (int i=0;i<MAX_LODS;i++)
	{
		if (bldgShape[i])
			bldgShape[i]->SetLightRGBs(hotPinkRGB, hotGreenRGB, hotYellowRGB);
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
	for (TG_TypeMultiShape* rs : {bldgRenderShape, bldgDmgRenderShape.get()})
		if (rs && propMaterialSource && !propMaterialSource->materials.empty())
			rs->SetLightRGBs(hotPinkRGB, hotGreenRGB, hotYellowRGB);
}

void BldgAppearanceType::registerRenderShapes (void)
{
	const bool isOverride = (bldgRenderShape != nullptr);
	for (int i = 0; i < MAX_LODS; ++i)
		GpuStaticPropBatcher::instance().registerMultiShape(getBldgRenderShape(i), isOverride,
			isOverride ? propMaterialSource : nullptr);
	GpuStaticPropBatcher::instance().registerMultiShape(getBldgDmgRenderShape(), bldgDmgRenderShape != nullptr,
		bldgDmgRenderShape ? propMaterialSource : nullptr);
}

void BldgAppearanceType::destroy (void)
{
	AppearanceType::destroy();

	nodeDataStore.reset();
	nodeData = nullptr;

	for (long i=0;i<MAX_LODS;i++)
	{
		if (bldgShape[i])
		{
			delete bldgShape[i];
			bldgShape[i] = nullptr;
		}
	}

	// MODEL-OVERRIDE dual-shape: free the render override shape if loaded.
	if (bldgRenderShape)
	{
		delete bldgRenderShape;
		bldgRenderShape = nullptr;
	}
	bldgDmgRenderShape.reset();
    propMaterialSource = nullptr;

 	if (bldgDmgShape)
	{
		delete bldgDmgShape;
		bldgDmgShape = nullptr;
	}
	
 	for (int i=0;i<MAX_BD_ANIMATIONS;i++)
	{
		if (bdAnimData[i])
		{
			delete bdAnimData[i];
			bdAnimData[i] = nullptr;
		}
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
					STOP(("LOD %d has no distance value in file %s",i,fileName));
				// Push out LOD-swap thresholds so high-detail meshes stay visible
				// at greater zoom-out. See visual_preference_knobs.md.
				lodDistance[i] *= 5.0f;

				treeShape[i] = new TG_TypeMultiShape;
				gosASSERT(treeShape[i] != nullptr);

				if (i == 0 && importSourceBase[0]) {
					// META-FIX design B PART 1: REQUIRED slot — checked import.
					long rc = treeShape[i]->LoadFromFile(importSourceBase); // ASSIMP-TREE-IMPORT-1: opt-in GLB probe
					tglVerifyShapeLoaded(treeShape[i], rc, "bdactor.tree_import",
						importSourceBase, &baseShapeLoadFailed);
				} else if (i == 0) {
					FullPathFileName treeName;
					treeName.init(tglPath,aseFileName,".ase");
					// META-FIX design B PART 1: REQUIRED slot (LOD0) — checked load.
					tglLoadShapeChecked(treeShape[i], treeName,
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
				STOP(("No base LOD for shape %s",fileName));
			}
		}
	}
	else
	{
		treeShape[0] = new TG_TypeMultiShape;
		gosASSERT(treeShape[0] != nullptr);

		if (importSourceBase[0]) {
			// META-FIX design B PART 1: REQUIRED slot — checked import.
			long rc = treeShape[0]->LoadFromFile(importSourceBase); // ASSIMP-TREE-IMPORT-1: opt-in GLB probe
			tglVerifyShapeLoaded(treeShape[0], rc, "bdactor.tree_import",
				importSourceBase, &baseShapeLoadFailed);
		} else {
			FullPathFileName treeName;
			treeName.init(tglPath,aseFileName,".ase");
			// META-FIX design B PART 1: REQUIRED slot — checked load.
			tglLoadShapeChecked(treeShape[0], treeName,
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
#ifdef ENABLE_ASSIMP_IMPORTER
			// TREE-OVERRIDE-LOD-MVP Task 1: the existing import fills LOD0 only.
			// Higher LODs stay NULL until Task 3 populates them; LOD0-only ⇒
			// getTreeRenderShape(any) → LOD0 → identical to pre-LOD behavior.
			treeRenderShape[0] = new TG_TypeMultiShape;
			char overridePath[1024];
			mc2::fmt::to_buf(overridePath, "{}/{}", ov->manifestDir.c_str(), ov->sourceRelPath.c_str());
			// Guard the importer: Assimp may throw (DeadlyImportError). A throw
			// here would leak the freshly-new'd render shape and unwind into
			// non-exception-safe engine code, so collapse any throw to stock.
			try
			{
				long r = ImportGeometryFromFile(overridePath, treeRenderShape[0]);
				if (r != 0 || treeRenderShape[0]->GetNumShapes() == 0)
				{
					delete treeRenderShape[0]; treeRenderShape[0] = nullptr;   // stock fallback
					treeRenderShapeLodCount = 0;
					mc2::fmt::print_err("[MODOVERRIDE] tree '{}': import failed ({}), using stock render\n", mc2::fmt::safe(treeBaseName), mc2::fmt::safe(overridePath));
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
					treeRenderShapeLodCount = 1;
					mc2::fmt::print_err("[MODOVERRIDE] tree '{}': render override applied ({})\n", mc2::fmt::safe(treeBaseName), mc2::fmt::safe(overridePath));
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
							mc2::fmt::print_err("[MODOVERRIDE] tree '{}': lod {:d} out of range, skipped\n", mc2::fmt::safe(treeBaseName), mc2::fmt::as_i32(li));
							fflush(stderr);
							continue;
						}
						if (treeRenderShape[li]) {
							mc2::fmt::print_err("[MODOVERRIDE] tree '{}': duplicate lod {:d}, skipped\n", mc2::fmt::safe(treeBaseName), mc2::fmt::as_i32(li));
							fflush(stderr);
							continue;
						}
						char lodPath[1024];
						mc2::fmt::to_buf(lodPath, "{}/{}", ov->manifestDir.c_str(), lentry.sourceRelPath.c_str());
						treeRenderShape[li] = new TG_TypeMultiShape;
						try {
							long lr = ImportGeometryFromFile(lodPath, treeRenderShape[li]);
							if (lr != 0 || treeRenderShape[li]->GetNumShapes() == 0) {
								delete treeRenderShape[li]; treeRenderShape[li] = nullptr;
								mc2::fmt::print_err("[MODOVERRIDE] tree '{}': lod {:d} import failed ({}), dropped\n", mc2::fmt::safe(treeBaseName), mc2::fmt::as_i32(li), mc2::fmt::safe(lodPath));
								fflush(stderr);
								continue;
							}
							treeRenderShape[li]->SetAlphaTest(true);
							treeRenderShape[li]->SetFilter(true);
							LoadOverrideRenderShapeTextures(treeRenderShape[li]);
							if (li + 1 > treeRenderShapeLodCount)
								treeRenderShapeLodCount = li + 1;
							mc2::fmt::print_err("[MODOVERRIDE] tree '{}': lod {:d} applied ({}) lodCount={:d}\n", mc2::fmt::safe(treeBaseName), mc2::fmt::as_i32(li), mc2::fmt::safe(lodPath), mc2::fmt::as_i32(treeRenderShapeLodCount));
							fflush(stderr);
						}
						catch (...) {
							delete treeRenderShape[li]; treeRenderShape[li] = nullptr;
							mc2::fmt::print_err("[MODOVERRIDE] tree '{}': lod {:d} import threw ({}), dropped\n", mc2::fmt::safe(treeBaseName), mc2::fmt::as_i32(li), mc2::fmt::safe(lodPath));
							fflush(stderr);
						}
					}
				}
			}
			catch (...)
			{
				delete treeRenderShape[0]; treeRenderShape[0] = nullptr;   // stock fallback
				treeRenderShapeLodCount = 0;
				mc2::fmt::print_err("[MODOVERRIDE] tree '{}': import threw ({}), using stock render\n", mc2::fmt::safe(treeBaseName), mc2::fmt::safe(overridePath));
				fflush(stderr);
			}
#else
			mc2::fmt::print_err("[MODOVERRIDE] tree '{}': override resolved but importer disabled, using stock render\n", mc2::fmt::safe(treeBaseName));
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
		gosASSERT(treeDmgShape != nullptr);
		treeDmgShape->LoadTGMultiShapeFromASE(dmgName); // TGL-LOAD-EXEMPT(optional damage shape; GetNumShapes-checked delete-on-fail)

		if (!treeDmgShape->GetNumShapes())
		{
			delete treeDmgShape;
			treeDmgShape = nullptr;
		}
		
	}
	else
	{
		treeDmgShape = nullptr;
	}

}

void TreeAppearanceType::destroy (void)
{
	AppearanceType::destroy();

	for (long i=0;i<MAX_LODS;i++)
	{
		if (treeShape[i])
		{
			delete treeShape[i];
			treeShape[i] = nullptr;
		}
	}

	for (long i = 0; i < MAX_LODS; i++)
	{
		if (treeRenderShape[i])
		{
			delete treeRenderShape[i];
			treeRenderShape[i] = nullptr;
		}
	}
	treeRenderShapeLodCount = 0;

	if (treeDmgShape)
	{
		delete treeDmgShape;
		treeDmgShape = nullptr;
	}
	
}
