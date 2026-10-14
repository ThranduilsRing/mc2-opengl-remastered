// Mission: Mission::init, the mission load sequence.
// Declared in the class headers; shared names in mission_internal.h.
#include "mission_internal.h"
#include "mission_init_frame.h"


// SIM-SPEED-BUTTON-1: dev-shell `sim_speed [--value V]` command — get (no
// value) or set the runtime multiplier through the SAME setter the HUD speed
// tab uses. Registered from Mission::init; inert unless MC2_DEV_SHELL=1.
#include "../GameOS/gameos/gos_dev_shell.h"
#include "mc2fmt.h"
#include "appearance_rng.h"
static bool devShellSimSpeed(const char* paramsJson,
                             std::string& dataJsonOut, std::string& errorOut)
{
	// flat params object from our own client: {"value":"2"} or {}
	if (paramsJson)
	{
		const char* p = strstr(paramsJson, "\"value\"");
		if (p)
		{
			p = strchr(p + 7, ':');
			if (p)
			{
				++p;
				while (*p == ' ' || *p == '\t' || *p == '"') ++p;
				simSpeedSetMultiplier((float)atof(p));
			}
		}
	}
	char buf[64];
	mc2::fmt::to_buf(buf, "{{\"multiplier\":{:.2f}}}", (double)simSpeedGetMultiplier());
	dataJsonOut = buf;
	(void)errorOut;
	return true;
}


// Reset the per-mission sim state, reseed the RNG and clear the termination flags.
void Mission::initPrologue(MissionInitFrame& f) {
	auto& missionName = f.missionName;
	mission_phase_begin();

	// SIM-SPEED-BUTTON-1: HUD-set sim speed is scoped to the mission that set
	// it — every mission starts back at the MC2_SIM_SPEED env seed (1.0 when
	// unset). Also (re)register the dev-shell get/set command (inert without
	// MC2_DEV_SHELL=1; re-registration replaces).
	simSpeedResetToSeed();
	// FREEZE-MOVERS-1: same scoping rule as sim speed above. Re-stamps the
	// mission-frame baseline and disarms any freeze left over from a previous
	// mission, so `freeze_movers on <frame>` counts frames from THIS mission's
	// start (g_mc2FrameCounter is process-lifetime and never resets) and a
	// campaign cannot inherit a freeze nobody asked for in this mission.
	mc2FreezeMoversMissionReset();
	gos_dev_shell::registerCommand("sim_speed", devShellSimSpeed);
	::mc2_cpu_proj_cost::hard_reset("Mission::init");

	// Reset GPU static-prop batcher state at every map boundary, before any
	// actor registerType() calls happen during actor spawn (Task 6).
	// M1 Task 13 (order-correct split): call order is byte-identical to original:
	//   1. GpuStaticPropBatcher::onMapLoad()   ← beginMissionEarly()
	//   2. GpuMechBatcher::onMapLoad()
	//   3. setMissionForIbl() + Registry::init() + RenderWorld::init() ← beginMissionLate()
	//   4. Mech::beginMission()
	GameAdapters::StaticProp::beginMissionEarly();         // M1 Task 13
	GpuMechBatcher::instance().onMapLoad();
	GameAdapters::StaticProp::beginMissionLate(missionName); // M1 Task 13
	GameAdapters::Mech::beginMission();              // M2: mech lifecycle

	// The mission seed: hash(missionName), overridable with MC2_RNG_SEED. The appearance stream always restarts from it
	// (a mission's decals and fall directions replay); the simulation stream only under MC2_DETERMINISTIC_RNG, so with the
	// gate OFF the game keeps the stock wall-clock srand (byte-identical to stock).
	{
		uint32_t seed = mc2_det_rng::hashMissionName(missionName);
		const std::string seedText = mc2::gate::str<"MC2_RNG_SEED">();   // "" when unset or empty
		if (!seedText.empty())
			seed = (uint32_t)strtoul(seedText.c_str(), nullptr, 0);
		mc2_appearance_rng::reseed(seed);
		ResetSimRandomDraws();
		if (mc2::gate::get<"MC2_DETERMINISTIC_RNG">()) {
			gos_srand(seed);
			mc2::fmt::print_err("[DET_RNG] mission={} reseed=0x{:08X} (MC2_DETERMINISTIC_RNG=1)\n", mc2::fmt::safe(missionName ? missionName : "(null)"), mc2::fmt::as_u32(seed));
		}
	}

	neverEndingStory = false;
	invulnerableON = false;

	terminationResult = mis_PLAYING; 


	loadProgress = 0.0f;

	loadProgress = 1.0f;
}

// Load the SP or MP team relation matrix and reset the turn counters.
void Mission::initTeamRelations(long loadType) {
	if ((loadType == MISSION_LOAD_SP_QUICKSTART) || (loadType == MISSION_LOAD_SP_LOGISTICS)) {
		char teamRelationsForSP[MAX_TEAMS][MAX_TEAMS] = {
			{0, 2, 1, 2, 2, 2, 2, 2},
			{2, 0, 2, 2, 2, 2, 2, 2},
			{1, 2, 0, 2, 2, 2, 2, 2},
			{2, 2, 2, 0, 2, 2, 2, 2},
			{2, 2, 2, 2, 0, 2, 2, 2},
			{2, 2, 2, 2, 2, 0, 2, 2},
			{2, 2, 2, 2, 2, 2, 0, 2},
			{2, 2, 2, 2, 2, 2, 2, 0}
		};
		for (long i = 0; i < MAX_TEAMS; i++)
			for (long j = 0; j < MAX_TEAMS; j++) {
				Team::relations[i][j] = teamRelationsForSP[i][j];
				TeamRelations[i][j] = teamRelationsForSP[i][j];
			}
		}
	else {
		char teamRelationsForMP[MAX_TEAMS][MAX_TEAMS] = {
			{0, 2, 2, 2, 2, 2, 2, 2},
			{2, 0, 2, 2, 2, 2, 2, 2},
			{2, 2, 0, 2, 2, 2, 2, 2},
			{2, 2, 2, 0, 2, 2, 2, 2},
			{2, 2, 2, 2, 0, 2, 2, 2},
			{2, 2, 2, 2, 2, 0, 2, 2},
			{2, 2, 2, 2, 2, 2, 0, 2},
			{2, 2, 2, 2, 2, 2, 2, 0}
		};
		for (long i = 0; i < MAX_TEAMS; i++)
			for (long j = 0; j < MAX_TEAMS; j++) {
				Team::relations[i][j] = teamRelationsForMP[i][j];
				TeamRelations[i][j] = teamRelationsForMP[i][j];
			}
	}
				
	turn = 0;
	terminationCounterStarted = 0;
	// GAMEPLAY-DRIVER-2: scope a forced outcome to the mission that forced it.
	forcedTerminationResult = mis_PLAYING;
}

