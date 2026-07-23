//==========================================================================//
// File:    gos_particle_bridge.cpp                                          //
// Contents: GameOS-side GL bridge for the GPU particle batcher. Owns the   //
//           SSBO (binding=14), the empty draw VAO, the billboard shader    //
//           program, and the per-group draw loop. Implements all 10 GPU-   //
//           direct bring-up traps per memory/gpu_direct_renderer_bringup_  //
//           checklist.md.                                                  //
//           FX-GPU-1 Phase 1: real per-effect texture binding.             //
//           FX-GPU-1 Phase 2: per-group UV sub-rect uniforms.              //
//===========================================================================//

#include "gos_particle_bridge.h"

#include "particles/spec.h"
#include "particles/batcher.h"  // GroupInfo

#include <gameos.hpp>
#include <GL/glew.h>
#include "utils/shader_builder.h"
#include "../../RenderCore/PipelineRegistry.h"  // VFX-APPLYPIPELINE-ROUTING-1
#include "pipeline_binder.h"                    // applyPipeline — VFX per-group blend
#include "gos_postprocess.h"  // VFX-SOFT-PARTICLES-MVP-1: scene-depth copy + invViewProj
#include "../../mclib/camera.h"  // VFX-LIT-PARTICLES-MVP-1: eye->light*/ambient* (same source as terrain)
#include "particle_stall_probe.h"  // PARTICLE-FLUSH-STALL-MEASURE-1 (gated timing)
#include "gpu_cull_readback.h"  // READBACK_SSBO_BINDING (slot 14) — single source of truth
#ifdef MC2_VULKAN_ISLAND
#include "vulkan_vfx_island.h"  // VK-FIELD-VFX-1: VK translucent particle pass
#endif

#include <cstdio>
#include <cstdlib>   // std::getenv, std::atoi (MC2_VFX_DEBUG_MODE)
#include <unordered_set>
#include <vector>    // MC2_VFX_ORACLE_TUBE: pos3->vec4 staging
#include <tracy/Tracy.hpp>               // Q2-S0 coarse CPU zones (flush paths)
#include "../../mclib/fx_trace/fx_cost_split.h"  // Q2-S0 FX cost-split + per-frame roll

// terrainMVP getter — same accessor used by gos_terrain_bridge_renderWaterFast
// at gameos_graphics.cpp:2171. C linkage upstream.
extern const float* gos_GetTerrainMVPMat4();
// B1 C14: 3-step projection chain accessors (mirror static_prop bridge at
// gos_static_prop_batcher.cpp:3010/3013). terrainMVP alone is D3D pixel-
// homog clip; the shader needs viewport + pixel->NDC remap to land in GL
// clip space.
extern const float* gos_GetProj2ScreenMat4();

// VAO rebind helper (memory/projectz_overlay_findings.md trap #4) — AMD
// silently drops draws when VAO=0; rebind to a known-non-zero VAO before
// any glDrawArrays in a bridge path.
extern void gos_RendererRebindVAO();

// P1-1: narrow GL-name resolver declared in GameOS/include/gameos.hpp and
// implemented in GameOS/gameos/gameos_graphics.cpp.  gameos.hpp is already
// included above, so this is informational only.
// unsigned int gos_GetGLTextureName(DWORD handle); — see gameos.hpp

