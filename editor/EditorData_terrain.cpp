// EditorData: terrain creation from PCV/TGA/generated recipes, height edits and the scaled colour map.
// Declared in the class headers; shared names in EditorData_internal.h.
#include "EditorData_internal.h"
#include "mc2fmt.h"
#include "mc2str.h"
#include "../GameOS/gameos/load_scope.h"


static void EditorDataWarmTerrain(const char* where)
{
	EditorDataTrace("EditorDataWarmTerrain: enter where=%s land=%p eye=%p", where ? where : "<null>", land, eye);

	if (!land || !eye)
	{
		EditorDataTrace("EditorDataWarmTerrain: skipped missing land/eye");
		return;
	}

	// The MFC editor paint path renders via eye->render(), but the terrain
	// render lists are prepared by the normal game tick:
	// Editor::update() -> eye->update() -> land->update() -> land->geometry().
	// The editor cannot safely run the full DoGameLogic() tick from OnPaint
	// in this port, so prime only the terrain/camera data immediately after
	// loading/creating a map.
	eye->update();
	EditorDataTrace("EditorDataWarmTerrain: after eye->update");

	land->update();
	EditorDataTrace("EditorDataWarmTerrain: after land->update");

	land->clearObjBlocksActive();
	EditorDataTrace("EditorDataWarmTerrain: after clearObjBlocksActive");

	land->geometry();
	EditorDataTrace("EditorDataWarmTerrain: after land->geometry");

	EditorObjectMgr::instance()->update();
	EditorDataTrace("EditorDataWarmTerrain: after EditorObjectMgr::update");
}


