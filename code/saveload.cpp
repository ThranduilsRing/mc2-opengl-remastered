//******************************************************************************************
//	saveload.cpp - This file contains the mission Save Load class code
//		Missions are what scenarios were in MechCommander 1.
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#include "saveload_internal.h"
#include "save_layout_guard.h"
#include "mc2fmt.h"
#include "mission_load_frame.h"
//----------------------------------------------------------------------------------------------------
// Save.
void Part::Save (FitIniFilePtr file, long partNum)
{
	char partId[1024];
	sprintf(partId,"Part%d", (int)partNum+1);

	file->writeBlock(partId);

	file->writeIdLong("ObjectWID",objectWID);
	file->writeIdULong("ObjectNumber", objNumber);
	file->writeIdULong("BaseColor",baseColor);
	file->writeIdULong("HighlightColor1",highlightColor1);
	file->writeIdULong("HighlightColor2",highlightColor2);
	file->writeIdLong("Active",active);
	file->writeIdLong("Exists",exists);
	file->writeIdBoolean("Destroyed",destroyed);
	file->writeIdFloat("PositionX",position.x);
	file->writeIdFloat("PositionY",position.y);
	file->writeIdFloat("PositionZ",position.z);
	file->writeIdFloat("Velocity",velocity);
	file->writeIdFloat("Rotation",rotation);
	file->writeIdLong("GestureId",gestureId);
	file->writeIdChar("Alignment",alignment);
	file->writeIdChar("TeamId",teamId);
	file->writeIdLong("CommanderId",commanderID);
	file->writeIdLong("SquadId",squadId);
	file->writeIdChar("MyIcon",myIcon);
	file->writeIdULong("ControlType",controlType);
	file->writeIdULong("ControlDataType",controlDataType);
	file->writeIdString("ObjectProfile",profileName);
	file->writeIdULong("Pilot",pilot);
	file->writeIdBoolean("Captureable",captureable);
	file->writeIdULong("VariantNum",variantNum);
}

// QUICKSAVE-TRACE-1 (field bug 2026-07-18: quickload brought up a DIFFERENT
// mission than the one quicksaved). Always-on, tiny (fires only on explicit
// save/load): append path + current mission to <savePath>/saveload_trace.log
// so a bad slot/ordering is diagnosable after the fact.
static void saveloadTrace(const char* op, const char* file, const char* missionFile)
{
	extern char savePath[];
	char tracePath[1024];
	S_snprintf(tracePath, sizeof(tracePath), "%s" PATH_SEPARATOR "saveload_trace.log", savePath);
	if (FILE* tf = fopen(tracePath, "a")) {
		fprintf(tf, "[SAVELOAD] op=%s file=%s mission=%s\n",
			op, file ? file : "(null)", missionFile ? missionFile : "(null)");
		fclose(tf);
	}
}

