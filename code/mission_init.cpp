// Mission: Mission::init, the mission load sequence.
// Declared in the class headers; shared names in mission_internal.h.
#include "mission_internal.h"


// SIM-SPEED-BUTTON-1: dev-shell `sim_speed [--value V]` command — get (no
// value) or set the runtime multiplier through the SAME setter the HUD speed
// tab uses. Registered from Mission::init; inert unless MC2_DEV_SHELL=1.
#include "../GameOS/gameos/gos_dev_shell.h"
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
	sprintf(buf, "{\"multiplier\":%.2f}", (double)simSpeedGetMultiplier());
	dataJsonOut = buf;
	(void)errorOut;
	return true;
}

void Mission::init (const char *missionName, long loadType, long dropZoneID, Stuff::Vector3D* dropZoneList, char commandersToLoad[8][3], long numMoversPerCommander)
{
	extern int g_lightProbeSetupPath; g_lightProbeSetupPath = 1; // [GPUPROPS v1]
	ZoneScopedN("Mission::init");
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

	// DETERMINISTIC-RNG-1: per-mission reseed hook. ONLY under MC2_DETERMINISTIC_RNG.
	// Seed = hash(missionName), overridable with MC2_RNG_SEED for manual pinning.
	// Gate OFF: this block is a no-op (no gos_srand call) => OFF byte-identical.
	{
		const bool detRng = mc2::gate::get<"MC2_DETERMINISTIC_RNG">();
		if (detRng) {
			uint32_t seed = mc2_det_rng::hashMissionName(missionName);
			const std::string seedText = mc2::gate::str<"MC2_RNG_SEED">();   // "" when unset or empty
			if (!seedText.empty())
				seed = (uint32_t)strtoul(seedText.c_str(), nullptr, 0);
			gos_srand(seed);
			std::fprintf(stderr, "[DET_RNG] mission=%s reseed=0x%08X (MC2_DETERMINISTIC_RNG=1)\n",
			             missionName ? missionName : "(null)", seed);
		}
	}

	neverEndingStory = false;
	invulnerableON = false;

	terminationResult = mis_PLAYING; 


	loadProgress = 0.0f;

	loadProgress = 1.0f;

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
	
	FullPathFileName fullGameSystemName;
	fullGameSystemName.init(missionPath, "gamesys", ".fit");
	
	FitIniFile* gameSystemFile = new FitIniFile;
	gosASSERT(gameSystemFile != NULL);
		
	long result = gameSystemFile->open(fullGameSystemName);
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

	long forestMoveCost;
	result = gameSystemFile->readIdLong("ForestMoveCost", forestMoveCost);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: [General] missing ForestMoveCost");

	result = gameSystemFile->readIdFloat("MaxUnitExtractDistance",MaxExtractUnitDistance);
	if (result != NO_ERR)
		MaxExtractUnitDistance = 1280.0f;	//Ten Tiles away
		
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

	memset(missionFileName,0,80);
	strncpy(missionFileName,missionName,79);

	FullPathFileName missionFileName;
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

	long maxTeamID = -1;
	long maxCommanderID = -1;
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
				sprintf(partName,"Part%d",i);

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


	FullPathFileName terrainFileName;
	terrainFileName.init( missionPath, missionName, ".pak" ); 

	PacketFile pakFile;
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
			printf("[MISSION] synthesizing blank MOVE: moveSide=%d (terrainVtx=%d)\n",
			       moveSide, (int)Terrain::realVerticesMapSide);
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

	long numErrors = 0, numLinesProcessed = 0;
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
				std::fprintf(stderr, "[ABL_CORE] WARN: required library %s.abx not found at %s\n", lib.name, missionPath);
			else if (ablCoreTrace)
				std::fprintf(stderr, "[ABL_CORE] optional library %s.abx absent — skipped\n", lib.name);
			continue;
		}
		numErrors = 0;
		ABLModulePtr library = ABLi_loadLibrary(libPath, &numErrors, &numLinesProcessed);
		if (ablCoreTrace || !library || numErrors > 0)
			std::fprintf(stderr, "[ABL_CORE] load %s.abx -> %s (errors=%ld)\n",
			             lib.name, library ? "OK" : "NULL", numErrors);
	}

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
			std::fprintf(stderr, "[MISSIONFIT_OPORD] empty ScenarioScript -> LegacyScenarioScript '%s'\n",
			             missionScriptName);
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

	loadProgress = 41.0f;

	MechWarrior::setup();

	result = missionFile->seekBlock("Warriors");
	// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1)
	MC2_VERIFY(result == NO_ERR, "Mission::init: no [Warriors] block in mission .fit: %s",
		missionName ? missionName : "(null)");

	unsigned long numWarriors;
	result = missionFile->readIdULong("NumWarriors",numWarriors);
	// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1)
	if (!MC2_VERIFY(result == NO_ERR, "Mission::init: [Warriors] NumWarriors read failed"))
		numWarriors = 0;	// log mode: degrade instead of looping over an uninitialized count
	// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1): the
	// 1-based warrior loops below index warriorList[numWarriors] -- a
	// file-supplied count >= MAX_WARRIORS reads past the static array.
	if (!MC2_VERIFY((long)numWarriors < MAX_WARRIORS,
			"Mission::init: NumWarriors %lu exceeds MAX_WARRIORS-1 (%d) -- bounding",
			numWarriors, MAX_WARRIORS - 1))
		numWarriors = MAX_WARRIORS - 1;

	bool loadBrainParameters = (result == NO_ERR);
	// CRASH-SOAK harness: tally how many mission AI brains (e.g. "magicx" on
	// MCO missions) load vs fail, so the soak runner can confirm AI is active.
	long soakBrainsLoaded = 0;
	long soakBrainsFailed = 0;
	if (numWarriors)
	{
		for (long i = 1; i <= numWarriors; i++)
		{
			char warriorName[12];
			sprintf(warriorName,"Warrior%d",i);
			
			result = missionFile->seekBlock(warriorName);
			Assert(result == NO_ERR, i, " Could not find Warrior Number Block ");

			char warriorFile[100];
			result = missionFile->readIdString("Profile", warriorFile, 99);
			Assert(result == NO_ERR, 0, " Could not find Warrior Profile in Warrior Number Block ");
			// QUICKSTART-PILOT-DROP-RECON-1: an empty Profile means the warrior
			// fit never loads and the pilot degrades to the silent "Bubba"
			// default (portrait 0, no radio). Editor re-saves have shipped
			// blanked profiles before — fail LOUD, not silent.
			MC2_VERIFY(warriorFile[0] != '\0',
				"Mission::init: %s has EMPTY Profile in %s — pilot will default to Bubba (blanked by an editor re-save?)",
				warriorName, missionName);

			MechWarriorPtr pilot = MechWarrior::newWarrior();
			if (!pilot)
				STOP(("Too many pilots in this mission!"));
			
			FullPathFileName pilotFullFileName;
			pilotFullFileName.init(warriorPath, warriorFile, ".fit");
			
			FitIniFile* pilotFile = new FitIniFile;
			gosASSERT(pilotFile != NULL);
		
			long result = pilotFile->open(pilotFullFileName);
			// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: warrior profile open failed: %s",
				(const char*)pilotFullFileName);
			result = pilot->init(pilotFile);
			// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: warrior init failed: %s",
				(const char*)pilotFullFileName);
			
			pilotFile->close();
			delete pilotFile;
			pilotFile = NULL;
			
			char moduleName[128];
			result = missionFile->readIdString("Brain", moduleName, 127);
			// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: %s missing Brain key", warriorName);
			
			//------------------------------------------------------------
			// For now, all mplayer brains are pbrain. Need to change when
			// we allow ai brains in mplayer...
			long numErrors, numLinesProcessed;
			FullPathFileName brainFileName;
			if (MPlayer) {
				pilot->setBrainName("pbrain");
				brainFileName.init(warriorPath, "pbrain", ".abl");
				}
			else {
				pilot->setBrainName(moduleName);
				brainFileName.init(warriorPath, moduleName, ".abl");
			}
			
			// BRAIN-EMPTY-BRAIN-SKIP-1: inline Brain{} (TechScript/Enhanced) missions leave the
			// warrior "Brain" field empty — the brain lives in the mission.fit Brain{} block, not a
			// .abl file. Without a guard the empty name builds "warriors/.abl" and ABLi_preProcess
			// STOPs (FATAL: missing ABL), so the mission never reaches the inline-Brain consumers
			// (e.g. BRAIN-MISSIONFIT-OPORD-CONSUMER-1 below). Gate MC2_BRAIN_INLINE_EMPTY_SKIP
			// (default OFF). Stock missions always name a brain, so stock is byte-identical — the
			// guard only fires on an empty, non-MPlayer name.
			static const bool inlineEmptySkip =
				mc2::gate::get<"MC2_BRAIN_INLINE_EMPTY_SKIP">();
			if (inlineEmptySkip && !MPlayer && moduleName[0] == '\0')
			{
				pilot->setBrainName("");
			}
			else
			{
				long moduleHandle = ABLi_preProcess(brainFileName, &numErrors, &numLinesProcessed);
				gosASSERT(moduleHandle >= 0);
				if (moduleHandle >= 0 && numErrors == 0)
					soakBrainsLoaded++;
				else
				{
					soakBrainsFailed++;
					printf("[SOAK] WARN abl brain load failed name=%s errors=%ld\n",
						moduleName, (long)numErrors);
				}

#ifdef _DEBUG
				long error =
#endif
					pilot->setBrain(moduleHandle);
				gosASSERT(error == 0);
			}
		}
	}
	if (mc2::gate::get<"MC2_SOAK_AUTOWIN">() || soakBrainsFailed > 0)
	{
		printf("[SOAK] abl mission brains loaded=%ld failed=%ld\n",
			soakBrainsLoaded, soakBrainsFailed);
		fflush(stdout);
	}

	if (loadBrainParameters) {
		//---------------------------------------------------------------
		// Load the brain parameter file and load 'em for each warrior...
		// BRAIN-EMPTY-BRAIN-SKIP-1: inline-Brain warriors carry no ABL brain (skipped above),
		// so loadBrainParameters would Fatal(0,"NULL brain"). Skip them under the same gate.
		static const bool inlineEmptySkipParams =
			mc2::gate::get<"MC2_BRAIN_INLINE_EMPTY_SKIP">();
		for (unsigned long i = 1; i <= numWarriors; i++) {
			if (inlineEmptySkipParams && MechWarrior::warriorList[i] &&
			    !MechWarrior::warriorList[i]->getBrain())
				continue;
			// MC2_VERIFY reclassified from gosASSERT (slice MC2-VERIFY-LIVE-1):
			// warriorList[i] is NULL when newWarrior() ran out of slots above
			// (the "Too many pilots" STOP continues in release) -- null-deref.
			if (!MC2_VERIFY(MechWarrior::warriorList[i] != NULL,
					"Mission::init: warriorList[%lu] is NULL (loadBrainParameters)", i))
				continue;	// log mode: skip the missing warrior
			result = MechWarrior::warriorList[i]->loadBrainParameters(missionFile, i);
		}

	}

	// BRAIN-MISSIONFIT-OPORD-CONSUMER-1: drive declarative patrol from the inline Brain{}
	// PrimaryOPORD blocks in mission.fit (carver_v_enhanced format). FitIniFile cannot see
	// brace-blocks, so raw-scan the mission.fit text via the File class (VFS/mod-overlay aware —
	// NOT fopen). Gate MC2_BRAIN_MISSIONFIT_OPORD (default OFF). Populates the existing
	// MechBrainRuntime patrol structures (BRAIN-OPORD-COREPATROL-1); the brain bundle drives them
	// (the patrol FIELDS persist through later _ai.fit/tactic loaders, which only touch mode).
	if (mc2::gate::get<"MC2_BRAIN_MISSIONFIT_OPORD">()) {
		File mfFile;
		if (mfFile.open(missionFileName) == NO_ERR) {
			unsigned long mfSz = mfFile.getLength();
			char* mfBuf = (char*)malloc(mfSz + 1);
			if (mfBuf) {
				mfFile.read((MemoryPtr)mfBuf, (long)mfSz);
				mfBuf[mfSz] = '\0';
				MissionFitBrain* mfBrains = (MissionFitBrain*)malloc(sizeof(MissionFitBrain) * 128);
				int mfN = mfBrains ? parseMissionFitBrains(mfBuf, mfBrains, 128) : 0;
				int mfPatrol = 0, mfGuard = 0, mfSentry = 0, mfEscort = 0, mfOther = 0;
				for (int r = 0; r < mfN; ++r) {
					const MissionFitBrain& b = mfBrains[r];
					if (b.warriorIndex < 1 || (unsigned long)b.warriorIndex > numWarriors) continue;
					uint8_t pType = brainOpordTypeId(b.primary.type);
					BrainArchetypeDefaults ad; brainArchetypeLookup(b.archetype, ad);  // archetype presets; switches override
					if (ad.playerControlled || pType == 9 || pType == 255) continue;   // player / unknown -> default path
					MechWarriorPtr w = MechWarrior::warriorList[b.warriorIndex];
					if (!w) continue;
					if (!w->getBrainRuntime()) w->setBrainRuntimeMode(BrainRuntimeMode::Enhanced);
					MechBrainRuntime* rt = w->getBrainRuntime();
					if (!rt) continue;
					// EngageRadius = per-unit COMMITMENT radius: the team SHARES the contact (all know the
					// enemy exists) but a unit only attacks once the threat is within its own EngageRadius,
					// so distant units keep patrolling/holding instead of the whole map swarming one sighting.
					rt->engageRadius         = (b.engageRadius >= 0) ? b.engageRadius : ad.engageRadius;
					rt->swAttackerHelpRadius = (b.attackerHelpRadius >= 0) ? b.attackerHelpRadius : ad.attackerHelpRadius;
					rt->swDefenderHelpRadius = (b.defenderHelpRadius >= 0) ? b.defenderHelpRadius : ad.defenderHelpRadius;
					rt->swRequestHelp        = (b.requestHelp  >= 0) ? (int8_t)b.requestHelp  : (int8_t)ad.requestHelp;
					rt->swReturnToPost       = (b.returnToPost >= 0) ? (int8_t)b.returnToPost : (int8_t)ad.returnToPost;
					rt->swWakeOnAttack       = (int8_t)b.wakeOnAttack;
					rt->swPoweredDown        = (int8_t)b.poweredDown;
					rt->opordType[0] = pType;
					rt->opordType[1] = brainOpordTypeId(b.secondary.type);
					rt->opordType[2] = brainOpordTypeId(b.tertiary.type);
					rt->opordCursor  = 0;
					// Load weighted Tactics (MC2_TACTIC_WEIGHTS selects per tick; _WEIGHTS_B applies to attacks).
					for (int ti = 0; ti < b.tacticCount; ++ti) {
						int tidx = brainTacticNameToIndex(b.tacticName[ti]);
						if (tidx >= 0 && tidx < 12) rt->tacticWeights[tidx] = b.tacticWeight[ti];
					}
					if (w->getVehicle()) {
						Stuff::Vector3D vp = w->getVehicle()->getPosition();
						rt->postPos[0] = vp.x; rt->postPos[1] = vp.y; rt->postPos[2] = vp.z; rt->postSet = true;
					}
					switch (pType) {
						case 0: {   // Patrol
							int wc = (b.primary.waypointCount > 8) ? 8 : b.primary.waypointCount;
							if (wc <= 0) { rt->guardHold = true; ++mfGuard; break; }
							for (int wi = 0; wi < wc; ++wi) {
								Stuff::Vector3D wp; wp.x = b.primary.waypoints[wi].x; wp.y = b.primary.waypoints[wi].y; wp.z = 0.0f;
								float z = land ? land->getTerrainElevation(wp) : 0.0f;
								rt->patrolWaypoints[wi][0] = b.primary.waypoints[wi].x;
								rt->patrolWaypoints[wi][1] = b.primary.waypoints[wi].y;
								rt->patrolWaypoints[wi][2] = z;
							}
							rt->patrolWaypointCount = (uint8_t)wc;
							rt->patrolWaypointIndex = 0;
							rt->patrolLoop   = b.primary.loop;
							rt->patrolActive = true;
							++mfPatrol; break;
						}
						case 1:     // Guard
							rt->guardHold = true; ++mfGuard; break;
						case 3:     // Sentry
							rt->guardHold = true; rt->sentryAsleep = (rt->swPoweredDown == 1); ++mfSentry; break;
						case 4: {   // Escort
							int ei = b.primary.escortTargetIndex;
							if (ei >= 1 && (unsigned long)ei <= numWarriors && MechWarrior::warriorList[ei] &&
							    MechWarrior::warriorList[ei]->getVehicle())
								rt->escortTargetWID = MechWarrior::warriorList[ei]->getVehicle()->getWatchID();
							rt->escortMoving = true; rt->guardHold = true; ++mfEscort; break;
						}
						case 2: {   // MoveTo - move to destination once, then hold + engage
							int wc = (b.primary.waypointCount > 8) ? 8 : b.primary.waypointCount;
							if (wc > 0) {
								for (int wi = 0; wi < wc; ++wi) {
									Stuff::Vector3D wp; wp.x = b.primary.waypoints[wi].x; wp.y = b.primary.waypoints[wi].y; wp.z = 0.0f;
									float z = land ? land->getTerrainElevation(wp) : 0.0f;
									rt->patrolWaypoints[wi][0] = b.primary.waypoints[wi].x;
									rt->patrolWaypoints[wi][1] = b.primary.waypoints[wi].y;
									rt->patrolWaypoints[wi][2] = z;
								}
								rt->patrolWaypointCount = (uint8_t)wc;
								rt->patrolWaypointIndex = 0;
								rt->patrolLoop = false;   // move once to destination, do not loop
								rt->patrolActive = true;
							}
							rt->guardHold = true; ++mfOther; break;
						}
						case 7:     // Attack - aggressive: engage any detected enemy (no EngageRadius limit)
							rt->guardHold = true; rt->engageRadius = 0.0f; ++mfOther; break;
						case 8: {   // Withdraw - move to rally point (primary waypoint) once, then hold
							int wc = (b.primary.waypointCount > 8) ? 8 : b.primary.waypointCount;
							if (wc > 0) {
								for (int wi = 0; wi < wc; ++wi) {
									Stuff::Vector3D wp; wp.x = b.primary.waypoints[wi].x; wp.y = b.primary.waypoints[wi].y; wp.z = 0.0f;
									float z = land ? land->getTerrainElevation(wp) : 0.0f;
									rt->patrolWaypoints[wi][0] = b.primary.waypoints[wi].x;
									rt->patrolWaypoints[wi][1] = b.primary.waypoints[wi].y;
									rt->patrolWaypoints[wi][2] = z;
								}
								rt->patrolWaypointCount = (uint8_t)wc;
								rt->patrolWaypointIndex = 0;
								rt->patrolLoop = false;   // move once to destination, do not loop
								rt->patrolActive = true;
							}
							rt->guardHold = true; ++mfOther; break;
						}
						case 5:     // Ambush - hidden until a threat is near, then strike (reuses sentry asleep/woken)
							rt->guardHold = true; rt->sentryAsleep = true; ++mfOther; break;
						case 6: {   // Scout - move the route observing only; never engages
							int wc = (b.primary.waypointCount > 8) ? 8 : b.primary.waypointCount;
							if (wc > 0) {
								for (int wi = 0; wi < wc; ++wi) {
									Stuff::Vector3D wp; wp.x = b.primary.waypoints[wi].x; wp.y = b.primary.waypoints[wi].y; wp.z = 0.0f;
									float z = land ? land->getTerrainElevation(wp) : 0.0f;
									rt->patrolWaypoints[wi][0] = b.primary.waypoints[wi].x;
									rt->patrolWaypoints[wi][1] = b.primary.waypoints[wi].y;
									rt->patrolWaypoints[wi][2] = z;
								}
								rt->patrolWaypointCount = (uint8_t)wc;
								rt->patrolWaypointIndex = 0;
								rt->patrolLoop = b.primary.loop;
								rt->patrolActive = true;
							}
							rt->scoutObserveOnly = true; ++mfOther; break;
						}
						default:    // MoveTo / Attack / Withdraw / Ambush / Scout -> hold + engage (verb wiring TBD)
							rt->guardHold = true; ++mfOther; break;
					}
				}
				std::fprintf(stderr, "[MISSIONFIT_OPORD] parsed %d Brain{} record(s): patrol=%d guard=%d sentry=%d escort=%d other=%d\n",
					mfN, mfPatrol, mfGuard, mfSentry, mfEscort, mfOther);
				if (mfBrains) free(mfBrains);
				free(mfBuf);
			}
			mfFile.close();
		}
	}

	// BRAIN-RUNTIME-1B: load per-unit Brain blocks from <missionName>_ai.fit.
	// Gate: MC2_BRAIN_RUNTIME=1 (default OFF). File absence is silent (no fixture = all Legacy).
	// Single-warrior fixture only: one Brain block keyed "Brain". unitRef "Warrior%d" -> idx.
	// MC2_BRAIN_RUNTIME_FORCE_MODE (if set) overrides all per-unit modes as a post-load global.
	if (mc2::gate::get<"MC2_BRAIN_RUNTIME">()) {
		char aiFitName[256];
		std::snprintf(aiFitName, sizeof(aiFitName), "%s%s_ai.fit", missionPath, missionName);
		std::fprintf(stderr, "[BRAIN_RT] mission load: MC2_BRAIN_RUNTIME=1 mission=%s seeking %s\n", missionName, aiFitName);
		std::fflush(stderr);
		FitIniFile* aiFit = new FitIniFile;
		if (aiFit && aiFit->open(aiFitName) == NO_ERR) {
			if (aiFit->seekBlock("Brain") == NO_ERR) {
				char unitRefBuf[32] = {};
				long modeValLong = 0;
				long unitRefResult  = aiFit->readIdString("unitRef",  unitRefBuf, 31);
				long modeResult     = aiFit->readIdLong("mode", modeValLong);
				if (unitRefResult == NO_ERR && modeResult == NO_ERR) {
					int warriorIdx = -1;
					std::sscanf(unitRefBuf, "Warrior%d", &warriorIdx);
					if (warriorIdx >= 1 && warriorIdx <= (int)numWarriors) {
						MechWarriorPtr w = MechWarrior::warriorList[warriorIdx];
						if (w) {
							BrainRuntimeMode fitMode = BrainRuntimeMode::Legacy;
							switch (modeValLong) {
								case 1: fitMode = BrainRuntimeMode::Hybrid;   break;
								case 2: fitMode = BrainRuntimeMode::Enhanced; break;
								default: break;
							}
							w->setBrainRuntimeMode(fitMode);
							std::fprintf(stderr, "[BRAIN_RT] FIT load warriorIdx=%d unitRef=%s mode=%s\n",
								warriorIdx,
								unitRefBuf,
								MechBrainRuntime::modeString(fitMode));
							std::fflush(stderr);
						}
					} else {
						std::fprintf(stderr, "[BRAIN_RT] WARNING: _ai.fit Brain block unitRef=%s idx=%d out of range [1..%lu]\n",
							unitRefBuf, warriorIdx, (unsigned long)numWarriors);
					}
				}
			}
			aiFit->close();
		}
		// MC2_BRAIN_RUNTIME_FORCE_MODE overrides all per-unit modes (post-load global override).
		if (mc2::gate::isSet<"MC2_BRAIN_RUNTIME_FORCE_MODE">()) {
			BrainRuntimeMode forced = BrainRuntimeMode::Legacy;   // "legacy" and any other text force Legacy
			if      (mc2::gate::is<"MC2_BRAIN_RUNTIME_FORCE_MODE", "hybrid">())   forced = BrainRuntimeMode::Hybrid;
			else if (mc2::gate::is<"MC2_BRAIN_RUNTIME_FORCE_MODE", "enhanced">()) forced = BrainRuntimeMode::Enhanced;
			for (unsigned long i = 1; i <= numWarriors; i++) {
				MechWarriorPtr w = MechWarrior::warriorList[i];
				if (w && w->getBrainRuntime())
					w->setBrainRuntimeMode(forced);
			}
		}
		delete aiFit;
	}

	// TACTIC-WEIGHTS-A: load [Tactics] block from <missionName>_ai.fit into tacticWeights[].
	// Gate: MC2_TACTIC_WEIGHTS=1 (default OFF).
	// File absence is silent. Block absence is silent (all weights stay zero).
	// Format: one float entry per tactic by short name, e.g. "f StopAndFire = 0.30"
	// Normalisation is enforced at load time (sum→1); weightsNormalized flag set.
	// Both raw-zero and absent-block warriors keep tacticWeights all-zero (FIT fallback
	// uses uniform weights via the all-zero branch in applyPilotModulation).
	if (mc2::gate::get<"MC2_TACTIC_WEIGHTS">()) {
		char tacFitName[256];
		std::snprintf(tacFitName, sizeof(tacFitName), "%s%s_ai.fit", missionPath, missionName);
		FitIniFile* tacFit = new FitIniFile;
		if (tacFit && tacFit->open(tacFitName) == NO_ERR) {
			if (tacFit->seekBlock("Tactics") == NO_ERR) {
				struct { const char* name; int idx; } kTacticKeys[] = {
					{ "None",             TACTIC_NONE             },
					{ "FlankLeft",        TACTIC_FLANK_LEFT       },
					{ "FlankRight",       TACTIC_FLANK_RIGHT      },
					{ "FlankRear",        TACTIC_FLANK_REAR       },
					{ "StopAndFire",      TACTIC_STOP_AND_FIRE    },
					{ "Turret",           TACTIC_TURRET           },
					{ "Joust",            TACTIC_JOUST            },
					{ "IndirectFire",     TACTIC_INDIRECT_FIRE    },
					{ "HullDown",         TACTIC_HULL_DOWN        },
					{ "FightingWithdraw", TACTIC_FIGHTING_WITHDRAW},
					{ "Pursue",           TACTIC_PURSUE           },
					{ "HitAndRun",        TACTIC_HIT_AND_RUN      },
				};
				float loadedWeights[NUM_TACTICS] = {};
				for (auto& kv : kTacticKeys) {
					float val = 0.0f;
					if (tacFit->readIdFloat(kv.name, val) == NO_ERR)
						loadedWeights[kv.idx] = (val < 0.0f) ? 0.0f : val;
				}
				float sum = 0.0f;
				for (int i = 0; i < NUM_TACTICS; i++) sum += loadedWeights[i];
				if (sum > 0.0f) {
					float inv = 1.0f / sum;
					for (int i = 0; i < NUM_TACTICS; i++) loadedWeights[i] *= inv;
				}
				int loadCount = 0;
				for (unsigned long i = 1; i <= numWarriors; i++) {
					MechWarriorPtr w = MechWarrior::warriorList[i];
					if (w) {
						if (!w->getBrainRuntime()) {
							w->setBrainRuntimeMode(BrainRuntimeMode::Legacy);
						}
						MechBrainRuntime* rt = w->getBrainRuntime();
						if (rt) {
							std::memcpy(rt->tacticWeights, loadedWeights, sizeof(rt->tacticWeights));
							rt->weightsNormalized = 1;
							loadCount++;
						}
					}
				}
				std::fprintf(stderr, "[BRAIN_TACTIC_SELECT] [Tactics] loaded from %s: %d warriors weighted\n",
				             tacFitName, loadCount);
				std::fflush(stderr);
			}
			tacFit->close();
		}
		delete tacFit;
	}

	// TECHSCRIPT-SPECIAL-DISPATCH-1A: load per-warrior BrainSpecial body from <missionName>_specials.fit.
	// Gate: MC2_BRAIN_DISPATCH=1 (default OFF). Requires MC2_BRAIN_RUNTIME=1 + MC2_BRAIN_RUNTIME_APPLY=1.
	// File absence is silent. Only warriors with brainRuntime allocated (Enhanced mode) are loaded.
	//
	// TECHSCRIPT-SPECIAL-DISPATCH-1C: gate MC2_BRAIN_DISPATCH_FSM_TODO=1 (requires MC2_BRAIN_DISPATCH=1).
	// Runs a MISSION-LEVEL raw-text scan of the specials file for
	// "; TODO: manual ABL line: <payload>" comments (stripped by FitIniFile).
	// Emits [BRAIN_DISPATCH_FSM_TODO] summary + detail lines. Information only — no behavior change.
	// Scan is done once per mission (not per-warrior) because the file is mission-level.
	// Gate-OFF: byte-identical to pre-1C; no extra parsing, no extra traces.
	{
		const bool dispatchOn   = mc2::gate::get<"MC2_BRAIN_DISPATCH">();
		const bool fsmTodoOn    = mc2::gate::get<"MC2_BRAIN_DISPATCH_FSM_TODO">();
		const bool dispatchVarOn = mc2::gate::get<"MC2_BRAIN_DISPATCH_VAR">();
		const bool missionVarOn  = mc2::gate::get<"MC2_BRAIN_VAR_MISSION">();

		// GAP-A: reset the mission-level specials cache at every mission load (unconditional;
		// produces no trace, so gate-OFF runs stay byte-identical). Prevents a stale cache
		// from a prior dispatch-ON mission leaking into a later one.
		resetMissionSpecialCache();

		// TECHSCRIPT-DISPATCH-1D-M: reset mission-scope Var store at every mission load.
		// resetMissionVarStore() is always safe to call (no-op if store already empty).
		// Gate: only reset when MC2_BRAIN_VAR_MISSION=1 to avoid noise in gate-OFF runs.
		if (missionVarOn) {
			resetMissionVarStore();
		}

		if (fsmTodoOn && !dispatchOn) {
			std::fprintf(stderr, "[BRAIN_DISPATCH_FSM_TODO] WARN: MC2_BRAIN_DISPATCH_FSM_TODO=1 requires MC2_BRAIN_DISPATCH=1 — inert\n");
			std::fflush(stderr);
		}
		if (dispatchVarOn && !dispatchOn) {
			std::fprintf(stderr, "[BRAIN_DISPATCH_VAR] WARNING: MC2_BRAIN_DISPATCH_VAR=1 requires MC2_BRAIN_DISPATCH=1 -- var handling is INERT\n");
			std::fflush(stderr);
		}

		if (dispatchOn) {
			// DISPATCH-EFFECT-UNITEJECT-1: MC2_BRAIN_SPECIAL_FIT allows test-fixture override.
			// When set, ALL warriors load from <override>_specials.fit instead of <mission>_specials.fit.
			// Intended for smoke gate verification only. Default: missionName (standard behavior).
			const std::string specialFitText = mc2::gate::str<"MC2_BRAIN_SPECIAL_FIT">();   // "" when unset or empty
			const char* specialFitName = specialFitText.empty() ? missionName : specialFitText.c_str();

			std::fprintf(stderr, "[BRAIN_DISPATCH] mission load: MC2_BRAIN_DISPATCH=1 mission=%s special_fit=%s\n",
			             missionName, specialFitName);
			std::fflush(stderr);

			// 1A/1B: per-warrior verb parse (only for warriors with brainRuntime allocated).
			// TECHSCRIPT-CALL-CHAIN-1A: pass specialIndex so the raw scanner populates it.
			// The index is per-warrior-runtime (each warrior gets its own copy); content
			// is identical across warriors for the same mission (all read the same file).
			// This is acceptable for 1A: index is small (<10 blocks), mission-ephemeral.
			//
			// GAP-A MULTI-WARRIOR SPECIALS: parse mission-level specials body ONCE into the
			// dispatch cache. Warriors whose brainRuntime is allocated lazily in runBrain
			// (not named in _ai.fit) copy from this cache on first tick, so every warrior
			// dispatches -- not just the _ai.fit-recorded ones.
			cacheMissionSpecialBody(specialFitName);
			for (unsigned long i = 1; i <= numWarriors; i++) {
				MechWarriorPtr w = MechWarrior::warriorList[i];
				if (w && w->getBrainRuntime()) {
					parseBrainSpecialBody(specialFitName, w->getBrainRuntime()->specialBody,
					                      &w->getBrainRuntime()->specialIndex);
				}
			}

			// TECHSCRIPT-SPECIAL-DISPATCH-1C: FSM-TODO mission-level scan + trace.
			// Done ONCE per mission (the specials file is mission-level, not per-warrior).
			// Gate-OFF: this entire block is skipped; no ifstream open, no extra traces.
			if (fsmTodoOn) {
				// Use a temporary BrainSpecialBody for the scan — trace output is mission-level.
				// fsmTodos are INFORMATION ONLY; they are not stored persistently here.
				// (Future per-warrior storage is deferred — the file has one FSM skeleton shared
				// by all warriors; per-warrior active-state gating is out of 1C scope.)
				BrainSpecialBody tmpBody;
				scanFsmTodosFromFile(specialFitName, tmpBody);

				int nStateDef = 0, nStateEnd = 0, nTrans = 0, nTransBack = 0, nOther = 0;
				for (const FsmTodoEntry& e : tmpBody.fsmTodos) {
					switch (e.kind) {
						case FsmTodoKind::STATE_DEF:  ++nStateDef;  break;
						case FsmTodoKind::STATE_END:  ++nStateEnd;  break;
						case FsmTodoKind::TRANS:      ++nTrans;     break;
						case FsmTodoKind::TRANS_BACK: ++nTransBack; break;
						case FsmTodoKind::OTHER_TODO: ++nOther;     break;
					}
				}

				// Summary line (always emitted when FSM_TODO gate is on, even if count=0).
				// wid=-1 signals mission-level (no specific warrior context).
				std::fprintf(stderr,
					"[BRAIN_DISPATCH_FSM_TODO] mission=%s wid=-1 stateDefs=%d transitions=%d otherTodos=%d\n",
					missionName, nStateDef, nTrans + nTransBack, nOther);
				std::fflush(stderr);

				// Detail lines: STATE_DEF, TRANS, TRANS_BACK, STATE_END only.
				// OTHER_TODO is counted in summary but NOT detail-traced (noise suppression).
				for (const FsmTodoEntry& e : tmpBody.fsmTodos) {
					switch (e.kind) {
						case FsmTodoKind::STATE_DEF:
							std::fprintf(stderr,
								"[BRAIN_DISPATCH_FSM_TODO] kind=STATE_DEF name=%s wid=-1\n",
								e.name.c_str());
							std::fflush(stderr);
							break;
						case FsmTodoKind::STATE_END:
							std::fprintf(stderr,
								"[BRAIN_DISPATCH_FSM_TODO] kind=STATE_END wid=-1\n");
							std::fflush(stderr);
							break;
						case FsmTodoKind::TRANS:
							std::fprintf(stderr,
								"[BRAIN_DISPATCH_FSM_TODO] kind=TRANS target=%s wid=-1\n",
								e.name.c_str());
							std::fflush(stderr);
							break;
						case FsmTodoKind::TRANS_BACK:
							std::fprintf(stderr,
								"[BRAIN_DISPATCH_FSM_TODO] kind=TRANS_BACK wid=-1\n");
							std::fflush(stderr);
							break;
						case FsmTodoKind::OTHER_TODO:
							break;
					}
				}
			} // fsmTodoOn
		} // dispatchOn
	}

