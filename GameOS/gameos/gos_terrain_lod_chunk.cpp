#include "gos_terrain_lod_chunk.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
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
#include "../../RenderCore/GpuBuffer.h"  // GPU-BUFFER-TIER0-LODCHUNK-RAII-1: owning RAII buffers (was GpuBufferOwner identity records)
#include "../../RenderCore/KtxLoader.h"  // TERRAIN-MATERIAL-TEXTURES-1: BC7 KTX2 albedo layer load
#include "gos_profiler.h"  // CLIFF-TESS-PERF: Tracy ZoneScopedN for the near-field tess mirror+draw
#include "gos_terrain_lighting.h"  // NIGHT-LIGHT-POOLS-1: per-frame light-pool SSBO accessors
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#include <optional>
#include <utility>  // std::move (GPU-BUFFER-TIER0-LODCHUNK-RAII-1: move-only patch buffers)
#include <vector>
#include <map>
#include "Gates/gate.h"

#ifdef MC2_VULKAN_ISLAND
#include "vulkan_terrain_island.h"   // VK-P10 TERRAIN TIER-1: LOD0 shared-GBuffer co-write
#include "vulkan_sky_island.h"       // VK-PARITY-F3: HDRI sky behind the terrain prefix
#include "vk_shared_gbuffer_gate.h"  // VK-P14 GATE-CONSOLIDATION-1 umbrella resolver
#include "vk_feed_dump.h"            // VK-OFFLINE-PROOFS-1: MC2_VK_FEED_DUMP golden dump
#endif


// Declarations, types and constants the split pieces share.


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

#ifdef MC2_VULKAN_ISLAND

struct TerrainGlFallbackAudit {
    int total = 0;
    int captured = 0;
    int glFallback = 0;
    int inactive = 0;
    int band = 0;
    int displace = 0;
    int edgeStitch = 0;   // patches that FELL to GL because of edge-stitch (now 0 by
                          // construction:
                          // stitched patches are VK-captured, not GL-fallback).
    int capturedStitched = 0; // of `captured`, how many carried a nonzero edgeStitch mask
                              // -- direct proof stitched patches are VK-owned (>0 expected
                              // on any map with LOD-band boundaries; the edgeStitch counter
                              // above is the excluded twin and must stay 0).
    int other = 0;
    int skirtEdges = 0;
};
#endif

extern unsigned int gos_GetTerrainNormalArrayTex();

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

extern void  gos_GetTerrainMatHeight(float*, float*, float*, float*);  // POM-PERLAYER-HEIGHT-1

extern bool  gos_TerrainMaterialLibEnabled();

// Remaining legacy tunables.
extern float gos_GetTerrainCliffShadowFloor();  // CLIFF SHADOW FLOOR

extern float gos_GetTerrainNormalsFromHeightStrength();

extern float gos_GetTerrainPOMScale();

// TERRAIN-CHUNK-POM-1: Stuff/MLR eye position (the SAME vec4 the legacy terrain
// frag consumes as "cameraPos"); local-extern per the established accessor
// pattern in this file (ruling R5).
extern void  gos_GetTerrainCameraPos(float*, float*, float*);

extern int   g_terrainMaterialProfile;   // global; 0 = legacy

// Step 5c: cement catalog atlas (tex3) accessors from gos_terrain_indirect.cpp.
extern unsigned int gos_terrain_indirect_getCementAtlasGLTex();

extern int          gos_terrain_indirect_getCementAtlasGridSide();

extern bool         gos_terrain_indirect_isCementAtlasReady();

// Stage B: transition mask array (GL_TEXTURE_2D_ARRAY, unit 11).
extern GLuint gos_terrain_indirect_getTransitionMaskArrayGL();

extern bool   gos_terrain_indirect_isTransitionMaskReady();

extern void  gos_GetTerrainMatNormalBoost(float*, float*, float*, float*);

extern void  gos_GetTerrainClassGrass(float*, float*, float*, float*);

extern void  gos_GetTerrainClassDirt(float*, float*, float*, float*);

extern float gos_GetTerrainDetailTiling();

extern float gos_GetTerrainDetailStrength();


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

// CEMENT-NIGHT-LIGHT-1: mission flat-pad light (gameos.hpp).
extern void   gos_GetTerrainMissionFlatLight(float* r, float* g, float* b);

// TERRAIN-AMBIENT-FLOOR-2: daylight level (gameos_graphics.cpp), pushed by
// gamecam.cpp from terrain_daylight.h. Night state is the TRACE input only.
extern float  gos_GetTerrainDaylightLevel();

float gos_TerrainSnowBrightnessDampen();   // VK-PARITY-F3 (defined below)

extern void   gos_GetNightState(bool* isNight, float* nightFactor);


// ---------------------------------------------------------------------------
// Patch geometry cache (Phase 4).
// Each unique (qcX, qcY, lodStep) triple gets one VBO+IBO pair.
// VBO contains int16_t[2] (localX, localY) per vertex.
// IBO contains uint16_t triangle indices.
// ---------------------------------------------------------------------------

// GPU-BUFFER-TIER0-LODCHUNK-RAII-1: the four per-patch buffers are owning
// RenderCore::GpuBuffer values (move-only) instead of bare GLuints. Untracked
// (no RenderResourceId): VBO/IBO are out of registry scope by design (per-draw
// rebind vertex/index data, not owned frame-graph resources) -- the RAII win
// here is create/delete pairing (cache clear frees GL, no hand-rolled loop).
struct PatchShape {
    RenderCore::GpuBuffer vbo =      // main patch: int16_t[2] (lx, ly) per vertex
        RenderCore::GpuBuffer(RenderCore::GpuBufferKind::Vertex, "TerrainLodChunkPatchVbo");
    RenderCore::GpuBuffer ibo =      // main patch: uint16_t triangle indices
        RenderCore::GpuBuffer(RenderCore::GpuBufferKind::Index, "TerrainLodChunkPatchIbo");
    int    vertexCount = 0;
    int    indexCount = 0;
    RenderCore::GpuBuffer skirtVbo = // Phase 6: int16_t[4] (lx, ly, isSkirt, _pad) per skirt vertex
        RenderCore::GpuBuffer(RenderCore::GpuBufferKind::Vertex, "TerrainLodChunkPatchSkirtVbo");
    RenderCore::GpuBuffer skirtIbo = // Phase 6: uint16_t triangle indices for skirts
        RenderCore::GpuBuffer(RenderCore::GpuBufferKind::Index, "TerrainLodChunkPatchSkirtIbo");
    int    skirtVertexCount = 0;
    int    skirtIndexCount = 0;
    // Phase 10.2b: per-edge index ranges into skirtIbo (build order N,S,W,E) so
    // the driver can draw ONLY the edges whose neighbour LOD differs (per-block
    // edge mask) instead of all four. Offsets/counts are in INDEX units.
    int    skirtEdgeOffset[4];   // 0=N, 1=S, 2=W, 3=E
    int    skirtEdgeCount[4];
};

namespace gos_terrain_indirect { bool IsFrameSolidArmed(); }

// GlStateGuard slice 2 kill-switch. Default-ON: when set, the terrain chunk
// draw owns its depth/blend/cull/mask/func via RAII guards (mc2gl::GlScoped*).
// MC2_GLSTATEGUARD_TERRAIN=0 reverts to the legacy hand-rolled save/restore
// (kept verbatim, nothing deleted) — the A/B used to prove the guards are
// pixel-neutral. Sampled once at process start.
static bool glStateGuardTerrainEnabled() {
    static const bool on = mc2::gate::get<"MC2_GLSTATEGUARD_TERRAIN">();
    return on;
}

// REDUNDANT-PASS-HUNT-1 (re-compute class): the terrain-chunk per-frame uniform
// setup in gos_TerrainLodChunk_SubmitDrawCommands used to call getenv() ~11 times
// EVERY frame to resolve mission-constant debug/tuning gates (FORCE_COLOR, DIAG,
// CEMENT_DIAG_CONNECT, SLOPE_BIAS[_STRENGTH], CLIFF_TRIPLANAR[_STRENGTH],
// MACRO_VARIATION[_STRENGTH], EDGE_FEATHER[_STRENGTH]). getenv() on Windows takes
// a CRT lock and linear-scans the process environment block; none of these vars
// can change mid-process, so resolving them per frame is pure redundant recompute.
// These resolvers cache the SAME values once at first use — byte-identical result,
// matching the established `static const ... = [](){...}()` pattern already used
// throughout this file. The per-frame block reads these
// instead of hitting getenv. Uniform VALUES uploaded are unchanged.
// Resolved-once cached mirrors of the per-frame terrain-uniform env gates.
const bool kTglcForceColor = mc2::gate::get<"MC2_TERRAIN_LOD_CHUNK_FORCE_COLOR">();
// SHORELINE-GLOW-1: the two shoreline accessors are deliberately NOT in this
// anonymous namespace -- mclib/terrain.cpp calls gos_TerrainShorelineGateOn()
// too, so it needs external linkage. Defined at global scope further down,
// next to gos_TerrainLodChunk_IsShorelineMaskActive(); declared in the header.
const int kTglcDiag = mc2::gate::get<"MC2_TERRAIN_LOD_CHUNK_DIAG">();
const bool  kTglcCementDiagConnect = mc2::gate::get<"MC2_TERRAIN_CEMENT_DIAG_CONNECT">();
// CEMENT-ATLAS-SEAM-1: default ON (killswitch MC2_CEMENT_ATLAS_SEAM_FIX=0).
const bool kTglcCementAtlasSeamFix = mc2::gate::get<"MC2_CEMENT_ATLAS_SEAM_FIX">();
// NIGHT-LIGHT-POOLS-2: soft falloff tail (default ON; =0 exact legacy
// GetFalloff linear ramp) + mover-light dim factor (default 0.5; 1.0 = off).
extern const bool kTglcLightPoolSoft = mc2::gate::get<"MC2_LIGHT_POOL_SOFT">();
// NIGHT-LIGHT-POOLS-4: default dropped 0.5 -> 0.25 (5-mech group still read as
// one saturated egg at 0.5; compression can't help five overlapping full discs).
extern const float kTglcLightPoolMoverDim = mc2::gate::get<"MC2_LIGHT_POOL_MOVER_DIM">();
// NIGHT-LIGHT-POOLS-4: mover pool radius scale (shrinks close/far distances of
// mover-attached lights only; 1.0 = legacy radii = killswitch).
extern const float kTglcLightPoolMoverRadius = mc2::gate::get<"MC2_LIGHT_POOL_MOVER_RADIUS">();
// NIGHT-LIGHT-POOLS-4: mover pools combine by per-channel MAX (no additive
// stacking across an overlapping lance); =0 restores the additive sum.
extern const bool kTglcLightPoolMoverMax = mc2::gate::get<"MC2_LIGHT_POOL_MOVER_MAXBLEND">();
// NIGHT-LIGHT-POOLS-3: tone-compress the summed pool term (default ON;
// =0 restores the legacy min(pool,1) hard clamp -> stacked-pool blowout).
extern const bool kTglcLightPoolCompress = mc2::gate::get<"MC2_LIGHT_POOL_COMPRESS">();
// MOVER-CONTACT-SHADOW-1 (MASTER-FAIL #7): strength in [0,1] of the under-mech
// contact shadow that carves a mover's own bright light-pool center + darkens
// the ground beneath the body. Default 0.0 => uniform 0 => byte-identical
// (mix() collapses, no darkening). Recommended ~0.7. MC2_MOVER_CONTACT_SHADOW.
extern const float kTglcMoverContactShadow = mc2::gate::get<"MC2_MOVER_CONTACT_SHADOW">();
const bool  kTglcSlopeBias         = mc2::gate::get<"MC2_TERRAIN_SLOPE_BIAS">();
const float kTglcSlopeBiasStr      = mc2::gate::get<"MC2_TERRAIN_SLOPE_BIAS_STRENGTH">();   // row: default 1, > 0
const bool  kTglcCliffTriplanar    = mc2::gate::get<"MC2_TERRAIN_CLIFF_TRIPLANAR">();
const float kTglcCliffTriplanarStr = mc2::gate::get<"MC2_TERRAIN_CLIFF_TRIPLANAR_STRENGTH">();
// TERRAIN-CLIFF-HEIGHT-NORMAL-1: derive the cliff SHADING normal from the height
// gradient of the cooked cliff displacement (mat5 layer-5 alpha) so the rich rock
// relief actually catches light (the smooth marble rgb normal left it flat).
// Trace knob MC2_TERRAIN_CLIFF_HEIGHT_NORMAL_STRENGTH, default 2.0; only consumed
// inside the useTriplanarCliff frag block. Values <= 0 are rejected (the row is > 0) and use the default.
const float kTglcCliffHeightNormalStr = mc2::gate::get<"MC2_TERRAIN_CLIFF_HEIGHT_NORMAL_STRENGTH">();
// TERRAIN-CLIFF-POM-1: triplanar Parallax Occlusion Mapping on cliff faces.
// Gate MC2_TERRAIN_CLIFF_POM (default OFF -> u_cliffPom.x=0 -> frag march skipped,
// the triplanar block behaves exactly as TRIPLANAR-1). Depth (world-unit height
// scale, default 12) via MC2_TERRAIN_CLIFF_POM_DEPTH; max march steps (clamp
// 8..32, default 24) via MC2_TERRAIN_CLIFF_POM_STEPS.
const bool  kTglcCliffPom          = mc2::gate::get<"MC2_TERRAIN_CLIFF_POM">();
const float kTglcCliffPomDepth     = mc2::gate::get<"MC2_TERRAIN_CLIFF_POM_DEPTH">();
const float kTglcCliffPomSteps     = []() {
    float s = mc2::gate::get<"MC2_TERRAIN_CLIFF_POM_STEPS">();
    if (s < 8.0f) s = 8.0f; if (s > 32.0f) s = 32.0f;
    return s;
}();
// TERRAIN-CLIFF-DEBUG: bounded debug-viz gate (default 0/off -> u_cliffDebug=0 ->
// frag block skipped -> byte-identical). 1..4 select the debug output (see frag).
const int kTglcCliffDebug = mc2::gate::get<"MC2_TERRAIN_CLIFF_DEBUG">();
const float kTglcMacroVariation    = []() {
    if (!mc2::gate::get<"MC2_TERRAIN_MACRO_VARIATION">()) return 0.0f;
    return mc2::gate::get<"MC2_TERRAIN_MACRO_VARIATION_STRENGTH">();
}();
// WATER-FAR-BAND-ATTRIB-1 (batch3 item C): default flipped ON to match
// WATER-EDGE-FEATHER (54a70e2d). With water feathered ON and terrain feather
// OFF, the water dissolves at the map edge but EXPOSES the un-feathered dark
// submerged-terrain rim -> the residual hard navy far-band line at grazing
// angles (mc2_01 saved_205644; MC2_WATER_DEBUG_MODE=2 shows water alpha
// already 0 across the band -> the navy pixels are terrain, not water).
// Killswitch: MC2_TERRAIN_EDGE_FEATHER=0.
const bool kTglcEdgeFeather = mc2::gate::get<"MC2_TERRAIN_EDGE_FEATHER">();
const float kTglcEdgeFeatherStr    = mc2::gate::get<"MC2_TERRAIN_EDGE_FEATHER_STRENGTH">();
// TERRAIN-MATERIAL-TEXTURES-1: per-layer PBR albedo array gate (default OFF ->
// u_useMatAlbedo uploads 0 -> frag composition verbatim -> byte-identical).
const bool  kTglcMatAlbedo         = mc2::gate::get<"MC2_TERRAIN_MATERIAL_TEXTURES">();

// ---------------------------------------------------------------------------
// SLICE 3a — cliff-tessellation PLUMBING spike (pass-through, NO displacement).
// Gate MC2_TERRAIN_CLIFF_DISPLACE (default OFF). When OFF nothing below runs:
// the tess program is never built, the draw stays GL_TRIANGLES on the base
// program, and the frame is byte-identical. When ON, a variant program with
// TCS/TES (pass-through TES, barycentric interp, no displacement) is built and
// LOD0 (near-field) chunks draw as GL_PATCHES through it. Because the TES only
// reconstructs the interpolated clip position, the output is visually the same
// as the base GL_TRIANGLES draw — this slice proves the tessellation PLUMBING.
// ---------------------------------------------------------------------------
extern const bool s_cliffTessGate = mc2::gate::get<"MC2_TERRAIN_CLIFF_DISPLACE">();
static const float s_cliffTessWanted = mc2::gate::get<"MC2_TERRAIN_CLIFF_DISPLACE_MAXTESS">();
static float s_cliffTessClamped = -1.0f;   // set once at program build (GL valid)

// Clamp the requested tess level to GL_MAX_TESS_GEN_LEVEL. Called once inside
// the tess-program success branch where a GL context is valid.
static float clampTess(float want) {
    GLint maxGen = 0;
    glGetIntegerv(GL_MAX_TESS_GEN_LEVEL, &maxGen);
    float m = (maxGen > 0) ? (float)maxGen : 64.0f;   // GL spec floor is 64
    float c = want < 1.0f ? 1.0f : (want > m ? m : want);
    if (c != want)
        std::fprintf(stderr, "[CLIFF_TESS 3a] tess %.1f clamped to GL_MAX_TESS_GEN_LEVEL %.0f\n", want, m);
    return c;
}

// The tess variant program + its u_cliffTessLevel location. Built in
// gos_TerrainLodChunk_Init next to the base makeProgram when the gate is ON.
glsl_program* s_terrainTessProgram = nullptr;
static GLint         s_locCliffTessLevel  = -1;

// CLIFF-TESS-PERF (slice 3a hoist): the full enumerate-and-copy mirror
// (mirrorTerrainUniforms) is expensive (glGetActiveUniform + per-uniform
// glGetUniformLocation ×2 + glGetnUniform* over ALL active uniforms). It was
// running ONCE PER NEAR-FIELD PATCH every frame (+~1ms measured at tess=1).
// It only needs to run ONCE PER FRAME for the frame-constant uniforms; the
// handful of per-patch uniforms are then mirrored with a targeted, cached-
// location fast path each patch. s_tessMirroredThisFrame is reset to false at
// the top of SubmitDrawCommands (once/frame) and set true by the first near-
// field tess patch's full mirror.
bool  s_tessMirroredThisFrame = false;
// Cached tess-program locations for the per-patch uniform set (resolved once,
// lazily, on the first tess patch). Base-program locations reuse the existing
// s_loc* statics. -2 = "not yet resolved"; -1 = "resolved, absent".
static GLint s_tessLocBlockOriginX = -2;
static GLint s_tessLocBlockOriginY = -2;
static GLint s_tessLocLodStep      = -2;
static GLint s_tessLocQuadCountX   = -2;
static GLint s_tessLocQuadCountY   = -2;
static GLint s_tessLocEdgeStitch   = -2;
static GLint s_tessLocShadowTier   = -2;
static GLint s_tessLocMorphFactor  = -2;
static GLint s_tessLocVisualDisplace = -2;
static GLint s_tessLocSkirtDepth   = -2;

// SLICE 3a — COMPLETE per-draw input mirror. The tess program shares the base
// program's VS+FS, so it needs EVERY per-draw uniform the base program was fed.
// Rather than duplicate the ~150-site inline bind block (and risk missing one),
// this reads back every active uniform from the (fully-configured) base program
// and copies it to the tess program with glProgramUniform* (samplers included;
// sampler uniforms are just their texunit int, and the textures/SSBOs themselves
// bind to program-independent global units/binding-points that the base block
// already set for this draw). Runs ONLY for near-field GL_PATCHES draws when the
// gate is ON; never default-OFF.
//
// COMPLETENESS: this mirror is complete for scalar/vector/mat4 uniforms and for
// any uniform with size==1. Uniform ARRAYS (size>1) of scalar/vector/int/sampler
// types are NOT mirrored — the scalar/int branches read only the FIRST element
// into a small stack buffer, so uploading `size` elements from it would read out
// of bounds and corrupt the GPU-side array. Those cases now emit a WARN and are
// SKIPPED (only mat4 arrays, e.g. cascade matrices, are handled per-element). If
// slice 3b introduces such an array uniform (cascade matrices packed as vecs,
// per-layer scalar arrays, etc.) it MUST be handled explicitly here.
void mirrorTerrainUniforms(GLuint src, GLuint dst) {
    GLint count = 0;
    glGetProgramiv(src, GL_ACTIVE_UNIFORMS, &count);
    for (GLint i = 0; i < count; ++i) {
        char name[128];
        GLsizei nlen = 0; GLint size = 0; GLenum type = 0;
        glGetActiveUniform(src, (GLuint)i, sizeof(name), &nlen, &size, &type, name);
        if (nlen == 0) continue;
        // Strip the "[0]" array suffix glGetActiveUniform reports for arrays so
        // the name resolves in the destination program too.
        if (nlen >= 3 && name[nlen-1] == ']') {
            char* br = std::strchr(name, '[');
            if (br) *br = '\0';
        }
        GLint sloc = glGetUniformLocation(src, name);
        GLint dloc = glGetUniformLocation(dst, name);
        if (sloc < 0 || dloc < 0) continue;   // not present in both
        switch (type) {
            case GL_FLOAT: {
                // GUARD: size>1 array would read only v[0] but upload `size`
                // elements from the 4-elem stack buffer -> OOB read + GPU
                // corruption. Fail loud + skip rather than corrupt (see the
                // COMPLETENESS note above; 3b must add a per-element path).
                if (size > 1) {
                    std::fprintf(stderr, "[CLIFF_TESS 3a] WARN unmirrored float ARRAY uniform '%s' size=%d (not handled; skipped)\n",
                                 name, (int)size);
                    break;
                }
                float v[4]={0,0,0,0}; glGetnUniformfv(src, sloc, sizeof(v), v);
                glProgramUniform1fv(dst, dloc, size, v); break; }
            case GL_FLOAT_VEC2: { float v[2]={0,0}; glGetnUniformfv(src, sloc, sizeof(v), v);
                glProgramUniform2fv(dst, dloc, 1, v); break; }
            case GL_FLOAT_VEC3: { float v[3]={0,0,0}; glGetnUniformfv(src, sloc, sizeof(v), v);
                glProgramUniform3fv(dst, dloc, 1, v); break; }
            case GL_FLOAT_VEC4: { float v[4]={0,0,0,0}; glGetnUniformfv(src, sloc, sizeof(v), v);
                glProgramUniform4fv(dst, dloc, 1, v); break; }
            case GL_FLOAT_MAT4: {
                // Arrays of mat4 (e.g. cascade matrices) copied element-by-element.
                for (GLint e = 0; e < size; ++e) {
                    float m[16]; GLint sl = (e==0)?sloc:glGetUniformLocation(src, name);
                    // For array elements re-resolve the indexed location.
                    if (size > 1) {
                        char en[160]; std::snprintf(en, sizeof(en), "%s[%d]", name, e);
                        sl = glGetUniformLocation(src, en);
                        GLint dl = glGetUniformLocation(dst, en);
                        if (sl < 0 || dl < 0) continue;
                        glGetnUniformfv(src, sl, sizeof(m), m);
                        glProgramUniformMatrix4fv(dst, dl, 1, GL_FALSE, m);
                    } else {
                        glGetnUniformfv(src, sl, sizeof(m), m);
                        glProgramUniformMatrix4fv(dst, dloc, 1, GL_FALSE, m);
                    }
                }
                break; }
            // Every integer/sampler uniform type used by this program.
            case GL_INT:
            case GL_BOOL:
            case GL_SAMPLER_2D:
            case GL_SAMPLER_2D_ARRAY:
            case GL_SAMPLER_2D_SHADOW:
            case GL_SAMPLER_2D_ARRAY_SHADOW: {
                // GUARD: size>1 array would read only v[0] but upload `size`
                // elements from the 4-elem stack buffer -> OOB read + GPU
                // corruption. Fail loud + skip rather than corrupt (see the
                // COMPLETENESS note above; 3b must add a per-element path).
                if (size > 1) {
                    std::fprintf(stderr, "[CLIFF_TESS 3a] WARN unmirrored int/sampler ARRAY uniform '%s' size=%d type=0x%x (not handled; skipped)\n",
                                 name, (int)size, (unsigned)type);
                    break;
                }
                GLint v[4]={0,0,0,0}; glGetnUniformiv(src, sloc, sizeof(v), v);
                glProgramUniform1iv(dst, dloc, size, v); break; }
            case GL_INT_VEC2: { GLint v[2]={0,0}; glGetnUniformiv(src, sloc, sizeof(v), v);
                glProgramUniform2iv(dst, dloc, 1, v); break; }
            case GL_INT_VEC3: { GLint v[3]={0,0,0}; glGetnUniformiv(src, sloc, sizeof(v), v);
                glProgramUniform3iv(dst, dloc, 1, v); break; }
            case GL_INT_VEC4: { GLint v[4]={0,0,0,0}; glGetnUniformiv(src, sloc, sizeof(v), v);
                glProgramUniform4iv(dst, dloc, 1, v); break; }
            default:
                // No other uniform types exist in terrain_lod_chunk. If one is
                // ever added, this warns rather than silently dropping it.
                std::fprintf(stderr, "[CLIFF_TESS 3a] WARN unmirrored uniform '%s' type=0x%x\n",
                             name, (unsigned)type);
                break;
        }
    }
    // The tess-only uniform (not present on the base program).
    if (s_locCliffTessLevel >= 0)
        glProgramUniform1f(dst, s_locCliffTessLevel, s_cliffTessClamped);
}

