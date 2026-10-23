#include "save_archive.h"
#include "save_exactness.h"
#include "appearance_rng.h"
#include "sim_rng_state.h"
// GameObjectManager: terrain-object counting, loading and geometry finalise, and the object CopyTo / CopyFrom / Save / Load.
// Declared in the class headers; shared names in objmgr_internal.h.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include <memory>
#include <span>
#include "objmgr_internal.h"
#include "saved_objects_check.h"
#include "mission_codec.h"
#include "mc2fmt.h"
#include "../GameOS/gameos/static_caster_set.h"

// Load tells an object's class from the byte size of its packet, so no two record types may share a size.
namespace {
constexpr bool recordSizesDistinct() {
	constexpr size_t sizes[] = { sizeof(TerrainObjectData), sizeof(BuildingData), sizeof(TurretData), sizeof(GateData),
	                             sizeof(ArtilleryData), sizeof(CarnageData), sizeof(MechData), sizeof(GroundVehicleData),
	                             sizeof(WeaponBoltData), sizeof(GameObjectData), sizeof(MoverData) };
	for (size_t i = 0; i < std::size(sizes); ++i)
		for (size_t j = i + 1; j < std::size(sizes); ++j)
			if (sizes[i] == sizes[j])
				return false;
	return true;
}
static_assert(recordSizesDistinct(), "two saved object record types share a size: Load could not tell them apart");
} // namespace


//---------------------------------------------------------------------------
constexpr int MAX_TERRAIN_OBJECTS = 3000;

constexpr int OLD_FILE_SIZE = 2200;

void GameObjectManager::countTerrainObjects (PacketFile* terrainFile, long firstHandle) 
{
	int packet = terrainFile->getCurrentPacket();
	int size = terrainFile->getPacketSize();
	auto pBuffer = std::make_unique_for_overwrite<unsigned char[]>(size);

#ifdef _DEBUG	
	int bytesRead = 
#endif
		terrainFile->readPacket( packet, pBuffer.get() );
	gosASSERT( bytesRead == size );
	
	const auto records = std::as_bytes(std::span(pBuffer.get(), static_cast<std::size_t>(size)));
	const auto count = mc2::mission::objectCount(records);
	if (!count) {
		STOP(("Invalid mission object packet count/size"));
		return;
	}
	totalObjCount = *count;

	if (totalObjCount)
	{
		objData = static_cast<ObjDataLoader *>(std::malloc(sizeof(ObjDataLoader) * totalObjCount));
		memset(objData,0,sizeof(ObjDataLoader) * totalObjCount);
	}
	else
	{
		objData = nullptr;
	}
	
	ObjDataLoader *data = objData;
	for ( int i = 0; i < totalObjCount; ++i )
	{
		const auto record = mc2::mission::objectRecord(records, i);
		data->objTypeNum = record.type;
		data->vector = glm::vec3(record.position[0], record.position[1], record.position[2]);
		data->rotation = record.rotation;
		data->damage = record.damage;
		data->teamId = record.team;
		data->parentId = record.parentId;

		// convert to block and vertex
		int tileCol;
		int tileRow;

		land->worldToTile( data->vector, tileRow, tileCol );

		int blockI = tileCol/20;
		int blockJ = tileRow/20;
		
		data->blockNumber = blockJ * land->blocksMapSide + blockI;
		tileCol -= blockI * 20;
		tileRow -= blockJ * 20;
		
		data->vertexNumber = tileRow * 20 + tileCol;
		countObject(data);
		
		data++;
	}

	// fix up handles
	long curHandle = firstHandle;
	for (int i = 0; i < Terrain::numObjBlocks; ++i )
	{
		Terrain::objBlockInfo[i].firstHandle = curHandle;
		curHandle += Terrain::objBlockInfo[i].numObjects;						
	}
}

void GameObjectManager::countObject( ObjDataLoader *data)
{
	ObjectTypePtr objType = objTypeManager->get(data->objTypeNum);
	if (!objType)
		objType = objTypeManager->load(data->objTypeNum);
	
	if (!objType)
		return;
			//Fatal(objDataBlock[i].objTypeNum, " GameObjectManager.countTerrainObjects: bad objType ");
		switch (objType->getObjectClass()) 
		{
			case TERRAINOBJECT:
			case TREE:
				Terrain::objBlockInfo[data->blockNumber].numCollidableObjects++;
				numTerrainObjects++;
				break;
			case TURRET:
				Terrain::objBlockInfo[data->blockNumber].numCollidableObjects++;
				numTurrets++;
				break;
			case GATE:
				Terrain::objBlockInfo[data->blockNumber].numCollidableObjects++;
				numGates++;
				break;
			case KLIEG_LIGHT:
				// Klieg lights are placed terrain objects loaded as BuildingType.
				// Count as buildings but skip the BuildingTypePtr sensor checks.
				numBuildings++;
				break;
			case BUILDING:
			case TREEBUILDING:
			if ((((static_cast<BuildingTypePtr>(objType))->perimeterAlarmRange > 0.0f) &&
				((static_cast<BuildingTypePtr>(objType))->perimeterAlarmTimer > 0.0f)) ||
				((static_cast<BuildingTypePtr>(objType))->lookoutTowerRange > 0.0f) ||
				((static_cast<BuildingTypePtr>(objType))->sensorRange > 0.0f))
				{
					Terrain::objBlockInfo[data->blockNumber].numCollidableObjects++;
				}
				numBuildings++;
				break;
			case BRIDGE:
				numTerrainObjects++;
				break;
			default:
				Fatal(objType->getObjectClass(), " GameObjectManager.countTerrainObjects: bad object type ");
		}
		Terrain::objBlockInfo[data->blockNumber].numObjects++;
}

//---------------------------------------------------------------------------

long GameObjectManager::getSpecificObjects (long objClass, long objSubType, GameObjectPtr* objects, long maxObjects) {

	long numValidObjects = 0;
	for (long i = 0; i < getNumObjects(); i++) {
		GameObjectPtr obj = objList[i];
		if (obj && (obj->getObjectClass() == objClass) && (obj->getObjectType()->getSubType() == objSubType)) {
			if (numValidObjects == maxObjects)
				Fatal(objClass, " GameObjectManager.getSpecificObjects: too many ");
			objects[numValidObjects++] = obj;
		}
	}
	return(numValidObjects);
}

//---------------------------------------------------------------------------