#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeWarriorLoad=x-x1;
#endif

	loadProgress = 43.0f;

	result = missionFile->seekBlock("Parts");
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1):
	// the SP object-instantiation pass; a missing [Parts] block leaves numParts
	// uninitialized and sizes the parts[] Malloc below off garbage.
	MC2_VERIFY(result == NO_ERR, "Mission::init: mission .fit missing [Parts] block (load pass)");

	result = missionFile->readIdULong("NumParts",numParts);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1):
	// numParts sizes missionHeap->Malloc(sizeof(Part)*(numParts+1)) below and
	// bounds every part loop; a failed read leaves it uninitialized.
	MC2_VERIFY(result == NO_ERR, "Mission::init: [Parts] NumParts read failed (load pass)");

	//--------------------------------------------------------------------------------
	// IMPORTANT NOTE: mission parts should always start with Part 1.
	// Part 0 is reserved as a "NULL" id for routines that reference the mission
	// parts. AI routines, Brain keywords, etc. use PART ID 0 as an "object not found"
	// error code. DO NOT USE PART 0!!!!!!! Start with Part 1...

#define MAX_SQUADS			256
#define	MAX_ALTERNATIVES	15
#define	USE_ALTERNATES

	loadProgress = 43.5f;

	long numMoversLoaded[MAX_MC_PLAYERS] = {0, 0, 0, 0, 0, 0, 0, 0};
	long numDropZonePositions = 0;
	if (numParts)
	{
		//-----------------------------------------------------
		// Since we leave part 0 unused, malloc numParts + 1...
		parts = (PartPtr)missionHeap->Malloc(sizeof(Part) * (numParts + 1));
		gosASSERT(parts != NULL);
		
		memset(parts,0,sizeof(Part) * (numParts + 1));

#ifdef USE_ALTERNATES
		long numSquads = 0;
		long squadMap[MAX_SQUADS];
		for (long i = 0; i < MAX_SQUADS; i++)
			squadMap[i] = -1;
		long maxAlternatives[MAX_SQUADS];
		long randomAlternative[MAX_SQUADS];
		for (long s = 0; s < MAX_SQUADS; s++) {
			maxAlternatives[s] = 0;
			randomAlternative[s] = -1;
		}

		long bigAlternatives = 0;

		for (int i = 1; i < long(numParts + 1); i++) {
			char partName[12];
			sprintf(partName,"Part%d",i);

			result = missionFile->seekBlock(partName);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1):
			// alternatives-prep pass; NumParts promised this Part block.
			MC2_VERIFY(result == NO_ERR, "Mission::init: mission .fit missing [%s] block (alt-prep pass)", partName);
			unsigned long squadNum;
			result = missionFile->readIdULong("squadNum", squadNum);
			long squadIndex = 0;
			for (squadIndex = 0; squadIndex < numSquads; squadIndex++)
				if (squadMap[squadIndex] == squadNum)
					break;
			if (squadIndex == numSquads)
				squadMap[numSquads++] = squadNum;

			long alternatives[MAX_ALTERNATIVES];
			result = missionFile->readIdLongArray("IndicesOfAlternatives", alternatives, MAX_ALTERNATIVES);
			long numAlternatives = 0;
			for (numAlternatives = 0; numAlternatives < MAX_ALTERNATIVES; numAlternatives++)
				if (alternatives[numAlternatives] == -1)
					break;
			if (maxAlternatives[squadIndex] < numAlternatives)
				maxAlternatives[squadIndex] = numAlternatives;

			if (numAlternatives > bigAlternatives)
				bigAlternatives = numAlternatives;
		}

		if (MPlayer && (bigAlternatives > 0)) {
			PAUSE(("Mission.init: multiplayer map has random squads"));
			bigAlternatives = 0;
		}

		long alternateChoice = RandomNumber(bigAlternatives + 1);
		for (int s = 0; s < numSquads; s++)
		{
			randomAlternative[s] = alternateChoice;
			if (GameDifficulty >= 2)
			{
				randomAlternative[s]--;
				if (randomAlternative[s] < 1)
					randomAlternative[s] = 1;
			}
		}

		//--------------------------------------------------------
		// This block is optional, and is used for testing only...
		result = missionFile->seekBlock("Squads");
		if (result == NO_ERR)
			for (long i = 0; i < numSquads; i++) {
				char s[128];
				sprintf(s, "Squad%d", i);
				unsigned long alternate = -1;
				result = missionFile->readIdULong(s, alternate);
				if (result == NO_ERR)
					randomAlternative[i] = alternate;
			}
