// Objective dialog: readable condition/action sentences for lists and viewers.
#include "stdafx.h"
#include "Objective.h"
#include "ObjectiveDlg_text_internal.h"
#include <format>

// --- Readable WHEN/THEN line formatting -------------------------------------
// Build a plain-English sentence for one condition/action, keyed on Species().
// Replaces the old "TypeLabel  rawParams" dump. Object-ref species (specific
// unit/structure) read coordinates from the SAFE noted-position accessors
// (NotedX/NotedY) and NEVER deref the live object pointer -- the engine
// InstanceDescription() for those species derefs m_pUnit/m_pBuilding unguarded
// and asserts the display name, which is unsafe from the editor list.
std::string objReadableCondition(CObjectiveCondition *cond) {
	std::string out;
	if (!cond) { out = "(empty condition)"; return out; }

	switch (cond->Species())
	{
	case DESTROY_ALL_ENEMY_UNITS:               out = "Destroy all enemy units"; break;
	case CAPTURE_OR_DESTROY_ALL_ENEMY_UNITS:    out = "Capture or destroy all enemy units"; break;
	case DEAD_OR_FLED_ALL_ENEMY_UNITS:          out = "All enemy units dead or fled"; break;
	case DESTROY_ENEMY_UNIT_GROUP:              out = "Destroy an enemy unit group"; break;
	case CAPTURE_OR_DESTROY_ENEMY_UNIT_GROUP:   out = "Capture or destroy an enemy unit group"; break;
	case DEAD_OR_FLED_ENEMY_UNIT_GROUP:         out = "An enemy unit group is dead or fled"; break;

	case DESTROY_NUMBER_OF_ENEMY_UNITS:
	case CAPTURE_OR_DESTROY_NUMBER_OF_ENEMY_UNITS:
	case DEAD_OR_FLED_NUMBER_OF_ENEMY_UNITS:
	{
		int n = 0;
		if (CNumberOfEnemyUnitsObjectiveCondition *p =
				dynamic_cast<CNumberOfEnemyUnitsObjectiveCondition *>(cond))
			n = p->Num();
		const char *verb =
			(cond->Species() == DESTROY_NUMBER_OF_ENEMY_UNITS) ? "Destroy" :
			(cond->Species() == CAPTURE_OR_DESTROY_NUMBER_OF_ENEMY_UNITS) ? "Capture or destroy" :
			"Disable (dead or fled)";
		out = std::format("{} {} enemy unit{}", verb, n, (n == 1) ? "" : "s");
		break;
	}

	case DESTROY_SPECIFIC_ENEMY_UNIT:
	case CAPTURE_OR_DESTROY_SPECIFIC_ENEMY_UNIT:
	case DEAD_OR_FLED_SPECIFIC_ENEMY_UNIT:
	case CAPTURE_UNIT:
	case GUARD_SPECIFIC_UNIT:
	{
		std::string tgt;
		if (CSpecificUnitObjectiveCondition *p =
				dynamic_cast<CSpecificUnitObjectiveCondition *>(cond))
			tgt = p->ReadableTarget();
		else
			tgt = "(unresolved)";
		const char *verb =
			(cond->Species() == DESTROY_SPECIFIC_ENEMY_UNIT) ? "Destroy" :
			(cond->Species() == CAPTURE_OR_DESTROY_SPECIFIC_ENEMY_UNIT) ? "Capture or destroy" :
			(cond->Species() == DEAD_OR_FLED_SPECIFIC_ENEMY_UNIT) ? "Disable (dead or fled)" :
			(cond->Species() == CAPTURE_UNIT) ? "Capture" : "Guard";
		const char *noun = (cond->Species() == GUARD_SPECIFIC_UNIT) ? "friendly unit" : "enemy unit";
		out = std::format("{} {}: {}", verb, noun, tgt);
		break;
	}

	case DESTROY_SPECIFIC_STRUCTURE:
	case CAPTURE_OR_DESTROY_SPECIFIC_STRUCTURE:
	case CAPTURE_STRUCTURE:
	case GUARD_SPECIFIC_STRUCTURE:
	{
		std::string tgt;
		if (CSpecificStructureObjectiveCondition *p =
				dynamic_cast<CSpecificStructureObjectiveCondition *>(cond))
			tgt = p->ReadableTarget();
		else
			tgt = "(unresolved)";
		const char *verb =
			(cond->Species() == DESTROY_SPECIFIC_STRUCTURE) ? "Destroy" :
			(cond->Species() == CAPTURE_OR_DESTROY_SPECIFIC_STRUCTURE) ? "Capture or destroy" :
			(cond->Species() == CAPTURE_STRUCTURE) ? "Capture" : "Guard";
		const char *noun = (cond->Species() == GUARD_SPECIFIC_STRUCTURE) ? "friendly structure" : "structure";
		out = std::format("{} {}: {}", verb, noun, tgt);
		break;
	}

	case MOVE_ANY_UNIT_TO_AREA:
	case MOVE_ALL_UNITS_TO_AREA:
	case MOVE_ALL_SURVIVING_UNITS_TO_AREA:
	case MOVE_ALL_SURVIVING_MECHS_TO_AREA:
	{
		float x = 0.f, y = 0.f, r = 0.f;
		if (CAreaObjectiveCondition *p = dynamic_cast<CAreaObjectiveCondition *>(cond))
		{ x = p->TargetCenterX(); y = p->TargetCenterY(); r = p->TargetRadius(); }
		const char *who =
			(cond->Species() == MOVE_ANY_UNIT_TO_AREA) ? "any unit" :
			(cond->Species() == MOVE_ALL_UNITS_TO_AREA) ? "all units" :
			(cond->Species() == MOVE_ALL_SURVIVING_UNITS_TO_AREA) ? "all surviving units" :
			"all surviving 'Mechs";
		out = std::format("Move {} into area ({:.0f}, {:.0f}) radius {:.0f}", who, x, y, r);
		break;
	}

	case BOOLEAN_FLAG_IS_SET:
	{
		const char *flag = "flag"; bool val = true;
		if (CBooleanFlagIsSet *p = dynamic_cast<CBooleanFlagIsSet *>(cond))
		{ const char *f = p->FlagID().c_str(); if (f && f[0]) flag = f; val = p->FlagValue(); }
		out = std::format("When flag '{}' is {}", flag, val ? "set" : "clear");
		break;
	}

	case ELAPSED_MISSION_TIME:
	{
		float t = 0.f;
		if (CElapsedMissionTime *p = dynamic_cast<CElapsedMissionTime *>(cond))
			t = p->Time();
		out = std::format("After {:.0f} seconds of mission time", t);
		break;
	}

	default:
		out = cond->Description();   // unknown species -> engine label fallback
		break;
	}
	return out;
}

