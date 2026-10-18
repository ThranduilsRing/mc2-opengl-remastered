// ABL native bindings for MechCommander 2: globals, initABL and closeABL.
//
// initABL holds the single ordered ABLi_addFunction list. Registration order is state (each routine's key is
// its position in a fixed table), so the list stays in this one file; the exec* bodies live in ablmc2_<domain>.cpp.
#include "ablmc2_internal.h"
#include "mc2fmt.h"
#include "abl_script_events.h"
#include "../GameOS/gameos/utils/mc2log.h"
#include <climits>


MoverGroupPtr			CurGroup = nullptr;
GameObjectPtr			CurObject = nullptr;
long					CurObjectClass = 0;
MechWarriorPtr			CurWarrior = nullptr;
GameObjectPtr			CurContact = nullptr;
long					CurAlarm;
MoverPtr				moverList[256];
bool					TacOrderOrigin = ORDER_ORIGIN_COMMANDER;
long					CurMultiplayCode = 0;
long					CurMultiplayParam = 0;

// The ABL environment holds a script file as an opaque handle (void*): ablFileCreateCB / ablFileOpenCB make it, ablFileCloseCB is the only place
// that frees it. These three are the single owner pair; every other callback only borrows the handle for the call.
long ablFileCreateCB (void** file, const char* fName) {

	auto created = std::make_unique<File>();
	*file = nullptr;
	long result = created->create(fName);
	if (result != NO_ERR) {
		return result;
	}
	*file = created.release();
	return(NO_ERR);
}

long ablFileOpenCB (void** file, const char* fName) {

	auto opened = std::make_unique<File>();

	*file = nullptr;
	// Archive lookup requires lowercase names.
	std::string name(fName);
	CharLower(name.data());
	long result = opened->open(name.c_str());
	if (result != NO_ERR) {
		return result;
	}
	*file = opened.release();
	return(NO_ERR);
}

long ablFileCloseCB (void** file) {

	std::unique_ptr<File> owned(static_cast<File*>(*file));
	if (owned)
		owned->close();
	*file = nullptr;

	return(0);
}

bool ablFileEofCB (void* file) {

	return(((FilePtr)file)->eof());

}

long ablFileRemainingCB (void* file) {
	auto* borrowed = static_cast<File*>(file);
	const auto size = borrowed->getLength();
	const auto position = borrowed->getLogicalPosition();
	return position <= size && size - position <= LONG_MAX ? static_cast<long>(size - position) : -1;
}

long ablFileReadCB (void* file, unsigned char* buffer, long length) {

	return(((FilePtr)file)->read(buffer, length));
}

int32_t ablFileReadIntCB (void* file) {

	return(((FilePtr)file)->readInt());
}

long ablFileReadLongCB (void* file) {

	return(((FilePtr)file)->readLong());
}

long ablFileReadStringCB (void* file, unsigned char* buffer) {

	return(((FilePtr)file)->readString(buffer));
}

long ablFileReadLineExCB (void* file, unsigned char* buffer, long maxLength) {

	return(((FilePtr)file)->readLineEx(buffer, maxLength));
}

long ablFileWriteCB (void* file, unsigned char* buffer, long length) {

	return(((FilePtr)file)->write(buffer, length));
}

long ablFileWriteByteCB (void* file, unsigned char byte) {

	return(((FilePtr)file)->writeByte(byte));
}

long ablFileWriteIntCB (void* file, int32_t value) {

	return(((FilePtr)file)->writeInt(value));
}

long ablFileWriteLongCB (void* file, long value) {

	return(((FilePtr)file)->writeLong(value));
}

long ablFileWriteStringCB (void* file, const char* buffer) {

	return(((FilePtr)file)->writeString(buffer));
}

void ablDebuggerPrintCallback (const char* s) {

	//ABLDebuggerOut->print(s);
	MC2_LOG(Core, Debug, "{}", mc2::fmt::safe(s));
}

void ablDebugPrintCallback (const char* s) {

	DEBUGWINS_print(s, 0);
}

void ablSeedRandom (unsigned long seed) {

	gos_srand(seed);
}

