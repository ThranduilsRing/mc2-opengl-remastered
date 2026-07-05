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
#include "../../RenderCore/KtxLoader.h"  // TERRAIN-MATERIAL-TEXTURES-1: BC7 KTX2 albedo layer load
#include "gos_profiler.h"  // CLIFF-TESS-PERF: Tracy ZoneScopedN for the near-field tess mirror+draw
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>
#include <map>

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

// REDUNDANT-PASS-HUNT-1 (re-compute class): the terrain-chunk per-frame uniform
// setup in gos_TerrainLodChunk_SubmitDrawCommands used to call getenv() ~11 times
// EVERY frame to resolve mission-constant debug/tuning gates (FORCE_COLOR, DIAG,
// CEMENT_DIAG_CONNECT, SLOPE_BIAS[_STRENGTH], CLIFF_TRIPLANAR[_STRENGTH],
// MACRO_VARIATION[_STRENGTH], EDGE_FEATHER[_STRENGTH]). getenv() on Windows takes
// a CRT lock and linear-scans the process environment block; none of these vars
// can change mid-process, so resolving them per frame is pure redundant recompute.
// These resolvers cache the SAME values once at first use — byte-identical result,
// matching the established `static const ... = [](){...}()` pattern already used
// throughout this file (e.g. s_v1Env/s_v2Env). The per-frame block reads these
// instead of hitting getenv. Uniform VALUES uploaded are unchanged.
namespace {
inline bool tglc_envOn(const char* name) {
    const char* e = std::getenv(name);
    return e && e[0] && e[0] != '0';
}
inline float tglc_envStrength(const char* name, float def) {
    const char* s = std::getenv(name);
    if (!s) return def;
    float v = (float)std::atof(s);
    return (v > 0.0f) ? v : def;
}
// Resolved-once cached mirrors of the per-frame terrain-uniform env gates.
const bool  kTglcForceColor        = []() { return std::getenv("MC2_TERRAIN_LOD_CHUNK_FORCE_COLOR") != nullptr; }();
const int   kTglcDiag              = []() { const char* v = std::getenv("MC2_TERRAIN_LOD_CHUNK_DIAG"); return v ? std::atoi(v) : 0; }();
const bool  kTglcCementDiagConnect = []() { return tglc_envOn("MC2_TERRAIN_CEMENT_DIAG_CONNECT"); }();
const bool  kTglcSlopeBias         = []() { return tglc_envOn("MC2_TERRAIN_SLOPE_BIAS"); }();
const float kTglcSlopeBiasStr      = []() { return tglc_envStrength("MC2_TERRAIN_SLOPE_BIAS_STRENGTH", 1.0f); }();
const bool  kTglcCliffTriplanar    = []() { return tglc_envOn("MC2_TERRAIN_CLIFF_TRIPLANAR"); }();
const float kTglcCliffTriplanarStr = []() { return tglc_envStrength("MC2_TERRAIN_CLIFF_TRIPLANAR_STRENGTH", 1.0f); }();
// TERRAIN-CLIFF-HEIGHT-NORMAL-1: derive the cliff SHADING normal from the height
// gradient of the cooked cliff displacement (mat5 layer-5 alpha) so the rich rock
// relief actually catches light (the smooth marble rgb normal left it flat).
// Trace knob MC2_TERRAIN_CLIFF_HEIGHT_NORMAL_STRENGTH, default 2.0; only consumed
// inside the useTriplanarCliff frag block (0 -> pure rgb-normal == TRIPLANAR-1).
const float kTglcCliffHeightNormalStr = []() { return tglc_envStrength("MC2_TERRAIN_CLIFF_HEIGHT_NORMAL_STRENGTH", 2.0f); }();
// TERRAIN-CLIFF-POM-1: triplanar Parallax Occlusion Mapping on cliff faces.
// Gate MC2_TERRAIN_CLIFF_POM (default OFF -> u_cliffPom.x=0 -> frag march skipped,
// the triplanar block behaves exactly as TRIPLANAR-1). Depth (world-unit height
// scale, default 12) via MC2_TERRAIN_CLIFF_POM_DEPTH; max march steps (clamp
// 8..32, default 24) via MC2_TERRAIN_CLIFF_POM_STEPS.
const bool  kTglcCliffPom          = []() { return tglc_envOn("MC2_TERRAIN_CLIFF_POM"); }();
const float kTglcCliffPomDepth     = []() { return tglc_envStrength("MC2_TERRAIN_CLIFF_POM_DEPTH", 12.0f); }();
const float kTglcCliffPomSteps     = []() {
    float s = tglc_envStrength("MC2_TERRAIN_CLIFF_POM_STEPS", 24.0f);
    if (s < 8.0f) s = 8.0f; if (s > 32.0f) s = 32.0f;
    return s;
}();
// TERRAIN-CLIFF-DEBUG: bounded debug-viz gate (default 0/off -> u_cliffDebug=0 ->
// frag block skipped -> byte-identical). 1..4 select the debug output (see frag).
const int   kTglcCliffDebug        = []() { const char* v = std::getenv("MC2_TERRAIN_CLIFF_DEBUG"); return v ? std::atoi(v) : 0; }();
const float kTglcMacroVariation    = []() {
    if (!tglc_envOn("MC2_TERRAIN_MACRO_VARIATION")) return 0.0f;
    return tglc_envStrength("MC2_TERRAIN_MACRO_VARIATION_STRENGTH", 1.0f);
}();
const bool  kTglcEdgeFeather       = []() { return tglc_envOn("MC2_TERRAIN_EDGE_FEATHER"); }();
const float kTglcEdgeFeatherStr    = []() { return tglc_envStrength("MC2_TERRAIN_EDGE_FEATHER_STRENGTH", 1.0f); }();
// TERRAIN-MATERIAL-TEXTURES-1: per-layer PBR albedo array gate (default OFF ->
// u_useMatAlbedo uploads 0 -> frag composition verbatim -> byte-identical).
const bool  kTglcMatAlbedo         = []() { return tglc_envOn("MC2_TERRAIN_MATERIAL_TEXTURES"); }();
}  // namespace

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
static const bool s_cliffTessGate = []() {
    const char* v = std::getenv("MC2_TERRAIN_CLIFF_DISPLACE");
    return v && v[0] && v[0] != '0';
}();
static const float s_cliffTessWanted = []() {
    const char* v = std::getenv("MC2_TERRAIN_CLIFF_DISPLACE_MAXTESS");
    return (v && v[0]) ? (float)atof(v) : 4.0f;
}();
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
static glsl_program* s_terrainTessProgram = nullptr;
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
static bool  s_tessMirroredThisFrame = false;
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
static void mirrorTerrainUniforms(GLuint src, GLuint dst) {
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
static void resolveTessPatchLocs(GLuint tessProg) {
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

// mirrorTerrainPatchUniforms (targeted per-patch mirror) is defined lower, after
// the s_loc* uniform-location statics it reads (near SubmitDrawCommands).
static void mirrorTerrainPatchUniforms(GLuint src, GLuint dst);

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
// args/order/flags/slots; the raw handle is reached only via owner.glName.
// Lifetime Mission: (re-)uploaded per mission load via glBufferData.
static RenderCore::GpuBufferOwner s_heightSsbo{  // GL handle; glName 0 = not yet allocated
    RenderCore::RenderResourceId::TerrainHeightSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainHeightSsbo",
    0u};
// TERRAIN-VISUAL-HEIGHT-SSBO-OWNER-1: the 4x visual heightfield SSBO (binding 26)
// is narrowed behind a GpuBufferOwner identity record (logical id + lifetime +
// debug name + GLuint value), mirroring the height/type/cement siblings. GL calls
// happen at the same sites with the same args/order/flags/slot; the raw handle is
// reached only via owner.glName. Lifetime Mission: uploaded per mission load via
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
// TERRAIN-LOD-GEOMORPH-1: float count of the max-mip levels APPENDED to the
// binding-26 SSBO after the V*V fine bake (5 levels x mapSide^2, strides
// 2/4/5/10/20). 0 = no mips shipped -> u_geomorphMips uploads 0 -> the vert's
// coarse-band branch behaves exactly as S2 (legacy layout untouched).
static int    s_visualMipFloats  = 0;
// TERRAIN-SHORELINE-V3 (band-vs-drawn-plane probe): retained CPU copies of the
// coarse (gameplay/water-plane) heightfield and the 4x visual/displaced bake so
// a one-shot load-time instrument (MC2_TERRAIN_SHORELINE_PROBE) can print the
// delta between where the shoreline band is PLACED (v_worldPos.z, = fine bake
// under displacement) and where the water plane actually DRAWS (u_waterElevation
// on the coarse grid). Populated by the two upload entries below; observe-only.
static std::vector<float> s_coarseHeightCpu;   // mapSide*mapSide row-major
static std::vector<float> s_visualHeightCpu;   // V*V row-major (fine bake)
static bool               s_shorelineProbeDone = false;
// TERRAIN-REAUTH-UNPIN-1 Half B: coarse object-proximity displacement damp
// (binding 27). Static half = building footprints from the bake sidecar
// (visual_damp.r32), uploaded once per mission load; dynamic half = per-frame
// mover stamps min-combined on the CPU and re-uploaded (side^2 floats — tiny).
// CREATE gated upstream like the visual-height SSBO (displace gate + sidecar);
// glName 0 => never bound, u_visualDampOn stays 0 (passthrough).
static RenderCore::GpuBufferOwner s_visualDampSsbo{
    RenderCore::RenderResourceId::TerrainVisualDampSsbo,
    RenderCore::RenderResourceLifetime::Mission,
    "TerrainVisualDampSsbo",
    0u};
static int s_visualDampSide = 0;                 // == mapSide when loaded
static std::vector<float> s_visualDampStatic;    // load-time (buildings)
static std::vector<float> s_visualDampCombined;  // static + mover stamps
static bool s_visualDampHadMovers = false;       // skip redundant re-uploads
// TERRAIN-VISUAL-HEIGHT-S2-ALLLOD: per-LOD-band displaced-chunk counters, reset
// and logged once per submit alongside [TerrainLOD v1] so acceptance has hard
// per-band numbers. Index = LOD level (0..5), same mapping as [TerrainLOD v1].
static int s_visualDisplacedCounts[6] = {0, 0, 0, 0, 0, 0};
// TERRAIN-LODCHUNK-SSBO-OWNER-1: the LOD-chunk type/cement SSBOs are no longer
// bare GLuints. Each is narrowed behind a GpuBufferOwner identity record (logical
// id + lifetime + debug name + GLuint value). GL calls still happen at the same
// sites with the same args/order/flags/slots; the raw handle is reached only via
// owner.glName. Lifetime Mission: (re-)uploaded per mission load via glBufferData.
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
static GLuint s_shorelineMaskTex   = 0;
static float  s_shorelineBounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // topLeftX,topLeftY(=maxY),sizeX,sizeY

// ---------------------------------------------------------------------------
// TERRAIN-MATERIAL-TEXTURES-1: 6-layer BC7 sRGB albedo GL_TEXTURE_2D_ARRAY
// (rock/grass/dirt/concrete/snow/cliff, layer order = MAT_LAYER_* 0..5) loaded
// straight from the cooked data/terrain_layers/<channel>_albedo.ktx2 files.
// Plain GLuint, same self-contained single-owner pattern as s_controlMapTex.
// glName 0 = not loaded (gate off / load failed) -> u_useMatAlbedo uploads 0
// at draw -> frag takes the verbatim colormap-tint else-path (byte-identical).
// Built lazily at first gate-ON bind (GL context guaranteed live at draw).
// ---------------------------------------------------------------------------
static GLuint s_matAlbedoArrayTex   = 0;
static bool   s_matAlbedoLoadTried  = false;
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
static float s_matAlbedoStrengthJson = -1.0f;

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
static void tglc_EnsureMatAlbedoArrayLoaded()
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
extern float gos_GetTerrainCliffShadowFloor();  // CLIFF SHADOW FLOOR
extern float gos_GetTerrainNormalsFromHeightStrength();
extern float gos_GetTerrainPOMScale();
// TERRAIN-CHUNK-POM-1: Stuff/MLR eye position (the SAME vec4 the legacy terrain
// frag consumes as "cameraPos"); local-extern per the established accessor
// pattern in this file (ruling R5).
extern void  gos_GetTerrainCameraPos(float*, float*, float*);
extern int   g_terrainMaterialProfile;   // global; 0 = legacy
static GLint s_locLightingV1     = -1;
static GLint s_locLightingV2     = -1;
static GLint s_locCliffShadowFloor = -1;  // CLIFF SHADOW FLOOR
static GLint s_locNfhStrength    = -1;
static GLint s_locUseRockSlopeBias = -1;  // TERRAIN-SLOPE-BIAS-VISUAL-1 (B4a)
static GLint s_locRockSlopeBiasStr = -1;
static GLint s_locUseTriplanarCliff = -1; // TERRAIN-CLIFF-MATERIAL-TRIPLANAR-1
static GLint s_locCliffTriplanarStr = -1;
static GLint s_locCliffHeightNormalStr = -1; // TERRAIN-CLIFF-HEIGHT-NORMAL-1
static GLint s_locCliffPom          = -1; // TERRAIN-CLIFF-POM-1: u_cliffPom (.x=gate,.y=depth,.z=steps)
static GLint s_locCliffDebug        = -1; // TERRAIN-CLIFF-DEBUG: u_cliffDebug (0=off,1..4 debug-viz)
static GLint s_locMacroVariation    = -1; // TERRAIN-MACRO-VARIATION-1
static GLint s_locEdgeFeather        = -1; // TERRAIN-EDGE-FEATHER-1
static GLint s_locEdgeFeatherStr     = -1;
static GLint s_locVisualDisplace     = -1; // TERRAIN-VISUAL-HEIGHT-SAMPLE-1
static GLint s_locVisualSide         = -1;
static GLint s_locVisualDisplaceFar  = -1; // TERRAIN-VISUAL-HEIGHT-S2-ALLLOD
static GLint s_locVisualDampOn       = -1; // TERRAIN-REAUTH-UNPIN-1 Half B
static GLint s_locGeomorphMips       = -1; // TERRAIN-LOD-GEOMORPH-1: b26 has max-mip levels
static GLint s_locMorphFactor        = -1; // TERRAIN-LOD-GEOMORPH-1: per-block parent-band lerp
static GLint s_locPomParams      = -1;
static GLint s_locCameraPos      = -1; // TERRAIN-CHUNK-POM-1: "cameraPos" (Stuff/MLR eye, legacy name)
static GLint s_locPomView        = -1; // TERRAIN-CHUNK-POM-1: u_pomView (.x=gate, .y=near, .z=far)
static GLint s_locMatProfile     = -1;
static GLint s_locControlMap     = -1;  // TERRAIN-CONTROLMAP-SAMPLE-1: u_controlMap sampler
static GLint s_locUseControlMap  = -1;  // u_useControlMap gate uniform
// TERRAIN-MATERIAL-TEXTURES-1: per-layer PBR albedo array (unit 4 -- free on
// the chunk program: 0=colormap 1=overlay 2=shoreline 3=cement 5=normalArray
// 9/10=shadows 11=transitionMask 12=controlMap 13=dynFullMap).
static GLint s_locMatAlbedoArray    = -1;  // u_matAlbedoArray sampler2DArray
static GLint s_locUseMatAlbedo      = -1;  // u_useMatAlbedo gate uniform
static GLint s_locMatAlbedoStrength = -1;  // u_matAlbedoStrength mix knob
static constexpr GLint kChunkTexUnitMatAlbedoArray = 4;
// TERRAIN-OVERLAY-V2-PARITY-1: authored cement/pad/runway overlay sidecar.
static GLint s_locOverlaySidecar    = -1;  // u_overlaySidecar sampler
static GLint s_locUseOverlaySidecar = -1;  // u_useOverlaySidecar gate uniform
static GLint s_locOverlayBounds     = -1;  // u_overlayBounds (vec4 minX,minY,sizeX,sizeY)
// TERRAIN-SHORELINE-V3: elevation-placed wet/foam band; mask is now an
// OPTIONAL modulator (u_hasShorelineMask), not the placement source.
static GLint s_locShorelineMask     = -1;  // u_shorelineMask sampler (modulator)
static GLint s_locUseShorelineMask  = -1;  // u_useShorelineMask: 1 = elevation bands active (MC2_TERRAIN_SHORELINE gate)
static GLint s_locHasShorelineMask  = -1;  // u_hasShorelineMask: 1 = sidecar loaded, apply modulator
static GLint s_locShorelineBounds   = -1;  // u_shorelineBounds (vec4 minX,minY,sizeX,sizeY) -- modulator sample bounds
static GLint s_locWaterElevation    = -1;  // u_waterElevation (Terrain::waterElevation, world units)
static GLint s_locShorelineWetHeight  = -1;  // u_shorelineWetHeight (world units above water)
static GLint s_locShorelineFoamHeight = -1;  // u_shorelineFoamHeight (world units above water)
static GLint s_locShorelineEdgeJitter = -1;  // u_shorelineEdgeJitter (V4-STYLE: static world-XY band jitter, wu)
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
    if (s_heightSsbo.glName != 0)
        return; // idempotent

    {
        GLuint heightBuf = 0;
        glGenBuffers(1, &heightBuf);
        s_heightSsbo.glName = static_cast<uint32_t>(heightBuf);
    }
    if (s_heightSsbo.glName == 0)
    {
        fprintf(stderr, "[TerrainLodChunk] glGenBuffers failed for height SSBO\n");
        fflush(stderr);
        return;
    }
    {
        GLuint typeBuf = 0, cementBuf = 0;
        glGenBuffers(1, &typeBuf);   // Step 5b: terrainType SSBO (concrete)
        glGenBuffers(1, &cementBuf); // Step 5c: cement word SSBO
        s_typeSsbo.glName   = static_cast<uint32_t>(typeBuf);
        s_cementSsbo.glName = static_cast<uint32_t>(cementBuf);
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
            s_locCliffShadowFloor = glGetUniformLocation(s_terrainProgram, "u_terrainCliffShadowFloor");
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

    if (s_visualHeightSsbo.glName != 0)
    {
        GLuint visualBuf = static_cast<GLuint>(s_visualHeightSsbo.glName);
        glDeleteBuffers(1, &visualBuf);
        s_visualHeightSsbo.glName = 0;
        s_visualMipFloats = 0;   // TERRAIN-LOD-GEOMORPH-1

        // TERRAIN-VISUAL-HEIGHT-SSBO-OWNER-1: mark the slot unavailable on teardown.
        RenderCore::RenderResourceDesc invalid;
        invalid.id = RenderCore::RenderResourceId::TerrainVisualHeightSsbo;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }
    // TERRAIN-REAUTH-UNPIN-1 Half B: free the damp SSBO + CPU copies on teardown.
    if (s_visualDampSsbo.glName != 0)
    {
        GLuint dampBuf = static_cast<GLuint>(s_visualDampSsbo.glName);
        glDeleteBuffers(1, &dampBuf);
        s_visualDampSsbo.glName = 0;

        RenderCore::RenderResourceDesc invalid;
        invalid.id = RenderCore::RenderResourceId::TerrainVisualDampSsbo;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }
    s_visualDampSide = 0;
    s_visualDampStatic.clear();
    s_visualDampCombined.clear();
    s_visualDampHadMovers = false;
    if (s_heightSsbo.glName != 0)
    {
        GLuint heightBuf = static_cast<GLuint>(s_heightSsbo.glName);
        glDeleteBuffers(1, &heightBuf);
        s_heightSsbo.glName = 0;
        s_mapSide    = 0;
        s_halfMap    = 0.0f;

        // REGISTRY-TERRAIN-SSBO-1: mark the slot unavailable on teardown.
        RenderCore::RenderResourceDesc invalid;
        invalid.id = RenderCore::RenderResourceId::TerrainHeightSsbo;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }
    if (s_typeSsbo.glName != 0)
    {
        GLuint typeBuf = static_cast<GLuint>(s_typeSsbo.glName);
        glDeleteBuffers(1, &typeBuf);
        s_typeSsbo.glName = 0;

        // TERRAIN-LODCHUNK-SSBO-OWNER-1: mark the slot unavailable on teardown.
        RenderCore::RenderResourceDesc invalid;
        invalid.id = RenderCore::RenderResourceId::TerrainTypeSsbo;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }
    if (s_cementSsbo.glName != 0)
    {
        GLuint cementBuf = static_cast<GLuint>(s_cementSsbo.glName);
        glDeleteBuffers(1, &cementBuf);
        s_cementSsbo.glName = 0;

        // TERRAIN-LODCHUNK-SSBO-OWNER-1: mark the slot unavailable on teardown.
        RenderCore::RenderResourceDesc invalid;
        invalid.id = RenderCore::RenderResourceId::TerrainCementSsbo;
        RenderCore::registerOrUpdateRenderResource(invalid);
    }
}

// ---------------------------------------------------------------------------
// TERRAIN-SHORELINE-V3 shore-delta probe (MC2_TERRAIN_SHORELINE_PROBE).
// One-shot, load-time-cheap instrument that answers the exact question behind
// the V3 band drift: the band is placed at v_worldPos.z - u_waterElevation, but
// v_worldPos.z is the FINE VISUAL bake under displacement while the water plane
// DRAWS on the coarse grid at u_waterElevation. This walks the coarse grid, finds
// cells adjacent to the true (coarse) waterline (a corner below and a corner above
// u_waterElevation), and reports the mean/max fine-minus-coarse height offset at
// exactly those shore cells — that offset IS how far up-bank the band drifts.
// Observe-only; no state mutation; runs once per mission when the gate is set.
static void gos_TerrainLodChunk_ShorelineProbe(float waterElev)
{
    static const bool s_probeGate = []() {
        const char* v = std::getenv("MC2_TERRAIN_SHORELINE_PROBE");
        return (v && v[0] && v[0] != '0');
    }();
    if (!s_probeGate || s_shorelineProbeDone) return;
    if (s_coarseHeightCpu.empty() || s_mapSide <= 1) return;
    s_shorelineProbeDone = true;

    const int   ms   = s_mapSide;
    const bool  haveFine = (!s_visualHeightCpu.empty() && s_visualSide > 0);
    const int   V    = s_visualSide;
    auto coarseAt = [&](int cx, int cy) -> float {
        cx = cx < 0 ? 0 : (cx > ms - 1 ? ms - 1 : cx);
        cy = cy < 0 ? 0 : (cy > ms - 1 ? ms - 1 : cy);
        return s_coarseHeightCpu[(size_t)cx + (size_t)cy * ms];
    };
    // Fine bake sampled at the coarse-cell grid point (fx = cx*4), i.e. the exact
    // vertex the coarse-band displacement (u_visualDisplace==2) writes to.
    auto fineAt = [&](int cx, int cy) -> float {
        if (!haveFine) return coarseAt(cx, cy);
        int fx = cx * 4; if (fx > V - 1) fx = V - 1;
        int fy = cy * 4; if (fy > V - 1) fy = V - 1;
        return s_visualHeightCpu[(size_t)fx + (size_t)fy * V];
    };

    (void)fineAt;
    // fine sample at ANY fine (fx,fy)
    auto fineRaw = [&](int fx, int fy) -> float {
        if (!haveFine) return 0.0f;
        fx = fx < 0 ? 0 : (fx > V - 1 ? V - 1 : fx);
        fy = fy < 0 ? 0 : (fy > V - 1 ? V - 1 : fy);
        return s_visualHeightCpu[(size_t)fx + (size_t)fy * V];
    };
    // bilinear coarse surface at a fine (fx,fy) — the height space the water
    // plane's land intersection lives on (== v_worldPos.z with displace OFF).
    auto coarseBilinAtFine = [&](int fx, int fy) -> float {
        float gx = (float)fx * 0.25f, gy = (float)fy * 0.25f;
        int cx = (int)gx, cy = (int)gy;
        float tx = gx - (float)cx, ty = gy - (float)cy;
        float a = coarseAt(cx, cy),   b = coarseAt(cx + 1, cy);
        float c = coarseAt(cx, cy+1), d = coarseAt(cx + 1, cy + 1);
        return (a*(1-tx) + b*tx)*(1-ty) + (c*(1-tx) + d*tx)*ty;
    };

    double coarseZatWaterline = 0.0; long shoreCells = 0;
    float  coarseMin = 1e30f, coarseMax = -1e30f;
    // TRUE band drift: over the INTERIOR fine verts of shore quads, how far does
    // the DISPLACED surface (fineRaw) sit above/below the coarse water-plane
    // surface (coarseBilinAtFine)? Corner-pin makes this 0 AT coarse corners but
    // the reshape bows the interior — that bow is the band's up-bank drift.
    double sumInt = 0.0, maxInt = 0.0; long intCells = 0;
    double sumSignedAtWL = 0.0; long wlSamples = 0; // signed gap at fine verts near the plane
    for (int cy = 0; cy < ms - 1; ++cy)
        for (int cx = 0; cx < ms - 1; ++cx) {
            float c00 = coarseAt(cx, cy),   c10 = coarseAt(cx + 1, cy);
            float c01 = coarseAt(cx, cy+1), c11 = coarseAt(cx + 1, cy + 1);
            float lo = c00, hi = c00;
            lo = c10 < lo ? c10 : lo; hi = c10 > hi ? c10 : hi;
            lo = c01 < lo ? c01 : lo; hi = c01 > hi ? c01 : hi;
            lo = c11 < lo ? c11 : lo; hi = c11 > hi ? c11 : hi;
            if (lo < coarseMin) coarseMin = lo;
            if (hi > coarseMax) coarseMax = hi;
            if (lo <= waterElev && hi >= waterElev) {
                coarseZatWaterline += (double)c00; ++shoreCells;
                if (haveFine) {
                    for (int sy = 0; sy <= 4; ++sy)
                        for (int sx = 0; sx <= 4; ++sx) {
                            int fx = cx * 4 + sx, fy = cy * 4 + sy;
                            double cb  = (double)coarseBilinAtFine(fx, fy);
                            double gap = (double)fineRaw(fx, fy) - cb;
                            double ga  = gap < 0 ? -gap : gap;
                            sumInt += ga; if (ga > maxInt) maxInt = ga; ++intCells;
                            if (cb >= waterElev - 4.0 && cb <= waterElev + 4.0) {
                                sumSignedAtWL += gap; ++wlSamples;
                            }
                        }
                }
            }
        }
    const double meanCoarseShoreZ = shoreCells ? coarseZatWaterline / (double)shoreCells : 0.0;
    const double meanInt   = intCells ? sumInt / (double)intCells : 0.0;
    const double meanAtWL  = wlSamples ? sumSignedAtWL / (double)wlSamples : 0.0;
    fprintf(stderr,
        "[SHORELINE_PROBE v1] u_waterElevation=%.3f (drawn plane Z; band placed at "
        "v_worldPos.z-this) | coarse terrain z range=[%.2f,%.2f] | shore quads=%ld "
        "meanCoarseShoreZ=%.3f | INTERIOR band-drift(displaced-fine minus coarse-plane "
        "surface): mean=%.3fwu max=%.3fwu | signed vertical gap at fine verts on the "
        "waterline=%.3fwu (n=%ld) | fineBake=%s\n",
        (double)waterElev, (double)coarseMin, (double)coarseMax, shoreCells,
        meanCoarseShoreZ, meanInt, maxInt, meanAtWL, wlSamples,
        haveFine ? "LOADED" : "absent(no displace)");
    fflush(stderr);
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
static void mirrorTerrainPatchUniforms(GLuint src, GLuint dst) {
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
    if (s_terrainProgram == 0 || s_heightSsbo.glName == 0) return;
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
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_HEIGHT_SSBO_BINDING, static_cast<GLuint>(s_heightSsbo.glName));
    // Step 5b: terrainType SSBO (concrete). 0 if never uploaded -> vert reads 0.
    if (s_typeSsbo.glName != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_TYPE_SSBO_BINDING, static_cast<GLuint>(s_typeSsbo.glName));
    // Step 5c: cement word SSBO.
    if (s_cementSsbo.glName != 0)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_CEMENT_SSBO_BINDING, static_cast<GLuint>(s_cementSsbo.glName));

    // TERRAIN-VISUAL-HEIGHT-SAMPLE-1: resolve geometry-displacement activation once
    // per frame. Active = MC2_TERRAIN_VISUAL_DISPLACE on AND the 4x bake loaded.
    // Bind binding 26 for the whole draw; the per-chunk u_visualDisplace gates the
    // near (LOD0) band only. Default OFF -> u_visualDisplace stays 0 -> byte-identical.
    static const bool s_visualDisplaceGate = []() {
        const char* v = getenv("MC2_TERRAIN_VISUAL_DISPLACE");
        return v && v[0] && v[0] != '0';
    }();
    const bool visualDisplaceActive = s_visualDisplaceGate && s_visualHeightSsbo.glName != 0 && s_visualSide > 0;
    if (visualDisplaceActive)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_VISUAL_HEIGHT_SSBO_BINDING, static_cast<GLuint>(s_visualHeightSsbo.glName));
    if (s_locVisualSide >= 0)
        glUniform1i(s_locVisualSide, s_visualSide);
    // TERRAIN-REAUTH-UNPIN-1 Half B: near-object displacement fade. Active only
    // when displacing AND the objfade gate is on (default ON when displacing —
    // it is the safety) AND a damp map for THIS map size was uploaded. Binds
    // binding 27 for the whole draw; u_visualDampOn=0 => shader never reads it.
    static const bool s_visualDampGate = []() {
        const char* v = getenv("MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE");
        return !(v && v[0] == '0');   // default ON (unset/other => on)
    }();
    const bool visualDampActive = visualDisplaceActive && s_visualDampGate
                                  && s_visualDampSsbo.glName != 0
                                  && s_visualDampSide == s_mapSide;
    if (visualDampActive)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_VISUAL_DAMP_SSBO_BINDING, static_cast<GLuint>(s_visualDampSsbo.glName));
    if (s_locVisualDampOn >= 0)
        glUniform1i(s_locVisualDampOn, visualDampActive ? 1 : 0);
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

    // TERRAIN-LOD-GEOMORPH-1: coarse-band vertices read their OWN max-mip level
    // (silhouette keeps peaks) when the bake shipped mips. Rides the same
    // MC2_TERRAIN_VISUAL_DISPLACE gate (visualDisplaceActive); mips absent ->
    // uploads 0 -> vert branch skipped (S2 behavior verbatim).
    // MC2_TERRAIN_LOD_GEOMORPH=0 is the killswitch when mips ARE present.
    static const bool s_geomorphKill = []() {
        const char* v = getenv("MC2_TERRAIN_LOD_GEOMORPH");
        return (v && v[0] == '0');
    }();
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
            glUniform1i(s_locUseCement, (cementReady && s_cementSsbo.glName != 0) ? 1 : 0);
        // CEMENT-DIAG-CONNECT-1: env gate MC2_TERRAIN_CEMENT_DIAG_CONNECT, default OFF
        // -> uploads 0 -> frag diagonal-fill block skipped (byte-identical).
        // REDUNDANT-PASS-HUNT-1: env resolved once (kTglcCementDiagConnect).
        if (s_locCementDiagConnect >= 0)
            glUniform1i(s_locCementDiagConnect, kTglcCementDiagConnect ? 1 : 0);
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
                static const float s_envMatAlbedoStrength = []() {
                    const char* v = std::getenv("MC2_TERRAIN_MATERIAL_TEXTURES_STRENGTH");
                    if (!v || !v[0]) return -1.0f;
                    float f = (float)std::atof(v);
                    if (!(f == f)) return -1.0f;  // NaN guard
                    if (f < 0.0f) f = 0.0f;
                    if (f > 1.0f) f = 1.0f;
                    return f;
                }();
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
        static const bool s_shorelineGateOn = []() {
            const char* v = std::getenv("MC2_TERRAIN_SHORELINE");
            return (v && v[0] && v[0] != '0');
        }();
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
                gos_TerrainLodChunk_ShorelineProbe(we);  // one-shot; gated
            }
            if (s_locShaderTime >= 0)
                glUniform1f(s_locShaderTime, gos_GetShaderClockSeconds());
            // TERRAIN-SHORELINE-V3 (visual-quality pass): runtime intensity
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
            static const float s_shorelineWetHeight = []() {
                const char* v = std::getenv("MC2_TERRAIN_SHORELINE_WET_RUN");
                if (!v) v = std::getenv("MC2_TERRAIN_SHORELINE_WET_HEIGHT"); // legacy alias
                float f = v ? (float)std::atof(v) : 16.0f;
                if (!(f == f) || f <= 0.0f) f = 16.0f; // NaN/non-positive guard
                return f;
            }();
            static const float s_shorelineFoamHeight = []() {
                const char* v = std::getenv("MC2_TERRAIN_SHORELINE_FOAM_RUN");
                if (!v) v = std::getenv("MC2_TERRAIN_SHORELINE_FOAM_HEIGHT"); // legacy alias
                float f = v ? (float)std::atof(v) : 5.0f;
                if (!(f == f) || f <= 0.0f) f = 5.0f; // NaN/non-positive guard
                return f;
            }();
            if (s_locShorelineWetHeight >= 0)
                glUniform1f(s_locShorelineWetHeight, s_shorelineWetHeight);
            if (s_locShorelineFoamHeight >= 0)
                glUniform1f(s_locShorelineFoamHeight, s_shorelineFoamHeight);
            // TERRAIN-SHORELINE-V4-STYLE (zigzag fix): static world-XY jitter
            // amplitude (wu) for the band's distance-from-waterline, so the
            // wet/foam lobes stop tracing the mesh waterline's straight
            // diamond segments. 0 = exact V3 contour. Clamp [0,32] so a bad
            // env value can't scatter the band across the whole beach.
            static const float s_shorelineEdgeJitter = []() {
                const char* v = std::getenv("MC2_TERRAIN_SHORELINE_EDGE_JITTER");
                float f = v ? (float)std::atof(v) : 4.0f;
                if (!(f == f)) f = 4.0f; // NaN guard
                if (f < 0.0f) f = 0.0f; if (f > 32.0f) f = 32.0f;
                return f;
            }();
            if (s_locShorelineEdgeJitter >= 0)
                glUniform1f(s_locShorelineEdgeJitter, s_shorelineEdgeJitter);
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
        // CLIFF SHADOW FLOOR: env-gated. Unset/=0 uploads 0.0 (shader no-op,
        // byte-identical). Non-zero uploads the member value (ImGui-tunable), 0..1.
        {
            static const char* s_cliffEnv = getenv("MC2_TERRAIN_CLIFF_SHADOW_FLOOR");
            static const bool  s_cliffOn  = (s_cliffEnv && s_cliffEnv[0] && s_cliffEnv[0] != '0');
            if (s_locCliffShadowFloor >= 0) {
                float cf = s_cliffOn ? gos_GetTerrainCliffShadowFloor() : 0.0f;
                if (cf < 0.0f) cf = 0.0f;
                if (cf > 1.0f) cf = 1.0f;
                glUniform1f(s_locCliffShadowFloor, cf);
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
        // TERRAIN-EDGE-FEATHER-1: env gate MC2_TERRAIN_EDGE_FEATHER, default OFF
        // -> uploads 0 -> frag block skipped (byte-identical). Fades the terrain
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
            static const bool s_pomGate = []() {
                const char* v = getenv("MC2_TERRAIN_POM");
                return v && v[0] && v[0] != '0';
            }();
            static const float s_pomScale = []() {
                const char* v = getenv("MC2_TERRAIN_POM_SCALE");
                if (v && v[0]) { float f = (float)atof(v); if (f > 0.0f) return f; }
                return -1.0f;   // <0 = no override -> gos_GetTerrainPOMScale()
            }();
            static const float s_pomSteps = []() {
                const char* v = getenv("MC2_TERRAIN_POM_STEPS");
                if (v && v[0]) {
                    float f = (float)atof(v);
                    if (f >= 4.0f && f <= 16.0f) return f;
                }
                return 16.0f;
            }();
            static const float s_pomNear = []() {
                const char* v = getenv("MC2_TERRAIN_POM_NEAR");
                float f = (v && v[0]) ? (float)atof(v) : 1500.0f;
                if (!(f == f) || f < 0.0f) f = 1500.0f;  // NaN/negative guard
                return f;
            }();
            static const float s_pomFar = []() {
                const char* v = getenv("MC2_TERRAIN_POM_FAR");
                float f = (v && v[0]) ? (float)atof(v) : 3500.0f;
                if (!(f == f) || f <= 0.0f) f = 3500.0f;
                return f;
            }();
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

    for (int i = 0; i < count; ++i)
    {
        const TerrainDrawCommand& cmd = cmds[i];
        int qcX = cmd.quadCountsPacked & 0xFF;
        int qcY = (cmd.quadCountsPacked >> 8) & 0xFF;

        if (qcX <= 0 || qcY <= 0) continue;

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
        glBindBuffer(GL_ARRAY_BUFFER, patch.vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribIPointer(0, 2, GL_SHORT, (GLsizei)(2 * sizeof(int16_t)), (const void*)0);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, patch.ibo);

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
}

// ---------------------------------------------------------------------------
// Full heightfield upload — called once at map load.
// elevations: float[mapSide*mapSide] row-major.
// ---------------------------------------------------------------------------

void gos_TerrainLodChunk_UploadHeightFull(const float* elevations, int mapSide)
{
    if (s_heightSsbo.glName == 0)
    {
        fprintf(stderr, "[TerrainLodChunk] UploadHeightFull called before Init\n");
        fflush(stderr);
        return;
    }
    if (!elevations || mapSide <= 0)
        return;

    GLsizeiptr bytes = (GLsizeiptr)mapSide * mapSide * sizeof(float);

    const GLuint heightBuf = static_cast<GLuint>(s_heightSsbo.glName);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, heightBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, elevations, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_HEIGHT_SSBO_BINDING, heightBuf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    s_mapSide = mapSide;
    s_halfMap = (float)mapSide * 128.0f * 0.5f;

    // TERRAIN-SHORELINE-V3 probe: retain the coarse (gameplay/water-plane)
    // heightfield so the one-shot shore-delta instrument can compare it to the
    // fine bake and u_waterElevation. Cheap (one mapSide² copy at load).
    s_coarseHeightCpu.assign(elevations, elevations + (size_t)mapSide * (size_t)mapSide);
    s_shorelineProbeDone = false;

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
        d.glName    = s_heightSsbo.glName;
        d.sizeBytes = static_cast<uint64_t>(bytes);
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

#ifdef _DEBUG
    // First-frame readback verify: confirm that the GPU round-trips the first
    // float correctly. glGetBufferSubData is available on all desktop GL >=3.1.
    float firstSample = 0.0f;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_heightSsbo.glName));
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
void gos_TerrainLodChunk_UploadVisualHeightFull(const float* visualHeights, int V,
                                                const float* mipMaxes, int mipFloats)
{
    if (!visualHeights || V <= 0)
        return;
    if (s_visualHeightSsbo.glName == 0)
    {
        GLuint local = 0;
        glGenBuffers(1, &local);
        s_visualHeightSsbo.glName = static_cast<uint32_t>(local);
        if (s_visualHeightSsbo.glName == 0)
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
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_visualHeightSsbo.glName));
    glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, nullptr, GL_STATIC_DRAW);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, fineBytes, visualHeights);
    if (mipBytes > 0)
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, fineBytes, mipBytes, mipMaxes);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    s_visualSide      = V;   // remembered for the displaced draw (u_visualSide)
    s_visualMipFloats = (int)mipFloats;

    // TERRAIN-SHORELINE-V3 probe: retain the fine visual bake (the height the
    // band is placed against under displacement) for the shore-delta instrument.
    s_visualHeightCpu.assign(visualHeights, visualHeights + (size_t)V * (size_t)V);
    s_shorelineProbeDone = false;

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
        d.glName    = s_visualHeightSsbo.glName;
        d.sizeBytes = static_cast<uint64_t>(bytes);
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }
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
    if (s_visualDampSsbo.glName == 0)
    {
        GLuint local = 0;
        glGenBuffers(1, &local);
        s_visualDampSsbo.glName = static_cast<uint32_t>(local);
        if (s_visualDampSsbo.glName == 0)
        {
            fprintf(stderr, "[VISUAL_DAMP v1] glGenBuffers failed\n");
            fflush(stderr);
            return;
        }
    }
    const GLsizeiptr bytes = (GLsizeiptr)(count * sizeof(float));
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_visualDampSsbo.glName));
    glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, s_visualDampCombined.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::TerrainVisualDampSsbo;
        d.kind      = RenderCore::RenderResourceKind::Buffer;
        d.lifetime  = RenderCore::RenderResourceLifetime::Mission;
        d.format    = RenderCore::RenderResourceFormat::BufferRaw;
        d.debugName = s_visualDampSsbo.debugName;
        d.glName    = s_visualDampSsbo.glName;
        d.sizeBytes = static_cast<uint64_t>(bytes);
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }
    fprintf(stderr, "[VISUAL_DAMP v1] SSBO uploaded binding=%u side=%d bytes=%lld\n",
            TERRAIN_VISUAL_DAMP_SSBO_BINDING, side, (long long)bytes);
    fflush(stderr);
}

