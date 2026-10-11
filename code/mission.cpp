//******************************************************************************************
//	mission.cpp - This file contains the mission class code
//		Missions are what scenarios were in MechCommander 1.
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//----------------------------------------------------------------------------------

#include "mission_internal.h"


static float simSpeedClamp(float v)
{
	if (v < 0.25f) v = 0.25f;
	if (v > 4.0f) v = 4.0f;
	return v;
}

static float s_simSpeedRuntime = -1.0f;   // < 0 = unseeded (read env on first use)

float simSpeedGetMultiplier()
{
	if (s_simSpeedRuntime < 0.0f)
	{
		s_simSpeedRuntime = mc2::gate::get<"MC2_SIM_SPEED">();   // the row clamps to [0.25, 4]
	}
	return s_simSpeedRuntime;
}

void simSpeedSetMultiplier(float v)
{
	const float prev = simSpeedGetMultiplier();
	s_simSpeedRuntime = simSpeedClamp(v);
	fprintf(stderr, "[SIM-SPEED v1] multiplier=%.2f (was %.2f)\n",
		(double)s_simSpeedRuntime, (double)prev);
	fflush(stderr);
}

void simSpeedResetToSeed()
{
	s_simSpeedRuntime = -1.0f;   // re-seed from MC2_SIM_SPEED on next read
}


//----------------------------------------------------------------------------------
// Macro Definitions
//#define NUM_FIRERANGES		3

// CP-1: frame counter used in the per-mission shadow reset probe.
// Defined in mclib/tgl.cpp; incremented by GameOS frame-end path.
extern uint32_t g_mc2FrameCounter;

//----------------------------------------------------------------------------------
// Static globals
float minFrameRate = 4.0;
float minFrameLength = 1.0/4.0;

long MissionStartTime =	0;			//No Idea

Mission *mission = nullptr;
unsigned long scenarioResult = mis_PLAYING;
long scenarioEndTurn = -1;
long MechSalvageChance = 100;

long globalPlayerSkills[4];		//Per spec.  Integer value = percentage * 100
long globalPlayerWeapons[4];
long globalEnemySkills[4];
long globalEnemyWeapons[4];

float globalMissionValues[MAX_GLOBAL_MISSION_VALUES];

ByteFlag *VisibleBits = nullptr;		//What can currently be seen
ByteFlag *SeenBits = nullptr;			//What HAS been seen

UserHeapPtr missionHeap = nullptr;

unsigned int MultiPlayTeamId = 0xFFFFFFFF;
unsigned int MultiPlayCommanderId = 0xFFFFFFFF;

bool useSensors = true;
bool useCollisions = true;
long missionLineChanged = 0;
bool GeneralAlarm = false;
#ifndef FINAL
float CheatHitDamage = 0.0f;
#endif

bool neverEndingStory = false;

void GetBlockedDoorCells (int moveLevel, int door, char* openCells);
void PlaceStationaryMovers (MoveMap* map);
void PlaceMovers (void);
//---------------------------------------------------------------------------
void initABL (void);
void closeABL (void);
bool showFrameRate = false;

bool Mission::terminationCounterStarted = false;
double Mission::missionTerminationTime = -1.0;
unsigned long Mission::terminationResult = mis_PLAYING;
// GAMEPLAY-DRIVER-2: see mission.h. mis_PLAYING == unarmed.
unsigned long Mission::forcedTerminationResult = mis_PLAYING;

long GameVisibleVertices		= 500;
float BaseHeadShotElevation		= 1.0f;

bool DisplayCameraAngle = false;

bool loadInMissionSave = false;
bool saveInMissionSave = false;

float forcedFrameRate = -1.0f;

// ---- MC2_MISSION_SPLIT: wall-ms split of the Mission::update sub-calls -----
// Locate which logic sub-call owns the 1K-map ~50ms (HITCH logic phase). Default
// OFF, zero behavior change. Emits per-call ns totals + averages at exit (stdout).
//--------------------------------------------------
// Game System Constants -- Definitions here.
float maxVisualRange = 0.0;
unsigned long MaxTreeLOSCellBlock = 0;
float MaxVisualRadius = 0.0;
float fireVisualRange = 0.0;
float baseSensorRange = 0.0;
unsigned char revealTacMap = 0;		//Is Tacmap always revealed?
unsigned char footPrints = 0;		//Do we use/draw footprints?
bool CantTouchThis = false;

