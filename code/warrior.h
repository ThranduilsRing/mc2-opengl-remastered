//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//***************************************************************************
//
//								WARRIOR.H
//
//***************************************************************************

#ifndef WARRIOR_H
#define	WARRIOR_H

#include "object_shared_constants.h"
#include "warrior_types.h"
#include <array>
#include <optional>
namespace mc2::render_view { struct HudMover; }

#ifndef MCLIB_H
#include"mclib.h"
#endif

#ifndef MOVE_H
#include"move.h"
#endif

#ifndef DMOVEMGR_H
#include"dmovemgr.h"
#endif

#ifndef DWARRIOR_H
#include"dwarrior.h"
#endif

#ifndef DGAMEOBJ_H
#include"dgameobj.h"
#endif

#ifndef DMOVER_H
#include"dmover.h"
#endif

#ifndef DMECH_H
#include"dmech.h"
#endif

#ifndef MECHCLASS_H
#include"mechclass.h"
#endif

#ifndef DGVEHICL_H
#include"dgvehicl.h"
#endif

#include "brain_task_queue.h"  // MC2_BRAIN_TASKQ: per-warrior brain task queue substrate
#include "mech_brain_runtime.h"  // MC2_BRAIN_RUNTIME: per-warrior brain runtime state

#ifndef DTEAM_H
#include"dteam.h"
#endif

#ifndef DCOMNDR_H
#include"dcomndr.h"
#endif

#ifndef TACORDR_H
#include"tacordr.h"
#endif

#ifndef DRADIO_H
#include"dradio.h"
#endif

#ifndef MECH3D_H
#include"mech3d.h"
#endif

#ifndef DGOAL_H
#include"dgoal.h"
#endif

#ifdef USE_MOVERGROUPS
#ifndef DGROUP_H
#include"dgroup.h"
#endif
#endif

#ifndef DGAMELOG_H
#include"dgamelog.h"
#endif

extern float scenarioTime;

class BldgAppearance;
#include "warrior_queue_markers.h"

//***************************************************************************

struct PilotAlarm {
	char			code;
	unsigned char	numTriggers;			
	int32_t         trigger[MAX_ALARM_TRIGGERS];
};

//---------------------------------------------------------------------------

struct RadioLog {
	float				lastUnderFire;			// time of last under fire message
	bool				weaponsIneffective;		// true if already informed player
	bool				weaponsOut;				// true if already informed player
	float				lastContact;			// time of last contact message
	RadioMessageType	lastMessageType;		// e.g. RADIO_WEAPONS_OUT
	DWORD				lastMessage;			// PacketNumber of message
	float				lastMessageTime;		// time of last message (of any type)
};


//---------------------------------------------------------------------------

struct _TargetPriority {
	int32_t				type;
	int32_t				params[3];
}; using TargetPriority = _TargetPriority;

using TargetPriorityPtr = TargetPriority*;

class TargetPriorityList {

	public:

		int32_t				size;
		TargetPriority		list[MAX_TARGET_PRIORITIES];

	public:


		
		void init (void);

		TargetPriorityList (void) {
			init ();
		}

		void destroy (void);

		~TargetPriorityList (void)	{
			destroy();
		}

		int32_t insert (int32_t index, TargetPriorityPtr priority);

		void remove (int32_t index);

		int32_t calcAction (MechWarriorPtr pilot, GameObjectPtr target);

		int32_t calcTarget (MechWarriorPtr pilot, glm::vec3 location, int32_t contactCriteria, int32_t& action);
};

//---------------------------------------------------------------------------



//---------------------------------------------------------------------------

struct _AttackerRec {
	unsigned long				WID;
	float						lastTime;
}; using AttackerRec = _AttackerRec;

using AttackerRecPtr = AttackerRec*;

// MAX_GLOBAL_PATH: defined once in move.h (included above).

struct _MoveOrders {
	//------------------
	// order  parameters
	float						time;
	char						origin;
	long						speedType;				// best, max, slow, moderate
	float						speedVelocity;			// based upon speedType (m/s)
	char						speedState;				// based upon speedVelocity (walk, etc.)
	char						speedThrottle;			// if speedState is walk, else ignored
	unsigned long				goalType;				// is there no goal, a location or object?
	GameObjectWatchID			goalObjectWID;			// if our goal is an object...
	glm::vec3				goalObjectPosition;		// object's position at time of path calc
	glm::vec3				goalLocation;			// goal location, if any
	//----------------------
	// pathfinding guts/data
	float						nextUpdate;
	bool						newGoal;
	glm::vec3				wayPath[MAX_WAYPTS];	// if this move order has a way path...
	char						numWayPts;				// how many way points?
	char						curWayPt;				// current goal waypoint
	char						curWayDir;				// waypath direction: 1 = forward, -1 = backward
	char						pathType;				// "quick" path or global/long-range?
	glm::vec3				originalGlobalGoal[2];	// 0 = original final goal, 1 = actual final goal
	glm::vec3				globalGoalLocation;
	GlobalPathStep				globalPath[MAX_GLOBAL_PATH];
	short						numGlobalSteps;
	short						curGlobalStep;
	MovePathPtr					path[2];				// 0 = current path, 1 = next path
	float						timeOfLastStep;			// last time a step was reached for this order
	long						moveState;				// forward, reverse, paused, etc.
	long						moveStateGoal;
	bool						twisting;
	float						yieldTime;
	long						yieldState;				// have we just yielded, been for a while, etc.
	float						waitForPointTime;
	bool						run;

	void init (void);

}; using MoveOrders = _MoveOrders;

struct _SaveableMoveOrders {
	//------------------
	// order  parameters
	float						time;
	char						origin;
	long						speedType;				// best, max, slow, moderate
	float						speedVelocity;			// based upon speedType (m/s)
	char						speedState;				// based upon speedVelocity (walk, etc.)
	char						speedThrottle;			// if speedState is walk, else ignored
	unsigned long				goalType;				// is there no goal, a location or object?
	GameObjectWatchID			goalObjectWID;			// if our goal is an object...
	glm::vec3				goalObjectPosition;		// object's position at time of path calc
	glm::vec3				goalLocation;			// goal location, if any
	//----------------------
	// pathfinding guts/data
	float						nextUpdate;
	bool						newGoal;
	glm::vec3				wayPath[MAX_WAYPTS];	// if this move order has a way path...
	char						numWayPts;				// how many way points?
	char						curWayPt;				// current goal waypoint
	char						curWayDir;				// waypath direction: 1 = forward, -1 = backward
	char						pathType;				// "quick" path or global/long-range?
	glm::vec3				originalGlobalGoal[2];	// 0 = original final goal, 1 = actual final goal
	glm::vec3				globalGoalLocation;
	GlobalPathStep				globalPath[MAX_SAVED_GLOBAL_PATH];
	char						numGlobalSteps;
	char						curGlobalStep;
	MovePath					path[2];				// 0 = current path, 1 = next path
	float						timeOfLastStep;			// last time a step was reached for this order
	long						moveState;				// forward, reverse, paused, etc.
	long						moveStateGoal;
	bool						twisting;
	float						yieldTime;
	long						yieldState;				// have we just yielded, been for a while, etc.
	float						waitForPointTime;
	bool						run;