void Mission::save (const char *saveFileName)
{
	saveloadTrace("save", saveFileName, missionFileName);
	//Check for sufficient hard Drive space on drive game is running from
	char currentPath[1024];
	gos_GetCurrentPath( currentPath, 1023 );
	__int64 driveSpace = gos_GetDriveFreeSpace(currentPath);
	if (driveSpace < (5 * 1024 * 1024))
	{
		soundSystem->playDigitalSample(INVALID_GUI);
		saveloadTrace("save_ABORT_diskspace", saveFileName, missionFileName);
		return;
	}

	userInput->mouseOff();

	DWORD oldXLoc = LoadScreen::xProgressLoc;
	DWORD oldYLoc = LoadScreen::yProgressLoc;

	LoadScreen::xProgressLoc = 0;
	LoadScreen::yProgressLoc = 0;

	bool turnOffAsyncMouse = mc2UseAsyncMouse;
	if ( !mc2UseAsyncMouse )
		MouseTimerInit();
	mc2UseAsyncMouse = true;			
	AsynFunc = ProgressTimer;

	//Get the campaign name for the campaign we are currently playing.
	EString campaignName = LogisticsData::instance->getCampaignName();
	const char *cmpName = campaignName.Data();

	//Open the Save File.
	// Assume path is correct when we get here.
	PacketFile saveFile;
	long result = saveFile.create((char *)saveFileName);

	loadProgress = 1.0f;

	//A thousand packets ought to get it!!
	saveFile.reserve(20000);
	DWORD currentPacket = 0;

	saveFile.writePacket(currentPacket,(MemoryPtr)versionStamp,strlen(versionStamp)+1,STORAGE_TYPE_RAW);
	currentPacket++;

	saveFile.writePacket(currentPacket,(MemoryPtr)cmpName,strlen(cmpName)+1, STORAGE_TYPE_RAW);
	currentPacket++;

	loadProgress = 3.0f;

	//Mission Settings
	// Create a temp fit file and shove it into Packet 0.
	// COmpress the Wee-wee out of it!!
	char tmpString[2048];
	sprintf(tmpString,"%d",timeGetTime());

	FullPathFileName tmpName;
	tmpName.init(savePath,tmpString,".fti");

	FitIniFile tempFile;
	result = tempFile.create(tmpName);

	if (eye)
		eye->save( &tempFile );

	loadProgress = 5.0f;

	if (land)
		land->save( &tempFile );

	loadProgress = 25.0f;

	tempFile.writeBlock( "MissionSettings" );
	tempFile.writeIdString( "MissionName"						, missionFileName);
	tempFile.writeIdFloat( "TimeLimit"							, m_timeLimit );
	tempFile.writeIdLong( "ResourcePoints"						, LogisticsData::instance->getResourcePoints() );
	tempFile.writeIdUChar( "scenarioTuneNum"					, missionTuneNum );

	tempFile.writeIdLong("OperationId"							, operationId);
	tempFile.writeIdLong("MissionId"							, missionId);
	tempFile.writeIdLong("ABLScriptHandle"						, missionBrain->getRealId());
	tempFile.writeIdBoolean("Active"							, active);
	tempFile.writeIdFloat("ActualTime"							, actualTime);
	tempFile.writeIdFloat("RunningTime"							, runningTime);
	tempFile.writeIdFloat("ScenarioTime"						, scenarioTime);
	tempFile.writeIdFloat("DropZoneX"							, dropZone.x);
	tempFile.writeIdFloat("DropZoneY"							, dropZone.y);
	tempFile.writeIdFloat("DropZoneZ"							, dropZone.z);

	tempFile.writeBlock("Parts");
	tempFile.writeIdULong("NumParts",numParts);

	//Must save all of the part data.  Not sure if its ever used after INIT!  Better safe then sorry.
	for (long i=0;i<numParts;i++)
		parts[i].Save(&tempFile,i);

	loadProgress = 35.0f;

	//Save Support Palette data at least from here.
	// VTOL current State, IF Needed.
	// Karnov current State, IF Needed.
	missionInterface->Save( &tempFile );

	loadProgress = 40.0f;

	if (weather)
		weather->save( &tempFile );

	loadProgress = 43.0f;

	tempFile.writeBlock("TheSky");
	tempFile.writeIdLong("SkyNumber"							, theSkyNumber);
	
	Team::home->objectives.Save( &tempFile );

	loadProgress = 50.0f;

	tempFile.writeBlock( "Script" );
	tempFile.writeIdString( "ScenarioScript"					, missionScriptName );

	tempFile.close();

	File dmyFile;
	result = dmyFile.open(tmpName);
	DWORD fileSz = dmyFile.fileSize();
	MemoryPtr tmpRAM = (MemoryPtr)malloc(fileSz);
	dmyFile.read(tmpRAM,fileSz);

	saveFile.writePacket(currentPacket,tmpRAM,fileSz,STORAGE_TYPE_ZLIB);
	currentPacket++;

	dmyFile.close();
	DeleteFile(tmpName);
	free(tmpRAM);
	//---------------------------

	//Terrain - NOT needed, the terrain does not change.
//	land->save( &saveFile, currentPacket, true );
//	currentPacket++;
	//---------------------------

	//Move Data - Needed, changes ALOT
	currentPacket += MOVE_saveData( &saveFile, currentPacket );
	//---------------------------

	loadProgress = 75.0f;

	//ABL
	sprintf(tmpString,"%d",timeGetTime());

	tmpName.init(savePath,tmpString,".abl");

	ABLFile ablFile;
	ablFile.create(tmpName);

	ABLi_saveEnvironment( &ablFile );

	ablFile.close();

	//Create a dummy file to stick in the packet file
	result = dmyFile.open(tmpName);
	fileSz = dmyFile.fileSize();
	tmpRAM = (MemoryPtr)malloc(fileSz);
	dmyFile.read(tmpRAM,fileSz);

	saveFile.writePacket(currentPacket,tmpRAM,fileSz,STORAGE_TYPE_ZLIB);
	currentPacket++;

	dmyFile.close();
	DeleteFile(tmpName);
	free(tmpRAM);
	//---------------------------

	loadProgress = 80.0f;

	//MechWarrior Manager
	// Save Number of Warriors.
	// Call Save for each Warrior on list.
	currentPacket = MechWarrior::Save( &saveFile, currentPacket );

	loadProgress = 85.0f;

	//Team Manager
	// Save Number of Teams
	// Save which number is HOME
	// Call Save for each team.
	currentPacket = Team::Save( &saveFile, currentPacket );

	//Object Manager
	// Save numbers of objects in all object lists in game
	// Save ClassID of objects IN the list.
	// For each object in each list:
	//		If NULL, save a magic Number to tell me this entry is NULL.
	//		If NOT NULL, 
	// 			Call Save DOWN the heirarchy.
	//
	currentPacket = ObjectManager->Save( &saveFile, currentPacket );

	loadProgress = 95.0f;

	//Commander Manager
	// Save Number of Commanders.
	// Save which number is HOME.
	// Call save for each commander.
	currentPacket = Commander::Save( &saveFile, currentPacket );

	//Logistics Manager
	// Save the Logistics Data
	FitIniFile tmpSaveFile;
	tmpSaveFile.create(tmpName);

	LogisticsData::instance->save(tmpSaveFile);

	tmpSaveFile.close();

	loadProgress = 99.0f;

	//Create a dummy file to stick in the packet file
	result = dmyFile.open(tmpName);
	fileSz = dmyFile.fileSize();
	tmpRAM = (MemoryPtr)malloc(fileSz);
	dmyFile.read(tmpRAM,fileSz);

	saveFile.writePacket(currentPacket,tmpRAM,fileSz,STORAGE_TYPE_ZLIB);
	currentPacket++;

	dmyFile.close();

	DeleteFile(tmpName);
	free(tmpRAM);
	//---------------------------

	//-------------------------------------------------------------------------------------------
	// Save the CurrentMission number in logistics cause Heidi don't save it with logisticsData.
	long currentMissionNum = LogisticsData::instance->getCurrentMissionNum();
	saveFile.writePacket(currentPacket,(MemoryPtr)&currentMissionNum,4,STORAGE_TYPE_RAW);
	currentPacket++;

	//-------------------------------------------------------------------------------------------
	saveFile.writePacket(currentPacket,(MemoryPtr)"END",4,STORAGE_TYPE_RAW);
	currentPacket++;

	saveFile.close();

	//YIKES!!  We could be checking the if before the null and executing after!!  Block the thread!
	//Wait for thread to finish.
	while (mc2IsInMouseTimer)
		;

	//ONLY set the mouse BLT data at the end of each update.  NO MORE FLICKERING THEN!!!
	// BLOCK THREAD WHILE THIS IS HAPPENING
	mc2IsInDisplayBackBuffer = true;

	AsynFunc = NULL;
	mc2UseAsyncMouse = turnOffAsyncMouse;
	if ( !mc2UseAsyncMouse)
		MouseTimerKill();

	mc2IsInDisplayBackBuffer = false;

	LoadScreen::xProgressLoc = oldXLoc;
	LoadScreen::yProgressLoc = oldYLoc;

	userInput->mouseOn();
}

