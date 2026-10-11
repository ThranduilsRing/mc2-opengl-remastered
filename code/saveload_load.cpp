// Mission: the second half of the Mission::load sequence -- the managers, terrain, movement data and mission script,
// the warriors, parts, teams and objects, the commanders, the HUD and the geometry finalize. The first half and the
// orchestrator are in saveload.cpp.
#include "saveload_internal.h"
#include "mission_load_frame.h"
#include "mc2fmt.h"


// Create the trigger-area, crater and object managers.
void Mission::loadAblAndManagers(MissionLoadFrame& f) {
	auto& result = f.result; auto& missionFile = f.missionFile;
#ifndef USE_ABL_LOAD
	//Start the scenario script the old fashion way for now.
	//----------------------
	// Load ABL Libraries...
	long numErrors, numLinesProcessed;
	FullPathFileName libraryFileName;
	libraryFileName.init(missionPath, "orders", ".abx");
	long err = ABLi_loadLibrary(libraryFileName, &numErrors, &numLinesProcessed);
	gosASSERT(err == NO_ERR);

	libraryFileName.init(missionPath, "miscfunc", ".abx");
	err = ABLi_loadLibrary(libraryFileName, &numErrors, &numLinesProcessed);

	libraryFileName.init(missionPath, "corebrain", ".abx");
	err = ABLi_loadLibrary(libraryFileName, &numErrors, &numLinesProcessed);

	FullPathFileName brainFileName;
	brainFileName.init(missionPath, missionScriptName, ".abl");
	
	missionScriptHandle = ABLi_preProcess(brainFileName, &numErrors, &numLinesProcessed);
	gosASSERT(missionScriptHandle >= 0);
	
	missionBrain = new ABLModule;
	gosASSERT(missionBrain != nullptr);
		
	missionBrain->init(missionScriptHandle);

	missionBrain->setName("Mission");
	//MissionBrain->setStep(TRUE);

	missionBrainParams = new ABLParam;
	gosASSERT(missionBrainParams != nullptr);

	missionBrainCallback = missionBrain->findFunction("handlemessage", TRUE);
#endif

	//-----------------------------
	// Init Trigger Area Manager...
	Mover::triggerAreaMgr = new TriggerAreaManager;
	if (Mover::triggerAreaMgr == nullptr)
		STOP(("Mission.Load: unable to init triggerAreaMgr "));

	//-----------------------------------------
	// Startup the Crater Manager.
	craterManager = (CraterManagerPtr)missionHeap->Malloc(sizeof(CraterManager));
	gosASSERT(craterManager != nullptr);
		
	result = craterManager->init(1000,20479,"feet");
	gosASSERT(result == NO_ERR);
	
	//-----------------------------------------------------------------
	// Start the object system next.	
	ObjectManager = new GameObjectManager;
	if (!ObjectManager)
		Fatal(0, " Mission.init: unable to create ObjectManager ");
	ObjectManager->init("object2", 716799, 3072000);
	gosASSERT(result == NO_ERR);

	//-----------------------------------------------------------------
	// Start the collision detection system. -- Doesn't need objects?
	ObjectManager->initCollisionSystem(&missionFile);
}

