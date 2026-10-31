//***************************************************************************
//
//	Move.h -- Defs for Movement/Pathfinding routines
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef MOVE_H
#define MOVE_H

#include "../code/navigation_save_data.h"

//***************************************************************************

//--------------
// Include Files
#ifndef MCLIB_H
#include"mclib.h"
#endif

#ifndef DMOVEMGR_H
#include"dmovemgr.h"
#endif

#ifndef TERRAIN_H
#include"terrain.h"
#endif

#ifndef DOBJCLASS_H
#include"dobjclass.h"
#endif

#ifndef DOBJBLCK_H
#include"dobjblck.h"
#endif

#ifndef DGAMELOG_H
#include"dgamelog.h"
#endif

#include<gameos.hpp>
#include<cstddef>
#include<cstdint>
#include<cstring>
#include<vector>
#include"open_heap.h"	// the per-solver open list (mc2::OpenHeap)

// MC2_MOVE_RECON: lightweight OOB counter for pathlock calls (see move_recon.h).
// Include is deferred to avoid pulling <chrono> into every TU that includes move.h;
// we only need the three extern globals used in the inlines below.
extern bool     g_moveReconEnabled;
extern uint64_t g_moveRecon_getPathlock_calls;
extern uint64_t g_moveRecon_setPathlock_calls;
extern uint64_t g_moveRecon_offmap_noops;

//***************************************************************************

#define	USE_FORESTS			0

inline constexpr int MAX_GLOBALMAP_DOORS = 32766;
inline constexpr int MAX_GLOBALMAP_AREAS = 30000;

inline constexpr int NUM_EXTRA_DOOR_LINKS = 2;

//#define	INNER_SPHERE	TEAM1
//#define	CLANS			TEAM2
//#define	ALLIED			TEAM3
//#define NEUTRAL			0xff		//NEUTRALS are NOT part of the eight Teams!
									//These are inanimate things like buildings, trees, rocks, etc.

inline constexpr int SIMPLE_PATHMAP = 0;
inline constexpr int SECTOR_PATHMAP = 1;

enum GlobalMapDoorOffset {
	DOOR_OFFSET_START,
	DOOR_OFFSET_GOAL,
	NUM_DOOR_OFFSETS
};

inline constexpr int SECTOR_DIM = 30;

inline constexpr int MOVEMAP_WIDTH = 12;			// Used outside of move file
inline constexpr int MOVEMAP_MAX_CELLOBJECTS = 4;

inline constexpr int TILE_PRESERVED_FLAG = 0x8000;

inline constexpr int NUM_DIRECTIONS = 8;
inline constexpr unsigned int HPRIME_NOT_CALCED = 0xFFFFFFFF;

inline constexpr int MOVEFLAG_OPEN = 1;
inline constexpr int MOVEFLAG_CLOSED = 2;
inline constexpr int MOVEFLAG_STEP = 4;
inline constexpr int MOVEFLAG_GOAL = 8;
inline constexpr int MOVEFLAG_MOVER_HERE = 16;
inline constexpr int MOVEFLAG_GOAL_BLOCKED = 32;
inline constexpr int MOVEFLAG_OFFMAP = 64;

inline constexpr int CELLSTATE_OPEN = 0;
inline constexpr int CELLSTATE_BLOCKED = 1;

inline constexpr int FOREST_INCREMENT = 2;

//----------------
// cell move costs
inline constexpr int COST_CLEAR = 100;
inline constexpr int COST_BLOCKED = 10000;

inline constexpr int OVERLAYINDEX_NULL = 41;
inline constexpr int IS_DIRTROAD_OVERLAY = 0;
inline constexpr int IS_PAVEDROAD_OVERLAY = 1;
inline constexpr int IS_RAILROAD_OVERLAY = 2;

inline constexpr int SCENARIOMAP_MAX_ELEVATION = 63;
inline constexpr int SCENARIOMAP_MAX_TILETYPE = 127;
inline constexpr int SCENARIOMAP_MAX_OVERLAYTYPE = 127;
//#define	SCENARIOMAP_LAST_BASETYPE_INDEX		63

//TileType and OverlayType now in terrain.h!
enum class OverlayWeightClass {
	OVERLAY_WEIGHT_VEHICLE,
	OVERLAY_WEIGHT_MECH,
	OVERLAY_WEIGHT_OTHER1,
	OVERLAY_WEIGHT_OTHER2,
	OVERLAY_WEIGHT_OTHER3,
	NUM_OVERLAY_WEIGHT_CLASSES
};
using enum OverlayWeightClass;

//------------------
// class Definitions
inline constexpr int MOVEPARAM_NONE = 0;
inline constexpr int MOVEPARAM_FACE_TARGET = 1;
inline constexpr int MOVEPARAM_DIR_OFFSET = 2;
inline constexpr int MOVEPARAM_BACK_UP = 4;
inline constexpr int MOVEPARAM_SOMEWHERE_ELSE = 8;
inline constexpr int MOVEPARAM_MOVING_MOVERS = 16;
inline constexpr int MOVEPARAM_MYSTERY_PARAM = 32;
inline constexpr int MOVEPARAM_STATIONARY_MOVERS = 64;
inline constexpr int MOVEPARAM_AVOID_PATHLOCKS = 128;
inline constexpr int MOVEPARAM_INIT = 256;
inline constexpr int MOVEPARAM_RECALC = 512;
inline constexpr int MOVEPARAM_STEP_TOWARD_TARGET = 1024;
inline constexpr int MOVEPARAM_PLAYER = 2048;
inline constexpr int MOVEPARAM_RADIO_RESULT = 4096;
inline constexpr int MOVEPARAM_ESCAPE_TILE = 8192;
inline constexpr int MOVEPARAM_STEP_ADJACENT_TARGET = 16384;
inline constexpr int MOVEPARAM_SWEEP_MINES = 32768;
inline constexpr int MOVEPARAM_FOLLOW_ROADS = 65536;
inline constexpr int MOVEPARAM_WATER_SHALLOW = 131072;
inline constexpr int MOVEPARAM_WATER_DEEP = 262144;
inline constexpr int MOVEPARAM_RANDOM_OPTIMAL = 524288;
inline constexpr int MOVEPARAM_JUMP = 1048576;

inline constexpr int TACORDER_PARAM_NONE = 0;
inline constexpr auto TACORDER_PARAM_RUN = (1<<0);
inline constexpr auto TACORDER_PARAM_WAIT = (1<<1);
inline constexpr auto TACORDER_PARAM_FACE_OBJECT = (1<<2);
inline constexpr auto TACORDER_PARAM_LAY_MINES = (1<<3);
inline constexpr auto TACORDER_PARAM_PURSUE = (1<<4);
inline constexpr auto TACORDER_PARAM_OBLITERATE = (1<<5);
inline constexpr auto TACORDER_PARAM_ESCAPE_TILE = (1<<6);
inline constexpr auto TACORDER_PARAM_SCAN = (1<<7);
inline constexpr auto TACORDER_PARAM_ATTACK_DFA = (1<<8);
inline constexpr auto TACORDER_PARAM_ATTACK_RAMMING = (1<<9);
inline constexpr auto TACORDER_PARAM_RANGE_RAMMING = (1<<10);
inline constexpr auto TACORDER_PARAM_RANGE_LONGEST = (1<<11);
inline constexpr auto TACORDER_PARAM_RANGE_OPTIMAL = (1<<12);
inline constexpr auto TACORDER_PARAM_RANGE_SHORT = (1<<13);
inline constexpr auto TACORDER_PARAM_RANGE_MEDIUM = (1<<14);
inline constexpr auto TACORDER_PARAM_RANGE_LONG = (1<<15);
inline constexpr auto TACORDER_PARAM_AIM_HEAD = (1<<16);
inline constexpr auto TACORDER_PARAM_AIM_LEG = (1<<17);
inline constexpr auto TACORDER_PARAM_AIM_ARM = (1<<18);
inline constexpr auto TACORDER_PARAM_DONT_SET_ORDER = (1<<19);
inline constexpr auto TACORDER_PARAM_JUMP = (1<<20);
inline constexpr auto TACORDER_PARAM_DONT_KEEP_MOVING = (1<<21);
inline constexpr auto TACORDER_PARAM_TACTIC_FLANK_RIGHT = (1<<22); //4194304
inline constexpr auto TACORDER_PARAM_TACTIC_FLANK_LEFT = (1<<23); //8388608
inline constexpr auto TACORDER_PARAM_TACTIC_FLANK_REAR = (1<<24); //16777216
inline constexpr auto TACORDER_PARAM_TACTIC_STOP_AND_FIRE = (1<<25); //33554432
inline constexpr auto TACORDER_PARAM_TACTIC_TURRET = (1<<26); //67108864
inline constexpr auto TACORDER_PARAM_TACTIC_JOUST = (1<<27); //134217728

