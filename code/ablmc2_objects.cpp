// ABL native bindings: object damage/distance/status, timers, objectives, music/speech/video and mission-status bindings.
// Declared in ablmc2_internal.h; registered, in order, by initABL in ablmc2.cpp.
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "ablmc2_internal.h"
#include "abl_script_events.h"
#include "Gates/gate.h"
#include "mc2fmt.h"
#include <algorithm>
#include <iterator>
#include <span>

void execDamageObject (void) {

	//	DAMAGE OBJECT function:
	//		Directly damages an object as if it were a weapon shot.
	//		Returns error code.
	//		PARAMS:	integer				target object id
	//				integer				attacker object id
	//				integer				weapon master component id
	//				real				damage points
	//				integer				hit location
	//				real				hit roll
	//				real				entry angle
	//		RETURN: integer				error code (always returns 0, for now)
	//									-1 = bad target
	//									-2 = bad attacker

	long targetId = ABLi_popInteger();
	long attackerId = ABLi_popInteger();
	long weaponMasterId = ABLi_popInteger();
	float damage = ABLi_popReal();
	long hitLocation = ABLi_popInteger();
	float hitRoll = ABLi_popReal();
	float entryAngle = ABLi_peekReal();

	GameObjectPtr attacker = getObject(attackerId);

	ABLi_pokeInteger(-1);
	if (attacker) {
		if ((targetId >= MIN_UNIT_PART_ID) && (targetId <= MAX_UNIT_PART_ID)) {
			long numObjects = getMovers(targetId, moverList, true);
			WeaponShotInfo shotInfo;
			shotInfo.init(attacker->getWatchID(), weaponMasterId, damage, hitLocation, entryAngle);
			if (MPlayer) {
				if (MPlayer->isServer()) {
					for (long i = 0; i < numObjects; i++) {
						moverList[i]->handleWeaponHit(&shotInfo, true);
					}
				}
				}
			else
				for (long i = 0; i < numObjects; i++)
					moverList[i]->handleWeaponHit(&shotInfo);
			ABLi_pokeInteger(numObjects);
			}
		else {
			GameObjectPtr target = getObject(targetId, true);
			if (target) {
				WeaponShotInfo shotInfo;
				shotInfo.init(attacker->getWatchID(), weaponMasterId, damage, hitLocation, entryAngle);
				if (MPlayer) {
					if (MPlayer->isServer()) {
						target->handleWeaponHit(&shotInfo, true);
					}
					}
				else
					target->handleWeaponHit(&shotInfo);
				ABLi_pokeInteger(1);
				}
			else
				ABLi_pokeInteger(-2);
		}
	}
}

void execSetAttackRadius (void) {

	//	SET ATTACK RADIUS tactical order:
	//		Sets the range at which the pilot will break off attack from its
	//		last target. Returns previous attack radius.
	//		PARAMS:	real				attack radius in meters
	//		RETURN: real				previous attack radius

	float radius = ABLi_peekReal();
	ABLi_pokeReal(CurWarrior->getAttackRadius());
	CurWarrior->setAttackRadius(radius);
}

void execObjectChangeSides (void) {

	//Object Change Side Function.
	//		PARAMS:	integer, integer
	//		Returns: nothing!
	
	long objectId = ABLi_popInteger();
	long newObjectSide = ABLi_popInteger();

	GameObjectPtr object = nullptr;
	if ((objectId >= MIN_UNIT_PART_ID) && (objectId <= MAX_UNIT_PART_ID)) {
		Fatal(0, " Cannot ABL:ObjectChangeSides for Mover Units ");

		}
	else {
		object = getObject(objectId);
		if (object)
			object->setTeam(Team::teams[newObjectSide]);
	}
}

