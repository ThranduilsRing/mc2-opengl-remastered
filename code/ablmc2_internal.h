// Shared surface of the ABL native-binding translation units (ablmc2*.cpp).
//
// ablmc2.cpp owns the globals, initABL and the ONE ordered ABLi_addFunction list: registration order is
// state (enterStandardRoutine assigns each routine its key positionally into a fixed table), so the list
// never moves next to the functions. The exec* bindings live in ablmc2_<domain>.cpp.
#ifndef ABLMC2_INTERNAL_H
#define ABLMC2_INTERNAL_H

#ifndef ABL_H
#include"abl.h"
#endif

#ifndef MCLIB_H
#include"mclib.h"
#endif

#include "gos_crashbundle.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1

#ifndef MISSION_H
#include"mission.h"
#endif

#ifndef MOVE_H
#include"move.h"
#endif

#ifndef MOVEMGR_H
#include"movemgr.h"
#endif

#ifndef GAMESOUND_H
#include"gamesound.h"
#endif

#ifndef SOUNDS_H
#include"sounds.h"
#endif

#ifndef COLLSN_H
#include"collsn.h"
#endif

#ifndef CMPONENT_H
#include"cmponent.h"
#endif

#ifndef OBJMGR_H
#include"objmgr.h"
#endif

#ifndef MOVER_H
#include"mover.h"
#endif

#ifndef MECH_H
#include"mech.h"
#endif

#ifndef GVEHICL_H
#include"gvehicl.h"
#endif

#ifndef CONTACT_H
#include"contact.h"
#endif

#ifndef TEAM_H
#include"team.h"
#endif

#ifndef COMNDR_H
#include"comndr.h"
#endif

#ifndef GROUP_H
#include"group.h"
#endif

#ifndef GAMECAM_H
#include"gamecam.h"
#endif

#ifndef MULTPLYR_H
#include"multplyr.h"
#endif

#ifndef WEAPONFX_H
#include"weaponfx.h"
#endif

#ifndef TURRET_H
#include"turret.h"
#endif

#ifndef GATE_H
#include"gate.h"
#endif

#ifndef ARTLRY_H
#include"artlry.h"
#endif

#ifndef GAMELOG_H
#include"gamelog.h"
#endif

#ifndef MISSIONBEGIN_H
#include"missionbegin.h"
#endif

#ifndef LOGISTICS_H
#include"logistics.h"
#endif


// ABL-OPCODE-BOUNDS-HARDEN gates and the ABL exec context (defined in mclib/ablxstd.cpp / ablexec.cpp).
extern bool s_ablArgGuard;
extern bool s_ablRuntimeSoftfail;
extern int         execLineNumber;
extern ABLModulePtr CurModule;
extern int32_t     FileNumber;

// Globals defined in ablmc2.cpp.
extern MoverGroupPtr	CurGroup;
extern GameObjectPtr	CurObject;
extern long	CurObjectClass;
extern MechWarriorPtr	CurWarrior;
extern GameObjectPtr	CurContact;
extern long	CurAlarm;
extern MoverPtr	moverList[256];
extern bool	TacOrderOrigin;
extern long	CurMultiplayCode;
extern long	CurMultiplayParam;

// Defined elsewhere in the engine.
extern float	MaxVisualRadius;
extern float	WeaponRange[NUM_FIRERANGES];
extern GameLog*	BugLog;
extern bool	GeneralAlarm;
extern bool	friendlyDestroyed;
extern bool	enemyDestroyed;
extern bool	invulnerableON;		// tutorials: mechs take damage but look like they are not
void DEBUGWINS_setGameObject (long debugObj, GameObjectPtr obj);
void DEBUGWINS_print (const char* s, long window);

// Emits an [ABL_ARG_GUARD] trace line (flushed, for smoke-log capture) when an exec* binding skips a null pointer arg.
static inline void abl_arg_guard_log(const char* funcName, const char* paramName) {
	const char* srcFile = (CurModule && FileNumber > -1) ? CurModule->getSourceFile(FileNumber) : "unavailable";
	const char* modName = CurModule ? CurModule->getName() : "(unknown)";
	printf("[ABL_ARG_GUARD] func=%s param=%s module=%s file=%s line=%d — null ptr, skipping\n",
		funcName, paramName, modName, srcFile, execLineNumber);
	fflush(stdout);
}

