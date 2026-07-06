#include "gos_terrain_lod_chunk.h"
#include "utils/gl_utils.h"
#include "utils/shader_builder.h"
#include "gos_postprocess.h"   // Phase 10 Step 1c: shadow textures + light matrices
#include <gameos.hpp>          // Item 1: mc2ShadowCsmEnabled/Count (CSM shader define)
#include <string>
#include "gl_state_guard.h"    // GlStateGuard slice 2: composable depth/blend/cull RAII
#include "../../RenderCore/PipelineRegistry.h"  // TERRAIN-LODCHUNK-APPLYPIPELINE-ROUTING-1
#include "../../RenderCore/terrain_path_telemetry.h"  // TERRAIN-PATH-TELEMETRY-1
#include "pipeline_binder.h"                     // applyPipeline(TerrainSolid)
#include "../../mclib/render_contract.h"  // [RENDER_PASS v1] noteRenderPass
#include "../../RenderCore/RenderResourceRegistry.h"  // REGISTRY-TERRAIN-SSBO-1: TerrainHeightSsbo
#include "../../RenderCore/GpuBufferOwner.h"  // TERRAIN-LODCHUNK-SSBO-OWNER-1: type/cement owner records
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>
#include <map>

#ifdef MC2_VULKAN_ISLAND
#include "vulkan_terrain_island.h"   // VK-P10 TERRAIN TIER-1: LOD0 shared-GBuffer co-write
#include "vk_shared_gbuffer_gate.h"  // VK-P14 GATE-CONSOLIDATION-1 umbrella resolver
#endif

// GlStateGuard slice 2 kill-switch. Default-ON: when set, the terrain chunk
// draw owns its depth/blend/cull/mask/func via RAII guards (mc2gl::GlScoped*).
// MC2_GLSTATEGUARD_TERRAIN=0 reverts to the legacy hand-rolled save/restore
// (kept verbatim, nothing deleted) — the A/B used to prove the guards are
// pixel-neutral. Sampled once at process start.
static bool glStateGuardTerrainEnabled() {
    static const bool on = []() {
        const char* v = std::getenv("MC2_GLSTATEGUARD_TERRAIN");
        return (v == nullptr) || (std::atoi(v) != 0);  // unset/nonzero=ON, 0=OFF
    }();
    return on;
}

// Terrain MVP matrix — exposed by gameos_graphics.cpp for all terrain draw paths.
extern const float* gos_GetTerrainMVPMat4();
// TERRAIN-SHORELINE-MASK-1: shared render-shader clock (SmokeMode fixed-
// timestep override, same computation as the water fast-path's "time"
// uniform) — exposed by gameos_graphics.cpp. f(worldPos,time)-ONLY foam
// animation; never fed by anything camera-dependent.
extern float gos_GetShaderClockSeconds();
// Fix-B frame-of-reference: the GPU water cull and DrawDecalStatic project with
// the PREVIOUS-frame dispatch MVP (baked in Terrain::geometry()) when armed, NOT
// the live current-frame MVP. The legacy terrain draw also uses that baked MVP,
// so terrain depth + water cull + decals all agree at frame N-1. The chunk MUST
// match, or it writes depth at frame N while water/decals project at N-1 -> a
// 1-frame offset -> shore-water dropout + decal tearing under camera motion.
extern "C" const float* gos_terrain_indirect_getDispatchMvp16();
namespace gos_terrain_indirect { bool IsFrameSolidArmed(); }

// ---------------------------------------------------------------------------
// Static SSBO state — all GL objects live here, never in mclib/.
// ---------------------------------------------------------------------------

// TERRAIN-HEIGHT-SSBO-OWNER-1: the LOD-chunk height SSBO (last raw GLuint in the
// live LOD-chunk terrain renderer) is narrowed behind a GpuBufferOwner identity
// record (logical id + lifetime + debug name + GLuint value), mirroring the
// type/cement siblings. GL calls happen at the same sites with the same
// args/order/flags/slots; the raw handle is reached only via owner.backendHandle.
// Lifetime Mission: (re-)uploaded per mission load via glBufferData.
static RenderCore::GpuBufferOwner s_heightSsbo{  // backend handle; 0 = not yet allocated
    RenderCore::RenderResourceId::TerrainHeightSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainHeightSsbo",
    0u};
// TERRAIN-VISUAL-HEIGHT-SSBO-OWNER-1: the 4x visual heightfield SSBO (binding 26)
// is narrowed behind a GpuBufferOwner identity record (logical id + lifetime +
// debug name + GLuint value), mirroring the height/type/cement siblings. GL calls
// happen at the same sites with the same args/order/flags/slot; the raw handle is
// reached only via owner.backendHandle. Lifetime Mission: uploaded per mission load via
// glBufferData. CREATE is gated upstream (mclib/terrain.cpp: MC2_TERRAIN_VISUAL_HEIGHT
// / _DISPLACE + a visual_height_4x.r32 bake) — when the gate is off / no bake ships,
// the upload entry is never called, glName stays 0, and nothing registers
// (passthrough byte-identical). No stock bake ships → create path is offline-only.
static RenderCore::GpuBufferOwner s_visualHeightSsbo{ // TERRAIN-VISUAL-HEIGHT-SAMPLE-1: 4x visual heightfield (binding 26)
    RenderCore::RenderResourceId::TerrainVisualHeightSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainVisualHeightSsbo",
    0u};
static int    s_visualSide       = 0; // V = (mapSide-1)*4+1, fine grid side (0 = bake not loaded)
// TERRAIN-VISUAL-HEIGHT-S2-ALLLOD: per-LOD-band displaced-chunk counters, reset
// and logged once per submit alongside [TerrainLOD v1] so acceptance has hard
// per-band numbers. Index = LOD level (0..5), same mapping as [TerrainLOD v1].
static int s_visualDisplacedCounts[6] = {0, 0, 0, 0, 0, 0};
// TERRAIN-LODCHUNK-SSBO-OWNER-1: the LOD-chunk type/cement SSBOs are no longer
// bare GLuints. Each is narrowed behind a GpuBufferOwner identity record (logical
// id + lifetime + debug name + GLuint value). GL calls still happen at the same
// sites with the same args/order/flags/slots; the raw handle is reached only via
// owner.backendHandle. Lifetime Mission: (re-)uploaded per mission load via glBufferData.
static RenderCore::GpuBufferOwner s_typeSsbo{   // Step 5b: per-vertex terrainType SSBO (binding 24)
    RenderCore::RenderResourceId::TerrainTypeSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainTypeSsbo",
    0u};
static RenderCore::GpuBufferOwner s_cementSsbo{ // Step 5c: per-vertex cement word SSBO (binding 25)
    RenderCore::RenderResourceId::TerrainCementSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainCementSsbo",
    0u};
static int    s_mapSide    = 0;   // mapSide stored at last UploadHeightFull
static float  s_halfMap    = 0.0f;// (mapSide * 128.0 * 0.5)

// TERRAIN-CONTROLMAP-SAMPLE-1: authored RGBA override control-map texture
// (unit 12). Plain GLuint (not GpuBufferOwner) — mirrors gos_terrain_height_tex.cpp's
// self-contained texture handling for a single-owner render-only raster. glName
// 0 = not loaded (gate off or no sidecar) -> u_useControlMap uploads 0 at draw.
static GLuint s_controlMapTex  = 0;
static int    s_controlMapSide = 0;

// TERRAIN-OVERLAY-V2-PARITY-1: authored cement/pad/runway overlay sidecar
// texture (unit TERRAIN_OVERLAY_SIDECAR_TEXUNIT). Plain GLuint, same
// self-contained single-owner pattern as s_controlMapTex. glName 0 = not
// loaded (gate off or no sidecar) -> u_useOverlaySidecar uploads 0 at draw.
static GLuint s_overlaySidecarTex = 0;
static float  s_overlayBounds[4]  = {0.0f, 0.0f, 0.0f, 0.0f};  // topLeftX,topLeftY(=maxY),sizeX,sizeY

// TERRAIN-SHORELINE-MASK-1: authored land-side wet/foam shoreline mask sidecar
// texture (unit TERRAIN_SHORELINE_TEXUNIT). Plain GLuint, same self-contained
// single-owner pattern as s_overlaySidecarTex. glName 0 = not loaded (gate off
// or no mask) -> u_useShorelineMask uploads 0 at draw (byte-identical -- no
// wet/foam band, legacy screen runShoreline() stays active).
static GLuint s_shorelineMaskTex   = 0;
static float  s_shorelineBounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // topLeftX,topLeftY(=maxY),sizeX,sizeY

// ---------------------------------------------------------------------------
// Shader program + uniform locations (Phase 4).
// ---------------------------------------------------------------------------

static GLuint   s_terrainProgram    = 0;
static int      s_submitZeroStreak  = 0;  // Phase 7.5: consecutive frames with count==0
static GLint    s_locBlockOriginX   = -1;
static GLint    s_locBlockOriginY   = -1;
static GLint    s_locMapSide        = -1;
static GLint    s_locHalfMap        = -1;
static GLint    s_locMvp            = -1;
static GLint    s_locLodStep        = -1;  // Phase 5: per-block LOD stride uniform
static GLint    s_locSkirtDepth     = -1;  // Phase 6: skirt depth uniform
static GLint    s_locForceColor     = -1;  // Phase 7.5: neon debug palette override
static GLint    s_locColormap       = -1;  // Phase 10: merged colormap atlas sampler
static GLint    s_locAtlasTLX       = -1;  // Phase 10: atlas top-left X (world)
static GLint    s_locAtlasTLY       = -1;  // Phase 10: atlas top-left Y (world)
static GLint    s_locAtlasOOW       = -1;  // Phase 10: atlas oneOverWorldUnitsMapSide
static GLint    s_locLightDir       = -1;  // Phase 10 Step 1b: terrainLightDir (sun)
static GLint    s_locDiag           = -1;  // bisection bitmask (MC2_TERRAIN_LOD_CHUNK_DIAG)
static GLint    s_locLightDebugView = -1;  // LIGHTING-DEBUG-VIEWS-1A-CHUNK: u_lightingDebugView
static GLint    s_locPathTint       = -1;  // MC2_SHADER_PATH_TINT debug (u_pathTint)
static GLint    s_locQuadCountX     = -1;  // Phase 10.4: block quad extent X (edge detect)
static GLint    s_locQuadCountY     = -1;  // Phase 10.4: block quad extent Y (edge detect)
static GLint    s_locEdgeStitch     = -1;  // Phase 10.4: packed coarser-neighbour stride
static GLint    s_locShadowTier     = -1;  // Slice B: per-chunk shadow tier (DIAG=40 tint only)
// Phase 10 Step 1c: shadow uniforms (declared by include/shadow.hglsl).
static GLint    s_locShadowMap          = -1;
static GLint    s_locLightSpaceMatrix   = -1;
static GLint    s_locEnableShadows      = -1;
static GLint    s_locShadowSoftness     = -1;
static GLint    s_locDynShadowMap       = -1;
static GLint    s_locDynLightSpaceMat   = -1;
static GLint    s_locEnableDynShadows   = -1;
// Item 1 CSM array-variant locs (only valid when MC2_SHADOW_CSM is ON)
static GLint    s_locDynShadowArray     = -1;
static GLint    s_locDynCascadeMats     = -1;
static GLint    s_locDynCsmCount        = -1;
static GLint    s_locDynCascadeTexel    = -1;  // Stage 3 texel bias
static GLint    s_locCsmDepthSpan       = -1;
// Per-cascade shadow resolution: separate full-map (last) cascade.
static GLint    s_locDynFullMapShadow   = -1;
static GLint    s_locDynFullMapTexel    = -1;
// Mirror gameos_graphics.cpp's file-static terrain shadow texture units (9/10).
static constexpr GLint kChunkTexUnitStaticShadow  = 9;
static constexpr GLint kChunkTexUnitDynamicShadow = 10;
static constexpr GLint kChunkTexUnitDynFullMap    = 13;  // free unit (chunk uses up to 11)
// Phase 10 Step 5a: merged material normal sampler2DArray (own unit, no collision
// with colormap=0 / shadows=9,10). Sourced from gos_GetTerrainNormalArrayTex().
static GLint    s_locMatNormalArray     = -1;
static constexpr GLint kChunkTexUnitMatNormalArray = 5;
extern unsigned int gos_GetTerrainNormalArrayTex();
// Step 5a: live material tunables (same uniforms + source as legacy terrain), so
// the ImGui terrain panel drives the chunk detail too.
static GLint    s_locClassGrass         = -1;
static GLint    s_locClassDirt          = -1;
static GLint    s_locMatTiling          = -1;
static GLint    s_locMatNormalBoost     = -1;
static GLint    s_locMatTilingSnow      = -1;
static GLint    s_locDetailTiling       = -1;
static GLint    s_locDetailStrength     = -1;
static GLint    s_locTintRock           = -1;
static GLint    s_locTintGrass          = -1;
static GLint    s_locTintDirt           = -1;
static GLint    s_locTintStrengthScale  = -1;
static GLint    s_locSnowBrightnessDampen = -1;  // <1 darkens detected snow
// TERRAIN-CONTROLMAP-ALBEDO-1: lifts tintStrength toward 1.0 (0=byte-identical).
static GLint    s_locControlAlbedoStrength = -1;
extern void  gos_GetTerrainMatTiling(float*, float*, float*, float*, float*);
extern void  gos_GetTerrainTintRock(float*, float*, float*);
extern void  gos_GetTerrainTintGrass(float*, float*, float*);
extern void  gos_GetTerrainTintDirt(float*, float*, float*);
extern float gos_GetTerrainTintStrengthScale();
extern float gos_GetTerrainControlAlbedoStrength();  // TERRAIN-CONTROLMAP-ALBEDO-1
// TERRAIN-MATERIAL-LIB-1: promoted frag-literal tints + per-layer roughness/AO
// scalars + the u_useMaterialLib gate. These were previously only wired to the
// legacy tessellated patch-stream path (terrainBindUniformsForPatchStream) --
// the live chunk binder never fetched these locs or uploaded them, so JSON-
// authored terrain_materials.json values never reached this shader in practice.
extern void  gos_GetTerrainTintConcrete(float*, float*, float*);
extern void  gos_GetTerrainTintSnow(float*, float*, float*);
extern void  gos_GetTerrainMatRoughness(float*, float*, float*, float*);
extern void  gos_GetTerrainMatAO(float*, float*, float*, float*);
extern bool  gos_TerrainMaterialLibEnabled();
static GLint    s_locTintConcrete   = -1;
static GLint    s_locTintSnow       = -1;
static GLint    s_locMatRoughness   = -1;
static GLint    s_locMatAO          = -1;
static GLint    s_locUseMaterialLib = -1;
// Remaining legacy tunables (env gates replicated in the upload so default==legacy).
extern float gos_GetTerrainLightingV1Strength();
extern float gos_GetTerrainLightingV2Floor();
extern float gos_GetTerrainNormalsFromHeightStrength();
extern float gos_GetTerrainPOMScale();
extern int   g_terrainMaterialProfile;   // global; 0 = legacy
static GLint s_locLightingV1     = -1;
static GLint s_locLightingV2     = -1;
static GLint s_locNfhStrength    = -1;
static GLint s_locUseRockSlopeBias = -1;  // TERRAIN-SLOPE-BIAS-VISUAL-1 (B4a)
static GLint s_locRockSlopeBiasStr = -1;
static GLint s_locUseTriplanarCliff = -1; // TERRAIN-CLIFF-MATERIAL-TRIPLANAR-1
static GLint s_locCliffTriplanarStr = -1;
static GLint s_locMacroVariation    = -1; // TERRAIN-MACRO-VARIATION-1
static GLint s_locEdgeFeather        = -1; // TERRAIN-EDGE-FEATHER-1
static GLint s_locEdgeFeatherStr     = -1;
static GLint s_locVisualDisplace     = -1; // TERRAIN-VISUAL-HEIGHT-SAMPLE-1
static GLint s_locVisualSide         = -1;
static GLint s_locVisualDisplaceFar  = -1; // TERRAIN-VISUAL-HEIGHT-S2-ALLLOD
static GLint s_locPomParams      = -1;
static GLint s_locMatProfile     = -1;
static GLint s_locControlMap     = -1;  // TERRAIN-CONTROLMAP-SAMPLE-1: u_controlMap sampler
static GLint s_locUseControlMap  = -1;  // u_useControlMap gate uniform
// TERRAIN-OVERLAY-V2-PARITY-1: authored cement/pad/runway overlay sidecar.
static GLint s_locOverlaySidecar    = -1;  // u_overlaySidecar sampler
static GLint s_locUseOverlaySidecar = -1;  // u_useOverlaySidecar gate uniform
static GLint s_locOverlayBounds     = -1;  // u_overlayBounds (vec4 minX,minY,sizeX,sizeY)
// TERRAIN-SHORELINE-MASK-1: authored land-side wet/foam shoreline mask.
static GLint s_locShorelineMask     = -1;  // u_shorelineMask sampler
static GLint s_locUseShorelineMask  = -1;  // u_useShorelineMask gate uniform
static GLint s_locShorelineBounds   = -1;  // u_shorelineBounds (vec4 minX,minY,sizeX,sizeY)
static GLint s_locShaderTime        = -1;  // u_shaderTime (f(worldPos,time)-only foam animation clock)
static GLint s_locShorelineStrength     = -1;  // u_shorelineStrength (wet/damp darken multiplier)
static GLint s_locShorelineFoamStrength = -1;  // u_shorelineFoamStrength (foam rim multiplier)
// Step 5c: cement catalog atlas (tex3) accessors from gos_terrain_indirect.cpp.
extern unsigned int gos_terrain_indirect_getCementAtlasGLTex();
extern int          gos_terrain_indirect_getCementAtlasGridSide();
extern bool         gos_terrain_indirect_isCementAtlasReady();
static GLint    s_locCementAtlas    = -1;
static GLint    s_locUseCement      = -1;
static GLint    s_locCementGridSide = -1;
static GLint    s_locCementWUPT     = -1;
static GLint    s_locCementDiagConnect = -1;  // CEMENT-DIAG-CONNECT-1 gate uniform
static constexpr GLint kChunkTexUnitCement = 3;  // matches legacy tex3
// Stage B: transition mask array (GL_TEXTURE_2D_ARRAY, unit 11).
extern GLuint gos_terrain_indirect_getTransitionMaskArrayGL();
extern bool   gos_terrain_indirect_isTransitionMaskReady();
static GLint s_locTransitionMaskArray = -1;
static GLint s_locUseTransitionMask   = -1;
static constexpr GLint kChunkTexUnitTransitionMask = 11;
extern void  gos_GetTerrainMatNormalBoost(float*, float*, float*, float*);
extern void  gos_GetTerrainClassGrass(float*, float*, float*, float*);
extern void  gos_GetTerrainClassDirt(float*, float*, float*, float*);
extern float gos_GetTerrainDetailTiling();
extern float gos_GetTerrainDetailStrength();