bool EditorData::initTerrainFromPCV( const char* fileName )
{
	mc2::load::begin();
	struct LoadReport {
		ULONGLONG start = GetTickCount64();
		~LoadReport() {
			EditorDataTrace("[LOAD_SCOPES v1] host=editor total_ms=%llu%s", GetTickCount64() - start, mc2::load::report().c_str());
		}
	} loadReport;
	EditorDataTrace("EditorData::initTerrainFromPCV: enter fileName=%s", fileName ? fileName : "<null>");
	if (!fileName || !fileName[0]) return false;
	EditorFit::MissionTextReader file;
	const auto previousMissionName = instance->MissionName();
	const auto previousAuthor = instance->Author();
	const auto previousBlurb = instance->Blurb();
	const auto previousBlurb2 = instance->Blurb2();
	const auto previousVideo = instance->VideoFilename();
	const auto previousURL = instance->DownloadURL();
	const std::string previousScript = missionScriptName;
	if (fileName && fileName[0])
	{
		auto fitPath = mc2::str::from_c(fileName);
		const auto extension = fitPath.rfind('.');
		if (extension != fitPath.npos) fitPath.replace(extension, fitPath.npos, ".fit");
        if (file.open(fitPath.c_str()) != NO_ERR
            || !file.requiredFields(GAME_MAX_PLAYERS, g_conditionSpeciesStringArray, g_actionSpeciesStringArray)) return false;
	}

	// Objective models and payloads belong to a candidate, so rejection retains the live map and history.
	CTeams candidateTeams;
	file.deferObjectReferences = true;
	if (!candidateTeams.Read(&file)) return false;
	file.deferObjectReferences = false;

	char lowerFileName[1024];
	strncpy(lowerFileName, fileName, sizeof(lowerFileName) - 1);
	lowerFileName[sizeof(lowerFileName) - 1] = 0;
	_strlwr(lowerFileName);
	EditorDataTrace("EditorData::initTerrainFromPCV: normalized fileName=%s", lowerFileName);

	PacketFile pFile;
	EditorDataTrace("EditorData::initTerrainFromPCV: before PacketFile::open");
	int result = pFile.open( lowerFileName );
	EditorDataTrace("EditorData::initTerrainFromPCV: after PacketFile::open result=%d", result);
	if ( result != NO_ERR )
	{
		char buffer[512];
		EditorSafeLoadString( IDS_COULDNT_OPEN, buffer, 256 , gameResourceHandle);
		char buffer2[512];
		sprintf( buffer2, buffer, lowerFileName );
		/*I think MessageBox() would not be modal wrt the application.*/
		if (!g_cliSuppressModals) AfxMessageBox(buffer2);
		EditorInterface::instance()->UnsetBusyMode();
		bIsLoading = false;
		return false;
	}

 	CString camFileName = lowerFileName;
	camFileName.MakeLower();
	camFileName.Replace( ".pak", ".fit" );

	EditorDataTrace("EditorData::initTerrainFromPCV: camFileName=%s", (const char*)camFileName);
	if ( fileExists( (char*)(const char*)camFileName ) )
	{
		EditorDataTrace("EditorData::initTerrainFromPCV: before cam file open");
		long fitResult = file.isOpen() ? NO_ERR : file.open( (char*)(const char*)camFileName );
		EditorDataTrace("EditorData::initTerrainFromPCV: after cam file open result=%ld", fitResult);
	}
	else
	{
		char buffer[512];
		EditorSafeLoadString( IDS_COULDNT_OPEN, buffer, 256, gameResourceHandle );
		char buffer2[512];
		sprintf( buffer2, buffer, (LPCTSTR)camFileName );   // an MFC CString goes through "..." as its character pointer
		/*I think MessageBox() would not be modal wrt the application.*/
		if (!g_cliSuppressModals) AfxMessageBox(buffer2);
		EditorInterface::instance()->UnsetBusyMode();
		bIsLoading = false;
		return false;
	
	}

	EditorDataTrace("EditorData::initTerrainFromPCV: before new Terrain");
	bool bRetVal = true;
	bIsLoading = true;
	EditorInterface::instance()->SetBusyMode(false/*no redraw*/);
	clear();
	const auto rejectDocument = [] {
		clear();
		if (eye) eye->active = false;
		EditorInterface::instance()->UnsetBusyMode();
		bIsLoading = false;
		return false;
	};
	land = new Terrain( );
	EditorDataTrace("EditorData::initTerrainFromPCV: after new Terrain land=%p", land);

	EditorDataTrace("EditorData::initTerrainFromPCV: before getColorMapName");
	land->getColorMapName(&file);
	EditorDataTrace("EditorData::initTerrainFromPCV: after getColorMapName");

	volatile float crap = 0;
	EditorDataTrace("EditorData::initTerrainFromPCV: before land->init");
	if ( NO_ERR != land->init( &pFile, 0, EDITOR_VISIBLE_VERTICES, crap, 100 ) )
	{
		EditorInterface::instance()->UnsetBusyMode();
		bIsLoading = false;
		 return false;
	}

	EditorDataTrace("EditorData::initTerrainFromPCV: after land->init realVerticesMapSide=%ld", land->realVerticesMapSide);
	EditorDataTrace("EditorData::initTerrainFromPCV: before eye init");
	eye->init();
	if (!EditorReadMissionCamera(file))
	{
		return rejectDocument();
	}
	eye->init( &file );
	EditorDataTrace("EditorData::initTerrainFromPCV: after eye init");

	// S2 mission-load chain — mirrors code/mission.cpp:1693-1695 + 2804-2839 + 3136-3143.
	// Canonical order locked by docs/superpowers/plans/2026-05-25-editor-rebuild-S0-contract.md.
	// Must run BEFORE loadMechs/loadDropZones/loadForests/EditorObjectMgr::load() so
	// Mech3DAppearanceType::init() + BldgAppearance::init() + TreeAppearance::init()
	// registrations (which fire transitively from addBuilding -> getAppearance during
	// those loaders) land against an armed batcher + registry state. S2.12 fix: prior
	// ordering had loadMechs() before this block, causing onMapLoad() to clear
	// s_typeLodIndex/s_typeLodRecords AFTER mechs registered, producing
	// "[MECHBATCHER v1] event=finalize_empty — no types registered" and a forced
	// MLR fallback rendering path.
	// clear() ran at the top of this function, which already ran
	// onMapUnload/endMission/destroy for any prior session.

	EditorBridge::beginMissionRenderResources();              // steps 1-3 — game lines 1693-1695
	GameAdapters::StaticProp::beginMission();
	GameAdapters::Mech::beginMission();
	EditorDataTrace("EditorData::initTerrainFromPCV: GPU batchers + registry armed for map load");

	// The editor has no GameObjectManager; cull substrate and readback capacity
	// include actor and static-prop headroom for placements after loading.
	const uint32_t kEditorMaxActors = 2048u;
	const uint32_t kStaticPropHeadroom = 8192u;
	gpu_cull::substrate_init(kEditorMaxActors + kEditorMaxActors / 4u + kStaticPropHeadroom);
	EditorBridge::cullComputeInit();
	gos_terrain_lighting::mission_init(
		static_cast<uint32_t>(Terrain::realVerticesMapSide * Terrain::realVerticesMapSide),
		64u);
	gpu_cull::readback_init(kEditorMaxActors + kEditorMaxActors / 4u + kStaticPropHeadroom);
	// Reset terrain shadow priming so a previous mission cannot leave stale
	// depth ordering between water and terrain shadow surfaces.

	EditorDataTrace("EditorData::initTerrainFromPCV: gpu_cull substrate/compute/readback + terrain_lighting + shadow priming initialized");

	EditorDataTrace("EditorData::initTerrainFromPCV: before loadMechs");
	if (!EditorObjectMgr::instance()->loadMechs(file))
	{
		return rejectDocument();
	}
	EditorDataTrace("EditorData::initTerrainFromPCV: before loadDropZones");
	EditorObjectMgr::instance()->loadDropZones( file );
	EditorDataTrace("EditorData::initTerrainFromPCV: before loadForests");
	EditorObjectMgr::instance()->loadForests( file );
	EditorDataTrace("EditorData::initTerrainFromPCV: before land->load");
	land->load( &file ) && bRetVal;
	EditorDataTrace("EditorData::initTerrainFromPCV: after land->load bRetVal=%d", bRetVal ? 1 : 0);
	EditorDataTrace("EditorData::initTerrainFromPCV: before recalcWater");
	land->recalcWater();

	// S2.8 — mirrors code/mission.cpp:2271. Builds water stream, dense terrain
	// recipe SSBO (BuildDenseRecipe → sets gos_terrain_indirect::g_recipeReady=true),
	// terrain surface generation, mask dispatch init. Without this the GPU terrain
	// dispatch fails preflight (reason=recipe_not_ready) and terrain renders black.
	// Editor has no overall load-progress variable; use a local dummy.
	{
		volatile float editorLoadProgress = 36.0f;
		EditorDataTrace("EditorData::initTerrainFromPCV: before primeMissionTerrainCache");
		land->primeMissionTerrainCache(editorLoadProgress, 4.0f);
		EditorDataTrace("EditorData::initTerrainFromPCV: after primeMissionTerrainCache");
	}

	EditorDataTrace("EditorData::initTerrainFromPCV: before EditorObjectMgr::load");
	// Load placed actors before registering their render resources.
	EditorObjectMgr::instance()->load( pFile, 1 );
	if (!EditorObjectMgr::instance()->loadNavMarkers(file))
	{
		return rejectDocument();
	}
	EditorDataTrace("EditorData::initTerrainFromPCV: after EditorObjectMgr::load");

	// Pull in existing patrols for EVERY loaded unit on mission open: parse each
	// unit's brain .abl (startPatrolPath literals) so the AI/Brain panel + viewport
	// overlay show stock patrols immediately, without selecting each unit first.
	// Display-only (orderAuthored stays false) -> stock missions are not rewritten.
	{
		EditorObjectMgr::UNIT_LIST allUnits = EditorObjectMgr::instance()->getUnits();
		for ( auto it = allUnits.begin(); it != allUnits.end(); it++ )
			if ( *it )
				(*it)->importPatrolFromBrainIfNeeded();
	}

	// Mirrors Mission::init Track B — must run after load() (types registered) and
	// before finalizeGeometry() (VBO upload). Sets per-instance position/rotation in
	// GpuStaticPropRegistry so objectIdRaw is nonzero and static_prop.frag can write
	// to COLOR_ATTACHMENT2 for ImGui OID picking.
	EditorObjectMgr::instance()->registerStaticPropsForMissionLoad();
	EditorDataTrace("EditorData::initTerrainFromPCV: after registerStaticPropsForMissionLoad");

	// pre-warm ALL building AppearanceTypes from the catalogue so
	// that object types not already present in the loaded map are registered
	// with the GPU batcher before finalizeGeometry() is called.  Without this,
	// placing a Bridge (or any other type absent from the starter map) hits the
	// late-register path (s_geometryFinalized==true at registerType time) and
	// renders solid black.
	{
		MC2_LOAD_SCOPE("editor.primeAllBuildingAppearanceTypes");
		EditorObjectMgr::instance()->primeAllBuildingAppearanceTypes();
	}
	EditorDataTrace("EditorData::initTerrainFromPCV: after primeAllBuildingAppearanceTypes");

	// Steps 11-13 (game lines 3136-3143): finalize batcher geometry, then build indirect buffer.
	// All TG_TypeMultiShape instances created during EditorObjectMgr::load() above have been
	// registered via BldgAppearance::init() against the armed batcher state. finalizeGeometry()
	// uploads the immutable VBO/IBO. submitMultiShape() fast-rejects until this fires.
	// Watch editor-startup.log for:
	//   "[GPUPROPS] finalize: N types, M packets"   -- N > 0 required
	EditorBridge::finalizeMissionGeometry();
	GameAdapters::Vegetation::missionLoaded(land, nullptr);   // biome view: scatter grass/ground patches as in game                   // steps 11-13 — game lines 3136-3143
	EditorDataTrace("EditorData::initTerrainFromPCV: GPU batchers finalized + indirect buffer built");

	{
		long result = 0;
		const auto readMetadata = [&file](const char* key, const std::string& previous, auto set) {
			std::string value;
			const auto status = file.text(key, value);
			if (status == EditorFit::TextStatus::Read) set(std::move(value));
			else if (status == EditorFit::TextStatus::Rejected) set(previous);
		};
		result = file.seekBlock("MissionSettings");
		if (NO_ERR == result)
		{
			readMetadata("MissionName", previousMissionName, [](std::string value) { instance->MissionName(std::move(value)); });

			bool tmpBool = false;
			result = sReadIdBoolean(&file, "MissionNameUseResourceString", tmpBool);
			if (NO_ERR == result)
			{
				EditorData::instance->MissionNameUseResourceString(tmpBool);
			}

			int tmpInt = 0;
			result = sReadIdWholeNum(&file, "MissionNameResourceStringID", tmpInt);
			if (NO_ERR == result)
			{
				EditorData::instance->MissionNameResourceStringID(tmpInt);
			}

			readMetadata("Author", previousAuthor, [](std::string value) { instance->Author(std::move(value)); });

			readMetadata("Blurb", previousBlurb, [](std::string value) { instance->Blurb(std::move(value)); });

			tmpBool = false;
			result = sReadIdBoolean(&file, "BlurbUseResourceString", tmpBool);
			if (NO_ERR == result)
			{
				EditorData::instance->BlurbUseResourceString(tmpBool);
			}

			tmpInt = 0;
			result = sReadIdWholeNum(&file, "BlurbResourceStringID", tmpInt);
			if (NO_ERR == result)
			{
				EditorData::instance->BlurbResourceStringID(tmpInt);
			}

			readMetadata("Blurb2", previousBlurb2, [](std::string value) { instance->Blurb2(std::move(value)); });

			tmpBool = false;
			result = sReadIdBoolean(&file, "Blurb2UseResourceString", tmpBool);
			if (NO_ERR == result)
			{
				EditorData::instance->Blurb2UseResourceString(tmpBool);
			}

			tmpInt = 0;
			result = sReadIdWholeNum(&file, "Blurb2ResourceStringID", tmpInt);
			if (NO_ERR == result)
			{
				EditorData::instance->Blurb2ResourceStringID(tmpInt);
			}

			float tmpFloat = 0.0;
			result = file.readIdFloat("TimeLimit", tmpFloat);
			if (NO_ERR == result)
			{
				EditorData::instance->TimeLimit(tmpFloat);
			}

			tmpFloat = 0.0;
			result = file.readIdFloat("DropWeightLimit", tmpFloat);
			if (NO_ERR == result)
			{
				EditorData::instance->DropWeightLimit(tmpFloat);
			}

			long tmpLong = 0;
			result = file.readIdLong("ResourcePoints", tmpLong);
			if (NO_ERR == result)
			{
				EditorData::instance->InitialResourcePoints(tmpLong);
			}

			result = file.readIdLong( "AdditionalCBills", tmpLong );
			if (NO_ERR == result)
			{
				EditorData::instance->CBills(tmpLong);
			}



			unsigned long tmpULong = 0;
			result = file.readIdULong("IsSinglePlayer", tmpULong);
			if (NO_ERR == result)
			{
				EditorData::instance->IsSinglePlayer(0 != tmpULong);
			}

			tmpULong = 0;
			result = file.readIdULong("MaximumNumberOfTeams", tmpULong);
			if (NO_ERR == result)
			{
				EditorData::instance->MaxTeams(tmpULong);
			}

			tmpULong = 0;
			result = file.readIdULong("MaximumNumberOfPlayers", tmpULong);
			if (NO_ERR == result)
			{
				EditorData::instance->MaxPlayers(tmpULong);
			}

			unsigned char tmpUChar = 0;
			result = file.readIdUChar("scenarioTuneNum", tmpUChar);
			if (NO_ERR == result)
			{
				EditorData::instance->ScenarioTune(tmpUChar);
			}

			readMetadata("AVIFilename", previousVideo, [](std::string value) { instance->VideoFilename(std::move(value)); });

			tmpULong = 0;
			result = file.readIdULong("NumRandomRPbuildings", tmpULong);
			if (NO_ERR == result)
			{
				EditorData::instance->NumRandomRPbuildings(tmpULong);
			}

			readMetadata("DownloadURL", previousURL, [](std::string value) { instance->DownloadURL(std::move(value)); });

			tmpULong = 0;
			result = file.readIdULong("MissionType", tmpULong);
			if (NO_ERR == result)
			{
				EditorData::instance->MissionType(tmpULong);
			}

			tmpBool = false;
			result = sReadIdBoolean(&file, "AirStrikesEnabledDefault", tmpBool);
			if (NO_ERR == result)
			{
				EditorData::instance->AirStrikesEnabledDefault(tmpBool);
			}

			tmpBool = false;
			result = sReadIdBoolean(&file, "MineLayersEnabledDefault", tmpBool);
			if (NO_ERR == result)
			{
				EditorData::instance->MineLayersEnabledDefault(tmpBool);
			}

			tmpBool = false;
			result = sReadIdBoolean(&file, "ScoutCoptersEnabledDefault", tmpBool);
			if (NO_ERR == result)
			{
				EditorData::instance->ScoutCoptersEnabledDefault(tmpBool);
			}

			tmpBool = false;
			result = sReadIdBoolean(&file, "SensorProbesEnabledDefault", tmpBool);
			if (NO_ERR == result)
			{
				EditorData::instance->SensorProbesEnabledDefault(tmpBool);
			}

			tmpBool = false;
			result = sReadIdBoolean(&file, "UnlimitedAmmoEnabledDefault", tmpBool);
			if (NO_ERR == result)
			{
				EditorData::instance->UnlimitedAmmoEnabledDefault(tmpBool);
			}

			tmpBool = false;
			result = sReadIdBoolean(&file, "AllTechEnabledDefault", tmpBool);
			if (NO_ERR == result)
			{
				EditorData::instance->AllTechEnabledDefault(tmpBool);
			}

			tmpBool = false;
			result = sReadIdBoolean(&file, "RepairVehicleEnabledDefault", tmpBool);
			if (NO_ERR == result)
			{
				EditorData::instance->RepairVehicleEnabledDefault(tmpBool);
			}

			tmpBool = false;
			result = sReadIdBoolean(&file, "SalvageCraftEnabledDefault", tmpBool);
			if (NO_ERR == result)
			{
				EditorData::instance->SalvageCraftEnabledDefault(tmpBool);
			}

			tmpBool = false;
			result = sReadIdBoolean(&file, "ResourceBuildingsEnabledDefault", tmpBool);
			if (NO_ERR == result)
			{
				EditorData::instance->ResourceBuildingsEnabledDefault(tmpBool);
			}

			tmpBool = false;
			result = sReadIdBoolean(&file, "NoVariantsEnabledDefault", tmpBool);
			if (NO_ERR == result)
			{
				EditorData::instance->NoVariantsEnabledDefault(tmpBool);
			}

			tmpBool = false;
			result = sReadIdBoolean(&file, "ArtilleryPieceEnabledDefault", tmpBool);
			if (NO_ERR == result)
			{
				EditorData::instance->ArtilleryPieceEnabledDefault(tmpBool);
			}

			tmpBool = false;
			result = sReadIdBoolean(&file, "RPsForMechsEnabledDefault", tmpBool);
			if (NO_ERR == result)
			{
				EditorData::instance->RPsForMechsEnabledDefault(tmpBool);
			}
		}

		result = file.seekBlock("Script");
		if (NO_ERR == result)
		{
			std::string script = previousScript;
			file.text("ScenarioScript", script, sizeof(missionScriptName) - 1);
			script = objective_text::toFileBreaks(script);
			std::copy(script.begin(), script.end(), missionScriptName);
			missionScriptName[script.size()] = '\0';
		}
		else
		{
			strcpy(missionScriptName,"m0101");
		}
	}

	{
		long result = 0;
		result = file.seekBlock("Weather");
		if (NO_ERR == result)
		{
			unsigned long tmpULong = 0;
			result = file.readIdULong("MaxRainDrops", tmpULong);
			if (NO_ERR == result)
			{
				EditorData::instance->MaxRaindrops(tmpULong);
			}

			float tmpFloat = 0.0;
			result = file.readIdFloat("StartingRainLevel", tmpFloat);
			if (NO_ERR == result)
			{
				EditorData::instance->StartingRainLevel(tmpFloat);
			}

			long tmpLong = 0;
			result = file.readIdLong("ChanceOfRain", tmpLong);
			if (NO_ERR == result)
			{
				EditorData::instance->ChanceOfRain(tmpLong);
			}

			tmpFloat = 0.0;
			result = file.readIdFloat("BaseLighteningChance", tmpFloat);
			if (NO_ERR == result)
			{
				EditorData::instance->BaseLightningChance(tmpFloat);
			}
		}
		
		result = file.seekBlock("TheSky");
		if (NO_ERR == result)
		{
			long tempLong = 0;
			result = file.readIdLong("SkyNumber",tempLong);
			if (result != NO_ERR)
				EditorData::instance->TheSkyNumber(1);
			else
				EditorData::instance->TheSkyNumber(tempLong);
		}
	}

	EditorData::instance->TeamsRef() = candidateTeams;
	if (!EditorData::instance->TeamsRef().RestoreObjectPointerReferencesFromNotedPositions())
	{
		return rejectDocument();
	}
	EditorData::instance->PlayersRef().Clear();
	EditorData::instance->PlayersRef().Read( &file );

	if (pFile.seekPacket(4) == NO_ERR)
	{
		long movePacketSize = pFile.getPacketSize();
		EditorDataTrace("EditorData::initTerrainFromPCV: move packet size=%ld", movePacketSize);
		if (movePacketSize != 0)
		{
			EditorDataTrace("EditorData::initTerrainFromPCV: before MOVE_readData");
			MOVE_readData(&pFile, 4);
			gEditorDataMoveDataReadyForFullSave = true;
			EditorDataTrace("EditorData::initTerrainFromPCV: after MOVE_readData; move data ready for full save");
		}
		else
		{
			// QuickSave/partial editor maps may not contain move data.  Do not
			// synthesize it here: MOVE_buildData(NULL) is the current CTD point
			// in the editor startup path.  The save path still rebuilds move data
			// from real MissionMapCellInfo when needed.
			gEditorDataMoveDataReadyForFullSave = false;
			EditorDataTrace("EditorData::initTerrainFromPCV: empty move packet; move data marked not ready; skipped MOVE_buildData fallback");
		}
	}
	else
	{
		gEditorDataMoveDataReadyForFullSave = false;
		EditorDataTrace("EditorData::initTerrainFromPCV: packet 4 missing; move data marked not ready; skipped MOVE_buildData fallback");
	}

	if ( tacMapBmp )
		free( tacMapBmp );

	long ramSize = ((land->realVerticesMapSide) * MAPCELL_DIM) * ((land->realVerticesMapSide) * MAPCELL_DIM);
	
	BYTE* pMemory = (BYTE*)malloc(ramSize * 4 );
	tacMapBmp = (DWORD*)((BYTE*)pMemory);
	
	setMapName( lowerFileName );

	EditorDataWarmTerrain("initTerrainFromPCV");

	EditorInterface::instance()->UnsetBusyMode();
	bIsLoading = false;
	EditorDataTrace("EditorData::initTerrainFromPCV: exit bRetVal=%d", bRetVal ? 1 : 0);

	return bRetVal;
}