// Load ABL, the TGL and the mission heap.
void Mission::initCoreSystems() {
	#ifdef LAB_ONLY
	x=GetCycles();
	#endif

	initABL();

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeABLLoad=x1-x;
#endif

	{ ZoneScopedN("Mission::init initBareMinimum"); initBareMinimum(); }
	loadProgress = 4.0f;
	{ ZoneScopedN("Mission::init initTGLForMission"); initTGLForMission(); }
	mission_phase_mark("setup_tgl_ready"); // LOAD-PHASE-FACTS-1
	
	missionHeap = new UserHeap;
	gosASSERT(missionHeap != NULL);
	
	missionHeap->init(missionHeapSize,"MISSION");
}

// Open gamesys.fit and read the weapon ranges and the general visual / sensor settings.
void Mission::initGameSystemRanges(MissionInitFrame& f) {
	auto& gameSystemFile = f.gameSystemFile; auto& result = f.result; auto& forestMoveCost = f.forestMoveCost;
	FullPathFileName fullGameSystemName;
	fullGameSystemName.init(missionPath, "gamesys", ".fit");
	
	gameSystemFile = new FitIniFile;
	gosASSERT(gameSystemFile != NULL);
		
	result = gameSystemFile->open(fullGameSystemName);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1):
	// gamesys.fit is a required per-mission data file; if it fails to open every
	// subsequent readId below reads uninitialized WeaponRanges/ranges/mine/smoke
	// values used unconditionally. fatal mode STOPs at the contract violation;
	// log mode logs and preserves the legacy (garbage-read) path bit-for-bit.
	MC2_VERIFY(result == NO_ERR, "Mission::init: gamesys.fit open failed (%ld): %s",
		result, (const char*)fullGameSystemName);

	result = gameSystemFile->seekBlock("WeaponRanges");
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: gamesys.fit missing [WeaponRanges] block");

	float span[2];
	result = gameSystemFile->readIdFloatArray("Short", span, 2);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [WeaponRanges] missing Short");
	WeaponRanges[WEAPON_RANGE_SHORT][0] = span[0];
	WeaponRanges[WEAPON_RANGE_SHORT][1] = span[1];

	result = gameSystemFile->readIdFloatArray("Medium", span, 2);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [WeaponRanges] missing Medium");
	WeaponRanges[WEAPON_RANGE_MEDIUM][0] = span[0];
	WeaponRanges[WEAPON_RANGE_MEDIUM][1] = span[1];

	result = gameSystemFile->readIdFloatArray("Long", span, 2);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [WeaponRanges] missing Long");
	WeaponRanges[WEAPON_RANGE_LONG][0] = span[0];
	WeaponRanges[WEAPON_RANGE_LONG][1] = span[1];

	result = gameSystemFile->readIdFloatArray("OptimalRangePoints", OptimalRangePoints, 5);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [WeaponRanges] missing OptimalRangePoints");

	for (long i = 0; i < 5; i++)
		for (long j = 0; j < 3; j++) {
			OptimalRangePointInRange[i][j] = false;
			if (OptimalRangePoints[i] > WeaponRanges[j][0])
				if (OptimalRangePoints[i] <= WeaponRanges[j][1])
					OptimalRangePointInRange[i][j] = true;
		}

	result = gameSystemFile->seekBlock("General");
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: gamesys.fit missing [General] block");

	result = gameSystemFile->readIdFloat("MaxVisualRange",maxVisualRange);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [General] missing MaxVisualRange");
	MaxVisualRadius = maxVisualRange * 1.4142;

	result = gameSystemFile->readIdFloat("FireVisualRange",fireVisualRange);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [General] missing FireVisualRange");

	result = gameSystemFile->readIdULong("MaxTreeLOSBlock",MaxTreeLOSCellBlock);
	if (result != NO_ERR)
		MaxTreeLOSCellBlock = 5;

	result = gameSystemFile->readIdFloatArray("WeaponRange", WeaponRange, NUM_FIRERANGES);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [General] missing WeaponRange");

	result = gameSystemFile->readIdFloat("DefaultAttackRange", DefaultAttackRange);
	if (result != NO_ERR)
		DefaultAttackRange = 75.0;

	result = gameSystemFile->readIdFloat("BaseSensorRange",baseSensorRange);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [General] missing BaseSensorRange");

	result = gameSystemFile->readIdLongArray("VisualRangeTable",visualRangeTable,256);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [General] missing VisualRangeTable");

	result = gameSystemFile->readIdFloat("BaseHeadShotElevation",BaseHeadShotElevation);
	if (result != NO_ERR)
		BaseHeadShotElevation = 1.0f;

	result = gameSystemFile->readIdLong("ForestMoveCost", forestMoveCost);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [General] missing ForestMoveCost");

	result = gameSystemFile->readIdFloat("MaxUnitExtractDistance",MaxExtractUnitDistance);
	if (result != NO_ERR)
		MaxExtractUnitDistance = 1280.0f;	//Ten Tiles away
}

