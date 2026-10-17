// Locals Mission::load hands from one load phase to the next.
#ifndef CODE_MISSION_LOAD_FRAME_H
#define CODE_MISSION_LOAD_FRAME_H

#include "saveload_internal.h"

// The save-file handle, the unpacked mission file and the counters the Mission::load phases share.
struct MissionLoadFrame {
	const char* loadFileName;
	DWORD oldXLoc;
	DWORD oldYLoc;
	bool turnOffAsyncMouse;
	bool incompatibleSave = false;
	PacketFile loadFile;
	long result;
	DWORD currentPacket;
	char tmpString[2048];
	FitIniFile gameSystemFile;   // gamesys.fit, open from loadGameSystemRanges until loadObjectivesCommanders closes it
	long forestMoveCost;
	FullPathFileName tmpName;
	FitIniFile missionFile;
	long rps;
	PacketFile pakFile;
};

#endif  // CODE_MISSION_LOAD_FRAME_H