void GameObjectManager::loadTerrainObjects (PacketFile* terrainFile,
											volatile float& progress, float progressRange ) 
{
	ZoneScopedN("GameObjectManager::loadTerrainObjects");
	long curTerrainObjectIndex = 0;
	long curBuildingIndex = 0;
	long curTurretIndex = 0;
	long curGateIndex = 0;

	//------------------------------------------------------------------
	auto handles = std::make_unique_for_overwrite<long[]>(2 * Terrain::numObjBlocks);

	for ( int i = 0; i < Terrain::numObjBlocks; ++i )
	{
		handles[2 * i] = Terrain::objBlockInfo[i].firstHandle;
		handles[(2 * i) + 1] = Terrain::objBlockInfo[i].firstHandle + Terrain::objBlockInfo[i].numCollidableObjects;
	}

	ObjDataLoader *data = objData;

	float increment = 0.0f;
	if (totalObjCount)
		increment = progressRange/totalObjCount;
	{
		ZoneScopedN("GameObjectManager::loadTerrainObjects addObjectLoop");
		for (int i = 0; i < totalObjCount; ++i )
		{
			addObject( data, 
				curTerrainObjectIndex, curBuildingIndex, 
				curTurretIndex, curGateIndex, handles[2 * data->blockNumber], 
				handles[(2 * data->blockNumber) + 1] 
				);

			data++;
			progress += increment;
		}
	}

	handles.reset();
	
	//Done loading the objects, free the memory holding them!!
	std::free(objData);
	objData = nullptr;

	//---------------------------------------------------
	// Finally, let's build the control building lists...
	{ ZoneScopedN("GameObjectManager::loadTerrainObjects turretLinks");
	for (int i = 0; i < numTurrets; i++) 
	{
		if (mc2::mission::parentCell(turrets[i]->parentId).has_value())
		{
			BuildingPtr controlBuilding = static_cast<BuildingPtr>(ObjectManager->findByCellPosition((turrets[i]->parentId>>16),(turrets[i]->parentId & 0x0000ffff)));
			if (controlBuilding && !controlBuilding->getFlag(OBJECT_FLAG_CONTROLBUILDING)) 
			{
				controlBuilding->setFlag(OBJECT_FLAG_CAPTURABLE, true);
				controlBuilding->setFlag(OBJECT_FLAG_CONTROLBUILDING, true);
				controlBuilding->listID = numTurretControls;
				turretControls[numTurretControls++] = controlBuilding;
			}
			else if (!controlBuilding)
			{
				glm::vec3 worldPos;
				if (land)
					land->cellToWorld((turrets[i]->parentId>>16),(turrets[i]->parentId & 0x0000ffff),worldPos);
				PAUSE(("Turret linked to bldg @ R %d, C %d  X:%f Y:%f No Bldg there!",(turrets[i]->parentId>>16),(turrets[i]->parentId & 0x0000ffff),worldPos.x,worldPos.y));
				turrets[i]->parentId = 0xffffffff;
			}
		}
	}
	}

	{ ZoneScopedN("GameObjectManager::loadTerrainObjects gateLinks");
	for (int i = 0; i < numGates; i++) 
	{
		if (mc2::mission::parentCell(gates[i]->parentId).has_value())
		{
			BuildingPtr controlBuilding = static_cast<BuildingPtr>(ObjectManager->findByCellPosition((gates[i]->parentId>>16),(gates[i]->parentId & 0x0000ffff)));
			if (controlBuilding && !controlBuilding->getFlag(OBJECT_FLAG_CONTROLBUILDING)) 
			{
				controlBuilding->setFlag(OBJECT_FLAG_CONTROLBUILDING, true);
				controlBuilding->listID = numGateControls;
				gateControls[numGateControls++] = controlBuilding;
			}
	   		else if (!controlBuilding)
	   		{
	   			PAUSE(("Gate tried to link to building at Row %d, Col %d.  No Bldg there!",(gates[i]->parentId>>16),(gates[i]->parentId & 0x0000ffff)));
				gates[i]->parentId = 0xffffffff;
	   		}
		}
	}
	}

	//-----------------------------------------------------------------------------------
	// Create list of special buildings.  These buildings will be updated at least once
	// every frame regardless of where they are on the terrain and what is visible.
	// This is because perimeter alarms and lookout buildings and ????? must work even
	// if the player is NOT looking at them!!
	{ ZoneScopedN("GameObjectManager::loadTerrainObjects specialBuildings");
	for (int i=0;i<numBuildings;i++)
	{
		if (buildings[i]->isSpecialBuilding())
		{
			specialBuildings[numSpecialBuildings++] = buildings[i];
		}
	}
	}
	
 	//--------------------------------------------------------------------
	//Now, lets point every lit building to at least one power generator.
	{ ZoneScopedN("GameObjectManager::loadTerrainObjects powerGenerators");
	for (int i=0;i<numBuildings;i++)
	{
		if (buildings[i]->isPowerSource())
		{
			powerGenerators[numPowerGenerators++] = buildings[i];
		}
	}
	}
	
	if (numPowerGenerators)
	{
		for (int i=0;i<numBuildings;i++)
		{
			glm::vec3 maxDist;
			long generatorIndex = 0;
			maxDist = buildings[i]->getPosition() - powerGenerators[0]->getPosition();
			float minDistance = glm::length(maxDist);
			
			for (long j=1;j<numPowerGenerators;j++)
			{
				maxDist = buildings[i]->getPosition() - powerGenerators[j]->getPosition(); 
				float newDistance = glm::length(maxDist); 
				
				if (newDistance < minDistance)
				{
					generatorIndex = j;
					minDistance = newDistance;
				}
			}	
			
			buildings[i]->setPowerSupply(powerGenerators[generatorIndex]);
		}
		
		for (int i=0;i<numTerrainObjects;i++)
		{
			glm::vec3 maxDist;
			long generatorIndex = 0;
			maxDist = terrainObjects[i]->getPosition() - powerGenerators[0]->getPosition();
			float minDistance = glm::length(maxDist);
			
			for (long j=1;j<numPowerGenerators;j++)
			{
				maxDist = terrainObjects[i]->getPosition() - powerGenerators[j]->getPosition(); 
				float newDistance = glm::length(maxDist); 
				
				if (newDistance < minDistance)
				{
					generatorIndex = j;
					minDistance = newDistance;
				}
			}	
			
			terrainObjects[i]->setPowerSupply(powerGenerators[generatorIndex]);
		}
	}
}

//---------------------------------------------------------------------------
void GameObjectManager::primeTerrainObjectsForMissionLoad (volatile float& progress, float progressRange)
{
	ZoneScopedN("GameObjectManager::primeTerrainObjectsForMissionLoad");
	if (numTerrainObjects <= 0)
		return;

	long warmedAppearances = 0;
	const float increment = progressRange / float(numTerrainObjects);
	for (long i = 0; i < numTerrainObjects; ++i)
	{
		TerrainObjectPtr obj = terrainObjects[i];
		if (obj && obj->getExists())
		{
			obj->primeAppearanceForMissionLoad();
			++warmedAppearances;
		}

		progress += increment;
	}

	TracyPlot("Terrain object appearances warmed during mission load", int64_t(warmedAppearances));
}

