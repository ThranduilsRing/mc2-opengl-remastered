//***************************************************************************
//
//	objmgr.h - This file contains the GameObject Manager class
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef OBJMGR_H
#define OBJMGR_H

//---------------------------------------------------------------------------

#ifndef DOBJMGR_H
#include"dobjmgr.h"
#endif
#include <optional>
#include <string_view>
#include "world/entity_registry.h"
#include "world/spatial_grid.h"
#include "objmgr_watch_policy.h"  // OBJMGR-WATCH-POLICY-EXTRACT-1: shared watch bounds/index policy
// Forward declaration for prewarmStaticPropLightBakes parameter.
class Camera;

#ifndef GAMEOBJ_H
#include"gameobj.h"
#endif

#ifndef MOVER_H
#include"mover.h"
#endif

#ifndef DMECH_H
#include"dmech.h"
#endif

#ifndef DGVEHICL_H
#include"dgvehicl.h"
#endif

#ifndef DELEMNTL_H
#include"delemntl.h"
#endif

#ifndef DTERROBJ_H
#include"dterrobj.h"
#endif

#ifndef DBLDNG_H
#include"dbldng.h"
#endif

#ifndef DWEAPONBOLT_H
#include"dweaponbolt.h"
#endif

#ifndef DCARNAGE_H
#include"dcarnage.h"
#endif

#ifndef DARTLRY_H
#include"dartlry.h"
#endif

#ifndef DOBJBLCK_H
#include"dobjblck.h"
#endif

#ifndef DTURRET_H
#include"dturret.h"
#endif

#ifndef DGATE_H
#include"dgate.h"
#endif

#ifndef DCOLLSN_H
#include"dcollsn.h"
#endif

class PacketFile;

//---------------------------------------------------------------------------

inline constexpr int MOVERLIST_DELETE = 0;
inline constexpr int MOVERLIST_ADD = 1;
inline constexpr int MOVERLIST_TRADE = 2;

inline constexpr unsigned int NO_RAM_FOR_TERRAIN_OBJECT_FILE = 0xBAAA0014;
inline constexpr unsigned int NO_RAM_FOR_TERRAIN_OBJECT_HEAP = 0xBAAA0015;
inline constexpr unsigned int NO_RAM_FOR_OBJECT_BLOCK_NUM = 0xBAAA0016;
inline constexpr unsigned int NO_RAM_FOR_OBJECT_LISTS = 0xBAAA0017;
inline constexpr unsigned int NO_RAM_FOR_OBJECT_DATA_BLOCK = 0xBAAA0018;
inline constexpr unsigned int NO_RAM_FOR_OBJECT_BLOCK_USER = 0xBAAA0019;
inline constexpr unsigned int NO_RAM_FOR_LAST_BLOCK = 0xBAAA001A;
inline constexpr unsigned int NO_RAM_FOR_CENTER_BLOCK = 0xBAAA001B;
inline constexpr unsigned int OBJECTBLOCK_OUTOFRANGE = 0xBAAA001C;
inline constexpr unsigned int NO_AVAILABLE_OBJQUEUE = 0xBAAA001D;
inline constexpr unsigned int COULDNT_MAKE_TERRAIN_OBJECT = 0xBAAA001E;
inline constexpr unsigned int OBJECTBLOCK_NULL = 0xBAAA001F;
inline constexpr unsigned int BLOCK_NOT_CACHED = 0xBAAA0020;
inline constexpr unsigned int COULDNT_CREATE_OBJECT = 0xBAAA0021;
inline constexpr unsigned int OBJECT_NOT_FOUND = 0xBAAA0022;

#pragma pack(1)

struct ObjData {
	short				objTypeNum;		//Type number of object
	unsigned short		vertexNumber;	//Vertex Number in Block.
	unsigned short		blockNumber;	//Which terrain Block.
	unsigned char		damage;			//Damage
	int				    teamId;
	unsigned int		parentId;		//hOW AM i LINKED.
};