namespace {

// B2 P1: active camera basis — set by GameCamera::render() before flush.
// Defaults to identity (right=+X, up=+Y) so particles still appear if the
// caller forgets to call gos_SetActiveCamera (produces east-up orientation,
// same as the pre-B2 fixed-axis behaviour).
float g_cam_right[3]        = {1.0f, 0.0f, 0.0f};
float g_cam_up[3]           = {0.0f, 1.0f, 0.0f};
bool  g_cam_set_this_frame  = false;

GLuint s_ssbo          = 0;   // GpuParticle SSBO at binding=14
GLsizei s_ssboCapacity = 0;   // current GL buffer-data size in records
GLuint s_vao           = 0;   // empty VAO (gl_VertexID-driven draw)
GLuint s_sampler       = 0;   // CLAMP_TO_EDGE + LINEAR
const ::glsl_program* s_prog = nullptr;

bool s_initFailed = false;

// Lazy-init env gate for verbose group/texture diagnostics.
// MC2_GOSFX_GROUP_LOG=1 enables: UV rect dump (first flush) + missing-texture errors.
// Normal runs see only the first-flush banner; set this flag for texture debug sessions.
bool s_groupLog_initialized = false;
bool s_groupLog_value       = false;
bool groupLogEnabled() {
    if (!s_groupLog_initialized) {
        const char* v = std::getenv("MC2_GOSFX_GROUP_LOG");
        s_groupLog_value       = (v && v[0] == '1');
        s_groupLog_initialized = true;
    }
    return s_groupLog_value;
}

// MC2_VFX_ORACLE_TUBE_COVERAGE=1: wrap the deferred ribbon draw in a
// GL_SAMPLES_PASSED occlusion query and log per-frame coverage + the bound
// draw-FBO. This is the DETERMINISTIC, nondeterminism-immune proof that the
// oracle tube fragments actually rasterize + pass depth into the composited
// scene FBO (cross-launch pixel-diff cannot validate combat FX -- mech-pose
// noise ~2.3% swamps it; an in-frame sample count does not depend on pose).
// samples>0 with fbo=scene target => tube pixels reach the frame. Diagnostic,
// default-OFF; the GL_QUERY_RESULT read stalls, acceptable when gated on.
bool s_tubeCoverage_initialized = false;
bool s_tubeCoverage_value       = false;
bool tubeCoverageEnabled() {
    if (!s_tubeCoverage_initialized) {
        const char* v = std::getenv("MC2_VFX_ORACLE_TUBE_COVERAGE");
        s_tubeCoverage_value       = (v && v[0] == '1');
        s_tubeCoverage_initialized = true;
    }
    return s_tubeCoverage_value;
}
GLuint s_tubeCoverageQuery = 0;  // lazily created occlusion query object

// VFX-DEBUG-VIEWS-1: particle billboard debug-mode selector.
// 0=Final (byte-identical default), 1=Albedo, 2=Alpha, 3=ParticleKind,
// 4=Overdraw proxy. Seeded once from MC2_VFX_DEBUG_MODE (clamped 0..4);
// diagnostic-only, no gameplay/emission/lifetime effect. Read-only getter
// gos_vfx_getDebugMode() surfaces the active value in the Object Inspector.
bool s_debugMode_initialized = false;
int  s_debugMode_value       = 0;
int  vfxDebugMode() {
    if (!s_debugMode_initialized) {
        const char* v = std::getenv("MC2_VFX_DEBUG_MODE");
        if (v && v[0] != '\0') {
            int m = std::atoi(v);
            if (m >= 0 && m <= 5) s_debugMode_value = m;  // 5=Age (VFX-SHADER-AGE-FADE-PARITY-1)
        }
        s_debugMode_initialized = true;
    }
    return s_debugMode_value;
}

// VFX-TUNING-UI-1: user intensity scales. All default 1.0 (= byte-identical
// no-op). Seeded once from MC2_TUNE_VFX_* (clamped 0..8); the Graphics Options
// "VFX Tuning" sliders override at runtime via the setters below. These tune
// LOOK only — no emission/lifetime/sorting/timing change.
// VFX-SHADER-AGE-FADE-PARITY-1: s_vfxAgeFade added (default 0.0 = gate OFF,
// byte-identical). 1.0 = full soft-death fade for oracle particles in final 30%
// of life. Env: MC2_TUNE_VFX_AGE_FADE (clamped 0..1).
bool  s_vfxTune_initialized   = false;
float s_vfxBrightness         = 1.0f;
float s_vfxAdditiveBrightness = 1.0f;
float s_vfxAlphaScale         = 1.0f;
float s_vfxAgeFade            = 0.0f;
static float clampVfxScale(float v) { return v < 0.0f ? 0.0f : (v > 8.0f ? 8.0f : v); }
static float clampAgeFade(float v)  { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static float envVfxScale(const char* name, float dflt) {
    const char* v = std::getenv(name);
    if (!v || v[0] == '\0') return dflt;
    return clampVfxScale((float)std::atof(v));
}
void vfxTuneInitIfNeeded() {
    if (s_vfxTune_initialized) return;
    s_vfxBrightness         = envVfxScale("MC2_TUNE_VFX_BRIGHTNESS",          1.0f);
    s_vfxAdditiveBrightness = envVfxScale("MC2_TUNE_VFX_ADDITIVE_BRIGHTNESS", 1.0f);
    s_vfxAlphaScale         = envVfxScale("MC2_TUNE_VFX_ALPHA_SCALE",         1.0f);
    {
        const char* v = std::getenv("MC2_TUNE_VFX_AGE_FADE");
        if (v && v[0] != '\0') s_vfxAgeFade = clampAgeFade((float)std::atof(v));
    }
    s_vfxTune_initialized   = true;
}

// VFX-SOFT-PARTICLES-MVP-1: depth-fade enable + world-unit fade band. Gate
// MC2_VFX_SOFT_PARTICLES (default OFF -> byte-identical). When ON, the flush
// snapshots scene depth and the FS softens alpha where alpha particles meet
// opaque geometry. Distance is ImGui-tunable (gos_vfx_setSoftDistance); enable
// is ImGui-toggleable (gos_vfx_setSoftEnabled). No emission/lifetime/timing
// effect; alpha groups only.
bool  s_soft_initialized = false;
bool  s_soft_enabled     = false;
float s_softDistance     = 30.0f;   // world-unit fade band
static float clampSoftDist(float v) { return v < 0.0f ? 0.0f : (v > 500.0f ? 500.0f : v); }
void vfxSoftInitIfNeeded() {
    if (s_soft_initialized) return;
    const char* v = std::getenv("MC2_VFX_SOFT_PARTICLES");
    s_soft_enabled       = (v && v[0] == '1');
    s_soft_initialized   = true;
}

// VFX-SCENECOLOR-GRAB-1: feedback-safe scene-COLOR copy (FRAME_RESOURCE_SUBSTRATE).
// Gate MC2_VFX_SCENECOLOR_GRAB (default OFF -> byte-identical: no copy performed,
// sceneColorCopyTex_ never allocated). When ON, the flush snapshots the resolved
// scene color (pre-VFX) into pp->sceneColorCopyTex_ via one glCopyImageSubData so
// a FUTURE distortion/refraction/soft-color slice can sample it without an FBO
// feedback loop. THIS SLICE HAS NO CONSUMER — nothing samples the copy, so even
// gate-ON is no visual change. No emission/lifetime/timing effect.
bool  s_scenecolor_initialized = false;
bool  s_scenecolor_enabled     = false;
void vfxSceneColorGrabInitIfNeeded() {
    if (s_scenecolor_initialized) return;
    const char* v = std::getenv("MC2_VFX_SCENECOLOR_GRAB");
    s_scenecolor_enabled     = (v && v[0] == '1');
    s_scenecolor_initialized = true;
}

// VFX-LIT-PARTICLES-MVP-1: scene-lit alpha smoke/dust. Gate MC2_VFX_LIT_PARTICLES
// (default OFF -> byte-identical). Strength = startup default MC2_TUNE_VFX_LIT_
// STRENGTH (clamped 0..1) / per-mission "vfxLitStrength" profile key / ImGui
// slider. When OFF the bridge uploads strength 0 so the FS lit branch is inert.
// Alpha groups only; additive flashes stay emissive.
bool  s_lit_initialized = false;
bool  s_lit_enabled     = false;
float s_litStrength     = 0.7f;   // applied only while the gate is ON
static float clampLit(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
void vfxLitInitIfNeeded() {
    if (s_lit_initialized) return;
    const char* en = std::getenv("MC2_VFX_LIT_PARTICLES");
    s_lit_enabled  = (en && en[0] == '1');
    const char* st = std::getenv("MC2_TUNE_VFX_LIT_STRENGTH");
    if (st && st[0] != '\0') s_litStrength = clampLit((float)std::atof(st));
    s_lit_initialized = true;
}

// VFX-BLACKBODY-1: blackbody (temperature->color) emissive tint for additive
// groups. Gate MC2_VFX_BLACKBODY (default OFF -> byte-identical). When OFF the
// bridge uploads u_vfxBlackbody=0 so the FS tint branch is skipped entirely.
// When ON, only additive/emissive groups get u_vfxBlackbody=1 in the draw loop;
// alpha groups are never tinted. No emission/lifetime/timing effect, no new
// per-particle data — temperature is derived in-shader from emissive brightness.
bool  s_blackbody_initialized = false;
bool  s_blackbody_enabled     = false;
void vfxBlackbodyInitIfNeeded() {
    if (s_blackbody_initialized) return;
    const char* v = std::getenv("MC2_VFX_BLACKBODY");
    s_blackbody_enabled     = (v && v[0] == '1');
    s_blackbody_initialized = true;
}

// VFX-DISTORTION-1: heat-haze refraction for the distortion alpha group. Gate
// MC2_VFX_DISTORTION (default OFF -> byte-identical; the FS u_vfxDistort branch is
// dead). REQUIRES the scene-color grab (VFX-SCENECOLOR-GRAB-1): if the grab is
// absent the bridge forces u_vfxDistort=0 so nothing samples a GL-0 texture.
// FIXTURE: MC2_VFX_DISTORT_FIXTURE (default OFF) tags every alpha group
// (blendMode==0) as a distortion group so the effect is observable on stock
// smoke/dust without any content effect->distortion mapping (out of scope). When
// the fixture is OFF, the gate is inert today (no content tags a distortion group
// yet) -> still byte-identical even with MC2_VFX_DISTORTION=1.
bool  s_distort_initialized = false;
bool  s_distort_enabled     = false;   // MC2_VFX_DISTORTION
bool  s_distort_fixture     = false;   // MC2_VFX_DISTORT_FIXTURE
float s_distortAmp          = 0.04f;   // screen-UV wobble amplitude (small)
void vfxDistortInitIfNeeded() {
    if (s_distort_initialized) return;
    const char* en = std::getenv("MC2_VFX_DISTORTION");
    s_distort_enabled = (en && en[0] == '1');
    const char* fx = std::getenv("MC2_VFX_DISTORT_FIXTURE");
    s_distort_fixture = (fx && fx[0] == '1');
    const char* amp = std::getenv("MC2_VFX_DISTORT_AMP");
    if (amp && amp[0] != '\0') {
        float a = (float)std::atof(amp);
        if (a > 0.0f && a < 0.5f) s_distortAmp = a;  // clamp to sane subtle range
    }
    s_distort_initialized = true;
}

// P0-4: Cached uniform locations — populated once in ensureInitialized()
// after the program links. -2 = not yet queried; -1 = not found (GLSL may
// strip unused uniforms); >= 0 = valid location.
GLint s_loc_worldToClipGL = -2;
GLint s_loc_mvp           = -2;
GLint s_loc_uAtlas        = -2;
// P2-1: UV sub-rect uniforms — set per draw group.
GLint s_loc_uvOffset      = -2;
GLint s_loc_uvSize        = -2;
// VFX-FLIPBOOK-ASSET-TABLE-1: atlas column count — set per draw group.
// 0 or 1 = non-animated (shader skips per-particle frame-offset path).
// >1 = animated; shader computes col=atlasIndex%columns, row=atlasIndex/columns.
GLint s_loc_atlasColumns  = -2;
// B2 P1: camera-basis uniforms — looked up once, bound per flush.
GLint s_loc_cameraRight   = -2;
GLint s_loc_cameraUp      = -2;
// VFX-DEBUG-VIEWS-1: particle debug-mode uniform.
GLint s_loc_debugMode     = -2;
// VFX-TUNING-UI-1: user intensity-scale uniforms.
GLint s_loc_vfxBrightness         = -2;
GLint s_loc_vfxAdditiveBrightness = -2;
GLint s_loc_vfxAlphaScale         = -2;
GLint s_loc_vfxIsAdditive         = -2;
// VFX-BLACKBODY-1: blackbody emissive-tint enable uniform (default 0 = OFF).
GLint s_loc_vfxBlackbody          = -2;
// VFX-DISTORTION-1: heat-haze refraction uniforms (default inert).
GLint s_loc_sceneColor      = -2;
GLint s_loc_vfxDistort      = -2;
GLint s_loc_time            = -2;
GLint s_loc_distortAmp      = -2;
// VFX-SHADER-AGE-FADE-PARITY-1: age-driven soft death fade (default 0.0 = OFF).
GLint s_loc_vfxAgeFade            = -2;
// VFX-SOFT-PARTICLES-MVP-1: soft-particle depth-fade uniforms.
GLint s_loc_uSceneDepth     = -2;
GLint s_loc_invWorldToClip  = -2;
GLint s_loc_screenSize      = -2;
GLint s_loc_softDistance    = -2;
// VFX-LIT-PARTICLES-MVP-1: scene-lighting uniforms.
GLint s_loc_vfxLitStrength  = -2;
GLint s_loc_vfxSunColor     = -2;
GLint s_loc_vfxAmbientColor = -2;

void ensureInitialized() {
    if (s_initFailed) return;
    if (s_vao != 0 && s_prog != nullptr && s_sampler != 0) {
        return;
    }

    if (s_vao == 0) {
        glGenVertexArrays(1, &s_vao);
    }
    if (s_sampler == 0) {
        // TEX-CLASS: per-pass-rebind -- particle sampler object
        glGenSamplers(1, &s_sampler);
        glSamplerParameteri(s_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(s_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    if (s_prog == nullptr) {
        // SSBO requires GL 4.3 + std430 → "#version 430\n" prefix per
        // CLAUDE.md "Shader #version" rule.
        static const char* kPrefix = "#version 430\n";
        s_prog = ::glsl_program::makeProgram(
            "particle_billboard",
            "shaders/particle_billboard.vert",
            "shaders/particle_billboard.frag",
            kPrefix);
        if (!s_prog || !s_prog->shp_) {
            s_initFailed = true;
            std::fprintf(stderr,
                         "[GPU_PARTICLES v1] event=prog_compile_fail — bridge disabled\n");
            std::fflush(stderr);
            s_prog = nullptr;
            return;
        }
        std::fprintf(stderr,
                     "[GPU_PARTICLES v1] event=prog_compiled prog=%u\n",
                     (unsigned)s_prog->shp_);
        std::fflush(stderr);

        // P0-4: cache uniform locations now that the program is linked.
        s_loc_worldToClipGL = glGetUniformLocation(s_prog->shp_, "u_worldToClipGL");
        s_loc_mvp           = glGetUniformLocation(s_prog->shp_, "u_mvp");
        s_loc_uAtlas        = glGetUniformLocation(s_prog->shp_, "uAtlas");
        // P2-1: UV sub-rect uniforms.
        s_loc_uvOffset      = glGetUniformLocation(s_prog->shp_, "u_uvOffset");
        s_loc_uvSize        = glGetUniformLocation(s_prog->shp_, "u_uvSize");
        // VFX-FLIPBOOK-ASSET-TABLE-1: atlas column count per draw group.
        s_loc_atlasColumns  = glGetUniformLocation(s_prog->shp_, "u_atlasColumns");
        // B2 P1: camera-basis uniforms.
        // A -1 is a legitimate "not in the program" result (driver stripped a
        // uniform that's unused after dead-code elim). Do NOT retry the lookup
        // every frame — that hides shader bugs and wastes GL calls.
        s_loc_cameraRight   = glGetUniformLocation(s_prog->shp_, "u_cameraRight");
        s_loc_cameraUp      = glGetUniformLocation(s_prog->shp_, "u_cameraUp");
        // VFX-DEBUG-VIEWS-1: debug-mode selector (may be -1 if mode 0 dead-code
        // elim strips it; upload is guarded on >= 0).
        s_loc_debugMode     = glGetUniformLocation(s_prog->shp_, "u_debugMode");
        // VFX-TUNING-UI-1: intensity-scale uniforms.
        s_loc_vfxBrightness         = glGetUniformLocation(s_prog->shp_, "u_vfxBrightness");
        s_loc_vfxAdditiveBrightness = glGetUniformLocation(s_prog->shp_, "u_vfxAdditiveBrightness");
        s_loc_vfxAlphaScale         = glGetUniformLocation(s_prog->shp_, "u_vfxAlphaScale");
        s_loc_vfxIsAdditive         = glGetUniformLocation(s_prog->shp_, "u_vfxIsAdditive");
        s_loc_vfxBlackbody          = glGetUniformLocation(s_prog->shp_, "u_vfxBlackbody");
        // VFX-DISTORTION-1: heat-haze uniforms (may be -1 if dead-code elim strips
        // them while MC2_VFX_DISTORTION is OFF).
        s_loc_sceneColor   = glGetUniformLocation(s_prog->shp_, "u_sceneColor");
        s_loc_vfxDistort   = glGetUniformLocation(s_prog->shp_, "u_vfxDistort");
        s_loc_time         = glGetUniformLocation(s_prog->shp_, "u_time");
        s_loc_distortAmp   = glGetUniformLocation(s_prog->shp_, "u_distortAmp");
        // VFX-SHADER-AGE-FADE-PARITY-1: age fade (VS uniform; may be -1 if
        // dead-code elim strips it when MC2_TUNE_VFX_AGE_FADE is not set).
        s_loc_vfxAgeFade            = glGetUniformLocation(s_prog->shp_, "u_vfxAgeFade");
        // VFX-SOFT-PARTICLES-MVP-1: soft-particle depth-fade uniforms (may be
        // -1 when MC2_VFX_SOFT_PARTICLES is OFF and dead-code elim strips them).
        s_loc_uSceneDepth    = glGetUniformLocation(s_prog->shp_, "u_sceneDepth");
        s_loc_invWorldToClip = glGetUniformLocation(s_prog->shp_, "u_invWorldToClip");
        s_loc_screenSize     = glGetUniformLocation(s_prog->shp_, "u_screenSize");
        s_loc_softDistance   = glGetUniformLocation(s_prog->shp_, "u_softDistance");
        // VFX-LIT-PARTICLES-MVP-1: scene-lighting uniforms (may be -1 when the
        // gate is OFF and dead-code elim strips them).
        s_loc_vfxLitStrength  = glGetUniformLocation(s_prog->shp_, "u_vfxLitStrength");
        s_loc_vfxSunColor     = glGetUniformLocation(s_prog->shp_, "u_vfxSunColor");
        s_loc_vfxAmbientColor = glGetUniformLocation(s_prog->shp_, "u_vfxAmbientColor");
        if (s_loc_cameraRight < 0 || s_loc_cameraUp < 0) {
            if (groupLogEnabled())
                std::fprintf(stderr, "[B2] gos_particle_bridge: uniform locations missing — right=%d up=%d\n",
                             s_loc_cameraRight, s_loc_cameraUp);
        }
    }
}

void ensureSsboCapacity(GLsizei needRecords) {
    if (s_ssbo == 0) {
        // TIER2-EXCLUDED: substrate-gated
        glGenBuffers(1, &s_ssbo);
    }
    if (needRecords > s_ssboCapacity) {
        // Grow with headroom; Stage 1' budget peaks at 4096 (batcher
        // default). Reallocate via glBufferData (no orphan-on-each-frame).
        GLsizei newCap = (needRecords < 1024) ? 1024 : needRecords;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)(newCap * sizeof(mc2::particles::GpuParticle)),
                     nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        s_ssboCapacity = newCap;
    }
}

}  // namespace

// ── MC2_VFX_ORACLE_TUBE slice 1: ribbon mesh bridge state ─────────────────
// Separate program + 3 SSBOs (pos/color/uv) + element buffer. Shares NOTHING
// with the billboard path above; the ribbon is an indexed swept-quad mesh, not
// gl_VertexID-expanded cards.
namespace {

GLuint s_tubePosSsbo   = 0;   // binding=14: vec4 worldpos
GLuint s_tubeColSsbo   = 0;   // binding=15: vec4 rgba
GLuint s_tubeUvSsbo    = 0;   // binding=16: vec2 uv
GLuint s_tubeIbo       = 0;   // GL_ELEMENT_ARRAY_BUFFER (unsigned short)
GLuint s_tubeVao       = 0;   // VAO owning the IBO binding
GLuint s_tubeSampler   = 0;   // CLAMP_TO_EDGE + LINEAR
const ::glsl_program* s_tubeProg = nullptr;
bool s_tubeInitFailed  = false;

GLint s_tloc_worldToClipGL = -2;
GLint s_tloc_uAtlas        = -2;
GLint s_tloc_uAdditive     = -2;  // blend-aware discard: 1=additive (PPC), 0=alpha ribbon

GLsizei s_tubePosCap = 0, s_tubeColCap = 0, s_tubeUvCap = 0, s_tubeIdxCap = 0;

void tubeEnsureInitialized() {
    if (s_tubeInitFailed) return;
    if (s_tubeProg && s_tubeVao && s_tubeSampler) return;

    if (s_tubeVao == 0) glGenVertexArrays(1, &s_tubeVao);
    if (s_tubeSampler == 0) {
        // TEX-CLASS: per-pass-rebind -- particle-tube sampler object
        glGenSamplers(1, &s_tubeSampler);
        glSamplerParameteri(s_tubeSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_tubeSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(s_tubeSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(s_tubeSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    if (s_tubeProg == nullptr) {
        static const char* kPrefix = "#version 430\n";
        s_tubeProg = ::glsl_program::makeProgram(
            "tube_ribbon",
            "shaders/tube_ribbon.vert",
            "shaders/tube_ribbon.frag",
            kPrefix);
        if (!s_tubeProg || !s_tubeProg->shp_) {
            s_tubeInitFailed = true;
            std::fprintf(stderr,
                "[VFX_ORACLE_TUBE v1] event=prog_compile_fail — ribbon bridge disabled\n");
            std::fflush(stderr);
            s_tubeProg = nullptr;
            return;
        }
        std::fprintf(stderr,
            "[VFX_ORACLE_TUBE v1] event=prog_compiled prog=%u\n",
            (unsigned)s_tubeProg->shp_);
        std::fflush(stderr);
        s_tloc_worldToClipGL = glGetUniformLocation(s_tubeProg->shp_, "u_worldToClipGL");
        s_tloc_uAtlas        = glGetUniformLocation(s_tubeProg->shp_, "uAtlas");
        s_tloc_uAdditive     = glGetUniformLocation(s_tubeProg->shp_, "uAdditive");
    }
}

void tubeEnsureBuffer(GLuint& buf, GLsizei& cap, GLenum target,
                      GLsizei needBytes, GLenum binding /*0=none*/) {
    if (buf == 0) glGenBuffers(1, &buf);
    if (needBytes > cap) {
        GLsizei newCap = (needBytes < 4096) ? 4096 : needBytes;
        glBindBuffer(target, buf);
        glBufferData(target, (GLsizeiptr)newCap, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(target, 0);
        cap = newCap;
    }
    (void)binding;
}

// ── VK-OPT-1: tube-ribbon upload instrumentation ──────────────────────────
// Automatable redundancy detection for the class-D tube upload path. Counts
// bytes pushed to the GPU + heap allocations per frame so a regression that
// re-introduces per-record scratch allocs or a fatter vertex format is
// measurable (and catchable in a future gate). Behind MC2_VFX_TUBE_UPLOAD_STATS
// so it is zero-cost when off. Frame totals are dumped from the deferred flush.
struct TubeUploadStats {
    unsigned long long uploadBytes   = 0;  // total glBufferSubData payload bytes this frame (opt)
    unsigned long long baselineBytes = 0;  // what the pre-opt vec4-padded format would upload
    unsigned long long heapAllocs    = 0;  // per-record scratch allocations this frame (0 with opt)
    unsigned int       records       = 0;  // ribbon records flushed this frame
    unsigned int       posVerts      = 0;  // total position verts uploaded (for the 12B vs 16B split)
};
TubeUploadStats s_tubeStats;

bool tubeUploadStatsEnabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = std::getenv("MC2_VFX_TUBE_UPLOAD_STATS");
        cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return cached != 0;
}

// Orphan-then-upload: rename the buffer's storage (glBufferData NULL) before
// writing, so the driver does NOT implicit-sync-stall waiting for the previous
// frame's draw to finish reading the same buffer (class-D fix). The orphan
// reuses the buffer's existing capacity (`cap`), so it is a cheap rename, not a
// reallocation. Reusable template for the other class-D buffers (#4/#13).
void tubeOrphanUpload(GLuint buf, GLsizei cap, GLenum target,
                      GLsizeiptr bytes, const void* data) {
    glBindBuffer(target, buf);
    glBufferData(target, (GLsizeiptr)cap, nullptr, GL_DYNAMIC_DRAW);  // orphan
    glBufferSubData(target, 0, bytes, data);
    if (tubeUploadStatsEnabled()) s_tubeStats.uploadBytes += (unsigned long long)bytes;
}

}  // namespace (tube ribbon)

extern "C" void gos_tube_ribbon_flush(const float*          positions,
                                      const float*          colors,
                                      const float*          uvs,
                                      unsigned int          numVerts,
                                      const unsigned short* indices,
                                      unsigned int          numIndices,
                                      unsigned int          gosHandle,
                                      int                   blendMode) {
    mc2::ScopedFlushTimer _pfst(1, "tube");  // PARTICLE-FLUSH-STALL-MEASURE-1
    if (numVerts == 0 || numIndices == 0 ||
        positions == nullptr || colors == nullptr ||
        uvs == nullptr || indices == nullptr) return;

    tubeEnsureInitialized();
    if (s_tubeInitFailed || s_tubeProg == nullptr || s_tubeProg->shp_ == 0) {
        static bool s_failLogged = false;
        if (!s_failLogged) {
            s_failLogged = true;
            std::fprintf(stderr, "[VFX_ORACLE_TUBE v1] ERROR ribbon_init_failed\n");
            std::fflush(stderr);
        }
        return;
    }

    // Resolve the gos texture handle to a GL name; skip if not resident.
    const GLuint glTex = (gosHandle != 0) ? (GLuint)gos_GetGLTextureName(gosHandle) : 0u;
    if (glTex == 0) {
        if (groupLogEnabled()) {
            static std::unordered_set<uint32_t> s_loggedMissing;
            if (s_loggedMissing.insert(gosHandle).second) {
                std::fprintf(stderr,
                    "[VFX_ORACLE_TUBE v1] skip missing_texture handle=%u\n", gosHandle);
                std::fflush(stderr);
            }
        }
        return;
    }

    // First-flush banner.
    {
        static bool s_banner = false;
        if (!s_banner) {
            s_banner = true;
            std::fprintf(stderr,
                "[VFX_ORACLE_TUBE v1] enabled=1 verts=%u indices=%u tex=%u blend=%s\n",
                numVerts, numIndices, gosHandle,
                blendMode == 1 ? "additive" : "alpha");
            std::fflush(stderr);
        }
    }

    // ── State save (mirror gos_particle_bridge_flush) ──────────────────
    GLint savedProgram   = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram);
    GLint savedVAO       = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVAO);
    GLint savedSrcRGB    = 0; glGetIntegerv(GL_BLEND_SRC_RGB, &savedSrcRGB);
    GLint savedDstRGB    = 0; glGetIntegerv(GL_BLEND_DST_RGB, &savedDstRGB);
    GLboolean savedBlend = glIsEnabled(GL_BLEND);
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint savedDepthFunc = 0; glGetIntegerv(GL_DEPTH_FUNC, &savedDepthFunc);
    GLint savedDepthMask = 0; glGetIntegerv(GL_DEPTH_WRITEMASK, &savedDepthMask);
    GLint savedSampler   = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &savedSampler);
    GLint savedActiveTex = 0; glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTex);
    GLint savedTex2D0    = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex2D0);
    GLboolean savedCullFace = glIsEnabled(GL_CULL_FACE);

    // ── Bind VAO + IBO (trap #4: AMD drops draws on VAO=0) ─────────────
    glBindVertexArray(s_tubeVao);

    // ── Upload mesh into the 3 SSBOs + the element buffer ──────────────
    // VK-OPT-1: tight float[] position layout (see deferred path) — keeps this
    // dead-but-live-shader path consistent with the shared tube_ribbon.vert.
    tubeEnsureBuffer(s_tubePosSsbo, s_tubePosCap, GL_SHADER_STORAGE_BUFFER,
                     (GLsizei)(numVerts * 3u * sizeof(float)), 0);
    tubeEnsureBuffer(s_tubeColSsbo, s_tubeColCap, GL_SHADER_STORAGE_BUFFER,
                     (GLsizei)(numVerts * 4u * sizeof(float)), 0);
    tubeEnsureBuffer(s_tubeUvSsbo,  s_tubeUvCap,  GL_SHADER_STORAGE_BUFFER,
                     (GLsizei)(numVerts * 2u * sizeof(float)), 0);
    tubeEnsureBuffer(s_tubeIbo,     s_tubeIdxCap, GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizei)(numIndices * sizeof(unsigned short)), 0);

    // Orphan-then-upload native data (no expansion / no scratch alloc).
    tubeOrphanUpload(s_tubePosSsbo, s_tubePosCap, GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)(numVerts * 3u * sizeof(float)), positions);
    tubeOrphanUpload(s_tubeColSsbo, s_tubeColCap, GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)(numVerts * 4u * sizeof(float)), colors);
    tubeOrphanUpload(s_tubeUvSsbo,  s_tubeUvCap,  GL_SHADER_STORAGE_BUFFER,
                     (GLsizeiptr)(numVerts * 2u * sizeof(float)), uvs);
    tubeOrphanUpload(s_tubeIbo,     s_tubeIdxCap, GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizeiptr)(numIndices * sizeof(unsigned short)), indices);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 14, s_tubePosSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, s_tubeColSsbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 16, s_tubeUvSsbo);

    // ── Program + uniforms ─────────────────────────────────────────────
    glUseProgram(s_tubeProg->shp_);
    {
        const float* mvp = gos_GetTerrainMVPMat4();  // row-major direct (GL_FALSE)
        if (mvp && s_tloc_worldToClipGL >= 0)
            glUniformMatrix4fv(s_tloc_worldToClipGL, 1, GL_FALSE, mvp);
    }
    if (s_tloc_uAtlas >= 0) glUniform1i(s_tloc_uAtlas, 0);
    // Blend-aware fragment discard: additive (PPC/ER-PPC) ribbons carry low/zero
    // per-vertex alpha (additive uses RGB, not alpha coverage), so the alpha
    // discard written for alpha ribbons must NOT apply to them. blendMode 1 =
    // additive (GL_SRC_ALPHA,GL_ONE below), 0 = alpha.
    if (s_tloc_uAdditive >= 0) glUniform1i(s_tloc_uAdditive, blendMode == 1 ? 1 : 0);

    // ── Texture + sampler on unit 0 ────────────────────────────────────
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, glTex);
    glBindSampler(0, s_tubeSampler);

    // ── Depth + blend (depth-test ON, depth-write OFF; alpha blend) ────
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);  // reverse-Z convention
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    if (blendMode == 1) glBlendFunc(GL_ONE, GL_ONE);               // pure additive = legacy OneOneMode parity (dead path; deferred is live)
    else                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // Ribbon is a swept double-sided strip; disable face culling so it shows
    // from both sides regardless of profile winding.
    glDisable(GL_CULL_FACE);

    glDrawElements(GL_TRIANGLES, (GLsizei)numIndices, GL_UNSIGNED_SHORT, (const void*)0);

    // ── Unbind SSBO slots (mirror billboard flush hygiene) ─────────────
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 14, 0u);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, 0u);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 16, 0u);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    // ── Restore state ──────────────────────────────────────────────────
    if (savedCullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex2D0);
    glBindSampler(0, (GLuint)savedSampler);
    if (savedActiveTex != GL_TEXTURE0) glActiveTexture((GLenum)savedActiveTex);
    glDepthMask((GLboolean)savedDepthMask);
    glDepthFunc((GLenum)savedDepthFunc);
    if (!savedDepthTest) glDisable(GL_DEPTH_TEST);
    glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
    if (!savedBlend) glDisable(GL_BLEND);
    glUseProgram((GLuint)savedProgram);
    glBindVertexArray((GLuint)savedVAO);

    extern void __stdcall gos_InvalidateRenderStateCache();
    gos_InvalidateRenderStateCache();
}