// CLIFF-TESS-PERF: resolve+cache the tess-program locations for the per-patch
// uniform set once (lazily, on the first tess patch). Base-program locations
// reuse the existing s_loc* statics.
void resolveTessPatchLocs(GLuint tessProg) {
    if (s_tessLocBlockOriginX != -2) return;  // already resolved
    s_tessLocBlockOriginX   = glGetUniformLocation(tessProg, "u_blockOriginX");
    s_tessLocBlockOriginY   = glGetUniformLocation(tessProg, "u_blockOriginY");
    s_tessLocLodStep        = glGetUniformLocation(tessProg, "u_lodStep");
    s_tessLocQuadCountX     = glGetUniformLocation(tessProg, "u_quadCountX");
    s_tessLocQuadCountY     = glGetUniformLocation(tessProg, "u_quadCountY");
    s_tessLocEdgeStitch     = glGetUniformLocation(tessProg, "u_edgeStitch");
    s_tessLocShadowTier     = glGetUniformLocation(tessProg, "u_shadowTier");
    s_tessLocMorphFactor    = glGetUniformLocation(tessProg, "u_morphFactor");
    s_tessLocVisualDisplace = glGetUniformLocation(tessProg, "u_visualDisplace");
    s_tessLocSkirtDepth     = glGetUniformLocation(tessProg, "u_skirtDepth");
}

// ---------------------------------------------------------------------------
// Static SSBO state — all GL objects live here, never in mclib/.
// ---------------------------------------------------------------------------

// GPU-BUFFER-TIER0-LODCHUNK-RAII-1: the five lod-chunk SSBOs graduate from
// GpuBufferOwner identity records to owning RenderCore::GpuBuffer RAII values
// (the "LATER slice" flagged in GpuBufferOwner.h:15). The wrapper's ops mirror
// the raw GL calls 1:1 (same args/order/flags/slots -- see gpu_buffer_gl.cpp),
// so each converted site issues the identical GL command stream; what moves
// into the class is create/destroy pairing, the registry upsert/invalidate,
// and the gated KHR_debug label. Lifetime Mission: (re-)uploaded per mission
// load via glBufferData.
RenderCore::GpuBuffer s_heightSsbo{  // GL handle; name() 0 = not yet allocated
    RenderCore::RenderResourceId::TerrainHeightSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainHeightSsbo",
    RenderCore::GpuBufferKind::Storage};
// TERRAIN-VISUAL-HEIGHT-SSBO-OWNER-1 (now RAII, see block above): the 4x visual
// heightfield SSBO (binding 26). Lifetime Mission: uploaded per mission load via
// glBufferData. CREATE is gated upstream (mclib/terrain.cpp: MC2_TERRAIN_VISUAL_HEIGHT
// / _DISPLACE + a visual_height_4x.r32 bake) — when the gate is off / no bake ships,
// the upload entry is never called, name() stays 0, and nothing registers
// (passthrough byte-identical). No stock bake ships → create path is offline-only.
RenderCore::GpuBuffer s_visualHeightSsbo{ // TERRAIN-VISUAL-HEIGHT-SAMPLE-1: 4x visual heightfield (binding 26)
    RenderCore::RenderResourceId::TerrainVisualHeightSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainVisualHeightSsbo",
    RenderCore::GpuBufferKind::Storage};
int    s_visualSide       = 0; // V = (mapSide-1)*4+1, fine grid side (0 = bake not loaded)
// TERRAIN-LOD-GEOMORPH-1: float count of the max-mip levels APPENDED to the
// binding-26 SSBO after the V*V fine bake (5 levels x mapSide^2, strides
// 2/4/5/10/20). 0 = no mips shipped -> u_geomorphMips uploads 0 -> the vert's
// coarse-band branch behaves exactly as S2 (legacy layout untouched).
int    s_visualMipFloats  = 0;
// TERRAIN-REAUTH-UNPIN-1 Half B: coarse object-proximity displacement damp
// (binding 27). Static half = building footprints from the bake sidecar
// (visual_damp.r32), uploaded once per mission load; dynamic half = per-frame
// mover stamps min-combined on the CPU and re-uploaded (side^2 floats — tiny).
// CREATE gated upstream like the visual-height SSBO (displace gate + sidecar);
// glName 0 => never bound, u_visualDampOn stays 0 (passthrough).
RenderCore::GpuBuffer s_visualDampSsbo{
    RenderCore::RenderResourceId::TerrainVisualDampSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainVisualDampSsbo",
    RenderCore::GpuBufferKind::Storage};
int s_visualDampSide = 0;                 // == mapSide when loaded
std::vector<float> s_visualDampStatic;    // load-time (buildings)
std::vector<float> s_visualDampCombined;  // static + mover stamps
bool s_visualDampHadMovers = false;       // skip redundant re-uploads
// TERRAIN-VISUAL-HEIGHT-S2-ALLLOD: per-LOD-band displaced-chunk counters, reset
// and logged once per submit alongside [TerrainLOD v1] so acceptance has hard
// per-band numbers. Index = LOD level (0..5), same mapping as [TerrainLOD v1].
static int s_visualDisplacedCounts[6] = {0, 0, 0, 0, 0, 0};
#ifdef MC2_VULKAN_ISLAND
static bool terrainGlFallbackAuditEnabled()
{
    static const bool on = mc2::gate::get<"MC2_VK_TERRAIN_GL_FALLBACK_AUDIT">();
    return on;
}
#endif
// TERRAIN-LODCHUNK-SSBO-OWNER-1 (now RAII, see block above): the LOD-chunk
// type/cement SSBOs. Lifetime Mission: (re-)uploaded per mission load via
// glBufferData.
RenderCore::GpuBuffer s_typeSsbo{   // Step 5b: per-vertex terrainType SSBO (binding 24)
    RenderCore::RenderResourceId::TerrainTypeSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainTypeSsbo",
    RenderCore::GpuBufferKind::Storage};
RenderCore::GpuBuffer s_cementSsbo{ // Step 5c: per-vertex cement word SSBO (binding 25)
    RenderCore::RenderResourceId::TerrainCementSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainCementSsbo",
    RenderCore::GpuBufferKind::Storage};
int    s_mapSide    = 0;   // mapSide stored at last UploadHeightFull
float  s_halfMap    = 0.0f;// (mapSide * 128.0 * 0.5)

// TERRAIN-CONTROLMAP-SAMPLE-1: authored RGBA override control-map texture
// (unit 12). Plain GLuint (not GpuBufferOwner) — mirrors gos_terrain_height_tex.cpp's
// self-contained texture handling for a single-owner render-only raster. glName
// 0 = not loaded (gate off or no sidecar) -> u_useControlMap uploads 0 at draw.
GLuint s_controlMapTex  = 0;
int    s_controlMapSide = 0;

// TERRAIN-OVERLAY-V2-PARITY-1: authored cement/pad/runway overlay sidecar
// texture (unit TERRAIN_OVERLAY_SIDECAR_TEXUNIT). Plain GLuint, same
// self-contained single-owner pattern as s_controlMapTex. glName 0 = not
// loaded (gate off or no sidecar) -> u_useOverlaySidecar uploads 0 at draw.
GLuint s_overlaySidecarTex = 0;
float  s_overlayBounds[4]  = {0.0f, 0.0f, 0.0f, 0.0f};  // topLeftX,topLeftY(=maxY),sizeX,sizeY

// TERRAIN-OVERLAY-V2-DECAL-SUPPRESS-1: read-only accessors (see header). Let the
// indirect decal bake key off the SAME resolved sidecar-loaded state the frag
// samples (not a re-read of the env), so suppression matches what actually draws.
bool         gos_TerrainLodChunk_OverlaySidecarLoaded() { return s_overlaySidecarTex != 0; }
const float* gos_TerrainLodChunk_OverlayBounds()        { return s_overlayBounds; }

// TERRAIN-SHORELINE-V3: authored land-side wet/foam shoreline mask sidecar
// texture (unit TERRAIN_SHORELINE_TEXUNIT). Plain GLuint, same self-contained
// single-owner pattern as s_overlaySidecarTex. glName 0 = not loaded (no
// sidecar found) -> u_hasShorelineMask uploads 0 at draw -> the elevation
// band still renders (mask is now an optional modulator, not a requirement);
// u_useShorelineMask (the band's own on/off) is driven directly by the
// MC2_TERRAIN_SHORELINE gate, independent of this texture's state.
GLuint s_shorelineMaskTex   = 0;
float  s_shorelineBounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // topLeftX,topLeftY(=maxY),sizeX,sizeY

// WATER-SHORE-MASK-1: water-shader accessors (see header).
unsigned int gos_TerrainLodChunk_ShorelineMaskTex()     { return s_shorelineMaskTex; }
const float* gos_TerrainLodChunk_ShorelineMaskBounds()  { return s_shorelineBounds; }

// WATER-SHORE-EDGE-1: hi-res R32F visual-height texture for the water FS
// per-fragment shore crossing. Same single-owner pattern as the mask above.
GLuint s_waterShoreHeightTex       = 0;
float  s_waterShoreHeightBounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};

unsigned int gos_TerrainLodChunk_WaterShoreHeightTex()    { return s_waterShoreHeightTex; }
// VK-PARITY-WATER-SHORE-1: bumped by every shore-data (mask / height) upload so
// the VK water island can re-import its copies of these GL textures when a new
// mission replaces the contents behind the SAME GL texture ids. GL never reads it.
unsigned s_waterShoreDataGen = 0;
unsigned int gos_TerrainLodChunk_WaterShoreDataGeneration() { return s_waterShoreDataGen; }
const float* gos_TerrainLodChunk_WaterShoreHeightBounds() { return s_waterShoreHeightBounds; }

// ---------------------------------------------------------------------------
// TERRAIN-MATERIAL-TEXTURES-1: 6-layer BC7 sRGB albedo GL_TEXTURE_2D_ARRAY
// (rock/grass/dirt/concrete/snow/cliff, layer order = MAT_LAYER_* 0..5) loaded
// straight from the cooked data/terrain_layers/<channel>_albedo.ktx2 files.
// Plain GLuint, same self-contained single-owner pattern as s_controlMapTex.
// glName 0 = not loaded (gate off / load failed) -> u_useMatAlbedo uploads 0
// at draw -> frag takes the verbatim colormap-tint else-path (byte-identical).
// Built lazily at first gate-ON bind (GL context guaranteed live at draw).
// ---------------------------------------------------------------------------
GLuint s_matAlbedoArrayTex   = 0;
bool   s_matAlbedoLoadTried  = false;
// JSON overrides (terrain_materials.json "layers.<channel>.albedo" +
// "textureRoot"), applied by terrainMaterials_apply BEFORE the first draw
// (mission load precedes the first chunk bind). Empty = shipped default path.
static const int kMatAlbedoLayerCount = 6;
static const char* const kMatAlbedoChannelNames[kMatAlbedoLayerCount] = {
    "rock", "grass", "dirt", "concrete", "snow", "cliff"
};
// TERRAIN-MATERIAL-TEXTURES-1-FIX (fix A, INDEX ORACLE -- shared source of truth):
// This table is the C++ mirror of the frag's MAT_LAYER_* constants
// (shaders/include/terrain_mat_layers.hglsl). The albedo array is uploaded in
// kMatAlbedoChannelNames[] order (layer i = channel i), and the frag samples
// channel C at texture()'s layer arg = MAT_LAYER_<C>. Those two orders MUST agree
// or dirt renders with the concrete/road albedo (the reported bug). kMatAlbedoExpectedLayer[i]
// records what the frag's constant table says layer i should be; a load-time HARD
// ERROR fires (and the array is disabled -> legacy tint path) if array index i !=
// its frag constant. Keep these six values equal to their index -- if the frag
// header ever reorders, update BOTH here and terrain_mat_layers.hglsl in lockstep.
//   MAT_LAYER_ROCK=0 GRASS=1 DIRT=2 CONCRETE=3 SNOW=4 MARBLE_CLIFF=5
static const int kMatAlbedoExpectedLayer[kMatAlbedoLayerCount] = {
    0,  // rock     == MAT_LAYER_ROCK
    1,  // grass    == MAT_LAYER_GRASS
    2,  // dirt     == MAT_LAYER_DIRT
    3,  // concrete == MAT_LAYER_CONCRETE
    4,  // snow     == MAT_LAYER_SNOW
    5,  // cliff    == MAT_LAYER_MARBLE_CLIFF
};
static char  s_matAlbedoLayerPath[kMatAlbedoLayerCount][512] = {{0}};
static char  s_matAlbedoTextureRoot[512] = {0};  // default data/terrain_layers/
// JSON strength (matAlbedoStrength). <0 = "no JSON value" sentinel; the bind
// resolves env MC2_TERRAIN_MATERIAL_TEXTURES_STRENGTH > JSON > 0.7 default.
float s_matAlbedoStrengthJson = -1.0f;

// Setters consumed by terrain_material_lib.cpp (TinyJson layers{} extension).
// Local externs per the established accessor pattern in that file.
void gos_SetTerrainMatAlbedoStrength(float s) { s_matAlbedoStrengthJson = s; }
void gos_SetTerrainMatAlbedoTextureRoot(const char* root) {
    if (!root) { s_matAlbedoTextureRoot[0] = '\0'; return; }
    snprintf(s_matAlbedoTextureRoot, sizeof(s_matAlbedoTextureRoot), "%s", root);
}
void gos_SetTerrainMatAlbedoLayerPath(int layer, const char* path) {
    if (layer < 0 || layer >= kMatAlbedoLayerCount) return;
    if (!path) { s_matAlbedoLayerPath[layer][0] = '\0'; return; }
    snprintf(s_matAlbedoLayerPath[layer], sizeof(s_matAlbedoLayerPath[layer]), "%s", path);
}
int gos_GetTerrainMatAlbedoLayerIndex(const char* channelName) {
    if (!channelName) return -1;
    for (int i = 0; i < kMatAlbedoLayerCount; ++i)
        if (strcmp(channelName, kMatAlbedoChannelNames[i]) == 0) return i;
    return -1;
}

// TERRAIN-MATERIAL-TEXTURES-1 lazy loader: called at the first gate-ON bind
// (GL context live, like the other lazy chunk resources). Loads the 6 cooked
// BC7 sRGB KTX2 layers and uploads the stored block streams VERBATIM
// (glCompressedTexSubImage3D) into an immutable
// GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM array -- sRGB decode happens in the
// sampler (the BC7-sRGB audit finding; a linear internalformat here would
// double-bright the terrain after the shader's own lighting).
// ALL-or-nothing: any layer failing (missing file, not BC7-sRGB, dim/mip
// mismatch vs layer 0) leaves glName 0 and logs why -- the draw then uploads
// u_useMatAlbedo=0 and the frag stays on the verbatim tint path (fail-soft).
void tglc_EnsureMatAlbedoArrayLoaded()
{
    if (s_matAlbedoLoadTried) return;
    s_matAlbedoLoadTried = true;

    if (!GLEW_ARB_texture_compression_bptc) {
        printf("[TERRAIN_MAT_TEX] BPTC unsupported on this GL -- albedo array disabled\n");
        fflush(stdout);
        return;
    }

    // TERRAIN-MATERIAL-TEXTURES-1-FIX (fix A, INDEX ORACLE): print the
    // channel->array-layer table the loader will build, alongside the frag's
    // MAT_LAYER_* constant each channel is sampled at, and HARD-ERROR (disable the
    // array -> legacy tint) on any mismatch. This is the shared-source-of-truth
    // guard: the manifest cook order, this upload order, and the frag constants
    // must all agree or the reported "dirt shows road albedo" index bug recurs.
    {
        bool orderOk = true;
        printf("[TERRAIN_MAT_TEX] channel->layer table (loader upload order vs frag MAT_LAYER_* constant):\n");
        for (int i = 0; i < kMatAlbedoLayerCount; ++i) {
            const int frag = kMatAlbedoExpectedLayer[i];
            const bool match = (frag == i);
            if (!match) orderOk = false;
            printf("[TERRAIN_MAT_TEX]   loader layer %d = '%s'  frag MAT_LAYER = %d  %s\n",
                   i, kMatAlbedoChannelNames[i], frag, match ? "OK" : "*** MISMATCH ***");
        }
        fflush(stdout);
        if (!orderOk) {
            printf("[TERRAIN_MAT_TEX] ERROR: channel->layer order disagrees with the frag MAT_LAYER_* "
                   "constants -- albedo array DISABLED to avoid a wrong-material splat (fix A oracle). "
                   "Sync kMatAlbedoChannelNames[]/kMatAlbedoExpectedLayer[] with terrain_mat_layers.hglsl.\n");
            fflush(stdout);
            return;
        }
    }

    RenderCore::KtxImage imgs[kMatAlbedoLayerCount];
    int refW = 0, refH = 0, refMips = 0;
    for (int i = 0; i < kMatAlbedoLayerCount; ++i) {
        char path[1024];
        if (s_matAlbedoLayerPath[i][0] != '\0') {
            snprintf(path, sizeof(path), "%s", s_matAlbedoLayerPath[i]);
        } else {
            const char* root = (s_matAlbedoTextureRoot[0] != '\0')
                             ? s_matAlbedoTextureRoot : "data/terrain_layers/";
            const size_t rl = strlen(root);
            const bool needSlash = (rl > 0 && root[rl - 1] != '/' && root[rl - 1] != '\\');
            snprintf(path, sizeof(path), "%s%s%s_albedo.ktx2",
                     root, needSlash ? "/" : "", kMatAlbedoChannelNames[i]);
        }
        if (!RenderCore::ktxLoadRgba8(path, imgs[i])) {
            printf("[TERRAIN_MAT_TEX] layer %d (%s): load FAILED '%s' -- albedo array disabled\n",
                   i, kMatAlbedoChannelNames[i], path);
            fflush(stdout);
            return;
        }
        const RenderCore::KtxImage& img = imgs[i];
        if (!img.isCompressed || img.vkFormat != 146u) {  // VK_FORMAT_BC7_SRGB_BLOCK
            printf("[TERRAIN_MAT_TEX] layer %d (%s): '%s' vkFormat=%u is not BC7 sRGB (146) -- albedo array disabled\n",
                   i, kMatAlbedoChannelNames[i], path, img.vkFormat);
            fflush(stdout);
            return;
        }
        if (i == 0) { refW = img.width; refH = img.height; refMips = img.mipCount; }
        if (img.width != refW || img.height != refH || img.mipCount != refMips) {
            printf("[TERRAIN_MAT_TEX] layer %d (%s): %dx%d mips=%d != layer0 %dx%d mips=%d -- albedo array disabled\n",
                   i, kMatAlbedoChannelNames[i], img.width, img.height, img.mipCount,
                   refW, refH, refMips);
            fflush(stdout);
            return;
        }
        printf("[TERRAIN_MAT_TEX] layer %d (%s): %s %dx%d BC7-sRGB mips=%d\n",
               i, kMatAlbedoChannelNames[i], path, img.width, img.height, img.mipCount);
        fflush(stdout);
    }

    const GLenum internalformat = GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM;
    const int levels = (refMips > 0) ? refMips : 1;
    GLint prevActive = 0, prevArrayBinding = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &prevArrayBinding);
    // TEX-CLASS: asset-pool -- terrain per-layer BC7 sRGB albedo 2D_ARRAY (content)
    glGenTextures(1, &s_matAlbedoArrayTex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_matAlbedoArrayTex);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, levels, internalformat,
                   refW, refH, kMatAlbedoLayerCount);
    size_t totalBytes = 0;
    for (int layer = 0; layer < kMatAlbedoLayerCount; ++layer) {
        const RenderCore::KtxImage& img = imgs[layer];
        for (int lvl = 0; lvl < levels; ++lvl) {
            const int lw = (refW >> lvl) ? (refW >> lvl) : 1;
            const int lh = (refH >> lvl) ? (refH >> lvl) : 1;
            const GLsizei imageSize =
                static_cast<GLsizei>(((lw + 3) / 4) * ((lh + 3) / 4) * 16);  // BC7: 16 B / 4x4 block
            glCompressedTexSubImage3D(GL_TEXTURE_2D_ARRAY, lvl, 0, 0, layer,
                                      lw, lh, 1, internalformat, imageSize,
                                      img.pixels.data() + img.mipByteOffsets[static_cast<size_t>(lvl)]);
            totalBytes += static_cast<size_t>(imageSize);
        }
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, levels - 1);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER,
                    (levels > 1) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);  // world-space tiling
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, static_cast<GLuint>(prevArrayBinding));
    glActiveTexture(static_cast<GLenum>(prevActive));
    printf("[TERRAIN_MAT_TEX] albedo array READY: %d layers %dx%d BC7-sRGB levels=%d vram=%.1f MiB tex=%u\n",
           kMatAlbedoLayerCount, refW, refH, levels,
           totalBytes / (1024.0 * 1024.0), s_matAlbedoArrayTex);
    // TERRAIN-MATERIAL-TEXTURES-1-FIX (fix D, MIPS GUARD): a mip-incomplete
    // (levels==1) array shimmers/aliases badly when tiled at terrain distances.
    // The cook writes 12 levels for a 2048^2 layer; levels==1 at runtime means a
    // STALE pre-mip cook was deployed (the cook output post-dates the deploy pickup
    // -- deploy_payload copies whatever _albedo.ktx2 is on disk). Loud so a bad
    // deploy is caught in the console instead of shipping as a soft visual bug.
    if (levels <= 1) {
        printf("[TERRAIN_MAT_TEX] WARNING: albedo array has levels=1 (NO MIPS). Tiled terrain "
               "albedo will shimmer. A mip-complete cook was NOT deployed -- re-cook "
               "(tools/mc2texcook/cook_terrain_layers.py) THEN re-deploy so the mip-complete "
               "data/terrain_layers/*_albedo.ktx2 reach the payload.\n");
    }
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Shader program + uniform locations (Phase 4).
// ---------------------------------------------------------------------------

