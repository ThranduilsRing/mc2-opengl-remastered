#include "tick_commit.h"
//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//								WARRIOR.CPP

#include "warrior_internal.h"
#include "mc2fmt.h"



BldgAppearance*		MechWarrior::wayPointMarkers[3] = 
{	nullptr,nullptr,nullptr };
GoalManager* MechWarrior::goalManager = nullptr;
#ifdef USE_SCENARIO
extern turn;
#endif

float FireOddsTable[NUM_FIREODDS] = {
	20.0,
	35.0,
	50.0,
	65.0,
	80.0
};

const char* pilotAlarmFunctionName[NUM_PILOT_ALARMS] = {
	"handletargetofweaponfire",
	"handlehitbyweaponfire",
	"handledamagetakenrate",
	"handledeathofmate",
	"handlecripplingoffriendlyvehicle",
	"handledestructionoffriendlyvehicle",
	"handleincapacitationofvehicle",
	"handledestructionofvehicle",
	"handlewithdraw",
	"handleattackorder",
	"handlecollision",
	"handleguardbreach",
	"handlekilledtarget",
	"handlematefiredweapon",
	"handleplayerorder",
	"handlenomovepath",
	"handlegateclosing",
	"handlefiredweapon",
	"handlenewmover"
};

// sebi: those are not used, only filled in mover.cpp
/*
const char ProfessionalismOffsetTable[NUM_OFFSET_RANGES][2] = {
	{10, 10},
	{20, 5},
	{30, 0},
	{40, 5},
	{100, -10}
};

const char DecorumOffsetTable[NUM_OFFSET_RANGES][2] = {
	{10, 10},
	{20, 5},
	{30, 0},
	{40, 5},
	{100, -10}
};

const char AmmoConservationModifiers[2][2] = {
	{50, -5},
	{20, -10}
};
*/

static const char* SkillsTable[NUM_SKILLS] = {
	"Piloting",
	"Sensors",
	"Gunnery"
};

// First column is ThreatRating, second column is
// for Inner Sphere, third column is Clan...
float ThreatRatingEffect[8][3] = {
	{0.5f, -1.0f, 0.0f},
	{0.75f, -0.2f, 0.0f},
	{1.0f, 0.0f, 0.0f},
	{1.25f, 0.0f, 0.0f},
	{1.5f, 0.0f, 0.0f},
	{2.0f, 0.1f, 0.0f},
	{2.25f, 0.2f, -0.1f},
	{1000000.0f, 0.2f, -1.0f}
};

float BrainUpdateFrequency = 2.25;
float MovementUpdateFrequency = 5.0;
float CombatUpdateFrequency = 0.25;
float CommandUpdateFrequency = 6.0;
float PilotCheckUpdateFrequency = 1.0;
float ThreatAnalysisFrequency = 5.0;

long PilotCheckModifierTable[2] = {
	25,
	25
};

float SkillWeightings[NUM_SKILLS] =
{
	1.0,
	1.0,
	1.0
};

float WarriorRankScale[NUM_WARRIOR_RANKS] = {
	50.0,
	60.0,
	70.0,
	79.0,
	999.0
};

const char *SpecialtySkillsTable[NUM_SPECIALTY_SKILLS] = {
	"LightMechSpecialist",
	"LaserSpecialist",
	"LightACSpecialist",
	"MediumACSpecialist",
	"SRMSpecialist",
	"SmallArmsSpecialist",
	"SensorProfileSpecialist",
	"ToughnessSpecialist",			//Thoughness Specialty

	"MediumMechSpecialist",
	"PulseLaserSpecialist",
	"ERLaserSpecialist",
	"LRMSpecialist",
	"Scout",						//Scouting Specialty
	"LongJump",						//Jump Jet Specialty

	"HevayMechSpecialist",			//Heavy mech Specialty
	"PPCSpecialist",
	"HeavyACSpecialist",
	"ShortRangeSpecialist",
	"MediumRangeSpecialist",
	"LongRangeSpecialist",

	"AssaultMechSpecialist",
	"GaussSpecialist",
	"SharpShooter",					//Sharpshooter specialty
};

int32_t			MechWarrior::numWarriors = 0;
int32_t			MechWarrior::numWarriorsInCombat = 0;
bool			MechWarrior::brainsEnabled[MAX_TEAMS] = {true, true, true, true, true, true, true, true};
float			MechWarrior::minSkill = 1;
float			MechWarrior::maxSkill = 100;
int32_t			MechWarrior::increaseCap = 100;
float			MechWarrior::maxVisualRadius = 100.0;
int32_t			MechWarrior::curEventID = 0;
int32_t			MechWarrior::curEventTrigger = 0;
MechWarrior*	MechWarrior::warriorList[MAX_WARRIORS];

