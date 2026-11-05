//***************************************************************************
//
//	mover.h - This file contains the Mover Class header
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MOVER_H
#define MOVER_H
#include "dradio.h"
#include "unit_anim_state.h"

#ifndef MCLIB_H
#include"mclib.h"
#endif

#ifndef CONTACT_H
#include"contact.h"
#endif

#ifndef DMOVER_H
#include"dmover.h"
#endif

#ifndef GAMEOBJ_H
#include"gameobj.h"
#endif

#ifndef OBJTYPE_H
#include"objtype.h"
#endif

#ifndef UNIT_ACTION_H
#include"unit_action.h"          // UNIT-ACTION-QUERY-RESULT-1: ActionAvailability
#endif

#ifndef ACTION_EXECUTOR_REGISTRY_H
#include"action_executor_registry.h"  // ACTION-EXECUTOR-REGISTRY-1: LocomotionModel + registry
#endif

#ifndef DAMAGE_SCHEMA_H
#include"damage_schema.h"             // DAMAGE-SCHEMA-FACET-1: DamageSchemaId
#endif

#ifndef HARDPOINT_H
#include"hardpoint.h"                 // LOADOUT-HARDPOINT-JOIN-1: HardpointProfile
#endif

#ifndef MOVE_H
#include"move.h"
#endif

#ifndef DOBJNUM_H
#include"dobjnum.h"
#endif

#ifndef CMPONENT_H
#include"cmponent.h"
#endif

#include "warrior_types.h"

#ifndef DGROUP_H
#include"dgroup.h"
#endif

#ifndef TRIGGER_H
#include"trigger.h"
#endif

#ifndef DTEAM_H
#include"dteam.h"
#endif

#ifndef DCOMNDR_H
#include"comndr.h"
#endif

extern float metersPerWorldUnit;

//***************************************************************************

#define	DEBUG_CHUNKS

inline constexpr int YIELD_WAITING = 0;
inline constexpr int YIELDED = 1;
inline constexpr int COLLIDER_YIELDED = 2;
inline constexpr int COLLIDER_BEHIND = 3;
inline constexpr int NO_ACTION = 4;
inline constexpr int ON_THE_MOVE = 5;

inline constexpr int MAX_LOCK_RANGE = 10;

inline constexpr int MAX_WEAPONFIRE_CHUNKS = 128;
inline constexpr int MAX_CRITICALHIT_CHUNKS = 128;
inline constexpr int MAX_RADIO_CHUNKS = 7;

inline constexpr int CHUNK_SEND = 0;
inline constexpr int CHUNK_RECEIVE = 1;

inline constexpr int NUM_WEAPONFIRE_MODIFIERS = 30;

inline constexpr int MAX_ANTI_MISSILE_SYSTEMS = 16;			// Way more than we should need!

inline constexpr int ARMOR_TYPE_STANDARD = 0;
inline constexpr int ARMOR_TYPE_FERROFIBROUS = 1;

inline constexpr int CHASSIS_TYPE_STANDARD = 0;
inline constexpr int CHASSIS_TYPE_ENDOSTEEL = 1;

inline constexpr int ENGINE_TYPE_COMBUSTION = 0;
inline constexpr int ENGINE_TYPE_FUSION_STANDARD = 1;
inline constexpr int ENGINE_TYPE_FUSION_XL = 2;

inline constexpr int HEATSINK_TYPE_SINGLE = 1;
inline constexpr int HEATSINK_TYPE_DOUBLE = 2;

inline constexpr int IS_DAMAGE_NONE = 0;
inline constexpr int IS_DAMAGE_PARTIAL = 1;
inline constexpr int IS_DAMAGE_DESTROYED = 2;

inline constexpr int UNLIMITED_SHOTS = 9999;

inline constexpr auto NO_APPEARANCE_FOR_MECH = 0xFFFA0001;
inline constexpr auto NO_RAM_FOR_DYNAMICS_TYPE = 0xFFFA0002;
inline constexpr auto INVALID_DYNAMICS_TYPE = 0xFFFA0003;
inline constexpr auto NO_RAM_FOR_CONTROL = 0xFFFA0004;
inline constexpr auto INVALID_CONTROL_TYPE = 0xFFFA0005;
inline constexpr auto NO_RAM_FOR_CONTROL_DATA = 0xFFFA0006;
inline constexpr auto INVALID_CONTROL_DATA_TYPE = 0xFFFA0007;
inline constexpr auto NO_RAM_FOR_DYNAMICS = 0xFFFA0008;
inline constexpr auto NO_APPEARANCE_TYPE_FOR_MECH = 0xFFFA0009;
inline constexpr auto APPEARANCE_NOT_SPRITE_TREE = 0xFFFA000A;
inline constexpr auto NO_RAM_FOR_FOOT_HS = 0xFFFA000B;
inline constexpr auto NO_RAM_FOR_WEAPON_HS = 0xFFFA000C;
inline constexpr auto FOOT_HS_WRONG_SIZE = 0xFFFA000D;
inline constexpr auto BAD_HS_PACKET = 0xFFFA000E;
inline constexpr auto NO_RAM_FOR_XLAT_HS = 0xFFFA000F;

//------------------------------------------------------------------------------
// Enums
enum class WeaponSortType : int32_t {
	WEAPONSORT_ATTACKCHANCE,
	NUM_WEAPONSORT_TYPES
};
using enum WeaponSortType;

enum class SpeedState : int32_t {
	SPEED_STATE_STATIONARY,
	SPEED_STATE_MOVING,
	SPEED_STATE_MOVING_FAST,
	NUM_MOVE_STATES
};
using enum SpeedState;

enum DeathCause
{
	DEBUGGER_DEATH = 0,
	ENGINE_DEATH = 1,
	PILOT_DEATH = 2,
	EJECTION_DEATH = 3,
	UNDETERMINED_DEATH = 4,
	POWER_USED_UP = 5
};

//---------------------------------------------------------------------------

class MoveChunk;
using MoveChunkPtr = MoveChunk*;

class MoveChunk {

	public:

		long				stepPos[4][2];		// 0 = curPos, 1 thru 3 = next steps
		long				stepRelPos[3];		// delta for steps 1 thru 3
		long				numSteps;
		bool				run;
		bool				moving;
		unsigned long		data;
		static long			err;

	public:


		
		void init (void) {
			stepPos[0][0] = 0xFFFFFFFF;
			stepPos[0][1] = 0xFFFFFFFF;
			numSteps = 0;
			run = false;
			moving = false;
			data = 0;
		}

		void destroy (void) {
		}

		MoveChunk (void) {
			init();
		}

		~MoveChunk (void) {
			destroy();
		}

		void build (MoverPtr mover, MovePath* path1, MovePath* path2 = nullptr);

		void build (MoverPtr mover, glm::vec3 jumpGoal);

		void pack (MoverPtr mover);

		void unpack (MoverPtr mover);

		bool equalTo (MoverPtr mover, MoveChunkPtr chunk);
};

//---------------------------------------------------------------------------
inline constexpr int MAX_YAW = 64;

enum DynamicsType {
	DYNAMICS_BASE,
	DYNAMICS_MECH,
	DYNAMICS_GROUNDVEHICLE,
	DYNAMICS_ELEMENTAL
};

