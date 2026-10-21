//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//								WARRIOR.CPP

#include "warrior_internal.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "mc2fmt.h"
#include <utility>
#include "sim_trace.h"


// ABL-VM-FACTS-1: TU-local definitions for the abl_trace externs.
namespace mc2_abl_trace {
	int g_peakCallDepth = 0;
	FrameCounters g_frame = {0, 0, 0, 0, 0.0};
}

// ABL-VM-FACTS-1: hook called from mclib/ablxstmt.cpp CallStackLevel++ site.
// Forwards to the gated inline tracker (byte-identical no-op when OFF).
void mc2AblNotePeakDepth(int level) { mc2_abl_trace::notePeakDepth(level); }

// MC2_BRAIN_TASKQ gate — checked once at first runBrain call
static bool s_brainTaskQEnabled     = false;
static bool s_brainTaskQTraceEnabled = false;
static bool s_brainTaskQGateChecked  = false;
static void initBrainTaskQGate() {
    if (s_brainTaskQGateChecked) return;
    s_brainTaskQGateChecked  = true;
    s_brainTaskQEnabled      = mc2::gate::get<"MC2_BRAIN_TASKQ">();
    s_brainTaskQTraceEnabled = mc2::gate::get<"MC2_BRAIN_TASKQ_TRACE">();
}

// MC2_BRAIN_RUNTIME gate — checked once at first runBrain call (requires MC2_BRAIN_TASKQ=1)
static bool             s_brainRuntimeEnabled      = false;
static bool             s_brainRuntimeTraceEnabled  = false;
static bool             s_brainRuntimeApplyEnabled  = false;   // MC2_BRAIN_RUNTIME_APPLY=1 (BRAIN-RUNTIME-1B)
static bool             s_brainRuntimeGateChecked   = false;
static BrainRuntimeMode s_brainRuntimeForcedMode    = BrainRuntimeMode::Legacy;
static void initBrainRuntimeGate() {
    if (s_brainRuntimeGateChecked) return;
    s_brainRuntimeGateChecked  = true;
    s_brainRuntimeEnabled      = mc2::gate::get<"MC2_BRAIN_RUNTIME">();
    s_brainRuntimeTraceEnabled = mc2::gate::get<"MC2_BRAIN_RUNTIME_TRACE">();
    // MC2_BRAIN_RUNTIME_APPLY=1 enables actual slot writes + short-circuit (BRAIN-RUNTIME-1B).
    // Requires MC2_BRAIN_RUNTIME=1; warn+stay inert if APPLY=1 but RUNTIME=0.
    s_brainRuntimeApplyEnabled = mc2::gate::get<"MC2_BRAIN_RUNTIME_APPLY">();
    if (s_brainRuntimeApplyEnabled && !s_brainRuntimeEnabled) {
        mc2::fmt::print_err("[BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME_APPLY=1 but MC2_BRAIN_RUNTIME=0 — apply staying inert\n");
        s_brainRuntimeApplyEnabled = false;
    }
    // Mode source: MC2_BRAIN_RUNTIME_FORCE_MODE env override only; per-unit FIT loader may override per-warrior.
    if (mc2::gate::isSet<"MC2_BRAIN_RUNTIME_FORCE_MODE">()) {
        if      (mc2::gate::is<"MC2_BRAIN_RUNTIME_FORCE_MODE", "hybrid">())   s_brainRuntimeForcedMode = BrainRuntimeMode::Hybrid;
        else if (mc2::gate::is<"MC2_BRAIN_RUNTIME_FORCE_MODE", "enhanced">()) s_brainRuntimeForcedMode = BrainRuntimeMode::Enhanced;
        else                                                                  s_brainRuntimeForcedMode = BrainRuntimeMode::Legacy;
    }
    if (s_brainRuntimeEnabled && !s_brainTaskQEnabled) {
        mc2::fmt::print_err("[BRAIN_RT] WARNING: MC2_BRAIN_RUNTIME=1 but MC2_BRAIN_TASKQ=0 — runtime staying inert\n");
        s_brainRuntimeEnabled      = false;
        s_brainRuntimeApplyEnabled = false;
    }
}

// MC2_BRAIN_FIXED_TICK gate — checked once at first brain-cadence check
// BRAIN-FIXED-TICK-1: fixed sim-time accumulator replaces per-warrior brainUpdate advance.
// Gate OFF (default): byte-identical to baseline (brainUpdate += BrainUpdateFrequency).
// Gate ON: global accumulator advanced by scenarioTime delta; integer tick index drives
//          which warriors fire; per-warrior brainUpdate still used as stagger offset only.
static bool  s_brainFixedTickEnabled     = false;
static bool  s_brainFixedTickTrace       = false;
static bool  s_brainFixedTickGateChecked = false;
static float s_brainFixedTickHz          = 10.0f;   // default 10 Hz = 0.1 s period
static float s_brainFixedTickPeriod      = 0.1f;    // 1.0 / hz, computed at init

// Per-frame accumulator: how many sim-seconds have accumulated toward next fixed tick.
// Advanced once per frame in updateBrainFixedTickAccumulator().
// Not per-warrior — this is a global sim-time counter.
static float    s_brainFixedTickAccum    = 0.0f;
static float    s_brainFixedTickSimLast  = -1.0f;   // last scenarioTime seen; -1 = uninit
static uint32_t s_brainTickIndex         = 0;       // monotonic tick counter, per-frame global
static uint32_t s_brainTicksThisFrame    = 0;       // ticks fired in current frame (reset each frame)

// BRAIN-DECISION-INTENT-QUEUE-1: accessor so brain_special_dispatch.cpp can stamp intents.
uint32_t getBrainTickIndex() { return s_brainTickIndex; }

// BRAINSPECIAL-FLOW-WAIT-1: sim-time accessor for WAIT deadlines (milliseconds).
// scenarioTime is the deterministic mission sim clock (same time base the fixed-tick
// accumulator above consumes); WAIT deadlines keyed on it hold under the determinism
// model (the queue key already carries sim_frame_ms). Harness stubs this symbol.
uint32_t getBrainTimeMs() {
    if (scenarioTime <= 0.0f) return 0;
    return static_cast<uint32_t>((scenarioTime * 1000.0f));
}

static void initBrainFixedTickGate() {
    if (s_brainFixedTickGateChecked) return;
    s_brainFixedTickGateChecked = true;
    s_brainFixedTickEnabled = mc2::gate::get<"MC2_BRAIN_FIXED_TICK">();
    s_brainFixedTickTrace   = mc2::gate::get<"MC2_BRAIN_FIXED_TICK_TRACE">();
    if (s_brainFixedTickEnabled) {
        const float hz = mc2::gate::get<"MC2_BRAIN_FIXED_TICK_HZ">();   // the row clamps to [1, 30]
        s_brainFixedTickHz     = hz;
        s_brainFixedTickPeriod = 1.0f / hz;
        mc2::fmt::print_err("[BRAIN_FIXED_TICK] init: hz={:.1f} period={:.4f}s trace={:d}\n", s_brainFixedTickHz, s_brainFixedTickPeriod, mc2::fmt::as_i32(static_cast<int>(s_brainFixedTickTrace)));
        std::fflush(stderr);
    }
}

// Called once per frame (at start of first warrior's updateActions for this frame) to
// advance the accumulator by the sim-time delta and count whole ticks.
// Returns true if at least one tick fired this frame (i.e., warriors may run brain).
// All warriors within a single render frame share the same brainTickIndex window.
static bool advanceBrainFixedTickAccumulator(float scenarioTime) {
    if (s_brainFixedTickSimLast < 0.0f) {
        // First call: seed last-time, no ticks yet.
        s_brainFixedTickSimLast  = scenarioTime;
        s_brainTicksThisFrame    = 0;
        return false;
    }
    float dt = scenarioTime - s_brainFixedTickSimLast;
    if (dt < 0.0f) dt = 0.0f;  // guard against rewind (save/load)
    s_brainFixedTickSimLast = scenarioTime;
    s_brainFixedTickAccum  += dt;
    s_brainTicksThisFrame   = 0;
    while (s_brainFixedTickAccum >= s_brainFixedTickPeriod) {
        s_brainFixedTickAccum -= s_brainFixedTickPeriod;
        ++s_brainTickIndex;
        ++s_brainTicksThisFrame;
    }
    return (s_brainTicksThisFrame > 0);
}

// Track which frame we last advanced the accumulator for, so each render frame advances once.
static uint32_t s_brainAccumLastFrame = 0xFFFFFFFFu;

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

void SeedRandomNumbers (void) {

	srand(static_cast<unsigned>(time(nullptr)));
}

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
	memset( killed, 0, sizeof( GameObject* ) * MAX_MOVERS / 3 );

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
			radioLog.lastMessage = radio->playMessage(static_cast<RadioMessageType>(message));
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
	memset( killed, 0, sizeof( GameObject* ) * MAX_MOVERS / 3 );

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

void MechWarrior::setBrainName (const char *brainName)
{
	strncpy(brainStr, brainName, MAXLEN_PILOT_BRAIN - 1);
	brainStr[MAXLEN_PILOT_BRAIN - 1] = '\0';
}

long MechWarrior::setBrain (long brainHandle) {

	long brainErr = NO_ERR;

	if (brain) {
		delete brain;
		brain = nullptr;
		for (long i = 0; i < NUM_PILOT_ALARMS; i++)
			brainAlarmCallback[i] = nullptr;
	}
	warriorBrainHandle = brainHandle;
	if (brainHandle > -1) 
	{
		brain = new ABLModule;
		brainErr = brain->init(brainHandle);
		if (brainErr == NO_ERR) {
			char brainName[500];
			mc2::fmt::to_buf(brainName, "Pilot {}", mc2::fmt::safe(name));
			brain->setName(brainName);
			for (long i = 0; i < NUM_PILOT_ALARMS; i++)
				brainAlarmCallback[i] = brain->findFunction(pilotAlarmFunctionName[i], true);
		}
	}
	return(brainErr);
}

