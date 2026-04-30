/***************************************************************
* FILENAME: EditorData.cpp
* DESCRIPTION: Implements Editor terrain, mission data, save, and load behavior.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Removed legacy provider-version dependency and uses EditorVersion SemVer.
****************************************************************/

 #define EDITORDATA_CPP
#include "stdafx.h"
#include "EditorData.h"
#include "MCLib.h"
#include "EditorObjectMgr.h"
#include "mclibresource.h"
#include "resource.h"
#include "action.h"
#include "move.h"
#include "Vertex.h"
#include "terrtxm.h"
#include "editorinterface.h"
#include "ECharString.h"

#include <stdarg.h>
#include <stdio.h>

// Patch savefix: MOVE packet readiness is tracked separately from terrain readiness.
// Existing missions set this after packet 4 is successfully read. Newly generated
// blank maps do not safely initialize the MOVE/special-area backend yet; attempting
// MOVE_buildData/MOVE_saveData for them is the known Save/Save As CTD point.
static bool gEditorDataMoveDataReadyForFullSave = false;

static void EditorDataTrace(const char* fmt, ...)
{
	FILE* f = fopen("editor-startup.log", "a");
	if (!f)
		return;
	va_list args;
	va_start(args, fmt);
	vfprintf(f, fmt, args);
	va_end(args);
	fputc('\n', f);
	fclose(f);
}

// From multplyr.h
// Can't include because of redefinition of MissionSettings struct
typedef enum {
	MISSION_TYPE_ELIMINATION,
	MISSION_TYPE_KING_OF_THE_HILL,
	MISSION_TYPE_CAPTURE_BASE,
	MISSION_TYPE_TERRITORIES,
	MISSION_TYPE_LAST_MAN_STANDING,
	MISSION_TYPE_LAST_MAN_ON_THE_HILL,
	MISSION_TYPE_OTHER,
	NUM_MISSION_TYPES
} MissionType;

#pragma warning( disable:4201 )
#include "mmsystem.h"
#pragma warning( default:4201 )

#pragma warning( disable:4244 )

#include <objbase.h>

// ARM
#include "../ARM/Microsoft.Xna.Arm.h"
using namespace Microsoft::Xna::Arm;
IProviderAsset * mapAsset = NULL;
IProviderAsset * mechAsset = NULL;
#include <set>
#include "EditorResourceFallback.h"
#include "EditorVersion.h"

char EditorData::mapName[256];
unsigned long* EditorData::tacMapBmp = NULL;

extern bool justResaveAllMaps;

EditorData* EditorData::instance = NULL;

char missionScriptName[1024]="M0101";

bool MissionSettings::save( FitIniFile* file )
{
	file->writeBlock( "Artillery" );
	
	file->writeIdLong( "NumLargeStrikes", largeArtillery );
	file->writeIdLong( "NumSmallStrikes", smallArtillery );
	file->writeIdLong( "NumSensorStrikes", sensors );
	file->writeIdLong( "NumCameraStrikes", cameraDrones );

	return true;
}


EditorData::EditorData()
{ 
	gosASSERT( !instance ); instance = this; 
	updateTitleBar();
	tacMapBmp = NULL;
	clear();
}

EditorData::~EditorData()
{
	if ( land )
		delete land;

	land = NULL;

	instance = NULL;

	if ( tacMapBmp )
		free( tacMapBmp );

	tacMapBmp = NULL;


		
}
//-------------------------------------------------------------------------------------------------
bool EditorData::clear()
{
	gEditorDataMoveDataReadyForFullSave = false;
	EditorDataTrace("EditorData::clear: move data marked not ready");

	// ActionUndoMgr::instance is set by EditorInterface's constructor (it holds
	// the ActionUndoMgr as a value member). During EditorData construction inside
	// "new Editor()", EditorInterface has not been constructed yet, so instance is
	// NULL. Guard here; EditorInterface::init() will call clear() again once ready.
	if (ActionUndoMgr::instance)
		ActionUndoMgr::instance->Reset();

	if ( land )
	{
		land->destroy();
		delete land;
		land = NULL;
	}

	if ( EditorObjectMgr::instance() )
		EditorObjectMgr::instance()->clear();

	EditorData::instance->MissionName(_T(""));
	EditorData::instance->MissionNameUseResourceString(false);
	EditorData::instance->MissionNameResourceStringID(0);
	EditorData::instance->Author(_T(""));
	EditorData::instance->Blurb(_T(""));
	EditorData::instance->BlurbUseResourceString(false);
	EditorData::instance->BlurbResourceStringID(0);
	EditorData::instance->Blurb2(_T(""));
	EditorData::instance->Blurb2UseResourceString(false);
	EditorData::instance->Blurb2ResourceStringID(0);
	EditorData::instance->TimeLimit(-1.0/*seconds*/);
	EditorData::instance->DropWeightLimit(300/*lbs*//*default initial drop weight limit*/);
	EditorData::instance->InitialResourcePoints(100000/*default initial rp*/);
	EditorData::instance->CBills(0);
	EditorData::instance->IsSinglePlayer(false);
	EditorData::instance->MaxTeams(2);
	EditorData::instance->MaxPlayers(2);
	EditorData::instance->TeamsRef().Clear();
	EditorData::instance->PlayersRef().Clear();
	EditorData::instance->ScenarioTune(0);
	EditorData::instance->VideoFilename(_T(""));
	EditorData::instance->NumRandomRPbuildings(0);
	EditorData::instance->DownloadURL(_T(""));
	EditorData::instance->MissionType(0);
	EditorData::instance->AirStrikesEnabledDefault(true);
	EditorData::instance->MineLayersEnabledDefault(true);
	EditorData::instance->ScoutCoptersEnabledDefault(true);
	EditorData::instance->SensorProbesEnabledDefault(true);
	EditorData::instance->UnlimitedAmmoEnabledDefault(true);
	EditorData::instance->AllTechEnabledDefault(true);
	EditorData::instance->RepairVehicleEnabledDefault(true);
	EditorData::instance->SalvageCraftEnabledDefault(true);
	EditorData::instance->ResourceBuildingsEnabledDefault(true);
	EditorData::instance->NoVariantsEnabledDefault(true);
	EditorData::instance->ArtilleryPieceEnabledDefault(true);
	EditorData::instance->RPsForMechsEnabledDefault(true);

	memset( mapName, 0, sizeof( mapName ) );

	if ( tacMapBmp )
	{
		free( tacMapBmp );
		tacMapBmp = NULL;
	}

	EditorData::instance->missionSettings.clear();

	//weather
	EditorData::instance->MaxRaindrops(0);
	EditorData::instance->StartingRainLevel(0.0);
	EditorData::instance->ChanceOfRain(0.0);
	EditorData::instance->BaseLightningChance(0.0);
	
	EditorData::instance->TheSkyNumber(1);

	EditorData::instance->MissionNeedsSaving(false);
	EditorData::instance->DetailTextureNeedsSaving(false);
	EditorData::instance->WaterTextureNeedsSaving(false);
	EditorData::instance->WaterDetailTextureNeedsSaving(false);

	if (mcTextureManager)
		mcTextureManager->flush();		//Toss the textures we aren't using anymore!	
	
	//TG_Shape::tglHeap->dumpRecordLog();	//Anything left in heap at this point is a leak!

	return true;
}

//-------------------------------------------------------------------------------------------------

static long sReadIdBoolean(FitIniFile* missionFile, const char *varName, bool &value) {
	long result = 0;
	bool tmpBool;
	result = missionFile->readIdBoolean((char *)varName, tmpBool);
	if (NO_ERR != result) {
		//assert(false);
	} else {
		value = tmpBool;
	}
	return result;
}

