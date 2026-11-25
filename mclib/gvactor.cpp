//---------------------------------------------------------------------------
//
//	gvactor.cpp - This file contains the code for the Ground Vehicle Actor class
#include "mc2_czstring.h"
#include "content_error.h"
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "texture_assets.h"
#include "../GameOS/gameos/gos_profiler.h"
#include "gvactor_internal.h"
#include "render_snap.h"
#include "texture_probe_memo.h"
#include "Gates/gate.h"
#include "../RenderCore/axis_convert.h"
#include "mc2fmt.h"
#include "model_override_registry.h"
#include "assimp_importer.h"
#include "loose_asset_priority.h"
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include "vfs.h"

void LoadOverrideRenderShapeTextures(TG_TypeMultiShape* shape);

static bool textureOrKtxSidecarExists(mc2::czstring tgaPath);

namespace {
// GVAppearance::init probes each texture with fileExists (a full vfs open) per instance: 11 ms of sensor-shape probes per
// ejected pilot on the sim worker (vehicle-hitch-1). Texture presence is fixed for the process, so each path is probed once.
bool gvTextureExistsCached(mc2::czstring path, bool orKtxSidecar)
{
    static std::mutex mutex;
    static std::unordered_map<std::string, bool> cache;
    std::string key(path);
    key += orKtxSidecar ? "|k" : "|t";
    {
        std::lock_guard<std::mutex> lock(mutex);
        static uint64_t seen = 0;
        if (seen != mc2::vfs::generation()) { cache.clear(); seen = mc2::vfs::generation(); }
        if (cache.contains(key))
            return cache.at(key);
    }
    const bool exists = orKtxSidecar ? textureOrKtxSidecarExists(path) : (fileExists(path) != 0);
    std::lock_guard<std::mutex> lock(mutex);
    cache.emplace(std::move(key), exists);
    return exists;
}
} // namespace


// C3: env-gated lifecycle routing killswitch (same env var as objmgr.cpp).
// MC2_GPU_CULL_LIFECYCLE=1 enables GPU visibility-based node-position early-outs.
const bool s_gpuCullLifecycle = false;


