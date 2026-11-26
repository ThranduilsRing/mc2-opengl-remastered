// Locals Mission::load hands from one load phase to the next.
#ifndef CODE_MISSION_LOAD_FRAME_H
#define CODE_MISSION_LOAD_FRAME_H

#include "saveload_internal.h"
#include "save_sections.h"

// The save-file handle, the unpacked mission file and the counters the Mission::load phases share.
struct MissionLoadFrame {
	const char* loadFileName;
	DWORD oldXLoc;
	DWORD oldYLoc;
	bool turnOffAsyncMouse;
	bool incompatibleSave = false;
	bool campaignRefused = false;   // incompatibleSave because the saved campaign file is gone or broken
	bool otherMissionRefused = false;   // incompatibleSave because the save is from another mission or mod than the running one
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
	bool saveV2 = false;                       // packet 0 carries mc2::save::saveStampV2
	long savedAi = 0;                          // selected before the ABL environment is recompiled
	DWORD sectionsPacket = 0;                  // v2: index of the section container, validated by openSaveFile
	mc2::save::SectionContainer sections;
};

#endif  // CODE_MISSION_LOAD_FRAME_H
