// Locals Mission::init hands from one load phase to the next.
#ifndef CODE_MISSION_INIT_FRAME_H
#define CODE_MISSION_INIT_FRAME_H

#include "mission_internal.h"
#include <memory>

// The mission-file handles, parse results and counters the Mission::init phases share.
struct MissionInitFrame {
	// Normal close order is gamesys.fit, then the mission file; unwinding keeps that order after pakFile.
	std::unique_ptr<FitIniFile> missionFile;
	std::unique_ptr<FitIniFile> gameSystemFile;
	// Mission phase helpers borrow this view; it is cleared before either file is destroyed.
	FitIniFile** missionFileView = nullptr;
	~MissionInitFrame() noexcept {
		if (missionFileView)
			*missionFileView = nullptr;
	}
	const char* missionName;
	long result;
	long forestMoveCost;
	FullPathFileName missionFileName;
	long maxTeamID;
	long maxCommanderID;
	// First [PartN] TeamId seen per CommanderId (-1 = no part). Lets initCommanders give a
	// commander the logistics table cannot place (identity {i,i,i} names team i, which may not
	// exist) the team its own parts declare, instead of guessing the home team.
	signed char commanderPartTeam[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
	PacketFile pakFile;
	long numErrors;
	long numLinesProcessed;
	unsigned long numWarriors;
	bool loadBrainParameters;
	long soakBrainsLoaded;
	long soakBrainsFailed;
};

#endif  // CODE_MISSION_INIT_FRAME_H