// ── TUBE-DEFERRED-FLUSH-1: per-frame ribbon queue ─────────────────────────
// Records are deep-copied at enqueue time (mclib pointers are transient).
// Positions are already in MC2 WORLD space (local_to_world applied by caller).
// The queue is drained once per frame from gos_tube_ribbon_flush_deferred().
namespace {

struct RibbonRecord {
    std::vector<float>          positions;   // numVerts * 3 floats, MC2 world space
    std::vector<float>          colors;      // numVerts * 4 floats RGBA
    std::vector<float>          uvs;         // numVerts * 2 floats
    std::vector<unsigned short> indices;     // numIndices uint16
    unsigned int                gosHandle;
    int                         blendMode;   // 0=alpha, 1=additive
};

std::vector<RibbonRecord> s_ribbonQueue;

}  // namespace (tube deferred queue)

extern "C" void gos_tube_ribbon_enqueue(const float*          positions,
                                        const float*          colors,
                                        const float*          uvs,
                                        unsigned int          numVerts,
                                        const unsigned short* indices,
                                        unsigned int          numIndices,
                                        unsigned int          gosHandle,
                                        int                   blendMode) {
    // gosHandle==0 must NOT be enqueued: untextured ribbons belong to the legacy
    // MLR path (DrawEffect via the MLR sorter, which is correctly post-renderLists).
    // The caller leaves ribbonSubmitted=false in that case so DrawEffect runs.
    if (gosHandle == 0) return;
    if (numVerts == 0 || numIndices == 0 ||
        positions == nullptr || colors == nullptr ||
        uvs == nullptr || indices == nullptr) return;

    RibbonRecord rec;
    rec.gosHandle  = gosHandle;
    rec.blendMode  = blendMode;
    rec.positions.assign(positions, positions + (size_t)numVerts * 3u);
    rec.colors.assign   (colors,    colors    + (size_t)numVerts * 4u);
    rec.uvs.assign      (uvs,       uvs       + (size_t)numVerts * 2u);
    rec.indices.assign  (indices,   indices   + (size_t)numIndices);
    s_ribbonQueue.push_back(std::move(rec));
}