struct MiscObjectData {
	int				blockNumber;	//Terrain Block I occupy
	int				vertexNumber;	//Terrain Vertex I occupy
	int				objectTypeNum;	//ObjectTypeNumber for this overlay tile
	int				damaged;		//Is this overlay tile damaged or not
};

struct ObjDataLoader {
		int objTypeNum;
		glm::vec3 vector;

		float rotation;		
		int damage;
		
		int teamId;
		int parentId;

		// convert to block and vertex
		int tileCol;
		int tileRow;
		
		int blockNumber;
		int vertexNumber;
};

#pragma pack()

using ObjDataPtr = ObjData *;

//---------------------------------------------------------------------------

inline constexpr int MAX_REINFORCEMENTS_PER_TEAM = 16;
inline constexpr int MAX_GATE_CONTROLS = 50;
inline constexpr int MAX_TURRET_CONTROLS = 50;
inline constexpr int MAX_POWER_GENERATORS = 50;
inline constexpr int MAX_SPECIAL_BUILDINGS = 200;

#pragma pack(1)

struct _TeamObjectInfo {
	short		numMechs;
	short		numVehicles;
	short		numElementals;
	short		mechHandleIndex;
	short		vehicleHandleIndex;
	short		elementalHandleIndex;
}; using TeamObjectInfo = _TeamObjectInfo;

#pragma pack()

inline constexpr int MAX_REMOVED = 20;
inline constexpr int MAX_WATCHES = 4000;

inline constexpr int MAX_CAPTURES_PER_TEAM = 30;

struct _RemovedMoverRec {
	unsigned int	turn;
	int			partID;
}; using RemovedMoverRec = _RemovedMoverRec;

struct _ObjectManagerData
{
	int					maxObjects;
	int					numElementals;
	int					numTerrainObjects;
	int					numBuildings;
	int					numTurrets;
	int					numWeapons;
	int					numCarnage;
	int					numLights;
	int					numArtillery;
	int					numGates;
	int					maxMechs;
	int					maxVehicles;
	int					numMechs;
	int					numVehicles;
	int					nextWatchId;
}; using ObjectManagerData = _ObjectManagerData;

class GameObjectManager {

	public:

		// B7o.7 (G7): objTypeManager is a VIEW (the unit tests point it at objects they own); ownedTypeManager is what init() fills and
		// destroy() releases. Both are written together in objmgr.cpp only.
		ObjectTypeManagerPtr	objTypeManager;
		std::unique_ptr<ObjectTypeManager>	ownedTypeManager;

		long					numMechs;
		long					numVehicles;
		long					numElementals;
		long					numTerrainObjects;
		long					numBuildings;
		long					numTurrets;
		long					numWeapons;
		long					numCarnage;
		long					numLights;
		long					numArtillery;
		long					numGates;
		long					maxMechs;
		long					maxVehicles;
		long					maxMovers;

		BattleMechPtr*			mechs;
		GroundVehiclePtr*		vehicles;
		ElementalPtr*			elementals;
		TerrainObjectPtr*		terrainObjects;
		BuildingPtr*			buildings;
		WeaponBoltPtr*			weapons;
		LightPtr*				lights;
		CarnagePtr*				carnage;
		ArtilleryPtr*			artillery;
		TurretPtr*				turrets;
		GatePtr*				gates;
		
		BuildingPtr				gateControls[MAX_GATE_CONTROLS];
		BuildingPtr				turretControls[MAX_TURRET_CONTROLS];
		BuildingPtr				powerGenerators[MAX_POWER_GENERATORS];
		BuildingPtr				specialBuildings[MAX_SPECIAL_BUILDINGS];		//These must be updated EVERY FRAME.  Perimeter alarms, etc.
		long					numGateControls;
		long					numTurretControls;
		long					numPowerGenerators;
		long					numSpecialBuildings;