	void copy (MoveOrders &orders)
	{
		time = orders.time;
		origin = orders.origin;
		speedType = orders.speedType;
		speedVelocity = orders.speedVelocity;
		speedState = orders.speedState;
		goalType = orders.goalType;
		goalObjectWID = orders.goalObjectWID;
		goalObjectPosition = orders.goalObjectPosition;
		goalLocation = orders.goalLocation;
		nextUpdate = orders.nextUpdate;
		newGoal = orders.newGoal;
		memcpy(wayPath,orders.wayPath,sizeof(glm::vec3) * MAX_WAYPTS);
		numWayPts = orders.numWayPts;
		curWayPt = orders.curWayPt;
		curWayDir = orders.curWayDir;	// SAVE/LOAD-CURWAYDIR-1: was omitted -> patrol dir lost across save/load
		pathType = orders.pathType;
		originalGlobalGoal[0] = orders.originalGlobalGoal[0];
		originalGlobalGoal[1] = orders.originalGlobalGoal[1];
		globalGoalLocation = orders.globalGoalLocation;
		memcpy(globalPath,orders.globalPath,sizeof(globalPath));
		numGlobalSteps = static_cast<char>(orders.numGlobalSteps);
		curGlobalStep = static_cast<char>(orders.curGlobalStep);
		path[0] = *(orders.path[0]);
		path[1] = *(orders.path[1]);
		// The record is written raw: its padding must be zero however the live MovePath was built, copied or loaded.
		mc2CanonicalizeMovePathRecord(path[0]);
		mc2CanonicalizeMovePathRecord(path[1]);
		timeOfLastStep = orders.timeOfLastStep;
		moveState = orders.moveState;
		moveStateGoal = orders.moveStateGoal;
		twisting = orders.twisting;
		yieldTime = orders.yieldTime;
		yieldState = orders.yieldState;
		waitForPointTime = orders.waitForPointTime;
		run = orders.run;
		if (orders.numGlobalSteps > MAX_SAVED_GLOBAL_PATH) {
			// Keep the final order, but never serialize a truncated active route.
			newGoal = true;
			nextUpdate = 0.0f;
			pathType = MOVEPATH_UNDEFINED;
			numGlobalSteps = curGlobalStep = 0;
			for (auto& localPath : path) {
				localPath.numSteps = localPath.numStepsWhenNotPaused = 0;
				localPath.curStep = 0;
				localPath.globalStep = -1;
			}
		}
	}

	void copyTo (MoveOrders &orders)
	{
		orders.time = time;
		orders.origin = origin;
		orders.speedType = speedType;
		orders.speedVelocity = speedVelocity;
		orders.speedState = speedState;
		orders.goalType = goalType;
		orders.goalObjectWID = goalObjectWID;
		orders.goalObjectPosition = goalObjectPosition;
		orders.goalLocation = goalLocation;
		orders.nextUpdate = nextUpdate;
		orders.newGoal = newGoal;
		memcpy(orders.wayPath,wayPath,sizeof(glm::vec3) * MAX_WAYPTS);
		orders.numWayPts = numWayPts;
		orders.curWayPt = curWayPt;
		orders.curWayDir = curWayDir;	// SAVE/LOAD-CURWAYDIR-1: was omitted -> patrol dir lost across save/load
		orders.pathType = pathType;
		orders.originalGlobalGoal[0] = originalGlobalGoal[0];
		orders.originalGlobalGoal[1] = originalGlobalGoal[1];
		orders.globalGoalLocation = globalGoalLocation;
		memcpy(orders.globalPath,globalPath,sizeof(globalPath));
		orders.numGlobalSteps = numGlobalSteps;
		orders.curGlobalStep = curGlobalStep;
		for (long i = 0; i < 2; i++)
		{
			//orders.path[i] = new MovePath;
			if (!orders.path[i])
				Fatal(0, " No RAM for warrior path ");

			*(orders.path[i]) = path[i];
		}

		orders.timeOfLastStep = timeOfLastStep;
		orders.moveState = moveState;
		orders.moveStateGoal = moveStateGoal;
		orders.twisting = twisting;
		orders.yieldTime = yieldTime;
		orders.yieldState = yieldState;
		orders.waitForPointTime = waitForPointTime;
		orders.run = run;
	}


}; using SaveableMoveOrders = _SaveableMoveOrders;

struct _AttackOrders {
	float						time;
	char						origin;
	long						type;				// to kill, to disable, etc.
	GameObjectWatchID			targetWID;		// current object targeted for attack, if attacking object
	glm::vec3				targetPoint;		// target location targeted for attack, if attacking point
	long						aimLocation;
	bool						pursue;				// does this attack entail a move order too?
	long						tactic;
	float						targetTime;

	void init (void);

}; using AttackOrders = _AttackOrders;

struct _SituationOrders {
	float						time;
	// general orders
	long						mode;				// NORMAL, WITHDRAW, RETREAT, ETC.
	float						defFormation;
	float						curFormation;
	// fire orders
	bool						openFire;
	bool						uponFireOnly;		// openFire set to TRUE upon getting attacked
	float						fireRange;
	float						fireOdds;
	// guard orders
	GameObjectWatchID			guardObjectWID;

	void init (void);

}; using SituationOrders = _SituationOrders;

#if 0
struct _RoleOrders {
	float						time;
	MechWarriorRole				role;
}; using RoleOrders = _RoleOrders;
#endif

//------------------------------------------------------------------------------------------

inline constexpr int NUM_MEMORY_CELLS = 60;

union _MemoryCell {
	float						real;
	int                         integer;
}; using MemoryCell = _MemoryCell;

//------------------------------------------------------------------------------------------

inline constexpr int MAX_ATTACKERS = 50;

//------------------------------------------------------------------------------------------

struct _QueuedTacOrder
{
	long						id;
	glm::vec3				point;
	unsigned long				packedData[2];
	unsigned char				tactic;
	BldgAppearance*				marker;
	unsigned char 				moveMode;			//So save/load can recreate this lovely pointer!
}; using QueuedTacOrder = _QueuedTacOrder;

using QueuedTacOrderPtr = QueuedTacOrder*;

//------------------------------------------------------------------------------------------

inline constexpr int NUM_PILOT_DEBUG_STRINGS = 5;
inline constexpr int MAXLEN_PILOT_DEBUG_STRING = 80;
inline constexpr int MAXLEN_PILOT_NAME = 80;
inline constexpr int MAXLEN_PILOT_CALLSIGN = 20;
inline constexpr int MAXLEN_PILOT_PHOTO = 20;
inline constexpr int MAXLEN_PILOT_VIDEO = 20;
inline constexpr int MAXLEN_PILOT_AUDIO = 20;
inline constexpr int MAXLEN_PILOT_BRAIN = 40;

struct _MechWarriorData
{
	bool					used;
	char					name[MAXLEN_PILOT_NAME];
	char					callsign[MAXLEN_PILOT_CALLSIGN];
	char					videoStr[MAXLEN_PILOT_VIDEO];
	char					audioStr[MAXLEN_PILOT_AUDIO];
	char					brainStr[MAXLEN_PILOT_BRAIN];
	int32_t					index;
	int32_t					paintScheme;
	int32_t					photoIndex;

