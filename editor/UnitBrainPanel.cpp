/***************************************************************
* FILENAME: UnitBrainPanel.cpp
* DESCRIPTION: Editor "AI / Brain / Orders" inspector panel — see header.
*   Read-only display of what a placed unit DOES today (AI brain script,
*   pilot, lance/squad) plus a scaffolded Orders & Stance section ("this is
*   where this goes") for patrol waypoints + attitude/ROE editing. The edit
*   controls are intentionally disabled: there is no editor-side persistence
*   for unit orders/waypoints yet, and runtime order delivery is tied to the
*   in-flight TECHSCRIPT brain dispatch (MC2_BRAIN_DISPATCH*, default-OFF).
* DATE: 2026-06-24
****************************************************************/

#include "stdafx.h"

#include "UnitBrainPanel.h"

#include "imgui.h"
#include "UnitActionFlowPanel.h"   // Inline condition/trigger/action flow

#include "EditorObjectMgr.h"
#include "EditorObjects.h"
#include "InspectorPanel.h"   // CategoryToken (shared category labelling)
#include "Action.h"           // ModifyUnitOrderAction + ActionUndoMgr (undo + dirty)
#include "Paths.h"            // warriorPath (brain .abl location) + missionPath
#include "File.h"             // File (raw brain .abl viewer)
#include "EditorData.h"       // EditorData::getMapName (current mission.fit name)
#include "cident.h"           // FullPathFileName (build mission.fit path)
#include "brain_missionfit_oporbd.h"  // parseMissionFitBrains (declarative Brain{} blocks)
// The parser TU (code/brain_missionfit_oporbd.cpp) is compiled into EditRel via
// EDITOR_BRIDGE_SOURCES in editor/CMakeLists.txt (same as gameplay_pick.cpp).
#include "MissionFitBrainSource.h"  // shared mission.fit Brain{} cache (also feeds
                                     // Unit::importPatrolFromBrainIfNeeded, EditorObjects.cpp)

#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>