union DynamicsLimits {
	struct {
		int32_t	yawRate;			//Degrees per sec
	} elemental;
	struct {
		int32_t	yawRate;			//Degrees per sec
		int32_t	turretYawRate;		//Degrees per sec
		int32_t	pivotRate;			//Degrees per sec
		float	accel;				//Meters per sec per sec
		float	speed;				//Meters per sec
		int32_t	turretYaw;			//Degrees
	} groundVehicle;
	struct {
		int32_t	torsoYawRate;		//Degrees per sec
		int32_t	torsoYaw;			//Degrees
	} mech;
};

class MoverDynamics {

	public:

		DynamicsType			type;
		DynamicsLimits			max;

	public:

		void operator = (MoverDynamics copy) {
			type = copy.type;
			max = copy.max;
			//cur = copy.cur;
		}
		
		virtual void init (void);

		MoverDynamics (void) {
			init();
		}
			
		virtual void destroy (void) {
		}

		~MoverDynamics (void) {
			destroy();
		}

		void init (DynamicsType newType);

		virtual void init (CSVFilePtr dynamicsFile);

		virtual void init (FitIniFilePtr dynamicsFile);
													
		void setType (DynamicsType newType) {
			type = newType;
		}

		DynamicsType getType (void) {
			return(type);
		}

		long brake (void);
};

//---------------------------------------------------------------------------

enum ControlType {
	CONTROL_BASE,
	CONTROL_PLAYER,
	CONTROL_AI,
	CONTROL_NET
};

enum ControlDataType {
	CONTROL_DATA_BASE,
	CONTROL_DATA_MECH,
	CONTROL_DATA_GROUNDVEHICLE,
	CONTROL_DATA_ELEMENTAL
};

union ControlSettings {
	struct {
		float	rotate;
	} elemental;
	struct {
		char	throttle;			
		float	rotate;
		float	rotateTurret;
		char	gestureGoal;
		bool	pivot;
		bool	isWalking;
	} groundVehicle;
	struct {
		char	throttle;			
		float	rotate;				//aka mechYaw
		float	facingRotate;		//Direction mech is FACING, NOT MOVING!!!!!
		float	rotateTorso;
		float	rotateLeftArm;		//aka leftArmYaw
		float	rotateRightArm;		//aka rightArmYaw
		char	gestureGoal;
		bool	blowLeftArm;
		bool	blowRightArm;
		bool	pivot;
	} mech;
};

class MoverControl {

	public:

		ControlType			type;
		ControlDataType		dataType;
		ControlSettings		settings;

	public:

		void operator = (MoverControl copy) {
			type = copy.type;
			dataType = copy.dataType;
			settings = copy.settings;
		}

		virtual void init (void) {
			type = CONTROL_BASE;
			dataType = CONTROL_DATA_BASE;
		}

		MoverControl (void) {
			init();
		}
			
		virtual void destroy (void) {
		}

		~MoverControl (void) {
			destroy();
		}

		void init (ControlType newType, ControlDataType newDataType) {
			setType(newType);
			setDataType(newDataType);
		}

		virtual long init (FitIniFilePtr controlFile);

		void setType (ControlType newType) {
			type = newType;
		}

		ControlType getType (void) {
			return(type);
		}

		void setDataType (ControlDataType newDataType) {
			dataType = newDataType;
			reset();
		}

		ControlDataType getDataType (void) {
			return(dataType);
		}

		void reset (void);

		void brake (void);

		void update (MoverPtr mover);
};

//---------------------------------------------------------------------------

inline constexpr int MAX_RANGE_RATINGS = 51;

using InventoryItemPtr = struct _InventoryItem*;

struct _RangeRating {
	float				accuracy;
	float				adjAccuracy;
}; using RangeRating = _RangeRating;

using RangeRatingPtr = struct _RangeRating*;

struct _InventoryItem {
	//------------------
	// general item info
	unsigned char		masterID;		// master component ID
	unsigned char		health;			// number of points left before destroyed
	bool				disabled;		// TRUE if effectively destroyed

	//----------------
	// weapon specific
	unsigned char		facing;			// weapon facing in torso: 0 = forward, 1 = rear -- NO WEAPONS fire rear.  This is the weapon Node ID now!!!!
	short				startAmount;	// ammo's mission-start level
	short				amount;			// generally for ammo, and weapon's total ammo
	short				ammoIndex;		// used by ammo to reference ammo pools
	float				readyTime;		// next time weapon will be ready
	unsigned char		bodyLocation;	// where is the weapon located
	short				effectiveness;	// weapon max effectiveness

	// LOADOUT-HARDPOINT-JOIN-1 (Phase-2 slice 3): OPTIONAL logical-hardpoint join
	// key. 0 == unset (the stock case: CSV loadouts carry no hardpoint column, so
	// this stays 0 and the legacy positional-CSV mount path is unchanged). When a
	// data source (additive-merge registry / .fit block) declares a HardpointId for
	// this mount, its FNV hash (hardpoint.h::hardpointHash / HardpointId::id) is
	// stored here. Report-only this slice: the validator gathers declared joins and
	// checks weapon-tag/damage-loc/socket coherence; NOTHING reads this to drive the
	// actual visual mount yet (inventory[].facing / getWeaponNode still own that --
	// slice 4 routes the mount through the HardpointId). Plain uint keeps
	// InventoryItem trivially copyable so raw MoverData save/load is unaffected.
	unsigned int		hardpointId;	// 0 == unset (legacy positional-CSV fallback)
}; using InventoryItem = _InventoryItem;

//------------------------------------------------------------------------------------------

inline constexpr int MAX_AMMO_TYPES = 10;

struct _AmmoTally {
	long				masterId;		// ammo type's Master Component Id
	long				curAmount;		// current amount of this ammo type in inventory
	long				maxAmount;		// starting amount of this ammo type in inventory
}; using AmmoTally = _AmmoTally;

using AmmoTallyPtr = AmmoTally*;

//------------------------------------------------------------------------------------------

using CriticalSpacePtr = struct _CriticalSpace*;

struct _CriticalSpace {

	unsigned char		inventoryID;	// indexes into mech's inventory
	bool				hit;			// TRUE, if this space has been hit

	void operator = (struct _CriticalSpace copy) {
		inventoryID = copy.inventoryID;
		hit = copy.hit;
	}

}; using CriticalSpace = _CriticalSpace;

//------------------------------------------------------------------------------------------

inline constexpr int MAX_CRITSPACES_PER_BODYLOCATION = 12;

class BodyLocation {

	public:

		bool				CASE;
		int32_t				totalSpaces;
		CriticalSpace		criticalSpaces[MAX_CRITSPACES_PER_BODYLOCATION];
		float				curInternalStructure;
		unsigned char		hotSpotNumber;
		unsigned char		maxInternalStructure;
		unsigned char		damageState;

	public:

		void operator = (BodyLocation copy) {
			CASE = copy.CASE;
			totalSpaces = copy.totalSpaces;
			for (int i = 0; i < MAX_CRITSPACES_PER_BODYLOCATION; i++)
				criticalSpaces[i] = copy.criticalSpaces[i];
			curInternalStructure = copy.curInternalStructure;
			hotSpotNumber = copy.hotSpotNumber;
			maxInternalStructure = copy.maxInternalStructure;
			damageState = copy.damageState;
		}
};

using BodyLocationPtr = BodyLocation*;

//---------------------------------------------------------------------------

class ArmorLocation {

	public:

		float			curArmor;
		unsigned char	maxArmor;
};

using ArmorLocationPtr = ArmorLocation*;

//---------------------------------------------------------------------------