// Register static props before geometry finalization so they are visible on the first frame.
// MC2_STATIC_PROP_MISSION_LOAD_REG defaults on; disabling it leaves first-render registration.
void GameObjectManager::registerStaticPropsForMissionLoad() {
	ZoneScopedN("GameObjectManager::registerStaticPropsForMissionLoad");
	if (!GpuStaticPropRegistry::isMissionLoadRegEnabled()) return;

	int totalEnumerated = 0, totalRegistered = 0, totalSkipped = 0;
	// Per-array counts distinguish missing appearances from failed registrations.
	int byArr_enum[4] = {0,0,0,0}; // 0=terrainObjects 1=buildings 2=turrets 3=gates
	int byArr_reg[4]  = {0,0,0,0};
	int byArr_skip[4] = {0,0,0,0};
	int byArr_noapp[4]= {0,0,0,0};
	int currentArr = 0;

	// Appearance state must match the object before registration to avoid props at world origin
	// and incorrect selection or allegiance after a restore.
	auto registerOne = [&](GameObjectPtr obj) {
		if (!obj) return;
		++totalEnumerated;
		++byArr_enum[currentArr];
		AppearancePtr app = obj->getAppearance();
		if (!app) { ++totalSkipped; ++byArr_skip[currentArr]; ++byArr_noapp[currentArr]; return; }
		const long teamId = obj->getTeamId();
		const long homeRelation = Team::home ? Team::getRelation(teamId, Team::home->getId()) : 0;
		app->setObjectParameters(obj->getPosition(), obj->getRotation(), obj->drawFlags, teamId, homeRelation);
		app->registerStatic();
		if (app->isStaticRegistered()) { ++totalRegistered; ++byArr_reg[currentArr]; }
		else                           { ++totalSkipped;    ++byArr_skip[currentArr]; }
	};

	currentArr = 0;
	for (long i = 0; i < numTerrainObjects; ++i)
		registerOne(terrainObjects[i]);
	currentArr = 1;
	for (long i = 0; i < numBuildings; ++i)
		registerOne(buildings[i]);
	currentArr = 2;
	for (long i = 0; i < numTurrets; ++i)
		registerOne(turrets[i]);
	currentArr = 3;
	for (long i = 0; i < numGates; ++i)
		registerOne(gates[i]);

	MC2_DIAGFE(stderr,
		"[STATIC_PROP_REG v1] event=mission_load enumerated=%d registered=%d skipped=%d\n",
		totalEnumerated, totalRegistered, totalSkipped);
	MC2_DIAGFE(stderr,
		"[STATIC_PROP_REG v1] event=mission_load_byarr "
		"terrainObjects=%d/%d (skip=%d noapp=%d) "
		"buildings=%d/%d (skip=%d noapp=%d) "
		"turrets=%d/%d (skip=%d noapp=%d) "
		"gates=%d/%d (skip=%d noapp=%d)\n",
		byArr_reg[0], byArr_enum[0], byArr_skip[0], byArr_noapp[0],
		byArr_reg[1], byArr_enum[1], byArr_skip[1], byArr_noapp[1],
		byArr_reg[2], byArr_enum[2], byArr_skip[2], byArr_noapp[2],
		byArr_reg[3], byArr_enum[3], byArr_skip[3], byArr_noapp[3]);
	fflush(stderr);
}

void GameObjectManager::buildStaticShadowCasterSet() {
	ZoneScopedN("GameObjectManager::buildStaticShadowCasterSet");
	mc2shadow::StaticCasterSet& set = mc2shadow::staticCasterSet();
	set.beginRebuild();
	auto addAll = [&set](auto objects, mc2shadow::CasterKind kind) {
		for (GameObjectPtr obj : objects) {
			if (!obj || !obj->getExists()) continue;
			AppearancePtr app = obj->getAppearance();
			if (!app) continue;
			const int32_t recipe = app->getShadowCasterRecipeIndex();
			if (recipe < 0 && kind != mc2shadow::CasterKind::Turret) continue;
			mc2shadow::StaticCasterEntry e;
			e.owner = app;
			e.gameHandle = static_cast<uint32_t>(obj->getHandle());
			e.recipeIndex = recipe;
			e.kind = kind;
			e.damageState = obj->isDestroyed() ? 1 : 0;
			set.add(e);
		}
	};
	addAll(std::span(terrainObjects, static_cast<size_t>(numTerrainObjects)), mc2shadow::CasterKind::TerrainObject);
	addAll(std::span(buildings, static_cast<size_t>(numBuildings)), mc2shadow::CasterKind::Building);
	addAll(std::span(turrets, static_cast<size_t>(numTurrets)), mc2shadow::CasterKind::Turret);
	addAll(std::span(gates, static_cast<size_t>(numGates)), mc2shadow::CasterKind::Gate);
	set.endRebuild();
}

// STATIC-REG-PREWARM-QUEUE-1: mission-load off-screen light bake.
// Must be called AFTER finalizeGeometry() and AFTER eye->init() so that
// world lights are valid (Camera::getWorldLights()/getNumLights() return live data).
// Walks the same four object arrays as registerStaticPropsForMissionLoad().
// For each registered-and-latched static prop, calls
//   app->prewarmStaticLightBake(cam)
// which runs SetLightList → TransformMultiShape_HierarchyOnly → mc2CacheOrBakeStaticGpuLight
// and clears needsFullBakeNextFrame if the bake produced a valid permanent SSBO slot.
// Guarded by MC2_STATIC_REG_PREWARM=1 (default off).
void GameObjectManager::prewarmStaticPropLightBakes(Camera* cam)
{
	ZoneScopedN("GameObjectManager::prewarmStaticPropLightBakes");
	static const bool s_enabled  = mc2::gate::get<"MC2_STATIC_REG_PREWARM">();
	static const bool s_trace    = mc2::gate::get<"MC2_STATIC_REG_PREWARM_TRACE">();
	if (!s_enabled) return;

	auto t0 = std::chrono::steady_clock::now();

	int attempted = 0, baked = 0;
	int skipped_no_lights = 0, skipped_not_registered = 0;
	int skipped_no_multishape = 0, failed_no_cached_index = 0;

	auto tryBake = [&](GameObjectPtr obj) {
		if (!obj) return;
		AppearancePtr app = obj->getAppearance();
		if (!app) { ++skipped_no_multishape; return; }
		if (!app->isStaticRegistered()) { ++skipped_not_registered; return; }
		if (!app->needsPrewarmBake()) return;  // already baked or not latched
		if (!cam || cam->getNumLights() == 0) { ++skipped_no_lights; return; }
		++attempted;
		if (s_trace) {
			fprintf(stderr,
				"STATIC_REG_PREWARM_TRACE: obj=%p recipeIdx=%d\n",
				static_cast<void*>(obj), app->getStaticRecipeIndex());
		}
		if (app->prewarmStaticLightBake(cam)) {
			++baked;
		} else {
			++failed_no_cached_index;
		}
	};

	for (long i = 0; i < numTerrainObjects; ++i) tryBake(terrainObjects[i]);
	for (long i = 0; i < numBuildings;      ++i) tryBake(buildings[i]);
	for (long i = 0; i < numTurrets;        ++i) tryBake(turrets[i]);
	for (long i = 0; i < numGates;          ++i) tryBake(gates[i]);

	// Count remaining needsPrewarmBake objects (acceptance metric).
	int remaining_needs_full_bake = 0;
	auto countRemaining = [&](GameObjectPtr obj) {
		if (!obj) return;
		AppearancePtr app = obj->getAppearance();
		if (app && app->needsPrewarmBake()) ++remaining_needs_full_bake;
	};
	for (long i = 0; i < numTerrainObjects; ++i) countRemaining(terrainObjects[i]);
	for (long i = 0; i < numBuildings;      ++i) countRemaining(buildings[i]);
	for (long i = 0; i < numTurrets;        ++i) countRemaining(turrets[i]);
	for (long i = 0; i < numGates;          ++i) countRemaining(gates[i]);

	auto t1 = std::chrono::steady_clock::now();
	long long elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

	printf("STATIC_REG_PREWARM: enabled=1"
	       " attempted=%d baked=%d"
	       " skipped_no_lights=%d skipped_not_registered=%d"
	       " skipped_no_multishape=%d failed_no_cached_index=%d"
	       " remaining_needs_full_bake=%d elapsed_us=%lld\n",
	       attempted, baked,
	       skipped_no_lights, skipped_not_registered,
	       skipped_no_multishape, failed_no_cached_index,
	       remaining_needs_full_bake, elapsed_us);
	fflush(stdout);
}

