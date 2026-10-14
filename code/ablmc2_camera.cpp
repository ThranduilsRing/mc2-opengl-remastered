// ABL native bindings: movie mode, camera get/set pairs, GUI/logistics callouts and help/target/shelter requests.
// Declared in ablmc2_internal.h; registered, in order, by initABL in ablmc2.cpp.
#include "ablmc2_internal.h"
#include "abl_script_events.h"
#include <algorithm>
#include <iterator>
#include <span>

std::span<int> ABLi_popIntBuf(void);
std::span<float> ABLi_popRealBuf(void);
namespace abl {
long clampArrayCount(long, size_t, std::string_view);
long copyIntArray(std::span<int>, std::span<const int>, std::string_view);
long copyRealArray(std::span<float>, std::span<const float>, std::string_view);
}



// Target-plan helpers used by the strike and help-request bindings below.

long getMoversWithinRadius (MoverPtr* moverList, Stuff::Vector3D center, float radius, long teamID, long commanderID, bool getEnemies, bool sortDescending, bool ignoreOrder) {

	static float sortValues[MAX_MOVERS];
	if (!Team::sortList) {
		Team::sortList = new SortList;
		if (!Team::sortList)
			Fatal(0, " Unable to create Team Contact sortList ");
		Team::sortList->init(MAX_CONTACTS_PER_SENSOR);
	}
	Team::sortList->clear(sortDescending);

	TeamPtr team = nullptr;
	if (teamID > -1)
		team = Team::teams[teamID];

	long numValidMovers = 0;
	if (getEnemies) {
		for (long i = 0; i < ObjectManager->getNumMovers(); i++) {
			MoverPtr mover = ObjectManager->getMover(i);
			if (mover->getExists() && !mover->isDisabled() && mover->isEnemy(team))
				if (mover->numFunctionalWeapons > 0)
					if (mover->distanceFrom(center) < radius) {
						if (ignoreOrder || (mover->getPilot()->getCurTacOrder()->code == TACTICAL_ORDER_NONE)) {
							Team::sortList->setId(numValidMovers, mover->getHandle());
							Team::sortList->setValue(numValidMovers, mover->getThreatRating());
							numValidMovers++;
						}
					}
		}
		}
	else {
		if (commanderID == -1)
			for (long i = 0; i < team->rosterSize; i++) {
				MoverPtr mover = team->getMover(i);
				if (mover->getExists() && !mover->isDisabled() && (mover->numFunctionalWeapons > 0))
					if (mover->distanceFrom(center) < radius) {
						if (mover->getPilot()->getWillHelp()) {
							if (ignoreOrder || (mover->getPilot()->getCurTacOrder()->code == TACTICAL_ORDER_NONE)) {
								Team::sortList->setId(numValidMovers, mover->getHandle());
								Team::sortList->setValue(numValidMovers, mover->getThreatRating());
								numValidMovers++;
							}
						}
					}
			}
		else
			for (long i = 0; i < team->rosterSize; i++) {
				MoverPtr mover = team->getMover(i);
				if ((commanderID == mover->commanderId) && mover->getExists() && !mover->isDisabled() && (mover->numFunctionalWeapons > 0))
					if (mover->distanceFrom(center) < radius) {
						if (mover->getPilot()->getWillHelp()) {
							if (ignoreOrder || (mover->getPilot()->getCurTacOrder()->code == TACTICAL_ORDER_NONE)) {
								Team::sortList->setId(numValidMovers, mover->getHandle());
								Team::sortList->setValue(numValidMovers, mover->getThreatRating());
								numValidMovers++;
							}
						}
					}
			}
	}

	if (numValidMovers > 0) {
		Team::sortList->sort(sortDescending);
		for (long i = 0; i < numValidMovers; i++)
			moverList[i] = (MoverPtr)ObjectManager->get(Team::sortList->getId(i));
	}
	return(numValidMovers);
}