namespace {

bool s_open = false;

// Waypoint placement mode state (see header). Target is held by editor id so a
// changed selection or a deleted unit never dangles.
bool s_placeActive = false;
long s_placeTargetId = 0;

// Find a Unit by editor id (placement target / re-find). getUnits() is a copy.
Unit* findUnitById( long id )
{
	EditorObjectMgr* mgr = EditorObjectMgr::instance();
	if ( !mgr )
		return nullptr;
	EditorObjectMgr::UNIT_LIST list = mgr->getUnits();
	for ( auto it = list.begin(); it != list.end(); it++ )
		if ( *it && (*it)->getID() == id )
			return *it;
	return nullptr;
}

// Run an order-state mutation on a unit as one undoable + dirty-marking edit.
template <typename Mutate>
void applyOrderEdit( Unit* unit, Mutate mut )
{
	if ( !unit )
		return;
	if ( !ActionUndoMgr::instance )
	{
		mut();   // no undo manager (shouldn't happen in-editor) — still apply
		return;
	}
	ModifyUnitOrderAction* act = new ModifyUnitOrderAction();
	act->capture( unit );
	mut();
	unit->setOrderAuthored( true );   // editor now owns this unit's order -> persisted on save
	act->commit( unit );
	ActionUndoMgr::instance->AddAction( act );
}

// First selected object from the shared selection list (viewport + Scene
// Outliner both feed it), mirroring InspectorPanel's selection access.
EditorObject* firstSelected(int* outCount)
{
	int count = 0;
	EditorObject* first = nullptr;
	EditorObjectMgr* mgr = EditorObjectMgr::instance();
	if (mgr)
	{
		EditorObjectMgr::EDITOR_OBJECT_LIST sel = mgr->getSelectedObjectList();
		for (auto it = sel.begin();
			it != sel.end(); it++)
		{
			if (*it)
			{
				if (!first)
					first = *it;
				++count;
			}
		}
	}
	if (outCount)
		*outCount = count;
	return first;
}

// Combat attitude / stance labels. Mirrors enum AttitudeType in code/tacordr.h
// (CAUTIOUS..SUICIDAL). Kept as a local string table so this panel pulls in no
// runtime-only game headers (the value a unit actually carries is runtime state
// today — see the Orders & Stance scaffold note).
const char* const kAttitudeNames[] = {
	"Cautious", "Conservative", "Normal", "Aggressive", "Berserker", "Suicidal"
};
const int kAttitudeCount = (int)(sizeof(kAttitudeNames) / sizeof(kAttitudeNames[0]));

const char* brainBehaviorName( int b )
{
	switch ( b )
	{
		case Unit::BRAIN_PATROL: return "Patrol";
		case Unit::BRAIN_GUARD:  return "Guard";
		case Unit::BRAIN_ATTACK: return "Attack";
		case Unit::BRAIN_IDLE:   return "Idle / static";
		default:                 return "Unknown";
	}
}

// Load a brain .abl as text for the raw viewer, cached by brain name so we only
// hit disk when the selection's brain changes.
const char* loadBrainText( const char* brainName )
{
	static std::string s_text;
	static std::string s_loadedFor;
	if ( !brainName || !brainName[0] ) { s_text.clear(); s_loadedFor.clear(); return ""; }
	if ( s_loadedFor == brainName ) return s_text.c_str();
	s_loadedFor = brainName;
	s_text.clear();

	char path[512];
	sprintf( path, "%s%s.abl", warriorPath, brainName );
	File f;
	if ( NO_ERR != f.open( path ) ) { s_text = "(brain .abl not found)"; return s_text.c_str(); }
	char line[1024];
	while ( !f.eof() )
	{
		long n = f.readLine( (MemoryPtr)line, (long)sizeof( line ) - 1 );
		if ( n <= 0 ) break;
		if ( n >= (long)sizeof( line ) ) n = (long)sizeof( line ) - 1;
		line[n] = 0;
		s_text += line;
		s_text += '\n';
	}
	return s_text.c_str();
}

// --- Declarative Brain (mission.fit) -----------------------------------------
// READ-ONLY display; the parse/cache/position-match live in MissionFitBrainSource
// (shared with Unit::importPatrolFromBrainIfNeeded, editor/EditorObjects.cpp, so the
// panel and the waypoint importer never disagree about which brain matches a unit).

// Render one OPORD slot (read-only). No-ops if the slot is absent.
void drawOpordSlot( const char* label, const MissionFitOpordSlot& s )
{
	if ( !s.type[0] )
		return;
	ImGui::Text( "%s: %s%s", label, s.type, s.loop ? " (loop)" : "" );
	if ( s.escortTargetIndex >= 0 )
		ImGui::Text( "    escort -> Warrior %d", s.escortTargetIndex );
	for ( int w = 0; w < s.waypointCount; ++w )
		ImGui::TextDisabled( "    wp %d: (%.1f, %.1f)", w, s.waypoints[w].x, s.waypoints[w].y );
}

} // namespace

void UnitBrainPanel::Open()  { s_open = true; }
void UnitBrainPanel::Close() { s_open = false; EndWaypointPlace(); }
void UnitBrainPanel::Toggle(){ s_open = !s_open; if ( !s_open ) EndWaypointPlace(); }
bool UnitBrainPanel::IsOpen(){ return s_open; }

bool UnitBrainPanel::SmokeProbe()
{
	Unit* unit = dynamic_cast<Unit*>(firstSelected(nullptr));
	if (!unit)
		return false;
	const auto count = unit->getWaypointCount();
	BeginWaypointPlace(unit->getID());
	const bool placed = HandlePlacementClick(32.0f, 48.0f, 0.0f);
	EndWaypointPlace();
	return placed && unit->isOrderAuthored() && unit->getWaypointCount() == count + 1
		&& unit->getWaypoints().back().x == 32.0f && unit->getWaypoints().back().y == 48.0f;
}