	char					rank;
	char					skills[NUM_SKILLS];				//Current
	char					professionalism;
	char					professionalismModifier;
	char					decorum;
	char					decorumModifier;
	char					aggressiveness;
	char					courage;
	char					baseCourage;

	float					wounds;
	float					health;
	int32_t					status;
	bool					escapesThruEjection;
	RadioLog				radioLog;
	bool					notMineYet;						// Pilot must EARNED through logistics!!!

	int32_t					teamId;
	GameObjectWatchID		vehicleWID;						// Must point to a Mover

	int					    numSkillUses[NUM_SKILLS][NUM_COMBAT_STATS];
	int					    numSkillSuccesses[NUM_SKILLS][NUM_COMBAT_STATS];
	int					    numMechKills[NUM_VEHICLE_CLASSES][NUM_COMBAT_STATS];
	int					    numPhysicalAttacks[NUM_PHYSICAL_ATTACKS][NUM_COMBAT_STATS];
	float					skillRank[NUM_SKILLS];
	float					skillPoints[NUM_SKILLS];
	char					originalSkills[NUM_SKILLS];
	char					startingSkills[NUM_SKILLS];
	bool					specialtySkills[NUM_SPECIALTY_SKILLS];
	GameObjectWatchID		killed[MAX_WARRIOR_KILLED];
	int32_t					numKilled;

	int32_t					descID;							//Used by Logistics to Desc.
	int32_t					nameIndex;						//Used by Logistics to Desc.		

	float					timeOfLastOrders;				// when I ask for orders
	AttackerRec				attackers[MAX_ATTACKERS];		// should never have more than 12 attackers...
	int32_t					numAttackers;
	float					attackRadius;

	MemoryCell				memory[NUM_MEMORY_CELLS];
	char					debugStrings[NUM_PILOT_DEBUG_STRINGS][MAXLEN_PILOT_DEBUG_STRING];

	float					brainUpdate;
	float					combatUpdate;
	float					movementUpdate;
	int32_t                 weaponsStatus[MAX_WEAPONS_PER_MOVER];
	int32_t					weaponsStatusResult;

	bool					useGoalPlan;
	int32_t					mainGoalAction;
	GameObjectWatchID		mainGoalObjectWID;
	glm::vec3			mainGoalLocation;
	float					mainGoalControlRadius;
	short					lastGoalPathSize;
	short					lastGoalPath[MAX_SAVED_GLOBAL_PATH];

	bool					newTacOrderReceived[NUM_ORDERSTATES];
	TacticalOrder			tacOrder[NUM_ORDERSTATES];
	TacticalOrder			lastTacOrder;
	TacticalOrder			curTacOrder;
	PilotAlarm				alarm[NUM_PILOT_ALARMS];
	PilotAlarm				alarmHistory[NUM_PILOT_ALARMS];	// used by brain update in ABL
	int32_t					alarmPriority;
	bool					curPlayerOrderFromQueue;
	bool					tacOrderQueueLocked;
	bool					tacOrderQueueExecuting;
	bool					tacOrderQueueLooping;
	char					numTacOrdersQueued;	// Currently, only player orders may be queued...
	QueuedTacOrder			tacOrderQueue[MAX_QUEUED_TACORDERS_PER_WARRIOR];
	int32_t					tacOrderQueueIndex;
	int32_t					nextTacOrderId;
	int32_t					lastTacOrderId;

	int32_t					coreScanTargetWID;
	int32_t					coreAttackTargetWID;
	int32_t					coreMoveTargetWID;
	TargetPriorityList		targetPriorityList;
	int32_t					brainState;
	bool					willHelp;

	SaveableMoveOrders		moveOrders;
	AttackOrders			attackOrders;
	SituationOrders			situationOrders;
	GameObjectWatchID		lastTargetWID;
	float					lastTargetTime;
	bool					lastTargetObliterate;
	bool					lastTargetFriendly;
	bool					lastTargetConserveAmmo;
	bool					keepMoving;				//If true, he won't find his "optimal" spot and stand there.

	char					orderState;

	uint32_t                debugFlags;

	unsigned char			oldPilot;

	int32_t                 warriorBrainHandle;
}; using MechWarriorData = _MechWarriorData;

struct _StaticMechWarriorData
{
	int32_t				numWarriors;
	int32_t				numWarriorsInCombat;
	bool				brainsEnabled[MAX_TEAMS];
	float				minSkill;
	float				maxSkill;
	int32_t				increaseCap;
	float				maxVisualRadius;
	int32_t				curEventID;
	int32_t				curEventTrigger;
}; using StaticMechWarriorData = _StaticMechWarriorData;

class MechWarrior {

	protected:

		// Misc.
		bool					used;
		char					name[MAXLEN_PILOT_NAME];
		char					callsign[MAXLEN_PILOT_CALLSIGN];
		char					videoStr[MAXLEN_PILOT_VIDEO];
		char					audioStr[MAXLEN_PILOT_AUDIO];
		char					brainStr[MAXLEN_PILOT_BRAIN];
		int32_t					index;
		int32_t					paintScheme;
		int32_t					photoIndex;

		// Skills and Traits
		char					rank;
		char					professionalism;
		char					professionalismModifier;
		char					decorum;
		char					decorumModifier;
		char					aggressiveness;
		char					courage;
		char					baseCourage;

		// Status
		float					wounds;
		float					health;
		int32_t					status;
		bool					escapesThruEjection;
		RadioLog				radioLog;
		bool					notMineYet;				// Pilot must EARNED through logistics!!!

		// Unit
		int32_t					teamId;
		GameObjectWatchID		vehicleWID;			// Must point to a Mover

	public:
		// Combat Stats and History
		int32_t                 numSkillUses[NUM_SKILLS][NUM_COMBAT_STATS];
		int32_t					numSkillSuccesses[NUM_SKILLS][NUM_COMBAT_STATS];
		int32_t					numMechKills[NUM_VEHICLE_CLASSES][NUM_COMBAT_STATS];
		int32_t					numPhysicalAttacks[NUM_PHYSICAL_ATTACKS][NUM_COMBAT_STATS];
		char					skills[NUM_SKILLS];				//Current
		float					skillRank[NUM_SKILLS];
		float					skillPoints[NUM_SKILLS];
		char					originalSkills[NUM_SKILLS];		//How I arrived in MechCommander
		char					startingSkills[NUM_SKILLS];		//How I arrived in the mission
		bool					specialtySkills[NUM_SPECIALTY_SKILLS];
		GameObjectWatchID		killed[MAX_WARRIOR_KILLED];
		int32_t					numKilled;

		int32_t					descID;							//Used by Logistics to Desc.
		int32_t					nameIndex;						//Used by Logistics to Desc.		
		static	SpecialtySkillType	skillTypes[NUM_SPECIALTY_SKILLS];
	protected:

		// AI
		float					timeOfLastOrders;		// when I ask for orders
		AttackerRec				attackers[MAX_ATTACKERS];			// should never have more than 12 attackers...
		int32_t					numAttackers;
		float					attackRadius;

