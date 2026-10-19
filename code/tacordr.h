//******************************************************************************************
//
//	tacordr.h - This file contains the Tactical Orders header
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef TACORDR_H
#define TACORDR_H

#include <utility>

//--------------
// Include Files

#ifndef GAMEOBJ_H
#include"gameobj.h"
#endif

#ifndef DGROUP_H
#include"dgroup.h"
#endif

#ifndef DWARRIOR_H
#include"dwarrior.h"
#endif

#ifndef DMOVER_H
#include"dmover.h"
#endif

#ifndef DTACORDR_H
#include"dtacordr.h"
#endif

//***************************************************************************

inline constexpr int NUM_MOVE_SPEEDS = 4;

inline constexpr int TACORDER_FAILURE = 0;
inline constexpr int TACORDER_SUCCESS = 1;

enum class MoveSpeedType : int {
	MOVE_SPEED_BEST,
	MOVE_SPEED_MAXIMUM,
	MOVE_SPEED_SLOW,
	MOVE_SPEED_MODERATE
};
using enum MoveSpeedType;

inline constexpr int NUM_MOVE_PATTERNS = 7;

enum class MovePatternType : int {
	MOVE_PATTERN_DIRECT,
	MOVE_PATTERN_ZIGZAG_NARROW,
	MOVE_PATTERN_ZIGZAG_MEDIUM,
	MOVE_PATTERN_ZIGZAG_WIDE,
	MOVE_PATTERN_RANDOM_NARROW,
	MOVE_PATTERN_RANDOM_MEDIUM,
	MOVE_PATTERN_RANDOM_WIDE
};
using enum MovePatternType;

inline constexpr int NUM_ATTITUDES = 6;

enum class AttitudeType : int {
	ATTITUDE_CAUTIOUS,
	ATTITUDE_CONSERVATIVE,
	ATTITUDE_NORMAL,
	ATTITUDE_AGGRESSIVE,
	ATTITUDE_BERSERKER,
	ATTITUDE_SUICIDAL
};
using enum AttitudeType;

//#define	FIRERANGE_OPTIMUM	-1

enum FireRangeType {
	FIRERANGE_DEFAULT = -4,
	FIRERANGE_RAMMING,
	FIRERANGE_LONGEST,
	FIRERANGE_OPTIMAL,
	FIRERANGE_SHORT,
	FIRERANGE_MEDIUM,
	FIRERANGE_LONG,
	FIRERANGE_CURRENT,
	NUM_FIRERANGES
};

enum AttackType {
	ATTACK_NONE,
	ATTACK_TO_DESTROY,
	ATTACK_TO_DISABLE,
	ATTACK_CONSERVING_AMMO,
	NUM_ATTACK_TYPES
};

enum AttackMethod {
	ATTACKMETHOD_RANGED,
	ATTACKMETHOD_DFA,
	ATTACKMETHOD_RAMMING,
	NUM_ATTACKMETHODS
};

enum class FireOddsType : int {
	FIREODDS_LUCKY,
	FIREODDS_BAD,
	FIREODDS_MEDIUM,
	FIREODDS_GOOD,
	FIREODDS_GREAT,
	NUM_FIREODDS
};
inline constexpr int NUM_FIREODDS = std::to_underlying(FireOddsType::NUM_FIREODDS);

enum TacticType {
	TACTIC_NONE,
	TACTIC_FLANK_LEFT,
	TACTIC_FLANK_RIGHT,
	TACTIC_FLANK_REAR,
	TACTIC_STOP_AND_FIRE,
	TACTIC_TURRET,
	TACTIC_JOUST,
	// TACTIC-WEIGHTS-A: GDD tactic additions (append; existing numeric values unchanged).
	TACTIC_INDIRECT_FIRE,     // 7  — stand-off indirect suppression
	TACTIC_HULL_DOWN,         // 8  — cover-crest discipline
	TACTIC_FIGHTING_WITHDRAW, // 9  — deliberate fighting withdrawal
	TACTIC_PURSUE,            // 10 — aggressive pursuit of fleeing targets
	TACTIC_HIT_AND_RUN,       // 11 — harass-then-disengage
	NUM_TACTICS               // 12
};

enum OrderOriginType {
	ORDER_ORIGIN_PLAYER,
	ORDER_ORIGIN_COMMANDER,
	ORDER_ORIGIN_SELF
};

enum TacticalOrderCode {
	TACTICAL_ORDER_NONE,
	TACTICAL_ORDER_WAIT,
	TACTICAL_ORDER_MOVETO_POINT,
	TACTICAL_ORDER_MOVETO_OBJECT,
	TACTICAL_ORDER_JUMPTO_POINT,
	TACTICAL_ORDER_JUMPTO_OBJECT,
	TACTICAL_ORDER_TRAVERSE_PATH,
	TACTICAL_ORDER_PATROL_PATH,
	TACTICAL_ORDER_ESCORT,
	TACTICAL_ORDER_FOLLOW,
	TACTICAL_ORDER_GUARD,
	TACTICAL_ORDER_STOP,
	TACTICAL_ORDER_POWERUP,
	TACTICAL_ORDER_POWERDOWN,
	TACTICAL_ORDER_WAYPOINTS_DONE,
	TACTICAL_ORDER_EJECT,
	TACTICAL_ORDER_ATTACK_OBJECT,
	TACTICAL_ORDER_ATTACK_POINT,
	TACTICAL_ORDER_HOLD_FIRE,
	TACTICAL_ORDER_WITHDRAW,
	TACTICAL_ORDER_SCRAMBLE,
	TACTICAL_ORDER_CAPTURE,
	TACTICAL_ORDER_REFIT,
	TACTICAL_ORDER_GETFIXED,
	TACTICAL_ORDER_LOAD_INTO_CARRIER,
	TACTICAL_ORDER_DEPLOY_ELEMENTALS,
	TACTICAL_ORDER_RECOVER,
	NUM_TACTICAL_ORDERS	// IF THIS CHANGES, ADD GUARD_POINT AS SEP. ORDER (see pack/unpack hack)
};

