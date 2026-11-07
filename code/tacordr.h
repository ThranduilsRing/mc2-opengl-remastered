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

#include <cstdint>
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

// LG14 lane 3: the order enums below are saved (TacticalOrder), packed into order words (TacticalOrder::pack) and read from ABL by number.
// Every value explicit and pinned in save_layout_guard.h. AttackMethod, TacticalOrderCode and SpecialMoveMode are scoped so an int
// cannot slip in unchecked; FireRangeType, AttackType, TacticType, OrderOriginType and _TravelModeType stay unscoped for now
// (20+ int-mixing files each; next LG14 commit). Inbound ints for all of them go through enum_in_domain(E{}, v) below
// (per-enum domain: fire range starts at -4), then static_cast. The save codec finds these by ADL.
enum FireRangeType {
	FIRERANGE_DEFAULT = -4,
	FIRERANGE_RAMMING = -3,
	FIRERANGE_LONGEST = -2,
	FIRERANGE_OPTIMAL = -1,
	FIRERANGE_SHORT = 0,
	FIRERANGE_MEDIUM = 1,
	FIRERANGE_LONG = 2,
	FIRERANGE_CURRENT = 3,
	NUM_FIRERANGES = 4
};
constexpr bool enum_in_domain(FireRangeType, int32_t v) { return v >= -4 && v <= 3; }

enum AttackType {
	ATTACK_NONE = 0,
	ATTACK_TO_DESTROY = 1,
	ATTACK_TO_DISABLE = 2,
	ATTACK_CONSERVING_AMMO = 3,
	NUM_ATTACK_TYPES = 4
};
constexpr bool enum_in_domain(AttackType, int32_t v) { return v >= 0 && v < 4; }

enum class AttackMethod : int32_t {
	ATTACKMETHOD_RANGED = 0,
	ATTACKMETHOD_DFA = 1,
	ATTACKMETHOD_RAMMING = 2,
	NUM_ATTACKMETHODS = 3
};
using enum AttackMethod;
constexpr bool enum_in_domain(AttackMethod, int32_t v) { return v >= 0 && v < 3; }

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
	TACTIC_NONE = 0,
	TACTIC_FLANK_LEFT = 1,
	TACTIC_FLANK_RIGHT = 2,
	TACTIC_FLANK_REAR = 3,
	TACTIC_STOP_AND_FIRE = 4,
	TACTIC_TURRET = 5,
	TACTIC_JOUST = 6,
	// TACTIC-WEIGHTS-A: GDD tactic additions (append; existing numeric values unchanged).
	TACTIC_INDIRECT_FIRE = 7,     // stand-off indirect suppression
	TACTIC_HULL_DOWN = 8,         // cover-crest discipline
	TACTIC_FIGHTING_WITHDRAW = 9, // deliberate fighting withdrawal
	TACTIC_PURSUE = 10,           // aggressive pursuit of fleeing targets
	TACTIC_HIT_AND_RUN = 11,      // harass-then-disengage
	NUM_TACTICS = 12
};
constexpr bool enum_in_domain(TacticType, int32_t v) { return v >= 0 && v < 12; }

enum OrderOriginType {
	ORDER_ORIGIN_PLAYER = 0,
	ORDER_ORIGIN_COMMANDER = 1,
	ORDER_ORIGIN_SELF = 2
};
constexpr bool enum_in_domain(OrderOriginType, int32_t v) { return v >= 0 && v <= 2; }

enum class TacticalOrderCode : int32_t {
	TACTICAL_ORDER_NONE = 0,
	TACTICAL_ORDER_WAIT = 1,
	TACTICAL_ORDER_MOVETO_POINT = 2,
	TACTICAL_ORDER_MOVETO_OBJECT = 3,
	TACTICAL_ORDER_JUMPTO_POINT = 4,
	TACTICAL_ORDER_JUMPTO_OBJECT = 5,
	TACTICAL_ORDER_TRAVERSE_PATH = 6,
	TACTICAL_ORDER_PATROL_PATH = 7,
	TACTICAL_ORDER_ESCORT = 8,
	TACTICAL_ORDER_FOLLOW = 9,
	TACTICAL_ORDER_GUARD = 10,
	TACTICAL_ORDER_STOP = 11,
	TACTICAL_ORDER_POWERUP = 12,
	TACTICAL_ORDER_POWERDOWN = 13,
	TACTICAL_ORDER_WAYPOINTS_DONE = 14,
	TACTICAL_ORDER_EJECT = 15,
	TACTICAL_ORDER_ATTACK_OBJECT = 16,
	TACTICAL_ORDER_ATTACK_POINT = 17,
	TACTICAL_ORDER_HOLD_FIRE = 18,
	TACTICAL_ORDER_WITHDRAW = 19,
	TACTICAL_ORDER_SCRAMBLE = 20,
	TACTICAL_ORDER_CAPTURE = 21,
	TACTICAL_ORDER_REFIT = 22,
	TACTICAL_ORDER_GETFIXED = 23,
	TACTICAL_ORDER_LOAD_INTO_CARRIER = 24,
	TACTICAL_ORDER_DEPLOY_ELEMENTALS = 25,
	TACTICAL_ORDER_RECOVER = 26,
	NUM_TACTICAL_ORDERS = 27	// IF THIS CHANGES, ADD GUARD_POINT AS SEP. ORDER (see pack/unpack hack)
};
using enum TacticalOrderCode;
constexpr bool enum_in_domain(TacticalOrderCode, int32_t v) { return v >= 0 && v < 27; }

//***************************************************************************

struct LocationNode {
	glm::vec3	location;
	bool			run;
	LocationNodePtr	next;
};

enum _TravelModeType {
	TRAVEL_MODE_INVALID = -1,
	TRAVEL_MODE_SLOW = 0,
	TRAVEL_MODE_FAST = 1,
	TRAVEL_MODE_JUMP = 2,
	NUM_TRAVEL_MODES = 3
}; using TravelModeType = _TravelModeType;

struct _WayPath {
	long			numPoints;
	long			curPoint;
	float			points[3 * MAX_WAYPTS];
	unsigned char	mode[MAX_WAYPTS];
}; using WayPath = _WayPath;

using WayPathPtr = WayPath*;

enum class SpecialMoveMode : int32_t {
	MOVE_MODE_NORMAL = 0,
	MOVE_MODE_MINELAYING = 1
};
using enum SpecialMoveMode;
constexpr bool enum_in_domain(SpecialMoveMode, int32_t v) { return v >= 0 && v <= 1; }

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