inline constexpr int TACORDER_ATTACK_MASK = 0x00000300;
inline constexpr int TACORDER_RANGE_MASK = 0x0000FC00;
inline constexpr int TACORDER_AIM_MASK = 0x00070000;
inline constexpr int TACORDER_TACTIC_MASK = 0x0FC00000;

inline constexpr int RELPOS_FLAG_ABS = 1;
inline constexpr int RELPOS_FLAG_PASSABLE_START = 2;
inline constexpr int RELPOS_FLAG_PASSABLE_GOAL = 4;

// Runtime routes must span both axes of the largest blank map. Save records
// retain their original 50-step layout; longer live routes are replanned on load.
inline constexpr int MAX_SAVED_GLOBAL_PATH = 50;
inline constexpr int MAX_GLOBAL_PATH = 256;
static_assert(2 * (MAX_MAP_CELL_WIDTH / SECTOR_DIM) + 2 <= MAX_GLOBAL_PATH);

inline constexpr int NUM_MOVE_LEVELS = 2;

inline constexpr int MAX_WALL_OBJECTS = 2000;
inline constexpr int MAX_GATES_OBJECT = 1000;
inline constexpr int MAX_CELL_COORDS = 5000;
inline constexpr int FILL_STACK_SIZE = 810000;


//***************************************************************************


#pragma pack(1)

//---------------------------------------------------------------------------

inline constexpr int CELL_INFO_FLAG_PASSABLE = 1;
inline constexpr int CELL_INFO_FLAG_LOS = 2;
inline constexpr int CELL_INFO_FLAG_WATER_DEEP = 4;
inline constexpr int CELL_INFO_FLAG_WATER_SHALLOW = 8;

inline constexpr int SPECIAL_NONE = 0;
inline constexpr int SPECIAL_WALL = 1;
inline constexpr int SPECIAL_GATE = 2;
inline constexpr int SPECIAL_LAND_BRIDGE = 3;
inline constexpr int SPECIAL_FOREST = 4;

inline constexpr int MINE_NONE = 0;
inline constexpr int MINE_INTACT = 1;
inline constexpr int MINE_EXPLODED = 2;

struct _ScenarioMapCellInfo {
	unsigned char		terrain;
	unsigned char		overlay;
	bool				road;
	bool				gate;
	bool				forest;
	char				mine;
	unsigned char		specialType;
	short				specialID;
	bool				passable;
	int                 lineOfSight;
}; using MissionMapCellInfo = _ScenarioMapCellInfo;

//---------------------------------------------------------------------------

inline constexpr int MAPCELL_TERRAIN_SHIFT = 0;
inline constexpr int MAPCELL_TERRAIN_MASK = 0x0000000F;

inline constexpr int MAPCELL_OVERLAY_SHIFT = 4;
inline constexpr int MAPCELL_OVERLAY_MASK = 0x00000030;

inline constexpr int MAPCELL_MOVER_SHIFT = 6;
inline constexpr int MAPCELL_MOVER_MASK = 0x00000040;

inline constexpr int MAPCELL_UNUSED1_SHIFT = 7;				// THIS BIT AVAILABLE!
inline constexpr int MAPCELL_UNUSED1_MASK = 0x00000080;

inline constexpr int MAPCELL_GATE_SHIFT = 8;
inline constexpr int MAPCELL_GATE_MASK = 0x00000100;

inline constexpr int MAPCELL_OFFMAP_SHIFT = 9;
inline constexpr int MAPCELL_OFFMAP_MASK = 0x00000200;

inline constexpr int MAPCELL_PASSABLE_SHIFT = 10;
inline constexpr int MAPCELL_PASSABLE_MASK = 0x00000400;

inline constexpr int MAPCELL_PATHLOCK_SHIFT = 11;
inline constexpr int MAPCELL_PATHLOCK_MASK = 0x00001800;
inline constexpr int MAPCELL_PATHLOCK_BASE = 0x00000800;

inline constexpr int MAPCELL_MINE_SHIFT = 13;
inline constexpr int MAPCELL_MINE_MASK = 0x0001E000;

inline constexpr int MAPCELL_PRESERVED_SHIFT = 17;
inline constexpr int MAPCELL_PRESERVED_MASK = 0x00020000;

inline constexpr int MAPCELL_HEIGHT_SHIFT = 18;
inline constexpr int MAPCELL_HEIGHT_MASK = 0x003C0000;

inline constexpr int MAPCELL_DEBUG_SHIFT = 22;
inline constexpr int MAPCELL_DEBUG_MASK = 0x00C00000;

inline constexpr int MAPCELL_WALL_SHIFT = 24;
inline constexpr int MAPCELL_WALL_MASK = 0x01000000;

inline constexpr int MAPCELL_ROAD_SHIFT = 25;
inline constexpr int MAPCELL_ROAD_MASK = 0x02000000;

inline constexpr int MAPCELL_SHALLOW_SHIFT = 26;
inline constexpr int MAPCELL_SHALLOW_MASK = 0x04000000;

inline constexpr int MAPCELL_DEEP_SHIFT = 27;
inline constexpr int MAPCELL_DEEP_MASK = 0x08000000;

inline constexpr int MAPCELL_FOREST_SHIFT = 28;
inline constexpr int MAPCELL_FOREST_MASK = 0x10000000;

//------------------------------------------------------
// The following are used ONLY when building map data...
inline constexpr int MAPCELL_BUILD_WALL_SHIFT = 29;
inline constexpr int MAPCELL_BUILD_WALL_MASK = 0x20000000;

inline constexpr int MAPCELL_BUILD_GATE_SHIFT = 30;
inline constexpr int MAPCELL_BUILD_GATE_MASK = 0x40000000;

inline constexpr int MAPCELL_BUILD_LAND_BRIDGE_SHIFT = 11;
inline constexpr int MAPCELL_BUILD_LAND_BRIDGE_MASK = 0x00000800;

inline constexpr int MAPCELL_BUILD_SPECIAL_MASK = 0x60000800;

inline constexpr int MAPCELL_BUILD_NOT_SET_SHIFT = 31;
inline constexpr unsigned int MAPCELL_BUILD_NOT_SET_MASK = 0x80000000;

struct _MapCell {
	unsigned int data;
	void serialize(mc2::save::Archive& a) { a(data); }

	unsigned int getTerrain (void) {
		return((data & MAPCELL_TERRAIN_MASK) >> MAPCELL_TERRAIN_SHIFT);
	}

	void setTerrain (unsigned int terrain) {
		data &= (MAPCELL_TERRAIN_MASK ^ 0xFFFFFFFF);
		data |= (terrain << MAPCELL_TERRAIN_SHIFT);
	}

	unsigned int getOverlay (void) {
		return((data & MAPCELL_OVERLAY_MASK) >> MAPCELL_OVERLAY_SHIFT);
	}

	void setOverlay (unsigned int overlay) {
		data &= (MAPCELL_OVERLAY_MASK ^ 0xFFFFFFFF);
		data |= (overlay << MAPCELL_OVERLAY_SHIFT);
	}

	bool getMover (void) {
		return((data & MAPCELL_MOVER_MASK) ? true : false);
	}

	void setMover (bool moverHere ) {
		data &= (MAPCELL_MOVER_MASK ^ 0xFFFFFFFF);
		if (moverHere)
			data |= MAPCELL_MOVER_MASK;
	}

	unsigned int getGate (void) {
		return((data & MAPCELL_GATE_MASK) >> MAPCELL_GATE_SHIFT);
	}

	void setGate (unsigned int gate) {
		data &= (MAPCELL_GATE_MASK ^ 0xFFFFFFFF);
		data |= (gate << MAPCELL_GATE_SHIFT);
	}
	
	bool getPassable (void) {
		return((data & MAPCELL_PASSABLE_MASK) ? true : false);
	}

	void setPassable (bool passable) {
		data &= (MAPCELL_PASSABLE_MASK ^ 0xFFFFFFFF);
		if (passable)
			data |= MAPCELL_PASSABLE_MASK;
	}

	bool getPathlock (DWORD level) {
		return((data & (MAPCELL_PATHLOCK_BASE << level)) ? true : false);
	}

	void setPathlock (DWORD level, bool pathlock) {
		DWORD bit = MAPCELL_PATHLOCK_BASE << level;
		data &= (bit ^ 0xFFFFFFFF);
		if (pathlock)
			data |= bit;
	}