void calcAttackPlan (long numAttackers, GameObjectPtr* attackers, long numDefenders, GameObjectPtr* defenders) {

	if ((numDefenders <= 0) || (numAttackers <= 0))
		return;

	// This assumes the attackers and defenders lists are already sorted
	// in descending order...
	float attackRatio[MAX_MOVERS];
	long attackTotal[MAX_MOVERS];
	GameObjectPtr target[MAX_MOVERS];
	for (long i = 0; i < numDefenders; i++) {
		attackTotal[i] = 0;
		attackRatio[i] = 0.0;
		target[i] = nullptr;
	}

	// Calc who they should attack, trying to spread the wealth, so to speak...
	for (long a = 0; a < numAttackers; a++) {
		long toughest = numDefenders - 1;
		for (long d = (numDefenders - 2); d > -1; d--) {
			if (attackRatio[d] < attackRatio[toughest])
				toughest = d;
			else if (attackRatio[d] == attackRatio[toughest]) {
				if (attackTotal[d] > attackTotal[toughest])
					toughest = d;
			}
		}
		attackTotal[toughest] += attackers[a]->getThreatRating();
		long defThreat = defenders[toughest]->getThreatRating();
		attackRatio[toughest] = (defThreat != 0) ? (float)attackTotal[toughest] / (float)defThreat : 0.0f;
		target[a] = defenders[toughest];
	}

	for (int i = 0; i < numAttackers; i++)
		attackers[i]->getPilot()->triggerAlarm(PILOT_ALARM_ATTACK_ORDER, target[i]->getWatchID());
}

GameObjectPtr calcBestTarget (MoverPtr attacker, long numAttackers, MoverPtr* attackers, long numDefenders, MoverPtr* defenders) {

	// This assumes the attackers and defenders lists are already sorted
	// in descending order...

	if (numDefenders == 0)
		return(nullptr);

	float attackRatio[MAX_MOVERS];
	long attackTotal[MAX_MOVERS];
	GameObjectPtr target[MAX_MOVERS];
    int i=0;
	for (; i < numDefenders; i++) {
		attackTotal[i] = 0;
		attackRatio[i] = 0.0;
		target[i] = nullptr;
	}

	for (long a = 0; a < numAttackers; a++) {
		GameObjectPtr target = attackers[a]->getPilot()->getCurrentTarget();
		for (int d = 0; d < numDefenders; d++)
			if (defenders[d] == target) {
				// sebi: ORIG BUG FIX found with help of VisualStudio Analysis
				// how could game work with this broken? this influences gameplay greatly, no?
				attackTotal[d] += attackers[a]->getThreatRating();
				break;
			}
	}
	for (int d = 0; d < numDefenders; d++)
		attackRatio[d] = attackTotal[d] / defenders[d]->getThreatRating();

	long bestTarget = numDefenders - 1;
	if (numDefenders > 1)
		for (int d = (numDefenders - 2); d > -1; d--) {
			if (attackRatio[d] < attackRatio[bestTarget])
				bestTarget = d;
			else if (attackRatio[d] == attackRatio[bestTarget]) {
				if (attackTotal[d] > attackTotal[bestTarget])
					bestTarget = d;
			}
		}
	attackTotal[bestTarget] += attacker->getThreatRating();
	attackRatio[bestTarget] = attackTotal[bestTarget] / defenders[bestTarget]->getThreatRating();
	return(defenders[bestTarget]);
}

void execSetMovieMode (void) 
{
	//	SetMovieMode
	//		Changes game to movie playback mode.  GUI disappears, Commands not allowed, Letterbox
	//		PARAMS: NONE
	//		RETURN: NONE

	if (eye)
		eye->setMovieMode();
}

void execEndMovieMode (void) 
{
	//	EndMovieMode
	//		Changes game to play mode.  GUI reappears, Commands allowed, no Letterbox
	//		PARAMS: NONE
	//		RETURN: NONE

	if (eye)
		eye->endMovieMode();
}

