// Shared surface of the gos_terrain_indirect translation units (gos_terrain_indirect*.cpp).
//
// gos_terrain_indirect.cpp keeps the gate readers, N1 counters and cost-split roll-ups; the rest of the indirect
// terrain path is split by responsibility (see DOC in each piece). State that more than one piece reads or writes
// lives in namespace gos_terrain_indirect_detail (declared here, defined in the piece that owns it).
#ifndef GOS_TERRAIN_INDIRECT_INTERNAL_H
#define GOS_TERRAIN_INDIRECT_INTERNAL_H

// GameOS/gameos/gos_terrain_indirect.cpp
//
// Indirect terrain SOLID-only PR1 — Stage 0 + Stage 2 implementation.
//
// See gos_terrain_indirect.h for the slice overview, the Tracy zone name
// reservations, and the public API contract (counter units = per-quad,
// parity-printer schema, env-gate semantics).
//
// Stage 0: env-gate readers, N1 counters, parity-printer skeleton.
// Stage 2: dense TerrainQuadRecipe SSBO build/reset/invalidate/flush,
//          parity body comparing recipe against live quadList values.
// Stage 3 wires the per-frame thin-record packer, indirect-command builder,
// preflight-arming, and bridge entry.

#include "gos_terrain_indirect.h"
#include "../../mclib/terrain_material.h"  // single-sourced terrain type -> material
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1

#include "gos_gpu_sync.h"               // GPU-SYNC-CONTRACT typed barrier helper
#include "gos_terrain_patch_stream.h"  // TerrainQuadRecipe
#include "gpu_driven_common.h"         // gpu_driven::IsTerrainSolidEnabled
#include "gos_terrain_lighting.h"      // gos_terrain_lighting::GetOutputSsbo()
#include "gos_static_prop_killswitch.h" // gos_GetTerrainMVPMat4()
#include "gos_postprocess.h"            // WATER-TERRAIN-REFLECTION-1: reflection FBO
#include "../../RenderCore/terrain_path_telemetry.h"  // TERRAIN-PATH-TELEMETRY-1
#include "../../RenderCore/RenderResourceRegistry.h"   // REGISTRY-TERRAIN-SSBO-1: recipe/thin/cement/mask ids
// WATER-TERRAIN-REFLECTION-1: install a mirror MVP into the terrain_mvp_ cache
// (defined __stdcall in gameos_graphics.cpp; same cache gos_GetTerrainMVPMat4 reads).
void __stdcall gos_SetTerrainMVP(const float* matrix16);

// View-epoch counter. Global-scope (defined in gameos_graphics.cpp), so the
// extern MUST live OUTSIDE namespace gos_terrain_indirect or the linker mangles
// it as a namespace-scoped symbol.
extern long g_viewContentEpoch;   // VIEW-EPOCH-DEDUPE-1 semantic view-content epoch

#include <map>
#include <vector>
#include <algorithm>  // CEMENT-ATLAS-TONE-MATCH-1: std::sort / std::min / std::max
#include <cstdio>
#include <cstdlib>
#include <cstring>  // memcpy
#include <cmath>    // std::abs (clipPos ULP-bounded parity comparator)
#include <chrono>   // MC2_RING_TRACE wait-time measurement (probe-only)
#include <ctime>    // RING_SINK timestamp on probe-sink open

#include <intrin.h>   // __rdtsc -- [LIGHT_COST_SPLIT v1]

#include <GL/glew.h>