// Read the component list, the god-mode / footprint flags and the mover game-system blocks.
void Mission::initGameSystemGeneral(MissionInitFrame& f) {
	auto& gameSystemFile = f.gameSystemFile; auto& result = f.result;
	if (!MasterComponent::masterList) {
		FullPathFileName compFileName;
		compFileName.init(objectPath,"compbas",".csv");
#ifdef _DEBUG
		long loadErr = 
#endif
		    MasterComponent::loadMasterList(compFileName, 255, baseSensorRange);
		gosASSERT(loadErr == NO_ERR);

		// EQUIPMENT-SOURCE-FLIP-SWITCH-1: gated stat overlay (MC2_EQUIPMENT_
		// DEFINITION_SOURCE=generated; DEFAULT legacy). Overwrites ONLY the green
		// weapon stat fields on matched rows from the generated defs; gate-OFF is
		// a no-op with zero I/O (byte-identical). Firing reads masterList as always.
		overlayGeneratedEquipmentStatsIfEnabled();

		// EQUIP-ADD-FILL-1 (S2): gated ADD pass -- mod defs with an explicit
		// LegacyMasterId on an UNDEFINED compbas ordinal materialize as live
		// MasterComponent rows (MC2_EQUIPMENT_ADD + MC2_UNIT_PROFILE_DATA, both
		// default OFF -> no-op). Runs BEFORE the registry rebuild below so the
		// filled rows register under their namespaced mod ids.
		applyEquipmentAddFillIfEnabled(baseSensorRange);

		// EQUIPMENT-REGISTRY-FACADE-1: build the parallel core:<slug> <-> ordinal
		// registry beside masterList (gated MC2_UNIT_PROFILE_DATA; gate-OFF no-op).
		// Additive -- reads masterList only, changes no gameplay path.
		buildAndDumpEquipmentRegistryIfEnabled();
		dumpFireExecutorProofIfEnabled();
	}

	result = gameSystemFile->readIdUChar("GodMode", godMode);
	if (result != NO_ERR)
		godMode = 0;

	unsigned char revealTacMap;
	result = gameSystemFile->readIdUChar("RevealTacMap", revealTacMap);
	if (result != NO_ERR)
		revealTacMap = 0;
		
	result = gameSystemFile->readIdUChar("FootPrints", footPrints);
	if (result != NO_ERR)
		footPrints = 1;

	result = gameSystemFile->readIdLong("BonusTonnageDivisor",tonnageDivisor);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [General] missing BonusTonnageDivisor");

	result = gameSystemFile->readIdLong("BonusPointsPerTon",resourcesPerTonDivided);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [General] missing BonusPointsPerTon");
	
#ifndef FINAL
 	result = gameSystemFile->readIdFloat("CheatHitDamage",CheatHitDamage);
	if (result != NO_ERR)
		CheatHitDamage = 5.0f;
#endif
	
	InitDifficultySettings(gameSystemFile);

	result = Mover::loadGameSystem(gameSystemFile, maxVisualRange);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: Mover::loadGameSystem failed (%ld)", result);


	result = BattleMech::loadGameSystem(gameSystemFile);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: BattleMech::loadGameSystem failed (%ld)", result);

	result = GroundVehicle::loadGameSystem(gameSystemFile);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: GroundVehicle::loadGameSystem failed (%ld)", result);

#ifdef USE_ELEMENTALS
	result = loadElementalGameSystem(gameSystemFile);
	gosASSERT(result == NO_ERR);
#endif
}

// Read the mine, smoke and fire settings.
void Mission::initGameSystemHazards(MissionInitFrame& f) {
	auto& gameSystemFile = f.gameSystemFile; auto& result = f.result;
	result = gameSystemFile->seekBlock("Mine");
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: gamesys.fit missing [Mine] block");

	result = gameSystemFile->readIdFloat("BaseDamage", MineDamage);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [Mine] missing BaseDamage");

	result = gameSystemFile->readIdFloat("SplashDamage", MineSplashDamage);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [Mine] missing SplashDamage");

	result = gameSystemFile->readIdFloat("SplashRange", MineSplashRange);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [Mine] missing SplashRange");

	result = gameSystemFile->readIdLong("Explosion", MineExplosion);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [Mine] missing Explosion");

	result = gameSystemFile->readIdLong("MineLayThrottle", MineLayThrottle);
	if (result != NO_ERR)
		MineLayThrottle = 50;

	result = gameSystemFile->readIdLong("MineSweepThrottle", MineSweepThrottle);
	if (result != NO_ERR)
		MineSweepThrottle = 50;

	result = gameSystemFile->readIdFloat("MineWaitTime", MineWaitTime);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [Mine] missing MineWaitTime");

	result = gameSystemFile->readIdFloat("StrikeWaitTime", StrikeWaitTime);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [Mine] missing StrikeWaitTime");

	result = gameSystemFile->readIdFloat("StrikeTimeToImpact", StrikeTimeToImpact);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [Mine] missing StrikeTimeToImpact");

	result = gameSystemFile->seekBlock("Smoke");
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: gamesys.fit missing [Smoke] block");

	result = gameSystemFile->readIdLong("MaxSmokeSpheres",totalSmokeSpheres);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [Smoke] missing MaxSmokeSpheres");

	result = gameSystemFile->readIdLong("TotalSmokeShapeSize",totalSmokeShapeSize);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [Smoke] missing TotalSmokeShapeSize");

	result = gameSystemFile->seekBlock("Fire");
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: gamesys.fit missing [Fire] block");

	result = gameSystemFile->readIdLong("MaxFiresBurning", maxFiresBurning);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [Fire] missing MaxFiresBurning");

	result = gameSystemFile->readIdFloat("MaxFireBurnTime", maxFireBurnTime);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [Fire] missing MaxFireBurnTime");
}