		// ABL Brain and Memory
		MemoryCell				memory[NUM_MEMORY_CELLS];
		ABLModulePtr			brain;
		BrainTaskQueue*         brainTaskQueue = nullptr;   // MC2_BRAIN_TASKQ: saved by the BRN1 section
		MechBrainRuntime*       brainRuntime = nullptr;    // MC2_BRAIN_RUNTIME: saved by the BRN1 section
		int32_t                 warriorBrainHandle;
		SymTableNodePtr			brainAlarmCallback[NUM_PILOT_ALARMS];
		char					debugStrings[NUM_PILOT_DEBUG_STRINGS][MAXLEN_PILOT_DEBUG_STRING];

		// Orders
		float					brainUpdate;
		float					combatUpdate;
		float					movementUpdate;
		int32_t                 weaponsStatus[MAX_WEAPONS_PER_MOVER];
		int32_t	                weaponsStatusResult;

		bool					useGoalPlan;
		int32_t					mainGoalAction;
		GameObjectWatchID		mainGoalObjectWID;
		glm::vec3			mainGoalLocation;
		float					mainGoalControlRadius;
		short					lastGoalPathSize;
		short					lastGoalPath[MAX_GLOBAL_PATH];

		bool					newTacOrderReceived[NUM_ORDERSTATES];
		TacticalOrder			tacOrder[NUM_ORDERSTATES];
		TacticalOrder			lastTacOrder;
		TacticalOrder			curTacOrder;
		PilotAlarm				alarm[NUM_PILOT_ALARMS];
		PilotAlarm				alarmHistory[NUM_PILOT_ALARMS];	// used by brain update in ABL
		int32_t					alarmPriority;
		bool					curPlayerOrderFromQueue;
		bool					tacOrderQueueLocked;
		bool					tacOrderQueueExecuting;
		bool					tacOrderQueueLooping;
		char					numTacOrdersQueued;	// Currently, only player orders may be queued...
		QueuedTacOrderPtr		tacOrderQueue;
		std::array<std::optional<GameObjectWatchID>, MAX_QUEUED_TACORDERS_PER_WARRIOR> queueTargets{};
		// B7o.4: runtime owners of the queued waypoint markers, index-parallel to the queue slice (a marker exists only for slot < numTacOrdersQueued).
		// The wire record QueuedTacOrder::marker is dead storage (always null); never copy these, never save them.
		mc2::QueueMarkerArray<BldgAppearance, MAX_QUEUED_TACORDERS_PER_WARRIOR> queueMarkers;
		int32_t					tacOrderQueueIndex;
		int32_t					nextTacOrderId;
		int32_t					lastTacOrderId;

		int32_t					coreScanTargetWID;
		int32_t					coreAttackTargetWID;
		int32_t					coreMoveTargetWID;
		TargetPriorityList		targetPriorityList;
		int32_t					brainState;
		bool					willHelp;

		MoveOrders				moveOrders;
		AttackOrders			attackOrders;
		SituationOrders			situationOrders;
		//RoleOrders				roleOrders;
		GameObjectWatchID		lastTargetWID;
		float					lastTargetTime;
		bool					lastTargetObliterate;
		bool					lastTargetFriendly;
		bool					lastTargetConserveAmmo;
		bool					keepMoving;				//If true, he won't find his "optimal" spot and stand there.

		char					orderState;

		PathQueueRecPtr			movePathRequest;

		// PATHFINDING-JUMP-FAIL-1: per-pilot doomed jump-path backoff (transient,
		// runtime-only -- NOT serialized; copyToData/copyFromData ignore these).
		// Records the goal cell of the last jump solve that returned no path and
		// the path-manager frame it failed on, so requestMovePath can short-circuit
		// re-enqueuing the same doomed jump goal within kJumpFailBackoffFrames.
		// Inert unless MC2_PATH_JUMP_FAIL_BACKOFF is set (default-OFF killswitch).
		int32_t					lastFailedJumpGoalCellR;
		int32_t					lastFailedJumpGoalCellC;
		long					lastJumpFailFrame;

		uint32_t                debugFlags;

		RadioPtr				radio;
		bool					isPlayingMsg;		//Always false unless I'm playing a message!
		unsigned char			oldPilot;

		//MechWarriorPtr			next;

	public:

		static int32_t			numWarriors;
		static int32_t			numWarriorsInCombat;
		static bool				brainsEnabled[MAX_TEAMS];
		static float			minSkill;
		static float			maxSkill;
		static int32_t			increaseCap;
		static float			maxVisualRadius;
		static int32_t			curEventID;
		static int32_t			curEventTrigger;

		static MechWarrior*		warriorList[MAX_WARRIORS];
		static GoalManager*		goalManager;

		static BldgAppearance*		wayPointMarkers[3];

	public:

		void* operator new (size_t ourSize);
		void operator delete (void* us);
		
		static void setup (void);

		static void shutdown (void);

		static MechWarrior* newWarrior (void);

		static void freeWarrior (MechWarrior* warrior);

		static bool warriorInUse (char *warriorName);

		void init (bool create);

		void init (MechWarriorData& data, bool wideTargets = false);   // by reference: a by-value copy leaves the struct padding as stack leftovers, which a later save writes

		MechWarrior (void) {
			init(true);
		}

		void destroy (void);

		~MechWarrior (void);	// out of line: queueMarkers holds unique_ptr<BldgAppearance>, complete only in warrior.cpp

		long init (FitIniFile* warriorFile);

		void clear (void);

		void update (void);

		void updateMissionSkills();


		char* getName (void) {
			return(name);
		}

		int32_t getPhoto (void) {
			return(photoIndex);
		}

		char* getCallsign (void) {
			return(callsign);
		}

		void setIndex (long i) {
			index = i;
		}

		long getIndex (void) {
			return(index);
		}

		bool isUsed() const { return used; }

		long getPaintScheme (void)
		{
			return paintScheme;
		}

		void setPaintScheme (long newPaintScheme)
		{
			paintScheme = newPaintScheme;
		}

		RadioPtr getRadio (void) {
			return radio;
		}

		void radioMessage (long message, bool propogateIfMultiplayer = false);

		void cancelRadioMessage (long message, bool propogateIfMultiplayer = false);
		
		DWORD getLastMessage(void)
		{
			return radioLog.lastMessage;
		}

		char getRank (void) {
			return(rank);
		}

		long getSkill (long skillId) {
			return(skills[skillId]);
		}

		void setSkill (long skillId, long skillValue)
		{
			if (skillId > 0 && skillId < NUM_SKILLS)
				skills[skillId] = skillValue;
		}

		long checkSkill (long skillId, float factor = 1.0);

		long getNumSkillUse (long skillStat, long skillId) {
			return(numSkillUses[skillId][skillStat]);
		}

		long getNumSkillSuccesses (long skillStat, long skillId) {
			return(numSkillSuccesses[skillId][skillStat]);
		}

		void incNumSkillUses (long skillStat, long skillId) {
			numSkillUses[skillId][skillStat]++;
		}

		void incNumSkillSuccesses (long skillStat, long skillId) {
			numSkillSuccesses[skillId][skillStat]++;
		}

