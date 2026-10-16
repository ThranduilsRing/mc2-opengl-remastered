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
	PacketFile pakFile;
	long numErrors;
	long numLinesProcessed;
	unsigned long numWarriors;
	bool loadBrainParameters;
	long soakBrainsLoaded;
	long soakBrainsFailed;
};

#endif  // CODE_MISSION_INIT_FRAME_H