inline constexpr int STATUSCHUNK_BODYSTATE_BITS = 3;
inline constexpr int STATUSCHUNK_TARGETTYPE_BITS = 3;
inline constexpr int STATUSCHUNK_MOVERINDEX_BITS = 7;
inline constexpr int STATUSCHUNK_TERRAINPARTID_BITS = 20;
inline constexpr int STATUSCHUNK_TERRAINBLOCK_BITS = 8;
inline constexpr int STATUSCHUNK_TERRAINVERTEX_BITS = 9;
inline constexpr int STATUSCHUNK_TERRAINITEM_BITS = 3;
inline constexpr int STATUSCHUNK_TRAIN_BITS = 8;
inline constexpr int STATUSCHUNK_TRAINCAR_BITS = 8;
inline constexpr int STATUSCHUNK_CELLPOS_BITS = 10;
inline constexpr int STATUSCHUNK_EJECTORDER_BITS = 1;
inline constexpr int STATUSCHUNK_JUMPORDER_BITS = 1;

inline constexpr int STATUSCHUNK_BODYSTATE_MASK = 0x00000007;
inline constexpr int STATUSCHUNK_TARGETTYPE_MASK = 0x00000007;
inline constexpr int STATUSCHUNK_MOVERINDEX_MASK = 0x0000007F;
inline constexpr int STATUSCHUNK_TERRAINPARTID_MASK = 0x000FFFFF;
inline constexpr int STATUSCHUNK_TERRAINBLOCK_MASK = 0x000000FF;
inline constexpr int STATUSCHUNK_TERRAINVERTEX_MASK = 0x000001FF;
inline constexpr int STATUSCHUNK_TERRAINITEM_MASK = 0x00000007;
inline constexpr int STATUSCHUNK_TRAIN_MASK = 0x000000FF;
inline constexpr int STATUSCHUNK_TRAINCAR_MASK = 0x000000FF;
inline constexpr int STATUSCHUNK_CELLPOS_MASK = 0x000003FF;
inline constexpr int STATUSCHUNK_EJECTORDER_MASK = 0x00000001;
inline constexpr int STATUSCHUNK_JUMPORDER_MASK = 0x00000001;

enum StatusChunkTarget {
	STATUSCHUNK_TARGET_NONE,
	STATUSCHUNK_TARGET_MOVER,
	STATUSCHUNK_TARGET_TERRAIN,
	STATUSCHUNK_TARGET_SPECIAL,
	STATUSCHUNK_TARGET_LOCATION
};

class StatusChunk;
using StatusChunkPtr = StatusChunk*;

class StatusChunk {

	public:

		unsigned long		bodyState;
		char				targetType;
		long				targetId;
		long				targetBlockOrTrainNumber;
		long				targetVertexOrCarNumber;
		char				targetItemNumber;
		short				targetCellRC[2];
		bool				ejectOrderGiven;
		bool				jumpOrder;

		unsigned long		data;

	public:


		
		void init (void) {
			bodyState = 0;
			targetType = 0;
			targetId = 0;
			targetBlockOrTrainNumber = 0;
			targetVertexOrCarNumber = 0;
			targetItemNumber = 0;
			targetCellRC[0] = -1;
			targetCellRC[1] = -1;
			ejectOrderGiven = false;
			jumpOrder = false;

			data = 0;
		}

		void destroy (void) {
		}

		StatusChunk (void) {
			init ();
		}

		~StatusChunk (void) {
			destroy();
		}

		virtual void build (MoverPtr mover);

		virtual void pack (MoverPtr mover);

		virtual void unpack (MoverPtr mover);

		bool equalTo (StatusChunkPtr chunk);
};


//---------------------------------------------------------------------------

inline constexpr int MAXLEN_MOVER_NAME = 65;
inline constexpr int MAX_MOVER_BODY_LOCATIONS = 8;
inline constexpr int MAX_MOVER_ARMOR_LOCATIONS = 11;
inline constexpr int MAX_MOVER_INVENTORY_ITEMS = 72;
inline constexpr int MAXLEN_NET_PLAYER_NAME = 256;
inline constexpr int MAXLEN_MECH_LONGNAME = 35;

inline constexpr auto MAX_ATTACK_CELLRANGE = 30;
inline constexpr int MAX_ATTACK_INCREMENTS = 32;
inline constexpr auto RANGED_CELLS_DIM = (MAX_ATTACK_CELLRANGE * 2 + 1) * (MAX_ATTACK_CELLRANGE * 2 + 1);

struct _MoverData : public GameObjectData
{
	bool				killed;
	bool				lost;
	glm::vec3		positionNormal;						
	glm::vec3		velocity;							
	char				name[MAXLEN_MOVER_NAME];			
	unsigned char		chassis;							
	bool				startDisabled;
	float				creationTime;

	int32_t				moveType;
	int32_t				moveLevel;
	bool				followRoads;

	int32_t				lastMapCell[2];

	float				damageRateTally;					
	float				damageRateCheckTime;				
	float				pilotCheckDamageTally;				

	BodyLocation		body[MAX_MOVER_BODY_LOCATIONS];		
	char				numBodyLocations;					
	int32_t				fieldedCV;

	int32_t				attackRange;						

	ArmorLocation		armor[MAX_MOVER_ARMOR_LOCATIONS];	
	char				numArmorLocations;
	char				longName[MAXLEN_MECH_LONGNAME];		

	InventoryItem		inventory[MAX_MOVER_INVENTORY_ITEMS];
	unsigned char		numOther;
	unsigned char		numWeapons;
	unsigned char		numAmmos;
	AmmoTally			ammoTypeTotal[MAX_AMMO_TYPES];	
	char				numAmmoTypes;					
	int32_t				pilotHandle;

	unsigned char		cockpit;										
	unsigned char		engine;											
	unsigned char		lifeSupport;									
	unsigned char		sensor;											
	unsigned char		ecm;											
	unsigned char		probe;											
	unsigned char		jumpJets;										
	unsigned char		nullSignature;									
	float				maxWeaponEffectiveness;							
	float				weaponEffectiveness;							

	float				minRange;										
	float				maxRange;										
	float				optimalRange;									
	int32_t				numFunctionalWeapons;							

	char				numAntiMissileSystems;							
	unsigned char		antiMissileSystem[MAX_ANTI_MISSILE_SYSTEMS];	

	float				engineBlowTime;
	float				maxMoveSpeed;
	bool				shutDownThisFrame;
	bool				startUpThisFrame;
	bool				disableThisFrame;

	char				teamId;
	char				groupId;
	int32_t				squadId;
	int32_t				selectionIndex;					
	int32_t				teamRosterIndex;				
	char				commanderId;
	int32_t				unitGroup;						
														
	int32_t				iconPictureIndex;				
	bool				suppressionFire;				

	int32_t				pilotCheckModifier;
	int32_t				prevPilotCheckModifier;
	int32_t				prevPilotCheckDelta;
	float				prevPilotCheckUpdate;
	bool				failedPilotingCheck;			
	float				lastWeaponEffectivenessCalc;	
	float				lastOptimalRangeCalc;			
	GameObjectWatchID	challengerWID;

	char				lastGesture;

	MoverControl		control;						
	MoverDynamics		dynamics;						

	int32_t				numWeaponHitsHandled;
	float				timeLeft;						
	bool				exploding;
	bool				withdrawing;

	float				yieldTimeLeft;					
	glm::vec3		lastValidPosition;				
	char				pivotDirection;					
	float				lastHustleTime;					

	bool				salvageVehicle;