	unsigned int getMine (void) {
		return((data & MAPCELL_MINE_MASK) >> MAPCELL_MINE_SHIFT);
	}

	void setMine (unsigned int mine) {
		data &= (MAPCELL_MINE_MASK ^ 0xFFFFFFFF);
		data |= (mine << MAPCELL_MINE_SHIFT);
	}

	bool getPreserved (void) {
		return((data & MAPCELL_PRESERVED_MASK) ? true : false);
	}

	void setPreserved (bool preserved) {
		data &= (MAPCELL_PRESERVED_MASK ^ 0xFFFFFFFF);
		if (preserved)
			data |= MAPCELL_PRESERVED_MASK;
	}

	void setLocalHeight (DWORD localElevation) {
		data &= (MAPCELL_HEIGHT_MASK ^ 0xFFFFFFFF);
		data |= (localElevation << MAPCELL_HEIGHT_SHIFT);
	}
	
	DWORD getLocalHeight (void) {
		return((data & MAPCELL_HEIGHT_MASK) >> MAPCELL_HEIGHT_SHIFT);
	}

	void setDebug (DWORD value) {
		data &= (MAPCELL_DEBUG_MASK ^ 0xFFFFFFFF);
		data |= (value << MAPCELL_DEBUG_SHIFT);
	}

	DWORD getDebug (void) {
		return((data & MAPCELL_DEBUG_MASK) >> MAPCELL_DEBUG_SHIFT);
	}

	bool getWall (void) {
		return((data & MAPCELL_WALL_MASK) ? true : false);
	}

	void setWall (bool wallHere) {
		data &= (MAPCELL_WALL_MASK ^ 0xFFFFFFFF);
		if (wallHere)
			data |= MAPCELL_WALL_MASK;
	}

	bool getRoad (void) {
		return((data & MAPCELL_ROAD_MASK) ? true : false);
	}

	void setRoad (bool roadHere) {
		data &= (MAPCELL_ROAD_MASK ^ 0xFFFFFFFF);
		if (roadHere)
			data |= MAPCELL_ROAD_MASK;
	}

	bool getShallowWater (void) {
		return((data & MAPCELL_SHALLOW_MASK) ? true : false);
	}

	void setShallowWater (bool shallowWaterHere) {
		data &= (MAPCELL_SHALLOW_MASK ^ 0xFFFFFFFF);
		if (shallowWaterHere)
			data |= MAPCELL_SHALLOW_MASK;
	}

	bool getDeepWater (void) {
		return((data & MAPCELL_DEEP_MASK) ? true : false);
	}

	void setDeepWater (bool deepWaterHere) {
		data &= (MAPCELL_DEEP_MASK ^ 0xFFFFFFFF);
		if (deepWaterHere)
			data |= MAPCELL_DEEP_MASK;
	}

	bool getBuildGate (void) {
		return((data & MAPCELL_BUILD_GATE_MASK) ? true : false);
	}

	void setBuildGate (bool set) {
		data &= (MAPCELL_BUILD_GATE_MASK ^ 0xFFFFFFFF);
		if (set)
			data |= MAPCELL_BUILD_GATE_MASK;
	}

	bool getBuildWall (void) {
		return((data & MAPCELL_BUILD_WALL_MASK) ? true : false);
	}

	void setBuildWall (bool set) {
		data &= (MAPCELL_BUILD_WALL_MASK ^ 0xFFFFFFFF);
		if (set)
			data |= MAPCELL_BUILD_WALL_MASK;
	}

	bool getBuildLandBridge (void) {
		return((data & MAPCELL_BUILD_LAND_BRIDGE_MASK) ? true : false);
	}

	void setBuildLandBridge (bool set) {
		data &= (MAPCELL_BUILD_LAND_BRIDGE_MASK ^ 0xFFFFFFFF);
		if (set)
			data |= MAPCELL_BUILD_LAND_BRIDGE_MASK;
	}

	bool getForest (void) {
		return((data & MAPCELL_FOREST_MASK) ? true : false);
	}

	void setForest (bool set) {
		data &= (MAPCELL_FOREST_MASK ^ 0xFFFFFFFF);
		if (set)
			data |= MAPCELL_FOREST_MASK;
	}

	bool getOffMap (void) {
		return((data & MAPCELL_OFFMAP_MASK) ? true : false);
	}

	void setOffMap (bool set) {
		data &= (MAPCELL_OFFMAP_MASK ^ 0xFFFFFFFF);
		if (set)
			data |= MAPCELL_OFFMAP_MASK;
	}

	bool getBuildSpecial (void) {
		return((data & MAPCELL_BUILD_SPECIAL_MASK) ? true : false);
	}

	bool getBuildNotSet (void) {
		return((data & MAPCELL_BUILD_NOT_SET_MASK) ? true : false);
	}

	void setBuildNotSet (bool set) {
		data &= (MAPCELL_BUILD_NOT_SET_MASK ^ 0xFFFFFFFF);
		if (set)
			data |= MAPCELL_BUILD_NOT_SET_MASK;
	}

}; using MapCell = _MapCell;

using MapCellPtr = MapCell*;

struct _PreservedCell {
	short				row;
	short				col;
	unsigned int        data;
}; using PreservedCell = _PreservedCell;

using PreservedCellPtr = PreservedCell*;

#pragma pack()

//---------------------------------------------------------------------------

extern float VerticesMapSideDivTwo;
extern float MetersMapSideDivTwo;

inline constexpr int MAX_DEBUG_CELLS = 1000;

// MOVE layer revisions. Static = terrain/overlay/passable/local height (changes at load or editing only). The overlays change
// during play: gate, mine, pathlock, mover occupancy, preserved. Every MissionMap setter bumps its layer's counter, so a path
// solve (M3 speculative solve) records the counters it read and is valid at commit only while they match.
struct MoveLayerRevisions {
	uint32_t statics = 0;
	uint32_t gate = 0;
	uint32_t mine = 0;
	uint32_t pathlock = 0;
	uint32_t mover = 0;
	uint32_t preserved = 0;
	bool operator== (const MoveLayerRevisions&) const = default;
};

class MissionMap {

	public:

		// The cell grid, height x width, row-major; empty until init(h, w). Saved and loaded as one packet of raw cells.
		std::vector<MapCell>	map;
        // those are read from packet file so cannot be longs on 64bit by definition
		int32_t             height;
		int32_t             width;
		int32_t             planet;
		bool				preserveCells;
		long				numPreservedCells;
		PreservedCell		preservedCells[MAX_MOVERS];
		long				numDebugCells;
		long				debugCells[MAX_DEBUG_CELLS][3];

		void				(*placeMoversCallback) (void);

		MoveLayerRevisions	rev;

		// Per-tile count of cells (within the MAPCELL_DIM x MAPCELL_DIM tile) whose
		// mine field is non-zero. Maintained incrementally by setMine() and rebuilt
		// from cell contents at the end of init(PacketFile*). Lets the per-frame
		// terrain quad loop skip the inner cell scan for mine-free tiles.
		std::vector<unsigned char>	tileMineCount;
		long                tileMineCountWidth;  // = width / MAPCELL_DIM (cached)

	public:


		void init (void) {
			map.clear();
			height = 0;
			width = 0;
            planet = 0;
			preserveCells = false;
			numPreservedCells = 0;
			placeMoversCallback = nullptr;
			numDebugCells = 0;
			tileMineCount.clear();
			tileMineCountWidth = 0;
		}
		
		MissionMap (void) {
			init();
		}
		
		void destroy (void);

		~MissionMap (void) {
			destroy();
		}

		void init (long h, long w);

		long init (long curHeight, long curWidth, long curPlanet, MissionMapCellInfo* mapData);

		void setPassable (long row, long col, char* footPrint, bool passable);

		long init (PacketFile* packetFile, long whichPacket = 0, bool taggedCells = false);

		long initOld (File* mapFile);

		long write (PacketFile* packetFile, long whichPacket = 0, bool taggedCells = false);

		// OOB guards (cd071ca9 pattern): worldToCell (terrain.h) does NOT clamp,
		// and on oversized maps the MOVE/cell grid (width x height) is smaller than
		// the terrain grid, so far-off-map cursor positions yield OOB row/col.
		// Every map[row*width+col] accessor must inBounds-guard or it READ-AVs
		// (e.g. getTerrain via printDebugInfo:3493). Getters return a benign
		// default off-map; setters no-op off-map.
		unsigned long getTerrain (long row, long col) {
			if (!inBounds(row, col)) return(0);
			return(map[row * width + col].getTerrain());
		}

		void setTerrain (long row, long col, unsigned long terrain) {
			if (!inBounds(row, col)) return;
			map[row * width + col].setTerrain(terrain);
			rev.statics++;
		}