// Open the mission .fit and activate the mod for it.
void Mission::initMissionFile(Stuff::Vector3D* dropZoneList, MissionInitFrame& f) {
	auto& missionName = f.missionName; auto& result = f.result; auto& missionFileName = f.missionFileName;
	missionFileName.init(missionPath,missionName,".fit");

	ActivateModForMission(missionFileName);

	duration = 60;

	missionFile = new FitIniFile;
	gosASSERT(missionFile != NULL);

	result = missionFile->open(missionFileName);
	if (result != NO_ERR)
		STOP(("Unable to open Mission File %s",missionFileName));

	if (!dropZoneList) {
		result = missionFile->seekBlock("Multiplayer");
		if (result == NO_ERR) {
		}
	}

#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeMiscToTeamLoad=x-x1;
#endif
}

// Read the sky number, reset stale teams and commanders and run the cycle-test fixture.
void Mission::initSkyAndTeamReset(MissionInitFrame& f) {
	auto& result = f.result;
	//-----------------------------------
	// Find the SKY Number and save it.
	// If no number, i.e. an old mission file,
	// simply make it 1 until its written out
	// in the magical editor (tm)
	result = missionFile->seekBlock("TheSky");
	if (result != NO_ERR)
		theSkyNumber = DEFAULT_SKY;
	else
	{
		result = missionFile->readIdLong("SkyNumber",theSkyNumber);
		if (result != NO_ERR)
			theSkyNumber = DEFAULT_SKY;
			
		if ((theSkyNumber < 1) || (theSkyNumber > 21))
			theSkyNumber = DEFAULT_SKY;
	}
		
	loadProgress = 10.0f;

	result = missionFile->seekBlock("Teams");
	Assert(result == NO_ERR, result, " Could not find Teams Block ");
	// TEAM-COMMANDER-OWNERSHIP-1: re-init guard against stale teams/commanders
	// from a prior mission that was NOT torn down (quickstart -> mission without a
	// logistics destroy). Was a duplicated inline free loop identical to the one
	// in Mission::destroy -- the divergence/dual-free source. Now the single
	// shared authority; also nulls the home aliases before they are re-pointed
	// below at :2808/:2833.
	Mission::resetTeamsAndCommanders();

	// MC2_MISSION_CYCLE_TEST fixture (default OFF -> stock byte-identical): the
	// real in-process proof for the class this slice retires. run_smoke launches
	// a fresh process per mission and kills it mid-play, so it never exercises an
	// in-process teardown+reload -- the exact path where the old duplicated free
	// loops used to double-free / leave home dangling. This runs a miniature
	// mission cycle of the static arrays in isolation, on EVERY mission load
	// (so it is observable inside the 30s smoke window):
	//   populate (mimics init's create loop, bumps the static counts + home) ->
	//   resetTeamsAndCommanders() (mimics destroy's free) ->
	//   resetTeamsAndCommanders() AGAIN (the ex-double-free init-then-destroy path).
	// A non-idempotent / non-null-safe helper AVs on the second free or trips the
	// verify; a helper that forgot the home aliases leaves them dangling. The
	// statics are left empty (as at entry), so the real init below is untouched.
	{
		static const bool s_missionCycleTest = mc2::gate::get<"MC2_MISSION_CYCLE_TEST">();
		if (s_missionCycleTest) {
			// State must be quiescent here (the reset above just ran).
			MC2_VERIFY(Team::numTeams == 0 && Commander::numCommanders == 0,
				"MC2_MISSION_CYCLE_TEST: entry state not clean (teams=%ld commanders=%ld)",
				Team::numTeams, Commander::numCommanders);

			// Populate a couple of slots exactly like the real create loop:
			// new Team + init(i) bumps Team::numTeams; new Commander bumps
			// Commander::numCommanders (see team.cpp:134 / comndr.cpp init()).
			for (long i = 0; i < 2 && i < MAX_TEAMS; i++) {
				Team::teams[i] = new Team;
				Team::teams[i]->init(i);
			}
			for (long i = 0; i < 2 && i < MAX_COMMANDERS; i++) {
				Commander::commanders[i] = new Commander;
				Commander::commanders[i]->setId(i);
			}
			Team::home = Team::teams[0];
			Commander::home = Commander::commanders[0];
			MC2_VERIFY(Team::numTeams == 2 && Commander::numCommanders == 2 &&
					Team::home != NULL && Commander::home != NULL,
				"MC2_MISSION_CYCLE_TEST: synthetic populate wrong "
				"(teams=%ld commanders=%ld)", Team::numTeams, Commander::numCommanders);

			// First reset = the "destroy frees" leg.
			Mission::resetTeamsAndCommanders();
			// Second reset back-to-back = the ex-double-free (init frees, then
			// destroy frees again). MUST be a clean no-op with home NULL.
			Mission::resetTeamsAndCommanders();
			MC2_VERIFY(Team::numTeams == 0 && Commander::numCommanders == 0 &&
					Team::home == NULL && Commander::home == NULL &&
					Team::teams[0] == NULL && Commander::commanders[0] == NULL,
				"MC2_MISSION_CYCLE_TEST: reset not idempotent / home not nulled "
				"(teams=%ld commanders=%ld home=%p/%p slot0=%p/%p)",
				Team::numTeams, Commander::numCommanders,
				(void*)Team::home, (void*)Commander::home,
				(void*)Team::teams[0], (void*)Commander::commanders[0]);
		}
	}
}