	float				markDistanceMoved;				

	GameObjectWatchID	refitBuddyWID;
	GameObjectWatchID	recoverBuddyWID;

	int32_t				crashAvoidSelf;
	int32_t				crashAvoidPath;
	int32_t				crashBlockSelf;
	int32_t				crashBlockPath;
	float				crashYieldTime;
	int32_t				pathLockLength;
	int32_t				pathLockList[MAX_LOCK_RANGE][2];
	glm::vec3		moveCenter;
	float				moveRadius;

	int32_t				overlayWeightClass;

	float				timeToClearSelection;

	float				timeSinceMoving;

	float 				timeSinceFiredLast;				

	GameObjectWatchID	lastMovingTargetWID;

	bool 				mechSalvage;					

	glm::vec3		teleportPosition;				
	int32_t				debugPage;

	bool				pathLocks;						
	bool				isOnGui;						

	int32_t				conStat;						
	float				fadeTime;						
	BYTE				alphaValue;						
	int32_t				causeOfDeath;

	int32_t				lowestWeaponNodeID;

	DWORD				psRed;
	DWORD				psBlue;
	DWORD				psGreen;

}; using MoverData = _MoverData;


enum MoveType {
	MOVETYPE_GROUND,
	MOVETYPE_AIR,
	NUM_MOVETYPES
};

class LogisticsPilot;

class Mover : public GameObject {
public:
    std::shared_ptr<mc2::sim::UnitAnimState> unitAnimation;
    std::shared_ptr<mc2::sim::SimPose> simulationPose;
    // Published during render extraction, before the next worker tick starts.
    glm::vec3 committedLosPosition{};
    bool committedHasWeaponNode = false;

	//------------
	//Data Members
	
	public:

		glm::vec3		positionNormal;						// normal to terrain at current position
		glm::vec3		velocity;							//How fast am I going?
		char				name[MAXLEN_MOVER_NAME];		// Name of this particular mover
		unsigned char		chassis;							// type of mover's chassis
		bool				startDisabled;
		float				creationTime;

		bool				killed;							// used to record when the kill score has been awarded
		bool				lost;							// used to record when the loss score has been awarded
		int32_t             moveType;
		int32_t             moveLevel;
		bool				followRoads;

		int32_t             lastMapCell[2];
		
		float				damageRateTally;					// damage points taken since last check
		float				damageRateCheckTime;				// time (in game time) of next damage check
		float				pilotCheckDamageTally;				// damage points taken since last pilot check

		BodyLocation		body[MAX_MOVER_BODY_LOCATIONS];		// body parts of this mech
		char				numBodyLocations;					// should be set based upon mover type
		int32_t             fieldedCV;

		int32_t             attackRange;						// attack range

		bool				salvaged;

		// Armor
		ArmorLocation		armor[MAX_MOVER_ARMOR_LOCATIONS];	// armor locations of this mover
		char				numArmorLocations;
		char				longName[MAXLEN_MECH_LONGNAME];		//Used by logistics (and the interface) to get long name.

		// Inventory
		InventoryItem		inventory[MAX_MOVER_INVENTORY_ITEMS];
		unsigned char		numOther;
		unsigned char		numWeapons;
		unsigned char		numAmmos;
		AmmoTally			ammoTypeTotal[MAX_AMMO_TYPES];	// tracks total ammo per ammo type
		char				numAmmoTypes;					// number of different ammo types
		MechWarriorPtr		pilot;
		int32_t             pilotHandle;
		SensorSystemPtr		sensorSystem;
		ContactInfoPtr		contactInfo;

		// Critical Component Indices
		unsigned char		cockpit;										// cockpit inventory index
		unsigned char		engine;											// engine inventory index
		unsigned char		lifeSupport;									// life support inventory index
		unsigned char		sensor;											// sensor inventory index
		unsigned char		ecm;											// ecm inventory index
		unsigned char		probe;											// probe inventory index
		unsigned char		jumpJets;										// jump jets inventory index
		unsigned char		nullSignature;									// null signature inventory index
		float				maxWeaponEffectiveness;							// max total damage possible
		float				weaponEffectiveness;							// basically, total damage possible
		
		float				minRange;										// current min attack range
		float				maxRange;										// current max attack range
		float				optimalRange;									// current optimum attack range
		int32_t             numFunctionalWeapons;							// takes into account damage, etc.

		char				numAntiMissileSystems;							// number of anti-missile systems
		unsigned char		antiMissileSystem[MAX_ANTI_MISSILE_SYSTEMS];	// anti-missile system list

		// Engine
		float				engineBlowTime;
		float				maxMoveSpeed;
		bool				shutDownThisFrame;
		bool				startUpThisFrame;
		bool				disableThisFrame;

		// Team
		//MoverGroupPtr		group;							// what group am I a member of?
		char				teamId;
		UnitRuntimeState	runtime;						// UNIT-PROFILE-SEAM-1
		char				groupId;
		int32_t				squadId;
		int32_t				selectionIndex;					// > 0 when in selected group
		int32_t				teamRosterIndex;				// where am I in my team's roster?
		char				commanderId;
		int32_t				unitGroup;						// the thing the user sets by hitting ctrl and a number
															// this is a field since they can belong to more than one
		int32_t				iconPictureIndex;				// the little picture that shows arms and stuff falling off
		bool				suppressionFire;				// is this guy permanently shooting at ground
		char				prevTeamId;
		char				prevCommanderId;
		

		// Update Info
		int32_t				pilotCheckModifier;
		int32_t				prevPilotCheckModifier;
		int32_t				prevPilotCheckDelta;
		float				prevPilotCheckUpdate;
		bool				failedPilotingCheck;			// Passed or failed this frame...
//		BaseObjectPtr		collisionFreeFrom;
//		float				collisionFreeTime;
		float				lastWeaponEffectivenessCalc;	// time of last calc
		float				lastOptimalRangeCalc;			// time of last calc
		GameObjectWatchID	challengerWID;

		char				lastGesture;

//		AppearancePtr		appearance;						// pointer to the Actor which is the appearance.
		MoverControl		control;						// control settings for this mover
		MoverDynamics		dynamics;						// dynamics settings for this mover
		
		// Network
		//DWORD				netPlayerId;
		char				netPlayerName[MAXLEN_NET_PLAYER_NAME];	// netPlayerName is the player who owns this mover
		long				localMoverId;					// if >= 0, is locally controlled
		long				netRosterIndex;					// used for mover id in net packets
		StatusChunk			statusChunk;					// last status chunk built/received
		bool				newMoveChunk;					// set if last movechunk not yet processed
		MoveChunk			moveChunk;						// last move chunk built/received
		long				numWeaponFireChunks[2];
		unsigned long		weaponFireChunks[2][MAX_WEAPONFIRE_CHUNKS];
		long				numCriticalHitChunks[2];
		unsigned char		criticalHitChunks[2][MAX_CRITICALHIT_CHUNKS];
		long				numRadioChunks[2];
		unsigned char		radioChunks[2][MAX_RADIO_CHUNKS];
		bool				ejectOrderGiven;
															// Still awaiting final destruct orders from update.
		int32_t				numWeaponHitsHandled;
		float				timeLeft;						// How long before we return FALSE to update.
		bool				exploding;
		bool				withdrawing;
	
		float				yieldTimeLeft;					// How much time do I have left to wait
		glm::vec3		lastValidPosition;				// Last valid move path point I've been to
		char				pivotDirection;					// Used in pivotTo(): -1 = not pivoting
		float				lastHustleTime;					// last time we had to hustle (on bridge, etc.)