long tonnageDivisor = 1.0;			//Amount we divide tons under drop weight by to calc resource bonus
long resourcesPerTonDivided = 0;	//Tons per above divided number

float MineDamage = 0.0;			//Damage mine does per tile
float MineSplashDamage = 0.0;	//Damage mine explosion does to all in tile.
float MineSplashRange = 0.0;	//Range of Splash Damage
long MineExplosion = -1;		//Object ID of mine explosion
long MineLayThrottle = 0;		//Throttle setting of mine Layer
long MineSweepThrottle = 0;		//Throttle setting of mine sweeper
float StrikeWaitTime = 0.0f;	//Time between aerospace Spotter strike calls
float StrikeTimeToImpact = 0.0f;//Time it takes air strike to come in.
float MineWaitTime = 0.0;		//Time Mine Layer must sit in Tile before mine is laid.

long totalSmokeSpheres = 0;			//Maximum number of smoke spheres in world.
long totalSmokeShapeSize = 0;		//Amount we allocate to smoke heap for spheres/textures.

long maxFiresBurning = 0;			//Maximum number of fires allowed in world.
float maxFireBurnTime = 0.0;		//Maximum time a fire can be burning before it dies out.

bool force64MB = TRUE;				//Do we have between 64 and 32 Mb of memory?
bool force32Mb = FALSE;				//Do we have 32Mb of memory only?

float InfluenceRadius = -1.0;		//Capture Radius
float InfluenceTime = 0.0;			//Time inside to Capture

//----------------------------------------------------------------------------------

long Mission::getStatus (void) {

	if (terminationCounterStarted)
		return(terminationResult);
	return(mis_PLAYING);
}

//----------------------------------------------------------------------------------
// GAMEPLAY-DRIVER-2: see the contract comment in mission.h.
void Mission::forceTerminate (unsigned long result) {

	terminationResult = result;
	forcedTerminationResult = result;   // survive the mission brain's per-frame write
	terminationCounterStarted = true;
	// The authored callers use actualTime + 5.0 to let the objective-results
	// GUI play out. Fire immediately instead -- the update() guard still waits
	// on ControlGui::resultsDone(), so the results screen is not skipped, only
	// the 5-second grace is.
	missionTerminationTime = actualTime;
}

//----------------------------------------------------------------------------------

long Mission::render (void)
{
	ZoneScopedN("Camera.UpdateRenderers Mission.render");
	if (active)
	{
		unsigned char tempAmbientLight[3];
		if (KillAmbientLight) {
			tempAmbientLight[0] = eye->ambientRed;
			tempAmbientLight[1] = eye->ambientGreen;
			tempAmbientLight[2] = eye->ambientBlue;
			eye->ambientRed = 0xFF;
			eye->ambientGreen = 0xFF;
			eye->ambientBlue = 0xFF;
		}
		{ ZoneScopedN("Camera.UpdateRenderers eye.render"); GosGlLostDrawsScope _lostSc_("scene.eye_render"); eye->render(); }
		{ ZoneScopedN("Camera.UpdateRenderers clearCellDebugs"); GameMap->clearCellDebugs(0); }

		//-----------------------------------------------------
		// FOG time.  Set Render state to FOG on!
		DWORD fogColor = eye->fogColor;
		if (useFog)
		{
			gos_SetRenderState( gos_State_Fog, fogColor);
		}
		else
		{
			gos_SetRenderState( gos_State_Fog, 0);
		}

		gos_SetRenderState( gos_State_Fog, 0);

		{ ZoneScopedN("Camera.UpdateRenderers FloatHelp.renderAll"); GosGlLostDrawsScope _lostSc_("hud.floathelp"); FloatHelp::renderAll(); }

		currentFloatHelp = 0;

		if (missionInterface)
		{
			ZoneScopedN("Camera.UpdateRenderers missionInterface.render"); GosGlLostDrawsScope _lostSc_("hud.mission_interface");
			missionInterface->render();
		}

		if (KillAmbientLight) {
			eye->ambientRed = tempAmbientLight[0];
			eye->ambientGreen = tempAmbientLight[1];
			eye->ambientBlue = tempAmbientLight[2];
		}

		//reset the TGL RAM pools.
		{
			ZoneScopedN("Camera.UpdateRenderers tglPools.reset");
			colorPool->reset();
			vertexPool->reset();
			facePool->reset();
			shadowPool->reset();
			trianglePool->reset();
		}
	}

	return scenarioResult;
}