static long sReadIdWholeNum(FitIniFile* missionFile, const char *varName, int &value) {
	long result = 0;
	unsigned long tmpULong;
	result = missionFile->readIdULong((char *)varName, tmpULong);
	if (NO_ERR != result) {
		//assert(false);
	} else {
		value = tmpULong;
	}
	return result;
}

static long sReadIdString(FitIniFile* missionFile, const char *varName, ECharString &ECStr) {
	long result = 0;
	char buffer[2001/*buffer size*/]; buffer[0] = '\0';
	result = missionFile->readIdString((char *)varName, buffer, 2001/*buffer size*/ - 1);
	CString CStr = buffer;
	/*readIdString can't read in "\r\n"*/
	CStr.Replace("\n", "\r\n");
	if ((NO_ERR != result) && (BUFFER_TOO_SMALL != result)) {
		//gosASSERT(false);
	} else {
		ECStr = CStr.GetBuffer(0);
	}
	return result;
}

static long sWriteIdString(FitIniFile* missionFile, const char *varName, const char *szStr) {
	if (!szStr) { return !(NO_ERR); }
	long result = 0;
	CString CStr = szStr;
	/*readIdString can't read in "\r\n"*/
	CStr.Replace("\r\n", "\n");
	result = missionFile->writeIdString(varName, CStr.GetBuffer(0));
	return result;
}