		static long			numMovers;

		bool				salvageVehicle;
		
		float				markDistanceMoved;				//Used to track distance object has moved since last mark of terrain visible.

		GameObjectWatchID	refitBuddyWID;
		GameObjectWatchID	recoverBuddyWID;

		int32_t				crashAvoidSelf;
		int32_t				crashAvoidPath;
		int32_t				crashBlockSelf;
		int32_t				crashBlockPath;
		float				crashYieldTime;
		int32_t				pathLockLength;
		int32_t             pathLockList[MAX_LOCK_RANGE][2];
		glm::vec3		moveCenter;
		float				moveRadius;

		int32_t				overlayWeightClass;

		float				timeToClearSelection;

		float				timeSinceMoving;

		float 				timeSinceFiredLast;				//used to track when they stop revealing

		//GameObjectPtr		lastMovingTarget;
		GameObjectWatchID	lastMovingTargetWID;

		bool 				mechSalvage;					//As this guy dies, ONLY check once if he's salvagable!

		glm::vec3		teleportPosition;				//debug feature :)
		int32_t				debugPage;

		static float		newThreatMultiplier;
		static float		marginOfError[2];
		static float		refitRange;
		static float		refitTime;
		static float		refitCostArray[NUM_COSTS][NUM_FIXERS];
		static float		refitAmount;
		static float		recoverRange;
		static float		recoverTime;
		static float		recoverCost;
		static float		recoverAmount;
		static bool			inRecoverUpdate;
		static char			optimalCells[MAX_ATTACK_CELLRANGE][MAX_ATTACK_INCREMENTS][2];
		static long			numOptimalIncrements;
		static short		rangedCellsIndices[MAX_ATTACK_CELLRANGE][2];
		static char			rangedCells[RANGED_CELLS_DIM][2];
		static long			IndirectFireWeapons[20];
		static long			AreaEffectWeapons[20];
		static unsigned long holdFireIconHandle;
		
		static TriggerAreaManager* triggerAreaMgr;


		bool				pathLocks;						//For movers which can be stepped on.  They do NOT lock!
		bool				isOnGui;						//For movers which start out on player team but not on gui.  Like raven in 0103

		int32_t             conStat;						//Contact status stored for this frame, for this machine
		float				fadeTime;						//Time between fade from LOS to non-LOS
		BYTE				alphaValue;						//Current Fade value;
		int32_t             causeOfDeath;

		// What the tick leaves for presentation to apply to the DRAW side of the appearance (names, pose, status, alpha, sensor
		// level, bars). The tick writes `appearancePush`; presentation (applyAppearancePush) applies it when `pushSerial`
		// moved past `pushPresented`, and only presentation writes `pushPresented`. The tick calls the appearance only for its
		// sim half (Appearance::setSimInput, gesture requests, update()).
		struct AppearancePush {
			bool				hasStatus = false;
			long				status = 0;
			bool				hasAlpha = false;			// apply alphaValue
			long				sensorLevel = -1;			// -1: leave unchanged
		};
		AppearancePush		appearancePush;
		uint32_t			pushSerial = 0;
		uint32_t			pushPresented = 0;

		int32_t				lowestWeaponNodeID;
		float				lowestWeaponNodeZ;

	//----------------
	//Member Functions

	public:

		virtual void init (bool create);

		Mover (void) {
			init(true);
		}
			
		virtual void destroy (void);

		virtual void init (bool create, ObjectTypePtr objType) {
			GameObject::init(create, objType);
		}

		virtual long init (FitIniFile* objProfile) {
			return(NO_ERR);
		}

		virtual long init (DWORD variantNum) {
			return(NO_ERR);
		}

		virtual void release (void);

		//virtual long update (void);

		//virtual void render (void);

		virtual void updateDebugWindow (GameDebugWindow* debugWindow);

		virtual char* getName (void) {
			return(name);
		}

		virtual void setName (char* s) {
			strncpy(name, s, MAXLEN_MOVER_NAME);
		}

		virtual void setControl (ControlType ctrlType) {
		}

		virtual void updateAIControl (void) {
		}

		virtual void updateNetworkControl (void) {
		}

		virtual void updatePlayerControl (void) {
		}

		virtual void updateDynamics (void) {
		}

	protected:
		friend class GameObjectManager;
		virtual void setPartId (long newPartId);
	public:

		virtual void setPosition (glm::vec3& newPosition);
		
		virtual void setTeleportPosition (glm::vec3& newPos);

		ContactInfoPtr getContactInfo (void) {
			return(contactInfo);
		}

		virtual void tradeRefresh (void);

		virtual void setMoveType (long type);

		virtual long getMoveType (void) {
			return(moveType);
		}

		virtual void setMoveLevel (long level) {
			moveLevel = level;
		}

		virtual long getMoveLevel (void) {
			return(moveLevel);
		}

		virtual bool getFollowRoads (void) {
			return(followRoads);
		}

		virtual void setFollowRoads (bool setting) {
			followRoads = setting;
		}

		virtual glm::vec3 getVelocity (void) {
			return(velocity);
		}

		virtual void setVelocity (glm::vec3& newVelocity) {
			velocity = newVelocity;
		}

		virtual float getSpeed (void) {
			return(glm::length(velocity));
		}

		virtual bool hasWeaponNode (void);

		glm::vec3 getLOSPosition (void);

		virtual void rotate (float angle);

		virtual void rotate (float angle, float facingAngle);
		
		virtual void setAwake (bool state);

		virtual bool isMarine(void) 
		{
			return(pathLocks == false);
		}

		void drawWaypointPath();
		void drawOverviewMovePath( unsigned long color );
		bool getMoveDestination( glm::vec3& out );
		void getWeaponBandsPresent( bool present[3] );	// short/medium/long weapon ranges

		void updateDrawWaypointPath();
		
//		virtual frame_of_ref getFrame (void)
//		{
//			return(frame);
//		}

		virtual SpeedState getSpeedState (void) {
			return(SPEED_STATE_STATIONARY);
		}

		virtual float getTerrainAngle (void);

		virtual float getVelocityTilt (void);

//		virtual void setFrame (frame_of_ref &newFrame)
//		{
//			frame = newFrame;
//		}
		
		virtual glm::vec3 relativePosition (float angle, float radius, unsigned long flags);

		virtual void setSensorRange (float range);
		
		virtual glm::vec3 getRotationVector (void) 
		{
			glm::vec3 rotationVec;
			rotationVec.x = 0.0f;
			rotationVec.y = -1.0f;
			rotationVec.z = 0.0f;
			Rotate(rotationVec, -rotation);
			return(rotationVec);
		}

		virtual bool hasActiveProbe (void);

		virtual float getEcmRange (void);

		virtual bool hasNullSignature (void);

		virtual void setSelected (bool set, long newSelectionIndex = 0) {
			GameObject::setSelected(set);
			selectionIndex = newSelectionIndex;
		}

		virtual long handleTacticalOrder (TacticalOrder tacOrder, long priority = 1, bool queuePlayerOrder = false);

		virtual AppearancePtr getAppearance (void) {
			return(appearance);
		}

//		virtual float getAppearRadius (void)
//		{
//			return appearance->getRadius();
//		}

		virtual void reduceAntiMissileAmmo (long numAntiMissiles);

		virtual void pilotingCheck (unsigned long situation = 0, float modifier = 0.0);

