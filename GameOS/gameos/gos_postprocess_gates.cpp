// gosPostProcess: gate readers for the shared-scene, Vulkan tier-1 and shadow knobs, scene placeholder textures, and the skybox-fog and screen-shadow canary gates.
// Declared in the class headers; shared names in gos_postprocess_internal.h.
#include "gos_postprocess_internal.h"
#include "mc2fmt.h"


// MC2_POSTPROCESS_BACKEND selects the backend of every routed region: "vk" (or "vulkan") asks for the
// Vulkan island, anything else (and unset) stays on GL.
bool postprocessBackendWantsVk()
{
    return mc2::gate::is<"MC2_POSTPROCESS_BACKEND", "vk">() || mc2::gate::is<"MC2_POSTPROCESS_BACKEND", "vulkan">();
}

// === Item 1: Cascaded Shadow Maps gate (dynamic shadow path) ==============
// MC2_SHADOW_CSM  : master gate, DEFAULT ON (reworked 2026-06-18, `8ff13a36`;
//                   see docs/tier1_env_vars.md "Dynamic CSM"). Set "=0" to opt
//                   out to the legacy single dynamic map path.
// MC2_SHADOW_CSM_COUNT  : cascade count, default 3, clamp [1,3].
// MC2_SHADOW_CSM_LAMBDA : log/uniform split blend, default 0.5, clamp [0,1].
bool mc2ShadowCsmEnabled()
{
    static const bool s_on = mc2::gate::get<"MC2_SHADOW_CSM">();
    return s_on;
}

// MC2_SHADER_PATH_TINT: debug-only solid-color-per-shader-path tint. DEFAULT OFF.
// 1 = each candidate ground shader returns its signature solid color early.
int mc2ShaderPathTint()
{
    static const int s_v = mc2::gate::get<"MC2_SHADER_PATH_TINT">();
    return s_v;
}

// VK-TIER1 SHARED SCENE DEPTH: presence-gated =1, DEFAULT OFF. When ON (and the
// island is compiled), the scene-depth texture (sceneDepthTex_) is backed by a
// VK-built exportable D32S8 image imported into GL as GL_DEPTH24_STENCIL8 instead
// of a plain GL texture. GL-only slice (no VK depth consumer yet). Fail-soft: if
// the shared VK context/interop is unavailable at scene-FBO creation, the plain
// GL alloc is used, so gate-ON with no interop degrades to the GL path. Gate OFF
// (or island not compiled) = byte-identical to today.
bool mc2VkSharedSceneDepthEnabled()
{
    // Umbrella family member: MC2_VK_SHARED_GBUFFER=1 arms it; explicit
    // MC2_VK_SHARED_SCENE_DEPTH=0/1 overrides. Default OFF (pure GL).
    const bool s_on = mc2vkgate::armed<"MC2_VK_SHARED_SCENE_DEPTH">();   // per-call: VK-FAILSOFT-DISARM-1
    return s_on;
}

// VK-P11 TIER-1 SHARED SCENE COLOR/NORMAL: presence-gated =1, DEFAULT OFF. When ON
// (island compiled) AND shared scene depth is ON, sceneColorTex_ (MRT0) and
// sceneNormalTex_ (MRT1) are backed by VK-built exportable RGBA16F images imported
// into GL as GL_RGBA16F, so the VK prop island can co-write props into the SAME
// GBuffer attachments in-place (Tier-1). REQUIRES MC2_VK_SHARED_SCENE_DEPTH (Tier-1
// depth-tests against the shared scene depth). Fail-soft: if the shared VK context /
// interop is unavailable at scene-FBO creation, the plain GL alloc is used, so
// gate-ON with no interop degrades to the GL path. Gate OFF (or the require missing,
// or island not compiled) = byte-identical to today.
static bool mc2VkPropColorTier1Enabled()
{
    static const bool s_on = mc2vkgate::regionTier1Armed<"MC2_VK_PROP_COLOR_TIER1">();
    return s_on;
}