long LastMoveCalcErr = 0;
long GroupMoveTrailLen[2] = {0, 1};
float MoveTimeOut = 30.0;
float MoveYieldTime = 1.5;
float DefaultAttackRadius = 275.0;

QueuedTacOrder TacOrderQueue[MAX_QUEUED_TACORDERS];
long TacOrderQueuePos = 0;

SpecialtySkillType	MechWarrior::skillTypes[NUM_SPECIALTY_SKILLS] = 
{
	CHASSIS_SPECIALTY,		//	"LightMechSpecialist",
	WEAPON_SPECIALTY,		//	"LaserSpecialist",
	WEAPON_SPECIALTY,		//	"LightACSpecialist",
	WEAPON_SPECIALTY,		//	"MediumACSpecialist",
	WEAPON_SPECIALTY,		//	"SRMSpecialist",
	WEAPON_SPECIALTY,		//	"SmallArmsSpecialist",
	OTHER_SPECIALTY,		//	"SensorProfileSpecialist",
	OTHER_SPECIALTY,		//	"ToughnessSpecialist",	
		
	CHASSIS_SPECIALTY,		//	"MediumMechSpecialist",
	WEAPON_SPECIALTY,		//	"PulseLaserSpecialist",
	WEAPON_SPECIALTY,		//	"ERLaserSpecialist",
	WEAPON_SPECIALTY,		//	"LRMSpecialist",
	OTHER_SPECIALTY,		//	"Scout",				
	OTHER_SPECIALTY,		//	"LongJump",				
	
	CHASSIS_SPECIALTY,		//	"HevayMechSpecialist",	
	WEAPON_SPECIALTY,		//	"PPCSpecialist",
	WEAPON_SPECIALTY,		//	"HeavyACSpecialist",
	RANGE_SPECIALTY,		//	"ShortRangeSpecialist",
	RANGE_SPECIALTY,		//	"MediumRangeSpecialist",
	RANGE_SPECIALTY,		//	"LongRangeSpecialist",
	
	CHASSIS_SPECIALTY,		//	"AssaultMechSpecialist",
	WEAPON_SPECIALTY,		//	"GaussSpecialist",
	OTHER_SPECIALTY,		//	"SharpShooter",			
};	
GameObjectPtr LastCoreAttackTarget = nullptr;

// MISC routines

long SignedRandomNumber (long range) {

	return(RandomNumber(range << 1) - range);
}

long GetMissionTurn (void) {

	return(turn);
}

// MECHWARRIOR class routines

void* MechWarrior::operator new (size_t mySize) {

	void *result = ::operator new(mySize);
	// Save files copy whole member arrays of a pilot, so bytes no constructor sets must be defined: zero.
	if (result)
		memset(result, 0, mySize);
	return(result);
}

void MechWarrior::operator delete (void* us) {

	::operator delete(us);
}

void MechWarrior::lobotomy (void) {

	delete brain;
	brain = nullptr;
	
	// Set up the pilot alarm callback table...
	for (long i = 0; i < NUM_PILOT_ALARMS; i++)
		brainAlarmCallback[i] = nullptr;
}	