//***************************************************************************

struct LocationNode {
	glm::vec3	location;
	bool			run;
	LocationNodePtr	next;
};

enum _TravelModeType {
	TRAVEL_MODE_INVALID = -1,
	TRAVEL_MODE_SLOW,
	TRAVEL_MODE_FAST,
	TRAVEL_MODE_JUMP,
	NUM_TRAVEL_MODES
}; using TravelModeType = _TravelModeType;

struct _WayPath {
	long			numPoints;
	long			curPoint;
	float			points[3 * MAX_WAYPTS];
	unsigned char	mode[MAX_WAYPTS];
}; using WayPath = _WayPath;

using WayPathPtr = WayPath*;

enum SpecialMoveMode {
	MOVE_MODE_NORMAL,
	MOVE_MODE_MINELAYING
};

struct _TacOrderMoveParams {
	WayPath			wayPath;
	bool			faceObject;
	bool			wait;
	SpecialMoveMode	mode;
	bool			escapeTile;
	bool			jump;
	long			fromArea;
	bool			keepMoving;
}; using TacOrderMoveParams = _TacOrderMoveParams;

struct _TacOrderAttackParams {
	AttackType		type; //NO LONGER USED! 1/7/00
	AttackMethod	method;
	FireRangeType	range;
	TacticType		tactic;
	long			aimLocation;
	bool			pursue;
	bool			obliterate;
	glm::vec3	targetPoint;
}; using TacOrderAttackParams = _TacOrderAttackParams;

class TacticalOrder {

	public:

		long					id;
		float					time;
		float					delayedTime;
		float					lastTime;
		bool					unitOrder;		// TRUE if unit order, else individual order
		bool					subOrder;
		OrderOriginType			origin;
		TacticalOrderCode		code;
		TacOrderMoveParams		moveParams;
		TacOrderAttackParams	attackParams;
		GameObjectWatchID		targetWID;
		long					targetObjectClass;
		long					selectionIndex;
		char					stage;
		char					statusCode;
		glm::vec3			lastMoveGoal;

		// for network use only
		char					pointLocalMoverId;
		unsigned long			groupFlags;
		unsigned long			data[2];

	public:


		void operator = (TacticalOrder copy) {
			time = copy.time;
			delayedTime = copy.delayedTime;
			lastTime = copy.lastTime;
			unitOrder = copy.unitOrder;
			origin = copy.origin;
			code = copy.code;
			moveParams = copy.moveParams;
			attackParams = copy.attackParams;
			targetWID = copy.targetWID;
			targetObjectClass = copy.targetObjectClass;
			selectionIndex = copy.selectionIndex;
			stage = copy.stage;
			subOrder = copy.subOrder;
			lastMoveGoal.x = -99999.0;
			statusCode = 0;
			pointLocalMoverId = copy.pointLocalMoverId;
			groupFlags = copy.groupFlags;
			data[0] = copy.data[0];
			data[1] = copy.data[1];
		}
		
		void init (void);

		void init (OrderOriginType _origin, TacticalOrderCode _code, bool _unitOrder = false);

		void initWayPath (LocationNodePtr path);
		
		void initAttackWayPath (LocationNodePtr path);
		
		void destroy (void);

		TacticalOrder (void) {
			init();
		}

		~TacticalOrder (void)	{
			destroy();
		}

		void setId (long newId) {
			id = newId;
		}

		void setId (MechWarriorPtr pilot);

		long getId (void) {
			return(id);
		}

		long execute (MechWarriorPtr warrior, long& message);

		long status (MechWarriorPtr warrior);

		glm::vec3 getWayPoint (long index);

		void setWayPoint (long index, glm::vec3 wayPoint);

		void addWayPoint (glm::vec3 wayPoint, long travelMode);

		GameObjectPtr getRamTarget (void);

		GameObjectPtr getJumpTarget (void);

		bool isGroupOrder (void);

		bool isCombatOrder (void);

		bool isMoveOrder (void);

		bool isJumpOrder (void);

		bool isWayPathOrder (void);

		long getParamData (float* time, int* paramList);

		long pack (MoverGroupPtr unit, MoverPtr point);

		long unpack (void);

		void setGroupFlag (long localMoverId, bool set);

		long getGroup (long commanderID, MoverPtr* moverList, MoverPtr* point, long sortType = 0);

		void setStage (long newStage) {
			stage = newStage;
		}

		GameObjectPtr getTarget (void);

		bool equals (TacticalOrder* tacOrder);

		char getStatusCode (void) {
			return(statusCode);
		}

		void debugString (MechWarriorPtr pilot, char* s);

};

//***************************************************************************

#endif