//----------------------------------------------------------------------------------------------------
// Load.
void Part::Load (FitIniFilePtr file, long partNum)
{
	char partId[1024];
	sprintf(partId,"Part%d", (int)partNum);

	long result = file->seekBlock(partId);
	if (result != NO_ERR)
		STOP(("Part Number %d missing from In-Mission Save",partNum));

	result = file->readIdInt("ObjectWID", objectWID);
	if (result != NO_ERR)
		STOP(("Part %d ObjectWID missing from In-Mission Save",partNum));

	result = file->readIdULong("ObjectNumber", objNumber);
	if (result != NO_ERR)
		STOP(("Part %d ObjectNumber missing from In-Mission Save",partNum));

	result = file->readIdULong("BaseColor",baseColor);
	if (result != NO_ERR)
		STOP(("Part %d BaseColor missing from In-Mission Save",partNum));

	result = file->readIdULong("HighlightColor1",highlightColor1);
	if (result != NO_ERR)
		STOP(("Part %d HighlightColor1 missing from In-Mission Save",partNum));

	result = file->readIdULong("HighlightColor2",highlightColor2);
	if (result != NO_ERR)
		STOP(("Part %d HighlightColor2 missing from In-Mission Save",partNum));

	result = file->readIdLong("Active",active);
	if (result != NO_ERR)
		STOP(("Part %d Active missing from In-Mission Save",partNum));

	result = file->readIdLong("Exists",exists);
	if (result != NO_ERR)
		STOP(("Part %d Exists missing from In-Mission Save",partNum));

	result = file->readIdBoolean("Destroyed",destroyed);
	if (result != NO_ERR)
		STOP(("Part %d Destroyed missing from In-Mission Save",partNum));

	result = file->readIdFloat("PositionX",position.x);
	if (result != NO_ERR)
		STOP(("Part %d PositionX missing from In-Mission Save",partNum));

	result = file->readIdFloat("PositionY",position.y);
	if (result != NO_ERR)
		STOP(("Part %d PositionY missing from In-Mission Save",partNum));

	result = file->readIdFloat("PositionZ",position.z);
	if (result != NO_ERR)
		STOP(("Part %d PositionZ missing from In-Mission Save",partNum));

	result = file->readIdFloat("Velocity",velocity);
	if (result != NO_ERR)
		STOP(("Part %d Velocity missing from In-Mission Save",partNum));

	result = file->readIdFloat("Rotation",rotation);
	if (result != NO_ERR)
		STOP(("Part %d Rotation missing from In-Mission Save",partNum));

	result = file->readIdULong("GestureId",gestureId);
	if (result != NO_ERR)
		STOP(("Part %d Rotation missing from In-Mission Save",partNum));

	result = file->readIdChar("Alignment",alignment);
	if (result != NO_ERR)
		STOP(("Part %d Alignment missing from In-Mission Save",partNum));

	result = file->readIdChar("TeamId",teamId);
	if (result != NO_ERR)
		STOP(("Part %d TeamId missing from In-Mission Save",partNum));

	result = file->readIdLong("CommanderId",commanderID);
	if (result != NO_ERR)
		STOP(("Part %d CommanderId missing from In-Mission Save",partNum));

	result = file->readIdChar("SquadId",squadId);
	if (result != NO_ERR)
		STOP(("Part %d SquadId missing from In-Mission Save",partNum));

	result = file->readIdChar("MyIcon",myIcon);
	if (result != NO_ERR)
		STOP(("Part %d MyIcon missing from In-Mission Save",partNum));

	result = file->readIdULong("ControlType",controlType);
	if (result != NO_ERR)
		STOP(("Part %d ControlType missing from In-Mission Save",partNum));

	result = file->readIdULong("ControlDataType",controlDataType);
	if (result != NO_ERR)
		STOP(("Part %d ControlDataType missing from In-Mission Save",partNum));

	result = file->readIdString("ProfileName",profileName,8);
	if (result != NO_ERR)
		STOP(("Part %d ProfileName missing from In-Mission Save",partNum));

	result = file->readIdULong("Pilot",pilot);
	if (result != NO_ERR)
		STOP(("Part %d Pilot missing from In-Mission Save",partNum));

	result = file->readIdBoolean("Captureable",captureable);
	if (result != NO_ERR)
		STOP(("Part %d Captureable missing from In-Mission Save",partNum));

	result = file->readIdULong("VariantNum",variantNum);
	if (result != NO_ERR)
		STOP(("Part %d VeriantNum missing from In-Mission Save",partNum));

}