// TERRAIN-DETAIL-ANTI-TILE-1: pack LOD-tier detail-normal fade knobs into the
// unused .yzw of detailNormalStrength (the chunk frag reads only .x today).
//   .y = LOD1 detail strength   .z = enable flag (>0.5)   .w = macro strength
// Gate MC2_TERRAIN_DETAIL_ANTITILE; default OFF => {0,0,0} => byte-identical.
// Read once. Returns pointer to a static float[3] {y,z,w}.
static const float* mc2_chunkDetailAntiTileYZW()
{
    static float s_yzw[3] = { 0.0f, 0.0f, 0.0f };
    static bool s_init = false;
    if (!s_init) {
        s_init = true;
        const char* g = getenv("MC2_TERRAIN_DETAIL_ANTITILE");
        if (g && *g && strcmp(g, "0") != 0) {
            auto envF = [](const char* k, float def) -> float {
                const char* v = getenv(k);
                return (v && *v) ? (float)atof(v) : def;
            };
            s_yzw[0] = envF("MC2_TERRAIN_DETAIL_LOD1_STRENGTH", 0.4f);  // .y LOD1 fade
            s_yzw[1] = 1.0f;                                            // .z enable
            s_yzw[2] = envF("MC2_TERRAIN_DETAIL_MACRO_STRENGTH", 0.0f); // .w macro (chunk: off)
        }
    }
    return s_yzw;
}

// Phase 10: colormap atlas accessors (defined in gos_terrain_indirect.cpp,
// global free functions). Same atlas tex1 + UV params the legacy gos_terrain.frag
// useAtlasColormap path consumes.
extern GLuint gos_terrain_indirect_getAtlasGLTex();
extern float  gos_terrain_indirect_getAtlasMapTopLeftX();
extern float  gos_terrain_indirect_getAtlasMapTopLeftY();
extern float  gos_terrain_indirect_getAtlasOneOverWorldUnits();
// Phase 10 Step 1b: terrain sun direction (gameos.hpp), same value the legacy
// terrain frag's terrainLightDir uniform receives.
extern void   gos_GetTerrainLightDir(float* x, float* y, float* z);

// ---------------------------------------------------------------------------
// Patch geometry cache (Phase 4).
// Each unique (qcX, qcY, lodStep) triple gets one VBO+IBO pair.
// VBO contains int16_t[2] (localX, localY) per vertex.
// IBO contains uint16_t triangle indices.
// ---------------------------------------------------------------------------

struct PatchShape {
    GLuint vbo;          // main patch: int16_t[2] (lx, ly) per vertex
    GLuint ibo;          // main patch: uint16_t triangle indices
    int    vertexCount;
    int    indexCount;
    GLuint skirtVbo;     // Phase 6: int16_t[4] (lx, ly, isSkirt, _pad) per skirt vertex
    GLuint skirtIbo;     // Phase 6: uint16_t triangle indices for skirts
    int    skirtVertexCount;
    int    skirtIndexCount;
    // Phase 10.2b: per-edge index ranges into skirtIbo (build order N,S,W,E) so
    // the driver can draw ONLY the edges whose neighbour LOD differs (per-block
    // edge mask) instead of all four. Offsets/counts are in INDEX units.
    int    skirtEdgeOffset[4];   // 0=N, 1=S, 2=W, 3=E
    int    skirtEdgeCount[4];
};

static std::map<uint32_t, PatchShape> s_patchCache;

// MC2_TERRAIN_LOD_CHECKER_DIAG (default ON, read once, static-cached). When ON,
// the per-quad triangle diagonal honors the same per-cell checkerboard used by
// grounding (worldQuadUVMode, mapdata.cpp:116), the road/cement overlay bake
// (gos_terrain_indirect.cpp:4404) and water/shadow/GPU-compute terrain, instead
// of the fixed TL-BR diagonal. The chunk render was the sole TL-BR outlier, which
// made roads/cement vanish on slopes (user-confirmed fixed). Set =0 to revert to
// the prior fixed TL-BR behaviour (killswitch).
static bool terrainLodCheckerDiagEnabled()
{
    static const bool s_on = [](){
        const char* v = std::getenv("MC2_TERRAIN_LOD_CHECKER_DIAG");
        return !(v && v[0] == '0');   // default ON; only "=0" reverts
    }();
    return s_on;
}

// patchKey now also carries the 2-bit block-origin parity (X&1, Y&1) so the
// shared (qcX,qcY,lodStep) patch cache distinguishes the two checkerboard
// phasings. Gate OFF -> parity bits always 0 (only the fixed-TL-BR patch built).
static uint32_t patchKey(int qcX, int qcY, int lodStep, int parityX, int parityY)
{
    return ((uint32_t)qcX & 0xFF)
         | (((uint32_t)qcY    & 0xFF) << 8)
         | (((uint32_t)lodStep & 0xFF) << 16)
         | (((uint32_t)(parityX & 1)) << 24)
         | (((uint32_t)(parityY & 1)) << 25);
}

// Build sample positions along one axis, always including far edge.
static std::vector<int> makeSamplePositions(int quadCount, int lodStep)
{
    std::vector<int> pos;
    for (int p = 0; p <= quadCount; p += lodStep)
        pos.push_back(p);
    if (pos.back() != quadCount)
        pos.push_back(quadCount);
    return pos;
}

// checkerDiag: when true, emit the per-cell checkerboard diagonal keyed on
// ABSOLUTE world-tile parity (blockOriginParity + mapped sample cell), matching
// worldQuadUVMode. parityX/parityY are blockOriginX&1 / blockOriginY&1.
// Caller MUST only pass checkerDiag=true for lodStep==1 (and not the 4x
// visualDisplace path) — coarse LOD per-cell parity is ill-defined.
static const PatchShape& getOrBuildPatch(int qcX, int qcY, int lodStep,
                                         bool checkerDiag, int parityX, int parityY)
{
    // parity bits only meaningful when the checkerboard is active; keep the
    // fixed-TL-BR cache entry under parity (0,0) so gate-OFF is byte-identical.
    const int keyParX = checkerDiag ? (parityX & 1) : 0;
    const int keyParY = checkerDiag ? (parityY & 1) : 0;
    uint32_t key = patchKey(qcX, qcY, lodStep, keyParX, keyParY);
    auto it = s_patchCache.find(key);
    if (it != s_patchCache.end()) return it->second;

    auto xs = makeSamplePositions(qcX, lodStep);
    auto ys = makeSamplePositions(qcY, lodStep);

    struct LocalVertex { int16_t lx, ly; };
    std::vector<LocalVertex> verts;
    verts.reserve(xs.size() * ys.size());
    for (int yy : ys)
        for (int xx : xs)
            verts.push_back({(int16_t)xx, (int16_t)yy});

    // Two CCW triangles per quad cell.
    //   FIXED / BOTTOMRIGHT (TL-BR diagonal): {TL,BL,BR} + {TL,BR,TR}
    //   BOTTOMLEFT          (TR-BL diagonal): {TL,BL,TR} + {BL,BR,TR}
    // Both keep the SAME front-face (CCW) winding as the original fixed split,
    // so backface culling is unchanged. The diagonal choice mirrors
    // worldQuadUVMode(absRow,absCol): BOTTOMRIGHT when (absY&1)==(absX&1).
    // absX = parityX*blockBase + xs[i] ; we have blockOriginX&1 in parityX and
    // the sample's local cell coord xs[i] -> absolute cell parity is the XOR.
    // NOTE xs[i] is the LOD-decimated map-cell coordinate (lodStep==1 here, so
    // xs[i] is exactly the cell index), used directly — NOT the raw loop i.
    std::vector<uint16_t> indices;
    int W = (int)xs.size();
    for (int j = 0; j < (int)ys.size() - 1; ++j) {
        for (int i = 0; i < (int)xs.size() - 1; ++i) {
            uint16_t tl = (uint16_t)(j*W+i);
            uint16_t tr = (uint16_t)(j*W+i+1);
            uint16_t bl = (uint16_t)((j+1)*W+i);
            uint16_t br = (uint16_t)((j+1)*W+i+1);

            bool bottomRight = true;  // fixed default == BOTTOMRIGHT (TL-BR)
            if (checkerDiag) {
                // Absolute map cell of this quad's TL corner. xs/ys are local
                // sample positions; with lodStep==1 they equal the local cell
                // index. Add block-origin parity for absolute world parity.
                const int absX = (parityX & 1) ^ (xs[i] & 1);
                const int absY = (parityY & 1) ^ (ys[j] & 1);
                // worldQuadUVMode: BOTTOMRIGHT when (tileR&1)==(tileC&1).
                bottomRight = (absY == absX);
            }

            if (bottomRight) {
                indices.insert(indices.end(), {tl, bl, br, tl, br, tr});
            } else {
                indices.insert(indices.end(), {tl, bl, tr, bl, br, tr});
            }
        }
    }

    PatchShape ps;
    ps.vertexCount = (int)verts.size();
    ps.indexCount  = (int)indices.size();

    glGenBuffers(1, &ps.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, ps.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(verts.size() * sizeof(LocalVertex)),
                 verts.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &ps.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ps.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)(indices.size() * sizeof(uint16_t)),
                 indices.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    // -----------------------------------------------------------------------
    // Phase 6: Build skirt geometry — four edge strips, each a quad-strip.
    // SkirtVertex: lx, ly, isSkirt (0=surface, 1=below), _pad.
    // The vertex shader reads isSkirt and applies: h -= isSkirt * u_skirtDepth.
    // Winding is CCW viewed from outside; backface culling is disabled during
    // skirt draws so winding does not need to be perfect in this first pass.
    // -----------------------------------------------------------------------
    struct SkirtVertex { int16_t lx, ly, isSkirt, _pad; };
    std::vector<SkirtVertex> skirtVerts;
    std::vector<uint16_t>    skirtIdx;

    // Each edge strip has 2 * edgeLen vertices.
    // Indices: for each quad in the strip, 2 triangles connecting top[i]/bot[i] to top[i+1]/bot[i+1].
    // Layout within strip (base offset B, 2 verts per column: top=B+2*i, bot=B+2*i+1):
    //   tri1: top[i], bot[i], bot[i+1]  → B+2*i, B+2*i+1, B+2*(i+1)+1
    //   tri2: top[i], bot[i+1], top[i+1] → B+2*i, B+2*(i+1)+1, B+2*(i+1)
    // This is CCW when the strip faces the camera from the outside (correct for all four edges
    // when backface culling is disabled, so no per-edge winding correction needed).

    auto buildEdge = [&](const std::vector<int>& uPos, int fixedCoord, bool fixedIsY)
    {
        int base = (int)skirtVerts.size();
        int n    = (int)uPos.size();
        for (int i = 0; i < n; ++i) {
            int16_t lx = fixedIsY ? (int16_t)uPos[i] : (int16_t)fixedCoord;
            int16_t ly = fixedIsY ? (int16_t)fixedCoord : (int16_t)uPos[i];
            skirtVerts.push_back({lx, ly, 0, 0});  // surface
            skirtVerts.push_back({lx, ly, 1, 0});  // below
        }
        for (int i = 0; i < n - 1; ++i) {
            uint16_t t0 = (uint16_t)(base + 2*i);
            uint16_t b0 = (uint16_t)(base + 2*i + 1);
            uint16_t t1 = (uint16_t)(base + 2*(i+1));
            uint16_t b1 = (uint16_t)(base + 2*(i+1) + 1);
            skirtIdx.insert(skirtIdx.end(), {t0, b0, b1, t0, b1, t1});
        }
    };

    // Phase 10.2b: record each edge's index range (build order N,S,W,E).
    ps.skirtEdgeOffset[0] = (int)skirtIdx.size(); buildEdge(xs, (int)ys.front(), false); ps.skirtEdgeCount[0] = (int)skirtIdx.size() - ps.skirtEdgeOffset[0]; // North (y=ys[0])
    ps.skirtEdgeOffset[1] = (int)skirtIdx.size(); buildEdge(xs, (int)ys.back(),  false); ps.skirtEdgeCount[1] = (int)skirtIdx.size() - ps.skirtEdgeOffset[1]; // South (y=ys.back)
    ps.skirtEdgeOffset[2] = (int)skirtIdx.size(); buildEdge(ys, (int)xs.front(), true ); ps.skirtEdgeCount[2] = (int)skirtIdx.size() - ps.skirtEdgeOffset[2]; // West  (x=xs[0])
    ps.skirtEdgeOffset[3] = (int)skirtIdx.size(); buildEdge(ys, (int)xs.back(),  true ); ps.skirtEdgeCount[3] = (int)skirtIdx.size() - ps.skirtEdgeOffset[3]; // East  (x=xs.back)

    ps.skirtVertexCount = (int)skirtVerts.size();
    ps.skirtIndexCount  = (int)skirtIdx.size();

    glGenBuffers(1, &ps.skirtVbo);
    glBindBuffer(GL_ARRAY_BUFFER, ps.skirtVbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(skirtVerts.size() * sizeof(SkirtVertex)),
                 skirtVerts.data(), GL_STATIC_DRAW);

    glGenBuffers(1, &ps.skirtIbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ps.skirtIbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)(skirtIdx.size() * sizeof(uint16_t)),
                 skirtIdx.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    s_patchCache[key] = ps;
    return s_patchCache.at(key);
}

// ---------------------------------------------------------------------------
// VAO for patch draws — reused every frame, attributes re-pointed per batch.
// ---------------------------------------------------------------------------

static GLuint s_patchVao = 0;

// ---------------------------------------------------------------------------
// Init / Destroy — called from gosRenderer::init / gosRenderer::destroy.
// ---------------------------------------------------------------------------