// MC2 types — resolve relative path from GameOS/gameos/
#include "../../mclib/terrain.h"
#include "../../mclib/quad.h"
#include "../../mclib/vertex.h"
#include "../../mclib/mapdata.h"
#include "../../mclib/move.h"       // PR2c Stage 1c — MissionMap, GameMap, MAPCELL_DIM
#include "../../mclib/terrtxm.h"    // TERRAIN_TXM_SIZE (extern int)
#include "../../mclib/txmmgr.h"     // MC_MAXTEXTURES (cement node-index space)
#include "gos_terrain_bridge.h"     // gos_terrain_bridge_glTextureForGosHandle (cement readback)
#include "../../RenderCore/KtxLoader.h"  // COLORMAP-BC7-KTX2-1: ktxLoadRgba8 for BC7 atlas upload
#include "gos_terrain_arm_logic.h"        // shared GPU-terrain arm predicate (editor↔game)
#include "gos_terrain_lod_chunk.h"        // Step 5c: push cement words to the LOD chunk path

#include "../gameos/gos_profiler.h"
#include "mc2_hitch_trace.h"
#include "Gates/gate_cold.h"   // gate::get<>() and str<>()

// CEMENT_DIAG: file-scope extern for the global mission name buffer
// (defined in code/mechcmd2.cpp:180 and code/logmain.cpp:88 as
// `char missionName[1024]`).  Used by BuildCementCatalogAtlas summary line.
extern char missionName[];

// [DEPTH_TRANSITION v1] CPU-water REAL screen-z sample - DEFINED at global
// scope in mclib/quad.cpp (writer) and reset in mclib/terrain.cpp. Declared
// here at GLOBAL file scope (NOT block-scope inside namespace
// gos_terrain_indirect) so the dump's unqualified use binds the global
// ::g_cpuWaterProbe* (a block-scope extern inside the namespace would bind
// gos_terrain_indirect:: and not link - C++ [basic.link] innermost-ns rule).
extern float              g_cpuWaterProbeZ;
extern bool               g_cpuWaterProbeAny;
extern unsigned long long g_cpuWaterProbeStamp;

// MC2_LOAD_RACE_TRACE (S2b corrected transient probes). Latches defined in
// mclib/apprtype.cpp; g_mc2FrameCounter is the engine frame clock; scenarioTime
// drives the negative-control atlas delay.
extern bool     g_loadRaceTrace;
extern long     g_loadRaceAtlasFirstFrame;
extern long     g_loadRaceArmFirstFrame;
extern long     g_loadRaceLastTerrainPath;
extern uint32_t g_mc2FrameCounter;
extern float    scenarioTime;

// Definitions the split pieces share: constants, small inline helpers, types and the Stage 3 includes.

// Terrain type -> PBR material index: single-sourced in mclib/terrain_material.h.
inline uint8_t terrainTypeToMaterialLocal(uint32_t terrainType) {
    return mc2terrain::terrainTypeToMaterial(terrainType);
}

// Soft-knee scale: linear up to `knee`, then asymptotic toward 255, so a lane
// marking scaled by a 2.5x gain stays a marking instead of clipping to flat 255.
static inline uint32_t RunwayToneScale(float v, float gain) {
    const float knee = 190.f;
    float y = v * gain;
    if (y > knee) {
        const float head = 255.f - knee;
        y = 255.f - head * expf(-(y - knee) / head);
    }
    return (uint32_t)std::min(255.f, std::max(0.f, y));
}

static const float kConcreteToneGainLo = 0.80f;  // Arm A gain clamp -- NARROW on purpose
static const float kConcreteToneGainHi = 1.25f;

struct CementCellStats {
    float r = 0.f, g = 0.f, b = 0.f;   // [P50,P90] band mean
    float lum = 0.f;                   // band mean luminance
    float chroma = 0.f;                // 0 = neutral grey, 1 = fully saturated
    float markFrac = 0.f;              // fraction of texels well ABOVE the band (paint)
    bool  ok = false;                  // enough usable texels
    bool  family = false;              // classified as actual concrete
    bool  marks = false;               // carries bright painted markings
    // CEMENT-INTRACELL-1 (ARM C): the concrete BACKGROUND, measured with the
    // baked road art excluded, plus the road census itself.
    float bgLum = 0.f;                 // [P60,P95] upper-mode luminance = the concrete
    float roadFrac = 0.f;              // fraction of texels well BELOW bgLum (asphalt)
    bool  road = false;                // carries baked road art
};