//----------------------------------------------------------------------------------
float applyDifficultySkill (float chance, bool isPlayer)
{
	if (isPlayer)
	{
		switch (GameDifficulty)
		{
			case 0:			//Easy
			case 1:			//Medium
			case 2:			//Hard
			case 3:			//Really Hard
				chance *= (float(globalPlayerSkills[GameDifficulty]) / 100.0);
				return(chance);
				break;

			default:
				return (chance);
				break;
		}
	}
	else
	{
		switch (GameDifficulty)
		{
			case 0:			//Easy
			case 1:			//Medium
			case 2:			//Hard
			case 3:			//Really Hard
				chance *= (float(globalEnemySkills[GameDifficulty]) / 100.0);
				return(chance);
				break;

			default:
				return(chance);
				break;
		}
	}
}

//----------------------------------------------------------------------------------
float applyDifficultyWeapon (float dmg, bool isPlayer)
{
	if (isPlayer)
	{
		switch (GameDifficulty)
		{
			case 0:			//Easy
			case 1:			//Medium -- NO CHANGE!
			case 2:			//Hard
			case 3:			//Very Hard
				dmg *= (float(globalPlayerWeapons[GameDifficulty]) / 100.0);
				break;
				
			default:
				break;
		}
	}
	else
	{
		switch (GameDifficulty)
		{
			case 0:			//Easy
			case 1:			//Medium -- NO CHANGE!
			case 2:			//Hard
			case 3:			//Very Hard
				dmg *= (float(globalEnemyWeapons[GameDifficulty]) / 100.0);
				break;
				
			default:
				break;
		}
	}

	//------------------------------------------------------------------
	// Must keep damage an integral part of .25 or bad things happen
	// In theory only needed for Multiplayer but better safe then...
	if (((float)((long)(dmg / 0.25)) * 0.25) != dmg)
	{
		dmg = ((float)((long)(dmg / 0.25)) * 0.25);
	}

	if (dmg < 0.0)
		dmg = 0.0;

	if (dmg > 255.0)
		dmg = 255.0;
		
	return dmg;
}

//----------------------------------------------------------------------------------
void InitDifficultySettings (FitIniFile *gameSystemFile)
{
	long result = gameSystemFile->seekBlock("DifficultySettings");
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "InitDifficultySettings: gamesys.fit missing [DifficultySettings] block");

	result = gameSystemFile->readIdLongArray("PlayerSkills", globalPlayerSkills,4);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "InitDifficultySettings: [DifficultySettings] missing PlayerSkills");

	result = gameSystemFile->readIdLongArray("EnemySkills",globalEnemySkills,4);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "InitDifficultySettings: [DifficultySettings] missing EnemySkills");

	result = gameSystemFile->readIdLongArray("PlayerWeapons",globalPlayerWeapons,4);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "InitDifficultySettings: [DifficultySettings] missing PlayerWeapons");

	result = gameSystemFile->readIdLongArray("EnemyWeapons",globalEnemyWeapons,4);
	// MC2_VERIFY reclassified from gosASSERT (slice MISSION-DATA-DEREF-HARDEN-1)
	MC2_VERIFY(result == NO_ERR, "InitDifficultySettings: [DifficultySettings] missing EnemyWeapons");
}	

//----------------------------------------------------------------------------------
// Sets mission active.  Assumes that whatever we needed to do to the mission data
// set for logistis has been done and player is ready to play!
void DEBUGWINS_setGameObject (long debugObj, GameObjectPtr obj);
extern long GameObjectWindowList[3];
extern long NumGameObjectsToDisplay;