long MechWarrior::runBrain (void) {

	ZoneScopedN("GameLogic.AI.BrainRun");

//	if (teamId  > -1)
//		if (teamId != Team::home->getId())
//		return(0);

	// PATROL-DRIVE-1 / pure-Enhanced: inline-Brain warriors (TechScript/Enhanced) carry NO
	// legacy ABL brain (brain==NULL), but their behavior is driven by the Enhanced runtime
	// further down this function (patrol tick, dispatch). The legacy ABL path (brain->execute)
	// is the only consumer of `brain`. So bail on NULL brain ONLY when there is also no
	// Enhanced runtime to run — otherwise fall through and let the runtime drive. Stock
	// warriors always have a brain, so this is byte-identical for stock.
	if (!brain) {
		const bool enhancedRuntimeActive =
			s_brainRuntimeEnabled && s_brainRuntimeApplyEnabled &&
			brainRuntime && (brainRuntime->mode == BrainRuntimeMode::Enhanced);
		if (!enhancedRuntimeActive)
			return(0);
	}

	// MOD-HEALTH-1: tools/mod_smoke.py asserts AI is alive ("dead AI on POAR") from these
	// milestone lines; always-on, three lines per process.
	{
		static unsigned long s_brainRuns = 0;
		++s_brainRuns;
		if (s_brainRuns == 1 || s_brainRuns == 100 || s_brainRuns == 1000) {
			mc2::fmt::print_out("[MOD_HEALTH v1] event=brain_runs count={:d} scenarioTime={:.2f}\n",
				mc2::fmt::as_u64(s_brainRuns), static_cast<double>(scenarioTime));
			fflush(stdout);
		}
	}

	// Param 1 is the ID of this mech...
	//ABLi_setIntegerParam(brainParams, 0, ((BattleMechPtr)owner)->ID);

	// Param 2 is the current game clock...
	//ABLi_setRealParam(brainParams, 1, scenarioTime);

	// Param 3 is the type of AI event (0 is
	// standard sequential update)...
	//ABLi_setIntegerParam(brainParams, 2, 0);

	// Now, call the brain...
	CurGroup = getGroup();
	CurObject = static_cast<GameObjectPtr>(getVehicle());
	CurObjectClass = getVehicle()->getObjectClass();
	CurWarrior = this;
	CurContact = nullptr;
	curEventID = 0;
	curEventTrigger = 0;
	ModuleInfo moduleInfo;
	if (brain)                       // PATROL-DRIVE-1: inline-Brain warriors have no ABL brain.
		brain->getInfo(&moduleInfo); // moduleInfo is only consumed by the legacy ABL path below.

	// BRAIN-RUNTIME-1B: check if this warrior is under Enhanced runtime control.
	// If so, skip brain->execute() and the ABL-derived tac order, push+drain HOLD once.
	// CurGroup/CurWarrior clear and clearAlarmsHistory() run UNCONDITIONALLY below.
	bool enhancedApply = s_brainRuntimeEnabled && s_brainRuntimeApplyEnabled
	                     && brainRuntime
	                     && (brainRuntime->mode == BrainRuntimeMode::Enhanced);

	// TACTIC-WEIGHTS-B: selection MUST run BEFORE dispatch so the attack order
	// (once-guarded) picks up selectedTactic this tick. Relocated above the brain
	// legacy/enhanced branch for that reason.
	// TACTIC-WEIGHTS-A: deterministic weighted tactic selection (TRACE-ONLY).
	// Gate: MC2_TACTIC_WEIGHTS=1 (default OFF).
	// Runs for BOTH legacy+enhanced paths; NO behavior write (deferred to TACTIC-WEIGHTS-B).
	// missionSeed: MC2_BRAIN_MISSION_SEED env override, else 0xDEADBEEF placeholder.
	{
		static bool   s_tacticWeightsEnabled  = false;
		static bool   s_tacticWeightsChecked  = false;
		static uint32_t s_missionSeed         = 0xDEADBEEFu;
		if (!s_tacticWeightsChecked) {
			s_tacticWeightsChecked = true;
			s_tacticWeightsEnabled = mc2::gate::get<"MC2_TACTIC_WEIGHTS">();
			const std::string seedText = mc2::gate::str<"MC2_BRAIN_MISSION_SEED">();   // "" when unset or empty
			if (!seedText.empty())
				s_missionSeed = static_cast<uint32_t>(std::strtoul(seedText.c_str(), nullptr, 0));
		}
		if (s_tacticWeightsEnabled && brainRuntime) {
			// Collect pilot stats for modulation
			int gunnery       = static_cast<int>(static_cast<unsigned char>(skills[MWS_GUNNERY]));
			int pilotAggr     = static_cast<int>(static_cast<unsigned char>(aggressiveness));
			int pilotCourage  = static_cast<int>(static_cast<unsigned char>(courage));

			// Wang hash: deterministic per (wid, brainTick, missionSeed)
			uint32_t h = tacticWang(static_cast<int>(vehicleWID), s_brainTickIndex, s_missionSeed);

			// Apply pilot modulation to FIT-loaded base weights
			float wEff[NUM_TACTICS] = {};
			applyPilotModulation(brainRuntime->tacticWeights, wEff, NUM_TACTICS,
			                     gunnery, pilotAggr, pilotCourage);

			// Weighted pick — pure, deterministic
			int chosen = selectTactic(wEff, NUM_TACTICS, h);
			brainRuntime->selectedTactic = chosen;

			// Emit trace (no behavior write)
			mc2::fmt::print_err("[BRAIN_TACTIC_SELECT] wid={:d} tick={:d} tactic={}({:d})\n", mc2::fmt::as_i32(vehicleWID), mc2::fmt::as_u32(s_brainTickIndex), mc2::fmt::safe(tacticName(chosen)), mc2::fmt::as_i32(chosen));
			std::fflush(stderr);
		}
	}

	long brainErr = 0;
	if (!enhancedApply) {
		// ── Legacy ABL path ──────────────────────────────────────────────────────────
		// ABL-VM-FACTS-1: time the fire, read stmts (execute() return) + state
		// transitions at exit. Byte-identical when the ABL diag tag is OFF.
		mc2_abl_trace::FireScope ablFire("warrior", static_cast<int>(vehicleWID));
		long ablStmts = brain->execute();
		ablFire.record(ablStmts, NumStateTransitions);
		// Well, we'll just set it every frame so it doesn't screw up :)
		setUseGoalPlan(!MPlayer && (getCommander() != Commander::home));

		if (useGoalPlan) {
			TacticalOrder tacOrder;
			long result = calcTacOrder(mainGoalAction, mainGoalObjectWID, mainGoalLocation, mainGoalControlRadius, 0, 1, 150.0, 300, tacOrder);
			if (result == 0) {
				if (tacOrder.code != TACTICAL_ORDER_NONE) {
					if (getCommander() == Commander::home)
						setPlayerTacOrder(tacOrder);
					else
						setGeneralTacOrder(tacOrder);
				}
				}
			else if (result > 0) {
				setMainGoal(GOAL_ACTION_NONE, nullptr, nullptr, -1.0);
				clearCurTacOrder();
			}
		}

		brainErr = brain->getInteger();
		switch (brainErr) {
			case 0:
				// No error...
				break;
			case 1:
				// General purpose error, for now, so bomb out...
				break;
		}
	} else {
		// ── APPLY=1 + Enhanced: skip ABL execute; push HOLD once, drain+apply ───────
		if (brainTaskQueue && !brainRuntime->initialHoldPushed) {
			brainRuntime->initialHoldPushed = 1;
			brainTaskQueue->push(/*tier=*/0, /*frame_ms=*/0, vehicleWID, BrainTaskType::BRAIN_TASK_HOLD);
		}
		// TECHSCRIPT-SPECIAL-DISPATCH-1B: DISPATCH_APPLY gate check.
		// When MC2_BRAIN_DISPATCH=1 + MC2_BRAIN_DISPATCH_APPLY=1 and the body contains a
		// recognized effect verb (Brain.CorePower false → POWERDOWN), the dispatcher owns
		// the GENERAL-slot write this tick. The synthetic HOLD_TASK is suppressed to ensure
		// EXACTLY ONE GENERAL-slot write per tick.
		// When DISPATCH_APPLY=0, synthetic HOLD fires as 1B-runtime default (unchanged).
		static const bool s_dispatchApply = ([](){
			if (mc2::gate::get<"MC2_BRAIN_DISPATCH_APPLY">()) {
				if (!mc2::gate::get<"MC2_BRAIN_DISPATCH">()) {
					mc2::fmt::print_err("[BRAIN_DISPATCH] WARNING: MC2_BRAIN_DISPATCH_APPLY=1 requires MC2_BRAIN_DISPATCH=1 — apply is INERT\n");
					std::fflush(stderr);
					return false;
				}
				return true;
			}
			return false;
		})();
		// BRAIN-DECISION-INTENT-QUEUE-1: gate check (default OFF).
		// When ON: executeSpecialBody_Apply emits intents; commitBrainIntents drains them.
		static const bool s_intentQueue = mc2::gate::get<"MC2_BRAIN_INTENT_QUEUE">();
		// BRAIN-COMMIT-PHASE-1: deferred commit gate (default OFF).
		// Requires MC2_BRAIN_INTENT_QUEUE=1 to be meaningful.
		// Gate ON → inline commitBrainIntents calls are SKIPPED here; intents persist in
		//   runtime->pendingIntents[] until commitAllBrainIntents() drains them in WID order
		//   after the full warrior update loop in objmgr.cpp (GameObjectManager::update).
		// Gate OFF → commitBrainIntents runs inline per-warrior (rung-5 behavior, unchanged).
		static const bool s_brainCommitPhase = mc2::gate::get<"MC2_BRAIN_COMMIT_PHASE">();

		// BRAIN-WORLD-SNAPSHOT-1: static populate helper (main-thread only).
		// Accesses protected MechWarrior members — must remain in warrior.cpp.
		// Called below when s_brainSnapshotEnabled() is true (gate ON).
		// FORBIDDEN: must not call setGeneralTacOrder or any order/movement/attack function.
		auto buildBrainWorldSnapshotLocal = [&](BrainWorldSnapshot& out) {
			// Warrior identity: vehicleWID is in scope and is the WID passed to dispatch.
			// MechWarrior itself is not a GameObject; vehicleWID == warrior's vehicle WatchID.
			out.warriorId = static_cast<int32_t>(vehicleWID);

			// Team ID: deferred to rung 7/8 — ATTACK friendly-fire still reads live.
			out.teamId = 0;

			// Liveness: query the vehicle (MoverPtr from getVehicle() — may be null; guard it).
			{
				MoverPtr veh = getVehicle();
				out.isDisabled = (veh && veh->isDisabled()) ? 1u : 0u;
			}
			out.commanderIsHome = 0u; // deferred — commander identity not queried this rung

			// Main goal (protected MechWarrior fields; directly accessible in warrior.cpp)
			out.mainGoalObjectWID     = static_cast<int32_t>(mainGoalObjectWID);
			out.mainGoalLocation[0]   = mainGoalLocation.x;
			out.mainGoalLocation[1]   = mainGoalLocation.y;
			out.mainGoalLocation[2]   = mainGoalLocation.z;
			out.mainGoalControlRadius = mainGoalControlRadius;

			// Attack orders target WID (protected field, directly accessible in warrior.cpp)
			out.attackOrderTargetWID = static_cast<int32_t>(attackOrders.targetWID);

			// Once-guard flags — the primary snapshot surface for rung 6
			out.effectApplied[std::to_underlying(BSNAPFX_POWERDOWN)] = brainRuntime->dispatchEffectApplied;
			out.effectApplied[std::to_underlying(BSNAPFX_EJECT)]     = brainRuntime->ejectEffectApplied;
			out.effectApplied[std::to_underlying(BSNAPFX_GUARD)]     = brainRuntime->guardEffectApplied;
			out.effectApplied[std::to_underlying(BSNAPFX_MOVETO)]    = brainRuntime->moveToEffectApplied;
			out.effectApplied[std::to_underlying(BSNAPFX_ATTACK)]    = brainRuntime->attackEffectApplied;
			out.effectApplied[std::to_underlying(BSNAPFX_RETREAT)]   = brainRuntime->retreatEffectApplied;

			// Debug tick stamp
			out.brainTick = getBrainTickIndex();
		};

		bool dispatcherAppliedEffect = false;
		if (s_dispatchApply && brainRuntime && brainRuntime->specialBody.loaded) {
			// DISPATCH-EFFECT-UNITRETREAT-1: bodyHasEffect() now covers POWERDOWN, EJECT, GUARD, MOVETO, ATTACK, RETREAT.
			// CALL-CHAIN-1A: dispatch ALL loaded bodies (not just effect bodies).
			// executeSpecialBody_Apply traces all verbs; applies effect ONCE if present.
			// The HOLD suppression only triggers when an effect verb was actually applied
			// (dispatcherAppliedEffect=true ← Apply return value true).
			const SpecialIndex* idx = brainRuntime->specialIndex.empty()
			    ? nullptr : &brainRuntime->specialIndex;

			const bool hasPowerdown = bodyHasPowerdown(brainRuntime->specialBody);
			const bool hasEject     = bodyHasUnitEject(brainRuntime->specialBody);
			const bool hasGuard     = bodyHasCoreGuard(brainRuntime->specialBody);
			const bool hasMoveTo    = bodyHasCoreMoveTo(brainRuntime->specialBody);
			const bool hasAttack    = bodyHasCoreAttack(brainRuntime->specialBody);
			const bool hasRetreat   = bodyHasUnitRetreat(brainRuntime->specialBody);
			const bool hasEffect    = hasPowerdown || hasEject || hasGuard || hasMoveTo || hasAttack || hasRetreat;

			// BRAINSPECIAL-FLOW-WAIT-1: flow-bearing bodies (WAIT/WAIT_UNTIL/STOP present,
			// gate MC2_BRAIN_FLOW) re-dispatch EVERY tick — the class-level once-guard
			// pre-set below would permanently suppress an effect a WAIT still gates.
			// Apply's per-verb-index flowFired guards prevent order re-emission instead.
			// flowFiredCount>0 means the dispatcher has claimed the GENERAL slot at some
			// point — keep suppressing the synthetic HOLD exactly like alreadyDone does.
			// Gate OFF: brainFlowActiveForBody is false (scanner emits no flow verbs) —
			// this branch is untouched dead code, byte-identical behavior.
			const bool flowActive = brainFlowActiveForBody(brainRuntime->specialBody);
			if (flowActive) {
				bool applied = executeSpecialBody_Apply(brainRuntime->specialBody, this, vehicleWID,
				                                        &brainRuntime->varStore, idx, "");
				if (s_intentQueue && !s_brainCommitPhase) commitBrainIntents(this, brainRuntime);
				dispatcherAppliedEffect = applied || (brainRuntime->flowFiredCount > 0);
				// (falls through to the shared dispatcherAppliedEffect drain below)
			} else {
			// (non-flow path below is the pre-slice code, unchanged)

			// BRAIN-WORLD-SNAPSHOT-1: capture world state before once-guard reads (gate ON only).
			// Gate OFF: snap is zero-initialised and unused — reads fall through to live runtime.
			BrainWorldSnapshot snap = {};
			const bool snapshotGate = s_brainSnapshotEnabled();
			if (snapshotGate)
				buildBrainWorldSnapshotLocal(snap);

			// Once-guard: if the effect was already applied, suppress HOLD without re-applying.
			// Gate ON:  read from snapshot.effectApplied[N] (the seam for future off-thread emit).
			// Gate OFF: read live from runtime (byte-identical to pre-snapshot behavior).
			const bool powerdownDone = hasPowerdown && (snapshotGate
			    ? snap.effectApplied[std::to_underlying(BSNAPFX_POWERDOWN)]
			    : brainRuntime->dispatchEffectApplied);
			const bool ejectDone     = hasEject     && (snapshotGate
			    ? snap.effectApplied[std::to_underlying(BSNAPFX_EJECT)]
			    : brainRuntime->ejectEffectApplied);
			const bool guardDone     = hasGuard     && (snapshotGate
			    ? snap.effectApplied[std::to_underlying(BSNAPFX_GUARD)]
			    : brainRuntime->guardEffectApplied);
			const bool moveToDone    = hasMoveTo    && (snapshotGate
			    ? snap.effectApplied[std::to_underlying(BSNAPFX_MOVETO)]
			    : brainRuntime->moveToEffectApplied);
			const bool attackDone    = hasAttack    && (snapshotGate
			    ? snap.effectApplied[std::to_underlying(BSNAPFX_ATTACK)]
			    : brainRuntime->attackEffectApplied);
			const bool retreatDone   = hasRetreat   && (snapshotGate
			    ? snap.effectApplied[std::to_underlying(BSNAPFX_RETREAT)]
			    : brainRuntime->retreatEffectApplied);
			const bool alreadyDone   = hasEffect && ((!hasPowerdown || powerdownDone) && (!hasEject || ejectDone) && (!hasGuard || guardDone) && (!hasMoveTo || moveToDone) && (!hasAttack || attackDone) && (!hasRetreat || retreatDone));

			if (!alreadyDone) {
				if (hasEffect) {
					// Effect body: apply once, suppress HOLD slot write thereafter.
					// Set the appropriate once-guard flag BEFORE calling Apply so a re-entrant
					// tick (edge case) can't fire a second order.
					if (hasPowerdown && !brainRuntime->dispatchEffectApplied)
						brainRuntime->dispatchEffectApplied = 1;
					if (hasEject && !brainRuntime->ejectEffectApplied)
						brainRuntime->ejectEffectApplied = 1;
					if (hasGuard && !brainRuntime->guardEffectApplied)
						brainRuntime->guardEffectApplied = 1;
					if (hasMoveTo && !brainRuntime->moveToEffectApplied)
						brainRuntime->moveToEffectApplied = 1;
					if (hasAttack && !brainRuntime->attackEffectApplied)
						brainRuntime->attackEffectApplied = 1;
					if (hasRetreat && !brainRuntime->retreatEffectApplied)
						brainRuntime->retreatEffectApplied = 1;
					bool applied = executeSpecialBody_Apply(brainRuntime->specialBody, this, vehicleWID,
					                                        &brainRuntime->varStore, idx, "");
					// BRAIN-DECISION-INTENT-QUEUE-1: drain pending intents inline (gate ON only).
					// BRAIN-COMMIT-PHASE-1: when s_brainCommitPhase is ON, skip inline commit —
					//   intents stay in runtime->pendingIntents[] for commitAllBrainIntents().
					// commitBrainIntents is the sole caller of setGeneralTacOrder when gate ON.
					if (s_intentQueue && !s_brainCommitPhase) commitBrainIntents(this, brainRuntime);
					if (applied)
						dispatcherAppliedEffect = true;
				} else {
					// No effect verb — trace-dispatch every tick (no slot ownership; HOLD fires normally).
					executeSpecialBody_Apply(brainRuntime->specialBody, this, vehicleWID,
					                        &brainRuntime->varStore, idx, "");
					// Gate ON + commit-phase OFF: drain inline (no-op since no effect verbs; defensive).
					// Gate ON + commit-phase ON: leave empty pendingIntents for the phase (no-op there too).
					if (s_intentQueue && !s_brainCommitPhase) commitBrainIntents(this, brainRuntime);
					// dispatcherAppliedEffect stays false → HOLD fires as normal.
				}
			} else {
				// Effect already applied — suppress HOLD without re-applying.
				dispatcherAppliedEffect = true;
			}
			}  // BRAINSPECIAL-FLOW-WAIT-1: end of non-flow (pre-slice) path
			if (dispatcherAppliedEffect) {
				if (brainTaskQueue) {
					BrainTaskEntry task;
					while (brainTaskQueue->drainWithTask(task)) {
						// Discard queued tasks — dispatcher owns GENERAL slot this body.
						// 1B-supersede: HOLD_TASK silenced (no setGeneralTacOrder STOP write).
					}
				}
			}
		}

		// BRAIN-OPORD-COREPATROL-1: per-tick patrol advance.
		// Gate: MC2_BRAIN_PATROL (default OFF).  No-op when gate OFF or patrolActive==false.
		// Called unconditionally (outside alreadyDone guard) so arrival is polled every tick.
		// tickPatrolAdvance handles arrival poll, cursor advance, and MOVETO_POINT re-emit.
		// moveToEffectApplied is NOT touched — patrol manages its own cursor guard.
		if (brainRuntime) {
			// BRAIN-ENGAGE-1: engagement takes priority over patrol. If a threat is in range the
			// unit attacks (and we suppress patrol advance so it fights instead of walking off);
			// otherwise patrol/idle proceeds as before. Both emit via the intent queue.
			bool engaging = tickEngageNearest(this, brainRuntime, vehicleWID);
			if (engaging) {
				dispatcherAppliedEffect = true;
			} else if (tickPatrolAdvance(this, brainRuntime, vehicleWID)) {
				// Patrol re-emitted a MOVETO_POINT — suppress HOLD so the order isn't stomped.
				dispatcherAppliedEffect = true;
			} else if (tickOpordMove(this, brainRuntime, vehicleWID)) {
				// BRAIN-OPORD-MOVE-1: Escort follow / ReturnToPost leash issued a move.
				dispatcherAppliedEffect = true;
			}
		}

		if (!dispatcherAppliedEffect) {
			// Synthetic HOLD path (1B-runtime default) — fires when dispatcher did NOT take slot.
			if (brainTaskQueue) {
				BrainTaskEntry task;
				while (brainTaskQueue->drainWithTask(task)) {
					if (task.type == BrainTaskType::BRAIN_TASK_HOLD) {
						TacticalOrder holdOrder;
						holdOrder.init(ORDER_ORIGIN_SELF, TACTICAL_ORDER_STOP);
						setGeneralTacOrder(holdOrder);
						mc2::fmt::print_err("[BRAIN_RT] HOLD_TASK applied wid={:d}\n", mc2::fmt::as_i32(vehicleWID));
						std::fflush(stderr);
					}
					// Other task types: discard (no handler this slice).
				}
			}
		}

		// TECHSCRIPT-SPECIAL-DISPATCH trace path (1A behavior preserved when APPLY=0).
		// CALL-CHAIN-1A: pass index for TechSpecial.Call resolution.
		if (!s_dispatchApply && brainRuntime && brainRuntime->specialBody.loaded) {
			const SpecialIndex* idx = brainRuntime->specialIndex.empty()
			    ? nullptr : &brainRuntime->specialIndex;
			executeSpecialBody_TraceOnly(brainRuntime->specialBody, vehicleWID,
			                             &brainRuntime->varStore, idx, "");
		}
		brainErr = 0;
	}

	// UNCONDITIONAL housekeeping — runs for BOTH paths (preserves Legacy invariant).
	CurGroup = nullptr;
	CurObject = nullptr;
	CurObjectClass = 0;
	CurWarrior = nullptr;
	CurContact = nullptr;

	clearAlarmsHistory();

	// SAVE/LOAD-AIFREEZE-TRACE-1: gated one-line diagnostic (default OFF ->
	// byte-identical) for the save/load AI-freeze investigation. Emits the
	// post-execution decision state so a quick-save/quick-load repro of idle
	// enemies can be classified:
	//   curTacOrder.code==NONE(0) && movePath.numSteps==0 -> standing-order not
	//     re-emitted after load (pursue standing-order re-emit).
	//   movePath.numSteps>0 with no actual movement       -> pivot to
	//     PathManager / reservation state.
	// NOTE: the recon's "initCalled" field does not exist on MechWarrior;
	// brainState + moveState are the real available signals and substituted.
	// ISSUE-10 extension (scripted units never engaging / permanent stalls):
	// added fields classify every stuck warrior without a debugger —
	//   orderState        ORDERSTATE_GENERAL(0)/PLAYER(1)/ALARM(2): pinned in
	//                     ALARM = escape-tile alarm loop (general orders never
	//                     consumed while the alarm slot is held).
	//   mainGoal/goalWID  goal-plan divert target (turret/gate/wall capture-
	//                     divert that never completes).
	//   genOrder          tacOrder[ORDERSTATE_GENERAL].code: brain/FSM emitted
	//                     an order vs no-order-from-brain (ABL stall).
	//   pos               position delta over ticks = machine-checkable
	//                     "path-but-not-moving" (reservation/collision) class.
	//   lastMoveErr       LastMoveCalcErr: no-path (LR path fail / MOVE-data
	//                     area-graph misread) class.
	{
		static const bool s_aiFreezeTrace = mc2::gate::get<"MC2_BRAIN_FREEZE_TRACE">();
		if (s_aiFreezeTrace) {
			MovePathPtr mp = getMovePath();
			MoverPtr traceVehicle = getVehicle();
			glm::vec3 tracePos;
			tracePos = glm::vec3(0.0f);
			if (traceVehicle)
				tracePos = traceVehicle->getPosition();
			mc2::fmt::print_err("[BRAIN_FREEZE_TRACE] wid={:d} brainState={:d} curTacOrder.code={:d} movePath.numSteps={:d} moveState={:d} hasBrain={:d} orderState={:d} mainGoal={:d} goalWID={:d} genOrder={:d} pos=({:.0f},{:.0f}) lastMoveErr={:d}\n", mc2::fmt::as_i32(static_cast<int>(vehicleWID)), mc2::fmt::as_i32(static_cast<int>(brainState)), mc2::fmt::as_i32(static_cast<int>(curTacOrder.code)), mc2::fmt::as_i32(mp ? static_cast<int>(mp->numSteps) : -1), mc2::fmt::as_i32(moveOrders.moveState), mc2::fmt::as_i32(brain ? 1 : 0), mc2::fmt::as_i32(static_cast<int>(orderState)), mc2::fmt::as_i32(static_cast<int>(mainGoalAction)), mc2::fmt::as_u32(static_cast<unsigned long>(mainGoalObjectWID)), mc2::fmt::as_i32(static_cast<int>(tacOrder[ORDERSTATE_GENERAL].code)), static_cast<double>(tracePos.x), static_cast<double>(tracePos.y), mc2::fmt::as_i32(LastMoveCalcErr));
			std::fflush(stderr);
		}
	}

	return(brainErr);
}