// Trace the load, silence the mouse and start the loading-screen timer.
void Mission::loadPrologue(MissionLoadFrame& f) {
	auto& loadFileName = f.loadFileName; auto& oldXLoc = f.oldXLoc; auto& oldYLoc = f.oldYLoc;
	auto& turnOffAsyncMouse = f.turnOffAsyncMouse;
	saveloadTrace("load_begin_currentMission", loadFileName, missionFileName);
	extern int g_lightProbeSetupPath; g_lightProbeSetupPath = 2; // [GPUPROPS v1]
	// F3 CPU projection cost-baseline: flush previous-mission samples and
	// clear ring buffer. No-op when env OFF.
	::mc2_cpu_proj_cost::hard_reset("Mission::load");
	userInput->mouseOff();
	loadProgress = 0.0f;

	oldXLoc = LoadScreen::xProgressLoc;
	oldYLoc = LoadScreen::yProgressLoc;

	LoadScreen::xProgressLoc = 0;
	LoadScreen::yProgressLoc = 0;

	turnOffAsyncMouse = mc2UseAsyncMouse;
	if ( !mc2UseAsyncMouse )
		MouseTimerInit();
	mc2UseAsyncMouse = true;			
	AsynFunc = ProgressTimer;
}

// Open the save packet file and reject a missing file or a wrong version stamp.
bool Mission::openSaveFile(MissionLoadFrame& f) {
	auto& loadFileName = f.loadFileName; auto& loadFile = f.loadFile; auto& result = f.result;
	auto& currentPacket = f.currentPacket;
	//Open the In-Mission Save packet file.
	result = loadFile.open((char *)loadFileName);
	if (result != NO_ERR)
		return true;		//Can't load.  No File.  Dialog?  Probably not.

	currentPacket = 0;

	char versionCheck[1024];
	loadFile.readPacket(currentPacket,(MemoryPtr)versionCheck);
	currentPacket++;

	if (strcmp(versionCheck,versionStamp) != 0)
	{
		char msg[2048];
		cLoadString(IDS_QUICKSAVE_VERSION_WRONG,msg,2047);
		PAUSE((msg));
		return true;
	}

	loadFile.seekPacket(currentPacket);
	char *campName = new char [loadFile.getPacketSize()+1];
	loadFile.readPacket(currentPacket,(MemoryPtr)(campName));
	currentPacket++;

	//Get the campaign name for the campaign we are currently playing.
	EString campaignName = LogisticsData::instance->getCampaignName();
	const char *cmpName = campaignName.Data();

	/*
	if (stricmp(campName,cmpName) != 0)
	{
		char msg[2048];
		cLoadString(IDS_QUICKSAVE_CAMPAIGN_WRONG,msg,2047);
		PAUSE((msg,campName,cmpName));
		return true;
	}
	*/

	delete [] campName;
	campName = NULL;

	// A save written by another build can hold object records this build cannot read. Refuse it here, while
	// the running mission is still intact.
	if (const auto why = GameObjectManager::checkSavedObjects(&loadFile)) {
		mc2::fmt::print_err("[SAVE v1] event=reject reason={} file={}\n", *why, mc2::fmt::safe(loadFileName));
		char msg[2048];
		cLoadString(IDS_QUICKSAVE_VERSION_WRONG,msg,2047);
		PAUSE((msg));
		return true;
	}
	return false;
}