		virtual void forcePilotingCheck (void) {
			if (pilotCheckModifier < 0)
				pilotCheckModifier = 0;
		}

		virtual bool canFireWeapons (void) {
			return(true);
		}

		virtual void updateDamageTakenRate (void);

		virtual long checkShortRangeCollision (void) {
			return(NO_ACTION);
		}

		virtual void setOverlayWeightClass (long overlayClass) {
			overlayWeightClass = overlayClass;
		}

		virtual long getOverlayWeightClass (void) {
			return(overlayWeightClass);
		}

		virtual void getStopInfo (float &stopTime, float &stopDistance) {}

		virtual bool getAdjacentCellPathLocked (long level, long cellRow, long cellCol, long dir);

		virtual void updatePathLock (bool set);

		virtual bool getPathRangeLock (long range, bool* reachedEnd = nullptr);

		virtual long setPathRangeLock (bool set, long range = 0);

		virtual bool getPathRangeBlocked (long range, bool* reachedEnd = nullptr);

		virtual bool crashAvoidanceSystem (void) {
			return(false);
		}

		void setLastHustleTime (float t) {
			lastHustleTime = t;
		}

		float getLastHustleTime (void) {
			return(lastHustleTime);
		}

		virtual void updateHustleTime (void);

		virtual void mineCheck (void) {
			//----------------------------
			// Does nothing, be default...
		}

		virtual void updateMovement (void) {}

		virtual long bounceToAdjCell (void);

#ifdef USE_MOVERCONTROLS
		unsigned long getControlClass (void) {
			return(control->getControlClass());
		}
#endif

		virtual void setSquadId (char newSquadId) {
			squadId = newSquadId;
		}

		virtual char getSquadId (void) {
			return(squadId);
		}

		virtual long setTeamId (long _teamId, bool setup);
		
		virtual long getTeamId (void) {
			return(teamId);
		}

		virtual TeamPtr getTeam (void);

		virtual bool isFriendly (TeamPtr team);

		virtual bool isEnemy (TeamPtr team);

		virtual bool isNeutral (TeamPtr team);

		virtual long setGroupId (long _groupId, bool setup);

		virtual long getGroupId (void) {
			return(groupId);
		}

		virtual MoverGroupPtr getGroup (void);

		virtual void setPilotHandle (long _pilotHandle);

		virtual void loadPilot (const char* pilotFileName, const char* brainFileName, LogisticsPilot *lPilot);

		virtual void setCommanderId (long _commanderId);

		virtual long getCommanderId (void) {
			return(commanderId);
		}

		virtual CommanderPtr getCommander (void);
		
		virtual MechWarriorPtr getPilot (void) {
			return(pilot);
		}

		MoverPtr getPoint (void);

		void setTeamRosterIndex (long index) {
			teamRosterIndex = index;
		}

		long getTeamRosterIndex (void) {
			return(teamRosterIndex);
		}

		virtual long getContacts (int* contactList, int contactCriteria, int sortType);

		long getContactStatus (long scanningTeamID, bool includingAllies);

//		void setNetPlayerId (DWORD playerId) {
//			netPlayerId = playerId;
//		}

//		DWORD getNetPlayerId (void) {
//			return(netPlayerId);
//		}

		void setNetPlayerName(const char *name) {
            strncpy(netPlayerName,name,255);
		}

		const char* getNetPlayerName(void) {
			return(netPlayerName);
		}

		void setLocalMoverId (long id) {
			localMoverId = id;
		}

		long getLocalMoverId (void) {
			return(localMoverId);
		}

		virtual long getCBills (void) {
			return(0);
		}

		void setNetRosterIndex (long index) {
			netRosterIndex = index;
		}

		long getNetRosterIndex (void) {
			return(netRosterIndex);
		}

		long getNumWeaponFireChunks (long which) {
			return(numWeaponFireChunks[which]);
		}

		long clearWeaponFireChunks (long which);

		long addWeaponFireChunk (long which, WeaponFireChunkPtr chunk);

		long addWeaponFireChunks (long which, unsigned long* packedChunkBuffer, long numChunks);

		long grabWeaponFireChunks (long which, unsigned long* packedChunkBuffer, long maxChunks);

		virtual long updateWeaponFireChunks (long which);

		long getNumCriticalHitChunks (long which) {
			return(numCriticalHitChunks[which]);
		}

		long clearCriticalHitChunks (long which);

		long addCriticalHitChunk (long which, long bodyLocation, long criticalSpace);

		long addCriticalHitChunks (long which, unsigned char* packedChunkBuffer, long numChunks);

		long grabCriticalHitChunks (long which, unsigned char* packedChunkBuffer);

		virtual long updateCriticalHitChunks (long which);

		long getNumRadioChunks (long which) {
			return(numRadioChunks[which]);
		}

		long clearRadioChunks (long which);

		long addRadioChunk (long which, unsigned char msg);

		long addRadioChunks (long which, unsigned char* packedChunkBuffer, long numChunks);

		long grabRadioChunks (long which, unsigned char* packedChunkBuffer);

		virtual long updateRadioChunks (long which);

		virtual StatusChunkPtr getStatusChunk (void) {
			return(&statusChunk);
		}

		virtual long buildStatusChunk (void) {
			return(NO_ERR);
		}

		virtual long handleStatusChunk (long updateAge, unsigned long chunk) {
			return(NO_ERR);
		}

		virtual MoveChunkPtr getMoveChunk (void) {
			return(&moveChunk);
		}

		virtual long buildMoveChunk (void) {
			return(NO_ERR);
		}

		virtual long handleMoveChunk (unsigned long chunk) {
			return(NO_ERR);
		}

		void setMoveChunk (MovePathPtr path, MoveChunkPtr chunk);

		void playMessage (RadioMessageType messageId, bool propogateIfMultiplayer = false);

		virtual long calcCV (bool calcMax = false) {
			return(0);
		}

		void setFieldedCV (long CV) {
			fieldedCV = CV;
		}

		unsigned long getFieldedCV (void) {
			return(fieldedCV);
		}

		virtual void setThreatRating (short rating);

		virtual long getThreatRating (void);

//		virtual void getDamageClass (long& damageClass, bool& shutDown);

		virtual bool refit (float pointsAvailable, float& pointsUsed, bool ammoOnly = false);

		float calcRecoverPrice (void);

		virtual bool recover (void);

		long getInventoryDamage (long itemIndex);

		long getInventoryMax (long itemIndex) {
			return(MasterComponent::masterList[inventory[itemIndex].masterID].getHealth());
		}

		virtual long getBodyState (void) {
			return(-1);
		}

		long getSensorMax (void) {
            gosASSERT(sensor < MAX_MOVER_INVENTORY_ITEMS);
			if(sensor < MAX_MOVER_INVENTORY_ITEMS)
		    	return(MasterComponent::masterList[inventory[sensor].masterID].getHealth());
			else
				return 0;
		}

		long getSensorHealth (void) {
            gosASSERT(sensor < MAX_MOVER_INVENTORY_ITEMS);
			if(sensor < MAX_MOVER_INVENTORY_ITEMS)
            	return(inventory[sensor].health);
			else
				return 0;
		}

		float getVisualRange (void);

		float getLastWeaponEffectivenessCalc (void) {
			return(lastWeaponEffectivenessCalc);
		}

		float getLastOptimalRangeCalc (void) {
			return(lastOptimalRangeCalc);
		}