void execDistanceToObject (void) {

	//Distance to Object From Object Function.
	//		PARAMS:	integer, integer
	//		Returns: (real) distance in meters
	
	long objectId1 = ABLi_popInteger();
	long objectId2 = ABLi_peekInteger();

	ABLi_pokeReal(-1.0);

	GameObjectPtr object2 = getObject(objectId2);
	if (object2) {
		glm::vec3 position2 = object2->getPosition();
		if ((objectId1 >= MIN_UNIT_PART_ID) && (objectId1 <= MAX_UNIT_PART_ID)) {
			float minDistance = 3.4E38f;
			float distance = 3.4E38f;
			long numObjects = getMovers(objectId1, moverList);
			for (long i = 0; i < numObjects; i++) {
				MoverPtr mover = moverList[i];
				if (!mover->getExistsAndAwake())
					continue;
				glm::vec3 position1 = mover->getPosition();
				position2.z = position1.z;
		
				glm::vec3 resultVector;
				resultVector = position2 - position1;
		
				distance = glm::length(resultVector);
				minDistance = std::min(minDistance, distance);
			}
			if (minDistance < 3.4E38)
				ABLi_pokeReal(minDistance * metersPerWorldUnit);
			}
		else {
			GameObjectPtr object1 = getObject(objectId1);
			if (object1) {
				glm::vec3 position1 = object1->getPosition();
				position2.z = position1.z;
			
				glm::vec3 resultVector;
				resultVector = position2 - position1;
			
				ABLi_pokeReal(glm::length(resultVector) * metersPerWorldUnit);
			}
		}
	}
}
			
void execDistanceToPosition (void) {

	//Distance to Object From Position Function.
	//		PARAMS:	integer, real[3]
	//		Returns: (real) distance in meters
	
	long objectId = ABLi_popInteger();
	float* coordList = ABLi_popRealPtr();

	ABLi_pushReal(-1.0);

	// For now, we only care about x and y...
	glm::vec3 position2;
	position2 = glm::vec3(0.0f);

	if (!isnan(coordList[0]))
		position2.x = coordList[0];

	if (!isnan(coordList[1]))
		position2.y = coordList[1];
	
	if ((objectId >= MIN_UNIT_PART_ID) && (objectId <= MAX_UNIT_PART_ID)) {
		long numObjects = getMovers(objectId, moverList);
		float minDistance = 3.4E38f;
		float distance = 3.4E38f;
		for (long i = 0; i < numObjects; i++) {
			MoverPtr mover = moverList[i];
			if (!mover->getExistsAndAwake())
				continue;
			glm::vec3 position1 = mover->getPosition();
			position2.z = position1.z;
		
			glm::vec3 resultVector;
			resultVector = position2 - position1;
		
			distance = glm::length(resultVector);
			minDistance = std::min(minDistance, distance);
		}
		if (minDistance < 3.4E38)
			ABLi_pokeReal(minDistance * metersPerWorldUnit);
		}
	else {
		GameObjectPtr obj = getObject(objectId);
		if (obj) {
			glm::vec3 position1 = obj->getPosition();
			position2.z = position1.z;
			
			glm::vec3 resultVector;
			resultVector = position2 - position1;
			ABLi_pokeReal(glm::length(resultVector) * metersPerWorldUnit);
		}
	}
}
			
void execObjectSuicide (void) {

	//Cause object to destroy itself Function.
	//		PARAMS:	integer
	//		Returns: nothing
	
	long objectId = ABLi_popInteger();
	
	GameObjectPtr object1 = nullptr;
	if ((objectId >= MIN_UNIT_PART_ID) && (objectId <= MAX_UNIT_PART_ID)) {
		long numObjects = getMovers(objectId, moverList);
		for (long i = 0; i < numObjects; i++)
			MC2_DESTROY(moverList[i], "abl_explicit_destroy");
		}
	else {
		object1 = getObject(objectId);
		if (object1)
			MC2_DESTROY(object1, "abl_explicit_destroy");
	}
}
			
void execObjectCreate (void) {

	//Cause object to create itself Function.
	//		PARAMS:	integer
	//		Returns: id of object created 
	
	long objectId = ABLi_popInteger();
	
	long result = 0;

	GameObjectPtr object = getObject(objectId);
	if (object && !object->getExists()) {
		object->setExists(true);
		for (long i = 0; i < Team::numTeams; i++)
			SensorManager->getTeamSensor(i)->scanBattlefield();
		result = objectId;
	}
	ABLi_pushInteger(result);
}
			
void execObjectExists (void) {

	//test if object exists Function.
	//		PARAMS:	integer
	//		Returns: integer
	
	long objectId = ABLi_peekInteger();
	ABLi_pokeInteger(0);

	if ((objectId >= MIN_UNIT_PART_ID) && (objectId <= MAX_UNIT_PART_ID)) {
		long numObjs = getMovers(objectId, moverList);
		long i = 0;
		while ((i < numObjs) && !moverList[i]->getExists())
			i++;
		ABLi_pokeInteger(i < numObjs);
		}
	else {
		GameObjectPtr obj = getObject(objectId);
		if (obj && obj->getExists())
			ABLi_pokeInteger(1);
	}
}
			