extern "C" void gos_tube_ribbon_flush_deferred(void) {
    // Q2-S0: this drain is called once per WALL frame from the unconditional
    // particlesFlush phase (code/gamecam.cpp ~454), so it is the per-frame tick
    // for the [FX_COST v1] summary. Tick BEFORE the empty-queue early-return so
    // the 600-frame denominator counts wall frames, not just tube-active ones.
    mc2::fx_cost_split::roll_frame_and_maybe_emit();

    if (s_ribbonQueue.empty()) return;
    mc2::ScopedFlushTimer _pfst(2, "tube_deferred");  // PARTICLE-FLUSH-STALL-MEASURE-1

    tubeEnsureInitialized();
    if (s_tubeInitFailed || s_tubeProg == nullptr || s_tubeProg->shp_ == 0) {
        s_ribbonQueue.clear();
        return;
    }

    // Q2-S0 instrumentation only (no behavior change). Coarse zone + bucket for
    // the per-frame ribbon SSBO upload + draw. Placed after the guards so empty
    // frames are not instrumented.
    ZoneScopedN("gos.TubeRibbon.FlushDeferred");
    mc2::fx_cost_split::Scope _fxcs(mc2::fx_cost_split::B_TUBE_BRIDGE_FLUSH);

    // VK-OPT-1: reset per-frame upload counters (heapAllocs is 0 by construction
    // now — the per-record scratch vector is gone; the field stays so a future
    // regression that re-adds a scratch alloc shows up non-zero).
    if (tubeUploadStatsEnabled()) s_tubeStats = TubeUploadStats{};

    // ── State save (same set as the immediate flush) ───────────────────
    GLint savedProgram   = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram);
    GLint savedVAO       = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVAO);
    GLint savedSrcRGB    = 0; glGetIntegerv(GL_BLEND_SRC_RGB, &savedSrcRGB);
    GLint savedDstRGB    = 0; glGetIntegerv(GL_BLEND_DST_RGB, &savedDstRGB);
    GLboolean savedBlend    = glIsEnabled(GL_BLEND);
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint savedDepthFunc = 0; glGetIntegerv(GL_DEPTH_FUNC, &savedDepthFunc);
    GLint savedDepthMask = 0; glGetIntegerv(GL_DEPTH_WRITEMASK, &savedDepthMask);
    GLint savedSampler   = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &savedSampler);
    GLint savedActiveTex = 0; glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTex);
    GLint savedTex2D0    = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex2D0);
    GLboolean savedCullFace = glIsEnabled(GL_CULL_FACE);

    // ── Bind VAO (trap #4: AMD drops draws on VAO=0) ───────────────────
    glBindVertexArray(s_tubeVao);

    // ── Program + shared uniforms (set ONCE for the whole batch) ──────
    glUseProgram(s_tubeProg->shp_);
    {
        const float* mvp = gos_GetTerrainMVPMat4();
        if (mvp && s_tloc_worldToClipGL >= 0)
            glUniformMatrix4fv(s_tloc_worldToClipGL, 1, GL_FALSE, mvp);
    }
    if (s_tloc_uAtlas >= 0) glUniform1i(s_tloc_uAtlas, 0);

    // ── Shared depth + blend state; per-record blend overridden in loop ─
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);  // reverse-Z convention
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glDisable(GL_CULL_FACE); // ribbon is double-sided

    glBindSampler(0, s_tubeSampler);

    // MRT FIX (root cause of "tubes draw but invisible"): the scene FBO has
    // multiple active draw buffers (color + normal + R32UI objectId — the 3rd
    // attachment landed with the RenderWorld arc, which is exactly when the
    // oracle broke). tube_ribbon.frag writes only location 0; with GL_BLEND
    // enabled AND an integer (R32UI) attachment in the active draw-buffer list,
    // AMD suppresses the COLOR0 write -> ribbons invisible. Probe confirmed
    // drawBufs=3 at draw time. Force a single COLOR_ATTACHMENT0 draw buffer for
    // the ribbon draws (tubes are transparent VFX — they must not write normal
    // or objectId anyway), restore the full MRT list after.
    GLenum savedDrawBufs[8];
    int    nSavedDrawBufs = 0;
    for (int i = 0; i < 8; ++i) {
        GLint d = GL_NONE; glGetIntegerv(GL_DRAW_BUFFER0 + i, &d);
        savedDrawBufs[i] = (GLenum)d;
        if (d != GL_NONE) nSavedDrawBufs = i + 1;
    }
    {
        const GLenum single[1] = { GL_COLOR_ATTACHMENT0 };
        glDrawBuffers(1, single);
    }

    // First-deferred-flush banner (once).
    {
        static bool s_banner = false;
        if (!s_banner) {
            s_banner = true;
            std::fprintf(stderr,
                "[VFX_ORACLE_TUBE deferred] enabled=1 queue=%u\n",
                (unsigned)s_ribbonQueue.size());
            std::fflush(stderr);
        }
    }

    // MC2_VFX_ORACLE_TUBE_COVERAGE: begin an occlusion query over the ribbon
    // draws + capture the bound draw-FBO (scene target). samples>0 with a
    // non-default fbo proves tube fragments rasterize + pass depth into the
    // composited scene FBO -- deterministic, immune to mech-pose nondeterminism.
    const bool covOn  = tubeCoverageEnabled();
    GLint      covFbo = 0;
    if (covOn) {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &covFbo);
        if (s_tubeCoverageQuery == 0) glGenQueries(1, &s_tubeCoverageQuery);
        glBeginQuery(GL_SAMPLES_PASSED, s_tubeCoverageQuery);
    }

    // ── Per-record draw loop ───────────────────────────────────────────
    for (const RibbonRecord& rec : s_ribbonQueue) {
        const unsigned numVerts   = (unsigned)(rec.positions.size() / 3u);
        const unsigned numIndices = (unsigned)(rec.indices.size());
        if (numVerts == 0 || numIndices == 0) continue;

        // Resolve texture; skip records whose handle won't resolve at draw time
        // (rare edge — dominant untextured gosHandle==0 case already blocked at
        // enqueue; non-zero handle that vanished is acceptable to skip).
        const GLuint glTex = (GLuint)gos_GetGLTextureName(rec.gosHandle);
        if (glTex == 0) continue;

        // Ensure SSBOs and IBO are large enough, then upload. VK-OPT-1: the
        // position SSBO is now a TIGHT float[] (3 floats/vert), not a padded
        // vec4[] — no CPU expansion loop, no per-record scratch heap alloc, and
        // 25% fewer position bytes. The w=1 expansion moved into the VS.
        tubeEnsureBuffer(s_tubePosSsbo, s_tubePosCap, GL_SHADER_STORAGE_BUFFER,
                         (GLsizei)(numVerts * 3u * sizeof(float)), 0);
        tubeEnsureBuffer(s_tubeColSsbo, s_tubeColCap, GL_SHADER_STORAGE_BUFFER,
                         (GLsizei)(numVerts * 4u * sizeof(float)), 0);
        tubeEnsureBuffer(s_tubeUvSsbo,  s_tubeUvCap,  GL_SHADER_STORAGE_BUFFER,
                         (GLsizei)(numVerts * 2u * sizeof(float)), 0);
        tubeEnsureBuffer(s_tubeIbo,     s_tubeIdxCap, GL_ELEMENT_ARRAY_BUFFER,
                         (GLsizei)(numIndices * sizeof(unsigned short)), 0);

        // Class-D fix: orphan-then-upload the native record data (no expansion,
        // no scratch alloc) so the driver renames storage instead of
        // implicit-sync-stalling on the previous frame's read.
        tubeOrphanUpload(s_tubePosSsbo, s_tubePosCap, GL_SHADER_STORAGE_BUFFER,
                         (GLsizeiptr)(numVerts * 3u * sizeof(float)), rec.positions.data());
        tubeOrphanUpload(s_tubeColSsbo, s_tubeColCap, GL_SHADER_STORAGE_BUFFER,
                         (GLsizeiptr)(numVerts * 4u * sizeof(float)), rec.colors.data());
        tubeOrphanUpload(s_tubeUvSsbo,  s_tubeUvCap,  GL_SHADER_STORAGE_BUFFER,
                         (GLsizeiptr)(numVerts * 2u * sizeof(float)), rec.uvs.data());
        tubeOrphanUpload(s_tubeIbo,     s_tubeIdxCap, GL_ELEMENT_ARRAY_BUFFER,
                         (GLsizeiptr)(numIndices * sizeof(unsigned short)), rec.indices.data());
        if (tubeUploadStatsEnabled()) {
            ++s_tubeStats.records;
            s_tubeStats.posVerts += numVerts;
        }

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 14, s_tubePosSsbo);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, s_tubeColSsbo);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 16, s_tubeUvSsbo);

        // Blend-aware fragment discard uniform (per-record, matches immediate path).
        if (s_tloc_uAdditive >= 0)
            glUniform1i(s_tloc_uAdditive, rec.blendMode == 1 ? 1 : 0);

        // Per-record blend func (alpha vs additive). Additive PPC/ER-PPC Tubes
        // are legacy MLRState::OneOneMode = pure additive GL_ONE,GL_ONE. Using
        // GL_SRC_ALPHA,GL_ONE double-attenuated the bolt (frag already folds
        // alpha into RGB via tex*v_color, then SRC_ALPHA scales by the small
        // age-ramped per-vertex alpha again) -> bright core + faint ghost ribbon
        // = the "extra tube / partial transparency" artifact. Match legacy.
        // VFX-APPLYPIPELINE-ROUTING-1: per-record blend via VfxTube row
        // (additive = AdditiveOneOne = ONE/ONE; alpha = SRC_ALPHA/ONE_MINUS_SRC_ALPHA).
        // Same GL state as the old hand-set; program(0)=skip keeps the tube program.
        pipeline_binder::applyPipeline(
            RenderCore::getPipelineDesc(rec.blendMode == 1
                ? RenderCore::PipelineId::VfxTubeAdditive
                : RenderCore::PipelineId::VfxTubeAlpha),
            rec.blendMode == 1 ? "VfxTubeAdditive" : "VfxTubeAlpha");

        glBindTexture(GL_TEXTURE_2D, glTex);
        glDrawElements(GL_TRIANGLES, (GLsizei)numIndices, GL_UNSIGNED_SHORT, (const void*)0);
    }

    // MC2_VFX_ORACLE_TUBE_COVERAGE: end the query, read the sample count (this
    // stalls -- acceptable for a gated diagnostic) and log per-frame coverage.
    // sceneDrawBufs = the scene FBO's MRT attachment count (3 with the
    // RenderWorld arc R32UI objectId attachment) -- confirms the target is the
    // composited scene FBO, not a scratch target.
    if (covOn) {
        glEndQuery(GL_SAMPLES_PASSED);
        GLuint covSamples = 0;
        glGetQueryObjectuiv(s_tubeCoverageQuery, GL_QUERY_RESULT, &covSamples);
        std::fprintf(stderr,
            "[VFX_ORACLE_TUBE coverage] ribbons=%u samples=%u fbo=%d sceneDrawBufs=%d\n",
            (unsigned)s_ribbonQueue.size(), covSamples, (int)covFbo, nSavedDrawBufs);
        std::fflush(stderr);
    }

    // ── Unbind SSBO/IBO slots ──────────────────────────────────────────
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 14, 0u);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 15, 0u);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 16, 0u);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    // MRT FIX: restore the scene's full MRT draw-buffer list.
    if (nSavedDrawBufs > 0) glDrawBuffers(nSavedDrawBufs, savedDrawBufs);

    // ── Restore state ──────────────────────────────────────────────────
    if (savedCullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex2D0);
    glBindSampler(0, (GLuint)savedSampler);
    if (savedActiveTex != GL_TEXTURE0) glActiveTexture((GLenum)savedActiveTex);
    glDepthMask((GLboolean)savedDepthMask);
    glDepthFunc((GLenum)savedDepthFunc);
    if (!savedDepthTest) glDisable(GL_DEPTH_TEST);
    glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
    if (!savedBlend) glDisable(GL_BLEND);
    glUseProgram((GLuint)savedProgram);
    glBindVertexArray((GLuint)savedVAO);

    extern void __stdcall gos_InvalidateRenderStateCache();
    gos_InvalidateRenderStateCache();

    // VK-OPT-1: per-frame upload/alloc summary (gated). Prints total bytes
    // pushed to the GPU + per-record scratch allocs so the redundancy is
    // measurable and a regression is catchable. With the fix heapAllocs=0 and
    // uploadBytes reflects the tight (pos=12B/vert) format.
    if (tubeUploadStatsEnabled()) {
        // baselineBytes = opt bytes + the 4 bytes/vert of position padding the
        // old vec4-format uploaded (16B/vert) but the tight float[] (12B/vert)
        // does not. saved = baselineBytes - uploadBytes.
        s_tubeStats.baselineBytes =
            s_tubeStats.uploadBytes + (unsigned long long)s_tubeStats.posVerts * 4ull;
        const unsigned long long saved =
            s_tubeStats.baselineBytes - s_tubeStats.uploadBytes;
        std::fprintf(stderr,
            "[VFX_TUBE_UPLOAD_STATS] records=%u posVerts=%u uploadBytes=%llu "
            "baselineBytes=%llu savedBytes=%llu heapAllocs=%llu\n",
            s_tubeStats.records, s_tubeStats.posVerts, s_tubeStats.uploadBytes,
            s_tubeStats.baselineBytes, saved, s_tubeStats.heapAllocs);
        std::fflush(stderr);
    }

    s_ribbonQueue.clear();
}

