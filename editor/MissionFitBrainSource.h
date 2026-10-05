#ifndef MISSION_FIT_BRAIN_SOURCE_H
#define MISSION_FIT_BRAIN_SOURCE_H
//------------------------------------------------------------------------------
// MissionFitBrainSource — TECHBRAIN-PATROL-VIEWER-1
//
// Shared cache of the CURRENT mission.fit's inline declarative Brain{} blocks
// (parsed by parseMissionFitBrains, code/brain_missionfit_oporbd.h), re-read
// only when EditorData::getMapName() changes. Two consumers share this cache
// (rather than each keeping its own) so they never drift apart:
//   - editor/UnitBrainPanel.cpp   — "Declarative Brain (mission.fit)" display.
//   - editor/EditorObjects.cpp    — Unit::importPatrolFromBrainIfNeeded() seeds
//     a unit's editable/drawable waypoint list from the matched brain's Primary
//     OPORD Patrol waypoints (same match a human would see in the panel).
// The actual nearest-match math is the pure matchBrainForPos() in
// code/brain_missionfit_oporbd.h — this module only owns the file I/O + cache.
//------------------------------------------------------------------------------
#include "brain_missionfit_oporbd.h"

namespace MissionFitBrainSource
{
	// Re-reads + re-parses the current mission.fit only when EditorData::getMapName()
	// (swapped .pak -> .fit) differs from the last call. Returns false (cache cleared)
	// when there is no current map. Cheap to call every frame on a cache hit.
	bool Refresh();

	int BrainCount();
	const MissionFitBrain* BrainAt( int i );   // 0 <= i < BrainCount(), else nullptr

	// The mission.fit path last (re)parsed, or "" if none. For debug/diagnostic display.
	const char* LastFitPath();

	// Nearest brain to (ux,uy) within the shared match tolerance; nullptr if none qualify.
	// Thin wrapper over brain_missionfit_oporbd.h's matchBrainForPos over the cached array.
	const MissionFitBrain* MatchForPos( float ux, float uy );
}

#endif // MISSION_FIT_BRAIN_SOURCE_H