// VK-P10 TERRAIN TIER-1: presence-gated =1, DEFAULT OFF. When ON (island compiled)
// AND shared scene depth is ON, the VK terrain island co-writes the LOD0 terrain
// prefix into the SAME shared scene color(MRT0)+normal(MRT1)+depth GBuffer in-place.
// It needs the SAME VK-backed exportable color/normal/depth substrate the prop Tier-1
// gate builds, so the shared-scene conversion below must trigger for EITHER gate.
// REQUIRES MC2_VK_SHARED_SCENE_DEPTH (Tier-1 depth-tests against the shared depth).
// Gate OFF / require missing / island not compiled = byte-identical.
static bool mc2VkTerrainTier1Enabled()
{
    static const bool s_on = mc2vkgate::regionTier1Armed<"MC2_VK_TERRAIN_TIER1">();
    return s_on;
}

// VK mech Tier-1 in-place co-write also renders into the shared scene GBuffer.
static bool mc2VkMechTier1Enabled()
{
    static const bool s_on = mc2vkgate::regionTier1Armed<"MC2_VK_MECH_TIER1">();
    return s_on;
}

// The shared scene color(MRT0)+normal(MRT1) exportable substrate is needed when
// the prop Tier-1 OR terrain Tier-1 OR mech Tier-1 in-place co-write gate is armed.
bool mc2VkSharedSceneColorNeeded()
{
    return mc2VkPropColorTier1Enabled() || mc2VkTerrainTier1Enabled()
        || mc2VkMechTier1Enabled();
}

bool mc2VkSharedSceneDirectResourceEnabled()
{
    // VK-RUNG8-ITEM10: the two present-tail conditions
    // (MC2_VK_PRESENT_NO_GL_IMPORT==1 && MC2_VK_PRESENT_SKIP_GL_HANDOFF==1) were
    // REMOVED. Item 8 deleted the present interop tail, so the present image is now
    // UNCONDITIONALLY a VK-only allocation with no GL->VK handoff (those two gates are
    // retired no-ops). Requiring them here was dead resolver code that would wrongly
    // decline the whole shared scene to pure GL under the umbrella unless a user set two
    // retired gates. The shared scene now arms on the post/composite direct-chain stack
    // alone (default-ON under MC2_VK_SHARED_GBUFFER).
    static const bool s_on = mc2vkgate::compositeDirectChainOn();
    return s_on;
}