extern "C" void gos_SetActiveCamera(const float right_xyz[3], const float up_xyz[3])
{
    for (int i = 0; i < 3; ++i) {
        g_cam_right[i] = right_xyz[i];
        g_cam_up[i]    = up_xyz[i];
    }
    g_cam_set_this_frame = true;
}

extern "C" void gos_GetCameraRight(float out_xyz[3])
{
    for (int i = 0; i < 3; ++i) out_xyz[i] = g_cam_right[i];
}

extern "C" void gos_GetCameraUp(float out_xyz[3])
{
    for (int i = 0; i < 3; ++i) out_xyz[i] = g_cam_up[i];
}

extern "C" void gos_ClearActiveCamera(void)
{
    g_cam_set_this_frame = false;
}

extern "C" void gos_particle_bridge_flush(const mc2::particles::GpuParticle* records,
                                          unsigned int                       count,
                                          const mc2::particles::GroupInfo*   groups,
                                          unsigned int                       numGroups) {
    if (count == 0 || records == nullptr) return;
    mc2::ScopedFlushTimer _pfst(0, "billboard");  // PARTICLE-FLUSH-STALL-MEASURE-1

    ensureInitialized();

    // P0-5: failure log — once only.
    if (s_initFailed) {
        static bool s_failLogEmitted = false;
        if (!s_failLogEmitted) {
            s_failLogEmitted = true;
            std::fprintf(stderr, "[GOSFX_GPU v1] ERROR init_failed\n");
            std::fflush(stderr);
        }
        return;
    }
    if (s_prog == nullptr || s_prog->shp_ == 0) return;

#ifdef MC2_VULKAN_ISLAND
    // VK-FIELD-VFX-1: under the VK shared-GBuffer umbrella the GL particle draw
    // renders into the DISCARDED GL backbuffer (the native VK composite samples
    // only the shared color0), so weapon fire VANISHES. When the VFX island is
    // armed (MC2_VK_VFX, default-ON under MC2_VK_SHARED_GBUFFER), render the SAME
    // billboard stream into the shared scene color0 via VK (depth-tested against
    // the shared depth) and SKIP the GL draw. UNTEXTURED slice — textured sprite
    // atlases, the tube-ribbon beams, and the gosFX MESH shapes are the documented
    // deferred remainder. Umbrella OFF -> island gate OFF -> GL draw (byte-identical).
    if (mc2_vk_vfx_island_enabled() && groups != nullptr && numGroups > 0) {
        gosPostProcess* ppVk = getGosPostProcess();
        const int vkW = ppVk ? (int)ppVk->getWidth()  : 0;
        const int vkH = ppVk ? (int)ppVk->getHeight() : 0;
        const float* mvp = gos_GetTerrainMVPMat4();
        if (vkW > 0 && vkH > 0 && mvp) {
            std::vector<VfxParticleDraw> vkDraws;
            vkDraws.reserve(numGroups);
            for (unsigned gi = 0; gi < numGroups; ++gi) {
                VfxParticleDraw d;
                d.start    = groups[gi].start;
                d.count    = groups[gi].count;
                d.additive = (groups[gi].blendMode == 1) ? 1u : 0u;
                vkDraws.push_back(d);
            }
            if (mc2_vk_vfx_island_run(records, count, vkDraws.data(),
                                      (uint32_t)vkDraws.size(), mvp,
                                      g_cam_right, g_cam_up, vkW, vkH)) {
                return;   // VK handled the frame; do NOT run the GL draw.
            }
            // Island declined (boot warmup / shared GBuffer not ready) -> fall
            // through to the GL draw; under the umbrella it lands in the discarded
            // backbuffer (invisible, same as pre-slice) until the island warms up.
        }
    }
#endif

    // Q2-S0 instrumentation only (no behavior change). Coarse zone + bucket for
    // the billboard particle SSBO upload + per-group draw. Placed after all the
    // early-returns so only real flushes are timed.
    ZoneScopedN("gos.ParticleBridge.Flush");
    mc2::fx_cost_split::Scope _fxcs(mc2::fx_cost_split::B_PARTICLE_BRIDGE_FLUSH);

    // P1-5: first-call banner — once only.
    {
        static bool s_bannerEmitted = false;
        if (!s_bannerEmitted) {
            s_bannerEmitted = true;
            std::fprintf(stderr,
                         "[GOSFX_GPU v1] enabled=1 sprites=%u draws=%u textures=%u blendMode=straight\n",
                         count, numGroups, numGroups);
            std::fflush(stderr);
        }
    }

    // P2-3: per-group UV debug log on first flush — gated behind MC2_GOSFX_GROUP_LOG=1.
    // Shows UV rects and blend modes being propagated from spawn through to the bridge.
    if (groupLogEnabled()) {
        static bool s_uvDumpDone = false;
        if (!s_uvDumpDone && numGroups > 0) {
            s_uvDumpDone = true;
            for (unsigned gi = 0; gi < numGroups; ++gi) {
                const mc2::particles::GroupInfo& g = groups[gi];
                std::fprintf(stderr,
                    "[GOSFX_GPU v1] group %u: tex=%u uv=(%.2f,%.2f)+(%.2f,%.2f) count=%u blend=%s\n",
                    gi, g.handle, g.u0, g.v0, g.us, g.vs, g.count,
                    g.blendMode == 1 ? "additive" : "alpha");
            }
            std::fflush(stderr);
        }
    }

    // B2 P1: warn once if the caller never called gos_SetActiveCamera this frame.
    // Gated behind MC2_GOSFX_GROUP_LOG=1 to avoid log noise in normal runs.
    if (!g_cam_set_this_frame) {
        static bool warned = false;
        if (!warned) {
            if (groupLogEnabled())
                std::fprintf(stderr, "[B2] gos_particle_bridge: flush without gos_SetActiveCamera; using last-known basis\n");
            warned = true;
        }
    }

    ensureSsboCapacity((GLsizei)count);

    // ── State save (trap #4 VAO, sampler, blend, depth, program) ──────
    GLint savedProgram   = 0; glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram);
    GLint savedVAO       = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVAO);
    GLint savedSrcRGB    = 0; glGetIntegerv(GL_BLEND_SRC_RGB, &savedSrcRGB);
    GLint savedDstRGB    = 0; glGetIntegerv(GL_BLEND_DST_RGB, &savedDstRGB);
    GLboolean savedBlend = glIsEnabled(GL_BLEND);
    GLboolean savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint savedDepthFunc = 0; glGetIntegerv(GL_DEPTH_FUNC, &savedDepthFunc);
    GLint savedDepthMask = 0; glGetIntegerv(GL_DEPTH_WRITEMASK, &savedDepthMask);
    GLint savedSampler   = 0; glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &savedSampler);
    GLint savedActiveTex = 0; glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTex);
    GLint savedTex2D0    = 0;
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex2D0);
    // P0-2 cull state: bridge must disable GL_CULL_FACE for the draw because
    // the particle shader is double-sided. Restore the caller's state after.
    GLboolean savedCullFace = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_CULL_FACE);

    // ── Bind our VAO (trap #4: AMD silently drops draws when VAO=0) ──
    glBindVertexArray(s_vao);

    // ── Bind program + uniforms (P0-4: use cached locations) ─────────
    glUseProgram(s_prog->shp_);
    {
        // terrainMVP is row-major direct-upload (GL_FALSE) per
        // memory/terrain_mvp_gl_false.md.
        const float* mvp = gos_GetTerrainMVPMat4();
        if (mvp && s_loc_worldToClipGL >= 0)
            glUniformMatrix4fv(s_loc_worldToClipGL, 1, GL_FALSE, mvp);
    }
    {
        // B1 C14: u_mvp — pixel-space -> GL NDC matrix. GL_TRUE (transpose)
        // matches the static_prop bridge at gos_static_prop_batcher.cpp:3014.
        const float* mm = gos_GetProj2ScreenMat4();
        if (mm && s_loc_mvp >= 0)
            glUniformMatrix4fv(s_loc_mvp, 1, GL_TRUE, mm);
    }
    {
        if (s_loc_uAtlas >= 0) glUniform1i(s_loc_uAtlas, 0);
    }
    // B2 P1: bind camera basis uniforms. g_cam_right/up are already in GL
    // world space (axis-swapped by gamecam.cpp before calling gos_SetActiveCamera).
    if (s_loc_cameraRight >= 0) glUniform3fv(s_loc_cameraRight, 1, g_cam_right);
    if (s_loc_cameraUp    >= 0) glUniform3fv(s_loc_cameraUp,    1, g_cam_up);
    // VFX-DEBUG-VIEWS-1: debug-mode selector (default 0 = byte-identical Final).
    if (s_loc_debugMode   >= 0) glUniform1i(s_loc_debugMode, vfxDebugMode());
    // VFX-TUNING-UI-1: per-flush intensity scales (defaults 1.0 = no-op).
    // VFX-SHADER-AGE-FADE-PARITY-1: age fade (default 0.0 = no-op).
    vfxTuneInitIfNeeded();
    if (s_loc_vfxBrightness         >= 0) glUniform1f(s_loc_vfxBrightness,         s_vfxBrightness);
    if (s_loc_vfxAdditiveBrightness >= 0) glUniform1f(s_loc_vfxAdditiveBrightness, s_vfxAdditiveBrightness);
    if (s_loc_vfxAlphaScale         >= 0) glUniform1f(s_loc_vfxAlphaScale,         s_vfxAlphaScale);
    if (s_loc_vfxAgeFade            >= 0) glUniform1f(s_loc_vfxAgeFade,            s_vfxAgeFade);
    // u_vfxIsAdditive defaults to alpha (0); set per-group in the draw loop.
    if (s_loc_vfxIsAdditive         >= 0) glUniform1i(s_loc_vfxIsAdditive, 0);
    // VFX-BLACKBODY-1: gate-OFF (default) uploads 0 here and never raises it, so
    // the FS tint branch is dead -> byte-identical. Gate-ON raises it to 1 ONLY
    // for additive groups in the per-group draw loop below.
    vfxBlackbodyInitIfNeeded();
    if (s_loc_vfxBlackbody          >= 0) glUniform1i(s_loc_vfxBlackbody, 0);

    // ── VFX-SOFT-PARTICLES-MVP-1: depth-fade setup ───────────────────
    // When the gate is OFF we upload u_softDistance=0 so the FS fade branch is
    // skipped -> byte-identical. When ON, snapshot scene depth (avoids the
    // FBO feedback loop), bind the copy on unit 1, and upload the inverse of
    // the SAME matrix the VS projects with (gosPostProcess::inverseViewProj_).
    GLint savedTex2D1 = 0;
    bool  softActive  = false;
    float softDist    = 0.0f;
    vfxSoftInitIfNeeded();
    if (s_soft_enabled && s_loc_softDistance >= 0) {
        gosPostProcess* pp = getGosPostProcess();
        if (pp) {
            pp->copySceneDepthForParticles();
            const GLuint depthCopy = pp->getSceneDepthCopyTexture();
            if (depthCopy != 0) {
                softActive = true;
                softDist   = (s_softDistance > 0.0f) ? s_softDistance : 0.0f;
                glActiveTexture(GL_TEXTURE1);
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex2D1);
                glBindTexture(GL_TEXTURE_2D, depthCopy);
                glBindSampler(1, 0);  // use the texture's own NEAREST params
                glActiveTexture(GL_TEXTURE0);
                if (s_loc_uSceneDepth    >= 0) glUniform1i(s_loc_uSceneDepth, 1);
                if (s_loc_invWorldToClip >= 0)
                    glUniformMatrix4fv(s_loc_invWorldToClip, 1, GL_FALSE, pp->getInverseViewProj());
                if (s_loc_screenSize     >= 0)
                    glUniform2f(s_loc_screenSize, (float)pp->getWidth(), (float)pp->getHeight());
            }
        }
    }
    if (s_loc_softDistance >= 0) glUniform1f(s_loc_softDistance, softDist);

    // ── VFX-SCENECOLOR-GRAB-1: feedback-safe scene-color snapshot ────────
    // FRAME_RESOURCE_SUBSTRATE — produce the resource only. Same frame window as
    // the soft-particle depth copy above (after opaque scene color is resolved,
    // before this VFX flush draws). Gate OFF (default) -> no copy, byte-identical.
    // Gate ON -> one glCopyImageSubData into pp->sceneColorCopyTex_; NOTHING
    // samples it yet (no consumer) -> still no visual change.
    vfxSceneColorGrabInitIfNeeded();
    if (s_scenecolor_enabled) {
        gosPostProcess* ppGrab = getGosPostProcess();
        if (ppGrab) ppGrab->copySceneColorForVfx();
    }

    // ── VFX-DISTORTION-1: heat-haze refraction setup ─────────────────────
    // Bind the pre-VFX scene-color grab on tex UNIT 2 (save/restore, mirroring the
    // unit-1 depth restore) so the distortion alpha group can sample it. Upload
    // u_time ONCE PER FLUSH (monotonic seconds accumulated from gos_GetElapsedTime).
    // Default u_vfxDistort=0 here; the draw loop raises it only for the distortion
    // group. distortActive REQUIRES both the gate AND the grab resource to be live;
    // if the grab is absent (MC2_VFX_SCENECOLOR_GRAB unset) distortActive stays
    // false -> u_vfxDistort never set to 1 -> nothing samples a GL-0 texture.
    GLint savedTex2D2  = 0;
    bool  distortActive = false;
    vfxDistortInitIfNeeded();
    if (s_distort_enabled) {
        gosPostProcess* ppd = getGosPostProcess();
        const GLuint colorCopy = ppd ? ppd->getSceneColorCopyTexture() : 0u;
        if (colorCopy != 0) {
            distortActive = true;
            glActiveTexture(GL_TEXTURE2);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex2D2);
            glBindTexture(GL_TEXTURE_2D, colorCopy);
            glBindSampler(2, 0);  // use the texture's own LINEAR/CLAMP params
            glActiveTexture(GL_TEXTURE0);
            if (s_loc_sceneColor >= 0) glUniform1i(s_loc_sceneColor, 2);
            if (s_loc_distortAmp >= 0) glUniform1f(s_loc_distortAmp, s_distortAmp);
            // Monotonic seconds: accumulate the per-call delta once per flush.
            static double s_distortClockSec = 0.0;
            s_distortClockSec += gos_GetElapsedTime(1);
            if (s_loc_time >= 0) glUniform1f(s_loc_time, (float)s_distortClockSec);
        }
    }
    // Default: distortion off for all groups; raised per-group in the draw loop.
    if (s_loc_vfxDistort >= 0) glUniform1i(s_loc_vfxDistort, 0);

    // ── VFX-LIT-PARTICLES-MVP-1: scene lighting for alpha groups ─────
    // When OFF, upload strength 0 -> FS lit branch inert -> byte-identical.
    // Sun/ambient come from the global camera (eye), the same source terrain
    // consumes (gos_terrain_lighting.cpp); 0..255 -> 0..1.
    vfxLitInitIfNeeded();
    {
        float litStrength = (s_lit_enabled && eye) ? s_litStrength : 0.0f;
        if (s_loc_vfxLitStrength >= 0) glUniform1f(s_loc_vfxLitStrength, litStrength);
        if (litStrength > 0.0f) {
            const float inv255 = 1.0f / 255.0f;
            if (s_loc_vfxSunColor >= 0)
                glUniform3f(s_loc_vfxSunColor,
                            (float)eye->lightRed   * inv255,
                            (float)eye->lightGreen * inv255,
                            (float)eye->lightBlue  * inv255);
            if (s_loc_vfxAmbientColor >= 0)
                glUniform3f(s_loc_vfxAmbientColor,
                            (float)eye->ambientRed   * inv255,
                            (float)eye->ambientGreen * inv255,
                            (float)eye->ambientBlue  * inv255);
        }
    }

    // ── Sampler on unit 0 (trap #5: sampler inheritance) ─────────────
    glBindSampler(0, s_sampler);

    // ── Depth + blend state (traps #9 depth, blend reset) ────────────
    // Particle billboards: alpha-blend, depth-test against scene, no write.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);  // reverse-Z convention (matches water fast path)
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // VFX-CARD-CULL-1: re-assert cull-off HERE, not just at the top of the
    // flush. The glDisable(GL_CULL_FACE) above runs BEFORE the soft-particle
    // depth-copy (copySceneDepthForParticles, ~L455), which runs a fullscreen
    // pass that re-enables GL_CULL_FACE as a side effect. With soft particles
    // active that left the per-group billboard draws below subject to face
    // culling: spinning fire/smoke cards (per-particle spin angle) flip winding
    // as they rotate and get backface-culled on some frames -> explosion cards
    // flicker in and out. (RenderDoc pixel history: PASSED=no FLAGS=backfaceCulled
    // on the spinning explosion card draws.) Billboards are double-sided; the
    // cull-disable must be the LAST state set before the draw loop.
    glDisable(GL_CULL_FACE);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_ssbo);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, /*binding=*/14, s_ssbo);

    if (numGroups == 0 || groups == nullptr) {
        // Fallback: no group metadata — treat entire buffer as one group,
        // full UV rect, handle from first record. Should not occur after
        // Phase 2 callers always call BeginGroup; kept for robustness.
        const uint32_t gosHandle = records[0].atlasIndex;
        const GLuint   glTex     = (GLuint)gos_GetGLTextureName(gosHandle);
        if (glTex != 0) {
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                            (GLsizeiptr)(count * sizeof(mc2::particles::GpuParticle)),
                            records);
            glBindTexture(GL_TEXTURE_2D, glTex);
            if (s_loc_uvOffset     >= 0) glUniform2f(s_loc_uvOffset,    0.0f, 0.0f);
            if (s_loc_uvSize       >= 0) glUniform2f(s_loc_uvSize,      1.0f, 1.0f);
            if (s_loc_atlasColumns >= 0) glUniform1ui(s_loc_atlasColumns, 0u);
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(count * 6u));
        } else {
            if (groupLogEnabled()) {
                std::fprintf(stderr,
                    "[GOSFX_GPU v1] ERROR missing_texture handle=%u\n", gosHandle);
                std::fflush(stderr);
            }
        }
    } else {
        // ── Per-group draw loop (P2-1) ────────────────────────────────
        // For each group produced by Batcher::BeginGroup:
        //   1. Resolve the gos handle to a GLuint; skip if not resident.
        //   2. Upload only this group's records to SSBO offset 0.
        //   3. Set the UV sub-rect uniforms for the billboard VS.
        //   4. Bind the resolved texture and draw.
        for (unsigned gi = 0; gi < numGroups; ++gi) {
            const mc2::particles::GroupInfo& grp = groups[gi];
            if (grp.count == 0) continue;

            // Resolve handle to GL texture name.
            const GLuint glTex = (GLuint)gos_GetGLTextureName(grp.handle);
            if (glTex == 0) {
                if (grp.handle == 0) {
                    // handle=0: B2 debt — point/shard/tube emitters not yet
                    // texture-wired via BeginGroup. Log once under MC2_GOSFX_GROUP_LOG=1.
                    if (groupLogEnabled()) {
                        static bool s_b2DebtWarned = false;
                        if (!s_b2DebtWarned) {
                            s_b2DebtWarned = true;
                            std::fprintf(stderr,
                                "[GOSFX_GPU v1] NOTE handle=0 groups present (B2 debt: point/shard/tube emitters not yet texture-wired)\n");
                            std::fflush(stderr);
                        }
                    }
                } else {
                    // Non-zero handle that failed to resolve — real error.
                    // Gated behind MC2_GOSFX_GROUP_LOG=1 to avoid spew on first
                    // frames before ForceLoadImages() completes.
                    if (groupLogEnabled()) {
                        static std::unordered_set<uint32_t> s_loggedMissingHandles;
                        if (s_loggedMissingHandles.insert(grp.handle).second) {
                            std::fprintf(stderr,
                                "[GOSFX_GPU v1] ERROR missing_texture handle=%u\n", grp.handle);
                            std::fflush(stderr);
                        }
                    }
                }
                continue;
            }

            // Upload this group's contiguous records to SSBO offset 0.
            const mc2::particles::GpuParticle* groupRecords = records + grp.start;
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                            (GLsizeiptr)(grp.count * sizeof(mc2::particles::GpuParticle)),
                            groupRecords);

            // P2-1: set UV sub-rect uniforms per group.
            if (s_loc_uvOffset >= 0) glUniform2f(s_loc_uvOffset, grp.u0, grp.v0);
            if (s_loc_uvSize   >= 0) glUniform2f(s_loc_uvSize,   grp.us, grp.vs);
            // VFX-FLIPBOOK-ASSET-TABLE-1: atlas column count (0 = non-animated).
            if (s_loc_atlasColumns >= 0)
                glUniform1ui(s_loc_atlasColumns, grp.atlasColumns);

            // VFX-APPLYPIPELINE-ROUTING-1: per-group blend now driven by the
            // VfxBillboard pipeline row (additive = AdditiveSrcAlphaOne = SRC_ALPHA/ONE;
            // alpha = AlphaBlend = SRC_ALPHA/ONE_MINUS_SRC_ALPHA). applyPipeline also
            // re-asserts the flush-level depth/cull (identical values) — provably the
            // same GL state as the old hand-set glBlendFunc; program(0)=skip, so the
            // bound particle program + uniforms + VAO are untouched.
            pipeline_binder::applyPipeline(
                RenderCore::getPipelineDesc(grp.blendMode == 1
                    ? RenderCore::PipelineId::VfxBillboardAdditive
                    : RenderCore::PipelineId::VfxBillboardAlpha),
                grp.blendMode == 1 ? "VfxBillboardAdditive" : "VfxBillboardAlpha");
            // VFX-TUNING-UI-1: tell the FS whether this group is additive so the
            // additive-brightness scale applies only to additive groups.
            if (s_loc_vfxIsAdditive >= 0)
                glUniform1i(s_loc_vfxIsAdditive, grp.blendMode == 1 ? 1 : 0);
            // VFX-BLACKBODY-1: enable the blackbody emissive tint only for
            // additive/emissive groups, and only while the gate is ON. Alpha
            // groups (smoke/dust) keep u_vfxBlackbody=0 -> never tinted.
            if (s_loc_vfxBlackbody >= 0)
                glUniform1i(s_loc_vfxBlackbody,
                            (s_blackbody_enabled && grp.blendMode == 1) ? 1 : 0);
            // VFX-DISTORTION-1: enable heat-haze refraction for the distortion
            // alpha group ONLY. distortActive already requires the gate AND a live
            // scene-color grab. The FIXTURE (MC2_VFX_DISTORT_FIXTURE) tags every
            // alpha group (blendMode==0) as distortion so the effect is observable
            // without content effect->distortion mapping (out of scope). Additive
            // groups are never distortion (refraction replaces dst, not adds).
            // Fixture OFF -> no group is tagged -> byte-identical even gate-ON.
            if (s_loc_vfxDistort >= 0)
                glUniform1i(s_loc_vfxDistort,
                            (distortActive && s_distort_fixture && grp.blendMode == 0) ? 1 : 0);

            // Bind the resolved texture.
            glBindTexture(GL_TEXTURE_2D, glTex);

            // Draw: 6 vertices per particle, gl_VertexID-driven.
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(grp.count * 6u));
        }
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    // D-01: symmetrically unbind indexed slot 14 (mirrors gpu_cull_compute.cpp:1187).
    // Without this the slot stays bound to s_ssbo across frame boundaries, creating
    // a latent hazard if frame ordering shifts (gpu_cull or any future SSBO consumer
    // at slot 14 would alias the particle buffer).
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, gpu_cull::READBACK_SSBO_BINDING, 0u);

    // ── Restore state ────────────────────────────────────────────────
    if (savedCullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    // VFX-SOFT-PARTICLES-MVP-1: restore unit-1 binding used for the depth copy
    // (active texture is GL_TEXTURE0 here — the draw loop binds on unit 0).
    if (softActive) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex2D1);
        glActiveTexture(GL_TEXTURE0);
    }
    // VFX-DISTORTION-1: restore unit-2 binding used for the scene-color grab
    // (mirrors the unit-1 restore above — avoids the tex-unit-leak class).
    if (distortActive) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex2D2);
        glActiveTexture(GL_TEXTURE0);
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)savedTex2D0);
    glBindSampler(0, (GLuint)savedSampler);
    if (savedActiveTex != GL_TEXTURE0) glActiveTexture((GLenum)savedActiveTex);
    glDepthMask((GLboolean)savedDepthMask);
    glDepthFunc((GLenum)savedDepthFunc);
    if (!savedDepthTest) glDisable(GL_DEPTH_TEST);
    glBlendFunc((GLenum)savedSrcRGB, (GLenum)savedDstRGB);
    if (!savedBlend) glDisable(GL_BLEND);
    glUseProgram((GLuint)savedProgram);
    glBindVertexArray((GLuint)savedVAO);

    // VFX-CACHE-SYNC-1: this pass set blend/cull/depth/program/sampler via raw
    // GL calls that bypass the gos render-state CACHE. Even though we restore
    // actual GL state above, the cache may now disagree with GL (incomplete
    // restore of separate-alpha blend, or a sub-pass like
    // copySceneDepthForParticles mutating cached state), so the next
    // gos_SetRenderState(sameValue) would be a skipped no-op and leave the wrong
    // blend/cull on subsequent legacy/MLR draws (e.g. explosion DebrisCloud/Shape
    // cards drawn after this bridge) -> intermittent state-driven flicker.
    // Force the cache to re-sync. Cost: a few extra state calls on the next
    // draw, once per frame — same guard the mech batcher uses (gos_mech_batcher.cpp).
    extern void __stdcall gos_InvalidateRenderStateCache();
    gos_InvalidateRenderStateCache();
}