void Mission::start (void)
{
	active = true;
	mission_phase_mark("mission_ready");
	mission_phase_report(); // LOAD-PHASE-FACTS-1: emit consolidated [LOAD_PHASES v1] line

	// APPEAR-AUDIT-BEGIN (META-FIX design B PART 4 — MC2_APPEAR_AUDIT,
	// default OFF, OBSERVATIONAL-FIRST: the appear_audit smoke bucket stays
	// disarmed until a 24-map failed=0 baseline passes, because whether any
	// object class legitimately reports NULL appearance at mission_ready is
	// unverified. One O(N) pass at mission_ready: every existing registered
	// object either has a non-NULL appearance whose base shape loaded, or
	// emits one classified [APPEAR_AUDIT v1] event=missing line.
	// POSITION-FREE by the same off-map-staging carve-out as design A
	// (INVIS-VEHICLES-FIX-1): staged movers have valid appearances and sit
	// off-map by authored design — this scan keys on NO positional or
	// visibility term (no getPosition, no canBeSeen, no in-view test);
	// check-tgl-load-callers.sh pins this region position-free.)
	{
		static const bool s_appearAudit = mc2::gate::get<"MC2_APPEAR_AUDIT">();
		if (s_appearAudit && ObjectManager)
		{
			long scanned = 0, missing = 0;
			const long maxObjs = ObjectManager->getMaxObjects();
			for (long h = 1; h <= maxObjs; h++)
			{
				GameObjectPtr obj = ObjectManager->get(h);
				if (!obj || !obj->getExists())
					continue;
				scanned++;
				AppearancePtr appear = obj->getAppearance();
				const char* why = nullptr;
				if (!appear)
					why = "appearance_null";
				else
				{
					AppearanceTypePtr at = appear->getAppearanceType();
					if (at && at->baseShapeLoadFailed)
						why = "base_shape_failed";
				}
				if (why)
				{
					missing++;
					if (missing <= 32)   // bounded output; summary carries totals
						mc2AuditLinef("[APPEAR_AUDIT v1] event=missing reason=%s "
							"handle=%ld class=%d part=%ld",
							why, h, (int)obj->getObjectClass(),
							obj->getPartId());
				}
			}
			mc2AuditLinef("[APPEAR_AUDIT v1] summary scanned=%ld missing=%ld",
				scanned, missing);
		}
	}
	// APPEAR-AUDIT-END
	// MISSION-START-HOVER-TARGET-LIFETIME-1: mission world is now fully live (objects loaded,
	// active set). Re-enable hover picking that was suppressed since invalidateHoverTarget().
	if (missionInterface)
		MissionInterfaceManager::armHoverTarget();
	gos_SetHudScaleActive(true);  // enable HUD shrink only during mission
	gos_SetHudCanvasActive(true); // UI-ASPECT-ANCHOR-1: HUD chrome on the 16:9 canvas
	// LOAD-BANNER-RESIDUE-1 (queue #4 floating banner): the ProgressTimer
	// async blit keeps compositing the load-progress art over live mission
	// frames while loadProgress lingers in 1..99 after mission_ready — the
	// ~1s world-floating blue panel at mission start. Load is DONE here;
	// zero the progress so the blit's (0,100) window closes immediately.
	// MP 'waiting for players' uses loadProgress==1000 and is untouched.
	if (!MPlayer)
	{
		extern float loadProgress;
		if (loadProgress > 0.0f && loadProgress < 100.0f)
			loadProgress = 0.0f;
	}
	for (long i = 0; i < NumGameObjectsToDisplay; i++)
		DEBUGWINS_setGameObject(-1, ObjectManager->getByWatchID(parts[GameObjectWindowList[i]].objectWID));
}