// Open the terrain pak, load the terrain and re-arm the terrain GPU state.
void Mission::loadTerrain(MissionLoadFrame& f) {
	auto& result = f.result; auto& missionFile = f.missionFile; auto& pakFile = f.pakFile;
	//------------------------------------------------------------
	// Start the Terrain System
	FullPathFileName terrainFileName;
	terrainFileName.init( missionPath, missionFileName, ".pak" ); 

	result = pakFile.open( terrainFileName );
	gosASSERT( result == NO_ERR );

	land = new Terrain;
	gosASSERT(land != nullptr);

	loadProgress = 15.0f;

	long terrainInitResult = land->init(&pakFile, 0, GameVisibleVertices, loadProgress, 15.0 );

	if (terrainInitResult != NO_ERR)
	{
		STOP(("Could not load terrain.  Probably size was wrong!"));
	}

	loadProgress = 35.0f;

	land->load( &missionFile );

	// Stage 0.5 §0 prereq (continued, Block A): mirror Mission::init's
	// terrain-side per-mission init that Mission::destroy() tore down at the
	// top of this function. Block B (gpu_cull substrate/compute/readback
	// re-init) is deferred to after ObjectManager->Load below, because those
	// subsystems size their SSBOs to ObjectManager->getMaxObjects() which
	// returns 0 until Load populates the actor counts. Plan:
	// docs/superpowers/plans/2026-05-20-mission-load-init-mirror-design.md.
	loadProgress = 36.0f;
	land->primeMissionTerrainCache(loadProgress, 4.0f);
	loadProgress = 40.0f;
	// Phase 1: terrain lighting GPU compute — per-mission init alongside
	// gpu_cull (mirror mission.cpp:2817). Uses Terrain::realVerticesMapSide
	// set during land->init() above.
	gos_terrain_lighting::mission_init(
		static_cast<uint32_t>(Terrain::realVerticesMapSide * Terrain::realVerticesMapSide),
		64u);
	// Static-shadow priming reset (process-scoped, mirror mission.cpp:2831-2832).
	gos_ResetStaticShadowPriming();
	mc_ResetTerrainShadowPrimed();
}

// Read the saved move maps and create the path manager.
void Mission::loadMovement(MissionLoadFrame& f) {
	auto& loadFile = f.loadFile; auto& currentPacket = f.currentPacket; auto& forestMoveCost = f.forestMoveCost;
	//----------------------------------------------------
	// Start GameMap for Movement System
	Assert(SimpleMovePathRange > 20, SimpleMovePathRange, " Simple MovePath Range too small ");
	MOVE_init(SimpleMovePathRange);
	if (loadFile.seekPacket(currentPacket) == NO_ERR)
	{
		if (loadFile.getPacketSize() != 0)
		{
			currentPacket += MOVE_readData(&loadFile, currentPacket);
			if (GlobalMoveMap[0]->badLoad)
				STOP((" Mission.Load: old version of move data In-Mission Save Obsolete."));

			GameMap->placeMoversCallback = PlaceMovers;
			GlobalMoveMap[0]->isGateDisabledCallback = IsGateDisabled;
			GlobalMoveMap[1]->isGateDisabledCallback = IsGateDisabled;
			GlobalMoveMap[2]->isGateDisabledCallback = IsGateDisabled;
			GlobalMoveMap[0]->isGateOpenCallback = IsGateOpen;
			GlobalMoveMap[1]->isGateOpenCallback = IsGateOpen;
			GlobalMoveMap[2]->isGateOpenCallback = IsGateOpen;
		}
		else
			STOP(("Mission has no movement Data.  QuickSaved Map"));
	}

	PathFindMap[SECTOR_PATHMAP]->blockedDoorCallback = GetBlockedDoorCells;
	PathFindMap[SECTOR_PATHMAP]->placeStationaryMoversCallback = PlaceStationaryMovers;
	PathFindMap[SIMPLE_PATHMAP]->placeStationaryMoversCallback = PlaceStationaryMovers;
	PathFindMap[SECTOR_PATHMAP]->forestCost = forestMoveCost;
	PathFindMap[SIMPLE_PATHMAP]->forestCost = forestMoveCost;
	PathManager = new MovePathManager;

	loadProgress = 40.0f;
}

