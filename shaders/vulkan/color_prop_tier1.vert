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
#extension GL_ARB_shader_draw_parameters : require
#extension GL_EXT_buffer_reference : require
layout(constant_id = 0) const int kBaseQuality = 2;

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in uint a_localVertexID;
layout(location = 4) in uint a_aRGBLight;

// Per-frame UBO: generated from RenderCore/gpu_abi/GpuColorFrameUBO.h.
#include "include/color_frame_ubo_vk.glsl"

// Mission environment block (SH ambient): the terrain set's binding 26, visible to this vertex stage.
#define TERRAIN_RESOURCE_SET 1
#include "include/environment_ubo_vk.glsl"
#include "include/lighting_vk.glsl"
#include "include/one_sun_vk.glsl"

// Props use frame sun and SH; local lights come from the fragment pool feed.

// SH-L2 evaluator -- verbatim port of static_prop.vert::evalShL2 (Ramamoorthi-Hanrahan
// diffuse-irradiance reconstruction). Raw radiance coefficients are convolved
// here; the diffuse response below applies Lambert's 1/pi once.
#include "include/prop_sh_l2.glsl"
#include "../include/surface_material.hglsl"

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
#include "include/prop_packet_vk.glsl"

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
layout(location = 3) out vec3 vArgb;          // encoded vertex multiplier for the legacy art adapter
flat layout(location = 4) out int  vLayerOn;  // alpha-ON albedo array layer (-1 = none)
flat layout(location = 5) out vec2 vUvScale;  // FENCE-UVSCALE-1: this packet's uvScaleX/Y
// StaticPropPbr uses the frame sun (Stuff frame, colour zero = none).
layout(location = 6) out vec3 vWorldPos;
flat layout(location = 8) out vec3 vSunColor;
// Per-instance objective / selection highlight (static_prop.vert step 6 + static_prop.frag): added to the vertex light and,
// alpha-weighted, to the fragment colour; zero when the game highlights nothing.
flat layout(location = 9) out vec4 vHighlight;
layout(location = 10) out vec3 vMaterialSeed;
flat layout(location = 12) out vec3 vMaterialSun;
flat layout(location = 13) out uint vMaterialFlags;
layout(location = 14) out vec4 vTree; // ambient AO, tree material active, forest sky/sun visibility
flat layout(location = 15) out uvec2 vWindow; // glass cell ID, stable building seed
flat layout(location = 16) out uvec3 vTeam;   // team paint 0x00RRGGBB x3 (red, green, blue slot), carried in the instance's fogRGB bits
flat layout(location = 17) out uint vPacketIdx; // this draw's PropPacket row (packetBase + gl_DrawID)
// 1 = double-sided pipeline: the lighting runs for the back face too. 0 = single-sided (CULL_BACK): front lighting only.
layout(constant_id = 11) const int kTwoSided = 1;
// The core depth pre-pass and the colour pass (depth EQUAL) must produce bit-identical depth.
invariant gl_Position;
layout(location = 18) out vec3 vBackNormalStuff;
layout(location = 19) out vec3 vBackArgb;
flat layout(location = 20) out float vLodBlend;
flat layout(location = 21) out vec3 vLodOrigin;

// Per-type hot-colour magic: the emission colours of the three night tags (windows, outside building lights, building base lights), taken from the type's ini.
// Three vec4 per type in the order pink, yellow, green; indexed by inst.typeID. Same table the GL prop shader reads.
layout(set = 0, binding = 7, std430) readonly buffer TypeHotColors {
    vec4 c[];
} uTypeHot;

// Strength of the directional modulation of the baked base light + ambient: a face pointing at the sun keeps factor 1.0, a face pointing away is scaled by (1 - strength).
// Same value as static_prop.vert so the back faces of buildings read soft instead of flat bright.
const float kPropBaseLightDirStrength = 0.45;

#include "include/prop_night_vk.glsl"

