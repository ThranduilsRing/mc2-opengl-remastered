// Headless authoring probes keep stock round trips free of intentional edits.
#include "EditorInterface_internal.h"
#include "EditorMFC.h"
#include "CampaignSummaryPanel.h"
#include "ObjectivesSummaryPanel.h"
#include "UnitBrainPanel.h"
#include "EditorCommands.h"
#include "InspectorPanel.h"
#include "AssetBrowser.h"
#include "MapGeneratorDialog.h"
#include "campaignData.h"
#include <memory>
#include <array>
#include <cstdio>
#include "../GameOS/gameos/static_caster_set.h"
#include "../GameOS/gameos/gos_postprocess.h"

void EditorMoverPopulationSmokeTick(long frame) {
	static bool fired=false;
	if (fired || frame<120 || !land || !EditorObjectMgr::instance()) return;
	fired=true;
	auto* mgr=EditorObjectMgr::instance();
	Unit* prototype=nullptr;
	for (auto* unit:mgr->getUnits())
		if (((unit->getID()>>24)&0xff)==MECH_TYPE && unit->appearance()) { prototype=unit; break; }
	int count=0;
	if (prototype) {
		const int group=prototype->getGroup(), index=prototype->getIndexInGroup();
		const glm::vec3 anchor=prototype->getPosition();
		const float span=(land->realVerticesMapSide-1)*land->worldUnitsPerVertex;
		const float west=land->mapTopLeft3d.x, north=land->mapTopLeft3d.y;
		const float water=getGosPostProcess()?getGosPostProcess()->getWaterElevation():-1e30f;
		mgr->unselectAll();
        // Find a dry, compact ten-by-ten spread, rather than a long row
        // that framing cannot fit at an ordinary editor zoom.
        std::vector<glm::vec3> positions;
        for (int row=-20;row<10 && positions.size()!=100;row+=5)
            for (int col=-20;col<10 && positions.size()!=100;col+=5) {
                positions.clear();
                for (int y=0;y<10;++y) for (int x=0;x<10;++x) {
                    glm::vec3 p{anchor.x+(col+x)*96.0f,anchor.y+(row+y)*96.0f,0};
                    if (p.x<west+256 || p.x>west+span-256 || p.y>north-256 || p.y<north-span+256) continue;
                    p.z=land->getTerrainElevation(p);
                    if (p.z>=water+8) positions.push_back(p);
                }
            }
        if (positions.size()==100)
            for (const auto& p:positions)
                if (auto* object=mgr->addBuilding(p,group,index,0,0,1.0f,false)) { mgr->select(*object,true); ++count; }
        EditorInterface::frameSelectedObjects();
        if (eye) eye->setCameraAltitudeDesired(2600.0f);
	}
	fprintf(stderr,"[EDITOR_CLI v1] event=mover_population count=%d ok=%d frame=%ld\n",count,count==100?1:0,frame);
	fflush(stderr);
}