GLuint   s_terrainProgram    = 0;
static int      s_submitZeroStreak  = 0;  // Phase 7.5: consecutive frames with count==0
GLint    s_locBlockOriginX   = -1;
GLint    s_locBlockOriginY   = -1;
GLint    s_locMapSide        = -1;
GLint    s_locHalfMap        = -1;
GLint    s_locMvp            = -1;
GLint    s_locLodStep        = -1;  // Phase 5: per-block LOD stride uniform
GLint    s_locSkirtDepth     = -1;  // Phase 6: skirt depth uniform
GLint    s_locForceColor     = -1;  // Phase 7.5: neon debug palette override
GLint    s_locColormap       = -1;  // Phase 10: merged colormap atlas sampler
GLint    s_locAtlasTLX       = -1;  // Phase 10: atlas top-left X (world)
GLint    s_locAtlasTLY       = -1;  // Phase 10: atlas top-left Y (world)
GLint    s_locAtlasOOW       = -1;  // Phase 10: atlas oneOverWorldUnitsMapSide
GLint    s_locLightDir       = -1;  // Phase 10 Step 1b: terrainLightDir (sun)
GLint    s_locMissionFlatLight = -1;  // CEMENT-NIGHT-LIGHT-1: u_missionFlatLight
GLint    s_locNumPoolLights  = -1;  // NIGHT-LIGHT-POOLS-1: u_numPoolLights (0 = pool loop off)
// LIGHT-CLUSTER-PERF-1: light-grid uniforms (dim 0 => linear full-pool loop)
GLint    s_locLightGridDim     = -1;
GLint    s_locLightGridMin     = -1;
GLint    s_locLightGridInvCell = -1;
GLint    s_locLightPoolSoft     = -1;  // NIGHT-LIGHT-POOLS-2: u_lightPoolSoft
GLint    s_locLightPoolMoverDim = -1;  // NIGHT-LIGHT-POOLS-2: u_lightPoolMoverDim
GLint    s_locLightPoolCompress = -1;  // NIGHT-LIGHT-POOLS-3: u_lightPoolCompress
GLint    s_locLightPoolMoverScale = -1;  // NIGHT-LIGHT-POOLS-4: u_lightPoolMoverScale
GLint    s_locLightPoolMoverMax   = -1;  // NIGHT-LIGHT-POOLS-4: u_lightPoolMoverMax
GLint    s_locMoverContactShadow  = -1;  // MOVER-CONTACT-SHADOW-1: u_moverContactShadow
GLint    s_locPadGlow             = -1;  // PAD-AREA-GLOW-1: u_padGlow (premult vec3)
GLint    s_locPadGlowSpill        = -1;  // PAD-AREA-GLOW-1: u_padGlowSpill (wu)
GLint    s_locCementLit           = -1;  // RUNWAY-APRON-TONE-2: u_cementLit (0..1)
GLint    s_locCementRampWord      = -1;  // CEMENT-WORD-RAMP-1: u_cementRampWord (0/1)
GLint    s_locDiag           = -1;  // bisection bitmask (MC2_TERRAIN_LOD_CHUNK_DIAG)
GLint    s_locLightDebugView = -1;  // LIGHTING-DEBUG-VIEWS-1A-CHUNK: u_lightingDebugView
GLint    s_locPathTint       = -1;  // MC2_SHADER_PATH_TINT debug (u_pathTint)
GLint    s_locQuadCountX     = -1;  // Phase 10.4: block quad extent X (edge detect)
GLint    s_locQuadCountY     = -1;  // Phase 10.4: block quad extent Y (edge detect)
GLint    s_locEdgeStitch     = -1;  // Phase 10.4: packed coarser-neighbour stride
GLint    s_locShadowTier     = -1;  // Slice B: per-chunk shadow tier (DIAG=40 tint only)
// Phase 10 Step 1c: shadow uniforms (declared by include/shadow.hglsl).
GLint    s_locShadowMap          = -1;
GLint    s_locLightSpaceMatrix   = -1;
GLint    s_locEnableShadows      = -1;
GLint    s_locShadowSoftness     = -1;
GLint    s_locDynShadowMap       = -1;
GLint    s_locDynLightSpaceMat   = -1;
GLint    s_locEnableDynShadows   = -1;
// Item 1 CSM array-variant locs (only valid when MC2_SHADOW_CSM is ON)
GLint    s_locDynShadowArray     = -1;
GLint    s_locDynCascadeMats     = -1;
GLint    s_locDynCsmCount        = -1;
GLint    s_locDynCascadeTexel    = -1;  // Stage 3 texel bias
GLint    s_locCsmDepthSpan       = -1;
// Per-cascade shadow resolution: separate full-map (last) cascade.
GLint    s_locDynFullMapShadow   = -1;
GLint    s_locDynFullMapTexel    = -1;
// Mirror gameos_graphics.cpp's file-static terrain shadow texture units (9/10).
static constexpr GLint kChunkTexUnitStaticShadow  = 9;
static constexpr GLint kChunkTexUnitDynamicShadow = 10;
static constexpr GLint kChunkTexUnitDynFullMap    = 13;  // free unit (chunk uses up to 11)
// Phase 10 Step 5a: merged material normal sampler2DArray (own unit, no collision
// with colormap=0 / shadows=9,10). Sourced from gos_GetTerrainNormalArrayTex().
GLint    s_locMatNormalArray     = -1;
static constexpr GLint kChunkTexUnitMatNormalArray = 5;
// Step 5a: live material tunables (same uniforms + source as legacy terrain), so
// the ImGui terrain panel drives the chunk detail too.
GLint    s_locClassGrass         = -1;
GLint    s_locClassDirt          = -1;
GLint    s_locMatTiling          = -1;
GLint    s_locMatNormalBoost     = -1;
GLint    s_locMatTilingSnow      = -1;
GLint    s_locDetailTiling       = -1;
GLint    s_locDetailStrength     = -1;
GLint    s_locTintRock           = -1;
GLint    s_locTintGrass          = -1;
GLint    s_locTintDirt           = -1;
GLint    s_locTintStrengthScale  = -1;
GLint    s_locSnowBrightnessDampen = -1;  // <1 darkens detected snow
// TERRAIN-CONTROLMAP-ALBEDO-1: lifts tintStrength toward 1.0 (0=byte-identical).
GLint    s_locControlAlbedoStrength = -1;
GLint    s_locTintConcrete   = -1;
GLint    s_locTintSnow       = -1;
GLint    s_locMatRoughness   = -1;
GLint    s_locMatAO          = -1;
GLint    s_locMatHeight      = -1;  // POM-PERLAYER-HEIGHT-1
GLint    s_locUseMaterialLib = -1;
GLint s_locCliffShadowFloor = -1;  // CLIFF SHADOW FLOOR
GLint s_locAmbientFloor     = -1;  // TERRAIN-AMBIENT-FLOOR-2
GLint s_locAmbientDayGate   = -1;  // TERRAIN-AMBIENT-FLOOR-2
GLint s_locNfhStrength    = -1;
GLint s_locUseRockSlopeBias = -1;  // TERRAIN-SLOPE-BIAS-VISUAL-1 (B4a)
GLint s_locRockSlopeBiasStr = -1;
GLint s_locUseTriplanarCliff = -1; // TERRAIN-CLIFF-MATERIAL-TRIPLANAR-1
GLint s_locCliffTriplanarStr = -1;
GLint s_locCliffHeightNormalStr = -1; // TERRAIN-CLIFF-HEIGHT-NORMAL-1
GLint s_locCliffPom          = -1; // TERRAIN-CLIFF-POM-1: u_cliffPom (.x=gate,.y=depth,.z=steps)
GLint s_locCliffDebug        = -1; // TERRAIN-CLIFF-DEBUG: u_cliffDebug (0=off,1..4 debug-viz)
GLint s_locMacroVariation    = -1; // TERRAIN-MACRO-VARIATION-1
GLint s_locEdgeFeather        = -1; // TERRAIN-EDGE-FEATHER-1
GLint s_locEdgeFeatherStr     = -1;
GLint s_locVisualDisplace     = -1; // TERRAIN-VISUAL-HEIGHT-SAMPLE-1
GLint s_locVisualSide         = -1;
GLint s_locVisualDisplaceFar  = -1; // TERRAIN-VISUAL-HEIGHT-S2-ALLLOD
GLint s_locVisualDampOn       = -1; // TERRAIN-REAUTH-UNPIN-1 Half B
GLint s_locGeomorphMips       = -1; // TERRAIN-LOD-GEOMORPH-1: b26 has max-mip levels
GLint s_locMorphFactor        = -1; // TERRAIN-LOD-GEOMORPH-1: per-block parent-band lerp
GLint s_locPomParams      = -1;
GLint s_locCameraPos      = -1; // TERRAIN-CHUNK-POM-1: "cameraPos" (Stuff/MLR eye, legacy name)
GLint s_locPomView        = -1; // TERRAIN-CHUNK-POM-1: u_pomView (.x=gate, .y=near, .z=far)
GLint s_locMatProfile     = -1;
GLint s_locControlMap     = -1;  // TERRAIN-CONTROLMAP-SAMPLE-1: u_controlMap sampler
GLint s_locUseControlMap  = -1;  // u_useControlMap gate uniform
// TERRAIN-MATERIAL-TEXTURES-1: per-layer PBR albedo array (unit 4 -- free on
// the chunk program: 0=colormap 1=overlay 2=shoreline 3=cement 5=normalArray
// 9/10=shadows 11=transitionMask 12=controlMap 13=dynFullMap).
GLint s_locMatAlbedoArray    = -1;  // u_matAlbedoArray sampler2DArray
GLint s_locUseMatAlbedo      = -1;  // u_useMatAlbedo gate uniform
GLint s_locMatAlbedoStrength = -1;  // u_matAlbedoStrength mix knob
static constexpr GLint kChunkTexUnitMatAlbedoArray = 4;
// TERRAIN-OVERLAY-V2-PARITY-1: authored cement/pad/runway overlay sidecar.
GLint s_locOverlaySidecar    = -1;  // u_overlaySidecar sampler
GLint s_locUseOverlaySidecar = -1;  // u_useOverlaySidecar gate uniform
GLint s_locOverlayBounds     = -1;  // u_overlayBounds (vec4 minX,minY,sizeX,sizeY)
// TERRAIN-SHORELINE-V3: elevation-placed wet/foam band; mask is now an
// OPTIONAL modulator (u_hasShorelineMask), not the placement source.
GLint s_locShorelineMask     = -1;  // u_shorelineMask sampler (modulator)
GLint s_locUseShorelineMask  = -1;  // u_useShorelineMask: 1 = elevation bands active (MC2_TERRAIN_SHORELINE gate)
GLint s_locHasShorelineMask  = -1;  // u_hasShorelineMask: 1 = sidecar loaded, apply modulator
GLint s_locShorelineBounds   = -1;  // u_shorelineBounds (vec4 minX,minY,sizeX,sizeY) -- modulator sample bounds
GLint s_locWaterElevation    = -1;  // u_waterElevation (Terrain::waterElevation, world units)
GLint s_locShorelineWetHeight  = -1;  // u_shorelineWetHeight (world units above water)
GLint s_locShorelineFoamHeight = -1;  // u_shorelineFoamHeight (world units above water)
GLint s_locShorelineEdgeJitter = -1;  // u_shorelineEdgeJitter (V4-STYLE: static world-XY band jitter, wu)
GLint s_locShaderTime        = -1;  // u_shaderTime (f(worldPos,time)-only foam animation clock)
GLint s_locShorelineStrength     = -1;  // u_shorelineStrength (wet/damp darken multiplier)
GLint s_locShorelineFoamStrength = -1;  // u_shorelineFoamStrength (foam rim multiplier)
GLint    s_locCementAtlas    = -1;
GLint    s_locUseCement      = -1;
GLint    s_locCementGridSide = -1;
GLint    s_locCementWUPT     = -1;
GLint    s_locCementDiagConnect = -1;  // CEMENT-DIAG-CONNECT-1 gate uniform
GLint    s_locCementAtlasSeamFix = -1; // CEMENT-ATLAS-SEAM-1 gate uniform
static constexpr GLint kChunkTexUnitCement = 3;  // matches legacy tex3
GLint s_locTransitionMaskArray = -1;
GLint s_locUseTransitionMask   = -1;
static constexpr GLint kChunkTexUnitTransitionMask = 11;

// TERRAIN-DETAIL-ANTI-TILE-1: pack LOD-tier detail-normal fade knobs into the
// unused .yzw of detailNormalStrength (the chunk frag reads only .x today).
//   .y = LOD1 detail strength   .z = enable flag (>0.5)   .w = macro strength
// Gate MC2_TERRAIN_DETAIL_ANTITILE; default OFF => {0,0,0} => byte-identical.
// Read once. Returns pointer to a static float[3] {y,z,w}.
const float* mc2_chunkDetailAntiTileYZW()
{
    static float s_yzw[3] = { 0.0f, 0.0f, 0.0f };
    static bool s_init = false;
    if (!s_init) {
        s_init = true;
        if (mc2::gate::get<"MC2_TERRAIN_DETAIL_ANTITILE">()) {
            s_yzw[0] = mc2::gate::get<"MC2_TERRAIN_DETAIL_LOD1_STRENGTH">();    // .y LOD1 fade (row default 0.4)
            s_yzw[1] = 1.0f;                                                    // .z enable
            s_yzw[2] = mc2::gate::get<"MC2_TERRAIN_DETAIL_MACRO_STRENGTH">();   // .w macro (chunk: off, row default 0)
        }
    }
    return s_yzw;
}

std::map<uint32_t, PatchShape> s_patchCache;