// Create the teams and commanders from the mission file or the multiplayer setup.
void Mission::initTeamsAndCommanders(long loadType, Stuff::Vector3D* dropZoneList, char (*commandersToLoad)[3], MissionInitFrame& f) {
	auto& missionName = f.missionName; auto& result = f.result; auto& maxTeamID = f.maxTeamID;
	auto& maxCommanderID = f.maxCommanderID;
	maxTeamID = -1;
	maxCommanderID = -1;
	if (loadType == MISSION_LOAD_MP_LOGISTICS) {
		for (long i = 0; i < MAX_MC_PLAYERS; i++) {
			if (MPlayer->playerInfo[i].team > maxTeamID)
				maxTeamID = MPlayer->playerInfo[i].team;
			if (MPlayer->playerInfo[i].commanderID > maxCommanderID)
				maxCommanderID = MPlayer->playerInfo[i].commanderID;
		}
		}
	else {
		result = missionFile->seekBlock("Parts");
		// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1)
		MC2_VERIFY(result == NO_ERR, "Mission::init: no [Parts] block in mission .fit: %s",
			missionName ? missionName : "(null)");
		result = missionFile->readIdULong("NumParts",numParts);
		// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1)
		if (!MC2_VERIFY(result == NO_ERR, "Mission::init: [Parts] NumParts read failed"))
			numParts = 0;	// log mode: degrade to no parts instead of iterating garbage
		if (numParts)
			for (int i = 1; i < long(numParts + 1); i++) {
				char partName[12];
				mc2::fmt::to_buf(partName, "Part{:d}", mc2::fmt::as_i32(i));

				result = missionFile->seekBlock(partName);
				// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1)
				if (!MC2_VERIFY(result == NO_ERR, "Mission::init: missing [%s] block", partName))
					continue;	// log mode: skip the absent part

				char teamID = -1;
				result = missionFile->readIdChar("TeamId", teamID);
				// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1)
				MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing TeamId", partName);

				char commanderID = -1;
				result = missionFile->readIdChar("CommanderId", commanderID);
				if (result != NO_ERR) {
					long cID;
					result = missionFile->readIdLong("CommanderId", cID);
					// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1)
					MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing CommanderId", partName);
					commanderID = (char)cID;
				}

				if (MPlayer && dropZoneList) {
					//-------------------------------------------------------------
					// Since dropZoneList is not NULL, we know this was not started
					// from the command-line...
					long origCommanderID = commanderID;
					commanderID = commandersToLoad[origCommanderID][0];
					teamID = commandersToLoad[origCommanderID][1];
				}

				// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1):
				// file-supplied TeamId/CommanderId (chars, up to 127) feed
				// maxTeamID/maxCommanderID unchecked; ids > 7 overflow the
				// static Team::teams[MAX_TEAMS]/Commander::commanders[
				// MAX_COMMANDERS] init-loop WRITES below (adversarial A5, the
				// SP-reachable landmine). Verify at the read site + bound.
				if (!MC2_VERIFY((long)teamID < MAX_TEAMS,
						"Mission::init: [%s] TeamId %d exceeds MAX_TEAMS-1 (%d) -- bounding",
						partName, (int)teamID, MAX_TEAMS - 1))
					teamID = MAX_TEAMS - 1;
				if (!MC2_VERIFY((long)commanderID < MAX_COMMANDERS,
						"Mission::init: [%s] CommanderId %d exceeds MAX_COMMANDERS-1 (%d) -- bounding",
						partName, (int)commanderID, MAX_COMMANDERS - 1))
					commanderID = MAX_COMMANDERS - 1;

				if (commanderID > maxCommanderID)
					maxCommanderID = commanderID;
				if (teamID > maxTeamID)
					maxTeamID = teamID;
			}
	}

	//----------------------------------------------
	// Now, init the teams and commanders we need...
	// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1):
	// write-side guard for the marquee static-array overflow -- covers ALL
	// sources of maxTeamID/maxCommanderID (file parts above, MP_LOGISTICS
	// playerInfo branch), not just the SP read site.
	if (!MC2_VERIFY(maxTeamID < MAX_TEAMS,
			"Mission::init: maxTeamID %ld >= MAX_TEAMS %d -- bounding (Team::teams[] write overflow)",
			maxTeamID, MAX_TEAMS))
		maxTeamID = MAX_TEAMS - 1;
	if (!MC2_VERIFY(maxCommanderID < MAX_COMMANDERS,
			"Mission::init: maxCommanderID %ld >= MAX_COMMANDERS %d -- bounding (Commander::commanders[] write overflow)",
			maxCommanderID, MAX_COMMANDERS))
		maxCommanderID = MAX_COMMANDERS - 1;
	for (int i = 0; i <= maxTeamID; i++) {
		Team::teams[i] = new Team;
		Team::teams[i]->init(i);
	}
	for (int i = 0; i <= maxCommanderID; i++) {
		Commander::commanders[i] = new Commander;
		Commander::commanders[i]->setId(i);
	}
	
	if (MPlayer) {
		Team::home = Team::teams[MultiPlayTeamId];
		Commander::home = Commander::commanders[MultiPlayCommanderId];
		for (long i = 0; i <= maxCommanderID; i++)
			if (MPlayer->playerInfo[i].commanderID > -1)
				Commander::commanders[MPlayer->playerInfo[i].commanderID]->setTeam(Team::teams[MPlayer->playerInfo[i].team]);
		}
	else {
		// CRASH-HARDEN (MISSION-NO-COMMANDERS): a mission .fit with no [Parts]
		// (or no part carrying TeamId/CommanderId) leaves maxTeamID/
		// maxCommanderID at -1, so the init loops above create NO teams and NO
		// commanders — teams[0]/commanders[0] stay NULL and the setTeam calls
		// below AV (0xC0000005 null read). The seekBlock("Parts") gosASSERTs
		// upstream are no-ops in RelWithDebInfo, so this is reachable with any
		// hand-made/imported map (e.g. terrain-gen imports that only wrote
		// [ColorMap]/[Terrain]). Fail with a clear fatal instead of the AV.
		// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1):
		// aligned with the existing CRASH-HARDEN STOP (b71702ba) below -- in
		// fatal mode the verify stops first with the same message; in log/off
		// mode the legacy STOP path is preserved unchanged.
		MC2_VERIFY((Team::teams[0] != NULL) && (Commander::commanders[0] != NULL),
			"mission has no commanders -- invalid mission data (no [Parts] with TeamId/CommanderId): %s",
			missionName ? missionName : "(null)");
		if ((Team::teams[0] == NULL) || (Commander::commanders[0] == NULL))
			STOP(("mission has no commanders -- invalid mission data (no [Parts] with TeamId/CommanderId): %s",
			      missionName ? missionName : "(null)"));
		Team::home = Team::teams[0];
		Commander::home = Commander::commanders[0];
		for (long i = 0; i <= maxCommanderID; i++) {
			// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1):
			// caller-supplied commandersToLoad[8][3] is read up to
			// maxCommanderID (now bounded above) and its commander/team ids
			// index the static arrays -- verify both ids + that the referenced
			// slots were actually created before the setTeam deref
			// (adversarial A5: the :2744 read-overflow companion).
			long ctlCommander = commandersToLoad[i][0];
			long ctlTeam = commandersToLoad[i][1];
			if (ctlCommander > -1) {
				if (!MC2_VERIFY_BOUNDS(ctlCommander, MAX_COMMANDERS, "Mission::init commandersToLoad commander") ||
					!MC2_VERIFY_BOUNDS(ctlTeam, MAX_TEAMS, "Mission::init commandersToLoad team") ||
					!MC2_VERIFY((Commander::commanders[ctlCommander] != NULL) && (Team::teams[ctlTeam] != NULL),
						"Mission::init: commandersToLoad[%ld] names uninitialized commander %ld / team %ld",
						i, ctlCommander, ctlTeam))
					continue;	// log mode: skip the malformed entry
				Commander::commanders[ctlCommander]->setTeam(Team::teams[ctlTeam]);
			}
		}
		Commander::commanders[0]->setTeam(Team::home);
	}
}