void execObjectStatus (void) {

	//returns object's status.
	//		PARAMS:	integer
	//		Returns: integer
	
	long objectId = ABLi_popInteger();
	
	long result = -1;
	
	if ((objectId >= MIN_UNIT_PART_ID) && (objectId <= MAX_UNIT_PART_ID)) {
		// We have a group.  Act accordingly.
		// No Group Check of actual status, return -1.
		// If any unit member is still alive, then the unit is...
		bool disabled = true;
		long numObjects = getMovers(objectId, moverList);
		for (long i = 0; i < numObjects; i++) {
			long status = moverList[i]->getStatus();
			if ((status != OBJECT_STATUS_DESTROYED) && (status != OBJECT_STATUS_DISABLED)) {
				MechWarriorPtr pilot = moverList[i]->getPilot();
				if (pilot && (pilot->getStatus() != WARRIOR_STATUS_WITHDRAWN)) {
					disabled = false;
					break;
				}
			}
		}
		if (disabled)
			result = OBJECT_STATUS_DISABLED;
		else
			result = OBJECT_STATUS_NORMAL;
		}
	else {
		GameObjectPtr obj = getObject(objectId);
		if (obj)
			result = obj->getStatus();
	}
	ABLi_pushInteger(result);
}
			
void execObjectStatusCount (void) {

	//return object status count for an object, unit or team.
	//		PARAMS:	integer							partId
	//				integer[9]						tally list
	//		Returns: NONE

	long objectId = ABLi_popInteger();
	const auto output = ABLi_popIntBuf();
	int tallyList[9] = {};
	if (!output.empty()) std::copy_n(output.begin(), std::min(output.size(), std::size(tallyList)), tallyList);
	
	if ((objectId >= MIN_UNIT_PART_ID) && (objectId <= MAX_UNIT_PART_ID)) {
		if ((objectId >= PlayerLance0) && (objectId < PlayerLance0 + MAX_MOVERGROUPS))
			Commander::commanders[0]->getGroup(objectId - PlayerLance0)->statusCount(tallyList);
		else if ((objectId >= AlliedLance0) && (objectId < AlliedLance0 + MAX_MOVERGROUPS))
			Commander::commanders[2]->getGroup(objectId - AlliedLance0)->statusCount(tallyList);
		else if ((objectId >= ClanStar0) && (objectId < ClanStar0 + MAX_MOVERGROUPS))
			Commander::commanders[1]->getGroup(objectId - ClanStar0)->statusCount(tallyList);
		else if ((objectId >= OBJ_ID_FIRST_TEAM) && (objectId <= OBJ_ID_LAST_TEAM)) {
			TeamPtr team = Team::teams[objectId - OBJ_ID_FIRST_TEAM];
			if (team)
				team->statusCount(tallyList);
		}
		}
	else {
		GameObjectPtr object1 = getObject(objectId);
		if (object1 && object1->getStatus() >= 0 && object1->getStatus() < 9)
			tallyList[object1->getStatus()]++;
	}
	abl::copyIntArray(output, tallyList, "objectstatuscount_truncated");
}
			
void execObjectVisible (void) {

	//test if object2 is visible from object1
	//		PARAMS:	integer, integer
	//		Returns: integer
	
	long objectId1 = ABLi_popInteger();
	long objectId2 = ABLi_popInteger();
	
	GameObjectPtr object1 = nullptr;
	GameObjectPtr object2 = getObject(objectId2);
	long result = 0;
	
	if (object2) {
		if ((objectId1 >= MIN_UNIT_PART_ID) && (objectId1 <= MAX_UNIT_PART_ID)) {
			// We have a group.  Act accordingly.
			// Run through all objects fitting the group.
			// Can anyone in the group see the object?
			long numObjects = getMovers(objectId1, moverList);
			for (long i = 0; i < numObjects; i++)
				if (moverList[i]->lineOfSight(object2)) {
					result = 1;
					break;
				}
			}
		else {
			object1 = getObject(objectId1);
			if (object1)
				result = object1->lineOfSight(object2);
		}
	}

	ABLi_pushInteger(result);
}				
			