// Unlike the synchronous authoring probes, leave each edit alive across rendered
// frames so the shadow job can consume it. These run only under -smoke-shadow-edits.
void EditorInterface::tickShadowEditSmoke(long frame)
{
	static long nextFrame = 0;
	static int stage = 0;
	static const Appearance* placedOwner = nullptr;
	if (!land || stage == 6) return;
	if (!nextFrame) { nextFrame = frame + 240; return; }
	if (frame < nextFrame) return;
	bool ok = false;
	if (stage == 0) {
		const int mid = (land->realVerticesMapSide - 1) / 2;
		glm::vec3 point(land->mapTopLeft3d.x + mid * land->worldUnitsPerVertex,
			land->mapTopLeft3d.y - mid * land->worldUnitsPerVertex, 0.0f);
		point.z = land->getTerrainElevation(point);
		HeightBrush brush(HeightBrush::RAISE, land->worldUnitsPerVertex * 0.25f, 32.0f);
		brush.beginPaint();
		ok = brush.paint(point, 0, 0);
		undoMgr.AddAction(brush.endPaint());
		ok = ok && land->getTerrainElevation(point) > point.z;
	} else if (stage == 2) {
		unsigned long group{}, index{};
		auto* mgr = EditorObjectMgr::instance();
		// A known stock static prop; never choose a marker or a mover.
		if (mgr->getBuildingFromID(263, group, index, true)) {
			setActiveBrush(new BuildingBrush(group, index, 0), -500, -500);
			auto* brush = dynamic_cast<BuildingBrush*>(curBrush);
			const int mid = (land->realVerticesMapSide - 1) / 2;
			for (int row = mid; row < mid + 24 && !ok; ++row)
				for (int col = mid; col < mid + 24 && !ok; ++col) {
					glm::vec3 point{};
					land->cellToWorld(row, col, point);
					point.z = land->getTerrainElevation(point);
					if (!brush->canPaint(point, 0, 0, 0)) continue;
					brush->beginPaint();
					ok = brush->paint(point, 0, 0);
					undoMgr.AddAction(brush->endPaint());
					if (ok) {
						auto* placed = mgr->getObjectAtLocation(point.x, point.y);
						placedOwner = placed->appearance();
					}
				}
		}
	} else if (stage == 4) {
		ok = undoMgr.Redo();
	} else {
		if (stage >= 3) {
			const Appearance* owner = nullptr;
			for (auto* object : EditorObjectMgr::instance()->getBuildings())
				if (object->appearance() == placedOwner) owner = object->appearance();
			int trackedRecipe = -2;
			for (const auto& entry : mc2shadow::staticCasterSet().entries())
				if (entry.owner == owner) trackedRecipe = entry.recipeIndex;
			fprintf(stderr, "[EDITOR_CLI v1] event=shadow_delete recipe=%d tracked_recipe=%d undo_index=%d found=%d\n",
				owner ? owner->getShadowCasterRecipeIndex() : -1, trackedRecipe, undoMgr.GetCurrentPosition(), owner ? 1 : 0);
		}
		ok = undoMgr.Undo();
		if (stage >= 3) {
			for (auto* object : EditorObjectMgr::instance()->getBuildings())
				if (object->appearance() == placedOwner) ok = false;
		}
	}
	fprintf(stderr, "[EDITOR_CLI v1] event=shadow_edit stage=%d ok=%d frame=%ld\n", stage, ok ? 1 : 0, frame);
	fflush(stderr);
	++stage;
	nextFrame = frame + 240;
}

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

static Unit* EditorSmokeUnitById(long id)
{
	EditorObjectMgr* mgr = EditorObjectMgr::instance();
	if (mgr)
		for (auto* u : mgr->getUnits())
			if (u && u->getID() == id)
				return u;
	return nullptr;
}

static bool EditorSmokeRouteEq(const std::vector<glm::vec3>& a, const std::vector<glm::vec3>& b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (fabsf(a[i].x - b[i].x) > 0.5f || fabsf(a[i].y - b[i].y) > 0.5f)
			return false;
	return true;
}