// MC2_GPU_CULL_LIFECYCLE_TRACE=1: verbose per-actor lifecycle boundary prints.
static const bool s_lcTraceGV = false;
static uint32_t s_lcSkipCountGV = 0u;
#define LCGV_TRACE(fmt, ...) \
    do { if (s_lcTraceGV) { printf("[GPU_CULL_LIFECYCLE v1] gvactor " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while(0)




// Returns true if the texture file or its .ktx2 BC7 sidecar exists.
// Sensor shape loaders (sensorTriangleShape, sensorCircleShape) use this so
// a_contacts.tga can be absent in a slim deploy while the .ktx2 sidecar serves
// the texture via route-2 in txmmgr.cpp.
static bool textureOrKtxSidecarExists(mc2::czstring tgaPath)
{
	if (fileExists(tgaPath))
		return true;
	const bool ktxPrimary = (1 != 0);   // default mode 1
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
// Explicit teardown precedes TglRegistry/heap destruction; never own these at exit.
TG_TypeMultiShapePtr GVAppearanceType::SensorTriangleShape = nullptr;
TG_TypeMultiShapePtr GVAppearanceType::SensorCircleShape = nullptr;
//-----------------------------------------------------------------------------
// class GVAppearanceType
void GVAppearanceType::init (mc2::czstring fileName)
{
	AppearanceType::init(fileName);

	//----------------------------------------------
	FullPathFileName iniName;
	iniName.init(tglPath,fileName,".ini");

	FitIniFile iniFile;
	long result = iniFile.open(iniName);
	if (result != NO_ERR)
	{
		gvShape[0] = std::make_unique<TG_TypeMultiShape>();
		tglVerifyShapeLoaded(gvShape[0].get(), -1, "appearance definition", iniName, &baseShapeLoadFailed);
		return;
	}

	// ASSIMP-GV-IMPORT-1 — optional GLB probe for vehicles. Mirror of
	// mech3d.cpp [Import] pattern. LOD0 only; shadow/damage shapes stay ASE.
	char importSourceBase[256] = "";
	tglReadImportSource(iniFile, importSourceBase);

	result = iniFile.seekBlock("TGLData");
	if (result != NO_ERR)
	{
		gvShape[0] = std::make_unique<TG_TypeMultiShape>();
		tglVerifyShapeLoaded(gvShape[0].get(), -1, "appearance definition", iniName, &baseShapeLoadFailed);
		return;
	}

	char aseFileName[512];
	result = iniFile.readIdString("FileName",aseFileName,511);
	if (result != NO_ERR)
	{
		//Check for LOD filenames instead
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

				//----------------------------------------------
				// Base LOD shape.  In stand Pose by default.
				gvShape[i] = std::make_unique<TG_TypeMultiShape>();
				gosASSERT(gvShape[i] != nullptr);

				if (i == 0 && importSourceBase[0]) {
					// META-FIX design B PART 1: REQUIRED slot — checked import.
					long rc = gvShape[i]->LoadFromFile(importSourceBase); // ASSIMP-GV-IMPORT-1: opt-in GLB probe
					tglVerifyShapeLoaded(gvShape[i].get(), rc, "gvactor.base_import",
						importSourceBase, &baseShapeLoadFailed);
				} else if (i == 0) {
					FullPathFileName gvName;
					gvName.init(tglPath,aseFileName,".ase");
					// META-FIX design B PART 1: REQUIRED slot (LOD0) — checked load.
					tglLoadShapeChecked(gvShape[i].get(), gvName,
						"gvactor.base", &baseShapeLoadFailed);
				} else {
					FullPathFileName gvName;
					gvName.init(tglPath,aseFileName,".ase");
					// Declared-but-absent higher LODs = stock degradation.
					gvShape[i]->LoadTGMultiShapeFromASE(gvName); // TGL-LOAD-EXEMPT(declared LOD>0 absent = stock degradation; LOD0 is required)
				}
			}
			else if (!i)
			{
				gvShape[0] = std::make_unique<TG_TypeMultiShape>();
				tglVerifyShapeLoaded(gvShape[0].get(), -1, "appearance definition", iniName, &baseShapeLoadFailed);
			}
		}
	}
	else
	{
		//----------------------------------------------
		// Base shape.  In stand Pose by default.
		gvShape[0] = std::make_unique<TG_TypeMultiShape>();
		gosASSERT(gvShape[0] != nullptr);

		if (importSourceBase[0]) {
			// META-FIX design B PART 1: REQUIRED slot — checked import.
			long rc = gvShape[0]->LoadFromFile(importSourceBase); // ASSIMP-GV-IMPORT-1: opt-in GLB probe
			tglVerifyShapeLoaded(gvShape[0].get(), rc, "gvactor.base_import",
				importSourceBase, &baseShapeLoadFailed);
		} else {
			FullPathFileName gvName;
			gvName.init(tglPath,aseFileName,".ase");
			// META-FIX design B PART 1: REQUIRED slot — checked load.
			tglLoadShapeChecked(gvShape[0].get(), gvName,
				"gvactor.base", &baseShapeLoadFailed);
		}
	}

	// Keep every stock shape: simulation nodes, picking and bounds never use the kit.
	const auto* iniOwner = LookupModOwner(iniName);
	const bool userIni = userLooseBuildingAsset(static_cast<const char*>(iniName), iniOwner ? iniOwner : "",
		[&] { return ReadArchiveBytes(iniName, 1024u * 1024u); });
	if (!importSourceBase[0])
	if (const auto* ov = ModelOverrideRegistry::instance().resolve("vehicle", fileName))
	{
		const std::string manifestPath = (std::filesystem::path(ov->manifestDir) / "models.json").generic_string();
		const auto* overrideOwner = LookupModOwner(manifestPath.c_str());
		if (!userIni || (iniOwner && overrideOwner && strcmp(iniOwner, overrideOwner) == 0))
		{
#ifdef ENABLE_ASSIMP_IMPORTER
		bool complete = ov->lods.size() == 2 && ov->lods[0].lod == 1 && ov->lods[1].lod == 2;
		for (int i = 0; complete && i < 3; ++i)
		{
			const auto rel = i == 0 ? ov->sourceRelPath : ov->lods[i-1].sourceRelPath;
			auto shape = std::make_unique<TG_TypeMultiShape>();
			try {
				const auto path = (std::filesystem::path(ov->manifestDir) / rel).generic_string();
				complete = ImportGeometryFromFile(path.c_str(), shape.get(), false, true, true) == 0
					&& shape->GetNumShapes() > 0;
			} catch (...) { complete = false; }
			if (complete) {
				LoadOverrideRenderShapeTextures(shape.get());
				renderLods[i] = std::move(shape);
				renderLodDistance[i] = i == 0 ? 0 : ov->lods[i-1].distance;
			}
		}
		if (!complete) for (auto& shape : renderLods) shape.reset();
		mc2::fmt::print_err("[VEHICLE_OVERRIDE] {}: {} (stock simulation/pick)\n", fileName, complete ? "3 LODs" : "stock fallback");
#endif
		}
	}

	result = iniFile.readIdString("ShadowName",aseFileName,511);
	if (result == NO_ERR)
	{
		//----------------------------------------------
		// Base Shadow shape.
		gvShadowShape = std::make_unique<TG_TypeMultiShape>();
		gosASSERT(gvShadowShape != nullptr);
	
		FullPathFileName gvName;
		gvName.init(tglPath,aseFileName,".ase");
	
		gvShadowShape->LoadTGMultiShapeFromASE(gvName); // TGL-LOAD-EXEMPT(optional shadow; loader-side PART-0 logging)
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
	
		gvDmgShape = std::make_unique<TG_TypeMultiShape>();
		gosASSERT(gvDmgShape != nullptr);
		gvDmgShape->LoadTGMultiShapeFromASE(dmgName); // TGL-LOAD-EXEMPT(optional damage shape; GetNumShapes-checked delete-on-fail)

		if (!gvDmgShape->GetNumShapes())
		{
			gvDmgShape.reset();
		}
		else
		{
			preloadDamageShapeTextures(gvDmgShape.get(), false);
		}
	}
	else
	{
		gvDmgShape = nullptr;
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

	//--------------------------------------------------------------------
	// Load Animation Information.
	// We can load up to 10 Animation States.
	for (long i=0;i<MAX_GV_ANIMATIONS;i++)
	{
		char blockId[512];
		mc2::fmt::to_buf(blockId, "Animation:{:d}", mc2::fmt::as_i32(i));
		
		result = iniFile.seekBlock(blockId);
		if (result == NO_ERR)
		{
			char animName[512];
			result = iniFile.readIdString("AnimationName",animName,511);
			gosASSERT(result == NO_ERR);
			
			result = iniFile.readIdBoolean("LoopAnimation",gvAnimLoop[i]);
			gosASSERT(result == NO_ERR);
			
			result = iniFile.readIdBoolean("Reverse",gvReverse[i]);
			gosASSERT(result == NO_ERR);
			
			result = iniFile.readIdBoolean("Random",gvRandom[i]);
			gosASSERT(result == NO_ERR);
			
			result = iniFile.readIdLong("StartFrame",gvStartF[i]);
			if (result != NO_ERR)
				gvStartF[i] = 0;
				
 			//-------------------------------
			// We have an animation to load.
			FullPathFileName animPath;
			animPath.init(tglPath,animName,".ase");

			FullPathFileName otherPath;
			otherPath.init(tglPath,animName,".agl");

			if (fileExists(animPath) || fileExists(otherPath))
			{
				gvAnimData[i] = std::make_unique<TG_AnimateShape>();
				gosASSERT(gvAnimData[i] != nullptr);
	
				//--------------------------------------------------------
				// If this animation does not exist, it is not a problem!
				// Building will simply freeze until animation is "over"
				gvAnimData[i]->LoadTGMultiShapeAnimationFromASE(animPath,gvShape[0].get());
			}
			else
				gvAnimData[i] = nullptr;
		}
		else
		{
			gvAnimData[i] = nullptr;
		}
	}
	
 	//--------------------------------------------------------------------
	// We can also load the node to yaw for vehicles with turrets.
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

	// An override body must keep the stock turret node (the render pose rotates it by name); a body without it
	// falls back to the stock shape rather than drawing a frozen turret.
	if (renderLods[0] && S_stricmp(rotationalNodeId, "NONE") != 0)
	{
		const char* turretName = S_stricmp(rotationalNodeId, "sc_turret") == 0 ? "Centipede_turret" : rotationalNodeId;
		for (int i = 0; i < MAX_LODS; ++i)
		{
			if (!renderLods[i]) continue;
			bool found = false;
			for (long n = 0; n < renderLods[i]->GetNumShapes() && !found; ++n)
			{
				const char* id = renderLods[i]->GetNodeId(n);
				found = id && S_stricmp(id, turretName) == 0;
			}
			if (!found)
			{
				mc2::fmt::print_err("[VEHICLE_OVERRIDE] {}: lod {} lacks stock node '{}'; stock fallback\n", fileName, i, turretName);
				for (auto& shape : renderLods) shape.reset();
				break;
			}
		}
	}

	//-----------------------------------------------
	// Load up the Node Data.
	// Automatic for vehicles now.
	numSmokeNodes = MAX_SMOKE_NODES;
	numWeaponNodes = MAX_WEAPON_NODES;
	numFootNodes = MAX_FOOT_NODES;
		
	nodeDataStore = std::make_unique<NodeData[]>(numWeaponNodes+numSmokeNodes+numFootNodes);
	nodeData = nodeDataStore.get();
		
	for (int i=1;i<=numSmokeNodes;i++)
	{
		char blockId[512];
		mc2::fmt::to_buf(blockId, "Smoke_{:02d}", mc2::fmt::as_i32(i));
			
		nodeData[i-1].setNodeId(blockId);
		nodeData[i-1].weaponType = MECH3D_WEAPONTYPE_NONE;
	}
		
	for (int i=1;i<=numWeaponNodes;i++)
	{
		char blockId[512];
		mc2::fmt::to_buf(blockId, "Weapon_{:02d}", mc2::fmt::as_i32(i));
		
		nodeData[(i-1)+numSmokeNodes].setNodeId(blockId);
		nodeData[(i-1)+numSmokeNodes].weaponType = MECH3D_WEAPONTYPE_ANY;
		
	}
	
	for (int i=1;i<=numFootNodes;i++)
	{
		char blockId[512];
		mc2::fmt::to_buf(blockId, "FootNode_{:02d}", mc2::fmt::as_i32(i));
			
		nodeData[(i-1)+numSmokeNodes+numWeaponNodes].setNodeId(blockId);
		nodeData[(i-1)+numSmokeNodes+numWeaponNodes].weaponType = MECH3D_WEAPONTYPE_NONE;
	}
 	
 	//----------------------------------------------
	// Load up sensor Shape if not yet defined.
	if (SensorCircleShape == nullptr)
	{
		FullPathFileName sensorName;
		sensorName.init(tglPath,"circularcontact",".ase");
	
		SensorCircleShape = new TG_TypeMultiShape;
		gosASSERT(SensorCircleShape != nullptr);

		// META-FIX design B PART 1: REQUIRED static (sensor contact art).
		tglLoadShapeChecked(SensorCircleShape, sensorName,
			"gvactor.sensor_circle", nullptr);
	}

	if (SensorTriangleShape == nullptr)
	{
		FullPathFileName sensorName;
		sensorName.init(tglPath,"trianglecontact",".ase");
	
		SensorTriangleShape = new TG_TypeMultiShape;
		gosASSERT(SensorCircleShape != nullptr);

		tglLoadShapeChecked(SensorTriangleShape, sensorName,
			"gvactor.sensor_triangle", nullptr);
	}
}

//----------------------------------------------------------------------------
void GVAppearanceType::destroy (void)
{
	for (auto& variant : renderVariants)
		for (long i = 0; i < variant.shape->GetNumTextures(); ++i) {
			const auto handle = variant.shape->GetTextureHandle(i);
			if (handle != 0xffffffff) mc2::textures::release(handle);
		}
	renderVariants.clear();
	for (auto& shape : renderLods) shape.reset();
	AppearanceType::destroy();

	for (auto & gvShapeElem : gvShape)
	{
		gvShapeElem.reset();
	}

	gvShadowShape.reset();

	gvDmgShape.reset();

	for (auto & gvAnimDataElem : gvAnimData)
	{
		gvAnimDataElem.reset();
	}

	nodeDataStore.reset();
	nodeData = nullptr;
}

//-----------------------------------------------------------------------------
void GVAppearanceType::setAnimation (TG_MultiShapePtr shape, DWORD animationNum)
{
	gosASSERT(shape != nullptr);
	gosASSERT(animationNum != 0xffffffff);
	gosASSERT(animationNum < MAX_GV_ANIMATIONS);

	if (gvAnimData[animationNum])
		gvAnimData[animationNum]->SetAnimationState(shape);
	else
		shape->ClearAnimation();
}

//-----------------------------------------------------------------------------

glm::vec3 debugGVActorPosition;
float vehicleDebugAngle = 0.0;
float turretDebugAngle = 0.0;
float debugVelMag = 0.0;
//-----------------------------------------------------------------------------
// class GVAppearance
void GVAppearance::setWeaponNodeUsed (long weaponNode)
{
	weaponNode -= appearType->numSmokeNodes;
   	if ((weaponNode >= 0) && (weaponNode < appearType->numWeaponNodes))
	{
		unitAnim().nodeUsed[weaponNode]++;
		unitAnim().nodeRecycle[weaponNode] = BASE_NODE_RECYCLE_TIME;
	}
}

//-----------------------------------------------------------------------------
glm::vec3 GVAppearance::getWeaponNodePosition (long nodeId)
{
    if (!simPresentationPass && simPose->ready) return nodeId >= 0 && static_cast<size_t>(nodeId) < simPose->nodes.size() ? simPose->nodes[nodeId] : simPose->root;

	GV_POSE_VIEW_LOCALS
	glm::vec3 result = position;
	if ((nodeId < appearType->numSmokeNodes) || (nodeId >= (appearType->numSmokeNodes+appearType->numWeaponNodes)))
		return result;



	//We already know we are using this node.  Do NOT increment recycle or nodeUsed!
	
   	//-------------------------------------------
   	// Create Matrix to conform to.
   	glm::quat qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
   
   	glm::vec3 xlatPosition;
   	mc2ToRender(xlatPosition, position);
   
   	glm::quat torsoRot;
   	torsoRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,(unitAnim().turretRotation * DEGREES_TO_RADS),0.0f));
	if (rotationalNodeIndex == -1)
	   	rotationalNodeIndex = gvShape->SetNodeRotation(appearType->rotationalNodeId,&torsoRot);

	gvShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);

	if (weaponNodeId[nodeId - appearType->numSmokeNodes] == -1)
		weaponNodeId[nodeId - appearType->numSmokeNodes] = gvShape->GetNodeNameId(appearType->nodeData[nodeId].nodeId);

	//If its STILL -1, we don't really have four weapon nodes.
	// Mark it with -2 to let us know to ignore it from now on.
	if (weaponNodeId[nodeId - appearType->numSmokeNodes] == -1)
		weaponNodeId[nodeId - appearType->numSmokeNodes] = -2;

	if (weaponNodeId[nodeId - appearType->numSmokeNodes] >= 0)
		result = mc2snap::unliftNodeZ(gvShape->GetTransformedNodePosition(&xlatPosition,&qRotation,weaponNodeId[nodeId - appearType->numSmokeNodes]), pv_.lift);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
 	return result;
}

