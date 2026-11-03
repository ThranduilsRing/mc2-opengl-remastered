// ABL native bindings: text messages, teleport, roster/pilot operations, campaign globals, strike toggles, radius counts, omni stubs, trigger areas and off-map queries.
// Declared in ablmc2_internal.h; registered, in order, by initABL in ablmc2.cpp.
#include "ablmc2_internal.h"
#include "abl_script_events.h"
#include <algorithm>
#include <iterator>
#include <span>


#include "mc2fmt.h"


// --- Omnitech ABL extensions ---
// Wrappers for ~40 ABL functions referenced by Omnitech mission-pack scripts
// (corebrain.abx / pbrain.abl) that were never registered in the baseline
// engine. Implementations are best-effort: mission/messaging/economy/mover
// manipulation are functional; GUI support-strike toggles and pilot-file
// creation are safe stubs (pop all args, push default, log). None of these
// may crash or leave the ABL stack unbalanced.

// File-scope state for Omnitech extension functions.
static int   gCampaignGlobalVars[16] = {0};
static bool  gAblAirStrikeEnabled       = true;
static bool  gAblSensorStrikeEnabled    = true;
static bool  gAblArtilleryPieceEnabled  = true;
static bool  gAblRepairTruckEnabled     = true;
static bool  gAblSalvageCraftEnabled    = true;

static inline void omniTrace (const char* tag) {
	char s[128];
	mc2::fmt::to_buf(s, "[OMNI] {}", mc2::fmt::safe(tag));
	DEBUGWINS_print(s, 0);
}

void execSetTextMsg (void) {

	//Display short text message (Omnitech).
	//		PARAMS: integer type, any msg, integer seconds
	//		Returns: nothing

	long msgType = ABLi_popInteger();
	ABLStackItem value;
	ABLi_popAnything(&value);
	long seconds = ABLi_popInteger();
	(void)msgType; (void)seconds;

	char s[512];
	if (value.type == std::to_underlying(ABLStackItemType::ABL_STACKITEM_CHAR_PTR) && value.data.characterPtr)
		mc2::fmt::to_buf(s, "[OMNI.TEXT] {}", mc2::fmt::safe(value.data.characterPtr));
	else if (value.type == std::to_underlying(ABLStackItemType::ABL_STACKITEM_INTEGER))
		mc2::fmt::to_buf(s, "[OMNI.TEXT] #{:d}", mc2::fmt::as_i32(value.data.integer));
	else
		mc2::fmt::to_buf(s, "[OMNI.TEXT] (type={:d})", mc2::fmt::as_i32(static_cast<int>(value.type)));
	DEBUGWINS_print(s, 0);
}

void execSetLargeMsg (void) {

	//Display long text message (Omnitech).
	//		PARAMS: integer type, any msg, integer seconds
	//		Returns: nothing

	long msgType = ABLi_popInteger();
	ABLStackItem value;
	ABLi_popAnything(&value);
	long seconds = ABLi_popInteger();
	(void)msgType; (void)seconds;

	char s[512];
	if (value.type == std::to_underlying(ABLStackItemType::ABL_STACKITEM_CHAR_PTR) && value.data.characterPtr)
		mc2::fmt::to_buf(s, "[OMNI.LARGE] {}", mc2::fmt::safe(value.data.characterPtr));
	else if (value.type == std::to_underlying(ABLStackItemType::ABL_STACKITEM_INTEGER))
		mc2::fmt::to_buf(s, "[OMNI.LARGE] #{:d}", mc2::fmt::as_i32(value.data.integer));
	else
		mc2::fmt::to_buf(s, "[OMNI.LARGE] (type={:d})", mc2::fmt::as_i32(static_cast<int>(value.type)));
	DEBUGWINS_print(s, 0);
}

void execSetMissionTune (void) {

	//Change the mission's looping music (Omnitech).
	//		PARAMS: integer tuneId
	//		Returns: nothing

	long tuneId = ABLi_popInteger();
	if (soundSystem)
		soundSystem->playABLDigitalMusic(tuneId);
}

void execAddResourcePoints (void) {

	//Add resource points to the player's RP pool (Omnitech).
	//		PARAMS: integer amount
	//		Returns: nothing

	long amt = ABLi_popInteger();
	if (LogisticsData::instance)
		LogisticsData::instance->addResourcePoints(static_cast<int>(amt));
}

void execAddMoney (void) {

	//Alias for addresourcepoints in Omnitech scripts.
	//		PARAMS: integer amount
	//		Returns: nothing

	long amt = ABLi_popInteger();
	if (LogisticsData::instance)
		LogisticsData::instance->addResourcePoints(static_cast<int>(amt));
}