		unsigned long getOverlay (long row, long col) {
			if (!inBounds(row, col)) return(0);
			return(map[row * width + col].getOverlay());
		}

		void setOverlay (long row, long col, unsigned long overlay) {
			if (!inBounds(row, col)) return;
			map[row * width + col].setOverlay(overlay);
			rev.statics++;
		}

		bool getMover (long row, long col) {
			if (!inBounds(row, col)) return(false);
			return(map[row * width + col].getMover());
		}

		void setMover (long row, long col, bool moverHere) {
			if (!inBounds(row, col)) return;
			map[row * width + col].setMover(moverHere);
			rev.mover++;
		}

		unsigned long getGate (long row, long col) {
			if (!inBounds(row, col)) return(0);
			return(map[row * width + col].getGate());
		}

		void setGate (long row, long col, unsigned long gate) {
			if (!inBounds(row, col)) return;
			map[row * width + col].setGate(gate);
			rev.gate++;
		}

		bool getPassable (long row, long col) {
			// Treat off-map as impassable instead of indexing map[] OOB.
			if (!inBounds(row, col))
				return(false);
			return(map[row * width + col].getPassable());
		}

		void setPassable (long row, long col, bool passable) {
			if (!inBounds(row, col)) return;
			map[row * width + col].setPassable(passable);
			rev.statics++;
		}

		bool getPassable (glm::vec3 cellPosition);

		bool getPathlock (long level, long row, long col) {
			if (!inBounds(row, col)) {
				if (g_moveReconEnabled) g_moveRecon_offmap_noops++;
				return(false);
			}
			if (g_moveReconEnabled) g_moveRecon_getPathlock_calls++;
			return(map[row * width + col].getPathlock(level));
		}

		void setPathlock (long level, long row, long col, bool pathlock) {
			// Oversized maps have a MOVE grid smaller than the terrain grid, so a
			// mover's cellPositionRow/Col can exceed width/height -> map[] OOB
			// READ-AV (mover.cpp:5327, Mover::updatePathLock on 1kbasicmap).
			if (!inBounds(row, col)) {
				if (g_moveReconEnabled) g_moveRecon_offmap_noops++;
				return;
			}
			if (g_moveReconEnabled) g_moveRecon_setPathlock_calls++;
			map[row * width + col].setPathlock(level, pathlock);
			rev.pathlock++;
		}

		unsigned long getMine (long row, long col) {
			if (!inBounds(row, col)) return(0);
			return(map[row * width + col].getMine());
		}

		void setMine (long row, long col, unsigned long mine) {
			if (!inBounds(row, col)) return;
			MapCellPtr cell = &map[row * width + col];
			const unsigned long oldMine = cell->getMine();
			cell->setMine(mine);
			rev.mine++;
			// Maintain per-tile mine count: only cells transitioning between zero
			// and non-zero affect the count. Updates between two non-zero values
			// (1 <-> 2 = mine <-> blown) leave the count unchanged.
			if (!tileMineCount.empty() && ((oldMine == 0) != (mine == 0))) {
				const long tileIdx = (row / MAPCELL_DIM) * tileMineCountWidth + (col / MAPCELL_DIM);
				if (mine == 0) --tileMineCount[tileIdx];
				else           ++tileMineCount[tileIdx];
			}
			// PR2c Stage 1c — mark MineStaticVBO dirty on any cell mutation
			// (forward-declared to avoid pulling gos_terrain_indirect.h into
			// this widely-included header). Idempotent — chain explosions
			// debounce to one rebuild/frame via dirty-flag.
			extern void gos_terrain_indirect_MarkMineDirty();
			gos_terrain_indirect_MarkMineDirty();
		}

		// Fast per-frame read used by terrain quad mine/scorch reservation. Returns
		// true if any cell in the (tileR, tileC) tile has a non-zero mine value.
		// Pre-rebuild (during init transient), returns true conservatively so callers
		// fall back to the per-cell scan.
		bool tileHasMines (long tileR, long tileC) const {
			if (tileMineCount.empty()) return true;
			if (tileR < 0 || tileC < 0) return false;
			if (tileR >= (height / MAPCELL_DIM) || tileC >= tileMineCountWidth) return false;
			return tileMineCount[tileR * tileMineCountWidth + tileC] > 0;
		}

		// Rebuilds tileMineCount from current cell contents. Called after a packet
		// readPacket() that writes cells directly into map memory, bypassing setMine().
		void rebuildTileMineCounts (void);

		bool getPreserved (long row, long col) {
			if (!inBounds(row, col)) return(false);
			return(map[row * width + col].getPreserved());
		}

		void setPreserved (long row, long col, bool preserved) {
			if (!inBounds(row, col)) return;
			map[row * width + col].setPreserved(preserved);
			rev.preserved++;
		}

		void setLocalHeight (long row, long col, DWORD localElevation) {
			if (!inBounds(row, col)) return;
			map[row * width + col].setLocalHeight(localElevation);
			rev.statics++;
		}

		DWORD getLocalHeight (long row, long col) {
			if (!inBounds(row, col)) return(0);
			return(map[row * width + col].getLocalHeight());
		}

		void setCellDebug (long row, long col, DWORD value, DWORD level) {
			if( row >= 0 && row < height && col >= 0 && col < width )
			{
				DWORD curValue = map[row * width + col].getDebug();
				if (curValue && (curValue != value)) {
					for (long i = 0; i < numDebugCells; i++)
						if (debugCells[i][0] == row)
							if (debugCells[i][1] == col) {
								if (debugCells[i][2] < level)
									break;
								return;
							}
				}
				if (value == 0) {
					// remove it...
					for (long i = 0; i < numDebugCells; i++)
						if (debugCells[i][0] == row)
							if (debugCells[i][1] == col) {
								numDebugCells--;
								debugCells[i][0] = debugCells[numDebugCells][0];
								debugCells[i][1] = debugCells[numDebugCells][1];
								debugCells[i][2] = debugCells[numDebugCells][2];
								break;
							}
					map[row * width + col].setDebug(0);
					}
				else {
					if (numDebugCells < MAX_DEBUG_CELLS) {
						debugCells[numDebugCells][0] = row;
						debugCells[numDebugCells][1] = col;
						debugCells[numDebugCells][2] = level;
						numDebugCells++;
						map[row * width + col].setDebug(value);
					}
				}
			}
		}

		void clearCellDebugs (void) {
			for (long i = 0; i < numDebugCells; i++)
				map[debugCells[i][0] * width + debugCells[i][1]].setDebug(0);
			numDebugCells = 0;
		}

		void clearCellDebugs (long level) {
			long i = 0;
			while (i < numDebugCells) {
				if (debugCells[i][2] == level) {
					numDebugCells--;
					map[debugCells[i][0] * width + debugCells[i][1]].setDebug(0);
					debugCells[i][0] = debugCells[numDebugCells][0];
					debugCells[i][1] = debugCells[numDebugCells][1];
					debugCells[i][2] = debugCells[numDebugCells][2];
					}
				else
					i++;
			}
		}

		DWORD getCellDebug (long row, long col) {
			if (!inBounds(row, col)) return(0);
			return(map[row * width + col].getDebug());
		}

		bool getWall (long row, long col) {
			if (!inBounds(row, col)) return(false);
			return(map[row * width + col].getWall());
		}

		void setWall (long row, long col, bool wallHere) {
			if (!inBounds(row, col)) return;
			map[row * width + col].setWall(wallHere);
			rev.statics++;
		}

		bool getRoad (long row, long col) {
			if (!inBounds(row, col)) return(false);
			return(map[row * width + col].getRoad());
		}

		void setRoad (long row, long col, bool roadHere) {
			if (!inBounds(row, col)) return;
			map[row * width + col].setRoad(roadHere);
			rev.statics++;
		}

		bool getShallowWater (long row, long col) {
			if (!inBounds(row, col)) return(false);
			return(map[row * width + col].getShallowWater());
		}

		void setShallowWater (long row, long col, bool shallowWaterHere) {
			if (!inBounds(row, col)) return;
			map[row * width + col].setShallowWater(shallowWaterHere);
			rev.statics++;
		}

		bool getDeepWater (long row, long col) {
			if (!inBounds(row, col)) return(false);
			return(map[row * width + col].getDeepWater());
		}

		void setDeepWater (long row, long col, bool deepWaterHere) {
			if (!inBounds(row, col)) return;
			map[row * width + col].setDeepWater(deepWaterHere);
			rev.statics++;
		}