// Tear down the running mission and reset the mech and static-prop batchers.
void Mission::loadDestroyAndRestore() {
	//Used to call this before the version check.  Wow.
	destroy();

	{
		static const bool s_mechRestoreTrace = mc2::gate::get<"MC2_MECH_RESTORE_TRACE">();
		if (s_mechRestoreTrace)
			fprintf(stderr,
				"[MECHRESTORE v1] event=saveload_phase phase=post_destroy "
				"mechFinalized=%d appearanceListNull=%d\n",
				GpuMechBatcher::instance().isFinalized() ? 1 : 0,
				(appearanceTypeList == NULL) ? 1 : 0);
	}

	// VPL-deferred item 11 FIX (pre-spawn half — completes the tail-side
	// finalizeGeometry() below). destroy() above ran Mission::destroy ->
	// GpuMechBatcher::onMapUnload() (mission.cpp:3260), which frees GL
	// buffers + clears s_geometryFinalized but does NOT clear the CPU
	// s_typeLodIndex / s_typeLodRecords / staging (only onMapLoad()
	// does). destroy() also UNCONDITIONALLY deletes appearanceTypeList
	// (mission.cpp:3438-3443). The mechs respawned below via
	// ObjectManager->Load heap-reuse freed Mech3DAppearanceType
	// addresses; registerTypeLod()'s idempotent guard
	// (gos_mech_batcher.cpp:337, key = {Mech3DAppearanceType*, lod})
	// then false-hits the stale entry and SILENTLY SKIPS registering the
	// reused-address type -> that type's mechs are invisible from a
	// savegame. Mission::init avoids this by calling onMapLoad() BEFORE
	// any spawn (mission.cpp:1682-1683); Mission::load is a parallel
	// path that skipped it. Mirror Init's pre-spawn reset exactly here
	// (after destroy(), before any ::Load respawn) so registration
	// rebuilds into a clean index; the finalizeGeometry() tail below is
	// Init's matching post-spawn half.
	GameAdapters::StaticProp::resetForRestore();
	GpuMechBatcher::instance().onMapLoad();

	loadProgress = 1.0f;
}

// Reset the turn counters and bring up ABL, the TGL and the mission heap.
void Mission::loadResetSimState(MissionLoadFrame& f) {
	auto& tmpString = f.tmpString;
	sprintf(tmpString,"%d",timeGetTime());

	//-------------------------------------------
	//Relationships saved with TEAMs!
				
	//-------------------------------------------
	// Always reset turn at scenario start
	turn = 0;
	terminationCounterStarted = 0;

	//-----------------------
	// Init the ABL system...
	initABL();

	initBareMinimum();
	loadProgress = 4.0f;
	initTGLForMission();
}