void MechWarrior::init (bool create) {

	used = false;
	name[0] = '\0';
	callsign[0] = '\0';
	videoStr[0] = '\0';
	audioStr[0] = '\0';
	brainStr[0] = '\0';
	photoIndex = 0;
	
	rank = WARRIOR_RANK_GREEN;
	for (int i = 0; i < NUM_SKILLS; i++) {
		for (int j = 0; j < NUM_COMBAT_STATS; j++) {
			numSkillUses[i][j] = 0;
			numSkillSuccesses[i][j] = 0;
		}
		skillRank[i] = 0.0;
		skillPoints[i] = 0.0;
        //sebi: WTF??? ORIG BUG FIX
		//originalSkills[NUM_SKILLS] = 0;
		//startingSkills[NUM_SKILLS] = 0;
        // maybe like this
		originalSkills[i] = 0;
		startingSkills[i] = 0;
		
	}
	professionalism = 40;
	professionalismModifier = 0;
	decorum = 40;
	decorumModifier = 0;
	aggressiveness = 40;
	courage = 40;
	baseCourage = courage;

	wounds = 0.0;
	health = 0.0;
	status = WARRIOR_STATUS_NORMAL;
	escapesThruEjection = false;
	radioLog.lastMessage = -1;
	radioLog.lastUnderFire = -1000.0f;
	radioLog.weaponsIneffective = false;
	radioLog.weaponsOut = false;
	radioLog.lastContact = -1000.0f;
    //sebi init those 2
	radioLog.lastMessageType = RADIO_MESSAGE_COUNT;
	radioLog.lastMessageTime = -1000.0f;
        
	notMineYet = false;

	teamId = 0;
	vehicleWID = 0;

	numKilled = 0;
	memset( killed, 0, sizeof( GameObject* ) * MAX_WARRIOR_KILLED );

	for (int i = 0; i < NUM_VEHICLE_CLASSES; i++)
		for (int j = 0; j < NUM_COMBAT_STATS; j++)
			numMechKills[i][j] = 0;

	for (int i = 0; i < NUM_PHYSICAL_ATTACKS; i++)
		for (int j = 0; j < NUM_COMBAT_STATS; j++)
			numPhysicalAttacks[i][j] = 0;

	for (int i = 0; i < NUM_SPECIALTY_SKILLS; i++)
		specialtySkills[i] = false;

	descID = 0;
	nameIndex = 0;

	keepMoving = false;

	timeOfLastOrders = -1.0;
	numAttackers = 0;
	attackRadius = DefaultAttackRadius;

	// ABL Brain and Memory
	for (int i = 0; i < NUM_MEMORY_CELLS; i++)
		memory[i].integer = 0;
	brain = nullptr;
	brainTaskQueue = nullptr;  // created on first brain fire if gate enabled
	brainRuntime = nullptr;    // MC2_BRAIN_RUNTIME: created on first brain cadence tick if gate enabled
	for (int i = 0; i < NUM_PILOT_ALARMS; i++)
		brainAlarmCallback[i] = nullptr;

	for (int i = 0; i < NUM_PILOT_DEBUG_STRINGS; i++)
		debugStrings[i][0] = '\0';

	brainUpdate = static_cast<float>((numWarriors % 30)) * 0.2;
	combatUpdate = static_cast<float>((numWarriors % 15)) * 0.1;
	movementUpdate = static_cast<float>((numWarriors % 15)) * 0.2;
	for (long w = 0; w < MAX_WEAPONS_PER_MOVER; w++)
		weaponsStatus[w] = 0;
	weaponsStatusResult = WEAPONS_STATUS_NO_TARGET;

	useGoalPlan = false;
	mainGoalAction = GOAL_ACTION_NONE;
	mainGoalObjectWID = 0;
	mainGoalLocation = glm::vec3(0.0f);
	mainGoalControlRadius = -1.0;
	lastGoalPathSize = 0;

	newTacOrderReceived[ORDERSTATE_GENERAL] = false;
	newTacOrderReceived[ORDERSTATE_PLAYER] = false;
	newTacOrderReceived[ORDERSTATE_ALARM] = false;

	tacOrder[ORDERSTATE_GENERAL].init();
	tacOrder[ORDERSTATE_PLAYER].init();
	tacOrder[ORDERSTATE_ALARM].init();
	lastTacOrder.init();
	curTacOrder.init();

	clearAlarms();
	clearAlarmsHistory();

	alarmPriority = 0;

	resetQueueMarkers();	// B7o.4: a pooled warrior re-initialised over a previous run owns no stale markers
	tacOrderQueue = nullptr;
	enableTacOrderQueue();
	curPlayerOrderFromQueue = false;
	tacOrderQueueLocked = false;
	tacOrderQueueExecuting = false;
	tacOrderQueueLooping = false;
	numTacOrdersQueued = 0;
	tacOrderQueueIndex = 0;
	nextTacOrderId = 0;
	lastTacOrderId = -1;

	coreScanTargetWID = 0;
	coreAttackTargetWID = 0;
	coreMoveTargetWID = 0;
	targetPriorityList.init();
	brainState = 0;
	willHelp = true;

	if (create)
		moveOrders.init();

	attackOrders.init();
	situationOrders.init();
	lastTargetWID = 0;
	lastTargetTime = -1.0;
	lastTargetObliterate = false;
	lastTargetFriendly = false;
	orderState = ORDERSTATE_GENERAL;
	movePathRequest = nullptr;
	// PATHFINDING-JUMP-FAIL-1: no doomed jump yet (-1 = never failed).
	lastFailedJumpGoalCellR = -1;
	lastFailedJumpGoalCellC = -1;
	lastJumpFailFrame = -1;

	if (create)
	{
		for (int i = 0; i < 2; i++) 
		{
			moveOrders.path[i] = new MovePath;
			if (!moveOrders.path[i])
				Fatal(0, " No RAM for warrior path ");
		}
	}

	debugFlags = WARRIOR_DEBUG_FLAG_NONE;
	radio = nullptr;
	isPlayingMsg = false;
	oldPilot = 0;

    lastTargetConserveAmmo = false; // sebi: int to not contain garbage

	numWarriors++;
}