//-----------------------------------------------------------------------------
glm::vec3 GVAppearance::getSmokeNodePosition (long nodeId)
{
	GV_POSE_VIEW_LOCALS
	glm::vec3 result = position;
	if ((nodeId < 0) || (nodeId >= appearType->numSmokeNodes))
		return result;



	//-------------------------------------------
   	// Create Matrix to conform to.
   	glm::quat qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
   
   	glm::vec3 xlatPosition;
   	mc2ToRender(xlatPosition, position);
   
   	glm::quat torsoRot;
   	torsoRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,(unitAnim().turretRotation * DEGREES_TO_RADS),0.0f));
	if (rotationalNodeIndex == -1)
	   	rotationalNodeIndex = gvShape->SetNodeRotation(appearType->rotationalNodeId,&torsoRot);

	gvShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);
   
	result = mc2snap::unliftNodeZ(gvShape->GetTransformedNodePosition(&xlatPosition,&qRotation,appearType->nodeData[nodeId].nodeId), pv_.lift);

	return result;
}

//-----------------------------------------------------------------------------
glm::vec3 GVAppearance::getDustNodePosition (long nodeId)
{
	GV_POSE_VIEW_LOCALS
	glm::vec3 result = position;
	if ((nodeId < 0) || (nodeId >= appearType->numFootNodes))
		return result;



   	//-------------------------------------------
   	// Create Matrix to conform to.
   	glm::quat qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
   
   	glm::vec3 xlatPosition;
   	mc2ToRender(xlatPosition, position);
   
   	glm::quat torsoRot;
   	torsoRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,(unitAnim().turretRotation * DEGREES_TO_RADS),0.0f));
	if (rotationalNodeIndex == -1)
	   	rotationalNodeIndex = gvShape->SetNodeRotation(appearType->rotationalNodeId,&torsoRot);

	gvShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);
   
	result = mc2snap::unliftNodeZ(gvShape->GetTransformedNodePosition(&xlatPosition,&qRotation,appearType->nodeData[nodeId+appearType->numWeaponNodes+appearType->numSmokeNodes].nodeId), pv_.lift);

	return result;
}