void execSetTxtBuildingName (void) {

	//Set a building's display name (Omnitech).
	//		PARAMS: integer objectId, char* name
	//		Returns: nothing

	long objectId = ABLi_popInteger();
	char* name = ABLi_popCharPtr();

	GameObjectPtr obj = getObject(objectId);
	if (obj && obj->isMover() && name)
		(static_cast<MoverPtr>(obj))->setName(name);
}

void execAblPrint (void) {

	//Omnitech logging helper. Routes to debug overlay prefixed with file name.
	//		PARAMS: char* file, any value
	//		Returns: nothing

	char* fileName = ABLi_popCharPtr();
	ABLStackItem value;
	ABLi_popAnything(&value);

	char s[512];
	const char* fn = fileName ? fileName : "abl";
	switch (static_cast<ABLStackItemType>(value.type)) {
		case ABLStackItemType::ABL_STACKITEM_CHAR:
			mc2::fmt::to_buf(s, "[ABL:{}] char={:c}", mc2::fmt::safe(fn), mc2::fmt::as_char(value.data.character)); break;
		case ABLStackItemType::ABL_STACKITEM_INTEGER:
			mc2::fmt::to_buf(s, "[ABL:{}] int={:d}", mc2::fmt::safe(fn), mc2::fmt::as_i32(value.data.integer)); break;
		case ABLStackItemType::ABL_STACKITEM_REAL:
			mc2::fmt::to_buf(s, "[ABL:{}] real={:.2f}", mc2::fmt::safe(fn), value.data.real); break;
		case ABLStackItemType::ABL_STACKITEM_BOOLEAN:
			mc2::fmt::to_buf(s, "[ABL:{}] bool={}", mc2::fmt::safe(fn), mc2::fmt::safe(value.data.boolean ? "true" : "false")); break;
		case ABLStackItemType::ABL_STACKITEM_CHAR_PTR:
			mc2::fmt::to_buf(s, "[ABL:{}] str={}", mc2::fmt::safe(fn), mc2::fmt::safe(value.data.characterPtr ? value.data.characterPtr : "")); break;
		default:
			mc2::fmt::to_buf(s, "[ABL:{}] (type={:d})", mc2::fmt::safe(fn), mc2::fmt::as_i32(static_cast<int>(value.type))); break;
	}
	DEBUGWINS_print(s, 0);
}

void execTeleportToPoint (void) {

	//Teleport a mover or object to a world point (Omnitech).
	//		PARAMS: integer partId, real[3] position
	//		Returns: nothing

	long partId = ABLi_popInteger();
	float* coordList = ABLi_popRealPtr();
	if (!coordList)
		return;

	glm::vec3 pos;
	pos.x = coordList[0];
	pos.y = coordList[1];
	pos.z = coordList[2];

	if (isUnitId(partId)) {
		long numObjects = getMovers(partId, moverList);
		for (long i = 0; i < numObjects; i++)
			if (moverList[i])
				moverList[i]->setPosition(pos);
	}
	else {
		GameObjectPtr obj = getObject(partId);
		if (obj)
			obj->setPosition(pos, true);
	}
}

void execAddMoverToPlayer (void) {

	//Transfer a mover (or mover-group) to a new team/commander (Omnitech).
	//		PARAMS: integer partId, integer teamId, integer cmdrId
	//		Returns: nothing

	long partId = ABLi_popInteger();
	long teamId = ABLi_popInteger();
	long cmdrId = ABLi_popInteger();

	if (teamId < 0 || teamId >= MAX_TEAMS)
		return;
	TeamPtr team = Team::teams[teamId];
	if (!team)
		return;

	if (isUnitId(partId)) {
		long numObjects = getMovers(partId, moverList);
		for (long i = 0; i < numObjects; i++) {
			MoverPtr m = moverList[i];
			if (!m)
				continue;
			m->setTeam(team);
			m->setCommanderId(cmdrId);
			team->addToRoster(m);
		}
	}
	else {
		GameObjectPtr obj = getObject(partId);
		if (obj && obj->isMover()) {
			obj->setTeam(team);
			obj->setCommanderId(cmdrId);
			team->addToRoster(static_cast<MoverPtr>(obj));
		}
	}
}

void execRemoveMoverFromPlayer (void) {

	//Remove a mover (or mover-group) from a team (Omnitech).
	//		PARAMS: integer partId, integer teamId, integer cmdrId
	//		Returns: nothing

	long partId = ABLi_popInteger();
	long teamId = ABLi_popInteger();
	long cmdrId = ABLi_popInteger();
	(void)cmdrId;

	if (teamId < 0 || teamId >= MAX_TEAMS)
		return;
	TeamPtr team = Team::teams[teamId];
	if (!team)
		return;

	if (isUnitId(partId)) {
		long numObjects = getMovers(partId, moverList);
		for (long i = 0; i < numObjects; i++)
			if (moverList[i])
				team->removeFromRoster(moverList[i]);
	}
	else {
		GameObjectPtr obj = getObject(partId);
		if (obj && obj->isMover())
			team->removeFromRoster(static_cast<MoverPtr>(obj));
	}
}