// Resolves the per-vertex hot-colour magic tags against the eye's night state, as lighting.hglsl get_base_light does for the GL prop path: the lit-window,
// outside-light and building-base-light tags are self-illuminated at night and read dark grey (windows) or zero by day, the blink tags stay dark and any other
// non-zero seed is added as authored. Without the night arms every window, base light and runway fixture stays off after dark.
vec3 hotColorBaseLight(uint startV, vec3 colouredSeed, bool lightsOut, vec3 pink, vec3 yellow, vec3 green) {
    const float kSmall = 1e-4;   // Stuff_SMALL
    const bool  isNight = uEnv.nightState.x != 0.0;
    const float nightFactor = uEnv.nightState.y;
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
    } else if (startV == 0xfeffffffu) {
        // Synthetic imported building body fill: preserve day brightness, but
        // match the former black seed exactly at full night (not the 20% bake).
        if (!lightsOut) outLight = colouredSeed * (1.0 - uEnv.nightState.z) * uEnv.sceneLight.yzw;   // prelit art: relit by the sun keys' flat-light ratio (1 without keys)
    } else if ((startV & 0x00ffffffu) != 0u) {
        // Any other light colour is authored vertex light (hangars, terminals carry a near-white bake): it is environment
        // light, not a lamp, so the night dims it like the sun and ambient -- by day it is added as authored.
        if (!lightsOut) outLight = colouredSeed * mix(1.0, kNightAuthoredLight, uEnv.nightState.z) * uEnv.sceneLight.yzw;
    }
    return clamp(outLight, vec3(0.0), vec3(1.0));
}

// Compacted slot list written by color_prop_cull.comp: the draw's firstInstance is the run's first slot, so
// compact[gl_InstanceIndex] is the surviving instance's slot. HZB-culled instances are not in the list.
layout(set = 0, binding = 17, std430) readonly buffer Compact {
    uint c[];
} uCompact;