long MechWarrior::init (FitIniFile* warriorFile) {

	// If we want to, we could make the following tables part of an ini file
	// that defines which skills, etc. are being used in the game system.
	// This way, non-programming designers could modify which skills are
	// being used (and so on), without having to change the underlying
	// system code. The global defines, NUM_SKILLS, etc. would also have to
	// a part of the ini file...

	// Main Info section
	long result = warriorFile->seekBlock("General");
	if (result != NO_ERR)
		return(result);
	
	result = warriorFile->readIdInt("DescIndex", descID);
	if (result != NO_ERR)
		descID = -1;

	cLoadString( descID, name, MAXLEN_PILOT_NAME - 1);
		
	result = warriorFile->readIdInt("NameIndex", nameIndex);
	if (result != NO_ERR)
		nameIndex = -1;

	result = warriorFile->readIdBoolean("NotMineYet",notMineYet);
	if (result != NO_ERR)
		notMineYet = false;
		
	result = warriorFile->readIdInt("PictureIndex", photoIndex);
	if (result != NO_ERR)
		photoIndex = 0;

	strcpy( callsign, name );
    const int len = strlen( callsign );
	for ( int i = 0; i < len; i ++ )
		CharUpper( callsign );
		
	result = warriorFile->readIdUChar("OldPilot",oldPilot);
	if (result != NO_ERR)
		oldPilot = 0;
	
	result = warriorFile->readIdString("pilotAudio",audioStr, MAXLEN_PILOT_AUDIO - 1);
	if (result != NO_ERR)
	{
		radio = nullptr;		//No pilot Audio, so no radio.
	}
	else
	{
		result = warriorFile->readIdString("pilotVideo",videoStr,MAXLEN_PILOT_VIDEO-1);
		if (result != NO_ERR)
		{
			videoStr[0] = '\0';
		}
		
		radio = new Radio;
		result = radio->init(audioStr,102400,videoStr);		//Size of radio heap should be data driven?
		if (result != NO_ERR)
		{
			delete radio;	//Radio couldn't start, so no radio.
			radio = nullptr;
		}
	}
	
	result = warriorFile->readIdInt("PaintScheme", paintScheme);
	if (result != NO_ERR)
		paintScheme = -1;		//This means mech is its normal color.
	
	result = warriorFile->seekBlock("PersonalityTraits");
	if (result != NO_ERR)
		return(result);

	result = warriorFile->readIdChar("Professionalism", professionalism);
	if (result != NO_ERR)
		return(result);

	result = warriorFile->readIdChar("Decorum", decorum);
	if (result != NO_ERR)
		return(result);

	result = warriorFile->readIdChar("Aggressiveness", aggressiveness);
	if (result != NO_ERR)
		return(result);

	result = warriorFile->readIdChar("Courage", courage);
	if (result != NO_ERR)
		return(result);
	baseCourage = courage;

	result = warriorFile->seekBlock("Skills");
	if (result != NO_ERR)
		return(result);
		
	int i=0;
	for (i = 0; i < NUM_SKILLS; i++) {
		result = warriorFile->readIdChar(SkillsTable[i], skills[i]);
		if (result != NO_ERR)
			return(result);
			
		skillRank[i] = skills[i];
	}

	result = warriorFile->seekBlock("SpecialtySkills");
	if (result == NO_ERR)
	{
		for (i = 0; i < NUM_SPECIALTY_SKILLS; i++) 
		{
			char tmpChar;
			result = warriorFile->readIdChar(SpecialtySkillsTable[i], tmpChar);
			if (result == NO_ERR)
				specialtySkills[i] = (tmpChar == 1);
		}
	}

	result = warriorFile->seekBlock("OriginalSkills");
	if (result == NO_ERR)
	{
		for (i = 0; i < NUM_SKILLS; i++) 
		{
			result = warriorFile->readIdChar(SkillsTable[i], originalSkills[i]);
			if (result != NO_ERR)
				originalSkills[i] = skills[i];
		}
	}
	else
	{
		for (i = 0; i < NUM_SKILLS; i++) 
		{
			originalSkills[i] = skills[i];
		}
	}

	result = warriorFile->seekBlock("LatestSkills");
	if (result == NO_ERR)
	{
		for (i = 0; i < NUM_SKILLS; i++) 
		{
			result = warriorFile->readIdChar(SkillsTable[i], startingSkills[i]);
			if (result != NO_ERR)
				startingSkills[i] = skills[i];
		}
	}
	else
	{
		for (i = 0; i < NUM_SKILLS; i++) 
		{
			startingSkills[i] = skills[i];
		}
	}

	result = warriorFile->seekBlock("SkillPoints");
	if (result != NO_ERR)
	{
		for (long i = 0; i < NUM_SKILLS; i++)
		{
			skillPoints[i] = 0.0;
		}
	}
	else
	{
		for (long i = 0; i < NUM_SKILLS; i++) 
		{
			result = warriorFile->readIdFloat(SkillsTable[i], skillPoints[i]);
			if (result != NO_ERR)
				skillPoints[i] = 0.0;
		}
	}

	// Calc warrior's rank...
	calcRank();

	result = warriorFile->seekBlock("Status");
	if (result != NO_ERR)
		return(result);
			
	char wnds;
	result = warriorFile->readIdChar("Wounds", wnds);
	if (result != NO_ERR)
		return(result);
	wounds = static_cast<float>(wnds);

	long roll = RandomNumber(100);
	escapesThruEjection = ((roll <= (getSkill(MWS_PILOTING) + 30)) && (roll < 95));
	if (MPlayer || isToughnessSpecialist())
		escapesThruEjection = true;

	enableTacOrderQueue();

	return NO_ERR;

}
						