void execGetGeneralAlarm (void) 
{
	//	GetGeneralAlarm
	//		Returns value of GeneralAlarm set by Perimeter alarms in game
	//		PARAMS: NONE
	//		RETURN: Integer

	ABLi_pushInteger(GeneralAlarm);
}

void execSetGeneralAlarm (void) 
{
	//	SetGeneralAlarm
	//		sets value of GeneralAlarm for Perimeter alarms in game
	//		PARAMS: Integer
	//		RETURN: NONE

	GeneralAlarm = (ABLi_popInteger() != 0);
}

void execFadeToColor (void) 
{
	//	FadeToColor
	//		Fades entire screen to color read in by the time read in.
	//		PARAMS: NONE
	//		RETURN: NONE

	DWORD fadeColor = ABLi_popInteger();
	DWORD fadeTime = ABLi_popReal();
	
	if (eye)
		eye->fadeToColor(fadeColor,fadeTime);
}

void execForceMovieEnd (void)
{
	// ForceMovieEnd
	//		Checks if the player has requested the movie to end.  If so, the script should endMovieMode immediately!!
	//		PARAMS: None
	//		RETURN: 1 if user asked movie to end.  Zero otherwise
	DWORD result = 0;
	if (eye && eye->forceMovieEnd)
		result = 1;
		
	ABLi_pushInteger(result);
}

void execGetCameraPosition (void) {

	//	GetCameraPosition
	//		Sets the value passed in to the camera position.
	//		PARAMS:	real[3]				camPos
	//		RETURN: NONE

	const auto output = ABLi_popRealBuf();
	float camPos[3] = {};

	Stuff::Vector3D result(0.0,0.0,0.0);
	if (eye)
		result = eye->getPosition();

	camPos[0] = result.x;
	camPos[1] = result.y;
	camPos[2] = result.z;
	abl::copyRealArray(output, camPos, "getcameraposition_truncated");
}

void execSetCameraPosition(void) {

	//	SetCameraPosition 
	//		Sets the camera position to the value passed in.
	//		PARAMS:	real[3]				camPos
	//		RETURN: NONE

	float* camPos = ABLi_popRealPtr();

	Stuff::Vector3D result(0.0,0.0,0.0);
	result.x = camPos[0];
	result.y = camPos[1];
	result.z = camPos[2];
	
 	if (eye)
		eye->setPosition(result,false);
}

void execSetCameraGoalPosition (void) {

	//	SetCameraGoalPosition 
	//		Sets the camera position to the value passed in.
	//		PARAMS:	real[3]				camPos
	//				real				time
	//		RETURN: NONE

	float* camPos = ABLi_popRealPtr();
	float time = ABLi_popReal();

	Stuff::Vector3D result(0.0,0.0,0.0);
	result.x = camPos[0];
	result.y = camPos[1];
	result.z = camPos[2];
	
 	if (eye) {
		eye->setGoalPosition(result);
		eye->setGoalPosTime(time);
	}
}

void execGetCameraGoalPosition (void) {

	//	GetCameraGoalPosition
	//		Gets the camera position to the value passed in.
	//		PARAMS:	real[3]				camPos
	//		RETURN: NONE

	const auto output = ABLi_popRealBuf();
	float camPos[3] = {};

	Stuff::Vector3D result(0.0,0.0,0.0);

	if (eye)
		result = eye->getGoalPosition();

	camPos[0] = result.x;
	camPos[1] = result.y;
	camPos[2] = result.z;
	abl::copyRealArray(output, camPos, "getcameragoalposition_truncated");
}

void execGetCameraRotation (void) {

	//	GetCameraRotation
	//		Sets the value passed in to the camera Rotation.
	//		PARAMS:	real[3]				camRot
	//		RETURN: NONE

	const auto output = ABLi_popRealBuf();
	float camRot[3] = {};

	Stuff::Vector3D result(0.0,0.0,0.0);
	if (eye)
		result = eye->getRotation();

	camRot[0] = result.x;
	camRot[1] = result.y;
	camRot[2] = result.z;
	abl::copyRealArray(output, camRot, "getcamerarotation_truncated");
}