//----------------------------------------------------------------------------------
void Mission::initTGLForMission()
{
	ZoneScopedN("Mission::initTGLForMission");
	//---------------------------------------------------------
	// End the Tiny Geometry Layer Heap for Logistics
	if (TG_Shape::tglHeap)
	{
		ZoneScopedN("Mission::initTGLForMission shutdown");
		logTglPoolPeaks("mission_unload");
		//Shut down the TGL RAM pools.
		if (colorPool)
		{
			colorPool->destroy();
			delete colorPool;
			colorPool = nullptr;
		}
		
		if (vertexPool)
		{
			vertexPool->destroy();
			delete vertexPool;
			vertexPool = nullptr;
		}

		if (facePool)
		{
			facePool->destroy();
			delete facePool;
			facePool = nullptr;
		}

		if (shadowPool)
		{
			shadowPool->destroy();
			delete shadowPool;
			shadowPool = nullptr;
		}

		if (trianglePool)
		{
			trianglePool->destroy();
			delete trianglePool;
			trianglePool = nullptr;
		}
		
		TG_Shape::tglHeap->destroy();

		delete TG_Shape::tglHeap;
		TG_Shape::tglHeap = nullptr;
	}

	loadProgress += 1.0f;

	//------------------------------------------------------
	// Start the Tiny Geometry Layer Heap for the Mission.
	if ( !TG_Shape::tglHeap )
	{
		ZoneScopedN("Mission::initTGLForMission startup");
		//---------------------------------------------------------
		// Bumped from 40MB to 128MB to accommodate the scaled-up TGL
		// pools below (500K vertex/color/shadow, 200K face/triangle).
		// Total pool footprint is ~60MB; headroom for per-shape
		// allocations via tglHeap->Malloc.
		unsigned long tglHeapSize = 128 * 1024 * 1024;

		TG_Shape::tglHeap = new UserHeap;
		TG_Shape::tglHeap->init(tglHeapSize,"TinyGeom");
		
		//Start up the TGL RAM pools.
		// Sizes scaled up from the original 30K/40K to support the GPU
		// static-prop path (RAlt+0) which forces all buildings/trees/
		// generics in active blocks to TransformShape regardless of
		// inView. At wolfman zoom on large maps the original 30K vertex
		// pool exhausted before mechs allocated, silently dropping every
		// shape whose getVerticesFromPool hit NULL (tgl.h:1022). That
		// manifested as "half the mechs don't render" because mechs
		// iterate AFTER buildings and got the empty pool.
		// MODEL-OVERRIDE: pools sized for modder model overrides. The old
		// 500k/200k caps were CPU-era limits; per-instance vertex/color/shadow
		// storage scales with override mesh complexity x instance count (e.g.
		// ~1000 trees x ~12k verts = ~12M). Static props/trees are GPU-batched,
		// but the per-instance lighting/transform buffers still draw from these
		// pools. Sized to fit a detailed-tree forest with headroom.
		colorPool 		= new TG_VertexPool;
		colorPool->init(32000000);

		vertexPool 		= new TG_GOSVertexPool;
		vertexPool->init(32000000);

		facePool 		= new TG_DWORDPool;
		facePool->init(16000000);

		shadowPool 		= new TG_ShadowPool;
		shadowPool->init(32000000);

		trianglePool 	= new TG_TrianglePool;
		trianglePool->init(16000000);
	}

	loadProgress += 4.0f;

	//Stupid hack for now.  Should really get from prefs!!
	// Needed cause Heidi resets in logistics.
	useFog = true;
	useShadows = prefs.useShadows;
}


//----------------------------------------------------------------------------------
// TEAM-COMMANDER-OWNERSHIP-1: the ONE owner / ONE destroy path for the
// Team::teams[]/Commander::commanders[] static arrays. Previously the identical
// free loop was duplicated inline in Mission::init (re-init guard) AND
// Mission::destroy -- two copies that could drift, double-free, or (the live
// hazard) leave Team::home/Commander::home dangling into a freed slot across a
// mission cycle. Both sites now call this single helper.
//
// Contract: idempotent + null-safe. Every live slot up to the recorded count is
// deleted and nulled, the counts are zeroed, and the home aliases are nulled.
// A second back-to-back call is a guaranteed no-op (counts are 0), which is the
// property the MC2_MISSION_CYCLE_TEST probe in destroy() exercises to prove the
// double-free can no longer happen.
void Mission::resetTeamsAndCommanders()
{
	long numt = Team::numTeams;
	for (long i = 0; i < numt; i++)
		if (Team::teams[i]) {
			delete Team::teams[i];
			Team::teams[i] = nullptr;
		}
	Team::numTeams = 0;
	// Null the raw alias into teams[] -- it pointed at a slot just freed above
	// (or was already NULL). Leaving it dangling is the cross-cycle hazard the
	// game-layer review flagged; readers (mission.cpp:659/930/... , update HUD /
	// objectives) guard some but not all dereferences.
	Team::home = nullptr;

	long numC = Commander::numCommanders;
	for (long i = 0; i < numC; i++)
		if (Commander::commanders[i]) {
			delete Commander::commanders[i];
			Commander::commanders[i] = nullptr;
		}
	Commander::numCommanders = 0;
	Commander::home = nullptr;

	// Post-condition: exactly one teardown authority left the statics empty.
	// Silent when TRUE; only the cold path (a leftover live slot / stale count
	// -> a would-be leak or double-free source) logs/stops per MC2_VERIFY_MODE.
	MC2_VERIFY(Team::numTeams == 0 && Commander::numCommanders == 0,
		"resetTeamsAndCommanders: counts not zeroed (teams=%ld commanders=%ld)",
		Team::numTeams, Commander::numCommanders);
}


