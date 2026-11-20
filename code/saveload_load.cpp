#include "save_archive.h"
#include "save_sections.h"
#include "save_brain_section.h"
#include "save_fix_section.h"
#include <optional>
#include <cstdlib>
#include "gos_crashbundle.h"
#include "utils/mc2log.h"
#include "logisticserrors.h"
#include "footprints.h"
// Mission: the second half of the Mission::load sequence -- the managers, terrain, movement data and mission script,
// the warriors, parts, teams and objects, the commanders, the HUD and the geometry finalize. The first half and the
// orchestrator are in saveload.cpp.
#include "saveload_internal.h"
#include "missiongui.h"
#include "mission_load_frame.h"
#include "static_prop_prewarm.h"
#include "render_resource_owners.h"
#include "mc2fmt.h"
#include "../GameOS/gameos/diagnostic_trace.h"
#include "tex_resolve_table.h"
#include "../mclib/texture_assets.h"
#include "../GameOS/gameos/vk_render_infra.h"
#include "frame_jobs.h"
#include "../GameAdapters/VegetationAdapter.h"
#include "../GameOS/gameos/visual_tuning_profile.h"
#include "../GameOS/gameos/terrain_material_lib.h"
#include "../GameOS/gameos/gpu_memory_report.h"
#include <new>

static void applySaveSections(std::string_view fileName, const mc2::save::SectionContainer& sections, mc2::sim::TacticalOrderQueue& orderQueue);
// FIX1 music, held from applySaveSections until loadActivateAndFinalize re-arms the mission tune.
static std::optional<mc2::save::FixMusic> s_restoredMusic;
extern void preloadMissionSupportAtLoad();


// Create the trigger-area and object managers.
void Mission::loadAblAndManagers(MissionLoadFrame& f) {
	auto& result = f.result; auto& missionFile = f.missionFile;
#ifndef USE_ABL_LOAD
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

	missionBrainParams = new ABLParam;
	gosASSERT(missionBrainParams != nullptr);

	missionBrainCallback = missionBrain->findFunction("handlemessage", TRUE);
#endif

	Mover::triggerAreaMgr = new TriggerAreaManager;
	if (Mover::triggerAreaMgr == nullptr)
		STOP(("Mission.Load: unable to init triggerAreaMgr "));

	Footprints::init();

	ObjectManager = new GameObjectManager;
	if (!ObjectManager)
		Fatal(0, " Mission.init: unable to create ObjectManager ");
	ObjectManager->init("object2", 716799, 3072000);
	gosASSERT(result == NO_ERR);

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
	if ( result != NO_ERR )
		STOP(( "Mission::loadTerrain: unable to open terrain pak %s", static_cast<const char*>(terrainFileName) ));

	// Mission::load can run mid-frame (quickload), after Terrain::geometry armed the per-frame
	// tex_resolve memo; Mission::destroy released the terrain nodes, so drop the stale handles
	// before the cement/colormap atlas builds resolve them (Mission::init runs out of frame).
	invalidateTexResolveTable();

	land = new Terrain;
	gosASSERT(land != nullptr);
	// Missing or malformed saved names need the authored asset selection before terrain init.
	char savedColorMapName[1024]{};
	if (missionFile.seekBlock("ColorMap") == NO_ERR &&
		missionFile.readIdString("ColorMapName", savedColorMapName, 1023) == NO_ERR &&
		savedColorMapName[0] != '\0') {
		land->setColorMapName(savedColorMapName);
	} else {
		FullPathFileName authoredMissionName;
		authoredMissionName.init(missionPath, missionFileName, ".fit");
		FitIniFile authoredMission;
		if (authoredMission.open(authoredMissionName) == NO_ERR)
			land->getColorMapName(&authoredMission);
		// Unavailable authored file: keep the pak-name fallback silently.
	}

	loadProgress = 15.0f;

	long terrainInitResult = land->init(&pakFile, 0, GameVisibleVertices, loadProgress, 15.0 );

	if (terrainInitResult != NO_ERR)
	{
		STOP(("Could not load terrain.  Probably size was wrong!"));
	}

	loadProgress = 35.0f;

	land->load( &missionFile );

	// Terrain caches depend only on terrain dimensions; actor buffers wait for ObjectManager::Load.
	loadProgress = 36.0f;
	land->primeMissionTerrainCache(loadProgress, 4.0f);
	loadProgress = 40.0f;
	gos_terrain_lighting::mission_init(
		static_cast<uint32_t>(Terrain::realVerticesMapSide * Terrain::realVerticesMapSide),
		64u);

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
			currentPacket += MOVE_readData(&loadFile, currentPacket, true,
				{.cells = f.sections.find(mc2::save::tagMapCells) != nullptr,
				 .areas = f.sections.find(mc2::save::tagMapAreas) != nullptr,
				 .doors = f.sections.find(mc2::save::tagMapDoors) != nullptr,
				 .links = f.sections.find(mc2::save::tagDoorLinks) != nullptr});
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

	configurePathfinding(forestMoveCost);

	loadProgress = 40.0f;
}