		long getProfessionalism (void) {
			return(professionalism);
		}

		long getProfessionalismModifier (void) {
			return(professionalismModifier);
		}

		long getDecorum (void) {
			return(decorum);
		}

		long getDecorumModifier (void) {
			return(decorumModifier);
		}

		long getAggressiveness (bool current = true);

		long getCourage (void) {
			return(courage);
		}

		void setCourage (unsigned char _courage) {
			courage = _courage;
		}

		long getTeamworkModifier (void) {
			return(professionalismModifier + decorumModifier);
		}

		float getWounds (void) {
			return(wounds);
		}

		bool alive (void) {
			return(wounds < 6.0);
		}

		bool active (void) {
			return(status == WARRIOR_STATUS_NORMAL);
		}

		void setStatus (long _status) {
			status = _status;
		}

		long getStatus (void) {
			return(status);
		}

		bool hasEjected (void) {
			return(status == WARRIOR_STATUS_EJECTED || status == WARRIOR_STATUS_MIA || status == WARRIOR_STATUS_CAPTURED);
		}

		void setEscapesThruEjection (bool escapes) {
			escapesThruEjection = escapes;
		}

		bool getEscapesThruEjection (void) {
			return(escapesThruEjection);
		}

		void setKeepMoving (bool set) {
			keepMoving = set;
		}

		bool getKeepMoving (void) {
			return(keepMoving);
		}

		bool getNotMineYet (void) {
			return notMineYet;
		}
		
		void setNotMineYet (bool result) {
			notMineYet = result;
		}

		bool getSpecialtySkill (long whichSkill) {
			return(specialtySkills[whichSkill]);
		}

		//---------------------------------
		// Specialty Skills		
		bool isLightMechSpecialist (void)			//DONE
		{
			return (specialtySkills[0] == 1);
		}
		
		bool isMediumMechSpecialist (void)			//DONE
		{
			return (specialtySkills[8] == 1);
		}

		bool isLaserSpecialist (void)				//DONE
		{
			return (specialtySkills[1] == 1);
		}

		bool isLightACSpecialist (void)				//DONE
		{
			return (specialtySkills[2] == 1);
		}

		bool isMediumACSpecialist (void)			//DONE
		{
			return (specialtySkills[3] == 1);
		}

		bool isSRMSpecialist (void)					//DONE
		{
			return (specialtySkills[4] == 1);
		}
	
		bool isSmallArmsSpecialist (void)			//DONE
		{
			return (specialtySkills[5] == 1);
		}

		bool isToughnessSpecialist (void)
		{
			return (specialtySkills[7] == 1);
		}

		bool isHevayMechSpecialist (void)			//DONE
		{
			return (specialtySkills[14] == 1);
		}

		bool isPulseLaserSpecialist (void)			//DONE
		{
			return (specialtySkills[9] == 1);
		}

		bool isERLaserSpecialist (void)				//DONE
		{
			return (specialtySkills[10] == 1);
		}

		bool isLRMSpecialist (void)					//DONE
		{
			return (specialtySkills[11] == 1);
		}

		bool isPPCSpecialist (void)					//DONE
		{
			return (specialtySkills[15] == 1);
		}

		bool isScout (void)
		{
			return (specialtySkills[12] == 1);
		}

		bool isLongJump (void)
		{
			return (specialtySkills[13] == 1);
		}

		bool isAssaultMechSpecialist (void)			//DONE
		{
			return (specialtySkills[20] == 1);
		}
	
		bool isGaussSpecialist (void)				//DONE
		{
			return (specialtySkills[21] == 1);
		}

		bool isHeavyACSpecialist (void)				//DONE
		{
			return (specialtySkills[16] == 1);
		}

		bool isShortRangeSpecialist (void)			//DONE
		{
			return (specialtySkills[17] == 1);
		}

		bool isMediumRangeSpecialist (void)			//DONE
		{
			return (specialtySkills[18] == 1);
		}

		bool isLongRangeSpecialist (void)			//DONE
		{
			return (specialtySkills[19] == 1);
		}

		/*			CUT~!!!!!!!!!
		bool isDeadeye (void)
		{
			return (specialtySkills[23] == 1);
		}
		*/

		bool isSharpShooter (void)
		{
			return (specialtySkills[22] == 1);
		}

		bool isSensorProfileSpecialist (void)
		{
			return (specialtySkills[6] == 1);
		}

		//--------------------------
		// TacOrderQueue routines...

		bool enableTacOrderQueue (void);
		void resetQueueMarkers (void) { mc2::resetQueueMarkers(queueMarkers); }	// the one wholesale clear (init, destroy, pool reset)

		long addQueuedTacOrder (TacticalOrder tacOrder);

		long removeQueuedTacOrder (TacticalOrderPtr tacOrder);

		long getNextQueuedTacOrder (TacticalOrderPtr tacOrder);

		long peekQueuedTacOrder (long index, TacticalOrderPtr tacOrder);

		void clearTacOrderQueue (void);

		void setUseGoalPlan (bool set) {
			useGoalPlan = set;
		}

		bool getUseGoalPlan (void) {
			return(useGoalPlan);
		}

		void setMainGoal (long action, GameObjectPtr obj, glm::vec3* location, float range);

		long getMainGoal (GameObjectPtr& obj, glm::vec3& location, float& range);

		long getNumTacOrdersQueued (void) {
			return(numTacOrdersQueued);
		}

		void drawWaypointPath();
		void extractHudPaths(mc2::render_view::HudMover& row);
		void drawOverviewMovePath( unsigned long color );
		bool getMoveDestination( glm::vec3& out );
		void updateDrawWaypointPath();

		void executeTacOrderQueue (void);

		void setExecutingQueue (bool setting) {
			tacOrderQueueExecuting = setting;
		}

		void setTacOrderQueueLooping (bool setting) {
			tacOrderQueueLooping = setting;
		}

		bool getExecutingTacOrderQueue (void) {
			return(tacOrderQueueExecuting);
		}

		bool getTacOrderQueueLooping (void) {
			return(tacOrderQueueLooping);
		}

		void lockTacOrderQueue (void);

		void unlockTacOrderQueue (void);

		bool getTacOrderQueueLocked (void) {
			return(tacOrderQueueLocked);
		}

		long getTacOrderQueue (QueuedTacOrderPtr list);

		bool getCurPlayerOrderFromQueue (void) {
			return(curPlayerOrderFromQueue);
		}

		long getNextTacOrderId (void) {
			return(nextTacOrderId);
		}

		void setNextTacOrderId (long newId) {
			nextTacOrderId = newId;
		}

		long getLastTacOrderId (void) {
			return(lastTacOrderId);
		}

		void setLastTacOrderId (long newId) {
			lastTacOrderId = newId;
		}

		void updateClientOrderQueue (long curTacOrderId);

		//------------------
		// Group routines...

		MoverGroupPtr getGroup (void);

		MoverPtr getPoint (void);

		TeamPtr getTeam (void);

		CommanderPtr getCommander (void);

		char *getAudioString (void)
		{
			return audioStr;
		}
		
