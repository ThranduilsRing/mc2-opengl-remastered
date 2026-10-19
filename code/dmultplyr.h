//***************************************************************************
//
//	dmultplyr.h -- File contains the MoverGroup Definition
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef DMULTPLYR_H
#define DMULTPLYR_H

//---------------------------------------------------------------------------

inline constexpr auto WORLDCHUNK_TYPE_BITS = 4;
inline constexpr auto WORLDCHUNK_TERRAINBLOCK_BITS = 8;
inline constexpr auto WORLDCHUNK_TERRAINVERTEX_BITS = 9;
inline constexpr auto WORLDCHUNK_TERRAINITEM_BITS = 3;
inline constexpr auto WORLDCHUNK_CELLPOS_BITS = 10;
inline constexpr auto WORLDCHUNK_MINE_BITS = 3;
inline constexpr auto WORLDCHUNK_TEAM_BITS = 4;
inline constexpr auto WORLDCHUNK_SECONDS_BITS = 6;
inline constexpr auto WORLDCHUNK_ARTILLERY_TYPE_BITS = 3;
inline constexpr auto WORLDCHUNK_ARTILLERY_TIME_BITS = 5;
inline constexpr auto WORLDCHUNK_TERROBJ_ALIGNMENT_BITS = 2;
inline constexpr auto WORLDCHUNK_MISSIONSCRIPT_CODE_BITS = 8;
inline constexpr auto WORLDCHUNK_MISSIONSCRIPT_PARM_BITS = 16;
inline constexpr auto WORLDCHUNK_PILOT_INDEX_BITS = 7;
inline constexpr auto WORLDCHUNK_VEHICLE_CLASS_BITS = 3;
inline constexpr auto WORLDCHUNK_SCORE_BITS = 16;
inline constexpr auto WORLDCHUNK_COMMANDER_BITS = 4;
inline constexpr auto WORLDCHUNK_SUBTYPE_BITS = 2;
inline constexpr auto WORLDCHUNK_CAPTURE_TIME_BITS = 20;

inline constexpr auto WORLDCHUNK_TYPE_MASK = 0x0000000F;
inline constexpr auto WORLDCHUNK_TERRAINBLOCK_MASK = 0x000000FF;
inline constexpr auto WORLDCHUNK_TERRAINVERTEX_MASK = 0x000001FF;
inline constexpr auto WORLDCHUNK_TERRAINITEM_MASK = 0x00000007;
inline constexpr auto WORLDCHUNK_CELLPOS_MASK = 0x000003FF;
inline constexpr auto WORLDCHUNK_MINE_MASK = 0x00000007;
inline constexpr auto WORLDCHUNK_TEAMID_MASK = 0x0000000F;
inline constexpr auto WORLDCHUNK_SECONDS_MASK = 0x0000003F;
inline constexpr auto WORLDCHUNK_ARTILLERY_TYPE_MASK = 0x00000007;
inline constexpr auto WORLDCHUNK_ARTILLERY_TIME_MASK = 0x0000001F;
inline constexpr auto WORLDCHUNK_TERROBJ_ALIGNMENT_MASK = 0x00000003;
inline constexpr auto WORLDCHUNK_MISSIONSCRIPT_CODE_MASK = 0x000000FF;
inline constexpr auto WORLDCHUNK_MISSIONSCRIPT_PARM_MASK = 0x0000FFFF;
inline constexpr auto WORLDCHUNK_PILOT_INDEX_MASK = 0x0000007F;
inline constexpr auto WORLDCHUNK_VEHICLE_CLASS_MASK = 0x00000007;
inline constexpr auto WORLDCHUNK_SCORE_MASK = 0x0000FFFF;
inline constexpr auto WORLDCHUNK_COMMANDER_MASK = 0x0000000F;
inline constexpr auto WORLDCHUNK_SUBTYPE_MASK = 0x00000003;
inline constexpr auto WORLDCHUNK_CAPTURE_TIME_MASK = 0x0003FFFF;

enum {
	WORLDCHUNK_MINE,
	WORLDCHUNK_FIRE,
	WORLDCHUNK_ARTILLERY_COMMANDER0,		// We pass the TEAM id now, NOT the commander
	WORLDCHUNK_ARTILLERY_COMMANDER1,		// We need for 8 player MPlayer
	WORLDCHUNK_ARTILLERY_COMMANDER2,		// -fs
	WORLDCHUNK_ARTILLERY_COMMANDER3,
	WORLDCHUNK_ARTILLERY_COMMANDER4,
	WORLDCHUNK_ARTILLERY_COMMANDER5,
	WORLDCHUNK_ARTILLERY_COMMANDER6,
	WORLDCHUNK_ARTILLERY_COMMANDER7,
	WORLDCHUNK_MISSION_SCRIPT_MESSAGE,
	WORLDCHUNK_PILOT_KILL_STAT,
	WORLDCHUNK_SCORE,
	WORLDCHUNK_KILL_LOSS,
	WORLDCHUNK_CAPTURE_BUILDING,
	WORLDCHUNK_END_MISSION,
	NUM_WORLDCHUNK_TYPES
};

//---------------------------------------------------------------------------

class WorldChunk;
using WorldChunkPtr = WorldChunk*;

#endif

//***************************************************************************