bool EditorData::reassignHeightsFromTGA( const char* fileName, int min, int max )
{
	float MinVal = min;
	float  MaxVal = max; // should prompt the user for these
	
	File tgaFile;

	gosASSERT( strstr( fileName, ".tga" ) || strstr( fileName, ".TGA" ) );

	long result = tgaFile.open(const_cast<char*>(fileName));
	gosASSERT(result == NO_ERR);

	struct TGAFileHeader theader;
	tgaFile.read((MemoryPtr)&theader,sizeof(TGAFileHeader));

	if ((theader.width > 120) || (theader.height > 120))
	{
		PAUSE(("TGA file is too large. Largest allowed is 120x120.  Press Continue"));
		return false;
	}

	tgaFile.seek(0);

	MemoryPtr tgaData = (MemoryPtr)std::malloc(tgaFile.fileSize());
	gosASSERT(tgaData != nullptr);

	result = tgaFile.read(tgaData,tgaFile.fileSize());

	TGAFileHeader *header = (TGAFileHeader *)tgaData;

	//MOST paint programs write out grey scale as a palette and mark it as UNC_PAL!!
	if ( header->image_type != UNC_GRAY && header->image_type != UNC_PAL)
	{
		AfxMessageBox( IDS_MUSTBEGRAYSCALE );
		return 0;
	}

	gosASSERT(header->image_type != UNC_TRUE);
	gosASSERT(header->image_type != RLE_TRUE);
	gosASSERT(header->image_type != RLE_PAL);

	gosASSERT( header->width == header->height );
	gosASSERT( !(header->width % 20) );

	BYTE* pTmp = nullptr;

	float mapMin = 255.;
	float mapMax = 0.;
	
	pTmp = tgaData + header->cm_length * header->cm_entry_size/8 + sizeof( TGAFileHeader );

	if (header->image_type == UNC_PAL || header->image_type == UNC_GRAY )
	{
		for ( int i = 0; i < header->width * header->width; ++i )
		{
			float val = (float)(*pTmp++);
			if ( val > mapMax )
				mapMax = val;
			if ( val < mapMin )
				mapMin = val;
		}

		int lineIncrement;
		int countIncrement;
		int linePreAdd;

		pTmp = tgaData + header->cm_length * header->cm_entry_size/8 + sizeof( TGAFileHeader ); 
		lineIncrement = header->width;
		countIncrement = 1;
		linePreAdd = 0;
		
		//------------------------------------------------------------------------
		// Must check image_descriptor to see if we need to un upside down image.
		//bool left = (colorMapInfo.iimage_descriptor & 16) != 0;
		bool top = (header->image_descriptor & 32) != 0;
		if (!top)
		{
			flipTopToBottom(pTmp,header->pixel_depth,header->width,header->height);
		}

		BYTE* pLine = pTmp;
		for ( int j = 0; j < header->height, j < land->realVerticesMapSide; ++j )
		{
			pTmp = pLine + linePreAdd;
			for ( int i = 0; i < header->width, i < land->realVerticesMapSide; ++i )
			{
				float val = (float)(*pTmp);
				pTmp += countIncrement;
				float h = MinVal;
				if (0.0 != mapMax)
				{
					h += (val - mapMin)/mapMax * ( MaxVal - MinVal );
				}
				land->setVertexHeight( j * land->realVerticesMapSide + i, h );
			}
			pLine += lineIncrement;

		}
	}

	std::free(tgaData);

	// need to go through all of the buildings and put them at the right spots
	EditorObjectMgr::instance()->adjustObjectsToNewTerrainHeights();

	land->reCalcLight();

	// Vertex elevations changed — recompute water flags so the renderer and
	// tacmap stay in sync with the new heightmap.  Without this, water flags
	// from before the import remain on vertices that may now be above water
	// (or vice-versa).
	land->recalcWater();

	return true;

}