		bool getBuildGate (long row, long col) {
			if (!inBounds(row, col)) return(false);
			return(map[row * width + col].getBuildGate());
		}

		void setBuildGate (long row, long col, bool set) {
			if (!inBounds(row, col)) return;
			map[row * width + col].setBuildGate(set);
			rev.statics++;
		}

		bool getBuildWall (long row, long col) {
			if (!inBounds(row, col)) return(false);
			return(map[row * width + col].getBuildWall());
		}

		void setBuildWall (long row, long col, bool set) {
			if (!inBounds(row, col)) return;
			map[row * width + col].setBuildWall(set);
			rev.statics++;
		}

		bool getBuildLandBridge (long row, long col) {
			if (!inBounds(row, col)) return(false);
			return(map[row * width + col].getBuildLandBridge());
		}

		void setBuildLandBridge (long row, long col, bool set) {
			if (!inBounds(row, col)) return;
			map[row * width + col].setBuildLandBridge(set);
			rev.statics++;
		}

		bool getForest (long row, long col) {
			if (!inBounds(row, col)) return(false);
			return(map[row * width + col].getForest());
		}

		void setForest (long row, long col, bool set) {
			if (!inBounds(row, col)) return;
			map[row * width + col].setForest(set);
			rev.statics++;
		}

		bool getOffMap (long row, long col) {
			// Off the grid IS off-map -> true is the correct answer here.
			if (!inBounds(row, col)) return(true);
			return(map[row * width + col].getOffMap());
		}

		void setOffMap (long row, long col, bool set) {
			if (!inBounds(row, col)) return;
			map[row * width + col].setOffMap(set);
			rev.statics++;
		}

		bool getBuildSpecial (long row, long col) {
			if (!inBounds(row, col)) return(false);
			return(map[row * width + col].getBuildSpecial());
		}

		bool getBuildNotSet (long row, long col) {
			if (!inBounds(row, col)) return(false);
			return(map[row * width + col].getBuildNotSet());
		}

		void setBuildNotSet (long row, long col, bool set) {
			if (!inBounds(row, col)) return;
			map[row * width + col].setBuildNotSet(set);
			rev.statics++;
		}

		long getHeight (void) {
			return(height);
		}
		
		long getWidth (void) {
			return(width);
		}

		bool inBounds (long row, long col) {
			return((row > -1) && (row < height) && (col > -1) && (col < width));
		}

		MapCellPtr getCell (long row, long col) {
			gosASSERT(inBounds(row, col));
			return(&map[row * width + col]);
		}

		void preserveCell (long row, long col) {
			if (!inBounds(row, col)) return;
			preservedCells[numPreservedCells].data = map[row * width + col].data;
			preservedCells[numPreservedCells].row = row;
			preservedCells[numPreservedCells].col = col;
			map[row * width + col].setPreserved(1);
			rev.preserved++;
			numPreservedCells++;
		}

		long getLOF (glm::vec3 position);

		unsigned long cellPassable (glm::vec3 cellPosition);

		void lineOfSensor (glm::vec3 start, glm::vec3 target, long& numBlockingTiles, long& numBlockingObjects);

		long getOverlayWeight (long row, long col, long moverOverlayWeightClass, long moverRelation);

		void spreadState (long r, long c, long radius);

		long placeObject (glm::vec3 position, float radius);

		void placeTerrainObject (long row,
								 long col,
								 long objectClass,
								 long objectTypeID,
								 __int64 footPrint,
								 bool blocksLineOfFire,
								 long mineType);

		void print (char* fileName);
};

using MissionMapPtr = MissionMap*;

//***************************************************************************


class MovePath;
using MovePathPtr = MovePath*;


//---------------------------------------------------------------------------
inline constexpr int MAX_STEPS_PER_MOVEPATH = 200;

struct _PathStep {
	short					cell[2];							// obvious
	float					distanceToGoal;						// dist, in meters, to goal from this step
	glm::vec3			destination;						// world pos of this step
	char					direction;							// 0 thru 7 direction into this step
	short					area;
}; using PathStep = _PathStep;

using PathStepPtr = PathStep*;

class MovePath {

	public:
	
		glm::vec3		goal;								// world pos of path goal
		glm::vec3		target;								// world pos of object target
		long				numSteps;							// if paused or no steps, == 0, else == numStepsWhenNotPaused
		long				numStepsWhenNotPaused;				// total number of steps
		long				curStep;							// cuurent step we're headed for
		long				cost;								// total cost of path
		PathStep			stepList[MAX_STEPS_PER_MOVEPATH];	// actual steps :)
		bool				marked;								// is it currently marked
		long				globalStep;							// if part of a complex path
		
	public:
	
		
		void init (void) {
			goal = glm::vec3(0.0f);
			numSteps = 0;
			numStepsWhenNotPaused = 0;
			curStep = 0;
			cost = 0;
			marked = false;
			globalStep = -1;
		}
		
		long init (long numberOfSteps);

		void clear (void);

		void setDirection (long stepNumber, char direction) {
			stepList[stepNumber].direction = direction;
		}

		long getDirection (long stepNumber) {
			return(stepList[stepNumber].direction);
		}

		void setDistanceToGoal (long stepNumber, float distance) {
			stepList[stepNumber].distanceToGoal = distance;
		}

		float getDistanceToGoal (long stepNumber) {
			return(stepList[stepNumber].distanceToGoal);
		}

		void setDestination (long stepNumber, glm::vec3	v) {
			stepList[stepNumber].destination = v;
		}

		glm::vec3 getDestination (long stepNumber) {
			return(stepList[stepNumber].destination);
		}

		void setCell (long stepNumber, long r, long c) {
			stepList[stepNumber].cell[0] = r;
			stepList[stepNumber].cell[1] = c;
		}

		float getDistanceLeft (glm::vec3 position, long stepNumber = -1);

		void lock (long level, long start, long range, bool setting);

		bool isLocked (long level, long start, long range, bool* reachedEnd = nullptr);

		bool isBlocked (long start, long range, bool* reachedEnd = nullptr);

		long crossesBridge (long start, long range);

		long crossesCell (long start, long range, long cellR, long cellC);

		long crossesClosedClanGate (long start, long range);

		long crossesClosedISGate (long start, long range);

		long crossesClosedGate (long start, long range);

		MovePath (void) {
			// Saves write this struct raw; zero it so padding (e.g. after 'marked') is deterministic.
			memset(static_cast<void*>(this), 0, sizeof(MovePath));
			init();
		}
		
		void destroy (void);

		~MovePath (void) {
			destroy();
		}

		void setCurStep (long _curStep) {
			curStep = _curStep;
		}

		long getNumSteps (void);

		long getCost (void) {
			return(cost);
		}
};

//---------------------------------------------------------------------------
// Save-record canonicalization. A MovePath is written to a save as a raw image, and copy/assignment is member-wise, so the padding bytes are
// not guaranteed to copy and a record loaded from an old save can carry nonzero padding. The save write point calls this on the staged copy.
// The static_asserts pin the layout the offsets below assume: a layout change fails the build here instead of changing the save format.
// (64-bit MSVC layout, like save_layout_guard.h.)
#if defined(_MSC_VER) && defined(_WIN64)
static_assert(sizeof(PathStep) == 24, "PathStep is written raw into saves");
static_assert(offsetof(PathStep, direction) == 20 && offsetof(PathStep, area) == 22, "PathStep padding byte sits at offset 21");
static_assert(offsetof(PathStep, direction) + sizeof(char) < offsetof(PathStep, area), "PathStep has one gap byte after direction");
static_assert(offsetof(PathStep, area) + sizeof(short) == sizeof(PathStep), "PathStep has no tail padding");
static_assert(offsetof(PathStep, distanceToGoal) == offsetof(PathStep, cell) + sizeof(short) * 2, "PathStep: no gap before distanceToGoal");
static_assert(offsetof(PathStep, destination) == offsetof(PathStep, distanceToGoal) + sizeof(float), "PathStep: no gap before destination");
static_assert(offsetof(PathStep, direction) == offsetof(PathStep, destination) + sizeof(glm::vec3), "PathStep: no gap before direction");
static_assert(sizeof(MovePath) == 4848, "MovePath is written raw into saves");
static_assert(offsetof(MovePath, stepList) == 40, "MovePath: stepList offset");
static_assert(offsetof(MovePath, stepList) == offsetof(MovePath, cost) + sizeof(long), "MovePath: no gap before stepList");
static_assert(offsetof(MovePath, marked) == offsetof(MovePath, stepList) + sizeof(PathStep) * MAX_STEPS_PER_MOVEPATH, "MovePath: no gap before marked");
static_assert(offsetof(MovePath, marked) == 4840 && offsetof(MovePath, globalStep) == 4844, "MovePath: 3 padding bytes follow marked");
static_assert(offsetof(MovePath, globalStep) + sizeof(long) == sizeof(MovePath), "MovePath has no tail padding");
#endif