void execSetBrainFixed (void) {

	//Replace a unit's brain script by name (Omnitech). partId -1 = CurObject.
	//		PARAMS: integer partId, char* brainName
	//		Returns: nothing

	long partId = ABLi_popInteger();
	char* brainName = ABLi_popCharPtr();
	if (!brainName)
		return;

	GameObjectPtr obj = (partId == -1) ? CurObject : getObject(partId);
	if (obj && obj->isMover()) {
		MoverPtr m = static_cast<MoverPtr>(obj);
		if (m->getPilot())
			m->getPilot()->setBrainName(brainName);
	}
}

void execSetBrainNew (void) {

	//Alias of setbrain (Omnitech).
	//		PARAMS: integer partId, char* brainName
	//		Returns: nothing

	long partId = ABLi_popInteger();
	char* brainName = ABLi_popCharPtr();
	if (!brainName)
		return;

	GameObjectPtr obj = (partId == -1) ? CurObject : getObject(partId);
	if (obj && obj->isMover()) {
		MoverPtr m = static_cast<MoverPtr>(obj);
		if (m->getPilot())
			m->getPilot()->setBrainName(brainName);
	}
}

void execDestroyMechBodyLocation (void) {

	//Destroy a specific mech body location (Omnitech). Safe stub.
	//		PARAMS: integer partId, integer locId
	//		Returns: nothing

	long partId = ABLi_popInteger();
	long locId  = ABLi_popInteger();
	(void)partId; (void)locId;
	omniTrace("destroymechbodylocation (stub)");
}

void execDamageMechArmor (void) {

	//Damage armor on a mech (Omnitech). Safe stub.
	//		PARAMS: integer partId
	//		Returns: nothing

	long partId = ABLi_popInteger();
	(void)partId;
	omniTrace("damagemecharmor (stub)");
}

void execSetMechGesture (void) {

	//Force a mech gesture (Omnitech).
	//		PARAMS: integer partId, integer gestureId
	//		Returns: nothing

	long partId = ABLi_popInteger();
	long gesture = ABLi_popInteger();

	GameObjectPtr obj = getObject(partId);
	if (obj && obj->getObjectClass() == BATTLEMECH)
		(static_cast<BattleMech*>(obj))->setGestureStateGoal(gesture);
}

void execSetUnitForceGroup (void) {

	//Reassign a unit to a force group (Omnitech). Safe stub -- no simple
	//engine hook and corrupting the commander roster is worse than no-op.
	//		PARAMS: integer partId, integer groupId
	//		Returns: nothing

	long partId = ABLi_popInteger();
	long groupId = ABLi_popInteger();
	(void)partId; (void)groupId;
	omniTrace("setunitforcegroup (stub)");
}

void execToggleAirStrike (void) {
	gAblAirStrikeEnabled = !gAblAirStrikeEnabled;
	omniTrace("toggleairstrike");
}

void execToggleSensorStrike (void) {
	gAblSensorStrikeEnabled = !gAblSensorStrikeEnabled;
	omniTrace("togglesensorstrike");
}

void execToggleArtilleryPiece (void) {
	gAblArtilleryPieceEnabled = !gAblArtilleryPieceEnabled;
	omniTrace("toggleartillerypiece");
}

void execToggleRepairTruck (void) {
	gAblRepairTruckEnabled = !gAblRepairTruckEnabled;
	omniTrace("togglerepairtruck");
}

void execToggleSalvageCraft (void) {
	gAblSalvageCraftEnabled = !gAblSalvageCraftEnabled;
	omniTrace("togglesalvagecraft");
}

void execSetRepairTruckEnabled (void) {
	bool on = ABLi_popBoolean();
	gAblRepairTruckEnabled = on;
	omniTrace("setrepairtruckenabled");
}

void execSetSalvageCraftEnabled (void) {
	bool on = ABLi_popBoolean();
	gAblSalvageCraftEnabled = on;
	omniTrace("setsalvagecraftenabled");
}

void execGetTargetRelativePosition (void) {

	//Compute range+angle from CurObject to target (Omnitech).
	//		PARAMS: integer targetId, real* rangeOut, real* angleOut
	//		Returns: nothing

	long targetId = ABLi_popInteger();
	float* rangeOut = ABLi_popRealPtr();
	float* angleOut = ABLi_popRealPtr();

	if (!rangeOut || !angleOut) {
		abl::reportBadArgument("gettargetrelativeposition", !rangeOut ? "rangeOut" : "angleOut");
		return;
	}
	rangeOut[0] = 0.0f;
	angleOut[0] = 0.0f;

	if (!CurObject)
		return;
	GameObjectPtr tgt = getObject(targetId);
	if (!tgt)
		return;

	glm::vec3 myPos = CurObject->getPosition();
	glm::vec3 tgtPos = tgt->getPosition();
	float dx = tgtPos.x - myPos.x;
	float dy = tgtPos.y - myPos.y;
	float range = sqrt(dx * dx + dy * dy);
	float angle = atan2(dy, dx);
	rangeOut[0] = range * metersPerWorldUnit;
	angleOut[0] = angle;
}