void ablFatalCallback (long code, const char* s) {

	// Rate-limit per-message spam (e.g. unimplemented ABL extensions firing
	// on every script tick). Fixed C array -- no heap, no STL.
	static const int kSlots    = 16;
	static const int kMaxPrints = 5;
	struct AblMsgSlot { unsigned int hash; int count; };
	static AblMsgSlot slots[kSlots];
	static int  slotCount = 0;
	static bool tableFull = false;

	// djb2-lite over first 64 chars.
	unsigned int h = 5381u;
	const char* p = s ? s : "";
	for (int i = 0; *p && i < 64; ++p, ++i)
		h = h * 33u ^ (unsigned char)*p;

	for (int i = 0; i < slotCount; ++i) {
		if (slots[i].hash == h) {
			if (++slots[i].count <= kMaxPrints)
				STOP((s));
			else if (slots[i].count == kMaxPrints + 1)
				STOP(("[ABL] suppressing further identical errors (hash %08x): %s", h, s));
			return;
		}
	}

	if (slotCount < kSlots) {
		slots[slotCount++] = { h, 1 };
		STOP((s));
	} else if (!tableFull) {
		tableFull = true;
		STOP(("[ABL] fatal callback: %d unique errors tracked; further distinct messages suppressed", kSlots));
	}
}

extern GameObjectPtr LastCoreAttackTarget;

void ablEndlessStateCallback (UserFile* log) {

	char s[256];
	mc2::fmt::to_buf(s, "Mover = {} ({:d})", mc2::fmt::safe(CurWarrior->getVehicle()->getName()), mc2::fmt::as_i32(CurWarrior->getVehicle()->getPartId()));
	log->write(s);
	mc2::fmt::to_buf(s, "     Status = {:d}", mc2::fmt::as_i32(CurWarrior->getVehicle()->getStatus()));
	log->write(s);
	mc2::fmt::to_buf(s, "     NumWeapons = {:d}", mc2::fmt::as_i32(CurWarrior->getVehicle()->numWeapons));
	log->write(s);
	mc2::fmt::to_buf(s, "     NumFunctionalWeapons = {:d}", mc2::fmt::as_i32(CurWarrior->getVehicle()->numFunctionalWeapons));
	log->write(s);
	mc2::fmt::to_buf(s, "     TacOrder = ");
	CurWarrior->getCurTacOrder()->debugString(CurWarrior, &s[16]);
	log->write(s);
	log->write(" ");
	if (LastCoreAttackTarget) {
		mc2::fmt::to_buf(s, "Target = {} ({:d})", mc2::fmt::safe(LastCoreAttackTarget->getName()), mc2::fmt::as_i32(LastCoreAttackTarget->getPartId()));
		log->write(s);
		Stuff::Vector3D pos;
		pos = LastCoreAttackTarget->getPosition();
		mc2::fmt::to_buf(s, "Target Position = ({:.3f}, {:.3f}, {:.3f}) [{:d}, {:d}]", pos.x, pos.y, pos.z, mc2::fmt::as_i32(LastCoreAttackTarget->cellPositionRow), mc2::fmt::as_i32(LastCoreAttackTarget->cellPositionCol));
		log->write(s);
		mc2::fmt::to_buf(s, "     Status = {:d}", mc2::fmt::as_i32(LastCoreAttackTarget->getStatus()));
		log->write(s);
		if (LastCoreAttackTarget->isMover()) {
			MoverPtr mover = (MoverPtr)LastCoreAttackTarget;
			mc2::fmt::to_buf(s, "     NumWeapons = {:d}", mc2::fmt::as_i32(mover->numWeapons));
			log->write(s);
			mc2::fmt::to_buf(s, "     NumFunctionalWeapons = {:d}", mc2::fmt::as_i32(mover->numFunctionalWeapons));
			log->write(s);
			mc2::fmt::to_buf(s, "     TacOrder = ");
			mover->getPilot()->getCurTacOrder()->debugString(CurWarrior, &s[16]);
			log->write(s);
		}
		}
	else {
		mc2::fmt::to_buf(s, "Target = NULL");
		log->write(s);
	}
}

static void logAblFunctionTableCapacity (void)
{
	char _cbbuf[128];
	mc2::fmt::to_buf(_cbbuf, "[ABL v1] event=functions_registered count={:d}/{:d}", mc2::fmt::as_i32(NumStandardFunctions), mc2::fmt::as_i32(MAX_STANDARD_FUNCTIONS));
	MC2_DIAG_PUTS(_cbbuf);
	crashbundle_append(_cbbuf);
	fflush(stdout);
}