// BRAIN-RUNTIME-1B: public accessor used by mission.cpp FIT loader.

void MechWarrior::setBrainRuntimeMode (BrainRuntimeMode fitMode) {
	if (!brainRuntime) {
		brainRuntime = new MechBrainRuntime();
	}
	brainRuntime->mode = fitMode;
}

long MechWarrior::getVehicleStatus (void) {

	long myVehicleStatus = -1;
	GameObjectPtr myVehicle = getVehicle();
	if (myVehicle)
		myVehicleStatus = myVehicle->getStatus();
	return(myVehicleStatus);
}

// PATHFINDING-JUMP-FAIL-1: backoff gate + constants. Default-OFF killswitch: when
// MC2_PATH_JUMP_FAIL_BACKOFF is unset, s_jumpFailBackoffOn is false and every new
// branch below is inert -> byte-identical legacy behavior (no stamp, no short-circuit).
// Cached once at first use (repo getenv idiom, mover.cpp style).
static const bool s_jumpFailBackoffOn = mc2::gate::get<"MC2_PATH_JUMP_FAIL_BACKOFF">();
// Frames to suppress re-requesting a goal cell that just failed a jump solve.
// ~45 path-manager update() frames ~= 0.5-1s of doomed re-solves eliminated.
static const long kJumpFailBackoffFrames = 45;