inline void mc2CanonicalizeMovePathRecord (MovePath& record) {
	unsigned char* raw = reinterpret_cast<unsigned char*>(&record);
	for (int i = 0; i < MAX_STEPS_PER_MOVEPATH; i++) {
		unsigned char* step = raw + offsetof(MovePath, stepList) + sizeof(PathStep) * i;
		std::memset(step + offsetof(PathStep, direction) + sizeof(char), 0, offsetof(PathStep, area) - (offsetof(PathStep, direction) + sizeof(char)));
		std::memset(step + offsetof(PathStep, area) + sizeof(short), 0, sizeof(PathStep) - (offsetof(PathStep, area) + sizeof(short)));
	}
	std::memset(raw + offsetof(MovePath, marked) + sizeof(bool), 0, offsetof(MovePath, globalStep) - (offsetof(MovePath, marked) + sizeof(bool)));
	std::memset(raw + offsetof(MovePath, globalStep) + sizeof(long), 0, sizeof(MovePath) - (offsetof(MovePath, globalStep) + sizeof(long)));
}

//******************************************************************************************


//******************************************************************************************
inline constexpr int GLOBAL_FLAG_SPECIAL_IMPOSSIBLE = 0x80;
inline constexpr int GLOBAL_FLAG_SPECIAL_CALC = 0x40;
inline constexpr int GLOBAL_FLAG_NORMAL_OPENS = 0x20;
inline constexpr int GLOBAL_FLAG_NORMAL_CLOSES = 0x10;

inline constexpr int GLOBAL_CONFIDENCE_BAD = 0;
inline constexpr int GLOBAL_CONFIDENCE_AT_LEAST = 1;
inline constexpr int GLOBAL_CONFIDENCE_GOOD = 2;

inline constexpr int GLOBALPATH_EXISTS_UNKNOWN = 0;
inline constexpr int GLOBALPATH_EXISTS_TRUE = 1;
inline constexpr int GLOBALPATH_EXISTS_FALSE = 2;

#pragma pack(1)

struct _DoorLink {
	short					doorIndex;
	char					doorSide;
	int					cost;
	int					openCost;
}; using DoorLink = _DoorLink;

using DoorLinkPtr = DoorLink*;

struct _GlobalMapDoor {
	//----------------
	// Map layout data
	short					row;
	short					col;
	char					length;					// in cells
	bool					open;
	char					teamID;
	short					area[2];
	short					areaCost[2];
	char					direction[2];
	short					numLinks[2];
	//DoorLinkPtr				links[2];
	DWORD                   links_Legacy32bitPtr[2];
	//------------------
	// Pathfinding  data
	int					cost;
	int					parent;
	int					fromAreaIndex;
	unsigned int	    flags;
	int					g;
	int					hPrime;
	int					fPrime;
}; using GlobalMapDoor = _GlobalMapDoor;

using GlobalMapDoorPtr = GlobalMapDoor*;

struct _DoorInfo {
	short					doorIndex;
	char					doorSide;
}; using DoorInfo = _DoorInfo;

using DoorInfoPtr = DoorInfo*;

enum class _AreaType : int {
	AREA_TYPE_NORMAL,
	AREA_TYPE_WALL,
	AREA_TYPE_GATE,
	AREA_TYPE_LAND_BRIDGE,
	AREA_TYPE_FOREST,
	NUM_AREA_TYPES
}; using AreaType = _AreaType;
using enum _AreaType;

enum class _WaterType : int {
	WATER_TYPE_NONE,
	WATER_TYPE_SHALLOW,
	WATER_TYPE_DEEP,
	NUM_WATER_TYPES
}; using WaterType = _WaterType;
using enum _WaterType;

struct _GlobalMapArea {
	//----------------
	// Map layout data
	short					sectorR;
	short					sectorC;
	//DoorInfoPtr				doors;
	DWORD                   doors_Legacy32bitPtr;
	AreaType				type;
	short					numDoors;
	int					    ownerWID;
	char					teamID;
	bool					offMap;
	bool					open;
    // sebi: 64bit fix 
	//short*					cellsCovered;
	DWORD                   cellsCovered_Legacy32bitPtr;
}; using GlobalMapArea = _GlobalMapArea;

#pragma pack()

using GlobalMapAreaPtr = GlobalMapArea*;

struct _GlobalPathStep {
	int					startDoor;
	int					thruArea;
	int					goalDoor;
	glm::vec3			start;			// "start" in this area
	glm::vec3			goal;			// "goal" in this area
	int					goalCell[2];	// which cell did we actually exit thru
	int					costToGoal;
}; using GlobalPathStep = _GlobalPathStep;

using GlobalPathStepPtr = GlobalPathStep*;

inline constexpr int MAX_SPECIAL_AREAS = 1500;
inline constexpr int MAX_SPECIAL_SUB_AREAS = 25;
inline constexpr int MAX_CELLS_PER_SUB_AREA = 49;
inline constexpr int MAX_OFFMAP_AREAS = 60;
inline constexpr int MAX_GAME_OBJECT_CELLS = 64;

struct _GameObjectFootPrint {
	short				cellPositionRow;
	short				cellPositionCol;
	short				preNumCells;
	short				numCells;
	short				cells[MAX_GAME_OBJECT_CELLS][2];
}; using GameObjectFootPrint = _GameObjectFootPrint;

struct _GlobalSpecialAreaInfo {
	unsigned char			type;
	short					numSubAreas;
	short					subAreas[MAX_SPECIAL_SUB_AREAS];
	short					numCells;
	short					cells[MAX_CELLS_PER_SUB_AREA][2];
}; using GlobalSpecialAreaInfo = _GlobalSpecialAreaInfo;

using DoorInfoLinksPtr = DoorLinkPtr[2];
class GlobalMap {

	public:

		int						height;				// in cells
		int						width;				// in cells
		int						sectorDim;			// in cells
		int						sectorHeight;		// in sectors
		int						sectorWidth;		// in sectors
		int						numAreas;
		int						numDoors;
		int						numDoorInfos;
		int						numDoorLinks;

		short*						areaMap;
		GlobalMapAreaPtr			areas;
        // sebi: moved pointer out of a struct which is loaded from file
        // because pointer size is platform dependent (64 vs. 32bit)
	    short**					    areas_cellsCovered;
	    DoorInfoPtr*                areas_doors;

		GlobalMapDoorPtr			doors;
        DoorInfoLinksPtr*           doors_links;

		DoorInfoPtr					doorInfos;
		DoorLinkPtr					doorLinks;
		GlobalMapDoorPtr			doorBuildList;
        DoorInfoLinksPtr*           doorBuildList_links;
#ifdef USE_PATH_COST_TABLE
		unsigned char*				pathCostTable;
#endif
		unsigned char*				pathExistsTable;

		int						    numSpecialAreas;
		GlobalSpecialAreaInfo*		specialAreas;	// used when building data

		// The owners of the buffers the pointers above view, for the build and the pak load alike. Each is sized once and never
		// regrown while its views are held; destroy() drops the views with the stores.
		std::vector<short>				areaMapStore;
		std::vector<GlobalMapArea>		areasStore;
		std::vector<short*>				areasCellsCoveredStore;
		std::vector<DoorInfoPtr>		areasDoorsStore;		// each pointer views a run of doorInfosStore
		std::vector<GlobalMapDoor>		doorsStore;
		std::vector<DoorLinkPtr>		doorLinkViewStore;		// pairs: doors_links[d][side]; each pointer views a run of doorLinksStore
		std::vector<DoorInfo>			doorInfosStore;
		std::vector<DoorLink>			doorLinksStore;
		std::vector<GlobalMapDoor>		doorBuildStore;
		std::vector<GlobalSpecialAreaInfo> specialAreasStore;
		std::vector<unsigned char>		pathExistsStore;

		bool						closes;
		bool						opens;

		int						    goalArea;
		int						    goalSector[2];

		int						    startCell[2];
		int						    goalCell[2];

		bool						blank;
		bool						hover;
		bool						useClosedAreas;
		char						moverTeamID;
		bool						badLoad;
		bool						calcedPathCost;

		// The OPEN list of every calcPath on this map (see MoveMap::openHeap).
		mc2::OpenHeap				openHeap;

		int						    numOffMapAreas;
		short						offMapAreas[MAX_OFFMAP_AREAS];

		bool (*isGateDisabledCallback) (int objectWID);
		bool (*isGateOpenCallback) (int objectWID);