//-----------------------------------------------------------------------------
long GVAppearance::getLowestWeaponNode (void)
{
	const auto root = !simPresentationPass && simPose->ready ? simPose->root : position;
	//------------------------------------------------
	// Scan all weapon nodes and find least used one.
	long bestNode = -1;
	float lowestPosZ;
	long numSmokeNodes = appearType->numSmokeNodes;
	lowestPosZ = 9999999999999.0f;
	for (long i=0;i<appearType->numWeaponNodes;i++)
	{
		glm::vec3 nodePosition = getWeaponNodePosition(i+numSmokeNodes);
		if (((nodePosition.z - root.z) < lowestPosZ) && (nodePosition.z != root.z))
		{
			lowestPosZ = nodePosition.z - root.z;
			bestNode = i+numSmokeNodes;
		}
	}
		
   	if ((lowestPosZ == 0.0f) || (bestNode < 0) || (bestNode >= appearType->getTotalNodes()))
   		return -1;

 	return bestNode;
}

//-----------------------------------------------------------------------------
long GVAppearance::getWeaponNode (long weaponType)
{
	//------------------------------------------------
	// Scan all weapon nodes and find least used one.
	long leastUsed = 999999999;
	long bestNode = -1;
	long numSmokeNodes = appearType->numSmokeNodes;
	for (long i=0;i<appearType->numWeaponNodes;i++)
	{
		//This weaponNode does not exist if its -2
		if (weaponNodeId[i] == -2)
			continue;

		switch (weaponType)
		{
			case MECH3D_WEAPONTYPE_MISSILE:
				if ((appearType->nodeData[numSmokeNodes+i].weaponType == weaponType) || 
					(appearType->nodeData[numSmokeNodes+i].weaponType == MECH3D_WEAPONTYPE_ANY) || 
					(appearType->nodeData[numSmokeNodes+i].weaponType == MECH3D_WEAPONTYPE_NONENERGY))
				{
//					if (getWeaponNodePosition(i + numSmokeNodes).z != position.z)
//					{
						if (unitAnim().nodeUsed[i] < leastUsed)
						{
							leastUsed = unitAnim().nodeUsed[i];
							bestNode = i + numSmokeNodes;
						}
//					}
				}
			break;
			
			case MECH3D_WEAPONTYPE_BALLISTIC:
				if ((appearType->nodeData[numSmokeNodes+i].weaponType == weaponType) || 
					(appearType->nodeData[numSmokeNodes+i].weaponType == MECH3D_WEAPONTYPE_ANY) || 
					(appearType->nodeData[numSmokeNodes+i].weaponType == MECH3D_WEAPONTYPE_NONENERGY))
				{
//					if (getWeaponNodePosition(i + numSmokeNodes).z != position.z)
//					{
	 					if (unitAnim().nodeUsed[i] < leastUsed)
						{
							leastUsed = unitAnim().nodeUsed[i];
							bestNode = i + numSmokeNodes;
						}
//					}
				}
			break;
			
			case MECH3D_WEAPONTYPE_ENERGY:
				if ((appearType->nodeData[numSmokeNodes+i].weaponType == weaponType) || 
					(appearType->nodeData[numSmokeNodes+i].weaponType == MECH3D_WEAPONTYPE_ANY))
				{
//					if (getWeaponNodePosition(i + numSmokeNodes).z != position.z)
//					{
	 					if (unitAnim().nodeUsed[i] < leastUsed)
						{
							leastUsed = unitAnim().nodeUsed[i];
							bestNode = i + numSmokeNodes;
						}
//					}
				}
			break;
			
			case MECH3D_WEAPONTYPE_ANY:
//				if (getWeaponNodePosition(i + numSmokeNodes).z != position.z)
//				{
	 				if (unitAnim().nodeUsed[i] < leastUsed)
					{
						leastUsed = unitAnim().nodeUsed[i];
						bestNode = i + numSmokeNodes;
					}
//				}
			break;

			default:
				STOP(("Sent down a bad weapon type %d",weaponType));
		}
	}
		
   	if ((bestNode < 0) || (bestNode >= appearType->getTotalNodes()))
   		return -1;

 	return bestNode;
}
		