#else
		long i;
#endif

		for (int i = 1; i < long(numParts + 1); i++)
		{
			char partName[12];
			sprintf(partName,"Part%d",i);
			
			result = missionFile->seekBlock(partName);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1):
			// NumParts promised this Part block; a missing block leaves parts[i]
			// zeroed and spawns a garbage object type at loadObjectType(objNumber).
			MC2_VERIFY(result == NO_ERR, "Mission::init: mission .fit missing [%s] block", partName);

#ifdef USE_ALTERNATES
			//----------------------------------------------------------------------
			// If we have alternatives, choose which one we're taking before we read
			// anything else in...
			bool usingAlternate = false;
			unsigned long realPilot = 0;
			unsigned long squadNum;
			result = missionFile->readIdULong("squadNum", squadNum);
			parts[i].squadId = squadNum;
			long squadIndex = 0;
			for (squadIndex = 0; squadIndex < numSquads; squadIndex++)
				if (squadMap[squadIndex] == squadNum)
					break;

			long alternatives[MAX_ALTERNATIVES];
			result = missionFile->readIdLongArray("IndicesOfAlternatives", alternatives, MAX_ALTERNATIVES);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing IndicesOfAlternatives", partName);
			if (maxAlternatives[squadIndex]) 
			{
				long partId = i;
				if (randomAlternative[squadIndex] > 0)
					partId = alternatives[randomAlternative[squadIndex] - 1];
				if (partId == -1)
					continue;
				Assert(partId > 0, partId, " Mission.init: Bad Alternate ");

				//MUST save off ORIGINAL Pilot.  WE don't load the alternate pilots!!!!!
				usingAlternate = true;
				result = missionFile->readIdULong("Pilot", realPilot);
				// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
				MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing Pilot (alternate)", partName);

				sprintf(partName, "Part%d", partId);
				result = missionFile->seekBlock(partName);
				// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
				MC2_VERIFY(result == NO_ERR, "Mission::init: alternate part block [%s] missing", partName);
			}