//-------------------------------------------------------------------------------------------------
bool bIsLoading = false;

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
	EditorDataTrace("EditorData::initTerrainFromPCV: enter fileName=%s", fileName ? fileName : "<null>");

	bool bRetVal = true;
	bIsLoading = true;

	EditorInterface::instance()->SetBusyMode(false/*no redraw*/);

	clear();

	if (!fileName || !fileName[0])
	{
		EditorDataTrace("EditorData::initTerrainFromPCV: empty file name");
		EditorInterface::instance()->UnsetBusyMode();
		bIsLoading = false;
		return false;
	}

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
		//MessageBox( NULL, buffer2, NULL, MB_OK );
		/*I think MessageBox() would not be modal wrt the application.*/
		AfxMessageBox(buffer2);
		EditorInterface::instance()->UnsetBusyMode();
		bIsLoading = false;
		return false;
	}

 	CString camFileName = lowerFileName;
	camFileName.MakeLower();
	camFileName.Replace( ".pak", ".fit" );

	FitIniFile file;

	EditorDataTrace("EditorData::initTerrainFromPCV: camFileName=%s", (const char*)camFileName);
	if ( fileExists( (char*)(const char*)camFileName ) )
	{
		EditorDataTrace("EditorData::initTerrainFromPCV: before cam file open");
		long fitResult = file.open( (char*)(const char*)camFileName );
		EditorDataTrace("EditorData::initTerrainFromPCV: after cam file open result=%ld", fitResult);
	}
	else
	{
		char buffer[512];
		EditorSafeLoadString( IDS_COULDNT_OPEN, buffer, 256, gameResourceHandle );
		char buffer2[512];
		sprintf( buffer2, buffer, camFileName );
		//MessageBox( NULL, buffer2, NULL, MB_OK );
		/*I think MessageBox() would not be modal wrt the application.*/
		AfxMessageBox(buffer2);
		EditorInterface::instance()->UnsetBusyMode();
		bIsLoading = false;
		return false;
	
	}

	EditorDataTrace("EditorData::initTerrainFromPCV: before new Terrain");
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
	eye->init( &file );
	EditorDataTrace("EditorData::initTerrainFromPCV: after eye init");
	EditorDataTrace("EditorData::initTerrainFromPCV: before loadMechs");
	EditorObjectMgr::instance()->loadMechs( file );
	EditorDataTrace("EditorData::initTerrainFromPCV: before loadDropZones");
	EditorObjectMgr::instance()->loadDropZones( file );
	EditorDataTrace("EditorData::initTerrainFromPCV: before loadForests");
	EditorObjectMgr::instance()->loadForests( file );
	EditorDataTrace("EditorData::initTerrainFromPCV: before land->load");
	land->load( &file ) && bRetVal;
	EditorDataTrace("EditorData::initTerrainFromPCV: after land->load bRetVal=%d", bRetVal ? 1 : 0);
	EditorDataTrace("EditorData::initTerrainFromPCV: before recalcWater");
	land->recalcWater();
	EditorDataTrace("EditorData::initTerrainFromPCV: before EditorObjectMgr::load");
	EditorObjectMgr::instance()->load( pFile, 1 );
	EditorDataTrace("EditorData::initTerrainFromPCV: after EditorObjectMgr::load");

	{
		long result = 0;
		result = file.seekBlock("MissionSettings");
		if (NO_ERR == result)
		{
			ECharString tmpECStr = "";
			result = sReadIdString(&file, "MissionName", tmpECStr);
			if (NO_ERR == result)
			{
				EditorData::instance->MissionName(_T(tmpECStr.Data()));
			}

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

			tmpECStr = "";
			result = sReadIdString(&file, "Author", tmpECStr);
			if (NO_ERR == result)
			{
				EditorData::instance->Author(_T(tmpECStr.Data()));
			}

			tmpECStr = "";
			result = sReadIdString(&file, "Blurb", tmpECStr);
			if (NO_ERR == result)
			{
				EditorData::instance->Blurb(_T(tmpECStr.Data()));
			}

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

			tmpECStr = "";
			result = sReadIdString(&file, "Blurb2", tmpECStr);
			if (NO_ERR == result)
			{
				EditorData::instance->Blurb2(_T(tmpECStr.Data()));
			}

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

			tmpECStr = "";
			result = sReadIdString(&file, "AVIFilename", tmpECStr);
			if (NO_ERR == result)
			{
				EditorData::instance->VideoFilename(_T(tmpECStr.Data()));
			}

			tmpULong = 0;
			result = file.readIdULong("NumRandomRPbuildings", tmpULong);
			if (NO_ERR == result)
			{
				EditorData::instance->NumRandomRPbuildings(tmpULong);
			}

			tmpECStr = "";
			result = sReadIdString(&file, "DownloadURL", tmpECStr);
			if (NO_ERR == result)
			{
				EditorData::instance->DownloadURL(_T(tmpECStr.Data()));
			}

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
			result = file.readIdString("ScenarioScript", missionScriptName,1023);
			if (NO_ERR != result)
				strcpy(missionScriptName,"m0101");
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

	EditorData::instance->TeamsRef().Clear();
	EditorData::instance->TeamsRef().Read( &file );
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

//-------------------------------------------------------------------------------------------------
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

	MemoryPtr tgaData = (MemoryPtr)systemHeap->Malloc(tgaFile.fileSize());
	gosASSERT(tgaData != NULL);

	result = tgaFile.read(tgaData,tgaFile.fileSize());

	//---------------------------------------
	// Parse out TGAHeader.
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

	BYTE* pTmp = NULL;

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

	systemHeap->Free(tgaData);

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

void* DecodeJPG( const char* FileName, BYTE* Data, DWORD DataSize, DWORD* TextureWidth, DWORD* TextureHeight, bool TextureLoad, void *pDestSurf );
//-------------------------------------------------------------------------------------------------
bool CreateScaledColorMap(long mapWidth, char *localColorMapName, MemoryPtr tmpRAM, long fileSize)
{
	EditorDataTrace("CreateScaledColorMap: enter mapWidth=%ld fileSize=%ld out=%s",
		mapWidth, fileSize, localColorMapName ? localColorMapName : "<null>");

	if (mapWidth <= 0 || !localColorMapName)
	{
		EditorDataTrace("CreateScaledColorMap: invalid input");
		return false;
	}

	// Terrain colormap pixels are 12.8 texels per map tile.
	long requestedWidth = (long)((float)mapWidth * 12.8f + 0.5f);
	long newWidth = requestedWidth;
	if (newWidth <= 0)
	{
		EditorDataTrace("CreateScaledColorMap: computed width invalid requested=%ld", requestedWidth);
		return false;
	}

	DWORD jpgColorMapWidth = 0;
	DWORD jpgColorMapHeight = 0;
	MemoryPtr decodedImage = NULL;

	if (tmpRAM && fileSize > 0)
	{
		decodedImage = (MemoryPtr)DecodeJPG("Startup.jpg", tmpRAM, fileSize,
			&jpgColorMapWidth, &jpgColorMapHeight, false, NULL);
	}

	EditorDataTrace("CreateScaledColorMap: DecodeJPG image=%p w=%lu h=%lu",
		decodedImage, (unsigned long)jpgColorMapWidth, (unsigned long)jpgColorMapHeight);

	bool useDecodedImage = (decodedImage && jpgColorMapWidth > 0 && jpgColorMapHeight > 0);
	if (useDecodedImage)
	{
		if (newWidth > (long)jpgColorMapWidth)
			newWidth = (long)jpgColorMapWidth;
		if (newWidth > (long)jpgColorMapHeight)
			newWidth = (long)jpgColorMapHeight;
	}
	else
	{
		// The retail random-map JPGs can fail this old DecodeJPG path on the
		// modern build.  Do not fail mission creation and bounce the user back
		// through the terrain/size dialogs forever; emit a valid neutral TGA
		// so the rest of the new-map pipeline can continue.
		EditorDataTrace("CreateScaledColorMap: DecodeJPG failed; writing fallback neutral TGA");
	}

	long physWidth = newWidth * 4;
	long outputBytes = physWidth * newWidth;

	EditorDataTrace("CreateScaledColorMap: requestedWidth=%ld finalWidth=%ld physWidth=%ld",
		requestedWidth, newWidth, physWidth);

	MemoryPtr tmpImage = (MemoryPtr)malloc(outputBytes + sizeof(TGAFileHeader));
	if (!tmpImage)
	{
		EditorDataTrace("CreateScaledColorMap: malloc failed bytes=%ld", outputBytes + (long)sizeof(TGAFileHeader));
		return false;
	}

	TGAFileHeader *output = (TGAFileHeader *)tmpImage;
	memset(output, 0, sizeof(TGAFileHeader));
	output->image_id_len = 0;
	output->color_map = 0;
	output->image_type = UNC_TRUE;
	output->cm_first_entry = 0;
	output->cm_length = 0;
	output->cm_entry_size = 0;
	output->x_origin = 0;
	output->y_origin = 0;
	output->width = (short)newWidth;
	output->height = (short)newWidth;
	output->pixel_depth = 32;
	output->image_descriptor = 32;

	MemoryPtr newImage = tmpImage + sizeof(TGAFileHeader);

	if (useDecodedImage)
	{
		MemoryPtr srcImage = decodedImage;
		for (long i = 0; i < newWidth; i++)
		{
			memcpy(newImage, srcImage, physWidth);
			srcImage += jpgColorMapWidth * 4;
			newImage += physWidth;
		}
	}
	else
	{
		// Patch: flat fallback instead of patterned fallback.
		// The previous generated (x+y)&0x3f grayscale pattern looked like
		// terrain/shadow striping and masked the real failure: selected JPG decode/path.
		for (long y = 0; y < newWidth; y++)
		{
			DWORD *dst = (DWORD *)newImage;
			for (long x = 0; x < newWidth; x++)
			{
				dst[x] = 0xff606060;
			}
			newImage += physWidth;
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
bool EditorData::initTerrainFromTGA( int mapSize, int min, int max, int terrain )
{
	gEditorDataMoveDataReadyForFullSave = false;
	EditorDataTrace("EditorData::initTerrainFromTGA: enter mapSize=%d min=%d max=%d terrain=%d; move data marked not ready", mapSize, min, max, terrain);
	EditorInterface::instance()->SetBusyMode(false/*no redraw*/);

	clear(); // get rid of all the old stuff now
	EditorData::instance->MissionNeedsSaving(true);

	float MinVal = min;
	float MaxVal = max;

	long mapWidth = 0;
	switch (mapSize)
	{
	case 0:
		mapWidth = 60;
		break;

	case 1:
		mapWidth = 80;
		break;

	case 2:
		mapWidth = 100;
		break;

	case 3:
		mapWidth = 120;
		break;

	default:
		mapWidth = 120;
		break;
	}

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
	land->init( mapWidth, NULL, EDITOR_VISIBLE_VERTICES, crap, 100 );
	EditorDataTrace("EditorData::initTerrainFromTGA: after land->init realVerticesMapSide=%ld terrainTextures2=%p", land->realVerticesMapSide, land->terrainTextures2);
	land->setUserSettings( min, max, terrain );
	EditorDataTrace("EditorData::initTerrainFromTGA: after setUserSettings");

	//-----------------------------------------------------------------
	// Startup the Terrain Color Map
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
			CreateDirectory(missionPath, NULL);
			CreateDirectory(warriorPath, NULL);
			CreateDirectory(terrainPath, NULL);
			CreateDirectory(texturePath, NULL);

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
				EditorInterface::instance()->UnsetBusyMode();
				return false;
			}
			src.read(tmpRAM,srcSize);
			src.close();

			if (!CreateScaledColorMap(mapWidth, (char*)(const char*)localColorMapName, tmpRAM, srcSize))
			{
				EditorDataTrace("EditorData::initTerrainFromTGA: CreateScaledColorMap failed");
				free(tmpRAM);
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
			EditorInterface::instance()->UnsetBusyMode();
			return false;
		}
	}
	else
	{
		if (!justResaveAllMaps)
		{
			AfxMessageBox( IDS_MUSTBERIGHTFORMAT );
		}
		EditorInterface::instance()->UnsetBusyMode();
		return 0;
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

	for ( int j = 0; j < land->realVerticesMapSide; ++j )
	{
		for ( int i = 0; i < land->realVerticesMapSide; ++i )
		{
			land->setVertexHeight( j * land->realVerticesMapSide + i, 0 );
		}
	}

	EditorDataTrace("EditorData::initTerrainFromTGA: before recalcWater waterDepth=%f waterElevation=%f",
		Terrain::mapData ? Terrain::mapData->waterDepth : -9999.0f,
		Terrain::waterElevation);
	land->recalcWater();
	{
		long waterCount = 0;
		long dryCount = 0;
		PostcompVertexPtr vertices = Terrain::mapData ? Terrain::mapData->getData() : NULL;
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

	EditorDataWarmTerrain("initTerrainFromTGA");

	EditorInterface::instance()->UnsetBusyMode();
	EditorDataTrace("EditorData::initTerrainFromTGA: success");

	return true;
}

//Temp until we I know it works.
float CliffTerrainAngle = 45.0f;

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

	bool saveMoveDataThisPass = (!quickSave && gEditorDataMoveDataReadyForFullSave);
	// by Methuselas: skipping MOVE data here is intentional CTD prevention for
	// newly generated blank maps, not an incomplete Save/Save-As implementation.
	// Existing missions still rebuild and write MOVE data after packet 4 is read.
	EditorDataTrace("EditorData::save: move data policy quickSave=%d moveReady=%d saveMove=%d",
		quickSave ? 1 : 0,
		gEditorDataMoveDataReadyForFullSave ? 1 : 0,
		saveMoveDataThisPass ? 1 : 0);

	if (!quickSave)
	{
		//-----------------------------------------------------------------------
		// Whenever we call calcWater, we should do this to speed up save times
		_ScenarioMapCellInfo* pInfo = (_ScenarioMapCellInfo *)malloc( sizeof(_ScenarioMapCellInfo) * land->realVerticesMapSide * land->realVerticesMapSide * 9);
		memset( pInfo, 0, sizeof( _ScenarioMapCellInfo ) * land->realVerticesMapSide * land->realVerticesMapSide * 9 );
		MissionMapCellInfo* pTmp = pInfo;

		for ( int i = 0; i < land->realVerticesMapSide; ++i  )
		{
			for ( int cellI = 0; cellI < 3; ++cellI )
			{
				for ( int j = 0; j < land->realVerticesMapSide; ++j )
				{
					for ( int cellJ = 0; cellJ < 3; ++cellJ )
					{
						pTmp->terrain = MC_NONE_TYPE;
						pTmp->overlay = (unsigned char)INVALID_OVERLAY;
						pTmp->road = false;
						pTmp->specialID = 0;
						pTmp->specialType = SPECIAL_NONE;
						pTmp->lineOfSight = 0;		//Now the local height!!!!!!
						pTmp->passable = true;
						
						pTmp->terrain = land->getTerrain( i, j );
						// If physical terrain type not passable, mark as such.
						// WATER IS NOT STORED IN THIS VARIABLE!!!!!!!!!!!!!!
						// Water is deep or not and not deep water is PASSABLE!

						//-------------------------------------------------------
						// NOW, mark cliffs as impassable.  ANY tile whose angle
						// Exceeds CliffTerrainAngle value.
						if ((i==0) || (j==0) || (i >= (land->realVerticesMapSide-1)) || (j >= (land->realVerticesMapSide-1)))
						{
							pTmp->passable = false;
						}
						else
						{
							//--------------------------------------------------------------------------------------
							//We need to check cliff angles based on UVMode.  This check is great for the old WAY!!
							bool tlx = (long(land->mapData->topLeftVertex.x) & 1);
							bool tly = (long(land->mapData->topLeftVertex.y) & 1);
						
							long x = i - land->mapData->topLeftVertex.x;
							long y = j - land->mapData->topLeftVertex.y;
						
							bool yby2 = (y & 1) ^ (tly);
							bool xby2 = (x & 1) ^ (tlx);
						
							long uvMode = 0;
							if (yby2)
							{
								if (xby2)
								{
									uvMode = BOTTOMRIGHT;
								}
								else
								{
									uvMode = BOTTOMLEFT;
								}
							}
							else
							{
								if (xby2)
								{
									uvMode = BOTTOMLEFT;
								}
								else
								{
									uvMode = BOTTOMRIGHT;
								}
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
								{
									pTmp->passable = false;
								}
								
								elvDiff1 = fabs(elv0 - elv2);
								elvDiff2 = fabs(elv0 - elv3);
								
								elvAngleFactor = Terrain::worldUnitsPerVertex * sin(CliffTerrainAngle * DEGREES_TO_RADS);
								if ((elvDiff1 > elvAngleFactor) || (elvDiff2 > elvAngleFactor))
								{
									pTmp->passable = false;
								}
							}
							else	//uvMode is BOTTOMLEFT!
							{
								float elv0 = land->getTerrainElevation(i  ,j);
								float elv1 = land->getTerrainElevation(i+1,j);
								float elv2 = land->getTerrainElevation(i+1,j+1);
								float elv3 = land->getTerrainElevation(i  ,j+1);
								
								float elvDiff1 = fabs(elv0 - elv1);
								float elvDiff2 = fabs(elv0 - elv3);
								
								float elvAngleFactor = Terrain::worldUnitsPerVertex * sin(CliffTerrainAngle * DEGREES_TO_RADS);
								if ((elvDiff1 > elvAngleFactor) || (elvDiff2 > elvAngleFactor))
								{
									pTmp->passable = false;
								}
								
								elvDiff1 = fabs(elv1 - elv2);
								elvDiff2 = fabs(elv1 - elv3);
								
								elvAngleFactor = Terrain::worldUnitsPerVertex * sin(CliffTerrainAngle * DEGREES_TO_RADS);
								if ((elvDiff1 > elvAngleFactor) || (elvDiff2 > elvAngleFactor))
								{
									pTmp->passable = false;
								}
							}
						}
						
						
						Overlays overlay;
						unsigned long offset;
						land->getOverlay( i, j, overlay, offset );
						if ( overlay != INVALID_OVERLAY )
						{
							pTmp->overlay = (int)overlay;
							pTmp->road = true;	// This should be more accurate!!!!! Need to fix...
						}

						int cellRow = i * 3 + cellI;
						int cellColumn = j * 3 + cellJ;
						pTmp->mine = (GameMap && GameMap->inBounds(cellRow, cellColumn)) ? GameMap->getMine(cellRow, cellColumn) : 0;

						pTmp++;
					}
				}
			}
		}
		
		//-----------------------------------------------------------------------------------------
		// Now scan through every single object and have them mark impassability!
		// Buildings will mark impassable WHEN PLACED to avoid placing buildings on top of each other.
		// This would mean, however, that the pInfo or whatever we use must exist always not just here.
		// For now, this is fine.  We should definitely change for ship version! -fs
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
					//ONLY Buildings mark impassable.  Thus, if we want trees, forest clumps etc to block, they must be buildings
					if ( ((*it)->getSpecialType() == EditorObjectMgr::EDITOR_GATE))
						gateTotalCount++;
					else if ((*it)->getSpecialType() == EditorObjectMgr::WALL)
						wallTotalCount++;
					else if ((*it)->getSpecialType() == EditorObjectMgr::EDITOR_BRIDGE)
						landBridgeTotalCount++;
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
					//ONLY Buildings mark impassable.  Thus, if we want trees, forest clumps etc to block, they must be buildings
					if ( ((*it)->getSpecialType() == EditorObjectMgr::EDITOR_GATE))
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
					else if (!IsSinglePlayer() && (*it)->getSpecialType() == EditorObjectMgr::RESOURCE_BUILDING)
						(*it)->markTerrain( pInfo, EditorObjectMgr::RESOURCE_BUILDING , 0);
					else
						(*it)->markTerrain(pInfo,SPECIAL_NONE, 0);
				}
				it++;
			}
		}

		for (long i = 0; i < gateCount + wallCount + landBridgeCount; i++) {
			specialAreaFootPrints[i].preNumCells = specialAreaFootPrints[i].numCells;
			specialAreaFootPrints[i].numCells = 0;
		}
		pTmp = pInfo;
		for (long r = 0; r < land->realVerticesMapSide * 3; r++)
			for (long c = 0; c < land->realVerticesMapSide * 3; c++) {
				if (pTmp->specialType != SPECIAL_NONE) {
					if (pTmp->passable) {
						specialAreaFootPrints[pTmp->specialID].cells[specialAreaFootPrints[pTmp->specialID].numCells][0] = r;
						specialAreaFootPrints[pTmp->specialID].cells[specialAreaFootPrints[pTmp->specialID].numCells][1] = c;
						specialAreaFootPrints[pTmp->specialID].numCells++;
					}
				}
				pTmp++;		
			}

		if (saveMoveDataThisPass)
		{
			EditorDataTrace("EditorData::save: before MOVE_buildData rows=%ld cols=%ld specials=%ld",
				land->realVerticesMapSide * 3, land->realVerticesMapSide * 3, gateCount + wallCount + landBridgeCount);
			MOVE_buildData(land->realVerticesMapSide * 3, land->realVerticesMapSide * 3, pInfo, gateCount + wallCount + landBridgeCount, specialAreaFootPrints);
			EditorDataTrace("EditorData::save: after MOVE_buildData");
		}
		else
		{
			EditorDataTrace("EditorData::save: skipped MOVE_buildData because move data is not ready for full save");
		}
		free( pInfo );
		pInfo = NULL;
	}
 
	// create a pak file with the correct number of entries
	PacketFile file;
	if ( NO_ERR != file.create( path ) )
	{
		char buffer[512];
		EditorSafeLoadString( IDS_INVALID_FILE, buffer, 256, gameResourceHandle );
		char buffer2[512];
		sprintf( buffer2, buffer, path );
		//MessageBox( NULL, buffer2, NULL, MB_OK );
		/*I think MessageBox() would not be modal wrt the application.*/
		AfxMessageBox(buffer2);
		EditorInterface::instance()->UnsetBusyMode();
		return false;
	}

	//------------------------------------------------------------------------
	// This reserve MUST come after we've initialized and built the move data.
	DWORD movePacketCount = 0;
	if (saveMoveDataThisPass)
	{
		EditorDataTrace("EditorData::save: before MOVE_saveData(NULL)");
		movePacketCount = MOVE_saveData(NULL);
		EditorDataTrace("EditorData::save: MOVE_saveData(NULL) count=%lu", (unsigned long)movePacketCount);
	}
	else
	{
		EditorDataTrace("EditorData::save: skipped MOVE_saveData(NULL); reserving empty packet 4 and GUID packet");
	}
	DWORD numPackets = saveMoveDataThisPass ? (5 + movePacketCount) : 6;
	EditorDataTrace("EditorData::save: numPackets=%lu", (unsigned long)numPackets);
	file.reserve(numPackets, false);
	land->unselectAll();
	bool bRetVal = land->save( &file, 0, (0.0 < eye->day2NightTransitionTime) ) ? true : false;
	bRetVal = EditorObjectMgr::instance()->save( file, 1 ) && bRetVal;

	if (!quickSave)
	{
		EditorDataTrace("EditorData::save: before saveTacMap");
		saveTacMap( &file, 3 );
		EditorDataTrace("EditorData::save: after saveTacMap");
		if (saveMoveDataThisPass)
		{
			EditorDataTrace("EditorData::save: before MOVE_saveData(file)");
			MOVE_saveData(&file, 4);
			EditorDataTrace("EditorData::save: after MOVE_saveData(file)");
		}
		else
		{
			EditorDataTrace("EditorData::save: skipped MOVE_saveData(file); packet 4 remains empty");
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

	file.writePacket(numPackets-1,MemoryPtr(&id),sizeof(GUID));

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
		AfxMessageBox(buffer2);
		EditorInterface::instance()->UnsetBusyMode();
		return false;
	}
	gosASSERT( result == NO_ERR );

	land->saveColorMapName(&fitFile);

	bRetVal = saveMissionFitFileStuff(fitFile) && bRetVal;

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

	// ARM
	mapAssetPtr->Close();
	mapAssetPtr = NULL;
	mapAsset = NULL;

	if (initializedCOM)
	{
		CoUninitialize();
	}

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
	eye->save( &fitFile );
	bRetVal = land->save( &fitFile ) && bRetVal;

	EditorObjectMgr::instance()->saveMechs( fitFile );
	EditorObjectMgr::instance()->saveDropZones( fitFile );
	EditorObjectMgr::instance()->saveForests( fitFile );

	missionSettings.save( &fitFile );

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
	
	saveObjectives( &fitFile );
	PlayersRef().Save( &fitFile );

	fitFile.writeBlock( "Music" );
	fitFile.writeIdUChar( "scenarioTuneNum", 0 );
	fitFile.writeBlock( "Script" );
	fitFile.writeIdString( "ScenarioScript", missionScriptName );

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
			AfxMessageBox(IDS_OBJECTIVES_WITH_NO_CONDITIONS);
		}
	}
	return true;
}