// Defined in movemgr.cpp (PATHFINDING-FACTS-2 counter, advanced once per update()).
extern long MovePathManager_getFrameCounter (void);

void MechWarrior::noteJumpPathFailed (void) {

	if (!s_jumpFailBackoffOn)
		return;
	// Key on the pilot's CURRENT move goal cell -- the exact same basis
	// requestMovePath compares against (getMoveGoal -> worldToCell). Using one
	// source of truth guarantees the stamp and the short-circuit test agree,
	// regardless of any goal revision the solver applied internally.
	glm::vec3 goalLoc;
	if (getMoveGoal(&goalLoc) == MOVEGOAL_NONE)
		return;
	int goalCellR, goalCellC;
	land->worldToCell(goalLoc, goalCellR, goalCellC);
	lastFailedJumpGoalCellR = goalCellR;
	lastFailedJumpGoalCellC = goalCellC;
	lastJumpFailFrame = MovePathManager_getFrameCounter();
}

void MechWarrior::requestMovePath (long selectionIndex, unsigned long moveParams, long source) {

	// PATHFINDING-JUMP-FAIL-1: short-circuit doomed jump re-requests. If the pilot's
	// current move goal is the SAME cell as the last jump solve that found no path,
	// and we are still inside the backoff window, skip the enqueue entirely -- this
	// is what collapses the queue width (the doomed solve was re-requested every AI
	// tick with no failure memory). Only AI/non-home jumpers ever fail jump solves
	// (jump path is enemy-only, mover.cpp), so player orders are never affected.
	if (s_jumpFailBackoffOn && (lastJumpFailFrame >= 0)) {
		glm::vec3 goalLoc;
		if (getMoveGoal(&goalLoc) != MOVEGOAL_NONE) {
			int goalCellR, goalCellC;
			land->worldToCell(goalLoc, goalCellR, goalCellC);
			if ((goalCellR == lastFailedJumpGoalCellR) &&
			    (goalCellC == lastFailedJumpGoalCellC)) {
				long now = MovePathManager_getFrameCounter();
				if ((now - lastJumpFailFrame) < kJumpFailBackoffFrames)
					return;  // doomed: don't re-enqueue yet
			}
		}
	}

	MC2_SIM_TRACE_LINE("REQ pilot={:d} src={:d} params={:d} sel={:d}", static_cast<int32_t>((getVehicle() ? getVehicle()->getPartId() : -1)), static_cast<int32_t>(source), static_cast<uint32_t>(moveParams), static_cast<int32_t>(selectionIndex));
	PathManager->request(this, selectionIndex, moveParams, source);
}