// S1 route tool: on the reloaded brain unit, author a route through the viewport gestures
// (facade set, Alt insert, drag move, Ctrl delete-one, Shift stroke, click append, Delete key,
// once->loop), each exactly one undo step; assign it to a second unit as one step; then save
// -> reload -> equal. The game admits the saved keys through Mission::admitAuthoredRoutes.
// Restores e0_authored.pak afterwards so later smoke sections see the same map.
static bool runRouteSmoke(Unit* unit)
{
	EditorObjectMgr* mgr = EditorObjectMgr::instance();
	Unit* peer = nullptr;
	if (mgr)
		for (auto* u : mgr->getUnits())
			if (u && u != unit) { peer = u; break; }
	if (!unit || !peer || !ActionUndoMgr::instance)
		return false;
	ActionUndoMgr& undo = *ActionUndoMgr::instance;
	const glm::vec3 o = unit->getPosition();
	const auto at = [&](float dx, float dy) { return glm::vec3(o.x + dx, o.y + dy, 0.0f); };
	std::vector<std::vector<glm::vec3>> routes;
	std::vector<int> types;
	const auto snap = [&]() { routes.push_back(unit->getWaypoints()); types.push_back(unit->getOrderType()); };
	const auto press = [&](const glm::vec3& p, bool ctrl, bool alt, bool shift) { return UnitBrainPanel::RoutePress(p.x, p.y, p.z, ctrl, alt, shift); };
	const auto drag = [&](float dx, float dy) { const glm::vec3 p = at(dx, dy); UnitBrainPanel::RouteDrag(p.x, p.y, p.z); };
	snap();
	bool ok = EditorCommands::setRoute(unit, Unit::ORDER_MOVE, {at(256, 0), at(512, 0), at(512, 256)});
	snap();
	UnitBrainPanel::BeginWaypointPlace(unit->getID());
	ok = ok && press(at(384, 8), false, true, false) && unit->getWaypointCount() == 4 && unit->getWaypoints()[1] == at(384, 8);
	snap();
	ok = ok && press(at(512, 0), false, false, false) && UnitBrainPanel::RouteGestureActive();
	drag(640, 128);
	ok = ok && UnitBrainPanel::RouteRelease() && unit->getWaypoints()[2] == at(640, 128);
	snap();
	ok = ok && press(at(384, 8), true, false, false) && unit->getWaypointCount() == 3;
	snap();
	ok = ok && press(at(768, 512), false, false, true);
	drag(832, 512);
	drag(1024, 512);
	drag(1280, 512);
	ok = ok && UnitBrainPanel::RouteRelease() && unit->getWaypointCount() == 6;
	snap();
	ok = ok && press(at(1280, 768), false, false, false) && unit->getWaypointCount() == 7;
	snap();
	ok = ok && UnitBrainPanel::RouteDeleteSelected() && unit->getWaypointCount() == 6;
	snap();
	UnitBrainPanel::EndWaypointPlace();
	ok = ok && EditorCommands::setRoute(unit, Unit::ORDER_PATROL, unit->getWaypoints());
	snap();
	const size_t steps = routes.size() - 1;
	bool undoOk = ok;
	for (size_t k = steps; undoOk && k > 0; --k)
		undoOk = undo.Undo() && unit->getWaypoints() == routes[k - 1] && unit->getOrderType() == types[k - 1];
	for (size_t k = 1; undoOk && k <= steps; ++k)
		undoOk = undo.Redo() && unit->getWaypoints() == routes[k] && unit->getOrderType() == types[k];
	const std::vector<glm::vec3> peerBefore = peer->getWaypoints();
	const bool assigned = undoOk && EditorCommands::assignRoute(unit, {unit, peer}) == 1
		&& peer->getWaypoints() == unit->getWaypoints() && peer->getOrderType() == Unit::ORDER_PATROL
		&& undo.Undo() && peer->getWaypoints() == peerBefore && unit->getWaypoints() == routes[steps]
		&& undo.Redo() && peer->getWaypoints() == unit->getWaypoints();
	const long unitId = unit->getID(), peerId = peer->getID();
	const std::vector<glm::vec3> route = unit->getWaypoints();
	const bool saved = assigned && EditorData::instance->save("terrain_gen_out/e0_route.pak", true);
	const bool reloaded = saved && EditorData::initTerrainFromPCV("terrain_gen_out/e0_route.pak");
	Unit* u = reloaded ? EditorSmokeUnitById(unitId) : nullptr;
	Unit* p = reloaded ? EditorSmokeUnitById(peerId) : nullptr;
	const bool equal = u && p && u->isOrderAuthored() && p->isOrderAuthored()
		&& u->getOrderType() == Unit::ORDER_PATROL && p->getOrderType() == Unit::ORDER_PATROL
		&& EditorSmokeRouteEq(u->getWaypoints(), route) && EditorSmokeRouteEq(p->getWaypoints(), route);
	const bool restored = EditorData::initTerrainFromPCV("terrain_gen_out/e0_authored.pak");
	fprintf(stderr, "[EDITOR_ROUTE v1] steps=%zu gestures=%d undo_redo=%d assigned=%d saved=%d reloaded=%d equal=%d restored=%d waypoints=%zu\n",
		steps, ok ? 1 : 0, undoOk ? 1 : 0, assigned ? 1 : 0, saved ? 1 : 0, reloaded ? 1 : 0, equal ? 1 : 0, restored ? 1 : 0, route.size());
	return equal && restored;
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
	// Rotation (owner item 2): rotate the placed building and the brain unit through
	// the user's [/] path, then prove the rotation survives save and reopen.
	float buildingYaw = 0.0f;
	float unitYaw = 0.0f;
	bool rotateReady = false;
	// Yaw is compared modulo 360 (a reload normalizes 225 to -135). A spinMe building (radar dish;
	// isSelectable() false) advances its yaw on every update(), so its yaw is animation, not authored
	// state: the proof then rests on the unit and the building only has to survive the round trip.
	const auto sameYaw = [](float a, float b) { const float d = fmodf(fabsf(a - b), 360.0f); return (d < 360.0f - d ? d : 360.0f - d) < 0.01f; };
	const bool buildingSpins = placed && placed->appearance() && !placed->appearance()->isSelectable();
	if (placed && brainUnit && placed->appearance() && brainUnit->appearance())
	{
		mgr->unselectAll();
		mgr->select(*placed, true);
		mgr->select(*brainUnit, true);
		const float oldBuildingYaw = placed->appearance()->rotation;
		const float oldUnitYaw = brainUnit->appearance()->rotation;
		EditorData::instance->MissionNeedsSaving(false);
		rotateSelectedObjects(1);
		buildingYaw = placed->appearance()->rotation;
		unitYaw = brainUnit->appearance()->rotation;
		const bool turned = (buildingSpins || !sameYaw(buildingYaw, oldBuildingYaw)) && !sameYaw(unitYaw, oldUnitYaw);
		const bool dirty = EditorData::instance->MissionNeedsSaving();
		const bool undone = undoMgr.Undo();
		const float undoBuildingYaw = placed->appearance()->rotation;
		const float undoUnitYaw = brainUnit->appearance()->rotation;
		const bool redone = undoMgr.Redo();
		fprintf(stderr, "[EDITOR_ROTATE v1] spins=%d turned=%d dirty=%d undo=%d redo=%d building=%.2f->%.2f undo=%.2f now=%.2f unit=%.2f->%.2f undo=%.2f now=%.2f\n",
			buildingSpins, turned, dirty, undone, redone, oldBuildingYaw, buildingYaw, undoBuildingYaw, placed->appearance()->rotation,
			oldUnitYaw, unitYaw, undoUnitYaw, brainUnit->appearance()->rotation);
		rotateReady = turned && dirty && undone && (buildingSpins || sameYaw(undoBuildingYaw, oldBuildingYaw)) && sameYaw(undoUnitYaw, oldUnitYaw)
			&& redone && (buildingSpins || sameYaw(placed->appearance()->rotation, buildingYaw)) && sameYaw(brainUnit->appearance()->rotation, unitYaw);
		mgr->unselectAll();
	}
	const bool authoredSaved = EditorData::instance->save("terrain_gen_out/e0_authored.pak", true);
	// Undo removes the authored state before reload, so in-memory remnants cannot pass.
	const bool removedRotation = rotateReady && undoMgr.Undo() && !sameYaw(brainUnit->appearance()->rotation, unitYaw)
		&& (buildingSpins || !sameYaw(placed->appearance()->rotation, buildingYaw));
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
	const auto yawMatches = [&](EditorObject* obj, float yaw) { return obj && obj->appearance() && sameYaw(obj->appearance()->rotation, yaw); };
	fprintf(stderr, "[EDITOR_ROTATE v1] ready=%d removed=%d reloaded_building=%.2f reloaded_unit=%.2f\n", rotateReady, removedRotation,
		loadedObject && loadedObject->appearance() ? loadedObject->appearance()->rotation : -999.0f,
		loadedBrain && loadedBrain->appearance() ? loadedBrain->appearance()->rotation : -999.0f);
	EditorSmokePanelFact("rotation", "Rotate", rotateReady && removedRotation && objectReloaded && brainReloaded
		&& (buildingSpins || yawMatches(loadedObject, buildingYaw)) && yawMatches(loadedBrain, unitYaw));
	EditorSmokePanelFact("terrain_paint", "PaintMaterial", terrainReady && removedTerrain && reloaded && land->getTerrain(mid, mid) == newType);
	EditorSmokePanelFact("object_placement", "AssetBrowser", placementUndo && removedObject && objectReloaded);
	EditorSmokePanelFact("teams_abl", "Inspector", teamReady && removedTeam && objectReloaded && loadedObject->getAlignment() == newTeam);
	EditorSmokePanelFact("teams_abl", "UnitBrain", brainReady && removedBrain && brainReloaded);
	EditorSmokePanelFact("mission_export", "MissionWriter", authoredSaved && reloaded && removedTerrain && removedObject && removedBrain && objectReloaded && brainReloaded);

	EditorSmokePanelFact("route", "RouteTool", reloaded && runRouteSmoke(loadedBrain));

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
	MapGeneratorDialog::Open();
	MapGeneratorDialog::ExecutePreview();
}