// Restore the mission ABL environment and find the mission brain.
void Mission::loadMissionScript(MissionLoadFrame& f) {
	auto& loadFile = f.loadFile; auto& result = f.result; auto& currentPacket = f.currentPacket;
	auto& tmpString = f.tmpString;
	//----------------------
	// Load ABL Libraries...
	// ASS U ME that these are loaded when I call ABLi_Load
	
	//---------------------------
	// Load the mission script...
	//Create a dummy file to read the packet into.
	FullPathFileName dmpName;
	dmpName.init(savePath,tmpString,".abl");

	if (loadFile.seekPacket(currentPacket) != NO_ERR)
		STOP(("ABL SaveData missing from IN-Mission Save"));

	{
		//Create a dummy file to read the packet into.
		File dmyFile;
		result = dmyFile.create(dmpName);
		DWORD fileSz = loadFile.getPacketSize();
		MemoryPtr tmpRAM = (MemoryPtr)malloc(fileSz);
		loadFile.readPacket(currentPacket,tmpRAM);
		dmyFile.write(tmpRAM,fileSz);
	
		currentPacket++;
	
		dmyFile.close();
		free(tmpRAM);
		//---------------------------
	}

	ABLFile ablFile;
	ablFile.open(dmpName);

#ifdef USE_ABL_LOAD
	ABLi_loadEnvironment(&ablFile,true);
#endif
		
	ablFile.close();
	DeleteFile(dmpName);

#ifdef USE_ABL_LOAD
	missionBrain = ABLi_getModule(missionScriptHandle);
	if (!missionBrain)
		STOP(("Could not find mission brain in In-Mission Save"));

	missionBrain->setName("Mission");

	missionBrainParams = new ABLParam;
	gosASSERT(missionBrainParams != nullptr);

	missionBrainCallback = missionBrain->findFunction("handlemessage", TRUE);
#endif
}

// Load the saved warriors and read the parts block header.
void Mission::loadWarriorsAndPartsHeader(MissionLoadFrame& f) {
	auto& loadFile = f.loadFile; auto& result = f.result; auto& currentPacket = f.currentPacket;
	auto& missionFile = f.missionFile;
	loadProgress = 42.0f;

	//-------------------------------------------
	// Load all MechWarriors for this mission...
	MechWarrior::setup();

	currentPacket = MechWarrior::Load( &loadFile, currentPacket );

	loadProgress = 45.0f;

	//-----------------------------------------------------------------
  	// All systems are GO if we reach this point.  Now we need to
	// parse the scenario file for the Objects we need for this scenario
	// We then create each object and place it in the world at the 
	// position we read in with the frame we read in.
	result = missionFile.seekBlock("Parts");
	gosASSERT(result == NO_ERR);
		
	result = missionFile.readIdULong("NumParts",numParts);
	gosASSERT(result == NO_ERR);

	//--------------------------------------------------------------------------------
	// IMPORTANT NOTE: mission parts should always start with Part 1.
	// Part 0 is reserved as a "NULL" id for routines that reference the mission
	// parts. AI routines, Brain keywords, etc. use PART ID 0 as an "object not found"
	// error code. DO NOT USE PART 0!!!!!!! Start with Part 1...

	loadProgress = 50.0f;
}