void execGetRelativePositionToTarget (void) {

	//Compute a point between CurObject and target at some distance (Omnitech).
	//		PARAMS: integer targetId, real distance, integer flag, real* outPoint
	//		Returns: nothing

	long targetId = ABLi_popInteger();
	float distance = ABLi_popIntegerReal();
	long flag = ABLi_popInteger();
	const auto output = ABLi_popRealBuf();
	float outPoint[3] = {};
	(void)flag;

	if (!CurObject)
		{ abl::copyRealArray(output, outPoint, "getrelativepositiontotarget_truncated"); return; }
	GameObjectPtr tgt = getObject(targetId);
	if (!tgt)
		{ abl::copyRealArray(output, outPoint, "getrelativepositiontotarget_truncated"); return; }

	glm::vec3 myPos = CurObject->getPosition();
	glm::vec3 tgtPos = tgt->getPosition();
	float dx = tgtPos.x - myPos.x;
	float dy = tgtPos.y - myPos.y;
	float len = sqrt(dx * dx + dy * dy);
	if (len > 0.0001f) {
		float nx = dx / len;
		float ny = dy / len;
		outPoint[0] = myPos.x + nx * distance;
		outPoint[1] = myPos.y + ny * distance;
		outPoint[2] = tgtPos.z;
	}
	else {
		outPoint[0] = myPos.x;
		outPoint[1] = myPos.y;
		outPoint[2] = myPos.z;
	}
	abl::copyRealArray(output, outPoint, "getrelativepositiontotarget_truncated");
}

void execNewDistanceToPosition (void) {

	//XY-only distance from a unit/object to a world position (Omnitech).
	//		PARAMS: integer partId, real[3] pos
	//		Returns: real distance (meters)

	long partId = ABLi_popInteger();
	float* coordList = ABLi_popRealPtr();

	ABLi_pushReal(-1.0f);

	if (!coordList)
		return;

	glm::vec3 position2;
	position2 = glm::vec3(0.0f);
	position2.x = coordList[0];
	position2.y = coordList[1];

	if (isUnitId(partId)) {
		long numObjects = getMovers(partId, moverList);
		float minDistance = 3.4E38f;
		for (long i = 0; i < numObjects; i++) {
			MoverPtr m = moverList[i];
			if (!m || !m->getExistsAndAwake())
				continue;
			glm::vec3 p1 = m->getPosition();
			float dx = position2.x - p1.x;
			float dy = position2.y - p1.y;
			float d = sqrt(dx * dx + dy * dy);
			if (d < minDistance)
				minDistance = d;
		}
		if (minDistance < 3.4E38f)
			ABLi_pokeReal(minDistance * metersPerWorldUnit);
	}
	else {
		GameObjectPtr obj = getObject(partId);
		if (obj) {
			glm::vec3 p1 = obj->getPosition();
			float dx = position2.x - p1.x;
			float dy = position2.y - p1.y;
			float d = sqrt(dx * dx + dy * dy);
			ABLi_pokeReal(d * metersPerWorldUnit);
		}
	}
}

static long countMoversWithinRadius (bool friendly, float radius) {

	if (!CurObject)
		return 0;

	TeamPtr selfTeam = CurObject->getTeam();
	glm::vec3 myPos = CurObject->getPosition();
	float r2 = radius * radius;
	long count = 0;

	for (long t = 0; t < Team::numTeams; t++) {
		TeamPtr team = Team::teams[t];
		if (!team)
			continue;
		long isFriendly = selfTeam ? (selfTeam->getRelation(team) == RELATION_FRIENDLY) : 0;
		if (friendly && !isFriendly)
			continue;
		if (!friendly && (isFriendly || team == selfTeam))
			continue;

		long rosterSize = team->getRosterSize();
		for (long i = 0; i < rosterSize; i++) {
			MoverPtr m = team->getMover(i);
			if (!m || !m->getExistsAndAwake())
				continue;
			glm::vec3 p = m->getPosition();
			float dx = p.x - myPos.x;
			float dy = p.y - myPos.y;
			if (dx * dx + dy * dy <= r2)
				count++;
		}
	}
	return count;
}