// Working JPEG decoder lives in mclib/burnin_jpeg_decode.cpp (stb_image, the
// same TU terrtxm2 uses for the .burnin.jpg sidecars). The legacy DecodeJPG()
// in gameos.cpp is a NULL stub on this build, which is why every editor
// new-map colormap fell back to flat grey. Route through the real decoder.
extern "C" unsigned char* BurninJpeg_DecodeRGB(const unsigned char* data, int dataLen, int* outW, int* outH);
extern "C" void           BurninJpeg_FreePixels(unsigned char* px);

//-------------------------------------------------------------------------------------------------
// Decode a source colormap JPEG (tmpRAM/fileSize) and (nearest-neighbour) scale
// it to a square colormap TGA sized to the map, written to localColorMapName as
// 32-bit BGRA. mapWidth is the terrain's vertices-per-side; colormap resolution
// is 12.8 texels per map tile, rounded up to a whole multiple of
// COLOR_MAP_TEXTURE_SIZE (terrtxm2 STOPs on a colormap whose side is not evenly
// divisible by COLOR_MAP_TEXTURE_SIZE). Scaling (not cropping) is what lets an
// author drop in an arbitrary-resolution colormap and have it fit any map size.
bool CreateScaledColorMap(long mapWidth, char *localColorMapName, MemoryPtr tmpRAM, long fileSize)
{
	EditorDataTrace("CreateScaledColorMap: enter mapWidth=%ld fileSize=%ld out=%s",
		mapWidth, fileSize, localColorMapName ? localColorMapName : "<null>");

	if (mapWidth <= 0 || !localColorMapName)
	{
		EditorDataTrace("CreateScaledColorMap: invalid input");
		return false;
	}

	// Terrain colormap pixels are 12.8 texels per map tile, snapped up to a whole
	// number of colormap tiles so terrtxm2's divisibility STOP passes.
	// COLOR_MAP_TEXTURE_SIZE is private to terrtxm2.cpp; mirror its value here.
	const long kColorMapTileSize = 256;
	long requestedWidth = (long)((float)mapWidth * 12.8f + 0.5f);
	long newWidth = ((requestedWidth + kColorMapTileSize - 1) / kColorMapTileSize) * kColorMapTileSize;
	if (newWidth <= 0)
	{
		EditorDataTrace("CreateScaledColorMap: computed width invalid requested=%ld", requestedWidth);
		return false;
	}

	int jpgW = 0, jpgH = 0;
	unsigned char* decodedRGB = nullptr;
	if (tmpRAM && fileSize > 0)
		decodedRGB = BurninJpeg_DecodeRGB((const unsigned char*)tmpRAM, (int)fileSize, &jpgW, &jpgH);

	const bool useDecodedImage = (decodedRGB && jpgW > 0 && jpgH > 0);
	EditorDataTrace("CreateScaledColorMap: decode rgb=%p w=%d h=%d use=%d",
		decodedRGB, jpgW, jpgH, useDecodedImage ? 1 : 0);
	if (!useDecodedImage)
		EditorDataTrace("CreateScaledColorMap: JPEG decode failed; writing fallback neutral TGA");

	long physWidth = newWidth * 4;
	long outputBytes = physWidth * newWidth;

	EditorDataTrace("CreateScaledColorMap: requestedWidth=%ld finalWidth=%ld physWidth=%ld",
		requestedWidth, newWidth, physWidth);

	MemoryPtr tmpImage = (MemoryPtr)malloc(outputBytes + sizeof(TGAFileHeader));
	if (!tmpImage)
	{
		EditorDataTrace("CreateScaledColorMap: malloc failed bytes=%ld", outputBytes + (long)sizeof(TGAFileHeader));
		if (decodedRGB) BurninJpeg_FreePixels(decodedRGB);
		return false;
	}

	TGAFileHeader *output = (TGAFileHeader *)tmpImage;
	memset(output, 0, sizeof(TGAFileHeader));
	output->image_type = UNC_TRUE;
	output->width = (short)newWidth;
	output->height = (short)newWidth;
	output->pixel_depth = 32;
	output->image_descriptor = 32;   // bit5 = top-left origin -> terrtxm2 skips its flip

	MemoryPtr newImage = tmpImage + sizeof(TGAFileHeader);

	if (useDecodedImage)
	{
		// Nearest-neighbour scale the decoded RGB (jpgW x jpgH, row-major,
		// top-to-bottom) into a newWidth x newWidth 32-bit BGRA image. terrtxm2
		// reads the 32-bit colormap as BGRA (atlas uploads it with GL_BGRA), so
		// emit B,G,R,A; stb gives R,G,B.
		for (long y = 0; y < newWidth; y++)
		{
			const long sy = (long)((long long)y * jpgH / newWidth);
			const unsigned char* srcRow = decodedRGB + (size_t)sy * jpgW * 3;
			unsigned char* dst = newImage + (size_t)y * physWidth;
			for (long x = 0; x < newWidth; x++)
			{
				const long sx = (long)((long long)x * jpgW / newWidth);
				const unsigned char* p = srcRow + (size_t)sx * 3;
				dst[0] = p[2];   // B
				dst[1] = p[1];   // G
				dst[2] = p[0];   // R
				dst[3] = 0xff;   // A
				dst += 4;
			}
		}
		BurninJpeg_FreePixels(decodedRGB);
	}
	else
	{
		// Flat neutral fallback (only if the JPEG genuinely won't decode).
		for (long y = 0; y < newWidth; y++)
		{
			DWORD *dst = (DWORD *)(newImage + (size_t)y * physWidth);
			for (long x = 0; x < newWidth; x++)
				dst[x] = 0xff606060;
		}
	}

	File newFile;
	newFile.create(localColorMapName);
	newFile.write((MemoryPtr)output, outputBytes + sizeof(TGAFileHeader));
	newFile.close();

	free(tmpImage);
	EditorDataTrace("CreateScaledColorMap: wrote %s bytes=%ld", localColorMapName, outputBytes + (long)sizeof(TGAFileHeader));
	return true;
}