// Read every part named by the saved mission file.
void Mission::loadParts(MissionLoadFrame& f) {
	auto& result = f.result; auto& missionFile = f.missionFile;
	if (numParts)
	{
		//-----------------------------------------------------
		// Since we leave part 0 unused, malloc numParts + 1...
		parts = (PartPtr)missionHeap->Malloc(sizeof(Part) * (numParts + 1));
		gosASSERT(parts != nullptr);
		
		memset(parts,0,sizeof(Part) * (numParts + 1));

		int i;

		for (i = 1; i < numParts + 1; i++)
		{
			char partName[12];
			mc2::fmt::to_buf(partName, "Part{:d}", mc2::fmt::as_i32(i));
			
			//------------------------------------------------------------------
			// Find the object to load
			result = missionFile.seekBlock(partName);
			gosASSERT(result == NO_ERR);

			//------------------------------------------------------------------
			// Find out what kind of object this is.
			result = missionFile.readIdULong("ObjectNumber",parts[i].objNumber);
			gosASSERT(result == NO_ERR);

			//-------------------------------------------------
			// Read in the data needed to control the object...
			result = missionFile.readIdULong("ControlType", parts[i].controlType);
			gosASSERT(result == NO_ERR);

			result = missionFile.readIdULong("ControlDataType", parts[i].controlDataType);
			gosASSERT(result == NO_ERR);

			result = missionFile.readIdString("ObjectProfile", parts[i].profileName, 9);
			gosASSERT(result == NO_ERR);
				
			result = missionFile.readIdULong("ObjectVariant", parts[i].variantNum);
			if (result != NO_ERR)
				parts[i].variantNum = 0;		//FOR NOW!!!!!!!!!!!!!!!!
												//MAKE a REAL error when Heidi fixes editor.
												//-fs 12/7/99

			result = missionFile.readIdULong("Pilot", parts[i].pilot);
			gosASSERT(result == NO_ERR);
			
			//------------------------------------------------------------------
			// Read the object's position, initial velocity and rotation.
			result = missionFile.readIdFloat("PositionX",parts[i].position.x);
			gosASSERT(result == NO_ERR);
				
			result = missionFile.readIdFloat("PositionY",parts[i].position.y);
			gosASSERT(result == NO_ERR);
				
			parts[i].position.z = -1.0;

			result = missionFile.readIdFloat("Rotation",parts[i].rotation);
			gosASSERT(result == NO_ERR);
				
			result = missionFile.readIdChar("TeamId",parts[i].teamId);
			gosASSERT(result == NO_ERR);

			char cmdId = 0;
			result = missionFile.readIdChar("CommanderId", cmdId);
			if (result != NO_ERR)
			{
				result = missionFile.readIdLong("CommanderId", parts[i].commanderID);
				gosASSERT(result == NO_ERR);
			}
			else
			{
				parts[i].commanderID = cmdId;
			}

			parts[i].gestureId = 2; // this has never changed
	
			result = missionFile.readIdULong("BaseColor",parts[i].baseColor);
			if (result != NO_ERR)
				parts[i].baseColor = 0xffffffff;
				
			result = missionFile.readIdULong("HighlightColor1",parts[i].highlightColor1);
			if (result != NO_ERR)
				parts[i].highlightColor1 = 0xffffffff;
				
			result = missionFile.readIdULong("HighlightColor2",parts[i].highlightColor2);
			if (result != NO_ERR)
				parts[i].highlightColor2 = 0xffffffff;
				
  			parts[i].velocity = 0;
			
			result = missionFile.readIdLong("Active",parts[i].active);
			gosASSERT(result == NO_ERR);

			result = missionFile.readIdLong("Exists",parts[i].exists);
			gosASSERT(result == NO_ERR);

			float fDamage = 0.0f;
			result = missionFile.readIdFloat("Damage",fDamage);
			if (result == NO_ERR) {
				if (fDamage >= 1.0) {
					parts[i].destroyed = true;
				};
			}

			result = missionFile.readIdChar("MyIcon", parts[i].myIcon);
			gosASSERT(result == NO_ERR);

			result = missionFile.readIdBoolean("Captureable", parts[i].captureable);
			if (result != NO_ERR)
				parts[i].captureable = FALSE;
		}
	}
}