		virtual float getTotalEffectiveness(void)
		{
			return (0.0);
		}
		
		//ContactWatchPtr addContactWatch (GameObjectPtr who, ContactRecPtr contact) {
		//	return(contactWatches.add(who, contact));
		//}

		//void removeContactWatch (ContactWatchPtr watch) {
		//	contactWatches.remove(watch);
		//}

		void setChallenger (GameObjectPtr challenger);
		
		GameObjectPtr getChallenger (void);

/*		void addContact (ContactRecPtr contact) {
			sensorSystem.addContact(contact);
		}
*/
		long scanContact (long contactType, unsigned long contactHandle);

		long analyzeContact (long contactType, unsigned long contactHandle);

		long scanBattlefield (long quadrant, long contactType, long potentialContactType);

		virtual glm::vec3 calcOffsetMoveGoal (glm::vec3 target);

		virtual long calcGlobalPath (GlobalPathStep* globalPath, GameObjectPtr obj, glm::vec3* location, bool useClosedAreas);

		virtual long calcMoveGoal (GameObjectPtr target,
						  glm::vec3 moveCenter,
						  float moveRadius,
						  glm::vec3 moveGoal,
						  long selectionIndex,
						  glm::vec3& newGoal,
						  long numValidAreas,
						  short* validAreas,
						  unsigned long moveParams);
								   

		virtual int calcMovePath (MovePathPtr path,
								   long pathType,
								   glm::vec3 start,
								   glm::vec3 goal,
								   int* goalCell,
								   unsigned long moveParams = MOVEPARAM_NONE);

		virtual int calcEscapePath (MovePathPtr path,
									 glm::vec3 start,
									 glm::vec3 goal,
									 int* goalCell,
									 unsigned long moveParams,
									 glm::vec3& escapeGoal);

		virtual int calcMovePath (MovePathPtr path,
								   glm::vec3 start,
								   long thruArea[2],
								   long goalDoor,
								   glm::vec3 finalGoal,
								   glm::vec3* goalWorldPos,
								   int* goalCell,
								   unsigned long moveParams = MOVEPARAM_NONE);

		virtual float weaponLocked (long weaponIndex, glm::vec3 targetPosition);

		virtual bool weaponInRange (long weaponIndex, float metersToTarget, float buffer);

		virtual long getWeaponsReady (int* list, int listSize);

		virtual long getWeaponsLocked (int* list, int listSize);

		virtual long getWeaponsInRange (int * list, int listSize, float orderFireRange);

		virtual long getWeaponShots (long weaponIndex);
		
		virtual bool getWeaponIndirectFire (long weaponIndex);

		virtual bool getWeaponAreaEffect (long weaponIndex);
		
		virtual float getWeaponAmmoLevel (long weaponIndex);

		bool	getWeaponIsEnergy( long weaponIndx );

		virtual void calcWeaponEffectiveness (bool setMax);

		virtual void calcAmmoTotals (void);

		virtual long calcFireRanges (void);

		virtual float getOrderedFireRange (long* attackRange = nullptr);

		virtual float getMinFireRange (void);

		virtual float getMaxFireRange (void);

		virtual float getOptimalFireRange (void);

		virtual bool isWeaponIndex (long itemIndex);

		virtual bool isWeaponReady (long weaponIndex);

		virtual bool isWeaponMissile (long weaponIndex);

		virtual void startWeaponRecycle (long weaponIndex);

		virtual long tallyAmmo (long ammoMasterId);

		virtual long reduceAmmo (long ammoMasterId, long amount);

		virtual long getNumAmmoTypes (void) {
			return(numAmmoTypes);
		}

		bool hasNonAreaWeapon (void);
	
		virtual long getAmmoType (long ammoTypeIndex) {
			return(ammoTypeTotal[ammoTypeIndex].masterId);
		}

		virtual long getAmmoTypeTotal (long ammoTypeIndex) {
			//--------------------------------------------------
			// This assumes 0 <= ammoTypeIndex < numAmmoTypes...
			return(ammoTypeTotal[ammoTypeIndex].curAmount);
		}

		virtual long getAmmoTypeStart (long ammoTypeIndex) {
			//--------------------------------------------------
			// This assumes 0 <= ammoTypeIndex < numAmmoTypes...
			return(ammoTypeTotal[ammoTypeIndex].maxAmount);
		}

		virtual void deductWeaponShot (long weaponIndex, long ammoAmount = 1);

		virtual bool needsRefit(void);
		
		virtual long sortWeapons (int* weaponList, long* valueList, long listSize, long sortType, bool skillCheck);

		virtual float calcAttackChance (GameObjectPtr target, long aimLocation, float targetTime, long weaponIndex, float modifiers, long* range, glm::vec3* targetPoint = nullptr);

		virtual float calcAttackModifier (GameObjectPtr target, long weaponIndex, bool skillCheck) {
			return(0.0);
		}

		MasterComponentPtr getMasterComponent (long itemIndex) {
			return(&MasterComponent::masterList[inventory[itemIndex].masterID]);
		}

		virtual bool hitInventoryItem (long itemIndex, bool setupOnly = FALSE) {
			return(false);
		}

		bool isInventoryDisabled (long itemIndex) {
			return(inventory[itemIndex].disabled);
		}

		virtual void disable (unsigned long cause);

		virtual void shutDown (void);
		
		virtual void startUp (void);

		virtual void destroyBodyLocation (long location) {}

		virtual void calcCriticalHit (long hitLocation) {}

		virtual bool injureBodyLocation (long bodyLocation, float damage) {
			return(false);
		}

		virtual void ammoExplosion (long ammoIndex);

		virtual long fireWeapon (GameObjectPtr target, float targetTime, long weaponIndex, long attackType, long aimLocation, glm::vec3* targetPoint, float &damageDone) {
			return(NO_ERR);
		}

		virtual long handleWeaponFire (long weaponIndex,
									   GameObjectPtr target,
									   glm::vec3* targetPoint,
									   bool hit,
									   float entryAngle,
									   long numMissiles,
									   long hitLocation) {
			return(NO_ERR);
		}

		virtual void printFireWeaponDebugInfo (GameObjectPtr target, glm::vec3* targetPoint, long chance, long aimLocation, long roll, WeaponShotInfo* shotInfo);

		virtual void printHandleWeaponHitDebugInfo (WeaponShotInfo* shotInfo);

		virtual float relFacingDelta (glm::vec3 goalPos, glm::vec3 targetPos);

		virtual float relFacingTo (glm::vec3 goal, long bodyLocation = -1);

		virtual float relViewFacingTo (glm::vec3 goal) {
			return(GameObject::relFacingTo(goal));
		}

		bool canMoveHere (glm::vec3 worldPos);

		void setLastValidPosition (glm::vec3 pt) {
			lastValidPosition = pt;
		}

		glm::vec3 getLastValidPosition (void) {
			return(lastValidPosition);
		}

		virtual bool canPowerUp (void) {
			return(true);
		}
		
		virtual bool canMove (void) {
			return(true);
		}
		