void MoveOrders::init (void) {

	time = scenarioTime;
	origin = ORDER_ORIGIN_COMMANDER;
    speedType = std::to_underlying(MOVE_SPEED_MODERATE);
	speedVelocity = 0.0;
	speedState = MECH_STATE_WALKING;
	speedThrottle = 100;
/*
	moveOrders[which].zone.angle[0] = 45.0;
	moveOrders[which].zone.angle[1] = 90.0;
	moveOrders[which].zone.range[0] = 60.0;
	moveOrders[which].zone.range[1] = 90.0;
	moveOrders[which].pattern = MOVE_PATTERN_DIRECT;
*/		
	goalType = MOVEGOAL_NONE;
	goalObjectWID = 0;
	goalObjectPosition = glm::vec3(0.0f);
	// Since world coord should never reach -999999...
	goalLocation.x = -999999.0;
	goalLocation.y = -999999.0;
	goalLocation.z = -999999.0;

	nextUpdate = 0.0;
	newGoal = false;
	numWayPts = 0;
	curWayPt = 0;
	curWayDir = 0;
	pathType = MOVEPATH_UNDEFINED;
	originalGlobalGoal[0].x = -999999.0;
	originalGlobalGoal[0].y = -999999.0;
	originalGlobalGoal[0].z = -999999.0;
	originalGlobalGoal[1].x = -999999.0;
	originalGlobalGoal[1].y = -999999.0;
	originalGlobalGoal[1].z = -999999.0;
	globalGoalLocation.x = -666666.0;
	globalGoalLocation.y = -666666.0;
	globalGoalLocation.z = -666666.0;
	numGlobalSteps = 0;
	curGlobalStep = 0;
	path[0] = nullptr;
	path[1] = nullptr;
	timeOfLastStep = -1.0;
	moveState = MOVESTATE_FORWARD;
	moveStateGoal = MOVESTATE_FORWARD;
	twisting = false;
	yieldTime = -1.0;
	yieldState = 0;
	waitForPointTime = -1.0;
	
	run = false;
}

void AttackOrders::init (void) {

	time = scenarioTime;
	origin = ORDER_ORIGIN_COMMANDER;
	type = ATTACK_NONE;
	targetWID = 0;
	targetPoint = glm::vec3(0.0f);
	aimLocation = -1;
	pursue = false;
	targetTime = 0.0;
}

void SituationOrders::init (void) {

	time = 0.0;
	mode = SITUATION_NORMAL;
	defFormation = 10.0;
	curFormation = 10.0;
	openFire = false;
	uponFireOnly = false;
	fireRange = -1.0;
	fireOdds = -1.0;
	guardObjectWID = 0;
}


void MechWarrior::radioMessage (long message, bool propogateIfMultiplayer) 
{
bool limitFrequency = true;

	if (message >= RADIO_MESSAGE_COUNT)
		return;

  	if (radio && active() && (message != -1) && GetMissionTurn() > 0) 
	{
		if (underHomeCommand()) 
		{
			switch (message) 
			{
				case RADIO_WEAPONS_OUT:
					if (radioLog.weaponsOut)
						return;
					radioLog.weaponsOut = true;
					break;
				case RADIO_SENSOR_CONTACT:
					if (radioLog.lastContact > (scenarioTime - 15.0))
						return;
					radioLog.lastContact = scenarioTime;
					break;
				case RADIO_UNDER_ATTACK:
					if (radioLog.lastUnderFire > (scenarioTime - 20.0))
						return;
					radioLog.lastUnderFire = scenarioTime;
					break;
			}
			switch (message)
			{
			case RADIO_TARGET_ACK:
			case RADIO_MOVE_BLOCKED:
			case RADIO_ILLEGAL_ORDER:
				limitFrequency = false;
			}

			if (limitFrequency != 0 && radioLog.lastMessageType == message &&
					radioLog.lastMessageTime > (scenarioTime - 10.0))
				return;
				
			radioLog.lastMessageTime = scenarioTime;
            // Radios are mission-owned. A reused pilot slot receives another radio.
            auto speak = [this, requestedRadio = radio, message] {
                if (radio == requestedRadio)
                    radioLog.lastMessage = requestedRadio->playMessage(static_cast<RadioMessageType>(message));
            };
            if (!mc2::tick::defer(mc2::tick::Queue::Sound, speak)) speak();
			radioLog.lastMessageType = static_cast<RadioMessageType>(message);
		}
		//According to Derek this never happens.  ONLY your pilots speak!!
		/*
		else if (MPlayer && MPlayer->isServer() && propogateIfMultiplayer) 
		{
			getVehicle()->addRadioChunk(CHUNK_SEND, (unsigned char)message);
		}
		*/
	}
}