// Load the teams, the sensor manager and the objects, then re-arm the GPU cull buffers.
void Mission::loadTeamsAndObjects(MissionLoadFrame& f) {
	auto& loadFile = f.loadFile; auto& result = f.result; auto& currentPacket = f.currentPacket;
	loadProgress = 55.0f;

	//----------------------------------------
	//Team Manager
	// Load Number of Teams
	// Load which number is HOME
	// Call Load for each team.
	currentPacket = Team::Load( &loadFile, currentPacket );

	//-----------------------------------
	// Setup the Sensor System Manager...
	SensorManager = new SensorSystemManager;
	Assert(SensorManager != nullptr, 0, " Unable to init sensor system manager ");
	result = SensorManager->init(true);
	Assert(result == NO_ERR, result, " could not start Sensor System Manager ");

	//-----------------------------------------
	//Object Manager
	// Save numbers of objects in all object lists in game
	// Save ClassID of objects IN the list.
	// For each object in each list:
	//		If NULL, save a magic Number to tell me this entry is NULL.
	//		If NOT NULL, 
	// 			Call Save DOWN the heirarchy.
	//
	currentPacket = ObjectManager->Load( &loadFile, currentPacket );

	{
		static const bool s_mechRestoreTrace = mc2::gate::get<"MC2_MECH_RESTORE_TRACE">();
		if (s_mechRestoreTrace)
			mc2::fmt::print_err("[MECHRESTORE v1] event=saveload_phase phase=post_objmgr_load mechFinalized={:d} appearanceListNull={:d}\n", mc2::fmt::as_i32(GpuMechBatcher::instance().isFinalized() ? 1 : 0), mc2::fmt::as_i32((appearanceTypeList == nullptr) ? 1 : 0));
	}

	// Stage 0.5 §0 prereq (continued, Block B): gpu_cull substrate/compute/readback
	// re-init paired with Mission::destroy's *_shutdown calls (mission.cpp:3264-3269).
	// Placed AFTER ObjectManager->Load above because substrate_init and readback_init
	// size their SSBOs to ObjectManager->getMaxObjects(), which returns 0 until that
	// Load call populates the actor counts. Mirror of Mission::init's order at
	// mission.cpp:2796 setNumObjects -> :2807 substrate_init -> :2811 compute_init
	// -> :2826 readback_init. Each *_init drops its prior state internally on re-init.
	// Plan: docs/superpowers/plans/2026-05-20-mission-load-init-mirror-design.md.
	{
		const uint32_t maxActors = static_cast<uint32_t>(ObjectManager->getMaxObjects());
		const uint32_t staticPropHeadroom = 8192u;  // visible static props at wolfman zoom
		gpu_cull::substrate_init(maxActors + maxActors / 4u + staticPropHeadroom);
	}
	gpu_cull::compute_init();
	{
		const uint32_t maxActors = static_cast<uint32_t>(ObjectManager->getMaxObjects());
		const uint32_t staticPropHeadroom = 8192u;
		gpu_cull::readback_init(maxActors + maxActors / 4u + staticPropHeadroom);
	}
	{
		static const bool s_mechRestoreTrace = mc2::gate::get<"MC2_MECH_RESTORE_TRACE">();
		if (s_mechRestoreTrace)
			mc2::fmt::print_err("[MECHRESTORE v1] event=saveload_phase phase=post_init_mirror maxActors={:d}\n", mc2::fmt::as_u32((uint32_t)ObjectManager->getMaxObjects()));
	}

	ObjectManager->buildMoverLists();
}

// Read the objectives and the commanders and close gamesys.fit.
void Mission::loadObjectivesCommanders(MissionLoadFrame& f) {
	auto& loadFile = f.loadFile; auto& currentPacket = f.currentPacket; auto& gameSystemFile = f.gameSystemFile;
	auto& missionFile = f.missionFile;
	//----------------------------------------------
	// Read in the Objectives.  Safe to have none.
	{
		numObjectives = 0; // this refers to the number of *old* objectives
		objectives = 0;

		warning1 = FALSE;
		warning2 = FALSE;

		Team::home->objectives.Clear();
		Team::home->objectives.Read(&missionFile);

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
	}

	//----------------------------
	// Read in Nav Markers
	// Not needed?  I saved EVERY Objective including the NavMarkers!

	//----------------------------
	//Commander Manager
	// Save Number of Commanders.
	// Save which number is HOME.
	// Call save for each commander.
	currentPacket = Commander::Load( &loadFile, currentPacket );

	//---------------------------------------------------------------------------------
	// If we're not playing multiplayer, make sure all home commander movers have their
	// localMoverId set to 0, so the iface can at least check if a mover is player
	// controlled...
	//
	// Should already be done but no harm in doing again?
	if (!MPlayer)
		Commander::home->setLocalMoverId(0);

	//-----------------------------------------------------
	// This tracks time since scenario started in seconds.
	// ALREADY LOADED above.

	gameSystemFile->close();
	delete gameSystemFile;
	gameSystemFile = nullptr;
}

