// EditorData: move-data rebuild, mission save and quick save, mission FIT sections, objectives and height map.
// Declared in the class headers; shared names in EditorData_internal.h.
#include "EditorData_internal.h"


//Temp until we I know it works.
float CliffTerrainAngle = 45.0f;

//-------------------------------------------------------------------------------------------------
// MOVE dirty/rebuild helpers
//-------------------------------------------------------------------------------------------------
static bool s_moveDirty = false;

/*static*/ void EditorData::MarkMoveDataDirty()
{
	s_moveDirty = true;
	gEditorDataMoveDataReadyForFullSave = false;
	EditorDataTrace("EditorData::MarkMoveDataDirty: MOVE marked dirty");
}

/*static*/ bool EditorData::RebuildMoveIfDirty(std::string* errorOut)
{
	if (!s_moveDirty)
		return true;   // nothing to do; MOVE is already valid

	bool ok = RebuildMoveFromCurrentTerrain(errorOut);
	if (ok)
		s_moveDirty = false;
	// If rebuild failed, leave dirty=true so next tick retries.
	return ok;
}

//-------------------------------------------------------------------------------------------------
// RebuildMoveFromCurrentTerrain
//
// Defensive in-place MOVE rebuild from current terrain + placed objects.
// MOVE is treated as cached derived data — call MarkMoveDataDirty() after any
// terrain load/generation, then let RebuildMoveIfDirty() fire from update().
//
// Returns false (with *errorOut set) on any validation or build failure.
// On success: GameMap + GlobalMoveMap[0..2] are valid; IsMoveDataReadyForFullSave() = true.
//
// IMPORTANT: MOVE_init() must have been called at editor startup before this (it is, at
// Editor.cpp:755).  No other ordering constraint beyond land being fully initialized.
//
// ── Three-layer map-size model ───────────────────────────────────────────────────────────
//  Layer       Variable    Meaning                         Contract
//  ─────────── ─────────── ─────────────────────────────── ─────────────────────────────
//  Render/load vertexSide  terrain vertex array dimension   (vertexSide-1) % 20 == 0
//  Gameplay    cellSide    quads per side (user-facing)     cellSide = vertexSide - 1
//  Pathfinding moveSide    MOVE grid dimension              moveSide % SECTOR_DIM(30)==0
//                          moveSide = cellSide * 3
//
//  All three contracts are asserted at the top of this function.
//  Never sample vertex heights with a cellSide index (off by 1 at border).
//  Never pass moveSide=vertexSide*3 to MOVE_buildData (SECTOR_DIM assert fires).
//-------------------------------------------------------------------------------------------------
/*static*/ bool EditorData::RebuildMoveFromCurrentTerrain(std::string* errorOut)
{
	auto SetError = [&](const char* msg) {
		if (errorOut) *errorOut = msg;
		printf("[MOVE_REBUILD] FAIL: %s\n", msg); fflush(stdout);
		gEditorDataMoveDataReadyForFullSave = false;
	};

	if (!land) {
		SetError("No terrain loaded (land is null).");
		return false;
	}

	const int vertexSide = land->realVerticesMapSide;
	const int cellSide   = vertexSide - 1;  // quads per side (gameplay layer)
	const int moveSide   = cellSide * 3;    // MOVE grid dimension (pathfinding layer)

	// ── System-boundary assertions (see docstring above) ──────────────────────────────
	gosASSERT(vertexSide > 1);                        // terrain must have at least one cell
	gosASSERT((cellSide % 20) == 0);                  // terrain block contract
	gosASSERT((moveSide % SECTOR_DIM) == 0);          // MOVE global-map sector contract

	// Diagnostic: always print so crash logs show all three dimensions.
	printf("[MOVE_REBUILD] begin: land=%p vertexSide=%d cellSide=%d moveSide=%d GameMap(before)=%p\n",
	       (void*)land, vertexSide, cellSide, moveSide, (void*)GameMap);
	fflush(stdout);

	if (cellSide <= 0) {
		char buf[128];
		snprintf(buf, sizeof(buf), "Invalid terrain dimensions: vertexSide=%d cellSide=%d", vertexSide, cellSide);
		SetError(buf);
		return false;
	}

	// ── Size gate: MOVE arrays are bounded by MAX_MAP_CELL_WIDTH (720 MOVE cells/side) ──
	// moveSide = cellSide*3, so max supported cellSide = 240.
	// Maps larger than this (260, 520, 1020 cells) would overflow fixed MOVE arrays
	// (tileMulMAPCELL_DIM[], trigger maps, etc.) and exceed systemHeap capacity.
	// Terrain save/load still works; AI pathfinding is not supported at this size.
	if (moveSide > MAX_MAP_CELL_WIDTH) {
		char buf[256];
		snprintf(buf, sizeof(buf),
			"Map too large for MOVE: moveSide=%d > MAX_MAP_CELL_WIDTH=%d "
			"(cellSide=%d; max supported cellSide=%d). "
			"Terrain editing and save/load still work. "
			"AI pathfinding not supported at this map size.",
			moveSide, MAX_MAP_CELL_WIDTH, cellSide, MAX_MAP_CELL_WIDTH / MAPCELL_DIM);
		SetError(buf);
		return false;
	}

	// pInfo: one _ScenarioMapCellInfo per MOVE cell.  moveSide² = (cellSide*3)² entries.
	_ScenarioMapCellInfo* pInfo = (_ScenarioMapCellInfo*)malloc(
		sizeof(_ScenarioMapCellInfo) * moveSide * moveSide);
	memset(pInfo, 0, sizeof(_ScenarioMapCellInfo) * moveSide * moveSide);
	MissionMapCellInfo* pTmp = pInfo;

	// Iterate over CELLS (quads), not vertices.  Each cell (i,j) has 3×3 MOVE sub-cells.
	for ( int i = 0; i < cellSide; ++i )
	{
		for ( int cellI = 0; cellI < 3; ++cellI )
		{
			for ( int j = 0; j < cellSide; ++j )
			{
				for ( int cellJ = 0; cellJ < 3; ++cellJ )
				{
					pTmp->terrain = MC_NONE_TYPE;
					pTmp->overlay = (unsigned char)INVALID_OVERLAY;
					pTmp->road = false;
					pTmp->specialID = 0;
					pTmp->specialType = SPECIAL_NONE;
					pTmp->lineOfSight = 0;
					pTmp->passable = true;

					pTmp->terrain = land->getTerrain( i, j );

					if ((i==0) || (j==0) || (i >= (cellSide-1)) || (j >= (cellSide-1)))
					{
						pTmp->passable = false;
					}
					else
					{
						bool tlx = (long(land->mapData->topLeftVertex.x) & 1);
						bool tly = (long(land->mapData->topLeftVertex.y) & 1);

						long x = i - land->mapData->topLeftVertex.x;
						long y = j - land->mapData->topLeftVertex.y;

						bool yby2 = (y & 1) ^ (tly);
						bool xby2 = (x & 1) ^ (tlx);

						long uvMode = 0;
						if (yby2)
						{
							if (xby2) uvMode = BOTTOMRIGHT;
							else       uvMode = BOTTOMLEFT;
						}
						else
						{
							if (xby2) uvMode = BOTTOMLEFT;
							else       uvMode = BOTTOMRIGHT;
						}

						if (uvMode == BOTTOMRIGHT)
						{
							float elv0 = land->getTerrainElevation(i  ,j);
							float elv1 = land->getTerrainElevation(i+1,j);
							float elv2 = land->getTerrainElevation(i+1,j+1);
							float elv3 = land->getTerrainElevation(i  ,j+1);

							float elvDiff1 = fabs(elv0 - elv2);
							float elvDiff2 = fabs(elv0 - elv1);
							float elvAngleFactor = Terrain::worldUnitsPerVertex * sin(CliffTerrainAngle * DEGREES_TO_RADS);
							if ((elvDiff1 > elvAngleFactor) || (elvDiff2 > elvAngleFactor))
								pTmp->passable = false;

							elvDiff1 = fabs(elv0 - elv2);
							elvDiff2 = fabs(elv0 - elv3);
							if ((elvDiff1 > elvAngleFactor) || (elvDiff2 > elvAngleFactor))
								pTmp->passable = false;
						}
						else	// uvMode is BOTTOMLEFT
						{
							float elv0 = land->getTerrainElevation(i  ,j);
							float elv1 = land->getTerrainElevation(i+1,j);
							float elv2 = land->getTerrainElevation(i+1,j+1);
							float elv3 = land->getTerrainElevation(i  ,j+1);

							float elvDiff1 = fabs(elv0 - elv1);
							float elvDiff2 = fabs(elv0 - elv3);
							float elvAngleFactor = Terrain::worldUnitsPerVertex * sin(CliffTerrainAngle * DEGREES_TO_RADS);
							if ((elvDiff1 > elvAngleFactor) || (elvDiff2 > elvAngleFactor))
								pTmp->passable = false;

							elvDiff1 = fabs(elv1 - elv2);
							elvDiff2 = fabs(elv1 - elv3);
							if ((elvDiff1 > elvAngleFactor) || (elvDiff2 > elvAngleFactor))
								pTmp->passable = false;
						}
					}

					Overlays overlay;
					unsigned long offset;
					land->getOverlay( i, j, overlay, offset );
					if ( overlay != INVALID_OVERLAY )
					{
						pTmp->overlay = (int)overlay;
						pTmp->road = true;
					}

					int cellRow = i * 3 + cellI;
					int cellColumn = j * 3 + cellJ;
					pTmp->mine = (GameMap && GameMap->inBounds(cellRow, cellColumn))
					             ? GameMap->getMine(cellRow, cellColumn) : 0;

					pTmp++;
				}
			}
		}
	}

	// Mark impassability from placed objects (gates, walls, bridges, forests, etc.)
	GameObjectFootPrint specialAreaFootPrints[MAX_SPECIAL_AREAS];
	long wallTotalCount = 0;
	long gateTotalCount = 0;
	long landBridgeTotalCount = 0;
	EditorObjectMgr::BUILDING_LIST completeBuildingList = EditorObjectMgr::instance()->getBuildings();
	if (completeBuildingList.Count() > 0)
	{
		EditorObjectMgr::BUILDING_LIST::EConstIterator it = completeBuildingList.Begin();
		while (!it.IsDone())
		{
			if ((*it)->getType() == BLDG_TYPE)
			{
				if      ((*it)->getSpecialType() == EditorObjectMgr::EDITOR_GATE)   gateTotalCount++;
				else if ((*it)->getSpecialType() == EditorObjectMgr::WALL)           wallTotalCount++;
				else if ((*it)->getSpecialType() == EditorObjectMgr::EDITOR_BRIDGE) landBridgeTotalCount++;
			}
			it++;
		}
	}
	long wallCount = 0;
	long gateCount = 0;
	long landBridgeCount = 0;
	completeBuildingList = EditorObjectMgr::instance()->getBuildings();
	if (completeBuildingList.Count() > 0)
	{
		EditorObjectMgr::BUILDING_LIST::EConstIterator it = completeBuildingList.Begin();
		while (!it.IsDone())
		{
			if ((*it)->getType() == BLDG_TYPE)
			{
				if ((*it)->getSpecialType() == EditorObjectMgr::EDITOR_GATE)
				{
					(*it)->markTerrain( pInfo, SPECIAL_GATE, gateCount );
					Stuff::Vector3D pos = (*it)->appearance()->position;
					int r, c;
					land->worldToCell(pos, r, c);
					specialAreaFootPrints[gateCount].cellPositionRow = r;
					specialAreaFootPrints[gateCount].cellPositionCol = c;
					gateCount++;
				}
				else if ((*it)->getSpecialType() == EditorObjectMgr::WALL)
				{
					(*it)->markTerrain( pInfo, SPECIAL_WALL, gateTotalCount + wallCount);
					Stuff::Vector3D pos = (*it)->appearance()->position;
					int r, c;
					land->worldToCell(pos, r, c);
					specialAreaFootPrints[gateTotalCount + wallCount].cellPositionRow = r;
					specialAreaFootPrints[gateTotalCount + wallCount].cellPositionCol = c;
					wallCount++;
				}
				else if ((*it)->getSpecialType() == EditorObjectMgr::EDITOR_BRIDGE)
				{
					(*it)->markTerrain( pInfo, SPECIAL_LAND_BRIDGE, gateTotalCount + wallTotalCount + landBridgeCount);
					Stuff::Vector3D pos = (*it)->appearance()->position;
					int r, c;
					land->worldToCell(pos, r, c);
					specialAreaFootPrints[gateTotalCount + wallTotalCount + landBridgeCount].cellPositionRow = r;
					specialAreaFootPrints[gateTotalCount + wallTotalCount + landBridgeCount].cellPositionCol = c;
					landBridgeCount++;
				}
				else if ((*it)->appearance()->isForestClump() || (*it)->getForestID() != -1 )
					(*it)->markTerrain( pInfo, SPECIAL_FOREST, 0);
				else if (EditorData::instance && !EditorData::instance->IsSinglePlayer()
				      && (*it)->getSpecialType() == EditorObjectMgr::RESOURCE_BUILDING)
					(*it)->markTerrain( pInfo, EditorObjectMgr::RESOURCE_BUILDING, 0);
				else
					(*it)->markTerrain(pInfo, SPECIAL_NONE, 0);
			}
			it++;
		}
	}

	for (long i = 0; i < gateCount + wallCount + landBridgeCount; i++) {
		specialAreaFootPrints[i].preNumCells = specialAreaFootPrints[i].numCells;
		specialAreaFootPrints[i].numCells = 0;
	}
	pTmp = pInfo;
	for (long r = 0; r < moveSide; r++)
		for (long c = 0; c < moveSide; c++) {
			if (pTmp->specialType != SPECIAL_NONE) {
				if (pTmp->passable) {
					specialAreaFootPrints[pTmp->specialID].cells[specialAreaFootPrints[pTmp->specialID].numCells][0] = r;
					specialAreaFootPrints[pTmp->specialID].cells[specialAreaFootPrints[pTmp->specialID].numCells][1] = c;
					specialAreaFootPrints[pTmp->specialID].numCells++;
				}
			}
			pTmp++;
		}

	printf("[MOVE_REBUILD] MOVE_buildData moveSide=%d specials=%ld\n",
	       moveSide, gateCount + wallCount + landBridgeCount);
	fflush(stdout);
	EditorDataTrace("EditorData::RebuildMoveFromCurrentTerrain: MOVE_buildData moveSide=%d specials=%ld",
		moveSide, gateCount + wallCount + landBridgeCount);

	MOVE_buildData(moveSide, moveSide, pInfo,
		gateCount + wallCount + landBridgeCount, specialAreaFootPrints);

	free(pInfo);

	// Post-build validation: MOVE_buildData should have set GameMap.
	if (!GameMap) {
		SetError("MOVE_buildData completed but GameMap is still null.");
		return false;
	}

	printf("[MOVE_REBUILD] ok: GameMap=%p\n", (void*)GameMap);
	fflush(stdout);
	EditorDataTrace("EditorData::RebuildMoveFromCurrentTerrain: done, GameMap=%p", (void*)GameMap);
	gEditorDataMoveDataReadyForFullSave = true;
	return true;
}