void MechWarrior::clear (void) {

	// Before we do anything, make sure the warrior's orders are cleared and
	// pulled from any combat...
	clearCurTacOrder();

	// Now, let's clear 'em...
	used = false;
	name[0] = '\0';
	callsign[0] = '\0';
	videoStr[0] = '\0';
	audioStr[0] = '\0';
	brainStr[0] = '\0';
	
	rank = WARRIOR_RANK_GREEN;
	for (int i = 0; i < NUM_SKILLS; i++) {
		for (int j = 0; j < NUM_COMBAT_STATS; j++) {
			numSkillUses[i][j] = 0;
			numSkillSuccesses[i][j] = 0;
		}
		skillRank[i] = 0.0;
		skillPoints[i] = 0.0;
        //sebi: WTF???
		//originalSkills[NUM_SKILLS] = 0;
		//startingSkills[NUM_SKILLS] = 0;
		originalSkills[i] = 0;
		startingSkills[i] = 0;
	}
	professionalism = 40;
	professionalismModifier = 0;
	decorum = 40;
	decorumModifier = 0;
	aggressiveness = 40;
	courage = 40;
	baseCourage = courage;

	wounds = 0.0;
	health = 0.0;
	status = WARRIOR_STATUS_NORMAL;
	escapesThruEjection = false;
	radioLog.lastMessage = -1;
	radioLog.lastUnderFire = -1000.0;
	radioLog.weaponsIneffective = false;
	radioLog.weaponsOut = false;
	radioLog.lastContact = -1000.0;
	notMineYet = false;

	teamId = 0;
	vehicleWID = 0;

	numKilled = 0;
	memset( killed, 0, sizeof( GameObject* ) * MAX_WARRIOR_KILLED );

	for (int i = 0; i < NUM_VEHICLE_CLASSES; i++)
		for (int j = 0; j < NUM_COMBAT_STATS; j++)
			numMechKills[i][j] = 0;

	for (int i = 0; i < NUM_PHYSICAL_ATTACKS; i++)
		for (int j = 0; j < NUM_COMBAT_STATS; j++)
			numPhysicalAttacks[i][j] = 0;

	for (int i = 0; i < NUM_SPECIALTY_SKILLS; i++)
		specialtySkills[i] = false;

	descID = 0;
	nameIndex = 0;

	timeOfLastOrders = -1.0;
	numAttackers = 0;
	attackRadius = DefaultAttackRadius;

	// ABL Brain and Memory
	for (int i = 0; i < NUM_MEMORY_CELLS; i++)
		memory[i].integer = 0;

	//OK if brain is NULL. Like for Infantry!!
	if (brain)
		brain->init(-1);

	for (int i = 0; i < NUM_PILOT_ALARMS; i++)
		brainAlarmCallback[i] = nullptr;

	for (int i = 0; i < NUM_PILOT_DEBUG_STRINGS; i++)
		debugStrings[i][0] = '\0';

	//brainUpdate = (float)(numWarriors % 30) * 0.2;
	//combatUpdate = (float)(numWarriors % 15) * 0.1;
	//movementUpdate = (float)(numWarriors % 15) * 0.2;
	for (long w = 0; w < MAX_WEAPONS_PER_MOVER; w++)
		weaponsStatus[w] = 0;
	weaponsStatusResult = WEAPONS_STATUS_NO_TARGET;

	newTacOrderReceived[ORDERSTATE_GENERAL] = false;
	newTacOrderReceived[ORDERSTATE_PLAYER] = false;
	newTacOrderReceived[ORDERSTATE_ALARM] = false;

	tacOrder[ORDERSTATE_GENERAL].init();
	tacOrder[ORDERSTATE_PLAYER].init();
	tacOrder[ORDERSTATE_ALARM].init();
	lastTacOrder.init();
	curTacOrder.init();

	for (long curAlarm = 0; curAlarm < NUM_PILOT_ALARMS; curAlarm++)
		clearAlarm(curAlarm);

	alarmPriority = 0;

	//tacOrderQueue = NULL;
	enableTacOrderQueue();
	curPlayerOrderFromQueue = false;
	tacOrderQueueLocked = false;
	tacOrderQueueExecuting = false;
	tacOrderQueueLooping = false;
	numTacOrdersQueued = 0;
	tacOrderQueueIndex = 0;
	nextTacOrderId = 0;
	lastTacOrderId = -1;

	coreScanTargetWID = 0;
	coreAttackTargetWID = 0;
	coreMoveTargetWID = 0;
	targetPriorityList.init();
	brainState = 0;

	MovePathPtr path1 = moveOrders.path[0];
	path1->numSteps = path1->numStepsWhenNotPaused = 0;
	MovePathPtr path2 = moveOrders.path[1];
	path2->numSteps = path2->numStepsWhenNotPaused = 0;
	moveOrders.init();
	moveOrders.path[0] = path1;
	moveOrders.path[1] = path2;

	attackOrders.init();
	situationOrders.init();
	lastTargetWID = 0;
	lastTargetTime = -1.0;
	lastTargetObliterate = false;
	lastTargetFriendly = false;
	orderState = ORDERSTATE_GENERAL;
	movePathRequest = nullptr;
	// PATHFINDING-JUMP-FAIL-1: no doomed jump yet (-1 = never failed).
	lastFailedJumpGoalCellR = -1;
	lastFailedJumpGoalCellC = -1;
	lastJumpFailFrame = -1;
	clearMoveOrders();
	//for (i = 0; i < 2; i++) {
	//	moveOrders.path[i] = new MovePath;
	//	if (!moveOrders.path[i])
	//		Fatal(0, " No RAM for warrior path ");
	//}
	debugFlags = WARRIOR_DEBUG_FLAG_NONE;
	//radio = NULL;
	oldPilot = 0;

}

