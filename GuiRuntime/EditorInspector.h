#pragma once
#include "../RenderWorld/RenderWorld.h"
#include "../RenderCore/MaterialGpu.h"
#include <cstdint>

namespace EditorInspector {

// Inspector's own pick-source taxonomy. Independent of RenderWorld::RenderObjectKind
// so terrain (which is not a RenderWorld object) can be a first-class pick result.
enum class InspectorPickKind : unsigned char {
    None       = 0,
    StaticProp = 1,
    Mech       = 2,
    Terrain    = 3,
};

struct InspectorSelection {
    bool valid = false;                                                  // (existing) RenderWorld lookup valid
    bool hasSelection = false;                                           // NEW
    InspectorPickKind pickKind = InspectorPickKind::None;                // NEW
    RenderWorld::RenderObjectKind kind = RenderWorld::RenderObjectKind::StaticProp;
    RenderCore::RenderObjectHandle handle;
    RenderWorld::LookupResult lookup;
    int screenX = 0;
    int screenY = 0;
};

struct StaticPropInspectorData {
    bool    populated    = false;
    int32_t recipeIndex  = -1;
    char    shapeName[128] = {};
    // MaterialGpu sidecar (populated when MC2_MATERIAL_GPU=1).
    bool    materialGpuActive     = false;
    uint32_t materialIdx          = 0xFFFFFFFFu;
    uint32_t materialGen          = 0u;
    bool    materialGpuPopulated  = false;
    RenderCore::MaterialGpu materialGpu = {};
};

struct MechInspectorData {
    bool    populated        = false;
    char    variantName[64]  = {};
    char    longName[64]     = {};
    int     chassisClass     = 0;
    long    teamId           = -1;
    bool    disabled         = false;
    bool    destroyed        = false;
    bool    crippled         = false;
    int32_t conStat          = 0;
    float   totalCurArmor    = 0.f;
    float   totalMaxArmor    = 0.f;
    float   totalCurStr      = 0.f;
    float   totalMaxStr      = 0.f;
    char    pilotName[64]    = {};
};

struct TerrainInspectorData {
    bool  populated  = false;
    float worldX = 0.f, worldY = 0.f, worldZ = 0.f;
    int   tileRow = -1, tileCol = -1;
    int   cellRow = -1, cellCol = -1;
    int   terrainType = -1;
};

// TERRAIN-SPINE-0: pass-level (not selection-level) snapshot of the terrain
// render spine. Filled per frame in gameosmain; displayed in the Object
// Inspector window. Read-only — no GL state, no mutation of any render path.
struct TerrainPassSnapshot {
    uint32_t surfaceProgramId       = 0;
    uint32_t thinProgramId          = 0;
    uint32_t waterFastProgramId     = 0;
    uint32_t overlayProgramId       = 0;
    uint32_t bucketCount            = 0;
    uint32_t vertCount              = 0;
    uint32_t thinRecCount           = 0;
    uint32_t recipeCount            = 0;
    bool     overflow               = false;
    // v1: hard-coded false — terrain shaders don't consume ViewUniforms (binding=3) yet.
    bool     viewUniformsBoundForTerrain = false;
    uint32_t currentViewId          = 0;
    const char* currentViewName     = "";  // RenderCore-owned string literal
    bool     tessellationOn         = true;
};

void onCtrlShiftClick(int mouseX, int mouseY);  // called by missiongui.cpp (Task 7)
// Task 7 bridge: missiongui.cpp calls tryGameplayPick, then passes the result here.
// Keeps gui_runtime layering clean (gui_runtime must not link against code/ targets).
void setPickResult(int mouseX, int mouseY, const RenderWorld::LookupResult& lookup);
void setStaticPropData(const StaticPropInspectorData& sd);
void setMechData(const MechInspectorData& md);
void setTerrainData(const TerrainInspectorData& td);
void setTerrainPassSnapshot(const TerrainPassSnapshot& ts);  // TERRAIN-SPINE-0

// SHADOW-SPINE-0: pass-level snapshot of the shadow-pass render spine. Filled
// per frame in gameosmain; displayed in the Object Inspector window.
// Read-only — no GL state, no mutation of any shadow path.
struct ShadowPassSnapshot {
    // Programs (raw GL program object ids; 0 if not linked).
    uint32_t terrainShadowProgramId    = 0;
    uint32_t mechShadowProgramId       = 0;
    uint32_t staticPropShadowProgramId = 0;
    // gosPostProcess state.
    bool     shadowsEnabled            = false;
    bool     staticLightMatrixBuilt    = false;
    int      shadowMapSize             = 0;
    int      dynShadowMapSize          = 0;
    // Caster counts written by the most recent flushShadow() per lane.
    uint32_t mechShadowTypesDrawn        = 0;
    uint32_t mechShadowInstDrawn         = 0;
    uint32_t staticPropShadowTypesDrawn  = 0;
    uint32_t staticPropShadowInstDrawn   = 0;
    // v1: shadow shaders do NOT consume ViewUniforms (binding=3). Hard-coded
    // so the closure-audit gap is visible in the inspector.
    bool     viewUniformsBoundForShadow = false;
};
void setShadowPassSnapshot(const ShadowPassSnapshot& sp);  // SHADOW-SPINE-0

// VFX-SPINE-0: pass-level snapshot of the GPU particle / VFX render spine.
// Filled per frame in gameosmain; displayed in the Object Inspector window.
// Read-only — no GL state, no mutation of any VFX path, no object-IDs.
struct VfxPassSnapshot {
    // Program (raw GL program object id for the billboard shader; 0 until linked).
    uint32_t particleProgramId          = 0;
    // Env-gate state.
    bool     gpuParticlesEnabled        = false;   // MC2_GPU_PARTICLES (default OFF)
    bool     gpuParticlesLogEnabled     = false;   // MC2_GPU_PARTICLES_LOG
    bool     initFailed                 = false;   // bridge init/compile failed
    bool     cameraSetThisFrame         = false;   // gos_SetActiveCamera fired this frame
    // Buffer state (Batcher CPU staging + GL SSBO at binding=14).
    uint32_t perFrameBudget             = 0;       // CPU staging budget (records)
    uint32_t ssboCapacityRecords        = 0;       // current GL SSBO capacity (records)
    bool     overflowReported           = false;   // last frame staging overflowed
    // Process-lifetime aggregates (from anonymous-namespace counters in batcher.cpp).
    // Per-frame counts are not exposed by the existing instrumentation — would
    // require a new counter and is out of scope for VFX-SPINE-0.
    unsigned long long emitTotal             = 0;
    unsigned long long flushTotal            = 0;
    unsigned long long nonemptyFlushTotal    = 0;
    unsigned long long recordsFlushedTotal   = 0;
    uint32_t           recordsPerFlushMax    = 0;
    unsigned long long trailSpawnTotal       = 0;
    unsigned long long trailHeadTotal        = 0;
    // v1: VFX shaders do NOT consume ViewUniforms (binding=3). Hard-coded so
    // the closure-audit gap is visible in the inspector.
    bool     viewUniformsBoundForVfx    = false;
    // VFX-DEBUG-VIEWS-1: active particle debug mode (gos_vfx_getDebugMode).
    // 0=Final, 1=Albedo, 2=Alpha, 3=ParticleKind, 4=Overdraw. Read-only display.
    int      debugMode                  = 0;
};
void setVfxPassSnapshot(const VfxPassSnapshot& vs);  // VFX-SPINE-0

void flushDebugHighlight();
void drawImGui();                                 // called by GuiRuntime::Render() each frame
void clear();                                     // clear selection

}  // namespace EditorInspector