//----------------------------------------------------------------------------------
void Mission::destroy (bool initLogistics)
{
	// MC2-VERIFY-LIVE-1: [VERIFY] counter line at mission end (soak evidence:
	// fires=0 expected on stock missions). Silent when MC2_VERIFY_MODE=off.
	mc2verify::MissionSummary(missionFileName);

	gos_SetHudScaleActive(false);  // back to 100% for menus/logistics
	gos_SetHudCanvasActive(false); // UI-ASPECT-ANCHOR-1: canvas off with the shrink

	// C2: release async readback ring buffer at mission teardown.
	gpu_cull::readback_shutdown();
	// C1a: release GPU compute resources at mission teardown.
	frameJobsShutdown();  // FRAME-JOBS-1: join workers before any GPU teardown (LIFO vs init)
	gpu_cull::compute_shutdown();
	// C0-3: release GPU cull substrate SSBO at mission teardown.
	// substrate_init() handles re-init on next mission load (calls shutdown internally).
	gpu_cull::substrate_shutdown();

	// Release GPU resources at mission shutdown.
	// M1 Task 13 (order-correct split): call order is byte-identical to original:
	//   1. GpuStaticPropBatcher::onMapUnload()  ← endMissionEarly()
	//   2. GpuMechBatcher::onMapUnload()
	//   3. GpuStaticPropRegistry::destroy() + RenderWorld::destroy() ← endMissionLate()
	//   4. Mech::endMission()
	GameAdapters::StaticProp::endMissionEarly();   // M1 Task 13
	GpuMechBatcher::instance().onMapUnload();
	GameAdapters::StaticProp::endMissionLate();    // M1 Task 13
	GameAdapters::Mech::endMission();               // M2: mech lifecycle
	GameAdapters::Vegetation::missionUnloaded();    // vegetation card system teardown

	//---------------------------------------------------------------
	// Shutdown the Mission Interface
	if (missionInterface)
	{
		// MISSION-START-HOVER-TARGET-LIFETIME-1: clear the static `target` pointer and the
		// file-scope hover caches at teardown (end boundary). The objects they reference are
		// about to be freed; suppress picking so nothing dereferences them before the next
		// mission arms hover again.
		MissionInterfaceManager::invalidateHoverTarget();
		missionInterface->destroy();

		delete missionInterface;
		missionInterface = nullptr;
	}
	
	delete missionBrain;
	missionBrain = nullptr;

	delete missionBrainParams;
	missionBrainParams = nullptr;

	//Team::home->objectives.Clear();

	//---------------------------------------------------------------
	// End the Camera and Lists
	if (eye)
	{
		eye->destroy();

		delete eye;
		eye = nullptr;
		// Null the static TG_Shape camera-matrix aliases that TG_Shape::
		// SetCameraMatrices() (tgl.cpp:1619-1620) populated with raw pointers
		// into the now-freed eye object. Both `s_cameraOrigin` and
		// `s_cameraToClip` outlive their owner; if a subsequent code path
		// (e.g. Mission::load -> MechWarrior::Load -> registerStaticProp ->
		// TG_MultiShape::TransformMultiShape at msl.cpp:1438) dereferences
		// the stale pointer before eye->update() re-primes them, the dereference
		// reads freed heap (latent UB; reliably crashed 2026-05-20 after Block A
		// allocations stomped the freed eye block first). The next eye->update()
		// in Mission::init / Mission::load re-populates both via
		// SetCameraMatrices(). quad.cpp:2008 carries the same dereference and
		// is also covered by this nulling.
		TG_Shape::s_cameraOrigin = nullptr;
		TG_Shape::s_cameraToClip = nullptr;
	}

	if (PathManager) {
		PathManager->destroy();
		delete PathManager;
		PathManager = nullptr;
	}
	MOVE_cleanup();

	MechWarrior::shutdown();
	
	delete weather;
	weather = nullptr;

	//------------------------------------------------------------
	// End the Terrain System
	if (land)
	{
		land->destroy();

		delete land;
		land = nullptr;
	}

	//--------------------------------------------------------------
	// Shut down the object system
	// CANNOT just delete heaps do to our ASSHOLE MUNGA/ADEPT
	// CODE which cannot be shutdown and restarted more then once.
	//
	// I wish I could just do demos and call that game experience!!!!
	//
	if (ObjectManager)
	{
		ObjectManager->destroy();
		delete ObjectManager;
		ObjectManager = nullptr;
	}
	
	if (Mover::sortList)
	{
		delete Mover::sortList;
		Mover::sortList = nullptr;
	}

	if (SensorSystem::sortList)
	{
		delete SensorSystem::sortList;
		SensorSystem::sortList = nullptr;
	}

	if (GVAppearanceType::SensorTriangleShape)
	{
		delete GVAppearanceType::SensorTriangleShape;
		GVAppearanceType::SensorTriangleShape = nullptr;
	}
	
	if (GVAppearanceType::SensorCircleShape)
	{
		delete GVAppearanceType::SensorCircleShape;
		GVAppearanceType::SensorCircleShape = nullptr;
	}

	if (Mech3DAppearanceType::SensorSquareShape)
	{
		delete Mech3DAppearanceType::SensorSquareShape;
		Mech3DAppearanceType::SensorSquareShape = nullptr;
	}
	
	if (MasterComponent::masterList) 
	{
		MasterComponent::freeMasterList();
	}

	//-----------------------------------
	// Get rid of the Effects manager
	if (weaponEffects)
		weaponEffects->destroy();

	delete weaponEffects;
	weaponEffects = nullptr;

	if ( craterManager )
	{
		craterManager->destroy();
		missionHeap->Free( craterManager );
		craterManager = nullptr;
	}

	//-----------------------------------
	// Sensors now stored in the missionHeap
	// Which goes away several lines below this
	//if (SensorManager)
	//{
	//	SensorManager->destroy();
	//	delete SensorManager;
	//	SensorManager = NULL;
	//}

	// TEAM-COMMANDER-OWNERSHIP-1: single teardown authority (was a duplicated
	// inline free loop; see resetTeamsAndCommanders()). Also nulls the
	// Team::home/Commander::home aliases.
	Mission::resetTeamsAndCommanders();

	if (Mover::triggerAreaMgr) {
		delete Mover::triggerAreaMgr;
		Mover::triggerAreaMgr = nullptr;
	}

	if (MoverGroup::goalMap)
	{
		systemHeap->Free(MoverGroup::goalMap);
		MoverGroup::goalMap = nullptr;
	}

	if (openList)
	{
		delete openList;
		openList = nullptr;
	}

	if (Team::sortList)
	{
		delete Team::sortList;
		Team::sortList = nullptr;
	}

	closeABL();

	//------------------------------------------------------------
	// End the Mission Heap
	if (missionHeap)
	{
		missionHeap->destroy();
		
		delete missionHeap;
		missionHeap = nullptr;
	}
	
	if ( appearanceTypeList )
	{
		appearanceTypeList->destroy();
		delete appearanceTypeList;
		appearanceTypeList = nullptr;
	}
	
	if (initLogistics)
		initTGLForLogistics();
	else
	{
		//---------------------------------------------------------
		// End the Tiny Geometry Layer Heap for Logistics
		if (TG_Shape::tglHeap)
		{
			logTglPoolPeaks("mission_unload");
			//Shut down the TGL RAM pools.
			if (colorPool)
			{
				colorPool->destroy();
				delete colorPool;
				colorPool = nullptr;
			}

			if (vertexPool)
			{
				vertexPool->destroy();
				delete vertexPool;
				vertexPool = nullptr;
			}

			if (facePool)
			{
				facePool->destroy();
				delete facePool;
				facePool = nullptr;
			}

			if (shadowPool)
			{
				shadowPool->destroy();
				delete shadowPool;
				shadowPool = nullptr;
			}

			if (trianglePool)
			{
				trianglePool->destroy();
				delete trianglePool;
				trianglePool = nullptr;
			}

			TG_Shape::tglHeap->destroy();

			delete TG_Shape::tglHeap;
			TG_Shape::tglHeap = nullptr;
		}
	}
	
//	userInput->mouseOff();

	active = FALSE;
	scenarioResult = mis_PLAYING;		//We've completed the mission.  Set it back to playing!
	GeneralAlarm = false;

	EllipseElement::removeTextureHandle();

	//The flush below will remove this one.
	Mover::holdFireIconHandle = 0;

	mcTextureManager->flush();
	mcTextureManager->freeVertices();
	mcTextureManager->freeShapes();

	soundSystem->purgeSoundSystem();

	missionFileName[0] = 0;
	
	if (CObjective::s_markerFont)
	{
		delete CObjective::s_markerFont;
		CObjective::s_markerFont = nullptr;
	}

	//Heading back to logistics now.  Change screen back to 800x600
//	if (renderer == 3)
//		gos_SetScreenMode(800,600,16,0,0,0,true,false,0,false,0,renderer);
//	else
//		gos_SetScreenMode(800,600,16,0,0,0,0,false,0,false,0,renderer);

	//Start finding the Leaks
	//systemHeap->dumpRecordLog();
}