void gos_TerrainLodChunk_Init()
{
    if (s_heightSsbo.backendHandle != 0)
        return; // idempotent

    {
        GLuint heightBuf = 0;
        glGenBuffers(1, &heightBuf);
        s_heightSsbo.backendHandle = static_cast<uint32_t>(heightBuf);
    }
    if (s_heightSsbo.backendHandle == 0)
    {
        fprintf(stderr, "[TerrainLodChunk] glGenBuffers failed for height SSBO\n");
        fflush(stderr);
        return;
    }
    {
        GLuint typeBuf = 0, cementBuf = 0;
        glGenBuffers(1, &typeBuf);   // Step 5b: terrainType SSBO (concrete)
        glGenBuffers(1, &cementBuf); // Step 5c: cement word SSBO
        s_typeSsbo.backendHandle   = static_cast<uint32_t>(typeBuf);
        s_cementSsbo.backendHandle = static_cast<uint32_t>(cementBuf);
    }

    // Shader program (Phase 4) — load unconditionally; SubmitDrawCommands gates
    // on the env var so no pixels change unless MC2_TERRAIN_LOD_CHUNK=1.
    {
        // Item 1: inject MC2_SHADOW_CSM define so terrain_lod_chunk.frag (which
        // #includes shadow.hglsl) compiles the array-sampler variant when ON.
        std::string prefix = "#version 430\n";
        if (mc2ShadowCsmEnabled()) {
            char csmDef[64];
            snprintf(csmDef, sizeof(csmDef),
                     "#define MC2_SHADOW_CSM 1\n#define MC2_SHADOW_CSM_MAX %d\n",
                     mc2ShadowCsmCount());
            prefix += csmDef;
        }
        glsl_program* prog = glsl_program::makeProgram(
            "terrain_lod_chunk",
            "shaders/terrain_lod_chunk.vert",
            "shaders/terrain_lod_chunk.frag",
            prefix.c_str());
        if (!prog || !prog->shp_)
        {
            fprintf(stderr, "[TerrainLodChunk] WARNING: shader compile failed"
                            " -- LOD chunk draw disabled\n");
            fflush(stderr);
        }
        else
        {
            s_terrainProgram  = prog->shp_;
            s_locBlockOriginX = glGetUniformLocation(s_terrainProgram, "u_blockOriginX");
            s_locBlockOriginY = glGetUniformLocation(s_terrainProgram, "u_blockOriginY");
            s_locMapSide      = glGetUniformLocation(s_terrainProgram, "u_mapSide");
            s_locHalfMap      = glGetUniformLocation(s_terrainProgram, "u_halfMap");
            s_locMvp          = glGetUniformLocation(s_terrainProgram, "u_worldToClipGL");
            s_locLodStep      = glGetUniformLocation(s_terrainProgram, "u_lodStep");    // Phase 5
            s_locSkirtDepth   = glGetUniformLocation(s_terrainProgram, "u_skirtDepth"); // Phase 6
            s_locForceColor   = glGetUniformLocation(s_terrainProgram, "u_forceColor"); // Phase 7.5
            s_locColormap     = glGetUniformLocation(s_terrainProgram, "u_colormap");   // Phase 10
            s_locAtlasTLX     = glGetUniformLocation(s_terrainProgram, "u_atlasTopLeftX");
            s_locAtlasTLY     = glGetUniformLocation(s_terrainProgram, "u_atlasTopLeftY");
            s_locAtlasOOW     = glGetUniformLocation(s_terrainProgram, "u_atlasOneOverWorldUnits");
            s_locLightDir     = glGetUniformLocation(s_terrainProgram, "terrainLightDir");
            s_locDiag         = glGetUniformLocation(s_terrainProgram, "u_diag");
            s_locLightDebugView = glGetUniformLocation(s_terrainProgram, "u_lightingDebugView"); // LIGHTING-DEBUG-VIEWS-1A-CHUNK
            s_locPathTint     = glGetUniformLocation(s_terrainProgram, "u_pathTint");
            s_locQuadCountX   = glGetUniformLocation(s_terrainProgram, "u_quadCountX");
            s_locQuadCountY   = glGetUniformLocation(s_terrainProgram, "u_quadCountY");
            s_locEdgeStitch   = glGetUniformLocation(s_terrainProgram, "u_edgeStitch");
            s_locShadowTier   = glGetUniformLocation(s_terrainProgram, "u_shadowTier"); // Slice B
            s_locShadowMap        = glGetUniformLocation(s_terrainProgram, "shadowMap");
            s_locLightSpaceMatrix = glGetUniformLocation(s_terrainProgram, "lightSpaceMatrix");
            s_locEnableShadows    = glGetUniformLocation(s_terrainProgram, "enableShadows");
            s_locShadowSoftness   = glGetUniformLocation(s_terrainProgram, "shadowSoftness");
            s_locDynShadowMap     = glGetUniformLocation(s_terrainProgram, "dynamicShadowMap");
            s_locDynLightSpaceMat = glGetUniformLocation(s_terrainProgram, "dynamicLightSpaceMatrix");
            s_locEnableDynShadows = glGetUniformLocation(s_terrainProgram, "enableDynamicShadows");
            s_locDynShadowArray   = glGetUniformLocation(s_terrainProgram, "dynamicShadowArray");
            s_locDynCascadeMats   = glGetUniformLocation(s_terrainProgram, "dynamicCascadeMatrices");
            s_locDynCsmCount      = glGetUniformLocation(s_terrainProgram, "dynamicCsmCount");
            s_locDynCascadeTexel  = glGetUniformLocation(s_terrainProgram, "dynamicCascadeTexelWorld");
            s_locCsmDepthSpan     = glGetUniformLocation(s_terrainProgram, "csmDepthSpan");
            s_locDynFullMapShadow = glGetUniformLocation(s_terrainProgram, "dynamicFullMapShadow");
            s_locDynFullMapTexel  = glGetUniformLocation(s_terrainProgram, "dynamicFullMapTexelWorld");
            s_locMatNormalArray   = glGetUniformLocation(s_terrainProgram, "matNormalArray");
            s_locClassGrass     = glGetUniformLocation(s_terrainProgram, "terrainClassGrass");
            s_locClassDirt      = glGetUniformLocation(s_terrainProgram, "terrainClassDirt");
            s_locMatTiling      = glGetUniformLocation(s_terrainProgram, "matTiling");
            s_locMatNormalBoost = glGetUniformLocation(s_terrainProgram, "matNormalBoost");
            s_locMatTilingSnow  = glGetUniformLocation(s_terrainProgram, "matTilingSnow");
            s_locDetailTiling   = glGetUniformLocation(s_terrainProgram, "detailNormalTiling");
            s_locDetailStrength = glGetUniformLocation(s_terrainProgram, "detailNormalStrength");
            s_locTintRock          = glGetUniformLocation(s_terrainProgram, "tintRock");
            s_locTintGrass         = glGetUniformLocation(s_terrainProgram, "tintGrass");
            s_locTintDirt          = glGetUniformLocation(s_terrainProgram, "tintDirt");
            s_locTintStrengthScale = glGetUniformLocation(s_terrainProgram, "tintStrengthScale");
            s_locSnowBrightnessDampen = glGetUniformLocation(s_terrainProgram, "snowBrightnessDampen");
            s_locControlAlbedoStrength = glGetUniformLocation(s_terrainProgram, "u_controlAlbedoStrength");  // TERRAIN-CONTROLMAP-ALBEDO-1
            // TERRAIN-MATERIAL-LIB-1: wire the live chunk binder (was dead-bridge-only).
            s_locTintConcrete   = glGetUniformLocation(s_terrainProgram, "tintConcrete");
            s_locTintSnow       = glGetUniformLocation(s_terrainProgram, "tintSnow");
            s_locMatRoughness   = glGetUniformLocation(s_terrainProgram, "matRoughness");
            s_locMatAO          = glGetUniformLocation(s_terrainProgram, "matAO");
            s_locUseMaterialLib = glGetUniformLocation(s_terrainProgram, "u_useMaterialLib");
            s_locLightingV1  = glGetUniformLocation(s_terrainProgram, "terrainLightingV1Strength");
            s_locLightingV2  = glGetUniformLocation(s_terrainProgram, "terrainLightingV2ShadowFillFloor");
            s_locNfhStrength = glGetUniformLocation(s_terrainProgram, "terrainNormalsFromHeightStrength");
            s_locUseRockSlopeBias = glGetUniformLocation(s_terrainProgram, "useRockSlopeBias");
            s_locRockSlopeBiasStr = glGetUniformLocation(s_terrainProgram, "rockSlopeBiasStrength");
            s_locUseTriplanarCliff = glGetUniformLocation(s_terrainProgram, "useTriplanarCliff");
            s_locCliffTriplanarStr = glGetUniformLocation(s_terrainProgram, "cliffTriplanarStrength");
            s_locMacroVariation    = glGetUniformLocation(s_terrainProgram, "macroVariationStrength");
            s_locEdgeFeather       = glGetUniformLocation(s_terrainProgram, "u_edgeFeather");
            s_locEdgeFeatherStr    = glGetUniformLocation(s_terrainProgram, "u_edgeFeatherStrength");
            s_locVisualDisplace    = glGetUniformLocation(s_terrainProgram, "u_visualDisplace");
            s_locVisualSide        = glGetUniformLocation(s_terrainProgram, "u_visualSide");
            s_locVisualDisplaceFar = glGetUniformLocation(s_terrainProgram, "u_visualDisplaceFar");
            s_locPomParams   = glGetUniformLocation(s_terrainProgram, "pomParams");
            s_locMatProfile  = glGetUniformLocation(s_terrainProgram, "g_terrainMaterialProfile");
            s_locCementAtlas    = glGetUniformLocation(s_terrainProgram, "u_cementAtlas");
            s_locUseCement      = glGetUniformLocation(s_terrainProgram, "u_useCement");
            s_locCementGridSide = glGetUniformLocation(s_terrainProgram, "u_cementGridSide");
            s_locCementWUPT     = glGetUniformLocation(s_terrainProgram, "u_cementWUPT");
            s_locCementDiagConnect = glGetUniformLocation(s_terrainProgram, "u_cementDiagConnect");
            s_locTransitionMaskArray = glGetUniformLocation(s_terrainProgram, "u_transitionMaskArray");
            s_locUseTransitionMask   = glGetUniformLocation(s_terrainProgram, "u_useTransitionMask");
            s_locControlMap    = glGetUniformLocation(s_terrainProgram, "u_controlMap");    // TERRAIN-CONTROLMAP-SAMPLE-1
            s_locUseControlMap = glGetUniformLocation(s_terrainProgram, "u_useControlMap");
            s_locOverlaySidecar    = glGetUniformLocation(s_terrainProgram, "u_overlaySidecar");    // TERRAIN-OVERLAY-V2-PARITY-1
            s_locUseOverlaySidecar = glGetUniformLocation(s_terrainProgram, "u_useOverlaySidecar");
            s_locOverlayBounds     = glGetUniformLocation(s_terrainProgram, "u_overlayBounds");
            s_locShorelineMask     = glGetUniformLocation(s_terrainProgram, "u_shorelineMask");    // TERRAIN-SHORELINE-MASK-1
            s_locUseShorelineMask  = glGetUniformLocation(s_terrainProgram, "u_useShorelineMask");
            s_locShorelineBounds   = glGetUniformLocation(s_terrainProgram, "u_shorelineBounds");
            s_locShaderTime        = glGetUniformLocation(s_terrainProgram, "u_shaderTime");
            s_locShorelineStrength     = glGetUniformLocation(s_terrainProgram, "u_shorelineStrength");
            s_locShorelineFoamStrength = glGetUniformLocation(s_terrainProgram, "u_shorelineFoamStrength");
            printf("[TerrainLodChunk] shader loaded prog=%u "
                   "locs: originX=%d originY=%d mapSide=%d halfMap=%d mvp=%d lodStep=%d skirtDepth=%d forceColor=%d\n",
                   (unsigned)s_terrainProgram,
                   s_locBlockOriginX, s_locBlockOriginY,
                   s_locMapSide, s_locHalfMap, s_locMvp, s_locLodStep, s_locSkirtDepth, s_locForceColor);
            fflush(stdout);
            // Phase 7.5: separate startup confirmation line for easy grep.
            printf("[TerrainLOD v1] shader program compiled OK (program=%u)\n", s_terrainProgram);
            fflush(stdout);
        }
    }

    // VAO — one global; attributes are re-pointed each draw in SubmitDrawCommands.
    glGenVertexArrays(1, &s_patchVao);
}

void gos_TerrainLodChunk_Destroy()
{
    // Free patch cache VBOs/IBOs (main + Phase 6 skirt).
    for (auto& kv : s_patchCache) {
        glDeleteBuffers(1, &kv.second.vbo);
        glDeleteBuffers(1, &kv.second.ibo);
        glDeleteBuffers(1, &kv.second.skirtVbo);
        glDeleteBuffers(1, &kv.second.skirtIbo);
    }
    s_patchCache.clear();

    if (s_patchVao != 0) {
        glDeleteVertexArrays(1, &s_patchVao);
        s_patchVao = 0;
    }

    // Shader program is owned by glsl_program cache; delete by name.
    if (s_terrainProgram != 0) {
        glsl_program::deleteProgram("terrain_lod_chunk");
        s_terrainProgram    = 0;
        s_locBlockOriginX   = -1;
        s_locBlockOriginY   = -1;
        s_locMapSide        = -1;
        s_locHalfMap        = -1;
        s_locMvp            = -1;
        s_locLodStep        = -1;
        s_locSkirtDepth     = -1;
        s_locForceColor     = -1;
    }

    // TERRAIN-CONTROLMAP-SAMPLE-1: free the authored control-map texture (if loaded).
    if (s_controlMapTex != 0)
    {
        glDeleteTextures(1, &s_controlMapTex);
        s_controlMapTex  = 0;
        s_controlMapSide = 0;
    }

    // TERRAIN-SHORELINE-MASK-1: free the authored shoreline mask texture (if loaded).
    if (s_shorelineMaskTex != 0)
    {
        glDeleteTextures(1, &s_shorelineMaskTex);
        s_shorelineMaskTex = 0;
    }

    if (s_visualHeightSsbo.backendHandle != 0)
    {
        GLuint visualBuf = static_cast<GLuint>(s_visualHeightSsbo.backendHandle);
        glDeleteBuffers(1, &visualBuf);
        s_visualHeightSsbo.backendHandle = 0;

        // TERRAIN-VISUAL-HEIGHT-SSBO-OWNER-1: mark the slot unavailable on teardown.
        RenderCore::RenderResourceDesc invalid;
        invalid.id = RenderCore::RenderResourceId::TerrainVisualHeightSsbo;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }
    if (s_heightSsbo.backendHandle != 0)
    {
        GLuint heightBuf = static_cast<GLuint>(s_heightSsbo.backendHandle);
        glDeleteBuffers(1, &heightBuf);
        s_heightSsbo.backendHandle = 0;
        s_mapSide    = 0;
        s_halfMap    = 0.0f;

        // REGISTRY-TERRAIN-SSBO-1: mark the slot unavailable on teardown.
        RenderCore::RenderResourceDesc invalid;
        invalid.id = RenderCore::RenderResourceId::TerrainHeightSsbo;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }
    if (s_typeSsbo.backendHandle != 0)
    {
        GLuint typeBuf = static_cast<GLuint>(s_typeSsbo.backendHandle);
        glDeleteBuffers(1, &typeBuf);
        s_typeSsbo.backendHandle = 0;

        // TERRAIN-LODCHUNK-SSBO-OWNER-1: mark the slot unavailable on teardown.
        RenderCore::RenderResourceDesc invalid;
        invalid.id = RenderCore::RenderResourceId::TerrainTypeSsbo;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }
    if (s_cementSsbo.backendHandle != 0)
    {
        GLuint cementBuf = static_cast<GLuint>(s_cementSsbo.backendHandle);
        glDeleteBuffers(1, &cementBuf);
        s_cementSsbo.backendHandle = 0;

        // TERRAIN-LODCHUNK-SSBO-OWNER-1: mark the slot unavailable on teardown.
        RenderCore::RenderResourceDesc invalid;
        invalid.id = RenderCore::RenderResourceId::TerrainCementSsbo;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }
}

