// LOD-chunk terrain: the program and uniform-location state, the height/type/cement SSBO state, Init and Destroy and the
// shared sampler/uniform plumbing. Draw, upload, VK feed, patch cache, material albedo and the tuning caches live in
// gos_terrain_lod_chunk_{draw,upload,feed,patch,matalbedo,gates}.cpp; what they share is gos_terrain_lod_chunk_internal.h.


#include "gos_terrain_lod_chunk_internal.h"
#include "mc2fmt.h"
#include "terrain_field_store.h"

#include <memory>

namespace {
// Lives from Init to Destroy, like the SSBOs it feeds.
std::unique_ptr<mc2terrain::TerrainFieldStore> s_fieldStore;
}

mc2terrain::TerrainFieldStore* gos_TerrainFieldStore() { return s_fieldStore.get(); }



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
    float m = (maxGen > 0) ? static_cast<float>(maxGen) : 64.0f;   // GL spec floor is 64
    float c = want < 1.0f ? 1.0f : (want > m ? m : want);
    if (c != want)
        mc2::fmt::print_err("[CLIFF_TESS 3a] tess {:.1f} clamped to GL_MAX_TESS_GEN_LEVEL {:.0f}\n", want, m);
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
                    mc2::fmt::print_err("[CLIFF_TESS 3a] WARN unmirrored float ARRAY uniform '{}' size={:d} (not handled; skipped)\n", mc2::fmt::safe(name), mc2::fmt::as_i32(static_cast<int>(size)));
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
                        char en[160]; mc2::fmt::to_buf(en, "{}[{:d}]", mc2::fmt::safe(name), mc2::fmt::as_i32(e));
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
                    mc2::fmt::print_err("[CLIFF_TESS 3a] WARN unmirrored int/sampler ARRAY uniform '{}' size={:d} type=0x{:x} (not handled; skipped)\n", mc2::fmt::safe(name), mc2::fmt::as_i32(static_cast<int>(size)), mc2::fmt::as_u32(static_cast<unsigned>(type)));
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
                mc2::fmt::print_err("[CLIFF_TESS 3a] WARN unmirrored uniform '{}' type=0x{:x}\n",
                             mc2::fmt::safe(name), mc2::fmt::as_u32(static_cast<unsigned>(type)));
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
// Shader program + uniform locations (Phase 4).
// ---------------------------------------------------------------------------