// Create the trigger-area and sensor managers and read the drop zone.
void Mission::initTriggerAndDropZone(MissionInitFrame& f) {
	auto& result = f.result;
	if (Mover::triggerAreaMgr) {
		delete Mover::triggerAreaMgr;
		Mover::triggerAreaMgr = NULL;
	}
	Mover::triggerAreaMgr = new TriggerAreaManager;
	Assert(Mover::triggerAreaMgr != NULL, 0, " Mossion.init: unable to init triggerAreaMgr ");

	SensorManager = new SensorSystemManager;
	Assert(SensorManager != NULL, 0, " Unable to init sensor system manager ");
	result = SensorManager->init(true);
	Assert(result == NO_ERR, result, " could not start Sensor System Manager ");

	result = missionFile->seekBlock( "DropZone0" );
	if ( result == NO_ERR ) // lets not enforce drop zones for now
	{
		missionFile->readIdFloat( "PositionX", dropZone.x );
		missionFile->readIdFloat( "PositionY", dropZone.y );
	}
	else
	{
		dropZone.x = -1.f;
		dropZone.y = -1.f;
	}

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeTeamLoad=x1-x;
#endif
}

// Read the mission settings, create the crater and object managers.
void Mission::initMissionSettings(MissionInitFrame& f) {
	auto& result = f.result;
	result = missionFile->seekBlock("MissionSettings");
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: mission .fit missing [MissionSettings] block");

	result = missionFile->readIdUChar("scenarioTuneNum",missionTuneNum);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [MissionSettings] missing scenarioTuneNum");

	long numRPoints;
	result = missionFile->readIdLong("ResourcePoints",numRPoints);
	if (MaxResourcePoints > -1)
		numRPoints = MaxResourcePoints;
	if (MPlayer) {
		numRPoints = MPlayer->missionSettings.resourcePoints;
		for (long i = 0; i < MAX_MC_PLAYERS; i++) {
			MPlayer->playerInfo[i].resourcePoints = MPlayer->missionSettings.resourcePoints;
			MPlayer->playerInfo[i].resourcePointsGained = 0;
			MPlayer->playerInfo[i].resourcePointsAtStart = MPlayer->missionSettings.resourcePoints;
		}
	}
	LogisticsData::instance->setResourcePoints(numRPoints);

	if (MPlayer) {
		result = missionFile->readIdULong("NumRandomRPbuildings", MPlayer->numRandomResourceBuildings);
	}

	craterManager = (CraterManagerPtr)missionHeap->Malloc(sizeof(CraterManager));
	gosASSERT(craterManager != NULL);
		
	result = craterManager->init(1000,20479,"feet");
	gosASSERT(result == NO_ERR);
	
	ObjectManager = new GameObjectManager;
	if (!ObjectManager)
		Fatal(0, " Mission.init: unable to create ObjectManager ");
	ObjectManager->init("object2", 716799, 3072000);
	gosASSERT(result == NO_ERR);

	
	ObjectManager->initCollisionSystem(missionFile);
}

// Open the terrain pak and load, prime and size the terrain.
void Mission::initTerrain(MissionInitFrame& f) {
	auto& missionName = f.missionName; auto& result = f.result; auto& pakFile = f.pakFile;
	FullPathFileName terrainFileName;
	terrainFileName.init( missionPath, missionName, ".pak" ); 

	result = pakFile.open( terrainFileName );
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1):
	// the terrain .pak is a required per-mission data file; a failed open feeds a
	// closed PacketFile into terrain load below (garbage/empty terrain).
	MC2_VERIFY( result == NO_ERR, "Mission::init: terrain .pak open failed (%ld): %s",
		result, (const char*)terrainFileName );

	land = new Terrain;

	land->getColorMapName(missionFile);

	gosASSERT(land != NULL);

	loadProgress = 15.0f;

#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeObjectLoad=x-x1;
#endif

	long terrainInitResult;
	{
		ZoneScopedN("Mission::init land->init");
		terrainInitResult = land->init(&pakFile, 0, GameVisibleVertices, loadProgress, 20.0 );
	}

	if (terrainInitResult != NO_ERR)
	{
		STOP(("Could not load terrain.  Probably size was wrong!"));
	}

	loadProgress = 35.0f;

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeTerrainLoad=x1-x;
#endif

	{ ZoneScopedN("Mission::init land->load"); land->load( missionFile ); }

	loadProgress = 36.0f;
	{ ZoneScopedN("Mission::init land->primeMissionTerrainCache"); land->primeMissionTerrainCache(loadProgress, 4.0f); }
	loadProgress = 40.0f;
	mission_phase_mark("terrain_ready"); // LOAD-PHASE-FACTS-1: FST/pak parse + terrain prime done
}