// ---------------------------------------------------------------------------
// Submit draw commands — Phase 4 real implementation.
// Called only from Terrain::flushDrawCommands() in mclib/terrain.cpp.
// count==0 is a strict no-op. Restores GL state on exit.
// ---------------------------------------------------------------------------

#ifdef MC2_VULKAN_ISLAND
// ---------------------------------------------------------------------------
// VK-P10 TERRAIN TIER-1 feed. When MC2_VK_TERRAIN_TIER1 (+ shared depth) is armed,
// SubmitDrawCommands captures the LOD0 (lodStep==1) patch list + shared params into
// this scratch, SKIPS the GL LOD0 main+skirt draws, and the VK terrain island
// co-writes those patches into the shared scene GBuffer instead. Default-OFF ->
// none of this runs -> the GL path draws every band exactly as before.
// ---------------------------------------------------------------------------
static uint64_t g_terrainHeightVersion = 0;  // bumps on UploadHeightFull
static uint64_t g_terrainCementVersion = 0;  // bumps on UploadCementWordsFull
static std::vector<Mc2TerrainTier1Patch> s_vkTerrainPatches;  // this frame's LOD0 patches
static float    s_vkTerrainMvp[16] = {0};
static bool     s_vkTerrainFeedValid = false;

extern "C" int mc2_terrain_tier1_get_frame_feed(Mc2TerrainTier1Feed* out)
{
    if (!out) return 0;
    if (!s_vkTerrainFeedValid || s_vkTerrainPatches.empty()) return 0;
    if (s_heightSsbo.backendHandle == 0 || s_mapSide <= 0) return 0;
    out->patches    = s_vkTerrainPatches.data();
    out->patchCount = (uint32_t)s_vkTerrainPatches.size();
    std::memcpy(out->worldToClipGL, s_vkTerrainMvp, sizeof(out->worldToClipGL));
    out->mapSide       = s_mapSide;
    out->halfMap       = s_halfMap;
    out->glHeightSsbo  = static_cast<uint32_t>(s_heightSsbo.backendHandle);
    out->heightBytes   = (uint64_t)s_mapSide * (uint64_t)s_mapSide * sizeof(float);
    out->heightVersion = g_terrainHeightVersion;
    // COLORMAP-BASE sub-slice: hand the GL colormap atlas tex + UV params to the
    // island so it can copy-once import the colormap and sample the REAL base color.
    out->glColormapTex          = static_cast<uint32_t>(gos_terrain_indirect_getAtlasGLTex());
    out->atlasTopLeftX          = gos_terrain_indirect_getAtlasMapTopLeftX();
    out->atlasTopLeftY          = gos_terrain_indirect_getAtlasMapTopLeftY();
    out->atlasOneOverWorldUnits = gos_terrain_indirect_getAtlasOneOverWorldUnits();

    // CEMENT/ROAD atlas sub-slice: hand the GL cement atlas + per-vertex cement-word
    // SSBO + the cement uniforms the GL LOD-chunk frag reads. useCement mirrors the GL
    // gate (gos_terrain_lod_chunk.cpp:1122-1124): atlas ready AND cement SSBO live.
    const bool cementReady = gos_terrain_indirect_isCementAtlasReady();
    const bool cementLive  = cementReady && s_cementSsbo.backendHandle != 0;
    out->glCementAtlasTex   = static_cast<uint32_t>(gos_terrain_indirect_getCementAtlasGLTex());
    out->cementGridSide     = gos_terrain_indirect_getCementAtlasGridSide();
    out->cementWUPT         = 128.0f;  // Terrain::worldUnitsPerVertex (GL uploads 128.0)
    out->useCement          = cementLive ? 1 : 0;
    out->useTransitionMask  = gos_terrain_indirect_isTransitionMaskReady() ? 1 : 0;
    {
        int diagOn = 0;
        if (const char* e = getenv("MC2_TERRAIN_CEMENT_DIAG_CONNECT")) diagOn = (e[0] && e[0] != '0') ? 1 : 0;
        out->cementDiagConnect = diagOn;
    }
    out->glCementSsbo       = static_cast<uint32_t>(s_cementSsbo.backendHandle);
    out->cementBytes        = (uint64_t)s_mapSide * (uint64_t)s_mapSide * sizeof(unsigned int);
    out->cementVersion      = g_terrainCementVersion;

    // VK-TERRAIN-LIGHTING sub-slice: hand the sun dir + the STATIC sun shadow map + the
    // static light-space matrix to the island so the VK terrain frag can reproduce the GL
    // NdotL relief band + baked static PCF shadow (terrain_lod_chunk.frag:889-928). Gate
    // MC2_VK_TERRAIN_LIGHTING (default-off -> lightingEnabled=0 -> VK stays flat, byte-safe).
    {
        // Umbrella family member: MC2_VK_SHARED_GBUFFER=1 arms it; explicit
        // MC2_VK_TERRAIN_LIGHTING=0/1 overrides.
        int lightingOn = mc2vkgate::subGate("MC2_VK_TERRAIN_LIGHTING") ? 1 : 0;
        out->lightingEnabled = lightingOn;
        float lx = 0.f, ly = 0.f, lz = 1.f;
        gos_GetTerrainLightDir(&lx, &ly, &lz);
        out->terrainLightDir[0] = lx; out->terrainLightDir[1] = ly; out->terrainLightDir[2] = lz;
        // Default: no shadow (lit-only) until the GL static shadow map is live.
        out->shadowsEnabled     = 0;
        out->glStaticShadowTex  = 0;
        out->staticShadowSize   = 0;
        out->staticShadowGeneration = 0;
        out->shadowSoftness     = 2.5f;   // shadow.hglsl default
        std::memset(out->staticLightSpaceMatrix, 0, sizeof(out->staticLightSpaceMatrix));
        gosPostProcess* pp = getGosPostProcess();
        if (lightingOn && pp && pp->shadowsEnabled_) {
            const GLuint sTex = pp->getShadowTexture();
            if (sTex != 0) {
                out->shadowsEnabled    = 1;
                out->glStaticShadowTex = static_cast<uint32_t>(sTex);
                out->staticShadowSize  = 4096;   // gos_postprocess.cpp:5113 shadowMapSize_
                out->staticShadowGeneration = gos_GetStaticShadowGeneration();
                std::memcpy(out->staticLightSpaceMatrix, pp->getLightSpaceMatrix(),
                            sizeof(out->staticLightSpaceMatrix));
            }
        }

        // VK-TERRAIN-DYNSHADOW: the DYNAMIC (building/mech) shadow map. Mirrors the GL
        // LOD-chunk enable gate (gos_terrain_lod_chunk.cpp:1120,1131): shadowsEnabled_
        // && getDynamicShadowFBO(). Non-CSM legacy single-map path only (CSM array path
        // deferred — matches the mechanism-proven shadow-compare spike). Copy source is
        // the RAW engine GL texture so we never toggle compare-mode on a VK interop tex.
        out->dynShadowsEnabled = 0;
        out->glDynamicShadowTex = 0;
        out->dynamicShadowSize = 0;
        std::memset(out->dynamicLightSpaceMatrix, 0, sizeof(out->dynamicLightSpaceMatrix));
        // Umbrella member; requires lighting (its own precondition) + subGate.
        int dynShadowGate = 0;
        if (out->lightingEnabled)
            dynShadowGate = mc2vkgate::subGate("MC2_VK_TERRAIN_DYNSHADOW") ? 1 : 0;
        if (dynShadowGate && pp && pp->shadowsEnabled_ && pp->getDynamicShadowFBO()
            && !mc2ShadowCsmEnabled()) {
            const GLuint dTex = pp->getDynamicShadowTextureRaw();
            const int    dSize = pp->getDynamicShadowMapSize();
            if (dTex != 0 && dSize > 0) {
                out->dynShadowsEnabled = 1;
                out->glDynamicShadowTex = static_cast<uint32_t>(dTex);
                out->dynamicShadowSize = dSize;
                std::memcpy(out->dynamicLightSpaceMatrix, pp->getDynamicLightSpaceMatrix(),
                            sizeof(out->dynamicLightSpaceMatrix));
            }
        }
        // VK-FLIP-FIELD-REPORT-1 CSM SUBSET: when CSM IS enabled (launcher default), the
        // single-map dyn path above is skipped (!mc2ShadowCsmEnabled), so VK terrain got
        // NO object shadow up close. GL LOD0 terrain samples calcDynamicShadow's CSM
        // cascades (near array + full-map catch-all). Full near-cascade-array interop is a
        // large lift (sampler2DArrayShadow import + N matrices) and is DEFERRED. The
        // BIGGEST-SAFE-SUBSET: feed the CSM FULL-MAP cascade (the last cascade — a single
        // GL_TEXTURE_2D DEPTH_COMPONENT texture, byte-shape-identical to the single dyn map
        // this island already imports) through the SAME binding-7 slot + computeBakedShadow.
        // Result: VK terrain gets whole-map object shadows (coarser than GL's near cascade
        // near the camera, but present) — closes the "no shadows at all" gap with the
        // proven single-map mechanism, zero new interop surface. Gate MC2_VK_TERRAIN_CSM
        // (requires the dynshadow gate chain; default-OFF -> byte-identical to today).
        // MC2_VK_TERRAIN_CSM is PRESENCE-GATED (explicit "=1" only, does NOT follow the
        // umbrella) because the full-map subset is not yet parity-proven default-on. Once
        // the oracle blesses it, it can join the umbrella family. Kept independent of
        // subGate() so the umbrella flip does NOT silently arm it.
        auto csmSubsetOn = []() {
            const char* v = std::getenv("MC2_VK_TERRAIN_CSM");
            return v && v[0] == '1';
        };
        if (dynShadowGate && !out->dynShadowsEnabled && pp && pp->shadowsEnabled_
            && mc2ShadowCsmEnabled()
            && csmSubsetOn()) {
            const GLuint fTex  = pp->getDynamicFullMapTexture();
            const int    fSize = pp->getDynamicFullMapSize();
            const int    ccount = pp->getDynamicShadowCascadeCount();
            const float* mats  = pp->getDynamicCascadeMatrices();
            if (fTex != 0 && fSize > 0 && ccount > 0 && mats) {
                out->dynShadowsEnabled = 1;
                out->glDynamicShadowTex = static_cast<uint32_t>(fTex);
                out->dynamicShadowSize = fSize;
                // Full-map cascade = the LAST cascade; its matrix is row (ccount-1).
                std::memcpy(out->dynamicLightSpaceMatrix, mats + (ccount - 1) * 16,
                            sizeof(out->dynamicLightSpaceMatrix));
            }
        }
    }

    // VK-TERRAIN-DETAIL sub-slice: hand the material detail-normal ARRAY tex + the GL
    // detail tuning scalars to the island so the VK terrain frag can perturb the surface
    // normal (macro slope from height + material detail normal) EXACTLY like GL
    // (terrain_lod_chunk.frag:740-808) -> grass reads with NdotL relief instead of flat.
    // Gate MC2_VK_TERRAIN_DETAIL (requires MC2_VK_TERRAIN_LIGHTING; default-off ->
    // detailEnabled=0 -> VK N stays flat-up, byte-identical to the lighting-only frame).
    {
        // Umbrella member; requires lighting (its own precondition) + subGate.
        int detailOn = 0;
        if (out->lightingEnabled)
            detailOn = mc2vkgate::subGate("MC2_VK_TERRAIN_DETAIL") ? 1 : 0;
        out->detailEnabled       = detailOn;
        out->glMatNormalArrayTex = detailOn ? (uint32_t)gos_GetTerrainNormalArrayTex() : 0u;
        // GL uniform values (gameos_graphics.cpp member defaults; all 1.0). The VK frag
        // bakes per-material tiling/boost/class thresholds as const, so only these three
        // scalars flow through the UBO. See terrain_tier1.frag MATV_* consts.
        out->nfhStrength    = 1.0f;   // terrainNormalsFromHeightStrength (terrain_nfh_strength_)
        out->detailTiling   = 1.0f;   // detailNormalTiling.x (terrain_detail_tiling_)
        out->detailStrength = 1.0f;   // detailNormalStrength.x (terrain_detail_strength_)
    }
    return 1;
}
#endif