GLuint   s_terrainProgram    = 0;
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
// Phase 10 Step 5a: merged material normal sampler2DArray (own unit, no collision
// with colormap=0 / shadows=9,10). Sourced from gos_GetTerrainNormalArrayTex().
GLint    s_locMatNormalArray     = -1;
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
GLint s_locMacroVariation    = -1; // TERRAIN-MACRO-VARIATION-1
GLint s_locEdgeFeather        = -1; // TERRAIN-EDGE-FEATHER-1
GLint s_locEdgeFeatherStr     = -1;
GLint s_locVisualDisplace     = -1; // TERRAIN-VISUAL-HEIGHT-SAMPLE-1
GLint s_locVisualSide         = -1;
GLint s_locVisualDisplaceFar  = -1; // TERRAIN-VISUAL-HEIGHT-S2-ALLLOD
GLint s_locVisualDampOn       = -1; // TERRAIN-REAUTH-UNPIN-1 Half B
GLint s_locGeomorphMips       = -1; // TERRAIN-LOD-GEOMORPH-1: b26 has max-mip levels
GLint s_locMorphFactor        = -1; // TERRAIN-LOD-GEOMORPH-1: per-block parent-band lerp
GLint s_locPomScale       = -1; // TERRAIN-CHUNK-POM-1: u_pomScale
GLint s_locCameraPos      = -1; // TERRAIN-CHUNK-POM-1: "cameraPos" (Stuff/MLR eye, legacy name)
GLint s_locMatProfile     = -1;
GLint s_locControlMap     = -1;  // TERRAIN-CONTROLMAP-SAMPLE-1: u_controlMap sampler
GLint s_locUseControlMap  = -1;  // u_useControlMap gate uniform
// TERRAIN-MATERIAL-TEXTURES-1: per-layer PBR albedo array (unit 4 -- free on
// the chunk program: 0=colormap 1=overlay 2=shoreline 3=cement 5=normalArray
// 9/10=shadows 11=transitionMask 12=controlMap 13=dynFullMap).
GLint s_locMatAlbedoArray    = -1;  // u_matAlbedoArray sampler2DArray
GLint s_locUseMatAlbedo      = -1;  // u_useMatAlbedo gate uniform
GLint s_locMatAlbedoStrength = -1;  // u_matAlbedoStrength mix knob
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
GLint s_locTransitionMaskArray = -1;
GLint s_locUseTransitionMask   = -1;

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
        mc2::fmt::print_err("[TerrainLodChunk] glGenBuffers failed for height SSBO\n");
        fflush(stderr);
        return;
    }
    s_typeSsbo.create();   // Step 5b: terrainType SSBO (concrete)
    s_cementSsbo.create(); // Step 5c: cement word SSBO
    s_fieldStore = std::make_unique<mc2terrain::TerrainFieldStore>();

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
            mc2::fmt::to_buf(csmDef, "#define MC2_SHADOW_CSM 1\n#define MC2_SHADOW_CSM_MAX {:d}\n", mc2::fmt::as_i32(mc2ShadowCsmCount()));
            prefix += csmDef;
        }
        // SHADOW-DIST-FADE-1: distance-scaled dynamic-shadow penumbra + fade
        // (shadow.hglsl). Lockstep with the gameos_graphics.cpp inject sites.
        if (mc2ShadowDistFadeWu() > 0.0f) {
            char fadeDef[240];
            mc2::fmt::to_buf(fadeDef, "#define MC2_SHADOW_DIST_FADE_WU {:.1f}\n#define MC2_SHADOW_DIST_FADE_MAX_F {:.2f}\n#define MC2_SHADOW_FADE_CONT_F {:d}\n#define MC2_SHADOW_FADE_RAMP0_F {:d}\n#define MC2_SHADOW_ALT_SCALE_F {:d}\n", mc2ShadowDistFadeWu(), mc2ShadowDistFadeMax(), mc2::fmt::as_i32(mc2ShadowFadeCont() ? 1 : 0), mc2::fmt::as_i32(mc2ShadowFadeRamp0() ? 1 : 0), mc2::fmt::as_i32(mc2ShadowAltScale() ? 1 : 0));
            prefix += fadeDef;
        }
        glsl_program* prog = glsl_program::makeProgram(
            "terrain_lod_chunk",
            "shaders/terrain_lod_chunk.vert",
            "shaders/terrain_lod_chunk.frag",
            prefix.c_str());
        if (!prog || !prog->shp_)
        {
            mc2::fmt::print_err("[TerrainLodChunk] WARNING: shader compile failed -- LOD chunk draw disabled\n");
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
            s_locMacroVariation    = glGetUniformLocation(s_terrainProgram, "macroVariationStrength");
            s_locEdgeFeather       = glGetUniformLocation(s_terrainProgram, "u_edgeFeather");
            s_locEdgeFeatherStr    = glGetUniformLocation(s_terrainProgram, "u_edgeFeatherStrength");
            s_locVisualDisplace    = glGetUniformLocation(s_terrainProgram, "u_visualDisplace");
            s_locVisualSide        = glGetUniformLocation(s_terrainProgram, "u_visualSide");
            s_locVisualDisplaceFar = glGetUniformLocation(s_terrainProgram, "u_visualDisplaceFar");
            s_locVisualDampOn      = glGetUniformLocation(s_terrainProgram, "u_visualDampOn");
            s_locGeomorphMips      = glGetUniformLocation(s_terrainProgram, "u_geomorphMips");   // TERRAIN-LOD-GEOMORPH-1
            s_locMorphFactor       = glGetUniformLocation(s_terrainProgram, "u_morphFactor");    // TERRAIN-LOD-GEOMORPH-1
            s_locPomScale    = glGetUniformLocation(s_terrainProgram, "u_pomScale");
            s_locCameraPos   = glGetUniformLocation(s_terrainProgram, "cameraPos");  // TERRAIN-CHUNK-POM-1
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
            mc2::fmt::print_out("[TerrainLodChunk] shader loaded prog={:d} locs: originX={:d} originY={:d} mapSide={:d} halfMap={:d} mvp={:d} lodStep={:d} skirtDepth={:d} forceColor={:d}\n", mc2::fmt::as_u32(static_cast<unsigned>(s_terrainProgram)), mc2::fmt::as_i32(s_locBlockOriginX), mc2::fmt::as_i32(s_locBlockOriginY), mc2::fmt::as_i32(s_locMapSide), mc2::fmt::as_i32(s_locHalfMap), mc2::fmt::as_i32(s_locMvp), mc2::fmt::as_i32(s_locLodStep), mc2::fmt::as_i32(s_locSkirtDepth), mc2::fmt::as_i32(s_locForceColor));
            fflush(stdout);
            // Phase 7.5: separate startup confirmation line for easy grep.
            mc2::fmt::print_out("[TerrainLOD v1] shader program compiled OK (program={:d})\n", mc2::fmt::as_u32(s_terrainProgram));
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
                mc2::fmt::print_err("[CLIFF_TESS 3a] tess program FAILED to compile\n");
            } else {
                s_cliffTessClamped = clampTess(s_cliffTessWanted);
                s_locCliffTessLevel =
                    glGetUniformLocation(s_terrainTessProgram->shp_, "u_cliffTessLevel");
                mc2::fmt::print_err("[CLIFF_TESS 3a] tess program compiled (prog={:d} maxTess={:.1f} locTess={:d})\n", mc2::fmt::as_u32(static_cast<unsigned>(s_terrainTessProgram->shp_)), s_cliffTessClamped, mc2::fmt::as_i32(static_cast<int>(s_locCliffTessLevel)));
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
    s_fieldStore.reset();

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