//-------------------------------------------------------------------------------------------------
bool EditorData::save( const char* fileName, bool quickSave )
{
	EditorInterface::instance()->SetBusyMode();

	setMapName( fileName );
	
	char path[256];
	
	// append ".pak"
	strcpy( path, fileName );
	
	char backupPath[256];
	char originalPath[256];

	strcpy( originalPath, fileName );
	strcpy( backupPath, originalPath );
	strcat( backupPath,".old" );
	//remove(backupPath);
	//rename(originalPath, backupPath);
	MoveFileEx(originalPath, backupPath, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);

	CString backupFit;
	CString newOriginalFit;
	newOriginalFit = originalPath;
	newOriginalFit.MakeLower();
	newOriginalFit.Replace(".pak",".fit");
	backupFit = backupPath;
	backupFit.MakeLower();
	backupFit.Replace( ".pak", ".fit" );
	//remove(backupPath);
	//rename(originalPath, backupPath);
	MoveFileEx(newOriginalFit, backupFit, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);

	//Don't move the ABL file.  Editor doesn't write one out!!!
	/*
	strcpy( originalPath, base );
	strcat( originalPath,".abl" );
	strcpy( backupPath, originalPath );
	strcat( backupPath,".old" );
	//remove(backupPath);
	//rename(originalPath, backupPath);
	MoveFileEx(originalPath, backupPath, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	*/

	// ARM

	bool initializedCOM = false;

	if (!armProvider)
	{
		CoInitialize(NULL);
		// by Methuselas: keep Editor provider identity on the Editor-owned
		// SemVer lane instead of the legacy engine stamp.
		armProvider = CreateProviderEngine("MC2Editor", (char*)EditorVersion_GetSemVer());
		initializedCOM = true;
	}

	IProviderAssetPtr mapAssetPtr = armProvider->OpenAsset(newOriginalFit.GetBuffer(), AssetType_Physical, ProviderType_Primary);
	mapAssetPtr->AddProperty("Type", "Mission");
	mapAssetPtr->AddRelationship("PackedBinary", fileName);

	mapAsset = (IProviderAsset*)mapAssetPtr;

	EditorDataTrace("EditorData::save: enter: quickSave=%d moveReady=%d moveDirty=%d GameMap=%p",
		quickSave ? 1 : 0,
		gEditorDataMoveDataReadyForFullSave ? 1 : 0,
		s_moveDirty ? 1 : 0,
		(void*)GameMap);

	// For full saves: rebuild MOVE only if dirty (terrain/objects changed since last build).
	// If the .pak was just loaded, s_moveDirty=false and MOVE is already in systemHeap from
	// MOVE_readData.  Unconditionally calling RebuildMoveFromCurrentTerrain here would
	// double-allocate from systemHeap (old + new MOVE data both live) → heap exhausted → crash.
	//
	// RebuildMoveIfDirty() short-circuits when not dirty and returns true immediately.
	// IsMoveDataReadyForFullSave() is then the authoritative gate for whether to write MOVE.
	bool moveRebuildOk = false;
	if (!quickSave)
	{
		std::string moveErr;
		RebuildMoveIfDirty(&moveErr);
		if (!moveErr.empty()) {
			// Rebuild was attempted and failed -- save proceeds without MOVE data.
			EditorDataTrace("EditorData::save: MOVE rebuild failed: %s", moveErr.c_str());
		}
		// True if MOVE was loaded from disk, or if dirty rebuild just succeeded.
		moveRebuildOk = IsMoveDataReadyForFullSave();
		EditorDataTrace("EditorData::save: after MOVE block: moveRebuildOk=%d GameMap=%p",
			moveRebuildOk ? 1 : 0, (void*)GameMap);
	}
 
	// create a pak file with the correct number of entries
	EditorDataTrace("EditorData::save: creating pak: %s", path);
	PacketFile file;
	if ( NO_ERR != file.create( path ) )
	{
		char buffer[512];
		EditorSafeLoadString( IDS_INVALID_FILE, buffer, 256, gameResourceHandle );
		char buffer2[512];
		sprintf( buffer2, buffer, path );
		//MessageBox( NULL, buffer2, NULL, MB_OK );
		/*I think MessageBox() would not be modal wrt the application.*/
		if (!g_cliSuppressModals) AfxMessageBox(buffer2);
		EditorInterface::instance()->UnsetBusyMode();
		return false;
	}

	//------------------------------------------------------------------------
	// This reserve MUST come after we've initialized and built the move data.
	// Gate: MOVE_saveData requires GameMap (set by MOVE_buildData).
	// Only query packet count if moveRebuildOk -- never call with GameMap==NULL.
	DWORD movePacketCount = 0;
	if (!quickSave && moveRebuildOk)
	{
		EditorDataTrace("EditorData::save: before MOVE_saveData(NULL): GameMap=%p", (void*)GameMap);
		movePacketCount = MOVE_saveData(NULL);
		EditorDataTrace("EditorData::save: MOVE_saveData(NULL) count=%lu", (unsigned long)movePacketCount);
	}
	else if (!quickSave)
	{
		EditorDataTrace("EditorData::save: skipping MOVE_saveData(NULL): moveRebuildOk=false");
	}
	DWORD numPackets = (!quickSave && moveRebuildOk) ? (5 + movePacketCount) : 6;
	EditorDataTrace("EditorData::save: numPackets=%lu land=%p", (unsigned long)numPackets, (void*)land);
	file.reserve(numPackets, false);
	land->unselectAll();
	EditorDataTrace("EditorData::save: land->save packet 0");
	bool bRetVal = land->save( &file, 0, (0.0 < eye->day2NightTransitionTime) ) ? true : false;
	EditorDataTrace("EditorData::save: EditorObjectMgr::save packet 1");
	bRetVal = EditorObjectMgr::instance()->save( file, 1 ) && bRetVal;

	if (!quickSave)
	{
		EditorDataTrace("EditorData::save: before saveTacMap");
		saveTacMap( &file, 3 );
		EditorDataTrace("EditorData::save: after saveTacMap");
		if (moveRebuildOk)
		{
			EditorDataTrace("EditorData::save: before MOVE_saveData(file): GameMap=%p", (void*)GameMap);
			MOVE_saveData(&file, 4);
			EditorDataTrace("EditorData::save: after MOVE_saveData(file)");
		}
		else
		{
			// Packet 4 is reserved but empty -- MOVE rebuild failed.
			// Mission loads but AI movement will not work.
			EditorDataTrace("EditorData::save: skipped MOVE_saveData(file): moveRebuildOk=false");
		}
	}
	else
	{
		//This will cause game to assert!
		//So they know to save a real version!
	}

	//Added a unique identifier to each saved packet file.
	// Used by multiplayer to insure the same version.
	GUID id;
	CoCreateGuid(&id);

	EditorDataTrace("EditorData::save: writePacket GUID (packet %lu)", (unsigned long)(numPackets-1));
	file.writePacket(numPackets-1,MemoryPtr(&id),sizeof(GUID));

	EditorDataTrace("EditorData::save: file.close()");
	file.close();

	CString newFit;
	newFit = fileName;
	newFit.MakeLower();
	newFit.Replace( ".pak", ".fit" );

	FitIniFile fitFile;
	int result = fitFile.create( (char*)(LPCSTR)newFit );
	if ( result != NO_ERR )
	{
		char buffer[512];
		EditorSafeLoadString( IDS_INVALID_FILE, buffer, 256, gameResourceHandle );
		char buffer2[512];
		sprintf( buffer2, buffer, path );
		//MessageBox( NULL, buffer2, NULL, MB_OK );
		/*I think MessageBox() would not be modal wrt the application.*/
		if (!g_cliSuppressModals) AfxMessageBox(buffer2);
		EditorInterface::instance()->UnsetBusyMode();
		return false;
	}
	gosASSERT( result == NO_ERR );

	EditorDataTrace("EditorData::save: land->saveColorMapName");
	land->saveColorMapName(&fitFile);

	EditorDataTrace("EditorData::save: saveMissionFitFileStuff");
	bRetVal = saveMissionFitFileStuff(fitFile) && bRetVal;

	EditorDataTrace("EditorData::save: fitFile.close()");
	fitFile.close();
 
	if (!quickSave)
	{
		if (!justResaveAllMaps)
			EditorInterface::instance()->updateTacMap();
	}

	if (DetailTextureNeedsSaving())
	{
		if ( land->terrainTextures2  && (land->terrainTextures2->colorMapStarted))
		{
			char name2[1024];
			if (land->colorMapName)
				strcpy(name2,land->colorMapName);
			else
				_splitpath(fileName,NULL,NULL,name2,NULL);
			land->terrainTextures2->saveDetailTexture(name2);
			DetailTextureNeedsSaving(false);
		}
	}

	if (WaterTextureNeedsSaving())
	{
		if ( land->terrainTextures2  && (land->terrainTextures2->colorMapStarted))
		{
			char name2[1024];
			if (land->colorMapName)
				strcpy(name2,land->colorMapName);
			else
				_splitpath(fileName,NULL,NULL,name2,NULL);
			land->terrainTextures2->saveWaterTexture(name2);
			WaterTextureNeedsSaving(false);
		}
	}

	if (WaterDetailTextureNeedsSaving())
	{
		if ( land->terrainTextures2  && (land->terrainTextures2->colorMapStarted))
		{
			char name2[1024];
			if (land->colorMapName)
				strcpy(name2,land->colorMapName);
			else
				_splitpath(fileName,NULL,NULL,name2,NULL);
			land->terrainTextures2->saveWaterDetail(name2);
			WaterDetailTextureNeedsSaving(false);
		}
	}

	EditorObjectMgr::BUILDING_LIST completeBuildingList = EditorObjectMgr::instance()->getBuildings();
	if (completeBuildingList.Count() > 0)
	{
		EditorObjectMgr::BUILDING_LIST::EConstIterator it = completeBuildingList.Begin();
		std::set<long> doneIds;

		while (!it.IsDone())
		{
			long id = (*it)->getID();

			if (doneIds.count(id) == 0) // filter repeats
			{
				doneIds.insert(id);

				if (id)
				{
					const char * objFilename = EditorObjectMgr::instance()->getFileName(id);
					char buf[512] = {0};

					if (objFilename[0])
					{
						strcpy(buf, "Data\\TGL\\");
						strcat(buf, objFilename);
						strcat(buf, ".ini");
						mapAssetPtr->AddRelationship("Object", buf);

						if (armProvider)
						{
							IProviderAssetPtr objAssetPtr = armProvider->OpenAsset(buf, 
								AssetType_Physical, ProviderType_Primary);

							objAssetPtr->AddProperty("DisplayName", (*it)->getDisplayName());
							
							switch ((*it)->getType())
							{
							case SPRITE_TREE:
								objAssetPtr->AddProperty("ObjectType", "Sprite Tree");
								break;
							case VFX_APPEAR:
								objAssetPtr->AddProperty("ObjectType", "VFX Appearance");
								break;
							case FSY_APPEAR:
								objAssetPtr->AddProperty("ObjectType", "FSY Appearance");
								break;
							case LINE_APPEAR:
								objAssetPtr->AddProperty("ObjectType", "Line Appearance");
								break;
							case GV_TYPE:
								objAssetPtr->AddProperty("ObjectType", "Ground Vehicle");
								break;
							case ARM_APPEAR:
								objAssetPtr->AddProperty("ObjectType", "Arm Appearance");
								break;
							case BUILD_APPEAR:
								objAssetPtr->AddProperty("ObjectType", "Building Appearance");
								break;
							case ELM_TREE:
								objAssetPtr->AddProperty("ObjectType", "Elm Tree");
								break;
							case PU_TYPE:
								objAssetPtr->AddProperty("ObjectType", "PU");
								break;
							case SMOKE_TYPE:
								objAssetPtr->AddProperty("ObjectType", "Smoke");
								break;
							case POLY_APPEARANCE:
								objAssetPtr->AddProperty("ObjectType", "Poly Appearance");
								break;
							case MLR_APPEARANCE:
								objAssetPtr->AddProperty("ObjectType", "MLR Appearance");
								break;
							case MECH_TYPE:
								objAssetPtr->AddProperty("ObjectType", "Mech");
								break;
							case TREED_TYPE:
								objAssetPtr->AddProperty("ObjectType", "Tree");
								break;
							case BLDG_TYPE:
								objAssetPtr->AddProperty("ObjectType", "Building");

								switch ((*it)->getSpecialType())
								{									
								case EditorObjectMgr::UNSPECIAL:
									objAssetPtr->AddProperty("BuildingType", "Unspecial");
									break;
								case EditorObjectMgr::NORMAL_BUILDING:
									objAssetPtr->AddProperty("BuildingType", "Normal");
									break;
								case EditorObjectMgr::DROP_ZONE:
									objAssetPtr->AddProperty("BuildingType", "Drop Zone");
									break;
								case EditorObjectMgr::TURRET_CONTROL:
									objAssetPtr->AddProperty("BuildingType", "Turret Control");
									break;
								case EditorObjectMgr::GATE_CONTROL:
									objAssetPtr->AddProperty("BuildingType", "Gate Control");
									break;
								case EditorObjectMgr::POWER_STATION:
									objAssetPtr->AddProperty("BuildingType", "Power Station");
									break;
								case EditorObjectMgr::TURRET_GENERATOR:
									objAssetPtr->AddProperty("BuildingType", "Turret Generator");
									break;
								case EditorObjectMgr::SENSOR_CONTROL:
									objAssetPtr->AddProperty("BuildingType", "Sensor Control");
									break;
								case EditorObjectMgr::EDITOR_GATE:
									objAssetPtr->AddProperty("BuildingType", "Editor Gate");
									break;
								case EditorObjectMgr::EDITOR_TURRET:
									objAssetPtr->AddProperty("BuildingType", "Editor Turret");
									break;
								case EditorObjectMgr::SENSOR_TOWER:
									objAssetPtr->AddProperty("BuildingType", "Sensor Tower");
									break;
								case EditorObjectMgr::EDITOR_BRIDGE:
									objAssetPtr->AddProperty("BuildingType", "Editor Bridge");
									break;
								case EditorObjectMgr::BRIDGE_CONTROL:
									objAssetPtr->AddProperty("BuildingType", "Bridge Control");
									break;
								case EditorObjectMgr::SPOTLIGHT:
									objAssetPtr->AddProperty("BuildingType", "Spotlight");
									break;
								case EditorObjectMgr::SPOTLIGHT_CONTROL:
									objAssetPtr->AddProperty("BuildingType", "Spotlight Control");
									break;
								case EditorObjectMgr::DROPZONE:
									objAssetPtr->AddProperty("BuildingType", "Drop Zone");
									break;
								case EditorObjectMgr::NAV_MARKER:
									objAssetPtr->AddProperty("BuildingType", "Nav Marker");
									break;
								case EditorObjectMgr::WALL:
									objAssetPtr->AddProperty("BuildingType", "Wall");
									break;
								case EditorObjectMgr::LOOKOUT:
									objAssetPtr->AddProperty("BuildingType", "Lookout");
									break;
								case EditorObjectMgr::RESOURCE_BUILDING:
									objAssetPtr->AddProperty("BuildingType", "Resource Building");
									break;
								case EditorObjectMgr::HELICOPTER:
									objAssetPtr->AddProperty("BuildingType", "Helicopter");
									break;
								}
								break;
							}

							objAssetPtr->Close();
						}
					}
				}
			}
			
			it++;
		}
	}

	EditorDataTrace("EditorData::save: mapAssetPtr->Close()");
	// ARM
	mapAssetPtr->Close();
	mapAssetPtr = NULL;
	mapAsset = NULL;

	if (initializedCOM)
	{
		CoUninitialize();
	}

	EditorDataTrace("EditorData::save: done: bRetVal=%d", bRetVal ? 1 : 0);
	EditorData::instance->MissionNeedsSaving(false);

	EditorInterface::instance()->undoMgr.NoteThatASaveHasJustOccurred();

	PlaySound("SystemDefault",NULL,SND_ASYNC);

	EditorInterface::instance()->UnsetBusyMode();

	return bRetVal;
}