bool EditorData::saveHeightMap( File* file )
{
	long row, column;
	float highest = land->getHighestVertex( row, column );
	float lowest = land->getLowestVertex( row, column );

	TGAFileHeader header;
	memset( &header, 0, sizeof( header ) );

	header.height = header.width = land->realVerticesMapSide;

	header.x_origin = 0;
	header.y_origin = 0;
	header.image_type = UNC_GRAY;
	header.pixel_depth = 8;
	header.image_descriptor = 0;

	file->write( (BYTE*)&header, sizeof( header ) );

	// now write image, upside down
	for ( int j = land->realVerticesMapSide - 1; j >= 0; j-- )
	{
		for ( int i = 0; i < land->realVerticesMapSide; i++ )
		{
			float height = land->getTerrainElevation( j, i );

			// turn this into 256 scale
			float difference = height - lowest;
			float ratio = (difference * 256)/(highest - lowest);
			BYTE final = ratio + .5 > 255. ? 255 : (int)(ratio + .5);

			// I could buffer this up if I need to make it faster.
			file->write( &final, sizeof( BYTE ) );
		}
	}

	return true;


}


struct TGARecs
{
	char fileName[64];
	MemoryPtr tgaData;
	long height;
	long width;
};

#define CONTRAST		128.0f
#define BRIGHTNESS		0.5f
#define LIGHT_YAW		-250.0f
#define LIGHT_PITCH		32.0f

