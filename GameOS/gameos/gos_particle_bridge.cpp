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

#include <cstdio>
#include "gos_particle_bridge_internal.h"
#include "mc2fmt.h"
#include "Gates/render_options.h"


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

}  // namespace

// Lazy-init env gate for verbose group/texture diagnostics.
// MC2_GOSFX_GROUP_LOG=1 enables: UV rect dump (first flush) + missing-texture errors.
// Normal runs see only the first-flush banner; set this flag for texture debug sessions.
bool s_groupLog_initialized = false;
bool s_groupLog_value       = false;
bool groupLogEnabled() {
    if (!s_groupLog_initialized) {
        s_groupLog_value = mc2::gate::get<"MC2_GOSFX_GROUP_LOG">();
        s_groupLog_initialized = true;
    }
    return s_groupLog_value;
}

namespace {

// VFX-DEBUG-VIEWS-1: particle billboard debug-mode selector.
// 0=Final (byte-identical default), 1=Albedo, 2=Alpha, 3=ParticleKind,
// 4=Overdraw proxy. Seeded once from MC2_VFX_DEBUG_MODE (clamped 0..4);
// diagnostic-only, no gameplay/emission/lifetime effect. Read-only getter
// gos_vfx_getDebugMode() surfaces the active value in the Object Inspector.
bool s_debugMode_initialized = false;
int  s_debugMode_value       = 0;
int  vfxDebugMode() {
    if (!s_debugMode_initialized) {
        s_debugMode_value = mc2::gate::get<"MC2_VFX_DEBUG_MODE">();   // row: 0..5 (5=Age, VFX-SHADER-AGE-FADE-PARITY-1), else 0
        s_debugMode_initialized = true;
    }
    return s_debugMode_value;
}

// VFX-TUNING-UI-1: user intensity scales. All default 1.0 (= byte-identical
// no-op); the per-mission visual tuning profile and the Graphics Options
// "VFX Tuning" sliders set them at runtime through the setters below. These
// tune LOOK only — no emission/lifetime/sorting/timing change.
// VFX-SHADER-AGE-FADE-PARITY-1: s_vfxAgeFade (default 0.0 = off, byte-identical;
// 1.0 = full soft-death fade for oracle particles in the final 30% of life).
float s_vfxBrightness         = 1.0f;
float s_vfxAdditiveBrightness = 1.0f;
float s_vfxAlphaScale         = 1.0f;
float s_vfxAgeFade            = 0.0f;
static float clampVfxScale(float v) { return v < 0.0f ? 0.0f : (v > 8.0f ? 8.0f : v); }   // the runtime setters
static float clampAgeFade(float v)  { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// VFX-SOFT-PARTICLES-MVP-1: depth-fade enable + world-unit fade band. The options.cfg
// key VfxSoftParticles (default off -> byte-identical). When on, the flush
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
    s_soft_enabled = mc2::options::current().vfxSoftParticles;
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
    s_scenecolor_enabled = mc2::gate::get<"MC2_VFX_SCENECOLOR_GRAB">();
    s_scenecolor_initialized = true;
}

// VFX-LIT-PARTICLES-MVP-1: scene-lit alpha smoke/dust. The options.cfg key
// VfxLitParticles (default off -> byte-identical). Strength = the 0.7 default / per-mission
// "vfxLitStrength" profile key / ImGui slider (clamped 0..1). When OFF the
// bridge uploads strength 0 so the FS lit branch is inert.
// Alpha groups only; additive flashes stay emissive.
bool  s_lit_initialized = false;
bool  s_lit_enabled     = false;
float s_litStrength     = 0.7f;   // applied only while the option is on
static float clampLit(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }   // the runtime setter
void vfxLitInitIfNeeded() {
    if (s_lit_initialized) return;
    s_lit_enabled = mc2::options::current().vfxLitParticles;
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
    s_blackbody_enabled = mc2::gate::get<"MC2_VFX_BLACKBODY">();
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
    s_distort_enabled = mc2::gate::get<"MC2_VFX_DISTORTION">();
    s_distort_fixture = mc2::gate::get<"MC2_VFX_DISTORT_FIXTURE">();
    s_distortAmp = mc2::gate::get<"MC2_VFX_DISTORT_AMP">();   // row: default 0.04, accepts (0, 0.5)
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
            mc2::fmt::print_err("[GPU_PARTICLES v1] event=prog_compile_fail — bridge disabled\n");
            std::fflush(stderr);
            s_prog = nullptr;
            return;
        }
        mc2::fmt::print_err("[GPU_PARTICLES v1] event=prog_compiled prog={:d}\n", mc2::fmt::as_u32((unsigned)s_prog->shp_));
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
        // dead-code elimination strips it).
        s_loc_vfxAgeFade            = glGetUniformLocation(s_prog->shp_, "u_vfxAgeFade");
        // VFX-SOFT-PARTICLES-MVP-1: soft-particle depth-fade uniforms (may be
        // -1 when VfxSoftParticles is off and dead-code elim strips them).
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
                mc2::fmt::print_err("[B2] gos_particle_bridge: uniform locations missing — right={:d} up={:d}\n", mc2::fmt::as_i32(s_loc_cameraRight), mc2::fmt::as_i32(s_loc_cameraUp));
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