#endif

			result = missionFile->readIdULong("ObjectNumber",parts[i].objNumber);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1):
			// objNumber feeds loadObjectType() below; a failed read leaves it
			// uninitialized-then-used to spawn the wrong/garbage object type.
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing ObjectNumber", partName);

			result = missionFile->readIdULong("ControlType", parts[i].controlType);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing ControlType", partName);

			result = missionFile->readIdULong("ControlDataType", parts[i].controlDataType);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing ControlDataType", partName);

			result = missionFile->readIdString("ObjectProfile", parts[i].profileName, 9);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing ObjectProfile", partName);

			// Mod-tolerance: read CSVFile so we can back-fill the ObjectType
			// appearance name when the pak lookup fails for a high ObjectNumber.
			parts[i].csvFile[0] = 0;
			missionFile->readIdString("CSVFile", parts[i].csvFile, sizeof(parts[i].csvFile) - 1);

			result = missionFile->readIdULong("VariantNumber", parts[i].variantNum);
			if (result != NO_ERR)
				parts[i].variantNum = 0;		//FOR NOW!!!!!!!!!!!!!!!!

			if (usingAlternate)
			{
				parts[i].pilot = realPilot;
			}
			else
			{
				result = missionFile->readIdULong("Pilot", parts[i].pilot);
				// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
				MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing Pilot", partName);
			}

			result = missionFile->readIdFloat("PositionX",parts[i].position.x);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing PositionX", partName);

			result = missionFile->readIdFloat("PositionY",parts[i].position.y);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing PositionY", partName);

			parts[i].position.z = -1.0;

			result = missionFile->readIdFloat("Rotation",parts[i].rotation);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing Rotation", partName);

			result = missionFile->readIdChar("TeamId",parts[i].teamId);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing TeamId", partName);
			//--------------------------------------------------------------------------
			// Hack for singleplayer, until editor spits this out properly for allies...
			if (!MPlayer && (parts[i].teamId == 2))
				parts[i].teamId = 0;
			
			if (dropZoneList && (dropZoneID == parts[i].teamId))
				dropZoneList[numDropZonePositions++] = parts[i].position;

			char cmdId = 0;
			result = missionFile->readIdChar("CommanderId", cmdId);
			if (result != NO_ERR)
			{
				result = missionFile->readIdLong("CommanderId", parts[i].commanderID);
				gosASSERT(result == NO_ERR);
			}
			else
			{
				parts[i].commanderID = cmdId;
			}

			if (loadType == MISSION_LOAD_MP_QUICKSTART) {
				long origCommanderID = parts[i].commanderID;
				parts[i].commanderID = commandersToLoad[origCommanderID][0];
				parts[i].teamId = commandersToLoad[origCommanderID][1];
				if (commandersToLoad[origCommanderID][0] > -1) {
					if (numMoversLoaded[commandersToLoad[origCommanderID][0]] == numMoversPerCommander) {
						parts[i].commanderID = -1;
						parts[i].teamId = -1;
						}
					else
						numMoversLoaded[commandersToLoad[origCommanderID][0]]++;
				}
			}

			parts[i].gestureId = 2; // this has never changed
	
			result = missionFile->readIdULong("BaseColor",parts[i].baseColor);
			if (result != NO_ERR || MPlayer )
				parts[i].baseColor = prefs.baseColor;
				
			result = missionFile->readIdULong("HighlightColor1",parts[i].highlightColor1);
			if (result != NO_ERR || MPlayer)
				parts[i].highlightColor1 = prefs.highlightColor;
				
			result = missionFile->readIdULong("HighlightColor2",parts[i].highlightColor2);
			if (result != NO_ERR || MPlayer )
				parts[i].highlightColor2 = prefs.highlightColor;
				
  			parts[i].velocity = 0;
			
			result = missionFile->readIdLong("Active",parts[i].active);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing Active", partName);

			result = missionFile->readIdLong("Exists",parts[i].exists);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing Exists", partName);

			float fDamage = 0.0f;
			result = missionFile->readIdFloat("Damage",fDamage);
			if (result == NO_ERR) {
				if (fDamage >= 1.0) {
					parts[i].destroyed = true;
				};
			}

			result = missionFile->readIdChar("MyIcon", parts[i].myIcon);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing MyIcon", partName);

			result = missionFile->readIdChar("MyIcon", parts[i].myIcon);
			// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
			MC2_VERIFY(result == NO_ERR, "Mission::init: [%s] missing MyIcon", partName);

			result = missionFile->readIdBoolean("Captureable", parts[i].captureable);
			if (result != NO_ERR)
				parts[i].captureable = FALSE;

			float increment = 5.0f/(numParts + 1);
			loadProgress += increment;

		}
	}

	loadProgress = 48.5f;
	long numMechs = 0;
	long numVehicles = 0;
	for (int i = 1; i < (numParts + 1); i++) 
	{
		ObjectTypePtr objType = ObjectManager->loadObjectType(parts[i].objNumber);
		if (!objType)
			objType = ObjectManager->getObjectType(parts[i].objNumber);
		if (objType)
			switch (objType->getObjectTypeClass()) 
			{
				case BATTLEMECH_TYPE:
					numMechs++;
					break;
				case VEHICLE_TYPE:
					numVehicles++;
					break;
			}

		float increment = 6.5f/(numParts + 1);
		loadProgress += increment;
	}

	switch (loadType) 
	{
		case MISSION_LOAD_SP_QUICKSTART:
		case MISSION_LOAD_SP_LOGISTICS:
			break;
		case MISSION_LOAD_MP_QUICKSTART:
		case MISSION_LOAD_MP_LOGISTICS:
			numMechs = 64;
			numVehicles = 64;
			break;
	}

	loadProgress = 55.0f;

	pakFile.seekPacket( 1 );
	ObjectManager->countTerrainObjects(&pakFile, (numMechs + MAX_TEAMS * MAX_REINFORCEMENTS_PER_TEAM) + (numVehicles + MAX_TEAMS * MAX_REINFORCEMENTS_PER_TEAM)/* + ObjectManager->maxElementals*/ + 1);
	loadProgress = 58.0f;
	ObjectManager->setNumObjects(numMechs, numVehicles, 0, -1, -1, -1, 100, 50, 0, 130, -1);

	// C0-3 + C1b GPU authority flip: init GPU cull substrate SSBO sized to
	// worst-case per-frame record count. Now includes BOTH dynamic actors
	// (getMaxObjects() + 25% headroom) AND static prop instances (appended by
	// GpuStaticPropRegistry::flush() before compute_dispatch()). Static props
	// at wolfman zoom can reach ~4096+ visible instances; add a flat 8192
	// record headroom beyond the dynamic actor count to cover them safely.
	{
		const uint32_t maxActors = static_cast<uint32_t>(ObjectManager->getMaxObjects());
		const uint32_t staticPropHeadroom = 8192u;  // visible static props at wolfman zoom
		gpu_cull::substrate_init(maxActors + maxActors / 4u + staticPropHeadroom);
	}
	// C1a: init GPU visibility compute pipeline (shadow/diagnostic mode).
	// No-op if MC2_GPU_CULL env var is not set (default off).
	gpu_cull::compute_init();
	frameJobsInit();  // FRAME-JOBS-1: fixed worker pool for parallel CPU prep
	// Phase 1: terrain lighting GPU compute — per-mission init alongside gpu_cull.
	// CRITICAL: use realVerticesMapSide * realVerticesMapSide, NOT getNumVertices()
	// (getNumVertices() returns 0 at this point — set per-frame by makeLists).
	// Terrain::realVerticesMapSide set during land->init() at mission.cpp:2222, before here.
	// No-op if MC2_TERRAIN_LIGHTING_GPU env var is not set (default off at Stage 1).
	gos_terrain_lighting::mission_init(
		static_cast<uint32_t>(Terrain::realVerticesMapSide * Terrain::realVerticesMapSide),
		64u);
	// C2: init async readback ring buffer.
	// Same maxActors sizing as substrate_init — readback slot must hold one flag per actor.
	// No-op if MC2_GPU_CULL_READBACK env var is not set (default off).
	{
		const uint32_t maxActors = static_cast<uint32_t>(ObjectManager->getMaxObjects());
		const uint32_t staticPropHeadroom = 8192u;
		gpu_cull::readback_init(maxActors + maxActors / 4u + staticPropHeadroom);
	}
	// CP-1: new mission - re-prime the world-fixed static shadow map and the
	// static light matrix (both are process-scoped and otherwise carry over
	// the previous mission's state into this mission).
	gos_ResetStaticShadowPriming();
	mc_ResetTerrainShadowPrimed();
	if (mc2::gate::get<"MC2_DECOR_SHADOW_TRACE">()) {
		printf("[DECOR_SHADOW v1] event=mission_reset_priming frame=%u\n",
		       g_mc2FrameCounter); fflush(stdout);
	}

	long curMech = 0;
	long curVehicle = 0;
	for (long t = 0; t < 8; t++)
		for (long i = 1; i < (numParts + 1); i++) 
		{
			bool loadEm = true;
			if (loadType == MISSION_LOAD_MP_LOGISTICS)
				loadEm = false;
			if (loadType == MISSION_LOAD_SP_LOGISTICS)
				if (parts[i].commanderID == 0 && !parts[i].destroyed)
					loadEm = false;
			if (loadType == MISSION_LOAD_MP_QUICKSTART)
				if (parts[i].commanderID == -1)
					loadEm = false;
			if (loadEm) {
				ObjectTypePtr objType = ObjectManager->getObjectType(parts[i].objNumber);
				if (objType)
					switch (objType->getObjectTypeClass()) 
					{
						case BATTLEMECH_TYPE:
							if (parts[i].teamId == t) 
							{
								BattleMechPtr mech = ObjectManager->getMech(curMech++);
								createPartObject(i, mech);
								float increment = 10.0f/(numParts + 1);
								loadProgress += increment;
							}
							break;
						case VEHICLE_TYPE:
							if (parts[i].teamId == t) 
							{
								GroundVehiclePtr vehicle = ObjectManager->getVehicle(curVehicle++);
								createPartObject(i, vehicle);
								float increment = 10.0f/(numParts + 1);
								loadProgress += increment;
							}
							break;
					}
				}
			else 
			{
				MechWarrior::freeWarrior(MechWarrior::warriorList[parts[i].pilot]);
			}
		}

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeMoverPartsLoad=x1-x;
#endif

	loadProgress = 68.0f;
	mission_phase_mark("actor_spawn_ready"); // LOAD-PHASE-FACTS-1: mover/vehicle/part object init done

	{ ZoneScopedN("Mission::init ObjectManager::loadTerrainObjects"); ObjectManager->loadTerrainObjects(&pakFile, loadProgress, 30); }
	{ ZoneScopedN("Mission::init ObjectManager::primeTerrainObjectsForMissionLoad"); ObjectManager->primeTerrainObjectsForMissionLoad(loadProgress, 2.0f); }
	{ ZoneScopedN("Mission::init Track B static-prop registration walk"); ObjectManager->registerStaticPropsForMissionLoad(); }
	mission_phase_mark("texture_prewarm_ready"); // LOAD-PHASE-FACTS-1: terrain-object load + static-prop residency prewarm done

	loadProgress = 98.0f;

	ObjectManager->buildMoverLists();

	if (MPlayer)
		MPlayer->initSpecialBuildings(commandersToLoad);

	{
		m_timeLimit = -1.0f	/*seconds  IF -1.0 mission never times out!!! -fs*/;
		long result = 0;
		result = missionFile->seekBlock("MissionSettings");
		if (NO_ERR == result)
		{
			float tmpFloat = 0.0;
			result = missionFile->readIdFloat("TimeLimit", tmpFloat);
			if (NO_ERR == result)
			{
				m_timeLimit = tmpFloat;
			}
		}

		if ( MPlayer )
			m_timeLimit = MPlayer->missionSettings.timeLimit;
	}

	if (NumDisableAtStart) {
		for (long i = 0; i < NumDisableAtStart; i++)
			ObjectManager->getByWatchID(parts[DisableAtStart[i]].objectWID)->setDebugFlag(OBJECT_DFLAG_DISABLE, true);
	}

	for (int i = 0; i <= maxTeamID; i++)
		Team::teams[i]->loadObjectives(missionFile);