long MechWarrior::combatDecisionTree (void) {

	long result = -1;

	combatUpdate = scenarioTime + CombatUpdateFrequency;

	glm::vec3 tPoint;

	MoverPtr myVehicle = getVehicle();
	Assert(myVehicle != nullptr, 0, " Pilot has no vehicle! ");

	// Let's see if we have ANY ammo...
	bool outOfAmmo = true;
	for (long i = 0; i < myVehicle->getNumAmmoTypes(); i++)
		if (myVehicle->getAmmoTypeTotal(i) > 0) {
			outOfAmmo = false;
			break;
		}

	GameObjectPtr target = getLastTarget(); //getAttackTarget(ORDER_CURRENT);

	// Our target is our last target, if we had one (and we weren't explicitly
	// told to hold fire since then)...
	glm::vec3 *targetPoint = nullptr;
	long attackType = ATTACK_TO_DESTROY;
	long aimLocation = -1;
	if (curTacOrder.isCombatOrder()) {
		attackType = curTacOrder.attackParams.type;
		aimLocation = curTacOrder.attackParams.aimLocation;
		if (curTacOrder.code == TACTICAL_ORDER_ATTACK_POINT)
		{
			tPoint = getAttackTargetPoint();
			targetPoint = &tPoint;

			//MUST set the suppressionFire flag if we are host and this is from client
			// AND we are an artilleryPiece WITHOUT suppressionFire set AND
			// OUR attackParams are method == ATTACKMETHOD_RANGED, range = FIRERANGE_CURRENT
			// AND pursue = false.  Complex but keeps from having to figure out how to pass
			// the friggering flag around.
			if (MPlayer && MPlayer->isServer())
			{
				if (myVehicle->isGuardTower() && !(myVehicle->suppressionFire) &&
					(curTacOrder.attackParams.method == ATTACKMETHOD_RANGED) &&
					(curTacOrder.attackParams.range == FIRERANGE_CURRENT) &&
					(curTacOrder.attackParams.pursue == false))
				{
					myVehicle->suppressionFire = true;
				}
			}
		}
	}
	else
	{
		if (lastTargetConserveAmmo)
			attackType = ATTACK_CONSERVING_AMMO;
	}

	if (target) {
		if (!curTacOrder.isCombatOrder()) {
			glm::vec3 pos = target->getPosition();
			if (myVehicle->distanceFrom(pos) > attackRadius) {
				setLastTarget(nullptr);
				target = nullptr;
			}
		}
	}
	
	if (!target && (curTacOrder.code != TACTICAL_ORDER_ATTACK_POINT)) {
		#if WARRIOR_DEBUGGING_ENABLED
			if (debugFlags & WARRIOR_DEBUG_FLAG_COMBAT) {
				char s[128];
				mc2::fmt::to_buf(s, "{} ({:.2f}) has no attack target.\n", mc2::fmt::safe(callsign), getSituationFireRange());
				debugPrint(s, true);
			}
		#endif
		return(result);
	}

	if (target) {
		if (target->isDestroyed()) {
			#if WARRIOR_DEBUGGING_ENABLED
				if (debugFlags & WARRIOR_DEBUG_FLAG_COMBAT) {
					char s[128];
					mc2::fmt::to_buf(s, "{} ({:.2f}) has a destroyed target.\n", mc2::fmt::safe(callsign), getSituationFireRange());
					debugPrint(s, true);
				}
			#endif
			return(result);
		}

		if (target->isDisabled() && !lastTargetObliterate) {
			#if WARRIOR_DEBUGGING_ENABLED
				if (debugFlags & WARRIOR_DEBUG_FLAG_COMBAT) {
					char s[128];
					mc2::fmt::to_buf(s, "{} ({:.2f}) has a disabled target.\n", mc2::fmt::safe(callsign), getSituationFireRange());
					debugPrint(s, true);
				}
			#endif
			return(result);
		}
	}

	#if WARRIOR_DEBUGGING_ENABLED
		if (debugFlags & WARRIOR_DEBUG_FLAG_COMBAT) {
			char s[512];
			if (!myVehicle->canFireWeapons()) {
				mc2::fmt::to_buf(s, "{}'s ({:.2f}) vehicle cannot fire now.\n", mc2::fmt::safe(callsign), getSituationFireRange());
				debugPrint(s, true);
				}
			else if (weaponsStatusResult < 1) {
				switch (weaponsStatusResult) {
					case WEAPONS_STATUS_CANNOT_FIRE:
						mc2::fmt::to_buf(s, "{}'s ({:.2f}) vehicle cannot fire now.", mc2::fmt::safe(callsign), getSituationFireRange());
						break;
					case WEAPONS_STATUS_NO_TARGET:
						mc2::fmt::to_buf(s, "{} ({:.2f}) has no target.\n", mc2::fmt::safe(callsign), getSituationFireRange());
						break;
					case WEAPONS_STATUS_OUT_OF_RANGE:
						mc2::fmt::to_buf(s, "{} ({:.2f}) out of range.\n", mc2::fmt::safe(callsign), getSituationFireRange());
						break;
					case 0: {
						long numWeaponsNotReady = 0;
						long numWeaponsOutOfRange = 0;
						long numWeaponsNotLocked = 0;
						long numWeaponsOutOfAmmo = 0;
						long numWeaponsNoChance = 0;
						long numWeaponsTooHot = 0;
						for (long i = 0; i < myVehicle->numWeapons; i++) {
							if (weaponsStatus[i] == WEAPON_STATUS_NOT_READY)
								numWeaponsNotReady++;
							if (weaponsStatus[i] == WEAPON_STATUS_OUT_OF_AMMO)
								numWeaponsOutOfAmmo++;
							if (weaponsStatus[i] == WEAPON_STATUS_OUT_OF_RANGE)
								numWeaponsOutOfRange++;
							if (weaponsStatus[i] == WEAPON_STATUS_NOT_LOCKED)
								numWeaponsNotLocked++;
							if (weaponsStatus[i] == WEAPON_STATUS_NO_CHANCE)
								numWeaponsNoChance++;
							if (weaponsStatus[i] == WEAPON_STATUS_TOO_HOT)
								numWeaponsTooHot++;
						}
						mc2::fmt::to_buf(s, "{} ({:.2f}) has no shot: {:d} !ready, {:d} !ammo, {:d} !inrange, {:d} !locked, {:d} !chance, {:d} hot\n", mc2::fmt::safe(callsign), getSituationFireRange(), mc2::fmt::as_i32(numWeaponsNotReady), mc2::fmt::as_i32(numWeaponsOutOfAmmo), mc2::fmt::as_i32(numWeaponsOutOfRange), mc2::fmt::as_i32(numWeaponsNotLocked), mc2::fmt::as_i32(numWeaponsNoChance), mc2::fmt::as_i32(numWeaponsTooHot));
						}
						break;
					default:
						mc2::fmt::to_buf(s, "{} ({:.2f})  cannot fire for unknown reason.\n", mc2::fmt::safe(callsign), getSituationFireRange());
				}
				debugPrint(s, true);
			}
		}
	#endif

	// So we don't muck up the brains at this late date, simply report we're
	// out of ammo...
	if (attackType != ATTACK_CONSERVING_AMMO && outOfAmmo )
	{
		radioMessage(RADIO_AMMO_OUT, TRUE);
	}

	BOOL cantConserve = FALSE;	// report if we're asked to use ammo conservation and that's impossible
	if (attackType == ATTACK_CONSERVING_AMMO)
		cantConserve = TRUE;

	bool firedWeapon = false;
	if (myVehicle->canFireWeapons() && (weaponsStatusResult > 0)) {
		// First, a taunt...
		// Only Taunt Moving things!!
		if (target && target->isMover() && RollDice(1))
			radioMessage(RADIO_TAUNT, true);

		// Let loose...
		float targetTime = getLastTargetTime();

		// Track how much damage I've done to the target when weapons finally hit.
		// If I've done enough to blow the target, STOP firing!!
		float damageDoneToTarget = 0.0f;

		for (long curWeapon = 0; curWeapon < myVehicle->numWeapons; curWeapon++)
		{
			long weaponIndex = myVehicle->numOther + curWeapon;
			if (weaponsStatus[curWeapon] > 0)
			{
				if (!target || (target && (damageDoneToTarget < target->getDestructLevel())))
				{
					if ( attackType != ATTACK_CONSERVING_AMMO || myVehicle->getWeaponIsEnergy(weaponIndex))
					{
						float dmgDone = 0.0f;
						if ( NO_ERR == myVehicle->fireWeapon(target, targetTime, myVehicle->numOther + curWeapon, attackType, aimLocation, targetPoint, dmgDone) )
							cantConserve = FALSE;
						firedWeapon = true;
						damageDoneToTarget += dmgDone;
					}
				}
			}
			result = NO_ERR;
		}
	}

	// ISSUE-17 (MC2_FIRE_DECISION_TRACE, default OFF = byte-identical): refusal-
	// side instrumentation. The fire decision has six SILENT per-weapon gates in
	// calcWeaponsStatus (recycle / ammo / range / LOS+fire-arc conflated into
	// NOT_LOCKED / odds<1) and none of them is observable — the reason-tally
	// debug block above is dead (WARRIOR_DEBUG_FLAG_COMBAT never set). When a
	// combat order is active and NO weapon fired this combat update, emit one
	// COMBAT JSONL event with the per-gate tallies, plus one direct lineOfSight
	// probe to split LOS from fire-arc within NOT_LOCKED. Naturally throttled to
	// the combat-update cadence (this function re-arms combatUpdate above).
	{
		static const bool s_fireDecisionTrace =
			mc2::gate::get<"MC2_FIRE_DECISION_TRACE">();
		if (s_fireDecisionTrace && !firedWeapon && curTacOrder.isCombatOrder() &&
			mc2_diag::tagEnabled("COMBAT")) {
			long tally[6] = {0, 0, 0, 0, 0, 0};	// [0]=rd [1]=am [2]=rn [3]=lk [4]=ch [5]=hot
			for (long tw = 0; tw < myVehicle->numWeapons; tw++) {
				switch (weaponsStatus[tw]) {
					case WEAPON_STATUS_NOT_READY:	tally[0]++; break;
					case WEAPON_STATUS_OUT_OF_AMMO:	tally[1]++; break;
					case WEAPON_STATUS_OUT_OF_RANGE:tally[2]++; break;
					case WEAPON_STATUS_NOT_LOCKED:	tally[3]++; break;
					case WEAPON_STATUS_NO_CHANCE:	tally[4]++; break;
					case WEAPON_STATUS_TOO_HOT:		tally[5]++; break;
				}
			}
			long losFlag = -1;
			float dist = -1.0f;
			if (target) {
				losFlag = myVehicle->lineOfSight(target) ? 1 : 0;
				dist = myVehicle->distanceFrom(target->getPosition());
			}
			else if (targetPoint) {
				losFlag = myVehicle->lineOfSight(*targetPoint) ? 1 : 0;
				dist = myVehicle->distanceFrom(*targetPoint);
			}
			char _nf[256];
			mc2::fmt::to_buf(_nf, "{{\"ev\":\"nofire\",\"wid\":{:d},\"tgt_wid\":{:d},\"ord\":{:d},\"result\":{:d},\"rd\":{:d},\"am\":{:d},\"rn\":{:d},\"lk\":{:d},\"ch\":{:d},\"hot\":{:d},\"los\":{:d},\"dist\":{:.0f}}}", mc2::fmt::as_i32(static_cast<long>(vehicleWID)), mc2::fmt::as_i32(target ? static_cast<long>(target->getWatchID()) : 0L), mc2::fmt::as_i32(static_cast<int>(curTacOrder.code)), mc2::fmt::as_i32(static_cast<long>(weaponsStatusResult)), mc2::fmt::as_i32(tally[0]), mc2::fmt::as_i32(tally[1]), mc2::fmt::as_i32(tally[2]), mc2::fmt::as_i32(tally[3]), mc2::fmt::as_i32(tally[4]), mc2::fmt::as_i32(tally[5]), mc2::fmt::as_i32(losFlag), static_cast<double>(dist));
			mc2_diag::writeEvent("COMBAT", 1, 0, _nf);
		}
	}

	if (curTacOrder.attackParams.tactic == TACTIC_STOP_AND_FIRE) {
		if (firedWeapon)
			clearMoveOrders();
	}
	if (cantConserve)	// probably a lie
	{
		for (long curWeapon = myVehicle->numOther; curWeapon < myVehicle->numOther + myVehicle->numWeapons; curWeapon++)
		{
			if (myVehicle->getWeaponShots(curWeapon) == UNLIMITED_SHOTS && myVehicle->isWeaponWorking(curWeapon))
			{
				cantConserve = FALSE;	// it is a lie! we have a working energy weapon, it's just recycling or something...
				break;
			}
		}
	}

	if (cantConserve)
	{
		radioMessage(RADIO_ILLEGAL_ORDER);
		setLastTarget(nullptr);
		clearCurTacOrder();	// so we don't keep trying (and announcing that it doesn't work!!)
	}

	return(result);
}