// MC2_TERRAIN_LOD_CHECKER_DIAG (default ON, read once, static-cached). When ON,
// the per-quad triangle diagonal honors the same per-cell checkerboard used by
// grounding (worldQuadUVMode, mapdata.cpp:116), the road/cement overlay bake
// (gos_terrain_indirect.cpp:4404) and water/shadow/GPU-compute terrain, instead
// of the fixed TL-BR diagonal. The chunk render was the sole TL-BR outlier, which
// made roads/cement vanish on slopes (user-confirmed fixed). Set =0 to revert to
// the prior fixed TL-BR behaviour (killswitch).
bool terrainLodCheckerDiagEnabled()
{
    static const bool s_on = mc2::gate::get<"MC2_TERRAIN_LOD_CHECKER_DIAG">();
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
const PatchShape& getOrBuildPatch(int qcX, int qcY, int lodStep,
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

    ps.vbo.create();
    ps.vbo.bind();
    ps.vbo.bufferData((intptr_t)(verts.size() * sizeof(LocalVertex)),
                      verts.data(), RenderCore::GpuBufferUsage::StaticDraw);

    ps.ibo.create();
    ps.ibo.bind();
    ps.ibo.bufferData((intptr_t)(indices.size() * sizeof(uint16_t)),
                      indices.data(), RenderCore::GpuBufferUsage::StaticDraw);

    ps.vbo.unbind();  // GL_ARRAY_BUFFER -> 0
    ps.ibo.unbind();  // GL_ELEMENT_ARRAY_BUFFER -> 0

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

    ps.skirtVbo.create();
    ps.skirtVbo.bind();
    ps.skirtVbo.bufferData((intptr_t)(skirtVerts.size() * sizeof(SkirtVertex)),
                           skirtVerts.data(), RenderCore::GpuBufferUsage::StaticDraw);

    ps.skirtIbo.create();
    ps.skirtIbo.bind();
    ps.skirtIbo.bufferData((intptr_t)(skirtIdx.size() * sizeof(uint16_t)),
                           skirtIdx.data(), RenderCore::GpuBufferUsage::StaticDraw);

    ps.skirtVbo.unbind();  // GL_ARRAY_BUFFER -> 0
    ps.skirtIbo.unbind();  // GL_ELEMENT_ARRAY_BUFFER -> 0

    s_patchCache[key] = std::move(ps);  // GpuBuffer members are move-only RAII
    return s_patchCache.at(key);
}

// ---------------------------------------------------------------------------
// VAO for patch draws — reused every frame, attributes re-pointed per batch.
// ---------------------------------------------------------------------------

GLuint s_patchVao = 0;

// ---------------------------------------------------------------------------
// Init / Destroy — called from gosRenderer::init / gosRenderer::destroy.
// ---------------------------------------------------------------------------

void gos_TerrainLodChunk_Init()
{
    if (s_heightSsbo.created())
        return; // idempotent

    if (!s_heightSsbo.create())
    {
        fprintf(stderr, "[TerrainLodChunk] glGenBuffers failed for height SSBO\n");
        fflush(stderr);
        return;
    }
    s_typeSsbo.create();   // Step 5b: terrainType SSBO (concrete)
    s_cementSsbo.create(); // Step 5c: cement word SSBO

    // Shader program (Phase 4) — load unconditionally; SubmitDrawCommands gates
    // on the env var so no pixels change unless MC2_TERRAIN_LOD_CHUNK=1.
    {
        // Item 1: inject MC2_SHADOW_CSM define so terrain_lod_chunk.frag (which
        // #includes shadow.hglsl) compiles the array-sampler variant when ON.
        std::string prefix = "#version 430\n";
        // STATIC-SHADOW-BANDING-2: slope-normal shadow bias (see
        // terrain_lod_chunk.frag). SHADOW-BANDING-DEFAULT-ON (2026-07-18):
        // route through the shared chokepoint helper (default ON, "=0" kills)
        // instead of a local env re-read -- the local default-OFF copy here is
        // exactly how the default-flip initially missed this LIVE path.
        if (mc2ShadowStaticBandFixV2Enabled())
            prefix += "#define MC2_SHADOW_STATIC_BANDFIX_V2 1\n";
        // TERRAIN-SHADOW-GUARD-1: this LIVE path consumes shadow.hglsl with the
        // terrain sun too — route it through the SAME shared helper (a local
        // re-read here is exactly how the BANDFIX_V2 default-flip missed this
        // file). The tess variant at ~:1382 reuses `prefix`, so it is covered.
        prefix += mc2TerrainShadowSunDefine();
        if (mc2ShadowCsmEnabled()) {
            char csmDef[64];
            snprintf(csmDef, sizeof(csmDef),
                     "#define MC2_SHADOW_CSM 1\n#define MC2_SHADOW_CSM_MAX %d\n",
                     mc2ShadowCsmCount());
            prefix += csmDef;
        }
        // SHADOW-DIST-FADE-1: distance-scaled dynamic-shadow penumbra + fade
        // (shadow.hglsl). Lockstep with the gameos_graphics.cpp inject sites.
        if (mc2ShadowDistFadeWu() > 0.0f) {
            char fadeDef[240];
            snprintf(fadeDef, sizeof(fadeDef),
                     "#define MC2_SHADOW_DIST_FADE_WU %.1f\n"
                     "#define MC2_SHADOW_DIST_FADE_MAX_F %.2f\n"
                     "#define MC2_SHADOW_FADE_CONT_F %d\n"
                     "#define MC2_SHADOW_FADE_RAMP0_F %d\n"
                     "#define MC2_SHADOW_ALT_SCALE_F %d\n",
                     mc2ShadowDistFadeWu(), mc2ShadowDistFadeMax(),
                     mc2ShadowFadeCont() ? 1 : 0,
                     mc2ShadowFadeRamp0() ? 1 : 0,
                     mc2ShadowAltScale() ? 1 : 0);
            prefix += fadeDef;
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
            s_locMissionFlatLight = glGetUniformLocation(s_terrainProgram, "u_missionFlatLight");  // CEMENT-NIGHT-LIGHT-1
            s_locNumPoolLights = glGetUniformLocation(s_terrainProgram, "u_numPoolLights");  // NIGHT-LIGHT-POOLS-1
            s_locLightGridDim     = glGetUniformLocation(s_terrainProgram, "u_lightGridDim");      // LIGHT-CLUSTER-PERF-1
            s_locLightGridMin     = glGetUniformLocation(s_terrainProgram, "u_lightGridMin");      // LIGHT-CLUSTER-PERF-1
            s_locLightGridInvCell = glGetUniformLocation(s_terrainProgram, "u_lightGridInvCell");  // LIGHT-CLUSTER-PERF-1
            s_locLightPoolSoft     = glGetUniformLocation(s_terrainProgram, "u_lightPoolSoft");      // NIGHT-LIGHT-POOLS-2
            s_locLightPoolMoverDim = glGetUniformLocation(s_terrainProgram, "u_lightPoolMoverDim");  // NIGHT-LIGHT-POOLS-2
            s_locLightPoolCompress = glGetUniformLocation(s_terrainProgram, "u_lightPoolCompress");  // NIGHT-LIGHT-POOLS-3
            s_locLightPoolMoverScale = glGetUniformLocation(s_terrainProgram, "u_lightPoolMoverScale");  // NIGHT-LIGHT-POOLS-4
            s_locLightPoolMoverMax   = glGetUniformLocation(s_terrainProgram, "u_lightPoolMoverMax");    // NIGHT-LIGHT-POOLS-4
            s_locMoverContactShadow  = glGetUniformLocation(s_terrainProgram, "u_moverContactShadow");   // MOVER-CONTACT-SHADOW-1
            s_locPadGlow      = glGetUniformLocation(s_terrainProgram, "u_padGlow");       // PAD-AREA-GLOW-1
            s_locPadGlowSpill = glGetUniformLocation(s_terrainProgram, "u_padGlowSpill");  // PAD-AREA-GLOW-1
            s_locCementLit    = glGetUniformLocation(s_terrainProgram, "u_cementLit");     // RUNWAY-APRON-TONE-2
            s_locCementRampWord  = glGetUniformLocation(s_terrainProgram, "u_cementRampWord");  // CEMENT-WORD-RAMP-1
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
            s_locMatHeight      = glGetUniformLocation(s_terrainProgram, "matHeight");  // POM-PERLAYER-HEIGHT-1
            s_locUseMaterialLib = glGetUniformLocation(s_terrainProgram, "u_useMaterialLib");
            s_locCliffShadowFloor = glGetUniformLocation(s_terrainProgram, "u_terrainCliffShadowFloor");
            s_locAmbientFloor     = glGetUniformLocation(s_terrainProgram, "u_terrainAmbientFloor");   // TERRAIN-AMBIENT-FLOOR-2
            s_locAmbientDayGate   = glGetUniformLocation(s_terrainProgram, "u_terrainAmbientDayGate"); // TERRAIN-AMBIENT-FLOOR-2
            s_locNfhStrength = glGetUniformLocation(s_terrainProgram, "terrainNormalsFromHeightStrength");
            s_locUseRockSlopeBias = glGetUniformLocation(s_terrainProgram, "useRockSlopeBias");
            s_locRockSlopeBiasStr = glGetUniformLocation(s_terrainProgram, "rockSlopeBiasStrength");
            s_locUseTriplanarCliff = glGetUniformLocation(s_terrainProgram, "useTriplanarCliff");
            s_locCliffTriplanarStr = glGetUniformLocation(s_terrainProgram, "cliffTriplanarStrength");
            s_locCliffHeightNormalStr = glGetUniformLocation(s_terrainProgram, "cliffHeightNormalStrength");  // TERRAIN-CLIFF-HEIGHT-NORMAL-1
            s_locCliffPom          = glGetUniformLocation(s_terrainProgram, "u_cliffPom");  // TERRAIN-CLIFF-POM-1
            s_locCliffDebug        = glGetUniformLocation(s_terrainProgram, "u_cliffDebug");  // TERRAIN-CLIFF-DEBUG
            s_locMacroVariation    = glGetUniformLocation(s_terrainProgram, "macroVariationStrength");
            s_locEdgeFeather       = glGetUniformLocation(s_terrainProgram, "u_edgeFeather");
            s_locEdgeFeatherStr    = glGetUniformLocation(s_terrainProgram, "u_edgeFeatherStrength");
            s_locVisualDisplace    = glGetUniformLocation(s_terrainProgram, "u_visualDisplace");
            s_locVisualSide        = glGetUniformLocation(s_terrainProgram, "u_visualSide");
            s_locVisualDisplaceFar = glGetUniformLocation(s_terrainProgram, "u_visualDisplaceFar");
            s_locVisualDampOn      = glGetUniformLocation(s_terrainProgram, "u_visualDampOn");
            s_locGeomorphMips      = glGetUniformLocation(s_terrainProgram, "u_geomorphMips");   // TERRAIN-LOD-GEOMORPH-1
            s_locMorphFactor       = glGetUniformLocation(s_terrainProgram, "u_morphFactor");    // TERRAIN-LOD-GEOMORPH-1
            s_locPomParams   = glGetUniformLocation(s_terrainProgram, "pomParams");
            s_locCameraPos   = glGetUniformLocation(s_terrainProgram, "cameraPos");  // TERRAIN-CHUNK-POM-1
            s_locPomView     = glGetUniformLocation(s_terrainProgram, "u_pomView");  // TERRAIN-CHUNK-POM-1
            s_locMatProfile  = glGetUniformLocation(s_terrainProgram, "g_terrainMaterialProfile");
            s_locCementAtlas    = glGetUniformLocation(s_terrainProgram, "u_cementAtlas");
            s_locUseCement      = glGetUniformLocation(s_terrainProgram, "u_useCement");
            s_locCementGridSide = glGetUniformLocation(s_terrainProgram, "u_cementGridSide");
            s_locCementWUPT     = glGetUniformLocation(s_terrainProgram, "u_cementWUPT");
            s_locCementDiagConnect = glGetUniformLocation(s_terrainProgram, "u_cementDiagConnect");
            s_locCementAtlasSeamFix = glGetUniformLocation(s_terrainProgram, "u_cementAtlasSeamFix");
            s_locTransitionMaskArray = glGetUniformLocation(s_terrainProgram, "u_transitionMaskArray");
            s_locUseTransitionMask   = glGetUniformLocation(s_terrainProgram, "u_useTransitionMask");
            s_locControlMap    = glGetUniformLocation(s_terrainProgram, "u_controlMap");    // TERRAIN-CONTROLMAP-SAMPLE-1
            s_locUseControlMap = glGetUniformLocation(s_terrainProgram, "u_useControlMap");
            s_locMatAlbedoArray    = glGetUniformLocation(s_terrainProgram, "u_matAlbedoArray");    // TERRAIN-MATERIAL-TEXTURES-1
            s_locUseMatAlbedo      = glGetUniformLocation(s_terrainProgram, "u_useMatAlbedo");
            s_locMatAlbedoStrength = glGetUniformLocation(s_terrainProgram, "u_matAlbedoStrength");
            s_locOverlaySidecar    = glGetUniformLocation(s_terrainProgram, "u_overlaySidecar");    // TERRAIN-OVERLAY-V2-PARITY-1
            s_locUseOverlaySidecar = glGetUniformLocation(s_terrainProgram, "u_useOverlaySidecar");
            s_locOverlayBounds     = glGetUniformLocation(s_terrainProgram, "u_overlayBounds");
            s_locShorelineMask     = glGetUniformLocation(s_terrainProgram, "u_shorelineMask");    // TERRAIN-SHORELINE-V3 (was MASK-1)
            s_locUseShorelineMask  = glGetUniformLocation(s_terrainProgram, "u_useShorelineMask");
            s_locHasShorelineMask  = glGetUniformLocation(s_terrainProgram, "u_hasShorelineMask");
            s_locShorelineBounds   = glGetUniformLocation(s_terrainProgram, "u_shorelineBounds");
            s_locWaterElevation    = glGetUniformLocation(s_terrainProgram, "u_waterElevation");
            s_locShorelineWetHeight  = glGetUniformLocation(s_terrainProgram, "u_shorelineWetHeight");
            s_locShorelineFoamHeight = glGetUniformLocation(s_terrainProgram, "u_shorelineFoamHeight");
            s_locShorelineEdgeJitter = glGetUniformLocation(s_terrainProgram, "u_shorelineEdgeJitter");  // TERRAIN-SHORELINE-V4-STYLE
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

        // SLICE 3a: build the tessellation variant program next to the base one,
        // reusing the SAME prefix PLUS "#define TERRAIN_TESS". Only when the gate
        // is ON — default OFF keeps this branch dead and the frame byte-identical.
        if (s_cliffTessGate && !s_terrainTessProgram) {
            std::string tessPrefix = prefix + "#define TERRAIN_TESS\n";
            s_terrainTessProgram = glsl_program::makeProgram2(
                "terrain_lod_chunk_tess",
                "shaders/terrain_lod_chunk.vert",
                "shaders/terrain_lod_chunk.tesc",   // HULL = TCS
                "shaders/terrain_lod_chunk.tese",   // DOMAINE = TES
                nullptr,                             // no geometry shader
                "shaders/terrain_lod_chunk.frag",
                0, nullptr, tessPrefix.c_str());
            if (!s_terrainTessProgram || !s_terrainTessProgram->is_valid()) {
                std::fprintf(stderr, "[CLIFF_TESS 3a] tess program FAILED to compile\n");
            } else {
                s_cliffTessClamped = clampTess(s_cliffTessWanted);
                s_locCliffTessLevel =
                    glGetUniformLocation(s_terrainTessProgram->shp_, "u_cliffTessLevel");
                std::fprintf(stderr,
                    "[CLIFF_TESS 3a] tess program compiled (prog=%u maxTess=%.1f locTess=%d)\n",
                    (unsigned)s_terrainTessProgram->shp_, s_cliffTessClamped,
                    (int)s_locCliffTessLevel);
            }
        }
    }

    // VAO — one global; attributes are re-pointed each draw in SubmitDrawCommands.
    glGenVertexArrays(1, &s_patchVao);
}

void gos_TerrainLodChunk_Destroy()
{
    // Free patch cache VBOs/IBOs (main + Phase 6 skirt). RAII: clearing the
    // cache runs each GpuBuffer dtor -> glDeleteBuffers (was a hand loop).
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

    // CLIFF-TESS-PERF: also drop the tess variant program + its cached per-patch
    // location cache so a later re-Init rebuilds and re-resolves cleanly.
    if (s_terrainTessProgram != 0) {
        glsl_program::deleteProgram("terrain_lod_chunk_tess");
        s_terrainTessProgram = nullptr;
        s_locCliffTessLevel  = -1;
        s_tessLocBlockOriginX = s_tessLocBlockOriginY = s_tessLocLodStep = -2;
        s_tessLocQuadCountX = s_tessLocQuadCountY = s_tessLocEdgeStitch = -2;
        s_tessLocShadowTier = s_tessLocMorphFactor = -2;
        s_tessLocVisualDisplace = s_tessLocSkirtDepth = -2;
    }

    // TERRAIN-CONTROLMAP-SAMPLE-1: free the authored control-map texture (if loaded).
    if (s_controlMapTex != 0)
    {
        glDeleteTextures(1, &s_controlMapTex);
        s_controlMapTex  = 0;
        s_controlMapSide = 0;
    }

    // TERRAIN-MATERIAL-TEXTURES-1: free the per-layer albedo array (if loaded).
    // Reset the load-tried latch so a renderer re-create reloads it.
    if (s_matAlbedoArrayTex != 0)
    {
        glDeleteTextures(1, &s_matAlbedoArrayTex);
        s_matAlbedoArrayTex = 0;
    }
    s_matAlbedoLoadTried = false;

    // TERRAIN-SHORELINE-MASK-1: free the authored shoreline mask texture (if loaded).
    if (s_shorelineMaskTex != 0)
    {
        glDeleteTextures(1, &s_shorelineMaskTex);
        s_shorelineMaskTex = 0;
    }

    // GPU-BUFFER-TIER0-LODCHUNK-RAII-1: GpuBuffer::destroy() = glDeleteBuffers
    // + registry-invalidate (the hand-copied "mark slot unavailable" blocks
    // moved into the class). Safe no-ops when never created.
    if (s_visualHeightSsbo.created())
        s_visualMipFloats = 0;   // TERRAIN-LOD-GEOMORPH-1
    s_visualHeightSsbo.destroy();
    // TERRAIN-REAUTH-UNPIN-1 Half B: free the damp SSBO + CPU copies on teardown.
    s_visualDampSsbo.destroy();
    s_visualDampSide = 0;
    s_visualDampStatic.clear();
    s_visualDampCombined.clear();
    s_visualDampHadMovers = false;
    if (s_heightSsbo.created())
    {
        s_mapSide    = 0;
        s_halfMap    = 0.0f;
    }
    s_heightSsbo.destroy();
    s_typeSsbo.destroy();
    s_cementSsbo.destroy();
}

// ---------------------------------------------------------------------------
// Submit draw commands — Phase 4 real implementation.
// Called only from Terrain::flushDrawCommands() in mclib/terrain.cpp.
// count==0 is a strict no-op. Restores GL state on exit.
// ---------------------------------------------------------------------------

// CLIFF-TESS-PERF: targeted per-patch mirror — copies ONLY the uniforms that
// change per near-field patch from the (already-configured) base program to the
// tess program. No glGetActiveUniform enumeration. The frame-constant uniforms
// are handled by the once/frame full mirrorTerrainUniforms() call. Correctness:
// this MUST cover every uniform the base per-patch bind block writes inside the
// SubmitDrawCommands loop, or the tess draw renders with stale per-block data.
// Verified base per-patch set (see the loop): u_visualDisplace, u_blockOriginX,
// u_blockOriginY, u_lodStep, u_quadCountX, u_quadCountY, u_edgeStitch,
// u_shadowTier, u_morphFactor, u_skirtDepth.
void mirrorTerrainPatchUniforms(GLuint src, GLuint dst) {
    // Integer per-patch uniforms: {base loc, tess loc} pairs.
    const GLint iset[][2] = {
        { s_locVisualDisplace, s_tessLocVisualDisplace },
        { s_locBlockOriginX,   s_tessLocBlockOriginX   },
        { s_locBlockOriginY,   s_tessLocBlockOriginY   },
        { s_locLodStep,        s_tessLocLodStep        },
        { s_locQuadCountX,     s_tessLocQuadCountX     },
        { s_locQuadCountY,     s_tessLocQuadCountY     },
        { s_locEdgeStitch,     s_tessLocEdgeStitch     },
        { s_locShadowTier,     s_tessLocShadowTier     },
    };
    for (size_t k = 0; k < sizeof(iset) / sizeof(iset[0]); ++k) {
        const GLint sloc = iset[k][0], dloc = iset[k][1];
        if (sloc < 0 || dloc < 0) continue;
        GLint v = 0; glGetUniformiv(src, sloc, &v);
        glProgramUniform1i(dst, dloc, v);
    }
    // Float per-patch uniforms (u_morphFactor, u_skirtDepth).
    const GLint fset[][2] = {
        { s_locMorphFactor, s_tessLocMorphFactor },
        { s_locSkirtDepth,  s_tessLocSkirtDepth  },
    };
    for (size_t k = 0; k < sizeof(fset) / sizeof(fset[0]); ++k) {
        const GLint sloc = fset[k][0], dloc = fset[k][1];
        if (sloc < 0 || dloc < 0) continue;
        GLfloat v = 0.0f; glGetUniformfv(src, sloc, &v);
        glProgramUniform1f(dst, dloc, v);
    }
}
#ifdef MC2_VULKAN_ISLAND
// ---------------------------------------------------------------------------
// VK-P10 TERRAIN TIER-1 feed. When MC2_VK_TERRAIN_TIER1 (+ shared depth) is armed,
// SubmitDrawCommands captures the LOD0 (lodStep==1) patch list + shared params into
// this scratch, SKIPS the GL LOD0 main+skirt draws, and the VK terrain island
// co-writes those patches into the shared scene GBuffer instead. Default-OFF ->
// none of this runs -> the GL path draws every band exactly as before.
// ---------------------------------------------------------------------------
uint64_t g_terrainHeightVersion = 0;  // bumps on UploadHeightFull
uint64_t g_terrainCementVersion = 0;  // bumps on UploadCementWordsFull
uint64_t g_terrainVisualHeightVersion = 0;  // bumps on UploadVisualHeightFull (rung 5)
#endif
#ifdef MC2_VULKAN_ISLAND
// VK-TERRAIN-VISUALDISPLACE-FAR-1: file-scope reads of the SAME env knobs the draw
// path caches locally (MC2_TERRAIN_VISUAL_DISPLACE / _FAR at gos_terrain_lod_chunk.cpp
// draw site) so the feed getter can report them without touching the draw statics.
bool terrainVisualDisplaceGateOn()
{
    static const bool s_on = mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE">();
    return s_on;
}
float terrainVisualDisplaceFarParam()
{
    static const float s_far = mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE_FAR">();
    return s_far;
}
#endif
#ifdef MC2_VULKAN_ISLAND
std::vector<Mc2TerrainTier1Patch> s_vkTerrainPatches;  // this frame's LOD0 patches
float    s_vkTerrainMvp[16] = {0};
bool     s_vkTerrainFeedValid = false;
#endif
#ifdef MC2_VULKAN_ISLAND
extern "C" int mc2_terrain_tier1_get_frame_feed(Mc2TerrainTier1Feed* out)
{
    if (!out) return 0;
    if (!s_vkTerrainFeedValid || s_vkTerrainPatches.empty()) return 0;
    if (!s_heightSsbo.created() || s_mapSide <= 0) return 0;
    out->patches    = s_vkTerrainPatches.data();
    out->patchCount = (uint32_t)s_vkTerrainPatches.size();
    std::memcpy(out->worldToClipGL, s_vkTerrainMvp, sizeof(out->worldToClipGL));
    out->mapSide       = s_mapSide;
    out->halfMap       = s_halfMap;
    out->glHeightSsbo  = s_heightSsbo.name();
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
    const bool cementLive  = cementReady && s_cementSsbo.created();
    out->glCementAtlasTex   = static_cast<uint32_t>(gos_terrain_indirect_getCementAtlasGLTex());
    out->cementGridSide     = gos_terrain_indirect_getCementAtlasGridSide();
    out->cementWUPT         = 128.0f;  // Terrain::worldUnitsPerVertex (GL uploads 128.0)
    out->useCement          = cementLive ? 1 : 0;
    out->useTransitionMask  = gos_terrain_indirect_isTransitionMaskReady() ? 1 : 0;
    {
        out->cementDiagConnect = mc2::gate::get<"MC2_TERRAIN_CEMENT_DIAG_CONNECT">() ? 1 : 0;
    }
    out->glCementSsbo       = s_cementSsbo.name();
    out->cementBytes        = (uint64_t)s_mapSide * (uint64_t)s_mapSide * sizeof(unsigned int);
    out->cementVersion      = g_terrainCementVersion;

    // VK-TERRAIN-VISUALDISPLACE-FAR-1 (rung 5): hand the 4x visual-height bake SSBO +
    // mode-2 params to the island. Armed ONLY when the sub-gate is on AND the GL displace
    // gate is on AND the bake is loaded -- else all-zero (island honors visualMode=0 only,
    // and the capture predicate below never emits mode-2 patches in that case anyway).
    out->glVisualHeightSsbo  = 0;
    out->visualHeightBytes   = 0;
    out->visualHeightVersion = 0;
    out->visualSide          = 0;
    out->visualDisplaceFar   = 0.0f;
    if (mc2_vk_terrain_visualdisplace_far_enabled() && terrainVisualDisplaceGateOn()
        && s_visualHeightSsbo.created() && s_visualSide > 0) {
        out->glVisualHeightSsbo  = s_visualHeightSsbo.name();
        out->visualHeightBytes   = (uint64_t)s_visualSide * (uint64_t)s_visualSide * sizeof(float);
        out->visualHeightVersion = g_terrainVisualHeightVersion;
        out->visualSide          = s_visualSide;
        out->visualDisplaceFar   = terrainVisualDisplaceFarParam();
    }

    // VK-TERRAIN-LIGHTING sub-slice: hand the sun dir + the STATIC sun shadow map + the
    // static light-space matrix to the island so the VK terrain frag can reproduce the GL
    // NdotL relief band + baked static PCF shadow (terrain_lod_chunk.frag:889-928). Gate
    // MC2_VK_TERRAIN_LIGHTING (default-off -> lightingEnabled=0 -> VK stays flat, byte-safe).
    {
        // Umbrella family member: MC2_VK_SHARED_GBUFFER=1 arms it; explicit
        // MC2_VK_TERRAIN_LIGHTING=0/1 overrides.
        int lightingOn = mc2vkgate::armed<"MC2_VK_TERRAIN_LIGHTING">() ? 1 : 0;
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
        // Umbrella member (mc2vkgate::armed<>); requires lighting (its own precondition).
        int dynShadowGate = 0;
        if (out->lightingEnabled)
            dynShadowGate = mc2vkgate::armed<"MC2_VK_TERRAIN_DYNSHADOW">() ? 1 : 0;
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
        // proven single-map mechanism, zero new interop surface.
        //
        // VK-FLIP-FIELD-BUG1-1 (field oracle 2026-07-06, mc2_01 overview_center): the
        // full-map subset is NET-NEGATIVE at normal play distance. GL LOD0 terrain samples
        // the NEAR cascade up close; the full-map cascade projects tall building/mech
        // occluders as huge soft blobs that swim with the camera = the user's
        // "camera-dependent garbage shadow wash". CSM-OFF was PIXEL-CLEAN vs GL on the same
        // bookmark; CSM-ON washed the whole airport. The matrix/texture ARE consistent
        // (island renders full-map, UBO carries the full-map cascade matrix) — the wash is
        // inherent to the coarse subset, NOT a binding mismatch. The real cure is near-
        // cascade-array interop (sampler2DArrayShadow + N matrices), still DEFERRED.
        //
        // Therefore this subset is now DOUBLE-GATED: MC2_VK_TERRAIN_CSM=1 (presence) is no
        // longer sufficient — it additionally requires MC2_VK_TERRAIN_CSM_FULLMAP_EXPERIMENTAL=1.
        // The launcher/field bat sets MC2_VK_TERRAIN_CSM=1 as part of the umbrella family;
        // that alone must NOT re-introduce the wash. Off by default -> VK terrain draws with
        // NO dynamic object shadow under CSM (matches the clean CSM-OFF capture), which is
        // strictly better than the wash until the near-cascade port lands.
        auto csmSubsetOn = []() {
            const bool csmSub = mc2vkgate::armed<"MC2_VK_TERRAIN_CSM">();
            return csmSub && mc2::gate::get<"MC2_VK_TERRAIN_CSM_FULLMAP_EXPERIMENTAL">();
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

        // VK-TERRAIN-CSM-NEAR-1: the REAL cure. Feed the near-cascade array + separate
        // full-map cascade so the VK frag can port GL's calcDynamicShadow cascade-select
        // verbatim (near cascade up close, coarse full-map only past its range) instead
        // of the FULLMAP_EXPERIMENTAL subset's "always coarse" wash. Independent of the
        // two paths above: MC2_VK_TERRAIN_CSM alone (no FULLMAP_EXPERIMENTAL) now means
        // THIS implementation. GL must actually have CSM active+built this frame.
        //
        // FAR-BAND-BLACK-1 FIX: this block was gated on dynShadowGate alone, NOT on
        // mc2vkgate::armed<"MC2_VK_TERRAIN_CSM">() as the comment above claims -- so
        // setting MC2_VK_TERRAIN_DYNSHADOW=1 with GL CSM active (MC2_SHADOW_CSM=1,
        // default ON) fed CSM cascade data to the VK terrain frag even with
        // MC2_VK_TERRAIN_CSM=0 (or unset with the umbrella off). The field launcher bat
        // sets exactly this combination (DYNSHADOW+CSM both umbrella members, CSM only
        // gets explicitly forced off in isolation tests) -- the resulting csmEnabled=1
        // fragments made the VK terrain frag's CSM cascade-select path run in a
        // configuration nobody exercises intentionally, collapsing shading to
        // near-black over most of the screen (root cause of FAR-BAND-BLACK; isolated
        // via bisecting MC2_VK_TERRAIN_LIGHTING/DETAIL/DYNSHADOW/CSM one at a time --
        // DYNSHADOW alone reproduces, and disabling GL CSM instead of the VK gate also
        // clears it, confirming the CSM feed itself is the trigger, not depth/format).
        // Now requires the CSM sub-gate explicitly, matching the documented contract.
        const bool csmNearGate = mc2vkgate::armed<"MC2_VK_TERRAIN_CSM">();
        out->csmEnabled = 0;
        out->glCsmArrayTex = 0;
        out->csmArraySize = 0;
        out->csmCount = 0;
        std::memset(out->csmCascadeMatrices, 0, sizeof(out->csmCascadeMatrices));
        std::memset(out->csmCascadeTexelWorld, 0, sizeof(out->csmCascadeTexelWorld));
        out->csmDepthSpan = 1.0f;
        out->glCsmFullMapTex = 0;
        out->csmFullMapSize = 0;
        out->csmFullMapTexelWorld = 0.0f;
        if (csmNearGate && dynShadowGate && pp && pp->shadowsEnabled_ && mc2ShadowCsmEnabled()) {
            const GLuint arrTex  = pp->getDynamicShadowArrayTexture();
            const GLuint fmTex   = pp->getDynamicFullMapTexture();
            const int    ccount2 = pp->getDynamicShadowCascadeCount();
            const float* mats2   = pp->getDynamicCascadeMatrices();
            const float* texelW  = pp->getDynamicCascadeTexelWorld();
            if (arrTex != 0 && fmTex != 0 && ccount2 > 0 && ccount2 <= 3 && mats2 && texelW) {
                out->csmEnabled = 1;
                out->glCsmArrayTex = static_cast<uint32_t>(arrTex);
                out->csmArraySize  = pp->getDynamicShadowMapSize();
                out->csmCount      = ccount2;
                std::memcpy(out->csmCascadeMatrices, mats2, sizeof(float) * 16 * ccount2);
                std::memcpy(out->csmCascadeTexelWorld, texelW, sizeof(float) * ccount2);
                out->csmDepthSpan  = pp->getCsmDepthSpan();
                out->glCsmFullMapTex = static_cast<uint32_t>(fmTex);
                out->csmFullMapSize  = pp->getDynamicFullMapSize();
                out->csmFullMapTexelWorld = pp->getDynamicFullMapTexelWorld();
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
            detailOn = mc2vkgate::armed<"MC2_VK_TERRAIN_DETAIL">() ? 1 : 0;
        out->detailEnabled       = detailOn;
        out->glMatNormalArrayTex = detailOn ? (uint32_t)gos_GetTerrainNormalArrayTex() : 0u;
        // GL uniform values (gameos_graphics.cpp member defaults; all 1.0). The VK frag
        // bakes per-material tiling/boost/class thresholds as const, so only these three
        // scalars flow through the UBO. See terrain_tier1.frag MATV_* consts.
        out->nfhStrength    = 1.0f;   // terrainNormalsFromHeightStrength (terrain_nfh_strength_)
        out->detailTiling   = 1.0f;   // detailNormalTiling.x (terrain_detail_tiling_)
        out->detailStrength = 1.0f;   // detailNormalStrength.x (terrain_detail_strength_)
    }

    // VK-PARITY-TERRAIN-1: ambient floor + mission flat light from the SAME producers
    // the GL uniform upload uses (gos_TerrainAmbientFloor/DayGate, gos_GetTerrainMissionFlatLight).
    out->ambientFloor   = gos_TerrainAmbientFloor();
    out->ambientDayGate = gos_TerrainAmbientDayGate();
    // VK-PARITY-F3: legacy material tint producers (same values as the GL upload below).
    {
        float tr[3]={0.36f,0.37f,0.40f}; gos_GetTerrainTintRock(&tr[0],&tr[1],&tr[2]);
        float tg[3]={0.35f,0.42f,0.25f}; gos_GetTerrainTintGrass(&tg[0],&tg[1],&tg[2]);
        float td[3]={0.48f,0.42f,0.33f}; gos_GetTerrainTintDirt(&td[0],&td[1],&td[2]);
        float tc[3]={0.55f,0.53f,0.50f}; gos_GetTerrainTintConcrete(&tc[0],&tc[1],&tc[2]);
        float tsn[3]={0.75f,0.78f,0.84f}; gos_GetTerrainTintSnow(&tsn[0],&tsn[1],&tsn[2]);
        for (int k = 0; k < 3; ++k) {
            out->tintRock[k] = tr[k]; out->tintGrass[k] = tg[k]; out->tintDirt[k] = td[k];
            out->tintConcrete[k] = tc[k]; out->tintSnow[k] = tsn[k];
        }
        out->tintStrengthScale = gos_GetTerrainTintStrengthScale();
        out->snowBrightnessDampen = gos_TerrainSnowBrightnessDampen();
    }
    {
        float fr = 1.f, fg = 1.f, fb = 1.f;
        gos_GetTerrainMissionFlatLight(&fr, &fg, &fb);
        out->missionFlatLight[0] = fr; out->missionFlatLight[1] = fg; out->missionFlatLight[2] = fb;
    }

    // VK-PARITY-TERRAIN-2: night light pools + light grid + pad-area glow, from the
    // SAME producers the GL upload uses. The pool/grid ARRAYS are the CPU copies of
    // what PackAndDispatch uploaded into GL bindings 28/29/30 this frame (cone-spill /
    // LIGHT-REALISM reshape, vis gate and mover flag already applied there).
    {
        out->numPoolLights = gos_TerrainLightPoolCount();
        out->poolLights    = nullptr;
        out->lightGridDim  = 0;
        out->lightGridMin[0] = out->lightGridMin[1] = 0.0f;
        out->lightGridInvCell[0] = out->lightGridInvCell[1] = 0.0f;
        out->lightGridHeader = nullptr; out->lightGridHeaderWords = 0;
        out->lightGridIndices = nullptr; out->lightGridIndexWords = 0;
        if (out->numPoolLights > 0) {
            uint32_t n = 0;
            const gos_terrain_lighting::GpuTerrainLight* pl = gos_terrain_lighting::GetLightPoolCpu(&n);
            if (!pl || n == 0) {
                out->numPoolLights = 0;
            } else {
                out->poolLights = pl;   // numPoolLights (<= n, capped 256) entries are read
                gos_terrain_lighting::LightGridCpu lg{};
                if (gos_terrain_lighting::GetLightGridCpu(lg)) {
                    out->lightGridDim        = static_cast<int32_t>(lg.dim);
                    out->lightGridMin[0]     = lg.minXY[0];
                    out->lightGridMin[1]     = lg.minXY[1];
                    out->lightGridInvCell[0] = lg.invCell[0];
                    out->lightGridInvCell[1] = lg.invCell[1];
                    out->lightGridHeader      = lg.header;
                    out->lightGridHeaderWords = lg.headerWords;
                    out->lightGridIndices     = lg.indices;
                    out->lightGridIndexWords  = lg.indexWords;
                }
            }
        }
        gos_TerrainLightPoolKnobs(&out->lightPoolSoft, &out->lightPoolMoverDim,
                                  &out->lightPoolMoverScale, &out->lightPoolMoverMax,
                                  &out->lightPoolCompress, &out->moverContactShadow);
        gos_TerrainPadGlowRGB(out->padGlow);
        out->padGlowSpill = gos_TerrainPadGlowSpillWu();
        // VK-PARITY-SHADOW-1: the SAME knobs GL bakes into the terrain program as defines.
        {
            const Mc2ShadowFadeKnobs sk = mc2ShadowFadeKnobs();
            out->shadowDistFadeWu  = sk.distFadeWu;
            out->shadowDistFadeMax = sk.distFadeMax;
            out->shadowMinSunSin   = sk.minSunSin;
            out->shadowKnobFlags   = sk.flags;
        }
    }

    // VK-TERRAIN-LODFADE sub-slice: camera-distance fade for detail-normal/POM, closing
    // the VK LOD0-vs-GL-band color seam (GL gos_terrain.frag lodNear, :322-328,616-736).
    // Gate MC2_VK_TERRAIN_LODFADE requires detailEnabled (fading a feature that's already
    // off is a no-op, so keep the precondition chain: LIGHTING -> DETAIL -> LODFADE).
    {
        int lodFadeOn = 0;
        if (out->detailEnabled)
            // VK-PARITY-F3: DEFAULT OFF even under the umbrella (explicit =1 still arms it).
            // The GL chunk terrain (the shipped renderer, terrain_lod_chunk.frag) has NO
            // camera-distance detail fade -- only the legacy gos_terrain.frag did -- so the
            // umbrella-armed fade removed ALL detail-normal relief beyond ~4000wu (every
            // high/RTS camera), leaving VK terrain flat-lit: measured N.x mean +0.07 vs GL
            // +0.53 (parF3 mc2_10 night), ~15% darker bare ground. All LOD bands are VK-drawn
            // by the same shader, so there is no LOD0-vs-band seam to close.
            lodFadeOn = mc2::gate::get<"MC2_VK_TERRAIN_LODFADE">() ? 1 : 0;
        out->lodFadeEnabled = lodFadeOn;
        // VK-FLIP oracle-visible gate-state proof: one-shot diagnostic line so a smoke
        // capture can confirm lodFadeEnabled's resolved value without a debugger attach.
        // Gated on presence of MC2_VK_TERRAIN_COPY_STATS (same opt-in convention as the
        // interop-copies stats line above) to stay silent by default.
        {
            static const bool s_diagOn = mc2::gate::get<"MC2_VK_TERRAIN_COPY_STATS">();
            static bool s_logged = false;
            if (s_diagOn && !s_logged) {
                s_logged = true;
                fprintf(stderr, "[VK_TERRAIN_LODFADE_DIAG] lodFadeEnabled=%d detailEnabled=%d "
                        "lightingEnabled=%d\n", lodFadeOn, out->detailEnabled, out->lightingEnabled);
                fflush(stderr);
            }
        }
        extern void gos_GetTerrainCameraPos(float*, float*, float*);
        float cx = 0.0f, cy = 0.0f, cz = 0.0f;
        gos_GetTerrainCameraPos(&cx, &cy, &cz);
        out->cameraPosMC2[0] = cx;
        out->cameraPosMC2[1] = cy;
        out->cameraPosMC2[2] = cz;
    }

    // VK-OFFLINE-PROOFS-1: golden feed dump. Fires once per process (paired
    // with the deterministic bookmark/sim-freeze capture so run-to-run this is
    // the SAME frame). No-op unless MC2_VK_FEED_DUMP is set.
    if (mc2vkfeeddump::enabled() && mc2vkfeeddump::readyToDump() && !mc2vkfeeddump::latch("terrain")) {
        mc2vkfeeddump::resetOnce();
        if (FILE* f = mc2vkfeeddump::openAppend(/*truncateFirst=*/false)) {
            unsigned displacedPatches = 0;
            for (uint32_t i = 0; i < out->patchCount; ++i)
                if (out->patches[i].visualMode == 2) ++displacedPatches;
            std::fprintf(f, "{\"island\":\"terrain\",\"patchCount\":%u,\"mapSide\":%d,"
                "\"halfMap\":%.6f,\"heightVersion\":%llu,\"cementVersion\":%llu,"
                "\"useCement\":%d,\"lightingEnabled\":%d,\"shadowsEnabled\":%d,"
                "\"dynShadowsEnabled\":%d,\"detailEnabled\":%d,\"lodFadeEnabled\":%d,"
                "\"visualSide\":%d,\"visualDisplaceFar\":%.6f,\"displacedPatches\":%u,"
                "\"patches\":[",
                out->patchCount, out->mapSide, out->halfMap,
                (unsigned long long)out->heightVersion, (unsigned long long)out->cementVersion,
                out->useCement, out->lightingEnabled, out->shadowsEnabled,
                out->dynShadowsEnabled, out->detailEnabled, out->lodFadeEnabled,
                out->visualSide, out->visualDisplaceFar, displacedPatches);
            for (uint32_t i = 0; i < out->patchCount; ++i) {
                const Mc2TerrainTier1Patch& p = out->patches[i];
                std::fprintf(f, "%s{\"qcX\":%d,\"qcY\":%d,\"blockOriginX\":%d,"
                    "\"blockOriginY\":%d,\"lodStep\":%d,\"skirtDepth\":%.6f,"
                    "\"skirtEdgeMask\":%u}",
                    i ? "," : "", p.quadCountX, p.quadCountY, p.blockOriginX,
                    p.blockOriginY, p.lodStep, p.skirtDepth, p.skirtEdgeMask);
            }
            std::fprintf(f, "]}\n");
            std::fclose(f);
        }
        mc2vkfeeddump::latch("terrain") = true;
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
    const float*              morphFactors,
    int                       count)
{
    if (count == 0) return;
    if (s_terrainProgram == 0 || !s_heightSsbo.created()) return;
    if (s_patchVao == 0) return;

    // CLIFF-TESS-PERF: reset the once/frame full-mirror guard. This entrypoint is
    // called once per terrain draw (once/frame), so clearing it here means the
    // first near-field tess patch does the full enumerate-and-copy mirror and
    // every subsequent patch this frame takes the cheap targeted per-patch path.
    // Inside the gate-ON path's effect only (the flag is unread when the gate is
    // off), so gate-OFF remains byte-identical.
    s_tessMirroredThisFrame = false;

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
    static const bool s_depthAlways = mc2::gate::get<"MC2_TERRAIN_LOD_DEPTH_ALWAYS">();
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
    s_heightSsbo.bindBase(TERRAIN_HEIGHT_SSBO_BINDING);
    // Step 5b: terrainType SSBO (concrete). 0 if never uploaded -> vert reads 0.
    if (s_typeSsbo.created())
        s_typeSsbo.bindBase(TERRAIN_TYPE_SSBO_BINDING);
    // Step 5c: cement word SSBO.
    if (s_cementSsbo.created())
        s_cementSsbo.bindBase(TERRAIN_CEMENT_SSBO_BINDING);

    // TERRAIN-VISUAL-HEIGHT-SAMPLE-1: resolve geometry-displacement activation once
    // per frame. Active = MC2_TERRAIN_VISUAL_DISPLACE on AND the 4x bake loaded.
    // Bind binding 26 for the whole draw; the per-chunk u_visualDisplace gates the
    // near (LOD0) band only. Default OFF -> u_visualDisplace stays 0 -> byte-identical.
#ifdef MC2_VULKAN_ISLAND
    const bool s_visualDisplaceGate = terrainVisualDisplaceGateOn();  // rung 5: shared helper (same env read)
#else
    static const bool s_visualDisplaceGate = mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE">();
#endif
    const bool visualDisplaceActive = s_visualDisplaceGate && s_visualHeightSsbo.created() && s_visualSide > 0;
    if (visualDisplaceActive)
        s_visualHeightSsbo.bindBase(TERRAIN_VISUAL_HEIGHT_SSBO_BINDING);
    if (s_locVisualSide >= 0)
        glUniform1i(s_locVisualSide, s_visualSide);
    // TERRAIN-REAUTH-UNPIN-1 Half B: near-object displacement fade. Active only
    // when displacing AND the objfade gate is on (default ON when displacing —
    // it is the safety) AND a damp map for THIS map size was uploaded. Binds
    // binding 27 for the whole draw; u_visualDampOn=0 => shader never reads it.
    static const bool s_visualDampGate = mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE">();
    const bool visualDampActive = visualDisplaceActive && s_visualDampGate
                                  && s_visualDampSsbo.created()
                                  && s_visualDampSide == s_mapSide;
    if (visualDampActive)
        s_visualDampSsbo.bindBase(TERRAIN_VISUAL_DAMP_SSBO_BINDING);
    if (s_locVisualDampOn >= 0)
        glUniform1i(s_locVisualDampOn, visualDampActive ? 1 : 0);
    // TERRAIN-VISUAL-HEIGHT-S2-ALLLOD: far-band displacement fade, 0..1, default 1
    // (full displacement). Only scales the coarser-band (u_visualDisplace==2) verts;
    // LOD0 (mode 1) is untouched by this knob.
#ifdef MC2_VULKAN_ISLAND
    const float s_visualDisplaceFar = terrainVisualDisplaceFarParam();  // rung 5: shared helper (same env read)
#else
    static const float s_visualDisplaceFar = mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE_FAR">();
#endif
    if (s_locVisualDisplaceFar >= 0)
        glUniform1f(s_locVisualDisplaceFar, s_visualDisplaceFar);

    // TERRAIN-LOD-GEOMORPH-1: coarse-band vertices read their OWN max-mip level
    // (silhouette keeps peaks) when the bake shipped mips. Rides the same
    // MC2_TERRAIN_VISUAL_DISPLACE gate (visualDisplaceActive); mips absent ->
    // uploads 0 -> vert branch skipped (S2 behavior verbatim).
    // MC2_TERRAIN_LOD_GEOMORPH=0 is the killswitch when mips ARE present.
    static const bool s_geomorphKill = !mc2::gate::get<"MC2_TERRAIN_LOD_GEOMORPH">();
    const bool geomorphActive =
        visualDisplaceActive && s_visualMipFloats > 0 && !s_geomorphKill;
    if (s_locGeomorphMips >= 0)
        glUniform1i(s_locGeomorphMips, geomorphActive ? 1 : 0);
    // One-shot draw-time truth line so a dead geomorph is diagnosable from the
    // console instead of a pixel A/B (mirrors [VISUAL_HEIGHT v1] cadence).
    {
        static bool s_geoLogged = false;
        if (!s_geoLogged && visualDisplaceActive) {
            s_geoLogged = true;
            printf("[GEOMORPH v1] active=%d mipFloats=%d kill=%d locMips=%d locMorph=%d locLodStep=%d\n",
                   geomorphActive ? 1 : 0, s_visualMipFloats, s_geomorphKill ? 1 : 0,
                   (int)s_locGeomorphMips, (int)s_locMorphFactor, (int)s_locLodStep);
            fflush(stdout);
        }
    }

    // Upload per-frame uniforms (same for every patch).
    if (s_locMapSide >= 0)
        glUniform1i(s_locMapSide, s_mapSide);
    if (s_locHalfMap >= 0)
        glUniform1f(s_locHalfMap, s_halfMap);
    if (s_locMvp >= 0)
        glUniformMatrix4fv(s_locMvp, 1, GL_FALSE, mvp);

    // Phase 7.5: neon force-color mode — set once per frame before the patch loop.
    // REDUNDANT-PASS-HUNT-1: env resolved once (kTglcForceColor), not per frame.
    if (s_locForceColor >= 0)
        glUniform1i(s_locForceColor, kTglcForceColor ? 1 : 0);

    // Bisection bitmask uniform (MC2_TERRAIN_LOD_CHUNK_DIAG): 1=no GBuffer1,
    // 2=no depth fudge, 4=no lighting. Shader-side A/B without rebuilding.
    // REDUNDANT-PASS-HUNT-1: env resolved once (kTglcDiag), not per frame.
    if (s_locDiag >= 0)
        glUniform1i(s_locDiag, kTglcDiag);

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
        // CEMENT-NIGHT-LIGHT-1: mission flat-pad light for the RAW cement-atlas
        // texels (night darkness is burned into the colormap CPU-side; the
        // cement atlas is built from unburned tile art). MUST be uploaded by
        // every binder of terrain_lod_chunk.frag or draws read 0 -> black pads.
        // (The tess variant program auto-mirrors this via mirrorTerrainUniforms.)
        if (s_locMissionFlatLight >= 0) {
            float fr = 1.f, fg = 1.f, fb = 1.f;
            gos_GetTerrainMissionFlatLight(&fr, &fg, &fb);
            glUniform3f(s_locMissionFlatLight, fr, fg, fb);
        }
        // PAD-AREA-GLOW-1: premultiplied night pad-area lift. nightFactor is
        // derived from the SAME mission flat light CEMENT-NIGHT-LIGHT-1 uses
        // (1 - brightest channel): day missions upload ~(1,1,1) -> factor 0 ->
        // u_padGlow = exact (0,0,0) -> byte-identical. Warm color matches the
        // building SpotLight_ register tint direction (0xffe8c870-ish).
        if (s_locPadGlow >= 0) {
            // VK-PARITY-TERRAIN-2: premultiply moved verbatim into the shared
            // producer gos_TerrainPadGlowRGB() (VK terrain island reads it too).
            float pg[3];
            gos_TerrainPadGlowRGB(pg);
            glUniform3f(s_locPadGlow, pg[0], pg[1], pg[2]);
            if (s_locPadGlowSpill >= 0)
                glUniform1f(s_locPadGlowSpill, gos_TerrainPadGlowSpillWu());
        }
        // RUNWAY-APRON-TONE-2: cement lighting participation. 0.0 == stock
        // flat-lit slab, so the default upload is a no-op by value.
        if (s_locCementLit >= 0)
            glUniform1f(s_locCementLit, gos_TerrainCementLit());
        if (s_locCementRampWord >= 0)
            glUniform1i(s_locCementRampWord, gos_TerrainCementRampWord());   // CEMENT-WORD-RAMP-1
        // NIGHT-LIGHT-POOLS-1 (gate MC2_TERRAIN_LIGHT_POOLS, DEFAULT ON): bind
        // the per-frame terrain light pool SSBO (packed+uploaded every frame by
        // gos_terrain_lighting::PackAndDispatch — the SAME buffer the legacy
        // vertex-lighting compute reads) at binding 28 and tell the frag how
        // many lights it holds. Gate=0, producer disabled
        // (MC2_TERRAIN_LIGHTING_GPU=0), or zero registered lights this frame
        // -> u_numPoolLights force-uploads 0 -> the frag loop is never entered
        // -> byte-identical output (the killswitch contract).
        {
            // VK-PARITY-TERRAIN-2: gate + 256 cap now resolved by the shared
            // producer gos_TerrainLightPoolCount() (VK terrain island reads it too).
            const int poolCount = gos_TerrainLightPoolCount();
            if (poolCount > 0)
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER,
                                 TERRAIN_LIGHT_POOL_SSBO_BINDING,
                                 gos_terrain_lighting::GetLightPoolSsbo());
            if (s_locNumPoolLights >= 0)
                glUniform1i(s_locNumPoolLights, poolCount);
            // LIGHT-CLUSTER-PERF-1: bind this frame's world-space XY light grid so
            // the frag loop visits only the lights whose ground circle covers the
            // fragment, instead of the whole (now 256-slot) pool. dim=0 uploads
            // when the grid is inactive (MC2_LIGHT_GRID=0 killswitch, small pool,
            // or no pool this frame) -> frag takes the exact linear loop.
            {
                gos_terrain_lighting::LightGridParams lg{};
                bool haveGrid = (poolCount > 0) &&
                                gos_terrain_lighting::GetLightGridParams(lg);
                if (haveGrid) {
                    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,
                                     TERRAIN_LIGHT_GRID_HEADER_SSBO_BINDING, lg.headerSsbo);
                    glBindBufferBase(GL_SHADER_STORAGE_BUFFER,
                                     TERRAIN_LIGHT_GRID_INDEX_SSBO_BINDING, lg.indexSsbo);
                }
                if (s_locLightGridDim >= 0)
                    glUniform1i(s_locLightGridDim, haveGrid ? static_cast<GLint>(lg.dim) : 0);
                if (haveGrid) {
                    if (s_locLightGridMin >= 0)
                        glUniform2f(s_locLightGridMin, lg.minXY[0], lg.minXY[1]);
                    if (s_locLightGridInvCell >= 0)
                        glUniform2f(s_locLightGridInvCell, lg.invCell[0], lg.invCell[1]);
                }
            }
            // NIGHT-LIGHT-POOLS-2 tuning (user feedback 2026-07-21): soften the
            // stamped-disc falloff edge (smoothstep tail; MC2_LIGHT_POOL_SOFT=0
            // -> exact legacy TG_Light::GetFalloff linear ramp) and dim
            // mover-attached lights (MC2_LIGHT_POOL_MOVER_DIM, default 0.5;
            // producer flags mover lights in the SSBO pad).
            if (s_locLightPoolSoft >= 0)
                glUniform1i(s_locLightPoolSoft, kTglcLightPoolSoft ? 1 : 0);
            if (s_locLightPoolMoverDim >= 0)
                glUniform1f(s_locLightPoolMoverDim, kTglcLightPoolMoverDim);
            // NIGHT-LIGHT-POOLS-3: summed-pool tone compression toggle.
            if (s_locLightPoolCompress >= 0)
                glUniform1i(s_locLightPoolCompress, kTglcLightPoolCompress ? 1 : 0);
            // NIGHT-LIGHT-POOLS-4: mover radius scale + max-blend.
            if (s_locLightPoolMoverScale >= 0)
                glUniform1f(s_locLightPoolMoverScale, kTglcLightPoolMoverRadius);
            if (s_locLightPoolMoverMax >= 0)
                glUniform1i(s_locLightPoolMoverMax, kTglcLightPoolMoverMax ? 1 : 0);
            // MOVER-CONTACT-SHADOW-1: under-mech contact shadow strength (0 = off).
            if (s_locMoverContactShadow >= 0)
                glUniform1f(s_locMoverContactShadow, kTglcMoverContactShadow);
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
            glUniform1i(s_locUseCement, (cementReady && s_cementSsbo.created()) ? 1 : 0);
        // CEMENT-DIAG-CONNECT-1: env gate MC2_TERRAIN_CEMENT_DIAG_CONNECT, default OFF
        // -> uploads 0 -> frag diagonal-fill block skipped (byte-identical).
        // REDUNDANT-PASS-HUNT-1: env resolved once (kTglcCementDiagConnect).
        if (s_locCementDiagConnect >= 0)
            glUniform1i(s_locCementDiagConnect, kTglcCementDiagConnect ? 1 : 0);
        // CEMENT-ATLAS-SEAM-1: half-texel sampling inset for the gutter-less
        // cement atlas (faint tile-grid seam lines across continuous pads).
        // Default ON; killswitch MC2_CEMENT_ATLAS_SEAM_FIX=0 reverts.
        if (s_locCementAtlasSeamFix >= 0)
            glUniform1i(s_locCementAtlasSeamFix, kTglcCementAtlasSeamFix ? 1 : 0);
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

    // TERRAIN-MATERIAL-TEXTURES-1: per-layer PBR albedo array at unit 4. Lazy
    // load at first gate-ON bind; u_useMatAlbedo uploads 0 when the gate is
    // OFF or the load failed -> frag takes the verbatim colormap-tint path
    // (byte-identical). Strength precedence: env > JSON (matAlbedoStrength,
    // via gos_SetTerrainMatAlbedoStrength) > 0.7 default.
    {
        GLuint albTex = 0;
        if (kTglcMatAlbedo) {
            tglc_EnsureMatAlbedoArrayLoaded();
            albTex = s_matAlbedoArrayTex;
        }
        const bool matAlbedoReady = (albTex != 0);
        if (s_locUseMatAlbedo >= 0)
            glUniform1i(s_locUseMatAlbedo, matAlbedoReady ? 1 : 0);
        if (matAlbedoReady) {
            if (s_locMatAlbedoArray >= 0) {
                glUniform1i(s_locMatAlbedoArray, kChunkTexUnitMatAlbedoArray);
                glActiveTexture(GL_TEXTURE0 + kChunkTexUnitMatAlbedoArray);
                glBindTexture(GL_TEXTURE_2D_ARRAY, albTex);
                glActiveTexture(GL_TEXTURE0);
            }
            if (s_locMatAlbedoStrength >= 0) {
                // env resolved once (mission-constant, REDUNDANT-PASS-HUNT-1
                // discipline); -1 sentinel = env absent -> JSON -> 0.7.
                static const float s_envMatAlbedoStrength = mc2::gate::get<"MC2_TERRAIN_MATERIAL_TEXTURES_STRENGTH">();
                const float strength =
                      (s_envMatAlbedoStrength >= 0.0f)  ? s_envMatAlbedoStrength
                    : (s_matAlbedoStrengthJson >= 0.0f) ? s_matAlbedoStrengthJson
                    : 0.7f;
                glUniform1f(s_locMatAlbedoStrength, strength);
            }
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

    // TERRAIN-SHORELINE-V3: elevation-placed wet/foam band. u_useShorelineMask
    // now gates the BAND ITSELF (MC2_TERRAIN_SHORELINE on), independent of
    // whether a mask sidecar was found -- v1/v2 required a loaded mask before
    // any band showed; V3's placement comes from v_worldPos.z vs
    // u_waterElevation, so the band works unconditionally once the gate is
    // on. The mask, when present (s_shorelineMaskTex != 0), is uploaded too
    // and applied as an OPTIONAL modulator (u_hasShorelineMask) -- it no
    // longer decides whether the band exists, only how it's shaped. Gate OFF
    // -> u_useShorelineMask uploads 0 -> frag skips the whole block -> byte-
    // identical (no band; legacy screen runShoreline() stays active per its
    // own gate).
    {
        // SHORELINE-SMOOTHING-REVISIT-1: default flipped ON. The elevation-
        // placed V3 band (fbm edge jitter, optional mask modulator) is what
        // draws the shoreline now. SHORELINE-GLOW-1: resolved through the one
        // shared TU accessor, so this read and the suppression accessor below
        // can no longer drift apart.
        const bool s_shorelineGateOn = gos_TerrainShorelineGateOn();
        const bool shorelineMaskReady = (s_shorelineMaskTex != 0);
        if (s_locUseShorelineMask >= 0)
            glUniform1i(s_locUseShorelineMask, s_shorelineGateOn ? 1 : 0);
        if (s_shorelineGateOn) {
            if (s_locHasShorelineMask >= 0)
                glUniform1i(s_locHasShorelineMask, shorelineMaskReady ? 1 : 0);
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
            }
            // TERRAIN-SHORELINE-V3: water elevation the bands are placed
            // relative to -- SAME source as the water fast path
            // (Terrain::waterElevation, mirrored into gosPostProcess at
            // mission load via gos_SetWaterElevation).
            if (s_locWaterElevation >= 0) {
                gosPostProcess* pp = getGosPostProcess();
                const float we = pp ? pp->getWaterElevation() : 0.0f;
                glUniform1f(s_locWaterElevation, we);
            }
            if (s_locShaderTime >= 0)
                glUniform1f(s_locShaderTime, gos_GetShaderClockSeconds());
            // TERRAIN-SHORELINE-V3 (visual-quality pass): runtime intensity
            // knobs, sampled once (feature gate is per-process anyway). Default
            // 1.0 = the authored modest band in terrain_lod_chunk.frag; clamp to
            // [0,2] so a bad env value can't blow the band out or invert it.
            static const float s_shorelineStrength = mc2::gate::get<"MC2_TERRAIN_SHORELINE_STRENGTH">();
            static const float s_shorelineFoamStrength = mc2::gate::get<"MC2_TERRAIN_SHORELINE_FOAM">();
            if (s_locShorelineStrength >= 0)
                glUniform1f(s_locShorelineStrength, s_shorelineStrength);
            if (s_locShorelineFoamStrength >= 0)
                glUniform1f(s_locShorelineFoamStrength, s_shorelineFoamStrength);
            // TERRAIN-SHORELINE-V3 (horizontal-run fix): band widths are
            // HORIZONTAL world-unit runs from the drawn waterline (the frag
            // converts vertical rise -> horizontal run via the macro slope;
            // see terrain_lod_chunk.frag). The c1593a1f conversion kept the
            // old VERTICAL defaults (wet 3.0wu / foam 1.2wu) as horizontal
            // runs -- ~1m of band, invisible at RTS zoom. Horizontal-native
            // defaults: wet 16.0wu (~4.8m) run, foam 5.0wu (~1.5m) run.
            // Primary knobs MC2_TERRAIN_SHORELINE_WET_RUN / _FOAM_RUN
            // (horizontal wu); legacy _WET_HEIGHT / _FOAM_HEIGHT names still
            // honored as aliases but are now interpreted as horizontal runs
            // (unit change documented in docs/tier1_env_vars.md).
            // MC2_TERRAIN_SHORELINE_WET_HEIGHT is the legacy alias, read only while WET_RUN is not set.
            static const float s_shorelineWetHeight = mc2::gate::isSet<"MC2_TERRAIN_SHORELINE_WET_RUN">()
                ? mc2::gate::get<"MC2_TERRAIN_SHORELINE_WET_RUN">()
                : mc2::gate::get<"MC2_TERRAIN_SHORELINE_WET_HEIGHT">();
            // MC2_TERRAIN_SHORELINE_FOAM_HEIGHT is the legacy alias, read only while FOAM_RUN is not set.
            static const float s_shorelineFoamHeight = mc2::gate::isSet<"MC2_TERRAIN_SHORELINE_FOAM_RUN">()
                ? mc2::gate::get<"MC2_TERRAIN_SHORELINE_FOAM_RUN">()
                : mc2::gate::get<"MC2_TERRAIN_SHORELINE_FOAM_HEIGHT">();
            if (s_locShorelineWetHeight >= 0)
                glUniform1f(s_locShorelineWetHeight, s_shorelineWetHeight);
            if (s_locShorelineFoamHeight >= 0)
                glUniform1f(s_locShorelineFoamHeight, s_shorelineFoamHeight);
            // TERRAIN-SHORELINE-V4-STYLE (zigzag fix): static world-XY jitter
            // amplitude (wu) for the band's distance-from-waterline, so the
            // wet/foam lobes stop tracing the mesh waterline's straight
            // diamond segments. 0 = exact V3 contour. Clamp [0,32] so a bad
            // env value can't scatter the band across the whole beach.
            static const float s_shorelineEdgeJitter = mc2::gate::get<"MC2_TERRAIN_SHORELINE_EDGE_JITTER">();
            if (s_locShorelineEdgeJitter >= 0)
                glUniform1f(s_locShorelineEdgeJitter, s_shorelineEdgeJitter);
        }
    }

    // Step 5a: upload the live material tunables (driven by the ImGui terrain
    // panel via the same gosRenderer members the legacy terrain reads).
    {
        float mt[5] = {3,2,1,6,1};  gos_GetTerrainMatTiling(&mt[0], &mt[1], &mt[2], &mt[3], &mt[4]);
        float nb[4] = {0.9f,1.1f,1.1f,2.5f}; gos_GetTerrainMatNormalBoost(&nb[0], &nb[1], &nb[2], &nb[3]);
        // The concrete detail normal is permanently off (user ruling 2026-07-26): nb[3]
        // is matNormalBoost.w, the only scale on the concrete branch in
        // terrain_lod_chunk.frag, so zeroing it makes that branch contribute exactly
        // vec3(0) without touching the other materials or the cement albedo path.
        nb[3] = 0.0f;
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
        if (s_locSnowBrightnessDampen >= 0) glUniform1f(s_locSnowBrightnessDampen, gos_TerrainSnowBrightnessDampen());
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
            float mh[4]={1,1,1,1}; gos_GetTerrainMatHeight(&mh[0],&mh[1],&mh[2],&mh[3]);  // POM-PERLAYER-HEIGHT-1
            if (s_locTintConcrete >= 0) glUniform3f(s_locTintConcrete, tc[0], tc[1], tc[2]);
            if (s_locTintSnow     >= 0) glUniform3f(s_locTintSnow,     tsn[0], tsn[1], tsn[2]);
            if (s_locMatRoughness >= 0) glUniform4f(s_locMatRoughness, mr[0], mr[1], mr[2], mr[3]);
            if (s_locMatAO        >= 0) glUniform4f(s_locMatAO,        ma[0], ma[1], ma[2], ma[3]);
            if (s_locMatHeight    >= 0) glUniform4f(s_locMatHeight,    mh[0], mh[1], mh[2], mh[3]);  // POM-PERLAYER-HEIGHT-1
            const bool matLibOn = gos_TerrainMaterialLibEnabled();
            if (s_locUseMaterialLib >= 0) glUniform1i(s_locUseMaterialLib, matLibOn ? 1 : 0);
        }

        // Remaining tunables. NFH strength scales the chunk's always-on smooth normal
        // (default 1.0 = no change). POM = legacy scale (default 0.02). Material
        // profile = global int (0=legacy).
        // CLIFF SHADOW FLOOR: env-gated. Unset/=0 uploads 0.0 (shader no-op,
        // byte-identical). Non-zero uploads the member value (ImGui-tunable), 0..1.
        {
            static const bool s_cliffOn = mc2::gate::get<"MC2_TERRAIN_CLIFF_SHADOW_FLOOR">();
            if (s_locCliffShadowFloor >= 0) {
                float cf = s_cliffOn ? gos_GetTerrainCliffShadowFloor() : 0.0f;
                if (cf < 0.0f) cf = 0.0f;
                if (cf > 1.0f) cf = 1.0f;
                glUniform1f(s_locCliffShadowFloor, cf);
            }
        }
        // ── TERRAIN-AMBIENT-FLOOR-2 (MC2_TERRAIN_AMBIENT_FLOOR) — DEFAULT-ON at 0.35 ──
        //
        // Terrain receives no other ambient term, so it is
        // exactly `baseColor * normalLight * shadow`, while static props standing
        // on it get ambient through SH-IBL. That is what bottoms out daylight
        // ground on mc2_03 beside the (correctly, user-ratified) flat-lit pads.
        // The frag adds `baseColor * ambCol * floor * ...`, albedo-scaled so dark
        // materials stay proportionally dark, masked by the flat-lit share so
        // concrete AND paved-overlay pavement are bit-identical.
        //
        // FULL REVERT IS THIS ONE GATE: MC2_TERRAIN_AMBIENT_FLOOR=0 uploads 0.0,
        // the frag block is skipped, output is byte-identical. There is no second
        // gate to remember -- the predecessor slice shipped two (a diffuse wrap
        // as well) and documented a one-gate revert that left half the change
        // running. Bounded 0..2; junk/NaN clamps to 0. Resolved once at start.
        {
            // ONE getenv. This used to carry a second, bare
            // `static const char* s_ambEnv = getenv("MC2_TERRAIN_AMBIENT_FLOOR");`
            // purely so the trace could echo the raw string, and that bare
            // capture -- a getenv with no idiom attached -- was the sole reason
            // check-gate-default-agreement.py reported this DEFAULT-ON gate as
            // UNCLASSIFIABLE (also called out in scripts/check-delivery.py's
            // notes as "ships default 0.35 and classified UNKNOWN"). The raw
            // string is now reported as a set/unset flag recorded by the
            // resolving lambda, so the single remaining reader is the ordinary
            // numeric-tunable form and the checker stops flagging it.
            // Removed 2026-07-30 (LIGHTING-INCAPABLE-BATCH-3). Behaviour is
            // unchanged: s_ambEnv was never read for anything but the trace line.
            // VK-PARITY: the env read + clamp now lives in gos_TerrainAmbientFloor()
            // (below, same file) so the VK terrain island shares ONE producer.
            const float s_amb = gos_TerrainAmbientFloor();
            const bool  s_ambEnvSet = gos_TerrainAmbientFloorEnvSet();
            if (s_locAmbientFloor >= 0) glUniform1f(s_locAmbientFloor, s_amb);
            // ── TERRAIN-AMBIENT-FLOOR-NIGHT-1 (MC2_TERRAIN_AMBIENT_FLOOR_NIGHT,
            //    DEFAULT 0.0 = OFF = byte-identical) ────────────────────────────
            //
            // WHY THIS EXISTS. TERRAIN-AMBIENT-FLOOR-2 shipped DEFAULT-ON to cure
            // "terrain is too dark", and its daylight gate is EXACTLY 0.0000 on
            // mc2_05, mc2_10, mc2_19, mc2_21 and mc2_23 -- which are precisely
            // the five lowest-multiplier (0.3670-0.4814), highest-floor-pin
            // (14.3-100% of fragments at diffuse == 0.02) maps in the game. The
            // shipped cure for dark terrain is switched off on exactly the five
            // maps where terrain is darkest, and those frames are byte-identical
            // to before it landed. Verified here at source, not inherited:
            // terrain_daylight.h uses kNightFullDeg = 10, and the authored
            // LightDirPitch values in mc2-srcdata/missions are 270 / 180 / 0 /
            // 270 / 9, folding to altitudes -1 / 0 / 0 / -1 / 9, all <= 10.
            //
            // WHAT THIS IS. A FLOOR under the daylight gate, not a second gate
            // and not a night-only branch: the uploaded value becomes
            // max(dayGate, floor). Consequences that made this the chosen shape:
            //   * default 0.0 -> max(dayGate, 0) == dayGate -> byte-identical on
            //     every mission, which is the whole revert;
            //   * monotone, so there is no discontinuity at the dusk band edge
            //     (a mission at gate 0.2 with floor 0.3 lands on 0.3, not on a
            //     step from 0.001 to 0.3);
            //   * it composes with MC2_TERRAIN_AMBIENT_FLOOR rather than
            //     duplicating it -- the strength stays one number, this is the
            //     fraction of it night maps are allowed.
            //
            // WHY DEFAULT OFF, AND IT MUST STAY OFF UNTIL THE USER GRADES IT.
            // The predecessor slice (TERRAIN-AMBIENT-FLOOR-1) was rejected at
            // merge review for lifting mc2_10's ground +59.1% through a fade that
            // could not fade out, and the user has explicitly PRAISED how mc2_05
            // currently looks. A night lift on these five maps is a taste call
            // this lane is not entitled to make, and it is not a regression fix.
            //
            // WHERE AN A/B ACTUALLY SHOWS SOMETHING (saturation checked first, so
            // this is not a recipe pointing at a mission where nothing can move):
            // the ambient term is ADDITIVE, so the normalLight floor pin does not
            // block it, and terrain's measured headroom to clip on these maps is
            // 9.07x (mc2_10) to 20.83x (mc2_21) -- nothing saturates. Grade
            // mc2_05 and mc2_10 from a ground-level pose that has both bare
            // terrain and a concrete pad in frame, 0 vs 0.30. EXPECT: bare
            // terrain up, pads 0.00% (flatLitShare exempts them), i.e. the
            // pad:ground ratio closes from the terrain side only.
            // ONE getenv, deliberately. The sibling gate above uses a second,
            // bare `static const char* = getenv(...)` purely so its trace can
            // echo the raw string, and that bare capture is exactly why
            // check-gate-default-agreement.py reports MC2_TERRAIN_AMBIENT_FLOOR
            // as UNCLASSIFIABLE: a getenv with no idiom on it is an unreadable
            // reader. Here the raw-string need is met by a set/unset flag the
            // resolving lambda records, so this gate presents a single reader
            // whose unset branch (`if (!e || !e[0]) return 0.0f;`) is the
            // canonical explicit DEFAULT-OFF form.
            const float s_nightGateFloor = gos_TerrainAmbientNightFloor();   // VK-PARITY shared producer
            const bool  s_nightEnvSet    = gos_TerrainAmbientNightFloorEnvSet();
            // Daylight level, computed by terrain_daylight.h from the AUTHORED sun
            // pitch min the engine night state and pushed per frame by gamecam.cpp.
            // 0 on every night mission in the shipping data -> the block is skipped
            // there -> night is byte-identical (unless the floor above arms it).
            const float rawDayGate = gos_GetTerrainDaylightLevel();
            const float dayGate    = (rawDayGate > s_nightGateFloor) ? rawDayGate
                                                                    : s_nightGateFloor;
            if (s_locAmbientDayGate >= 0) glUniform1f(s_locAmbientDayGate, dayGate);
            // MC2_TERRAIN_AMBIENT_FLOOR_TRACE: one line, first upload. Prints the
            // resolved floor, the day gate actually uploaded, and both raw inputs,
            // so "did the gate engage on this mission and why" is answerable from
            // a log instead of from a capture diff. raw_dayGate is printed
            // alongside the uploaded gate so the night floor's effect is visible
            // rather than inferred.
            static bool s_traced = false;
            if (!s_traced && mc2::gate::get<"MC2_TERRAIN_AMBIENT_FLOOR_TRACE">()) {
                s_traced = true;
                bool  isNight = false; float nf = 0.0f;
                gos_GetNightState(&isNight, &nf);
                float lx = 0.0f, ly = 0.0f, lz = 0.0f;
                gos_GetTerrainLightDir(&lx, &ly, &lz);
                fprintf(stderr,
                        "[TERRAIN_AMBIENT_FLOOR v2] floor=%.3f dayGate=%.4f "
                        "raw_dayGate=%.4f night_floor=%.4f night_env_set=%d "
                        "floor_env_set=%d "
                        "runtime_isNight=%d runtime_nightFactor=%.3f runtime_lightDirZ=%.4f\n",
                        s_amb, dayGate, rawDayGate, s_nightGateFloor,
                        s_nightEnvSet ? 1 : 0, s_ambEnvSet ? 1 : 0,
                        isNight ? 1 : 0, nf, lz);
                fflush(stderr);
            }
        }
        if (s_locNfhStrength >= 0) glUniform1f(s_locNfhStrength, gos_GetTerrainNormalsFromHeightStrength());
        // TERRAIN-SLOPE-BIAS-VISUAL-1 (B4a): env gate MC2_TERRAIN_SLOPE_BIAS, default
        // OFF. When unset/0 the gate uploads 0 and the frag block is a no-op
        // (byte-identical). Strength via MC2_TERRAIN_SLOPE_BIAS_STRENGTH (default 1.0).
        // REDUNDANT-PASS-HUNT-1: env resolved once (kTglcSlopeBias/Str), not per frame.
        {
            if (s_locUseRockSlopeBias >= 0) glUniform1i(s_locUseRockSlopeBias, kTglcSlopeBias ? 1 : 0);
            if (s_locRockSlopeBiasStr >= 0) glUniform1f(s_locRockSlopeBiasStr, kTglcSlopeBiasStr);
        }
        // TERRAIN-CLIFF-MATERIAL-TRIPLANAR-1: env gate MC2_TERRAIN_CLIFF_TRIPLANAR,
        // default OFF -> uploads 0 -> frag block no-op (byte-identical). Strength via
        // MC2_TERRAIN_CLIFF_TRIPLANAR_STRENGTH (default 1.0).
        // REDUNDANT-PASS-HUNT-1: env resolved once (kTglcCliffTriplanar/Str).
        {
            if (s_locUseTriplanarCliff >= 0) glUniform1i(s_locUseTriplanarCliff, kTglcCliffTriplanar ? 1 : 0);
            if (s_locCliffTriplanarStr >= 0) glUniform1f(s_locCliffTriplanarStr, kTglcCliffTriplanarStr);
            // TERRAIN-CLIFF-HEIGHT-NORMAL-1: shading normal from cliff displacement
            // height gradient (default 2.0). Only consumed inside the frag cliff
            // block (useTriplanarCliff!=0); gate-OFF stays byte-identical.
            if (s_locCliffHeightNormalStr >= 0) glUniform1f(s_locCliffHeightNormalStr, kTglcCliffHeightNormalStr);
            // TERRAIN-CLIFF-POM-1: gate MC2_TERRAIN_CLIFF_POM (default OFF -> .x=0
            // -> frag POM march skipped, triplanar block == TRIPLANAR-1 exactly ->
            // byte-identical). Depth/steps knobs resolved once (kTglcCliffPom*).
            if (s_locCliffPom >= 0)
                glUniform4f(s_locCliffPom, kTglcCliffPom ? 1.0f : 0.0f,
                            kTglcCliffPomDepth, kTglcCliffPomSteps, 0.0f);
            // TERRAIN-CLIFF-DEBUG: bounded debug-viz (default 0 -> byte-identical).
            if (s_locCliffDebug >= 0) glUniform1i(s_locCliffDebug, kTglcCliffDebug);
        }
        // TERRAIN-MACRO-VARIATION-1: env gate MC2_TERRAIN_MACRO_VARIATION, default
        // OFF -> uploads 0 -> frag block skipped (byte-identical). Strength via
        // MC2_TERRAIN_MACRO_VARIATION_STRENGTH (default 1.0).
        // REDUNDANT-PASS-HUNT-1: env resolved once (kTglcMacroVariation).
        if (s_locMacroVariation >= 0)
            glUniform1f(s_locMacroVariation, kTglcMacroVariation);
        // TERRAIN-EDGE-FEATHER-1: env gate MC2_TERRAIN_EDGE_FEATHER, default ON
        // (kill-switch; reader gos_terrain_lod_chunk.cpp:158 is `!(e && e[0]=='0')`.
        // Said "default OFF" until ENV-DROP-CLOSE-1, 2026-07-30).
        // =0 uploads 0 -> frag block skipped (byte-identical). Fades the terrain
        // colormap to sky/haze over the last ~tile band so the hard straight
        // map-perimeter line dissolves into the fog. Strength via
        // MC2_TERRAIN_EDGE_FEATHER_STRENGTH (default 1.0).
        // REDUNDANT-PASS-HUNT-1: env resolved once (kTglcEdgeFeather/Str).
        {
            if (s_locEdgeFeather >= 0) glUniform1i(s_locEdgeFeather, kTglcEdgeFeather ? 1 : 0);
            if (s_locEdgeFeatherStr >= 0) glUniform1f(s_locEdgeFeatherStr, kTglcEdgeFeatherStr);
        }
        // TERRAIN-CHUNK-POM-1: gate MC2_TERRAIN_POM (default OFF). Gate OFF ->
        // u_pomView.x=0 -> the frag takes the legacy faux-view-vector
        // chunkParallax() path VERBATIM, and pomParams keeps the stock upload
        // below unchanged (supervisor ruling: gate-OFF byte-identity INCLUDES
        // the faux shear — do NOT zero pomParams when OFF). Gate ON swaps in
        // the REAL per-fragment view vector with a world-distance fade. Knobs:
        //   MC2_TERRAIN_POM_SCALE  float march scale (default gos_GetTerrainPOMScale()=0.02)
        //   MC2_TERRAIN_POM_STEPS  int   max march layers, clamp 4..16 (default 16)
        //   MC2_TERRAIN_POM_NEAR / MC2_TERRAIN_POM_FAR  fade band in world units
        //                          (default 1500..3500; 1 tile = 384 wu)
        {
            static const bool s_pomGate = mc2::gate::get<"MC2_TERRAIN_POM">();
            static const float s_pomScale = mc2::gate::get<"MC2_TERRAIN_POM_SCALE">();
            static const float s_pomSteps = mc2::gate::get<"MC2_TERRAIN_POM_STEPS">();
            static const float s_pomNear = mc2::gate::get<"MC2_TERRAIN_POM_NEAR">();
            static const float s_pomFar = mc2::gate::get<"MC2_TERRAIN_POM_FAR">();
            if (s_locPomParams >= 0) {
                if (s_pomGate) {
                    const float scale    = (s_pomScale > 0.0f) ? s_pomScale : gos_GetTerrainPOMScale();
                    const float minSteps = (s_pomSteps < 8.0f) ? s_pomSteps : 8.0f;
                    glUniform4f(s_locPomParams, scale, minSteps, s_pomSteps, 0.0f);
                } else {
                    // Stock upload — byte-for-byte the pre-slice line.
                    glUniform4f(s_locPomParams, gos_GetTerrainPOMScale(), 8.0f, 32.0f, 0.0f);
                }
            }
            if (s_locPomView >= 0)
                glUniform4f(s_locPomView, s_pomGate ? 1.0f : 0.0f,
                            s_pomNear, (s_pomFar > s_pomNear) ? s_pomFar : s_pomNear + 1.0f, 0.0f);
            // cameraPos uploads every submit regardless of the gate (the frag
            // only reads it on the gate-ON branch and the 8192 oracle viz).
            if (s_locCameraPos >= 0) {
                float cx = 0.0f, cy = 0.0f, cz = 0.0f;
                gos_GetTerrainCameraPos(&cx, &cy, &cz);
                glUniform4f(s_locCameraPos, cx, cy, cz, 1.0f);
            }
        }
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
    TerrainGlFallbackAudit s_vkTerrainGlAudit;
    const bool s_vkTerrainGlAuditOn = terrainGlFallbackAuditEnabled();
#endif

    for (int i = 0; i < count; ++i)
    {
        const TerrainDrawCommand& cmd = cmds[i];
        int qcX = cmd.quadCountsPacked & 0xFF;
        int qcY = (cmd.quadCountsPacked >> 8) & 0xFF;

        if (qcX <= 0 || qcY <= 0) continue;

#ifdef MC2_VULKAN_ISLAND
        if (s_vkTerrainGlAuditOn) {
            ++s_vkTerrainGlAudit.total;
        }
        // VK-P10 TERRAIN TIER-1 (+ rung-3 VK-TERRAIN-ALL-LODS): hand main patches to
        // the VK island and skip their GL main+skirt draws. LOD0 (lodStep==1) is the
        // original probe scope; MC2_VK_TERRAIN_ALL_LODS extends capture to coarse bands
        // (lodStep>1) too -- BANDS ONLY, still excluding mode-1 visual-displace (LOD0
        // 4x-finer topology, a separate L slice). Edge-stitch is NO LONGER excluded
        // (VK-TERRAIN-EDGE-STITCH-1 / commit 0aae0337 removed the edgeStitch[i]==0
        // predicate term; stitched patches are VK-captured, corner-pin math ported into
        // the tier1 verts). Default-OFF (MC2_VK_TERRAIN_ALL_LODS unset)
        // -> capture predicate is IDENTICAL to before (lodStep==1 only).
        const bool s_vkAllLodsActive = s_vkTerrainActive &&
            (mc2_vk_terrain_all_lods_enabled() != 0);
        const bool s_vkCaptureThisBand = (cmd.lodStep == 1) || s_vkAllLodsActive;
        // VK-TERRAIN-VISUALDISPLACE-FAR-1 (rung 5): relax the visualDisplaceActive
        // exclusion for mode-2 patches (lodStep>1, pure Z-swap, same vertex density)
        // when the sub-gate is armed. Mode-1 patches (lodStep==1, 4x-finer topology +
        // corner-pinned edges) stay GL-owned (separate L slice later). Stitched patches
        // are VK-captured (edge-stitch exclusion removed, see above). Gate OFF ->
        // predicate IDENTICAL to rung 4.
        const bool s_vkVisDispFarActive = s_vkAllLodsActive &&
            (mc2_vk_terrain_visualdisplace_far_enabled() != 0);
        const bool s_vkOwnsDisplace = !visualDisplaceActive ||
            (s_vkVisDispFarActive && cmd.lodStep != 1);
        const bool s_vkNoEdgeStitch = (edgeStitch == nullptr || edgeStitch[i] == 0);
        if (s_vkTerrainActive && s_vkCaptureThisBand &&
            s_vkOwnsDisplace) {
            const int maxOffX = (cmd.lodStep > 0) ? (qcX / cmd.lodStep) * cmd.lodStep : qcX;
            const int maxOffY = (cmd.lodStep > 0) ? (qcY / cmd.lodStep) * cmd.lodStep : qcY;
            Mc2TerrainTier1Patch p;
            p.blockOriginX = cmd.blockOriginX;
            p.blockOriginY = cmd.blockOriginY;
            p.quadCountX   = maxOffX;
            p.quadCountY   = maxOffY;
            p.lodStep      = cmd.lodStep;
            // VK-TERRAIN-SKIRT (rung 4): snapshot this block's skirt inputs into the
            // SAME record so the VK island can draw the skirt in the same co-write pass
            // as the main patch, with no extra GL state read on the VK side. Only
            // populated when the skirt sub-gate is armed; else 0/0 (island draws no
            // skirt for this patch, matching today's GL-owns-all-skirts fallback --
            // the GL skip below is ALSO gated on the same predicate so GL keeps drawing
            // the skirt whenever the VK island isn't going to).
            const bool s_vkSkirtsActive = mc2_vk_terrain_skirts_enabled() != 0;
            p.skirtDepth    = (s_vkSkirtsActive && skirtDepths)    ? skirtDepths[i]    : 0.0f;
            p.skirtEdgeMask = (s_vkSkirtsActive && skirtEdgeMasks) ? skirtEdgeMasks[i] : 0u;
            // VK-TERRAIN-VISUALDISPLACE-FAR-1: mirror the GL displaceFar decision below
            // (visualDisplaceActive && lodStep != 1 -> mode 2). Mode-1 patches never get
            // here (s_vkOwnsDisplace excluded them), so this is only ever 0 or 2.
            p.visualMode    = (visualDisplaceActive && cmd.lodStep != 1) ? 2 : 0;
            p.edgeStitch    = edgeStitch ? edgeStitch[i] : 0u;
            s_vkTerrainPatches.push_back(p);
            s_vkTerrainFeedValid = true;
            if (s_vkTerrainGlAuditOn) {
                ++s_vkTerrainGlAudit.captured;
                if (!s_vkNoEdgeStitch) {
                    ++s_vkTerrainGlAudit.capturedStitched;
                }
            }
            continue;   // GL skips this patch's main + skirt draws unconditionally (this
                        // was already true pre-rung-4: VK-owned patches never got a GL
                        // skirt draw, since `continue` fires before the skirt-draw code
                        // below runs). Pre-rung-4 that meant VK-owned patches had NO
                        // skirt at all -- a latent crack risk masked only by rung-3's
                        // narrow scope (bands still matched GL neighbor geometry closely
                        // enough per the ladder doc). This rung closes that gap: when the
                        // skirts sub-gate is armed, skirtDepth/skirtEdgeMask above are
                        // populated and the VK island draws the skirt itself; when the
                        // sub-gate is off, behavior is UNCHANGED from rung-3 (0/0 ->
                        // island draws no skirt for this patch).
        }
        if (s_vkTerrainGlAuditOn) {
            ++s_vkTerrainGlAudit.glFallback;
            if (!s_vkTerrainActive) {
                ++s_vkTerrainGlAudit.inactive;
            } else if (!s_vkCaptureThisBand) {
                ++s_vkTerrainGlAudit.band;
            } else if (!s_vkOwnsDisplace) {
                ++s_vkTerrainGlAudit.displace;
            } else if (!s_vkNoEdgeStitch) {
                ++s_vkTerrainGlAudit.edgeStitch;
            } else {
                ++s_vkTerrainGlAudit.other;
            }
            if (skirtDepths && skirtEdgeMasks && skirtDepths[i] > 0.0f) {
                unsigned int mask = skirtEdgeMasks[i];
                s_vkTerrainGlAudit.skirtEdges +=
                    ((mask & 1u) ? 1 : 0) + ((mask & 2u) ? 1 : 0) +
                    ((mask & 4u) ? 1 : 0) + ((mask & 8u) ? 1 : 0);
            }
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
        // TERRAIN-LOD-GEOMORPH-1: per-block geomorph factor. Uploaded 0 whenever
        // the geomorph is inactive so a stale value can never leak into a draw.
        if (s_locMorphFactor >= 0)
            glUniform1f(s_locMorphFactor,
                        (geomorphActive && morphFactors) ? morphFactors[i] : 0.0f);

        // --- Draw main patch (skirtDepth=0 so isSkirtFlag pulls height by 0) ---
        if (s_locSkirtDepth >= 0)
            glUniform1f(s_locSkirtDepth, 0.0f);

        // Attrib 0: ivec2 localOffset. Attrib 1 (isSkirt) left disabled -> reads as 0.
        glDisableVertexAttribArray(1);
        patch.vbo.bind();
        glEnableVertexAttribArray(0);
        glVertexAttribIPointer(0, 2, GL_SHORT, (GLsizei)(2 * sizeof(int16_t)), (const void*)0);

        patch.ibo.bind();

        // SLICE 3a: near-field (LOD0 == lodStep 1) chunks draw as GL_PATCHES
        // through the tess variant program when the gate is ON and the program
        // built. Pass-through TES => visually identical to the base draw. All
        // other bands, and the skirts below, stay GL_TRIANGLES on the base
        // program. Default OFF => useTess is always false => byte-identical.
        const bool useTess = s_cliffTessGate && s_terrainTessProgram
                             && s_terrainTessProgram->is_valid()
                             && (cmd.lodStep == 1 /* near-field proxy */);
        if (useTess) {
            // CLIFF-TESS-PERF: hoisted mirror. The expensive full enumerate-and-
            // copy (all active uniforms) runs ONCE PER FRAME on the first near-
            // field tess patch; every subsequent patch mirrors only the ~10
            // per-patch uniforms via the cached-location targeted fast path.
            const GLuint tessProg = s_terrainTessProgram->shp_;
            glUseProgram(tessProg);
            resolveTessPatchLocs(tessProg);  // one-time lazy location cache
            if (!s_tessMirroredThisFrame) {
                // Full mirror: copies frame-constant uniforms (mvp, mapSide,
                // material/POM/shadow/light state, samplers, u_cliffTessLevel).
                // Per-patch uniforms are (re)set by the targeted mirror below.
                mirrorTerrainUniforms(s_terrainProgram, tessProg);
                s_tessMirroredThisFrame = true;
            }
            // Targeted per-patch mirror: up-to-date per-block values every patch.
            mirrorTerrainPatchUniforms(s_terrainProgram, tessProg);
            ZoneScopedN("Terrain.CliffTess");
            // NOTE: global GL state, left set intentionally. Safe today because
            // this is the only GL_PATCHES draw in the frame; a future second
            // tessellated path with a different patch size MUST re-set this.
            glPatchParameteri(GL_PATCH_VERTICES, 3);
            glDrawElements(GL_PATCHES, patch.indexCount, GL_UNSIGNED_SHORT, 0);
            // Restore the base program for the skirt draw / next patch.
            glUseProgram(s_terrainProgram);
        } else {
            glDrawElements(GL_TRIANGLES, patch.indexCount, GL_UNSIGNED_SHORT, 0);
        }

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
                patch.skirtVbo.bind();
                glEnableVertexAttribArray(0);
                glVertexAttribIPointer(0, 2, GL_SHORT, (GLsizei)(4 * sizeof(int16_t)), (const void*)0);
                glEnableVertexAttribArray(1);
                glVertexAttribIPointer(1, 1, GL_SHORT, (GLsizei)(4 * sizeof(int16_t)), (const void*)(2 * sizeof(int16_t)));

                patch.skirtIbo.bind();
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

#ifdef MC2_VULKAN_ISLAND
    if (s_vkTerrainGlAuditOn) {
        static unsigned long s_auditFrame = 0;
        ++s_auditFrame;
        if (s_auditFrame <= 60 || (s_auditFrame % 180) == 0) {
            fprintf(stderr,
                    "[VK_TERRAIN_GL_FALLBACK_AUDIT v1] frame=%lu total=%d captured=%d "
                    "gl_fallback=%d inactive=%d band=%d displace=%d edge_stitch=%d "
                    "captured_stitch=%d other=%d skirt_edges=%d\n",
                    s_auditFrame,
                    s_vkTerrainGlAudit.total,
                    s_vkTerrainGlAudit.captured,
                    s_vkTerrainGlAudit.glFallback,
                    s_vkTerrainGlAudit.inactive,
                    s_vkTerrainGlAudit.band,
                    s_vkTerrainGlAudit.displace,
                    s_vkTerrainGlAudit.edgeStitch,
                    s_vkTerrainGlAudit.capturedStitched,
                    s_vkTerrainGlAudit.other,
                    s_vkTerrainGlAudit.skirtEdges);
            fflush(stderr);
        }
    }
#endif

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
        // runShoreline() pass so the seam isn't brightened twice.
        //
        // LEGACY-SHORELINE-MISAIM-1: the AND with IsShorelineMaskActive() (i.e.
        // with MC2_TERRAIN_SHORELINE) is REMOVED. It encoded "don't brighten the
        // seam twice", but the legacy pass provably cannot reach a water pixel
        // on this path (see gos_TerrainShorelineLegacySuppressOn's block), so
        // there is no second brightening of the seam to avoid -- there is an
        // animated foam rim on prop silhouettes, and turning the V3 band off
        // must not bring it back. Suppression is now solely its own gate,
        // default ON.
        //
        // SHORELINE-DISPOSITION-1 (2026-07-31) CORRECTION: this comment used to
        // end "This call site stays inside the LOD-chunk draw ON PURPOSE: the
        // legacy terrain path (MC2_TERRAIN_LOD_CHUNK=0) never reaches it, so
        // that path keeps the pass, which is the one configuration where the
        // 0.15-0.35 classifier actually matches water." That is FALSE -- see the
        // CORRECTION block on gos_TerrainShorelineLegacySuppressOn() below.
        // gos_terrain.frag's materialAlpha write is behind #ifdef MRT_ENABLED,
        // which reaches exactly one program, and that program is never bound.
        // The classifier matches water in NO configuration.
        //
        // The call site staying here is therefore an ACCIDENT OF HISTORY, not a
        // design: under MC2_TERRAIN_LOD_CHUNK=0 the setter simply never runs and
        // the mis-aimed pass runs unsuppressed. That is a latent wart, not a
        // feature. It is left in place only because the correct fix is deleting
        // the pass outright rather than relocating a setter for a dead effect.
        pp->setShorelineSuppressedByTerrainMask(
            gos_TerrainShorelineLegacySuppressOn());
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
    if (visualDampActive)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_VISUAL_DAMP_SSBO_BINDING, 0);
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
    // VK-TERRAIN-ALL-LODS diagnostic (rung 3 oracle evidence): per-600-submit
    // lodStep breakdown of what the VK island actually captured this submit,
    // so a smoke log proves coarse bands (lodStep>1) are reaching the island
    // (not just LOD0). Cheap (once per 600 submits), no gate needed.
    if (s_submitCount % 600 == 0) {
        int cnt1 = 0, cntCoarse = 0;
        for (const auto& p : s_vkTerrainPatches) {
            if (p.lodStep == 1) ++cnt1; else ++cntCoarse;
        }
        printf("[VK_TERRAIN_ALL_LODS] submit=%d feedValid=%d active=%d allLods=%d vkPatches=%zu lodStep1=%d coarse(lodStep>1)=%d\n",
                s_submitCount, s_vkTerrainFeedValid ? 1 : 0, s_vkTerrainActive ? 1 : 0,
                mc2_vk_terrain_all_lods_enabled(), s_vkTerrainPatches.size(), cnt1, cntCoarse);
        fflush(stdout);
    }

    if (s_vkTerrainEnabled) {
        if (gosPostProcess* ppVk = getGosPostProcess()) {
            const int ran = mc2_vk_terrain_island_run(ppVk->getWidth(), ppVk->getHeight());
            // VK-PARITY-F3: the terrain pass just cleared color0 to the flat sky colour;
            // fill the untouched (depth==far) pixels with the HDRI sky GL would have drawn.
            if (ran) (void)mc2_vk_sky_island_run(ppVk->getWidth(), ppVk->getHeight());
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
    if (!s_heightSsbo.created())
    {
        fprintf(stderr, "[TerrainLodChunk] UploadHeightFull called before Init\n");
        fflush(stderr);
        return;
    }
    if (!elevations || mapSide <= 0)
        return;

    GLsizeiptr bytes = (GLsizeiptr)mapSide * mapSide * sizeof(float);

    s_heightSsbo.bind();
    s_heightSsbo.bufferData((intptr_t)bytes, elevations,
                            RenderCore::GpuBufferUsage::DynamicDraw);
    s_heightSsbo.bindBase(TERRAIN_HEIGHT_SSBO_BINDING);
    s_heightSsbo.unbind();

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
    s_heightSsbo.registerNow(static_cast<uint64_t>(bytes));

#ifdef _DEBUG
    // First-frame readback verify: confirm that the GPU round-trips the first
    // float correctly. glGetBufferSubData is available on all desktop GL >=3.1.
    float firstSample = 0.0f;
    s_heightSsbo.bind();
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(float), &firstSample);
    s_heightSsbo.unbind();
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
void gos_TerrainLodChunk_UploadVisualHeightFull(const float* visualHeights, int V,
                                                const float* mipMaxes, int mipFloats)
{
    if (!visualHeights || V <= 0)
        return;
    if (!s_visualHeightSsbo.created())
    {
        if (!s_visualHeightSsbo.create())
        {
            fprintf(stderr, "[VISUAL_HEIGHT v1] glGenBuffers failed\n");
            fflush(stderr);
            return;
        }
    }
    // TERRAIN-LOD-GEOMORPH-1: mips (when shipped) are appended to the SAME
    // buffer after the fine bake — shader offsets are computable from
    // u_visualSide/u_mapSide alone, no extra binding slot consumed.
    if (!mipMaxes) mipFloats = 0;
    GLsizeiptr fineBytes = (GLsizeiptr)V * (GLsizeiptr)V * (GLsizeiptr)sizeof(float);
    GLsizeiptr mipBytes  = (GLsizeiptr)mipFloats * (GLsizeiptr)sizeof(float);
    GLsizeiptr bytes     = fineBytes + mipBytes;
    s_visualHeightSsbo.bind();
    s_visualHeightSsbo.bufferData((intptr_t)bytes, nullptr,
                                  RenderCore::GpuBufferUsage::StaticDraw);
    s_visualHeightSsbo.bufferSubData(0, (intptr_t)fineBytes, visualHeights);
    if (mipBytes > 0)
        s_visualHeightSsbo.bufferSubData((intptr_t)fineBytes, (intptr_t)mipBytes, mipMaxes);
    s_visualHeightSsbo.unbind();
    s_visualSide      = V;   // remembered for the displaced draw (u_visualSide)
    s_visualMipFloats = (int)mipFloats;

#ifdef MC2_VULKAN_ISLAND
    // VK-TERRAIN-VISUALDISPLACE-FAR-1: the visual bake changed -> bump the snapshot key
    // so the VK terrain island re-copies it (mission-load cadence, mirrors the height key).
    ++g_terrainVisualHeightVersion;
#endif

    // TERRAIN-VISUAL-HEIGHT-SSBO-OWNER-1: register the live visual-height SSBO
    // (observe-only metadata; never read by the draw path). Reached here only when
    // the upstream gate (MC2_TERRAIN_VISUAL_HEIGHT/_DISPLACE + bake) called us.
    s_visualHeightSsbo.registerNow(static_cast<uint64_t>(bytes));
    // Bound to base 26 in the draw only when displacement is active.
    fprintf(stderr, "[VISUAL_HEIGHT v1] SSBO uploaded binding=%u V=%d bytes=%lld first=%.3f "
            "geomorphMips=%s(%d floats)\n",
            TERRAIN_VISUAL_HEIGHT_SSBO_BINDING, V, (long long)bytes, visualHeights[0],
            mipFloats > 0 ? "YES" : "no", (int)mipFloats);
    fflush(stderr);
}

// TERRAIN-REAUTH-UNPIN-1 Half B: static (buildings) object-proximity damp map.
// Reached only when the upstream gates passed (mclib/terrain.cpp). Keeps a CPU
// copy so per-frame mover stamps can min-combine without re-reading the GPU.
void gos_TerrainLodChunk_UploadVisualDampStatic(const float* damp01, int side)
{
    if (!damp01 || side <= 0)
        return;
    const size_t count = (size_t)side * (size_t)side;
    s_visualDampStatic.assign(damp01, damp01 + count);
    s_visualDampCombined = s_visualDampStatic;
    s_visualDampSide = side;
    s_visualDampHadMovers = false;
    if (!s_visualDampSsbo.created())
    {
        if (!s_visualDampSsbo.create())
        {
            fprintf(stderr, "[VISUAL_DAMP v1] glGenBuffers failed\n");
            fflush(stderr);
            return;
        }
    }
    const GLsizeiptr bytes = (GLsizeiptr)(count * sizeof(float));
    s_visualDampSsbo.bind();
    s_visualDampSsbo.bufferData((intptr_t)bytes, s_visualDampCombined.data(),
                                RenderCore::GpuBufferUsage::DynamicDraw);
    s_visualDampSsbo.unbind();
    s_visualDampSsbo.registerNow(static_cast<uint64_t>(bytes));
    fprintf(stderr, "[VISUAL_DAMP v1] SSBO uploaded binding=%u side=%d bytes=%lld\n",
            TERRAIN_VISUAL_DAMP_SSBO_BINDING, side, (long long)bytes);
    fflush(stderr);
}

bool gos_TerrainLodChunk_VisualDampWanted()
{
    static const bool s_displaceGate = mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE">();
    static const bool s_dampGate = mc2::gate::get<"MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE">();
    return s_displaceGate && s_dampGate && s_visualDampSsbo.created()
           && s_visualDampSide > 0 && !s_visualDampStatic.empty();
}

// Per-frame mover stamps. combined = min(static, smoothstep(inner, inner+radius,
// dist)) per mover; one full-buffer BufferSubData (side^2 floats, ~57KB@120 —
// negligible). Zero-mover frames upload only on the first one after movers
// disappear (restores the pure-static map, then no-ops).
void gos_TerrainLodChunk_UpdateVisualDampMovers(const float* cellXY, int count,
                                                float radiusCells, float innerCells)
{
    if (!s_visualDampSsbo.created() || s_visualDampSide <= 0 || s_visualDampStatic.empty())
        return;
    if (count <= 0 && !s_visualDampHadMovers)
        return;   // steady state: static map already on the GPU
    const int N = s_visualDampSide;
    s_visualDampCombined = s_visualDampStatic;
    if (radiusCells < 0.25f) radiusCells = 0.25f;
    if (innerCells < 0.0f) innerCells = 0.0f;
    const float reach = innerCells + radiusCells;
    for (int m = 0; m < count; ++m)
    {
        const float cx = cellXY[m * 2 + 0];
        const float cy = cellXY[m * 2 + 1];
        int x0 = (int)floorf(cx - reach), x1 = (int)ceilf(cx + reach);
        int y0 = (int)floorf(cy - reach), y1 = (int)ceilf(cy + reach);
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
        if (x1 > N - 1) x1 = N - 1; if (y1 > N - 1) y1 = N - 1;
        for (int iy = y0; iy <= y1; ++iy)
            for (int ix = x0; ix <= x1; ++ix)
            {
                const float dx = (float)ix - cx;
                const float dy = (float)iy - cy;
                float t = (sqrtf(dx * dx + dy * dy) - innerCells) / radiusCells;
                if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
                const float s = t * t * (3.0f - 2.0f * t);
                float& ref = s_visualDampCombined[(size_t)ix + (size_t)iy * N];
                if (s < ref) ref = s;
            }
    }
    s_visualDampHadMovers = (count > 0);
    s_visualDampSsbo.bind();
    s_visualDampSsbo.bufferSubData(0,
                    (intptr_t)(s_visualDampCombined.size() * sizeof(float)),
                    s_visualDampCombined.data());
    s_visualDampSsbo.unbind();
}

// Step 5b: per-vertex terrainType upload (parallel to the heightfield). Used by
// the chunk frag's concrete material/colour selection (pureConcrete).
void gos_TerrainLodChunk_UploadTerrainTypeFull(const float* types, int mapSide)
{
    if (!s_typeSsbo.created() || !types || mapSide <= 0)
        return;
    GLsizeiptr bytes = (GLsizeiptr)mapSide * mapSide * sizeof(float);
    s_typeSsbo.bind();
    s_typeSsbo.bufferData((intptr_t)bytes, types,
                          RenderCore::GpuBufferUsage::DynamicDraw);
    s_typeSsbo.bindBase(TERRAIN_TYPE_SSBO_BINDING);
    s_typeSsbo.unbind();

    // TERRAIN-LODCHUNK-SSBO-OWNER-1: register the live type SSBO (observe-only
    // metadata; never read by the draw path). Registered here (not at Init)
    // because the byte size is only known once the type data is uploaded.
    s_typeSsbo.registerNow(static_cast<uint64_t>(bytes));
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
        // TEX-CLASS: asset-pool -- terrain control-map sidecar (mission-lifetime content)
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
        // TEX-CLASS: asset-pool -- terrain overlay sidecar (mission-lifetime content)
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
        // TEX-CLASS: asset-pool -- shoreline mask sidecar (mission-lifetime content, SHORE-MASK deferred-sweep-1)
        glGenTextures(1, &s_shorelineMaskTex);
    if (s_shorelineMaskTex == 0)
    {
        MC2_DIAGFE(stderr, "[TERRAIN_SHORELINE v1] glGenTextures failed\n");
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

    ++s_waterShoreDataGen;   // VK-PARITY-WATER-SHORE-1
    s_shorelineBounds[0] = boundsTopLeftX;
    s_shorelineBounds[1] = boundsTopLeftY;
    s_shorelineBounds[2] = boundsSizeX;
    s_shorelineBounds[3] = boundsSizeY;

    MC2_DIAGFE(stderr, "[TERRAIN_SHORELINE v1] uploaded handle=%u w=%d h=%d bytes=%zu "
            "bounds=(%.1f,%.1f,%.1f,%.1f)\n",
            (unsigned)s_shorelineMaskTex, w, h, (size_t)w * (size_t)h * 4u,
            boundsTopLeftX, boundsTopLeftY, boundsSizeX, boundsSizeY);
    fflush(stderr);
}

// WATER-SHORE-EDGE-1: upload the hi-res visual heightfield as R32F GL_LINEAR /
// CLAMP_TO_EDGE (see header for the bounds/half-texel contract). Mirrors
// gos_TerrainLodChunk_UploadShorelineMask's save/restore discipline.
void gos_TerrainLodChunk_UploadWaterShoreHeight(const float* heights, int w, int h,
                                                 float boundsTopLeftX, float boundsTopLeftY,
                                                 float boundsSizeX, float boundsSizeY)
{
    if (!heights || w <= 0 || h <= 0)
        return;

    if (s_waterShoreHeightTex == 0)
        // TEX-CLASS: asset-pool -- water shore hi-res height sidecar (mission-lifetime content)
        glGenTextures(1, &s_waterShoreHeightTex);
    if (s_waterShoreHeightTex == 0)
    {
        MC2_DIAGFE(stderr, "[WATER_SHORE_EDGE v1] glGenTextures failed\n");
        fflush(stderr);
        return;
    }

    GLint prevActive = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    GLint prev2D = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev2D);

    GLint prevUnpack = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevUnpack);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    glBindTexture(GL_TEXTURE_2D, s_waterShoreHeightTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, w, h, 0, GL_RED, GL_FLOAT, heights);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glPixelStorei(GL_UNPACK_ALIGNMENT, prevUnpack);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev2D);
    glActiveTexture((GLenum)prevActive);

    ++s_waterShoreDataGen;   // VK-PARITY-WATER-SHORE-1
    s_waterShoreHeightBounds[0] = boundsTopLeftX;
    s_waterShoreHeightBounds[1] = boundsTopLeftY;
    s_waterShoreHeightBounds[2] = boundsSizeX;
    s_waterShoreHeightBounds[3] = boundsSizeY;

    MC2_DIAGFE(stderr, "[WATER_SHORE_EDGE v1] uploaded handle=%u w=%d h=%d "
            "bounds=(%.1f,%.1f,%.1f,%.1f)\n",
            (unsigned)s_waterShoreHeightTex, w, h,
            boundsTopLeftX, boundsTopLeftY, boundsSizeX, boundsSizeY);
    fflush(stderr);
}

// WATER-SHORE-EDGE-1: single engine-wide resolution of the water shore-edge
// mode (see header). Same lazy-once idiom as gos_TerrainShorelineGateOn so the
// loader (mclib/terrain.cpp) and the draw path (gameos_graphics.cpp) can never
// disagree about what mode a stock launch runs.
int gos_WaterShoreMode()
{
    static const int s_mode = []() {
        // Enum row 0|1|2|3: anything else (unset, empty, unparseable) falls through to default resolution
        if (mc2::gate::is<"MC2_WATER_SHORE_MODE", "0">()) return 0;
        if (mc2::gate::is<"MC2_WATER_SHORE_MODE", "1">()) return 1;
        if (mc2::gate::is<"MC2_WATER_SHORE_MODE", "2">()) return 2;
        if (mc2::gate::is<"MC2_WATER_SHORE_MODE", "3">()) return 3;   // GL-only smooth-shore prototype
        // Back-compat killswitch: MC2_WATER_SHORE_MASK=0 predates the mode
        // gate and meant "full legacy ramp"; keep that meaning.
        const bool shoreMaskOn = mc2::gate::get<"MC2_WATER_SHORE_MASK">();  // default ON
        return shoreMaskOn ? 2 : 0;   // default: per-fragment signed height
    }();
    return s_mode;
}

// TERRAIN-SHORELINE-V3: true whenever the elevation-based shoreline band is
// active (MC2_TERRAIN_SHORELINE gate on) -- NOT dependent on a mask sidecar
// being loaded, since V3 places the band by elevation unconditionally (the
// mask, if present, is only an optional modulator). Consumed by
// gos_postprocess.cpp to suppress the legacy screen runShoreline() pass
// (recon landmine #6 -- avoid double-brightening the seam). Name kept for
// caller-site stability; semantics widened from "mask uploaded" to "gate on".
bool gos_TerrainLodChunk_IsShorelineMaskActive()
{
    // SHORELINE-GLOW-1: was an INDEPENDENT presence test here, left behind when
    // SHORELINE-SMOOTHING-REVISIT-1 flipped the default. Now the one shared
    // accessor, so this answers the same thing the draw path does.
    return gos_TerrainShorelineGateOn();
}

// SHORELINE-GLOW-1 -- ONE resolution of the V3 shoreline gate, engine-wide.
//
// This gate used to be resolved by THREE independent getenv() reads: the
// draw-path read above, the accessor above that, and mclib/terrain.cpp's
// sidecar-load read. SHORELINE-SMOOTHING-REVISIT-1 flipped the default ON and
// updated two of them; gos_TerrainLodChunk_IsShorelineMaskActive() kept the
// pre-flip presence test `(v && v[0] && v[0] != '0')`. One unset environment
// therefore answered ON where the band is drawn and OFF where the legacy
// screen pass is suppressed, so the suppression could never fire on a stock
// launch. Resolving the gate exactly once makes that drift structurally
// impossible rather than merely fixed. Default ON; `MC2_TERRAIN_SHORELINE=0`
// turns the terrain-side band off.
bool gos_TerrainShorelineGateOn()
{
    static const bool s_on = mc2::gate::get<"MC2_TERRAIN_SHORELINE">();
    return s_on;
}

// SHORELINE-GLOW-1 -- does the V3 band actually SUPPRESS the legacy screen pass?
// LEGACY-SHORELINE-MISAIM-1 (2026-07-31) -- DEFAULT FLIPPED OFF -> ON.
//
// SHORELINE-GLOW-1 left this opt-in because it could not GRADE the visible
// change. That was the right call for a pass whose effect was unknown. It is
// no longer unknown, and the thing being graded is not shoreline foam:
//
//   shaders/shoreline.frag:22 selects its pixels with
//       isWater = (GBuffer1.a > 0.15 && GBuffer1.a < 0.35)
//   and on the default LOD-chunk renderer stack NO WATER PIXEL CAN SATISFY IT.
//   Water writes alpha 0.0 (rc_gbuffer1_screenShadowEligible, three sites in
//   gos_terrain_water_mdi.frag), terrain writes 1.0
//   (rc_gbuffer1_shadowHandled_flatUp), and the MRT1 clear is 0.0. A census of
//   every fragment shader that writes attachment 1 leaves exactly two things
//   inside (0.15, 0.35): static props at 0.25 and alpha-test lattice props at
//   0.1875. So the pass has NOT been painting foam on water. It has been
//   painting an ANIMATED fbm foam brighten (up to 1.4x multiplicative, scrolled
//   by `time` and pulsed by a sin()) onto BUILDING AND FENCE SILHOUETTES within
//   ~6 screen texels of terrain, on every default frame, on every mission.
//
// The only shader that ever wrote a continuous ~0.25 there was the LEGACY
// terrain renderer (gos_terrain.frag rc_gbuffer1_legacyTerrainMaterialAlpha),
// superseded by the LOD-chunk cutover on 2026-06-09.
//
// ---------------------------------------------------------------------------
// CORRECTION -- SHORELINE-DISPOSITION-1 (2026-07-31). READ THIS BEFORE QUOTING
// THE PARAGRAPH ABOVE.
//
// This block used to continue: "That is why the suppression is still driven
// from the LOD-chunk draw path and nowhere else: under MC2_TERRAIN_LOD_CHUNK=0
// the setter is never called, the pass keeps running, and on THAT path it still
// does the job it was written for."
//
// THAT IS FALSE, and it was load-bearing -- it was the stated reason the pass
// was kept rather than deleted. Verified at source:
//
//   * gos_terrain.frag's two GBuffer1 writes (:1103, :1156) are both inside
//     `#ifdef MRT_ENABLED`.
//   * `#define MRT_ENABLED` is injected from exactly ONE C++ site --
//     gameos_graphics.cpp:495, inside getMaterialVariation(), which feeds only
//     gosRenderMaterial::load(). The three makeProgram() variants built from
//     the same .frag (thin / surface / mask_solid) never append it, so those
//     two lines are PREPROCESSED OUT of every one of them.
//   * The single program that does get MRT_ENABLED is terrain_material_
//     (gameos_graphics.cpp:5193). It is created, pushed to materialList_, and
//     destroyed -- and never apply()'d. Its draw, terrainDrawIndexedPatches,
//     was deleted by TERRAIN-DEADCODE-DELETE-1 (75b8ebefe); the survivors at
//     :7619/:7632 rewind() and fire a LegacyMLR tripwire without drawing.
//
// So MC2_TERRAIN_LOD_CHUNK=0 is NOT the configuration this pass was written
// for -- it is not a working configuration for this pass at all. There is NO
// configuration in which the (0.15, 0.35) classifier matches a water pixel.
// `MC2_SHORELINE_LEGACY_SUPPRESS=0` does not restore shoreline foam anywhere;
// it restores the animated rim on prop silhouettes, in every configuration.
//
// Census 4 of scripts/check-render-contract-gbuffer1.sh now pins the
// single-injection-site fact so this cannot silently rot back.
//
// DISPOSITION: the pass should be DELETED, not kept and not repointed. The V3
// terrain band (MC2_TERRAIN_SHORELINE, elevation-placed in the frag) owns the
// shoreline look now, and a screen-space classifier keyed on a G-buffer alpha
// tag is the exact design that failed here. The retirement is mechanical but
// wide -- ~121 non-comment sites across gos_postprocess.{cpp,h},
// vulkan_post_screenfx_native.cpp, frame_executor.h and PipelineRegistry, plus
// two shaders, the feature registry, the launcher, doctests and the pipeline
// schema -- so it is a separate slice, not a drive-by. See the report for the
// site inventory and the pipeline-slot tombstone rationale.
//
// Cost of the flip, stated plainly because it is a real cost: prop silhouettes
// adjacent to terrain lose up to 1.4x of edge brightness while a dark-props
// report is open. What they lose with it is a shimmer that moves on a static
// building, which no grading pass would have kept.
//
// Default ON (canonical killswitch); `MC2_SHORELINE_LEGACY_SUPPRESS=0` restores
// the legacy pass exactly as it shipped.
bool gos_TerrainShorelineLegacySuppressOn()
{
    static const bool s_on = mc2::gate::get<"MC2_SHORELINE_LEGACY_SUPPRESS">();
    return s_on;
}

// Step 5c: per-vertex cement word upload (valid bit | atlas layer index). Built by
// gos_terrain_indirect after the cement catalog atlas is ready (PopulateRecipeCementWords).
void gos_TerrainLodChunk_UploadCementWordsFull(const unsigned int* words, int count, int mapSide)
{
    if (!s_cementSsbo.created() || !words || count <= 0 || mapSide <= 0)
        return;
    GLsizeiptr bytes = (GLsizeiptr)count * sizeof(unsigned int);
    s_cementSsbo.bind();
    s_cementSsbo.bufferData((intptr_t)bytes, words,
                            RenderCore::GpuBufferUsage::DynamicDraw);
    s_cementSsbo.bindBase(TERRAIN_CEMENT_SSBO_BINDING);
    s_cementSsbo.unbind();

#ifdef MC2_VULKAN_ISLAND
    // VK-P10 CEMENT/ROAD sub-slice: the cement-word field changed -> bump the snapshot
    // key so the VK terrain island re-copies it (glGetBufferSubData map-load) next frame.
    ++g_terrainCementVersion;
#endif

    // TERRAIN-LODCHUNK-SSBO-OWNER-1: register the live cement SSBO. Conditional by
    // construction -- this upload only runs when cement words actually exist, so
    // the cement slot is registered only when the buffer carries real data.
    s_cementSsbo.registerNow(static_cast<uint64_t>(bytes));
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
    if (!s_heightSsbo.created() || !rowData || mapSide <= 0)
        return;

    s_heightSsbo.bind();

    for (int row = 0; row <= quadCountY; ++row)
    {
        int        dstIdx    = (originY + row) * mapSide + originX;
        GLintptr   dstOffset = (GLintptr)dstIdx * sizeof(float);
        GLsizeiptr bytes     = (GLsizeiptr)(quadCountX + 1) * sizeof(float);
        const float* src     = rowData + row * (quadCountX + 1);
        s_heightSsbo.bufferSubData((intptr_t)dstOffset, (intptr_t)bytes, src);
    }

    s_heightSsbo.unbind();
}

// PAD-AREA-GLOW-1: chunk cement-grid geometry for the overlay/decal uploader
// (gameos_graphics.cpp uploadOverlayUniforms_). mapSide==0 means the chunk
// terrain grid is not resident (legacy terrain path / pre-upload) — the
// overlay shader then force-uploads u_padGlowMapSide=0 and never reads the
// binding-25 cement SSBO.
void gos_GetTerrainChunkGridInfo(int* mapSide, float* halfMap)
{
    if (mapSide) *mapSide = s_mapSide;
    if (halfMap) *halfMap = s_halfMap;
}

// PAD-AREA-GLOW-1: uniform night lift on cement pads + a feathered spill
// margin (MC2_PAD_AREA_GLOW_SPILL wu) onto surrounding terrain, so buildings
// with bright pool lights no longer read as pasted-bright cutouts on black
// ground. Value = additive lift strength (pre nightFactor/color). Default ON
// (0.12); =0 kill. GLOBAL scope (not the anon namespace) — the overlay/decal
// uploader in gameos_graphics.cpp reuses both values for lockstep.
float gos_TerrainPadGlowStrength()
{
    static const float s_v = mc2::gate::get<"MC2_PAD_AREA_GLOW">();
    return s_v;
}

// VK-PARITY-TERRAIN-1: shared producers for TERRAIN-AMBIENT-FLOOR-2 /
// -NIGHT-1. Moved verbatim out of the per-frame uniform upload so the GL frag
// uniforms and the VK terrain island read the SAME resolved values (one getenv
// each). Semantics unchanged: floor default 0.35 (unset), clamped 0..2, NaN->0;
// night floor default 0.0, clamped 0..1.
// VK-PARITY-F3: single reader for the snow brightness dampen (default 0.78;
// MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN overrides), shared by the GL uniform upload and the VK feed.
float gos_TerrainSnowBrightnessDampen()
{
    static const float s_v = mc2::gate::get<"MC2_TERRAIN_SNOW_BRIGHTNESS_DAMPEN">();
    return s_v;
}
float gos_TerrainAmbientFloor()
{
    // Row: default 0.35 (DEFAULT-ON), clampRange(0, 2).
    static const float s_v = mc2::gate::get<"MC2_TERRAIN_AMBIENT_FLOOR">();
    return s_v;
}
bool gos_TerrainAmbientFloorEnvSet() { return mc2::gate::isNonEmpty<"MC2_TERRAIN_AMBIENT_FLOOR">(); }
float gos_TerrainAmbientNightFloor()
{
    // Row: default 0 (DEFAULT-OFF), clampRange(0, 1).
    static const float s_v = mc2::gate::get<"MC2_TERRAIN_AMBIENT_FLOOR_NIGHT">();
    return s_v;
}
bool gos_TerrainAmbientNightFloorEnvSet() { return mc2::gate::isNonEmpty<"MC2_TERRAIN_AMBIENT_FLOOR_NIGHT">(); }
// Daylight gate actually uploaded: max(daylight level, night floor).
float gos_TerrainAmbientDayGate()
{
    const float raw = gos_GetTerrainDaylightLevel();
    const float nf  = gos_TerrainAmbientNightFloor();
    return (raw > nf) ? raw : nf;
}

// VK-PARITY-TERRAIN-2: NIGHT-LIGHT-POOLS-1 pool count actually consumed by the
// terrain frag this frame (GL u_numPoolLights / VK uT.numPoolLights). Moved verbatim
// out of the GL upload: gate MC2_TERRAIN_LIGHT_POOLS (default ON; =0 -> 0), pool
// SSBO must exist, capped at 256.
int gos_TerrainLightPoolCount()
{
    static const bool s_lightPoolsGate = mc2::gate::get<"MC2_TERRAIN_LIGHT_POOLS">();
    if (!s_lightPoolsGate) return 0;
    const GLuint   poolSsbo = gos_terrain_lighting::GetLightPoolSsbo();
    const uint32_t n        = gos_terrain_lighting::GetLightPoolLightCount();
    if (poolSsbo == 0 || n == 0) return 0;
    return static_cast<int>(n > 256u ? 256u : n);
}

// VK-PARITY-TERRAIN-2: PAD-AREA-GLOW-1 premultiplied rgb (GL u_padGlow), moved
// verbatim out of the GL upload. nightFactor = 1 - brightest mission flat-light
// channel: day missions -> exact (0,0,0).
void gos_TerrainPadGlowRGB(float out[3])
{
    float fr = 1.f, fg = 1.f, fb = 1.f;
    gos_GetTerrainMissionFlatLight(&fr, &fg, &fb);
    float flMax = fr;                     // (std::max unusable: windows.h max macro)
    if (fg > flMax) flMax = fg;
    if (fb > flMax) flMax = fb;
    float nf = 1.0f - flMax;
    if (nf < 0.0f) nf = 0.0f;
    if (nf > 1.0f) nf = 1.0f;
    const float s = gos_TerrainPadGlowStrength() * nf;
    out[0] = s * 1.00f; out[1] = s * 0.88f; out[2] = s * 0.70f;
}

// VK-PARITY-TERRAIN-2: the NIGHT-LIGHT-POOLS-2..4 / MOVER-CONTACT-SHADOW-1 knob
// constants (anonymous-namespace kTglc*) for the VK island.
void gos_TerrainLightPoolKnobs(int* soft, float* moverDim, float* moverScale,
                               int* moverMax, int* compress, float* contactShadow)
{
    if (soft)          *soft          = kTglcLightPoolSoft ? 1 : 0;
    if (moverDim)      *moverDim      = kTglcLightPoolMoverDim;
    if (moverScale)    *moverScale    = kTglcLightPoolMoverRadius;
    if (moverMax)      *moverMax      = kTglcLightPoolMoverMax ? 1 : 0;
    if (compress)      *compress      = kTglcLightPoolCompress ? 1 : 0;
    if (contactShadow) *contactShadow = kTglcMoverContactShadow;
}

// RUNWAY-APRON-TONE-2 (MC2_CEMENT_LIT): 0..1 share of the terrain lighting term
// that cement fragments obey. Default 0.0 == stock fully flat-lit cement slab.
float gos_TerrainCementLit()
{
    static const float s_v = mc2::gate::get<"MC2_CEMENT_LIT">();
    return s_v;
}

// CEMENT-WORD-RAMP-1 (MC2_CEMENT_RAMP_WORD): default OFF. When ON, the
// terrainType ramp band (0 < pureConcrete < 1, one tile wide, NO cement word of
// its own) borrows the adjacent pad's cement atlas cell as its albedo instead of
// keeping the dark baked colormap. MEASURED root cause of the user's "atrocious
// outlines": the band is an ALBEDO-source discontinuity, not a lighting one --
// every prior lighting-side lever (MC2_CEMENT_LIT, the tone-match gates)
// could not close it because base never became cement.
int gos_TerrainCementRampWord()
{
    static const int s_v = mc2::gate::get<"MC2_CEMENT_RAMP_WORD">();
    return s_v;
}

float gos_TerrainPadGlowSpillWu()
{
    static const float s_v = mc2::gate::get<"MC2_PAD_AREA_GLOW_SPILL">();
    return s_v;
}