		char *getVideoString (void)
		{
			return videoStr;
		}
		
		char *getBrainString (void)
		{
			return brainStr;
		}
		
		int32_t getPilotPhoto (void)
		{
			return photoIndex;
		}
		
		bool onHomeTeam (void);

		bool underHomeCommand (void);

		MoverPtr getVehicle (void);

		GameObjectPtr calcTurretThreats (float threatRange, long minThreat);

		long getVehicleStatus (void);

		long getWeaponsStatus (int* list) {
            MemCpy(list, weaponsStatus);
			return(weaponsStatusResult);
		}

//		void setUnit (MoverGroupPtr _group, long _unitMateId);
		
		void setTeam (TeamPtr team);

		void setVehicle (GameObjectPtr _vehicle);

		void setWounds (float _wounds) {
			wounds = _wounds;
		}

		long setBrain (long brainHandle);

		void setBrainName (const char *brainName);
		
		ABLModulePtr getBrain (void) {
			return(brain);
		}

		long runBrain (void);

		long loadBrainParameters (FitIniFile* brainFile, long warriorId);

		// BRAIN-RUNTIME-1B: allocate brainRuntime if needed and set its mode.
		// Called from mission.cpp after loading _ai.fit Brain block.
		void setBrainRuntimeMode (BrainRuntimeMode fitMode);

		// BRAIN-RUNTIME-1B: direct access for mission loader (public accessor).
		MechBrainRuntime* getBrainRuntime () { return brainRuntime; }
		// Save v2 BRN1 (B2): the task queue, and its lazily created twin used when a restore needs it before the first brain tick.
		BrainTaskQueue* getBrainTaskQueue () { return brainTaskQueue; }
		BrainTaskQueue* ensureBrainTaskQueue () { if (!brainTaskQueue) brainTaskQueue = std::make_unique<BrainTaskQueue>().release(); return brainTaskQueue; }

		bool injure (float numWounds, bool checkEject = true);

		void eject (void);

		float getTimeOfLastOrders (void) {
			return(timeOfLastOrders);
		}

		float getAttackRadius (void) {
			return(attackRadius);
		}

		void setAttackRadius (float radius) {
			attackRadius = radius;
		}

		void setIntegerMemory (long index, long val) {
			memory[index].integer = val;
		}

		void setRealMemory (long index, float val) {
			memory[index].real = val;
		}

		long getIntegerMemory (long index) {
			return(memory[index].integer);
		}

		float getRealMemory (long index) {
			return(memory[index].real);
		}

		void updateAttackerStatus (unsigned long attackerWID, float time);

		AttackerRecPtr getAttackerInfo (unsigned long attackerWID);

		long getAttackers (unsigned int* attackerList, float seconds);

		long scanOwnVehicle (void);

		void lobotomy (void);		//Whacks the ABL brain and just blindly follows last order.

		//-----------
		// Tac Orders

		TacticalOrderPtr getCurTacOrder (void) {
			return(&curTacOrder);
		}

		TacticalOrderPtr getLastTacOrder (void) {
			return(&lastTacOrder);
		}

		TacticalOrderPtr getTacOrder (long _orderState) {
			return(&tacOrder[_orderState]);
		}

		long getOrderState (void) {
			return(orderState);
		}

		void setOrderState (long state) {
			orderState = state;
		}

		//------------
		// Move Orders

		MoveOrders* getMoveOrders (void) {
			return(&moveOrders);
		}

		void setMoveOrigin (long origin) {
			moveOrders.origin = origin;
		}

		long getMoveOrigin (void) {
			return(moveOrders.origin);
		}

		void setMoveSpeedType (long type);

		long getMoveSpeedType (void) {
			return(moveOrders.speedType);
		}

		void setMoveSpeedVelocity (float speed);

		float getMoveSpeedVelocity (void) {
			return(moveOrders.speedVelocity);
		}

		long getMoveSpeedState (void) {
			return(moveOrders.speedState);
		}

		long getMoveSpeedThrottle (void) {
			return(moveOrders.speedThrottle);
		}

		long setMoveGoal (unsigned long type, glm::vec3* location, GameObjectPtr obj = nullptr);

		unsigned long getMoveGoal (glm::vec3* location = nullptr, GameObjectPtr* obj = nullptr);

		bool getMoveGlobalGoal (glm::vec3& location) {
			if (moveOrders.pathType != MOVEPATH_UNDEFINED) {
				location = moveOrders.originalGlobalGoal[1];
				return(true);
			}
			return(false);
		}

		void clearMoveGoal (void) {
			moveOrders.origin = ORDER_ORIGIN_COMMANDER;
			moveOrders.goalType = MOVEGOAL_NONE;
			moveOrders.goalObjectWID = 0;
			moveOrders.goalLocation.x = -999999.0;
			moveOrders.goalLocation.y = -999999.0;
			moveOrders.goalLocation.z = -999999.0;
		}

		void clearMovePath (long pathNum); /* {
			if (moveOrders.path[pathNum])
				moveOrders.path[pathNum]->clear();
		}*/

		bool hasMoveGoal (void) {
			return(moveOrders.goalType != MOVEGOAL_NONE);
		}

		bool getMoveNewGoal (void) {
			return(moveOrders.newGoal);
		}

		void setMoveNewGoal (bool state) {
			moveOrders.newGoal = state;
		}
		
		MovePathPtr getMovePath (long which) {
			return(moveOrders.path[which]);
		}

		MovePathPtr getMovePath (void);

		void setMoveGoalObjectPos (glm::vec3 pos) {
			moveOrders.goalObjectPosition = pos;
		}

		glm::vec3 getMoveGoalObjectPos (void) {
			return(moveOrders.goalObjectPosition);
		}

		void setMoveRun (bool run) {
			moveOrders.run = run;
		}

		bool getMoveRun (void) {
			return(moveOrders.run);
		}

		void setMovePathRequest (PathQueueRecPtr rec) {
			movePathRequest = rec;
		}

		PathQueueRecPtr getMovePathRequest (void) {
			return(movePathRequest);
		}

		void pausePath (void);

		void resumePath (void);

		void rethinkPath (unsigned long strategy);

		void setMoveState (long state) {
			moveOrders.moveState = state;
		}

		long getMoveState (void) {
			return(moveOrders.moveState);
		}

		void setMoveTimeOfLastStep (float t) {
			moveOrders.timeOfLastStep = t;
		}

		long getMoveTimeOfLastStep (void) {
			return(moveOrders.timeOfLastStep);
		}

		void setMoveStateGoal (long state) {
			moveOrders.moveStateGoal = state;
		}

		long getMoveStateGoal (void) {
			return(moveOrders.moveStateGoal);
		}

		void setMoveTwisting (bool set) {
			moveOrders.twisting = set;
		}

		bool getMoveTwisting (void) {
			return(moveOrders.twisting);
		}

		void setMoveYieldTime (float time) {
			moveOrders.yieldTime = time;
		}

		void setMoveYieldState (long state) {
			moveOrders.yieldState = state;
		}

		float getMoveYieldTime (void) {
			return(moveOrders.yieldTime);
		}

		long getMoveYieldState (void) {
			return(moveOrders.yieldState);
		}