// Create the weather and the camera.
void Mission::loadWeatherCamera(MissionLoadFrame& f) {
	auto& result = f.result; auto& missionFile = f.missionFile;
	//----------------------------
	// Create and load the Weather
	loadProgress = 92.f;
	weather = new Weather;
	weather->init(&missionFile);
	
	loadProgress = 95.0;
 	//---------------------------------------------------------------
	// Start the Camera and Lists
	eye = new GameCamera;
	eye->init();
	gosASSERT(eye != nullptr);

	result = eye->init(&missionFile);
	gosASSERT(result == NO_ERR);

	loadProgress = 96.0;
}

// Create the mission interface and finish the pilot log.
void Mission::loadInterface(MissionLoadFrame& f) {
	auto& result = f.result; auto& missionFile = f.missionFile; auto& pakFile = f.pakFile;
	//----------------------------------------------------------------------------
	// Start the Mission GUI

	//MUST set this back to NULL to insure that the new gosHandle gets loaded for it!!
	// ITS been flushed up top.  NO leakage!
	PilotIcon::s_pilotTextureHandle = 0;

	missionInterface = new MissionInterfaceManager;
	gosASSERT(missionInterface != nullptr);
	
	missionInterface->initTacMap( &pakFile, 2 );
	
	FullPathFileName missionScreenName;
	missionScreenName.init(artPath,"missionScrn",".fit");
	
	FitIniFile missionLoader;
	result = missionLoader.open(missionScreenName);
	gosASSERT(result == NO_ERR);
	
	missionInterface->init(&missionLoader);
	missionInterface->initMechs();
	missionInterface->Load(&missionFile);
	missionLoader.close();
	
	//----------------------------------------------------------------------------
	userInput->setMouseCursor(mState_NORMAL);

	loadProgress = 98.0;
	
	MechWarrior::initGoalManager(200);

	if (tempSpecialAreaFootPrints) 
	{
		systemHeap->Free(tempSpecialAreaFootPrints);
		tempSpecialAreaFootPrints = nullptr;
		tempNumSpecialAreas = 0;
	}

	Mover::initOptimalCells(32);

	if (CombatLog)
		MechWarrior::logPilots(CombatLog);
}

// Close the mission file and restore the logistics data and the current mission number.
void Mission::loadLogistics(MissionLoadFrame& f) {
	auto& loadFile = f.loadFile; auto& result = f.result; auto& currentPacket = f.currentPacket;
	auto& tmpName = f.tmpName; auto& missionFile = f.missionFile; auto& rps = f.rps;
	missionFile.close();
	DeleteFile(tmpName);

	if (loadFile.seekPacket(currentPacket) != NO_ERR)
		STOP(("Logistics Data missing from IN-Mission Save"));

	{
		//Create a dummy file to read the packet into.
		File dmyFile;
		result = dmyFile.create(tmpName);
		DWORD fileSz = loadFile.getPacketSize();
		MemoryPtr tmpRAM = (MemoryPtr)malloc(fileSz);
		loadFile.readPacket(currentPacket,tmpRAM);
		dmyFile.write(tmpRAM,fileSz);
	
		currentPacket++;
	
		dmyFile.close();
		free(tmpRAM);
		//---------------------------
	}

	FitIniFile logisticsFile;
	result = logisticsFile.open(tmpName);
	if (result != NO_ERR)
		STOP(("Logistics Data missing from IN-Mission Save 2"));

	LogisticsData::instance->load(logisticsFile);
	LogisticsData::instance->setResourcePoints(rps);

	logisticsFile.close();

	DeleteFile(tmpName);

	//-------------------------------------------------------------------------------------------
	// Load the CurrentMission number in logistics cause Heidi don't save it with logisticsData.
	long currentMissionNum = 0;
	loadFile.readPacket(currentPacket,(MemoryPtr)&currentMissionNum);

	LogisticsData::instance->setCurrentMissionNum(currentMissionNum);
	currentPacket++;
	//-------------------------------------------------------------------------------------------
	loadFile.close();
}