/*	numObjectives = 0; // this refers to the number of *old* objectives
	objectives = 0;

	warning1 = FALSE;
	warning2 = FALSE;

	Team::home->objectives.Clear();
	Team::home->objectives.Read(missionFile);

	Team::home->numPrimaryObjectives = 0;
	CObjectives::EIterator it = Team::home->objectives.Begin();
	while (!it.IsDone())
	{
		if (1 == (*it)->Priority())
		{
			Team::home->numPrimaryObjectives += 1;
		}
		it++;
	}
	ReadNavMarkers(missionFile, Team::home->objectives);
*/
#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeObjectiveLoad=x-x1;
#endif

	for (long curCommanderId = 0; curCommanderId < MAX_MC_PLAYERS; curCommanderId++) {
		long numGroups = 0;
		char headingStr[128];
		
		if (commandersToLoad[curCommanderId][0] == -1)
			continue;

		// TEAM-COMMANDER-OWNERSHIP-1 audit: this reads commandersToLoad back to
		// INDEX + DEREF Commander::commanders[] below, but (unlike the setTeam site
		// at :2851) only checked != -1. A caller-supplied id >= MAX_COMMANDERS is an
		// OOB read (always a bug -> bound it up front). The NULL-slot case (init
		// loops only created [0..maxCommanderID]) is NOT gated here: the original
		// only dereferences the slot INSIDE the group-block while loop, so a NULL
		// slot with no CommanderNGroup:0 block is benign and must stay silent to
		// keep stock byte-identical. The null-check therefore gates only the actual
		// dereference site, once per commander that truly has group blocks.
		long ctlCommander = commandersToLoad[curCommanderId][0];
		if (!MC2_VERIFY_BOUNDS(ctlCommander, MAX_COMMANDERS, "Mission::init commander-groups commandersToLoad"))
			continue;

		sprintf(headingStr, "Commander%dGroup:%d", curCommanderId, numGroups);
		result = missionFile->seekBlock(headingStr);
		// A group block exists for this commander -> the loop below WILL deref the
		// slot. Verify it was created before entering (matches the original's
		// reachable deref exactly; skips the malformed commander's groups in log
		// mode instead of AVing on the uninitialized slot).
		if (result == NO_ERR &&
			!MC2_VERIFY_NOTNULL(Commander::commanders[ctlCommander], "Mission::init commander-groups Commander::commanders[]"))
			continue;
		while (result == NO_ERR) {
			bool pointSet = FALSE;
			long groupMates[MAX_MOVERGROUP_COUNT_START];
			result = missionFile->readIdLongArray("Mates", groupMates, MAX_MOVERGROUP_COUNT_START);
			Assert(result == NO_ERR, result, " could not find Mates in Group in Scenario File ");
			for (long curMate = 0; curMate < MAX_MOVERGROUP_COUNT_START; curMate++) {
				if ((groupMates[curMate] > 0) && parts[groupMates[curMate]].objectWID) {
					ObjectManager->setPartId(ObjectManager->getByWatchID(parts[groupMates[curMate]].objectWID), ctlCommander, numGroups, curMate);
					Commander::commanders[ctlCommander]->getGroup(numGroups)->add((MoverPtr)ObjectManager->getByWatchID(parts[groupMates[curMate]].objectWID));
					if (!pointSet) {
						Commander::commanders[ctlCommander]->getGroup(numGroups)->selectPoint(true);
						pointSet = true;
					}
				}
			}
			
			numGroups++;
			sprintf(headingStr, "Commander%dGroup:%d", curCommanderId, numGroups);
			result = missionFile->seekBlock(headingStr);
		}
	}

	for (int i = 0; i < Team::numTeams; i++)
		Team::teams[i]->buildRoster();

	//---------------------------------------------------------------------------------
	// If we're not playing multiplayer, make sure all home commander movers have their
	// localMoverId set to 0, so the iface can at least check if a mover is player
	// controlled...
	if (!MPlayer)
		Commander::home->setLocalMoverId(0);

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeCommanderLoad=x1-x;
#endif

	LastTimeGetTime = 0xffffffff;
	scenarioTime = 0.0;
	MissionStartTime = 0;
	runningTime = 0.0;
	actualTime = 0.0;

	gameSystemFile->close();
	delete gameSystemFile;
	gameSystemFile = NULL;

	weather = new Weather;
	weather->init(missionFile);
	
	eye = new GameCamera;
	eye->init();
	gosASSERT(eye != NULL);

	result = eye->init(missionFile);
	gosASSERT(result == NO_ERR);

	eye->inMovieMode = false;

	loadProgress = 99.0;