		char*					moverLineOfSightTable;
		bool					useMoverLineOfSightTable;

		GameObjectPtr*			objList;
		GameObjectPtr*			collidableList;
		MoverPtr				moverList[MAX_MOVERS];
		MoverPtr				goodMoverList[MAX_MOVERS];
		MoverPtr				badMoverList[MAX_MOVERS];
		long					numCollidables;
		long					numGoodMovers;
		long					numBadMovers;
		long					numMovers;
		long					nextReinforcementPartId;
		long					numRemoved;
		RemovedMoverRec			moversRemoved[MAX_REMOVED];
		unsigned long			nextWatchID;
		GameObjectPtr			*watchList;
		// MF3-GENERATIONAL-HANDLE-1: runtime-only generation counters, one per
		// watchList[] slot, heap-allocated in EXACT lockstep with watchList
		// (same size getMaxObjects()+1, same memset-0). Never serialized; reset
		// to base at Load by design. Inert unless MC2_WATCHID_GENERATION is set.
		uint16_t				*watchGeneration;

		// The pointer arrays above are views of the registry's legacy slots and these vector owners.
		// bindPoolArray() allocates the other arrays (setNumObjects, Load, buildCollidableList: zeroed storage, view
		// refreshed); releasePoolArrays() swaps every owner empty and nulls every view, after destroy()'s last actor deletion.
		mc2::world::EntityRegistry<GameObject> entities;
		mc2::world::SpatialGrid moverGrid, buildingGrid;
		bool moverGridDirty = true, buildingGridDirty = true;
		std::vector<GameObjectPtr>		watchListStore;
		std::vector<uint16_t>			watchGenerationStore;
		std::vector<GameObjectPtr>		collidableStore;
		std::vector<BattleMechPtr>		mechStore;
		std::vector<GroundVehiclePtr>	vehicleStore;
		std::vector<ElementalPtr>		elementalStore;
		std::vector<TerrainObjectPtr>	terrainObjectStore;
		std::vector<BuildingPtr>		buildingStore;
		std::vector<TurretPtr>			turretStore;
		std::vector<GatePtr>			gateStore;
		std::vector<WeaponBoltPtr>		weaponStore;
		std::vector<CarnagePtr>			carnageStore;
		std::vector<ArtilleryPtr>		artilleryStore;

		template <class T>
		static T* bindPoolArray (std::vector<T>& owner, long count)
		{
			std::vector<T>(static_cast<size_t>(count)).swap(owner);
			return owner.data();
		}
		void releasePoolArrays (void);

		long					currentWeaponsIndex;			//points to next entry in rotating weapon array.
		long					currentCarnageIndex;			//points to next entry in rotating carnage list		
		long					currentLightIndex;
		long					currentArtilleryIndex;
		
		bool					rebuildCollidableList;

		long					numCaptures[MAX_TEAMS];
		GameObjectWatchID		captureList[MAX_TEAMS][MAX_CAPTURES_PER_TEAM];

		long					totalObjCount;
		ObjDataLoader			*objData;						//Used to keep from loading twice!!
		CollisionSystemPtr		collisionSystem;

	private:

		GameObjectPtr findObjectByMouse (long mouseX,
										 long mouseY,
										 GameObjectPtr* searchList,
										 long listSize,
										 bool skipDisabled = false);

		GameObjectPtr findMoverByMouse (long mouseX,
										long mouseY,
										long teamId,
										bool skipDisabled = false);

		GameObjectPtr findTerrainObjectByMouse (long mouseX,
												long mouseY,
												bool skipDisabled = false);

		void addObject(ObjDataLoader *objData, long& objIndex, long& buildIndex, 
			long &turretIndex, long &gateIndex, long& curCollideHandle, long& curNonCollideHandle);

		void countObject( ObjDataLoader* objType);


	public:


		
		void init (void);
		
		GameObjectManager (void);