std::string objReadableAction(CObjectiveAction *act) {
	std::string out;
	if (!act) { out = "(empty action)"; return out; }

	switch (act->Species())
	{
	case PLAY_BIK:
	{
		const char *p = "(movie)";
		if (CPlayBIK *a = dynamic_cast<CPlayBIK *>(act)) { const char *s = a->Pathname().c_str(); if (s && s[0]) p = s; }
		out = std::format("Play movie {}", p);
		break;
	}
	case PLAY_WAV:
	{
		const char *p = "(sound)";
		if (CPlayWAV *a = dynamic_cast<CPlayWAV *>(act)) { const char *s = a->Pathname().c_str(); if (s && s[0]) p = s; }
		out = std::format("Play sound {}", p);
		break;
	}
	case DISPLAY_TEXT_MESSAGE:
	{
		const char *m = "";
		if (CDisplayTextMessage *a = dynamic_cast<CDisplayTextMessage *>(act)) { const char *s = a->Message().c_str(); if (s) m = s; }
		out = std::format("Show message: \"{}\"", m);
		break;
	}
	case DISPLAY_RESOURCE_TEXT_MESSAGE:
	{
		int id = 0;
		if (CDisplayResourceTextMessage *a = dynamic_cast<CDisplayResourceTextMessage *>(act)) id = a->ResourceStringID();
		out = std::format("Show message (resource string #{})", id);
		break;
	}
	case SET_BOOLEAN_FLAG:
	{
		const char *flag = "flag"; bool val = true;
		if (CSetBooleanFlag *a = dynamic_cast<CSetBooleanFlag *>(act)) { const char *f = a->FlagID().c_str(); if (f && f[0]) flag = f; val = a->FlagValue(); }
		out = std::format("Set flag '{}' to {}", flag, val ? "set" : "clear");
		break;
	}
	case MAKE_NEW_TECHNOLOGY_AVAILABLE:
	{
		const char *p = "(file)";
		if (CMakeNewTechnologyAvailable *a = dynamic_cast<CMakeNewTechnologyAvailable *>(act)) { const char *s = a->PurchaseFile().c_str(); if (s && s[0]) p = s; }
		out = std::format("Unlock technology from {}", p);
		break;
	}
	default:
		out = act->Description();
		break;
	}
	return out;
}