// VFX-SPINE-0: read-only accessors for the Object Inspector. Pure getters
// over file-static state (SSBO capacity, linked program id, init-failure flag,
// per-frame camera-basis "set" flag). No GL calls, no mutation.
extern "C" unsigned int gos_vfx_getParticleProgramId()
{
    return (s_prog && s_prog->shp_) ? (unsigned int)s_prog->shp_ : 0u;
}
extern "C" unsigned int gos_vfx_getSsboCapacity()
{
    return (unsigned int)s_ssboCapacity;
}
extern "C" int gos_vfx_getInitFailed()
{
    return s_initFailed ? 1 : 0;
}
extern "C" int gos_vfx_getCameraSetThisFrame()
{
    return g_cam_set_this_frame ? 1 : 0;
}
// VFX-DEBUG-VIEWS-1: active particle debug mode (0..4), seeded from
// MC2_VFX_DEBUG_MODE. Read-only; resolves the env lazily on first call.
extern "C" int gos_vfx_getDebugMode()
{
    return vfxDebugMode();
}
// VFX-TUNING-UI-1: runtime debug-mode override (Graphics Options combo).
// Clamped 0..5 (5=Age added by VFX-SHADER-AGE-FADE-PARITY-1); marks initialized
// so it wins over a later env read. Look-only.
extern "C" void gos_vfx_setDebugMode(int m)
{
    s_debugMode_initialized = true;
    s_debugMode_value = (m >= 0 && m <= 5) ? m : 0;
}