void gos_TerrainLodChunk_SubmitDrawCommands(
    const TerrainDrawCommand* cmds,
    const float*              skirtDepths,
    const unsigned char*      skirtEdgeMasks,
    const unsigned int*       edgeStitch,
    const int*                shadowTiers,
    int                       count)
{
    if (count == 0) return;
    if (s_terrainProgram == 0 || s_heightSsbo.backendHandle == 0) return;
    if (s_patchVao == 0) return;

    // SAME-ORDER-EXECUTOR-VALIDATE-1: top-level validate-only wrapper (gate MC2_FRAMEGRAPH_EXECUTOR).
    // No-op when gate unset (byte-identical). PIN INVARIANT: additive only — no GL state change,
    // no reorder. markTerrainDrawn/g_dispatchMvp16/knownEarly undisturbed.
    render_contract::executorOwnBeginTopLevel(render_contract::PassIdentity::TerrainBase,
                                              "gos_TerrainLodChunk_SubmitDrawCommands");
    struct TopLevelGuard_ {
        ~TopLevelGuard_() {
            render_contract::executorOwnEndTopLevel(render_contract::PassIdentity::TerrainBase,
                                                    "gos_TerrainLodChunk_SubmitDrawCommands");
        }
    } _tlGuard;

    // [RENDER_PASS v1] advisory telemetry (env-gated, rate-limited).
    // Chunk path is the default-on production terrain draw (8z cutover).
    render_contract::noteRenderPass(render_contract::PassIdentity::TerrainBase,
                                    "gos_TerrainLodChunk_SubmitDrawCommands");

    // Match the water-cull / decal frame-of-reference: use the baked dispatch MVP
    // when the solid pass is armed (== what the legacy terrain draw used), else
    // the live MVP. Eliminates the 1-frame offset that caused shore-water dropout
    // + decal tearing under camera motion (greybeard META-FIX).
    const float* mvp = gos_terrain_indirect::IsFrameSolidArmed()
                       ? gos_terrain_indirect_getDispatchMvp16()
                       : gos_GetTerrainMVPMat4();
    if (!mvp) mvp = gos_GetTerrainMVPMat4();
    {
        // Diag 7.5: log every time mvp is null (causes silent bail-out).
        static int s_mvpNullCount = 0;
        if (!mvp) {
            ++s_mvpNullCount;
            if (s_mvpNullCount <= 5 || s_mvpNullCount % 300 == 0)
                printf("[TerrainLOD submit] mvp=NULL bail count=%d (count=%d)\n",
                    s_mvpNullCount, count);
            return;
        }
    }

    // Save state.
    GLint prevProg = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    GLint prevVAO = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVAO);

    // Phase 10.3: chunk terrain is OPAQUE and must EXPLICITLY own depth/blend/
    // cull state. The driver previously inherited GL_DEPTH_TEST / glDepthMask /
    // GL_BLEND / glDepthFunc / GL_CULL_FACE from whatever pass ran before. If a
    // prior transparent/overlay pass left depth WRITES off (glDepthMask FALSE)
    // or blend on, the terrain top renders color but writes NO depth -> never
    // occludes -> "transparent, see-through to the skirts / lower terrain",
    // flipping with draw order and mech-selection (which changes the prior
    // pass). Confirmed independent of frag output (diag7). The opaque reverse-Z
    // state is set explicitly here and restored at the end.
    //
    // GlStateGuard slice 2: when MC2_GLSTATEGUARD_TERRAIN is on (default), RAII
    // guards (mc2gl::GlScoped*) own that save/set/restore — the function-scope
    // optionals capture prev in ctor and restore in dtor at the closing brace
    // (no gos_InvalidateRenderStateCache() here, so function scope is correct).
    // =0 reverts to the legacy hand-rolled path below, byte-for-byte. cull is
    // disabled for the WHOLE draw (terrainMVP bakes the GL-NDC X-flip -> winding
    // is inverted -> the terrain TOP would be culled as a backface; terrain is
    // an opaque heightfield so double-sided is free).
    static const bool s_depthAlways = (getenv("MC2_TERRAIN_LOD_DEPTH_ALWAYS") != nullptr);
    const GLenum s_wantDepthFunc = s_depthAlways ? GL_ALWAYS : GL_GEQUAL;
    const bool useGuards = glStateGuardTerrainEnabled();

    // Guard-path objects (constructed only when useGuards; restore at scope end).
    std::optional<mc2gl::GlScopedCapability> gDepthTest, gBlend, gCull;
    std::optional<mc2gl::GlScopedDepthState> gDepthState;
    // Legacy-path saved values (used only when !useGuards).
    GLboolean prevCullFace  = glIsEnabled(GL_CULL_FACE);
    GLboolean prevDepthTest = GL_FALSE;
    GLboolean prevDepthMask = GL_TRUE;
    GLboolean prevBlend     = GL_FALSE;
    GLint     prevDepthFunc = GL_GEQUAL;

    if (useGuards) {
        gDepthTest.emplace(GL_DEPTH_TEST, /*enable=*/true);
        gDepthState.emplace(GL_TRUE, s_wantDepthFunc);
        gBlend.emplace(GL_BLEND, /*enable=*/false);
        gCull.emplace(GL_CULL_FACE, /*enable=*/false);
    } else {
        prevDepthTest = glIsEnabled(GL_DEPTH_TEST);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &prevDepthMask);
        prevBlend     = glIsEnabled(GL_BLEND);
        glGetIntegerv(GL_DEPTH_FUNC, &prevDepthFunc);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDepthFunc(s_wantDepthFunc);   // reverse-Z opaque terrain
        glDisable(GL_CULL_FACE);        // double-sided (see comment above)
    }

    // TERRAIN-LODCHUNK-APPLYPIPELINE-ROUTING-1: make the TerrainSolid pipeline row the
    // AUTHORITATIVE fixed-function state for the live terrain-solid path (this LOD-chunk
    // draw is the default; the bridge is suppressed by mc2TerrainLodChunkEnabled). Both
    // branches above already SET + save/restore the identical state (RAII guards when
    // MC2_GLSTATEGUARD_TERRAIN on, manual otherwise); this re-asserts it from the row so
    // the state is row-owned and [PIPELINE_BIND] TerrainSolid fires (the proof hook). It
    // is byte-identical: depth ON / GEQUAL / depthWrite ON / blend Opaque / cull None, and
    // the row's frontFace=Ccw + polygonOffset=off are the ambient defaults (no-ops).
    // colorMask is owned by the COLORMASK-ROLLOUT-1 beginScene keystone. glProgramName=0
    // -> glUseProgram(s_terrainProgram) below still binds the program. (The default-OFF
    // s_depthAlways debug override is intentionally not modeled — the row forces GEQUAL.)
    pipeline_binder::applyPipeline(
        RenderCore::getPipelineDesc(RenderCore::PipelineId::TerrainSolid), "TerrainSolid");

    glUseProgram(s_terrainProgram);
    glBindVertexArray(s_patchVao);

    // (Depth/blend/cull state is owned above — RAII guards when
    // MC2_GLSTATEGUARD_TERRAIN is on, else the legacy explicit calls. Cull is
    // disabled for the whole draw: terrainMVP bakes the GL-NDC X-flip so winding
    // is inverted and the opaque heightfield is rendered double-sided.)

    // Bind height SSBO (stays bound for all patches this frame).
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_HEIGHT_SSBO_BINDING, static_cast<GLuint>(s_heightSsbo.backendHandle));
    // Step 5b: terrainType SSBO (concrete). 0 if never uploaded -> vert reads 0.
    if (s_typeSsbo.backendHandle != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_TYPE_SSBO_BINDING, static_cast<GLuint>(s_typeSsbo.backendHandle));
    // Step 5c: cement word SSBO.
    if (s_cementSsbo.backendHandle != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_CEMENT_SSBO_BINDING, static_cast<GLuint>(s_cementSsbo.backendHandle));

    // TERRAIN-VISUAL-HEIGHT-SAMPLE-1: resolve geometry-displacement activation once
    // per frame. Active = MC2_TERRAIN_VISUAL_DISPLACE on AND the 4x bake loaded.
    // Bind binding 26 for the whole draw; the per-chunk u_visualDisplace gates the
    // near (LOD0) band only. Default OFF -> u_visualDisplace stays 0 -> byte-identical.
    static const bool s_visualDisplaceGate = []() {
        const char* v = getenv("MC2_TERRAIN_VISUAL_DISPLACE");
        return v && v[0] && v[0] != '0';
    }();
    const bool visualDisplaceActive = s_visualDisplaceGate && s_visualHeightSsbo.backendHandle != 0 && s_visualSide > 0;
    if (visualDisplaceActive)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_VISUAL_HEIGHT_SSBO_BINDING, static_cast<GLuint>(s_visualHeightSsbo.backendHandle));
    if (s_locVisualSide >= 0)
        glUniform1i(s_locVisualSide, s_visualSide);
    // TERRAIN-VISUAL-HEIGHT-S2-ALLLOD: far-band displacement fade, 0..1, default 1
    // (full displacement). Only scales the coarser-band (u_visualDisplace==2) verts;
    // LOD0 (mode 1) is untouched by this knob.
    static const float s_visualDisplaceFar = []() {
        const char* v = getenv("MC2_TERRAIN_VISUAL_DISPLACE_FAR");
        if (!v || !v[0]) return 1.0f;
        float f = (float)atof(v);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        return f;
    }();
    if (s_locVisualDisplaceFar >= 0)
        glUniform1f(s_locVisualDisplaceFar, s_visualDisplaceFar);

    // Upload per-frame uniforms (same for every patch).
    if (s_locMapSide >= 0)
        glUniform1i(s_locMapSide, s_mapSide);
    if (s_locHalfMap >= 0)
        glUniform1f(s_locHalfMap, s_halfMap);
    if (s_locMvp >= 0)
        glUniformMatrix4fv(s_locMvp, 1, GL_FALSE, mvp);

    // Phase 7.5: neon force-color mode — set once per frame before the patch loop.
    {
        int forceColorMode = getenv("MC2_TERRAIN_LOD_CHUNK_FORCE_COLOR") ? 1 : 0;
        if (s_locForceColor >= 0)
            glUniform1i(s_locForceColor, forceColorMode);
    }

    // Bisection bitmask uniform (MC2_TERRAIN_LOD_CHUNK_DIAG): 1=no GBuffer1,
    // 2=no depth fudge, 4=no lighting. Shader-side A/B without rebuilding.
    if (s_locDiag >= 0) {
        const char* dv = getenv("MC2_TERRAIN_LOD_CHUNK_DIAG");
        glUniform1i(s_locDiag, dv ? atoi(dv) : 0);
    }

    // LIGHTING-DEBUG-VIEWS-1A-CHUNK: unified lighting debug channel (40-series),
    // the SAME enum as static_prop / gos_terrain.frag. Separate uniform from
    // u_diag (which is a bitmask, so reusing it would mis-trigger bits). Resolver
    // returns -1 when MC2_LIGHTING_DEBUG_VIEW is unset/unknown -> upload 0 ->
    // shader skips all channels (pixel-invariant default).
    if (s_locLightDebugView >= 0) {
        extern int mc2LightingDebugMode();
        int lvm = mc2LightingDebugMode();
        glUniform1i(s_locLightDebugView, lvm < 0 ? 0 : lvm);
    }

    // MC2_SHADER_PATH_TINT: solid GREEN for the chunk terrain path (default 0 = OFF).
    if (s_locPathTint >= 0)
        glUniform1i(s_locPathTint, mc2ShaderPathTint());

    // Phase 10 (Step 1a): bind the merged colormap atlas (tex1) on unit 0 and
    // feed the atlas-UV reconstruction params (same source as the legacy
    // gos_terrain.frag useAtlasColormap path). When the atlas is not yet built
    // (g_atlasGLTex==0) the sampler reads the default texture -> dark; the
    // colormap pipeline normally has it ready by first in-mission frame.
    {
        const GLuint atlasTex = gos_terrain_indirect_getAtlasGLTex();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, atlasTex);
        if (s_locColormap >= 0) glUniform1i(s_locColormap, 0);
        if (s_locAtlasTLX >= 0) glUniform1f(s_locAtlasTLX, gos_terrain_indirect_getAtlasMapTopLeftX());
        if (s_locAtlasTLY >= 0) glUniform1f(s_locAtlasTLY, gos_terrain_indirect_getAtlasMapTopLeftY());
        if (s_locAtlasOOW >= 0) glUniform1f(s_locAtlasOOW, gos_terrain_indirect_getAtlasOneOverWorldUnits());
        // Step 1b: sun direction for NdotL relief lighting (same as legacy terrain).
        if (s_locLightDir >= 0) {
            float lx = 0.f, ly = 0.f, lz = 1.f;
            gos_GetTerrainLightDir(&lx, &ly, &lz);
            glUniform4f(s_locLightDir, lx, ly, lz, 0.0f);
        }
    }

    // Phase 10 Step 1c: bind the shadow maps + light matrices that
    // include/shadow.hglsl reads. The chunk draw is a bolt-on — like the GL depth
    // state, it MUST set these or enableShadows defaults to 0 and calcShadow
    // returns 1.0 (no shadows). Mirrors gosRenderer::terrainBindShadowUniforms.
    {
        gosPostProcess* pp = getGosPostProcess();
        if (pp && pp->shadowsEnabled_) {
            if (s_locLightSpaceMatrix >= 0)
                glUniformMatrix4fv(s_locLightSpaceMatrix, 1, GL_FALSE, pp->getLightSpaceMatrix());
            if (s_locEnableShadows >= 0)  glUniform1i(s_locEnableShadows, 1);
            if (s_locShadowSoftness >= 0) glUniform1f(s_locShadowSoftness, 2.5f);  // shadow.hglsl default
            if (s_locShadowMap >= 0) {
                glUniform1i(s_locShadowMap, kChunkTexUnitStaticShadow);
                glActiveTexture(GL_TEXTURE0 + kChunkTexUnitStaticShadow);
                glBindTexture(GL_TEXTURE_2D, pp->getShadowTexture());
                glActiveTexture(GL_TEXTURE0);
            }
            if (pp->getDynamicShadowFBO()) {
                if (s_locEnableDynShadows >= 0) glUniform1i(s_locEnableDynShadows, 1);
                if (mc2ShadowCsmEnabled() && pp->getDynamicShadowArrayTexture()) {
                    // Item 1 CSM: array-sampler variant.
                    if (s_locDynCascadeMats >= 0)
                        glUniformMatrix4fv(s_locDynCascadeMats, pp->getDynamicShadowCascadeCount(),
                                           GL_FALSE, pp->getDynamicCascadeMatrices());
                    if (s_locDynCsmCount >= 0) glUniform1i(s_locDynCsmCount, pp->getDynamicShadowCascadeCount());
                    // Stage 3: per-cascade texel-scaled depth bias inputs.
                    if (s_locDynCascadeTexel >= 0)
                        glUniform1fv(s_locDynCascadeTexel, pp->getDynamicShadowCascadeCount(),
                                     pp->getDynamicCascadeTexelWorld());
                    if (s_locCsmDepthSpan >= 0)
                        glUniform1f(s_locCsmDepthSpan, pp->getCsmDepthSpan());
                    if (s_locDynShadowArray >= 0) {
                        glUniform1i(s_locDynShadowArray, kChunkTexUnitDynamicShadow);
                        glActiveTexture(GL_TEXTURE0 + kChunkTexUnitDynamicShadow);
                        glBindTexture(GL_TEXTURE_2D_ARRAY, pp->getDynamicShadowArrayTexture());
                        glActiveTexture(GL_TEXTURE0);
                    }
                    // Per-cascade shadow resolution: separate full-map (last) cascade.
                    if (s_locDynFullMapTexel >= 0)
                        glUniform1f(s_locDynFullMapTexel, pp->getDynamicFullMapTexelWorld());
                    if (s_locDynFullMapShadow >= 0) {
                        glUniform1i(s_locDynFullMapShadow, kChunkTexUnitDynFullMap);
                        glActiveTexture(GL_TEXTURE0 + kChunkTexUnitDynFullMap);
                        glBindTexture(GL_TEXTURE_2D, pp->getDynamicFullMapTexture());
                        glActiveTexture(GL_TEXTURE0);
                    }
                } else {
                    if (s_locDynLightSpaceMat >= 0)
                        glUniformMatrix4fv(s_locDynLightSpaceMat, 1, GL_FALSE, pp->getDynamicLightSpaceMatrix());
                    if (s_locDynShadowMap >= 0) {
                        glUniform1i(s_locDynShadowMap, kChunkTexUnitDynamicShadow);
                        glActiveTexture(GL_TEXTURE0 + kChunkTexUnitDynamicShadow);
                        glBindTexture(GL_TEXTURE_2D, pp->getDynamicShadowTexture());
                        glActiveTexture(GL_TEXTURE0);
                    }
                }
            } else if (s_locEnableDynShadows >= 0) {
                glUniform1i(s_locEnableDynShadows, 0);
            }
        } else {
            if (s_locEnableShadows >= 0)    glUniform1i(s_locEnableShadows, 0);
            if (s_locEnableDynShadows >= 0) glUniform1i(s_locEnableDynShadows, 0);
        }
    }

    // Phase 10 Step 5a: bind the merged material normal sampler2DArray (same
    // texture the legacy terrain uses) on its own unit. 0 until all 5 material
    // slots are populated -> the frag samples the default texture (flat-ish
    // normal -> falls back to the smooth base normal, no crash).
    if (s_locMatNormalArray >= 0) {
        GLuint matArrTex = (GLuint)gos_GetTerrainNormalArrayTex();
        glUniform1i(s_locMatNormalArray, kChunkTexUnitMatNormalArray);
        glActiveTexture(GL_TEXTURE0 + kChunkTexUnitMatNormalArray);
        glBindTexture(GL_TEXTURE_2D_ARRAY, matArrTex);
        glActiveTexture(GL_TEXTURE0);
    }

    // Step 5c: cement catalog atlas (tex3 / unit 3) + params — concrete tiles
    // sample this instead of the colormap (legacy gos_terrain.frag:414-426).
    {
        bool cementReady = gos_terrain_indirect_isCementAtlasReady();
        if (s_locUseCement >= 0)
            glUniform1i(s_locUseCement, (cementReady && s_cementSsbo.backendHandle != 0) ? 1 : 0);
        // CEMENT-DIAG-CONNECT-1: env gate MC2_TERRAIN_CEMENT_DIAG_CONNECT, default OFF
        // -> uploads 0 -> frag diagonal-fill block skipped (byte-identical).
        if (s_locCementDiagConnect >= 0) {
            int diagOn = 0;
            if (const char* e = getenv("MC2_TERRAIN_CEMENT_DIAG_CONNECT")) diagOn = (e[0] && e[0] != '0') ? 1 : 0;
            glUniform1i(s_locCementDiagConnect, diagOn);
        }
        if (cementReady) {
            if (s_locCementGridSide >= 0)
                glUniform1i(s_locCementGridSide, gos_terrain_indirect_getCementAtlasGridSide());
            if (s_locCementWUPT >= 0)
                glUniform1f(s_locCementWUPT, 128.0f);  // Terrain::worldUnitsPerVertex
            if (s_locCementAtlas >= 0) {
                glUniform1i(s_locCementAtlas, kChunkTexUnitCement);
                glActiveTexture(GL_TEXTURE0 + kChunkTexUnitCement);
                glBindTexture(GL_TEXTURE_2D, (GLuint)gos_terrain_indirect_getCementAtlasGLTex());
                glActiveTexture(GL_TEXTURE0);
            }
        }
    }

    // Stage B: transition mask array at unit 11.
    {
        const bool tmReady = gos_terrain_indirect_isTransitionMaskReady();
        if (s_locUseTransitionMask >= 0)
            glUniform1i(s_locUseTransitionMask, tmReady ? 1 : 0);
        if (tmReady && s_locTransitionMaskArray >= 0) {
            glUniform1i(s_locTransitionMaskArray, kChunkTexUnitTransitionMask);
            glActiveTexture(GL_TEXTURE0 + kChunkTexUnitTransitionMask);
            glBindTexture(GL_TEXTURE_2D_ARRAY, gos_terrain_indirect_getTransitionMaskArrayGL());
            glActiveTexture(GL_TEXTURE0);
        }
    }

    // TERRAIN-CONTROLMAP-SAMPLE-1: authored control map at unit 12. Only bound
    // when a sidecar was actually loaded (s_controlMapTex != 0); u_useControlMap
    // uploads 0 otherwise (gate off / no sidecar) -> frag takes the verbatim
    // chunkColorWeights() else-branch -> byte-identical.
    {
        const bool controlMapReady = (s_controlMapTex != 0);
        if (s_locUseControlMap >= 0)
            glUniform1i(s_locUseControlMap, controlMapReady ? 1 : 0);
        if (controlMapReady && s_locControlMap >= 0) {
            glUniform1i(s_locControlMap, TERRAIN_CONTROLMAP_TEXUNIT);
            glActiveTexture(GL_TEXTURE0 + TERRAIN_CONTROLMAP_TEXUNIT);
            glBindTexture(GL_TEXTURE_2D, s_controlMapTex);
            glActiveTexture(GL_TEXTURE0);
        }
    }

    // TERRAIN-OVERLAY-V2-PARITY-1: authored cement/pad/runway overlay sidecar
    // at unit TERRAIN_OVERLAY_SIDECAR_TEXUNIT. Only bound when a sidecar was
    // actually loaded (s_overlaySidecarTex != 0); u_useOverlaySidecar uploads
    // 0 otherwise (gate off / no sidecar) -> frag takes the verbatim legacy
    // cement-word composite else-branch -> byte-identical.
    {
        const bool overlaySidecarReady = (s_overlaySidecarTex != 0);
        if (s_locUseOverlaySidecar >= 0)
            glUniform1i(s_locUseOverlaySidecar, overlaySidecarReady ? 1 : 0);
        if (overlaySidecarReady) {
            if (s_locOverlaySidecar >= 0) {
                glUniform1i(s_locOverlaySidecar, TERRAIN_OVERLAY_SIDECAR_TEXUNIT);
                glActiveTexture(GL_TEXTURE0 + TERRAIN_OVERLAY_SIDECAR_TEXUNIT);
                glBindTexture(GL_TEXTURE_2D, s_overlaySidecarTex);
                glActiveTexture(GL_TEXTURE0);
            }
            if (s_locOverlayBounds >= 0)
                glUniform4f(s_locOverlayBounds, s_overlayBounds[0], s_overlayBounds[1],
                            s_overlayBounds[2], s_overlayBounds[3]);
        }
    }

    // TERRAIN-SHORELINE-MASK-1: authored land-side wet/foam shoreline mask at
    // unit TERRAIN_SHORELINE_TEXUNIT. Only bound when a mask was actually
    // loaded (s_shorelineMaskTex != 0); u_useShorelineMask uploads 0 otherwise
    // (gate off / no mask) -> frag skips the whole wet/foam block -> byte-
    // identical (no band; legacy screen runShoreline() stays active per its
    // own gate). u_shaderTime is uploaded unconditionally (cheap scalar); the
    // frag only reads it inside the gated shoreline block, so this is
    // render-invariant when the gate is off.
    {
        const bool shorelineMaskReady = (s_shorelineMaskTex != 0);
        if (s_locUseShorelineMask >= 0)
            glUniform1i(s_locUseShorelineMask, shorelineMaskReady ? 1 : 0);
        if (shorelineMaskReady) {
            if (s_locShorelineMask >= 0) {
                glUniform1i(s_locShorelineMask, TERRAIN_SHORELINE_TEXUNIT);
                glActiveTexture(GL_TEXTURE0 + TERRAIN_SHORELINE_TEXUNIT);
                glBindTexture(GL_TEXTURE_2D, s_shorelineMaskTex);
                glActiveTexture(GL_TEXTURE0);
            }
            if (s_locShorelineBounds >= 0)
                glUniform4f(s_locShorelineBounds, s_shorelineBounds[0], s_shorelineBounds[1],
                            s_shorelineBounds[2], s_shorelineBounds[3]);
            if (s_locShaderTime >= 0)
                glUniform1f(s_locShaderTime, gos_GetShaderClockSeconds());
            // TERRAIN-SHORELINE-MASK-1 (visual-quality pass): runtime intensity
            // knobs, sampled once (feature gate is per-process anyway). Default
            // 1.0 = the authored modest band in terrain_lod_chunk.frag; clamp to
            // [0,2] so a bad env value can't blow the band out or invert it.
            static const float s_shorelineStrength = []() {
                const char* v = std::getenv("MC2_TERRAIN_SHORELINE_STRENGTH");
                float f = v ? (float)std::atof(v) : 1.0f;
                if (!(f == f)) f = 1.0f; // NaN guard
                if (f < 0.0f) f = 0.0f; if (f > 2.0f) f = 2.0f;
                return f;
            }();
            static const float s_shorelineFoamStrength = []() {
                const char* v = std::getenv("MC2_TERRAIN_SHORELINE_FOAM");
                float f = v ? (float)std::atof(v) : 1.0f;
                if (!(f == f)) f = 1.0f; // NaN guard
                if (f < 0.0f) f = 0.0f; if (f > 2.0f) f = 2.0f;
                return f;
            }();
            if (s_locShorelineStrength >= 0)
                glUniform1f(s_locShorelineStrength, s_shorelineStrength);
            if (s_locShorelineFoamStrength >= 0)
                glUniform1f(s_locShorelineFoamStrength, s_shorelineFoamStrength);
        }
    }

    // Step 5a: upload the live material tunables (driven by the ImGui terrain
    // panel via the same gosRenderer members the legacy terrain reads).
    {
        float mt[5] = {3,2,1,6,1};  gos_GetTerrainMatTiling(&mt[0], &mt[1], &mt[2], &mt[3], &mt[4]);
        float nb[4] = {0.9f,1.1f,1.1f,2.5f}; gos_GetTerrainMatNormalBoost(&nb[0], &nb[1], &nb[2], &nb[3]);
        float cg[4] = {-0.02f,0.06f,0.22f,0.40f}; gos_GetTerrainClassGrass(&cg[0], &cg[1], &cg[2], &cg[3]);
        float cd[4] = {-0.02f,0.06f,0.22f,0.45f}; gos_GetTerrainClassDirt(&cd[0], &cd[1], &cd[2], &cd[3]);
        float dt = gos_GetTerrainDetailTiling();
        float ds = gos_GetTerrainDetailStrength();
        if (s_locMatTiling      >= 0) glUniform4f(s_locMatTiling,      mt[0], mt[1], mt[2], mt[3]);
        if (s_locMatTilingSnow  >= 0) glUniform1f(s_locMatTilingSnow,  mt[4]);
        if (s_locMatNormalBoost >= 0) glUniform4f(s_locMatNormalBoost, nb[0], nb[1], nb[2], nb[3]);
        if (s_locClassGrass     >= 0) glUniform4f(s_locClassGrass,     cg[0], cg[1], cg[2], cg[3]);
        if (s_locClassDirt      >= 0) glUniform4f(s_locClassDirt,      cd[0], cd[1], cd[2], cd[3]);
        if (s_locDetailTiling   >= 0) glUniform4f(s_locDetailTiling,   dt, 0.0f, 0.0f, 0.0f);
        const float* _datYZW = mc2_chunkDetailAntiTileYZW();  // TERRAIN-DETAIL-ANTI-TILE-1 (yzw=0 when gate OFF)
        if (s_locDetailStrength >= 0) glUniform4f(s_locDetailStrength, ds, _datYZW[0], _datYZW[1], _datYZW[2]);

        float tr[3]={0.36f,0.37f,0.40f}; gos_GetTerrainTintRock(&tr[0],&tr[1],&tr[2]);
        float tg[3]={0.35f,0.42f,0.25f}; gos_GetTerrainTintGrass(&tg[0],&tg[1],&tg[2]);
        float td[3]={0.48f,0.42f,0.33f}; gos_GetTerrainTintDirt(&td[0],&td[1],&td[2]);
        float tss = gos_GetTerrainTintStrengthScale();
        if (s_locTintRock          >= 0) glUniform3f(s_locTintRock,  tr[0], tr[1], tr[2]);
        if (s_locTintGrass         >= 0) glUniform3f(s_locTintGrass, tg[0], tg[1], tg[2]);
        if (s_locTintDirt          >= 0) glUniform3f(s_locTintDirt,  td[0], td[1], td[2]);
        if (s_locTintStrengthScale >= 0) glUniform1f(s_locTintStrengthScale, tss);
        // Snow brightness dampen: <1 darkens detected snow. Default 0.78 (visibly
        // turned down); MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN overrides.
        static const float s_snowDampen = [](){ const char* v = getenv("MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN"); return v ? (float)atof(v) : 0.78f; }();
        if (s_locSnowBrightnessDampen >= 0) glUniform1f(s_locSnowBrightnessDampen, s_snowDampen);
        // TERRAIN-CONTROLMAP-ALBEDO-1: default member 0.0f -> uploaded verbatim
        // -> frag's mix(x,1.0,0.0)==x (byte-identical) when gate is OFF.
        if (s_locControlAlbedoStrength >= 0)
            glUniform1f(s_locControlAlbedoStrength, gos_GetTerrainControlAlbedoStrength());

        // TERRAIN-MATERIAL-LIB-1: promoted tints always upload (no gate -- they
        // replace former frag literals; default member values are the exact
        // former literals, so this is byte-identical when no JSON was loaded).
        // Roughness/AO + the branch flag ARE gated: u_useMaterialLib defaults to
        // 0 so the frag's roughness/AO branch is never taken unless the env gate
        // is on, matching the legacy patch-stream path's byte-identity contract.
        {
            float tc[3]={0.55f,0.53f,0.50f}; gos_GetTerrainTintConcrete(&tc[0],&tc[1],&tc[2]);
            float tsn[3]={0.75f,0.78f,0.84f}; gos_GetTerrainTintSnow(&tsn[0],&tsn[1],&tsn[2]);
            float mr[4]={1,1,1,1}; gos_GetTerrainMatRoughness(&mr[0],&mr[1],&mr[2],&mr[3]);
            float ma[4]={1,1,1,1}; gos_GetTerrainMatAO(&ma[0],&ma[1],&ma[2],&ma[3]);
            if (s_locTintConcrete >= 0) glUniform3f(s_locTintConcrete, tc[0], tc[1], tc[2]);
            if (s_locTintSnow     >= 0) glUniform3f(s_locTintSnow,     tsn[0], tsn[1], tsn[2]);
            if (s_locMatRoughness >= 0) glUniform4f(s_locMatRoughness, mr[0], mr[1], mr[2], mr[3]);
            if (s_locMatAO        >= 0) glUniform4f(s_locMatAO,        ma[0], ma[1], ma[2], ma[3]);
            const bool matLibOn = gos_TerrainMaterialLibEnabled();
            if (s_locUseMaterialLib >= 0) glUniform1i(s_locUseMaterialLib, matLibOn ? 1 : 0);
            static const bool s_matLibTrace = (getenv("MC2_MATERIALLIB_TRACE") != nullptr);
            if (s_matLibTrace) {
                static bool s_matLibLogged = false;
                if (!s_matLibLogged) {
                    s_matLibLogged = true;
                    printf("[MaterialLib] chunk-binder upload useMaterialLib=%d "
                           "tintConcrete=(%.3f,%.3f,%.3f) tintSnow=(%.3f,%.3f,%.3f) "
                           "matRoughness=(%.3f,%.3f,%.3f,%.3f) matAO=(%.3f,%.3f,%.3f,%.3f)\n",
                           matLibOn ? 1 : 0,
                           tc[0], tc[1], tc[2], tsn[0], tsn[1], tsn[2],
                           mr[0], mr[1], mr[2], mr[3], ma[0], ma[1], ma[2], ma[3]);
                    fflush(stdout);
                }
            }
        }

        // Remaining tunables. Hemisphere V1/V2 are env-gated OFF by default (match
        // legacy: force-zeroed unless MC2_TERRAIN_LIGHTING_V1/V2 set). NFH strength
        // scales the chunk's always-on smooth normal (default 1.0 = no change). POM
        // = legacy scale (default 0.02). Material profile = global int (0=legacy).
        static const bool s_v1Env = (getenv("MC2_TERRAIN_LIGHTING_V1") != nullptr);
        static const bool s_v2Env = (getenv("MC2_TERRAIN_LIGHTING_V2") != nullptr);
        if (s_locLightingV1  >= 0) glUniform1f(s_locLightingV1,  s_v1Env ? gos_GetTerrainLightingV1Strength() : 0.0f);
        if (s_locLightingV2  >= 0) glUniform1f(s_locLightingV2,  s_v2Env ? gos_GetTerrainLightingV2Floor()    : 1.0f);
        if (s_locNfhStrength >= 0) glUniform1f(s_locNfhStrength, gos_GetTerrainNormalsFromHeightStrength());
        // TERRAIN-SLOPE-BIAS-VISUAL-1 (B4a): env gate MC2_TERRAIN_SLOPE_BIAS, default
        // OFF. When unset/0 the gate uploads 0 and the frag block is a no-op
        // (byte-identical). Strength via MC2_TERRAIN_SLOPE_BIAS_STRENGTH (default 1.0).
        {
            int sbOn = 0;
            if (const char* e = getenv("MC2_TERRAIN_SLOPE_BIAS")) sbOn = (e[0] && e[0] != '0') ? 1 : 0;
            if (s_locUseRockSlopeBias >= 0) glUniform1i(s_locUseRockSlopeBias, sbOn);
            if (s_locRockSlopeBiasStr >= 0) {
                float sbStr = 1.0f;
                if (const char* s = getenv("MC2_TERRAIN_SLOPE_BIAS_STRENGTH")) {
                    float v = (float)atof(s);
                    if (v > 0.0f) sbStr = v;
                }
                glUniform1f(s_locRockSlopeBiasStr, sbStr);
            }
        }
        // TERRAIN-CLIFF-MATERIAL-TRIPLANAR-1: env gate MC2_TERRAIN_CLIFF_TRIPLANAR,
        // default OFF -> uploads 0 -> frag block no-op (byte-identical). Strength via
        // MC2_TERRAIN_CLIFF_TRIPLANAR_STRENGTH (default 1.0).
        {
            int tcOn = 0;
            if (const char* e = getenv("MC2_TERRAIN_CLIFF_TRIPLANAR")) tcOn = (e[0] && e[0] != '0') ? 1 : 0;
            if (s_locUseTriplanarCliff >= 0) glUniform1i(s_locUseTriplanarCliff, tcOn);
            if (s_locCliffTriplanarStr >= 0) {
                float tcStr = 1.0f;
                if (const char* s = getenv("MC2_TERRAIN_CLIFF_TRIPLANAR_STRENGTH")) {
                    float v = (float)atof(s);
                    if (v > 0.0f) tcStr = v;
                }
                glUniform1f(s_locCliffTriplanarStr, tcStr);
            }
        }
        // TERRAIN-MACRO-VARIATION-1: env gate MC2_TERRAIN_MACRO_VARIATION, default
        // OFF -> uploads 0 -> frag block skipped (byte-identical). Strength via
        // MC2_TERRAIN_MACRO_VARIATION_STRENGTH (default 1.0).
        if (s_locMacroVariation >= 0) {
            float mvStr = 0.0f;
            if (const char* e = getenv("MC2_TERRAIN_MACRO_VARIATION")) {
                if (e[0] && e[0] != '0') {
                    mvStr = 1.0f;
                    if (const char* s = getenv("MC2_TERRAIN_MACRO_VARIATION_STRENGTH")) {
                        float v = (float)atof(s);
                        if (v > 0.0f) mvStr = v;
                    }
                }
            }
            glUniform1f(s_locMacroVariation, mvStr);
        }
        // TERRAIN-EDGE-FEATHER-1: env gate MC2_TERRAIN_EDGE_FEATHER, default OFF
        // -> uploads 0 -> frag block skipped (byte-identical). Fades the terrain
        // colormap to sky/haze over the last ~tile band so the hard straight
        // map-perimeter line dissolves into the fog. Strength via
        // MC2_TERRAIN_EDGE_FEATHER_STRENGTH (default 1.0).
        {
            int efOn = 0;
            if (const char* e = getenv("MC2_TERRAIN_EDGE_FEATHER")) efOn = (e[0] && e[0] != '0') ? 1 : 0;
            if (s_locEdgeFeather >= 0) glUniform1i(s_locEdgeFeather, efOn);
            if (s_locEdgeFeatherStr >= 0) {
                float efStr = 1.0f;
                if (const char* s = getenv("MC2_TERRAIN_EDGE_FEATHER_STRENGTH")) {
                    float v = (float)atof(s);
                    if (v > 0.0f) efStr = v;
                }
                glUniform1f(s_locEdgeFeatherStr, efStr);
            }
        }
        if (s_locPomParams   >= 0) glUniform4f(s_locPomParams,   gos_GetTerrainPOMScale(), 8.0f, 32.0f, 0.0f);
        if (s_locMatProfile  >= 0) glUniform1i(s_locMatProfile,  g_terrainMaterialProfile);
    }

    // Phase 7.5: log first successful submit so the user can confirm the path is live.
    // Also reset the zero-submit streak counter on any non-zero submit.
    {
        static bool s_firstSubmit = true;
        s_submitZeroStreak = 0;  // reset on any non-zero submit
        if (s_firstSubmit && count > 0) {
            printf("[TerrainLOD v1] FIRST SUBMIT: %d draw commands queued\n", count);
            fflush(stdout);
            s_firstSubmit = false;
        }
    }

    // Cardinality probe: log every 600 submits.
    static int s_submitCount = 0;
    ++s_submitCount;
    if (s_submitCount % 600 == 0) {
        printf("[TerrainLOD submit] count=%d cmds=%d\n", s_submitCount, count);
        fflush(stdout);
    }

    // TERRAIN-VISUAL-HEIGHT-S2-ALLLOD: reset per-band displaced-chunk counters
    // for this submit; populated per-command below, printed after the loop.
    for (int lvl = 0; lvl < 6; ++lvl) s_visualDisplacedCounts[lvl] = 0;