//----------------------------------------------------------------------------------

void Mission::handleMultiplayMessage (long code, long param1) {

	if (missionBrainCallback) {
		CurMultiplayCode = code;
		CurMultiplayParam = param1;
		missionBrain->execute(nullptr, missionBrainCallback);
		CurMultiplayCode = 0;
		CurMultiplayParam = 0;
	}
}

//----------------------------------------------------------------------------------

void Mission::startObjectiveTimers (void) {

	for (long i=0;i<(long)numObjectives;i++)
	{
		if (objectives[i].timeLeft > 0.0)
		{
			setObjectiveTimer(i,objectives[i].timeLeft);
		}
	}
}

//----------------------------------------------------------------------------------
long Mission::setObjectiveTimer (long objectiveNum, float timeLeft)
{
	gosASSERT((objectiveNum >= 0) && objectiveNum < (long)numObjectives);
		
	long timerNumber = OBJECTIVE_1_TIMER + objectiveNum;
	
	//------------
	// Add Timer.
	TimerPtr timer = timerManager->getTimer(timerNumber);
	gosASSERT(timer != nullptr);
	timer->setTimer(timeLeft);

	return(NO_ERR);
}	

//----------------------------------------------------------------------------------
float Mission::checkObjectiveTimer (long objectiveNum)
{
	gosASSERT((objectiveNum >= 0) || objectiveNum < (long)numObjectives);

	long timerNumber = OBJECTIVE_1_TIMER + objectiveNum;
	unsigned long timeLeft = 0;

	TimerPtr timer = timerManager->getTimer(timerNumber);
	gosASSERT(timer != nullptr);

	timeLeft = timer->getCurrentTime();
	
	return(timeLeft);
}	