GameObjectPtr getObject (long partId, bool existsOnly = false);
long getMovers (long partId, MoverPtr* list, bool existsOnly = false);


inline signed int double2long(double _in) {

	_in+=6755399441055744.0;
	return(*(signed int*)&_in);
}

inline bool isObjectId (long partId) {

	return(partId > MAX_UNIT_PART_ID);
}

inline bool isUnitId (long partId) {

	return((partId >= MIN_UNIT_PART_ID) && (partId <= MAX_UNIT_PART_ID));
}

inline bool isTeamId (long partId) {

	return((partId >= MIN_TEAM_PART_ID) && (partId <= MAX_TEAM_PART_ID));
}

inline GameObjectPtr getObject (long partId, bool existsOnly) {

	GameObjectPtr obj = nullptr;
	if (partId == -1)
		obj = CurObject;
	else
		obj = ObjectManager->findByPartId(partId);
	if (existsOnly) {
		if (obj && obj->getExists())
			return(obj);
		return(nullptr);
	}
	return(obj);
}

inline MechWarriorPtr getWarrior (long warriorIndex) {

	if (warriorIndex == -1)
		return(CurWarrior);
	else
		return(MechWarrior::warriorList[warriorIndex]);
}

inline long getMovers (long partId, MoverPtr* list, bool existsOnly) {

	// NOTE: It should be safe to assume that all objects here are movers,
	// since only movers may be members of a unit/team.
	// ALSO NOTE: If a group is referenced, we better be in a single-player
	// game (and it better be a computer-commander, which won't re-arrange
	// its groups)!

	long numObjects = 0;
	if ((partId >= OBJ_ID_FIRST_COMMANDER) && (partId <= OBJ_ID_LAST_COMMANDER))
		Fatal(3, " ABL.getMovers: bad id ");
	else if ((partId >= OBJ_ID_FIRST_TEAM) && (partId <= OBJ_ID_LAST_TEAM)) {
		TeamPtr team = Team::teams[partId - OBJ_ID_FIRST_TEAM];
		if (team)
			numObjects = Team::teams[partId - OBJ_ID_FIRST_TEAM]->getRoster((GameObjectPtr*)list, existsOnly);
		}
	else {
		for (long i = 0; i < ObjectManager->getNumMovers(); i++) {
			MoverPtr mover = ObjectManager->getMover(i);
			if (mover && (mover->getSquadId() == partId))
				list[numObjects++] = mover;
		}
	}

	return(numObjects);
}

// Non-exec functions shared across the translation units.
void execSetWillRequestHelp (void);
void initABL (void);
void closeABL (void);
void runAblArgGuardRepro(void);