#ifdef MC2_VULKAN_ISLAND
    // VK-P10 TERRAIN TIER-1: when armed, the VK terrain island co-writes the LOD0
    // (lodStep==1) MAIN patches into the shared GBuffer. Capture them here and SKIP
    // their GL main+skirt draws (so the A/B reference and the VK path both exclude
    // LOD0 skirts — the probe compares LOD0 main-triangle pixels like-for-like).
    // Default-OFF -> s_vkTerrainEnabled=false -> the loop below is byte-identical.
    //
    // FAIL-SOFT (DEFAULT-ON safety): the SKIP is gated on island-READY, not merely
    // enabled. If the island can't co-write this frame (SPV/pipeline/init/device
    // failure, or a latched transient — mc2_vk_terrain_island_ready()==0), GL keeps
    // drawing LOD0 so there is NO terrain hole. run() is still called below whenever
    // enabled (even when we did NOT skip) so the island can BIRTH/heal — the first
    // successful run flips ready for the next frame (one-frame GL-draw then VK takeover,
    // never a hole).
    const bool s_vkTerrainEnabled = (mc2_vk_terrain_island_enabled() != 0);
    // PREPARE the island NOW (GL context current) so the skip decision below can gate on
    // real readiness — this births the island independent of the feed, breaking the
    // skip<->ready<->feed deadlock. On any failure prepare()==0 -> GL draws LOD0.
    bool s_vkTerrainReady = false;
    if (s_vkTerrainEnabled) {
        if (gosPostProcess* ppPrep = getGosPostProcess()) {
            s_vkTerrainReady =
                (mc2_vk_terrain_island_prepare(ppPrep->getWidth(), ppPrep->getHeight()) != 0);
        }
    }
    const bool s_vkTerrainActive = s_vkTerrainEnabled && s_vkTerrainReady;
    static bool s_vkTerrainFailSoftLogged = false;
    if (s_vkTerrainEnabled && !s_vkTerrainActive && !s_vkTerrainFailSoftLogged) {
        fprintf(stderr, "[VK_TERRAIN_ISLAND] FAIL-SOFT: gate ON but island not ready this "
                        "frame -> GL draws LOD0 (no hole). Will retry birth/heal via run().\n");
        fflush(stderr);
        s_vkTerrainFailSoftLogged = true;
    }
    s_vkTerrainPatches.clear();
    s_vkTerrainFeedValid = false;
    if (s_vkTerrainActive) {
        std::memcpy(s_vkTerrainMvp, mvp, sizeof(s_vkTerrainMvp)); // same MVP the GL draw uses
    }