bool gos_TerrainLodChunk_VisualDampWanted()
{
    static const bool s_displaceGate = []() {
        const char* v = getenv("MC2_TERRAIN_VISUAL_DISPLACE");
        return v && v[0] && v[0] != '0';
    }();
    static const bool s_dampGate = []() {
        const char* v = getenv("MC2_TERRAIN_VISUAL_DISPLACE_OBJFADE");
        return !(v && v[0] == '0');
    }();
    return s_displaceGate && s_dampGate && s_visualDampSsbo.glName != 0
           && s_visualDampSide > 0 && !s_visualDampStatic.empty();
}

// Per-frame mover stamps. combined = min(static, smoothstep(inner, inner+radius,
// dist)) per mover; one full-buffer BufferSubData (side^2 floats, ~57KB@120 —
// negligible). Zero-mover frames upload only on the first one after movers
// disappear (restores the pure-static map, then no-ops).
void gos_TerrainLodChunk_UpdateVisualDampMovers(const float* cellXY, int count,
                                                float radiusCells, float innerCells)
{
    if (s_visualDampSsbo.glName == 0 || s_visualDampSide <= 0 || s_visualDampStatic.empty())
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
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_visualDampSsbo.glName));
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                    (GLsizeiptr)(s_visualDampCombined.size() * sizeof(float)),
                    s_visualDampCombined.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