//---------------------------------------------------------------------------
inline bool isCementType (DWORD type)
{
	bool isCement = ((type == BASE_CEMENT_TYPE) ||
					((type >= START_CEMENT_TYPE) && (type <= END_CEMENT_TYPE)));
	return isCement;
}

//---------------------------------------------------------------------------
void EditorData::drawTacMap( BYTE* pDest, long dataSize, int tacMapSize )
{
	EditorInterface::instance()->SetBusyMode();

	long ramSize = ((land->realVerticesMapSide) * MAPCELL_DIM) * ((land->realVerticesMapSide) * MAPCELL_DIM);

	DWORD* pShrunken = (DWORD*)pDest;
	
	memset( pShrunken, 0, dataSize );

	gosASSERT(tacMapBmp != NULL);
	memset(tacMapBmp,0,ramSize*4);

	Stuff::Vector3D optimalLight;
	optimalLight.x = cos(LIGHT_YAW * DEGREES_TO_RADS);
	optimalLight.y = sin(LIGHT_YAW * DEGREES_TO_RADS);
	optimalLight.z = sin(LIGHT_PITCH * DEGREES_TO_RADS);

	//------------------------------------------------------
	// First Pass - Find Max and Min Elevation.
	if (!land->mapData) { return; }
	PostcompVertexPtr currentVertex = land->mapData->getData();

	long i = 0;
	long y = 0;

	float maxElv=0, minElv=20000000;
	for (i=0;i<(land->realVerticesMapSide * land->realVerticesMapSide);i++)
	{
		if (currentVertex->elevation > maxElv)
			maxElv = currentVertex->elevation;

		if (currentVertex->elevation < minElv)
			minElv = currentVertex->elevation;

		currentVertex++;
	}

	currentVertex = land->mapData->getData();
	float elevationRange = maxElv - minElv;
	elevationRange /= 128.0f;			//Used to scale color based on elevation.

	//------------------------------------------------------
	// Second Pass - Draw Base Terrain Colors and elevations
	// This were the old way.
	//
	// Now we grab the colormap and shrink it on down!!
	// IF NO colormap, try the burnin?
	if (land->terrainTextures2)
	{
		land->terrainTextures2->getScaledColorMap((MemoryPtr)tacMapBmp,((land->realVerticesMapSide) * MAPCELL_DIM));
	}

	for (y=0;y<(land->realVerticesMapSide) * MAPCELL_DIM;y++)
	{
		for (long x=0;x<(land->realVerticesMapSide) * MAPCELL_DIM;x++)
		{
			//-----------------------------------------------
			// Get the data needed to make this terrain quad
			PostcompVertex *pVertex1 = currentVertex;
				
			//-------------------------------------------------------------------------------
			// Store texture in bottom part from TxmIndex provided by TerrainTextureManager
			DWORD terrainType1RGB = 0xffffffff;

			if (pVertex1->elevation < (Terrain::waterElevation - MapData::shallowDepth))
				terrainType1RGB = land->terrainTextures->getTextureTypeRGB(0); 
			else if (pVertex1->elevation < Terrain::waterElevation)
				terrainType1RGB = 0x002e7599; 		//Derek's magic number for shallow water.
			else if (isCementType(pVertex1->terrainType))
				terrainType1RGB = land->terrainTextures->getTextureTypeRGB(pVertex1->terrainType);

			float lightIntensity1 = 1.0f;

			//----------------------------------------------
			// Get Pointer to BMP data for this point.
			unsigned long *tBMP = &(tacMapBmp[(x) + ((y) * land->realVerticesMapSide * 3)]);

			if (terrainType1RGB != 0xffffffff)
				*tBMP = terrainType1RGB;

			tBMP++;

			if (x && !(x % 3))
			{
				currentVertex++;
			}
		}
		
		//Reset and read the same vertices until we hit the next set of cells!!
		currentVertex = land->mapData->getData(); 
		currentVertex += land->realVerticesMapSide * (y / 3);
	}

	unsigned long *tBMP = tacMapBmp;
	// now draw on the trees
	for( long i = 0; i < land->realVerticesMapSide * MAPCELL_DIM; i++ )
	{
		for ( int j = 0; j < land->realVerticesMapSide * MAPCELL_DIM; j++ )
		{
			EditorObject* pObj = EditorObjectMgr::instance()->getObjectAtCell( j, i );
			if ( pObj && EditorObjectMgr::instance()->getAppearanceType( pObj->getID() ) == TREED_TYPE )
			{
				DWORD green = ((*tBMP & 0x0000ff00) >> 8);
				green += 10;
				if (green > 255)
					green = 255;
				
				*tBMP &= 0xffff00ff;
				*tBMP += (green << 8);
			}

			tBMP++;
		}
	}

	currentVertex = land->mapData->getData();
	// now draw on the Roads
	for (long y=0;y<(land->realVerticesMapSide-1);y++)
	{
		for (long x=0;x<(land->realVerticesMapSide-1);x++)
		{
			//-----------------------------------------------
			// Get the data needed to make this terrain quad
			PostcompVertex *pVertex1 = currentVertex;
				
			//-------------------------------------------------------------------------------
			// Store texture in bottom part from TxmIndex provided by TerrainTextureManager
			Overlays o = (Overlays)-1;
			unsigned long offset = 0;
			
			land->getOverlay(y,x,o,offset);
			
			DWORD terrainTypeRGB = 0xffffffff;
			if (o != (Overlays)-1)
				terrainTypeRGB = land->terrainTextures->getOverlayTypeRGB(o);

			if (terrainTypeRGB == 0xffffffff)
				continue;

			PostcompVertex *pVertex2 = currentVertex + 1;
			PostcompVertex *pVertex3 = currentVertex + land->realVerticesMapSide + 1;
			PostcompVertex *pVertex4 = currentVertex + land->realVerticesMapSide;
			
 			float lightIntensity1 = pVertex1->vertexNormal * optimalLight;
			float lightIntensity2 = pVertex2->vertexNormal * optimalLight;
			float lightIntensity3 = pVertex3->vertexNormal * optimalLight;
			float lightIntensity4 = pVertex4->vertexNormal * optimalLight;

			//----------------------------------------------
			// Get Pointer to BMP data for this point.
			unsigned long *tBMP = &(tacMapBmp[(x * 3) + ((y * 3) * land->realVerticesMapSide * 3)]); 
			float totalR1, totalG1, totalB1, totalR2, totalG2, totalB2, totalR3, totalG3, totalB3, totalR4, totalG4, totalB4;

			long elvOffset1 = (lightIntensity1 - 0.5f) * CONTRAST;
			totalR1 = ((terrainTypeRGB>>16) & 0x000000ff);
			totalG1 = ((terrainTypeRGB>>8) & 0x000000ff);
			totalB1 = (terrainTypeRGB & 0x000000ff);

			totalR1 += elvOffset1;
			if (totalR1 > 255.0f)
				totalR1 = 255.0f;

			totalG1 += elvOffset1;
			if (totalG1 > 255.0f)
				totalG1 = 255.0f;

			totalB1 += elvOffset1;
			if (totalB1 > 255.0f)
				totalB1 = 255.0f;

			if (totalR1 < 0.0f)
				totalR1 = 0.0f;
			if (totalG1 < 0.0f)
				totalG1 = 0.0f;
			if (totalB1 < 0.0f)
				totalB1 = 0.0f;

			DWORD color1 = (((DWORD)totalR1) << 16) + (((DWORD)totalG1) << 8) + ((DWORD)totalB1); 

			long elvOffset2 = (lightIntensity2 - 0.5f) * CONTRAST;
			totalR2 = ((terrainTypeRGB>>16) & 0x000000ff);
			totalG2 = ((terrainTypeRGB>>8) & 0x000000ff);
			totalB2 = (terrainTypeRGB & 0x000000ff);

			totalR2 += elvOffset2;
			totalG2 += elvOffset2;
			totalB2 += elvOffset2;

			if (totalR2 > 255.0f)
				totalR2 = 255.0f;
			if (totalG2 > 255.0f)
				totalG2 = 255.0f;
			if (totalB2 > 255.0f)
				totalB2 = 255.0f;

			if (totalR2 < 0.0f)
				totalR2 = 0.0f;
			if (totalG2 < 0.0f)
				totalG2 = 0.0f;
			if (totalB2 < 0.0f)
				totalB2 = 0.0f;

			DWORD color2 = (((DWORD)totalR2) << 16) + (((DWORD)totalG2) << 8) + ((DWORD)totalB2); 

			long elvOffset4 = (lightIntensity4 - 0.5f) * CONTRAST;
			totalR4 = ((terrainTypeRGB>>16) & 0x000000ff);
			totalG4 = ((terrainTypeRGB>>8) & 0x000000ff);
			totalB4 = (terrainTypeRGB & 0x000000ff);

			totalR4 += elvOffset4;
			totalG4 += elvOffset4;
			totalB4 += elvOffset4;

			if (totalR4 > 255.0f)
				totalR4 = 255.0f;
			if (totalG4 > 255.0f)
				totalG4 = 255.0f;
			if (totalB4 > 255.0f)
				totalB4 = 255.0f;

			if (totalR4 < 0.0f)
				totalR4 = 0.0f;
			if (totalG4 < 0.0f)
				totalG4 = 0.0f;
			if (totalB4 < 0.0f)
				totalB4 = 0.0f;

			DWORD color4 = (((DWORD)totalR4) << 16) + (((DWORD)totalG4) << 8) + ((DWORD)totalB4); 

			long elvOffset3 = (lightIntensity3 - 0.5f) * CONTRAST;
			totalR3 = ((terrainTypeRGB>>16) & 0x000000ff);
			totalG3 = ((terrainTypeRGB>>8) & 0x000000ff);
			totalB3 = (terrainTypeRGB & 0x000000ff);

			totalR3 += elvOffset3;
			totalG3 += elvOffset3;
			totalB3 += elvOffset3;

			if (totalR3 > 255.0f)
				totalR3 = 255.0f;
			if (totalG3 > 255.0f)
				totalG3 = 255.0f;
			if (totalB3 > 255.0f)
				totalB3 = 255.0f;

			if (totalR3 < 0.0f)
				totalR3 = 0.0f;
			if (totalG3 < 0.0f)
				totalG3 = 0.0f;
			if (totalB3 < 0.0f)
				totalB3 = 0.0f;

			DWORD color3 = (((DWORD)totalR3) << 16) + (((DWORD)totalG3) << 8) + ((DWORD)totalB3); 

			*tBMP = color1;
			tBMP++;

			*tBMP = color1;
			tBMP++;

			*tBMP = color2;
			tBMP += (land->realVerticesMapSide*3)-2;

			*tBMP = color1;
			tBMP++;

			*tBMP = color1;
			tBMP++;

			*tBMP = color2;
			tBMP += (land->realVerticesMapSide*3)-2;

			*tBMP = color4;
			tBMP++;

			*tBMP = color4;
			tBMP++;

			*tBMP = color3;
			currentVertex++;
		}
			
		currentVertex++;
	}
	 
	tBMP = tacMapBmp;
	// now draw on the Buildings and forests
	TGARecs *tRecs = (TGARecs *)systemHeap->Malloc(sizeof(TGARecs) * 255);
	memset(tRecs,0,sizeof(TGARecs) * 255);
	
	for( i = 0; i < land->realVerticesMapSide * MAPCELL_DIM; i++ )
	{
		for ( int j = 0; j < land->realVerticesMapSide * MAPCELL_DIM; j++ )
		{
			EditorObject* pObj = EditorObjectMgr::instance()->getObjectAtCell( i, j );
			if ( pObj && EditorObjectMgr::instance()->getTacMapColor(pObj->getID()) )
			{
				DWORD tgaColor = EditorObjectMgr::instance()->getTacMapColor(pObj->getID());
				float tBlue = tgaColor & 0x000000ff;
				float tGreen = (tgaColor >> 8) & 0x000000ff;
				float tRed = (tgaColor >> 16) & 0x000000ff;
				float alpha = (tgaColor >> 24) & 0x000000ff;
				float alphaFactor = alpha / 255.0;
									
				DWORD bmpColor = *tBMP;
				float bBlue = bmpColor & 0x000000ff;
				float bGreen = (bmpColor >> 8) & 0x000000ff;
				float bRed = (bmpColor >> 16) & 0x000000ff;
									
				float nBlue = (tBlue * alphaFactor) + (bBlue * (1.0f - alphaFactor));
				float nGreen = (tGreen * alphaFactor) + (bGreen * (1.0f - alphaFactor));
				float nRed = (tRed * alphaFactor) + (bRed * (1.0f - alphaFactor));
				
				if (nBlue > 255.0f)
					nBlue = 255.0f;
					
				if (nGreen > 255.0f)
					nGreen = 255.0f;
					
				if (nRed > 255.0f)
					nRed = 255.0f;
				
				BYTE nb = nBlue,ng = nGreen,nr = nRed;
				DWORD nColor = (nr << 16) + (ng << 8) + (nb);

				if (i && j && 
					(i < ((land->realVerticesMapSide * MAPCELL_DIM) - 1)) &&
					(j < ((land->realVerticesMapSide * MAPCELL_DIM) - 1)))
				{
					unsigned long *tmptBMP = tBMP + (land->realVerticesMapSide * MAPCELL_DIM);
					tmptBMP -= 1;
					
					*tmptBMP = nColor;
					tmptBMP++;
		
					*tmptBMP = nColor; 
					tmptBMP++;
		
					*tmptBMP = nColor; 
					tmptBMP += (land->realVerticesMapSide*3)-2;
		
					*tmptBMP = nColor; 
					tmptBMP++;
		
					*tmptBMP = nColor; 
					tmptBMP++;
		
					*tmptBMP = nColor; 
					tmptBMP += (land->realVerticesMapSide*3)-2;
		
					*tmptBMP = nColor; 
					tmptBMP++;
		
					*tmptBMP = nColor; 
					tmptBMP++;
		
					*tmptBMP = nColor; 
				}
			}

			if ( pObj && 
				strcmp(EditorObjectMgr::instance()->getTGAFileName(pObj->getID()),"NONE") != 0 )
			{
				long recNum = -1;
				MemoryPtr tgaRAM = NULL;
				long tHeight = 0, tWidth = 0;
				for (long it = 0;it<255;it++)
				{
					if (tRecs[it].tgaData && strcmp(EditorObjectMgr::instance()->getTGAFileName(pObj->getID()),tRecs[it].fileName) == 0)
					{
						recNum = it;
						break;
					}
				}
				
				if (recNum != -1)
				{
					tgaRAM = tRecs[recNum].tgaData;
					tWidth = tRecs[recNum].width;
					tHeight = tRecs[recNum].height;
				}
				else
				{
					File tgaFile;
					long result = ~NO_ERR;
					if (0 != strcmp("0", EditorObjectMgr::instance()->getTGAFileName(pObj->getID())))
					{
						//Draw the TGA at this location into the tBMP
						FullPathFileName tgaName;
						tgaName.init(artPath,EditorObjectMgr::instance()->getTGAFileName(pObj->getID()),".tga");
						
						result = tgaFile.open(tgaName);
					}
					if (result == NO_ERR)
					{
						TGAFileHeader header;
						tgaFile.read((MemoryPtr)&header,sizeof(TGAFileHeader));
						tgaFile.seek(0);
						
						tgaRAM = (MemoryPtr)systemHeap->Malloc(header.width * header.height * sizeof(DWORD));
						
						loadTGATexture (&tgaFile, tgaRAM, header.width, header.height);
						
						for (long it = 0;it<255;it++)
						{
							if (!(tRecs[it].tgaData))
							{
								tRecs[it].tgaData = tgaRAM;
								strcpy(tRecs[it].fileName,EditorObjectMgr::instance()->getTGAFileName(pObj->getID()));
								tRecs[it].height = tHeight = header.height;
								tRecs[it].width = tWidth = header.width;
								break;
							}
						}
					}
				}
   				
				if (tgaRAM)
				{
					unsigned long *tmptBMP = tBMP;
					unsigned long *tgaBMP = (unsigned long *)tgaRAM;
					for (long ib=0;ib<tHeight;ib++)
					{
						if (((i - (tHeight >> 1)) >= 0) &&
							(((tHeight >> 1) + i) < (land->realVerticesMapSide * MAPCELL_DIM)))
						{
							if (((j - (tWidth >> 1)) >= 0) &&
								(((tWidth >> 1) + j) < (land->realVerticesMapSide * MAPCELL_DIM)))
							{
								tmptBMP = tBMP + ((land->realVerticesMapSide * MAPCELL_DIM) * (ib - (tHeight >> 1)));
								tmptBMP -= (tWidth >> 1);
								
								for (long jb=0;jb<tWidth;jb++)
								{
									DWORD tgaColor = *tgaBMP;
									float tBlue = tgaColor & 0x000000ff;
									float tGreen = (tgaColor >> 8) & 0x000000ff;
									float tRed = (tgaColor >> 16) & 0x000000ff;
									float alpha = (tgaColor >> 24) & 0x000000ff;
									float alphaFactor = alpha / 255.0;
									
									DWORD bmpColor = *tmptBMP;
									float bBlue = bmpColor & 0x000000ff;
									float bGreen = (bmpColor >> 8) & 0x000000ff;
									float bRed = (bmpColor >> 16) & 0x000000ff;
									
									float nBlue = (tBlue * alphaFactor) + (bBlue * (1.0f - alphaFactor));
									float nGreen = (tGreen * alphaFactor) + (bGreen * (1.0f - alphaFactor));
									float nRed = (tRed * alphaFactor) + (bRed * (1.0f - alphaFactor));
									
									if (nBlue > 255.0f)
										nBlue = 255.0f;
										
									if (nGreen > 255.0f)
										nGreen = 255.0f;
										
									if (nRed > 255.0f)
										nRed = 255.0f;
									
									BYTE nb = nBlue,ng = nGreen,nr = nRed;
									DWORD nColor = (nr << 16) + (ng << 8) + (nb);
									
									*tmptBMP = nColor;
									tmptBMP++;
									tgaBMP++;
								}
							}
						}
					}
				}
			}
			
			tBMP++;
		}
	}
	
	for (long it = 0;it<255;it++)
	{
		if (tRecs[it].tgaData)
		{
			systemHeap->Free(tRecs[it].tgaData);
			tRecs[it].tgaData = NULL;	
		}
	}
	
	systemHeap->Free(tRecs);
	tRecs = NULL;
	
 	// now shrink 'er down
	double xRatio = 3. * (land->realVerticesMapSide)/tacMapSize;
	double yRatio = 3. * (land->realVerticesMapSide)/tacMapSize;

	DWORD *pTmp = pShrunken;
	for ( i = 0; i < tacMapSize; i++ )
	{
		y = ((float)i * yRatio) + .5;

		int j = 0;
		for ( ; j < tacMapSize; j++ )
		{
			long x = ((float)j * xRatio) + .5;
			unsigned long *tBMP = &(tacMapBmp[(x) + ((y) * land->realVerticesMapSide * 3)]); 
			*pTmp++ = *tBMP;			
		}

		for ( ; j < 128; ++j ) // leave the edges blank.
		{
			pTmp++;
		}
	}

	EditorInterface::instance()->UnsetBusyMode();
}