// WATCHID-LOAD-GUARD-1: rate-limited diagnostic for invalid watch data read
// from an untrusted/corrupt/cross-version save. Emits a one-time stderr line
// (visible without any diag tag) plus a rate-limited WATCHID_LOAD JSONL event.
static void mc2_watchidLoadDiag(const char* what, long a, long b)
{
	static unsigned long s_loadDrops = 0;
	++s_loadDrops;
	if (s_loadDrops == 1)
		fprintf(stderr, "[WATCHID_LOAD] invalid watch data on load (%s a=%ld b=%ld); "
			"slot invalidated. Further occurrences rate-limited.\n", what, a, b);
	if (mc2_diag::tagEnabled("WATCHID_LOAD") &&
		(s_loadDrops == 1 || (s_loadDrops & 0xFF) == 0)) {
		char _wl_buf[160];
		(*std::format_to_n(_wl_buf, (sizeof(_wl_buf)) - 1,
			"{{\"what\":\"{}\",\"a\":{:d},\"b\":{:d},\"drops\":{:d}}}", mc2::fmt::safe(what), mc2::fmt::as_i32(a), mc2::fmt::as_i32(b), mc2::fmt::as_u32(s_loadDrops)).out = '\0');
		mc2_diag::writeEvent("WATCHID_LOAD", 1, 0, _wl_buf);
	}
}

//-------------------------------------------------------------------
void GameObjectManager::CopyTo (ObjectManagerData *data)
{
	data->maxObjects 		= getMaxObjects();
	data->numElementals		= numElementals;
	data->numTerrainObjects = numTerrainObjects;
	data->numBuildings		= numBuildings;
	data->numTurrets		= numTurrets;
	data->numWeapons		= numWeapons;
	data->numCarnage		= numCarnage;
	data->numLights			= numLights;
	data->numArtillery		= numArtillery;
	data->numGates			= numGates;
	data->maxMechs			= maxMechs;
	data->maxVehicles		= maxVehicles;
	data->numMechs			= numMechs;
	data->numVehicles		= numVehicles;
	data->nextWatchId		= nextWatchID;
}

//-------------------------------------------------------------------
void GameObjectManager::CopyFrom (ObjectManagerData *data)
{
	numElementals		=    data->numElementals;
	numTerrainObjects   =    data->numTerrainObjects;
	numBuildings        =    data->numBuildings;
	numTurrets          =    data->numTurrets;
	numWeapons          =    data->numWeapons;
	numCarnage          =    data->numCarnage;
	numLights           =    data->numLights;
	numArtillery        =    data->numArtillery;
	numGates            =    data->numGates;
	maxMechs            =    data->maxMechs;
	maxVehicles         =    data->maxVehicles;
	numMechs            =    data->numMechs;
	numVehicles         =    data->numVehicles;

	// WATCHID-LOAD-GUARD-1: nextWatchId is untrusted on-disk data. getByWatchID()
	// reads watchList[watchID] for watchID < nextWatchID, and watchList is sized
	// getMaxObjects()+1 (allocated in Load right after this), so a corrupt or
	// cross-version nextWatchId would let a later getByWatchID() index out of
	// bounds. Clamp to the allocation. The count fields above are already applied,
	// so getMaxObjects() reflects the loaded scene. Never trust the serialized
	// value blindly (mirrors the Save-side clamp in OBJMGR-WATCHID-BOUNDS-1).
	const long _cap = getMaxObjects() + 1;
	const unsigned long _clampedNext = mc2watch::clampSavedNextWatchId(data->nextWatchId, getMaxObjects());
	if (static_cast<long>(_clampedNext) != data->nextWatchId)
		mc2_watchidLoadDiag("nextWatchId", static_cast<long>(data->nextWatchId), _cap);
	nextWatchID = _clampedNext;
}

//-------------------------------------------------------------------
#include "save_struct_zero.h"
long GameObjectManager::Save (PacketFilePtr file, long packetNum)
{
	int32_t *watchSave = static_cast<int32_t*>(malloc(sizeof(int32_t) * (getMaxObjects() + 1)));
	memset(watchSave,0,sizeof(int32_t) * (getMaxObjects() + 1));

	MC2_SAVE_STRUCT(ObjectManagerData, data);
	CopyTo(&data);

	file->writePacket(packetNum,reinterpret_cast<MemoryPtr>(&data),sizeof(ObjectManagerData),STORAGE_TYPE_RAW);
	packetNum++;

	for (long i=0;i<=getMaxObjects();i++)
	{
		if (objList[i])
		{
			objList[i]->Save(file,packetNum);
			packetNum++;

			//Find the watchID from the watchlist for this object.
			// If none, let it be.  It'll be zero already
			// DO NOT CALL getWatchID!!!!!!!
			// That will assign them to objects which don't have them!!!!
			// OBJMGR-WATCHID-BOUNDS-1: clamp independently of setWatchID —
			// watchList and watchSave are both sized getMaxObjects()+1, so j must
			// not exceed getMaxObjects(). Previously j<=nextWatchID could read
			// watchList[j] / write watchSave[j] one past the allocation
			// (nextWatchID can legitimately equal getMaxObjects()+1).
			const long _watchHi = mc2watch::clampSaveLoopLimit(nextWatchID, getMaxObjects());
			for (long j=0;j<=_watchHi;j++)
			{
				if (watchList[j] == objList[i])
				{
					watchSave[j] = i;
				}
			}
		}
		else
		{
			packetNum++;
		}
	}

	file->writePacket(packetNum,reinterpret_cast<MemoryPtr>(watchSave), sizeof(int32_t) * (getMaxObjects() + 1), STORAGE_TYPE_ZLIB);
	packetNum++;

	free(watchSave);
	watchSave = nullptr;

	return packetNum;
}

//-------------------------------------------------------------------
// The object type a saved record names, or null (logged) when this build does not know it. A null type
// makes the slot an empty one, the same as a record that never had a type.
ObjectTypePtr GameObjectManager::savedObjectType (long typeNum, long slot)
{
	if (typeNum <= 0)
		return nullptr;
	ObjectTypePtr objType = getObjectType(typeNum);
	if (!objType)
		mc2::fmt::print_err("[SAVE v1] event=object_skipped reason=unknown_object_type slot={:d} type={:d}\n", mc2::fmt::as_i32(slot), mc2::fmt::as_i32(typeNum));
	return objType;
}

