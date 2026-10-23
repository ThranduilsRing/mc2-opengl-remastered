// VK-P11 STATIC-PROP COLOR — TIER-1 (parity) instanced static-prop VS.
// =============================================================================
// Uses the same world->clip transform as the GL static-prop path and forwards
// the Stuff-space normal to the FS so the GBuffer1 (MRT1) normal can be written
// bit-adjacent to the GL producer (shaders/static_prop.frag):
//   GBuffer1 = vec4(normalize(v_normal)*0.5+0.5, 0.25)
//
// ★v_normal is STUFF-SPACE, NOT GL/MC2-space. The GL producer's Stuff->MC2 frame
// swap applies to POSITIONS only; the GBuffer normal is written un-swapped (only
// the GL PBR block swaps for lighting). We therefore forward a_normal WITHOUT the
// vec3(-x, z, y) swizzle, exactly like static_prop.vert forwards v_normal.
#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in uint a_localVertexID;
layout(location = 4) in uint a_aRGBLight;

// Per-frame UBO: generated from RenderCore/gpu_abi/GpuColorFrameUBO.h.
#include "include/color_frame_ubo_vk.glsl"

// VK-PROP-AMBIENT-IBL-1: LightsData SSBO (binding 4), verbatim mirror of
// shaders/include/lighting.hglsl's ObjectLights layout (byte-identical std430/std140
// per that header's note -- every member is mat4/vec4-array + ivec4 tail). Copied
// per-frame from the live GL LightsData SSBO (binding 20) by the island. We only need
// the AMBIENT-type sum here (matches static_prop.vert:414-429's ambientTerm scan) --
// NOT the full calc_light dispatch; sun color/direction stay the island's existing
// kSunColor/terrainSunMC2 approximation (out of scope for this fix).
#include "../include/gpu_abi_object_lights.gen.hglsl"
#define MAX_LIGHTS_IN_WORLD GPU_ABI_OBJECTLIGHTS_LIGHT_TO_WORLD_LEN
#define TG_LIGHT_AMBIENT 0
layout(set = 0, binding = 4, std430) readonly buffer LightsData {
    ObjectLights light[];
} lightsData_;

// SH-L2 evaluator -- verbatim port of static_prop.vert::evalShL2 (Ramamoorthi-Hanrahan
// diffuse-irradiance reconstruction). Coefficients arrive pre-convolved (see
// RenderCore/IblShCoeffs.h header comment); do not re-apply the cosine kernel here.
vec3 evalShL2(vec3 n, vec3 sh[9]) {
    const float kSH_c1 = 0.429043;
    const float kSH_c2 = 0.511664;
    const float kSH_c3 = 0.743125;
    const float kSH_c4 = 0.886227;
    const float kSH_c5 = 0.247708;
    return
          kSH_c1 * sh[8] * (n.x * n.x - n.z * n.z)
        + kSH_c3 * sh[6] * (n.y * n.y)
        + kSH_c4 * sh[0]
        - kSH_c5 * sh[6]
        + 2.0 * kSH_c1 * sh[4] * (n.x * n.y)
        + 2.0 * kSH_c1 * sh[5] * (n.y * n.z)
        + 2.0 * kSH_c1 * sh[7] * (n.x * n.z)
        + 2.0 * kSH_c2 * sh[3] * n.x
        + 2.0 * kSH_c2 * sh[1] * n.y
        + 2.0 * kSH_c2 * sh[2] * n.z;
}

// VK-FLIP-FIELD-REPORT-2 BLACK-FENCE FIX: per-PACKET (not per-type) albedo layer,
// pushed fresh before each vkCmdDrawIndexed. GL's coalesce path samples texArrayLayer
// per-DRAW (PerDrawEntry), never per-type — a multi-packet type (e.g. a fence type's
// separate "post"/"arch" sub-mesh packets) can have DIFFERENT textures per packet.
// The prior per-type SSBO-by-typeID lookup forced every packet of a type to reuse
// one ("first-valid-wins") layer, so non-first packets sampled the WRONG BC7 layer —
// this produced solid-black fragments (wrong layer's black margin block, alpha>=0.5
// so no alpha-test discard caught it). -1 = no texture for this packet (teal fallback).
// FENCE-UVSCALE-1: uvScaleX/Y added alongside the layer fields (same push
// constant range, widened 8->16 bytes). GL's coalesce path crops each draw's
// sampled UV to this packet's sub-region within the group's max-dims array
// layer (static_prop.frag:206 `fract(v_uv) * vec2(uvScaleX, uvScaleY)`);
// forwarded to the FS via vUvScale (flat) so the FS applies the identical crop
// right where GL does, instead of assuming 1.0 (full layer -> narrower/gapped
// panels wherever a packet's unique texture is smaller than the group max).
layout(push_constant) uniform PacketLayerPC {
    int   packetLayerOff;
    int   packetLayerOn;
    float packetUvScaleX;
    float packetUvScaleY;
    int   packetAlphaTest;   // GL's live per-packet ALPHA_TEST bit (the FS reads it)
    int   coverageFringePass;   // PROP-COVERAGE-1 (FS only)
    int   hzbEnabled;
} uPacket;