void MechWarrior::updateActions (void) {

	if ((teamId == -1) || !brainsEnabled[teamId]) {
		clearCurTacOrder(false);
		clearMoveOrders();
		clearAttackOrders();
		tacOrder[ORDERSTATE_GENERAL].init();
		tacOrder[ORDERSTATE_PLAYER].init();
		tacOrder[ORDERSTATE_ALARM].init();
		setMainGoal(GOAL_ACTION_NONE, nullptr, nullptr, -1.0);
		return;
	}

	MoverPtr myVehicle = getVehicle();

	if (myVehicle->isCaptured()) {
		clearCurTacOrder();
		setLastTarget(nullptr);
		return;
	}

	// MC2_FX_FORCE_SPAWN fixture (default-OFF). Idle tier1/visual captures spawn
	// NO weapon FX, so the PPC TUBE path never renders headlessly. This forces
	// ONE mech to fire its ENERGY weapons (PPCs -> tube ribbons) at a point in
	// front of it. Mechs ALWAYS tick here (unlike static buildings, which are
	// update-skipped), so the FX drives + renders + cleans via the normal
	// weaponbolt path. Fire-at-POINT: fireWeapon(target=NULL, &targetPoint) --
	// the target-specific logic is if(target)-guarded (mech.cpp). Fires ONCE
	// (global latch). Output is stdout, so it needs MC2_LOG=1 to be visible.
	static const bool s_fxForceSpawn =
		mc2::gate::isSet<"MC2_FX_FORCE_SPAWN">() && !mc2::gate::is<"MC2_FX_FORCE_SPAWN", "0">();
	// VK-FIELD-VFX-2: continuous re-fire mode for a deterministic on-camera capture
	// window. MC2_FX_FORCE_SPAWN=loop (or =2) re-fires one mech's whole weapon bank on a
	// fixed cadence so there is ALWAYS a live bolt on screen during a desktop grab
	// (the =1 fire-once fixture leaves only a ~0.5s transient that is easy to miss).
	static const bool s_fxLoop =
		mc2::gate::is<"MC2_FX_FORCE_SPAWN", "loop">() || mc2::gate::is<"MC2_FX_FORCE_SPAWN", "2">() ||
		mc2::gate::is<"MC2_FX_FORCE_SPAWN", "3">()    || mc2::gate::is<"MC2_FX_FORCE_SPAWN", "4">() ||
		mc2::gate::is<"MC2_FX_FORCE_SPAWN", "5">()    || mc2::gate::is<"MC2_FX_FORCE_SPAWN", "6">() ||
		mc2::gate::is<"MC2_FX_FORCE_SPAWN", "7">()    || mc2::gate::is<"MC2_FX_FORCE_SPAWN", "8">() ||
		mc2::gate::is<"MC2_FX_FORCE_SPAWN", "9">();
	if (s_fxLoop) {
		static float s_nextFire = 0.0f;
		if (myVehicle && scenarioTime > 5.0f && scenarioTime >= s_nextFire && myVehicle->canFireWeapons()) {
			glm::vec3 pos = myVehicle->getPosition();
			glm::vec3 targetPoint = pos;
			targetPoint.x += 150.0f;
			int firedCount = 0;
			for (long cw = 0; cw < myVehicle->numWeapons; cw++) {
				long wi = myVehicle->numOther + cw;
				float dmgDone = 0.0f;
				if (NO_ERR == myVehicle->fireWeapon(nullptr, scenarioTime, wi,
						ATTACK_TO_DESTROY, -1, &targetPoint, dmgDone))
					firedCount++;
			}
			if (firedCount > 0) {
				s_nextFire = scenarioTime + 0.05f;   // ~20 volleys/sec -> a live bolt every frame
				mc2::fmt::print_out("[FX_FORCE_SPAWN loop] event=mech_fire wid={:d} weapons={:d} pos={:.0f},{:.0f},{:.0f} scenarioTime={:.2f}\n",
					mc2::fmt::as_i32(myVehicle->getWatchID()), mc2::fmt::as_i32(firedCount), pos.x, pos.y, pos.z, scenarioTime);
				fflush(stdout);
				// VK-FIELD-VFX-5: fire-event capture arm. With
				// MC2_VK_PRESENT_DUMP_ON_FIRE set, the FIRST loop volley arms the
				// present-PPM pin ~0.5s after itself, so the capture lands
				// mid-volley instead of pin-frame roulette against the mission's
				// intro/scenario clock. Consumed by verifyReadbackContent
				// (engine_present_vk.cpp) + the gameosmain verify window.
				extern uint32_t g_mc2RenderFrame;   // g_mc2FxFireCaptureFrame is a RENDER-clock frame
				extern uint32_t g_mc2FxFireCaptureFrame;
				// VK-FIELD-VFX-8: MC2_VISUAL_CAPTURE_ON_FIRE arms the same pin for
				// the GL engine capture (gos_visual_capture.cpp), so GL/VK pairs
				// capture the same fire-relative instant.
				static const bool s_dumpOnFire =
					(mc2::gate::get<"MC2_VK_PRESENT_DUMP_ON_FIRE">() ||
					 mc2::gate::get<"MC2_VISUAL_CAPTURE_ON_FIRE">());
				if (s_dumpOnFire && g_mc2FxFireCaptureFrame == 0) {
					g_mc2FxFireCaptureFrame = g_mc2RenderFrame + 30u;
					mc2::fmt::print_out("[FX_FORCE_SPAWN loop] event=capture_armed frame={:d} pin={:d}\n", mc2::fmt::as_u32(g_mc2RenderFrame), mc2::fmt::as_u32(g_mc2FxFireCaptureFrame));
					fflush(stdout);
				}
			}
		}
	} else if (s_fxForceSpawn) {
		// Fire ALL weapons (not just energy): tube ribbons come from PPC AND
		// missiles (SRM/LRM/TBolt) -- the energy-only filter both missed the
		// missile tubes and caught non-tube lasers. Spread across up to 8
		// DISTINCT mechs (per-WID latch) so at least one with a tube weapon
		// fires, then stop. dmgDone/target=NULL -> FX only, no real damage.
		static long s_firedWIDs[8];
		static int  s_firedMechs = 0;
		if (s_firedMechs < 8 && myVehicle && scenarioTime > 5.0f && myVehicle->canFireWeapons()) {
			long wid = myVehicle->getWatchID();
			bool already = false;
			for (int i = 0; i < s_firedMechs; i++)
				if (s_firedWIDs[i] == wid) { already = true; break; }
			if (!already) {
				glm::vec3 pos = myVehicle->getPosition();
				glm::vec3 targetPoint = pos;
				targetPoint.x += 150.0f;   // a point in front; any on-map point spawns a bolt
				int firedCount = 0;
				for (long cw = 0; cw < myVehicle->numWeapons; cw++) {
					long wi = myVehicle->numOther + cw;
					float dmgDone = 0.0f;
					if (NO_ERR == myVehicle->fireWeapon(nullptr, scenarioTime, wi,
							ATTACK_TO_DESTROY, -1, &targetPoint, dmgDone))
						firedCount++;
				}
				if (firedCount > 0) {
					s_firedWIDs[s_firedMechs++] = wid;
					mc2::fmt::print_out("[FX_FORCE_SPAWN v1] event=mech_fire mech={:d} wid={:d} weapons={:d} pos={:.0f},{:.0f},{:.0f} scenarioTime={:.2f}\n", mc2::fmt::as_i32(s_firedMechs), mc2::fmt::as_i32(wid), mc2::fmt::as_i32(firedCount), pos.x, pos.y, pos.z, scenarioTime);
					fflush(stdout);
				}
			}
		}
	}

	// MC2_AIMED_FIRE_FIXTURE (ISSUE-9 probe, default-OFF = byte-identical):
	// headless called-shot driver. Every home-team mech without an active combat
	// order is issued an aimed-HEAD attack (ATTACK_TO_DESTROY / RANGED /
	// OPTIMAL, aim = MECH_BODY_LOCATION_HEAD, pursue) at the nearest live enemy
	// mech. Origin MUST be ORDER_ORIGIN_COMMANDER: orderAttackObject only
	// installs the tac order (setGeneralTacOrder -> curTacOrder) for COMMANDER
	// origin — with PLAYER origin only lastTarget is set and fire is UNAIMED
	// (aimLocation flows solely from curTacOrder.attackParams in
	// combatDecisionTree). Re-issues when the order clears (target died), so a
	// run with MC2_DIAG_TAGS=COMBAT yields a hit-location distribution ("loc"
	// field) to A/B MC2_CALLED_SHOT_FIX. Global 2 s throttle bounds order spam.
	static const bool s_aimedFireFixture =
		mc2::gate::get<"MC2_AIMED_FIRE_FIXTURE">();
	if (s_aimedFireFixture && myVehicle && myVehicle->isMech() &&
		Team::home && (myVehicle->getTeam() == Team::home) &&
		(scenarioTime > 5.0f) && !curTacOrder.isCombatOrder() &&
		myVehicle->canFireWeapons()) {
		static float s_nextAimedOrderTime = 0.0f;
		if (scenarioTime >= s_nextAimedOrderTime) {
			s_nextAimedOrderTime = scenarioTime + 2.0f;
			MoverPtr enemy = nullptr;
			float bestDist = 1e30f;
			// Mover iteration (not getMech): warrior.cpp only sees the BattleMech
			// forward declaration (mech.h is USE_MECHS-gated here).
			for (long mi = 0; mi < ObjectManager->getNumMovers(); mi++) {
				MoverPtr pM = ObjectManager->getMover(mi);
				if (!pM || !pM->isMech() || pM->isDisabled() || pM->isDestroyed())
					continue;
				if (pM->getTeamId() == Team::home->getId())
					continue;
				float d = myVehicle->distanceFrom(pM->getPosition());
				if (d < bestDist) {
					bestDist = d;
					enemy = pM;
				}
			}
			if (enemy) {
				long res = orderAttackObject(false, ORDER_ORIGIN_COMMANDER, enemy,
					ATTACK_TO_DESTROY, ATTACKMETHOD_RANGED, FIRERANGE_OPTIMAL,
					MECH_BODY_LOCATION_HEAD, -1, TACORDER_PARAM_PURSUE);
				mc2::fmt::print_out("[AIMED_FIRE v1] event=order wid={:d} tgt_wid={:d} dist={:.0f} res={:d}\n", mc2::fmt::as_i32(static_cast<long>(vehicleWID)), mc2::fmt::as_i32(static_cast<long>(enemy->getWatchID())), static_cast<double>(bestDist), mc2::fmt::as_i32(res));
				fflush(stdout);
			}
		}
	}

	if (combatUpdate <= scenarioTime)
		combatDecisionTree();

	movementDecisionTree();
}

