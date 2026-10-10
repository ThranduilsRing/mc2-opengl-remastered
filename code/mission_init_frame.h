// Locals Mission::init hands from one load phase to the next.
#ifndef CODE_MISSION_INIT_FRAME_H
#define CODE_MISSION_INIT_FRAME_H

#include "mission_internal.h"

// The mission-file handles, parse results and counters the Mission::init phases share.
struct MissionInitFrame {
	const char* missionName;
	FitIniFile* gameSystemFile;
	long result;
	long forestMoveCost;
	FullPathFileName missionFileName;
	long maxTeamID;
	long maxCommanderID;
	PacketFile pakFile;
	long numErrors;
	long numLinesProcessed;
	unsigned long numWarriors;
	bool loadBrainParameters;
	long soakBrainsLoaded;
	long soakBrainsFailed;
};

#endif  // CODE_MISSION_INIT_FRAME_H