// Activate the camera and finalise the static-prop and mech geometry and the GPU cull buffers.
void Mission::loadActivateAndFinalize() {
	eye->activate();
	eye->update();
	loadProgress = 100.f;

	DebugGameObject[0] = DebugGameObject[1] = DebugGameObject[2] = nullptr;

	// VPL-deferred item 11 FIX (a): Mission::load is a parallel mission-
	// setup path that never routes through Mission::init, so it skips
	// Init's unconditional finalizeGeometry() tail (mission.cpp:3114-3115).
	// Mission::destroy() above ran onMapUnload() (s_geometryFinalized=
	// false, s_typeLodIndex cleared); the mechs respawned via
	// ObjectManager->Load re-ran registerTypeLod() while !finalized (OK),
	// but without this call s_geometryFinalized stays false and
	// GpuMechBatcher::submitActor() fast-rejects every actor -> mechs
	// invisible from a savegame. Mirror Init's known-good tail exactly
	// (both batchers; finalizeGeometry() is idempotent / early-returns if
	// already finalized). GL context is live here (camera + mission
	// interface already initialized above), same precondition as
	// mission.cpp:3112-3115.
	GameAdapters::StaticProp::finalizeGeometry();
	GpuMechBatcher::instance().finalizeGeometry();

	// Stage 0.5 §0 prerequisite: mirror Mission::init's compute_buildIndirectBuffer
	// tail (mission.cpp:3134-3136). Without this, s_blockVisBuf inherits the
	// previous mission's block-temporal stamps on savegame restore, breaking any
	// future readback consumer that depends on block-vis correctness post-restore.
	// Logistics::beginMission inherits this transitively via mission->init().
	if (gpu_cull::compute_isEnabled()) {
		gpu_cull::compute_buildIndirectBuffer(GameAdapters::StaticProp::typeCount());
	}
	{
		static const bool s_mechRestoreTrace = mc2::gate::get<"MC2_MECH_RESTORE_TRACE">();
		if (s_mechRestoreTrace)
			mc2::fmt::print_err("[MECHRESTORE v1] event=saveload_phase phase=post_finalize mechFinalized={:d} appearanceListNull={:d}\n", mc2::fmt::as_i32(GpuMechBatcher::instance().isFinalized() ? 1 : 0), mc2::fmt::as_i32((appearanceTypeList == nullptr) ? 1 : 0));
	}
}

// Stop the loading-screen timer and restore the mouse and the frame clock.
void Mission::loadRestoreInput(MissionLoadFrame& f) {
	auto& oldXLoc = f.oldXLoc; auto& oldYLoc = f.oldYLoc; auto& turnOffAsyncMouse = f.turnOffAsyncMouse;
	//YIKES!!  We could be checking the if before the null and executing after!!  Block the thread!
	//Wait for thread to finish.
	while (mc2IsInMouseTimer)
		;

	//ONLY set the mouse BLT data at the end of each update.  NO MORE FLICKERING THEN!!!
	// BLOCK THREAD WHILE THIS IS HAPPENING
	mc2IsInDisplayBackBuffer = true;

	AsynFunc = nullptr;
	mc2UseAsyncMouse = turnOffAsyncMouse;
	if ( !mc2UseAsyncMouse)
		MouseTimerKill();

	mc2IsInDisplayBackBuffer = false;

	LoadScreen::xProgressLoc = oldXLoc;
	LoadScreen::yProgressLoc = oldYLoc;

	userInput->mouseOn();

	//Reset so the LastTimeGetTime is RIGHT NOW!!!
	//  So quickLoad works!!
	DWORD currentTimeGetTime = timeGetTime();
	LastTimeGetTime = currentTimeGetTime;
}