		static int					minRow;
		static int					maxRow;
		static int					minCol;
		static int					maxCol;

		static GameLogPtr			log;
		static bool					logEnabled;

	public:

		
		void init (void) {
			height = 0;
			width = 0;
			sectorDim = 30;
			sectorHeight = 0;
			sectorWidth = 0;

			numAreas = 0;
			areaMap = nullptr;
			areas = nullptr;
            areas_cellsCovered = nullptr;
            areas_doors = nullptr;

			numDoors = 0;
			numDoorInfos = 0;
			numDoorLinks = 0;
			doors = nullptr;
            doors_links = nullptr;
			doorInfos = nullptr;
			doorLinks = nullptr;
			doorBuildList = nullptr;
			doorBuildList_links = nullptr;

			goalSector[0] = goalSector[1] = 0;
			blank = false;
			hover = false;
			useClosedAreas = false;
			badLoad = false;
			calcedPathCost = false;

			startCell[0] = -1;
			startCell[1] = -1;
			goalCell[0] = -1;
			goalCell[1] = -1;

			specialAreas = nullptr;
			closes = false;
			opens = false;
			numOffMapAreas = 0;

			// Path tables -- must be zero-initialised here, not only inside
			// init(long,long). Carver5O-style content-format mismatch during
			// mission load can reach clearPathExistsTable() / setPathExists()
			// without ever running the sized init path; without this line the
			// field holds whatever the allocator returned (ASan reports the
			// MSVC /fsanitize=address `0xBE` stack/heap poison pattern) and
			// the null-guard in those methods passes because the garbage is
			// non-zero. See docs/asan-follow-ups.md finding #2.
			pathExistsTable = nullptr;
#ifdef USE_PATH_COST_TABLE
			pathCostTable = nullptr;
#endif

			log = nullptr;
			logEnabled = false;

			isGateDisabledCallback = nullptr;
			isGateOpenCallback = nullptr;
		}

		GlobalMap (void) {
			init();
		}

		// The pointer members are views into the stores, so a copy would alias the source's buffers.
		GlobalMap (const GlobalMap&) = delete;
		GlobalMap& operator= (const GlobalMap&) = delete;

		void destroy (void);

		// The helicopter map does not use the path table; this frees it early.
		void releasePathExistsTable (void);

		~GlobalMap (void) {
			destroy();
		}

		DoorInfoLinksPtr* ownDoorLinkViews (size_t count);

		void init (long h, long w);

		long build (MissionMapCellInfo* mapData);

		long init (PacketFilePtr mapFile, long whichPacket = 0, bool restoreSavedCosts = false);

		long write (PacketFile* packetFile, long whichPacket = 0);

		long setTempArea (long row, long col, long cost);

		bool fillNorthSouthBridgeArea (long row, long col, long area);

		bool fillEastWestBridgeArea (long row, long col, long area);

		bool fillNorthSouthRailroadBridgeArea (long row, long col, long area);

		bool fillEastWestRailroadBridgeArea (long row, long col, long area);

		bool fillSpecialArea (long row, long col, long area, long wallGateID);

		bool fillArea (long row, long col, long area, bool offMap);

		void calcSectorAreas (long sectorR, long sectorC);

		void beginDoorProcessing (void);

		long numAreaDoors (long area);

		void getAreaDoors (long area, DoorInfoPtr dorrList);

		void addDoor (long adjArea, long curArea, long row, long col, long length, long dir);

		void endDoorProcessing (void);

		void calcAreas (void);

		void calcSpecialAreas (MissionMapCellInfo* mapData);

		void calcCellsCovered (void);

		void calcSpecialTypes (void);

		void calcGlobalDoors (void);

		void calcAreaDoors (void);

		long calcLinkCost (long startDoor, long thruArea, long goalDoor);

		void changeAreaLinkCost (long area, long cost);

		void calcDoorLinks (void);

        long getPathCost (int startArea, int goalArea, bool withSpecialAreas, int& confidence, bool calcIt);

#ifdef USE_PATH_COST_TABLE
		void initPathCostTable (void);

		void resetPathCostTable (void);

		void calcPathCostTable (void);

		void setPathCost (long startArea, long goalArea, bool withSpecialAreas, unsigned char cost);

		void setPathFlag (long startArea, long goalArea, unsigned char flag, bool set);

		long getPathFlag (long startArea, long goalArea, unsigned char flag);
#endif
		void clearPathExistsTable (void);

		void setPathExists (long fromArea, long toArea, unsigned char set);

		unsigned char getPathExists (long fromArea, long toArea);

		long exitDirection (long doorIndex, long fromArea);

		void setStartDoor (long startArea);

		void resetStartDoor (long startArea);

		void setAreaTeamID (long area, char teamID);

		void setAreaOwnerWID (long area, long objWID);

		void setGoalDoor (long goalArea);

		void resetGoalDoor (long goalArea);

		long calcHPrime (long area);

		//void propogateCost (long area, long g);
		void propogateCost (long door, long cost, long fromAreaIndex, long g);

		long calcPath (long startArea,
					   long goalArea,
					   GlobalPathStepPtr path,
					   long startRow = -1,
					   long startCol = -1,
					   long goalRow = -1,
					   long goalCol = -1);

		long calcPath (glm::vec3 start, glm::vec3 goal, GlobalPathStepPtr path);

		long calcArea (long row, long col) {
			// Bounds guard: synthesized blank MOVE grids are clamped to
			// MAX_MAP_CELL_WIDTH and can be smaller than the terrain cell
			// range, so worldToCell-derived row/col may exceed this grid.
			// Out-of-range = no area (mirrors the areaId<0 sentinel).
			if ((row < 0) || (row >= height) || (col < 0) || (col >= width))
				return(-1);
			long areaId = areaMap[row * width + col];
			if (areaId < 0)
				return(-1);
			return(areaId);
		}

		void getDoorTiles (long area, long door, GlobalMapDoorPtr areaDoor);

		void getSectorCoords (long area, long& sectorR, long& sectorC) {
			sectorR = areas[area].sectorR;
			sectorC = areas[area].sectorC;
		}

		glm::vec3 getDoorWorldPos (long area, long door, long* goalCell);

		void openDoor (long door);

		void closeDoor (long door);

		void closeArea (long area);

		void closeArea (long row, long col) {
			long area = calcArea(row, col);
			gosASSERT(area > -1);
			closeArea(area);
		}

		void openArea (long area);

		void openArea (long row, long col) {
			long area = calcArea(row, col);
			gosASSERT(area > -1);
			openArea(area);
		}

		void openOffMapAreas (void) {
			for (long i = 0; i < numOffMapAreas; i++)
				openArea(offMapAreas[i]);
		}

		void closeOffMapAreas (void) {
			for (long i = 0; i < numOffMapAreas; i++)
				closeArea(offMapAreas[i]);
		}

		bool getDoorOpen (long door) {
			return(doors[door].open);
		}

		bool isClosedArea (long area) {
			return(!areas[area].open);
		}

		bool getAdjacentAreaCell (long area, long adjacentArea, long& cellRow, long& cellCol);

		void print (char* fileName);

		static bool toggleLog (void);

		static void writeLog (char* s);
};

using GlobalMapPtr = GlobalMap*;

//******************************************************************************************

inline constexpr int NUM_ADJ_CELLS = 8;

struct _MoveMapNode {
    // sebi moved 'cost' field up for less data cache misses (I hope)
    // maybe will have to move adjCells in a separate array
	int				cost;								// normal cost to travel here, based upon terrain
	int				parent;								// where we came from (parent cell)
	unsigned int    flags;								// CLOSED, OPEN, STEP flags
	int				g;									// known cost from START to this node
	int				hPrime;								// estimated cost from this node to GOAL
	int				fPrime;								// = g + hPrime

	short		    adjCells[NUM_ADJ_CELLS];

	void setFlag (unsigned int flag) {
		flags |= flag;
	}

	void clearFlag (unsigned int flag) {
		flags &= (flag ^ 0xFFFFFFFF);
	}
}; using MoveMapNode = _MoveMapNode;

using MoveMapNodePtr = MoveMapNode*;

inline constexpr int DISTANCE_TABLE_DIM = 80;

class MoveMap {

	public:

		int				    ULr;		// upper-left cell row
		int				    ULc;		// upper-left cell col
		int				    width;
		int				    height;
		int				    minRow;
		int				    maxRow;
		int				    minCol;
		int				    maxCol;
		int				    maxWidth;
		int				    maxHeight;
		// The solve grid (maxWidth x maxHeight cells) and its index tables, sized by init(h, w) and empty until then; empty = not allocated.
		std::vector<MoveMapNode>	map;
		std::vector<int>			mapRowStartTable;	// first cell index of each row
		std::vector<int>			mapRowTable;		// row of each cell index
		std::vector<int>			mapColTable;		// column of each cell index
		int				    moveLevel;
		glm::vec3		start;
		int				    startR;
		int				    startC;
		glm::vec3		goal;			// actual world-coord goal
		int				    goalR;			// cell goal row relative to move map
		int				    goalC;			// cell goal col relative to move map
		int				    thruAreas[2];
		int				    door;
		int				    doorSide;
		int				    doorDirection;	// if goal is not a door, set to -1
		glm::vec3		target;			// actual world-coord target
		int				    clearCost;		// cost, in tenths of secs, to move to clear cell
		int				    jumpCost;		// cost, in tenths of secs, to jump to cell
		int				    numOffsets;		// set by calcMovePath function (don't touch:)
		float				calcTime;
		int*				overlayWeightTable;
		int				    moverWID;
		int				    moverTeamID;
		bool				moverLayingMines;
		bool				moverWithdrawing;
		bool				travelOffMap;
		bool				cannotEnterOffMap;

		void				(*blockedDoorCallback) (int moveLevel, int door, char* openCells);
		void				(*placeStationaryMoversCallback) (MoveMapPtr map);

		// PATHFINDING-SOLVER-ISOLATION-1: per-solve seam. Set at the top of each
		// solve (calcPath/calcPathJUMP/calcEscapePath): the flag snapshot the solve
		// and its cost helpers read instead of the file-static global.
		bool				solveJumpOnBlocked;

		// Per-solve hPrime ceiling, set at the top of each solve (was the file-static MaxHPrime).
		long				maxHPrime;

		// The OPEN list of every solve this map runs. WHY a member: the list used to be one global shared by all solvers, so a solve could meet nodes another
		// solve had left in it; each map now owns its list and a solve starts it empty.
		mc2::OpenHeap		openHeap;
		// A prepared grid may be reused only against the mission inputs it baked.
		MissionMapPtr inputMap = nullptr;
		MoveLayerRevisions inputRevisions;
		bool inputRevisionsValid (void) const;

		static float		distanceFloat[DISTANCE_TABLE_DIM][DISTANCE_TABLE_DIM];
		static int			distanceInt[DISTANCE_TABLE_DIM][DISTANCE_TABLE_DIM];
		static int			forestCost;

	protected:

		// Gives the OPEN node of a map cell its new key; a cell flagged OPEN that the list does not hold is skipped (one log line per map, an assert in debug).
		void reKeyOpen (long cellId, long newKey);
		void updateOpenNode(MoveMapNode& node, long parent, long g, long cellId);
		bool adjacentCellOpen (long mapCellIndex, long dir);
		bool adjacentCellOpenJUMP (long r, long c, long dir);
		void propogateCost (long mapCellIndex, long cost, long g);
		void propogateCostJUMP (long r, long c, long cost, long g);
		int calcHPrime (int r, int c);
		
	public:

		
		void init (void) {
			ULr = 0;
			ULc = 0;
			maxHeight = 0;
			maxWidth = 0;
			height = 0;
			width = 0;
			minRow = 0;
			maxRow = 0;
			minCol = 0;
			maxCol = 0;
			map.clear();
			mapRowStartTable.clear();
			mapRowTable.clear();
			mapColTable.clear();
			moveLevel = 0;
			startR = -1;
			startC = -1;
			goalR = -1;
			goalC = -1;
			thruAreas[0] = -1;
			thruAreas[1] = -1;
			goal = glm::vec3(0.0f);
			door = -1;
			doorSide = -1;
			doorDirection = -1;
			target.x = -999999.0;
			target.y = -999999.0;
			target.z = -999999.0;
			clearCost = 1;
			jumpCost = 0;
			numOffsets = 8;
			calcTime = 0.0;
			travelOffMap = false;
			cannotEnterOffMap = true;
			overlayWeightTable = nullptr;
			blockedDoorCallback = nullptr;
			placeStationaryMoversCallback = nullptr;
			solveJumpOnBlocked = false;
			maxHPrime = 1000;
			inputMap = nullptr;
		}
		
		MoveMap (void) {
			init();
		}
		
		void destroy (void);

		~MoveMap (void) {
			destroy();
		}

		void init (long h, long w);

		long init (FitIniFile* mapFile);

		long setUp (long ULr,
					long ULc,
					long height,
					long width,
					long moveLevel,
					glm::vec3* startPos,
					long startRow,
					long startCol,
					glm::vec3 goalPos,
					long goalRow,
					long goalCol,
					long clearCellCost,
					long jumpCellCost,
					long offsets,
					unsigned long params = MOVEPARAM_NONE);

		long setUp (long moveLevel,
					glm::vec3* startPos,
					long startRow,
					long startCol,
					long thruArea[2],
					long goalDoor,
					glm::vec3 finalGoal,
					long clearCellCost,
					long jumpCellCost,
					long offsets,
					unsigned long params = MOVEPARAM_NONE);

		void clear (void);

		void placeMovers (bool stationaryOnly);

		void setOverlayWeightTable (int* table) {
			overlayWeightTable = table;
		}

		void setStart (glm::vec3* startPos, long startCellRow, long startCellCol);

		void setGoal (glm::vec3 goalPos, long goalCellRow, long goalCellCol);

		void setGoal (long thruArea, long goalDoor);

		long markGoals (glm::vec3 finalGoal);

		long markEscapeGoals (glm::vec3 finalGoal);

		void setTarget (glm::vec3 targetPos);

		char getCost (long row, long col) {
			// Off-map: return a high (avoid) cost rather than indexing map[] OOB.
			// Real paths are inBounds-gated elsewhere, so this only guards stray
			// off-grid queries on oversized maps (MOVE grid < terrain grid).
			if (!inBounds(row, col)) return(static_cast<char>(127));
			return(map[row * width + col].cost);
		}

		void setCost (long row, long col, long newCost);

		void adjustCost (long row, long col, long costAdj);

		void setStart (long row, long col) {
			startR = row;
			startC = col;
		}

		void setClearCost (long cost) {
			clearCost = cost;
		}

		void setJumpCost (long cost, long offsets = 8) {
			jumpCost = cost;
			numOffsets = offsets;
		}

		void setMover (long watchID, long teamID = 0, bool layingMines = false, bool withdrawing = false) {
			moverWID = watchID;
			moverTeamID = teamID;
			moverLayingMines = layingMines;
			moverWithdrawing = withdrawing;
		}

		long calcPath (MovePathPtr path, glm::vec3* goalWorldPos, int* goalCell);

		long calcPathJUMP (MovePathPtr path, glm::vec3* goalWorldPos, int* goalCell);

		long calcEscapePath (MovePathPtr path, glm::vec3* goalWorldPos, long* goalCell);

		float getDistanceFloat (long rowDelta, long colDelta) {
			return(distanceFloat[rowDelta][colDelta]);
		}

		float getDistanceLong (long rowDelta, long colDelta) {
			return(distanceInt[rowDelta][colDelta]);
		}

		void writeDebug (File* debugFile);

		bool inBounds (long row, long col) {
			return((row >= minRow) && (row <= maxRow) && (col >= minCol) && (col <= maxCol));
		}
};

//---------------------------------------------------------------------------

void SaveMapCells (char* fileName,  long height, long width,  MissionMapCellInfo* mapData);

MissionMapCellInfo* LoadMapCells (char* fileName, long& height, long& width);

void DeleteMapCells (MissionMapCellInfo* mapData);

void MOVE_init (long moveRange);
void MOVE_buildData (long height, long width, MissionMapCellInfo* mapData, long numSpecialAreas, GameObjectFootPrint* specialAreaFootPrints);
long MOVE_saveData (PacketFile* packetFile, long whichPacket = 0, mc2::save::NavigationFormat format = {});
long MOVE_readData (PacketFile* packetFile, long whichPacket, bool restoreSavedCosts = false, mc2::save::NavigationFormat format = {});
void MOVE_cleanup (void);

//long BuildAndSaveMoveData (char* fileName, long height, long width, MissionMapCellInfo* mapData);

//***************************************************************************

extern MissionMapPtr		GameMap;
extern GlobalMapPtr			GlobalMoveMap[3];
extern thread_local MoveMapPtr	PathFindMap[2];
// Preallocated frame/load (0) and overlap-simulation (1) scratch views.
void MOVE_bindPathFindMaps (int worker);
void MOVE_configurePathFindMaps (void);
extern long					SimpleMovePathRange;

//***************************************************************************

#endif