void execNumFriendsWithinRadius (void) {

	//Count friendly movers within radius of CurObject (Omnitech).
	//		PARAMS: integer partId (ignored, uses CurObject), real radius
	//		Returns: integer count

	long partId = ABLi_popInteger();
	float radius = ABLi_popIntegerReal();
	(void)partId;
	ABLi_pushInteger(static_cast<int>(countMoversWithinRadius(true, radius)));
}

void execNumEnemiesWithinRadius (void) {

	//Count non-friendly movers within radius of CurObject (Omnitech).
	//		PARAMS: integer partId (ignored), real radius
	//		Returns: integer count

	long partId = ABLi_popInteger();
	float radius = ABLi_popIntegerReal();
	(void)partId;
	ABLi_pushInteger(static_cast<int>(countMoversWithinRadius(false, radius)));
}

void execHasOrderFromPlayer (void) {

	//Stub -- no reliable hook for player-origin tac orders in this branch.
	//		PARAMS: none (scripts use it as a bare identifier)
	//		Returns: bool false

	ABLi_pushBoolean(false);
}

void execIsSelected (void) {

	//Stub -- CurObject selection state not exposed through this shim path.
	//		PARAMS: none
	//		Returns: bool false

	ABLi_pushBoolean(false);
}

void execIsRefit (void) {

	//Is CurObject currently refitting?
	//		PARAMS: none
	//		Returns: bool

	bool result = false;
	if (CurObject && CurObject->isMover()) {
		MoverPtr m = static_cast<MoverPtr>(CurObject);
		result = m->isRefit();
	}
	ABLi_pushBoolean(result);
}

void execNeedsRefit (void) {

	//Does CurObject need refit?
	//		PARAMS: none
	//		Returns: bool

	bool result = false;
	if (CurObject && CurObject->isMover()) {
		MoverPtr m = static_cast<MoverPtr>(CurObject);
		result = m->needsRefit();
	}
	ABLi_pushBoolean(result);
}

void execHirePilot (void) {

	//Add a pilot to the roster (Omnitech). Safe stub.
	//		PARAMS: char* pilotName
	//		Returns: nothing

	char* pilotName = ABLi_popCharPtr();
	(void)pilotName;
	omniTrace("hirepilot (stub)");
}

void execAddNewPilot (void) {

	//Install a new pilot+brain on an existing mover (Omnitech).
	//Pilot-file loading is nontrivial; wiring only the brain is the safe
	//cross-section that keeps AI responsive without corrupting pilot state.
	//		PARAMS: integer partId, char* pilotName, char* brainName
	//		Returns: nothing

	long partId = ABLi_popInteger();
	char* pilotName = ABLi_popCharPtr();
	char* brainName = ABLi_popCharPtr();
	(void)pilotName;

	GameObjectPtr obj = getObject(partId);
	if (obj && obj->isMover() && brainName) {
		MoverPtr m = static_cast<MoverPtr>(obj);
		if (m->getPilot())
			m->getPilot()->setBrainName(brainName);
	}
	omniTrace("addnewpilot");
}

void execAddNewPilotToPlayer (void) {

	//Alias of addnewpilot (Omnitech).
	//		PARAMS: integer partId, char* pilotName, char* brainName
	//		Returns: nothing

	long partId = ABLi_popInteger();
	char* pilotName = ABLi_popCharPtr();
	char* brainName = ABLi_popCharPtr();
	(void)pilotName;

	GameObjectPtr obj = getObject(partId);
	if (obj && obj->isMover() && brainName) {
		MoverPtr m = static_cast<MoverPtr>(obj);
		if (m->getPilot())
			m->getPilot()->setBrainName(brainName);
	}
	omniTrace("addnewpilottoplayer");
}

void execAutoRepairWithinRadius (void) {

	//Repair friendlies within radius (Omnitech). Safe stub.
	//		PARAMS: integer partId, real radius
	//		Returns: nothing

	long partId = ABLi_popInteger();
	float radius = ABLi_popIntegerReal();
	(void)partId; (void)radius;
	omniTrace("autorepairwithinradius (stub)");
}

void execAutoMedicalWithinRadius (void) {

	//Heal friendlies within radius (MC2X mc2xplay.abl). Safe stub, same contract as autorepairwithinradius.
	//		PARAMS: integer partId, real radius
	//		Returns: nothing

	long partId = ABLi_popInteger();
	float radius = ABLi_popIntegerReal();
	(void)partId; (void)radius;
	omniTrace("automedicalwithinradius (stub)");
}

void execAutoScan (void) {

	//MC2X "autoscan" player-brain option (mc2xplay.abl: gates the wolfman auto-repair/capture scan loop).
	//		PARAMS: none
	//		Returns: bool. false = loop off, so the brain just FireAtWill's, which is the stock behaviour.

	omniTrace("autoscan (stub)");
	ABLi_pushBoolean(false);
}