//-------------------------------------------------------------------------------------------------
bool EditorData::quickSave( const char* fileName )
{
	return save(fileName, true/*quick save enabled*/);
}

bool EditorData::saveMissionFitFileStuff( FitIniFile &fitFile )
{
	bool bRetVal = true;
	EditorDataTrace("EditorData::saveMissionFitFileStuff: eye->save");
	eye->save( &fitFile );
	EditorDataTrace("EditorData::saveMissionFitFileStuff: land->save(fitFile)");
	bRetVal = land->save( &fitFile ) && bRetVal;

	EditorDataTrace("EditorData::saveMissionFitFileStuff: saveMechs");
	EditorObjectMgr::instance()->saveMechs( fitFile );
	EditorDataTrace("EditorData::saveMissionFitFileStuff: saveDropZones");
	EditorObjectMgr::instance()->saveDropZones( fitFile );
	EditorDataTrace("EditorData::saveMissionFitFileStuff: saveForests");
	EditorObjectMgr::instance()->saveForests( fitFile );

	EditorDataTrace("EditorData::saveMissionFitFileStuff: missionSettings.save");
	missionSettings.save( &fitFile );

	EditorDataTrace("EditorData::saveMissionFitFileStuff: MissionSettings block");
	fitFile.writeBlock( "MissionSettings" );
	fitFile.writeIdString( "MissionName", EditorData::instance->MissionName() );
	fitFile.writeIdBoolean( "MissionNameUseResourceString", EditorData::instance->MissionNameUseResourceString() );
	fitFile.writeIdULong( "MissionNameResourceStringID", EditorData::instance->MissionNameResourceStringID() );
	fitFile.writeIdString( "Author", EditorData::instance->Author() );
	sWriteIdString( &fitFile, "Blurb", EditorData::instance->Blurb().Data() );
	fitFile.writeIdBoolean( "BlurbUseResourceString", EditorData::instance->BlurbUseResourceString() );
	fitFile.writeIdULong( "BlurbResourceStringID", EditorData::instance->BlurbResourceStringID() );
	sWriteIdString( &fitFile, "Blurb2", EditorData::instance->Blurb2().Data() );
	fitFile.writeIdBoolean( "Blurb2UseResourceString", EditorData::instance->Blurb2UseResourceString() );
	fitFile.writeIdULong( "Blurb2ResourceStringID", EditorData::instance->Blurb2ResourceStringID() );
	fitFile.writeIdFloat( "TimeLimit", EditorData::instance->TimeLimit() );
	fitFile.writeIdFloat( "DropWeightLimit", EditorData::instance->DropWeightLimit() );
	fitFile.writeIdLong( "ResourcePoints", EditorData::instance->InitialResourcePoints() );
	fitFile.writeIdULong( "IsSinglePlayer", EditorData::instance->IsSinglePlayer()?1:0 );
	fitFile.writeIdULong( "MaximumNumberOfTeams", EditorData::instance->MaxTeams() );
	fitFile.writeIdULong( "MaximumNumberOfPlayers", EditorData::instance->MaxPlayers() );
	fitFile.writeIdUChar( "scenarioTuneNum", EditorData::instance->ScenarioTune() );
	fitFile.writeIdString( "AVIFilename", EditorData::instance->VideoFilename() );
	fitFile.writeIdLong( "AdditionalCBills", EditorData::instance->m_CBills );
	fitFile.writeIdULong( "NumRandomRPbuildings", EditorData::instance->NumRandomRPbuildings() );
	fitFile.writeIdString( "DownloadURL", EditorData::instance->DownloadURL() );
	fitFile.writeIdULong( "MissionType", EditorData::instance->MissionType() );
	fitFile.writeIdBoolean( "AirStrikesEnabledDefault", EditorData::instance->AirStrikesEnabledDefault() );
	fitFile.writeIdBoolean( "MineLayersEnabledDefault", EditorData::instance->MineLayersEnabledDefault() );
	fitFile.writeIdBoolean( "ScoutCoptersEnabledDefault", EditorData::instance->ScoutCoptersEnabledDefault() );
	fitFile.writeIdBoolean( "SensorProbesEnabledDefault", EditorData::instance->SensorProbesEnabledDefault() );
	fitFile.writeIdBoolean( "UnlimitedAmmoEnabledDefault", EditorData::instance->UnlimitedAmmoEnabledDefault() );
	fitFile.writeIdBoolean( "AllTechEnabledDefault", EditorData::instance->AllTechEnabledDefault() );
	fitFile.writeIdBoolean( "RepairVehicleEnabledDefault", EditorData::instance->RepairVehicleEnabledDefault() );
	fitFile.writeIdBoolean( "SalvageCraftEnabledDefault", EditorData::instance->SalvageCraftEnabledDefault() );
	fitFile.writeIdBoolean( "ResourceBuildingsEnabledDefault", EditorData::instance->ResourceBuildingsEnabledDefault() );
	fitFile.writeIdBoolean( "NoVariantsEnabledDefault", EditorData::instance->NoVariantsEnabledDefault() );
	fitFile.writeIdBoolean( "ArtilleryPieceEnabledDefault", EditorData::instance->ArtilleryPieceEnabledDefault() );
	fitFile.writeIdBoolean( "RPsForMechsEnabledDefault", EditorData::instance->RPsForMechsEnabledDefault() );

	fitFile.writeBlock( "Weather" );
	fitFile.writeIdULong( "MaxRainDrops", EditorData::instance->MaxRaindrops() );
	fitFile.writeIdFloat( "StartingRainLevel", EditorData::instance->StartingRainLevel() );
	fitFile.writeIdLong( "ChanceOfRain", EditorData::instance->ChanceOfRain() );
	fitFile.writeIdFloat( "BaseLighteningChance", EditorData::instance->BaseLightningChance() );

	fitFile.writeBlock("TheSky");
	fitFile.writeIdLong("SkyNumber",EditorData::instance->TheSkyNumber());
	
	EditorDataTrace("EditorData::saveMissionFitFileStuff: saveObjectives");
	saveObjectives( &fitFile );
	EditorDataTrace("EditorData::saveMissionFitFileStuff: PlayersRef().Save");
	PlayersRef().Save( &fitFile );

	EditorDataTrace("EditorData::saveMissionFitFileStuff: Music/Script blocks");
	fitFile.writeBlock( "Music" );
	fitFile.writeIdUChar( "scenarioTuneNum", 0 );
	fitFile.writeBlock( "Script" );
	fitFile.writeIdString( "ScenarioScript", missionScriptName );
	EditorDataTrace("EditorData::saveMissionFitFileStuff: done");

	if (mapAsset)
	{
		char buf[512] = {0};
		strncpy(buf, EditorData::instance->MissionName(), 511);		
		if (buf[0])
		{
			mapAsset->AddProperty("MissionName", buf);
		}

		memset(buf, 0, 512);
		strncpy(buf, EditorData::instance->Author(), 511);
		if (buf[0])
		{
			mapAsset->AddProperty("MissionAuthor", buf);
		}

		mapAsset->AddProperty("IsSinglePlayer", EditorData::instance->IsSinglePlayer() ? "Yes":"No");
		
		switch (EditorData::instance->MissionType())
		{
		case MISSION_TYPE_ELIMINATION:
			mapAsset->AddProperty("MissionType", "Elimination");
			break;
		case MISSION_TYPE_KING_OF_THE_HILL:
			mapAsset->AddProperty("MissionType", "KingOfTheHill");
			break;
		case MISSION_TYPE_CAPTURE_BASE:
			mapAsset->AddProperty("MissionType", "CaptureBase");
			break;
		case MISSION_TYPE_TERRITORIES:
			mapAsset->AddProperty("MissionType", "Territories");
			break;
		case MISSION_TYPE_LAST_MAN_STANDING:
			mapAsset->AddProperty("MissionType", "LastManStanding");
			break;
		case MISSION_TYPE_LAST_MAN_ON_THE_HILL:
			mapAsset->AddProperty("MissionType", "LastManOnTheHill");
			break;
		default:
			mapAsset->AddProperty("MissionType", "(Other)");
			break;
		}

		if (missionScriptName[0])
		{
			strcpy(buf, "Data\\Missions\\");
			strcat(buf, missionScriptName);
			strcat(buf, ".abl");
			mapAsset->AddRelationship("ScenarioScript", buf);
		}
		
		if (EditorData::instance->VideoFilename()[0])
		{
			strcpy(buf, "Data\\Movies\\");
			strcat(buf, EditorData::instance->VideoFilename());
			strcat(buf, ".abl");
			mapAsset->AddRelationship("AVIFilename", buf);
		}

	}

	return bRetVal;
}