//-----------------------------------------------------------------------------
float GVAppearance::getWeaponNodeRecycle (long node)
{
	node -= appearType->numSmokeNodes;
	
 	if ((node >=0) && (node < appearType->numWeaponNodes))
		return unitAnim().nodeRecycle[node];
		
	return 9999.0f;		//NOT a weapon node.  Never recycled!!
}

//-----------------------------------------------------------------------------
void GVAppearance::init (AppearanceTypePtr tree, GameObjectPtr obj)
{
	// Artillery's bomber is a cosmetic child; its frame clock belongs to presentation.
	Appearance::init(tree, obj && obj->getObjectClass() != ARTILLERY ? obj : nullptr);
	appearType = static_cast<GVAppearanceType *>(tree);

	// C3: cache owner handle for GPU-cull node-position early-outs.
	actorHandle_ = (obj != nullptr) ? obj->getHandle() : -1;
	LCGV_TRACE("event=init actorHandle=%ld lifecycle=%d", actorHandle_, static_cast<int>(s_gpuCullLifecycle));

	shapeMin.x = shapeMin.y = -25;
	shapeMax.x = shapeMax.y = 50;
	
	paintScheme = -1;
	objectNameId = 30862;
	pilotNameID = -1;
	hazeFactor = 0.0f;

	currentFlash = duration = flashDuration = 0.0f;
	flashColor = 0x00000000;
	drawFlash = false;
	
	rotationalNodeIndex = -1;
	dustNodeIndex = activityNodeIndex = hitNodeId = weaponNodeId[0] = weaponNodeId[1] = weaponNodeId[2] = weaponNodeId[3] = -1;

	// (E) T1.10: lazy-init key for SpotLight_ children. Vectors default-init
	// to empty. GVAppearance has no legacy pointLight pair to reset.
	spotlightsRegistered_ = false;

	screenPos.x = screenPos.y = screenPos.z = screenPos.w = -999.0f;
	position = glm::vec3(0.0f);
	rotation = 0.0;;
	unitAnim().turretRotation= 0.0;
	selected = 0;
	teamId = -1;
	homeTeamRelationship = 0;
	actualRotation = rotation;
	actualTurretRotation = unitAnim().turretRotation;
	inDebugMoveMode = false;

	sensorLevel = 0;
	
	destructFX = nullptr;
	
	status = 0;
	simStatus_ = 0;
	simRotationalNodeIndex_ = -1;
	unitAnim().roll = unitAnim().pitch = 0.0f;

	currentLOD = 0;
	
	OBBRadius = 0.0f;

	sensorSpin = 0.0f;
	
	unitAnim().gvAnimationState =-1;
	unitAnim().currentFrame = 0.0f;
	unitAnim().gvFrameRate = 0.0f;
	unitAnim().isReversed = false;
	unitAnim().isLooping = false;
	unitAnim().setFirstFrame = false;
	unitAnim().canTransition = true;
 
	localTextureHandle = 0xffffffff;
	
	
	waterWake = nullptr;
	dustCloud = nullptr;
	activity = nullptr;
	isWaking = false;
	movedThisFrame = false;
	dustCloudStart = false;
	isActivitying = false;

	if (appearType)
	{
		{ ZoneScopedN("GVAppearance.init.BodyTex");
		gvShapeOwner_.reset(appearType->gvShape[0]->CreateFrom());
		gvShape = gvShapeOwner_.get();
	
		//-------------------------------------------------
		// Load the texture and store its handle.
		for (long i=0;i<gvShape->GetNumTextures();i++)
		{
			char txmName[1024];
			gvShape->GetTextureName(i,txmName,256);

			char texturePath[1024];
			mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));
	
			FullPathFileName textureName;
			textureName.init(texturePath,txmName,"");

			if (gvTextureExistsCached(textureName, false))
			{
				if (S_strnicmp(txmName,"a_",2) == 0)
				{
					DWORD gosTextureHandle = 0;
					
					if (!i)
					{
						localTextureHandle = mc2::textures::load(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink,true);
						gvShape->SetTextureHandle(i,localTextureHandle);
						gvShape->SetTextureAlpha(i,true);
					}
					else
					{
						gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
						gvShape->SetTextureHandle(i,gosTextureHandle);
						gvShape->SetTextureAlpha(i,true);
					}
					
				}
				else
				{
					DWORD gosTextureHandle = 0;
					
					if (!i)
					{
						localTextureHandle = mc2::textures::load(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink,true);
						gvShape->SetTextureHandle(i,localTextureHandle);
						gvShape->SetTextureAlpha(i,false);
					}
					else
					{
						gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
						gvShape->SetTextureHandle(i,gosTextureHandle);
						gvShape->SetTextureAlpha(i,false);
					}
				}
			}
			else
			{
				//PAUSE(("Warning: %s texture name not found",textureName));
				gvShape->SetTextureHandle(i,0xffffffff);
			}
		}
		
		}
		{ ZoneScopedN("GVAppearance.init.ShadowTex");
		if (appearType->gvShadowShape)
		{
			gvShadowShapeOwner_.reset(appearType->gvShadowShape->CreateFrom());
			gvShadowShape = gvShadowShapeOwner_.get();
	
			//-------------------------------------------------
			// Load the texture and store its handle.
			for (long i=0;i<gvShadowShape->GetNumTextures();i++)
			{
				char txmName[1024];
				gvShadowShape->GetTextureName(i,txmName,256);
		
				char texturePath[1024];
				mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));
		
				FullPathFileName textureName;
				textureName.init(texturePath,txmName,"");
		
				if (gvTextureExistsCached(textureName, false))
				{
					if (S_strnicmp(txmName,"a_",2) == 0)
					{
						DWORD gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						gvShadowShape->SetTextureHandle(i,gosTextureHandle);
						gvShadowShape->SetTextureAlpha(i,true);
					}
					else
					{
						DWORD gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
						gosASSERT(gosTextureHandle != 0xffffffff);
						gvShadowShape->SetTextureHandle(i,gosTextureHandle);
						gvShadowShape->SetTextureAlpha(i,false);
					}
				}
				else
				{
					gvShadowShape->SetTextureHandle(i,0xffffffff);
				}
			}
		}
		else
		{
			gvShadowShape = nullptr;
		}
 		
		}
		{ ZoneScopedN("GVAppearance.init.SensorTex");
		sensorTriangleShapeOwner_.reset(GVAppearanceType::SensorTriangleShape->CreateFrom());
		sensorTriangleShape = sensorTriangleShapeOwner_.get();
		//-------------------------------------------------
		// Load the texture and store its handle.
		for (int i=0;i<sensorTriangleShape->GetNumTextures();i++)
		{
			char txmName[1024];
			sensorTriangleShape->GetTextureName(i,txmName,256);

			char texturePath[1024];
			mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));
	
			FullPathFileName textureName;
			textureName.init(texturePath,txmName,"");
	
			if (gvTextureExistsCached(textureName, true))
			{
				if (S_strnicmp(txmName,"a_",2) == 0)
				{
					DWORD gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
					sensorTriangleShape->SetTextureHandle(i,gosTextureHandle);
					sensorTriangleShape->SetTextureAlpha(i,true);
				}
				else
				{
					DWORD gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
					sensorTriangleShape->SetTextureHandle(i,gosTextureHandle);
					sensorTriangleShape->SetTextureAlpha(i,false);
				}
			}
			else
			{
				//PAUSE(("Warning: %s texture name not found",textureName));
				sensorTriangleShape->SetTextureHandle(i,0xffffffff);
			}
		}
 		
		sensorCircleShapeOwner_.reset(GVAppearanceType::SensorCircleShape->CreateFrom());
		sensorCircleShape = sensorCircleShapeOwner_.get();
 		//-------------------------------------------------
		// Load the texture and store its handle.
		for (int i=0;i<sensorCircleShape->GetNumTextures();i++)
		{
			char txmName[1024];
			sensorCircleShape->GetTextureName(i,txmName,256);

			char texturePath[1024];
			mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));
	
			FullPathFileName textureName;
			textureName.init(texturePath,txmName,"");
	
			if (gvTextureExistsCached(textureName, true))
			{
				if (S_strnicmp(txmName,"a_",2) == 0)
				{
					DWORD gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
					sensorCircleShape->SetTextureHandle(i,gosTextureHandle);
					sensorCircleShape->SetTextureAlpha(i,true);
				}
				else
				{
					DWORD gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
					sensorCircleShape->SetTextureHandle(i,gosTextureHandle);
					sensorCircleShape->SetTextureAlpha(i,false);
				}
			}
			else
			{
				//PAUSE(("Warning: %s texture name not found",textureName));
				sensorCircleShape->SetTextureHandle(i,0xffffffff);
			}
		}
		}
    		 
 		glm::vec3 boxCoords[8];
		glm::vec3 nodeCenter = gvShape->GetRootNodeCenter();

		boxCoords[0].x = position.x + gvShape->GetMinBox().x + nodeCenter.x;
		boxCoords[0].y = position.y + gvShape->GetMinBox().z + nodeCenter.z;
		boxCoords[0].z = position.z + gvShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[1].x = position.x + gvShape->GetMinBox().x + nodeCenter.x;
		boxCoords[1].y = position.y + gvShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[1].z = position.z + gvShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[2].x = position.x + gvShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[2].y = position.y + gvShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[2].z = position.z + gvShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[3].x = position.x + gvShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[3].y = position.y + gvShape->GetMinBox().z + nodeCenter.z;
		boxCoords[3].z = position.z + gvShape->GetMaxBox().y + nodeCenter.y;
		
		boxCoords[4].x = position.x + gvShape->GetMinBox().x + nodeCenter.x;
		boxCoords[4].y = position.y + gvShape->GetMinBox().z + nodeCenter.z;
		boxCoords[4].z = position.z + gvShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[5].x = position.x + gvShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[5].y = position.y + gvShape->GetMinBox().z + nodeCenter.z;
		boxCoords[5].z = position.z + gvShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[6].x = position.x + gvShape->GetMaxBox().x + nodeCenter.x;
		boxCoords[6].y = position.y + gvShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[6].z = position.z + gvShape->GetMinBox().y + nodeCenter.y;
		
		boxCoords[7].x = position.x + gvShape->GetMinBox().x + nodeCenter.x;
		boxCoords[7].y = position.y + gvShape->GetMaxBox().z + nodeCenter.z;
		boxCoords[7].z = position.z + gvShape->GetMinBox().y + nodeCenter.y;
		
		float testRadius = 0.0;
		
		for (const auto & boxCoord : boxCoords)
		{
			testRadius = glm::length(boxCoord);
			OBBRadius = std::max(OBBRadius, testRadius);
		}
		
		appearType->boundsUpperLeftX = (-OBBRadius * 2.0);
		appearType->boundsUpperLeftY = (-OBBRadius * 2.0);
		appearType->boundsLowerRightX = (OBBRadius * 2.0);
		appearType->boundsLowerRightY = (OBBRadius);
		
		if (!appearType->getDesignerTypeBounds())
		{
			appearType->typeUpperLeft = gvShape->GetMinBox();
			appearType->typeLowerRight = gvShape->GetMaxBox();

			//Now expand box by some percentage to make selection easier.
			appearType->typeUpperLeft.x *= EXPAND_FACTOR;
			appearType->typeUpperLeft.y *= EXPAND_FACTOR;
			appearType->typeUpperLeft.z *= EXPAND_FACTOR;

			appearType->typeLowerRight.x *= EXPAND_FACTOR;
			appearType->typeLowerRight.y *= EXPAND_FACTOR;
			appearType->typeLowerRight.z *= EXPAND_FACTOR;
		}
		
		if (appearType->numWeaponNodes > 0)
		{
			unitAnim().nodeUsed.assign(static_cast<size_t>(appearType->numWeaponNodes), 0);
			unitAnim().nodeRecycle.assign(static_cast<size_t>(appearType->numWeaponNodes), 0.0f);
		}
			
		if (!InEditor)
		{
			if (!dustCloud && useNonWeaponEffects)
			{
				if (strcmp(weaponEffects->GetEffectName(VEHICLE_DUST_CLOUD),"NONE") != 0)
				{
					//--------------------------------------------
					// Yes, load it on up.
					unsigned flags = gosFX::Effect::ExecuteFlag|gosFX::Effect::LoopFlag;
		
					Check_Object(gosFX::EffectLibrary::Instance);
					gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(weaponEffects->GetEffectName(VEHICLE_DUST_CLOUD));
					
					if (gosEffectSpec)
					{
						{ ZoneScopedN("GVAppearance.init.DustMake");
						dustCloud = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
						gosASSERT(dustCloud != nullptr); }

						{ ZoneScopedN("GVAppearance.init.DustLoadImages");
						MidLevelRenderer::MLRTexturePool::Instance->LoadImages(); }
					}
				}
			}
		}

		// GPU static-prop batcher: register this GV's type shapes + variants.
		//
		// VEHICLE-LIGHTING-PATH-1 -- LATENT TRAP, DO NOT REMOVE BLIND.
		// These registerMultiShape calls put GV types into the batcher's
		// s_typeIndex, which is the sole membership test of
		// eligibleForGpuObjects() (gos_static_prop_batcher.cpp).
		// Stock gvactor never calls submitMultiShape() -- stock vehicles are movers and
		// are deliberately kept off the static-prop path (static_prop.vert:713
		// omits point/spot by design, so it would match buildings but still not
		// mechs). So GV shapes report "GPU-eligible" while actually drawing
		// through legacy TG_Shape::Render.
		//
		// eligibleForGpuObjects() gates the GatherLightsParameters calls at
		// tgl.cpp and :2869. Those are currently unreachable anyway --
		// both sit inside `if (bShadersDrawPathEnabled && ...)` and
		// bShadersDrawPathEnabled is false (tgl.cpp) -- so this
		// misregistration is harmless TODAY only by that accident. If anyone
		// ever flips bShadersDrawPathEnabled true, vehicles will silently lose
		// their gather again and regress to unlit raw albedo.
		// Removing these lines is NOT the fix either: registration is still
		// consumed elsewhere in the batcher. Fix the eligibility predicate
		// (submitted-ness, not registered-ness) if this ever goes live.
		ZoneScopedN("GVAppearance.init.Register");
		for (auto & gvShapeElem : appearType->gvShape)
			GpuStaticPropBatcher::instance().registerMultiShape(gvShapeElem.get());
		// An override body with object maps is one shared shape per LOD (paint is shader-side), so it joins the
		// batcher here, before finalize, and draws through submitMultiShape instead of the CPU fallback.
		if (const auto* ov = ModelOverrideRegistry::instance().resolve("vehicle", appearType->name); ov && ov->usesShaderTeamPaint())
			for (const auto & renderLod : appearType->renderLods)
				if (renderLod)
					GpuStaticPropBatcher::instance().registerMultiShape(renderLod.get(), true, ov);
		GpuStaticPropBatcher::instance().registerMultiShape(appearType->gvShadowShape.get());
		GpuStaticPropBatcher::instance().registerMultiShape(appearType->gvDmgShape.get());
		// Stock bodies (and the destroyed shape) draw from per-node matrices through the mech batcher.
		for (auto & gvShapeElem : appearType->gvShape)
			GpuMechBatcher::instance().registerVehicleType(gvShapeElem.get());
		GpuMechBatcher::instance().registerVehicleType(appearType->gvDmgShape.get());
	}
}