// ABL native bindings, one per registered routine (registered in initABL, ablmc2.cpp).
void execGetId (void);
void execGetTime (void);
void execGetTimeLeft (void);
void execSelectObject (void);
void execSelectWarrior (void);
void execGetWarriorStatus (void);
void execGetContacts (void);
void execGetEnemyCount (void);
void execSelectContact (void);
void execIsContact (void);
void execGetContactId (void);
void execGetContactStatus (void);
void execGetContactRelativePosition (void);
void execSetTarget (void);
void execGetTarget (void);
void execGetWeaponsReady (void);
void execGetWeaponsLocked (void);
void execGetWeaponsInRange (void);
void execGetWeaponShots (void);
void execGetWeaponRanges (void);
void execGetObjectPosition (void);
void execGetIntegerMemory (void);
void execGetRealMemory (void);
void execGetAlarmTriggers (void);
void execGetChallenger (void);
void execGetTimeWithoutOrders (void);
void execGetFireRanges (void);
void execGetAttackers (void);
void execGetAttackerInfo (void);
void execSetChallenger (void);
void execSetIntegerMemory (void);
void execSetRealMemory (void);
void execHasMoveGoal (void);
void execHasMovePath (void);
void execSortWeapons (void);
void execGetVisualRange (void);
void execGetUnitMates (void);
void execGetTacOrder(void);
void execGetLastTacOrder(void);
void execGetObjects (void);
void execOrderWait (void);
void execOrderMoveTo (void);
void execOrderMoveToObject (void);
void execOrderMoveToContact (void);
void execOrderPowerDown (void);
void execOrderPowerUp (void);
void execOrderAttackObject (void);
void execOrderAttackContact (void);
void execOrderWithdraw(void);
void execObjectInWithdrawal(void);
void execDamageObject (void);
void execSetAttackRadius (void);
void execObjectChangeSides (void);
void execDistanceToObject (void);
void execDistanceToPosition (void);
void execObjectSuicide (void);
void execObjectCreate (void);
void execObjectExists (void);
void execObjectStatus (void);
void execObjectStatusCount (void);
void execObjectVisible (void);
void execObjectTeam (void);
void execObjectCommander (void);
void execObjectClass(void);
void execSetTimer (void);
void execCheckTimer (void);
void execEndTimer (void);
void execSetObjectiveTimer (void);
void execCheckObjectiveTimer (void);
void execSetObjectiveStatus (void);
void execCheckObjectiveStatus (void);
void execSetObjectiveType (void);
void execCheckObjectiveType (void);
void execPlayDigitalMusic (void);
void execStopMusic(void);
void execPlaySoundEffect(void);
void execPlayVideo(void);
void execSetRadio (void);
void execPlaySpeech (void);
void execPlayBetty (void);
void execGetMissionWon (void);
void execGetMissionLost (void);
void execGetObjectiveSuccess (void);
void execGetObjectiveFailed (void);
void execGetEnemyDestroyed (void);
void execGetFriendlyDestroyed (void);
void execPlayerInCombat (void);
void execGetSensorsActive (void);
void execGetCurrentMusicId (void);
void execGetMissionTuneId (void);
void execSetObjectActive(void);
void execObjectTypeID (void);
void execGetTerrainObjectPartID (void);
void execGetWeaponAmmo (void);
void execInArea (void);
void execObjectRemove (void);
void execCreateInfantry (void);
void execGetSensorsWorking (void);
void execGetCurrentBRValue (void);
void execSetCurrentBRValue (void);
void execGetArmorPts (void);
void execGetMaxArmor (void);
void execGetPilotID (void);
void execGetPilotWounds (void);
void execSetPilotWounds (void);
void execGetObjectActive (void);
void execGetObjectDamage (void);
void execGetObjectDmgPts (void);
void execGetObjectMaxDmg (void);
void execSetObjectDamage (void);
void execGetGlobalValue (void);
void execSetGlobalValue (void);
void execSetObjectivePos (void);
void execSetSensorRange (void);
void execSetTonnage (void);
void execSetExplosionDamage (void);
void execSetExplosionRadius (void);
void execSetSalvage (void);
void execSetSalvageStatus (void);
void execSetAnimation (void);
void execSetRevealed (void);
void execGetSalvage (void);
void execOrderRefit (void);
void execSetCaptured (void);
void execOrderCapture (void);
void execSetCapturable (void);
void execIsCaptured (void);
void execIsCapturable (void);
void execWasEverCapturable (void);
void execSetBuildingName (void);
void execCallStrike (void);
void execCallStrikeEx (void);
void execOrderLoadElementals (void);
void execOrderDeployElementals (void);
void execAddPrisoner (void);
void execLockGateOpen (void);
void execLockGateClosed (void);
void execReleaseGateLock (void);
void execIsGateOpen (void);
void execGetRelativePositionToPoint (void);
void execGetRelativePositionToObject (void);
void execGetUnitStatus (void);
void execRepair (void);
void execGetFixed (void);
void execGetRepairState (void);
void execIsTeamTargeting (void);
void execIsTeamCapturing (void);
void execSendMessage (void);
void execGetMessage (void);
void execGetHomeTeam (void);
void execIsServer (void);
void execCalcPartID (void);
void execSetDebugString (void);
void execBreak (void);
void execPathExists (void);
void execConvertCoords (void);
void execCoreMoveTo (void);
void execCoreMoveToObject (void);
void execCorePower (void);
void execCoreAttack (void);
void execCoreCapture (void);
void execCoreScan (void);
void execCoreControl (void);
void execCoreEject (void);
void execSetPilotState (void);
void execGetPilotState (void);
void execGetNextPilotEvent (void);
void execSetTargetPriority (void);
void execSetDebugWindow (void);
void execSetMovieMode (void);
void execEndMovieMode (void);
void execGetGeneralAlarm (void);
void execSetGeneralAlarm (void);
void execFadeToColor (void);
void execForceMovieEnd (void);
void execGetCameraPosition (void);
void execSetCameraPosition(void);
void execSetCameraGoalPosition (void);
void execGetCameraGoalPosition (void);
void execGetCameraRotation (void);
void execSetCameraRotation (void);
void execSetCameraGoalRotation (void);
void execGetCameraGoalRotation (void);
void execGetCameraZoom (void);
void execSetCameraZoom (void);
void execGetCameraGoalZoom (void);
void execSetCameraGoalZoom (void);
void execSetCameraVelocity (void);
void execGetCameraVelocity (void);
void execSetCameraGoalVelocity (void);
void execGetCameraGoalVelocity (void);
void execSetCameraLookObject (void);
void execGetCameraLookObject (void);
void execGetCameraFrameLength (void);
void execAnimationCallout (void);
void execTutorialText (void);
void execGUIIsAOEStyle (void);
void execSetInvulnerable (void);
void execFreezeGUI (void);
void execLogisticsScreenId (void);
void execLogisticsAnimationCallout (void);
void execLogisticsInCallout (void);
void execInCallout (void);
void execIsPlayingVoiceOver(void);
void execStopVoiceOver(void);
void execGetLogisticsTime(void);
void execRequestHelp (void);
void execRequestTarget (void);
void execRequestShelter (void);
void execSetTextMsg (void);
void execSetLargeMsg (void);
void execSetMissionTune (void);
void execAddResourcePoints (void);
void execAddMoney (void);
void execSetTxtBuildingName (void);
void execAblPrint (void);
void execTeleportToPoint (void);
void execAddMoverToPlayer (void);
void execRemoveMoverFromPlayer (void);
void execSetBrainFixed (void);
void execSetBrainNew (void);
void execDestroyMechBodyLocation (void);
void execDamageMechArmor (void);
void execSetMechGesture (void);
void execSetUnitForceGroup (void);
void execToggleAirStrike (void);
void execToggleSensorStrike (void);
void execToggleArtilleryPiece (void);
void execToggleRepairTruck (void);
void execToggleSalvageCraft (void);
void execSetRepairTruckEnabled (void);
void execSetSalvageCraftEnabled (void);
void execGetTargetRelativePosition (void);
void execGetRelativePositionToTarget (void);
void execNewDistanceToPosition (void);
void execNumFriendsWithinRadius (void);
void execNumEnemiesWithinRadius (void);
void execHasOrderFromPlayer (void);
void execIsSelected (void);
void execIsRefit (void);
void execNeedsRefit (void);
void execHirePilot (void);
void execAddNewPilot (void);
void execAddNewPilotToPlayer (void);
void execAutoRepairWithinRadius (void);
void execScanAreaCapture (void);
void execScanAreaRepair (void);
void execSetCampaignGlobalVar (void);
void execGetCampaignGlobalVar (void);
void execSetFixedBuildingRp (void);
void execGetTimeOfLastStep (void);
void execIsCurTacOrderMoveOrder (void);
void execGetCurTacOrderTime (void);
void execClearMoveOrdersOmni (void);
void execTDebugString (void);
void execIsDeadOrFled (void);
void execPrintTeamStatus (void);
void execMCPrint (void);
void execGetMissionStatus (void);
void execAddTriggerArea (void);
void execIsTriggerAreaHit (void);
void execResetTriggerArea (void);
void execRemoveTriggerArea (void);
void execGetWeapons (void);
void execGetWeaponsStatus (void);
void execSetMoveArea (void);
void execClearTacOrder (void);
void execPlayWave (void);
void execSetWillHelp (void);
void execGetLastScan (void);
void execGetMapInfo (void);
void execIsOffMap (void);
void execSetGoalPlanning (void);
void execSetEject (void);
void execSetKeepMoving (void);

#endif // ABLMC2_INTERNAL_H