void execSetCameraRotation (void) {

	//	SetCameraRotation 
	//		Sets the value passed in to be the camera Rotation.
	//		PARAMS:	real[3]				camRot
	//		RETURN: NONE

	float* camRot = ABLi_popRealPtr();

	Stuff::Vector3D result(0.0,0.0,0.0);
	
	result.x = camRot[0];
	result.y = camRot[1];
	result.z = camRot[2];
 	
	if (eye)
		eye->setRotation(result);
}

void execSetCameraGoalRotation (void) {

	//	SetCameraGoalRotation 
	//		Sets the camera Rotation to the value passed in.
	//		PARAMS:	real[3]				camRot
	//				real				time
	//		RETURN: NONE

	float* camRot = ABLi_popRealPtr();
	float time = ABLi_popReal();

	Stuff::Vector3D result(0.0,0.0,0.0);
	result.x = camRot[0];
	result.y = camRot[1];
	result.z = camRot[2];
	
 	if (eye) {
		eye->setGoalRotation(result);
		eye->setGoalRotTime(time);
	}
}

void execGetCameraGoalRotation (void) {

	//	GetCameraGoalRotation
	//		Gets the camera Rotation into the value passed in.
	//		PARAMS:	real[3]				camRot
	//		RETURN: NONE

	const auto output = ABLi_popRealBuf();
	float camRot[3] = {};

	Stuff::Vector3D result(0.0,0.0,0.0);

	if (eye)
		result = eye->getGoalRotation();

	camRot[0] = result.x;
	camRot[1] = result.y;
	camRot[2] = result.z;
	abl::copyRealArray(output, camRot, "getcameragoalrotation_truncated");
}

void execGetCameraZoom (void) {

	//	GetCameraZoom
	//		Gets the camera FOV into the value passed in.
	//		PARAMS:	real				camFOV
	//		RETURN: NONE

	float result = 0.0;
   
 	if (eye)
		result = eye->getFieldOfView();
   
	ABLi_pushReal(result);	
}

void execSetCameraZoom (void) {

	//	SetCameraZoom
	//		Sets the camera FOV to the value passed in.
	//		PARAMS:	real				camFOV
	//		RETURN: NONE

	float camFOV = ABLi_popReal();

 	if (eye)
		eye->setFieldOfView(camFOV);
}

void execGetCameraGoalZoom (void) {

	//	GetCameraGoalZoom
	//		Gets the camera FOV into the value passed in.
	//		PARAMS:	real				camFOV
	//		RETURN: NONE

	float result = 0.0;
 	if (eye)
		result = eye->getFieldOfViewGoal();

	ABLi_pushReal(result);	
}

void execSetCameraGoalZoom (void) {

	//	SetCameraGoalZoom
	//		Sets the camera FOV to the value passed in.
	//		PARAMS:	real				camFOV
	//		RETURN: NONE

	float camFOV = ABLi_popReal();
	float time = ABLi_popReal();

 	if (eye)
		eye->setFieldOfViewGoal(camFOV, time);
}

void execSetCameraVelocity (void) {

	//	SetCameraVelocity
	//		Sets the camera velocity to the value passed in.
	//		PARAMS:	real[3]				camVel
	//		RETURN: NONE

	float* camVel = ABLi_popRealPtr();

	if (s_ablArgGuard && !camVel) {
		abl_arg_guard_log("execSetCameraVelocity", "camVel");
		return;
	}

	Stuff::Vector3D result(0.0,0.0,0.0);
	result.x = camVel[0];
	result.y = camVel[1];
	result.z = camVel[2];
	
 	if (eye)
		eye->setVelocity(result);
}

void execGetCameraVelocity (void) {

	//	GetCameraVelocity
	//		gets the camera velocity into the value passed in.
	//		PARAMS:	real[3]				camVel
	//		RETURN: NONE

	const auto output = ABLi_popRealBuf();
	float camVel[3] = {};

	Stuff::Vector3D result(0.0,0.0,0.0);

	if (eye)
		result = eye->getVelocity();

	camVel[0] = result.x;
	camVel[1] = result.y;
	camVel[2] = result.z;
	abl::copyRealArray(output, camVel, "getcameravelocity_truncated");
}