		void destroy (void);

		~GameObjectManager (void);
		
		void init (const char* objTypeDataFile, long objTypeCacheSize, long objCacheSize);

		void setNumObjects (long nMechs,
							long nVehicles,
							long nElementals,
							long nTerrainObjects,
							long nBuildings,
							long nTurrets,
							long nWeapons,
							long nCarnage,
							long nLights,
							long nArtillery,
							long nGates);

		long getNumObjects (void) {
			return(numMechs +
				   numVehicles +
				   numElementals +
				   numTerrainObjects +
				   numBuildings +
				   numTurrets +
				   numWeapons +
				   numCarnage +
				   numLights +
				   numArtillery + 
				   numGates);
		}

		long getMaxObjects (void) {
			return(maxMechs +
				   maxVehicles +
				   numElementals +
				   numTerrainObjects +
				   numBuildings +
				   numTurrets +
				   numWeapons +
				   numCarnage +
				   numLights +
				   numArtillery + 
				   numGates);
		}

		long load (File* gameFile);

		long save (File* gameFile);

		BattleMechPtr newMech (void);

		GroundVehiclePtr newVehicle (void);

		void freeMover (MoverPtr mover);
		
		void tradeMover (MoverPtr mover, long newTeamID, long newCommanderID);

#ifdef USE_ELEMENTAL
		BattleMechPtr addElemental (void);
#endif

		BattleMechPtr getMech (long mechIndex);

		GroundVehiclePtr getVehicle (long vehicleIndex);

		GroundVehiclePtr getOpenVehicle (void);

		ElementalPtr getElemental (long elementalIndex);

		TerrainObjectPtr getTerrainObject (long terrainObjectIndex);

		BuildingPtr getBuilding (long buildingIndex);

		TurretPtr getTurret (long turretIndex);
		
		GatePtr	getGate (long gateIndex);
		
		WeaponBoltPtr getWeapon (void);				//gets next available weapon.

		CarnagePtr getCarnage (CarnageEnumType carnageType);

		void releaseCarnage (CarnagePtr obj);

		LightPtr getLight (void);

		void releaseLight (LightPtr obj);

		ArtilleryPtr getArtillery (void);

		long getNumArtillery (void) {
			return(numArtillery);
		}

		ArtilleryPtr getArtillery (long artilleryIndex) {
			return(artillery[artilleryIndex]);
		}

		long getNumGateControls (void) {
			return(numGateControls);
		}

		long getNumTurretControls (void) {
			return(numTurretControls);
		}

		BuildingPtr getGateControl (long index) {
			return(gateControls[index]);
		}

		BuildingPtr getTurretControl (long index) {
			return(turretControls[index]);
		}

		void countTerrainObjects (PacketFile* pFile, long firstHandle);

		void loadTerrainObjects ( PacketFile* pFile, volatile float& progress, float progressRange );
		void primeTerrainObjectsForMissionLoad (volatile float& progress, float progressRange);
		// Task 5 (Track B): bulk static-prop registration walk.
		// Called after primeTerrainObjectsForMissionLoad (positions/rotations set)
		// and before finalizeGeometry. Guarded by MC2_STATIC_PROP_MISSION_LOAD_REG.
		void registerStaticPropsForMissionLoad();

		// STATIC-REG-PREWARM-QUEUE-1: off-screen light bake at mission load.
		// Called after finalizeGeometry() + eye init, when world lights are valid.
		// Drains needsFullBakeNextFrame latches for all registered static props
		// using the permanent SSBO path, so off-screen set-dressing is baked before
		// the first frame. Guarded by MC2_STATIC_REG_PREWARM=1 (default off).
		void prewarmStaticPropLightBakes(Camera* cam);

		// The static shadow bake's caster set: every registered terrain object, building, turret and gate with its shadow recipe, rebuilt once
		// the static-prop geometry is final. Reads the objects; changes nothing in the simulation.
		void buildStaticShadowCasterSet();