// Restore the mission ABL environment and find the mission brain.
void Mission::loadMissionScript(MissionLoadFrame& f) {
	auto& loadFile = f.loadFile; auto& result = f.result; auto& currentPacket = f.currentPacket;
	auto& tmpString = f.tmpString;
	visualTuning_applyProfile(missionScriptName);
	terrainMaterials_apply(missionScriptName);
	FullPathFileName dmpName;
	dmpName.init(savePath,tmpString,".abl");

	if (loadFile.seekPacket(currentPacket) != NO_ERR)
		STOP(("ABL SaveData missing from IN-Mission Save"));

	{
		File dmyFile;
		result = dmyFile.create(dmpName);
		DWORD fileSz = loadFile.getPacketSize();
		MemoryPtr tmpRAM = static_cast<MemoryPtr>(malloc(fileSz));
		loadFile.readPacket(currentPacket,tmpRAM);
		dmyFile.write(tmpRAM,fileSz);
	
		currentPacket++;
	
		dmyFile.close();
		free(tmpRAM);
	}

	ABLFile ablFile;
	ablFile.open(dmpName);

#ifdef USE_ABL_LOAD
	ABLi_loadEnvironment(&ablFile, true, f.sections.find(mc2::save::tagAblEnvironment) != nullptr);
#endif
		
	ablFile.close();
	removeSaveScratchFile(dmpName);

#ifdef USE_ABL_LOAD
	missionBrain = ABLi_getModule(missionScriptHandle);
	if (!missionBrain) {
		if (missionScriptHandle != -1)
			mc2::fmt::print_err("[ABL_LOAD_BAD] missing mission brain handle={}\n", missionScriptHandle);
		missionBrainParams = nullptr;
		missionBrainCallback = nullptr;
		return;
	}

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

	currentPacket = MechWarrior::Load( &loadFile, currentPacket,
		f.sections.find(mc2::save::tagWarriorStatic) != nullptr,
		f.sections.find(mc2::save::tagWarrior) != nullptr );

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
		parts = static_cast<PartPtr>(std::malloc(sizeof(Part) * (numParts + 1)));
		gosASSERT(parts != nullptr);
		
		memset(parts,0,sizeof(Part) * (numParts + 1));

		int i;
		bool shiftedLegacyParts = false;
		unsigned long partsFormat = 0;
		const bool haveFormatTag = (missionFile.seekBlock("Parts") == NO_ERR) && (missionFile.readIdULong("PartsFormat", partsFormat) == NO_ERR);

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

			//------------------------------------------------------------------
			// The part's physical incarnation and squad. Saves written before Mission::Save wrote parts[1..N] carry a blank
			// Part1 and every later part shifted by one; those WIDs would name the wrong objects, so they stay 0 as before.
			{
				int savedWID = 0;
				long savedSquad = 0;
				const bool haveWID = (missionFile.readIdInt("ObjectWID", savedWID) == NO_ERR);
				const bool haveSquad = (missionFile.readIdLong("SquadId", savedSquad) == NO_ERR);
				if (i == 1)
					shiftedLegacyParts = mc2::save::partsAreShiftedLegacy(haveFormatTag, haveWID, savedWID, parts[i].objNumber);
				if (!shiftedLegacyParts)
				{
					if (haveWID)
						parts[i].objectWID = savedWID;
					if (haveSquad)
						parts[i].squadId = static_cast<char>(savedSquad);
				}
			}

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
			// A save writes the variant as VariantNum (read after Pilot/Captureable); legacy shifted saves keep 0.
			if (!shiftedLegacyParts)
			{
				unsigned long savedVariant = 0;
				if (missionFile.readIdULong("VariantNum", savedVariant) == NO_ERR)
					parts[i].variantNum = savedVariant;
			}

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
	currentPacket = Team::Load( &loadFile, currentPacket, f.sections.find(mc2::save::tagTeam) != nullptr );

	//-----------------------------------
	// Setup the Sensor System Manager...
	SensorManager = std::make_unique<SensorSystemManager>();
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
	currentPacket = ObjectManager->Load( &loadFile, currentPacket,
		f.sections.find(mc2::save::tagObjectManager) != nullptr,
		f.sections.find(mc2::save::tagWatchList) != nullptr,
		f.sections.find(mc2::save::tagObjectSlots) != nullptr );

	{
		static const bool s_mechRestoreTrace = false;
		if (s_mechRestoreTrace)
			mc2::fmt::print_err("[MECHRESTORE v1] event=saveload_phase phase=post_objmgr_load mechFinalized={:d} appearanceListNull={:d}\n", mc2::fmt::as_i32(GpuMechBatcher::instance().isFinalized() ? 1 : 0), mc2::fmt::as_i32((appearanceTypeList == nullptr) ? 1 : 0));
	}

	// Actor buffer capacity is known only after ObjectManager::Load.
	{
		const uint32_t maxActors = static_cast<uint32_t>(ObjectManager->getMaxObjects());
		const uint32_t staticPropHeadroom = 8192u;  // visible static props at wolfman zoom
		gpu_cull::substrate_init(maxActors + maxActors / 4u + staticPropHeadroom);
	}
	gpu_cull::compute_init();
	frameJobsInit();
	{
		const uint32_t maxActors = static_cast<uint32_t>(ObjectManager->getMaxObjects());
		const uint32_t staticPropHeadroom = 8192u;
		gpu_cull::readback_init(maxActors + maxActors / 4u + staticPropHeadroom);
	}
	{
		static const bool s_mechRestoreTrace = false;
		if (s_mechRestoreTrace)
			mc2::fmt::print_err("[MECHRESTORE v1] event=saveload_phase phase=post_init_mirror maxActors={:d}\n", mc2::fmt::as_u32(static_cast<uint32_t>(ObjectManager->getMaxObjects())));
	}

	ObjectManager->registerStaticPropsForMissionLoad();
	ObjectManager->buildMoverLists();
}