void execSetCameraGoalVelocity (void) {

	//	SetCameraGoalVelocity
	//		Sets the camera Velocity Goal to the value passed in.
	//		PARAMS:	real[3]				camVel
	//				real				time
	//		RETURN: NONE

	float* camVel = ABLi_popRealPtr();
	float time = ABLi_popReal();

	if (s_ablArgGuard && !camVel) {
		abl_arg_guard_log("execSetCameraGoalVelocity", "camVel");
		return;
	}

	Stuff::Vector3D result(0.0,0.0,0.0);
	result.x = camVel[0];
	result.y = camVel[1];
	result.z = camVel[2];
	
 	if (eye) {
		eye->setGoalVelocity(result);
		eye->setGoalVelTime(time);
	}
}

void execGetCameraGoalVelocity (void) {

	//	GetCameraGoalVelocity
	//		Sets the camera Velocity Goal into the value passed in.
	//		PARAMS:	real[3]				camVel
	//				real				time
	//		RETURN: NONE

	const auto output = ABLi_popRealBuf();
	float camVel[3] = {};

	Stuff::Vector3D result(0.0,0.0,0.0);

	if (eye)
		result = eye->getGoalVelocity();

	camVel[0] = result.x;
	camVel[1] = result.y;
	camVel[2] = result.z;
	abl::copyRealArray(output, camVel, "getcameragoalvelocity_truncated");
}

void execSetCameraLookObject (void) {

	//	SetCameraLookObject 
	//		Sets the camera Look Object
	//		PARAMS:	long				objectId
	//		RETURN: NONE

	long objectId = ABLi_popInteger();

 	if (eye)
		eye->setCameraTargetId(objectId);
}

void execGetCameraLookObject (void) {

	//	GetCameraGetLookObject
	//		Gets the camera FOV into the value passed in.
	//		PARAMS:	NONE
	//		RETURN: Integer

	long result = 0;
  	if (eye)
		result = eye->getCameraTargetId();

	ABLi_pushInteger(result);
}

void execGetCameraFrameLength (void) {

	//	GetCameraFrameLength
	//		Gets the Last Frame's Length
	//		PARAMS:	NONE
	//		RETURN: real

	ABLi_pushReal(frameLength);	
}

void execAnimationCallout (void)
{
	// Takes an integer for button num, bool for if its a button, bool for if it should press it, real for time to get there, integer for num flashes.
	// Returns a bool.  True if the animation played, false if an animation is running.
	
	long buttonId = ABLi_popInteger();
	bool isButton = ABLi_popBoolean();
	bool isPressed = ABLi_popBoolean();
	float timeToScroll = ABLi_popReal();
	long numFlashes = ABLi_popInteger();
	
	bool result = mission->missionInterface->startAnimation(buttonId,isButton,isPressed,timeToScroll,numFlashes);
	
	ABLi_pushBoolean(result);
}

static char tutorialMessages[MAX_CHAT_COUNT][1024];
static unsigned char currentMessage = 0;
void execTutorialText (void)
{
	//Takes an integer for TextID to send.
	// returns nothing.
	
	long textId = ABLi_popInteger();
	
	cLoadString(textId,tutorialMessages[currentMessage],1023);
	
	mission->missionInterface->setTutorialText(tutorialMessages[currentMessage]);
	
	currentMessage++;
	if (currentMessage >= MAX_CHAT_COUNT)
		currentMessage = 0;
}

void execGUIIsAOEStyle (void) 
{
	//Takes NOTHING
	// Returns state of GUI.
	
	ABLi_pushBoolean(mission->missionInterface->isAOEStyle());
}

void execSetInvulnerable (void) 
{
	//Takes a bool and sets local team invulnerability to the flag passed in.
	// Returns NOTHING.
	
	bool invulnerableFlag = ABLi_popBoolean();
	invulnerableON = invulnerableFlag;
}