// Initialise the move system, the path-find maps and the path manager.
void Mission::initPathfinding(MissionInitFrame& f) {
	auto& forestMoveCost = f.forestMoveCost; auto& pakFile = f.pakFile;
	Assert(SimpleMovePathRange > 20, SimpleMovePathRange, " Simple MovePath Range too small ");
	MOVE_init(SimpleMovePathRange);
	if (pakFile.seekPacket(4) == NO_ERR)
	{
		if (pakFile.getPacketSize() != 0)
		{
			MOVE_readData(&pakFile, 4);
			if (GlobalMoveMap[0]->badLoad) {
				PAUSE((" Mission.init: bad/old move data — skipping gate callback wiring; pathfinding degraded "));
				GameMap->placeMoversCallback = PlaceMovers;
			} else {
				GameMap->placeMoversCallback = PlaceMovers;
				GlobalMoveMap[0]->isGateDisabledCallback = IsGateDisabled;
				GlobalMoveMap[1]->isGateDisabledCallback = IsGateDisabled;
				GlobalMoveMap[2]->isGateDisabledCallback = IsGateDisabled;
				GlobalMoveMap[0]->isGateOpenCallback = IsGateOpen;
				GlobalMoveMap[1]->isGateOpenCallback = IsGateOpen;
				GlobalMoveMap[2]->isGateOpenCallback = IsGateOpen;
			}
		}
		else {
			// Packet 4 is empty — mission was saved without MOVE data.
			STOP(("Mission has not movement Data.  QuickSaved Map?"));
		}
	}

	// Safety net: if GameMap is still NULL here the mission will crash in
	// goal.cpp, group.cpp, etc. at frame ~1455 when the AI starts calculating
	// paths (GameMap->width/height accessed without null guard everywhere).
	// Covers three failure modes:
	//   (a) seekPacket(4) returned error — no packet 4 at all
	//   (b) getPacketSize() == 0 — packet reserved but empty
	//   (c) badLoad on GlobalMoveMap[0] left GameMap initialised but
	//       GlobalMoveMap[1/2] == NULL (hover/heli units crash if ever used)
	// Build a blank all-passable MOVE from terrain dimensions.
	// Round moveSide DOWN to the nearest SECTOR_DIM multiple so
	// GlobalMap::build's assertion passes on non-standard map sizes.
	if (!GameMap) {
		int cellSide = Terrain::realVerticesMapSide - 1;
		int moveSide = cellSide * MAPCELL_DIM;
		if (moveSide % SECTOR_DIM != 0)
			moveSide = (moveSide / SECTOR_DIM) * SECTOR_DIM;  // round down
		if (moveSide > MAX_MAP_CELL_WIDTH)
			moveSide = MAX_MAP_CELL_WIDTH;
		if (moveSide > 0) {
			mc2::fmt::print_out("[MISSION] synthesizing blank MOVE: moveSide={:d} (terrainVtx={:d})\n", mc2::fmt::as_i32(moveSide), mc2::fmt::as_i32((int)Terrain::realVerticesMapSide));
			fflush(stdout);
			MOVE_buildData(moveSide, moveSide, NULL /*all-passable blank*/, 0, NULL);
			if (GameMap)
				GameMap->placeMoversCallback = PlaceMovers;
		}
	}

	PathFindMap[SECTOR_PATHMAP]->blockedDoorCallback = GetBlockedDoorCells;
	PathFindMap[SECTOR_PATHMAP]->placeStationaryMoversCallback = PlaceStationaryMovers;
	PathFindMap[SIMPLE_PATHMAP]->placeStationaryMoversCallback = PlaceStationaryMovers;
	PathFindMap[SECTOR_PATHMAP]->forestCost = forestMoveCost;
	PathFindMap[SIMPLE_PATHMAP]->forestCost = forestMoveCost;
	PathManager = new MovePathManager;

#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeMoveLoad=x-x1;
#endif
}

// Mount the ABL core libraries.
void Mission::initAblCoreLibs(MissionInitFrame& f) {
	auto& numErrors = f.numErrors; auto& numLinesProcessed = f.numLinesProcessed;
	numErrors = 0, numLinesProcessed = 0;
	// ABL-CORE-LIBRARY-MOUNT-1: declarative core-library table (replaces three hardcoded
	// gosASSERT'd loads). orders/miscfunc are universal; corebrain (MCO / wolfman-MC2X) and
	// mc2xcore (cveg / MC2X-R52) are the two core-brain variants — load whichever a mod
	// provides, so MC2X- and MCO-lineage mod missions (e.g. DarkRain "torrin") get their core
	// brain orders + eternals (corePatrol/coreGuard/coreRepair/noAttackCode/wxmCode/…) defined.
	// Absent optional cores are a clean no-op (stock/MCO ship no mc2xcore.abx). Our Enhanced
	// brain (MC2_BRAIN_*) layers on top via the _ai.fit/_specials.fit loader below — this only
	// provides the native ABL base. Soft-fail (log, no crash) so a missing lib degrades rather
	// than asserting. Trace with MC2_ABL_CORE_TRACE=1.
	static const struct AblCoreLib { const char* name; bool required; } kAblCoreLibs[] = {
		{ "orders",    true  },
		{ "miscfunc",  true  },
		{ "corebrain", false },   // MCO / wolfman-MC2X core
		{ "mc2xcore",  false },   // cveg / MC2X-R52 core
	};
	const bool ablCoreTrace = mc2::gate::get<"MC2_ABL_CORE_TRACE">();
	for (const AblCoreLib& lib : kAblCoreLibs) {
		FullPathFileName libPath;
		libPath.init(missionPath, lib.name, ".abx");
		if (!fileExists(libPath, FILE_ON_DISK | FILE_ON_FST)) {
			if (lib.required)
				mc2::fmt::print_err("[ABL_CORE] WARN: required library {}.abx not found at {}\n", mc2::fmt::safe(lib.name), mc2::fmt::safe(missionPath));
			else if (ablCoreTrace)
				mc2::fmt::print_err("[ABL_CORE] optional library {}.abx absent — skipped\n", mc2::fmt::safe(lib.name));
			continue;
		}
		numErrors = 0;
		ABLModulePtr library = ABLi_loadLibrary(libPath, &numErrors, &numLinesProcessed);
		if (ablCoreTrace || !library || numErrors > 0)
			mc2::fmt::print_err("[ABL_CORE] load {}.abx -> {} (errors={:d})\n", mc2::fmt::safe(lib.name), mc2::fmt::safe(library ? "OK" : "NULL"), mc2::fmt::as_i32(numErrors));
	}
}