//-------------------------------------------------------------------------------------------------
// Does things the new Colormap WAY!!! (tm)
// Terrain-generator (Path B) hand-off: generateMission() fills these, then calls
// initTerrainFromTGA(), which uses the pre-generated burnin colormap and applies
// the generated elevation file instead of a flat zero terrain. Cleared at the end
// of initTerrainFromTGA so a normal blank New is unaffected.
static char s_genColormapName[256] = {0};   // map name; burnin at data/textures/<name>.burnin.tga
static char s_genElevPath[1024]    = {0};    // raw float32 N*N world-unit elevations (row-major)

// ---------------------------------------------------------------------------
// MapSizeToCellSide / MapSizeToVertexSide
//
// Single source of truth for the three-layer map-size model:
//
//   Layer          Formula          Example (index 0)
//   ─────────────  ───────────────  ─────────────────
//   cellSide       switch table     60    (quads/side, user-facing label)
//   vertexSide     cellSide + 1     61    (terrain vertex array dim)
//   moveSide       cellSide * 3     180   (MOVE grid dim; 180%30==0 ✓)
//
// cellSide values are multiples of 20 (terrain block contract) AND multiples
// of 10 (MOVE sector contract), so moveSide % SECTOR_DIM(30) is always 0.
//
// DO NOT write raw +1/*3 or new switch tables at call sites.  Use these.
// ---------------------------------------------------------------------------
/*static*/ int EditorData::MapSizeToCellSide( int sizeIndex )
{
	switch ( sizeIndex )
	{
		case 0: return   60;
		case 1: return   80;
		case 2: return  100;
		case 3: return  120;
		case 4: return  260;
		case 5: return  520;
		case 6: return 1020;
		default: return 120;
	}
}

/*static*/ int EditorData::MapSizeToVertexSide( int sizeIndex )
{
	return MapSizeToCellSide( sizeIndex ) + 1;
}

// Thin shim kept for the generateMission() call-site only.
static int genMapSizeToN( int mapSize )
{
	return EditorData::MapSizeToVertexSide( mapSize );
}

bool EditorData::amplifyTerrain( float factor )
{
	if ( !land || !Terrain::mapData )
		return false;
	const long side  = land->realVerticesMapSide;
	const long count = side * side;
	if ( count <= 0 )
		return false;

	// Pivot about the mean elevation so peaks rise and basins drop symmetrically.
	double sum = 0.0;
	for ( long k = 0; k < count; ++k )
		sum += Terrain::mapData->getVertexHeight( (int)k );
	const float pivot = (float)( sum / (double)count );

	for ( long k = 0; k < count; ++k )
	{
		const float e = Terrain::mapData->getVertexHeight( (int)k );
		land->setVertexHeight( (int)k, pivot + ( e - pivot ) * factor );
	}

	land->recalcWater();
	WaterStream::Reset();            // rebuild water recipes against the new relief
	WaterStream::Build();
	Terrain::mapData->calcLight();   // recompute normals for the new relief

	for ( long j = 0; j < side - 1; ++j )
		for ( long i = 0; i < side - 1; ++i )
			gos_terrain_indirect::InvalidateRecipeForVertexNum( (int)( j * side + i ) );

	EditorDataTrace( "EditorData::amplifyTerrain: factor=%.2f pivot=%.1f side=%ld", factor, pivot, side );
	return true;
}