#include "../include/gpu_abi_static_prop_instance.gen.hglsl"
#include "../include/axis.hglsl"
layout(set = 0, binding = 1, std430) readonly buffer Instances {
    GpuStaticPropInstance i[];
} uInstances;

// VK-P11 STATIC-PROP MATERIAL: per-type group-relative albedo layer (int; -1 = no
// alpha-OFF texture -> FS keeps teal for that type). Indexed by inst.typeID.
layout(set = 0, binding = 2, std430) readonly buffer TypeAlbedoLayer {
    int layer[];
} uTypeLayer;

// VK-P11 STATIC-PROP MATERIAL alpha-ON: per-type group-relative layer into the alpha-ON
// array (-1 for non-class-1 types). Indexed by inst.typeID.
layout(set = 0, binding = 5, std430) readonly buffer TypeAlbedoLayerOn {
    int layer[];
} uTypeLayerOn;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec3 vNormalStuff;   // Stuff-space normal (un-swapped) for GBuffer1
flat layout(location = 2) out int  vLayer;    // alpha-OFF albedo array layer (-1 = none)
layout(location = 3) out vec3 vArgb;          // VERTEX-BAKED lit color (GL default model)
flat layout(location = 4) out int  vLayerOn;  // alpha-ON albedo array layer (-1 = none)
flat layout(location = 5) out vec2 vUvScale;  // FENCE-UVSCALE-1: this packet's uvScaleX/Y
// Inputs of the StaticPropPbr specular: the surface position (GL world) and the instance's sun light as stored in its ObjectLights row (Stuff frame, colour zero = none).
layout(location = 6) out vec3 vWorldPos;
flat layout(location = 7) out vec3 vSunDirStuff;
flat layout(location = 8) out vec3 vSunColor;
// Per-instance objective / selection highlight (static_prop.vert step 6 + static_prop.frag): added to the vertex light and,
// alpha-weighted, to the fragment colour; zero when the game highlights nothing.
flat layout(location = 9) out vec4 vHighlight;

// Per-type hot-colour magic: the emission colours of the three night tags (windows, outside building lights, building base lights), taken from the type's ini.
// Three vec4 per type in the order pink, yellow, green; indexed by inst.typeID. Same table the GL prop shader reads.
layout(set = 0, binding = 7, std430) readonly buffer TypeHotColors {
    vec4 c[];
} uTypeHot;

// Strength of the directional modulation of the baked base light + ambient: a face pointing at the sun keeps factor 1.0, a face pointing away is scaled by (1 - strength).
// Same value as static_prop.vert so the back faces of buildings read soft instead of flat bright.
const float kPropBaseLightDirStrength = 0.45;

// Resolves the per-vertex hot-colour magic tags against the eye's night state, as lighting.hglsl get_base_light does for the GL prop path: the lit-window,
// outside-light and building-base-light tags are self-illuminated at night and read dark grey (windows) or zero by day, the blink tags stay dark and any other
// non-zero seed is added as authored. Without the night arms every window, base light and runway fixture stays off after dark.
vec3 hotColorBaseLight(uint startV, vec3 colouredSeed, bool lightsOut, vec3 pink, vec3 yellow, vec3 green) {
    const float kSmall = 1e-4;   // Stuff_SMALL
    const bool  isNight = uFrame.isNight != 0;
    const float nightFactor = uFrame.nightFactor;
    vec3 outLight = vec3(0.0);
    if (startV == 0xffff00ffu) {                       // hot pink: lit windows, night only
        if (isNight)                   outLight = pink;
        else if (nightFactor > kSmall) outLight = pink * nightFactor;
        else                           outLight = vec3(0x2f / 255.0);   // windows are painted dark grey by day
    } else if (startV == 0xffffff00u) {                // hot yellow: outside building lights, night only
        if (nightFactor >= 0.75) outLight = isNight ? yellow : yellow * nightFactor;
    } else if (startV == 0xff00ff00u) {                // hot green: building base lights, night only
        if (isNight)                   outLight = green;
        else if (nightFactor > kSmall) outLight = green * nightFactor;
    } else if (startV == 0xffff0000u || startV == 0xff0000ffu) {
        // blink tags: dark
    } else if ((startV & 0x00ffffffu) != 0u) {
        if (!lightsOut) outLight = colouredSeed;       // any other light colour is added as authored
    }
    return clamp(outLight, vec3(0.0), vec3(1.0));
}