MechWarrior::~MechWarrior (void) {
	destroy();
}

void MechWarrior::destroy (void) {

	// Queued waypoint markers are owned appearances; mission end used to sweep them up with the appearance heap.
	clearTacOrderQueue();

	if (brain) {
		delete brain;
		brain = nullptr;
	}

	delete brainTaskQueue;
	brainTaskQueue = nullptr;
	delete brainRuntime;
	brainRuntime = nullptr;

	for (long i = 0; i < 2; i++)
		if (moveOrders.path[i]) {
			delete moveOrders.path[i];
			moveOrders.path[i] = nullptr;
		}

	numWarriors--;
}

long MechWarrior::getAggressiveness (bool current) {

	if (current && curTacOrder.isCombatOrder())
		return(aggressiveness + (100 - aggressiveness) / 2);
	else
		return(aggressiveness);
}

// QUEUED PLAYER ORDERS end

void MechWarrior::setMainGoal (long action, GameObjectPtr obj, glm::vec3* location, float range) {

	mainGoalAction = action;
	if (obj)
		mainGoalObjectWID = obj->getWatchID();
	else
		mainGoalObjectWID = 0;
	if (location)
		mainGoalLocation = *location;
	else
		mainGoalLocation = glm::vec3(0.0f);
	mainGoalControlRadius = range;
}

long MechWarrior::getMainGoal (GameObjectPtr& obj, glm::vec3& location, float& radius) {

	obj = nullptr;
	if (mainGoalObjectWID)
		obj = ObjectManager->getByWatchID(mainGoalObjectWID);
	location = mainGoalLocation;
	radius = mainGoalControlRadius;
	return(mainGoalAction);
}

MoverGroupPtr MechWarrior::getGroup (void) {

	return(nullptr);
}

MoverPtr MechWarrior::getPoint (void) {

	return(nullptr);
}

bool MechWarrior::onHomeTeam (void) {

	return(getTeam() == Team::home);
}

bool MechWarrior::underHomeCommand (void) {

	MoverPtr myVehicle = getVehicle();
	if (myVehicle)
		return(myVehicle->getCommanderId() == Commander::home->getId());
	return(false);

}

TeamPtr MechWarrior::getTeam (void) {

	if (teamId > -1)
		return(Team::teams[teamId]);
	return(nullptr);
}

CommanderPtr MechWarrior::getCommander (void) {

	MoverPtr myVehicle = getVehicle();
	if (myVehicle && (myVehicle->commanderId > -1))
		return(Commander::commanders[myVehicle->commanderId]);
	return(nullptr);
}

MoverPtr MechWarrior::getVehicle (void) {

	// This assumes we have a Mover. Do we want to verify?
	// I say no, for speed's sake...
	return(MoverPtr(ObjectManager->getByWatchID(vehicleWID)));
}

long MechWarrior::checkSkill (long skillId, float factor) {

	// Makes a skill check, and returns the success\fail magnitude.
	// If returned value is negative, the check failed. If non-negative,
	// the check succeeded.

	numSkillUses[skillId][COMBAT_STAT_MISSION]++;
	skillPoints[skillId] += SkillTry[skillId];
	long successMargin = static_cast<long>((getSkill(skillId) * factor)) - RandomNumber(100) - 1;
	if (successMargin >= 0 && skillId != MWS_SENSORS)
	{
		numSkillSuccesses[skillId][COMBAT_STAT_MISSION]++;
		skillPoints[skillId] += SkillSuccess[skillId];
	}
	return(successMargin);
}