// Open gamesys.fit and read the weapon ranges and the general visual / sensor settings.
void Mission::loadGameSystemRanges(MissionLoadFrame& f) {
	auto& result = f.result; auto& gameSystemFile = f.gameSystemFile; auto& forestMoveCost = f.forestMoveCost;
	//--------------------------------------------------------------
	// Start the Mission Heap
	missionHeap = new UserHeap;
	gosASSERT(missionHeap != NULL);
	
	missionHeap->init(missionHeapSize,"MISSION");
	
	//--------------------------
	// Load Game System stuff...
	FullPathFileName fullGameSystemName;
	fullGameSystemName.init(missionPath, "gamesys", ".fit");
	
	gameSystemFile = new FitIniFile;
	gosASSERT(gameSystemFile != NULL);
		
	result = gameSystemFile->open(fullGameSystemName);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->seekBlock("WeaponRanges");
	gosASSERT(result == NO_ERR);

	float span[2];
	result = gameSystemFile->readIdFloatArray("Short", span, 2);
	gosASSERT(result == NO_ERR);
	WeaponRanges[WEAPON_RANGE_SHORT][0] = span[0];
	WeaponRanges[WEAPON_RANGE_SHORT][1] = span[1];

	result = gameSystemFile->readIdFloatArray("Medium", span, 2);
	gosASSERT(result == NO_ERR);
	WeaponRanges[WEAPON_RANGE_MEDIUM][0] = span[0];
	WeaponRanges[WEAPON_RANGE_MEDIUM][1] = span[1];

	result = gameSystemFile->readIdFloatArray("Long", span, 2);
	gosASSERT(result == NO_ERR);
	WeaponRanges[WEAPON_RANGE_LONG][0] = span[0];
	WeaponRanges[WEAPON_RANGE_LONG][1] = span[1];

	result = gameSystemFile->readIdFloatArray("OptimalRangePoints", OptimalRangePoints, 5);
	gosASSERT(result == NO_ERR);

	for (long i = 0; i < 5; i++)
		for (long j = 0; j < 3; j++) {
			OptimalRangePointInRange[i][j] = false;
			if (OptimalRangePoints[i] > WeaponRanges[j][0])
				if (OptimalRangePoints[i] <= WeaponRanges[j][1])
					OptimalRangePointInRange[i][j] = true;
		}

	result = gameSystemFile->seekBlock("General");
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloat("MaxVisualRange",maxVisualRange);
	gosASSERT(result == NO_ERR);
	MaxVisualRadius = maxVisualRange * 1.4142;

	result = gameSystemFile->readIdFloat("FireVisualRange",fireVisualRange);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloatArray("WeaponRange", WeaponRange, NUM_FIRERANGES);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloat("DefaultAttackRange", DefaultAttackRange);
	if (result != NO_ERR)
		DefaultAttackRange = 75.0;

	result = gameSystemFile->readIdFloat("BaseSensorRange",baseSensorRange);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdLongArray("VisualRangeTable",visualRangeTable,256);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloat("BaseHeadShotElevation",BaseHeadShotElevation);
	if (result != NO_ERR)
		BaseHeadShotElevation = 1.0f;

	result = gameSystemFile->readIdLong("ForestMoveCost", forestMoveCost);
	gosASSERT(result == NO_ERR);
}