void main() {
    vPacketIdx = uPacketBase.packetBase + uint(gl_DrawIDARB);
    GpuStaticPropInstance inst = uInstances.i[uFrame.instBase + (uPacketBase.directIndex != 0u ? uint(gl_InstanceIndex) : uCompact.c[gl_InstanceIndex])];
    // Importer reserves alpha 0x7d/0x7e for UV1 glass IDs with white/black seeds.
    uint lightTag = a_aRGBLight >> 24;
    bool windowCell = (inst.flags & 64u) != 0u && (lightTag == 0x7du || lightTag == 0x7eu);
    uint vertexLight = windowCell ? (lightTag == 0x7du ? 0xffffffffu : 0xff000000u) : a_aRGBLight;
    // Translation is the last row (row-vector transform).
    vec3 origin = vec3(inst.modelMatrix[0][3], inst.modelMatrix[1][3], inst.modelMatrix[2][3]);
    vWindow = uvec2(windowCell ? a_aRGBLight & 0xffffffu : 0u,
                    floatBitsToUint(origin.x) ^ (floatBitsToUint(origin.y) * 1664525u)
                    ^ (floatBitsToUint(origin.z) * 1013904223u));
    // Row-vector * M — identical to the GL VS (Stuff std430 column-major convention).
    vec4 world_stuff = vec4(a_position, 1.0) * inst.modelMatrix;
    // POSITION: Stuff -> MC2 frame swizzle — identical to the GL VS.
    vec3 world_mc2 = mc2ToRender(world_stuff.xyz);
    vec4 clip = uFrame.worldToClipGL * vec4(world_mc2, 1.0);
    clip.z += uFrame.objectDepthBias * clip.z;
    gl_Position = clip;
    vWorldPos = world_mc2;
    vSunColor = vec3(0.0);
    vUv = a_uv;
    // NORMAL: forward in STUFF-SPACE (un-swapped). BIT-EXACT mirror of
    // static_prop.vert:352 `worldNormal = a_normal * mat3(inst.modelMatrix)`
    // (ROW-VECTOR convention — Stuff std430 col-major; `mat3(M) * a_normal` would
    // apply the INVERSE rotation and break parity). The Stuff->MC2 axis swap is NOT
    // applied to the normal (positions only), so this is the un-swapped Stuff-space
    // normal the GL GBuffer1 is written from. The FS then does normalize()*0.5+0.5.
    uint canopyBase = inst.typeID * 3u;
    vec3 canopy = uFrame.hotTableReady != 0 && canopyBase + 2u < uint(uTypeHot.c.length())
        ? vec3(uTypeHot.c[canopyBase].w, uTypeHot.c[canopyBase+1u].w, uTypeHot.c[canopyBase+2u].w) : vec3(0);
    bool tree = canopy.y > 0.0 && (inst.flags & 2u) == 0u;
    vTree = vec4(1.0, tree ? 1.0 : 0.0, 1.0, 1.0);
    // Forest light bake (flags[8..31]): sun shadow-line height, crown-base and crown-top sky occlusion.
    // hn = vertex height in tree-top units (crown centre 0.65, crown top 1.0); crowns are lit above the shadow line.
    float forestSky = 1.0, forestSun = 1.0;
    if (tree) {
        float hn = 0.65 + sign(canopy.z) * (a_position.y - canopy.x) / (2.0 * abs(canopy.z));
        vec3 bake = vec3((inst.flags >> 8u) & 255u, (inst.flags >> 16u) & 255u, inst.flags >> 24u) / 255.0;
        float line = bake.x * 1.25;   // lit once hn clears the shadow line; an unshadowed edge tree is lit from the ground up
        forestSun = mix(0.02, 1.0, max(smoothstep(line - 0.05, line + 0.2, hn), 1.0 - smoothstep(0.0, 0.1, line)));
        forestSky = pow(1.0 - mix(bake.y, bake.z, smoothstep(0.3, 1.0, hn)), 3.0); // contrast: deep interior falls off fast
    }
    vTree.z = forestSky;
    vTree.w = forestSun;
    // Root plate and flare (non-leaf geometry in the lowest ~5% of the tree) take the terrain colour so the base does not read as a pale
    // slab on the ground. The weight rides in vTree.y (1 = plain tree .. 0.6 = full base blend); the tree test stays y > 0.5.
    if (tree && (inst.flags & 4u) == 0u) {
        float treeHeight = abs(canopy.x) + 0.7 * abs(canopy.z);
        vTree.y = 1.0 - 0.4 * (1.0 - smoothstep(0.015, 0.06, abs(a_position.y) / max(treeHeight, 1.0)));
    }
    vec3 localNormal = a_normal;
    vec3 backNormal = -a_normal;
    if (tree && (inst.flags & 4u) != 0u) {
        vec3 crown = vec3(a_position.x / canopy.y, (a_position.y - canopy.x) / abs(canopy.z), a_position.z / canopy.y);
        if (dot(crown,crown) > 1e-6) {
            localNormal = normalize(mix(normalize(a_normal), normalize(crown), 0.7));
            backNormal = normalize(mix(-normalize(a_normal), normalize(crown), 0.7));
        }
        vTree.x = mix(0.65, 1.0, clamp(0.5 + sign(canopy.z) * (a_position.y - canopy.x) / (2.0 * abs(canopy.z)), 0.0, 1.0));
    }
    // Single-level stock-leaf sway; trunk and baked shadows remain fixed.
    if (tree && (inst.flags & 4u) != 0u) {
        float height = clamp(0.5 + sign(canopy.z) * (a_position.y - canopy.x) / (2.0 * abs(canopy.z)), 0.0, 1.0);
        float phase = uFrame.sway.x * 1.3 + dot(world_mc2.xy, vec2(0.017,0.023));
        world_mc2.xy += vec2(1.0,0.6) * sin(phase) * height * height * min(canopy.y * 0.025,1.5);
        clip = uFrame.worldToClipGL * vec4(world_mc2,1.0);
        clip.z += uFrame.objectDepthBias * clip.z;
        gl_Position = clip; vWorldPos = world_mc2;
    }
    vNormalStuff = localNormal * mat3(inst.modelMatrix);
    vBackNormalStuff = backNormal * mat3(inst.modelMatrix);
    for (int side = 0; side < (kTwoSided != 0 ? 2 : 1); ++side) {
    vec3 worldNormalStuff = side == 0 ? vNormalStuff : vBackNormalStuff;

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
    // Ambient fill is evaluated once; preserve HDR lighting for fragment shading.
    // BASE-LIGHT SEED. The raw per-vertex bytes are NOT light: the hot-colour magic tags must be resolved first (the green-glow field report was the raw
    // 0xff00ff00 tag dumped into the colour). hotColorBaseLight resolves them against the eye's night state and the type's own emission colours, so the
    // windows, building base lights and runway / beacon fixtures light up at night exactly as authored and stay dark by day.
    const uint kFlagIsLightsOut = 1u << 0;
    const uint kFlagIsWindow    = 1u << 1;
    uint b8 = (vertexLight >>  0) & 0xFFu;
    uint g8 = (vertexLight >>  8) & 0xFFu;
    uint r8 = (vertexLight >> 16) & 0xFFu;
    uint a8 = (vertexLight >> 24) & 0xFFu;
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
    vLodBlend = inst.fogRGB.w;
    vLodOrigin = mc2ToRender(origin);
    vMaterialSeed = seed;
    vMaterialSun = vec3(0);
    vMaterialFlags = inst.flags;
    vTeam = floatBitsToUint(inst.fogRGB.xyz);
    if ((inst.flags & kFlagIsWindow) != 0u) {
        vArgb = surfaceEncodeSrgb(surfaceDecodeSrgb(seed) + surfaceDecodeSrgb(inst.aRGBHighlight.rgb));
        vBackArgb = vArgb;
        return;
    }

    // NdotL vs terrain sun (both in MC2 world). static_prop.vert:363 maps the
    // Stuff-space normal to MC2 world as normalize(mc2ToRender(n)).
    vec3  nMC2   = normalize(mc2ToRender(worldNormalStuff));
    vec3  L      = normalize(oneSunAuthFromLive() * uEnv.sunDirLive.xyz);   // authored sun direction (one-sun rule)
    // GL static_prop.vert:~730: ALPHA_TEST (tree cards/foliage) instances are two-sided,
    // so ndl uses abs(); solid props use max(.,0). The alpha-ON packet group IS the
    // ALPHA_TEST class (same test the FS uses). Without this, back-facing cards read dark.
    bool alphaTestPacket = (inst.flags & 4u) != 0u;   // per-instance flag bit-2, same as GL
    float ndlRaw = dot(nMC2, L);
    float ndl    = alphaTestPacket ? abs(ndlRaw) : max(ndlRaw, 0.0);

    // One environment SH source plus the authored vertex emission.
    const vec3 kSunColor = vec3(0.95, 0.93, 0.86);
    // Light contract (mclib/prop_light_contract.h): the sun colour is Camera::getLight*(I) from the
    // frame S/A/F and the actor's terrain light I carried in inst.actorTint; no light-row read.
    uint lightWord = inst.actorTint;
    bool sunOff = (lightWord & 0x10000u) != 0u;
    float propI = ((lightWord & 0x20000u) != 0u) ? 1.0 : float(lightWord & 0xffffu) / 65535.0;
    vec3 contractC = (propI > 0.0)
        ? min(max(floor(round(uEnv.sunLive.rgb * 255.0) * propI + round(uEnv.ambLive.rgb * 255.0)), vec3(uFrame.lightFloor)), vec3(255.0))
        : max(round(uEnv.ambLive.rgb * 255.0), vec3(uFrame.lightFloor));
    bool sunFound = (uFrame.lightingReady != 0) && !sunOff;
    // The live pitch is in propI (terrain light at the live sun) and in ndl above; pitchUndo (encoded, like the seed's x R) puts it back at the authored elevation.
    vec3 sunColorFed = sunFound ? contractC / 255.0 * uEnv.pitchUndo.rgb : vec3(0.0);
    vec3 sunTerm = (uFrame.lightingReady != 0) ? sunColorFed : kSunColor;
    vMaterialSun = sunTerm;
    vSunColor = sunFound ? sunColorFed : vec3(0.0);

    // Every tree uses the same fragment lighting, independent of imported
    // white vertex seeds or quality. Low tiers only omit detail-map sampling.
    if (tree) {
        vArgb = vec3(0);
        vBackArgb = vec3(0);
        return;
    }

    vec3 iblTerm = vec3(0.0);
    if (uFrame.iblShStrength > 0.0) {
        vec3 shCoeffs[9];
        for (int i = 0; i < 9; ++i) shCoeffs[i] = uEnv.shDiffuse[i].xyz;
        // worldNormal is Stuff-space Y-up (un-swapped), matching static_prop.vert's
        // evalShL2(normalize(worldNormal)) call (SH projector's axis convention).
        iblTerm = evalShL2(normalize(worldNormalStuff), shCoeffs) * uFrame.iblShStrength;
    }

    // ADJUDICATION-1: one ambient source replaces the stacked fills. Preserve
    // HDR material colour and highlights until the output transform.
    // Directional wrap on the orientation-free part (baked seed + ambient): factor 1.0 facing the sun, (1 - strength) facing away; the sun term itself is never
    // scaled. Alpha-test cards (foliage, fences) are two-sided and carry no solid-surface normal, so they stay at 1.0. The IBL fill is added unscaled, as in GL.
    float propDirFactor = 1.0;
    if (!alphaTestPacket) {
        float wrap = 0.5 + 0.5 * dot(nMC2, L);
        propDirFactor = mix(1.0 - kPropBaseLightDirStrength * propNightDirectShare(), 1.0, wrap);
    }
    // SH evaluates irradiance, not reflected radiance. Apply the Lambert BRDF
    // once, and decode authored light bytes before summing with linear SH.
    vec3 linearLight = surfaceDecodeSrgb(seed) * propDirFactor
        + incidentLight(max(iblTerm, vec3(0.0)) * forestSky, surfaceDecodeSrgb(sunTerm) * vTree.w * propNightSunWeight(ndl, nMC2), vec3(1), 1.0) / 3.14159265
        + surfaceDecodeSrgb(inst.aRGBHighlight.rgb);
    if (side == 0) vArgb = surfaceEncodeSrgb(linearLight);
    else vBackArgb = surfaceEncodeSrgb(linearLight);
    }
}