//extern L_INTEGER startCk;
//extern L_INTEGER endCk;
//extern long		 srWAblUpd;

long MechWarrior::mainDecisionTree (void) {

    // sebi: WTF??? path is an array of2 elements => comparison always true
	//Assert(moveOrders.path != NULL, 0, " bad warrior path ");
	Assert(moveOrders.path[0] != nullptr, 0, " bad warrior path ");

	if ((teamId == -1) || !brainsEnabled[teamId]) {
		clearCurTacOrder(false);
		tacOrder[ORDERSTATE_GENERAL].init();
		tacOrder[ORDERSTATE_PLAYER].init();
		tacOrder[ORDERSTATE_ALARM].init();
		setMainGoal(GOAL_ACTION_NONE, nullptr, nullptr, -1.0);
	}

	//If we currently have no order, check if we have any player orders queued...
	if (curTacOrder.code == TACTICAL_ORDER_NONE) {
		if (tacOrderQueueExecuting && (numTacOrdersQueued > 0)) {
			if (!newTacOrderReceived[ORDERSTATE_PLAYER]) {
				// First queued order just given, so begin executing it immediately...
				executeTacOrderQueue();
			}
		}
	}
	
	// Is the current tac order accomplished?
	if ((curTacOrder.code != TACTICAL_ORDER_NONE) && (curTacOrder.status(this) == TACORDER_SUCCESS)) {
		bool playerOrderQueued = false;
		if ((orderState == ORDERSTATE_PLAYER) && tacOrderQueueExecuting) {
			executeTacOrderQueue();
			playerOrderQueued = newTacOrderReceived[ORDERSTATE_PLAYER];
		}
		clearCurTacOrder();
		newTacOrderReceived[ORDERSTATE_PLAYER] = playerOrderQueued;
	}

	// If we're without orders now, let's record the time...
	if (underHomeCommand() && (curTacOrder.code == TACTICAL_ORDER_NONE) && (timeOfLastOrders < 0.0))
		timeOfLastOrders = scenarioTime;

	// Update Weapons Status, if needed this frame...
	if ((brainUpdate <= scenarioTime) || (combatUpdate <= scenarioTime) || (movementUpdate <= scenarioTime)) {
		GameObjectPtr target = getCurrentTarget(); //getAttackTarget(ORDER_CURRENT);
		if (target)
			weaponsStatusResult = calcWeaponsStatus(target, weaponsStatus);
		else if (curTacOrder.code == TACTICAL_ORDER_ATTACK_POINT) {
			glm::vec3 pos = getAttackTargetPoint();
			weaponsStatusResult = calcWeaponsStatus(target, weaponsStatus, &pos);
		}
	}

	// Make sure, if I have a target, it's still legit.
	// Now, when we go into runBrain, we know the current target is up-to-date with current
	// game conditions...
	GameObjectPtr tacOrderTarget = nullptr;
	if (curTacOrder.code != TACTICAL_ORDER_NONE)
		tacOrderTarget = ObjectManager->getByWatchID(curTacOrder.targetWID);
	GameObjectPtr target = getLastTarget();
	if (target) {
		bool clearTarget = false;
		if (target->isDestroyed())
			clearTarget = true;
		else if (target->isDisabled() && !lastTargetObliterate)
			clearTarget = true;
		if (clearTarget) {
			setLastTarget(nullptr);
			if ((curTacOrder.code == TACTICAL_ORDER_ATTACK_OBJECT) && (target == tacOrderTarget)) {
				clearCurTacOrder();
				target = nullptr;
				tacOrderTarget = nullptr;
			}
		}
	}
	if ((curTacOrder.code == TACTICAL_ORDER_ATTACK_OBJECT) && (target != tacOrderTarget))
		setLastTarget(tacOrderTarget);
	
	// Update pilot brain...
	// BRAIN-FIXED-TICK-1: advance global sim-time accumulator once per render frame,
	// before the per-warrior gate so even staggered-out warriors contribute to the advance.
	initBrainFixedTickGate();
	if (s_brainFixedTickEnabled && s_brainAccumLastFrame != g_mc2FrameCounter) {
		s_brainAccumLastFrame = g_mc2FrameCounter;
		bool ticked = advanceBrainFixedTickAccumulator(scenarioTime);
		if (s_brainFixedTickTrace && ticked) {
			mc2::fmt::print_err("[BRAIN_FIXED_TICK] tick={:d} simTime={:.4f} ranThisFrame={:d}\n", mc2::fmt::as_u32(s_brainTickIndex), static_cast<double>(scenarioTime), mc2::fmt::as_u32(s_brainTicksThisFrame));
			std::fflush(stderr);
		}
	}
	if ((brainUpdate <= scenarioTime) && ((teamId == -1) || brainsEnabled[teamId])) {
		// MC2_BRAIN_TASKQ: drain one task per brain cadence (inert stub — no tacOrder writes this slice)
		initBrainTaskQGate();
		if (s_brainTaskQEnabled) {
			if (!brainTaskQueue) {
				brainTaskQueue = new BrainTaskQueue();
			}
			if (s_brainTaskQTraceEnabled) {
				brainTaskQueue->dumpTrace(vehicleWID);
			}
			// Task queue allocated here; consumed inside runBrain() per BRAIN-RUNTIME-1B.
		}

		// MC2_BRAIN_RUNTIME: init struct and set mode from global forced mode (per-unit FIT override applied at mission load).
		initBrainRuntimeGate();
		if (s_brainRuntimeEnabled && s_brainTaskQEnabled) {
			if (!brainRuntime) {
				brainRuntime = new MechBrainRuntime();
				// Set mode: per-unit FIT loader (mission_ai.fit) may have already set brainRuntime->mode
				// at mission load via loadBrainRuntimeFromFit(). If not yet created, use global forced mode.
				brainRuntime->mode = s_brainRuntimeForcedMode;
				// GAP-A MULTI-WARRIOR SPECIALS: this runtime was allocated lazily — AFTER the
				// mission.cpp specials-load loop — so its specialBody is empty. Copy the parsed
				// body from the mission-level cache so this warrior dispatches like the
				// _ai.fit-recorded ones. No-op when the cache is empty (gate-OFF / no file).
				applyCachedSpecialBodyToRuntime(brainRuntime);
			} else {
				// GAP-A-ROBUSTNESS-1: a brainRuntime can be eager-allocated BEFORE this point by
				// the [Tactics] loader (mission.cpp setBrainRuntimeMode(Legacy), so tactic weights
				// can attach). That pre-empts the lazy-alloc branch above — so without this, the
				// mission-level specials cache is never applied AND the runtime stays Legacy →
				// enhancedApply==false → no Enhanced dispatch (observed: TACTIC_WEIGHTS=1 collapses
				// multi-warrior patrol 70→1). When a forced mode is set (FORCE_MODE != Legacy),
				// honor it over the eager Legacy default; then apply the cached specials. Both are
				// no-ops if already satisfied (idempotent — safe to run every tick).
				if (s_brainRuntimeForcedMode != BrainRuntimeMode::Legacy &&
				    brainRuntime->mode != s_brainRuntimeForcedMode) {
					brainRuntime->mode = s_brainRuntimeForcedMode;
				}
				applyCachedSpecialBodyToRuntime(brainRuntime);
			}
		}

		// BRAIN-RUNTIME-1B seam: short-circuit logic lives INSIDE runBrain().
		// APPLY=0: trace-log the would-own mask (1A behavior, byte-identical to baseline).
		// APPLY=1+Enhanced: runBrain() skips brain->execute(), pushes/drains HOLD_TASK.
		// All paths: CurGroup/CurWarrior clear + clearAlarmsHistory() run inside runBrain().
		if (s_brainRuntimeEnabled && s_brainTaskQEnabled && brainRuntime) {
			if (s_brainRuntimeTraceEnabled && !s_brainRuntimeApplyEnabled) {
				uint8_t wouldOwn = brainRuntime->computeWouldOwnMask();
				// Build human-readable slot list
				char slotsBuf[32] = "(none)";
				if (wouldOwn) {
					slotsBuf[0] = '\0';
					if (wouldOwn & kBrainOwnsGeneral) { if (slotsBuf[0]) std::strcat(slotsBuf, "|"); std::strcat(slotsBuf, "GENERAL"); }
					if (wouldOwn & kBrainOwnsPlayer)  { if (slotsBuf[0]) std::strcat(slotsBuf, "|"); std::strcat(slotsBuf, "PLAYER");  }
					if (wouldOwn & kBrainOwnsAlarm)   { if (slotsBuf[0]) std::strcat(slotsBuf, "|"); std::strcat(slotsBuf, "ALARM");   }
				}
				mc2::fmt::print_err("[BRAIN_RT] wid={:d} mode={} would-own {} (NOT applied)\n", mc2::fmt::as_i32(vehicleWID), mc2::fmt::safe(MechBrainRuntime::modeString(brainRuntime->mode)), mc2::fmt::safe(slotsBuf));
			}
		}
		// runBrain() always called — short-circuit is gated inside it.
		runBrain();
		// BRAIN-FIXED-TICK-1: advance brainUpdate by fixed tick period (gate ON)
		// or legacy BrainUpdateFrequency (gate OFF — byte-identical to baseline).
		if (s_brainFixedTickEnabled) {
			brainUpdate += s_brainFixedTickPeriod;
		} else {
			brainUpdate += BrainUpdateFrequency;
		}
	}

	// In case the brain set a new target, let's recalc weaponsStatus...
	target = getLastTarget(); //getAttackTarget(ORDER_CURRENT);
	if (target && (lastTargetTime/*getLastTargetTime()*/ == scenarioTime))
		weaponsStatusResult = calcWeaponsStatus(target, weaponsStatus);

	// Any Alarms?
	checkAlarms();

	// Now, check if we have a new current tac order...
	TacticalOrder newTacOrder;
	switch (orderState) {
		case ORDERSTATE_GENERAL:
			if (newTacOrderReceived[ORDERSTATE_ALARM]) {
                suspendAuthoredRoute(this);
				clearCurTacOrder(false);
				newTacOrder = tacOrder[ORDERSTATE_ALARM];
				orderState = ORDERSTATE_ALARM;
				}
			else if (newTacOrderReceived[ORDERSTATE_PLAYER]) {
				// If we're currently executing a general (brain) order,
				// purge it permanently (NOW, we DON'T want them to
				// fall back to the general order--the brain must re-order
				// it)...
  				tacOrder[ORDERSTATE_GENERAL].init();
				// Now grab the player order...
				newTacOrder = tacOrder[ORDERSTATE_PLAYER];
				orderState = ORDERSTATE_PLAYER;
				}
			else if (newTacOrderReceived[ORDERSTATE_GENERAL])
				newTacOrder = tacOrder[ORDERSTATE_GENERAL];
			break;
		case ORDERSTATE_PLAYER:
			if (newTacOrderReceived[ORDERSTATE_ALARM]) {
				newTacOrder = tacOrder[ORDERSTATE_ALARM];
				orderState = ORDERSTATE_ALARM;
				}
			else if (newTacOrderReceived[ORDERSTATE_PLAYER]) {
				// Make sure we have no general order we fall back to when
				// we finish this, per Tim's request...
  				tacOrder[ORDERSTATE_GENERAL].init();
				newTacOrder = tacOrder[ORDERSTATE_PLAYER];
			}
			break;
		case ORDERSTATE_ALARM:
			// If the player issues a new order, let it override any current
			// alarm (self-issued) order...
			if (newTacOrderReceived[ORDERSTATE_PLAYER]) {
				alarmPriority = 0;
  				tacOrder[ORDERSTATE_ALARM].init();
				newTacOrder = tacOrder[ORDERSTATE_PLAYER];
				}
			else if (newTacOrderReceived[ORDERSTATE_ALARM])
				newTacOrder = tacOrder[ORDERSTATE_ALARM];
			break;
	}
	if (newTacOrder.code != TACTICAL_ORDER_NONE) {
		// Note how we need to copy the move path over, since we
		// allocate it only once now...
		MovePathPtr tempPath[2];
		if ((newTacOrder.code == TACTICAL_ORDER_MOVETO_POINT) || (newTacOrder.code == TACTICAL_ORDER_MOVETO_OBJECT)) {
			for (long i = 0; i < 2; i++) {
				tempPath[i] = moveOrders.path[i];
				if (i > 0)
					tempPath[i]->numSteps = 0;
			}
			}
		else {
			for (long i = 0; i < 2; i++) {
				tempPath[i] = moveOrders.path[i];
				tempPath[i]->numSteps = 0;
			}
		}
		long tempMoveState = moveOrders.moveState;
		long tempMoveStateGoal = moveOrders.moveStateGoal;
		bool running = moveOrders.run;
		moveOrders.init();
		moveOrders.run = running;
		PathManager->remove(this);
		moveOrders.moveState = tempMoveState;
		moveOrders.moveStateGoal = tempMoveStateGoal;
		moveOrders.path[0] = tempPath[0];
		moveOrders.path[1] = tempPath[1];
		attackOrders.init();
		long message = -1;
		newTacOrder.execute(this, message);
		if (orderState == ORDERSTATE_PLAYER)
			radioMessage(message, true);
		setCurTacOrder(newTacOrder);
		if (useGoalPlan) {
			glm::vec3 movePoint = newTacOrder.getWayPoint(0);
			if (!newTacOrder.subOrder)
				switch (newTacOrder.code) {
					case TACTICAL_ORDER_MOVETO_POINT:
						setMainGoal(GOAL_ACTION_MOVE, nullptr, &movePoint, -1.0);
						break;
					case TACTICAL_ORDER_ATTACK_OBJECT:
						setMainGoal(GOAL_ACTION_ATTACK, newTacOrder.getTarget(), nullptr, -1.0);
						break;
					case TACTICAL_ORDER_CAPTURE:
						setMainGoal(GOAL_ACTION_CAPTURE, newTacOrder.getTarget(), nullptr, 400.0);
						break;
					default:
						setMainGoal(GOAL_ACTION_NONE, nullptr, nullptr, -1.0);
				}
		}
		timeOfLastOrders = -1.0;
	}

	// These must be cleared every frame...
	newTacOrderReceived[ORDERSTATE_GENERAL] = false;
	newTacOrderReceived[ORDERSTATE_PLAYER] = false;
	newTacOrderReceived[ORDERSTATE_ALARM] = false;

	// Now that we know what we're doing, do it...
	updateActions();

	return(NO_ERR);
}