// Read the component list, the god-mode / footprint flags and the mover game-system blocks.
void Mission::loadGameSystemGeneral(MissionLoadFrame& f) {
	auto& result = f.result; auto& gameSystemFile = f.gameSystemFile;
	//----------------------------------------------------------------------
	// Now that we have some base values, load the master component table...
	if (!MasterComponent::masterList) {
		FullPathFileName compFileName;
		compFileName.init(objectPath,"compbas",".csv");
#ifdef _DEBUG
		long loadErr = 
#endif
			MasterComponent::loadMasterList(compFileName, 255, baseSensorRange);
		gosASSERT(loadErr == NO_ERR);

		// EQUIPMENT-SOURCE-FLIP-SWITCH-1: gated green-stat overlay (no-op OFF).
		overlayGeneratedEquipmentStatsIfEnabled();
		// EQUIP-ADD-FILL-1 (S2): gated ADD pass -- mod defs fill UNDEFINED
		// compbas ordinals (no-op OFF). Before the registry rebuild below.
		applyEquipmentAddFillIfEnabled(baseSensorRange);
		// EQUIPMENT-REGISTRY-FACADE-1: gated parallel registry build (no-op OFF).
		buildAndDumpEquipmentRegistryIfEnabled();
		// FIRE-EXECUTOR-REGISTRY-1 (E7): gated fireExecutor lookup proof (no-op OFF).
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
	gosASSERT(result == NO_ERR);
	
	result = gameSystemFile->readIdLong("BonusPointsPerTon",resourcesPerTonDivided);
	gosASSERT(result == NO_ERR);
	
#ifndef FINAL
 	result = gameSystemFile->readIdFloat("CheatHitDamage",CheatHitDamage);
	if (result != NO_ERR)
		CheatHitDamage = 5.0f;
#endif
	
	//---------------------------------------
	// Read in difficulty here if it exits.
	InitDifficultySettings(gameSystemFile);

	result = Mover::loadGameSystem(gameSystemFile, maxVisualRange);
	gosASSERT(result == NO_ERR);

	result = BattleMech::loadGameSystem(gameSystemFile);
	gosASSERT(result == NO_ERR);

	//--------------------------------------------------------------------
	result = GroundVehicle::loadGameSystem(gameSystemFile);
	gosASSERT(result == NO_ERR);
}

// Read the mine, smoke and fire settings.
void Mission::loadGameSystemHazards(MissionLoadFrame& f) {
	auto& result = f.result; auto& gameSystemFile = f.gameSystemFile;
	result = gameSystemFile->seekBlock("Mine");
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloat("BaseDamage", MineDamage);
	gosASSERT(result == NO_ERR);
		
	result = gameSystemFile->readIdFloat("SplashDamage", MineSplashDamage);
	gosASSERT(result == NO_ERR);
		
	result = gameSystemFile->readIdFloat("SplashRange", MineSplashRange);
	gosASSERT(result == NO_ERR);
		
	result = gameSystemFile->readIdLong("Explosion", MineExplosion);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdLong("MineLayThrottle", MineLayThrottle);
	if (result != NO_ERR)
		MineLayThrottle = 50;

	result = gameSystemFile->readIdLong("MineSweepThrottle", MineSweepThrottle);
	if (result != NO_ERR)
		MineSweepThrottle = 50;

	result = gameSystemFile->readIdFloat("MineWaitTime", MineWaitTime);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloat("StrikeWaitTime", StrikeWaitTime);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdFloat("StrikeTimeToImpact", StrikeTimeToImpact);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->seekBlock("Smoke");
	gosASSERT(result == NO_ERR);
	
	result = gameSystemFile->readIdLong("MaxSmokeSpheres",totalSmokeSpheres);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->readIdLong("TotalSmokeShapeSize",totalSmokeShapeSize);
	gosASSERT(result == NO_ERR);

	result = gameSystemFile->seekBlock("Fire");
	gosASSERT(result == NO_ERR);
	
	result = gameSystemFile->readIdLong("MaxFiresBurning", maxFiresBurning);
	gosASSERT(result == NO_ERR);
	
	result = gameSystemFile->readIdFloat("MaxFireBurnTime", maxFireBurnTime);
	gosASSERT(result == NO_ERR);
}

// Unpack the saved mission .fti and read the mission settings, the sky and the script name.
void Mission::loadMissionSettings(MissionLoadFrame& f) {
	auto& loadFileName = f.loadFileName; auto& loadFile = f.loadFile; auto& result = f.result;
	auto& currentPacket = f.currentPacket; auto& tmpString = f.tmpString; auto& tmpName = f.tmpName;
	auto& missionFile = f.missionFile; auto& rps = f.rps;
	//---------------------------------------------------------------------------------
	// OK, load up the in_mission Save mission data
	if (loadFile.seekPacket(currentPacket) != NO_ERR)
		STOP(("Mission Data missing from IN-Mission Save"));

	duration = 60;
	
	tmpName.init(savePath,tmpString,".fti");
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

	result = missionFile.open(tmpName);
	if (result != NO_ERR)
		STOP(("Mission Data missing from IN-Mission Save 2"));

	//--------------------------------
	//Load up the mission Settings.
	if (missionFile.seekBlock( "MissionSettings" ) != NO_ERR)
		STOP(("MissionSettings missing from IN-Mission Save"));

	result = missionFile.readIdString( "MissionName"					, missionFileName,79);
	if (result != NO_ERR)
		STOP(("Mission Name missing from IN-Mission Save"));
	saveloadTrace("load_slotMission", loadFileName, missionFileName);

	result = missionFile.readIdFloat( "TimeLimit"						, m_timeLimit );
	if (result != NO_ERR)
		STOP(("Time Limit  missing from IN-Mission Save"));

	result = missionFile.readIdLong( "ResourcePoints"					, rps );
	if (result != NO_ERR)
		STOP(("Resource Points missing from IN-Mission Save"));

	result = missionFile.readIdUChar( "scenarioTuneNum"					, missionTuneNum );
	if (result != NO_ERR)
		STOP(("Scenario Tune Num missing from IN-Mission Save"));

	result = missionFile.readIdLong("OperationId"						, operationId);
	if (result != NO_ERR)
		STOP(("Operation ID missing from IN-Mission Save"));

	result = missionFile.readIdLong("MissionId"							, missionId);
	if (result != NO_ERR)
		STOP(("MissionId missing from IN-Mission Save"));

	result = missionFile.readIdLong("ABLScriptHandle"					, missionScriptHandle);
	if (result != NO_ERR)
		STOP(("ABL Script Handle missing from IN-Mission Save"));

	result = missionFile.readIdBoolean("Active"							, active);
	if (result != NO_ERR)
		STOP(("Active flag missing from IN-Mission Save"));

	result = missionFile.readIdFloat("ActualTime"						, actualTime);
	if (result != NO_ERR)
		STOP(("Actual Time missing from IN-Mission Save"));

	result = missionFile.readIdFloat("RunningTime"						, runningTime);
	if (result != NO_ERR)
		STOP(("Running Time missing from IN-Mission Save"));

	result = missionFile.readIdFloat("ScenarioTime"						, scenarioTime);
	if (result != NO_ERR)
		STOP(("ScenarioTime missing from IN-Mission Save"));

	result = missionFile.readIdFloat("DropZoneX"						, dropZone.x);
	if (result != NO_ERR)
		STOP(("DropZoneX missing from IN-Mission Save"));

	result = missionFile.readIdFloat("DropZoneY"						, dropZone.y);
	if (result != NO_ERR)
		STOP(("DropZoneY missing from IN-Mission Save"));

	result = missionFile.readIdFloat("DropZoneZ"						, dropZone.z);
	if (result != NO_ERR)
		STOP(("DropZoneZ missing from IN-Mission Save"));

	//-----------------------------------
	// Find the SKY Number and save it.
	// If no number, i.e. an old mission file,
	// simply make it 1 until its written out
	// in the magical editor (tm)
	result = missionFile.seekBlock("TheSky");
	if (result != NO_ERR)
		theSkyNumber = DEFAULT_SKY;
	else
	{
		result = missionFile.readIdLong("SkyNumber",theSkyNumber);
		if (result != NO_ERR)
			theSkyNumber = DEFAULT_SKY;
			
		if ((theSkyNumber < 1) || (theSkyNumber > 20))
			theSkyNumber = DEFAULT_SKY;
	}
		
	result = missionFile.seekBlock( "Script" );
	if (result != NO_ERR)
		STOP(("Mission ABL Script missing from In-Mission Save"));
	else
	{
		result = missionFile.readIdString( "ScenarioScript"					, missionScriptName,79 );
		if (result != NO_ERR)
			missionScriptName[0] = 0;
	}
}
// In-mission save restore. Each phase is a member helper declared in mission.h, run in the original order, so the
// packets are read from the save file in the order Mission::save wrote them; the shared locals live in
// MissionLoadFrame.
void Mission::load (const char *loadFileName)
{
	MissionLoadFrame f;
	f.loadFileName = loadFileName;
	loadPrologue(f);

	if (openSaveFile(f)) {
		loadRestoreInput(f);
		return;
	}

	loadDestroyAndRestore();
	loadResetSimState(f);
	loadGameSystemRanges(f);
	loadGameSystemGeneral(f);
	loadGameSystemHazards(f);
	loadMissionSettings(f);
	loadAblAndManagers(f);
	loadTerrain(f);
	loadMovement(f);
	loadMissionScript(f);
	loadWarriorsAndPartsHeader(f);
	loadParts(f);
	loadTeamsAndObjects(f);
	loadObjectivesCommanders(f);
	loadWeatherCamera(f);
	loadInterface(f);
	loadLogistics(f);
	loadActivateAndFinalize();
	loadRestoreInput(f);
}

//----------------------------------------------------------------------------------------------------