#endif

    for (int i = 0; i < count; ++i)
    {
        const TerrainDrawCommand& cmd = cmds[i];
        int qcX = cmd.quadCountsPacked & 0xFF;
        int qcY = (cmd.quadCountsPacked >> 8) & 0xFF;

        if (qcX <= 0 || qcY <= 0) continue;

#ifdef MC2_VULKAN_ISLAND
        // VK-P10 TERRAIN TIER-1: hand LOD0 (lodStep==1) main patches to the VK island
        // and skip their GL main+skirt draws. Only the non-displaced coarse LOD0 path
        // is ported (visual-displace / edge-stitch are DEFERRED); if either is active
        // for this patch, leave it on the GL path (do NOT capture) so nothing regresses.
        if (s_vkTerrainActive && cmd.lodStep == 1 &&
            !(visualDisplaceActive && cmd.lodStep == 1) &&
            (edgeStitch == nullptr || edgeStitch[i] == 0)) {
            const int maxOffX = qcX;  // (qcX/1)*1
            const int maxOffY = qcY;
            Mc2TerrainTier1Patch p;
            p.blockOriginX = cmd.blockOriginX;
            p.blockOriginY = cmd.blockOriginY;
            p.quadCountX   = maxOffX;
            p.quadCountY   = maxOffY;
            s_vkTerrainPatches.push_back(p);
            s_vkTerrainFeedValid = true;
            continue;   // GL skips this patch's main + skirt draws; VK owns it
        }
#endif

        // TERRAIN-VISUAL-HEIGHT-SAMPLE-1 (LOD0) + TERRAIN-VISUAL-HEIGHT-S2-ALLLOD
        // (LOD1+): displace every band. LOD0 uses the 4x-finer builder (mode 1,
        // unchanged from S1: fine grid + skirts, corner-pinned edges). Coarser
        // bands (lodStep>1) reuse the EXISTING coarse-density patch unmodified
        // (mode 2: no new vertices — the vert shader Z-swaps each existing sample
        // point to its coincident bake index, sec vert comment for stitch proof).
        const bool displaceLod0  = visualDisplaceActive && cmd.lodStep == 1;
        const bool displaceFar   = visualDisplaceActive && cmd.lodStep != 1;
        const int  visualMode    = displaceLod0 ? 1 : (displaceFar ? 2 : 0);
        // Checkerboard diagonal applies ONLY to lodStep==1 fine cells on the
        // normal (non-4x-displace) path — coarse LOD (lodStep>1) per-cell parity
        // is ill-defined and the 4x heightsFine path is a separate follow-up;
        // both keep the existing fixed TL-BR diagonal. Mode 2 does NOT change the
        // patch builder at all (same qcX/qcY/lodStep/checkerDiag as the
        // non-displaced path), so this is unaffected by S2.
        const bool checkerDiag = terrainLodCheckerDiagEnabled()
                                 && !displaceLod0 && cmd.lodStep == 1;
        const PatchShape& patch = displaceLod0
            ? getOrBuildPatch(qcX * 4, qcY * 4, 1, /*checkerDiag*/false, 0, 0)
            : getOrBuildPatch(qcX, qcY, cmd.lodStep, checkerDiag,
                              cmd.blockOriginX & 1, cmd.blockOriginY & 1);
        if (s_locVisualDisplace >= 0)
            glUniform1i(s_locVisualDisplace, visualMode);

        // TERRAIN-VISUAL-HEIGHT-S2-ALLLOD: per-LOD-band displaced-chunk counters,
        // logged alongside the existing [TerrainLOD v1] telemetry so the crack /
        // silhouette acceptance pass has hard numbers per band.
        if (visualDisplaceActive) {
            int lvl = (cmd.lodStep == 1) ? 0 : (cmd.lodStep == 2) ? 1
                    : (cmd.lodStep == 4) ? 2 : (cmd.lodStep == 5) ? 3
                    : (cmd.lodStep == 10) ? 4 : 5;
            ++s_visualDisplacedCounts[lvl];
        }

        // Per-block uniforms (shared by main patch and skirt).
        if (s_locBlockOriginX >= 0)
            glUniform1i(s_locBlockOriginX, cmd.blockOriginX);
        if (s_locBlockOriginY >= 0)
            glUniform1i(s_locBlockOriginY, cmd.blockOriginY);
        if (s_locLodStep >= 0)
            glUniform1i(s_locLodStep, cmd.lodStep);  // Phase 5: LOD band for debug vis

        // Phase 10.4: edge stitching. Block quad extent (for edge detection) +
        // packed coarser-neighbour stride per edge. Skirt verts (isSkirtFlag!=0)
        // skip the snap in the vert, so this is safe to set once per block.
        // u_quadCount* must be the MAX localOffset the patch actually emits, which
        // makeSamplePositions caps at the last multiple of lodStep <= quad count.
        // (For partial map-edge blocks qcX may not be a multiple of lodStep.)
        const int maxOffX = (cmd.lodStep > 0) ? (qcX / cmd.lodStep) * cmd.lodStep : qcX;
        const int maxOffY = (cmd.lodStep > 0) ? (qcY / cmd.lodStep) * cmd.lodStep : qcY;
        if (s_locQuadCountX >= 0) glUniform1i(s_locQuadCountX, maxOffX);
        if (s_locQuadCountY >= 0) glUniform1i(s_locQuadCountY, maxOffY);
        if (s_locEdgeStitch >= 0)
            glUniform1i(s_locEdgeStitch, edgeStitch ? (GLint)edgeStitch[i] : 0);
        if (s_locShadowTier >= 0)  // Slice B: per-chunk shadow tier (DIAG=40 tint only)
            glUniform1i(s_locShadowTier, shadowTiers ? (GLint)shadowTiers[i] : 0);

        // --- Draw main patch (skirtDepth=0 so isSkirtFlag pulls height by 0) ---
        if (s_locSkirtDepth >= 0)
            glUniform1f(s_locSkirtDepth, 0.0f);

        // Attrib 0: ivec2 localOffset. Attrib 1 (isSkirt) left disabled -> reads as 0.
        glDisableVertexAttribArray(1);
        glBindBuffer(GL_ARRAY_BUFFER, patch.vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribIPointer(0, 2, GL_SHORT, (GLsizei)(2 * sizeof(int16_t)), (const void*)0);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, patch.ibo);
        glDrawElements(GL_TRIANGLES, patch.indexCount, GL_UNSIGNED_SHORT, 0);

        // --- Phase 6: Draw skirt strips ---
        if (patch.skirtIndexCount > 0 && skirtDepths != nullptr)
        {
            float skirtDepth = skirtDepths[i];
            if (skirtDepth > 0.0f)
            {
                if (s_locSkirtDepth >= 0)
                    glUniform1f(s_locSkirtDepth, skirtDepth);

                // (GL_CULL_FACE already disabled for the whole draw — see top.)
                // Attrib 0: lx, ly (first 2 int16_t of SkirtVertex, stride=8).
                // Attrib 1: isSkirt (third int16_t of SkirtVertex, offset=4).
                glBindBuffer(GL_ARRAY_BUFFER, patch.skirtVbo);
                glEnableVertexAttribArray(0);
                glVertexAttribIPointer(0, 2, GL_SHORT, (GLsizei)(4 * sizeof(int16_t)), (const void*)0);
                glEnableVertexAttribArray(1);
                glVertexAttribIPointer(1, 1, GL_SHORT, (GLsizei)(4 * sizeof(int16_t)), (const void*)(2 * sizeof(int16_t)));

                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, patch.skirtIbo);
                // Phase 10.2b: draw ONLY the edges flagged in the per-block mask
                // (bit 0=N,1=S,2=W,3=E). No mask array -> all four (back-compat).
                const unsigned int mask = skirtEdgeMasks ? skirtEdgeMasks[i] : 0xFu;
                for (int e = 0; e < 4; ++e) {
                    if (((mask >> e) & 1u) == 0u) continue;
                    if (patch.skirtEdgeCount[e] <= 0) continue;
                    glDrawElements(GL_TRIANGLES, patch.skirtEdgeCount[e], GL_UNSIGNED_SHORT,
                                   (const void*)(size_t)(patch.skirtEdgeOffset[e] * sizeof(uint16_t)));
                }

                glDisableVertexAttribArray(1);
            }
        }
    }

    // TERRAIN-VISUAL-HEIGHT-S2-ALLLOD: per-band displaced-chunk-count telemetry.
    // Same cadence as [TerrainLOD v1] (every frame for the first 60, then every
    // 180th submit) so the acceptance pass has hard numbers per LOD band without
    // spamming the log. Only fires when the displace gate is actually active.
    if (visualDisplaceActive) {
        static unsigned long s_visFrame = 0;
        ++s_visFrame;
        if (s_visFrame <= 60 || (s_visFrame % 180) == 0) {
            printf("[TerrainLOD] visualDisplace band-counts LOD0=%d LOD1=%d LOD2=%d LOD3=%d LOD4=%d LOD5=%d\n",
                   s_visualDisplacedCounts[0], s_visualDisplacedCounts[1], s_visualDisplacedCounts[2],
                   s_visualDisplacedCounts[3], s_visualDisplacedCounts[4], s_visualDisplacedCounts[5]);
            fflush(stdout);
        }
    }

    // Terrain pixels are now on screen. Mark terrain drawn so the terrain-gated
    // post-process passes (runScreenShadow / runCloudShadow / runShoreline /
    // runGodRays) actually run. The chunk path is the default-on production
    // terrain draw (8z cutover); the legacy markTerrainDrawn() sites
    // (gameos_graphics.cpp tess draw + gos_terrain_patch_stream.cpp) do NOT
    // fire under this path, so sceneHasTerrain_ would otherwise stay false and
    // all four passes silently skip (root cause of the dead cloud-shadow pass).
    if (gosPostProcess* pp = getGosPostProcess()) {
        pp->markTerrainDrawn();
        // TERRAIN-SHORELINE-MASK-1 (recon landmine #6): when the terrain-side
        // wet/foam band is active, suppress the legacy screen-space
        // runShoreline() pass so the seam isn't brightened twice. Mask
        // inactive (gate off / no sidecar) -> setShorelineSuppressedByTerrainMask(false)
        // every frame -> runShoreline() behaves exactly as before (byte-identical).
        pp->setShorelineSuppressedByTerrainMask(gos_TerrainLodChunk_IsShorelineMaskActive());
    }
    RenderCore::framegraph::noteTerrainPath(RenderCore::framegraph::TerrainPath::LODChunk);  // TERRAIN-PATH-TELEMETRY-1

    // Restore GL state.
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_HEIGHT_SSBO_BINDING, 0);
    if (visualDisplaceActive)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_VISUAL_HEIGHT_SSBO_BINDING, 0);
    if (!useGuards) {
        // Phase 10.3: restore inherited cull/depth/blend state (legacy path).
        if (prevCullFace)   glEnable(GL_CULL_FACE);
        glDepthMask(prevDepthMask);
        if (!prevDepthTest) glDisable(GL_DEPTH_TEST);
        if (prevBlend)      glEnable(GL_BLEND);
        glDepthFunc((GLenum)prevDepthFunc);
    }
    // useGuards path: gCull/gBlend/gDepthState/gDepthTest restore depth/blend/
    // cull/mask/func when their optionals destruct at the closing brace below.
    glBindVertexArray((GLuint)prevVAO);
    glUseProgram((GLuint)prevProg);