void execScanAreaCapture (void) {

	//Area-scan for capturable objects (Omnitech). Safe stub.
	//		PARAMS: integer-or-real radius
	//		Returns: integer, 0 = scan finished (scripts clear their "not yet scanned" flag on 0)

	float radius = ABLi_popIntegerReal();
	(void)radius;
	omniTrace("scanareacapture (stub)");
	ABLi_pushInteger(0);
}

void execScanAreaRepair (void) {

	//Area-scan for repair targets (Omnitech). Safe stub.
	//		PARAMS: real radius
	//		Returns: nothing

	float radius = ABLi_popIntegerReal();
	(void)radius;
	omniTrace("scanarearepair (stub)");
}

void execSetCampaignGlobalVar (void) {

	//Persist a campaign-scoped integer slot (Omnitech).
	//		PARAMS: integer index, integer value
	//		Returns: nothing

	long idx = ABLi_popInteger();
	long val = ABLi_popInteger();
	if (idx >= 0 && idx < 16)
		gCampaignGlobalVars[idx] = static_cast<int>(val);
}

void execGetCampaignGlobalVar (void) {

	//Read a campaign-scoped integer slot (Omnitech).
	//		PARAMS: integer index
	//		Returns: integer value

	long idx = ABLi_popInteger();
	int val = 0;
	if (idx >= 0 && idx < 16)
		val = gCampaignGlobalVars[idx];
	ABLi_pushInteger(val);
}

void execSetFixedBuildingRp (void) {

	//Assign resource-point reward to a fixed building (Omnitech).
	//RP field is not cleanly exposed in this branch; flag the building
	//capturable as a functional best-effort and log the rp value.
	//		PARAMS: integer partId, integer rp
	//		Returns: nothing

	long partId = ABLi_popInteger();
	long rp = ABLi_popInteger();
	(void)rp;

	GameObjectPtr obj = getObject(partId);
	if (obj && obj->isBuilding())
		obj->setFlag(OBJECT_FLAG_CAPTURABLE, true);
	omniTrace("setfixedbuildingrp");
}

void execGetTimeOfLastStep (void) {

	//Time (scenario seconds) that CurWarrior's last move-order step landed.
	//		PARAMS: none
	//		Returns: real

	float t = 0.0f;
	if (CurWarrior) {
		MoveOrders* mo = CurWarrior->getMoveOrders();
		if (mo)
			t = mo->timeOfLastStep;
	}
	ABLi_pushReal(t);
}

void execIsCurTacOrderMoveOrder (void) {

	//Stub -- tac-order type not plumbed through this shim.
	//		PARAMS: none
	//		Returns: bool false

	ABLi_pushBoolean(false);
}

void execGetCurTacOrderTime (void) {

	//Stub -- tac-order time not plumbed through this shim.
	//		PARAMS: none
	//		Returns: real 0.0

	ABLi_pushReal(0.0f);
}

void execClearMoveOrdersOmni (void) {

	//Clear CurWarrior's move-order queue (Omnitech).
	//		PARAMS: none
	//		Returns: nothing

	if (CurWarrior)
		CurWarrior->clearMoveOrders();
}

// --- Omnitech ABL extensions, tier-2 ---
// Safe stubs for names the Carver5O / Omnitech warriorbrain FSMs call: pop all args and push a
// default on non-void returns, so the ABL parser and stack stay balanced.
//
// magicAttack/Patrol/Guard/Escort and coreWait/Guard/Patrol are deliberately NOT defined natively:
// the stock and MCO brain libraries (corebrain.abx, mco-compat) define them, and a native
// ABLi_addFunction registration would shadow those routines in the global symtable and break the AI
// (docs/observations/2026-04-25-abl-library-shadow-rule.md). Re-enable only for content packs whose
// deployed corebrain.abx lacks these symbols.

void execSetWillRequestHelp (void) {
	// args: b. No return. Pilot-level help-request flag; stubbed.
	ABLi_popBoolean();
}

void execTDebugString (void) {
	// args: C (string). No return. Debug print; route to debug window.
	char* s = ABLi_popCharPtr();
	if (s) DEBUGWINS_print(s, 0);
}


void execIsDeadOrFled (void) {
	// args: i (partID). Returns: b. Conservative: return false (still in play)
	// unless we can clearly see the object is gone/destroyed.
	long partId = ABLi_popInteger();
	bool deadOrFled = false;
	GameObjectPtr obj = getObject(partId);
	if (!obj) {
		deadOrFled = true;
	} else if (obj->isDestroyed() || obj->isDisabled()) {
		deadOrFled = true;
	}
	ABLi_pushBoolean(deadOrFled);
}

void execPrintTeamStatus (void) {
	// args: i (teamID). No return. Diagnostic; stub.
	ABLi_popInteger();
}

