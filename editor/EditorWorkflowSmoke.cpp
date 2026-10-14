// Headless authoring probes keep stock round trips free of intentional edits.
#include "EditorInterface_internal.h"
#include "EditorMFC.h"
#include "CampaignSummaryPanel.h"
#include "ObjectivesSummaryPanel.h"
#include "UnitBrainPanel.h"
#include "InspectorPanel.h"
#include "AssetBrowser.h"
#include "campaignData.h"
#include <memory>

void EditorInterface::runWorkflowSmoke(const char* campaignPath)
{
	if (!land || !EditorData::instance || !EditorObjectMgr::instance())
		return;
	EditorSmokePanelFact("mission_definitions", "MissionDefinitions", runDefinitionSmoke());

	const int mid = (land->realVerticesMapSide - 1) / 2;
	const int oldType = land->getTerrain(mid, mid);
	// Zero means refresh existing material, so the probe needs a positive replacement.
	const int newType = oldType == 1 ? 2 : 1;
	setPaintMaterialBrush(newType);
	auto* brush = dynamic_cast<TerrainBrush*>(curBrush);
	bool painted = false;
	bool restored = false;
	if (brush)
	{
		Stuff::Vector3D point;
		point.x = land->mapTopLeft3d.x + mid * land->worldUnitsPerVertex;
		point.y = land->mapTopLeft3d.y - mid * land->worldUnitsPerVertex;
		point.z = land->getTerrainElevation(point);
		brush->setRadius(land->worldUnitsPerVertex * 0.25f);
		brush->beginPaint();
		painted = brush->paint(point, 0, 0) && land->getTerrain(mid, mid) == newType;
		undoMgr.AddAction(brush->endPaint());
		undoMgr.Undo();
		restored = land->getTerrain(mid, mid) == oldType;
	}
	const bool terrainReady = painted && restored && undoMgr.Redo() && land->getTerrain(mid, mid) == newType;

	AssetBrowser::Open();
	const bool activated = AssetBrowser::ActivateFirstObject();
	EditorObjectMgr* mgr = EditorObjectMgr::instance();
	auto* placement = dynamic_cast<BuildingBrush*>(curBrush);
	EditorObject* placed = nullptr;
	Stuff::Vector3D placedPosition;
	bool placementUndo = false;
	if (activated && placement)
	{
		for (int row = mid; row < mid + 12 && !placed; ++row)
			for (int col = mid; col < mid + 12 && !placed; ++col)
			{
				Stuff::Vector3D point;
				land->cellToWorld(row, col, point);
				point.z = land->getTerrainElevation(point);
				if (!placement->canPaint(point, 0, 0, 0))
					continue;
				const auto count = mgr->getBuildings().Count();
				placement->beginPaint();
				const bool paintedObject = placement->paint(point, 0, 0);
				undoMgr.AddAction(placement->endPaint());
				placed = mgr->getObjectAtLocation(point.x, point.y);
				if (!paintedObject || !placed || mgr->getBuildings().Count() != count + 1)
				{
					placed = nullptr;
					continue;
				}
				placedPosition = placed->getPosition();
				placementUndo = undoMgr.Undo() && mgr->getBuildings().Count() == count
					&& !mgr->getObjectAtLocation(placedPosition.x, placedPosition.y);
				const bool redone = undoMgr.Redo();
				placed = redone ? mgr->getObjectAtLocation(placedPosition.x, placedPosition.y) : nullptr;
			}
	}
	const long placedID = placed ? placed->getID() : -1;
	InspectorPanel::Open();
	const int oldTeam = placed ? placed->getAlignment() : -1;
	const int newTeam = oldTeam == 1 ? 2 : 1;
	bool teamReady = placed && applyObjectAlignment(placed, newTeam) && placed->getAlignment() == newTeam;
	teamReady = teamReady && undoMgr.Undo() && placed->getAlignment() == oldTeam
		&& undoMgr.Redo() && placed->getAlignment() == newTeam;

	const auto units = mgr->getUnits();
	Unit* brainUnit = nullptr;
	bool brainReady = false;
	bool beforeAuthored = false;
	for (auto it = units.Begin(); !it.IsDone(); it++)
	{
		if (!*it)
			continue;
		mgr->unselectAll();
		mgr->select(**it, true);
		UnitBrainPanel::Open();
		const auto count = (*it)->getWaypointCount();
		const int order = (*it)->getOrderType();
		const int stance = (*it)->getStance();
		beforeAuthored = (*it)->isOrderAuthored();
		if (UnitBrainPanel::SmokeProbe())
		{
			brainUnit = *it;
			brainReady = undoMgr.Undo() && brainUnit->getWaypointCount() == count
				&& brainUnit->getOrderType() == order && brainUnit->getStance() == stance
				&& brainUnit->isOrderAuthored() == beforeAuthored && undoMgr.Redo()
				&& brainUnit->isOrderAuthored() && brainUnit->getWaypointCount() == count + 1;
			break;
		}
	}
	const long brainID = brainUnit ? brainUnit->getID() : -1;
	const Stuff::Vector3D brainPosition = brainUnit ? brainUnit->getPosition() : Stuff::Vector3D{};
	const auto waypointCount = brainUnit ? brainUnit->getWaypointCount() : 0;
	const int orderType = brainUnit ? brainUnit->getOrderType() : Unit::ORDER_NONE;
	const int stance = brainUnit ? brainUnit->getStance() : -1;
	const bool authoredSaved = EditorData::instance->save("terrain_gen_out/e0_authored.pak", true);
	// Undo removes the authored state before reload, so in-memory remnants cannot pass.
	const bool removedBrain = brainReady && undoMgr.Undo() && brainUnit->getWaypointCount() + 1 == waypointCount
		&& brainUnit->isOrderAuthored() == beforeAuthored;
	const bool removedTeam = teamReady && undoMgr.Undo() && placed->getAlignment() == oldTeam;
	const bool removedObject = placementUndo && undoMgr.Undo()
		&& !mgr->getObjectAtLocation(placedPosition.x, placedPosition.y);
	const bool removedTerrain = terrainReady && undoMgr.Undo() && land->getTerrain(mid, mid) == oldType;
	const bool reloaded = authoredSaved && EditorData::initTerrainFromPCV("terrain_gen_out/e0_authored.pak");
	mgr = EditorObjectMgr::instance();
	EditorObject* loadedObject = reloaded && placedID >= 0 ? mgr->getObjectAtLocation(placedPosition.x, placedPosition.y) : nullptr;
	const bool objectReloaded = loadedObject && loadedObject->getID() == placedID;
	Unit* loadedBrain = reloaded ? dynamic_cast<Unit*>(mgr->getObjectAtLocation(brainPosition.x, brainPosition.y)) : nullptr;
	const bool brainReloaded = loadedBrain && loadedBrain->getID() == brainID && loadedBrain->isOrderAuthored()
		&& loadedBrain->getOrderType() == orderType && loadedBrain->getStance() == stance && loadedBrain->getWaypointCount() == waypointCount
		&& loadedBrain->getWaypoints().back().x == 32.0f && loadedBrain->getWaypoints().back().y == 48.0f;
	EditorSmokePanelFact("terrain_paint", "PaintMaterial", terrainReady && removedTerrain && reloaded && land->getTerrain(mid, mid) == newType);
	EditorSmokePanelFact("object_placement", "AssetBrowser", placementUndo && removedObject && objectReloaded);
	EditorSmokePanelFact("teams_abl", "Inspector", teamReady && removedTeam && objectReloaded && loadedObject->getAlignment() == newTeam);
	EditorSmokePanelFact("teams_abl", "UnitBrain", brainReady && removedBrain && brainReloaded);
	EditorSmokePanelFact("mission_export", "MissionWriter", authoredSaved && reloaded && removedTerrain && removedObject && removedBrain && objectReloaded && brainReloaded);

	ObjectivesSummaryPanel::Open();
	CObjectives& objectives = EditorData::instance->TeamsRef().TeamRef(0).ObjectivesRef();
	CObjectives before(objectives);
	auto objective = std::make_unique<CObjective>(0);
	objective->Title("E0 authored objective");
	objective->ActivateOnFlag(true);
	objective->ActivateFlagID("E0_flag");
	auto condition = std::make_unique<CDestroyAllEnemyUnits>(0);
	objective->Append(condition.get());
	condition.release();
	objectives.Append(objective.get());
	objective.release();
	const unsigned long authoredCount = objectives.Count();
	bool objectiveRoundtrip = false;
	{
		FitIniFile file;
		if (file.create("terrain_gen_out/e0_objectives.fit") == NO_ERR)
		{
			// The collection writer needs the same version envelope as a mission export.
			file.writeBlock("Objectives Version");
			file.writeIdULong("Version", 3);
			const bool saved = objectives.Save(&file);
			file.close();
			CObjectives loaded(0);
			if (file.open("terrain_gen_out/e0_objectives.fit") == NO_ERR)
			{
				objectiveRoundtrip = saved && loaded.Read(&file) && loaded == objectives;
				file.close();
			}
		}
	}
	objectives = before;
	EditorSmokePanelFact("objectives", "ObjectivesSummary", authoredCount == before.Count() + 1 && objectiveRoundtrip && objectives == before, true);

	CampaignSummaryPanel::Open();
	CCampaignData campaign;
	bool linked = false;
	if (campaign.Read(campaignPath) && campaign.m_GroupList.Count() > 0)
	{
		const CCampaignData beforeCampaign(campaign);
		auto group = campaign.m_GroupList.Begin();
		CMissionData mission;
		mission.m_MissionFile = "mc2_24";
		mission.m_PurchaseFile = "Purchase24";
		(*group).m_MissionList.Append(mission);
		CCampaignData loaded;
		linked = campaign.Save("terrain_gen_out/e0_campaign.fit")
			&& loaded.Read("terrain_gen_out/e0_campaign.fit") && loaded == campaign
			&& CampaignSummaryPanel::Load("terrain_gen_out/e0_campaign.fit");
		campaign = beforeCampaign;
		linked = linked && campaign == beforeCampaign;
	}
	EditorSmokePanelFact("campaign_links", "CampaignOverview", linked, true);
}