#ifdef LAB_ONLY
	x=GetCycles();
	MCTimeMiscLoad=x-x1;
#endif

	missionInterface = new MissionInterfaceManager;
	gosASSERT(missionInterface != NULL);
	
	missionInterface->initTacMap( &pakFile, 2 );
	
	FullPathFileName missionScreenName;
	missionScreenName.init(artPath,"missionScrn",".fit");
	
	FitIniFile missionLoader;
	result = missionLoader.open(missionScreenName);
	gosASSERT(result == NO_ERR);
	
	missionInterface->init(&missionLoader);
	missionInterface->initMechs();
	missionLoader.close();

	// MISSION-START-HOVER-TARGET-LIFETIME-1: this MissionInterfaceManager is freshly
	// constructed, but the file-scope hover/target caches and the static `target` pointer
	// survive across missions. Clear them now (begin boundary) so the first Mission::update
	// can't pick a stale/freed object from the previous mission. Picking stays SUPPRESSED
	// until Mission::start() arms it (world fully live).
	MissionInterfaceManager::invalidateHoverTarget();
	
	userInput->setMouseCursor(mState_NORMAL);

	loadProgress = 100.0;

	

	if (tempSpecialAreaFootPrints) {
		systemHeap->Free(tempSpecialAreaFootPrints);
		tempSpecialAreaFootPrints = NULL;
		tempNumSpecialAreas = 0;
	}

	Mover::initOptimalCells(32);


	if (CombatLog)
		MechWarrior::logPilots(CombatLog);