void execMCPrint (void) {

	ABLStackItem value;
	ABLi_popAnything(&value);
	char s[256];
	switch (static_cast<ABLStackItemType>(value.type)) {
		case ABLStackItemType::ABL_STACKITEM_CHAR:
			mc2::fmt::to_buf(s, "char={:c}", mc2::fmt::as_char(value.data.character));
			DEBUGWINS_print(s, 0);
			break;
		case ABLStackItemType::ABL_STACKITEM_INTEGER:
			mc2::fmt::to_buf(s, "int={:d}", mc2::fmt::as_i32(value.data.integer));
			DEBUGWINS_print(s, 0);
			break;
		case ABLStackItemType::ABL_STACKITEM_REAL:
			mc2::fmt::to_buf(s, "real={:.2f}", value.data.real);
			DEBUGWINS_print(s, 0);
			break;
		case ABLStackItemType::ABL_STACKITEM_BOOLEAN:
			mc2::fmt::to_buf(s, "bool={}", mc2::fmt::safe(value.data.boolean ? "true" : "false"));
			DEBUGWINS_print(s, 0);
			break;
		case ABLStackItemType::ABL_STACKITEM_CHAR_PTR:
			mc2::fmt::to_buf(s, "char*={}", mc2::fmt::safe(value.data.characterPtr));
			DEBUGWINS_print(s, 0);
			break;
		case ABLStackItemType::ABL_STACKITEM_INTEGER_PTR:
			mc2::fmt::to_buf(s, "int*={:d},{:d},{:d}", mc2::fmt::as_i32(value.data.integerPtr[0]), mc2::fmt::as_i32(value.data.integerPtr[1]), mc2::fmt::as_i32(value.data.integerPtr[2]));
			DEBUGWINS_print(s, 0);
			break;
		case ABLStackItemType::ABL_STACKITEM_REAL_PTR:
			mc2::fmt::to_buf(s, "real*={:.2f},{:.2f},{:.2f}", value.data.realPtr[0], value.data.realPtr[1], value.data.realPtr[2]);
			DEBUGWINS_print(s, 0);
			break;
		case ABLStackItemType::ABL_STACKITEM_BOOLEAN_PTR:
			mc2::fmt::to_buf(s, "bool*={:d},{:d},{:d}", mc2::fmt::as_i32(value.data.booleanPtr[0]), mc2::fmt::as_i32(value.data.booleanPtr[1]), mc2::fmt::as_i32(value.data.booleanPtr[2]));
			DEBUGWINS_print(s, 0);
			break;	
	}
}

void execGetMissionStatus (void) {

	ABLi_pushInteger(mission->getStatus());
}

void execAddTriggerArea (void) {

	long ULrow = ABLi_popInteger();
	long ULcol = ABLi_popInteger();
	long LRrow = ABLi_popInteger();
	long LRcol = ABLi_popInteger();
	long type = ABLi_popInteger();
	long param = ABLi_popInteger();

	long handle = Mover::triggerAreaMgr->add(ULrow, ULcol, LRrow, LRcol, type, param);
	if (handle == 0)
		Fatal(0, " Too many Trigger Areas ");
	ABLi_pushInteger(handle);
}

void execIsTriggerAreaHit (void) {

	long handle = ABLi_popInteger();
	ABLi_pushBoolean(Mover::triggerAreaMgr->isHit(handle));
}

void execResetTriggerArea (void) {

	long handle = ABLi_popInteger();
	Mover::triggerAreaMgr->reset(handle);
}

void execRemoveTriggerArea (void) {

	long handle = ABLi_popInteger();
	Mover::triggerAreaMgr->remove(handle);
}

void execGetWeapons (void) {

	const auto output = ABLi_popIntBuf();
	int weaponList[MAX_WEAPONS_PER_MOVER] = {};
	long infoType = ABLi_popInteger();

	long numWpns = 0;
	if (CurObject && CurObject->isMover()) {
		MoverPtr mover = static_cast<MoverPtr>(CurObject);
		switch (infoType) {
			case 0: {
				for (long curWeapon = mover->numOther; curWeapon < (mover->numOther + mover->numWeapons); curWeapon++)
					weaponList[numWpns++] = mover->inventory[curWeapon].masterID;
				}
				break;
			case 1: {
				for (long curWeapon = mover->numOther; curWeapon < (mover->numOther + mover->numWeapons); curWeapon++) {
					if (!mover->inventory[curWeapon].disabled && (mover->getWeaponShots(curWeapon) > 0))
						weaponList[numWpns++] = mover->inventory[curWeapon].masterID;
				}
				#ifdef BUGLOG
				if ((numWpns != mover->numFunctionalWeapons) && BugLog) {
					char s[50];
					mc2::fmt::to_buf(s, "[{:.2f}] ablmc2.execGetWeapons: numWpns != numFunctionalWeapons ({:05d}){}", scenarioTime, mc2::fmt::as_i32(mover->getPartId()), mc2::fmt::safe(mover->getName()));
					BugLog->write(s);
					BugLog->write(" ");
				}
				#endif
				}
				break;
		}
	}
	ABLi_pushInteger(abl::copyIntArray(output, {weaponList, static_cast<size_t>(numWpns)}, "getweapons_truncated"));
}

