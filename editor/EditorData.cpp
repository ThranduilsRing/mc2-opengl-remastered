/***************************************************************
* FILENAME: EditorData.cpp
* DESCRIPTION: Implements Editor terrain, mission data, save, and load behavior.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 05/16/2026
* MODIFICATION: by Methuselas
* CHANGES: Gate EditorData trace logging behind MC2_EDITOR_TRACE.
****************************************************************/

 #define EDITORDATA_CPP

#include "EditorData_internal.h"
#include "Gates/gate.h"


// Patch savefix: MOVE packet readiness is tracked separately from terrain readiness.
// Existing missions set this after packet 4 is successfully read. Newly generated
// blank maps do not safely initialize the MOVE/special-area backend yet; attempting
// MOVE_buildData/MOVE_saveData for them is the known Save/Save As CTD point.
bool gEditorDataMoveDataReadyForFullSave = false;

/*static*/ bool EditorData::IsMoveDataReadyForFullSave() {
    return gEditorDataMoveDataReadyForFullSave;
}

void EditorDataTrace(const char* fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	// Always feed the crash ring buffer so the last ~64KB of editor activity
	// appears in crashes/<timestamp>/last_trace.txt on any CTD.
	crashbundle_append(buf);

	if (!mc2::gate::get<"MC2_EDITOR_TRACE">())
		return;

	FILE* f = fopen("editor-startup.log", "a");
	if (!f)
		return;
	fputs(buf, f);
	fputc('\n', f);
	fclose(f);
}
IProviderAsset * mapAsset = NULL;
IProviderAsset * mechAsset = NULL;

char EditorData::mapName[256];
unsigned long* EditorData::tacMapBmp = NULL;

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
	// EDITOR-EXIT-CRASH-1 (#1): route through the SAME S2 mission-unload chain
	// clear() runs (GpuStaticPropBatcher/GpuMechBatcher onMapUnload, cull
	// substrate/readback shutdown, EditorObjectMgr::clear(), land->destroy()+
	// delete). Historically this destructor only did `delete land`, so any
	// exit path that reached ~EditorData WITHOUT a prior explicit clear() call
	// (no map loaded, or a generator-path exit where beginMissionRenderResources
	// armed the batchers but the happy-path clear() never re-ran) left GPU
	// batcher state (SSBOs/textures/VAOs) unfreed while the GL context was
	// still alive to free it, and any lingering batcher pointer into a
	// just-freed actor TG_MultiShape became a UAF at context teardown.
	// clear() is idempotent by construction (every step below is null/instance
	// guarded), so calling it again here when Editor::destroy() already called
	// it is a cheap no-op, not a double-free. Must run BEFORE `instance = NULL`
	// below: clear() dereferences EditorData::instance throughout.
	EditorDataTrace("[EDITOR_SHUTDOWN] phase=~EditorData_enter");
	clear();

	instance = NULL;
	EditorDataTrace("[EDITOR_SHUTDOWN] phase=~EditorData_exit");
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

	EditorDataTrace("[EDITOR_SHUTDOWN] phase=clear_enter land=%p", land);

	if ( land )
	{
		land->destroy();
		delete land;
		land = NULL;
	}
	EditorDataTrace("[EDITOR_SHUTDOWN] phase=land_destroyed");

	// S2 mission-unload chain — mirrors code/mission.cpp:3272-3283.
	// Canonical order locked by docs/superpowers/plans/2026-05-25-editor-rebuild-S0-contract.md.
	// Must run BEFORE EditorObjectMgr::clear() so batcher state tears down
	// while actor TG_MultiShape pointers are still valid.
	EditorBridge::endMissionRenderResources();                 // steps 1-6 — game lines 3272-3283
	EditorDataTrace("[EDITOR_SHUTDOWN] phase=gpu_batchers_unloaded");
	GameAdapters::StaticProp::endMission();
	GameAdapters::Mech::endMission();
	EditorDataTrace("[EDITOR_SHUTDOWN] phase=render_adapters_ended");

	if ( EditorObjectMgr::instance() )
		EditorObjectMgr::instance()->clear();
	EditorDataTrace("[EDITOR_SHUTDOWN] phase=object_mgr_cleared");

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

	EditorDataTrace("[EDITOR_SHUTDOWN] phase=clear_exit");

	return true;
}

//-------------------------------------------------------------------------------------------------
bool bIsLoading = false;

#pragma warning( default:4244 )