// Forward-declare the bridge (defined in gameos_graphics.cpp).
// Signature uses unsigned int (not GLuint) to match gos_terrain_bridge.h
// without pulling in GL headers there.
bool gos_terrain_bridge_drawIndirect(int cmdCount, unsigned int recipeSSBO,
                                     unsigned int thinRecordSSBO,
                                     unsigned int indirectCmdBuffer);

// Include pVertex via PostcompVertex — already available through mapdata.h.
// (terrain.h already included above, which pulls mapdata.h.)
#include "../../mclib/vertex.h"     // ScreenVertex, vertexNum
// tex_resolve — lazy per-frame memoization. Header includes txmmgr.h.
#include "../../mclib/tex_resolve_table.h"

// TERRAIN_DEPTH_FUDGE for the per-tri pz check: single source of truth is
// mclib/terrain_depth_bias.h (was a re-stated local constexpr; the old
// #ifndef guard defended against a quad.cpp MACRO collision that the
// constexpr header eliminates). Consumer below uses it unqualified.
#include "../../mclib/terrain_depth_bias.h"
#ifdef MC2_VULKAN_ISLAND
#include "vulkan_terrain_island.h"   // VK-PARITY-OVERLAY-1: overlay owner queue
#endif
using mc2depth::TERRAIN_DEPTH_FUDGE;

static constexpr int    kThinRingFrames    = 3;
static constexpr size_t kMaxThinRecords    = 65536u;  // ≥ max visible quads
static constexpr size_t kThinRecordBytes   = kMaxThinRecords * sizeof(TerrainQuadThinRecord);

// Indirect command buffer — driver-only, GL_DRAW_INDIRECT_BUFFER.
// Sized for 16 commands (256 B); PR1 emits exactly 1.
struct DrawArraysIndirectCommand {
    GLuint count;
    GLuint instanceCount;
    GLuint first;
    GLuint baseInstance;
};
static_assert(sizeof(DrawArraysIndirectCommand) == 16,
    "DrawArraysIndirectCommand is 4 GLuints = 16 B per GL spec");

static constexpr size_t kIndirectCmdBufferBytes = 16 * sizeof(DrawArraysIndirectCommand);

// ---------------------------------------------------------------------------
// InMissionTransition() — simple stub; returns false until needed.
// Stage 4's quintuple soak gate validates this boundary.
// ---------------------------------------------------------------------------
static inline bool InMissionTransition() { return false; }

// Globals defined in gos_terrain_indirect.cpp (or elsewhere in the engine) that the split pieces share.

// Eager gate caches (read once at static init) with one definition each; several pieces consult them.
extern const bool s_loadRaceDelayAtlas;
namespace gos_terrain_indirect { extern const bool s_thinCanaryEnabled; }