#ifdef MC2_VULKAN_ISLAND
    // VK-P10 TERRAIN TIER-1: after the GL terrain bands drew (and GL state is fully
    // restored), the VK island co-writes the captured LOD0 patches into the SAME
    // shared scene GBuffer (color MRT0 + normal MRT1 + shared depth), reverse-Z
    // depth-tested against what GL just authored. Default-OFF -> feed empty -> no-op.
    // markTerrainDrawn already fired above (GL owns it); the island also latches it
    // for robustness if a frame is all-LOD0.
    //
    // FAIL-SOFT: run() is called whenever the gate is ENABLED (not only when we skipped)
    // so the island can BIRTH/heal even on a frame where we did NOT skip (ready==0 this
    // frame -> GL drew LOD0 -> feed empty -> run births the island + returns 0 harmlessly;
    // next frame ready==1 -> skip + VK draws). On a frame where we DID skip (ready was 1),
    // a mid-frame run()==0 (a fresh transient) latches s->disabled inside the island, so
    // ready flips 0 next frame (GL resumes LOD0) — at worst a single self-healing frame,
    // and the fail-soft log above fires. No persistent hole.
    if (s_vkTerrainEnabled) {
        if (gosPostProcess* ppVk = getGosPostProcess()) {
            const int ran = mc2_vk_terrain_island_run(ppVk->getWidth(), ppVk->getHeight());
            if (s_vkTerrainActive && s_vkTerrainFeedValid && ran == 0) {
                // We SKIPPED LOD0 this frame but VK failed to co-write -> this frame has a
                // 1-frame LOD0 gap. Loud one-shot; the island latched disabled so ready
                // will be 0 next frame and GL resumes drawing LOD0 (self-heal).
                static bool s_gapLogged = false;
                if (!s_gapLogged) {
                    fprintf(stderr, "[VK_TERRAIN_ISLAND] FAIL-SOFT: run()==0 AFTER LOD0-skip "
                                    "-> 1-frame LOD0 gap this frame; island latched off, GL "
                                    "resumes LOD0 next frame (self-heal).\n");
                    fflush(stderr);
                    s_gapLogged = true;
                }
            }
        }
    }
#endif
}

// ---------------------------------------------------------------------------
// Full heightfield upload — called once at map load.
// elevations: float[mapSide*mapSide] row-major.
// ---------------------------------------------------------------------------

void gos_TerrainLodChunk_UploadHeightFull(const float* elevations, int mapSide)
{
    if (s_heightSsbo.backendHandle == 0)
    {
        fprintf(stderr, "[TerrainLodChunk] UploadHeightFull called before Init\n");
        fflush(stderr);
        return;
    }
    if (!elevations || mapSide <= 0)
        return;

    GLsizeiptr bytes = (GLsizeiptr)mapSide * mapSide * sizeof(float);

    const GLuint heightBuf = static_cast<GLuint>(s_heightSsbo.backendHandle);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, heightBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, elevations, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_HEIGHT_SSBO_BINDING, heightBuf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    s_mapSide = mapSide;
    s_halfMap = (float)mapSide * 128.0f * 0.5f;

#ifdef MC2_VULKAN_ISLAND
    // VK-P10 TERRAIN TIER-1: the height field changed -> bump the snapshot key so the
    // VK terrain island re-copies it (glGetBufferSubData map-load) on the next frame.
    ++g_terrainHeightVersion;
#endif

    // REGISTRY-TERRAIN-SSBO-1: register the live height SSBO (observe-only metadata;
    // never read by the draw path). Registered here (not at Init) because the byte
    // size is only known once the full heightfield is uploaded.
    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::TerrainHeightSsbo;
        d.kind      = RenderCore::RenderResourceKind::Buffer;
        d.lifetime  = RenderCore::RenderResourceLifetime::Mission;  // rebuilt per mission load (heightfield upload)
        d.format    = RenderCore::RenderResourceFormat::BufferRaw;
        d.debugName = s_heightSsbo.debugName;
        d.glName    = static_cast<GLuint>(s_heightSsbo.backendHandle);
        d.sizeBytes = static_cast<uint64_t>(bytes);
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

#ifdef _DEBUG
    // First-frame readback verify: confirm that the GPU round-trips the first
    // float correctly. glGetBufferSubData is available on all desktop GL >=3.1.
    float firstSample = 0.0f;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_heightSsbo.backendHandle));
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(float), &firstSample);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    if (firstSample != elevations[0])
    {
        fprintf(stderr,
            "[TerrainLodChunk] readback mismatch: wrote %.6f, got %.6f\n",
            elevations[0], firstSample);
        fflush(stderr);
    }
#endif
}

// TERRAIN-VISUAL-HEIGHT-SAMPLE-1 Stage 1: upload the 4x VISUAL heightfield bake to
// a dedicated SSBO (binding 26). Lazily allocated. NO geometry samples it yet —
// Stage 2 (corner-pinned interior subdivision) consumes it. Load+log only here.
void gos_TerrainLodChunk_UploadVisualHeightFull(const float* visualHeights, int V)
{
    if (!visualHeights || V <= 0)
        return;
    if (s_visualHeightSsbo.backendHandle == 0)
    {
        GLuint local = 0;
        glGenBuffers(1, &local);
        s_visualHeightSsbo.backendHandle = static_cast<uint32_t>(local);
        if (s_visualHeightSsbo.backendHandle == 0)
        {
            fprintf(stderr, "[VISUAL_HEIGHT v1] glGenBuffers failed\n");
            fflush(stderr);
            return;
        }
    }
    GLsizeiptr bytes = (GLsizeiptr)V * (GLsizeiptr)V * (GLsizeiptr)sizeof(float);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_visualHeightSsbo.backendHandle));
    glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, visualHeights, GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    s_visualSide = V;   // remembered for the displaced draw (u_visualSide)

    // TERRAIN-VISUAL-HEIGHT-SSBO-OWNER-1: register the live visual-height SSBO
    // (observe-only metadata; never read by the draw path). Reached here only when
    // the upstream gate (MC2_TERRAIN_VISUAL_HEIGHT/_DISPLACE + bake) called us.
    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::TerrainVisualHeightSsbo;
        d.kind      = RenderCore::RenderResourceKind::Buffer;
        d.lifetime  = RenderCore::RenderResourceLifetime::Mission;  // rebuilt per mission load
        d.format    = RenderCore::RenderResourceFormat::BufferRaw;
        d.debugName = s_visualHeightSsbo.debugName;
        d.glName    = static_cast<GLuint>(s_visualHeightSsbo.backendHandle);
        d.sizeBytes = static_cast<uint64_t>(bytes);
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }
    // Bound to base 26 in the draw only when displacement is active.
    fprintf(stderr, "[VISUAL_HEIGHT v1] SSBO uploaded binding=%u V=%d bytes=%lld first=%.3f\n",
            TERRAIN_VISUAL_HEIGHT_SSBO_BINDING, V, (long long)bytes, visualHeights[0]);
    fflush(stderr);
}

// Step 5b: per-vertex terrainType upload (parallel to the heightfield). Used by
// the chunk frag's concrete material/colour selection (pureConcrete).
void gos_TerrainLodChunk_UploadTerrainTypeFull(const float* types, int mapSide)
{
    if (s_typeSsbo.backendHandle == 0 || !types || mapSide <= 0)
        return;
    GLsizeiptr bytes = (GLsizeiptr)mapSide * mapSide * sizeof(float);
    const GLuint typeBuf = static_cast<GLuint>(s_typeSsbo.backendHandle);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, typeBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, types, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_TYPE_SSBO_BINDING, typeBuf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // TERRAIN-LODCHUNK-SSBO-OWNER-1: register the live type SSBO (observe-only
    // metadata; never read by the draw path). Registered here (not at Init)
    // because the byte size is only known once the type data is uploaded.
    {
        RenderCore::RenderResourceDesc d;
        d.id        = s_typeSsbo.id;
        d.kind      = RenderCore::RenderResourceKind::Buffer;
        d.lifetime  = s_typeSsbo.lifetime;
        d.format    = RenderCore::RenderResourceFormat::BufferRaw;
        d.debugName = s_typeSsbo.debugName;
        d.glName    = static_cast<GLuint>(s_typeSsbo.backendHandle);
        d.sizeBytes = static_cast<uint64_t>(bytes);
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }
}

// TERRAIN-CONTROLMAP-SAMPLE-1: upload the authored RGBA control map as a plain
// GL_RGBA8 2D texture (GL_LINEAR / CLAMP_TO_EDGE, matches the colormap's
// filtering so bilinear blend across cells is consistent with today's sampling).
// Called ONLY when mclib/terrain.cpp actually loaded a sidecar (gate ON + file
// present). rgba: uint8[side*side*4] row-major, R=rock G=grass B=dirt A=concrete.
void gos_TerrainLodChunk_UploadControlMap(const unsigned char* rgba, int side)
{
    if (!rgba || side <= 0)
        return;

    if (s_controlMapTex == 0)
        glGenTextures(1, &s_controlMapTex);
    if (s_controlMapTex == 0)
    {
        fprintf(stderr, "[TERRAIN_CONTROLMAP v1] glGenTextures failed\n");
        fflush(stderr);
        return;
    }

    GLint prevActive = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    GLint prev2D = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev2D);

    glBindTexture(GL_TEXTURE_2D, s_controlMapTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, side, side, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, (GLuint)prev2D);
    glActiveTexture((GLenum)prevActive);

    s_controlMapSide = side;

    fprintf(stderr, "[TERRAIN_CONTROLMAP v1] uploaded handle=%u side=%d bytes=%zu\n",
            (unsigned)s_controlMapTex, side, (size_t)side * (size_t)side * 4u);
    fflush(stderr);
}

// TERRAIN-OVERLAY-V2-PARITY-1: upload the authored cement/pad/runway overlay
// sidecar as a plain GL_RGBA8 2D texture (GL_LINEAR / CLAMP_TO_EDGE, arbitrary
// WxH -- NOT tied to the vertex grid or the 128wu cement tile grid; sampled by
// world XY via u_overlayBounds in the frag). Called ONLY when mclib/terrain.cpp
// actually loaded a sidecar (gate ON + file present). rgba: uint8[w*h*4]
// row-major, RGB = pre-tinted cement/overlay diffuse, A = coverage/edge alpha.
void gos_TerrainLodChunk_UploadOverlaySidecar(const unsigned char* rgba, int w, int h,
                                               float boundsTopLeftX, float boundsTopLeftY,
                                               float boundsSizeX, float boundsSizeY)
{
    if (!rgba || w <= 0 || h <= 0)
        return;

    if (s_overlaySidecarTex == 0)
        glGenTextures(1, &s_overlaySidecarTex);
    if (s_overlaySidecarTex == 0)
    {
        fprintf(stderr, "[TERRAIN_OVERLAY_V2 v1] glGenTextures failed\n");
        fflush(stderr);
        return;
    }

    GLint prevActive = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    GLint prev2D = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev2D);

    glBindTexture(GL_TEXTURE_2D, s_overlaySidecarTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, (GLuint)prev2D);
    glActiveTexture((GLenum)prevActive);

    s_overlayBounds[0] = boundsTopLeftX;
    s_overlayBounds[1] = boundsTopLeftY;
    s_overlayBounds[2] = boundsSizeX;
    s_overlayBounds[3] = boundsSizeY;

    fprintf(stderr, "[TERRAIN_OVERLAY_V2 v1] uploaded handle=%u w=%d h=%d bytes=%zu "
            "bounds=(%.1f,%.1f,%.1f,%.1f)\n",
            (unsigned)s_overlaySidecarTex, w, h, (size_t)w * (size_t)h * 4u,
            boundsTopLeftX, boundsTopLeftY, boundsSizeX, boundsSizeY);
    fflush(stderr);
}

// TERRAIN-SHORELINE-MASK-1: upload the authored land-side wet/foam shoreline
// mask as a plain GL_RGBA8 2D texture (GL_LINEAR / CLAMP_TO_EDGE, arbitrary
// WxH -- NOT tied to the vertex grid; sampled by world XY via
// u_shorelineBounds in the frag, same pattern as the overlay-V2 sidecar).
// Called ONLY when mclib/terrain.cpp actually loaded a mask (gate ON + file
// present). rgba: uint8[w*h*4] row-major, R=signed dist, G=wet, B=foam,
// A=valid.
void gos_TerrainLodChunk_UploadShorelineMask(const unsigned char* rgba, int w, int h,
                                              float boundsTopLeftX, float boundsTopLeftY,
                                              float boundsSizeX, float boundsSizeY)
{
    if (!rgba || w <= 0 || h <= 0)
        return;

    if (s_shorelineMaskTex == 0)
        glGenTextures(1, &s_shorelineMaskTex);
    if (s_shorelineMaskTex == 0)
    {
        fprintf(stderr, "[TERRAIN_SHORELINE v1] glGenTextures failed\n");
        fflush(stderr);
        return;
    }

    GLint prevActive = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    GLint prev2D = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev2D);

    glBindTexture(GL_TEXTURE_2D, s_shorelineMaskTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, (GLuint)prev2D);
    glActiveTexture((GLenum)prevActive);

    s_shorelineBounds[0] = boundsTopLeftX;
    s_shorelineBounds[1] = boundsTopLeftY;
    s_shorelineBounds[2] = boundsSizeX;
    s_shorelineBounds[3] = boundsSizeY;

    fprintf(stderr, "[TERRAIN_SHORELINE v1] uploaded handle=%u w=%d h=%d bytes=%zu "
            "bounds=(%.1f,%.1f,%.1f,%.1f)\n",
            (unsigned)s_shorelineMaskTex, w, h, (size_t)w * (size_t)h * 4u,
            boundsTopLeftX, boundsTopLeftY, boundsSizeX, boundsSizeY);
    fflush(stderr);
}

// TERRAIN-SHORELINE-MASK-1: true once a shoreline mask has been uploaded.
// Consumed by gos_postprocess.cpp to suppress the legacy screen runShoreline()
// pass (recon landmine #6 -- avoid double-brightening the seam).
bool gos_TerrainLodChunk_IsShorelineMaskActive()
{
    return s_shorelineMaskTex != 0;
}

// Step 5c: per-vertex cement word upload (valid bit | atlas layer index). Built by
// gos_terrain_indirect after the cement catalog atlas is ready (PopulateRecipeCementWords).
void gos_TerrainLodChunk_UploadCementWordsFull(const unsigned int* words, int count, int mapSide)
{
    if (s_cementSsbo.backendHandle == 0 || !words || count <= 0 || mapSide <= 0)
        return;
    GLsizeiptr bytes = (GLsizeiptr)count * sizeof(unsigned int);
    const GLuint cementBuf = static_cast<GLuint>(s_cementSsbo.backendHandle);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, cementBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, words, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_CEMENT_SSBO_BINDING, cementBuf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

#ifdef MC2_VULKAN_ISLAND
    // VK-P10 CEMENT/ROAD sub-slice: the cement-word field changed -> bump the snapshot
    // key so the VK terrain island re-copies it (glGetBufferSubData map-load) next frame.
    ++g_terrainCementVersion;
#endif

    // TERRAIN-LODCHUNK-SSBO-OWNER-1: register the live cement SSBO. Conditional by
    // construction -- this upload only runs when cement words actually exist, so
    // the cement slot is registered only when the buffer carries real data.
    {
        RenderCore::RenderResourceDesc d;
        d.id        = s_cementSsbo.id;
        d.kind      = RenderCore::RenderResourceKind::Buffer;
        d.lifetime  = s_cementSsbo.lifetime;
        d.format    = RenderCore::RenderResourceFormat::BufferRaw;
        d.debugName = s_cementSsbo.debugName;
        d.glName    = static_cast<GLuint>(s_cementSsbo.backendHandle);
        d.sizeBytes = static_cast<uint64_t>(bytes);
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }
}

// ---------------------------------------------------------------------------
// Dirty-patch upload — called after setVertexHeight() modifies a block.
// rowData: compact float[(quadCountY+1)*(quadCountX+1)] row-major.
// The full SSBO is row-major with stride mapSide, so this MUST be row-by-row.
// ---------------------------------------------------------------------------

void gos_TerrainLodChunk_UploadHeightPatch(
    const float* rowData,
    int originX, int originY,
    int quadCountX, int quadCountY,
    int mapSide)
{
    if (s_heightSsbo.backendHandle == 0 || !rowData || mapSide <= 0)
        return;

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_heightSsbo.backendHandle));

    for (int row = 0; row <= quadCountY; ++row)
    {
        int        dstIdx    = (originY + row) * mapSide + originX;
        GLintptr   dstOffset = (GLintptr)dstIdx * sizeof(float);
        GLsizeiptr bytes     = (GLsizeiptr)(quadCountX + 1) * sizeof(float);
        const float* src     = rowData + row * (quadCountX + 1);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, dstOffset, bytes, src);
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}