void EditorData::makeTacMap(BYTE*& pReturn, long& dataSize, int tacMapSize )
{
	// the bitmap is always 128 * 128, the size of the tac map can be anything,
	// the edges will be filled in with transparency
	
	//-----------------------------------------------------
	// Create a block of RAM for Bitmap.
	// 24 Bits of color at Cell resolution.
	long outputSize = 128 * 128 * 4;

	BYTE* pOutput = (BYTE*)malloc( outputSize + sizeof( TGAFileHeader ) );
	pReturn = pOutput;
	BYTE* pShrunken = pOutput + sizeof( TGAFileHeader );
	
	dataSize = outputSize + sizeof( TGAFileHeader );

	memset( pShrunken, 0, outputSize );
	
	drawTacMap( pShrunken, outputSize, tacMapSize );

	TGAFileHeader* header = (TGAFileHeader*)pOutput;
	memset( header, 0, sizeof( TGAFileHeader ) );

	header->height = header->width = 128;

	header->x_origin = 0;
	header->y_origin = 0;
	header->image_type = UNC_TRUE;
	header->pixel_depth = 32;
	header->image_descriptor = 32;

}

//---------------------------------------------------------------------------
void EditorData::loadTacMap(PacketFile *file, BYTE*& pReturn, long dataSize, int tacMapSize )
{
	// the bitmap is always 128 * 128, the size of the tac map can be anything,
	// the edges will be filled in with transparency
	
	//-----------------------------------------------------
	// Create a block of RAM for Bitmap.
	// 24 Bits of color at Cell resolution.
	long outputSize = 128 * 128 * 4;

	BYTE* pOutput = (BYTE*)new BYTE[ outputSize + sizeof( TGAFileHeader )];
	pReturn = pOutput;
	BYTE* pShrunken = pOutput + sizeof( TGAFileHeader );
	
	dataSize = outputSize + sizeof( TGAFileHeader );

	memset( pShrunken, 0, outputSize );

	if ((file->seekPacket(3) == NO_ERR) && (file->getPacketSize() != 0))
	{
		file->readPacket(3,pOutput);
	}
	else
	{
		makeTacMap(pReturn,dataSize,tacMapSize);
	}
}