		bool isYielding (void) {
			return(moveOrders.yieldTime > -1.0);
		}

		void setMoveWaitForPointTime (float time) {
			moveOrders.waitForPointTime = time;
		}

		float getMoveWaitForPointTime (void) {
			return(moveOrders.waitForPointTime);
		}

		bool isWaitingForPoint (void) {
			return(moveOrders.waitForPointTime > -1.0);
		}

		float getMoveDistanceLeft (void);

		bool isJumping (glm::vec3* jumpGoal = nullptr);

		void setWillHelp (bool setting) {
			willHelp = setting;
		}

		bool getWillHelp (void) {
			return(willHelp);
		}

		long getMovePathType (void) {
			return(moveOrders.pathType);
		}

		void setMoveWayPath (WayPathPtr wayPath, bool patrol = false);

		void addMoveWayPoint (glm::vec3 wayPt, bool patrol = false);

		void setMoveGlobalPath (GlobalPathStepPtr path, long numSteps);

		long getMoveNumGlobalSteps (void) {
			return(moveOrders.numGlobalSteps);
		}

		long getMoveCurGlobalStep (void) {
			return(moveOrders.curGlobalStep);
		}

		long getMovePathGlobalStep (void) {
			return(moveOrders.path[0]->globalStep);
		}

		void requestMovePath (long selectionIndex, unsigned long moveParams, long source);

		// PATHFINDING-JUMP-FAIL-1: stamp a doomed jump-solve failure (goal cell of
		// the solve that returned no path). Called from Mover::calcMovePath on the
		// jump branch. No-op unless the backoff gate is on.
		void noteJumpPathFailed (void);

		long calcMovePath (long selectionIndex, unsigned long moveParams = MOVEPARAM_NONE);

		long calcMoveSpeedState (void) {
			//-------------------------------------------------
			// Assumes we want to go as fast as orders allow...
			return(moveOrders.speedState);
		}

		long calcMoveSpeedThrottle (long speedState, long speedThrottle) {
			//----------------------------------------------------------
			// Always Assume 100 percent in MC2 for now  --fs
			return(100);
		}

		bool getNextWayPoint (glm::vec3& nextPoint, bool incWayPoint);

		//--------------
		// Attack Orders

		void setAttackOrigin (long origin) {
			attackOrders.origin = origin;
		}

		long getAttackOrigin (void) {
			return(attackOrders.origin);
		}

		void setAttackType (long type) {
			attackOrders.type = type;
		}

		long getAttackType (void) {
			return(attackOrders.type);
		}

		void changeAttackRange( long newRange );

		long setAttackTarget (GameObjectPtr object);

		GameObjectPtr getLastTarget (void);

		void setLastTarget (GameObjectPtr target, bool obliterate = false, bool conserveAmmo = 0);

		float getLastTargetTime (void) {
			return(lastTargetTime);
		}

		GameObjectPtr getAttackTarget (void);

		GameObjectPtr getCurrentTarget (void) {
			return(getLastTarget());
		}

		void setCurrentTarget (GameObjectPtr target);

		void setAttackPursuit (bool pursue) {
			attackOrders.pursue = pursue;
		}

		bool getAttackPursuit (void) {
			return(attackOrders.pursue);
		}

		void setAttackAimLocation (long location) {
			attackOrders.aimLocation = location;
		}

		long getAttackAimLocation (void) {
			return(attackOrders.aimLocation);
		}

		void setAttackTargetPoint (glm::vec3 location) {
			attackOrders.targetPoint = location;
		}

		glm::vec3 getAttackTargetPoint (void) {
			return(attackOrders.targetPoint);
		}

		float getAttackTargetTime (void) {
			return(attackOrders.targetTime);
		}

		GameObjectPtr getAttackTargetPosition (glm::vec3& pos);

		void clearAttackOrders (void);

		void clearMoveOrders (void);

		//-----------------
		// Situation Orders

		long getSituationMode (void) {
			return(situationOrders.mode);
		}

		void setSituationMode (long mode) {
			situationOrders.mode = mode;
		}

		float getSituationDefFormation (void) {
			return(situationOrders.defFormation);
		}

		void setSituationDefFormation (float distance) {
			situationOrders.defFormation = distance;
		}

		float getSituationCurFormation (void) {
			return(situationOrders.curFormation);
		}

		void setSituationCurFormation (float distance) {
			situationOrders.curFormation = distance;
		}

		bool getSituationOpenFire (void) {
			return(situationOrders.openFire);
		}

		void setSituationOpenFire (bool openFire) {
			situationOrders.openFire = openFire;
		}

		float getSituationFireRange (void) {
			return(situationOrders.fireRange);
		}

		void setSituationFireRange (float range) {
			situationOrders.fireRange = range;
		}

		float getSituationFireOdds (void) {
			return(situationOrders.fireOdds);
		}

		void setSituationFireOdds (float odds) {
			situationOrders.fireOdds = odds;
		}

		GameObjectPtr getSituationGuardObject (void);

		void setSituationGuardObject (GameObjectWatchID objWID);

		unsigned long getSituationGuardObjectPartId (void);
		
		void setMessagePlaying()
		{
			isPlayingMsg = true;
		}
		
		void clearMessagePlaying()
		{
			isPlayingMsg = false;
		}
		
		bool getMessagePlaying()
		{
			return isPlayingMsg;
		}

#if 0
		//-------------
		// Role Orders

		long getRole (void) {
			return(roleOrders.role);
		}

		void setRole (long role) {
			roleOrders.role = static_cast<MechWarriorRole>(role);
		}
#endif

		//--------------------
		// Command/Observation
		long executeTacticalOrder (TacticalOrderPtr order = nullptr);

		long calcWeaponsStatus (GameObjectPtr target, int* weaponList, glm::vec3* targetPoint = nullptr);

		void printWeaponsStatus (char* s);

		long combatDecisionTree (void);

		glm::vec3 calcWithdrawGoal (float withdrawRange = 1000.0);

		bool movingOverBlownBridge (void);

		bool movementDecisionTree (void);

		void collisionAlert (GameObjectPtr obstacle, float distance, float timeToImpact);

		long triggerAlarm (long alarmCode, unsigned long triggerId = 0);

		long handleAlarm (long alarmCode, unsigned long triggerId = 0);

		int32_t getAlarmTriggers (int32_t alarmCode, int32_t* triggerList);

		int32_t getAlarmTriggersHistory (int32_t alarmCode, int32_t* triggerList);

		void clearAlarm (long alarmCode);

		void clearAlarms (void);

		void clearAlarmHistory (long alarmCode);

		void clearAlarmsHistory (void);

		void clearCurTacOrder (bool updateTacOrder = true);

		void setCurTacOrder (TacticalOrder tacOrder);

		void setGeneralTacOrder (TacticalOrder tacOrder);
		
		void setPlayerTacOrder (TacticalOrder tacOrder, bool fromQueue = false);

		void setAlarmTacOrder (TacticalOrder tacOrder, long priority);

		long checkCommandDecision (void);

		long checkAlarms (void);

		void updateActions (void);

		long mainDecisionTree (void);