// Pick the mission script and build the mission brain.
void Mission::initMissionScript(MissionInitFrame& f) {
	auto& result = f.result; auto& numErrors = f.numErrors; auto& numLinesProcessed = f.numLinesProcessed;
	result = missionFile->seekBlock("Script");
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: mission .fit missing [Script] block");

	result = missionFile->readIdString("ScenarioScript",missionScriptName,79);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1):
	// missionScriptName drives ABLi_preProcess of the scenario .abl below; a failed
	// read leaves it uninitialized (the MC2_BRAIN_INLINE_EMPTY_SKIP fallback below
	// only triggers on an *empty*, not garbage, name).
	MC2_VERIFY(result == NO_ERR, "Mission::init: [Script] missing ScenarioScript");

	// FULL-CAMPAIGN-1: TechScript/Enhanced-converted missions (carver_v_enhanced) leave
	// ScenarioScript empty — the per-warrior brains move to inline Brain{} blocks and mission
	// logic to *_specials.fit — but preserve the original scenario name as LegacyScenarioScript.
	// An empty name builds "<missionPath>/.abl" and ABLi_preProcess STOPs before the mission can
	// load. Fall back to the legacy name so the original scenario ABL (objectives/events) still
	// drives. Gate MC2_BRAIN_INLINE_EMPTY_SKIP (default OFF); stock ScenarioScript is never empty
	// so stock is byte-identical.
	if (missionScriptName[0] == '\0' &&
	    mc2::gate::get<"MC2_BRAIN_INLINE_EMPTY_SKIP">()) {
		char legacyScriptName[80] = {0};
		if (missionFile->readIdString("LegacyScenarioScript", legacyScriptName, 79) == NO_ERR &&
		    legacyScriptName[0] != '\0') {
			strncpy(missionScriptName, legacyScriptName, 79);
			missionScriptName[79] = '\0';
			mc2::fmt::print_err("[MISSIONFIT_OPORD] empty ScenarioScript -> LegacyScenarioScript '{}'\n", mc2::fmt::safe(missionScriptName));
		}
	}

	visualTuning_applyProfile(missionScriptName);  // MISSION-VISUAL-TUNING-1
	terrainMaterials_apply(missionScriptName);     // TERRAIN-MATERIAL-LIB-1: applied AFTER
	                                                // visual_tuning.json so it wins on any
	                                                // overlapping terrain material key.

	FullPathFileName brainFileName;
	brainFileName.init(missionPath, missionScriptName, ".abl");
	
	missionScriptHandle = ABLi_preProcess(brainFileName, &numErrors, &numLinesProcessed);
	gosASSERT(missionScriptHandle >= 0);
	
	missionBrain = new ABLModule;
	gosASSERT(missionBrain != NULL);
		
#ifdef _DEBUG
	long brainErr = 
#endif
		missionBrain->init(missionScriptHandle);
	gosASSERT(brainErr == NO_ERR);

	missionBrain->setName("Mission");

	missionBrainParams = new ABLParam;
	gosASSERT(missionBrainParams != NULL);

	missionBrainCallback = missionBrain->findFunction("handlemessage", TRUE);

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeMissionABLLoad=x1-x;
#endif
}
// Mission load sequence. Each phase is a member helper declared in mission.h, run in the original order; the
// shared locals live in MissionInitFrame. The Tracy zone and the missionFileName char-array reset stay here.
void Mission::init (const char *missionName, long loadType, long dropZoneID, Stuff::Vector3D* dropZoneList, char commandersToLoad[8][3], long numMoversPerCommander)
{
	extern int g_lightProbeSetupPath; g_lightProbeSetupPath = 1; // [GPUPROPS v1]
	ZoneScopedN("Mission::init");
	MissionInitFrame f;
	f.missionName = missionName;
	resetOrderIngress(); // Old tokens cannot cross reinitialization.
	initPrologue(f);
	initTeamRelations(loadType);
	initCoreSystems();
	initGameSystemRanges(f);
	initGameSystemGeneral(f);
	initGameSystemHazards(f);
	memset(missionFileName,0,80);
	strncpy(missionFileName,missionName,79);
	initMissionFile(dropZoneList, f);
	initSkyAndTeamReset(f);
	initTeamsAndCommanders(loadType, dropZoneList, commandersToLoad, f);
	initTriggerAndDropZone(f);
	initMissionSettings(f);
	initTerrain(f);
	initPathfinding(f);
	initAblCoreLibs(f);
	initMissionScript(f);
	initWarriorsHeader(f);
	loadWarriors(f);
	soakAutowinCheck(f);
	loadBrainParamsPhase(f);
	loadMissionFitOpord(f);
	loadBrainRuntime(f);
	loadTacticWeights(f);
	loadBrainDispatch(f);
initPartsHeader(f);
	initPartsLoad(loadType, dropZoneID, dropZoneList, commandersToLoad, numMoversPerCommander, f);
	initObjectCounts(loadType, f);
	initGpuAndLighting();
	initSpawnActors(loadType);
	initTerrainObjectsLoad(commandersToLoad, f);
	initDisableAndObjectives(f);
	initCommanders(commandersToLoad, f);
	bindOrderRecipients(); // Commander groups finalize the actors' runtime roster IDs.
	initWeatherCamera(f);
	initInterface(f);
initLabTimingAndClose();
	initFinalizeGeometry();
}