//---------------------------------------------------------------------------
bool EditorData::saveTacMap (PacketFile* file, long whichPacket )
{
	BYTE* data = NULL;
	long size = 0;

	makeTacMap( data, size, 128 );
	file->writePacket( whichPacket, data, size);

	File test;
	test.open( "tgatest.tga", CREATE );
	test.write( data, size);
	test.close();
	
	delete []  data;

	return true;
}

bool CTeam::operator==(const CTeam &rhs) const {
	bool retval = false;
	if ((m_alignment == rhs.m_alignment) && (m_objectives == rhs.m_objectives)) {
		retval = true;
	}
	return retval;
}

CTeams::CTeams() {
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		m_teamArray[i].Alignment(i);
	}
}

void CTeams::Clear() {
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		m_teamArray[i].Clear();
	}
}

CTeams &CTeams::operator=(const CTeams &master) {
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		m_teamArray[i] = master.m_teamArray[i];
	}
	return (*this);
}

bool CTeams::operator==(const CTeams &rhs) const {
	bool retval = true;
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		if (!(rhs.m_teamArray[i] == m_teamArray[i])) {
			retval = false;
			break;
		}
	}
	return retval;
}

bool CTeams::Read( FitIniFile* missionFile ) {
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		m_teamArray[i].Read(missionFile);
	}
	return true;
}