//-------------------------------------------------------------------
std::optional<std::string_view> GameObjectManager::checkSavedObjects (PacketFilePtr file, long* headerPacket)
{
	using namespace mc2_saved_objects;
	RecordSizes sizes = {};
	sizes.byClass[kTerrain] = sizeof(TerrainObjectData);
	sizes.byClass[kBuilding] = sizeof(BuildingData);
	sizes.byClass[kTurret] = sizeof(TurretData);
	sizes.byClass[kGate] = sizeof(GateData);
	sizes.byClass[kArtillery] = sizeof(ArtilleryData);
	sizes.byClass[kCarnage] = sizeof(CarnageData);
	sizes.byClass[kMech] = sizeof(MechData);
	sizes.byClass[kVehicle] = sizeof(GroundVehicleData);
	sizes.byClass[kBolt] = sizeof(WeaponBoltData);
	sizes.pureObject = sizeof(GameObjectData);
	sizes.pureMover = sizeof(MoverData);

	const long resume = file->getCurrentPacket();
	if (!mc2::combat::readCombatClock(*file))
		return "invalid_combat_clock";
	const long numPackets = file->getNumPackets();
	auto sizeAt = [file](uint32_t packet) -> uint32_t {
		file->seekPacket(static_cast<int>(packet));
		return static_cast<uint32_t>(file->getPacketSize());
	};

	// The header's position depends on how many packets the earlier sections wrote, so look for it by size
	// and by the run it describes.
	std::optional<std::string_view> verdict = "no_object_manager_header";
	for (long p = 0; p < numPackets; ++p) {
		if (sizeAt(static_cast<uint32_t>(p)) != sizeof(ObjectManagerData))
			continue;
		ObjectManagerData data{};
		file->readPacket(static_cast<int>(p), reinterpret_cast<MemoryPtr>(&data));
		HeaderCounts h = {};
		h.byClass[kTerrain] = data.numTerrainObjects;
		h.byClass[kBuilding] = data.numBuildings;
		h.byClass[kTurret] = data.numTurrets;
		h.byClass[kGate] = data.numGates;
		h.byClass[kArtillery] = data.numArtillery;
		h.byClass[kCarnage] = data.numCarnage;
		h.byClass[kMech] = data.maxMechs;
		h.byClass[kVehicle] = data.maxVehicles;
		h.byClass[kBolt] = data.numWeapons;
		h.elementals = data.numElementals;
		h.lights = data.numLights;
		if (!plausibleHeader(h))
			continue;
		const std::optional<std::string_view> why = checkRun(h, sizes, static_cast<uint32_t>(p), static_cast<uint32_t>(numPackets), sizeAt);
		if (!why) {
			if (headerPacket)
				*headerPacket = p;
			verdict = std::nullopt;
			for (long slot = 0; slot <= slotCount(h); ++slot) {
				const auto packet = static_cast<int>(p + 1 + slot);
				if (sizeAt(packet) != sizeof(WeaponBoltData)) continue;
				WeaponBoltData bolt{};
				if (file->readPacket(packet, reinterpret_cast<MemoryPtr>(&bolt)) != sizeof(bolt) ||
					!mc2::combat::validSavedDeadline(bolt.retireDeadlineTick, bolt.retireDeadlineState)) {
					verdict = "invalid_bolt_deadline";
					break;
				}
			}
			break;
		}
		verdict = why;
	}
	file->seekPacket(static_cast<int>(resume));
	return verdict;
}