void execGetWeaponsStatus (void) {

	const auto output = ABLi_popIntBuf();
	int weaponList[MAX_WEAPONS_PER_MOVER] = {};

	long status = 0;
	if (CurObject && CurObject->isMover()) {
		MoverPtr mover = static_cast<MoverPtr>(CurObject);
		if (mover->getPilot()) {
			status = mover->getPilot()->getWeaponsStatus(weaponList);
			abl::copyIntArray(output, weaponList, "getweaponsstatus_truncated");
		}
	}
	ABLi_pushInteger(status);
}

void execSetMoveArea (void) {

	float* center = ABLi_popRealPtr();
	float radius = ABLi_popReal();

	if (s_ablArgGuard && !center) {
		abl_arg_guard_log("execSetMoveArea", "center");
		return;
	}

	if (CurObject->isMover()) {
		glm::vec3 loc;
		loc.x = center[0];
		loc.y = center[1];
		loc.z =  land->getTerrainElevation(loc);
		(static_cast<MoverPtr>(CurObject))->moveCenter = loc;
		(static_cast<MoverPtr>(CurObject))->moveRadius = radius;
	}
}

void execClearTacOrder (void) {

	if (CurObject)
		CurObject->getPilot()->clearCurTacOrder();
}

void execPlayWave (void) {

	char* fileName = ABLi_popCharPtr();
	long type = ABLi_popInteger();

	soundSystem->playSupportSample(-1, fileName);

	ABLi_pushInteger(0);
}

void execSetWillHelp (void) {

	bool willHelp = ABLi_popBoolean();

	bool wasHelping = CurWarrior->getWillHelp();
	CurWarrior->setWillHelp(willHelp);

	ABLi_pushInteger(wasHelping);
}

void execGetLastScan (void) {

	GameObjectPtr lastScanTarget = ObjectManager->getByWatchID(CurWarrior->getCoreScanTargetWID());
	long lastScanTargetID = 0;
	if (lastScanTarget)
		lastScanTargetID = lastScanTarget->getPartId();

	ABLi_pushInteger(lastScanTargetID);
}

void execGetMapInfo (void) {

	const auto output = ABLi_popIntBuf();
	int mapInfo[2] = {};

	mapInfo[0] = GameMap->getHeight();
	mapInfo[1] = GameMap->getWidth();
	abl::copyIntArray(output, mapInfo, "getmapinfo_truncated");
}

void execIsOffMap (void) {

	float* worldPos = ABLi_popRealPtr();

	if (s_ablArgGuard && !worldPos) {
		abl_arg_guard_log("execIsOffMap", "worldPos");
		ABLi_pushBoolean(false);
		return;
	}

	glm::vec3 pos;
	pos.x = worldPos[0];
	pos.y = worldPos[1];
	if (pos.x > 999000) {
		if (CurObject)
			pos = CurObject->getPosition();
		else {
			ABLi_pushBoolean(false);
			return;
		}
	}
	int row, col;
	land->worldToCell(pos, row, col);
	ABLi_pushBoolean(GameMap->getOffMap(row, col) ? true : false);
}

void execSetGoalPlanning (void) {

	bool setting = ABLi_popBoolean();

	bool oldSetting = false;
	if (CurObject) {
		oldSetting = (static_cast<MoverPtr>(CurObject))->getPilot()->getUseGoalPlan();
		(static_cast<MoverPtr>(CurObject))->getPilot()->setUseGoalPlan(setting);
		if (oldSetting != setting)
			(static_cast<MoverPtr>(CurObject))->getPilot()->setMainGoal(GOAL_ACTION_NONE, nullptr, nullptr, -1.0);
	}
	ABLi_pushBoolean(oldSetting);
}

void execSetEject (void) {

	bool setting = ABLi_popBoolean();

	bool oldSetting = false;
	if (CurObject) {
		oldSetting = (static_cast<MoverPtr>(CurObject))->getPilot()->getEscapesThruEjection();
		(static_cast<MoverPtr>(CurObject))->getPilot()->setEscapesThruEjection(setting);
	}
	ABLi_pushBoolean(oldSetting);
}

void execSetKeepMoving (void) {

	bool setting = ABLi_popBoolean();

	bool oldSetting = false;
	if (CurObject) {
		oldSetting = (static_cast<MoverPtr>(CurObject))->getPilot()->getKeepMoving();
		(static_cast<MoverPtr>(CurObject))->getPilot()->setKeepMoving(setting);
	}
	ABLi_pushBoolean(oldSetting);
}