    // ROW29-TEMP-DIAG (not to be committed)
    if (count >= 90 && count <= 140 && groups) {
        static int s_dumped = 0;
        if (s_dumped < 3) {
            ++s_dumped;
            fprintf(stderr, "[ROW29] flush count=%u groups=%u
", count, numGroups);
            for (unsigned gi = 0; gi < numGroups; ++gi) {
                const auto& g = groups[gi];
                fprintf(stderr, "[ROW29] G%u handle=%u uv=%.3f,%.3f,%.3f,%.3f blend=%d cols=%u start=%u count=%u
", gi, g.handle, g.u0, g.v0, g.us, g.vs, (int)g.blendMode, g.atlasColumns, g.start, g.count);
                for (unsigned i = g.start; i < g.start + g.count && i < count; ++i) {
                    const auto& r = records[i];
                    fprintf(stderr, "[ROW29]   P%u pos=%.1f,%.1f,%.1f size=%.2f vel=%.2f,%.2f,%.2f col=%.2f,%.2f,%.2f,%.2f atlas=%u age=%.3f life=%.3f kf=%u
", i, r.position[0], r.position[1], r.position[2], r.size, r.velocity[0], r.velocity[1], r.velocity[2], r.color[0], r.color[1], r.color[2], r.color[3], r.atlasIndex, r.age, r.lifetime, r.kind_flags);
                }
            }
            fflush(stderr);
        }
    }

    // P0-5: failure log — once only.
    if (s_initFailed) {
        static bool s_failLogEmitted = false;
        if (!s_failLogEmitted) {
            s_failLogEmitted = true;
            mc2::fmt::print_err("[GOSFX_GPU v1] ERROR init_failed\n");
            std::fflush(stderr);
        }
        return;
    }
    if (s_prog == nullptr || s_prog->shp_ == 0) return;

#ifdef MC2_VULKAN_ISLAND
    // Under the VK umbrella the GL draw lands in a discarded backbuffer, so the
    // armed VFX island draws this billboard stream into the shared scene colour
    // instead and the GL draw is skipped. Umbrella off: GL draw unchanged.
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
                d.texture  = groups[gi].handle;
                d.u0 = groups[gi].u0;  d.v0 = groups[gi].v0;
                d.us = groups[gi].us;  d.vs = groups[gi].vs;
                d.atlasColumns = groups[gi].atlasColumns;
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
            mc2::fmt::print_err("[GOSFX_GPU v1] enabled=1 sprites={:d} draws={:d} textures={:d} blendMode=straight\n", mc2::fmt::as_u32(count), mc2::fmt::as_u32(numGroups), mc2::fmt::as_u32(numGroups));
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
                mc2::fmt::print_err("[GOSFX_GPU v1] group {:d}: tex={:d} uv=({:.2f},{:.2f})+({:.2f},{:.2f}) count={:d} blend={}\n", mc2::fmt::as_u32(gi), mc2::fmt::as_u32(g.handle), g.u0, g.v0, g.us, g.vs, mc2::fmt::as_u32(g.count), mc2::fmt::safe(g.blendMode == 1 ? "additive" : "alpha"));
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
                mc2::fmt::print_err("[B2] gos_particle_bridge: flush without gos_SetActiveCamera; using last-known basis\n");
            warned = true;
        }
    }

    ensureSsboCapacity((GLsizei)count);

    // ── State save (GLSTATE-GUARD-ADOPTION-4: RAII for the top block; see
    // gos_tube_ribbon_flush for the declaration-order rationale — invalidate
    // guard first so it destructs LAST. CULL stays fully manual — VFX-CARD-
    // CULL-1 re-asserts cull-off a SECOND time below, after the soft-particle
    // depth-copy sub-pass runs (copySceneDepthForParticles re-enables
    // GL_CULL_FACE as a side effect); a restore-previous guard set once here
    // would not re-assert -> reintroduces the spinning explosion-card flicker
    // bug (glstate-guard-epilogue-recon-1.md Site 4 trap). The two soft/
    // distortion texture-unit saves (unit 1/2, below) also stay manual: they
    // are conditional mid-function resource binds, not part of this top-
    // block/bottom-block epilogue shape.) ──────────────────────────────────
    mc2gl::GlScopedRenderStateInvalidate stateResync;
    mc2gl::GlScopedVertexArray  guardVao;
    mc2gl::GlScopedProgram      guardProgram;
    mc2gl::GlScopedCapability   guardBlend(GL_BLEND);
    mc2gl::GlScopedBlendFunc    guardBlendFunc;
    mc2gl::GlScopedCapability   guardDepthTest(GL_DEPTH_TEST);
    mc2gl::GlScopedDepthState   guardDepth;
    mc2gl::GlScopedSampler      guardSampler0(0);
    mc2gl::GlScopedTextureUnit  guardTexUnit0(0);
    // P0-2 cull state: bridge must disable GL_CULL_FACE for the draw because
    // the particle shader is double-sided. Restore the caller's state after.
    // (Manual — see the cull-trap note above.)
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
                mc2::fmt::print_err("[GOSFX_GPU v1] ERROR missing_texture handle={:d}\n", mc2::fmt::as_u32(gosHandle));
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
                            mc2::fmt::print_err("[GOSFX_GPU v1] NOTE handle=0 groups present (B2 debt: point/shard/tube emitters not yet texture-wired)\n");
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
                            mc2::fmt::print_err("[GOSFX_GPU v1] ERROR missing_texture handle={:d}\n", mc2::fmt::as_u32(grp.handle));
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
    // (Manual — cull double-assert trap, see the top-block note.)
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

    // VFX-CACHE-SYNC-1: this pass set blend/cull/depth/program/sampler via raw
    // GL calls that bypass the gos render-state CACHE. Even though the guards
    // above (unit 0 tex/sampler, depth, blend, program, VAO) restore actual GL
    // state, the cache may now disagree with GL (incomplete restore of
    // separate-alpha blend, or a sub-pass like copySceneDepthForParticles
    // mutating cached state), so the next gos_SetRenderState(sameValue) would
    // be a skipped no-op and leave the wrong blend/cull on subsequent legacy/
    // MLR draws (e.g. explosion DebrisCloud/Shape cards drawn after this
    // bridge) -> intermittent state-driven flicker. Force the cache to
    // re-sync via GlScopedRenderStateInvalidate (declared FIRST in the top
    // block, so it destructs LAST — after every other guard has restored,
    // firing exactly where the old explicit call ran).
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
extern "C" float gos_vfx_getBrightness()         { return s_vfxBrightness; }
extern "C" float gos_vfx_getAdditiveBrightness() { return s_vfxAdditiveBrightness; }
extern "C" float gos_vfx_getAlphaScale()         { return s_vfxAlphaScale; }
extern "C" void  gos_vfx_setBrightness(float v)         { s_vfxBrightness = clampVfxScale(v); }
extern "C" void  gos_vfx_setAdditiveBrightness(float v) { s_vfxAdditiveBrightness = clampVfxScale(v); }
extern "C" void  gos_vfx_setAlphaScale(float v)         { s_vfxAlphaScale = clampVfxScale(v); }

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
// ImGui VFX Tuning slider (clamped 0..1).
extern "C" float gos_vfx_getAgeFade()        { return s_vfxAgeFade; }
extern "C" void  gos_vfx_setAgeFade(float v) { s_vfxAgeFade = clampAgeFade(v); }