// VFX-TUNING-UI-1: runtime intensity-scale get/set (Graphics Options sliders).
// Clamped 0..8; default 1.0 = byte-identical no-op. Look-only.
extern "C" float gos_vfx_getBrightness()         { vfxTuneInitIfNeeded(); return s_vfxBrightness; }
extern "C" float gos_vfx_getAdditiveBrightness() { vfxTuneInitIfNeeded(); return s_vfxAdditiveBrightness; }
extern "C" float gos_vfx_getAlphaScale()         { vfxTuneInitIfNeeded(); return s_vfxAlphaScale; }
extern "C" void  gos_vfx_setBrightness(float v)         { vfxTuneInitIfNeeded(); s_vfxBrightness = clampVfxScale(v); }
extern "C" void  gos_vfx_setAdditiveBrightness(float v) { vfxTuneInitIfNeeded(); s_vfxAdditiveBrightness = clampVfxScale(v); }
extern "C" void  gos_vfx_setAlphaScale(float v)         { vfxTuneInitIfNeeded(); s_vfxAlphaScale = clampVfxScale(v); }

// VFX-SOFT-PARTICLES-MVP-1: enable + fade-band accessors (ImGui + inspector).
extern "C" int   gos_vfx_getSoftEnabled()         { vfxSoftInitIfNeeded(); return s_soft_enabled ? 1 : 0; }
extern "C" void  gos_vfx_setSoftEnabled(int e)    { vfxSoftInitIfNeeded(); s_soft_enabled = (e != 0); }
extern "C" float gos_vfx_getSoftDistance()        { return s_softDistance; }
extern "C" void  gos_vfx_setSoftDistance(float v) { s_softDistance = clampSoftDist(v); }

