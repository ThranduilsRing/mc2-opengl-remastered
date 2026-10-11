// Shared surface of the terrain.cpp translation units (terrain.cpp, terrain_{init,raycast,update,render,geometry}.cpp).
//
// terrain.cpp keeps the map globals, the init entry points, the setters / getters and the save / load / selection code; the rest
// of Terrain is split by responsibility (see DOC in each piece).
#ifndef TERRAIN_INTERNAL_H
#define TERRAIN_INTERNAL_H

#ifndef TERRAIN_H
#include"terrain.h"
#include"terrain_material.h"
#endif

#ifndef ERR_H
#include"err.h"   // C1b temporal-superset: 3-arg Assert() for worldToBlockIdx guard
#endif

#ifndef VERTEX_H
#include"vertex.h"
#endif

#ifndef CAMERA_H
#include"camera.h"
#endif

#ifndef TERRTXM_H
#include"terrtxm.h"
#include"tex_resolve_table.h"
#endif

#ifndef DBASEGUI_H
#include"dbasegui.h"
#endif

#include"../GameOS/gameos/gos_profiler.h"
#include"../GameOS/gameos/gos_terrain_water_stream.h"
#include"../GameOS/gameos/gpu_driven_common.h"
#include"../GameOS/gameos/gos_terrain_indirect.h"
#include"../GameOS/gameos/gos_terrain_surface.h"  // PR-1: continuous-surface mission-load generation
#include"../GameOS/gameos/gos_terrain_mask_dispatch.h"
#include"../GameOS/gameos/gos_terrain_bridge.h"
#include"../GameOS/gameos/gos_terrain_lighting.h"
#include"../GameOS/gameos/gos_terrain_height_tex.h"  // TERRAIN-NORMALS-FROM-HEIGHT-1
#include"../GameOS/gameos/gos_terrain_lod_chunk.h"   // Terrain LOD chunk Phase 1
#include"../GameOS/gameos/gos_render_pass_timer.h"   // [RENDER_PASS_TIME v1] chunk-terrain scope
#include"../GameOS/gameos/gos_frame_pass_stats.h"     // [FRAME_PASS_STATS v1] chunk draw count
#include"terrain_admission_mode.h"  // F6 T2: shared isModern() flag for terrain.cpp + quad.cpp
#include "Gates/gate_cold.h"   // gate::get<>() and str<>()
#include"terrain_runtime.h"  // TERRAIN-RUNTIME-API-1: compat seam + gated parity walk
#include <cstdio>            // TERRAIN-RUNTIME-API-1: parity self-test log
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1

#include"move.h"   // MC2_TERRAIN_MINE_AB diagnostic: GameMap (extern MissionMapPtr)
#include <vector>
#include <algorithm>   // MC2_TERRAIN_MINE_AB diagnostic: sort/unique/set_difference
#include <iterator>    // MC2_TERRAIN_MINE_AB diagnostic: std::back_inserter
#include <cstdint>

// Externals from quad.cpp / mapdata.cpp / mechcmd2.cpp used by the water fast path.
extern float MaxMinUV;
extern float cloudScrollX;

// TERRAIN-CONTROLMAP-SAMPLE-1: stb_image PNG decode wrapper (mclib/control_map_png_decode.cpp).
extern "C" unsigned char* ControlMapPng_DecodeRGBA(const unsigned char* data, int dataLen, int* outW, int* outH);
extern "C" void ControlMapPng_FreePixels(unsigned char* px);
extern float cloudScrollY;
extern long  sprayFrame;
extern bool  useWaterInterestTexture;

#ifndef CIDENT_H
#include"cident.h"
#endif

#ifndef PATHS_H
#include"paths.h"
#endif

#ifndef USERINPUT_H
#include"userinput.h"
#endif

#ifndef TIMING_H
#include"timing.h"
#endif

#ifndef PACKET_H
#include"packet.h"
#endif

#ifndef INIFILE_H
#include"fitinifile.h"
#endif

#ifndef TGAINFO_H
#include"tgainfo.h"
#endif

inline constexpr int MAX_TERRAIN_HEAP_SIZE = 1024000;

// Globals defined in terrain.cpp (or elsewhere in the engine) that the split pieces share.
extern float worldUnitsPerMeter;
extern float metersPerWorldUnit;
extern long terrainLineChanged;
extern int g_terrainMaterialProfile;
extern bool drawTerrainGrid;
extern bool drawEditorPassability;
extern uint8_t* gEditorNavFlags;
extern int gEditorNavCellSide;
extern bool drawLOSGrid;
extern bool drawTerrainTiles;
extern bool drawTerrainOverlays;
extern bool drawTerrainMines;
extern bool renderObjects;
extern bool renderTrees;
extern TerrainPtr land;
extern long *usedBlockList;;
extern long *moverBlockList;
extern unsigned long blockMemSize;
extern bool useOldProject;
extern bool projectAll;
extern bool useFog;
extern bool useVertexLighting;
extern bool useFaceLighting;
extern bool					useRealLOS;
extern unsigned char godMode;
extern long 				DrawDebugCells;
extern long visualRangeTable[256];
extern bool justResaveAllMaps;

// Per-draw-command scratch the update and render pieces share (defined in terrain.cpp), and the AABB helpers init and update call.
extern std::vector<uint8_t> s_skirtEdgeMaskVec;
extern std::vector<uint32_t> s_stitchStepVec;
extern std::vector<int> s_shadowTierVec;
extern std::vector<float> s_morphFactorVec;
void recomputeBlockAabb(TerrainBlockMeta& bm);
void recomputeSuperchunkAabb(int scX, int scY);
void clearList (void);
void clearMoverList (void);

#endif // TERRAIN_INTERNAL_H