// Step 5b: per-vertex terrainType upload (parallel to the heightfield). Used by
// the chunk frag's concrete material/colour selection (pureConcrete).
void gos_TerrainLodChunk_UploadTerrainTypeFull(const float* types, int mapSide)
{
    if (s_typeSsbo.glName == 0 || !types || mapSide <= 0)
        return;
    GLsizeiptr bytes = (GLsizeiptr)mapSide * mapSide * sizeof(float);
    const GLuint typeBuf = static_cast<GLuint>(s_typeSsbo.glName);
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
        d.glName    = s_typeSsbo.glName;
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

// TERRAIN-SHORELINE-V3: true whenever the elevation-based shoreline band is
// active (MC2_TERRAIN_SHORELINE gate on) -- NOT dependent on a mask sidecar
// being loaded, since V3 places the band by elevation unconditionally (the
// mask, if present, is only an optional modulator). Consumed by
// gos_postprocess.cpp to suppress the legacy screen runShoreline() pass
// (recon landmine #6 -- avoid double-brightening the seam). Name kept for
// caller-site stability; semantics widened from "mask uploaded" to "gate on".
bool gos_TerrainLodChunk_IsShorelineMaskActive()
{
    static const bool s_shorelineGateOn = []() {
        const char* v = std::getenv("MC2_TERRAIN_SHORELINE");
        return (v && v[0] && v[0] != '0');
    }();
    return s_shorelineGateOn;
}

// Step 5c: per-vertex cement word upload (valid bit | atlas layer index). Built by
// gos_terrain_indirect after the cement catalog atlas is ready (PopulateRecipeCementWords).
void gos_TerrainLodChunk_UploadCementWordsFull(const unsigned int* words, int count, int mapSide)
{
    if (s_cementSsbo.glName == 0 || !words || count <= 0 || mapSide <= 0)
        return;
    GLsizeiptr bytes = (GLsizeiptr)count * sizeof(unsigned int);
    const GLuint cementBuf = static_cast<GLuint>(s_cementSsbo.glName);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, cementBuf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, words, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, TERRAIN_CEMENT_SSBO_BINDING, cementBuf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

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
        d.glName    = s_cementSsbo.glName;
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
    if (s_heightSsbo.glName == 0 || !rowData || mapSide <= 0)
        return;

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(s_heightSsbo.glName));

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