void initABL (void) {

	abl::resetEventBudgets();

	ABLFile::remainingCB = ablFileRemainingCB;
	ABLi_init(65535, // Runtime stack slots
			  1048576, // Code-buffer bytes; mod scripts exceed the stock limit
			  1024, // Registered modules
			  512, //AblMaxStaticVariables, was 100
			  ablFileCreateCB,
			  ablFileOpenCB,
			  ablFileCloseCB,
			  ablFileEofCB,
			  ablFileReadCB,
			  ablFileReadIntCB,
			  ablFileReadLongCB,
			  ablFileReadStringCB,
			  ablFileReadLineExCB,
			  ablFileWriteCB,
			  ablFileWriteByteCB,
			  ablFileWriteIntCB,
			  ablFileWriteLongCB,
			  ablFileWriteStringCB,
			  ablDebuggerPrintCallback,
			  ablFatalCallback,
			  true,
			  false);

	ABLi_setDebugPrintCallback(ablDebugPrintCallback);
	ABLi_setRandomCallbacks(ablSeedRandom, RandomNumber);
	ABLi_setEndlessStateCallback(ablEndlessStateCallback);

	runAblArgGuardRepro();

	ABLi_addFunction("getid", false, nullptr, "i", execGetId);
	ABLi_addFunction("gettime", false, nullptr, "r", execGetTime);
	ABLi_addFunction("gettimeleft", false, nullptr, "r", execGetTimeLeft);
	ABLi_addFunction("selectobject", false, "i", "i", execSelectObject);
	ABLi_addFunction("selectwarrior", false, "i", "i", execSelectWarrior);
	ABLi_addFunction("getwarriorstatus", false, "i", "i", execGetWarriorStatus);
	ABLi_addFunction("getcontacts", false, "Iii", "i", execGetContacts);
	ABLi_addFunction("getenemycount", false, "i", "i", execGetEnemyCount);
	ABLi_addFunction("selectcontact", false, "ii", "i", execSelectContact);
	ABLi_addFunction("getcontactid", false, nullptr, "i", execGetContactId);
	ABLi_addFunction("iscontact", false, "iii", "i", execIsContact);
	ABLi_addFunction("getcontactstatus", false, "I", "i", execGetContactStatus);
	ABLi_addFunction("getcontactrelativeposition", false, "rr", "i", execGetContactRelativePosition);
	ABLi_addFunction("settarget", false, "ii", nullptr, execSetTarget);
	ABLi_addFunction("gettarget", false, "i", "i", execGetTarget);
	ABLi_addFunction("getweaponsready", false, "Ii", "i", execGetWeaponsReady);
	ABLi_addFunction("getweaponslocked", false, "Ii", "i", execGetWeaponsLocked);
	ABLi_addFunction("getweaponsinrange", false, "Ii", "i", execGetWeaponsInRange);
	ABLi_addFunction("getweaponshots", false, "i", "i", execGetWeaponShots);
	ABLi_addFunction("getweaponranges", false, "iR", nullptr, execGetWeaponRanges);
	ABLi_addFunction("getobjectposition", false, "iR", nullptr, execGetObjectPosition);
	ABLi_addFunction("getintegermemory", false, "i", "i", execGetIntegerMemory);
	ABLi_addFunction("getrealmemory", false, "i", "r", execGetRealMemory);
	ABLi_addFunction("getalarmtriggers", false, "I", "i", execGetAlarmTriggers);
	ABLi_addFunction("getchallenger", false, "i", "i", execGetChallenger);
	ABLi_addFunction("gettimewithoutorders", false, nullptr, "r", execGetTimeWithoutOrders);
	ABLi_addFunction("getfireranges", false, "R", nullptr, execGetFireRanges);
	ABLi_addFunction("getattackers", false, "Ir", "i", execGetAttackers);
	ABLi_addFunction("getattackerinfo", false, "i", "r", execGetAttackerInfo);
	ABLi_addFunction("setchallenger", false, "ii", "i", execSetChallenger);
	ABLi_addFunction("setintegermemory", false, "ii", nullptr, execSetIntegerMemory);
	ABLi_addFunction("setrealmemory", false, "ir", nullptr, execSetRealMemory);
	ABLi_addFunction("hasmovegoal", false, nullptr, "b", execHasMoveGoal);
	ABLi_addFunction("hasmovepath", false, nullptr, "b", execHasMovePath);
	ABLi_addFunction("sortweapons", false, "Ii", "i", execSortWeapons);
	ABLi_addFunction("getvisualrange", false, "i", "r", execGetVisualRange);
	ABLi_addFunction("getunitmates", false, "iI", "i", execGetUnitMates);
	ABLi_addFunction("gettacorder", false, "irI", "i", execGetTacOrder);
	ABLi_addFunction("getlasttacorder", false, "irI", "i", execGetLastTacOrder);
	ABLi_addFunction("getobjects", false, "iI", "i", execGetObjects);
	ABLi_addFunction("orderwait", false, "rb", "i", execOrderWait);
	ABLi_addFunction("ordermoveto", false, "Rb", "i", execOrderMoveTo);
	ABLi_addFunction("ordermovetoobject", false, "ib", "i", execOrderMoveToObject);
	ABLi_addFunction("ordermovetocontact", false, "b", "i", execOrderMoveToContact);
	ABLi_addFunction("orderpowerdown", false, nullptr, "i", execOrderPowerDown);
	ABLi_addFunction("orderpowerup", false, nullptr, "i", execOrderPowerUp);
	ABLi_addFunction("orderattackobject", false, "iiiib", "i", execOrderAttackObject);
	ABLi_addFunction("orderattackcontact", false, "iiib", "i", execOrderAttackContact);
	ABLi_addFunction("orderwithdraw", false, nullptr, "i", execOrderWithdraw);
	ABLi_addFunction("objectinwithdrawal", false, "i", "i", execObjectInWithdrawal);
	ABLi_addFunction("damageobject", false, "iiirirr", "i", execDamageObject);
	ABLi_addFunction("setattackradius", false, "r", "r", execSetAttackRadius);
	ABLi_addFunction("objectchangesides", false, "ii", nullptr, execObjectChangeSides);
	ABLi_addFunction("distancetoobject", false, "ii", "r", execDistanceToObject);
	ABLi_addFunction("distancetoposition", false, "iR", "r", execDistanceToPosition);
	ABLi_addFunction("objectsuicide", false, "i", nullptr, execObjectSuicide);
	ABLi_addFunction("objectcreate", false, "i", "i", execObjectCreate);
	ABLi_addFunction("objectexists", false, "i", "i", execObjectExists);
	ABLi_addFunction("objectstatus", false, "i", "i", execObjectStatus);
	ABLi_addFunction("objectstatuscount", false, "iI", nullptr, execObjectStatusCount);
	ABLi_addFunction("objectvisible", false, "ii", "i", execObjectVisible);
	ABLi_addFunction("objectside", false, "i", "i", execObjectTeam);
	ABLi_addFunction("objectcommander", false, "i", "i", execObjectCommander);
	ABLi_addFunction("objectclass", false, "i", "i", execObjectClass);
	ABLi_addFunction("settimer", false, "i*", "i", execSetTimer);
	ABLi_addFunction("checktimer", false, "i", "r", execCheckTimer);
	ABLi_addFunction("endtimer", false, "i", nullptr, execEndTimer);
	ABLi_addFunction("setobjectivestatus", false, "ii", "i", execSetObjectiveStatus);
	ABLi_addFunction("checkobjectivestatus", false, "i", "i", execCheckObjectiveStatus);
	ABLi_addFunction("setobjectivetype", false, "ii", "i", execSetObjectiveType);
	ABLi_addFunction("checkobjectivetype", false, "i", "i", execCheckObjectiveType);
	ABLi_addFunction("playdigitalmusic", false, "i", "i", execPlayDigitalMusic);
	ABLi_addFunction("stopmusic", false, nullptr, "i", execStopMusic);
	ABLi_addFunction("playsoundeffect", false, "i", "i", execPlaySoundEffect);
	ABLi_addFunction("playvideo", false, "C", "i", execPlayVideo);
	ABLi_addFunction("setradio", false, "ib", "i", execSetRadio);
	ABLi_addFunction("playspeech", false, "ii", "i", execPlaySpeech);
	ABLi_addFunction("playbetty", false, "i", "i", execPlayBetty);
	ABLi_addFunction("setobjectactive", false, "ib", "i", execSetObjectActive);
	ABLi_addFunction("objecttypeid", false, "i", "i", execObjectTypeID);
	ABLi_addFunction("getterrainobjectpartid", false, "ii", "i", execGetTerrainObjectPartID);
	ABLi_addFunction("objectremove", false, "i", "i", execObjectRemove);
	ABLi_addFunction("inarea", false, "iRri", "b", execInArea);
	ABLi_addFunction("createinfantry", false, "Ri", "i", execCreateInfantry);
	ABLi_addFunction("getsensorsworking", false, "i", "i", execGetSensorsWorking);
	ABLi_addFunction("getcurrentbrvalue", false, "i", "i", execGetCurrentBRValue);
	ABLi_addFunction("setcurrentbrvalue", false, "ii", nullptr, execSetCurrentBRValue);
	ABLi_addFunction("getarmorpts", false, "i", "i", execGetArmorPts);
	ABLi_addFunction("getmaxarmor", false, "i", "i", execGetMaxArmor);
	ABLi_addFunction("getpilotid", false, "i", "i", execGetPilotID);
	ABLi_addFunction("getpilotwounds", false, "i", "r", execGetPilotWounds);
	ABLi_addFunction("setpilotwounds", false, "ii", nullptr, execSetPilotWounds);
	ABLi_addFunction("getobjectactive", false, "i", "i", execGetObjectActive);
	ABLi_addFunction("getobjectdamage", false, "i", "i", execGetObjectDamage);
	ABLi_addFunction("getobjectdmgpts", false, "i", "i", execGetObjectDmgPts);
	ABLi_addFunction("getobjectmaxdmg", false, "i", "i", execGetObjectMaxDmg);
	ABLi_addFunction("setobjectdamage", false, "ii", "i", execSetObjectDamage);
	ABLi_addFunction("getglobalvalue", false, "i", "r", execGetGlobalValue);
	ABLi_addFunction("setglobalvalue", false, "i*", nullptr, execSetGlobalValue);
	ABLi_addFunction("setobjectivepos", false, "i***", nullptr, execSetObjectivePos);
	ABLi_addFunction("setsensorrange", false, "ir", nullptr, execSetSensorRange);
	ABLi_addFunction("settonnage", false, "ir", nullptr, execSetTonnage);
	ABLi_addFunction("setexplosiondamage", false, "ir", nullptr, execSetExplosionDamage);
	ABLi_addFunction("setexplosionradius", false, "ir", nullptr, execSetExplosionRadius);
	ABLi_addFunction("setsalvage", false, "iii", "b", execSetSalvage);
	ABLi_addFunction("setsalvagestatus", false, "ib", "b", execSetSalvageStatus);
	ABLi_addFunction("setanimation", false, "iii", nullptr, execSetAnimation);
	ABLi_addFunction("setrevealed", false, "i*R", nullptr, execSetRevealed);
	ABLi_addFunction("getsalvage", false, "iiII", nullptr, execGetSalvage);
	ABLi_addFunction("orderrefit", false, "ii", nullptr, execOrderRefit);
	ABLi_addFunction("setcaptured", false, "i", nullptr, execSetCaptured);
	ABLi_addFunction("ordercapture", false, "ii", nullptr, execOrderCapture);
	ABLi_addFunction("setcapturable", false, "ib", nullptr, execSetCapturable);
	ABLi_addFunction("iscaptured", false, "i", "i", execIsCaptured);
	ABLi_addFunction("iscapturable", false, "ii", "b", execIsCapturable);
	ABLi_addFunction("wasevercapturable", false, "i", "b", execWasEverCapturable);
	ABLi_addFunction("setbuildingname", false, "ii", nullptr, execSetBuildingName);
	ABLi_addFunction("callstrike", false, "iirrrb", nullptr, execCallStrike);
	ABLi_addFunction("callstrikeex", false, "iirrrbr", nullptr, execCallStrikeEx);
	ABLi_addFunction("orderloadelementals", false, "i", nullptr, execOrderLoadElementals);
	ABLi_addFunction("orderdeployelementals", false, "i", nullptr, execOrderDeployElementals);
	ABLi_addFunction("addprisoner", false, "ii", "i", execAddPrisoner);
	ABLi_addFunction("lockgateopen", false, "i", nullptr, execLockGateOpen);
	ABLi_addFunction("lockgateclosed", false, "i", nullptr, execLockGateClosed);
	ABLi_addFunction("releasegatelock", false, "i", nullptr, execReleaseGateLock);
	ABLi_addFunction("isgateopen", false, "i", "b", execIsGateOpen);
	ABLi_addFunction("getrelativepositiontopoint", false, "RrriR", nullptr, execGetRelativePositionToPoint);
	ABLi_addFunction("getrelativepositiontoobject", false, "irriR", nullptr, execGetRelativePositionToObject);
	ABLi_addFunction("getunitstatus", false, "i", "r", execGetUnitStatus);
	ABLi_addFunction("repair", false, "ir", nullptr, execRepair);
	ABLi_addFunction("getfixed", false, "iii", "i", execGetFixed);
	ABLi_addFunction("getrepairstate", false, "i", "i", execGetRepairState);
	ABLi_addFunction("isteamtargeting", false, "iii", "b", execIsTeamTargeting);
	ABLi_addFunction("isteamcapturing", false, "iii", "b", execIsTeamCapturing);
	ABLi_addFunction("sendmessage", false, "ii", nullptr, execSendMessage);
	ABLi_addFunction("getmessage", false, "i", "i", execGetMessage);
	ABLi_addFunction("gethometeam", false, nullptr, "i", execGetHomeTeam);
	ABLi_addFunction("isserver", false, nullptr, "b", execIsServer);
	ABLi_addFunction("calcpartid", false, "iiii", "i", execCalcPartID);
	ABLi_addFunction("setdebugstring", false, "iiC", nullptr, execSetDebugString);
	ABLi_addFunction("break", false, nullptr, nullptr, execBreak);
	ABLi_addFunction("pathexists", false, "iiiii", "i", execPathExists);
	ABLi_addFunction("convertcoords", false, "iRI", "i", execConvertCoords);
	ABLi_addFunction("newmoveto", true, "Ri", "i", execCoreMoveTo);
	ABLi_addFunction("newmovetoobject", true, "ii", "i", execCoreMoveToObject);
	ABLi_addFunction("newpower", true, "b", "i", execCorePower);
	ABLi_addFunction("newattack", true, "ii", "i", execCoreAttack);
	ABLi_addFunction("newcapture", true, "ii", "i", execCoreCapture);
	ABLi_addFunction("newscan", true, "ii", "i", execCoreScan);
	ABLi_addFunction("newcontrol", true, "ii", "i", execCoreControl);
	ABLi_addFunction("coremoveto", true, "Ri", "i", execCoreMoveTo);
	ABLi_addFunction("coremovetoobject", true, "ii", "i", execCoreMoveToObject);
	ABLi_addFunction("corepower", true, "b", "i", execCorePower);
	ABLi_addFunction("coreattack", true, "ii", "i", execCoreAttack);
	ABLi_addFunction("corecapture", true, "ii", "i", execCoreCapture);
	ABLi_addFunction("corescan", true, "ii", "i", execCoreScan);
	ABLi_addFunction("corecontrol", true, "ii", "i", execCoreControl);
	ABLi_addFunction("coreeject", true, nullptr, "i", execCoreEject);
	ABLi_addFunction("setpilotstate", false, "i", "i", execSetPilotState);
	ABLi_addFunction("getpilotstate", false, nullptr, "i", execGetPilotState);
	ABLi_addFunction("getnextpilotevent", false, "I", "i", execGetNextPilotEvent);
	ABLi_addFunction("settargetpriority", false, "iiiii", "i", execSetTargetPriority);
	ABLi_addFunction("setdebugwindow", false, "ii", "i", execSetDebugWindow);
	
	ABLi_addFunction("setmoviemode", false, nullptr, nullptr, execSetMovieMode);
	ABLi_addFunction("endmoviemode", false, nullptr, nullptr, execEndMovieMode);
	ABLi_addFunction("fadetocolor", false, "ir", nullptr, execFadeToColor);
	ABLi_addFunction("forcemovieend", false, nullptr, "i", execForceMovieEnd);
	
	ABLi_addFunction("getcameraposition", false, "R", nullptr, execGetCameraPosition);
	ABLi_addFunction("setcameraposition", false, "R", nullptr, execSetCameraPosition);
	ABLi_addFunction("setcameragoalposition", false, "Rr", nullptr, execSetCameraGoalPosition);
	ABLi_addFunction("getcameragoalposition", false, "R", nullptr, execGetCameraGoalPosition);
	ABLi_addFunction("getcamerarotation", false, "R", nullptr, execGetCameraRotation);
	ABLi_addFunction("setcamerarotation", false, "R", nullptr, execSetCameraRotation);
	ABLi_addFunction("setcameragoalrotation", false, "Rr", nullptr, execSetCameraGoalRotation);
	ABLi_addFunction("getcameragoalrotation", false, "R", nullptr, execGetCameraGoalRotation);
	ABLi_addFunction("getcamerazoom", false, nullptr, "r", execGetCameraZoom);
	ABLi_addFunction("setcamerazoom", false, "r", nullptr, execSetCameraZoom);
	ABLi_addFunction("getcameragoalzoom", false, nullptr, "r", execGetCameraGoalZoom);
	ABLi_addFunction("setcameragoalzoom", false, "rr", nullptr, execSetCameraGoalZoom);
	ABLi_addFunction("setcameravelocity", false, "R", nullptr, execSetCameraVelocity);
	ABLi_addFunction("getcameravelocity", false, "R", nullptr, execGetCameraVelocity);
	ABLi_addFunction("setcameragoalvelocity", false, "Rr", nullptr, execSetCameraGoalVelocity);
	ABLi_addFunction("getcameragoalvelocity", false, "R", nullptr, execGetCameraGoalVelocity);
	ABLi_addFunction("setcameralookobject", false, "i", nullptr, execSetCameraLookObject);
	ABLi_addFunction("getcameralookobject", false, nullptr, "i", execGetCameraLookObject);
	ABLi_addFunction("getcameraframelength", false, nullptr, "r", execGetCameraFrameLength);
	
	ABLi_addFunction("getmissionwon", false, nullptr, "b", execGetMissionWon);
	ABLi_addFunction("getmissionlost", false, nullptr, "b", execGetMissionLost);
	ABLi_addFunction("getobjectivesuccess", false, nullptr, "b", execGetObjectiveSuccess);
	ABLi_addFunction("getobjectivefailed", false, nullptr, "b", execGetObjectiveFailed);
	ABLi_addFunction("getenemydestroyed", false, nullptr, "b", execGetEnemyDestroyed);
	ABLi_addFunction("getfriendlydestroyed", false, nullptr, "b", execGetFriendlyDestroyed);
	ABLi_addFunction("getplayerincombat", false, nullptr, "b", execPlayerInCombat);
	ABLi_addFunction("getsensorsactive", false, nullptr, "b", execGetSensorsActive);
	ABLi_addFunction("getcurrentmusicid", false, nullptr, "i", execGetCurrentMusicId);
	ABLi_addFunction("getmissiontune", false, nullptr, "i", execGetMissionTuneId);
	
	ABLi_addFunction("requesthelp", false, "iRrRri", "r", execRequestHelp);
	ABLi_addFunction("requesttarget", false, "Rr", "i", execRequestTarget);
	ABLi_addFunction("requestshelter", false, "*", "i", execRequestShelter);
	ABLi_addFunction("mcprint", false, "?", nullptr, execMCPrint);

	ABLi_addFunction("getmissionstatus", false, nullptr, "i", execGetMissionStatus);
	ABLi_addFunction("addtriggerarea", false, "iiiiii", "i", execAddTriggerArea);
	ABLi_addFunction("istriggerareahit", false, "i", "b", execIsTriggerAreaHit);
	ABLi_addFunction("resettriggerarea", false, "i", nullptr, execResetTriggerArea);
	ABLi_addFunction("removetriggerarea", false, "i", nullptr, execRemoveTriggerArea);
	ABLi_addFunction("getweapons", false, "Ii", "i", execGetWeapons);
	ABLi_addFunction("setmovearea", false, "Rr", nullptr, execSetMoveArea);
	ABLi_addFunction("getweaponsstatus", false, "I", "i", execGetWeaponsStatus);
	ABLi_addFunction("cleartacorder", false, nullptr, nullptr, execClearTacOrder);
	ABLi_addFunction("playwave", false, "Ci", "i", execPlayWave);
	ABLi_addFunction("objectteam", false, "i", "i", execObjectTeam);
	ABLi_addFunction("setwillhelp", false, "b", "b", execSetWillHelp);
	ABLi_addFunction("getlastscan", false, nullptr, "i", execGetLastScan);
	ABLi_addFunction("getmapinfo", false, "I", nullptr, execGetMapInfo);
	
	ABLi_addFunction("getgeneralalarm", false, nullptr, "i", execGetGeneralAlarm);
	ABLi_addFunction("setgeneralalarm", false, "i", nullptr, execSetGeneralAlarm);

	ABLi_addFunction("isoffmap", false, "R", "b", execIsOffMap);
	ABLi_addFunction("setgoalplanning", false, "b", "b", execSetGoalPlanning);

	ABLi_addFunction("seteject", false, "b", "b", execSetEject);
	ABLi_addFunction("setkeepmoving", false, "b", "b", execSetKeepMoving);

	ABLi_addFunction("animationcallout", false, "ibbri", "b", execAnimationCallout);
	ABLi_addFunction("tutorialtext", false, "i", nullptr, execTutorialText);
	ABLi_addFunction("guiisaoe", false, nullptr, "b", execGUIIsAOEStyle);
	ABLi_addFunction("logisticsscreenid", false, nullptr, "i", execLogisticsScreenId);
	ABLi_addFunction("logisticsanimationcallout", false, "ibri", "b", execLogisticsAnimationCallout);
	ABLi_addFunction("logisticsincallout", false, nullptr, "b", execLogisticsInCallout);
	ABLi_addFunction("isplayingvoiceover", false, nullptr, "b", execIsPlayingVoiceOver);
	ABLi_addFunction("getlogisticstime", false, nullptr, "r", execGetLogisticsTime);
	ABLi_addFunction("stopvoiceover", false, nullptr, nullptr, execStopVoiceOver);
	ABLi_addFunction("incallout", false, nullptr, "b", execInCallout);
	ABLi_addFunction("setinvulnerable", false, "b", nullptr, execSetInvulnerable);
	ABLi_addFunction("freezegui", false, "b", nullptr, execFreezeGUI);

	// Brain libraries supply the magic/core attack, patrol, guard and escort routines.
	// Native registrations would shadow them and disable proactive AI.
	ABLi_addFunction("setwillrequesthelp",   false, "b",   nullptr, execSetWillRequestHelp);
	ABLi_addFunction("tdebugstring",         false, "C",   nullptr, execTDebugString);
	ABLi_addFunction("isdeadorfled",         false, "i",   "b",  execIsDeadOrFled);
	ABLi_addFunction("printteamstatus",      false, "i",   nullptr, execPrintTeamStatus);

	ABLi_addFunction("settextmsg",           false, "i*i", nullptr, execSetTextMsg);
	ABLi_addFunction("setlargemsg",          false, "i*i", nullptr, execSetLargeMsg);
	ABLi_addFunction("setmissiontune",       false, "i",   nullptr, execSetMissionTune);
	ABLi_addFunction("addresourcepoints",    false, "i",   nullptr, execAddResourcePoints);
	ABLi_addFunction("addmoney",             false, "i",   nullptr, execAddMoney);
	ABLi_addFunction("settxtbuildingname",   false, "iC",  nullptr, execSetTxtBuildingName);
	ABLi_addFunction("ablprint",             false, "C*",  nullptr, execAblPrint);

	ABLi_addFunction("teleporttopoint",      false, "iR",  nullptr, execTeleportToPoint);
	ABLi_addFunction("addmovertoplayer",     false, "iii", nullptr, execAddMoverToPlayer);
	ABLi_addFunction("removemoverfromplayer",false, "iii", nullptr, execRemoveMoverFromPlayer);
	ABLi_addFunction("setbrain",             false, "iC",  nullptr, execSetBrainFixed);
	ABLi_addFunction("setbrainnew",          false, "iC",  nullptr, execSetBrainNew);
	ABLi_addFunction("destroymechbodylocation", false, "ii", nullptr, execDestroyMechBodyLocation);
	ABLi_addFunction("damagemecharmor",      false, "i",   nullptr, execDamageMechArmor);
	ABLi_addFunction("setmechgesture",       false, "ii",  nullptr, execSetMechGesture);
	ABLi_addFunction("setunitforcegroup",    false, "ii",  nullptr, execSetUnitForceGroup);

	ABLi_addFunction("toggleairstrike",      false, nullptr,  nullptr, execToggleAirStrike);
	ABLi_addFunction("togglesensorstrike",   false, nullptr,  nullptr, execToggleSensorStrike);
	ABLi_addFunction("toggleartillerypiece", false, nullptr,  nullptr, execToggleArtilleryPiece);
	ABLi_addFunction("togglerepairtruck",    false, nullptr,  nullptr, execToggleRepairTruck);
	ABLi_addFunction("togglesalvagecraft",   false, nullptr,  nullptr, execToggleSalvageCraft);
	ABLi_addFunction("setrepairtruckenabled",  false, "b", nullptr, execSetRepairTruckEnabled);
	ABLi_addFunction("setsalvagecraftenabled", false, "b", nullptr, execSetSalvageCraftEnabled);

	ABLi_addFunction("gettargetrelativeposition",    false, "iRR", nullptr, execGetTargetRelativePosition);
	ABLi_addFunction("getrelativepositiontotarget",  false, "iriR", nullptr, execGetRelativePositionToTarget);
	ABLi_addFunction("newdistancetoposition", false, "iR", "r", execNewDistanceToPosition);
	ABLi_addFunction("numfriendswithinradius", false, "ir", "i", execNumFriendsWithinRadius);
	ABLi_addFunction("numenemieswithinradius", false, "ir", "i", execNumEnemiesWithinRadius);
	ABLi_addFunction("hasorderfromplayer",   false, "i",   "b",  execHasOrderFromPlayer);
	ABLi_addFunction("isselected",           false, nullptr,  "b",  execIsSelected);
	ABLi_addFunction("isrefit",              false, nullptr,  "b",  execIsRefit);
	ABLi_addFunction("needsrefit",           false, nullptr,  "b",  execNeedsRefit);

	ABLi_addFunction("hirepilot",            false, "C",   nullptr, execHirePilot);
	ABLi_addFunction("addnewpilot",          false, "iCC", nullptr, execAddNewPilot);
	ABLi_addFunction("addnewpilottoplayer",  false, "iCC", nullptr, execAddNewPilotToPlayer);
	ABLi_addFunction("autorepairwithinradius", false, "ir", nullptr, execAutoRepairWithinRadius);
	ABLi_addFunction("scanareacapture",      false, "r",   nullptr, execScanAreaCapture);
	ABLi_addFunction("scanarearepair",       false, "r",   nullptr, execScanAreaRepair);
	ABLi_addFunction("setcampaignglobalvar", false, "ii",  nullptr, execSetCampaignGlobalVar);
	ABLi_addFunction("getcampaignglobalvar", false, "i",   "i",  execGetCampaignGlobalVar);
	ABLi_addFunction("setfixedbuildingrp",   false, "ii",  nullptr, execSetFixedBuildingRp);
	ABLi_addFunction("gettimeoflaststep",    false, nullptr,  "r",  execGetTimeOfLastStep);
	ABLi_addFunction("iscurtacordermoveorder", false, nullptr, "b", execIsCurTacOrderMoveOrder);
	ABLi_addFunction("getcurtacordertime",   false, nullptr,  "r",  execGetCurTacOrderTime);
	ABLi_addFunction("clearmoveorders",      false, nullptr,  nullptr, execClearMoveOrdersOmni);


	logAblFunctionTableCapacity();
}

void closeABL (void) {

	// ABLi_close hands back every block of the environment (its pools are freed last).
	ABLi_close();
}