		long getSpecificObjects (long objClass, long objSubType, GameObjectPtr* objects, long maxObjects);

		GameObjectPtr getObjBlockObject (long blockNumber, long objLocalIndex) {
			return(objList[Terrain::objBlockInfo[blockNumber].firstHandle + objLocalIndex]);
		}

		long getObjBlockNumObjects (long blockNumber) {
			return(Terrain::objBlockInfo[blockNumber].numObjects);
		}

		long getObjBlockNumCollidables (long blockNumber) {
			return(Terrain::objBlockInfo[blockNumber].numCollidableObjects);
		}

		long getObjBlockFirstHandle (long blockNumber) {
			return(Terrain::objBlockInfo[blockNumber].firstHandle);
		}

		long getNumMechs (void) {
			return(numMechs);
		}

		long getNumVehicles (void) {
			return(numVehicles);
		}

		long getNumMovers (void) {
			return(numMovers);
		}

		long getMaxMovers (void) {
			return(maxMovers);
		}

		long getNumGoodMovers (void) {
			return(numGoodMovers);
		}

		long getNumBadMovers (void) {
			return(numBadMovers);
		}

		long getNumTerrainObjects (void) {
			return(numTerrainObjects);
		}

		long getNumBuildings (void) {
			return(numBuildings);
		}

		long getNumTurrets (void) {
			return(numTurrets);
		}

		long getNumGates (void) {
			return(numGates);
		}

		void render (bool terrain, bool movers, bool other);

		void renderShadows (bool terrain, bool movers, bool other);

		void update (bool terrain, bool movers, bool other);

	// GameObjectManager::update phases (objmgr_update.cpp); callers use update().
	private:
		void updateDestroyTestHook();
		void updateReadbackSnapshot();
		void updateActiveSweep();
		void updateFrameJobsPrepass(int& frameJobsPrePassCount);
		void updateTerrainObjects(int& frameJobsPrePassCount);
		void updateMoverWalk(MoverPtr (&removeList)[MAX_MOVERS], long& numRemoved);
		void updateBrainCommit();
		void updateOtherFx();
	public:
		void updateAppearancesOnly( bool terrain, bool mover, bool other);
		// R1 render interpolation: mover poses at the start of the latest tick, and the 0..1 weight between them and the
		// current poses that updateAppearancesOnly draws mechs and vehicles at (1 = the sim pose).
		void snapshotMoverPoses();
        void extractRenderSnapshots();
        void updatePresentationCull();
        std::vector<uint8_t> saveAnimationState();
        bool restoreAnimationState(const std::vector<uint8_t>& bytes, uint32_t sectionVersion = 3);
		void setPresentationAlpha( float alpha );

		GameObjectPtr get (GameObjectHandle handle);
		mc2::world::EntityId entityId(GameObjectHandle handle) const { return entities.id(handle); }
		template<class T> T* getEntity(mc2::world::EntityId id) const { return entities.resolve<T>(id); }
		void invalidateEntity(GameObjectHandle handle, GameObjectPtr object) {
			entities.invalidate(handle, object);
			invalidateSpatialGrid();
		}
		void invalidateSpatialGrid(bool includeBuildings = true) {
			moverGridDirty = true;
			if (includeBuildings) buildingGridDirty = true;
		}
		std::vector<mc2::world::SpatialGrid::Candidate> nearbyMovers(const glm::vec3& position, float radius);
		std::vector<mc2::world::SpatialGrid::Candidate> nearbyBuildings(const glm::vec3& position, float radius);

		GameObjectPtr getByWatchID (unsigned long watchID) {
			// OBJMGR-WATCH-POLICY-EXTRACT-1: shared resolve predicate (behavior-identical).
			return mc2watch::isResolvableWatchId(watchID, nextWatchID) ? watchList[watchID] : nullptr;
		}