void execObjectTeam (void) {

	//return what side object plays for Function.
	//		PARAMS:	integer
	//		Returns: integer
	
	long objectId = ABLi_popInteger();

	long result = -1;
	GameObjectPtr object = getObject(objectId);
	if (object)
		result = MIN_TEAM_PART_ID + object->getTeamId();
	
	ABLi_pushInteger(result);
}

void execObjectCommander (void) {

	long objectId = ABLi_peekInteger();
	ABLi_pokeInteger(-1);
	
	if ((objectId >= MIN_UNIT_PART_ID) && (objectId <= MAX_UNIT_PART_ID)) {
		}
	else {
		GameObjectPtr obj = getObject(objectId);
		if (obj)
			ABLi_pokeInteger(obj->getCommanderId());
	}
}

void execObjectClass(void) {

	//return what object class the obj is.
	//		PARAMS:	integer
	//		Returns: integer
	
	long objectId = ABLi_popInteger();
	
	long result = -1;
	GameObjectPtr object = getObject(objectId);
	if (object)
		result = object->getObjectClass();
	ABLi_pushInteger(result);
}
			
void execSetTimer (void) {

	//Creates a timer which counts down from time passed in
	//		PARAMS:	integer, real
	//		Returns: integer (ID of timer created)
	
	short timerNumber = ABLi_popInteger();
	float duration = ABLi_popReal();

	if (timerNumber < SCENARIO_TIMER_1 || timerNumber > SCENARIO_TIMER_8)
		timerNumber = 0;
	else {
		TimerPtr timer = timerManager->getTimer(timerNumber);
		Assert(timer != nullptr, timerNumber, " ABL.execHbSetTimer: NULL timer ");
		timer->setTimer(duration);
	}

	ABLi_pushInteger(timerNumber);
}
			
void execCheckTimer (void) {

	//Checks a timer created and return time left
	//		PARAMS:	integer
	//		Returns: real (time left)
	
	short timerNumber = ABLi_popInteger();

	float timeLeft = 0.0;
	if ((timerNumber >= 0) && (timerNumber < MAX_TIMERS)) {
		TimerPtr timer = timerManager->getTimer(timerNumber);
		Assert(timer != nullptr, timerNumber, " ABL.execHbSetTimer: NULL timer ");
		timeLeft = timer->getCurrentTime();
	}

	timeLeft = std::max(timeLeft, 0.0f);

	ABLi_pushReal(timeLeft);
}
			
void execEndTimer (void) {

	//sets time to zero and destroys timer.
	//		PARAMS:	integer
	//		Returns: nothing
	
	long timerID = ABLi_popInteger();

// NOT REALLY NECESSARY ANYMORE--since the timers have no callback, they just
// keep counting down... no harm...
}
	
void execSetObjectiveTimer (void) {

	//Sets timer of objective passed in.
	//		PARAMS:	integer, real
	//		Returns: integer (result)
	
	long objectiveNum = ABLi_popInteger();
	float duration = ABLi_popReal();

	ABLi_pushInteger(mission->setObjectiveTimer(objectiveNum, duration));
}

void execCheckObjectiveTimer (void) {

	//Checks an objective timer passed in and returns time left
	//		PARAMS:	integer
	//		Returns: real (time left)
	
	long objectiveNum = ABLi_popInteger();
	
	ABLi_pushReal(mission->checkObjectiveTimer(objectiveNum));
}

void execSetObjectiveStatus (void) {

	//Sets status of objective passed in.
	//		PARAMS:	integer, integer
	//		Returns: integer (result)

	long objectiveNum = ABLi_popInteger();
	long status = ABLi_popInteger();
	
	ABLi_pushInteger(mission->setObjectiveStatus(objectiveNum,status));
}

void execCheckObjectiveStatus (void) {

	//Checks an objective status passed in and returns it
	//		PARAMS:	integer
	//		Returns: integer (status)

	long objectiveNum = ABLi_popInteger();
	
	ABLi_pushInteger(mission->checkObjectiveStatus(objectiveNum));
}

void execSetObjectiveType (void) {

	//Sets type of objective passed in.
	//		PARAMS:	integer, integer
	//		Returns: integer (result)

	long objectiveNum = ABLi_popInteger();
	long type = ABLi_popInteger();
	
	ABLi_pushInteger(mission->setObjectiveType(objectiveNum,type));
}

void execCheckObjectiveType (void) {

	//Checks an objective type passed in and returns it
	//		PARAMS:	integer
	//		Returns: integer (status)

	long objectiveNum = ABLi_popInteger();
	
	ABLi_pushInteger(mission->checkObjectiveType(objectiveNum));
}