long MechWarrior::loadBrainParameters (FitIniFile* brainFile, long warriorId) {

	if (!brain)
		Fatal(0, " Warrior.loadBrainParameters: NULL brain ");

	// Main Info section
	char warriorName[32];
	mc2::fmt::to_buf(warriorName, "Warrior{:d}", mc2::fmt::as_i32(warriorId));
	long result = brainFile->seekBlock(warriorName);
	if (result != NO_ERR)
		return(result);

	long numCells = 0;
	result = brainFile->readIdLong("NumCells", numCells);
	if (result != NO_ERR)
		return(result);
		
	long numStaticVars = 0;
	result = brainFile->readIdLong("NumStaticVars", numStaticVars);
	if (result != NO_ERR)
		return(result);

	for (long i = 0; i < numCells; i++) {
		char blockName[64];
		mc2::fmt::to_buf(blockName, "{}Cell{:d}", mc2::fmt::safe(warriorName), mc2::fmt::as_i32(i));
		long result = brainFile->seekBlock(blockName);
		if (result != NO_ERR)
			return(result);
		long cell = 0;
		result = brainFile->readIdLong("Cell", cell);
		if (result != NO_ERR)
			return(result);
		long memType = 0;
		result = brainFile->readIdLong("MemType", memType);
		if (result != NO_ERR)
			return(result);
		switch (memType) {
			case 0: {
				// Integer Memory
				long value = 0;
				result = brainFile->readIdLong("Value", value);
				if (result != NO_ERR)
					return(result);
				setIntegerMemory(cell, value);
				}
				break;
			case 1: {
				// Real Memory
				float value = 0.0;
				result = brainFile->readIdFloat("Value", value);
				if (result != NO_ERR)
					return(result);
				setRealMemory(cell, value);
				}
				break;
			default:
				result = 666;
				return(result);
		}
	}

	for (int i = 0; i < numStaticVars; i++) {
		char blockName[64];
		mc2::fmt::to_buf(blockName, "{}Static{:d}", mc2::fmt::safe(warriorName), mc2::fmt::as_i32(i));
		long result = brainFile->seekBlock(blockName);
		if (result != NO_ERR)
			return(result);
		long type = 0;
		result = brainFile->readIdLong("type", type);
		if (result != NO_ERR)
			return(result);
		char varName[256];
		result = brainFile->readIdString("Name", varName, 255);
		if (result != NO_ERR)
			return(result);
		switch (type) {
			case 0: {
				// Integer
				long value = 0;
				result = brainFile->readIdLong("Value", value);
				if (result != NO_ERR)
					return(result);

				brain->setStaticInteger(varName, value);
				}
				break;
			case 1: {
				// Real
				float value = 0;
				result = brainFile->readIdFloat("Value", value);
				if (result != NO_ERR)
					return(result);

				brain->setStaticReal(varName, value);
				}
				break;
			case 2: {
				// Integer Array
				long numValues = 0;
				result = brainFile->readIdLong("NumValues", numValues);
				if (result != NO_ERR)
					return(result);

				int values[1024];
				result = brainFile->readIdIntArray("Values", values, numValues);
				if (result != NO_ERR)
					return(result);

				brain->setStaticIntegerArray(varName, numValues, values);
				}
				break;
			case 3: {
				// Real Array
				long numValues = 0;
				result = brainFile->readIdLong("NumValues", numValues);
				if (result != NO_ERR)
					return(result);

				float values[1024];
				result = brainFile->readIdFloatArray("Values", values, numValues);
				if (result != NO_ERR)
					return(result);

				brain->setStaticRealArray(varName, numValues, values);
				}
				break;
			default:
				result = 667;
				return(result);
		}
	}

	return(NO_ERR);
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