//-------------------------------------------------------------------
long GameObjectManager::Load (PacketFilePtr file, long packetNum)
{
	ObjectManagerData data{};
	file->readPacket(packetNum,reinterpret_cast<MemoryPtr>(&data));
	packetNum++;

	CopyFrom(&data);

	//------------------------------------------------------------------
	//Create the ObjList and WatchList EXACTLY like we normally do.
	GameObject::setInitialize(true);

	//-----------------------------------------------------------
	// First element in list is NULL (handle of 0 is always NULL)
	objList = bindPoolArray(objListStore, (getMaxObjects() + 1));
	memset(objList,0,sizeof(GameObjectPtr) * (getMaxObjects() + 1));

	watchList = bindPoolArray(watchListStore, (getMaxObjects() + 1));
	memset(watchList,0,sizeof(GameObjectPtr) * (getMaxObjects() + 1));

	// MF3-GENERATIONAL-HANDLE-1: Load re-alloc of the generation side array, in
	// LOCKSTEP with watchList above. Load = fresh generations by design (reset
	// to 0); generation is runtime-only and never deserialized.
	watchGeneration = bindPoolArray(watchGenerationStore, (getMaxObjects() + 1));
	memset(watchGeneration,0,sizeof(uint16_t) * (getMaxObjects() + 1));

	long i = 0;
	long curHandle = 1;
	maxMovers = maxMechs + maxVehicles + numElementals;
	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (maxMechs > 0) 
	{
		mechs = bindPoolArray(mechStore, maxMechs);
		if (!mechs)
			Fatal(maxMechs, " GameObjectManager.setNumObjects: cannot malloc mechs ");
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (maxVehicles > 0) 
	{
		vehicles = bindPoolArray(vehicleStore, maxVehicles);
		if (!vehicles)
			Fatal(maxVehicles, " GameObjectManager.setNumObjects: cannot malloc vehicles ");
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (numTerrainObjects > 0) 
	{
		terrainObjects = bindPoolArray(terrainObjectStore, numTerrainObjects);
		if (!terrainObjects)
			Fatal(numTerrainObjects, " GameObjectManager.setNumObjects: cannot malloc terrain objects ");
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (numBuildings > 0) 
	{
		buildings = bindPoolArray(buildingStore, numBuildings);
		if (!buildings)
			Fatal(numBuildings, " GameObjectManager.setNumObjects: cannot malloc buildings ");
	}

	if (numTurrets > 0)
	{
		turrets = bindPoolArray(turretStore, numTurrets);
		if ( !turrets )
			Fatal(numTurrets, " GameObjectManager.setNumObjects: cannot malloc Turrets ");
	}

	if (numGates > 0)
	{
		gates = bindPoolArray(gateStore, numGates);
		if ( !gates )
			Fatal(numGates, " GameObjectManager.setNumObjects: cannot malloc Gates ");
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (numWeapons > 0) 
	{
		weapons = bindPoolArray(weaponStore, numWeapons);
		if (!weapons)
			Fatal(numWeapons, " GameObjectManager.setNumObjects: cannot malloc weapons ");
	}

	if (numCarnage > 0) 
	{
		carnage = bindPoolArray(carnageStore, numCarnage);
		if (!carnage)
			Fatal(numCarnage, " GameObjectManager.setNumObjects: cannot malloc carnage ");
	}

	if (numArtillery > 0) 
	{
		artillery = bindPoolArray(artilleryStore, numArtillery);
		if (!artillery)
			Fatal(numArtillery, " GameObjectManager.setNumObjects: cannot malloc artillery ");
	}

	useMoverLineOfSightTable = true;
	moverLineOfSightTable = static_cast<char*>(std::malloc(maxMovers * maxMovers));
	if (!moverLineOfSightTable)
		Fatal(numGates, " GameObjectManager.setNumObjects: cannot malloc moverLineOfSightTable ");

	long curTerrObjNum = 0;
	long curBuildingNum = 0;
	long curTurretNum = 0;
	long curGateNum = 0;
	long curArtilleryNum = 0;
	long curCarnageNum = 0;
	long curMechNum = 0;
	long curVehicleNum = 0;
	long curBoltNum = 0;

	long oldBlockNum = -1;
	for (i=0;i<=getMaxObjects();i++)
	{
		//OK.
		// For each object that was saved, Load the data.
		// If objectTypeNum != 0, create the object from the type EXACTLY like we do above.
		// then init from the savedData.
		// Next Object.

		//First, get the size of the packet saved.
		// This will tell me what kind of object it was!
		file->seekPacket(packetNum);
		DWORD packetSize = file->getPacketSize();
		if (packetSize == 0)
		{
			//NO Object stored here.
			// Increment Packet and move on
			// First object in ObjList, for example!!
			packetNum++;
		}
		else if ((packetSize == sizeof(GameObjectData)) ||
				(packetSize == sizeof(MoverData)))
		{
			STOP(("We saved a pure GameObject or Pure Mover in slot %d, packet %d",i,packetNum));
		}
		else if (packetSize == sizeof(TerrainObjectData))
		{
			//We have a TerrainObject.
			// Get its data.
			TerrainObjectData data{};
			file->readPacket(packetNum,reinterpret_cast<MemoryPtr>((&data)));
			packetNum++;

			TerrainObjectPtr obj = new TerrainObject; 
			objList[i] = terrainObjects[curTerrObjNum] = obj;
			objList[i]->setHandle(i);

			ObjectTypePtr objType = savedObjectType(data.objectTypeNum, i);
			if (objType)	//We have an object type to copy from!!
			{

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
				obj->setDamage(data.damage);
				obj->setRotation(data.rotation);

				if (data.blockNumber != oldBlockNum)
				{
					oldBlockNum = data.blockNumber;
					Terrain::objBlockInfo[data.blockNumber].firstHandle = i;
				}

				Terrain::objBlockInfo[data.blockNumber].numCollidableObjects++;
				Terrain::objBlockInfo[data.blockNumber].numObjects++;
			}
			else
			{
				MC2_DESTROY(obj, "load_empty_slot");
			}

			curTerrObjNum++;
		}
		else if (packetSize == sizeof(BuildingData))
		{
			//We have a Building.
			BuildingData data{};
			file->readPacket(packetNum,reinterpret_cast<MemoryPtr>((&data)));
			packetNum++;

			BuildingPtr obj = new Building; 
			objList[i] = buildings[curBuildingNum] = obj;
			objList[i]->setHandle(i);

			ObjectTypePtr objType = savedObjectType(data.objectTypeNum, i);
			if (objType)	//We have an object type to copy from!!
			{

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
				obj->setDamage(data.damage);
				obj->setRotation(data.rotation);

				if (data.blockNumber != oldBlockNum)
				{
					oldBlockNum = data.blockNumber;
					Terrain::objBlockInfo[data.blockNumber].firstHandle = i;
				}

				if ((((static_cast<BuildingTypePtr>(objType))->perimeterAlarmRange > 0.0f) &&
					((static_cast<BuildingTypePtr>(objType))->perimeterAlarmTimer > 0.0f)) ||
					((static_cast<BuildingTypePtr>(objType))->lookoutTowerRange > 0.0f) ||
					((static_cast<BuildingTypePtr>(objType))->sensorRange > 0.0f))
				{
					Terrain::objBlockInfo[data.blockNumber].numCollidableObjects++;
				}

				Terrain::objBlockInfo[data.blockNumber].numObjects++;
			}
			else
			{
				MC2_DESTROY(obj, "load_empty_slot");
			}
			curBuildingNum++;
		}
		else if (packetSize == sizeof(TurretData))
		{
			//We have a Turret.
			TurretData data{};
			file->readPacket(packetNum,reinterpret_cast<MemoryPtr>((&data)));
			packetNum++;

			TurretPtr obj = new Turret; 
			objList[i] = turrets[curTurretNum] = obj;
			objList[i]->setHandle(i);

			ObjectTypePtr objType = savedObjectType(data.objectTypeNum, i);
			if (objType)	//We have an object type to copy from!!
			{

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
				obj->setDamage(data.damage);
				obj->setRotation(data.rotation);

				if (data.blockNumber != oldBlockNum)
				{
					oldBlockNum = data.blockNumber;
					Terrain::objBlockInfo[data.blockNumber].firstHandle = i;
				}

				Terrain::objBlockInfo[data.blockNumber].numCollidableObjects++;
				Terrain::objBlockInfo[data.blockNumber].numObjects++;
			}
			else
			{
				MC2_DESTROY(obj, "load_empty_slot");
			}
			curTurretNum++;
		}
		else if (packetSize == sizeof(GateData))
		{
			//We have a Gate.
			GateData data{};
			file->readPacket(packetNum,reinterpret_cast<MemoryPtr>((&data)));
			packetNum++;

			GatePtr obj = new Gate; 
			objList[i] = gates[curGateNum] = obj;
			objList[i]->setHandle(i);

			ObjectTypePtr objType = savedObjectType(data.objectTypeNum, i);
			if (objType)	//We have an object type to copy from!!
			{

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
				obj->setDamage(data.damage);
				obj->setRotation(data.rotation);

				if (data.blockNumber != oldBlockNum)
				{
					oldBlockNum = data.blockNumber;
					Terrain::objBlockInfo[data.blockNumber].firstHandle = i;
				}

				Terrain::objBlockInfo[data.blockNumber].numCollidableObjects++;
				Terrain::objBlockInfo[data.blockNumber].numObjects++;
			}
			else
			{
				MC2_DESTROY(obj, "load_empty_slot");
			}
			curGateNum++;
		}
		else if (packetSize == sizeof(ArtilleryData))
		{
			//We have an Artillery.
			ArtilleryData data{};
			file->readPacket(packetNum,reinterpret_cast<MemoryPtr>((&data)));
			packetNum++;

			ArtilleryPtr obj = new Artillery; 
			objList[i] = artillery[curArtilleryNum] = obj;
			objList[i]->setHandle(i);

			ObjectTypePtr objType = savedObjectType(data.objectTypeNum, i);
			if (objType)	//We have an object type to copy from!!
			{

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
			}
			else
			{
				MC2_DESTROY(obj, "load_empty_slot");
			}
			curArtilleryNum++;
		}
		else if (packetSize == sizeof(CarnageData))
		{
			//We have a Carnage.
			CarnageData data{};
			file->readPacket(packetNum,reinterpret_cast<MemoryPtr>((&data)));
			packetNum++;

			CarnagePtr obj = new Carnage; 
			objList[i] = carnage[curCarnageNum] = obj;
			objList[i]->setHandle(i);

			ObjectTypePtr objType = savedObjectType(data.objectTypeNum, i);
			if (objType)	//We have an object type to copy from!!
			{

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
			}
			else
			{
				MC2_DESTROY(obj, "load_empty_slot");
			}
			curCarnageNum++;
		}
		else if (packetSize == sizeof(MechData))
		{
			//We have a BattleMech.
			MechData data{};
			file->readPacket(packetNum,reinterpret_cast<MemoryPtr>((&data)));
			packetNum++;

			BattleMechPtr obj = new BattleMech; 
			objList[i] = mechs[curMechNum] = obj;
			objList[i]->setHandle(i);

			ObjectTypePtr objType = savedObjectType(data.objectTypeNum, i);
			if (objType)	//We have an object type to copy from!!
			{

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
			}
			else
			{
				MC2_DESTROY(obj, "load_empty_slot");
			}
			curMechNum++;
		}
		else if (packetSize == sizeof(GroundVehicleData))
		{
			//We have a groundVehicle.
			GroundVehicleData data{};
			file->readPacket(packetNum,reinterpret_cast<MemoryPtr>((&data)));
			packetNum++;

			GroundVehiclePtr obj = new GroundVehicle; 
			objList[i] = vehicles[curVehicleNum] = obj;
			objList[i]->setHandle(i);

			ObjectTypePtr objType = savedObjectType(data.objectTypeNum, i);
			if (objType)	//We have an object type to copy from!!
			{

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
			}
			else
			{
				MC2_DESTROY(obj, "load_empty_slot");
			}
			curVehicleNum++;
		}
		else if (packetSize == sizeof(WeaponBoltData))
		{
			//We have a WeaponBolt.
			WeaponBoltData data{};
			file->readPacket(packetNum,reinterpret_cast<MemoryPtr>((&data)));
			packetNum++;

			WeaponBoltPtr obj = new WeaponBolt; 
			objList[i] = weapons[curBoltNum] = obj;
			objList[i]->setHandle(i);

			ObjectTypePtr objType = savedObjectType(data.objectTypeNum, i);
			if (objType)	//We have an object type to copy from!!
			{

				obj->init(true, objType);
				obj->setExists(true);
				obj->Load(&data);
			}
			else
			{
				MC2_DESTROY(obj, "load_empty_slot");
			}
			curBoltNum++;
		}
		else
		{
			// checkSavedObjects refuses such a file before the mission is torn down; this keeps the loader from
			// re-reading the same packet if one gets here anyway.
			mc2::fmt::print_err("[SAVE v1] event=object_skipped reason=unknown_record_size slot={:d} packet={:d} size={:d}\n", mc2::fmt::as_i32(i), mc2::fmt::as_i32(packetNum), mc2::fmt::as_i32(static_cast<int>(packetSize)));
			packetNum++;
		}
	}

	if (curTerrObjNum != numTerrainObjects)
		STOP(("Didn't load %d but instead %d TerrainObjects",numTerrainObjects,curTerrObjNum));

	if (curBuildingNum != numBuildings)
		STOP(("Didn't load %d but instead %d Buildings",numBuildings,curBuildingNum));

	if (curTurretNum != numTurrets)
		STOP(("Didn't load %d but instead %d Turrets",numTurrets,curTurretNum));

	if (curGateNum != numGates)
		STOP(("Didn't load %d but instead %d Gates",numGates,curGateNum));

	if (curArtilleryNum != numArtillery)
		STOP(("Didn't load %d but instead %d Artillery",numArtillery,curArtilleryNum));

	if (curCarnageNum != numCarnage)
		STOP(("Didn't load %d but instead %d Carnage",numCarnage,curCarnageNum));

	if (curMechNum != maxMechs)
		STOP(("Didn't load %d but instead %d Mechs",maxMechs,curMechNum));

	if (curVehicleNum != maxVehicles)
		STOP(("Didn't load %d but instead %d Vehicles",maxVehicles,curVehicleNum));

	if (curBoltNum != numWeapons)
		STOP(("Didn't load %d but instead %d WeaponBolts",numWeapons,curBoltNum));

	rebuildCollidableList = true;

	//---------------------------------------------------
	// Finally, let's build the control building lists...
	for (i = 0; i < numTurrets; i++) 
	{
		if (mc2::mission::parentCell(turrets[i]->parentId).has_value())
		{
			BuildingPtr controlBuilding = static_cast<BuildingPtr>(ObjectManager->findByCellPosition((turrets[i]->parentId>>16),(turrets[i]->parentId & 0x0000ffff)));
			if (controlBuilding && !controlBuilding->getFlag(OBJECT_FLAG_CONTROLBUILDING)) 
			{
				controlBuilding->setFlag(OBJECT_FLAG_CAPTURABLE, true);
				controlBuilding->setFlag(OBJECT_FLAG_CONTROLBUILDING, true);
				controlBuilding->listID = numTurretControls;
				turretControls[numTurretControls++] = controlBuilding;
			}
			else if (!controlBuilding)
			{
				glm::vec3 worldPos;
				if (land)
					land->cellToWorld((turrets[i]->parentId>>16),(turrets[i]->parentId & 0x0000ffff),worldPos);
				PAUSE(("Turret linked to bldg @ R %d, C %d  X:%f Y:%f No Bldg there!",(turrets[i]->parentId>>16),(turrets[i]->parentId & 0x0000ffff),worldPos.x,worldPos.y));
				turrets[i]->parentId = 0xffffffff;
			}
		}
	}

	for (i = 0; i < numGates; i++) 
	{
		if (mc2::mission::parentCell(gates[i]->parentId).has_value())
		{
			BuildingPtr controlBuilding = static_cast<BuildingPtr>(ObjectManager->findByCellPosition((gates[i]->parentId>>16),(gates[i]->parentId & 0x0000ffff)));
			if (controlBuilding && !controlBuilding->getFlag(OBJECT_FLAG_CONTROLBUILDING)) 
			{
				controlBuilding->setFlag(OBJECT_FLAG_CONTROLBUILDING, true);
				controlBuilding->listID = numGateControls;
				gateControls[numGateControls++] = controlBuilding;
			}
	   		else if (!controlBuilding)
	   		{
	   			PAUSE(("Gate tried to link to building at Row %d, Col %d.  No Bldg there!",(gates[i]->parentId>>16),(gates[i]->parentId & 0x0000ffff)));
				gates[i]->parentId = 0xffffffff;
	   		}
		}
	}

	//-----------------------------------------------------------------------------------
	// Create list of special buildings.  These buildings will be updated at least once
	// every frame regardless of where they are on the terrain and what is visible.
	// This is because perimeter alarms and lookout buildings and ????? must work even
	// if the player is NOT looking at them!!
	for (i=0;i<numBuildings;i++)
	{
		if (buildings[i]->isSpecialBuilding())
		{
			specialBuildings[numSpecialBuildings++] = buildings[i];
		}
	}
	
 	//--------------------------------------------------------------------
	//Now, lets point every lit building to at least one power generator.
	for (i=0;i<numBuildings;i++)
	{
		if (buildings[i]->isPowerSource())
		{
			powerGenerators[numPowerGenerators++] = buildings[i];
		}
	}
	
	if (numPowerGenerators)
	{
		for (i=0;i<numBuildings;i++)
		{
			glm::vec3 maxDist;
			long generatorIndex = 0;
			maxDist = buildings[i]->getPosition() - powerGenerators[0]->getPosition();
			float minDistance = glm::length(maxDist);
			
			for (long j=1;j<numPowerGenerators;j++)
			{
				maxDist = buildings[i]->getPosition() - powerGenerators[j]->getPosition(); 
				float newDistance = glm::length(maxDist); 
				
				if (newDistance < minDistance)
				{
					generatorIndex = j;
					minDistance = newDistance;
				}
			}	
			
			buildings[i]->setPowerSupply(powerGenerators[generatorIndex]);
		}
		
		for (i=0;i<numTerrainObjects;i++)
		{
			glm::vec3 maxDist;
			long generatorIndex = 0;
			maxDist = terrainObjects[i]->getPosition() - powerGenerators[0]->getPosition();
			float minDistance = glm::length(maxDist);
			
			for (long j=1;j<numPowerGenerators;j++)
			{
				maxDist = terrainObjects[i]->getPosition() - powerGenerators[j]->getPosition(); 
				float newDistance = glm::length(maxDist); 
				
				if (newDistance < minDistance)
				{
					generatorIndex = j;
					minDistance = newDistance;
				}
			}	
			
			terrainObjects[i]->setPowerSupply(powerGenerators[generatorIndex]);
		}
	}

	GameObject::setInitialize(false);

	//Reload the Object Watchers
	int32_t *watchSave = static_cast<int32_t*>(malloc(sizeof(int32_t) * (getMaxObjects() + 1)));
	memset(watchSave,0,sizeof(int32_t) * (getMaxObjects() + 1));

	file->readPacket(packetNum,reinterpret_cast<MemoryPtr>(watchSave));
	packetNum++;

	//Find the watchID from the watchlist for this object.
	// If none, let it be.  It'll be zero already
	// DO NOT CALL getWatchID!!!!!!!
	// That will assign them to objects which don't have them!!!!
	// WATCHID-LOAD-GUARD-1: watchSave[] is untrusted on-disk data. A corrupt or
	// cross-version save can hold an index outside [0, maxObj]; objList is sized
	// maxObj+1. Validate each entry before dereferencing objList. On an invalid
	// index, invalidate that watch slot (treat the saved object reference as
	// gone) and continue — never OOB-read objList, never crash the load. Loop is
	// bounded by the allocated capacity (getMaxObjects()), not any serialized
	// value.
	const long _maxObj = getMaxObjects();
	for (int j=0;j<_maxObj;j++)
	{
		const int32_t _idx = watchSave[j];
		if (!mc2watch::isValidWatchSaveIndex(_idx, _maxObj))
		{
			watchList[j] = nullptr;
			mc2_watchidLoadDiag("watchSave", static_cast<long>(_idx), _maxObj);
			continue;
		}
		watchList[j] = objList[_idx];
	}

	free(watchSave);
	watchSave = nullptr;

	return packetNum;
}

std::vector<uint8_t> GameObjectManager::saveAnimationState() {
    mc2::save::Archive archive;
    uint32_t state = mc2GetSimRngState(), draws = SimRandomDraws();
    uint32_t animState = mc2_anim_rng::state(), animDraws = mc2_anim_rng::drawCount();
    uint32_t count = 0;
    for (long i = 1; i <= getMaxObjects(); ++i)
        if (objList[i] && objList[i]->getExists() && objList[i]->getAppearance() &&
            objList[i]->getAppearance()->simulationOwned()) ++count;
    archive(state, draws, animState, animDraws, count);
    for (long i = 1; i <= getMaxObjects(); ++i) {
        auto* object = objList[i];
        if (!object || !object->getExists() || !object->getAppearance() ||
            !object->getAppearance()->simulationOwned()) continue;
        int32_t handle = static_cast<int32_t>(i);
        archive(handle, *object->getAppearance()->simulationAnimation());
    }
    // Pool order: the sim updates turrets, buildings, ... in pool order and a load fills each pool in handle order, so objects
    // that draw from the sim RNG in the same tick (idle turrets) would draw in a different order after a load.
    auto poolOrder = [&](auto pool) {
        std::vector<int32_t> handles;
        if (pool.data() && pool.size() <= mc2::save::maxSequenceLength) {
            handles.reserve(pool.size());
            for (auto* object : pool) handles.push_back(object ? static_cast<int32_t>(object->getHandle()) : -1);
        }
        archive(handles);
    };
    poolOrder(std::span(turrets, static_cast<size_t>(numTurrets))); poolOrder(std::span(buildings, static_cast<size_t>(numBuildings)));
    poolOrder(std::span(gates, static_cast<size_t>(numGates))); poolOrder(std::span(artillery, static_cast<size_t>(numArtillery)));
    poolOrder(std::span(carnage, static_cast<size_t>(numCarnage))); poolOrder(std::span(weapons, static_cast<size_t>(numWeapons)));
    // Sensor scan state: one team scans per tick and getSensorsActive reads the flag the scans leave; mission scripts branch on it.
    int32_t scanCursor = SensorManager ? static_cast<int32_t>(SensorManager->getScanCursor()) : 0;
    bool enemyInLOS = SensorSystemManager::enemyInLOS;
    archive(scanCursor, enemyInLOS);
    if (!archive.good()) Fatal(0, "Unable to serialize authoritative animation state");
    return archive.bytes();
}
bool GameObjectManager::restoreAnimationState(const std::vector<uint8_t>& bytes, uint32_t sectionVersion) {
    mc2::save::Archive archive(bytes);
    archive.sectionVersion = sectionVersion;
    uint32_t state = 0, draws = 0, animState = 0, animDraws = 0, count = 0;
    archive(state, draws, animState, animDraws, count);
    if (!archive.good() || count > static_cast<uint32_t>(getMaxObjects())) return false;
    std::vector<std::pair<int32_t, mc2::sim::UnitAnimState>> states;
    int32_t previousHandle = 0;
    for (uint32_t i = 0; i < count; ++i) {
        int32_t handle = 0;
        mc2::sim::UnitAnimState animation;
        archive(handle, animation);
        if (!archive.good() || handle < 1 || handle > getMaxObjects() ||
              !objList[handle] || !objList[handle]->getAppearance() ||
              !objList[handle]->getAppearance()->simulationOwned()) return false;
        const auto& expected = *objList[handle]->getAppearance()->simulationAnimation();
        if (handle <= previousHandle ||
            animation.nodeUsed.size() != expected.nodeUsed.size() ||
            animation.nodeRecycle.size() != expected.nodeRecycle.size()) return false;
        states.emplace_back(handle, std::move(animation));
        previousHandle = handle;
    }
    // Pool orders follow the animation states; saves written before them end here and keep the pools in handle order.
    std::vector<int32_t> order[6];
    if (archive.remaining() > 0)
        for (auto& handles : order) archive(handles);
    // Saves written before the sensor scan state end after the pool orders (or before them) and keep the defaults.
    int32_t scanCursor = 0;
    bool enemyInLOS = SensorSystemManager::enemyInLOS, hasScan = false;
    if (archive.remaining() > 0) {
        archive(scanCursor, enemyInLOS);
        hasScan = true;
        if (archive.good() && (!SensorManager || scanCursor < 0 || scanCursor >= Team::numTeams)) archive.reject("sensor_scan_cursor");
    }
    if (!archive.finish()) return false;
    if (hasScan) SensorManager->restoreScan(scanCursor, enemyInLOS);
    mc2::save::applyPoolOrder(std::span(turrets, static_cast<size_t>(numTurrets)), order[0], objList, getMaxObjects());
    mc2::save::applyPoolOrder(std::span(buildings, static_cast<size_t>(numBuildings)), order[1], objList, getMaxObjects());
    mc2::save::applyPoolOrder(std::span(gates, static_cast<size_t>(numGates)), order[2], objList, getMaxObjects());
    mc2::save::applyPoolOrder(std::span(artillery, static_cast<size_t>(numArtillery)), order[3], objList, getMaxObjects());
    mc2::save::applyPoolOrder(std::span(carnage, static_cast<size_t>(numCarnage)), order[4], objList, getMaxObjects());
    mc2::save::applyPoolOrder(std::span(weapons, static_cast<size_t>(numWeapons)), order[5], objList, getMaxObjects());
    for (auto& [handle, animation] : states) {
        auto* appearance = objList[handle]->getAppearance();
        *appearance->simulationAnimation() = std::move(animation);
        appearance->simPose->ready = false;
        appearance->refreshSimPose();
    }
    mc2RestoreSimRngState(state); RestoreSimRandomDraws(draws);
    mc2_anim_rng::restore(animState, animDraws);
    return true;
}