		// UNIT-PROFILE-SEAM-1
		bool         hasCapability (Capability c) const { return runtime.currentCapabilities.has(c); }
		// UNIT-ACTION-QUERY-RESULT-1: reasoned availability built on the pure
		// unit_action.h contract. canPerform() is now a thin adapter over it.
		virtual ActionAvailability queryAction (UnitAction a) const;
		bool         canPerform    (UnitAction a) const { return queryAction(a) == ActionAvailability::Available; }
		// LOCOMOTION-FACET-1 (slice 4): DATA read, no longer class-derived. The
		// locomotion model is a facet on the unit's type profile (UnitProfile::
		// locomotion), populated at type-load: LOCO_MECH_WALKER for mechs,
		// LOCO_WHEELED for the base/vehicle default, optionally overridden by a
		// `.fit [UnitProfile] Locomotion=` token. queryAction() feeds it into
		// theActionExecutorRegistry().isCompatible(Jump, locomotionModel()). No
		// longer virtual — the value comes from data, not the C++ class, so the
		// BattleMech override was removed. Defined in mover.cpp (needs ObjectType).
		int  locomotionModel (void) const;   // reads getObjectType()->profile().locomotion
		// DAMAGE-SCHEMA-FACET-1 (slice 5): DATA read of the unit's hit-location
		// model from its type profile (UnitProfile::damageSchema), populated at
		// type-load: DMG_MECH_8PART for mechs, DMG_VEHICLE_FACINGS for the
		// base/vehicle default, optionally overridden by a `.fit [UnitProfile]
		// DamageSchema=` token. PURE ACCESSOR — read by nobody yet; it is the
		// source a future resolveHit(HitRequest) and the Phase-2 HardpointId
		// validator will consult. A null type falls back to the base default so
		// early-init paths never crash. Defined in mover.cpp (needs ObjectType).
		DamageSchemaId damageSchema (void) const;

		// LOADOUT-HARDPOINT-JOIN-1 (Phase-2 slice 3): OPTIONAL loadout->hardpoint
		// join, report-only. Resolve an inventory item's optional hardpointId
		// (InventoryItem::hardpointId, 0 == unset) to its HardpointProfile. The base
		// has no hardpoint table -> always returns false (stock declares none, so no
		// join is ever gathered -> zero findings -> gate-ON parity). A future data
		// source (additive-merge registry / .fit block) overrides this to supply
		// declared hardpoints. PURE lookup, no mount/render effect.
		virtual bool resolveHardpoint (unsigned int /*hardpointId*/,
		                               HardpointProfile& /*out*/) const { return false; }

		// Gated (MC2_UNIT_PROFILE_DATA) report-only pass: walk inventory[], gather
		// every DECLARED loadout->hardpoint join (hardpointId != 0, resolvable), and
		// validate weapon-tag / damage-location / socket coherence via the pure
		// validateLoadoutHardpointJoins(). Logs modder-readable findings; changes NO
		// runtime state and NO visual mount (inventory[].facing / getWeaponNode still
		// own that -- slice 4). Stock -> nothing gathered -> silent. Defined in mover.cpp.
		void validateLoadoutHardpointJoins (const char* unitLabel) const;

		virtual bool canJump (void) {
			if (mc2UnitProfileDataEnabled()) return canPerform(UnitAction::Jump);
			return false;                       // original Mover behavior
		}

		// UNIT-PROFILE-SEAM-1: copy type baseline -> runtime at the type-bind seam.
		virtual void onTypeBound (void);

		// LOADOUT-THREE-LAYER-1 (Phase 3): variant identity for the shadow-compare
		// witness. The chassis stem alone is too coarse -- a chassis has many
		// variants (the CSV 97-row-stride blocks) whose loadouts differ (Atlas
		// Prime has no jumpjets; other Atlas variants do). This lets the offline
		// compare join by (chassis, variant) instead of chassis stem, so it diffs
		// the ACTUAL spawned variant against the SAME converted variant. Base Mover
		// has no variant concept -> empty string (falls back to chassis-level join).
		virtual const char* getVariantName (void) const { return ""; }
		virtual long getVariantId (void) const { return -1; }

		virtual float getJumpRange (long* numOffsets = nullptr, long* jumpCost = nullptr) {
			if (numOffsets)
				*numOffsets = 8;
			if (jumpCost)
				*jumpCost = 0;
			return(0.0);
		}

		virtual bool isJumping (glm::vec3* jumpGoal = nullptr) {
			return(false);
		}

		virtual bool isMineSweeper (void) {
			return(false);
		}

		virtual bool isMineLayer (void) {
			return(false);
		}

		virtual bool isLayingMines (void) {
			return(false);
		}

		virtual float getFireArc (void);

		virtual float calcMaxSpeed (void) {
			return(0.0);
		}

		virtual float calcSlowSpeed (void) {
			return(0.0);
		}

		virtual float calcModerateSpeed (void) {
			return(0.0);
		}

		virtual long calcSpriteSpeed (float speed, unsigned long flags, long& state, long& throttle) {
			state = 0;
			throttle = 100;
			return(-1);
		}

		virtual glm::vec3 getPositionFromHS (long weaponType) 
		{
			glm::vec3 pos;
			pos = glm::vec3(0.0f);
			return(pos);
		}

		virtual float getGestureStopDistance (void) {
			return 0.0;
		}
		
		virtual bool handleEjection (void);

		virtual bool isWithdrawing (void);
		
		virtual bool underPlayerControl (void) {
			return (true);
		}
		
		~Mover (void) {
			destroy();
		}

		bool enemyRevealed (void);

		virtual const char* getIfaceName(void) {
            STOP((""));
            return ("No Name");
        }

		void drawSensorTextHelp (float screenX, float screenY, long resID, DWORD color, bool drawBOLD);

		virtual SensorSystem* getSensorSystem(){ return sensorSystem; }

		
		static long loadGameSystem (FitIniFilePtr mechFile, float visualRange);

		bool isCloseToFirstTacOrder( glm::vec3& pos );

		void removeFromUnitGroup( int id );
		void addToUnitGroup( int id );
		bool isInUnitGroup( int id );
		virtual bool isRefit(){ return 0; }
		virtual bool isRecover() { return 0; }

		int getIconPictureIndex(){ return iconPictureIndex; }

		bool isWeaponWorking(long index );

		virtual void startShutDown (void)
		{
		
		}
		
		virtual bool isMech (void)
		{
			return false;
		}
		
		virtual bool isVehicle (void)
		{
			return false;
		}
		
		virtual bool isGuardTower (void)
		{
			return false;
		}

		virtual bool isOnGUI (void)
		{
			return isOnGui;
		}
		
		virtual void setOnGUI (bool onGui)
		{
			isOnGui = onGui;
		}
		
		virtual void Save (PacketFilePtr file, long packetNum);

		void CopyTo (MoverData *data);

		void Load (MoverData *data);

		static void initOptimalCells (long numIncrements);
};

//---------------------------------------------------------------------------

struct _MoverInitData {
	char			pilotFileName[50];
	bool			overrideLoadedPilot;
	float			gunnerySkill;
	float			pilotingSkill;
	bool			specialtySkills[NUM_SPECIALTY_SKILLS];
	char			brainFileName[50];
	char			csvFileName[50];
	int32_t			objNumber;
	int32_t 		rosterIndex;
	DWORD           controlType;
	DWORD           controlDataType;
	DWORD           variant;
	glm::vec3	position;
	int32_t 		rotation;
	char			teamID;
	char			commanderID;
	DWORD			baseColor;
	DWORD			highlightColor1;
	DWORD			highlightColor2;
	char			gestureID;
	bool			active;
	bool			exists;
	char			icon;
	bool			capturable;
	long            numComponents;
	long            components[50];
}; using MoverInitData = _MoverInitData;

//---------------------------------------------------------------------------

extern MoverPtr getMoverFromHandle (long partId);

//******************************************************************************************

#endif