void execFreezeGUI (void) 
{
	//Takes a bool and turns mouse input on or off based on flag passed in.
	// Returns NOTHING.
	
	bool guiFlag = ABLi_popBoolean();
	mission->missionInterface->freezeGUI(guiFlag);
}

void execLogisticsScreenId (void)
{
	//Takes NOTHING
	//Returns ID of logistics Screen we are IN.  Returns -1 if not in logistics or missionBegin is NULL!
	//singlePlayerScreens[0][1] = pMissionSelectionScreen;		ID 1
	//singlePlayerScreens[1][1] = pBriefingScreen;				ID 11
	//singlePlayerScreens[2][1] = pMechBayScreen;				ID 21
	//singlePlayerScreens[3][1] = pPilotSelectionScreen;		ID 31
	//singlePlayerScreens[2][0] = pPurchaseMechScreen;			ID 20
	//singlePlayerScreens[2][2] = pMechLabScreen;				ID 22
	//singlePlayerScreens[4][1] = pLoadScreen;					ID 41

	if (logistics && logistics->getMissionBegin())
	{
		ABLi_pushInteger(logistics->getMissionBegin()->getCurrentScreenId());
	}
	else
	{
		ABLi_pushInteger(-1);
	}
}

void execLogisticsAnimationCallout (void)
{
	// Takes an integer for button num, real for time to get there, integer for num flashes.
	// Returns a bool.  True if the animation played, false if an animation is running.
	
	long buttonId = ABLi_popInteger();
	bool isButton = ABLi_popBoolean();
	float timeToScroll = ABLi_popReal();
	long numFlashes = ABLi_popInteger();
	
	if (logistics && logistics->getMissionBegin())
	{
		bool result = logistics->getMissionBegin()->startAnimation(buttonId,isButton,timeToScroll,numFlashes);
		ABLi_pushBoolean(result);
	}
	else
	{
		ABLi_pushBoolean(false);
	}
}

void execLogisticsInCallout (void)
{
	//takes NOTHING
	// Returns a bool.  True if we are currently animating the mouse to the button area OR no logistics, false if not animating

	if (logistics && logistics->getMissionBegin())
	{
		ABLi_pushBoolean(logistics->getMissionBegin()->isInCalloutAnimation());
	}
	else
	{
		ABLi_pushBoolean(true);
	}
}

void execInCallout (void)
{
	//takes NOTHING
	// Returns a bool.  True if we are currently animating the mouse to the button area OR no logistics, false if not animating

	if (mission && mission->missionInterface)
	{
		ABLi_pushBoolean(mission->missionInterface->isInCalloutAnimation());
	}
	else
	{
		ABLi_pushBoolean(true);
	}
}

void execIsPlayingVoiceOver(void)
{
	//Takes NOTHING
	// Returns a bool,  true if voiceover channel is active, false if not.

	ABLi_pushBoolean(soundSystem->isPlayingVoiceOver());
}

void execStopVoiceOver(void)
{
	//Takes NOTHING
	// Returns a NOTHING

	soundSystem->stopSupportSample();
}

void execGetLogisticsTime(void)
{
	// The script clock, in milliseconds of mission time.
	// Intro scripts time their camera cuts against it, next to camera goal times that run on mission time. A wall
	// clock here made the cut land on a different frame every run.
	ABLi_pushReal(mission ? mission->actualTime * 1000.0 : 0.0);
}