// Read the objectives and the commanders and close gamesys.fit.
void Mission::loadObjectivesCommanders(MissionLoadFrame& f) {
	auto& loadFile = f.loadFile; auto& currentPacket = f.currentPacket; FitIniFile* const gameSystemFile = &f.gameSystemFile;
	auto& missionFile = f.missionFile;
	//----------------------------------------------
	// Read in the Objectives.  Safe to have none.
	{
		numObjectives = 0; // this refers to the number of *old* objectives
		objectives = nullptr;

		warning1 = FALSE;
		warning2 = FALSE;

		Team::home->objectives.Clear();
		Team::home->objectives.Read(&missionFile);

		Team::home->numPrimaryObjectives = 0;
		for ( CObjective* pObjective : Team::home->objectives )
		{
			if (1 == pObjective->Priority())
			{
				Team::home->numPrimaryObjectives += 1;
			}
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
	currentPacket = Commander::Load( &loadFile, currentPacket, f.sections.find(mc2::save::tagCommander) != nullptr );

	//---------------------------------------------------------------------------------
	// If we're not playing multiplayer, make sure all home commander movers have their
	// localMoverId set to 0, so the iface can at least check if a mover is player
	// controlled...
	//
	// Should already be done but no harm in doing again?
	if (!MPlayer)
		Commander::home->setLocalMoverId(0);
	bindOrderRecipients();

	//-----------------------------------------------------
	// This tracks time since scenario started in seconds.
	// ALREADY LOADED above.

	gameSystemFile->close();
}

// Create the weather and the camera.
void Mission::loadWeatherCamera(MissionLoadFrame& f) {
	auto& result = f.result; auto& missionFile = f.missionFile;
	//----------------------------
	// Create and load the Weather
	loadProgress = 92.f;
	missionWeather = std::make_unique_for_overwrite<Weather>();
	weather = missionWeather.get();
	weather->init(&missionFile);
	
	loadProgress = 95.0;
 	//---------------------------------------------------------------
	// Start the Camera and Lists
	missionCamera = std::make_unique_for_overwrite<GameCamera>();
	eye = missionCamera.get();
	eye->init();
	gosASSERT(eye != nullptr);

	mc2SetMissionLightProfileName(missionFileName);   // LOCAL-LIGHTS-4: the save's MissionName (authored mission)
	result = eye->init(&missionFile);
	gosASSERT(result == NO_ERR);
	eye->inMovieMode = false;

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
	if (result != NO_ERR)
		STOP(("Unable to open mission screen layout %s", static_cast<const char*>(missionScreenName)));
	
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
		std::free(tempSpecialAreaFootPrints);
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
	removeSaveScratchFile(tmpName);

	if (loadFile.seekPacket(currentPacket) != NO_ERR)
		STOP(("Logistics Data missing from IN-Mission Save"));

	{
		//Create a dummy file to read the packet into.
		File dmyFile;
		result = dmyFile.create(tmpName);
		DWORD fileSz = loadFile.getPacketSize();
		MemoryPtr tmpRAM = static_cast<MemoryPtr>(malloc(fileSz));
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

	// A v2 save's campaign is probed in openSaveFile before teardown; this is a v1 save or a post-probe failure. The mission is
	// already rebuilt and STOP is a no-op in RelWithDebInfo, so a missing campaign aborts with a bundle instead of continuing on
	// the old campaign's roster. Any other failure (version, a variant whose chassis pack is absent) keeps the old behaviour: log and continue.
	if (const auto restored = LogisticsData::instance->load(logisticsFile); !restored) {
		if (restored.error() == CAMPAIGN_FILE_UNAVAILABLE) {
			crashbundle_fatal("In-mission save: the campaign file this save names is missing or broken, the save cannot be loaded");
			std::abort();
		}
		MC2_LOG(Io, Error, "in-mission save: the logistics data could not be fully restored (code {}), continuing", restored.error());
	}
	LogisticsData::instance->setResourcePoints(rps);
	LogisticsData::instance->publishRules(); // first point the saved rules are read: they replace the old mission's snapshot (current() is stale until here)

	logisticsFile.close();

	removeSaveScratchFile(tmpName);

	// Legacy mech records were restored before the saved variants existed; re-derive their heat capacity now.
	for (auto* mech : std::span(ObjectManager->mechs, ObjectManager->maxMechs))
		if (mech && mech->getExists()) {
			if (!mech->heatCapacityRestored) mech->rederiveHeatCapacity(); // legacy records only
			if (mc2_diag::tagRequested("ALPHA_HEAT"))
				mc2::fmt::print_err("[ALPHA_HEAT v1] event=load mech={} capacity={} heat={} mask={} expire={}\n",
					mech->getWatchID(false), mech->heatState.capacity, mech->heatState.heatMilli,
					mech->heatState.alphaMask.count(), mech->heatState.alphaExpireTick);
		}

	//-------------------------------------------------------------------------------------------
	// Load the CurrentMission number in logistics cause Heidi don't save it with logisticsData.
	long currentMissionNum = 0;
	loadFile.readPacket(currentPacket,reinterpret_cast<MemoryPtr>(&currentMissionNum));

	LogisticsData::instance->setCurrentMissionNum(currentMissionNum);
	currentPacket++;
	//-------------------------------------------------------------------------------------------
    // Save v2 sections follow END (validated in openSaveFile). A v1 file carries the legacy R2 packet in that slot, or
    // nothing; it is upgraded to the same sections in memory, so both versions restore through one path.
    mc2::save::SectionContainer sections;
    if (f.saveV2) {
        if (f.sectionsPacket == currentPacket + 1) sections = std::move(f.sections);
        else mc2::fmt::print_err("[SAVE v2] event=reject reason=section_position file={}\n", mc2::fmt::safe(f.loadFileName));
    } else if (currentPacket + 1 < loadFile.getNumPackets() &&
               loadFile.seekPacket(currentPacket + 1) == NO_ERR && loadFile.getPacketSize() > 0) {
        // Legacy files reserve unused packet slots after END: an empty slot is absent, not an invalid section.
        const auto size = loadFile.getPacketSize();
        std::string why = "r2_size";
        if (size > 0 && size <= static_cast<int>(mc2::save::maxFileBytes)) {
            mc2::save::Bytes bytes(static_cast<size_t>(size));
            loadFile.readPacket(currentPacket + 1, bytes.data());
            if (mc2::save::upgradeLegacyR2(bytes, sections, &why)) why.clear();
        }
        if (!why.empty())
            mc2::fmt::print_err("[SAVE v1] event=reject reason={} file={}\n", why, mc2::fmt::safe(f.loadFileName));
    }
    applySaveSections(mc2::fmt::safe(f.loadFileName), sections, orderIngress.queue());
	loadFile.close();
}

// Restore each known save section, or apply its default when absent (a v1 save, or a v2 save from before the section
// existed). Defaults equal what a v1 load always reconstructed. B2 (BRN1), B3 (FIX1), M5 (WRLD) and PATH add one entry each.
static void applySaveSections(std::string_view fileName, const mc2::save::SectionContainer& sections, mc2::sim::TacticalOrderQueue& orderQueue) {
    turn = mc2::save::SavedMoverLists{}.turn; // Legacy v1 default, shared with WRLD v1.
    s_restoredMusic.reset();
    auto reject = [&](std::string_view reason) {
        mc2::fmt::print_err("[SAVE v2] event=reject reason={} file={}\n", reason, fileName);
    };
    if (const auto* world = sections.find(mc2::save::tagWorld)) {
        mc2::save::Archive archive(world->payload);
        mc2::save::SavedMoverLists lists;
        lists.serialize(archive, world->version);
        auto valid = [](const auto& handles, int kind) {
            std::vector<std::uint8_t> seen(ObjectManager->getMaxObjects() + 1);
            for (auto handle : handles) {
                if (!handle || handle > ObjectManager->getMaxObjects() || seen[handle]) return false;
                auto* object = ObjectManager->get(handle);
                if ((kind == 0 && !dynamic_cast<BattleMechPtr>(object)) ||
                    (kind == 1 && !dynamic_cast<GroundVehiclePtr>(object)) ||
                    (kind == 2 && !dynamic_cast<MoverPtr>(object))) return false;
                seen[handle] = true;
            }
            return true;
        };
        const bool poolsValid = archive.finish() && lists.mechs.size() == ObjectManager->maxMechs &&
            lists.vehicles.size() == ObjectManager->maxVehicles && valid(lists.mechs, 0) && valid(lists.vehicles, 1) &&
            ObjectManager->numMechs >= 0 && ObjectManager->numMechs <= lists.mechs.size() &&
            ObjectManager->numVehicles >= 0 && ObjectManager->numVehicles <= lists.vehicles.size();
        // Active pool prefixes define complete membership. The reconstructed
        // pools are in handle order until WRLD restores their saved ordering.
        mc2::save::SavedMoverLists rebuilt;
        if (poolsValid) {
            auto collect = [&](auto pool, size_t active) {
                for (auto handle : pool.first(active)) {
                    auto* mover = static_cast<MoverPtr>(ObjectManager->get(handle));
                    rebuilt.movers.push_back(handle);
                    if (!mover->getTeam()) continue;
                    if (mover->getTeam()->isFriendly(Team::home)) rebuilt.friendly.push_back(handle);
                    else if (mover->getTeam()->isEnemy(Team::home)) rebuilt.hostile.push_back(handle);
                }
            };
            collect(std::span(lists.mechs), ObjectManager->numMechs);
            collect(std::span(lists.vehicles), ObjectManager->numVehicles);
        }
        if (!poolsValid || !lists.sameMoverMembership(rebuilt) ||
            !valid(lists.movers, 2) || !valid(lists.friendly, 2) || !valid(lists.hostile, 2)) {
            reject("world_section");
        } else {
            turn = lists.turn;
            for (size_t i = 0; i < lists.mechs.size(); ++i)
                ObjectManager->mechs[i] = static_cast<BattleMechPtr>(ObjectManager->get(lists.mechs[i]));
            for (size_t i = 0; i < lists.vehicles.size(); ++i)
                ObjectManager->vehicles[i] = static_cast<GroundVehiclePtr>(ObjectManager->get(lists.vehicles[i]));
            auto restore = [](auto& slots, long& count, const auto& handles) {
                count = static_cast<long>(handles.size());
                for (long i = 0; i < count; ++i)
                    slots[i] = static_cast<MoverPtr>(ObjectManager->get(handles[i]));
            };
            restore(ObjectManager->moverList, ObjectManager->numMovers, lists.movers);
            restore(ObjectManager->goodMoverList, ObjectManager->numGoodMovers, lists.friendly);
            restore(ObjectManager->badMoverList, ObjectManager->numBadMovers, lists.hostile);
            ObjectManager->invalidateSpatialGrid(false);
        }
    } // Legacy saves retain the lists reconstructed from their object records.
    if (const auto* anim = sections.find(mc2::save::tagAnimation)) {
        // restoreAnimationState validates before it mutates, so a rejected section leaves default animation state.
        // Dev saves written between de4413807 and f8b4174c3 carry the v3 record layout under version 2, so a failed v2 parse retries as v3.
        if (!(ObjectManager->restoreAnimationState(anim->payload, anim->version) ||
              (anim->version == 2 && ObjectManager->restoreAnimationState(anim->payload, 3))))
            reject("anim_section");
    } // default: the animation state the restored objects rebuilt from their own records
    if (const auto* orders = sections.find(mc2::save::tagOrders)) {
        mc2::save::Archive archive(orders->payload);
        archive.sectionVersion = orders->version;
        auto queue = orderQueue;
        queue.serialize(archive);
        if (archive.finish()) orderQueue = std::move(queue);
        else reject("orders_section");
    } // default: the empty queue resetOrderIngress left
    if (const auto* brain = sections.find(mc2::save::tagBrain)) {
        if (!mc2::save::restoreBrainSection(brain->payload, brain->version)) reject("brain_section");
    } // default: no brain runtime; the first brain tick allocates it from the mission files, as a v1 load always did
    if (const auto* fix = sections.find(mc2::save::tagFix)) {
        const std::int32_t musicCount = (soundSystem && soundSystem->musicTrackCount() > 0)
            ? static_cast<std::int32_t>(soundSystem->musicTrackCount()) : std::numeric_limits<std::int32_t>::max();
        const auto decoded = mc2::save::decodeValidFixSection(fix->version, fix->payload, MAX_WARRIORS, musicCount);
        if (!decoded) {
            reject("fix_section");  // exactly one [SAVE v2] event=reject line per bad section
        } else {
            const mc2::save::FixSection& value = *decoded;
            s_restoredMusic = value.music;
            for (const auto& b : value.backoffs) {
                MechWarrior* w = MechWarrior::warriorList[b.warrior];
                if (w && w->isUsed()) w->setJumpBackoff(b.goalR, b.goalC, b.age);
            }
            // Requests re-enter the queue in saved order; request() purges a pilot's earlier one, so each pilot keeps its last.
            for (const auto& p : value.paths) {
                MechWarrior* w = MechWarrior::warriorList[p.warrior];
                // request() Fatals on an empty free list; refuse the item instead (a pilot with a queued record reuses it).
                if (PathManager && w && w->isUsed() && (PathManager->freeList || w->getMovePathRequest()))
                    PathManager->request(w, p.selectionIndex, p.moveParams, p.source);
            }
        }
    } // default: music re-armed from the mission tune, no jump backoff, no queued path (the next order re-requests), as a v1 load always did
}

// Activate the camera and finalise the static-prop and mech geometry and the GPU cull buffers.
void Mission::loadActivateAndFinalize() {
	// Quickload replaces the type manager without passing through Mission::init.
	ObjectManager->preloadTickSpawnTypes();
	// Contacts are derived from restored movers; rebuild before ABL observes the first resumed sensor pass.
	const bool enemyInLOS = SensorSystemManager::enemyInLOS;
	for (long team = 0; team < Team::numTeams; ++team)
		SensorManager->getTeamSensor(team)->scanBattlefield();
	SensorSystemManager::enemyInLOS = enemyInLOS;
	// Re-arm the script's music clock after teardown stopped it, before resumed ABL can branch on silence.
	soundSystem->playABLDigitalMusic(missionTuneNum);
	// FIX1: the script's own music view overrides that re-arm (it may have stopped, ended or switched tunes).
	if (s_restoredMusic)
		soundSystem->restoreScriptMusic(s_restoredMusic->id, s_restoredMusic->remaining, s_restoredMusic->loops);
	s_restoredMusic.reset();
	ObjectManager->snapshotMoverPoses();
	ObjectManager->extractRenderSnapshots();
	// activate() re-centres on the first mover; a resumed game keeps the camera it was saved with.
	const glm::vec3 savedCameraPosition = eye->getPosition();
	eye->activate();
	eye->setPosition(savedCameraPosition,false);
	eye->update();
	loadProgress = 100.f;

	DebugGameObject[0] = DebugGameObject[1] = DebugGameObject[2] = nullptr;

	// Restored appearances need immutable geometry before rendering or light prewarm.
	// Quickload also needs the support palette, even when no craft was in flight.
	preloadMissionSupportAtLoad();
	GameAdapters::StaticProp::finalizeGeometry();
	GpuMechBatcher::instance().finalizeGeometry();


	eye->primeActiveLightsForPrewarm();
	prewarmStaticPropLightBakes(*ObjectManager, eye);
	ObjectManager->buildStaticShadowCasterSet(); // quickload cleared the static caster set in onMapLoad; rebuild it as mission_init_load does
	GameAdapters::Vegetation::missionLoaded(land, GameMap);
	// Quickload resumes without Mission::start, which normally arms input and HUD layout.
	MissionInterfaceManager::armHoverTarget();
	gos_SetHudScaleActive(true);
	gos_SetHudCanvasActive(true);
	if (missionCamera) missionCamera->prepareCompass();
	if (missionInterface) missionInterface->getControlGui()->prepareHudFrame();
	mc2::textures::prewarmCpuTextures();
	prewarmMoverGpuTextures(*ObjectManager); prewarmHudSheets();
	mc2PrewarmMissionPipelines();
	mc2::gpumem::log("reload_restore_complete");
	{
		static const bool s_mechRestoreTrace = false;
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