#ifdef LAB_ONLY
	x1=GetCycles();
	MCTimeGUILoad=x1-x;
#endif

#ifdef LAB_ONLY	
	MCTimeABLLoad       *= OneOverProcessorSpeed;
	MCTimeMiscToTeamLoad*= OneOverProcessorSpeed;
	MCTimeTeamLoad      *= OneOverProcessorSpeed;
	MCTimeObjectLoad    *= OneOverProcessorSpeed;
	MCTimeTerrainLoad   *= OneOverProcessorSpeed;
	MCTimeMoveLoad      *= OneOverProcessorSpeed;
	MCTimeMissionABLLoad*= OneOverProcessorSpeed;
	MCTimeWarriorLoad   *= OneOverProcessorSpeed;
	MCTimeMoverPartsLoad*= OneOverProcessorSpeed;
	MCTimeObjectiveLoad *= OneOverProcessorSpeed;
	MCTimeCommanderLoad *= OneOverProcessorSpeed;
	MCTimeMiscLoad      *= OneOverProcessorSpeed;
	MCTimeGUILoad       *= OneOverProcessorSpeed;
#endif

	missionFile->close();
	delete missionFile;
	missionFile = NULL;

	// All actors have been spawned and have called registerType() into the
	// GPU static-prop batcher. Upload the shared VBO/IBO/VAO now so that
	// submit()/flush() can reference immutable geometry for the rest of the
	// mission. Safe here: GL context is live (textures/shadow FBOs already
	// exist by this point) and this is the unconditional tail of map-load.
	GameAdapters::StaticProp::finalizeGeometry();
	GpuMechBatcher::instance().finalizeGeometry();

	// C1b: build the DrawElementsIndirectCommand buffer now that all static prop
	// types are registered and geometry is finalized. No-op if MC2_GPU_CULL is
	// not set or if there are no static prop types in this mission.
	if (gpu_cull::compute_isEnabled()) {
		gpu_cull::compute_buildIndirectBuffer(GameAdapters::StaticProp::typeCount());
	}

	// STATIC-REG-PREWARM-QUEUE-1: bake light slots for registered static props
	// that are off-screen (never reach render() → never cleared their
	// needsFullBakeNextFrame H4 latch). Must run AFTER finalizeGeometry() (geometry
	// is final) AND after eye->init() (world lights are valid — eye is created at
	// mission.cpp:3589 before we reach here). Guarded by MC2_STATIC_REG_PREWARM=1.
	// updateLights() is called first so activeLights is populated (numActiveLights > 0);
	// without it activeLights is empty at load time and SetLightList gets zero lights.
	{ ZoneScopedN("Mission::init prewarmStaticPropLightBakes");
	  eye->primeActiveLightsForPrewarm();
	  ObjectManager->prewarmStaticPropLightBakes(eye); }
	mission_phase_mark("gpu_finalize_ready"); // LOAD-PHASE-FACTS-1: geometry finalize + indirect-buffer build + light-bake prewarm done

	// Vegetation card system — notify adapter that terrain + move map are ready.
	// land and GameMap are both stable by this point (terrain init + MOVE_readData
	// both completed above). No-op when MC2_VEGETATION_CARDS is unset.
	GameAdapters::Vegetation::missionLoaded(land, GameMap);
}