bool UnitBrainPanel::WaypointPlaceActive()   { return s_placeActive; }
long UnitBrainPanel::WaypointPlaceTargetId() { return s_placeTargetId; }
void UnitBrainPanel::BeginWaypointPlace( long unitId ) { s_placeActive = true;  s_placeTargetId = unitId; }
void UnitBrainPanel::EndWaypointPlace()                { s_placeActive = false; s_placeTargetId = 0; }

bool UnitBrainPanel::HandlePlacementClick( float worldX, float worldY, float worldZ )
{
	if ( !s_placeActive )
		return false;
	Unit* unit = findUnitById( s_placeTargetId );
	if ( !unit )
	{
		EndWaypointPlace();
		return false;
	}
	glm::vec3 wp; wp.x = worldX; wp.y = worldY; wp.z = worldZ;
	applyOrderEdit( unit, [&]() {
		// Adding a path implies an order — default a None unit to PATROL on first point.
		if ( unit->getOrderType() == Unit::ORDER_NONE )
			unit->setOrderType( Unit::ORDER_PATROL );
		unit->addWaypoint( wp );
	} );
	return true;
}

void UnitBrainPanel::Draw()
{
	if (!s_open)
		return;

	// Its own panel, exactly like InspectorPanel — a standalone window that docks in
	// the editor layout (not inline in the Tools column). The toggle button in
	// renderToolbarImGui owns the "AI / Brain / Orders" label; give the WINDOW the
	// same title (window title vs button are different ID scopes, so no clash).
	ImGui::SetNextWindowSize(ImVec2(320.f, 420.f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("AI / Brain / Orders", &s_open))
	{
		ImGui::End();
		return;
	}

	EditorObjectMgr* mgr = EditorObjectMgr::instance();
	if (!mgr)
	{
		ImGui::TextDisabled("No map loaded.");
		ImGui::End();
		return;
	}

	int selCount = 0;
	EditorObject* obj = firstSelected(&selCount);
	if (!obj)
	{
		ImGui::TextDisabled("Select a unit to view its AI / orders.");
		ImGui::End();
		return;
	}

	Unit* unit = dynamic_cast<Unit*>(obj);
	if (!unit)
	{
		ImGui::TextDisabled("Selected object is not a unit.");
		ImGui::Text("Category: %s", InspectorPanel::CategoryToken(obj));
		ImGui::End();
		return;
	}

	// Pull in an existing patrol from the unit's brain .abl (display only) the
	// first time we show it, so stock patrols appear without any edit.
	unit->importPatrolFromBrainIfNeeded();

	if (selCount > 1)
		ImGui::TextDisabled("%d objects selected (showing first unit)", selCount);

	// --- Identity ---------------------------------------------------------
	const char* name = obj->getDisplayName();
	if (!name || !name[0])
		name = "(unnamed)";
	ImGui::Text("Unit:  %s", name);
	ImGui::Text("ID:    0x%08lX", (unsigned long)obj->getID());

	Pilot* pilot = unit->getPilot();
	const char* pilotName = (pilot && pilot->info) ? pilot->getName() : nullptr;
	ImGui::Text("Pilot: %s", (pilotName && pilotName[0]) ? pilotName : "(none)");

	int lance = -1, lanceIdx = -1;
	unit->getLanceInfo(lance, lanceIdx);
	ImGui::Text("Lance: %d (slot %d)   Squad: %lu",
		lance, lanceIdx, (unsigned long)unit->getSquad());

	ImGui::Separator();

	// --- Chassis (the mech/vehicle + its asset refs the .arm tracks) -------
	if (ImGui::CollapsingHeader("Chassis", ImGuiTreeNodeFlags_DefaultOpen))
	{
		EditorObjectMgr* m = EditorObjectMgr::instance();
		const char* file = m ? m->getFileName(obj->getID()) : nullptr;   // chassis CSV (e.g. Madcat)
		ImGui::Text("Chassis:  %s", (file && file[0]) ? file : "(unknown)");
		ImGui::Text("Variant:  %d", unit->getVariant());
		ImGui::Text("Pilot:    %s", (pilotName && pilotName[0]) ? pilotName : "(none)");
	}

	// --- AI Brain (what this unit actually runs) --------------------------
	if (ImGui::CollapsingHeader("AI Brain", ImGuiTreeNodeFlags_DefaultOpen))
	{
		const Brain& brain = unit->getBrain();
		const char* bn = brain.getBrainName();
		ImGui::Text("Brain script: %s", (bn && bn[0]) ? bn : "(none / default)");
		const char* fsm = unit->getBrainFsm();
		if (fsm && fsm[0])
			ImGui::Text("FSM:          %s", fsm);
		ImGui::Text("Behavior:     %s", brainBehaviorName(unit->getBrainBehavior()));
		ImGui::Text("ABL cells:    %ld   Static vars: %ld",
			brain.getNumCells(), brain.getNumStaticVars());

		// Raw .abl script viewer (read-only, loaded on demand, cached by name).
		if (ImGui::TreeNode("View .abl script"))
		{
			if (bn && bn[0])
			{
				const char* txt = loadBrainText(bn);
				ImGui::InputTextMultiline("##ablsrc", const_cast<char*>(txt),
					strlen(txt) + 1, ImVec2(-1.f, 220.f), ImGuiInputTextFlags_ReadOnly);
			}
			else
			{
				ImGui::TextDisabled("(no brain script for this unit)");
			}
			ImGui::TreePop();
		}

		// condition/trigger/action flow extracted from the .abl text.
		UnitActionFlowPanel::DrawInline((bn && bn[0]) ? loadBrainText(bn) : "");
	}

	// --- Declarative Brain (mission.fit) — READ-ONLY ----------------------
	// Inline Brain{} block from the current mission.fit, matched to this unit
	// by spawn position. Parsed by parseMissionFitBrains (carver_v_enhanced).
	if (ImGui::CollapsingHeader("Declarative Brain (mission.fit)"))
	{
		if (!EditorData::getMapName())
		{
			ImGui::TextDisabled("No map loaded.");
		}
		else
		{
			MissionFitBrainSource::Refresh();
			const glm::vec3& p = obj->getPosition();
			const int brainCount = MissionFitBrainSource::BrainCount();

			// Diagnostics (collapsed): the path we read, how many brains parsed, the selected
			// unit's world pos, and the NEAREST brain (uncapped) so a "no match" / coordinate
			// mismatch can be debugged without a rebuild.
			if (ImGui::TreeNode("debug"))
			{
				const char* lastPath = MissionFitBrainSource::LastFitPath();
				ImGui::TextDisabled("fit: %s", (lastPath && lastPath[0]) ? lastPath : "(none)");
				ImGui::TextDisabled("brains: %d   unit pos: (%.1f, %.1f)", brainCount, (float)p.x, (float)p.y);
				const MissionFitBrain* nb = nullptr; float nd2 = 1e30f;
				for (int i = 0; i < brainCount; ++i)
				{
					const MissionFitBrain* bb = MissionFitBrainSource::BrainAt(i);
					if (!bb || bb->posX <= -1e8f) continue;
					float dx = bb->posX - (float)p.x, dy = bb->posY - (float)p.y, d2 = dx * dx + dy * dy;
					if (d2 < nd2) { nd2 = d2; nb = bb; }
				}
				if (nb)
					ImGui::TextDisabled("nearest: W%d (%.1f,%.1f) dist=%.1f",
						nb->warriorIndex, nb->posX, nb->posY, (float)sqrt((double)nd2));
				ImGui::TreePop();
			}

			const MissionFitBrain* b = MissionFitBrainSource::MatchForPos((float)p.x, (float)p.y);
			if (!b)
			{
				ImGui::TextDisabled("No declarative brain matches this unit (or mission.fit has none).");
			}
			else
			{
				if (b->archetype[0])
					ImGui::Text("Archetype: %s", b->archetype);
				else
					ImGui::TextDisabled("Archetype: (none)");
				if (b->compatibilityMode[0])
					ImGui::Text("Compatibility: %s", b->compatibilityMode);
				else
					ImGui::TextDisabled("Compatibility: (none)");

				ImGui::Separator();
				drawOpordSlot("Primary",   b->primary);
				drawOpordSlot("Secondary", b->secondary);
				drawOpordSlot("Tertiary",  b->tertiary);
				if (!b->primary.type[0] && !b->secondary.type[0] && !b->tertiary.type[0])
					ImGui::TextDisabled("OPORDs: (none)");

				ImGui::Separator();
				if (b->engageRadius >= 0.0f)
					ImGui::Text("EngageRadius: %.1f", b->engageRadius);
				if (b->requestHelp != -1)
					ImGui::Text("RequestHelp: %s", b->requestHelp ? "yes" : "no");
				if (b->returnToPost != -1)
					ImGui::Text("ReturnToPost: %s", b->returnToPost ? "yes" : "no");
				if (b->wakeOnAttack != -1)
					ImGui::Text("WakeOnAttack: %s", b->wakeOnAttack ? "yes" : "no");
				if (b->poweredDown != -1)
					ImGui::Text("PoweredDown: %s", b->poweredDown ? "yes" : "no");
				if (b->attackerHelpRadius >= 0.0f)
					ImGui::Text("AttackerHelpRadius: %.1f", b->attackerHelpRadius);
				if (b->defenderHelpRadius >= 0.0f)
					ImGui::Text("DefenderHelpRadius: %.1f", b->defenderHelpRadius);

				if (b->tacticCount > 0)
				{
					ImGui::Separator();
					ImGui::Text("Tactics:");
					for (int t = 0; t < b->tacticCount; ++t)
						ImGui::Text("    %s = %.2f", b->tacticName[t], b->tacticWeight[t]);
				}
			}
		}
	}

	// --- Orders & Stance (live editing; persists to the mission .fit) ------
	if (ImGui::CollapsingHeader("Orders & Stance", ImGuiTreeNodeFlags_DefaultOpen))
	{
		// Order type: None / Move (one-way) / Patrol (closed loop).
		static const char* const kOrderNames[] = { "None", "Move (one-way)", "Patrol (loop)" };
		int orderType = unit->getOrderType();
		if (ImGui::Combo("Order", &orderType, kOrderNames, 3))
			applyOrderEdit(unit, [&]() { unit->setOrderType(orderType); });

		// Stance / attitude.
		int stance = unit->getStance();
		if (ImGui::Combo("Stance", &stance, kAttitudeNames, kAttitudeCount))
			applyOrderEdit(unit, [&]() { unit->setStance(stance); });

		ImGui::Separator();
		ImGui::Text("Waypoints: %lu", unit->getWaypointCount());

		// Placement mode toggle (this unit). While active, map clicks append points.
		const bool placingThis = s_placeActive && (s_placeTargetId == obj->getID());
		if (placingThis)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.25f, 0.15f, 1.0f));
			if (ImGui::Button("Click map to add… (Stop)"))
				UnitBrainPanel::EndWaypointPlace();
			ImGui::PopStyleColor();
		}
		else
		{
			if (ImGui::Button("Add Waypoints"))
				UnitBrainPanel::BeginWaypointPlace(obj->getID());
		}
		ImGui::SameLine();
		if (ImGui::Button("Remove Last") && unit->getWaypointCount() > 0)
			applyOrderEdit(unit, [&]() { unit->removeLastWaypoint(); });
		ImGui::SameLine();
		if (ImGui::Button("Clear") && unit->getWaypointCount() > 0)
			applyOrderEdit(unit, [&]() { unit->clearWaypoints(); });

		// Point list.
		const std::vector<glm::vec3>& wps = unit->getWaypoints();
		if (!wps.empty() && ImGui::BeginChild("wplist", ImVec2(0.f, 90.f), true))
		{
			for (size_t i = 0; i < wps.size(); ++i)
				ImGui::Text("%2u:  %.0f, %.0f", (unsigned)i, wps[i].x, wps[i].y);
		}
		if (!wps.empty())
			ImGui::EndChild();

		ImGui::TextDisabled(
			"Authoring + persistence are live. Units actually executing the "
			"order in-game is wired later via the brain dispatch.");
	}

	ImGui::End();
}