//-----------------------------------------------------------------------------
void GVAppearance::setObjStatus (long oStatus)
{
	// Kill handlers (GroundVehicle destruction, turrets) push the status mid-tick: the sim copy follows at once and the
	// draw shape swap waits for presentation (flushSimEvents).
	if (simulationOwned() && !simPresentationPass)
	{
		simStatus_ = oStatus;
		recordSimStatus(oStatus);
		return;
	}
	if (status != oStatus)
	{
		if ((oStatus == OBJECT_STATUS_DESTROYED) || (oStatus == OBJECT_STATUS_DISABLED))
		{
			if (appearType->gvDmgShape)
			{
				gvShape->ClearAnimation();
				gvShapeOwner_.reset();
				gvShape = nullptr;
				
				gvShapeOwner_.reset(appearType->gvDmgShape->CreateFrom());
				gvShape = gvShapeOwner_.get();
			}

			currentLOD = 0;
		}
		
		if (oStatus == OBJECT_STATUS_NORMAL)
		{
			if (appearType->gvShape[0])
			{
				gvShapeOwner_.reset();
				gvShape = nullptr;
				
				gvShapeOwner_.reset(appearType->gvShape[0]->CreateFrom());
				gvShape = gvShapeOwner_.get();
			}

			currentLOD = 0;
		}
		
		//-------------------------------------------------
		// Load the texture and store its handle.
		for (long i=0;i<gvShape->GetNumTextures();i++)
		{
			char txmName[1024];
			gvShape->GetTextureName(i,txmName,256);

			char texturePath[1024];
			mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));
	
			FullPathFileName textureName;
			textureName.init(texturePath,txmName,"");
	
			if (textureTgaExistsMemo(textureName))
			{
				if (S_strnicmp(txmName,"a_",2) == 0)
				{
					DWORD gosTextureHandle = 0;
					
					if (!i)
					{
						localTextureHandle = mc2::textures::load(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink,true);
						gvShape->SetTextureHandle(i,localTextureHandle);
						gvShape->SetTextureAlpha(i,true);
					}
					else
					{
						gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
						gvShape->SetTextureHandle(i,gosTextureHandle);
						gvShape->SetTextureAlpha(i,true);
					}
					
				}
				else
				{
					DWORD gosTextureHandle = 0;
					
					if (!i)
					{
						localTextureHandle = mc2::textures::load(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink,true);
						gvShape->SetTextureHandle(i,localTextureHandle);
						gvShape->SetTextureAlpha(i,false);
					}
					else
					{
						gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
						gvShape->SetTextureHandle(i,gosTextureHandle);
						gvShape->SetTextureAlpha(i,false);
					}
				}
			}
			else
			{
				//PAUSE(("Warning: %s texture name not found",textureName));
				gvShape->SetTextureHandle(i,0xffffffff);
			}
		}
		
		DWORD r, g, b;
		getPaintScheme(r, g, b);
		resetPaintScheme(r, g, b);
	}
	
	status = oStatus;
}