GLuint createSceneColorPlaceholderTex(int w, int h, GLint internalFormat,
                                             GLenum format, GLenum type,
                                             GLint minMagFilter)
{
    GLuint tex = 0;
    // TEX-CLASS: render-target -- postprocess helper-allocated target texture
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, w, h, 0, format, type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minMagFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, minMagFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

GLuint createSceneDepthPlaceholderTex(int w, int h)
{
    GLuint tex = 0;
    // TEX-CLASS: render-target -- scene-depth placeholder (depth24s8)
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH24_STENCIL8, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

// VK-RUNG8 item-6: MC2_VK_STATIC_SHADOW_MAP is RETIRED. The VK-backed static-shadow
// import substrate (exportable D32_SFLOAT image imported into GL) is deleted; the static
// shadow map is now always the plain-GL GL_DEPTH_COMPONENT24 allocation. Setting the env
// prints a one-shot deprecation note (item-9 pattern) and changes nothing.
void mc2VkStaticShadowMapDeprecationNote()
{
    static const bool s_once = [] {
        if (mc2::gate::get<"MC2_VK_STATIC_SHADOW_MAP">())
            mc2::fmt::print_err("[VK_SHARED_STATIC_SHADOW] MC2_VK_STATIC_SHADOW_MAP is deprecated and now a no-op (VK-RUNG8-ITEM6): the VK static-shadow import substrate was deleted; static shadow uses the plain GL GL_DEPTH_COMPONENT24 path.\n");
        return true;
    }();
    (void)s_once;
}

int mc2ShadowCsmCount()
{
    static const int s_n = mc2::gate::get<"MC2_SHADOW_CSM_COUNT">();
    return s_n;
}

float mc2ShadowCsmLambda()
{
    static const float s_l = mc2::gate::get<"MC2_SHADOW_CSM_LAMBDA">();
    return s_l;
}

// MC2_SHADOW_MAP_SIZE : dynamic shadow map edge in texels. Default 8192.
// Clamp {2048,4096,8192}. VRAM (CSM 3 layers depth24): 4096=~201MB, 8192=~805MB.
int mc2ShadowMapSize()
{
    static const int s_sz = []() {
        const int s = mc2::gate::get<"MC2_SHADOW_MAP_SIZE">();   // row default 8192
        if (s <= 2048) return 2048;
        if (s <= 4096) return 4096;
        return 8192;
    }();
    return s_sz;
}

// Per-cascade shadow resolution: separate full-map (last) cascade.
// MC2_SHADOW_FULLMAP_SEPARATE (default ON): render the last (map-centered
// full-map catch-all) cascade into a separate lower-res 2D depth texture so the
// high-res near-cascade array can be (csmCount-1) layers instead of csmCount.
// When OFF, dynamicFullMapSize_ is forced == dynShadowMapSize_ (equivalent VRAM
// to the old all-in-array layout; the texture still exists, just full-res).
bool mc2ShadowFullMapSeparate()
{
    static const bool s_on = mc2::gate::get<"MC2_SHADOW_FULLMAP_SEPARATE">();
    return s_on;
}

// MC2_SHADOW_FULLMAP_SIZE : edge of the separate full-map cascade depth texture.
// Default 4096. Clamp {2048,4096,8192}. VRAM (depth24, 2D): 4096=~67MB, 8192=~268MB.
int mc2ShadowFullMapSize()
{
    static const int s_sz = []() {
        const int s = mc2::gate::get<"MC2_SHADOW_FULLMAP_SIZE">();   // row default 4096
        if (s <= 2048) return 2048;
        if (s <= 4096) return 4096;
        return 8192;
    }();
    return s_sz;
}

// CSM-REDESIGN: fixed near/mid cascade radii (WU), texel-snapped, frame-stable
// (kills frustum-refit popping). Last cascade is full-map (map-centered).
float mc2ShadowCsmR0()
{
    static const float s = []() {
        const float r = mc2::gate::get<"MC2_SHADOW_CSM_R0">();   // row default 512
        return (r < 64.0f) ? 64.0f : r;
    }();
    return s;
}
float mc2ShadowCsmR1()
{
    static const float s = []() {
        const float r = mc2::gate::get<"MC2_SHADOW_CSM_R1">();   // row default 4096
        return (r < 128.0f) ? 128.0f : r;
    }();
    return s;
}

// MC2_SHADOW_CSM_SOFTNESS : PCF penumbra radius in texels (pre-adaptiveScale).
// Default 0.9. Lower = sharper mech/terrain shadows; higher = softer. Clamp [0.1, 6.0].
float mc2ShadowCsmSoftness()
{
    static const float s_s = mc2::gate::get<"MC2_SHADOW_CSM_SOFTNESS">();
    return s_s;
}

// SHADOW-DIST-FADE-1: distance-scaled dynamic-shadow softness + fade.
// MC2_SHADOW_DIST_FADE = occluder->receiver ray distance (world units) at which
// a dynamic shadow has fully dissolved (blur ramps in from ~0, opacity fade
// starts ~45% of the way). Default 80. =0 killswitch -> the define is not
// injected -> shadow.hglsl folds the whole block away (current behavior).
float mc2ShadowDistFadeWu()
{
    static const float s_f = mc2::gate::get<"MC2_SHADOW_DIST_FADE">();
    return s_f;
}

// SHADOW-DONUT-FIX-1: max opacity the distance fade may remove.
// MC2_SHADOW_DIST_FADE_MAX in [0,1], default 0.65. 1.0 = old full erase at the
// fade distance; lower keeps a soft shadow floor on far-caster interiors.
float mc2ShadowDistFadeMax()
{
    static const float s_m = mc2::gate::get<"MC2_SHADOW_DIST_FADE_MAX">();
    return s_m;
}

// SHADOW-FADE-CONTINUOUS-1: continuous occluder-distance estimate (8 midpoint
// refs + per-pixel ladder jitter) for the distance fade. Default ON; =0
// restores the original 4-step staircase ladder (visible fade/penumbra step
// boundaries, the batch-6 mech-shadow report).
bool mc2ShadowFadeCont()
{
    static const bool s_on = mc2::gate::get<"MC2_SHADOW_FADE_CONT">();
    return s_on;
}

// SHADOW-FADE-RAMP0-1 (batch-7 item 1): continuous-from-contact opacity fade
// (fadeMax * dNorm^2) instead of the smoothstep(0.45, 1.0) onset window whose
// boundary drew a visible crisp/soft SEAM along tree shadows. Default ON; =0
// restores the windowed onset.
bool mc2ShadowFadeRamp0()
{
    static const bool s_on = mc2::gate::get<"MC2_SHADOW_FADE_RAMP0">();
    return s_on;
}

// SHADOW-ALT-SCALE-1 (batch-7 item 2): scale the dist-fade penumbra/fade model
// by sun altitude (lightDir.z in-shader) — grazing sun keeps the aggressive
// night dissolve, high sun goes mostly-crisp with tip softening only. Default
// ON; =0 restores the fixed low-sun-tuned gains at every altitude.
bool mc2ShadowAltScale()
{
    static const bool s_on = mc2::gate::get<"MC2_SHADOW_ALT_SCALE">();
    return s_on;
}

// Object shadow-receive normal-offset bias (in cascade texels). Default 2.0,
// clamped [0,16]. 0 disables the normal-offset (A/B). Slope-scale term in the
// shader is always-on (tiny); this only gates the world-space normal-offset.
float mc2ShadowObjNormalBias()
{
    static const float s_b = mc2::gate::get<"MC2_SHADOW_OBJ_NORMAL_BIAS">();
    return s_b;
}

// Mech self-shadow softening factor. MC2_SHADOW_MECH_SOFT, default 1.0,
// clamped [0,4]. Scales the extra PCF penumbra radius + terminator smoothstep
// width applied to MECH pixels only (a<0.1). 0 = no extra softening (legacy
// hard mech self-shadow); >1 = blurrier/softer terminator.
float mc2ShadowMechSoft()
{
    static const float s_s = mc2::gate::get<"MC2_SHADOW_MECH_SOFT">();
    return s_s;
}

// PROP-SHADOW-RECEIVE-1: static props RECEIVE the dynamic CSM cascade shadow
// (building self-shadow + prop-on-prop) in shadow_screen.frag instead of the
// static map only. MC2_PROP_SHADOW_RECEIVE, DEFAULT OFF; =1 enables. OFF =
// legacy: the GBuffer1.a=0.25 prop class skips sampleDynamicShadow entirely
// (uniform uploads 0 -> shader branch unreachable -> pixel-identical).
int mc2PropShadowReceive()
{
    static const int s_v = mc2::gate::get<"MC2_PROP_SHADOW_RECEIVE">();
    return s_v;
}

// SHADOW-SUNCLAMP-1: the screen-shadow back-face guard must dot against the
// SAME sun the shadow MATRICES were built from.
//
// gameos_graphics.cpp:10197 builds the static light matrix from
// buildStaticLightMatrix(-lx, -ly, -lz) where (lx,ly,lz) = gos_GetTerrainLightDir();
// runScreenShadow() below uploads the byte-identical expression {-lx,-ly,-lz}
// into the `lightDir` uniform. Both are therefore the same light->scene vector
// in the same convention -- but only the matrix path runs it through
// mc2ClampShadowSunAltitude() (15 deg minimum altitude).
//
// On missions that author a horizontal sun (mc2_10: LightDirPitch=180 ->
// lightDirection.z = sin(180 deg) = 0) the clamp FIRES for the matrices and does
// not for the guard. shadow_screen.frag's `if (NdotL < 0.05) return 1.0;` then
// degenerates into a purely AZIMUTHAL test -- with no vertical component to lift
// it, every surface leaning away from the sun azimuth is declared "unlit" and
// discards shadows the shadow map legitimately contains (that map having been
// rendered from a 15deg-ELEVATED sun that does light those surfaces). The world
// splits into a shadowed half and an unshadowed half along one compass axis.
//
// Map-specificity falls straight out: the clamp only fires where
// sin(LightDirPitch) < sin(15 deg) = 0.2588 -- mc2_10 (180 deg -> 0.0) alone;
// mc2_01 = 50 deg, mc2_03 = 46 deg, mc2_24 = 75 deg are all untouched no-ops.
//
// Fix: feed the guard the clamped vector too. Only the shadow-visibility guard
// and its normal-offset change; the SHADING light dir (terrain N-dot-L, diffuse
// brightness) is a different uniform on a different path and is NOT touched, so
// scene brightness/colour is unaffected -- this only restores shadows that the
// guard was wrongly discarding.
// MC2_SHADOW_GUARD_CLAMPED_SUN, DEFAULT ON; =0 reverts to the raw-sun guard.
void mc2ClampShadowSunAltitude(float& fx, float& fy, float& fz);

bool mc2ShadowGuardClampedSun()
{
    static const bool s_on = mc2::gate::get<"MC2_SHADOW_GUARD_CLAMPED_SUN">();
    return s_on;
}

// Cloud-shadow master gate. MC2_CLOUD_SHADOW is DEFAULT OFF -- opt in with =1.
// The reader below early-returns false on unset (`if (!v || !v[0]) return false`),
// so a default launch takes NO fullscreen cloud-shadow pass; the legacy
// inline-cloud behaviour this pass replaces is what still ships. Setting =0
// also disables it (the C++ early-return skips the pass entirely).
// This line claimed DEFAULT ON until ENV-DROP-CLOSE-1 (2026-07-30), which would
// have had anyone A/B-ing "=0 versus unset" grading OFF against OFF.
bool mc2CloudShadowEnabled()
{
    static const bool s_on = mc2::gate::get<"MC2_CLOUD_SHADOW">();
    return s_on;
}

// SKYBOX-FOG-EXCLUDE-1: gate helper (read once).
// SKYBOX-FOG-EXCLUDE-DEFAULT-ON-1: flipped default OFF->ON. The gate is inert
// on non-HDRI missions -- renderHdriSkybox() early-returns before the stencil-
// tag pass ("black sky baseline"), so no stencil is ever tagged, the fog frags'
// stencil branch is a no-op, and fog_oob.frag's worldDir.z<-0.22 band fallback
// is retained (byte-identical to legacy there). On HDRI missions it excludes
// fog from true-sky pixels so the skybox is visible instead of being painted
// over by the OOB cloud bank (the user-reported "can't see the skybox").
// Kill-switch preserved: MC2_SKYBOX_FOG_EXCLUDE=0 restores the legacy OFF path.
bool skyboxFogExcludeEnabled()
{
    static const bool s_on = mc2::gate::get<"MC2_SKYBOX_FOG_EXCLUDE">();
    return s_on;
}

// SKYBOX-FOG-EXCLUDE-SINGLE-READER-1: THE one exported authority for this gate.
//
// vulkan_post_fog_native.cpp used to carry its OWN `getenv("MC2_SKYBOX_FOG_EXCLUDE")`
// static, written `v && v[0] == '1'` -- default OFF -- while the helper directly
// above defaults ON. Both were "the guard that keeps VK from fogging sky pixels GL
// excludes"; they disagreed about when it fires, so with the env UNSET the guard was
// inert and the VK-native fog path rendered sky WITHOUT the exclusion GL applies.
// A second copy of a gate reader is a second default, and defaults drift. There is
// now exactly one getenv site for this variable in the tree; the VK TU calls this.
bool gos_SkyboxFogExcludeEnabled() { return skyboxFogExcludeEnabled(); }

bool sceneGlProducerQueryEnabled()
{
    static const bool s_on = mc2::gate::get<"MC2_VK_SCENE_GL_PRODUCER_QUERY">();
    return s_on;
}

bool screenShadowGlCanaryEnabled()
{
    static const bool s_on = mc2::gate::get<"MC2_VK_SCREENSHADOW_GL_CANARY">();
    return s_on;
}