layout(set = 0, binding = 9, std430) readonly buffer HzbVisibility {
    uint culled;
    uint visible[];
} uHzb;

void main() {
    // Render-only: the instance feed and game/AI visibility are never modified.
    if (uPacket.hzbEnabled != 0 && uHzb.visible[gl_InstanceIndex] == 0u) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    GpuStaticPropInstance inst = uInstances.i[uFrame.instBase + gl_InstanceIndex];
    // Row-vector * M — identical to the GL VS (Stuff std430 column-major convention).
    vec4 world_stuff = vec4(a_position, 1.0) * inst.modelMatrix;
    // POSITION: Stuff -> MC2 frame swizzle — identical to the GL VS.
    vec3 world_mc2 = mc2ToRender(world_stuff.xyz);
    vec4 clip = uFrame.worldToClipGL * vec4(world_mc2, 1.0);
    clip.z += uFrame.objectDepthBias * clip.w;
    gl_Position = clip;
    vWorldPos = world_mc2;
    vSunDirStuff = vec3(0.0);
    vSunColor = vec3(0.0);
    vUv = a_uv;
    // NORMAL: forward in STUFF-SPACE (un-swapped). BIT-EXACT mirror of
    // static_prop.vert:352 `worldNormal = a_normal * mat3(inst.modelMatrix)`
    // (ROW-VECTOR convention — Stuff std430 col-major; `mat3(M) * a_normal` would
    // apply the INVERSE rotation and break parity). The Stuff->MC2 axis swap is NOT
    // applied to the normal (positions only), so this is the un-swapped Stuff-space
    // normal the GL GBuffer1 is written from. The FS then does normalize()*0.5+0.5.
    vec3 worldNormalStuff = a_normal * mat3(inst.modelMatrix);
    vNormalStuff = worldNormalStuff;

    // VK-FLIP-FIELD-REPORT-2 BLACK-FENCE FIX: albedo layer now comes from the
    // per-PACKET push constant (this draw's own layer), not a per-type SSBO lookup
    // by inst.typeID. Fixes multi-packet types (e.g. fences) where sub-mesh packets
    // target different texture layers within the same group array.
    vLayer   = (uFrame.materialReady   != 0) ? uPacket.packetLayerOff : -1;
    vLayerOn = (uFrame.materialReadyOn != 0) ? uPacket.packetLayerOn  : -1;
    // FENCE-UVSCALE-1: forward this packet's uvScale unconditionally (cheap;
    // defaults to 1.0 from the CPU side when unavailable, so no gating needed).
    vUvScale = vec2(uPacket.packetUvScaleX, uPacket.packetUvScaleY);

    // Legacy vertex emission plus environment SH and the actor's directional light.
    // Keep vertex evaluation and the washout clamp until the prop material packet.
    // BASE-LIGHT SEED. The raw per-vertex bytes are NOT light: the hot-colour magic tags must be resolved first (the green-glow field report was the raw
    // 0xff00ff00 tag dumped into the colour). hotColorBaseLight resolves them against the eye's night state and the type's own emission colours, so the
    // windows, building base lights and runway / beacon fixtures light up at night exactly as authored and stay dark by day.
    const uint kFlagIsLightsOut = 1u << 0;
    const uint kFlagIsWindow    = 1u << 1;
    const uint b8 = (a_aRGBLight >>  0) & 0xFFu;
    const uint g8 = (a_aRGBLight >>  8) & 0xFFu;
    const uint r8 = (a_aRGBLight >> 16) & 0xFFu;
    const uint a8 = (a_aRGBLight >> 24) & 0xFFu;
    const uint startV = b8 | (g8 << 8) | (r8 << 16) | (a8 << 24);   // start_v_light packing (lighting.hglsl)
    // Until the real table is bound (placeholder: one entry) or for a type past its end, the emission colours are black.
    const uint hotBase = inst.typeID * 3u;
    const bool hotOk = uFrame.hotTableReady != 0 && hotBase + 2u < uint(uTypeHot.c.length());
    vec3 seed = hotColorBaseLight(startV, vec3(float(r8), float(g8), float(b8)) / 255.0,
                                  (inst.flags & kFlagIsLightsOut) != 0u,
                                  hotOk ? uTypeHot.c[hotBase + 0u].rgb : vec3(0.0),
                                  hotOk ? uTypeHot.c[hotBase + 1u].rgb : vec3(0.0),
                                  hotOk ? uTypeHot.c[hotBase + 2u].rgb : vec3(0.0));
    // A window node carries only its hot-colour light: no ambient, sun or IBL (they would wash out the night glow and the by-day grey).
    vHighlight = inst.aRGBHighlight;
    if ((inst.flags & kFlagIsWindow) != 0u) {
        vArgb = clamp(seed + inst.aRGBHighlight.rgb, 0.0, 1.0);
        return;
    }

    // NdotL vs terrain sun (both in MC2 world). static_prop.vert:363 maps the
    // Stuff-space normal to MC2 world as normalize(mc2ToRender(n)).
    vec3  nMC2   = normalize(mc2ToRender(worldNormalStuff));
    vec3  L      = normalize(uFrame.terrainSunMC2.xyz);
    // GL static_prop.vert:~730: ALPHA_TEST (tree cards/foliage) instances are two-sided,
    // so ndl uses abs(); solid props use max(.,0). The alpha-ON packet group IS the
    // ALPHA_TEST class (same test the FS uses). Without this, back-facing cards read dark.
    bool alphaTestPacket = (inst.flags & 4u) != 0u;   // per-instance flag bit-2, same as GL
    float ndlRaw = dot(nMC2, L);
    float ndl    = alphaTestPacket ? abs(ndlRaw) : max(ndlRaw, 0.0);

    // One environment SH source; baked vertex color and the washout clamp remain.
    const vec3 kSunColor = vec3(0.95, 0.93, 0.86);
    vec3 sunColorFed = vec3(0.0);
    vec3 sunDirFed   = vec3(0.0);
    bool sunFound    = false;
    if (uFrame.lightingReady != 0) {
        ObjectLights ld = lightsData_.light[int(inst.lightDataIndex)];
        int n = min(ld.numLights.x, MAX_LIGHTS_IN_WORLD);
        for (int i = 0; i < n; ++i) {
            int lt = int(ld.light_dir[i].w);
            if (!sunFound && (lt == 1 || lt == 2)) {
                sunColorFed = ld.light_color[i].xyz;
                sunDirFed = ld.light_dir[i].xyz;
                sunFound = true;
            }
        }
    }
    // Use the real scanned sun color when the feed is live (GL parity). Only the
    // fail-soft branch (no feed) falls back to the neutral kSunColor stand-in.
    vec3 sunTerm = (uFrame.lightingReady != 0) ? sunColorFed : kSunColor;
    vSunDirStuff = sunDirFed;
    vSunColor = sunFound ? sunColorFed : vec3(0.0);

    vec3 iblTerm = vec3(0.0);
    if (uFrame.iblShStrength > 0.0) {
        vec3 shCoeffs[9];
        for (int i = 0; i < 9; ++i) shCoeffs[i] = uFrame.iblSh[i].xyz;
        // worldNormal is Stuff-space Y-up (un-swapped), matching static_prop.vert's
        // evalShL2(normalize(worldNormal)) call (SH projector's axis convention).
        iblTerm = evalShL2(normalize(worldNormalStuff), shCoeffs) * uFrame.iblShStrength;
    }

    // ADJUDICATION-1 WASHOUT FIX (round 2): GL's static_prop.vert:524 clamps its
    // final `lit` to [0,1] before handing it to the fragment (`lit = clamp(lit +
    // aRGBHighlight, 0.0, 1.0)`). This island had NO clamp at all -- vArgb was
    // passed raw (often >1.0: measured iblSh0 DC term alone, 1.71 * kSH_c4 * 0.5
    // strength, already exceeds 1.0 before adding the 0.3 ambient fill) straight
    // into the frag's `albedo * vArgb` multiply. An unclamped >1 vArgb multiplies
    // the albedo brighter than GL's clamped-at-white equivalent on every bright
    // material (hangar roofs measured GL 123.8 vs VK 171.1, +47/255) -- this is
    // the dominant remaining washout term after the sun-color fix (91051001 /
    // ADJUDICATION-1 sun fix landed +47.3->+43.5; the missing clamp is the rest).
    // Directional wrap on the orientation-free part (baked seed + ambient): factor 1.0 facing the sun, (1 - strength) facing away; the sun term itself is never
    // scaled. Alpha-test cards (foliage, fences) are two-sided and carry no solid-surface normal, so they stay at 1.0. The IBL fill is added unscaled, as in GL.
    float propDirFactor = 1.0;
    if (!alphaTestPacket) {
        float wrap = 0.5 + 0.5 * dot(nMC2, L);
        propDirFactor = mix(1.0 - kPropBaseLightDirStrength, 1.0, wrap);
    }
    vArgb = clamp(seed * propDirFactor + iblTerm + sunTerm * ndl, 0.0, 1.0);
    vArgb = clamp(vArgb + inst.aRGBHighlight.rgb, 0.0, 1.0);
}
