//---------------------------------------------------------------------------
//
//	objmgr.cpp - This file contains the GameObject Manager class code
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "objmgr_internal.h"


// ---------------------------------------------------------------------------
// MF3-GENERATIONAL-HANDLE-1: generational watch-id validation killswitch.
// MC2_WATCHID_GENERATION present -> bump-on-free + validate-on-access via the
// watchGeneration[] side array. Default off: the side array is still allocated
// in lockstep with watchList[] (always valid) but is never read/written, so
// behavior is byte-identical to before. No slot reuse this slice.
// ---------------------------------------------------------------------------
static const bool s_watchGen = mc2::gate::get<"MC2_WATCHID_GENERATION">();

constexpr int BRIDGE_OBJTYPE = 448;
constexpr int MINE1 = 60;
constexpr int MINE2 = 251;
constexpr int MINE3 = 521;
constexpr int MINE4 = 522;
constexpr int MINE5 = 610;
constexpr int MINE6 = 611;
constexpr int MINE7 = 612;

//---------------------------------------------------------------------------
// Static Globals
#if 1	// ids can be 8 long, INCLUDING TERMINATOR!!!!!
char DEFAULT_LIST_ID[] = "DEFAULT";
char CLANMECH_LIST_ID[] = "CLANMEC";
char ISMECH_LIST_ID[] = "ISMECH";
char ICON_LIST_ID[] = "ICONS";
char WEAPON_LIST_ID[] = "WEAPON";
#else
char DEFAULT_LIST_ID[] = "DEFAULT!";
char CLANMECH_LIST_ID[] = "CLANMECH";
char ISMECH_LIST_ID[] = "ISMECH";
char ICON_LIST_ID[] = "ICONS";
char WEAPON_LIST_ID[] = "WEAPON";
#endif

GameObjectManagerPtr ObjectManager = nullptr;
GameObjectPtr* collisionList = nullptr;
long numCollidables = 0;
//***************************************************************************
//* MISC routines
//***************************************************************************

bool blockInList (long blockNum) 
{
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;

	for (long i = 0; i < totalBlocks; i++) {
		if (usedBlockList[i] == blockNum)
			return(TRUE);
		else if (usedBlockList[i] == -1)
			return(FALSE);
	}
	
	return(FALSE);
}

//---------------------------------------------------------------------------

bool moverInList (long blockNum) 
{
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;

	for (long i = 0; i < totalBlocks; i++) {
		if (moverBlockList[i] == blockNum)
			return(TRUE);
		else if (moverBlockList[i] == -1)
			return(FALSE);
	}
	
	return(FALSE);
}

//***************************************************************************
//* GAMEOBJECT MANAGER class
//***************************************************************************

void* GameObjectManager::operator new (size_t ourSize) {

	void* result = systemHeap->Malloc(ourSize);
	if (!result) 
	{
		Fatal(0, " GameObjectManager.new: unable to create GameObject Manager ");
	}

	return(result);
}

//---------------------------------------------------------------------------

void GameObjectManager::operator delete (void* us) {

	systemHeap->Free(us);
}

//---------------------------------------------------------------------------

void GameObjectManager::init (void) {

	ownedTypeManager.reset();
	objTypeManager = nullptr;

	numMechs = 0;
	numVehicles = 0;
	numElementals = 0;
	numTerrainObjects = 0;
	numBuildings = 0;
	numTurrets = 0;
	numWeapons = 0;
	numCarnage = 0;
	numLights = 0;
	numGates = 0;
	numArtillery = 0;

	numGateControls = 0;
	numTurretControls = 0;
	numPowerGenerators = 0;
	numSpecialBuildings = 0;

	//carnageCount = 0;
	Terrain::numObjBlocks = 0;

	mechs = nullptr;
	vehicles = nullptr;
	elementals = nullptr;
	terrainObjects = nullptr;
	buildings = nullptr;
	turrets = nullptr;
	weapons = nullptr;
	carnage = nullptr;
	lights = nullptr;
	artillery = nullptr;
	gates = nullptr;

	objList = nullptr;
	collidableList = nullptr;
	numCollidables = 0;
	numGoodMovers = 0;
	numBadMovers = 0;
	numMovers = 0;
	nextReinforcementPartId = MIN_REINFORCEMENT_PART_ID;
	numRemoved = 0;
	nextWatchID = 1;

	currentWeaponsIndex = 0;
	currentCarnageIndex = 0;
	currentLightIndex = 0;
	currentArtilleryIndex = 0;
	
	long totalBlocks = Terrain::blocksMapSide * Terrain::blocksMapSide;

	for (long i = 0; i < totalBlocks; i++) {
		Terrain::objBlockInfo[i].active = false;
		Terrain::objBlockInfo[i].firstHandle = 0;
		Terrain::objBlockInfo[i].numObjects = 0;
	}

}

//---------------------------------------------------------------------------

void GameObjectManager::init (const char* objTypeDataFile, long objTypeHeapSize, long objHeapSize) {

	// The previous manager goes first (its destroy() deletes the static type/object heaps the new init() recreates).
	ownedTypeManager.reset();
	ownedTypeManager = std::make_unique<ObjectTypeManager>();
	objTypeManager = ownedTypeManager.get();
	if (!objTypeManager)
		Fatal(0, " GameObjectManager.init: unable to create objTypeManager ");
	long result = objTypeManager->init(objTypeDataFile, objTypeHeapSize, objHeapSize);
	if (result != NO_ERR)
		Fatal(0, " GameObjectManager.init: unable to init objTypeManager ");
}

//---------------------------------------------------------------------------