void EditorData::setMapName( const char* name )
{
	if ( name )
	{
		gosASSERT( strlen( name ) < 256 );
		strcpy( mapName, name );
	}
	else
		mapName[0] = 0;

	updateTitleBar();
}

void EditorData::updateTitleBar()
{
	/*
		Editor migration note:

		The original editor loaded the title-bar format and "Untitled" text
		from the legacy GameOS/Win32 resource system via EditorSafeLoadString().

		In the Remastered runtime, that resource table is not initialized when
		the editor constructs EditorData. Calling EditorSafeLoadString() here eventually
		enters legacy resource lookup(), which dereferences a null resource table
		and crashes during startup.

		Keep this fix local to the editor. Do not patch Remastered's resource
		system just to satisfy this old UI label path.
	*/

	if (!EditorData::instance)
		return;

	char tmp2[512];

	// by Methuselas: the Editor Remaster owns its own SemVer now.
	// Keep the title bar tied to EditorVersion.* so Editor migration fixes
	// can advance independently from the engine version.
	if (0 != strcmp("", mapName))
	{
		sprintf(tmp2, "%s - %s", EditorVersion_GetDisplayString(), mapName);
	}
	else
	{
		sprintf(tmp2, "%s - Untitled", EditorVersion_GetDisplayString());
	}

	CWnd* mainWnd = AfxGetMainWnd();
	if (mainWnd)
		mainWnd->SetWindowText(tmp2);
}