		void setDebugFlags (unsigned long flags) {
			debugFlags = flags;
		}

		void setDebugFlag (unsigned long flag, bool on);

		bool getDebugFlag (unsigned long flag);

		void debugPrint (char* s, bool debugMode = false);

		void debugOrders (void);

		void initTargetPriorities (void);

		long setTargetPriority (long index, long type, long param1, long param2, long param3);

		long insertTargetPriority (long index, long type, long param1, long param2, long param3);

		long setBrainState (long newState);

		long getBrainState (void);

		long getEventHistory (long alarmCode, int* paramList);

		struct EventHistoryResult { long eventID = -1; long count = 0; };
		EventHistoryResult getNextEventHistory (int* paramList);

		//--------------
		// CORE COMMANDS

		long getCoreScanTargetWID (void) {
			return(coreScanTargetWID);
		}

		long coreMoveTo (glm::vec3 location, unsigned long params);

		long coreMoveToObject (GameObjectPtr object, unsigned long params);

		long coreEject (void);

		long corePower (bool powerUp);

		long coreAttack (GameObjectPtr target, unsigned long params);

		long coreCapture (GameObjectPtr object, unsigned long params);

		long coreScan (GameObjectPtr object, unsigned long params);

		long coreControl (GameObjectPtr object, unsigned long params);

		//long coreWithdraw (Stuff::Vector3D location, unsigned long params);

		long coreSetState (long stateID, bool thinkAgain);

		//----------------
		// Core Tac Orders

		long orderWait (bool unitOrder, long origin, long seconds = 1969000.0, bool clearLastTarget = false);

		long orderStop (bool unitOrder, bool setTacOrder);

		long orderMoveToPoint (bool unitOrder, bool setTacOrder, long origin, glm::vec3 location, long selectionIndex = -1, unsigned long params = TACORDER_PARAM_NONE);

		long orderFormation (bool unitOrder, bool setTacOrder, long origin);

		long orderMoveToObject (bool unitOrder, bool setTacOrder, long origin, GameObjectPtr target, long fromArea, long selectionIndex = -1, unsigned long params = TACORDER_PARAM_FACE_OBJECT);

		long orderJumpToPoint (bool unitOrder, bool setTacOrder, long origin, glm::vec3 location, long selectionIndex = -1);

		long orderJumpToObject (bool unitOrder, bool setTacOrder, long origin, GameObjectPtr target, long selectionIndex = -1);

		long orderTraversePath (bool unitOrder, bool setTacOrder, long origin, WayPathPtr wayPath, unsigned long params = TACORDER_PARAM_NONE);
		
		long orderPatrolPath (bool unitOrder, bool setTacOrder, long origin, WayPathPtr wayPath);

		long orderPowerUp (bool unitOrder, long origin);

		long orderPowerDown (bool unitOrder, long origin);

		long orderUseSpeed (float speed);

		long orderUsePattern (long pattern);

		long orderOrbitPoint (glm::vec3 location);

		long orderOrbitObject (GameObjectPtr target);

		long orderUseOrbitRange (long type, float range);

		long orderAttackObject (bool unitOrder, long origin, GameObjectPtr target, long type, long method, long range, long aimLocation = -1, long fromArea = -1, unsigned long params = TACORDER_PARAM_NONE);

		long orderAttackPoint (bool unitOrder, long origin, glm::vec3 location, long type, long method, long range, unsigned long params = TACORDER_PARAM_NONE);

		long orderWithdraw (bool unitOrder, long origin, glm::vec3 location);

		long orderEject (bool unitOrder, bool setTacOrder, long origin);

		//long orderUseFireOdds (long odds);

		long orderRefit (long origin, GameObjectPtr target, unsigned long params = TACORDER_PARAM_NONE);

		long orderRecover (long origin, GameObjectPtr target, unsigned long params = TACORDER_PARAM_NONE);

		long orderGetFixed (long origin, GameObjectPtr target, unsigned long params = TACORDER_PARAM_NONE);

		long orderLoadIntoCarrier (long origin, GameObjectPtr target, unsigned long params = TACORDER_PARAM_NONE);

		long orderDeployElementals (long origin, unsigned long params = TACORDER_PARAM_NONE);

		long orderCapture (long origin, GameObjectPtr target, long fromArea = -1, unsigned long params = TACORDER_PARAM_NONE);

		//--------------
		// Combat Events

		long handleTargetOfWeaponFire (void);

		long handleHitByWeaponFire (void);

		long handleDamageTakenRate (void);

		long handleFriendlyVehicleCrippled (void);

		long handleFriendlyVehicleDestruction (void);

		long handleOwnVehicleIncapacitation (unsigned long cause);

		long handleOwnVehicleDestruction (unsigned long cause);

		long handleOwnVehicleWithdrawn (void);

		long handleAttackOrder (void);

		long handleCollisionAlert (void);

		long handleCollision (void);

		long handleKilledTarget (void);

		long handleUnitMateFiredWeapon (void);

		long handleUnitMateDeath (void);

		long handlePlayerOrder (void);

		long handleNoMovePath (void);

		long handleGateClosing (void);

		long handleFiredWeapon (void);

		long handleNewMover (void);

		//---------------
		// Status Windows
		long openStatusWindow (long x, long y, long w, long h);

		long closeStatusWindow (void);

		long missionLog (FilePtr file, long unitLevel);

		void calcRank (void);

		bool isCloseToFirstTacOrder( glm::vec3& pos );

		std::unique_ptr<BldgAppearance> getWayPointMarker( const glm::vec3& pos, const char* name );


		void setDebugString (long stringNum, char* s) {
			if ((stringNum > -1) && (stringNum < NUM_PILOT_DEBUG_STRINGS)) {
				if (s == nullptr) {
					debugStrings[stringNum][0] = '\0';
                } else {
					strncpy(debugStrings[stringNum], s, MAXLEN_PILOT_DEBUG_STRING - 1);
                }
            }
		}

		char* getDebugString (long stringNum) 
		{
			if ((stringNum > -1) && (stringNum < NUM_PILOT_DEBUG_STRINGS))
				return(debugStrings[stringNum]);
			return(nullptr);
		}

		long calcTacOrder (long goalAction,
						   long goalWID,
						   glm::vec3 goalLocation,
						   float controlRange,
						   long aggressiveness,
						   long searchDepth,
						   float turretRange,
						   long turretThreat,
						   TacticalOrder& tacOrder);

		static void initGoalManager (long poolSize);

		static void logPilots (GameLogPtr log);
		
		static bool anyPlayerInCombat (void);

		void copyToData (MechWarriorData &data);
		void copyFromData (MechWarriorData &data, bool wideTargets = false);

		void save (PacketFilePtr file, long packetNum); 
		void load (PacketFilePtr file, long packetNum, bool values = false);

		long static Save (PacketFilePtr file, long packetNum);
		long static Load (PacketFilePtr file, long packetNum, bool staticValues = false, bool values = false);
};

//---------------------------------------------------------------------------

extern MovePathManagerPtr	PathManager;

long RandomNumber (long range);
bool RollDice (long percent);
long SignedRandomNumber (long range);
long GetMissionTurn (void);

//***************************************************************************

#endif