// VFX-LIT-PARTICLES-MVP-1: enable + strength accessors (ImGui + profile + inspector).
extern "C" int   gos_vfx_getLitEnabled()         { vfxLitInitIfNeeded(); return s_lit_enabled ? 1 : 0; }
extern "C" void  gos_vfx_setLitEnabled(int e)    { vfxLitInitIfNeeded(); s_lit_enabled = (e != 0); }
extern "C" float gos_vfx_getLitStrength()        { vfxLitInitIfNeeded(); return s_litStrength; }
extern "C" void  gos_vfx_setLitStrength(float v) { vfxLitInitIfNeeded(); s_litStrength = clampLit(v); }

// VFX-SHADER-AGE-FADE-PARITY-1: age-driven soft death fade (0.0=OFF / 1.0=full).
// 0.0 default = byte-identical. Oracle particles only (p.lifetime sentinel).
// Env: MC2_TUNE_VFX_AGE_FADE (clamped 0..1). ImGui VFX Tuning slider.
extern "C" float gos_vfx_getAgeFade()        { vfxTuneInitIfNeeded(); return s_vfxAgeFade; }
extern "C" void  gos_vfx_setAgeFade(float v) { vfxTuneInitIfNeeded(); s_vfxAgeFade = clampAgeFade(v); }