//----------------------------------------------------------------------------------
long Mission::setObjectiveStatus (long objectiveNum, ObjectiveStatus status)
{
	gosASSERT((objectiveNum >= 0) || objectiveNum < (long)numObjectives);
	
	if (objectives)
		objectives[objectiveNum].status = status;
	
	return(NO_ERR);
}	

//----------------------------------------------------------------------------------
bool Mission::checkObjectiveSuccess (void)
{
	//NOW uses NEW(tm) objectives.
	long count = 0;
	bool result = false;
	if (Team::home->objectives.Count())
	{
		for ( CObjectives::EIterator iter = Team::home->objectives.Begin(); !iter.IsDone(); iter++, count++ )
		{
			//Must iterate through ALL objectives and check.
			result |= (*iter)->StatusChangedSuccess();
		}
	}
	
	return result;
}

//----------------------------------------------------------------------------------
bool Mission::checkObjectiveFailed (void)
{
	//NOW uses NEW(tm) objectives.
	long count = 0;
	bool result = false;
	if (Team::home->objectives.Count())
	{
		for ( CObjectives::EIterator iter = Team::home->objectives.Begin(); !iter.IsDone(); iter++, count++ )
		{
			//Must iterate through ALL objectives and check.
			result |= (*iter)->StatusChangedFailed();
		}
	}
	
	return result;
}

//----------------------------------------------------------------------------------
ObjectiveStatus Mission::checkObjectiveStatus (long objectiveNum)
{
	//NOW uses NEW(tm) objectives.
	long count = 0;
	if (Team::home->objectives.Count())
	{
		for ( CObjectives::EIterator iter = Team::home->objectives.Begin(); !iter.IsDone(); iter++, count++ )
		{
			if (count == objectiveNum)
			{
				return (*iter)->Status(Team::home->objectives);
			}
		}
	}
	
	return OS_UNDETERMINED;
}	

//----------------------------------------------------------------------------------
long Mission::setObjectiveType (long objectiveNum, ObjectiveType type)
{
	gosASSERT((objectiveNum >= 0) || objectiveNum < (long)numObjectives);

	if (objectives)
		objectives[objectiveNum].type = type;
	
	return(NO_ERR);
}	

//----------------------------------------------------------------------------------
ObjectiveType Mission::checkObjectiveType (long objectiveNum)
{
	gosASSERT((objectiveNum >= 0) || objectiveNum < (long)numObjectives);
		
	ObjectiveType result = PrimaryGoal;
	
	if (objectives)
		result = objectives[objectiveNum].type;
	
	return(result);
}	

//----------------------------------------------------------------------------------
void Mission::setObjectivePos (long objectiveNum, float realX, float realY, float realZ)
{
	gosASSERT((objectiveNum >= 0) || objectiveNum < (long)numObjectives);
		
	if (objectives)
	{
		objectives[objectiveNum].position.x = realX;
		objectives[objectiveNum].position.y = realY;
		objectives[objectiveNum].position.z = realZ;
	}
}
