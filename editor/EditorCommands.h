#ifndef EDITOR_COMMANDS_H
#define EDITOR_COMMANDS_H
// Editor command facade: the authoring verbs the viewport, panels and smoke call,
// each one undo step through ActionUndoMgr. Kept free of MFC/ImGui so the verbs can
// move to the new editor host unchanged. Today it carries the route verbs (S1) and
// the team verb; place/rotate/remove still live on EditorInterface.

#include "math/glm_config.h"
#include <vector>

class Unit;
class EditorObject;

namespace EditorCommands
{
	// Replace a unit's order type and waypoints as ONE undo step; the order becomes
	// editor-authored (persisted by Unit::save). False if nothing changed.
	bool setRoute( Unit* unit, int orderType, const std::vector<glm::vec3>& waypoints );

	// Give every target the source's order type and waypoints as ONE undo step
	// (N-unit assign). Returns how many targets changed.
	int assignRoute( const Unit* source, const std::vector<Unit*>& targets );

	// Team change through EditorInterface::applyObjectAlignment (one undo step).
	bool setTeam( EditorObject* obj, int team );

	// Pure route geometry used by the viewport tool (no undo, no unit).
	// Index of the waypoint within radius of (x,y), nearest first; -1 if none.
	int nearestWaypoint( const std::vector<glm::vec3>& wps, float x, float y, float radius );
	// Index at which a point at (x,y) splits the nearest segment (loop adds the
	// closing segment). Empty or single-point routes append.
	int insertIndexFor( const std::vector<glm::vec3>& wps, float x, float y, bool loop );
}

#endif // EDITOR_COMMANDS_H