bool CTeams::Save( FitIniFile* missionFile ) {
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		m_teamArray[i].Save(missionFile);
	}
	return true;
}

CTeam &CTeams::TeamRef(int i) {
	gosASSERT(GAME_MAX_PLAYERS > i); gosASSERT(0 <= i);
	return m_teamArray[i];
}

void CTeams::handleObjectInvalidation(const EditorObject *pObj) {
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		m_teamArray[i].handleObjectInvalidation(pObj);
	}
}

bool CTeams::NoteThePositionsOfObjectsReferenced() {
	bool retval = true;
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		bool res = m_teamArray[i].NoteThePositionsOfObjectsReferenced();
		if (false == res) {
			retval = false;
		}
	}
	return retval;
}

bool CTeams::RestoreObjectPointerReferencesFromNotedPositions() {
	bool retval = true;
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		bool res = m_teamArray[i].RestoreObjectPointerReferencesFromNotedPositions();
		if (false == res) {
			retval = false;
		}
	}
	return retval;
}

bool CTeams::ThereAreObjectivesWithNoConditions() {
	bool retval = false;
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		bool res = m_teamArray[i].ThereAreObjectivesWithNoConditions();
		if (true == res) {
			retval = true;
			break;
		}
	}
	return retval;
}

void CPlayer::DefaultTeam(int team) {
	gosASSERT((GAME_MAX_PLAYERS > team) && (0 <= team));
	m_defaultTeam = team%GAME_MAX_PLAYERS;
}

bool CPlayer::Read( FitIniFile* missionFile, int playerNum )
{
	long result = 0;
	EString tmpStr;
	tmpStr.Format("Player%d", playerNum);
	result = missionFile->seekBlock(tmpStr.Data());
	result = sReadIdWholeNum(missionFile, "DefaultTeam", m_defaultTeam);
	return result;
}

bool CPlayer::Save( FitIniFile* missionFile, int playerNum )
{
	EString tmpStr;
	tmpStr.Format("Player%d", playerNum);
	missionFile->writeBlock( tmpStr.Data() );
	missionFile->writeIdULong( "DefaultTeam", DefaultTeam() );
	return true;
}

void CPlayers::Clear() {
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		m_playerArray[i].Clear();
	}
	/*set player2's default to team2*/
	m_playerArray[1].DefaultTeam(1);
}

bool CPlayers::Read( FitIniFile* missionFile ) {
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		m_playerArray[i].Read(missionFile, i);
	}
	return true;
}

bool CPlayers::Save( FitIniFile* missionFile ) {
	for (int i = 0; i < GAME_MAX_PLAYERS; i++) {
		m_playerArray[i].Save(missionFile, i);
	}
	return true;
}

#pragma warning( default:4244 )