void execRequestHelp (void) {

	// Defender is the CurPilot (calling this function).

	long enemyPartID = ABLi_popInteger();
	float* friendlyCenter = ABLi_popRealPtr();
	float friendlyRadius = ABLi_popReal();
	float* enemyCenter = ABLi_popRealPtr();
	float enemyRadius = ABLi_popReal();
	long priority = ABLi_popInteger();

	if (CurWarrior->getTeam()) {
		Stuff::Vector3D friendlyPos;
		friendlyPos.x = friendlyCenter[0];
		friendlyPos.y = friendlyCenter[1];
		friendlyPos.z = 0.0;
		MoverPtr friendlies[MAX_MOVERS];
		long commanderID = -1;
		if (MPlayer || (CurWarrior->getTeam() == Team::home)) {
			CommanderPtr _cmdr = CurWarrior->getCommander();
			if (s_ablArgGuard && !_cmdr)
				abl_arg_guard_log("execRequestHelp", "commander");
			else if (_cmdr)
				commanderID = _cmdr->getId();
			else
				commanderID = CurWarrior->getCommander()->getId();
		}
		long numFriendlies = getMoversWithinRadius(friendlies, friendlyPos, friendlyRadius, CurWarrior->getTeam()->getId(), commanderID, false, true, priority > 0);

		GameObjectPtr enemy = ObjectManager->findByPartId(enemyPartID);
		if (enemy && enemy->getTeam() && (enemy->getTeam() != CurWarrior->getTeam())) {
			Stuff::Vector3D enemyPos;
			enemyPos.x = enemyCenter[0];
			enemyPos.y = enemyCenter[1];
			enemyPos.z = 0.0;
			MoverPtr enemies[MAX_MOVERS];
			long numEnemies = getMoversWithinRadius(enemies, enemyPos, enemyRadius, enemy->getTeamId(), -1, false, true, true);
			calcAttackPlan(numFriendlies, (GameObjectPtr*)friendlies, numEnemies, (GameObjectPtr*)enemies);
		}
	}
	
	ABLi_pushReal(0.0);
}

void execRequestTarget (void) {

	float* center = ABLi_popRealPtr();
	float radius = ABLi_popReal();

	if (CurWarrior->getTeam()) {
		Stuff::Vector3D friendlyPos;
		friendlyPos.x = center[0];
		friendlyPos.y = center[1];
		friendlyPos.z = 0.0;
		MoverPtr friendlies[MAX_MOVERS];
		MoverPtr enemies[MAX_MOVERS];
		long commanderID = -1;
		if (MPlayer || (CurWarrior->getTeam() == Team::home)) {
			CommanderPtr _cmdr = CurWarrior->getCommander();
			if (s_ablArgGuard && !_cmdr)
				abl_arg_guard_log("execRequestTarget", "commander");
			else if (_cmdr)
				commanderID = _cmdr->getId();
			else
				commanderID = CurWarrior->getCommander()->getId();
		}
		long numFriendlies = getMoversWithinRadius(friendlies, friendlyPos, radius, CurWarrior->getTeam()->getId(), commanderID, false, true, true);
		long numEnemies = getMoversWithinRadius(enemies, friendlyPos, radius, CurWarrior->getTeam()->getId(), -1,true, true, true);
		GameObjectPtr bestTarget = nullptr;
		if (numEnemies > 0)
			bestTarget = calcBestTarget(CurWarrior->getVehicle(), numFriendlies, friendlies, numEnemies, enemies);
	
		if (bestTarget)
			ABLi_pushInteger(bestTarget->getPartId());
		else
			ABLi_pushInteger(0);
	}
}

constexpr int MAX_SHELTERS = 10;

void execRequestShelter (void) {

	float range = ABLi_popIntegerReal();

	long numValidBuildings = 0;
	BuildingPtr validBuildings[MAX_SHELTERS];
	long numBuildings = ObjectManager->getNumBuildings();
	for (long i = 0; i < numBuildings; i++) {
		BuildingPtr building = ObjectManager->getBuilding(i);
		if (CurObject->distanceFrom(building->getPosition()) < range) {
			validBuildings[numValidBuildings++] = building;
			if (numValidBuildings == MAX_SHELTERS)
				break;
		}
	}
	if (numValidBuildings > 0) {
		long buildingIndex = RandomNumber(numValidBuildings);
		ABLi_pushInteger(validBuildings[buildingIndex]->getPartId());
		}
	else
		ABLi_pushInteger(0);
}