bool MechWarrior::injure (float numWounds, bool checkEject) {

	if (status != WARRIOR_STATUS_NORMAL)
		return(false);

	wounds += numWounds;
	if (wounds >= 6.0) 
	{
		MoverPtr myVehicle = getVehicle();
		Assert(myVehicle != nullptr, 0, " Pilot has no vehicle ");
		if (checkEject || escapesThruEjection) 
		{

			numSkillUses[MWS_PILOTING][COMBAT_STAT_MISSION]++;
			skillPoints[MWS_PILOTING] += SkillTry[MWS_PILOTING];

			if (escapesThruEjection) 
			{
				wounds = 3.0f;
				numSkillSuccesses[MWS_PILOTING][COMBAT_STAT_MISSION]++;
				skillPoints[MWS_PILOTING] += SkillSuccess[MWS_PILOTING];

				// If we can't eject, we're dead...
				// We always succeed according to this function...
				// How about, we comment it out then!!
				// -fs
				if (!myVehicle->handleEjection())
					wounds = 6.0;
			}
		}

		// Warrior's vehicle immediately disabled...
		if ((wounds >= 6.0) && !escapesThruEjection) 
		{
			radioMessage(RADIO_DEATH);
			status = WARRIOR_STATUS_DEAD;

			if (myVehicle)
				myVehicle->disable(PILOT_DEATH);
	
			if (getGroup())
				getGroup()->handleMateDestroyed(myVehicle->getWatchID());
	
			// Kill our radio. Dead Men Don't Talk...
			if (radio)
				radio->turnOff();

			return(true);
		}
	}
	else		//DON'T play this if they die!
	{
		if (numWounds > 0)
			radioMessage(RADIO_PILOT_HURT);
	}

	return(false);
}

void MechWarrior::eject (void) {

	if ((status != WARRIOR_STATUS_NORMAL) && (status != WARRIOR_STATUS_WITHDRAWING))
		return;

	if (wounds < 6)
		wounds++;		//Pilots take one point on ejection.  Only if won't kill them?

	if (wounds >= 6)	//Nope, eject 'em with one point and they die!
	{
		radioMessage(RADIO_DEATH);
		status = WARRIOR_STATUS_DEAD;
	}
	else
	{
		radioMessage(RADIO_EJECTING);
		status = WARRIOR_STATUS_EJECTED;
	}

	// Warrior's vehicle immediately disabled...
	MoverPtr myVehicle = getVehicle();
	if (myVehicle) {
		myVehicle->disable(EJECTION_DEATH);
		if (getGroup())
			getGroup()->handleMateEjected(myVehicle->getWatchID());
	}

	// Kill our radio. Dead (missing) Men Don't Talk...
	if (radio)
		radio->turnOff();
}

void MechWarrior::setTeam (TeamPtr team) {

	if (team)
		teamId = team->getId();
	else
		teamId = -1;
	if (!MPlayer && (team != Team::home)) {
		setUseGoalPlan(true);
		keepMoving = true;
		}
	else {
		setUseGoalPlan(false);
		keepMoving = false;
	}
}

void MechWarrior::setVehicle (GameObjectPtr vehicle) {

	if (vehicle) {
		if (!vehicle->isMover())
			Fatal(0, " bad vehicle type ");
		vehicleWID = vehicle->getWatchID();
		}
	else
		vehicleWID = 0;

	if (radio)
		radio->setOwner(this);
}

void MechWarrior::setup (void) {

	TacOrderQueuePos = 0;
	for (long i = 0; i < MAX_WARRIORS; i++) {
		warriorList[i] = new MechWarrior;
		warriorList[i]->index = i;
		warriorList[i]->used = false;
		if (warriorList[i] == nullptr)
			Fatal(i, " MechWarrior.setup: NULL warrior ");
	}

}

MechWarrior* MechWarrior::newWarrior (void) {

	for (long i = 1; i < MAX_WARRIORS; i++)
		if (!warriorList[i]->used) {
			warriorList[i]->used = true;
			return(warriorList[i]);
		}
	return(nullptr);
}

void MechWarrior::freeWarrior (MechWarrior* warrior) {

	warrior->clear();
}

void MechWarrior::changeAttackRange( long newRange )
{
	if ( curTacOrder.isCombatOrder() )
	{
		TacticalOrder order;
		// hope this assignment operator works
		order = curTacOrder;
		order.attackParams.range = static_cast<FireRangeType>(newRange);
		order.pack( nullptr, nullptr );
		getVehicle()->handleTacticalOrder( order );
	}
}

void MechWarrior::shutdown (void) {

	for (long i = 0; i < MAX_WARRIORS; i++) {
		if (warriorList[i]) {
			delete warriorList[i];
			warriorList[i] = nullptr;
		}
	}
}