// State and helpers shared between the pieces. Each is defined in the piece that owns it.
namespace gos_terrain_indirect_detail {
extern const bool g_envRingTrace;
extern std::vector<TerrainQuadRecipe> g_denseRecipes;
extern std::vector<bool>              g_denseRecipeDirty;
extern std::vector<uint32_t> g_solidWindowStaging;
extern GLuint   g_solidQuadWindowSsbo;
extern uint32_t g_solidQuadWindowCapacity;
extern uint32_t g_solidWindowMaxSeen;
extern bool                           g_denseRecipeAnyDirty;
extern GLuint                         g_recipeSSBO;
extern int32_t                        g_recipeMapSide;
extern bool                           g_recipeReady;
extern bool s_firstDrawPrintedThisMission;
bool traceOn();
extern GLuint  g_atlasGLTex;
extern int     g_atlasSize;
extern float   g_atlasNumTexturesAcross;
extern float   g_atlasMapTopLeftX;
extern float   g_atlasMapTopLeftY;
extern float   g_atlasOneOverWorldUnits;
extern GLuint  g_cementAtlasGLTex;
extern int     g_cementAtlasGridSide;
extern int     g_cementAtlasTileCount;
extern bool    g_cementLayerMapReady;
extern int     g_cementCatalogTruncated;
extern uint16_t g_cementLayerIndexByNodeIdx[MC_MAXTEXTURES];
extern uint16_t g_cementLayerIndexBySlot[MC_MAX_TERRAIN_TXMS];
extern GLuint g_transitionMaskArrayGL;
extern bool   g_transitionMaskReady;
int deriveMaskIdFromCorners(bool c0, bool c1, bool c2, bool c3);
bool PadEdgeRoadFixEnabled();
bool CementAtlasMagentaKeyEnabled();
bool CementAlphaAtlasEnabled();
extern uint16_t g_cementAlphaLayerIndexBySlot[MC_MAX_TERRAIN_TXMS];
extern bool     g_cementAlphaLayerMapReady;
bool RoadArtDetectEnabled();
bool ConcreteToneEnabled();
bool ConcreteCanonEnabled();
void CementCellComputeStats(const std::vector<uint32_t>& atlasBuf, int atlasPixelSide, int gridSide, int txmSize, int k, CementCellStats& s);
int ClassifyConcreteFamily(std::vector<CementCellStats>& st, int NT, float& refR, float& refG, float& refB);
bool TileRoadAdjacent8(long r, long c, long mapSide, PostcompVertexPtr blocks);
void BuildTransitionMaskArray();
void PopulateRecipeCementWords();
extern std::vector<uint32_t> g_uniqueTerrainNodeIds;
void CollectUniqueNodeIds();
bool retireCpuColorMap();
extern uint32_t g_cementPackUnmappedCount;
extern uint32_t g_cementMappedThisFrame;
extern uint32_t g_concreteAllCornersThisFrame;
void BuildColormapAtlas();
void BuildCementCatalogAtlas();
extern GLuint g_thinRecordSSBO;
extern int    g_thinRingSlot;
extern GLsync g_thinRingFences[kThinRingFrames];
extern GLuint g_indirectCmdBuffer;
extern bool  s_frameSolidArmed;
extern int   s_frameSolidPackedThinCount;
extern int   s_frameSolidCmdCount;
extern bool  s_processArmingDisabled;
extern bool  s_everFrameSolidArmed;
extern GLuint  g_solidComputeProgram;
extern GLuint  g_solidBucketHeaderSsbo;
extern GLuint  g_terrainHandleLutSSBO;
extern GLuint  g_thinCanarySSBO;
extern uint32_t g_dispatchMvpFp;
extern uint64_t g_dispatchMvpFrameIdx;
extern long     g_dispatchMvpViewEpoch;
extern float    g_dispatchMvpFloats[4];
extern float    g_dispatchMvp16[16];
extern float    g_thinSlotMVP[kThinRingFrames][16];
extern bool     g_thinSlotMVPValid[kThinRingFrames];
extern GLint g_locSolidWC;
extern GLint g_locSolidAO;
extern GLint g_locSolidMVP;
extern GLint g_locSolidBHT;
extern GLint g_locSolidUW;
extern GLint g_locSolidMS;
extern GLint g_locSolidReflPass;
extern bool  s_solidReflectionPassActive;
extern bool s_solidGpuDispatchRanThisFrame;
bool ResourcesReady();
int PackThinRecordsForFrame();
int BuildIndirectCommands(int thinCount);
uint32_t UploadTerrainHandleLUT();
uint32_t BuildSolidQuadWindowSSBO(int* outUseWindow);
}  // namespace gos_terrain_indirect_detail
using namespace gos_terrain_indirect_detail;

#endif // GOS_TERRAIN_INDIRECT_INTERNAL_H