void execPlayDigitalMusic (void) {

	//Starts playback of Digital music
	//		PARAMS:	integer
	//		Returns: integer (result)

	long soundNum = ABLi_popInteger();

	if (soundSystem)
		soundSystem->playABLDigitalMusic(soundNum);
		
	ABLi_pushInteger(0);	
}

void execStopMusic(void) {

	//Stops playback of Digitalmusic
	//		PARAMS:	None
	//		Returns: None

	if (soundSystem)
		soundSystem->stopABLMusic();
}

void execPlaySoundEffect(void) {

	//Starts playback of Digital sound effect
	//		PARAMS:	integer, real
	//		Returns: integer (result)

	long soundNum = ABLi_popInteger();
	
	if (soundSystem)
		soundSystem->playABLSFX(soundNum);
	ABLi_pushInteger(0);
}

void execPlayVideo(void) {

	//Starts playback of video sequence
	//		PARAMS:	integer, real
	//		Returns: integer (result)

	char* fileName = ABLi_popCharPtr();

	mission->missionInterface->playMovie(fileName);

	ABLi_pushInteger(0);
}

void execSetRadio (void) {

	//	SET RADIO function:
	//		Turns the radio(s) on or off.
	//		PARAMS:	integer					warrior id
	//				integer					FALSE = off, TRUE= on
	//		RETURN: none

	long warriorIndex = ABLi_popInteger();
	bool radioSetting = ABLi_popBoolean();

	MechWarriorPtr pilot = getWarrior(warriorIndex);

	if (pilot && pilot->getRadio()) {
		if (radioSetting)
			pilot->getRadio()->turnOn();
		else
			pilot->getRadio()->turnOff();
	}
}

void execPlaySpeech (void) {

	//Starts playback of Digital speech
	//		PARAMS:	integer, integer
	//		Returns: integer (result)

	long warriorIndex = ABLi_popInteger();
	long message = ABLi_popInteger();

	MechWarriorPtr pilot = getWarrior(warriorIndex);
	if (pilot)
		pilot->radioMessage(message, true);

	ABLi_pushInteger(0);		
}

void execPlayBetty (void) {

	//Starts playback of betty message
	//		PARAMS:	integer
	//		Returns: integer (result)

	long messageIndex = ABLi_popInteger();
	long result = soundSystem->playBettySample(messageIndex);
	
	ABLi_pushInteger(result);
}

void execGetMissionWon (void)
{
	if (Mission::terminationResult != -1)
	{
		ABLi_pushBoolean(Mission::terminationResult >= mis_PLAYER_WIN_SMALL);
	}
	else
	{
		ABLi_pushBoolean(false);
	}
}

void execGetMissionLost (void) 
{
	ABLi_pushBoolean((Mission::terminationResult == mis_PLAYER_LOST_BIG) || 
					(Mission::terminationResult == mis_PLAYER_LOST_SMALL));
}

void execGetObjectiveSuccess (void) 
{
	ABLi_pushBoolean(mission->checkObjectiveSuccess());
}

void execGetObjectiveFailed (void) 
{
	ABLi_pushBoolean(mission->checkObjectiveFailed());
}

void execGetEnemyDestroyed (void) 
{
	ABLi_pushBoolean(enemyDestroyed);
	enemyDestroyed = false;
}

void execGetFriendlyDestroyed (void) 
{
	ABLi_pushBoolean(friendlyDestroyed);
	friendlyDestroyed = false;
}

void execPlayerInCombat (void) 
{
	ABLi_pushBoolean(MechWarrior::anyPlayerInCombat());
}

void execGetSensorsActive (void) 
{
	ABLi_pushBoolean(SensorSystemManager::enemyInLOS);
}

void execGetCurrentMusicId (void) 
{
	if (mc2::gate::get<"MC2_SIM_IDENTITY_TRACE_DETAIL">()) mc2::fmt::print_err("[LOADMUSIC] t={:.4f} turn={} id={} combat={} sensor={}\n", scenarioTime, turn, soundSystem->getCurrentMusicId(), MechWarrior::anyPlayerInCombat(), SensorSystemManager::enemyInLOS);
	ABLi_pushInteger(soundSystem->getCurrentMusicId());
}

void execGetMissionTuneId (void)
{
	ABLi_pushInteger(mission->getMissionTuneId());
}