void EditorData::MaxPlayers(int maxPlayers)
{
	if (2 > maxPlayers)
	{
		gosASSERT(false);
		maxPlayers = 2;
	}
	else if (GAME_MAX_PLAYERS < maxPlayers)
	{
		gosASSERT(false);
		maxPlayers = GAME_MAX_PLAYERS;
	}

	m_maxPlayers = maxPlayers;
}

bool EditorData::saveObjectives( FitIniFile* file )
{
	file->writeBlock( "Objectives Version" );
	file->writeIdULong( "Version", 3 );
	TeamsRef().Save( file );
	if (!justResaveAllMaps) {
		if (TeamsRef().ThereAreObjectivesWithNoConditions()) {
			// Non-blocking save-validation warning (objectives missing conditions).
			// Headless smoke: suppress + capture the text instead of hanging on a
			// modal with no one to click OK. Continue is the only/safe response.
			if (g_cliSuppressModals) {
				extern void EditorSmokeLogSuppressedModal(const char* text, const char* caption);
				char buffer[512];
				EditorSafeLoadString( IDS_OBJECTIVES_WITH_NO_CONDITIONS, buffer, 512, gameResourceHandle );
				EditorSmokeLogSuppressedModal( buffer, "Warning" );
			} else {
				AfxMessageBox(IDS_OBJECTIVES_WITH_NO_CONDITIONS);
			}
		}
	}
	return true;
}