void EditorData::refreshTerrainAfterEdit()
{
	if ( !land || !Terrain::mapData )
		return;
	// setTerrain/setOverlay called invalidateTerrainFaceCache(), nulling the whole
	// Shape-C face cache. Rebuild it so recipe slots read valid nodeIds.
	volatile float prog = 0.0f;
	Terrain::mapData->buildTerrainFaceCache( &prog, 0.0f );

	// Invalidate and rebuild all recipe slots. Uses InvalidateAllRecipes() (O(N),
	// calls PopulateRecipeCementWords once) instead of the old per-vertex loop
	// (O(N²), called PopulateRecipeCementWords N times).
	// NOTE: RebuildCementAtlas() must NOT be called here. Calling it mid-frame
	// caused normal/detail texture tiling to reset across the whole map
	// (glGetTexImage stalls break GL state mid-frame). Solid cement fill DOES
	// need a fresh atlas (newly-painted quads have no atlas layer index otherwise
	// -> interior renders blank, only the decal edge path draws), so instead of
	// rebuilding here we MARK the atlas dirty and let RebuildCementAtlasIfDirty()
	// run the rebuild at FRAME-START (EditorGameOS), before any terrain/GL draws,
	// where the readback is safe.
	gos_terrain_indirect::InvalidateAllRecipes();
	gos_terrain_indirect::MarkCementAtlasDirty();

	// Belt-and-suspenders: after any brush stroke the decal VBO must be
	// rebuilt even if setTerrain()'s own MarkDecalDirty() was somehow skipped.
	gos_terrain_indirect::MarkDecalDirty();
	EditorDataTrace( "EditorData::refreshTerrainAfterEdit: rebuilt face cache + recipes + decal dirty side=%ld", land->realVerticesMapSide );
}

bool EditorData::generateMission( int mapSize, int terrain, unsigned long seed )
{
	const int N = genMapSizeToN( mapSize );

	// Map the chosen terrain TYPE NAME (not its list index -- the dialog listbox
	// order does not match a fixed enum) to a generator biome. Resolve the name
	// the same way initTerrainFromTGA does, then match case-insensitively.
	char typeName[256] = {0};
	EditorSafeLoadString( TerrainColorMap::getTextureNameID( terrain ), typeName, 256 );
	char lname[256];
	strncpy( lname, typeName, sizeof(lname) - 1 ); lname[sizeof(lname) - 1] = 0;
	_strlwr( lname );

	const char* biome = "temperate_hills";
	if ( strstr( lname, "desert" ) || strstr( lname, "sand" ) || strstr( lname, "dune" ) )
		biome = "desert";
	else if ( strstr( lname, "snow" ) || strstr( lname, "ice" ) || strstr( lname, "arctic" ) || strstr( lname, "cold" ) || strstr( lname, "tundra" ) )
		biome = "snow_mountain";
	else if ( strstr( lname, "forest" ) || strstr( lname, "swamp" ) || strstr( lname, "jungle" ) || strstr( lname, "marsh" ) )
		biome = "swamp_forest";
	else if ( strstr( lname, "rock" ) || strstr( lname, "badland" ) || strstr( lname, "mountain" ) || strstr( lname, "lunar" ) || strstr( lname, "crater" ) || strstr( lname, "volcanic" ) || strstr( lname, "lava" ) || strstr( lname, "ash" ) )
		biome = "rocky_badlands";   // no dedicated volcanic biome yet; rocky is the closest barren look
	EditorDataTrace( "generateMission: terrain=%d typeName='%s' -> biome=%s", terrain, typeName, biome );

	const char* mapName = "genmap";

	CreateDirectory( "terrain_gen_out", nullptr );

	char recipePath[512];
	sprintf( recipePath, "terrain_gen_out\\%s.recipe.json", mapName );
	FILE* rf = fopen( recipePath, "wb" );
	if ( !rf )
	{
		if (!g_cliSuppressModals) AfxMessageBox( "Generate Mission: could not write recipe file." );
		return false;
	}
	fprintf( rf, "{\"version\":1,\"name\":\"%s\",\"size\":%d,\"biome\":\"%s\",\"seed\":%lu}\n",
		mapName, N, biome, (unsigned long)( seed & 0x7fffffff ) );
	fclose( rf );

	// Blocking run of the python generator in tools\terrain_gen\ (CWD is the deploy root).
	char cmd[1024];
	sprintf( cmd, "py -3 tools\\terrain_gen\\terrain_gen.py \"%s\" --out terrain_gen_out", recipePath );
	EditorDataTrace( "generateMission: N=%d biome=%s seed=%lu cmd=%s", N, biome, seed, cmd );
	int rc = EditorTaskRunner::RunBlockingHidden( cmd, "terrain_gen_out\\terrain_gen.log" );
	EditorDataTrace( "generateMission: generator rc=%d", rc );
	if ( rc != 0 )
	{
		if (!g_cliSuppressModals) AfxMessageBox( "Terrain generator failed (output: terrain_gen_out\\terrain_gen.log).\n\nNeeds Python 3 on PATH with numpy, Pillow and opensimplex installed\n(pip install -r tools\\terrain_gen\\requirements.txt)." );
		return false;
	}

	// Place the burnin colormap where terrtxm2 looks (data\textures\<name>.burnin.tga).
	char srcBurnin[512], dstBurnin[512];
	mc2::fmt::to_buf(srcBurnin, "terrain_gen_out\\{}.burnin.tga", mc2::fmt::safe(mapName));
	mc2::fmt::to_buf(dstBurnin, "{}{}.burnin.tga", mc2::fmt::safe(texturePath), mc2::fmt::safe(mapName));
	SetFileAttributes( dstBurnin, FILE_ATTRIBUTE_NORMAL );
	if ( !CopyFile( srcBurnin, dstBurnin, FALSE ) )
		EditorDataTrace( "generateMission: WARNING CopyFile burnin %s -> %s failed", srcBurnin, dstBurnin );

	strcpy( s_genColormapName, mapName );
	sprintf( s_genElevPath, "terrain_gen_out\\%s.elev.r32", mapName );

	return initTerrainFromTGA( mapSize, 0, 0, terrain );
}

bool EditorData::generateFromDialogParams( int mapSizeIndex, const char* biome )
{
	// The MapGeneratorDialog has already written the recipe JSON and run the
	// python generator; we only need to stage the burnin + hand off to
	// initTerrainFromTGA (same as the tail of generateMission).
	const char* mapName = "genmap";

	EditorDataTrace( "generateFromDialogParams: mapSizeIndex=%d biome=%s", mapSizeIndex, biome ? biome : "" );

	// Copy the burnin colormap to where terrtxm2 looks (data\textures\<name>.burnin.tga).
	char srcBurnin[512], dstBurnin[512];
	mc2::fmt::to_buf(srcBurnin, "terrain_gen_out\\{}.burnin.tga", mc2::fmt::safe(mapName));
	mc2::fmt::to_buf(dstBurnin, "{}{}.burnin.tga", mc2::fmt::safe(texturePath), mc2::fmt::safe(mapName));
	SetFileAttributes( dstBurnin, FILE_ATTRIBUTE_NORMAL );
	if ( !CopyFile( srcBurnin, dstBurnin, FALSE ) )
		EditorDataTrace( "generateFromDialogParams: WARNING CopyFile burnin %s -> %s failed", srcBurnin, dstBurnin );

	// Hand off to initTerrainFromTGA: use the burnin colormap + apply the heightmap.
	// terrain=0 with s_genColormapName set makes initTerrainFromTGA use the burnin path.
	strcpy( s_genColormapName, mapName );
	sprintf( s_genElevPath, "terrain_gen_out\\%s.elev.r32", mapName );

	return initTerrainFromTGA( mapSizeIndex, 0, 0, 0 );
}

