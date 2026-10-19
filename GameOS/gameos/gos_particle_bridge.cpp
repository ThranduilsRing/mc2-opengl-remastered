// Particle camera/tuning state and native Vulkan billboard dispatch.
#include "gos_particle_bridge_internal.h"
#include "mc2fmt.h"
#include "Gates/render_options.h"


namespace {

// B2 P1: active camera basis — set by GameCamera::render() before flush.
// Defaults to identity (right=+X, up=+Y) so particles still appear if the
// caller forgets to call gos_SetActiveCamera (produces east-up orientation,
// same as the pre-B2 fixed-axis behaviour).
float g_cam_right[3]        = {1.0f, 0.0f, 0.0f};
float g_cam_up[3]           = {0.0f, 1.0f, 0.0f};
bool  g_cam_set_this_frame  = false;

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

#ifdef MC2_VULKAN_ISLAND
    // Vulkan owns the visible scene; a declined island has no GL fallback.
    if (mc2_vk_vfx_island_enabled() && groups != nullptr && numGroups > 0) {
        gosPostProcess* ppVk = getGosPostProcess();
        const int vkW = ppVk ? static_cast<int>(ppVk->getWidth())  : 0;
        const int vkH = ppVk ? static_cast<int>(ppVk->getHeight()) : 0;
        const float* mvp = gos_GetTerrainMVPMat4();
        if (vkW > 0 && vkH > 0 && mvp) {
            std::vector<VfxParticleDraw> vkDraws;
            vkDraws.reserve(numGroups);
            for (unsigned gi = 0; gi < numGroups; ++gi) {
                VfxParticleDraw d;
                d.start    = groups[gi].start;
                d.count    = groups[gi].count;
                d.additive = (groups[gi].blendMode == 2) ? 2u : (groups[gi].blendMode == 1) ? 1u : 0u;
                d.texture  = groups[gi].handle;
                d.u0 = groups[gi].u0;  d.v0 = groups[gi].v0;
                d.us = groups[gi].us;  d.vs = groups[gi].vs;
                d.atlasColumns = groups[gi].atlasColumns;
                vkDraws.push_back(d);
            }
            if (mc2_vk_vfx_island_run(records, count, vkDraws.data(),
                                      static_cast<uint32_t>(vkDraws.size()), mvp,
                                      g_cam_right, g_cam_up, vkW, vkH)) {
                return;   // VK handled the frame; do NOT run the GL draw.
            }
        }
    }
#endif

}

extern "C" unsigned int gos_vfx_getParticleProgramId()
{
    return 0u;
}
extern "C" unsigned int gos_vfx_getSsboCapacity()
{
    return 0u;
}
extern "C" int gos_vfx_getInitFailed()
{
    return 0;
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