void GameObjectManager::setNumObjects (long nMechs,
									   long nVehicles,
									   long nElementals,
									   long nTerrainObjects,
									   long nBuildings,
									   long nTurrets,
									   long nWeapons,
									   long nCarnage,
									   long nLights,
									   long nArtillery,
									   long nGates) 
{

	long i=0;

	numMechs = nMechs;
	numVehicles = nVehicles;
	numElementals = nElementals;

	//-------------------------------------------------------------------------
	// We need to make room for a potential max # of reinforcements per team...
	//
	// Please DONT add 2 to each of these.   You wind up pointing into the next list!!!!
	// -fs   6/14/2000
	maxMechs = numMechs + MAX_TEAMS * MAX_REINFORCEMENTS_PER_TEAM;
	maxVehicles = numVehicles + MAX_TEAMS * MAX_REINFORCEMENTS_PER_TEAM;
	maxMovers = maxMechs + maxVehicles + numElementals;

	if (nTerrainObjects > -1)
		numTerrainObjects = nTerrainObjects;
	if (nBuildings > -1)
		numBuildings = nBuildings;
	if (nTurrets > -1)
		numTurrets = nTurrets;
	if (nWeapons > -1)
		numWeapons = nWeapons;
	if (nCarnage > -1)
		numCarnage = nCarnage;
	if (nLights > -1)
		numLights = nLights;
	if (nArtillery > -1)
		numArtillery = nArtillery;
	if (nGates > -1)
		numGates = nGates;

	GameObject::setInitialize(true);

	//-----------------------------------------------------------
	// First element in list is NULL (handle of 0 is always NULL)
	objList = bindPoolArray(objListStore, (getMaxObjects() + 1));
	memset(objList,0,sizeof(GameObjectPtr) * (getMaxObjects() + 1));

	watchList = bindPoolArray(watchListStore, (getMaxObjects() + 1));
	memset(watchList,0,sizeof(GameObjectPtr) * (getMaxObjects() + 1));

	// MF3-GENERATIONAL-HANDLE-1: generation side array, LOCKSTEP with watchList
	// (same size, same memset-0). Allocated unconditionally so the pointer is
	// always valid; only read/written under s_watchGen. Owned by watchGenerationStore
	// like watchList (released by releasePoolArrays() in destroy()).
	watchGeneration = bindPoolArray(watchGenerationStore, (getMaxObjects() + 1));
	memset(watchGeneration,0,sizeof(uint16_t) * (getMaxObjects() + 1));

	long curHandle = 1;
	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (maxMechs > 0) {
		mechs = bindPoolArray(mechStore, maxMechs);
		if (!mechs)
			Fatal(maxMechs, " GameObjectManager.setNumObjects: cannot malloc mechs ");
		for (i = 0; i < maxMechs; i++) {
			mechs[i] = new BattleMech;
			mechs[i]->setHandle(curHandle);
			objList[curHandle++] = mechs[i];
		}
		for (i = numMechs; i < maxMechs; i++)
			MC2_DESTROY(mechs[i], "pool_unused");
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (maxVehicles > 0) {
		vehicles = bindPoolArray(vehicleStore, maxVehicles);
		if (!vehicles)
			Fatal(maxVehicles, " GameObjectManager.setNumObjects: cannot malloc vehicles ");
		for (i = 0; i < maxVehicles; i++) {
			vehicles[i] = new GroundVehicle;
			vehicles[i]->setHandle(curHandle);
			objList[curHandle++] = vehicles[i];
		}
		for (i = numVehicles; i < maxVehicles; i++)
			MC2_DESTROY(vehicles[i], "pool_unused");
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
#ifdef USE_ELEMENTALS
	if (numElementals > 0) {
		elementals = bindPoolArray(elementalStore, numElementals);
		if (!elementals)
			Fatal(numElementals, " GameObjectManager.setNumObjects: cannot malloc elementals ");
		for (i = 0; i < numElementals; i++) {
			elementals[i] = new Elemental;
			elementals[i]->setHandle(curHandle);
			objList[curHandle++] = elementals[i];
		}
	}
#endif

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (numTerrainObjects > 0) {
		terrainObjects = bindPoolArray(terrainObjectStore, numTerrainObjects);
		if (!terrainObjects)
			Fatal(numTerrainObjects, " GameObjectManager.setNumObjects: cannot malloc terrain objects ");

		for (i = 0; i < numTerrainObjects; i++) {
			terrainObjects[i] = new TerrainObject;
			terrainObjects[i]->setHandle(curHandle);
			objList[curHandle++] = terrainObjects[i];
		}
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (numBuildings > 0) {
		buildings = bindPoolArray(buildingStore, numBuildings);
		if (!buildings)
			Fatal(numBuildings, " GameObjectManager.setNumObjects: cannot malloc buildings ");
		for (i = 0; i < numBuildings; i++) {
			buildings[i] = new Building;
			buildings[i]->setHandle(curHandle);
			objList[curHandle++] = buildings[i];
		}
	}

	if (numTurrets > 0)
	{
		turrets = bindPoolArray(turretStore, numTurrets);
		if ( !turrets )
			Fatal(numTurrets, " GameObjectManager.setNumObjects: cannot malloc Turrets ");
			
		for (i = 0; i < numTurrets; i++) 
		{
			turrets[i] = new Turret;
			turrets[i]->setHandle(curHandle);
			objList[curHandle++] = turrets[i];
		}
	}

	if (numGates > 0)
	{
		gates = bindPoolArray(gateStore, numGates);
		if ( !gates )
			Fatal(numGates, " GameObjectManager.setNumObjects: cannot malloc Gates ");
			
		for (i = 0; i < numGates; i++) 
		{
			gates[i] = new Gate;
			gates[i]->setHandle(curHandle);
			objList[curHandle++] = gates[i];
		}
	}

	//--------------------------------------------------------------
	// For now, we'll use an array of pointers due to the irritating
	// 'new' for arrays problem...
	if (numWeapons > 0) {
		weapons = bindPoolArray(weaponStore, numWeapons);
		if (!weapons)
			Fatal(numWeapons, " GameObjectManager.setNumObjects: cannot malloc weapons ");
		for (i = 0; i < numWeapons; i++) {
			weapons[i] = new WeaponBolt;
			weapons[i]->setHandle(curHandle);
			objList[curHandle++] = weapons[i];
		}
	}

	if (numCarnage > 0) {
		carnage = bindPoolArray(carnageStore, numCarnage);
		if (!carnage)
			Fatal(numCarnage, " GameObjectManager.setNumObjects: cannot malloc carnage ");
		for (i = 0; i < numCarnage; i++) {
			carnage[i] = new Carnage;
			carnage[i]->setHandle(curHandle);
			objList[curHandle++] = carnage[i];
		}
	}

	if (numArtillery > 0) {
		artillery = bindPoolArray(artilleryStore, numArtillery);
		if (!artillery)
			Fatal(numArtillery, " GameObjectManager.setNumObjects: cannot malloc artillery ");
		for (i = 0; i < numArtillery; i++) {
			artillery[i] = new Artillery;
			artillery[i]->setHandle(curHandle);
			objList[curHandle++] = artillery[i];
		}
	}

	useMoverLineOfSightTable = true;
	moverLineOfSightTable = static_cast<char*>(systemHeap->Malloc(maxMovers * maxMovers));
	if (!moverLineOfSightTable)
		Fatal(numGates, " GameObjectManager.setNumObjects: cannot malloc moverLineOfSightTable ");

	GameObject::setInitialize(false);
}

//---------------------------------------------------------------------------

BattleMechPtr GameObjectManager::getMech (long mechIndex) {

	if (!mechs || (mechIndex < 0) || (mechIndex >= numMechs))
		return(nullptr);

	return(mechs[mechIndex]);
}

//---------------------------------------------------------------------------

GroundVehiclePtr GameObjectManager::getVehicle (long vehicleIndex) {

	if (!vehicles || (vehicleIndex < 0) || (vehicleIndex >= numVehicles))
		return(nullptr);

	return(vehicles[vehicleIndex]);
}

//---------------------------------------------------------------------------

GroundVehiclePtr GameObjectManager::getOpenVehicle (void)
{
	for (long i=0;i<maxVehicles;i++)
	{
		if (!vehicles[i]->getExists())
		{
			if (i > numVehicles)
				numVehicles = i;

			return vehicles[i];
		}
	}

	return nullptr;
}

//---------------------------------------------------------------------------

ElementalPtr GameObjectManager::getElemental (long elementalIndex) {

	if (!elementals || (elementalIndex < 0) || (elementalIndex >= numElementals))
		return(nullptr);

	return(elementals[elementalIndex]);
}

//---------------------------------------------------------------------------

BattleMechPtr GameObjectManager::newMech (void) {

	if (numMechs < maxMechs) {
		mechs[numMechs]->init(false);
		mechs[numMechs]->setExists(true);
		setPartId(mechs[numMechs], -1, -1, -1);
		mechs[numMechs]->watchID = 0;
		
		//Gotta rebuild the list or the new stuff don't show up!
		// This flag will force rebuild at next collision check
		rebuildCollidableList = true;
		
		return(mechs[numMechs++]);
	}

	return(nullptr);
}

//---------------------------------------------------------------------------

GroundVehiclePtr GameObjectManager::newVehicle (void) {

	if (numVehicles < maxVehicles) {
		vehicles[numVehicles]->init(false);
		vehicles[numVehicles]->setExists(true);
		setPartId(vehicles[numVehicles], -1, -1, -1);
		vehicles[numVehicles]->watchID = 0;
		
		//Gotta rebuild the list or the new stuff don't show up!
		// This flag will force rebuild at next collision check
		rebuildCollidableList = true;
		
 		return(vehicles[numVehicles++]);
	}

	return(nullptr);
}

//---------------------------------------------------------------------------

//---------------------------------------------------------------------------

void GameObjectManager::setWatchID (GameObjectPtr obj) {

	if (obj->watchID == 0) {
		// OBJMGR-WATCHID-BOUNDS-1: watchList is sized getMaxObjects()+1 at
		// allocation and getMaxObjects() never grows after load, so valid
		// indices are [0, getMaxObjects()]. On watch-ID exhaustion (long
		// reinforcement churn burns a fresh slot per re-acquire) fail safe:
		// leave the object un-watchable (watchID stays 0) instead of writing
		// OOB. Reuse is deliberately avoided — it would reintroduce ABA /
		// stale-target hazards. The Save-side clamp is independent (see ::Save).
		if (!mc2watch::canAssignWatchId(nextWatchID, getMaxObjects())) {
			static unsigned long s_watchIdDrops = 0;
			++s_watchIdDrops;
			if (s_watchIdDrops == 1)
				fprintf(stderr, "[WATCHID_BOUND] watch-ID space exhausted "
					"(cap=%ld); object left un-watchable. Further drops rate-limited.\n",
					getMaxObjects());
			if (mc2_diag::tagEnabled("WATCHID_BOUND") &&
				(s_watchIdDrops == 1 || (s_watchIdDrops & 0xFF) == 0)) {
				char _wb_buf[128];
				snprintf(_wb_buf, sizeof(_wb_buf),
					"{\"nextWatchID\":%lu,\"cap\":%ld,\"drops\":%lu}",
					nextWatchID, getMaxObjects(), s_watchIdDrops);
				mc2_diag::writeEvent("WATCHID_BOUND", 1, 0, _wb_buf);
			}
			return;
		}
		watchList[nextWatchID] = obj;
		// MF3-GENERATIONAL-HANDLE-1: stamp the slot's generation on assign (gated).
		// No reuse this slice, so nextWatchID is a fresh slot (gen 0 -> 1).
		if (s_watchGen)
			watchGeneration[nextWatchID] = mc2watch::nextGenerationOnAssign(watchGeneration[nextWatchID]);
		obj->watchID = nextWatchID++;
	}
}

//---------------------------------------------------------------------------
// MF3-GENERATIONAL-HANDLE-1: additive generational resolve. When the gate is
// OFF this is behavior-identical to getByWatchID (generation ignored). When ON,
// a handle whose expectedGen no longer matches the slot's current generation is
// rejected (fail-safe NULL + rate-limited diag), the same shape as an
// unresolvable id. No hard assert: a stale result is a recoverable drop, like
// the existing un-watchable / load-invalidate fail-safes. Zero callers this
// slice; exercised by gate-ON proof only.
GameObjectPtr GameObjectManager::getByWatchIDGenerational (unsigned long watchID, uint16_t expectedGen) {

	if (!mc2watch::isResolvableWatchId(watchID, nextWatchID))
		return nullptr;
	if (s_watchGen && !mc2watch::generationMatches(watchGeneration[watchID], expectedGen)) {
		static unsigned long s_watchStaleDrops = 0;
		++s_watchStaleDrops;
		if (mc2_diag::tagEnabled("WATCHID_STALE") &&
			(s_watchStaleDrops == 1 || (s_watchStaleDrops & 0xFF) == 0)) {
			char _ws_buf[160];
			snprintf(_ws_buf, sizeof(_ws_buf),
				"{\"watchID\":%lu,\"expected\":%u,\"stored\":%u,\"drops\":%lu}",
				watchID, static_cast<unsigned>(expectedGen),
				static_cast<unsigned>(watchGeneration[watchID]), s_watchStaleDrops);
			mc2_diag::writeEvent("WATCHID_STALE", 1, 0, _ws_buf);
		}
		return nullptr;
	}
	return watchList[watchID];
}

//---------------------------------------------------------------------------

void GameObjectManager::freeMover (MoverPtr mover) {

	bool foundIt = modifyMoverLists(mover, MOVERLIST_DELETE);
	if (foundIt) {
		mover->release();
		MC2_DESTROY(mover, "mover_freed");
		mover->setFlag(OBJECT_FLAG_REMOVED, true);
		mover->setPartId(0);
		// MF3-GENERATIONAL-HANDLE-1: bump the slot's generation on free (gated) so
		// any cross-frame handle captured at the old generation reads as stale.
		if (s_watchGen)
			watchGeneration[mover->watchID] = mc2watch::bumpGenerationOnFree(watchGeneration[mover->watchID]);
		watchList[mover->watchID] = nullptr;
		mover->watchID = 0;
	}
}

//---------------------------------------------------------------------------

void GameObjectManager::tradeMover (MoverPtr mover, long newTeamID, long newCommanderID) {

	if (newTeamID > -1) {
		if (mover->teamId != newTeamID)
			mover->salvaged = true;
		if (mover->getObjectClass() == BATTLEMECH) {
			(static_cast<BattleMech*>(mover))->killed = false;
			(static_cast<BattleMech*>(mover))->lost = false;
		}
		mover->tradeRefresh();
	}
	mover->setTeamId(newTeamID, true);
	if (mover->getGroup())
		mover->getGroup()->remove(mover);
	mover->watchID = 0;
	if (newTeamID > -1)
		Team::teams[newTeamID]->addToRoster(mover);
	mover->setCommanderId(newCommanderID);
	modifyMoverLists(mover, MOVERLIST_TRADE);
}

//---------------------------------------------------------------------------

#ifdef USE_ELEMENTALS

ElementalPtr GameObjectManager::addElemental (void) {

	if (numMechs < maxMechs) {
		elementals[numElementals]->setExists(true);
		setPartId(elementals[numElementals], -1, -1, -1);
		return(elementals[numElementals++]);
	}
	return(nullptr);
}

#endif

//---------------------------------------------------------------------------

TerrainObjectPtr GameObjectManager::getTerrainObject (long terrainObjectIndex) {

	if (!terrainObjects || (terrainObjectIndex < 0) || (terrainObjectIndex >= numTerrainObjects))
		return(nullptr);

	return(terrainObjects[terrainObjectIndex]);
}

//---------------------------------------------------------------------------

BuildingPtr GameObjectManager::getBuilding (long buildingIndex) {

	if (!buildings || (buildingIndex < 0) || (buildingIndex >= numBuildings))
		return(nullptr);

	return(buildings[buildingIndex]);
}

//---------------------------------------------------------------------------

TurretPtr GameObjectManager::getTurret (long turretIndex) 
{
	if (!turrets || (turretIndex < 0) || (turretIndex >= numTurrets))
		return(nullptr);

	return(turrets[turretIndex]);
}

//---------------------------------------------------------------------------

GatePtr GameObjectManager::getGate (long gateIndex) 
{
	if (!gates || (gateIndex < 0) || (gateIndex >= numGates))
		return(nullptr);

	return(gates[gateIndex]);
}

//---------------------------------------------------------------------------

WeaponBoltPtr GameObjectManager::getWeapon (void) 
{
	if (!weapons || (currentWeaponsIndex < 0) || (currentWeaponsIndex >= numWeapons))
		return(nullptr);

	currentWeaponsIndex++;
	if (currentWeaponsIndex >= numWeapons)
		currentWeaponsIndex = 0;

	//Make sure this weapon has an opportunity to damage its target
	weapons[currentWeaponsIndex]->finishNow();	

	return(weapons[currentWeaponsIndex]);
}

//---------------------------------------------------------------------------

CarnagePtr GameObjectManager::getCarnage (CarnageEnumType carnageType) {

	if (!carnage || (currentCarnageIndex < 0) || (currentCarnageIndex >= numCarnage))
		return(nullptr);

	currentCarnageIndex++;
	if (currentCarnageIndex >= numCarnage)
		currentCarnageIndex = 0;

	carnage[currentCarnageIndex]->finishNow();
	carnage[currentCarnageIndex]->init(carnageType);
	
	return(carnage[currentCarnageIndex]);
}

//---------------------------------------------------------------------------

void GameObjectManager::releaseCarnage (CarnagePtr obj) {

	obj->setOwner(nullptr);
	MC2_DESTROY(obj, "pool_released");
}

//---------------------------------------------------------------------------

LightPtr GameObjectManager::getLight (void) {

	if (!lights || (currentLightIndex < 0) || (currentLightIndex >= numLights))
		return(nullptr);

	if (currentLightIndex >= numLights)
		currentLightIndex = 0;

//	lights[currentLightIndex]->finishNow();
	lights[currentLightIndex]->init(false);
	lights[currentLightIndex]->setExists(true);
	return(lights[currentLightIndex++]);
}

//---------------------------------------------------------------------------

void GameObjectManager::releaseLight (LightPtr obj) {

	MC2_DESTROY(obj, "pool_released");
//	obj->setOwner(NULL);
}

//---------------------------------------------------------------------------

ArtilleryPtr GameObjectManager::getArtillery (void) 
{
	if (!artillery || (currentArtilleryIndex < 0) || (currentArtilleryIndex >= numArtillery))
		STOP(("Artillery Strikes are out of range in ObjectManager %d",currentArtilleryIndex));

	currentArtilleryIndex++;
	if (currentArtilleryIndex >= numArtillery)
		currentArtilleryIndex = 0;

	//OK to return NULL now.  Lets Multiplayer know that there are no more strikes available.
	if (artillery[currentArtilleryIndex]->getExists())
		return nullptr;
		
	artillery[currentArtilleryIndex]->init(false);
	artillery[currentArtilleryIndex]->setExists(true);
	
	return(artillery[currentArtilleryIndex]);
}

//---------------------------------------------------------------------------

inline bool isLandMine (long objTypeNum) 
{
	return false;
}


void GameObjectManager::addObject (ObjDataLoader *objData, long& curTerrainObjectIndex, 
								  long& curBuildingIndex, long& curTurretIndex, long &curGateIndex,
								  long& curCollidableHandle, long& curNonCollidableHandle )
{
	// THIS IS NOT COMPLETELY DONE! MAKE SURE ALL TERRAIN OBJ TYPES
	// ARE ACCOUNTED FOR HERE!

	long objTypeNum = objData->objTypeNum;
	if (!isLandMine(objTypeNum)) {
		ObjectTypePtr objType = getObjectType(objTypeNum);
		GameObjectPtr obj = nullptr;
		if (!objType)
			return;
			//Fatal();
		
		Stuff::Vector2DOf<long> numbers;
		numbers.x = objData->vertexNumber;
		numbers.y = objData->blockNumber;

		unsigned char realDmg = objData->damage & 0x0f;
		switch (objType->getObjectClass()) {
			case TERRAINOBJECT:
			case TREE:
				obj = getTerrainObject(curTerrainObjectIndex++);
				if (obj)
				{
					(static_cast<TerrainObjectPtr>(obj))->init(true, objType);
					if (realDmg)
						(static_cast<TerrainObjectPtr>(obj))->setDamage((static_cast<TerrainObjectTypePtr>(objType))->getDamageLevel());
						
					objList[curCollidableHandle] = obj;
					obj->setHandle(curCollidableHandle++);
					
					obj->setExists(true);
				}
				else
				{
					Fatal(curBuildingIndex," No More Trees ");
				}
				break;
			case TURRET:
				obj = getTurret(curTurretIndex++);
				if (obj)
				{
					(static_cast<TurretPtr>(obj))->init(true, objType);
					if (realDmg)
						obj->setDamage((static_cast<TurretTypePtr>(objType))->getDamageLevel());

					objList[curCollidableHandle] = obj;
					obj->setHandle(curCollidableHandle++);
					obj->setExists(true);
					obj->setParentId(objData->parentId);
					obj->setTeamId(objData->teamId,true);
				}
				else
				{
					Fatal(curTurretIndex," No More Turrets ");
				}
				break;

			case GATE:
				obj = getGate(curGateIndex++);
				if (obj)
				{
					(static_cast<GatePtr>(obj))->init(true, objType);
					if (realDmg)
						obj->setDamage((static_cast<GateTypePtr>(objType))->getDamageLvl());

					objList[curCollidableHandle] = obj;
					obj->setHandle(curCollidableHandle++);
					obj->setExists(true);
					obj->setParentId(objData->parentId);
					obj->setTeamId(objData->teamId,true);
				}
				else
				{
					Fatal(curGateIndex," No More Gates ");
				}
				break;

			case BUILDING:
			case TREEBUILDING:
				obj = getBuilding(curBuildingIndex++);
				if (obj)
				{
					(static_cast<BuildingPtr>(obj))->init(true, objType);
					if (realDmg)
						obj->setDamage((static_cast<BuildingTypePtr>(objType))->getDamageLevel());
					(static_cast<BuildingPtr>(obj))->baseTileId = (objData->damage >> 4);
					if (obj->isSpecialBuilding())
					{
						objList[curCollidableHandle] = obj;
						obj->setHandle(curCollidableHandle++);
					}
					else
					{
						objList[curNonCollidableHandle] = obj;
						obj->setHandle(curNonCollidableHandle++);
					}
					
					obj->setExists(true);
					obj->setParentId(objData->parentId);
					obj->setTeamId(objData->teamId,true);
				}
				else
				{
					Fatal(curBuildingIndex," No More Buildings ");
				}
				break;
			case BRIDGE:
				break;
			default:
				Fatal(objType->getObjectClass(), " GameObjectManager.countTerrainObjects: bad object type ");
		}

		if (obj) {
			obj->setTerrainPosition(objData->vector, numbers);
			int cellRow, cellCol;
			obj->getCellPosition(cellRow, cellCol);
			setPartId(obj, cellRow, cellCol);

			//Just keep the designers from hurting themselves
			if (objType->getObjectClass() != TURRET)
				obj->setRotation(objData->rotation);

			if (obj->isTerrainObject()) {
				glm::vec3 pos = objData->vector;
				(static_cast<TerrainObjectPtr>(obj))->calcCellFootprint(pos);
				if (obj->getObjectClass() == BUILDING) {
					if (objType->getSubType() == BUILDING_SUBTYPE_WALL) {
						for (long i = 0; i < tempNumSpecialAreas; i++)
							if (tempSpecialAreaFootPrints[i].cellPositionRow == cellRow)
								if (tempSpecialAreaFootPrints[i].cellPositionCol == cellCol) {
									(static_cast<BuildingPtr>(obj))->calcSubAreas(tempSpecialAreaFootPrints[i].numCells, tempSpecialAreaFootPrints[i].cells);
									break;
								}
						(static_cast<BuildingPtr>(obj))->closeSubAreas();
						}
					else if (objType->getSubType() == BUILDING_SUBTYPE_LANDBRIDGE) {
						for (long i = 0; i < tempNumSpecialAreas; i++)
							if (tempSpecialAreaFootPrints[i].cellPositionRow == cellRow)
								if (tempSpecialAreaFootPrints[i].cellPositionCol == cellCol) {
									(static_cast<BuildingPtr>(obj))->calcSubAreas(tempSpecialAreaFootPrints[i].numCells, tempSpecialAreaFootPrints[i].cells);
									break;
								}
						(static_cast<BuildingPtr>(obj))->openSubAreas();
					}
					}
				else if (obj->getObjectClass() == GATE) {
					for (long i = 0; i < tempNumSpecialAreas; i++)
						if (tempSpecialAreaFootPrints[i].cellPositionRow == cellRow)
							if (tempSpecialAreaFootPrints[i].cellPositionCol == cellCol) {
								(static_cast<BuildingPtr>(obj))->calcSubAreas(tempSpecialAreaFootPrints[i].numCells, tempSpecialAreaFootPrints[i].cells);
								break;
							}
					(static_cast<GatePtr>(obj))->openSubAreas();
					// NULL guards — GlobalMoveMap[1] NULL on bail'd mod content.
					for (int i = 0; i < (static_cast<GatePtr>(obj))->numSubAreas0; i++) {
						if (GlobalMoveMap[0]) GlobalMoveMap[0]->setAreaOwnerWID((static_cast<GatePtr>(obj))->subAreas0[i], (static_cast<GatePtr>(obj))->getWatchID());
						if (GlobalMoveMap[1]) GlobalMoveMap[1]->setAreaOwnerWID((static_cast<GatePtr>(obj))->subAreas1[i], (static_cast<GatePtr>(obj))->getWatchID());
						if ((static_cast<GatePtr>(obj))->status == OBJECT_STATUS_DESTROYED) {
							if (GlobalMoveMap[0]) GlobalMoveMap[0]->setAreaTeamID((static_cast<GatePtr>(obj))->subAreas0[i], -1);
							if (GlobalMoveMap[1]) GlobalMoveMap[1]->setAreaTeamID((static_cast<GatePtr>(obj))->subAreas1[i], -1);
							}
						else {
							if (GlobalMoveMap[0]) GlobalMoveMap[0]->setAreaTeamID((static_cast<GatePtr>(obj))->subAreas0[i], (static_cast<GatePtr>(obj))->teamId);
							if (GlobalMoveMap[1]) GlobalMoveMap[1]->setAreaTeamID((static_cast<GatePtr>(obj))->subAreas1[i], -1);
						}
					}
					(static_cast<GatePtr>(obj))->setTeamId(obj->getTeamId(), true);
					short* curCoord = (static_cast<GatePtr>(obj))->cellsCovered;
					for (int i = 0; i < (static_cast<GatePtr>(obj))->numCellsCovered; i++) {
						long r = *curCoord++;
						long c = *curCoord++;
						GameMap->setGate(r, c, true);
					}
				}
			}
		}
	}
}

//---------------------------------------------------------------------------

void GameObjectManager::releasePoolArrays (void)
{
	std::vector<GameObjectPtr>().swap(objListStore);
	std::vector<GameObjectPtr>().swap(watchListStore);
	std::vector<uint16_t>().swap(watchGenerationStore);
	std::vector<GameObjectPtr>().swap(collidableStore);
	std::vector<BattleMechPtr>().swap(mechStore);
	std::vector<GroundVehiclePtr>().swap(vehicleStore);
	std::vector<ElementalPtr>().swap(elementalStore);
	std::vector<TerrainObjectPtr>().swap(terrainObjectStore);
	std::vector<BuildingPtr>().swap(buildingStore);
	std::vector<TurretPtr>().swap(turretStore);
	std::vector<GatePtr>().swap(gateStore);
	std::vector<WeaponBoltPtr>().swap(weaponStore);
	std::vector<CarnagePtr>().swap(carnageStore);
	std::vector<ArtilleryPtr>().swap(artilleryStore);
	objList = watchList = collidableList = nullptr;
	watchGeneration = nullptr;
	mechs = nullptr;
	vehicles = nullptr;
	elementals = nullptr;
	terrainObjects = nullptr;
	buildings = nullptr;
	turrets = nullptr;
	gates = nullptr;
	weapons = nullptr;
	carnage = nullptr;
	artillery = nullptr;
	numCollidables = 0;
}

//---------------------------------------------------------------------------

void GameObjectManager::destroy (void) 
{
	//--------------------------------------------------------------
	// Free 'em all up!!
	long i=0;
	if (mechs && maxMechs > 0) 
	{
		for (i = 0; i < maxMechs; i++) 
		{
			delete mechs[i];
			mechs[i] = nullptr;
		}
	}
	mechs = nullptr;

	//--------------------------------------------------------------
	if (vehicles && maxVehicles > 0) 
	{
		for (i = 0; i < maxVehicles; i++) 
		{
			delete vehicles[i];
			vehicles[i] = nullptr;
		}
	}
	vehicles = nullptr;

	//--------------------------------------------------------------
	if (terrainObjects && numTerrainObjects > 0) 
	{
		for (i = 0; i < numTerrainObjects; i++) 
		{
			delete terrainObjects[i];
			terrainObjects[i] = nullptr;
		}
	}
	terrainObjects = nullptr;

	//--------------------------------------------------------------
	if (buildings && numBuildings > 0) 
	{
		for (i = 0; i < numBuildings; i++) 
		{
			delete buildings[i];
			buildings[i] = nullptr;
		}
	}
	buildings = nullptr;

	//--------------------------------------------------------------
	if (turrets && numTurrets > 0)
	{
		for (i = 0; i < numTurrets; i++) 
		{
			delete turrets[i];
			turrets[i] = nullptr;
		}
	}
	turrets = nullptr;

	//--------------------------------------------------------------
	if (gates && numGates > 0)
	{
		for (i = 0; i < numGates; i++) 
		{
			delete gates[i];
			gates[i] = nullptr;
		}
	}
	gates = nullptr;

	//--------------------------------------------------------------
	if (weapons && numWeapons > 0) 
	{
		for (i = 0; i < numWeapons; i++) 
		{
			delete weapons[i];
			weapons[i] = nullptr;
		}
	}
	weapons = nullptr;

	//--------------------------------------------------------------
	if (carnage && numCarnage > 0) 
	{
		for (i = 0; i < numCarnage; i++) 
		{
			delete carnage[i];
			carnage[i] = nullptr;
		}
	}
	carnage = nullptr;

	//--------------------------------------------------------------
	if (lights && numLights > 0) 
	{
		for (i = 0; i < numLights; i++) 
		{
			delete lights[i];
			lights[i] = nullptr;
		}
	}
	lights = nullptr;

	//--------------------------------------------------------------
	if (artillery && numArtillery > 0) 
	{
		for (i = 0; i < numArtillery; i++) 
		{
			delete artillery[i];
			artillery[i] = nullptr;
		}
	}
	artillery = nullptr;

	// Every actor is gone: only now do the lookup arrays go (objList/watchList stay readable through all the loops above).
	releasePoolArrays();

	//--------------------------------------------------------------
	ownedTypeManager.reset();
	objTypeManager = nullptr;

	delete collisionSystem;
	collisionSystem = nullptr;

	systemHeap->Free(moverLineOfSightTable);
	moverLineOfSightTable = nullptr;
}

//---------------------------------------------------------------------------

void GameObjectManager::render (bool terrain, bool movers, bool other) {

	//-----------------------------------------------------
	//Set render states as few times as possible.

	if (drawOldWay)
	{
		//--------------------------------
		//Set States for Software Renderer
		if (Environment.Renderer == 3)
		{
			gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
	
			gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
			gos_SetRenderState( gos_State_MonoEnable, 1);
			gos_SetRenderState( gos_State_Perspective, 0);
			gos_SetRenderState( gos_State_Clipping, 1);
			gos_SetRenderState( gos_State_Specular, 0);
			gos_SetRenderState( gos_State_Dither, 0);
			gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
			gos_SetRenderState( gos_State_Filter, gos_FilterNone);
			gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
			gos_SetRenderState( gos_State_ZCompare, 1);
			gos_SetRenderState(	gos_State_ZWrite, 1);
		}
		//--------------------------------
		//Set States for Hardware Renderer	
		else
		{
			gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
			gos_SetRenderState( gos_State_ShadeMode, gos_ShadeGouraud);
			gos_SetRenderState( gos_State_MonoEnable, 0);
			gos_SetRenderState( gos_State_Perspective, 1);
			gos_SetRenderState( gos_State_Clipping, 1);
			gos_SetRenderState( gos_State_Specular, 1);
			gos_SetRenderState( gos_State_Dither, 1);
			gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
			gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
			gos_SetRenderState( gos_State_ZCompare, 1);
			gos_SetRenderState(	gos_State_ZWrite, 1);
		}
	}

	if (terrain && renderObjects)
	{
		for (long terrainBlock = 0; terrainBlock < Terrain::numObjBlocks; terrainBlock++)
		{
			// Cull-cascade consumer (object render). Post-8c source of truth for
			// objBlockInfo[].active / objVertexActive[] is the cull-merged Step 6
			// slim reduction loop in mclib/terrain.cpp (8c-part-1 merges the
			// per-vertex cull writes into that loop) — NOT the deleted VPL body,
			// NOT a "Step 5 / 5B slim pass" (that producer never existed; v3.2
			// deleted it). See VPL-retirement plan v3.5 note (CRIT-0).
			if (Terrain::objBlockInfo[terrainBlock].active)
			{
				long numObjs = Terrain::objBlockInfo[terrainBlock].numObjects;
				long objIndex = Terrain::objBlockInfo[terrainBlock].firstHandle;
				for (long terrainObj = 0; terrainObj < numObjs; terrainObj++, objIndex++)
				{
					if (objList[objIndex] &&
						objList[objIndex]->getExists() &&
						Terrain::objVertexActive[objList[objIndex]->getVertexNum()])
					{
						objList[objIndex]->render();
						if (MaxObjectsDrawn)
						{
							//-----------------------------------------
							// No more element groups, so stop drawing.
							return;
						}
					}
				}
			}
		}
	}

	if (movers) {
		if (mechs && (numMechs < maxMechs)) 
		{
			for (long i = 0; i < numMechs; i++)
			{
				if (mechs[i] && mechs[i]->getExists())
					mechs[i]->render();
			}
		}

		if (vehicles) {
			for (long i = 0; i < maxVehicles; i++)
				if (vehicles[i] && vehicles[i]->getExists())
					vehicles[i]->render();
		}

#ifdef USE_ELEMENTALS
		if (elementals) {
			for (long i = 0; i < numElementals; i++)
				if (elementals[i] && elementals[i]->getExists())
					elementals[i]->render();
		}
#endif
	}

	if (other) {
		//----------------------------------------
		// All other objects should be rendered...
		// VK-FIELD-VFX-1: the "other" objects are the scene VFX — weapon bolts
		// (WeaponBolt::render -> gos_DrawTriangles), carnage/explosions, muzzle
		// lights, artillery. In the field VK env (MC2_VK_SHARED_GBUFFER, the
		// launcher's tracy-vk-compare "on" env) the DEFAULT weapon-fire path is
		// the legacy MLR/gos-immediate draw (MC2_GPU_PARTICLES is user-reverted
		// OFF), which draws into the DISCARDED GL backbuffer under the VK-only
		// present -> weapon fire VANISHES. These draws are already SCREEN-SPACE
		// (projectForScreenXY), so route them through gos_State_IsHUD to buffer
		// them into the HUD tail the VK present composites (same mechanism the
		// selection brackets use). scaleExempt so the bottom-band HUD shrink
		// doesn't move the effects. Screen-space => no depth-occlusion (VFX draw
		// on top), an accepted look-only tradeoff for the untextured-tail slice;
		// the depth-correct VK scene-VFX pass is the deferred remainder. Umbrella
		// OFF -> IsHUD untouched -> byte-identical GL. NOTE: the mechs/vehicles
		// (VK-owned islands) are in the SEPARATE `if (movers)` block above and are
		// NOT routed here.
		const bool prevVfxExempt = gos_GetHudScaleExempt();
		// VK-FIELD-VFX-2 trace (MC2_VK_VFX_TRACE): confirm the route + live effect counts.
		static const bool s_vfxRouteTrace = mc2vkgate::vfxTraceOn();
		if (s_vfxRouteTrace) {
			long liveW = 0;
			if (weapons) for (long i = 0; i < numWeapons; i++) if (weapons[i] && weapons[i]->getExists()) liveW++;
			if (liveW > 0) {
				printf("[VK_VFX_TRACE] objmgr other-block route=%d IsHUD-wrap=%d numWeapons=%ld liveWeapons=%ld numCarnage=%ld numArtillery=%ld\n",
					1, 1, numWeapons, liveW, numCarnage, numArtillery);
				fflush(stdout);
			}
		}
		// VK-FIELD-VFX-2: route ONLY the weapon bolts through the IsHUD tail. The VK HUD
		// tail composites via an ALL-OR-NOTHING gate (nativeHudCanSkipGl in
		// gosRenderer::flushHUDBatch): if ANY queued HUD draw is non-representable the WHOLE
		// native HUD tail is bypassed and the HUD renders to the discarded GL backbuffer ->
		// brackets/bars/beams ALL vanish. carnage/lights/artillery are gosFX effects that emit
		// TEXTURED (effect-sprite) draws with no HUD texture mirror -> non-representable -> they
		// would poison the whole tail whenever any effect is live. So keep them OUT of the IsHUD
		// scope (they stay on the normal GL path, invisible under VK-only present, same as before
		// -- the deferred textured-effect remainder), and route only the representable weapon
		// bolts (WeaponBolt::render: untextured laser tris + beam tris + beam-axis lines).
		// VK-FIELD-VFX-3: route ALL scene VFX (weapon bolts + carnage/explosions +
		// muzzle lights + artillery/missile) through the IsHUD tail. This is now SAFE:
		// the all-or-nothing HUD veto (nativeHudCanSkipGl) was replaced by per-draw
		// self-decline (gameos_graphics.cpp flushHUDBatch, same MC2_VK_VFX gate), so a
		// non-representable textured effect sprite drops individually in the tail
		// (recordPendingHudColorProof `continue`) instead of poisoning the whole HUD.
		// The dominant combat classes emit textured effect sprites whose TGA-loaded
		// textures carry an rgba mirror -> the tail mirrors + rasterizes them (alpha
		// blend). scaleExempt so the bottom-band HUD shrink doesn't move the effects.
		gos_SetHudScaleExempt(true);
		gos_SetRenderState(gos_State_IsHUD, 1);
		if (weapons) {
			for (long i = 0; i < numWeapons; i++) {
				if (weapons[i] && weapons[i]->getExists())
					weapons[i]->render();
			}
		}

		if (carnage) {
			for (long i = 0; i < numCarnage; i++) {
				if (carnage[i] && carnage[i]->getExists())
				{
					carnage[i]->render();
				}
			}
		}

		if (lights) {
			for (long i = 0; i < numLights; i++) {
				if (lights[i] && lights[i]->getExists())
					lights[i]->render();
			}
		}

		if (artillery) {
			for (long i = 0; i < numArtillery; i++) {
				if (artillery[i] && artillery[i]->getExists())
					artillery[i]->render();
			}
		}
		gos_SetRenderState(gos_State_IsHUD, 0);
		gos_SetHudScaleExempt(prevVfxExempt);
	}
	gos_SetRenderState( gos_State_Fog, 0);
}

//---------------------------------------------------------------------------

void GameObjectManager::renderShadows (bool terrain, bool movers, bool other) {

	//-----------------------------------------------------
	//Set render states as few times as possible.

	//--------------------------------
	//Set States for Software Renderer
	if (Environment.Renderer == 3)
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_OneZero);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeFlat);
		gos_SetRenderState( gos_State_MonoEnable, 1);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 0);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 0);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendDecal);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
		gos_SetRenderState( gos_State_ZCompare, 0);
		gos_SetRenderState(	gos_State_ZWrite, 0);
	}
	//--------------------------------
	//Set States for Hardware Renderer	
	else
	{
		gos_SetRenderState( gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
		gos_SetRenderState( gos_State_ShadeMode, gos_ShadeFlat);
		gos_SetRenderState( gos_State_MonoEnable, 1);
		gos_SetRenderState( gos_State_Perspective, 0);
		gos_SetRenderState( gos_State_Clipping, 1);
		gos_SetRenderState( gos_State_AlphaTest, 1);
		gos_SetRenderState( gos_State_Specular, 0);
		gos_SetRenderState( gos_State_Dither, 1);
		gos_SetRenderState( gos_State_TextureMapBlend, gos_BlendModulate);
		gos_SetRenderState( gos_State_Filter, gos_FilterNone);
		gos_SetRenderState( gos_State_TextureAddress, gos_TextureWrap );
		gos_SetRenderState( gos_State_ZCompare, 2);
		gos_SetRenderState(	gos_State_ZWrite, 1);
	}

	if (terrain && renderObjects)
	{
		for (long terrainBlock = 0; terrainBlock < Terrain::numObjBlocks; terrainBlock++)
		{
			// Cull-cascade consumer (shadow render). Post-8c source of truth for
			// objBlockInfo[].active / objVertexActive[] is the cull-merged Step 6
			// slim reduction loop in mclib/terrain.cpp (8c-part-1 merges the
			// per-vertex cull writes into that loop) — NOT the deleted VPL body,
			// NOT a "Step 5 / 5B slim pass" (that producer never existed; v3.2
			// deleted it). See VPL-retirement plan v3.5 note (CRIT-0).
			if (Terrain::objBlockInfo[terrainBlock].active)
			{
				long numObjs = Terrain::objBlockInfo[terrainBlock].numObjects;
				long objIndex = Terrain::objBlockInfo[terrainBlock].firstHandle;
				for (long terrainObj = 0; terrainObj < numObjs; terrainObj++, objIndex++)
				{
					if (objList[objIndex] &&
						objList[objIndex]->getExists() &&
						Terrain::objVertexActive[objList[objIndex]->getVertexNum()])
					{
						objList[objIndex]->renderShadows();
						if (MaxObjectsDrawn) {
							//-----------------------------------------
							// No more element groups, so stop drawing.
							return;
						}
					}
				}
			}
		}
	}

	if (movers) {
		if (mechs) {
			for (long i = 0; i < numMechs; i++)
				if (mechs[i] && mechs[i]->getExists())
					mechs[i]->renderShadows();
		}

		if (vehicles) {
			for (long i = 0; i < numVehicles; i++)
				if (vehicles[i] && vehicles[i]->getExists())
					vehicles[i]->renderShadows();
		}
#ifdef USE_ELEMENTALS
		if (elementals) {
			for (long i = 0; i < numElementals; i++)
				if (elementals[i] && elementals[i]->getExists())
					elementals[i]->renderShadows();
		}
#endif
	}

	if (other) {
		if (carnage) {
			for (long i = 0; i < numCarnage; i++) {
				if (carnage[i] && carnage[i]->getExists())
					carnage[i]->renderShadows();
			}
		}
	}

	gos_SetRenderState( gos_State_Fog, 0);
}