bool EditorData::initTerrainFromTGA( int mapSize, int min, int max, int terrain )
{
	gEditorDataMoveDataReadyForFullSave = false;
	EditorDataTrace("EditorData::initTerrainFromTGA: enter mapSize=%d min=%d max=%d terrain=%d; move data marked not ready", mapSize, min, max, terrain);
	EditorInterface::instance()->SetBusyMode(false/*no redraw*/);

	clear(); // get rid of all the old stuff now
	EditorData::instance->MissionNeedsSaving(true);

	float MinVal = min;
	float MaxVal = max;

	// vertexSide = cellSide + 1.  Terrain requires (vertexSide-1) % 20 == 0.
	// Use MapSizeToVertexSide — never repeat the lookup table here.
	long mapWidth = (long)EditorData::MapSizeToVertexSide( mapSize );

	EditorDataTrace("EditorData::initTerrainFromTGA: resolved mapWidth=%ld", mapWidth);
	if (mapWidth <= 0)
	{
		EditorDataTrace("EditorData::initTerrainFromTGA: invalid mapWidth");
		EditorInterface::instance()->UnsetBusyMode();
		return false;
	}

	land = new Terrain( );
	EditorDataTrace("EditorData::initTerrainFromTGA: Terrain new land=%p", land);
	volatile float crap = 0;
	EditorDataTrace("EditorData::initTerrainFromTGA: before land->init");
	land->init( mapWidth, nullptr, EDITOR_VISIBLE_VERTICES, crap, 100 );
	EditorDataTrace("EditorData::initTerrainFromTGA: after land->init realVerticesMapSide=%ld terrainTextures2=%p", land->realVerticesMapSide, land->terrainTextures2);
	land->setUserSettings( min, max, terrain );
	EditorDataTrace("EditorData::initTerrainFromTGA: after setUserSettings");

	//-----------------------------------------------------------------
	// Generated mission (Path B): the burnin colormap already exists in
	// data/textures/<name>.burnin.tga. Point terrainTextures2 at it (terrtxm2
	// resolves the .burnin.tga from terrainName) so the JPG-synthesis block below
	// is skipped. mapName is set to newmap by the else-branch so Save prompts.
	if ( s_genColormapName[0] && !land->terrainTextures2 )
	{
		CreateDirectory(missionPath, nullptr);
		CreateDirectory(terrainPath, nullptr);
		CreateDirectory(texturePath, nullptr);
		land->terrainTextures2 = new TerrainColorMap;
		land->terrainName = (char *)gos_Malloc( strlen( s_genColormapName ) + 1 );
		strcpy( land->terrainName, s_genColormapName );
		EditorDataTrace("EditorData::initTerrainFromTGA: GEN colormap terrainName=%s", land->terrainName );
	}

	if ( !land->terrainTextures2 )
	{
		char name[1024];
		char textureRawPath[2048];

		EditorSafeLoadString(TerrainColorMap::getTextureNameID(terrain), name, 256 );
		EditorDataTrace("EditorData::initTerrainFromTGA: selected terrain=%d textureNameID=%ld textureName=%s",
			terrain, TerrainColorMap::getTextureNameID(terrain), name);

		sprintf(textureRawPath,"%sRandom_Maps\\",texturePath);

		FullPathFileName tgaColorMapName;
		tgaColorMapName.init(textureRawPath,name,".jpg");
		EditorDataTrace("EditorData::initTerrainFromTGA: trying terrain JPG=%s exists=%d",
			(const char*)tgaColorMapName, fileExists(tgaColorMapName) ? 1 : 0);
				
		if (fileExists(tgaColorMapName))
		{
			char name2[1024];
			strcpy(name2,"newMap");
			//Copy the base map to the new mapname in the data textures directory.
			// OVERWRITE IT if it exists.
			// DELETE any burnin version of the map!!
			// Maybe we should warn here?
			FullPathFileName localColorMapName;
			localColorMapName.init(texturePath,name2,".tga");
			
			SetFileAttributes(localColorMapName,FILE_ATTRIBUTE_NORMAL);
			CreateDirectory(missionPath, nullptr);
			CreateDirectory(warriorPath, nullptr);
			CreateDirectory(terrainPath, nullptr);
			CreateDirectory(texturePath, nullptr);

			//Maps are in the fastfiles.  Dork.
			//CopyFile(tgaColorMapName,localColorMapName,false);
			// YOU must scale the maps to match the mapsize.
			// If its 120 x 120, no problem.
			// If its smaller, you must create a SMALLER colormap!   Dork.
			//
			// And now, you must not only create a smaller one but convert
			// From JPG to TGA.  Big Dork!
			File src;
			src.open(tgaColorMapName);
			DWORD srcSize = src.fileSize();
			EditorDataTrace("EditorData::initTerrainFromTGA: source colormap=%s size=%lu", (const char*)tgaColorMapName, (unsigned long)srcSize);
			MemoryPtr tmpRAM = (MemoryPtr)malloc(srcSize);
			if (!tmpRAM)
			{
				EditorDataTrace("EditorData::initTerrainFromTGA: malloc failed for jpg buffer");
				src.close();
				if (!justResaveAllMaps)
				{
					AfxMessageBox("Could not create terrain: memory allocation failed while reading texture file.", MB_OK | MB_ICONERROR);
				}
				EditorInterface::instance()->UnsetBusyMode();
				return false;
			}
			src.read(tmpRAM,srcSize);
			src.close();

			if (!CreateScaledColorMap(mapWidth, (char*)(const char*)localColorMapName, tmpRAM, srcSize))
			{
				EditorDataTrace("EditorData::initTerrainFromTGA: CreateScaledColorMap failed");
				free(tmpRAM);
				if (!justResaveAllMaps)
				{
					AfxMessageBox("Could not create terrain: failed to scale the color map texture.", MB_OK | MB_ICONERROR);
				}
				EditorInterface::instance()->UnsetBusyMode();
				return false;
			}

			free(tmpRAM);
			
			FullPathFileName burnInMapName;
			burnInMapName.init(texturePath,name2,".burnin.tga");
			BOOL res = TRUE;
			if (fileExists(burnInMapName))
			{
				if (!DeleteFile(burnInMapName))
				{
					res = FALSE;
				}
			}

			FullPathFileName heightMapName;
			heightMapName.init(terrainPath,name2,".height.tga");
			if (fileExists(heightMapName))
			{
				if (!DeleteFile(heightMapName))
				{
					res = FALSE;
				}
			}

			FullPathFileName pakName;
			pakName.init(missionPath,name2,".pak");
			if (fileExists(pakName))
			{
				if (!DeleteFile(pakName))
				{
					res = FALSE;
				}
			}

			FullPathFileName fitName;
			fitName.init(missionPath,name2,".fit");
			if (fileExists(fitName))
			{
				if (!DeleteFile(fitName))
				{
					res = FALSE;
				}
			}

			if (!justResaveAllMaps)
			{
				if (FALSE == res)
				{
					AfxMessageBox(IDS_UNABLE_TO_DELETE_NEWMAP);
				}
			}

			land->terrainTextures2 = new TerrainColorMap;		//Otherwise, this will stay NULL and we know not to use them
			
			land->terrainName = (char *)gos_Malloc(strlen(name2) + 1);
			strcpy(land->terrainName,name2);

			FullPathFileName missionName;
			missionName.init(missionPath,name2,".pak");
			setMapName(missionName);
		}
		else
		{
			EditorDataTrace("EditorData::initTerrainFromTGA: terrain JPG missing; selected terrain cannot be created path=%s",
				(const char*)tgaColorMapName);
			if (!justResaveAllMaps)
			{
				CString errMsg;
				errMsg.Format("Could not create terrain: texture file not found.\n\n%s\n\nPlease verify your data\\textures\\Random_Maps folder contains the required terrain JPG files.", (const char*)tgaColorMapName);
				AfxMessageBox(errMsg, MB_OK | MB_ICONERROR);
			}
			EditorInterface::instance()->UnsetBusyMode();
			return false;
		}
	}
	else
	{
		// terrainTextures2 was already allocated by Terrain::init().
		// For a new map, this is fine — the color map system is pre-initialized.
		// Just set up the terrain/map name so the rest of the pipeline works.
		EditorDataTrace("EditorData::initTerrainFromTGA: terrainTextures2 already set by Terrain::init, setting up map name");
		
		CreateDirectory(missionPath, nullptr);
		CreateDirectory(warriorPath, nullptr);
		CreateDirectory(terrainPath, nullptr);
		CreateDirectory(texturePath, nullptr);

		const char* name2 = "newMap";
		if (!land->terrainName)
		{
			land->terrainName = (char *)gos_Malloc(strlen(name2) + 1);
			strcpy(land->terrainName, name2);
		}

		FullPathFileName missionName;
		missionName.init(missionPath, name2, ".pak");
		setMapName(missionName);
	}

	// New-map water smoke-test default.
	// Set both values to min - 1, which is below the generated flat terrain
	// and guarantees recalcWater() flags zero vertices.
	// Keep this local to new-map generation and log it so we can verify the live
	// build is actually running this path.
	Terrain::mapData->waterDepth = (float)min - 1.0f;
	Terrain::waterElevation = (float)min - 1.0f;
	EditorDataTrace("EditorData::initTerrainFromTGA: WATER_PATCH applied min=%d waterDepth=%f waterElevation=%f",
		min, Terrain::mapData->waterDepth, Terrain::waterElevation);

	// Patch: New-map fog defaults.
	// New missions were inheriting/keeping a bad fog color, which showed as
	// magenta/pink until manually set to 0 in the Fog dialog.  Keep fog range
	// below the generated terrain as before, but force a sane black fog color
	// for newly generated maps.
	eye->dayFogColor = 0x00000000;
	eye->fogColor = eye->dayFogColor;
	eye->fogStart = min - 1; 
	eye->fogFull = min - 2;

	const long genSide = land->realVerticesMapSide;
	bool genHeightApplied = false;
	if ( s_genElevPath[0] )
	{
		// Generated mission: load raw float32 world-unit elevations (row-major,
		// y outer, x inner — matches the generator's vertex order) into the grid.
		FILE* ef = fopen( s_genElevPath, "rb" );
		if ( ef )
		{
			const long count = genSide * genSide;
			float* elev = (float *)malloc( sizeof(float) * count );
			if ( elev && (long)fread( elev, sizeof(float), count, ef ) == count )
			{
				for ( long k = 0; k < count; ++k )
					land->setVertexHeight( (int)k, elev[k] );
				genHeightApplied = true;
				EditorDataTrace("EditorData::initTerrainFromTGA: GEN elevations applied count=%ld from %s", count, s_genElevPath);
			}
			else
			{
				EditorDataTrace("EditorData::initTerrainFromTGA: GEN elev read failed (%s)", s_genElevPath);
			}
			if ( elev ) free( elev );
			fclose( ef );
		}
		else
		{
			EditorDataTrace("EditorData::initTerrainFromTGA: GEN elev open failed (%s)", s_genElevPath);
		}
	}
	if ( !genHeightApplied )
	{
		for ( int j = 0; j < land->realVerticesMapSide; ++j )
		{
			for ( int i = 0; i < land->realVerticesMapSide; ++i )
			{
				land->setVertexHeight( j * land->realVerticesMapSide + i, 0 );
			}
		}
	}
	else if ( Terrain::mapData )
	{
		// Generated terrain has real slopes -> recompute vertex normals before the
		// recipe build (primeMissionTerrainCache) so lighting isn't flat.
		Terrain::mapData->calcLight();
	}

	EditorDataTrace("EditorData::initTerrainFromTGA: before recalcWater waterDepth=%f waterElevation=%f",
		Terrain::mapData ? Terrain::mapData->waterDepth : -9999.0f,
		Terrain::waterElevation);
	land->recalcWater();
	{
		long waterCount = 0;
		long dryCount = 0;
		PostcompVertexPtr vertices = Terrain::mapData ? Terrain::mapData->getData() : nullptr;
		if (vertices)
		{
			const long vertexCount = land->realVerticesMapSide * land->realVerticesMapSide;
			for (long vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
			{
				PostcompVertexPtr v = vertices + vertexIndex;
				if (v->water)
					++waterCount;
				else
					++dryCount;
			}
		}
		EditorDataTrace("EditorData::initTerrainFromTGA: after recalcWater water=%ld dry=%ld waterDepth=%f waterElevation=%f",
			waterCount, dryCount,
			Terrain::mapData ? Terrain::mapData->waterDepth : -9999.0f,
			Terrain::waterElevation);
	}

	if ( tacMapBmp )
		free( tacMapBmp );

	long ramSize = ((land->realVerticesMapSide) * MAPCELL_DIM) * ((land->realVerticesMapSide) * MAPCELL_DIM);
	
	BYTE* pMemory = (BYTE*)malloc( ramSize * 4 );
	tacMapBmp = (DWORD*)((BYTE*)pMemory);

	// New maps do not have MissionMapCellInfo yet.  Calling MOVE_buildData with
	// NULL mapData is the observed CTD point after color-map creation.  Defer
	// movement-data generation to the save path, where the editor builds it
	// with real map-cell information.
	EditorDataTrace("EditorData::initTerrainFromTGA: skipped MOVE_buildData for new blank terrain side=%ld", land->realVerticesMapSide);

	// New blank maps must run the same terrain/GPU priming as a loaded map, or the
	// GPU-direct terrain has no dense recipes (g_recipeReady stays false) and the
	// whole map renders black (dispatch preflight reason=recipe_not_ready). clear()
	// at the top of this function already tore down any prior map's GPU state.
	// Mirrors the load path (initTerrainFromPCV); object-load steps are omitted
	// because a freshly generated map has no objects.
	EditorBridge::beginMissionRenderResources();
	GameAdapters::StaticProp::beginMission();
	GameAdapters::Mech::beginMission();
	{
		const uint32_t kEditorMaxActors = 2048u;
		const uint32_t kStaticPropHeadroom = 8192u;
		gpu_cull::substrate_init(kEditorMaxActors + kEditorMaxActors / 4u + kStaticPropHeadroom);
		EditorBridge::cullComputeInit();
		gos_terrain_lighting::mission_init(
			static_cast<uint32_t>(Terrain::realVerticesMapSide * Terrain::realVerticesMapSide), 64u);
		gpu_cull::readback_init(kEditorMaxActors + kEditorMaxActors / 4u + kStaticPropHeadroom);

	}
	// Load the colormap into terrainTextures2. The load path does this inside
	// Terrain::load (terrain.cpp:568-573), which the new-map path skips -- leaving
	// terrainTextures2 an EMPTY colormap, so the terrain has no texture and renders
	// black. terrainName is "newMap", so this loads the newMap.tga colormap built
	// above. Must run BEFORE primeMissionTerrainCache (recipes resolve texture
	// handles from terrainTextures2).
	if ( land->terrainTextures2 )
	{
		if ( land->colorMapName )
			land->terrainTextures2->init( land->colorMapName );
		else
			land->terrainTextures2->init( land->terrainName );
		EditorDataTrace("EditorData::initTerrainFromTGA: NEWMAP colormap init done terrainName=%s",
			land->terrainName ? land->terrainName : "<null>");
	}
	{
		// Warm camera frustum before primeMissionTerrainCache — buildTerrainFaceCache
		// uses eye clip planes for culling. On the generator startup path, eye->update()
		// was never called (land was NULL so Editor::update skipped the land block).
		// Calling it once here puts eye in the same state as the toolbar-generate path.
		if ( eye )
			eye->update();
		volatile float editorLoadProgress = 36.0f;
		land->primeMissionTerrainCache(editorLoadProgress, 4.0f);   // builds dense terrain recipes -> g_recipeReady=true
	}
	EditorObjectMgr::instance()->registerStaticPropsForMissionLoad();
	EditorObjectMgr::instance()->primeAllBuildingAppearanceTypes();
	EditorBridge::finalizeMissionGeometry();
	GameAdapters::Vegetation::missionLoaded(land, nullptr);   // biome view: scatter grass/ground patches as in game

	EditorDataWarmTerrain("initTerrainFromTGA");

	EditorInterface::instance()->UnsetBusyMode();
	EditorDataTrace("EditorData::initTerrainFromTGA: success");

	// One-shot: clear generator hand-off so a subsequent plain New is a flat map.
	s_genColormapName[0] = 0;
	s_genElevPath[0]     = 0;

	return true;
}
