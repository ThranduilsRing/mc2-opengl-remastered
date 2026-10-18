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
#include <array>
#include <cstdio>

void EditorInterface::runCatalogReferenceSmoke()
{
	if (!land || !EditorData::instance || !EditorObjectMgr::instance()) return;
	struct Placement { const char* family; long fitId; const char* stem; };
	constexpr std::array placements{
		Placement{"mech", 53, "Anubis"}, Placement{"vehicle", 117, "APC"},
		Placement{"building", 263, "Barrels"}, Placement{"turret", 525, "AutoCannonCamo1"},
		Placement{"tree", 252, "Birch1"}, Placement{"gate", 157, "D_HeavyGate"},
		Placement{"bridge", 607, "bridge"}, Placement{"light", 68, "Spotlight"},
		Placement{"marker", 290, "Nav1"}};
	const char* before = "terrain_gen_out/e5_reference_before.pak";
	const bool baseline = EditorData::instance->save(before, true);
	const int mid = (land->realVerticesMapSide - 1) / 2;
	for (const auto& spec : placements)
	{
		auto* mgr = EditorObjectMgr::instance();
		unsigned long group{}, index{};
		const bool identified = mgr->getBuildingFromID(spec.fitId, group, index, true)
			&& _stricmp(mgr->getFileName(mgr->getID(group, index)), spec.stem) == 0;
		const int expectedTeam = identified && mgr->isAlignable(mgr->getID(group, index)) ? 0 : -1;
		// The fixed team and ordinary brush isolate reference values from UI selection state.
		if (identified) setActiveBrush(new BuildingBrush(group, index, 0), -500, -500);
		auto* brush = identified ? dynamic_cast<BuildingBrush*>(curBrush) : nullptr;
		const auto count = mgr->getBuildings().size() + mgr->getUnits().size();
		EditorObject* placed{};
		glm::vec3 position{};
		bool painted{}, undone{}, redone{}, saved{}, removed{}, reloaded{};
		for (int row = mid; brush && row < mid + 24 && !placed; ++row)
			for (int col = mid; col < mid + 24 && !placed; ++col)
			{
				land->cellToWorld(row, col, position);
				position.z = land->getTerrainElevation(position);
				if (!brush->canPaint(position, 0, 0, 0)) continue;
				brush->beginPaint();
				painted = brush->paint(position, 0, 0);
				undoMgr.AddAction(brush->endPaint());
				placed = painted ? mgr->getObjectAtLocation(position.x, position.y) : nullptr;
			}
		const auto objectId = placed ? placed->getID() : -1;
		if (placed) {
			position = placed->getPosition();
			painted = painted && mgr->getFitID(objectId) == spec.fitId
				&& mgr->getBuildings().size() + mgr->getUnits().size() == count + 1;
			undone = undoMgr.Undo() && !mgr->getObjectAtLocation(position.x, position.y)
				&& mgr->getBuildings().size() + mgr->getUnits().size() == count;
			redone = undoMgr.Redo() && mgr->getObjectAtLocation(position.x, position.y)
				&& mgr->getBuildings().size() + mgr->getUnits().size() == count + 1;
		}
		char path[128];
		snprintf(path, sizeof(path), "terrain_gen_out/e5_reference_%s.pak", spec.family);
		if (painted && undone && redone) saved = EditorData::instance->save(path, true);
		if (saved) removed = undoMgr.Undo() && !mgr->getObjectAtLocation(position.x, position.y);
		if (removed) {
			undoMgr.Reset();
			reloaded = EditorData::initTerrainFromPCV(path);
			mgr = EditorObjectMgr::instance();
			auto* loaded = reloaded ? mgr->getObjectAtLocation(position.x, position.y) : nullptr;
			reloaded = loaded && loaded->getID() == objectId && mgr->getFitID(loaded->getID()) == spec.fitId
				&& loaded->getAlignment() == expectedTeam && mgr->getBuildings().size() + mgr->getUnits().size() == count + 1;
		}
		if (auto* log = fopen("editor-startup.log", "a")) {
			fprintf(log, "[EDITOR_LEGACY_PLACEMENT v1] family=%s fit_id=%ld stem=%s x=%.9g y=%.9g z=%.9g input_team=0 team=%d painted=%d undo=%d redo=%d saved=%d removed=%d reopened=%d\n",
				spec.family, spec.fitId, spec.stem, position.x, position.y, position.z, expectedTeam, painted, undone, redone, saved, removed, reloaded);
			fclose(log);
		}
		undoMgr.Reset();
		if (!baseline || !EditorData::initTerrainFromPCV(before)) break;
	}
}

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
		glm::vec3 point;
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
	glm::vec3 placedPosition;
	bool placementUndo = false;
	if (activated && placement)
	{
		for (int row = mid; row < mid + 12 && !placed; ++row)
			for (int col = mid; col < mid + 12 && !placed; ++col)
			{
				glm::vec3 point;
				land->cellToWorld(row, col, point);
				point.z = land->getTerrainElevation(point);
				if (!placement->canPaint(point, 0, 0, 0))
					continue;
				const auto count = mgr->getBuildings().size();
				placement->beginPaint();
				const bool paintedObject = placement->paint(point, 0, 0);
				undoMgr.AddAction(placement->endPaint());
				placed = mgr->getObjectAtLocation(point.x, point.y);
				if (!paintedObject || !placed || mgr->getBuildings().size() != count + 1)
				{
					placed = nullptr;
					continue;
				}
				placedPosition = placed->getPosition();
				placementUndo = undoMgr.Undo() && mgr->getBuildings().size() == count
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
	for (auto it = units.begin(); it != units.end(); it++)
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
	const glm::vec3 brainPosition = brainUnit ? brainUnit->getPosition() : glm::vec3{};
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
	objective->m_conditions.entries.push_back(condition.get());
	condition.release();
	objectives.entries.push_back(objective.get());
	objective.release();
	const auto authoredCount = objectives.entries.size();
	bool objectiveRoundtrip = false;
	{
		EditorFit::MissionTextReader file;
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
	EditorSmokePanelFact("objectives", "ObjectivesSummary", authoredCount == before.entries.size() + 1 && objectiveRoundtrip && objectives == before, true);

	CampaignSummaryPanel::Open();
	CCampaignData campaign;
	bool linked = false;
	if (campaign.Read(campaignPath) && !campaign.m_GroupList.empty())
	{
		const CCampaignData beforeCampaign(campaign);
		auto group = campaign.m_GroupList.begin();
		CMissionData mission;
		mission.m_MissionFile = "mc2_24";
		mission.m_PurchaseFile = "Purchase24";
		(*group).m_MissionList.push_back(mission);
		CCampaignData loaded;
		linked = campaign.Save("terrain_gen_out/e0_campaign.fit")
			&& loaded.Read("terrain_gen_out/e0_campaign.fit") && loaded == campaign
			&& CampaignSummaryPanel::Load("terrain_gen_out/e0_campaign.fit");
		campaign = beforeCampaign;
		linked = linked && campaign == beforeCampaign;
	}
	EditorSmokePanelFact("campaign_links", "CampaignOverview", linked, true);
}