		// MF3-GENERATIONAL-HANDLE-1: additive generational resolve. Behaves like
		// getByWatchID, plus (when MC2_WATCHID_GENERATION is set) rejects a handle
		// whose expectedGen no longer matches the slot's current generation
		// (stale -> NULL + rate-limited diag). Gate OFF: generation ignored, so
		// behavior is byte-identical to getByWatchID. Defined in objmgr.cpp.
		// Zero callers this slice (jobs-arc reconcile entry point).
		GameObjectPtr getByWatchIDGenerational (unsigned long watchID, uint16_t expectedGen);

		long buildMoverLists (void);

		bool modifyMoverLists (MoverPtr mover, long action);

		MoverPtr getMover (long index) {
			return(moverList[index]);
		}

		MoverPtr getGoodMover (long index) {
			return(goodMoverList[index]);
		}

		MoverPtr getBadMover (long index) {
			return(badMoverList[index]);
		}

		GameObjectPtr findObject (glm::vec3 position);

		GameObjectPtr findByPartId (long partId);

		GameObjectPtr findByBlockVertex (long blockNum, long vertex);

		GameObjectPtr findByCellPosition (long row, long col);

		GameObjectPtr findByUnitInfo (long commander, long group, long mate);

		GameObjectPtr findObjectByTypeHandle (long typeHandle);

		GameObjectPtr findObjectByMouse (long mouseX, long mouseY);
		
		bool moverInRect(long index, glm::vec3 &dStart, glm::vec3 &dEnd);

		ObjectTypePtr loadObjectType (ObjectTypeNumber typeHandle);

		ObjectTypePtr getObjectType (ObjectTypeNumber typeHandle);

		void removeObjectType (ObjectTypeNumber typeHandle);

		GameObjectHandle getHandle (GameObjectPtr obj);

		long calcPartId (long objectClass, long param1 = 0, long param2 = 0, long param3 = 0);

		void setPartId (GameObjectPtr obj, long param1 = 0, long param2 = 0, long param3 = 0);

		long buildCollidableList (void);

		long initCollisionSystem (FitIniFile* missionFile);

		long getCollidableList (GameObjectPtr*& objList);

		long updateCollisions (void);

		void detectStaticCollision (GameObjectPtr obj1, GameObjectPtr obj2);
		
		void updateCaptureList (void);

		bool isTeamCapturing (TeamPtr team, GameObjectWatchID targetWID);

		CarnagePtr createFire (ObjectTypeNumber fireObjTypeHandle,
							   GameObjectPtr owner,
							   glm::vec3& position,
							   float tonnage);

		CarnagePtr createExplosion (long effectId,
									GameObjectPtr owner,
									glm::vec3& position,
									float dmg = 0.0,
									float radius = 0.0);

		LightPtr createLight (ObjectTypeNumber lightObjTypeHandle);

		WeaponBoltPtr createWeaponBolt (long effectId);

		ArtilleryPtr createArtillery (long artilleryType, glm::vec3& position);

		RemovedMoverRec* getRemovedMovers (long& numRemovedMovers) {
			numRemovedMovers = numRemoved;
			return(moversRemoved);
		}

		bool isRemoved (MoverPtr mover);

		void setWatchID (GameObjectPtr obj);

		void CopyTo (ObjectManagerData *data);
		void CopyFrom (ObjectManagerData *data);

		long Save (PacketFilePtr file, long packetNum);
		long Load (PacketFilePtr file, long packetNum);

		// Structural check of the object packets of a save; nothing when sound, else a reason tag. Run before the
		// running mission is torn down so a bad save is refused instead of crashing the loader.
		static std::optional<std::string_view> checkSavedObjects (PacketFilePtr file, long* headerPacket = nullptr);
		ObjectTypePtr savedObjectType (long typeNum, long slot);
};

extern GameObjectManagerPtr ObjectManager;

//***************************************************************************

#endif
